// MediBang Paint / FireAlpaca / mdiapp (.mdp, "mdipack"), after the format notes its author published. A
// 20-byte header ("mdipack ", version, the two part sizes) leads an XML part (canvas, thumbnail, the layer
// list) and a binary part of named "PAC " blocks. Layers are listed bottom first, a folder after its members,
// which name it by parentId. A layer's block holds its painted 128×128 tiles, each zlib, Snappy or FastLZ
// compressed: 32bpp tiles are BGRA with straight alpha; 8bpp and 1bpp (most significant bit first) tiles are
// the coverage of the layer's one colour. The thumbnail is BGRA, untiled.
#include "prepress.h"
#include <QHash>
#include <QImage>
#include <QXmlStreamReader>
#include <cstring>
#include <functional>
#include <zlib.h>

namespace xara {
namespace {

const qint64 kMaxPixels = qint64(1) << 28, kMaxTotalPixels = qint64(1) << 29;

quint32 le32(const uchar* p){ return quint32(p[0]) | quint32(p[1]) << 8 | quint32(p[2]) << 16 | quint32(p[3]) << 24; }

// Snappy's raw block format: the output length as a varint, then literals and back-references.
bool snappy(const uchar* s, qint64 n, uchar* d, qint64 size){
    qint64 i = 0, o = 0; quint64 len = 0;
    for(int sh = 0; ; sh += 7){
        if(i >= n || sh > 28) return false;
        const uchar c = s[i++]; len |= quint64(c & 0x7f) << sh;
        if(!(c & 0x80)) break;
    }
    if(len != quint64(size)) return false;
    while(i < n){
        const uchar tag = s[i++];
        qint64 count, off = 0;
        if((tag & 3) == 0){                                   // literal
            count = tag >> 2;
            if(count >= 60){
                const int k = int(count) - 59;
                if(i + k > n) return false;
                count = 0; for(int b = 0; b < k; ++b) count |= qint64(s[i + b]) << (8 * b);
                i += k;
            }
            ++count;
            if(count > n - i || count > size - o) return false;
            std::memcpy(d + o, s + i, size_t(count)); i += count; o += count;
            continue;
        }
        if((tag & 3) == 1){ if(i >= n) return false; count = ((tag >> 2) & 7) + 4; off = qint64(tag >> 5) << 8 | s[i++]; }
        else if((tag & 3) == 2){ if(i + 2 > n) return false; count = (tag >> 2) + 1; off = s[i] | s[i + 1] << 8; i += 2; }
        else { if(i + 4 > n) return false; count = (tag >> 2) + 1; off = le32(s + i); i += 4; }
        if(off < 1 || off > o || count > size - o) return false;
        for(qint64 k = 0; k < count; ++k, ++o) d[o] = d[o - off];    // the source may overlap what's being written
    }
    return o == size;
}

// FastLZ, levels 1 and 2 (the level is in the first byte's top bits).
bool fastlz(const uchar* s, qint64 n, uchar* d, qint64 size){
    if(n < 1) return false;
    const int level = (s[0] >> 5) + 1;
    if(level != 1 && level != 2) return false;
    qint64 i = 0, o = 0;
    quint32 ctrl = s[i++] & 31;
    for(;;){
        if(ctrl >= 32){
            qint64 len = (ctrl >> 5) - 1, ref = o - qint64((ctrl & 31) << 8) - 1;
            if(len == 6){
                if(level == 1){ if(i >= n) return false; len += s[i++]; }
                else for(uchar c = 255; c == 255; ){ if(i >= n) return false; c = s[i++]; len += c; }
            }
            if(i >= n) return false;
            const uchar code = s[i++];
            ref -= code;
            len += 3;
            if(level == 2 && code == 255 && (ctrl & 31) == 31){       // a 16-bit distance follows
                if(i + 2 > n) return false;
                ref = o - (qint64(s[i]) << 8 | s[i + 1]) - 8191 - 1; i += 2;
            }
            if(ref < 0 || len > size - o) return false;
            for(qint64 k = 0; k < len; ++k, ++o) d[o] = d[ref + k];
        } else {
            const qint64 run = qint64(ctrl) + 1;
            if(run > n - i || run > size - o) return false;
            std::memcpy(d + o, s + i, size_t(run)); i += run; o += run;
        }
        if(level == 1 ? i > n - 2 : i >= n) break;     // as the reference decoder stops
        ctrl = s[i++];
    }
    return o == size;
}

bool inflateInto(const uchar* s, qint64 n, uchar* d, qint64 size){
    if(n > 0x7fffffff || size > 0x7fffffff) return false;
    uLongf out = uLongf(size);
    return uncompress(d, &out, s, uLong(n)) == Z_OK && qint64(out) == size;
}

struct Block { qint64 at = 0, size = 0, outSize = 0; quint32 type = 0; };

// A block's bytes, unpacked if the block itself is zlib-compressed.
QByteArray blockBytes(const QByteArray& f, const Block& b){
    if(b.type == 0) return f.mid(int(b.at), int(b.size));
    if(b.type != 1 || b.outSize < 1 || b.outSize > kMaxPixels * 4) return QByteArray();
    QByteArray out(int(b.outSize), Qt::Uninitialized);
    if(!inflateInto(reinterpret_cast<const uchar*>(f.constData()) + b.at, b.size, reinterpret_cast<uchar*>(out.data()), b.outSize)) return QByteArray();
    return out;
}

QByteArray modeKey(QString m, bool* known){
    m = m.toLower().remove(QLatin1Char(' ')).remove(QLatin1Char('_')).remove(QLatin1Char('-'));
    static const QHash<QString, QByteArray> keys = {
        {"normal","norm"},{"multiply","mul "},{"screen","scrn"},{"overlay","over"},{"darken","dark"},{"lighten","lite"},
        {"difference","diff"},{"exclusion","smud"},{"add","lddg"},{"additive","lddg"},{"lineardodge","lddg"},
        {"burn","idiv"},{"colorburn","idiv"},{"dodge","div "},{"colordodge","div "},{"softlight","sLit"},{"hardlight","hLit"},
        {"hue","hue "},{"saturation","sat "},{"color","colr"},{"colour","colr"},{"luminosity","lum "},{"linearburn","lbrn"},
        {"vividlight","vLit"},{"linearlight","lLit"},{"pinlight","pLit"},{"hardmix","hMix"},{"subtract","fsub"},{"divide","fdiv"} };
    const auto it = keys.constFind(m);
    *known = m.isEmpty() || it != keys.constEnd();
    return it != keys.constEnd() ? it.value() : QByteArrayLiteral("norm");
}

// Clipping: a layer only shows where the layer it is clipped to has paint.
void clipAlpha(QImage& img, int X, int Y, const QImage& base, int bx, int by){
    for(int y = 0; y < img.height(); ++y){
        QRgb* row = reinterpret_cast<QRgb*>(img.scanLine(y));
        const int sy = Y + y - by;
        const QRgb* b = sy >= 0 && sy < base.height() ? reinterpret_cast<const QRgb*>(base.constScanLine(sy)) : nullptr;
        for(int x = 0; x < img.width(); ++x){
            const int sx = X + x - bx;
            const int a = b && sx >= 0 && sx < base.width() ? qAlpha(b[sx]) : 0;
            row[x] = (row[x] & 0xffffff) | uint(qAlpha(row[x]) * a / 255) << 24;
        }
    }
}

struct MLayer {
    QString id, parent, name, type, mode, bin; QRgb colour = 0xff000000;
    int x = 0, y = 0, w = 0, h = 0, alpha = 255; bool visible = true, clipping = false, masking = false;
    QVector<int> kids;   // folder members, bottom first
};

} // namespace

Document importMdpDocument(const QByteArray& f, MdpImportStats* st){
    Document doc;
    MdpImportStats local; if(!st) st = &local;
    const uchar* p = reinterpret_cast<const uchar*>(f.constData());
    const qint64 n = f.size();
    if(n < 20 || !f.startsWith("mdipack")) return doc;
    const qint64 mdiSize = le32(p + 12);
    if(mdiSize > n - 20) return doc;

    // The named blocks of the binary part.
    QHash<QString, Block> blocks;
    for(qint64 o = 20 + mdiSize; o + 132 <= n && blocks.size() < 100000; ){
        if(std::memcmp(p + o, "PAC ", 4)) break;
        Block b; const qint64 chunk = le32(p + o + 4); b.type = le32(p + o + 8); b.size = le32(p + o + 12); b.outSize = le32(p + o + 16); b.at = o + 132;
        const QString name = QString::fromLatin1(reinterpret_cast<const char*>(p + o + 68), int(qstrnlen(reinterpret_cast<const char*>(p + o + 68), 64)));
        if(b.size > n - b.at) break;
        if(!blocks.contains(name)) blocks.insert(name, b);
        if(chunk < 132 + b.size) break;
        o += chunk;
    }

    // The canvas and the layer list.
    int W = 0, H = 0, thumbW = 0, thumbH = 0; QString thumbBin;
    QVector<MLayer> layers; QHash<QString, int> byId;
    {
        QXmlStreamReader r(f.mid(20, int(mdiSize)));
        while(!r.atEnd() && layers.size() < 10000){
            if(r.readNext() != QXmlStreamReader::StartElement) continue;
            const QXmlStreamAttributes a = r.attributes();
            auto num = [&](const char* k, int def){ bool ok = false; const int v = a.value(QLatin1String(k)).toInt(&ok); return ok ? v : def; };
            auto flag = [&](const char* k){ return a.value(QLatin1String(k)) == QLatin1String("true"); };
            if(r.name() == QLatin1String("Mdiapp")){ W = num("width", 0); H = num("height", 0); }
            else if(r.name() == QLatin1String("Thumb")){ thumbW = num("width", 0); thumbH = num("height", 0); thumbBin = a.value("bin").toString(); }
            else if(r.name() == QLatin1String("Layer")){
                MLayer L;
                L.id = a.value("id").toString(); L.parent = a.value("parentId").toString(); L.name = a.value("name").toString();
                L.type = a.value("type").toString(); L.mode = a.value("mode").toString(); L.bin = a.value("bin").toString();
                L.x = qBound(-10000000, num("ofsx", 0), 10000000); L.y = qBound(-10000000, num("ofsy", 0), 10000000);
                L.w = num("width", 0); L.h = num("height", 0); L.alpha = qBound(0, num("alpha", 255), 255);
                L.visible = a.value("visible") != QLatin1String("false"); L.clipping = flag("clipping"); L.masking = flag("masking");
                bool ok = false; const uint c = a.value("color").toUInt(&ok, 16); if(ok) L.colour = c;
                if(!L.id.isEmpty() && !byId.contains(L.id)) byId.insert(L.id, layers.size());
                layers << L;
            }
        }
    }
    if(W < 1 || H < 1 || W > 1000000 || H > 1000000 || qint64(W) * H > kMaxPixels) return doc;
    doc.pageSize = QSizeF(W, H);

    // Members join their folder; anything naming no folder in the file stays at the top level.
    QVector<int> top;
    for(int k = 0; k < layers.size(); ++k){
        const int at = byId.value(layers[k].parent, -1);
        if(at >= 0 && at != k && layers[at].type == QLatin1String("folder")) layers[at].kids << k; else top << k;
    }

    qint64 budget = kMaxTotalPixels;
    // A layer's pixels, placed on its own bounds; false when it has none to show.
    auto pixels = [&](const MLayer& L, QImage& img) -> bool {
        const int bpp = L.type == QLatin1String("32bpp") ? 32 : L.type == QLatin1String("8bpp") ? 8 : L.type == QLatin1String("1bpp") ? 1 : 0;
        if(!bpp){ if(st) ++st->unknownTypes; return false; }
        if(L.w < 1 || L.h < 1 || L.w > 1000000 || L.h > 1000000 || qint64(L.w) * L.h > kMaxPixels || qint64(L.w) * L.h > budget) return false;
        const auto it = blocks.constFind(L.bin);
        if(it == blocks.constEnd()){ if(st) ++st->badTiles; return false; }
        const QByteArray data = blockBytes(f, it.value());
        const uchar* s = reinterpret_cast<const uchar*>(data.constData()); const qint64 m = data.size();
        if(m < 4 || le32(s) == 0) return false;                         // nothing painted
        const qint64 tiles = le32(s), dim = m >= 8 ? le32(s + 4) : 0;
        if(dim < 1 || dim > 4096 || tiles > m / 16) return false;
        const qint64 tileBytes = bpp == 1 ? (dim * dim + 7) / 8 : dim * dim * bpp / 8;
        img = QImage(L.w, L.h, QImage::Format_ARGB32);
        if(img.isNull()) return false;
        budget -= qint64(L.w) * L.h;
        img.fill(Qt::transparent);
        QByteArray tile(int(tileBytes), Qt::Uninitialized);
        uchar* t = reinterpret_cast<uchar*>(tile.data());
        const QRgb ink = L.colour & 0xffffff; const int inkA = qAlpha(L.colour);
        qint64 o = 8;
        for(qint64 k = 0; k < tiles; ++k){
            if(o + 16 > m){ if(st) st->badTiles += int(tiles - k); break; }
            const qint64 col = le32(s + o), row = le32(s + o + 4), comp = le32(s + o + 8), size = le32(s + o + 12);
            o += 16;
            if(size > m - o){ if(st) st->badTiles += int(tiles - k); break; }
            const uchar* z = s + o;
            o += (size + 3) & ~qint64(3);
            const bool ok = comp == 0 ? inflateInto(z, size, t, tileBytes) : comp == 1 ? snappy(z, size, t, tileBytes) : comp == 2 && fastlz(z, size, t, tileBytes);
            if(!ok){ if(st) ++st->badTiles; continue; }
            const qint64 x0 = col * dim, y0 = row * dim;
            if(x0 >= L.w || y0 >= L.h) continue;
            for(int y = 0; y < dim && y0 + y < L.h; ++y){
                QRgb* d = reinterpret_cast<QRgb*>(img.scanLine(int(y0 + y))) + x0;
                for(int x = 0; x < dim && x0 + x < L.w; ++x){
                    const qint64 i = qint64(y) * dim + x;
                    if(bpp == 32){ const uchar* q = t + i * 4; d[x] = qRgba(q[2], q[1], q[0], q[3]); }
                    else {
                        const int cov = bpp == 8 ? t[i] : ((t[i >> 3] >> (7 - (i & 7))) & 1) * 255;
                        d[x] = ink | uint(cov * inkA / 255) << 24;
                    }
                }
            }
        }
        return true;
    };

    std::function<QVector<Shape>(const QVector<int>&, int)> build = [&](const QVector<int>& list, int depth) -> QVector<Shape> {
        QVector<Shape> out;
        QImage base; int bx = 0, by = 0; bool haveBase = false;
        for(int k : list){
            const MLayer& L = layers[k];
            bool known = true; const bool pass = L.mode.compare(QLatin1String("passthrough"), Qt::CaseInsensitive) == 0;
            const QByteArray key = pass ? QByteArrayLiteral("norm") : modeKey(L.mode, &known);
            if(!known && st) ++st->unknownModes;
            Shape s;
            if(L.type == QLatin1String("folder")){
                if(depth >= 64) continue;
                s.type = ShapeType::Group; s.name = L.name.isEmpty() ? QStringLiteral("Folder") : L.name;
                s.children = build(L.kids, depth + 1);
                if(s.children.isEmpty()) continue;
                s.isolate = !pass || L.alpha < 255;
                if(st) ++st->groups;
                haveBase = false;
            } else {
                QImage img;
                if(!pixels(L, img)){
                    if(!L.clipping){ base = QImage(); haveBase = true; }   // what clips to an empty layer shows nothing
                    continue;
                }
                if(L.clipping && haveBase){ clipAlpha(img, L.x, L.y, base, bx, by); if(st) ++st->clipped; }
                else if(!L.clipping){ base = img; bx = L.x; by = L.y; haveBase = true; }
                // Only what's painted becomes the object.
                int x0 = img.width(), y0 = img.height(), x1 = -1, y1 = -1;
                for(int y = 0; y < img.height(); ++y){
                    const QRgb* row = reinterpret_cast<const QRgb*>(img.constScanLine(y));
                    for(int x = 0; x < img.width(); ++x) if(qAlpha(row[x])){ x0 = qMin(x0, x); x1 = qMax(x1, x); y0 = qMin(y0, y); y1 = y; }
                }
                if(x1 < 0){ if(st) ++st->empty; continue; }
                const QRect box(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
                s = psdBitmapShape(box.size() == img.size() ? img : img.copy(box), L.x + x0, L.y + y0, L.name.isEmpty() ? QStringLiteral("Layer") : L.name);
                if(L.masking && st) ++st->masks;
                if(st) ++st->layers;
            }
            s.opacity = L.alpha / 255.0;
            s.visible = L.visible;
            psdApplyBlend(s, key);
            if(st && !L.visible) ++st->hidden;
            out << s;
        }
        return out;
    };
    doc.shapes = build(top, 0);

    // With nothing readable where something was meant to be, the thumbnail stands in.
    if(doc.shapes.isEmpty() && (st->badTiles || st->unknownTypes) && thumbW > 0 && thumbH > 0 && qint64(thumbW) * thumbH <= kMaxPixels){
        const QByteArray t = blocks.contains(thumbBin) ? blockBytes(f, blocks.value(thumbBin)) : QByteArray();
        if(t.size() >= qint64(thumbW) * thumbH * 4){
            QImage img(thumbW, thumbH, QImage::Format_ARGB32);
            for(int y = 0; y < thumbH; ++y){
                const uchar* q = reinterpret_cast<const uchar*>(t.constData()) + qint64(y) * thumbW * 4;
                QRgb* d = reinterpret_cast<QRgb*>(img.scanLine(y));
                for(int x = 0; x < thumbW; ++x, q += 4) d[x] = qRgba(q[2], q[1], q[0], q[3]);
            }
            Shape s = psdBitmapShape(img, 0, 0, QStringLiteral("Preview"));
            s.transform = QTransform::fromScale(double(W) / thumbW, double(H) / thumbH);
            doc.shapes = { s };
            if(st) st->thumbnailOnly = true;
        }
    }
    return doc;
}

} // namespace xara
