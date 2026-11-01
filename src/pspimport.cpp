// PaintShop Pro (.psp, .pspimage, .tub, …) → a Document of layers. Written from the published PSP file
// format specification; the vector shape layout was checked against real files. A file is a signature, a
// version, then "~BK\0" blocks; version 3 and earlier give each block an extra initial-chunk length.
#include "prepress.h"
#include "svgimport.h"
#include <QImage>
#include <QVector>
#include <zlib.h>
#include <cstring>

namespace xara {
namespace {

const qint64 kMaxPixels = qint64(1) << 28;

struct Rd {
    const uchar* p; qint64 n;
    bool has(qint64 o, qint64 len) const { return o >= 0 && len >= 0 && o + len <= n; }
    int u8(qint64 o) const { return has(o, 1) ? p[o] : 0; }
    int u16(qint64 o) const { return has(o, 2) ? p[o] | p[o+1] << 8 : 0; }
    quint32 u32(qint64 o) const { return has(o, 4) ? quint32(p[o]) | quint32(p[o+1]) << 8 | quint32(p[o+2]) << 16 | quint32(p[o+3]) << 24 : 0; }
    qint32 i32(qint64 o) const { return qint32(u32(o)); }
    double f64(qint64 o) const { double d = 0; if(has(o, 8)) std::memcpy(&d, p + o, 8); return qIsFinite(d) ? d : 0.0; }
};
struct Block { int id = -1; quint32 init = 0, total = 0; qint64 body = 0; qint64 end() const { return body + total; } };

enum { BImage = 0, BColor = 2, BLayerBank = 3, BLayer = 4, BChannel = 5, BVector = 13, BShape = 14, BPaint = 15 };

struct Ctx {
    Rd r; int major = 0;
    int width = 0, height = 0, compression = 0, depth = 0; bool gray = false;
    int bytesPerSample = 1;
    QVector<QRgb> palette;
    PspImportStats* stats = nullptr;
};

bool readBlock(const Ctx& c, qint64 o, qint64 limit, Block& b){
    const int hdr = c.major < 4 ? 14 : 10;
    if(o + hdr > limit || !c.r.has(o, hdr) || std::memcmp(c.r.p + o, "~BK\0", 4) != 0) return false;
    b.id = c.r.u16(o + 4);
    if(c.major < 4){ b.init = c.r.u32(o + 6); b.total = c.r.u32(o + 10); }
    else b.total = c.r.u32(o + 6);
    b.body = o + hdr;
    return b.end() <= limit;
}

// Channel bytes, `rows * rowBytes` long. Bad data leaves zeroes in whatever wasn't reached.
QByteArray decodeChannel(const Ctx& c, qint64 o, qint64 limit, quint32 compressed, qint64 outSize){
    QByteArray out(int(outSize), '\0');
    uchar* q = reinterpret_cast<uchar*>(out.data());
    const qint64 avail = qMin<qint64>(limit - o, compressed ? qint64(compressed) : limit - o);
    if(avail <= 0) return out;
    const uchar* s = c.r.p + o;
    if(c.compression == 0){
        std::memcpy(q, s, size_t(qMin<qint64>(avail, outSize)));
    } else if(c.compression == 1){
        qint64 si = 0, di = 0;
        while(si < avail && di < outSize){
            const int run = s[si++];
            if(run > 128){
                const int n = run - 128;
                if(si >= avail) break;
                const int v = s[si++];
                const qint64 take = qMin<qint64>(n, outSize - di);
                std::memset(q + di, v, size_t(take)); di += take;
            } else {
                const qint64 take = qMin<qint64>(qMin<qint64>(run, avail - si), outSize - di);
                std::memcpy(q + di, s + si, size_t(take)); di += take; si += run;
            }
        }
    } else if(c.compression == 2){
        z_stream z; std::memset(&z, 0, sizeof z);
        if(inflateInit(&z) == Z_OK){
            z.next_in = const_cast<Bytef*>(s); z.avail_in = uInt(avail);
            z.next_out = q; z.avail_out = uInt(outSize);
            inflate(&z, Z_FINISH);
            inflateEnd(&z);
        }
    }
    return out;
}

struct LayerInfo {
    QString name; int type = 0; qint32 ir[4] = {0,0,0,0}, sr[4] = {0,0,0,0}, mr[4] = {0,0,0,0}, smr[4] = {0,0,0,0};
    int opacity = 255, blend = 0; bool visible = true, maskDisabled = false;
    qint64 subs = 0;      // where the sub-blocks start
};

QByteArray blendKey(int b){
    static const char* const keys[] = { "norm","dark","lite","hue ","sat ","colr","lum ","mul ","scrn","norm","over","hLit","sLit","diff","div ","idiv","smud","hue ","sat ","colr","lum " };
    return (b >= 0 && b < int(sizeof keys / sizeof *keys)) ? QByteArray(keys[b], 4) : QByteArray("norm");
}

bool readLayerInfo(const Ctx& c, const Block& b, LayerInfo& L){
    const Rd& r = c.r; qint64 o = b.body;
    if(c.major >= 4){
        const quint32 chunk = r.u32(o); const int nameLen = r.u16(o + 4);
        if(!r.has(o + 6, nameLen)) return false;
        L.name = QString::fromLatin1(reinterpret_cast<const char*>(r.p + o + 6), nameLen);
        o += 6 + nameLen;
        L.subs = b.body + chunk;
    } else {
        if(!r.has(o, 256)) return false;
        L.name = QString::fromLatin1(reinterpret_cast<const char*>(r.p + o), int(qstrnlen(reinterpret_cast<const char*>(r.p + o), 256)));
        o += 256;
        L.subs = b.body + b.init;
    }
    if(!r.has(o, 71)) return false;
    L.type = r.u8(o++);
    for(int i = 0; i < 4; ++i) L.ir[i] = r.i32(o + 4*i);   o += 16;
    for(int i = 0; i < 4; ++i) L.sr[i] = r.i32(o + 4*i);   o += 16;
    L.opacity = r.u8(o); L.blend = r.u8(o + 1); L.visible = (r.u8(o + 2) & 1) != 0; o += 5;
    for(int i = 0; i < 4; ++i) L.mr[i] = r.i32(o + 4*i);   o += 16;
    for(int i = 0; i < 4; ++i) L.smr[i] = r.i32(o + 4*i);  o += 16;
    L.maskDisabled = r.u8(o + 1) != 0;
    return true;
}

// Newer files put a raster-extension block before the chunk holding the bitmap and channel counts; both it
// and that chunk come before the channels, and the channels are the first blocks that matter.
qint64 firstChannelBlock(const Ctx& c, const LayerInfo& L, qint64 end){
    qint64 o = L.subs;
    if(c.major < 4) return o;
    for(int guard = 0; guard < 8 && c.r.has(o, 4); ++guard){
        if(std::memcmp(c.r.p + o, "~BK\0", 4) == 0){
            Block b; if(!readBlock(c, o, end, b)) return end;
            if(b.id == BChannel || b.id == BVector) return o;
            o = b.end();
        } else {
            const quint32 len = c.r.u32(o);
            if(len < 4) return end;
            o += len;
        }
    }
    return o;
}

// Sample i of a plane, as 8 bits. 16-bit samples are little-endian.
inline int sample8(const uchar* pl, qint64 i, int bps){ return bps == 2 ? pl[i*2 + 1] : pl[i]; }

Shape rasterLayer(const Ctx& c, const LayerInfo& L, const Block& b, bool& ok){
    Shape s; ok = false;
    const int w = L.sr[2] - L.sr[0], h = L.sr[3] - L.sr[1];
    if(w <= 0 || h <= 0 || w > 262144 || h > 262144 || qint64(w) * h > kMaxPixels) return s;
    const int bps = c.bytesPerSample;
    const bool sub8 = c.depth < 8;
    const qint64 rowBytes = sub8 ? qint64((w * c.depth + 7) / 8 + 3) / 4 * 4 : qint64(w) * bps;
    const qint64 planeSize = rowBytes * h;

    QImage img(w, h, QImage::Format_ARGB32);
    if(img.isNull()) return s;
    img.fill(qRgba(0, 0, 0, 255));
    bool any = false; QByteArray maskPlane; int maskW = 0, maskH = 0;

    qint64 o = firstChannelBlock(c, L, b.end());
    Block ch;
    while(readBlock(c, o, b.end(), ch)){
        o = ch.end();
        if(ch.id != BChannel) continue;
        const qint64 hdr = ch.body + (c.major >= 4 ? 4 : 0);
        const quint32 chunk = c.major >= 4 ? c.r.u32(ch.body) : ch.init;
        if(chunk < 12 || !c.r.has(hdr, 12)) continue;
        const quint32 comp = c.r.u32(hdr);
        const int bmpType = c.r.u16(hdr + 8), chType = c.r.u16(hdr + 10);
        const qint64 data = ch.body + chunk;
        if(bmpType == 2){        // the layer's mask: its own rectangle, one 8-bit plane
            maskW = L.smr[2] - L.smr[0]; maskH = L.smr[3] - L.smr[1];
            if(maskW > 0 && maskH > 0 && qint64(maskW) * maskH <= kMaxPixels){
                Ctx m = c; m.bytesPerSample = 1;
                maskPlane = decodeChannel(m, data, ch.end(), comp, qint64(maskW) * maskH);
            }
            continue;
        }
        if(bmpType > 1) continue;
        const QByteArray plane = decodeChannel(c, data, ch.end(), comp, planeSize);
        const uchar* pl = reinterpret_cast<const uchar*>(plane.constData());
        any = true;
        if(bmpType == 1){        // transparency mask
            for(int y = 0; y < h; ++y){
                QRgb* d = reinterpret_cast<QRgb*>(img.scanLine(y));
                for(int x = 0; x < w; ++x) d[x] = (d[x] & 0x00ffffffu) | quint32(sample8(pl, qint64(y)*w + x, bps)) << 24;
            }
        } else if(c.depth >= 24){
            const int shift = chType == 1 ? 16 : chType == 2 ? 8 : chType == 3 ? 0 : -1;
            if(shift < 0) continue;
            for(int y = 0; y < h; ++y){
                QRgb* d = reinterpret_cast<QRgb*>(img.scanLine(y));
                for(int x = 0; x < w; ++x) d[x] = (d[x] & ~(0xffu << shift)) | quint32(sample8(pl, qint64(y)*w + x, bps)) << shift;
            }
        } else if(c.gray){
            for(int y = 0; y < h; ++y){
                QRgb* d = reinterpret_cast<QRgb*>(img.scanLine(y));
                for(int x = 0; x < w; ++x){ const int v = sample8(pl, qint64(y)*w + x, bps); d[x] = (d[x] & 0xff000000u) | qRgb(v, v, v); }
            }
        } else {                 // palette: 1, 4 or 8 bits, sub-byte rows padded to four bytes
            for(int y = 0; y < h; ++y){
                QRgb* d = reinterpret_cast<QRgb*>(img.scanLine(y));
                const uchar* row = pl + rowBytes * y;
                for(int x = 0; x < w; ++x){
                    int idx;
                    if(c.depth == 8) idx = row[x];
                    else if(c.depth == 4) idx = (row[x / 2] >> (x & 1 ? 0 : 4)) & 15;
                    else idx = (row[x / 8] >> (7 - (x & 7))) & 1;
                    const QRgb pc = idx < c.palette.size() ? c.palette[idx] : qRgb(0, 0, 0);
                    d[x] = (d[x] & 0xff000000u) | (pc & 0x00ffffffu);
                }
            }
        }
    }
    if(!any) return s;

    const int X = L.ir[0] + L.sr[0], Y = L.ir[1] + L.sr[1];
    if(!maskPlane.isEmpty() && !L.maskDisabled){
        const int mx = L.mr[0] + L.smr[0] - X, my = L.mr[1] + L.smr[1] - Y;
        const uchar* pl = reinterpret_cast<const uchar*>(maskPlane.constData());
        for(int y = 0; y < h; ++y){
            QRgb* d = reinterpret_cast<QRgb*>(img.scanLine(y));
            for(int x = 0; x < w; ++x){
                const int u = x - mx, v = y - my;
                const int m = (u >= 0 && v >= 0 && u < maskW && v < maskH) ? pl[qint64(v) * maskW + u] : 0;
                d[x] = (d[x] & 0x00ffffffu) | quint32(qAlpha(d[x]) * m / 255) << 24;
            }
        }
    }
    s = psdBitmapShape(img, X, Y, L.name);
    ok = true;
    return s;
}

// A paint-style block: flags, then a chunk per kind of paint. Only flat colour is decoded.
bool readPaint(const Ctx& c, const Block& b, QColor& out, bool& known){
    const quint32 chunk = c.r.u32(b.body);
    const int type = c.r.u16(b.body + 4);
    known = type == 0 || (type & 1) != 0;
    if(!(type & 1)) return false;
    const qint64 o = b.body + chunk;
    const quint32 len = c.r.u32(o);
    if(len < 8 || !c.r.has(o + 4, 4)) return false;
    out = QColor(c.r.u8(o + 4), c.r.u8(o + 5), c.r.u8(o + 6));      // red, green, blue, reserved
    return true;
}

QString svgNum(double v){ return QString::number(v, 'g', 12); }

// One polyline shape. Nodes are a point and its two bézier handles, in canvas coordinates.
bool readPolyline(const Ctx& c, const Block& b, qint64 o, QString& svg){
    const Rd& r = c.r;
    const quint32 attr = r.u32(o);
    if(attr < 49 || !r.has(o, attr)) return false;
    const bool stroked = r.u8(o + 4) != 0, filled = r.u8(o + 5) != 0;
    const double width = r.f64(o + 7);
    const int startCap = r.u8(o + 15), join = attr >= 60 ? r.u8(o + 51) : 1;
    o += attr;

    QColor strokeC, fillC; bool hasStroke = false, hasFill = false; int paints = 0; bool unknownPaint = false;
    Block sub;
    while(readBlock(c, o, b.end(), sub) && (sub.id == BPaint || sub.id == 19)){
        if(sub.id == BPaint){
            bool known = true;
            if(paints == 0) hasStroke = readPaint(c, sub, strokeC, known);
            else if(paints == 1) hasFill = readPaint(c, sub, fillC, known);
            if(!known) unknownPaint = true;
            ++paints;
        }
        o = sub.end();
    }
    if(unknownPaint && c.stats) ++c.stats->unsupportedPaints;
    const quint32 def = r.u32(o);
    if(def < 8) return false;
    const quint32 nodes = r.u32(o + 4);
    o += def;
    if(nodes == 0 || nodes > 2000000) return false;

    QString d; QPointF prevOut; bool open = false; QPointF startP, startIn;
    for(quint32 i = 0; i < nodes; ++i){
        const quint32 len = r.u32(o);
        if(len < 51 || !r.has(o, len)) return false;
        const QPointF p(r.f64(o + 4), r.f64(o + 12)), in(r.f64(o + 20), r.f64(o + 28)), out(r.f64(o + 36), r.f64(o + 44));
        const bool move = r.u8(o + 52) != 0;
        const int flags = r.u16(o + 53);
        if(!open || move){
            if(open) d += QStringLiteral("Z ");
            d += QStringLiteral("M%1 %2 ").arg(svgNum(p.x()), svgNum(p.y()));
            startP = p; startIn = in; open = true;
        } else {
            d += QStringLiteral("C%1 %2 %3 %4 %5 %6 ").arg(svgNum(prevOut.x()), svgNum(prevOut.y()), svgNum(in.x()), svgNum(in.y()), svgNum(p.x()), svgNum(p.y()));
        }
        prevOut = out;
        if(flags & 0x80){      // closes the contour back to its first node
            d += QStringLiteral("C%1 %2 %3 %4 %5 %6 Z ").arg(svgNum(out.x()), svgNum(out.y()), svgNum(startIn.x()), svgNum(startIn.y()), svgNum(startP.x()), svgNum(startP.y()));
            open = false;
        }
        o += len;
    }
    QString attrs;
    attrs += hasFill && filled ? QStringLiteral(" fill=\"%1\"").arg(fillC.name()) : QStringLiteral(" fill=\"none\"");
    if(hasStroke && stroked && width > 0){
        attrs += QStringLiteral(" stroke=\"%1\" stroke-width=\"%2\"").arg(strokeC.name(), svgNum(width));
        attrs += startCap == 1 ? QStringLiteral(" stroke-linecap=\"round\"") : startCap == 2 ? QStringLiteral(" stroke-linecap=\"square\"") : QString();
        attrs += join == 1 ? QStringLiteral(" stroke-linejoin=\"round\"") : join == 2 ? QStringLiteral(" stroke-linejoin=\"bevel\"") : QStringLiteral(" stroke-linejoin=\"miter\"");
    } else attrs += QStringLiteral(" stroke=\"none\"");
    svg += QStringLiteral("<path d=\"%1\"%2/>\n").arg(d, attrs);
    return true;
}

void readShapes(const Ctx& c, qint64 o, qint64 end, QString& svg, int depth){
    Block b;
    while(depth < 32 && readBlock(c, o, end, b)){
        o = b.end();
        if(b.id != BShape) continue;
        const quint32 chunk = c.r.u32(b.body);
        const int nameLen = c.r.u16(b.body + 4);
        const int type = c.r.u16(b.body + 6 + nameLen);
        const quint32 flags = c.r.u32(b.body + 8 + nameLen);
        if(!(flags & 4)) continue;   // hidden
        const qint64 body = b.body + chunk;
        if(type == 2){
            if(!readPolyline(c, b, body, svg) && c.stats) ++c.stats->skippedShapes;
        } else if(type == 5){        // a vector group: a count chunk, then its shapes
            const quint32 cl = c.r.u32(body);
            if(cl >= 4) readShapes(c, body + cl, b.end(), svg, depth + 1);
        } else if(c.stats){
            if(type == 1) ++c.stats->skippedText; else ++c.stats->skippedShapes;
        }
    }
}

} // namespace

Document importPspDocument(const QByteArray& d, PspImportStats* stats){
    Document doc;
    static const char sig[] = "Paint Shop Pro Image File\n\x1a";
    if(d.size() < 36 || std::memcmp(d.constData(), sig, sizeof sig - 1) != 0) return doc;
    Ctx c; c.r = Rd{ reinterpret_cast<const uchar*>(d.constData()), d.size() };
    c.major = c.r.u16(32);
    c.stats = stats;
    qint64 o = 36;
    Block b; bool haveImage = false;
    QVector<Shape> layers;
    while(readBlock(c, o, c.r.n, b)){
        o = b.end();
        if(b.id == BImage){
            const qint64 q = b.body + (c.major >= 4 ? 4 : 0);
            c.width = c.r.i32(q); c.height = c.r.i32(q + 4);
            c.compression = c.r.u16(q + 17);
            c.depth = c.r.u16(q + 19);
            c.gray = c.r.u8(q + 27) != 0;
            c.bytesPerSample = (c.depth == 48 || c.depth == 16) ? 2 : 1;
            if(c.width <= 0 || c.height <= 0 || c.width > 262144 || c.height > 262144 || qint64(c.width) * c.height > kMaxPixels) return doc;
            if(c.compression > 2) return doc;
            const bool okDepth = c.depth == 24 || c.depth == 48 || c.depth == 1 || c.depth == 4 || c.depth == 8 || (c.depth == 16 && c.gray);
            if(!okDepth) return doc;
            haveImage = true;
        } else if(b.id == BColor){
            qint64 q = b.body; quint32 count;
            if(c.major >= 4){ const quint32 chunk = c.r.u32(q); count = c.r.u32(q + 4); q += chunk; }
            else { count = c.r.u32(q); q += 4; }
            if(count <= 65536 && c.r.has(q, qint64(count) * 4))
                for(quint32 i = 0; i < count; ++i) c.palette.push_back(qRgb(c.r.u8(q + i*4 + 2), c.r.u8(q + i*4 + 1), c.r.u8(q + i*4)));
        } else if(b.id == BLayerBank && haveImage){
            qint64 q = b.body; Block lb;
            while(readBlock(c, q, b.end(), lb)){
                q = lb.end();
                if(lb.id != BLayer) continue;
                LayerInfo L;
                if(!readLayerInfo(c, lb, L)) continue;
                Shape s; bool ok = false;
                if(L.type == 0 || L.type == 1 || L.type == 2) s = rasterLayer(c, L, lb, ok);
                else if(L.type == 3){
                    qint64 v = firstChannelBlock(c, L, lb.end()); Block vb;
                    QString body;
                    while(readBlock(c, v, lb.end(), vb)){
                        v = vb.end();
                        if(vb.id != BVector) continue;
                        const quint32 cl = c.r.u32(vb.body);
                        if(cl >= 4) readShapes(c, vb.body + cl, vb.end(), body, 0);
                    }
                    if(!body.isEmpty()){
                        const QString svg = QStringLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%1\" height=\"%2\" viewBox=\"0 0 %1 %2\">\n%3</svg>\n").arg(c.width).arg(c.height).arg(body);
                        const QVector<Shape> shapes = parseSvg(svg.toUtf8());
                        if(!shapes.isEmpty()){
                            s.type = ShapeType::Group; s.name = L.name; s.children = shapes; s.isolate = L.opacity < 255;
                            ok = true;
                            if(stats) stats->vectorShapes += shapes.size();
                        }
                    }
                } else if(stats){
                    if(L.type == 4) ++stats->adjustments;
                    else if(L.type == 5) ++stats->groups;
                    else if(L.type == 6) ++stats->maskLayers;
                    else if(L.type == 7) ++stats->artMedia;
                }
                if(!ok) continue;
                s.opacity = L.opacity / 255.0;
                s.visible = L.visible;
                psdApplyBlend(s, blendKey(L.blend));
                if(L.blend == 9 && stats) ++stats->dissolve;
                layers.push_back(s);
            }
        }
    }
    if(!haveImage) return doc;
    doc.pageSize = QSizeF(c.width, c.height);
    doc.shapes = layers;
    return doc;
}

} // namespace xara
