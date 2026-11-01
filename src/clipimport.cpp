// Clip Studio Paint (.clip) → a Document of layers. The file is a chain of big-endian "CHNK" chunks: tiled
// bitmaps ("Exta", named by an id) and one SQLite database holding every layer's settings. Each layer's
// picture is reached Layer → Mipmap → MipmapInfo → Offscreen → Exta. The application can leave a vector,
// text, gradient or placed-image layer's picture out of the file and redraw it on load, so those are
// rebuilt from their own settings when it does.
#include "prepress.h"
#include "sqlitemini.h"
#include "svgimport.h"
#include <QFontMetricsF>
#include <QHash>
#include <QImage>
#include <QPainter>
#include <QSet>
#include <QtMath>
#include <zlib.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <tuple>

namespace xara {
namespace {

const qint64 kMaxPixels = qint64(1) << 28;
inline quint32 be32(const uchar* p){ return quint32(p[0]) << 24 | quint32(p[1]) << 16 | quint32(p[2]) << 8 | p[3]; }
inline quint64 be64(const uchar* p){ return quint64(be32(p)) << 32 | be32(p + 4); }
inline quint32 le32(const uchar* p){ return quint32(p[3]) << 24 | quint32(p[2]) << 16 | quint32(p[1]) << 8 | p[0]; }

struct Span { qint64 off = 0, len = 0; };

// The Offscreen.Attribute blob: three named sections, each a UTF-16BE tag then big-endian integers.
struct Attr {
    int w = 0, h = 0, gridW = 0, gridH = 0;
    int alphaCh = 0, bufCh = 0, bufDepth = 0, alphaDepth = 0, blockW = 0, blockH = 0;
    bool mono = false;
    quint32 initColor = 0;
};
bool parseAttr(const QByteArray& a, Attr& t){
    const uchar* p = reinterpret_cast<const uchar*>(a.constData());
    const int n = a.size();
    if(n < 16 + 4 + 18 + 16 + 64) return false;
    int o = 16;
    const quint32 tag = be32(p + o); o += 4;
    if(tag > 64 || o + int(tag) * 2 + 16 + 64 > n) return false;
    o += int(tag) * 2;
    auto i32 = [&](int at){ return qint32(be32(p + at)); };
    t.w = i32(o); t.h = i32(o + 4); t.gridW = i32(o + 8); t.gridH = i32(o + 12); o += 16;
    t.alphaCh = i32(o + 4); t.bufCh = i32(o + 8);
    t.bufDepth = i32(o + 24) >> 5; t.alphaDepth = i32(o + 32) >> 5;
    t.blockW = i32(o + 40); t.blockH = i32(o + 44);
    t.mono = i32(o + 56) != 0;
    o += 64;
    if(o + 4 <= n){
        const quint32 tag2 = be32(p + o);
        if(tag2 <= 64 && o + 4 + int(tag2) * 2 + 8 <= n) t.initColor = be32(p + o + 4 + tag2 * 2 + 4);
    }
    if(t.w < 0 || t.h < 0 || t.gridW < 0 || t.gridH < 0 || t.gridW > 65536 || t.gridH > 65536) return false;
    if(qint64(t.w) * t.h > kMaxPixels) return false;
    if(t.blockW <= 0 || t.blockH <= 0 || t.blockW > 4096 || t.blockH > 4096) return false;
    return (t.alphaCh == 0 || t.alphaCh == 1) && (t.bufCh == 0 || t.bufCh == 1 || t.bufCh == 3 || t.bufCh == 4);
}

struct Clip {
    const uchar* base = nullptr; qint64 size = 0;
    QHash<QByteArray,Span> exta;
    QHash<qint64,SqliteMini::Row> layers, mipmaps, infos, offscreens;
};

// One bitmap, ARGB. In a tile the alpha plane comes first, then the colour (blue, green, red, padding).
bool readOffscreen(const Clip& c, qint64 mipmapId, QImage& out, bool maskOnly){
    const SqliteMini::Row mip = c.mipmaps.value(mipmapId);
    const SqliteMini::Row info = c.infos.value(mip.value("BaseMipmapInfo").toLongLong());
    const SqliteMini::Row off = c.offscreens.value(info.value("Offscreen").toLongLong());
    if(off.isEmpty()) return false;
    Attr a;
    if(!parseAttr(off.value("Attribute").toByteArray(), a) || a.w <= 0 || a.h <= 0) return false;
    if(a.alphaCh == 0 && a.bufCh == 0) return false;
    const auto it = c.exta.constFind(off.value("BlockData").toByteArray());
    if(it == c.exta.constEnd()) return false;

    const qint64 area = qint64(a.blockW) * a.blockH;
    const qint64 alphaBytes = a.alphaCh ? (a.mono ? area / 8 : area) : 0;
    if(!a.mono && ((a.alphaCh && a.alphaDepth != 8) || (a.bufCh && a.bufDepth != 8 * a.bufCh))) return false;   // depth counts the whole pixel
    const qint64 bufBytes = a.bufCh ? (a.mono ? area / 8 : area * a.bufCh) : 0;
    const qint64 tileBytes = alphaBytes + bufBytes;

    QImage img(a.w, a.h, QImage::Format_ARGB32);
    if(img.isNull()) return false;
    // Tiles left out of the file hold the initial colour: all clear, or for a mask usually all open.
    img.fill(a.initColor ? 0xffffffffu : (a.alphaCh ? 0x00000000u : 0xff000000u));

    QByteArray tile(int(tileBytes), Qt::Uninitialized);
    qint64 o = it->off; const qint64 end = it->off + it->len;
    const QByteArray beginTag = QByteArray::fromHex("0042006c006f0063006b00440061007400610042006500670069006e004300680075006e006b");
    const int beginLen = beginTag.size();
    while(o + 8 <= end){
        const quint32 sz = be32(c.base + o);
        const quint32 tagLen = be32(c.base + o + 4);
        if(tagLen != 19 || o + 8 + beginLen + 20 > end || std::memcmp(c.base + o + 8, beginTag.constData(), size_t(beginLen)) != 0) break;
        if(sz < quint32(8 + beginLen) || o + sz > end) break;
        qint64 q = o + 8 + beginLen;
        const quint32 index = be32(c.base + q);
        const quint32 flag = be32(c.base + q + 16);
        q += 20;
        if(flag == 1 && q + 8 <= end && index < quint32(a.gridW) * quint32(a.gridH)){
            const quint32 dataSize = le32(c.base + q + 4);
            if(q + 8 + dataSize <= o + sz){
                uLongf got = uLongf(tileBytes);
                const int rc = uncompress(reinterpret_cast<Bytef*>(tile.data()), &got, c.base + q + 8, dataSize);
                if((rc == Z_OK || rc == Z_BUF_ERROR) && qint64(got) >= tileBytes){
                    const int tx = int(index % quint32(a.gridW)) * a.blockW, ty = int(index / quint32(a.gridW)) * a.blockH;
                    const uchar* al = reinterpret_cast<const uchar*>(tile.constData());
                    const uchar* buf = al + alphaBytes;
                    for(int y = 0; y < a.blockH && ty + y < a.h; ++y){
                        QRgb* d = reinterpret_cast<QRgb*>(img.scanLine(ty + y)) + tx;
                        for(int x = 0; x < a.blockW && tx + x < a.w; ++x){
                            const qint64 i = qint64(y) * a.blockW + x;
                            int A = 255, R = 0, G = 0, B = 0;
                            if(a.alphaCh) A = a.mono ? ((al[i / 8] >> (7 - (i & 7))) & 1) * 255 : al[i];
                            if(a.bufCh == 1){ const int v = a.mono ? ((buf[i / 8] >> (7 - (i & 7))) & 1) * 255 : buf[i]; R = G = B = v; }
                            else if(a.bufCh >= 3){ B = buf[i * a.bufCh]; G = buf[i * a.bufCh + 1]; R = buf[i * a.bufCh + 2]; }
                            else if(maskOnly){ R = G = B = A; }
                            d[x] = qRgba(R, G, B, A);
                        }
                    }
                }
            }
        }
        o += sz;
    }
    out = img;
    return true;
}

QByteArray blendKey(int m){
    switch(m){
        case 1: return "dark"; case 2: return "mul "; case 3: return "idiv"; case 4: return "lbrn"; case 5: return "fsub";
        case 6: return "dkCl"; case 7: return "lite"; case 8: return "scrn"; case 9: case 10: return "div "; case 11: case 12: return "lddg";
        case 13: return "lgCl"; case 14: return "over"; case 15: return "sLit"; case 16: return "hLit"; case 17: return "vLit";
        case 18: return "lLit"; case 19: return "pLit"; case 20: return "hMix"; case 21: return "diff"; case 22: return "smud";
        case 23: return "hue "; case 24: return "sat "; case 25: return "colr"; case 26: return "lum "; case 36: return "fdiv";
        default: return "norm";
    }
}

inline double beF64(const uchar* p){ const quint64 v = be64(p); double d; std::memcpy(&d, &v, 8); return qIsFinite(d) ? d : 0.0; }
inline float beF32(const uchar* p){ const quint32 v = be32(p); float f; std::memcpy(&f, &v, 4); return std::isfinite(f) ? f : 0.f; }
inline qint32 leI32(const uchar* p){ return qint32(le32(p)); }
inline double leF64(const uchar* p){ const quint64 v = quint64(le32(p + 4)) << 32 | le32(p); double d; std::memcpy(&d, &v, 8); return qIsFinite(d) ? d : 0.0; }
QString numStr(double v){ return QString::number(v, 'g', 12); }
QString xmlEsc(QString s){ s.replace('&',"&amp;"); s.replace('<',"&lt;"); s.replace('>',"&gt;"); s.replace('"',"&quot;"); return s; }

// A fill or gradient layer's picture, drawn over the whole canvas. The "GradationSetting" fields follow
// the tag directly; the stops sit in "GradationData".
bool renderGradation(const QByteArray& g, const QColor& mainColor, const QColor& subColor, int W, int H, QImage& out){
    const uchar* p = reinterpret_cast<const uchar*>(g.constData());
    const int n = g.size();
    auto tagAt = [&](const char* name){ QByteArray b; for(const char* c = name; *c; ++c){ b.append('\0'); b.append(*c); } return g.indexOf(b); };
    struct Stop { QColor c; double pos; };
    QVector<Stop> stops;
    bool flat = false; QColor flatColor(0, 0, 0);
    const int dataTag = tagAt("GradationData");
    if(dataTag >= 0){
        const int s = dataTag + 13 * 2;          // tag, size, 16, 28, count, 16, then 28 bytes a stop
        if(s + 20 <= n){
            const int count = qint32(be32(p + s + 12));
            int q = s + 20;
            for(int i = 0; i < count && i < 4096 && q + 28 <= n; ++i, q += 28){
                QColor c(p[q], p[q + 4], p[q + 8], p[q + 12]);
                const quint32 ref = be32(p + q + 16);          // 1 and 2: the layer's own main and sub colours
                if(ref == 1 && mainColor.isValid()) c = QColor(mainColor.red(), mainColor.green(), mainColor.blue(), c.alpha());
                else if(ref == 2 && subColor.isValid()) c = QColor(subColor.red(), subColor.green(), subColor.blue(), c.alpha());
                stops.push_back({ c, qBound(0.0, qint32(be32(p + q + 20)) / 32768.0, 1.0) });
            }
        }
    }
    const int addTag = tagAt("GradationSettingAdd0001");
    if(addTag >= 0){
        const int s = addTag + 23 * 2 + 4;       // past the section's size
        if(s + 16 <= n){ flat = be32(p + s) != 0; flatColor = QColor(p[s + 4], p[s + 8], p[s + 12]); }
    }
    int setTag = -1;
    for(int from = 0;;){ const int t = g.indexOf(QByteArray::fromHex("0047007200610064006100740069006f006e00530065007400740069006e0067"), from); if(t < 0) break;
        if(!(t + 32 + 2 <= n && p[t + 33] == 'A')){ setTag = t; break; } from = t + 2; }
    int repeat = 0, shape = 0; double ellipse = 100, rotation = 0; QPointF start(0, 0), end(W, H);
    if(setTag >= 0 && setTag + 32 + 68 <= n){
        const int s = setTag + 32;
        repeat = qint32(be32(p + s)); shape = qint32(be32(p + s + 4));
        ellipse = beF64(p + s + 20); rotation = beF64(p + s + 28);
        start = QPointF(beF64(p + s + 36), beF64(p + s + 44)); end = QPointF(beF64(p + s + 52), beF64(p + s + 60));
    }
    if(W <= 0 || H <= 0 || qint64(W) * H > kMaxPixels) return false;
    out = QImage(W, H, QImage::Format_ARGB32);
    if(out.isNull()) return false;
    if(flat || stops.isEmpty()){ out.fill(flat ? flatColor.rgba() : qRgba(0, 0, 0, 0)); return true; }
    std::sort(stops.begin(), stops.end(), [](const Stop& a, const Stop& b){ return a.pos < b.pos; });
    // A table is far cheaper than interpolating at every pixel.
    QVector<QRgb> lut(1025);
    for(int i = 0; i <= 1024; ++i){
        const double t = i / 1024.0;
        int k = 0; while(k + 1 < stops.size() && stops[k + 1].pos < t) ++k;
        const Stop& a = stops[k]; const Stop& b = k + 1 < stops.size() ? stops[k + 1] : stops[k];
        const double u = b.pos > a.pos ? qBound(0.0, (t - a.pos) / (b.pos - a.pos), 1.0) : (t >= b.pos ? 1.0 : 0.0);
        const auto mix = [&](int x, int y){ return int(x + (y - x) * u + 0.5); };
        lut[i] = qRgba(mix(a.c.red(), b.c.red()), mix(a.c.green(), b.c.green()), mix(a.c.blue(), b.c.blue()), mix(a.c.alpha(), b.c.alpha()));
    }
    const QPointF axis = end - start;
    const double len2 = qMax(1e-9, axis.x() * axis.x() + axis.y() * axis.y()), len = std::sqrt(len2);
    const double rot = qDegreesToRadians(rotation), cr = std::cos(rot), sr = std::sin(rot), squash = qMax(0.01, ellipse / 100.0);
    for(int y = 0; y < H; ++y){
        QRgb* d = reinterpret_cast<QRgb*>(out.scanLine(y));
        for(int x = 0; x < W; ++x){
            const double dx = x + 0.5 - start.x(), dy = y + 0.5 - start.y();
            double t;
            if(shape == 0) t = (dx * axis.x() + dy * axis.y()) / len2;
            else if(shape == 1) t = std::sqrt(dx * dx + dy * dy) / len;
            else { const double u = dx * cr + dy * sr, v = (-dx * sr + dy * cr) / squash; t = std::sqrt(u * u + v * v) / len; }
            bool blank = false;
            if(repeat == 1) t -= std::floor(t);
            else if(repeat == 2){ t = std::fmod(std::fabs(t), 2.0); if(t > 1) t = 2 - t; }
            else if(repeat == 3){ if(t < 0 || t > 1) blank = true; }
            d[x] = blank ? 0u : lut[int(qBound(0.0, t, 1.0) * 1024)];
        }
    }
    return true;
}

// Vector objects: a run of strokes, each a header then fixed-size points. Splines pass through their points.
QString vectorSvg(const QByteArray& b, int* count){
    const uchar* p = reinterpret_cast<const uchar*>(b.constData());
    const qint64 n = b.size();
    QString out; qint64 o = 0;
    while(o + 88 <= n){
        const qint32 hdr = qint32(be32(p + o)), mark = qint32(be32(p + o + 4)), ptSize = qint32(be32(p + o + 8));
        if(mark != 72 || hdr < 76 || ptSize < 88 || ptSize > 4096 || o + hdr > n) break;
        const qint32 np = qint32(be32(p + o + 16));
        const quint32 flags = be32(p + o + 20);
        if(np < 0 || o + hdr + qint64(np) * ptSize > n) break;
        const QColor main(p[o + 40], p[o + 44], p[o + 48]);
        const double opacity = qBound(0.0, beF64(p + o + 64), 1.0);
        double radius = 0;
        qint64 q = o + 76;
        if(flags & 1){ if(q + 8 <= o + hdr) radius = beF64(p + q); }
        else if(flags & 8){ if(q + 16 <= o + hdr) radius = beF64(p + q + 8); }
        struct Pt { QPointF at, c1, c2; bool corner; double width; };
        QVector<Pt> pts; pts.reserve(np);
        const qint64 first = o + hdr;
        for(int i = 0; i < np; ++i){
            const qint64 a = first + qint64(i) * ptSize;
            Pt t; t.at = QPointF(beF64(p + a), beF64(p + a + 8));
            t.corner = (be32(p + a + 32) & 1) != 0;
            t.width = qBound(0.0f, beF32(p + a + 56), 16.0f);
            if(ptSize >= 88 + 16) t.c1 = QPointF(beF64(p + a + 88), beF64(p + a + 96)); else t.c1 = t.at;
            if(ptSize >= 88 + 32) t.c2 = QPointF(beF64(p + a + 104), beF64(p + a + 112)); else t.c2 = t.c1;
            pts.push_back(t);
        }
        o = first + qint64(np) * ptSize;
        if(pts.size() < 2 || radius <= 0) continue;
        const bool closed = (flags & 256) != 0;
        QString d = QStringLiteral("M%1 %2").arg(numStr(pts[0].at.x()), numStr(pts[0].at.y()));
        const int segs = closed ? pts.size() : pts.size() - 1;
        const int m = pts.size();
        for(int i = 0; i < segs; ++i){
            const Pt& a = pts[i]; const Pt& z = pts[(i + 1) % m];
            if(flags & 128){
                // Catmull-Rom; a corner point has no tangent through it.
                const QPointF prev = (i > 0 || closed) ? pts[(i - 1 + m) % m].at : a.at;
                const QPointF next = (i + 2 < m || closed) ? pts[(i + 2) % m].at : z.at;
                const QPointF h1 = a.corner ? a.at : a.at + (z.at - prev) / 6.0;
                const QPointF h2 = z.corner ? z.at : z.at - (next - a.at) / 6.0;
                d += QStringLiteral(" C%1 %2 %3 %4 %5 %6").arg(numStr(h1.x()), numStr(h1.y()), numStr(h2.x()), numStr(h2.y()), numStr(z.at.x()), numStr(z.at.y()));
            } else if(flags & 64){
                d += QStringLiteral(" C%1 %2 %3 %4 %5 %6").arg(numStr(a.c1.x()), numStr(a.c1.y()), numStr(a.c2.x()), numStr(a.c2.y()), numStr(z.at.x()), numStr(z.at.y()));
            } else if(flags & 32){
                d += QStringLiteral(" Q%1 %2 %3 %4").arg(numStr(a.c1.x()), numStr(a.c1.y()), numStr(z.at.x()), numStr(z.at.y()));
            } else d += QStringLiteral(" L%1 %2").arg(numStr(z.at.x()), numStr(z.at.y()));
        }
        if(closed) d += QStringLiteral(" Z");
        // Pen pressure along the stroke's length.
        QString press; double total = 0; for(int i = 1; i < m; ++i) total += QLineF(pts[i-1].at, pts[i].at).length();
        bool varies = false; for(const Pt& t : pts) if(qAbs(t.width - pts[0].width) > 0.01) varies = true;
        if(varies && total > 0){
            double run = 0; QStringList kv;
            for(int i = 0; i < m; ++i){ if(i) run += QLineF(pts[i-1].at, pts[i].at).length(); kv << QStringLiteral("%1:%2").arg(numStr(run / total), numStr(pts[i].width)); }
            press = QStringLiteral(" xarq:pressure=\"%1\"").arg(kv.join(','));
        }
        out += QStringLiteral("<path d=\"%1\" fill=\"none\" stroke=\"%2\" stroke-width=\"%3\" stroke-linecap=\"round\" stroke-linejoin=\"round\"%4%5/>\n")
            .arg(d, main.name(), numStr(radius * (varies ? 1.0 : pts[0].width)), opacity < 1 ? QStringLiteral(" stroke-opacity=\"%1\"").arg(numStr(opacity)) : QString(), press);
        if(count) ++*count;
    }
    return out;
}

// One text object's little-endian attribute list: (id, size, payload)…
struct TextAttrs {
    QVector<std::tuple<int,int,int,QColor,QString>> runs;   // start, length, style, colour, font
    int align = 0, style = 0; double sizePt = 12, lineStep = 120, spacing = 0, hscale = 100; bool absSpacing = false;
    QColor color; QRectF bbox; double vscale = 100; QString font;
    QPointF quad[4]; bool hasQuad = false; double boxW = 0;
};
TextAttrs readTextAttrs(const QByteArray& b){
    TextAttrs t;
    const uchar* p = reinterpret_cast<const uchar*>(b.constData());
    int o = 0; const int n = b.size();
    while(o + 8 <= n){
        const int id = leI32(p + o), sz = leI32(p + o + 4);
        o += 8;
        if(sz < 0 || o + sz > n) break;
        const uchar* v = p + o;
        if(id == 11 && sz >= 4){
            const int c = leI32(v); int q = 4;
            for(int i = 0; i < c && i < 65536 && q + 12 <= sz; ++i){
                const int st = leI32(v + q), ln = leI32(v + q + 4), es = leI32(v + q + 8);
                if(es < 0 || q + 12 + es > sz) break;
                const uchar* e = v + q + 12;
                QColor col; QString fn; int style = 0;
                if(es >= 18){
                    style = e[0];
                    col = QColor(e[3], e[5], e[7]);
                    const int nl = e[16] | e[17] << 8;
                    if(nl > 0 && 18 + nl * 2 <= es){ for(int k = 0; k < nl; ++k) fn += QChar(ushort(e[18 + k*2] | e[19 + k*2] << 8)); }
                }
                t.runs.push_back({ st, ln, style, col, fn });
                q += 12 + es;
            }
        } else if(id == 13 && sz >= 4 + 12 + 18){
            const uchar* e = v + 16;
            const bool absolute = (e[0] | e[1] << 8) == 0 ? false : true;   // relative when the flag is set
            t.absSpacing = !absolute;
            t.lineStep = absolute ? leF64(e + 10) : leF64(e + 2);
        } else if(id == 14 && sz >= 4 + 12 + 8) t.spacing = leF64(v + 16);
        else if(id == 26 && sz >= 4 + 12 + 16){ t.hscale = leF64(v + 16); t.vscale = leF64(v + 24); }
        else if(id == 31) t.font = QString::fromUtf8(reinterpret_cast<const char*>(v), sz);
        else if(id == 32 && sz >= 4) t.sizePt = leI32(v) / 100.0;
        else if(id == 33 && sz >= 4) t.style = leI32(v);
        else if(id == 34 && sz >= 12) t.color = QColor(v[3], v[7], v[11]);
        else if(id == 35 && sz >= 4) t.align = leI32(v);
        else if(id == 42 && sz >= 16) t.bbox = QRectF(QPointF(leI32(v), leI32(v + 4)), QPointF(leI32(v + 8), leI32(v + 12)));
        else if(id == 63 && sz >= 8) t.boxW = leI32(v);
        else if(id == 64 && sz >= 32){ for(int i = 0; i < 4; ++i) t.quad[i] = QPointF(leI32(v + i*8) / 100.0, leI32(v + i*8 + 4) / 100.0); t.hasQuad = true; }
        o += sz;
    }
    return t;
}
QString textSvg(const QByteArray& text, const TextAttrs& t, double dpi){
    if(t.bbox.isEmpty()) return QString();
    const double px = qMax(0.1, t.sizePt * dpi / 72.0);
    QString s = QString::fromUtf8(text); s.remove(QLatin1Char('\r'));
    const QStringList lines = s.split(QLatin1Char('\n'));
    const double step = t.absSpacing ? qMax(0.1, px + t.lineStep * dpi / 72.0) : px * t.lineStep / 100.0;   // an absolute value adds to the font's height
    const QString anchor = t.align == 2 ? QStringLiteral("middle") : t.align == 1 ? QStringLiteral("end") : QStringLiteral("start");
    const QPointF c = t.bbox.center();
    QFont font(t.font); font.setPixelSize(qMax(1, qRound(px)));
    const double ascent = QFontMetricsF(font).ascent();
    const bool vertical = (t.style & 16) != 0;
    const QString fam = xmlEsc(t.font);
    const QString weight = (t.style & 1) ? QStringLiteral("bold") : QStringLiteral("normal");
    const QString italic = (t.style & 2) ? QStringLiteral("italic") : QStringLiteral("normal");
    const QString col = t.color.isValid() ? t.color.name() : QStringLiteral("#000000");
    QString out;
    if(vertical){      // columns run top to bottom, right to left
        double colX = t.bbox.right() - px / 2;
        for(const QString& line : lines){
            double y = t.bbox.top() + ascent;
            for(const QChar ch : line){
                out += QStringLiteral("<text x=\"%1\" y=\"%2\" text-anchor=\"middle\" font-family=\"%3\" font-size=\"%4\" font-weight=\"%5\" font-style=\"%6\" fill=\"%7\">%8</text>\n")
                    .arg(numStr(colX), numStr(y), fam, numStr(px), weight, italic, col, xmlEsc(QString(ch)));
                y += px;
            }
            colX -= step;
        }
    } else {
        // Lines are centred on the box vertically, so a font of different metrics stays put.
        const double block = ascent + step * (lines.size() - 1);
        double y = c.y() - block / 2 + ascent;
        const double x = t.align == 2 ? c.x() : t.align == 1 ? t.bbox.right() : t.bbox.left();
        const QString track = t.spacing != 0 ? QStringLiteral(" letter-spacing=\"%1\"").arg(numStr(t.spacing * dpi / 72.0)) : QString();
        for(const QString& line : lines){
            if(!line.isEmpty())
                out += QStringLiteral("<text xml:space=\"preserve\" x=\"%1\" y=\"%2\" text-anchor=\"%3\" font-family=\"%4\" font-size=\"%5\" font-weight=\"%6\" font-style=\"%7\" fill=\"%8\"%9>%10</text>\n")
                    .arg(numStr(x), numStr(y), anchor, fam, numStr(px), weight, italic, col).arg(track, xmlEsc(line));
            y += step;
        }
    }
    if(out.isEmpty()) return QString();
    // The quad is the text's box after rotation and skew; only its shape matters here, about the box's centre.
    QTransform shape;
    if(t.hasQuad && t.boxW > 0 && t.bbox.height() > 0){
        const QPointF u = (t.quad[1] - t.quad[0]) / t.boxW, v = (t.quad[3] - t.quad[0]) / t.bbox.height();
        shape = QTransform(u.x(), u.y(), v.x(), v.y(), 0, 0);
    }
    if(!shape.isIdentity() || t.hscale != 100 || t.vscale != 100){
        const double ax = t.align == 2 ? c.x() : t.align == 1 ? t.bbox.right() : t.bbox.left();
        QTransform m; m.translate(c.x(), c.y()); m = shape * m; m.translate(ax - c.x(), 0); m.scale(t.hscale / 100.0, t.vscale / 100.0); m.translate(-ax, -c.y());
        return QStringLiteral("<g transform=\"matrix(%1 %2 %3 %4 %5 %6)\">\n%7</g>\n").arg(numStr(m.m11()), numStr(m.m12()), numStr(m.m21()), numStr(m.m22()), numStr(m.dx()), numStr(m.dy()), out);
    }
    return out;
}

// A correction layer's effect as a per-pixel table or function, from FilterLayerInfo.
struct Correction {
    QVector<int> lr, lg, lb;      // channel tables, when it's table-shaped
    QVector<QRgb> gmap;           // a gradient map, by grey level
    int hue = 0, sat = 0, lig = 0; bool hsl = false;
    bool valid() const { return hsl || lr.size() == 256 || gmap.size() == 256; }
    QRgb apply(QRgb c) const {
        if(gmap.size() == 256){ const QRgb m = gmap[qGray(c)]; return qRgba(qRed(m), qGreen(m), qBlue(m), qAlpha(c) * qAlpha(m) / 255); }
        if(lr.size() == 256) return qRgba(lr[qRed(c)], lg[qGreen(c)], lb[qBlue(c)], qAlpha(c));
        QColor h = QColor::fromRgba(c).toHsl();
        const int H = (h.hslHue() < 0 ? 0 : h.hslHue()) + hue;
        const int S = qBound(0, h.hslSaturation() + sat * 255 / 100, 255), L = qBound(0, h.lightness() + lig * 255 / 100, 255);
        QColor o = QColor::fromHsl(((H % 360) + 360) % 360, S, L, qAlpha(c));
        return o.rgba();
    }
};
bool readCorrection(const QByteArray& b, Correction& c){
    const uchar* p = reinterpret_cast<const uchar*>(b.constData());
    const int n = b.size();
    if(n < 4) return false;
    const int type = qint32(be32(p));
    const auto i32 = [&](int o){ return o + 4 <= n ? qint32(be32(p + o)) : 0; };
    const auto u8hi = [&](int o){ return o + 2 <= n ? int(p[o]) : 0; };     // a 16-bit value kept in its high byte
    const auto table = [&](const std::function<int(int)>& f){ c.lr.resize(256); for(int i = 0; i < 256; ++i) c.lr[i] = qBound(0, f(i), 255); c.lg = c.lr; c.lb = c.lr; };
    switch(type){
    case 1: {                      // brightness and contrast, -100…100
        const int br = i32(8), co = i32(12);
        const double k = co >= 0 ? 1.0 + co / 50.0 : 1.0 + co / 100.0;
        table([=](int v){ return qRound((v - 127.5) * k + 127.5 + br * 255.0 / 200.0); });
        return true;
    }
    case 2: {                      // levels: master then red, green, blue, five values each
        const auto level = [&](int o, QVector<int>& t, const QVector<int>* then){
            const int inL = u8hi(o), mid = u8hi(o + 2), inR = u8hi(o + 4), outL = u8hi(o + 6), outR = u8hi(o + 8);
            t.resize(256);
            const double gamma = (inR > inL && mid > inL && mid < inR) ? std::log(0.5) / std::log(double(mid - inL) / (inR - inL)) : 1.0;
            for(int i = 0; i < 256; ++i){
                const double u = inR > inL ? qBound(0.0, double(i - inL) / (inR - inL), 1.0) : (i >= inR ? 1.0 : 0.0);
                int v = qBound(0, qRound(outL + std::pow(u, 1.0 / gamma) * (outR - outL)), 255);
                t[i] = then ? (*then)[v] : v;
            }
        };
        if(n < 8 + 40) return false;
        QVector<int> rgb; level(8, rgb, nullptr);
        level(18, c.lr, &rgb); level(28, c.lg, &rgb); level(38, c.lb, &rgb);
        return true;
    }
    case 3: {                      // tone curves: master then red, green, blue
        int o = 8;
        QVector<QVector<int>> curves;
        for(int k = 0; k < 4; ++k){
            if(o + 2 + 32 * 4 > n) return false;
            const int count = qBound(0, int(p[o] << 8 | p[o + 1]), 32);
            o += 2;
            QVector<QPointF> pts;
            for(int i = 0; i < count; ++i) pts.push_back(QPointF(p[o + i*4], p[o + i*4 + 2]));
            o += 32 * 4;
            if(pts.size() < 2) pts = { QPointF(0, 0), QPointF(255, 255) };
            std::sort(pts.begin(), pts.end(), [](const QPointF& a, const QPointF& b){ return a.x() < b.x(); });
            QVector<int> t(256);
            for(int i = 0; i < 256; ++i){       // piecewise-linear between the user's points
                int s = 0; while(s + 1 < pts.size() && pts[s + 1].x() < i) ++s;
                const QPointF a = pts[s], z = pts[qMin(s + 1, pts.size() - 1)];
                const double u = z.x() > a.x() ? qBound(0.0, (i - a.x()) / (z.x() - a.x()), 1.0) : 0.0;
                t[i] = qBound(0, qRound(a.y() + (z.y() - a.y()) * u), 255);
            }
            curves.push_back(t);
        }
        c.lr.resize(256); c.lg.resize(256); c.lb.resize(256);
        for(int i = 0; i < 256; ++i){ c.lr[i] = curves[0][curves[1][i]]; c.lg[i] = curves[0][curves[2][i]]; c.lb[i] = curves[0][curves[3][i]]; }
        return true;
    }
    case 4: c.hue = i32(8); c.sat = i32(12); c.lig = i32(16); c.hsl = true; return true;
    case 5: {                      // colour balance: cyan-red, magenta-green, yellow-blue for shadows, midtones, highlights
        if(n < 12 + 36) return false;
        const auto chan = [&](int k, QVector<int>& t){
            t.resize(256);
            for(int i = 0; i < 256; ++i){
                const double l = i / 255.0, sh = qMax(0.0, 1 - 2 * l), hi = qMax(0.0, 2 * l - 1), mi = 1 - sh - hi;
                t[i] = qBound(0, qRound(i + (i32(12 + k*4) * sh + i32(24 + k*4) * mi + i32(36 + k*4) * hi) * 127.0 / 100.0), 255);
            }
        };
        chan(0, c.lr); chan(1, c.lg); chan(2, c.lb);
        return true;
    }
    case 6: table([](int v){ return 255 - v; }); return true;
    case 7: { const int lv = qBound(2, i32(8), 256); table([=](int v){ return (v * lv / 256) * 255 / (lv - 1); }); return true; }
    case 8: { const int th = i32(8); table([=](int v){ return v >= th ? 255 : 0; }); return true; }
    case 9: {                      // gradient map: the same stop records as a gradient layer
        const int count = i32(24);
        if(count < 1 || count > 4096 || 32 + count * 28 > n) return false;
        struct St { QColor col; double pos; };
        QVector<St> st;
        for(int i = 0; i < count; ++i){ const int q = 32 + i * 28; st.push_back({ QColor(p[q], p[q + 4], p[q + 8], p[q + 12]), qBound(0.0, i32(q + 20) / 32768.0, 1.0) }); }
        std::sort(st.begin(), st.end(), [](const St& a, const St& z){ return a.pos < z.pos; });
        c.gmap.resize(256);
        for(int i = 0; i < 256; ++i){
            const double t = i / 255.0;
            int k = 0; while(k + 1 < st.size() && st[k + 1].pos < t) ++k;
            const St& a = st[k]; const St& z = st[qMin(k + 1, st.size() - 1)];
            const double u = z.pos > a.pos ? qBound(0.0, (t - a.pos) / (z.pos - a.pos), 1.0) : 0.0;
            const auto mix = [&](int x, int y){ return int(x + (y - x) * u + 0.5); };
            c.gmap[i] = qRgba(mix(a.col.red(), z.col.red()), mix(a.col.green(), z.col.green()), mix(a.col.blue(), z.col.blue()), mix(a.col.alpha(), z.col.alpha()));
        }
        return true;
    }
    default: return false;
    }
}
// Bake a correction into every bitmap below it in the same folder, through its mask where it has one.
void applyCorrection(QVector<Shape>& list, const Correction& c, const QImage* mask, int mx, int my, int depth){
    for(Shape& s : list){
        if(s.type == ShapeType::Group){ if(depth < 64) applyCorrection(s.children, c, mask, mx, my, depth + 1); continue; }
        if(s.fill.type != FillType::Bitmap || s.fill.image.isNull()) continue;
        QImage im = s.fill.image.convertToFormat(QImage::Format_ARGB32);
        const QPointF at = s.transform.map(QPointF(0, 0));
        const int X = qRound(at.x()), Y = qRound(at.y());
        for(int y = 0; y < im.height(); ++y){
            QRgb* d = reinterpret_cast<QRgb*>(im.scanLine(y));
            for(int x = 0; x < im.width(); ++x){
                if(!qAlpha(d[x])) continue;
                const QRgb o = c.apply(d[x]);
                int m = 255;
                if(mask){ const int u = X + x - mx, v = Y + y - my; m = (u >= 0 && v >= 0 && u < mask->width() && v < mask->height()) ? qAlpha(mask->pixel(u, v)) : 255; }
                if(m == 255) d[x] = o;
                else if(m > 0){ const auto mix = [&](int a, int z){ return a + (z - a) * m / 255; }; d[x] = qRgba(mix(qRed(d[x]), qRed(o)), mix(qGreen(d[x]), qGreen(o)), mix(qBlue(d[x]), qBlue(o)), qAlpha(d[x])); }
            }
        }
        s.fill.image = im; s.fill.origImage = im;
    }
}

struct Builder {
    const Clip& c; ClipImportStats* stats; QSet<qint64> seen; double W, H, dpi;
    QHash<qint64,QVector<QByteArray>> vectorsByLayer;

    // "Layer colour": when on, it replaces the layer's black and white.
    static void layerColors(const SqliteMini::Row& L, QColor& mainC, QColor& subC){
        if(!L.value("DrawColorEnable").toInt()) return;
        const auto top = [&](const char* k){ return int((L.value(k).toLongLong() >> 24) & 255); };
        mainC = QColor(top("DrawColorMainRed"), top("DrawColorMainGreen"), top("DrawColorMainBlue"));
        subC = L.contains("DrawColorSubRed") ? QColor(top("DrawColorSubRed"), top("DrawColorSubGreen"), top("DrawColorSubBlue")) : QColor(255, 255, 255);
    }
    static void recolor(QImage& img, const QColor& a, const QColor& b){
        for(int y = 0; y < img.height(); ++y){
            QRgb* d = reinterpret_cast<QRgb*>(img.scanLine(y));
            for(int x = 0; x < img.width(); ++x){
                const int v = qGray(d[x]);
                const auto mix = [&](int p, int q){ return p + (q - p) * v / 255; };
                d[x] = qRgba(mix(a.red(), b.red()), mix(a.green(), b.green()), mix(a.blue(), b.blue()), qAlpha(d[x]));
            }
        }
    }
    QVector<Shape> svgShapes(const QString& body){
        const QString svg = QStringLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xarq=\"urn:xarq\" width=\"%1\" height=\"%2\" viewBox=\"0 0 %1 %2\">\n%3</svg>\n").arg(W).arg(H).arg(body);
        return parseSvg(svg.toUtf8());
    }
    QByteArray extaBytes(const QByteArray& id) const {
        const auto it = c.exta.constFind(id);
        if(it == c.exta.constEnd()) return QByteArray();
        return QByteArray(reinterpret_cast<const char*>(c.base + it->off), int(it->len));
    }

    // Multiply a layer's alpha by its mask, in canvas coordinates; beyond the mask's bitmap it's all open or
    // all shut according to its initial colour, which readOffscreen gave the bitmap's own fill.
    void applyMask(QImage& img, int X, int Y, const QImage& mask, int mx, int my, bool outside){
        for(int y = 0; y < img.height(); ++y){
            QRgb* d = reinterpret_cast<QRgb*>(img.scanLine(y));
            for(int x = 0; x < img.width(); ++x){
                const int u = X + x - mx, v = Y + y - my;
                const int m = (u >= 0 && v >= 0 && u < mask.width() && v < mask.height()) ? qAlpha(mask.pixel(u, v)) : (outside ? 255 : 0);
                d[x] = (d[x] & 0x00ffffffu) | quint32(qAlpha(d[x]) * m / 255) << 24;
            }
        }
    }

    QVector<Shape> siblings(qint64 first, int depth){
        QVector<Shape> out;
        QImage clipBase; int baseX = 0, baseY = 0; bool haveBase = false;
        for(qint64 id = first; id > 0;){
            if(seen.contains(id) || depth > 64) break;
            seen.insert(id);
            const SqliteMini::Row L = c.layers.value(id);
            const qint64 next = L.value("LayerNextIndex").toLongLong();
            if(L.isEmpty()) break;
            const int type = L.value("LayerType").toInt();
            const int vis = L.value("LayerVisibility").toInt();
            const int masking = L.value("LayerMasking").toInt();
            const bool folder = (L.value("LayerFolder").toInt() & 1) != 0;
            const bool isClip = L.value("LayerClip").toInt() != 0;
            const int comp = L.value("LayerComposite").toInt();
            const double opacity = qBound(0.0, L.value("LayerOpacity", 256).toDouble() / 256.0, 1.0);
            const QString name = L.value("LayerName").toString();
            const int ox = L.value("LayerOffsetX").toInt(), oy = L.value("LayerOffsetY").toInt();
            Shape s; bool ok = false;
            if(folder){
                QVector<Shape> kids = siblings(L.value("LayerFirstChildIndex").toLongLong(), depth + 1);
                if(vectorsByLayer.contains(id)){       // a comic frame's border belongs to its folder and sits on top
                    QString body; int count = 0;
                    for(const QByteArray& ext : vectorsByLayer.value(id)) body += vectorSvg(extaBytes(ext), &count);
                    if(!body.isEmpty()){ kids += svgShapes(QStringLiteral("<g transform=\"translate(%1 %2)\">\n%3</g>\n").arg(ox).arg(oy).arg(body)); if(stats) stats->vectorStrokes += count; }
                }
                if(!kids.isEmpty()){
                    s.type = ShapeType::Group; s.name = name; s.children = kids;
                    s.isolate = (masking & 32) == 0 || comp != 0 || opacity < 1;   // CSP's "pass through" folders set bit 32
                    ok = true;
                    if(stats) ++stats->folders;
                }
            } else if(type & 4096){
                Correction cor;
                if((vis & 1) && readCorrection(L.value("FilterLayerInfo").toByteArray(), cor) && cor.valid()){
                    const qint64 mm = L.value("LayerLayerMaskMipmap").toLongLong();
                    QImage mask;
                    const bool hasMask = mm && (masking & 1) && readOffscreen(c, mm, mask, true);
                    applyCorrection(out, cor, hasMask ? &mask : nullptr, ox + L.value("LayerMaskOffsetX").toInt() + L.value("LayerMaskOffscrOffsetX").toInt(),
                                    oy + L.value("LayerMaskOffsetY").toInt() + L.value("LayerMaskOffscrOffsetY").toInt(), 0);
                    if(stats) ++stats->corrections;
                } else if((vis & 1) && stats) ++stats->skippedCorrections;
            } else {
                QImage img;
                const qint64 mip = L.value("LayerRenderMipmap").toLongLong();
                const bool paper = (type & 1584) == 1584;
                int X = ox + L.value("LayerRenderOffscrOffsetX").toInt(), Y = oy + L.value("LayerRenderOffscrOffsetY").toInt();
                if(mip && readOffscreen(c, mip, img, false)){
                    if(!paper){ QColor mc, sc; layerColors(L, mc, sc); if(mc.isValid()) recolor(img, mc, sc); }
                    const qint64 mm = L.value("LayerLayerMaskMipmap").toLongLong();
                    if(mm && (masking & 1)){
                        QImage mask;
                        if(readOffscreen(c, mm, mask, true)){
                            const int mx = ox + L.value("LayerMaskOffsetX").toInt() + L.value("LayerMaskOffscrOffsetX").toInt();
                            const int my = oy + L.value("LayerMaskOffsetY").toInt() + L.value("LayerMaskOffscrOffsetY").toInt();
                            applyMask(img, X, Y, mask, mx, my, false);
                            if(stats) ++stats->masks;
                        }
                    }
                    if(isClip && haveBase){
                        applyMask(img, X, Y, clipBase, baseX, baseY, false);
                        if(stats) ++stats->clipped;
                    } else if(!isClip){ clipBase = img; baseX = X; baseY = Y; haveBase = true; }
                    s = psdBitmapShape(img, X, Y, name);
                    ok = true;
                    if(stats){
                        if(paper) ++stats->paper; else ++stats->pixelLayers;
                    }
                } else if(L.contains("GradationFillInfo")){
                    QColor mainC, subC;
                    layerColors(L, mainC, subC);
                    if(renderGradation(L.value("GradationFillInfo").toByteArray(), mainC, subC, int(W), int(H), img)){
                        const qint64 mm = L.value("LayerLayerMaskMipmap").toLongLong();
                        if(mm && (masking & 1)){
                            QImage mask;
                            if(readOffscreen(c, mm, mask, true)){
                                const int mx = ox + L.value("LayerMaskOffsetX").toInt() + L.value("LayerMaskOffscrOffsetX").toInt();
                                const int my = oy + L.value("LayerMaskOffsetY").toInt() + L.value("LayerMaskOffscrOffsetY").toInt();
                                applyMask(img, 0, 0, mask, mx, my, false);
                                if(stats) ++stats->masks;
                            }
                        }
                        s = psdBitmapShape(img, 0, 0, name);
                        ok = true;
                        if(stats) ++stats->fills;
                    }
                } else if(L.contains("ResizableOriginalMipmap")){
                    QImage original;
                    const QByteArray info = L.value("ResizableImageInfo").toByteArray();
                    if(readOffscreen(c, L.value("ResizableOriginalMipmap").toLongLong(), original, false) && info.size() >= 64 && original.width() > 0 && original.height() > 0){
                        // The last 64 bytes are the placed image's four corners: top left, top right, bottom left, bottom right.
                        const uchar* e = reinterpret_cast<const uchar*>(info.constData()) + info.size() - 64;
                        const QPointF tl(beF64(e), beF64(e + 8)), tr(beF64(e + 16), beF64(e + 24)), bl(beF64(e + 32), beF64(e + 40));
                        s = psdBitmapShape(original, 0, 0, name);
                        const QPointF br = tr + bl - tl;
                        if(qint32(be32(reinterpret_cast<const uchar*>(info.constData()) + 24)) != 0){    // repeats over the canvas
                            s.rect = QRectF(0, 0, W, H);
                            s.fill.tiled = true;
                            s.fill.p0 = bl + QPointF(ox, oy); s.fill.p1 = br + QPointF(ox, oy); s.fill.p2 = tl + QPointF(ox, oy);
                            s.transform = QTransform();
                        } else
                            s.transform = QTransform((tr.x() - tl.x()) / original.width(), (tr.y() - tl.y()) / original.width(),
                                                     (bl.x() - tl.x()) / original.height(), (bl.y() - tl.y()) / original.height(), tl.x() + ox, tl.y() + oy);
                        ok = true;
                        if(stats) ++stats->placedImages;
                    }
                } else if(L.contains("TextLayerAttributes")){
                    QString body = textSvg(L.value("TextLayerString").toByteArray(), readTextAttrs(L.value("TextLayerAttributes").toByteArray()), dpi);
                    // More text objects on the same layer, as two length-prefixed lists.
                    const QByteArray sa = L.value("TextLayerStringArray").toByteArray(), aa = L.value("TextLayerAttributesArray").toByteArray();
                    int sp = 0, ap = 0;
                    const uchar* su = reinterpret_cast<const uchar*>(sa.constData()); const uchar* au = reinterpret_cast<const uchar*>(aa.constData());
                    while(sp + 4 <= sa.size() && ap + 4 <= aa.size()){
                        const qint64 sl = le32(su + sp), al = le32(au + ap);
                        if(sp + 4 + sl > sa.size() || ap + 4 + al > aa.size()) break;
                        body += textSvg(sa.mid(sp + 4, int(sl)), readTextAttrs(aa.mid(ap + 4, int(al))), dpi);
                        sp += 4 + int(sl); ap += 4 + int(al);
                    }
                    if(!body.isEmpty()){
                        const QVector<Shape> shapes = svgShapes(QStringLiteral("<g transform=\"translate(%1 %2)\">\n%3</g>\n").arg(ox).arg(oy).arg(body));
                        if(!shapes.isEmpty()){
                            if(shapes.size() == 1){ s = shapes[0]; s.name = name; }
                            else { s.type = ShapeType::Group; s.name = name; s.children = shapes; }
                            ok = true;
                            if(stats) stats->textObjects += shapes.size();
                        }
                    }
                } else if(vectorsByLayer.contains(id)){
                    QString body; int count = 0;
                    for(const QByteArray& ext : vectorsByLayer.value(id)) body += vectorSvg(extaBytes(ext), &count);
                    if(!body.isEmpty()){
                        const QVector<Shape> shapes = svgShapes(QStringLiteral("<g transform=\"translate(%1 %2)\">\n%3</g>\n").arg(ox).arg(oy).arg(body));
                        if(!shapes.isEmpty()){
                            s.type = ShapeType::Group; s.name = name; s.children = shapes; s.isolate = opacity < 1;
                            ok = true;
                            if(stats) stats->vectorStrokes += count;
                        }
                    }
                } else if(paper && L.value("DrawColorEnable").toInt()){
                    img = QImage(qMax(1, int(W)), qMax(1, int(H)), QImage::Format_ARGB32);
                    const auto top = [&](const char* k){ return int((L.value(k).toLongLong() >> 24) & 255); };
                    img.fill(qRgb(top("DrawColorMainRed"), top("DrawColorMainGreen"), top("DrawColorMainBlue")));
                    s = psdBitmapShape(img, 0, 0, name);
                    ok = true;
                    if(stats) ++stats->paper;
                }
            }
            if(ok && L.value("LayerEffectAttached").toInt() && s.fill.type == FillType::Bitmap && !s.fill.image.isNull()){
                // A screen tone: dots whose coverage follows the shade. Drawn as the even shade they average to.
                QImage tone = s.fill.image.convertToFormat(QImage::Format_ARGB32);
                for(int y = 0; y < tone.height(); ++y){
                    QRgb* d = reinterpret_cast<QRgb*>(tone.scanLine(y));
                    for(int x = 0; x < tone.width(); ++x) d[x] = qRgba(0, 0, 0, qAlpha(d[x]) * (255 - qGray(d[x])) / 255);
                }
                s.fill.image = tone; s.fill.origImage = tone;
                if(stats) ++stats->tones;
            }
            if(ok){
                s.opacity = opacity;
                s.visible = (vis & 1) != 0;
                psdApplyBlend(s, blendKey(comp));
                out.push_back(s);
            }
            id = next;
        }
        return out;
    }
};

} // namespace

Document importClipDocument(const QByteArray& d, ClipImportStats* stats){
    Document doc;
    if(d.size() < 24 || std::memcmp(d.constData(), "CSFCHUNK", 8) != 0) return doc;
    Clip c; c.base = reinterpret_cast<const uchar*>(d.constData()); c.size = d.size();
    qint64 o = 24;
    QByteArray dbBytes;
    while(o + 16 <= c.size){
        const quint64 len = be64(c.base + o + 8);
        if(len > quint64(c.size - o - 16)) break;
        if(std::memcmp(c.base + o, "CHNKExta", 8) == 0 && len >= 56){
            const quint64 idLen = be64(c.base + o + 16);
            if(idLen == 40){
                const QByteArray id(reinterpret_cast<const char*>(c.base + o + 24), 40);
                const quint64 body = be64(c.base + o + 64);
                if(body <= len - 56) c.exta.insert(id, Span{ o + 72, qint64(body) });
            }
        } else if(std::memcmp(c.base + o, "CHNKSQLi", 8) == 0){
            dbBytes = d.mid(int(o + 16), int(len));
        }
        o += 16 + qint64(len);
    }
    SqliteMini db;
    if(!db.open(dbBytes)) return doc;
    const QVector<SqliteMini::Row> canvases = db.rows("Canvas");
    if(canvases.isEmpty()) return doc;
    const SqliteMini::Row canvas = canvases.first();
    const double W = canvas.value("CanvasWidth").toDouble(), H = canvas.value("CanvasHeight").toDouble();
    if(!(W >= 1 && H >= 1) || W * H > double(kMaxPixels)) return doc;
    auto index = [&](const char* table, QHash<qint64,SqliteMini::Row>& into){
        for(const SqliteMini::Row& r : db.rows(QLatin1String(table))) into.insert(r.value("MainId").toLongLong(), r); };
    index("Layer", c.layers); index("Mipmap", c.mipmaps); index("MipmapInfo", c.infos); index("Offscreen", c.offscreens);
    const qint64 root = canvas.value("CanvasRootFolder").toLongLong();
    const SqliteMini::Row rootRow = c.layers.value(root);
    if(rootRow.isEmpty()) return doc;
    Builder b{ c, stats, {}, W, H, qMax(1.0, canvas.value("CanvasResolution", 72.0).toDouble()), {} };
    for(const SqliteMini::Row& r : db.rows("VectorObjectList")) b.vectorsByLayer[r.value("LayerId").toLongLong()].push_back(r.value("VectorData").toByteArray());
    b.seen.insert(root);
    doc.pageSize = QSizeF(W, H);
    doc.shapes = b.siblings(rootRow.value("LayerFirstChildIndex").toLongLong(), 0);
    return doc;
}

// The picture the application saved for this file's own thumbnail.
QImage clipPreview(const QByteArray& d){
    if(d.size() < 24 || std::memcmp(d.constData(), "CSFCHUNK", 8) != 0) return QImage();
    const uchar* base = reinterpret_cast<const uchar*>(d.constData());
    qint64 o = 24;
    while(o + 16 <= d.size()){
        const quint64 len = be64(base + o + 8);
        if(len > quint64(d.size() - o - 16)) return QImage();
        if(std::memcmp(base + o, "CHNKSQLi", 8) == 0){
            SqliteMini db;
            if(!db.open(d.mid(int(o + 16), int(len)))) return QImage();
            const QVector<SqliteMini::Row> rows = db.rows("CanvasPreview");
            QImage best;
            for(const SqliteMini::Row& r : rows){
                QImage im; im.loadFromData(r.value("ImageData").toByteArray());
                if(!im.isNull() && im.width() > best.width()) best = im;
            }
            return best;
        }
        o += 16 + qint64(len);
    }
    return QImage();
}
} // namespace xara
