// Krita (.kra): a ZIP holding maindoc.xml (the layer tree), each paint layer's pixels in Krita's tile
// format, vector layers as SVG and a flattened mergedimage.png. A tile file is a short text header
// (VERSION, TILEWIDTH, TILEHEIGHT, PIXELSIZE, DATA n) and then n tiles, each a "x,y,LZF,size" line and
// `size` bytes: a flag (1 LZF-compressed, 0 stored), then the tile with each byte of the pixel stored as
// its own plane. Integer colour spaces keep blue first, float ones red first.
#include "prepress.h"
#include "icc.h"
#include "svgimport.h"
#include <QHash>
#include <QImage>
#include <QRegularExpression>
#include <QXmlStreamReader>
#include <cmath>
#include <cstring>
#include <functional>
#include <memory>

namespace xara {
namespace {

const qint64 kMaxPixels = qint64(1) << 28;

bool lzf(const uchar* s, qint64 n, uchar* d, qint64 cap){
    qint64 i = 0, o = 0;
    while(i < n){
        const int c = s[i++];
        if(c < 32){
            const int len = c + 1;
            if(i + len > n || o + len > cap) return false;
            std::memcpy(d + o, s + i, size_t(len)); i += len; o += len;
        } else {
            int len = c >> 5;
            if(len == 7){ if(i >= n) return false; len += s[i++]; }
            if(i >= n) return false;
            const qint64 ref = o - ((c & 31) << 8) - 1 - s[i++];
            len += 2;
            if(ref < 0 || o + len > cap) return false;
            for(int k = 0; k < len; ++k) d[o + k] = d[ref + k];
            o += len;
        }
    }
    return o == cap;
}

// The colour space as Krita names it: channel count, bytes per channel, float, blue first, CMYK.
struct Space { int ch = 0, bpc = 1; bool flt = false, bgr = false, cmyk = false, grey = false; };
Space spaceOf(const QString& name){
    Space s; const QString n = name.toUpper();
    if(n.startsWith("RGBA")){ s.ch = 4; s.bgr = true; }
    else if(n.startsWith("GRAYA")){ s.ch = 2; s.grey = true; }
    else if(n.startsWith("CMYK")){ s.ch = 5; s.cmyk = true; }
    else return s;
    const QString d = n.mid(n.startsWith("RGBA") ? 4 : n.startsWith("GRAYA") ? 5 : (n.startsWith("CMYKA") ? 5 : 4));
    if(d.isEmpty() || d == "U8") s.bpc = 1;
    else if(d == "16" || d == "U16" || d == "AU16") s.bpc = 2;
    else if(d == "F16"){ s.bpc = 2; s.flt = true; }
    else if(d == "F32"){ s.bpc = 4; s.flt = true; }
    else { s.ch = 0; return s; }
    if(s.flt) s.bgr = false;
    return s;
}
inline float chan(const uchar* p, const Space& s){
    if(s.bpc == 1) return p[0] / 255.0f;
    if(s.bpc == 2){
        const quint16 v = quint16(p[0] | p[1] << 8);
        if(!s.flt) return v / 65535.0f;
        const int e = (v >> 10) & 31, m = v & 1023; float f;
        if(e == 0) f = std::ldexp(float(m), -24); else if(e == 31) f = m ? 0.0f : 65504.0f; else f = std::ldexp(float(m | 1024), e - 25);
        return (v & 0x8000) ? -f : f;
    }
    float f; std::memcpy(&f, p, 4); return std::isfinite(f) ? f : 0.0f;
}
inline uchar enc(float v, bool linear){
    v = qBound(0.0f, v, 1.0f);
    if(linear) v = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1 / 2.4f) - 0.055f;
    return uchar(v * 255.0f + 0.5f);
}
QRgb pixelOf(const uchar* p, const Space& s, bool linear){
    const int b = s.bpc;
    if(s.ch == 4){
        const float a = chan(p + 3 * b, s);
        const float c0 = chan(p, s), c1 = chan(p + b, s), c2 = chan(p + 2 * b, s);
        return s.bgr ? qRgba(enc(c2, linear), enc(c1, linear), enc(c0, linear), enc(a, false))
                     : qRgba(enc(c0, linear), enc(c1, linear), enc(c2, linear), enc(a, false));
    }
    if(s.ch == 2){ const uchar g = enc(chan(p, s), linear); return qRgba(g, g, g, enc(chan(p + b, s), false)); }
    // CMYK, without a colour-managed conversion.
    const float c = chan(p, s), m = chan(p + b, s), y = chan(p + 2 * b, s), k = chan(p + 3 * b, s), a = chan(p + 4 * b, s);
    return qRgba(enc((1 - c) * (1 - k), false), enc((1 - m) * (1 - k), false), enc((1 - y) * (1 - k), false), enc(a, false));
}

// A tile file decoded onto a canvas-sized area: `bytes` per pixel, the layer's own origin at (ox, oy).
struct Tiles { int pixelSize = 0, tw = 0, th = 0; QVector<QPoint> at; QVector<QByteArray> data; };
bool readTiles(const QByteArray& f, Tiles& t, int* bad){
    qint64 p = 0; int count = -1;
    for(int guard = 0; guard < 16 && p < f.size(); ++guard){
        const qint64 e = f.indexOf('\n', p); if(e < 0) return false;
        const QByteArray line = f.mid(p, e - p); p = e + 1;
        const int sp = line.indexOf(' '); const QByteArray k = line.left(sp), v = line.mid(sp + 1);
        if(k == "TILEWIDTH") t.tw = v.toInt(); else if(k == "TILEHEIGHT") t.th = v.toInt(); else if(k == "PIXELSIZE") t.pixelSize = v.toInt();
        else if(k == "DATA"){ count = v.toInt(); break; }
    }
    if(count < 0 || t.tw <= 0 || t.th <= 0 || t.tw > 1024 || t.th > 1024 || t.pixelSize <= 0 || t.pixelSize > 40) return false;
    const qint64 tileBytes = qint64(t.tw) * t.th * t.pixelSize;
    QByteArray buf(int(tileBytes), '\0');
    for(int i = 0; i < count && i < 1000000; ++i){
        const qint64 e = f.indexOf('\n', p); if(e < 0) return false;
        const QList<QByteArray> parts = f.mid(p, e - p).split(',');
        p = e + 1;
        if(parts.size() < 4) return false;
        const qint64 size = parts[3].toLongLong();
        if(size < 1 || size > f.size() - p) return false;
        const uchar* s = reinterpret_cast<const uchar*>(f.constData()) + p;
        bool ok;
        if(s[0] == 1) ok = lzf(s + 1, size - 1, reinterpret_cast<uchar*>(buf.data()), tileBytes);
        else { ok = size - 1 >= tileBytes; if(ok) std::memcpy(buf.data(), s + 1, size_t(tileBytes)); }
        p += size;
        const int tx = parts[0].toInt(), ty = parts[1].toInt();
        if(!ok || qAbs(tx) > 10000000 || qAbs(ty) > 10000000){ if(bad) ++*bad; continue; }
        // Planes back to interleaved pixels.
        QByteArray px(int(tileBytes), '\0');
        const int n = t.tw * t.th;
        uchar* D = reinterpret_cast<uchar*>(px.data()); const uchar* B = reinterpret_cast<const uchar*>(buf.constData());
        for(int c = 0; c < t.pixelSize; ++c) for(int j = 0; j < n; ++j) D[j * t.pixelSize + c] = B[c * n + j];
        t.at.push_back(QPoint(tx, ty)); t.data.push_back(px);
    }
    return true;
}

QString compositeKey(const QString& op){
    static const QHash<QString, QString> m = {
        {"normal","norm"},{"multiply","mul "},{"screen","scrn"},{"overlay","over"},{"darken","dark"},{"lighten","lite"},
        {"dodge","div "},{"burn","idiv"},{"add","lddg"},{"linear_dodge","lddg"},{"subtract","fsub"},{"difference","diff"},
        {"exclusion","smud"},{"hard_light","hLit"},{"soft_light","sLit"},{"soft_light_svg","sLit"},{"divide","fdiv"},
        {"linear_burn","lbrn"},{"vivid_light","vLit"},{"pin_light","pLit"},{"linear_light","lLit"},{"hard_mix","hMix"},
        {"hard_mix_photoshop","hMix"},{"hue","hue "},{"saturation","sat "},{"color","colr"},{"luminize","lum "},
        {"darker color","dkCl"},{"lighter color","lgCl"} };
    return m.value(op);
}

struct Ctx {
    const QMap<QString, QByteArray>& zip; QString base; int W = 0, H = 0; bool linear = false; KraImportStats* st = nullptr;
    bool srgb = true;                                     // the image's profile is sRGB (or linear sRGB)
    // Clone layers show another layer's content; a second pass places them, reusing what the first decoded.
    mutable QHash<QString, Shape> built; mutable bool clones = false; bool resolving = false;
    QByteArray file(const QString& name) const {
        auto it = zip.find(base + QStringLiteral("/layers/") + name);
        if(it != zip.end()) return it.value();
        const QString tail = QStringLiteral("/layers/") + name;
        for(auto i = zip.begin(); i != zip.end(); ++i) if(i.key().endsWith(tail)) return i.value();
        return QByteArray();
    }
};

// A paint layer's pixels over the canvas, its tiles placed at their own coordinates plus the layer's offset.
QImage paintImage(const Ctx& c, const QString& file, const Space& sp, int ox, int oy, QPoint* origin){
    Tiles t; int bad = 0;
    const QByteArray f = c.file(file);
    KraImportStats* st = c.resolving ? nullptr : c.st;     // the clone pass doesn't count again
    if(f.isEmpty() || !readTiles(f, t, &bad)){ if(st) st->badTiles += bad + 1; return QImage(); }
    if(st) st->badTiles += bad;
    const int ps = sp.ch * sp.bpc;
    if(t.pixelSize != ps){ if(st) ++st->unsupportedColour; return QImage(); }
    const QByteArray def = c.file(file + QStringLiteral(".defaultpixel"));
    const QRgb defPx = def.size() >= ps ? pixelOf(reinterpret_cast<const uchar*>(def.constData()), sp, c.linear) : 0;
    QRect r;
    if(qAlpha(defPx)) r = QRect(-ox, -oy, c.W, c.H);           // a default pixel that shows fills the canvas
    for(const QPoint& p : t.at) r |= QRect(p, QSize(t.tw, t.th));
    if(r.isEmpty() || qint64(r.width()) * r.height() > kMaxPixels) return QImage();
    QImage img(r.size(), QImage::Format_ARGB32);
    if(img.isNull()) return img;
    img.fill(defPx);
    // CMYK, and integer RGB or grey in another profile, go through the layer's own profile when lcms2 is there.
    const int comps = sp.cmyk ? 4 : sp.grey ? 1 : 3;
    std::unique_ptr<IccToSrgb> icc;
    if(!sp.flt && (sp.cmyk || !c.srgb)){
        const QByteArray prof = c.file(file + QStringLiteral(".icc"));
        if(!prof.isEmpty()){ icc.reset(new IccToSrgb(prof, comps)); if(!icc->ok()) icc.reset(); }
    }
    QByteArray in, out;
    if(icc){ in.resize(t.tw * comps); out.resize(t.tw * 3); }
    for(int i = 0; i < t.at.size(); ++i){
        const uchar* s = reinterpret_cast<const uchar*>(t.data[i].constData());
        for(int y = 0; y < t.th; ++y){
            QRgb* d = reinterpret_cast<QRgb*>(img.scanLine(t.at[i].y() - r.top() + y)) + (t.at[i].x() - r.left());
            if(icc){
                uchar* I = reinterpret_cast<uchar*>(in.data());
                for(int x = 0; x < t.tw; ++x){
                    const uchar* p = s + (y * t.tw + x) * ps;
                    for(int k = 0; k < comps; ++k){
                        const int ch = (!sp.cmyk && !sp.grey) ? 2 - k : k;   // integer RGB is stored blue first
                        I[x * comps + k] = sp.bpc == 2 ? p[ch * 2 + 1] : p[ch];
                    }
                }
                icc->convert(I, reinterpret_cast<uchar*>(out.data()), t.tw);
                const uchar* O = reinterpret_cast<const uchar*>(out.constData());
                for(int x = 0; x < t.tw; ++x){
                    const uchar* p = s + (y * t.tw + x) * ps;
                    const uchar a = enc(chan(p + (sp.ch - 1) * sp.bpc, sp), false);
                    d[x] = qRgba(O[x * 3], O[x * 3 + 1], O[x * 3 + 2], a);
                }
            } else for(int x = 0; x < t.tw; ++x) d[x] = pixelOf(s + (y * t.tw + x) * ps, sp, c.linear);
        }
    }
    *origin = r.topLeft() + QPoint(ox, oy);
    return img;
}
// A selection (mask) as coverage over the canvas.
QImage selectionImage(const Ctx& c, const QString& file, int ox, int oy){
    Tiles t; int bad = 0;
    const QByteArray f = c.file(file);
    if(f.isEmpty() || !readTiles(f, t, &bad) || t.pixelSize != 1) return QImage();
    const QByteArray def = c.file(file + QStringLiteral(".defaultpixel"));
    QImage m(c.W, c.H, QImage::Format_Grayscale8);
    if(m.isNull()) return m;
    m.fill(def.isEmpty() ? 0 : uchar(def[0]));
    for(int i = 0; i < t.at.size(); ++i){
        const uchar* s = reinterpret_cast<const uchar*>(t.data[i].constData());
        for(int y = 0; y < t.th; ++y){
            const int yy = t.at[i].y() + oy + y; if(yy < 0 || yy >= c.H) continue;
            uchar* d = m.scanLine(yy);
            for(int x = 0; x < t.tw; ++x){ const int xx = t.at[i].x() + ox + x; if(xx >= 0 && xx < c.W) d[xx] = s[y * t.tw + x]; }
        }
    }
    return m;
}

struct Item { Shape shape; bool keep = false; };

Item readLayer(QXmlStreamReader& r, const Ctx& c, int depth);

// <layers>…</layers>: topmost first in the file, returned bottom first.
QVector<Shape> readLayers(QXmlStreamReader& r, const Ctx& c, int depth){
    QVector<Shape> out;
    while(r.readNextStartElement()){
        if(r.name() == QLatin1String("layer")){ const Item it = readLayer(r, c, depth); if(it.keep) out.prepend(it.shape); }
        else r.skipCurrentElement();
    }
    return out;
}

Item readLayer(QXmlStreamReader& r, const Ctx& c, int depth){
    const QXmlStreamAttributes a = r.attributes();
    const QString type = a.value("nodetype").toString(), file = a.value("filename").toString();
    const QString name = a.value("name").toString();
    const int ox = qBound(-10000000, a.value("x").toInt(), 10000000), oy = qBound(-10000000, a.value("y").toInt(), 10000000);
    Item it;
    const QString uuid = a.value("uuid").toString();
    // A group is put together again (it may hold clones); anything else decoded in the first pass is reused.
    const bool cached = c.resolving && type != QLatin1String("grouplayer") && !uuid.isEmpty() && c.built.contains(uuid);
    QVector<Shape> kids; QImage mask; int filters = 0;
    while(r.readNextStartElement()){
        if(r.name() == QLatin1String("layers") && depth < 64) kids = readLayers(r, c, depth + 1);
        else if(r.name() == QLatin1String("masks") && !cached){
            while(r.readNextStartElement()){
                const QXmlStreamAttributes ma = r.attributes();
                const QString mt = ma.value("nodetype").toString();
                if(mt == QLatin1String("transparencymask") && ma.value("visible") != QLatin1String("0")){
                    const QImage m = selectionImage(c, ma.value("filename").toString() + QStringLiteral(".pixelselection"),
                                                    qBound(-10000000, ma.value("x").toInt(), 10000000), qBound(-10000000, ma.value("y").toInt(), 10000000));
                    if(!m.isNull()){ mask = m; if(c.st && !c.resolving) ++c.st->masks; }
                } else if(mt == QLatin1String("filtermask") || mt == QLatin1String("colorizemask") || mt == QLatin1String("transformmask")) ++filters;
                r.skipCurrentElement();
            }
        } else r.skipCurrentElement();
    }
    if(c.st && !c.resolving) c.st->filters += filters;
    Shape s;
    if(cached) s = c.built.value(uuid);
    else if(type == QLatin1String("clonelayer")){
        if(!c.resolving){ c.clones = true; return it; }
        const auto src = c.built.find(a.value("clonefromuuid").toString());
        if(src == c.built.end()){ if(c.st) ++c.st->otherLayers; return it; }
        s = src.value(); s.name = name;
        s.transform = s.transform * QTransform::fromTranslate(ox, oy);
    } else if(type == QLatin1String("paintlayer")){
        const Space sp = spaceOf(a.value("colorspacename").toString());
        if(sp.ch == 0){ if(c.st && !c.resolving) ++c.st->unsupportedColour; return it; }
        QPoint at; QImage img = paintImage(c, file, sp, ox, oy, &at);
        if(img.isNull()) return it;
        if(!mask.isNull()){
            for(int y = 0; y < img.height(); ++y){
                const int my = at.y() + y; QRgb* d = reinterpret_cast<QRgb*>(img.scanLine(y));
                const uchar* m = (my >= 0 && my < c.H) ? mask.constScanLine(my) : nullptr;
                for(int x = 0; x < img.width(); ++x){ const int mx = at.x() + x; const int v = (m && mx >= 0 && mx < c.W) ? m[mx] : 0; d[x] = (d[x] & 0xffffffu) | quint32(qAlpha(d[x]) * v / 255) << 24; }
            }
        }
        s = psdBitmapShape(img, at.x(), at.y(), name);
        if(c.st && !c.resolving) ++c.st->layers;
    } else if(type == QLatin1String("grouplayer")){
        s.type = ShapeType::Group; s.name = name; s.children = kids;
        if(!mask.isNull()){ s.isolate = true; psdDocMask(s, mask, QRect(0, 0, c.W, c.H)); }
        if(c.st && !c.resolving) ++c.st->groups;
    } else if(type == QLatin1String("shapelayer")){
        const QByteArray svg = c.file(file + QStringLiteral(".shapelayer/content.svg"));
        QVector<Shape> shapes = svg.isEmpty() ? QVector<Shape>() : parseSvg(svg);
        if(shapes.isEmpty()) return it;
        // The SVG spans the canvas, in points; the parser's own units for that span give the scale to pixels.
        const QSizeF vp = svgViewportSize(svg);
        const double sc = vp.width() > 0 ? c.W / vp.width() : 1.0;
        // The parser maps the viewBox onto the page through each shape's transform, which line widths don't
        // follow; they're widened by that mapping here (the group's own scale does reach them).
        const QRegularExpressionMatch vb = QRegularExpression(QStringLiteral("viewBox=\"\\s*[-\\d.]+[ ,]+[-\\d.]+[ ,]+([\\d.]+)")).match(QString::fromUtf8(svg.left(4096)));
        const double vbw = vb.hasMatch() ? vb.captured(1).toDouble() : 0.0;
        if(vbw > 0 && vp.width() > 0 && qAbs(vp.width() / vbw - 1.0) > 1e-6){
            const double k = vp.width() / vbw;
            std::function<void(QVector<Shape>&)> widen = [&](QVector<Shape>& v){ for(Shape& sh : v){ sh.stroke.width *= k; widen(sh.children); } };
            widen(shapes);
        }
        s.type = ShapeType::Group; s.name = name; s.children = shapes;
        s.transform = QTransform::fromScale(sc, sc) * QTransform::fromTranslate(ox, oy);
        if(c.st && !c.resolving) ++c.st->vectorLayers;
    } else {
        if(c.st && !c.resolving) ++c.st->otherLayers;  // file, fill, adjustment and reference layers
        return it;
    }
    if(!cached && !uuid.isEmpty() && type != QLatin1String("clonelayer")) c.built.insert(uuid, s);
    const QString op = a.value("compositeop").toString();
    QString key = compositeKey(op);
    const bool pass = type == QLatin1String("grouplayer") && a.value("passthrough") == QLatin1String("1");
    const bool erase = op == QLatin1String("erase");
    if(key.isEmpty() && !erase){ key = QStringLiteral("norm"); if(c.st && !op.isEmpty()) ++c.st->unknownModes; }
    if(erase) s.blend = QPainter::CompositionMode_DestinationOut;      // takes away what's under it in its group
    else psdApplyBlend(s, key.toLatin1());
    s.opacity = qBound(0, a.value("opacity").toInt(), 255) / 255.0;
    if(!a.hasAttribute("opacity")) s.opacity = 1.0;
    s.visible = a.value("visible") != QLatin1String("0");
    if(type == QLatin1String("grouplayer")) s.isolate = !pass || s.opacity < 0.999;     // Krita composites a group on its own unless it passes through
    if(type == QLatin1String("shapelayer")) s.isolate = s.opacity < 0.999;
    it.shape = s; it.keep = true;
    return it;
}

} // namespace

Document importKraDocument(const QMap<QString, QByteArray>& zip, KraImportStats* st){
    Document doc;
    const QByteArray xml = zip.value(QStringLiteral("maindoc.xml"));
    if(xml.isEmpty()) return doc;
    QXmlStreamReader r(xml);
    while(r.readNextStartElement()){
        if(r.name() == QLatin1String("DOC")) continue;
        if(r.name() != QLatin1String("IMAGE")){ r.skipCurrentElement(); continue; }
        const QXmlStreamAttributes a = r.attributes();
        Ctx c{ zip, a.value("name").toString(), a.value("width").toInt(), a.value("height").toInt(), false, st };
        if(c.W <= 0 || c.H <= 0 || qint64(c.W) * c.H > kMaxPixels) return doc;
        const QString prof = a.value("profile").toString();
        c.linear = prof.contains(QLatin1String("g10")) || prof.contains(QLatin1String("linear"), Qt::CaseInsensitive) || prof.contains(QLatin1String("scRGB"));
        c.srgb = prof.isEmpty() || prof.contains(QLatin1String("sRGB"), Qt::CaseInsensitive) || prof.contains(QLatin1String("gray built-in"), Qt::CaseInsensitive);
        // High dynamic range encodings aren't something an 8-bit sRGB layer can hold.
        if(prof.contains(QLatin1String("2084")) || prof.contains(QLatin1String("HLG")) || prof.contains(QLatin1String(" PQ"))){ if(st) ++st->unsupportedColour; return doc; }
        doc.pageSize = QSizeF(c.W, c.H);
        while(r.readNextStartElement()){
            if(r.name() == QLatin1String("layers")) doc.shapes = readLayers(r, c, 0);
            else r.skipCurrentElement();
        }
        if(c.clones){
            // Everything a clone can show has been decoded now; read the tree again to place the clones.
            c.resolving = true;
            QXmlStreamReader r2(xml);
            while(r2.readNextStartElement()) if(r2.name() == QLatin1String("layers")){ doc.shapes = readLayers(r2, c, 0); break; } else if(r2.name() != QLatin1String("DOC") && r2.name() != QLatin1String("IMAGE")) r2.skipCurrentElement();
        }
        break;
    }
    return doc;
}
} // namespace xara
