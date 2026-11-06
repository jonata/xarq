// Corel PHOTO-PAINT 7+ (.cpt, magic "CPT7FILE"). No official specification; this comes from the sample files,
// checked against the thumbnail each file carries.
// Header: u32 palette size at 12 (blue, green, red triples from 0x13c), u32 object count at 0x28, then after
// the palette a u64 offset per object.
// Object: u32 width, height, tile width, tile height, bit depth; u32 kind at 0x18 (1 background, 8 object,
// 4 the mask of the object before it, 2 a saved mask, 16 the thumbnail); u32 header size at 0x20, and the tile
// table, a (u32 position, u32 size) pair per tile row by row, at 0x3c past that. An object also has its bounds
// at 0x44 (left, top, right, bottom, y counting up from the bottom), visibility at 0x5c, opacity in percent at
// 0x64 and its name at 0x6c.
// Tile: a whole tile buffer with nothing else when compressing didn't pay, else u16 compression, u16 filter.
// Compression 1 is stored, 2 GIF-style LZW with an early code-width change, 4 one flat colour (at +12) and
// 5 runs of signed fixed-width samples, least significant bit first: an 8-bit header whose low five bits are
// the run length less one and high three a width code (0 for zeros, else that plus one bits).
// Filter 3 means each sample was stored as its difference from the one above, the first row from the one to
// its left. Rows are blue first and padded to four bytes, and run upward through the whole picture.
#include "prepress.h"
#include <QImage>
#include <cstring>

namespace xara {
namespace {

inline quint32 le32(const uchar* p){ return quint32(p[3]) << 24 | quint32(p[2]) << 16 | quint32(p[1]) << 8 | p[0]; }
inline int le16(const uchar* p){ return p[1] << 8 | p[0]; }
const qint64 kMaxPixels = qint64(1) << 28;

// LZW, least significant bit first, 8-bit symbols, clear = 256, end = 257.
bool lzwDecode(const uchar* s, qint64 n, uchar* d, qint64 cap){
    QVector<int> prefix(4096, -1); QVector<uchar> suffix(4096, 0); QVector<int> length(4096, 0);
    for(int i = 0; i < 256; ++i){ suffix[i] = uchar(i); length[i] = 1; }
    int bits = 9, next = 258, prev = -1; quint32 acc = 0; int nb = 0; qint64 ip = 0, op = 0;
    const auto put = [&](int code) -> int {      // writes the code's string and gives its first byte, or -1
        const int len = length[code];
        if(len <= 0 || op + len > cap) return -1;
        qint64 w = op + len; int c = code, guard = 4097;
        while(c >= 0 && guard-- > 0){ d[--w] = suffix[c]; c = prefix[c]; }
        op += len;
        return d[op - len];
    };
    for(;;){
        while(nb < bits){ if(ip >= n) return op == cap; acc |= quint32(s[ip++]) << nb; nb += 8; }
        const int code = int(acc & ((1u << bits) - 1)); acc >>= bits; nb -= bits;
        if(code == 256){ bits = 9; next = 258; prev = -1; continue; }
        if(code == 257) return op == cap;
        if(prev < 0){
            if(code >= 256 || put(code) < 0) return false;
            prev = code; continue;
        }
        int first;
        if(code < next){
            first = put(code);
            if(first < 0) return false;
        } else if(code == next && next < 4096){
            // The string being defined: the previous one, then its own first byte.
            const int len = length[prev];
            if(len <= 0 || op + len + 1 > cap) return false;
            const qint64 at = op;
            if(put(prev) < 0) return false;
            first = d[at];
            d[op++] = uchar(first);
        } else return false;
        if(next < 4096){
            prefix[next] = prev; suffix[next] = uchar(first); length[next] = length[prev] + 1;
            ++next;
            if(next + 1 == (1 << bits) && bits < 12) ++bits;
        }
        prev = code;
    }
}

// Compression 5. Samples are kept modulo 256, which is all the filter needs.
bool runDecode(const uchar* s, qint64 n, uchar* d, qint64 cap){
    quint64 acc = 0; int nb = 0; qint64 ip = 0, op = 0;
    const auto need = [&](int k){ while(nb < k){ if(ip >= n) return false; acc |= quint64(s[ip++]) << nb; nb += 8; } return true; };
    while(op < cap){
        if(!need(8)) return false;
        const int h = int(acc & 255); acc >>= 8; nb -= 8;
        const int count = (h & 31) + 1, code = h >> 5, w = code ? code + 1 : 0;
        if(!w){ const qint64 m = qMin<qint64>(count, cap - op); std::memset(d + op, 0, size_t(m)); op += m; continue; }
        const quint32 mask = (1u << w) - 1, sign = 1u << (w - 1);
        for(int i = 0; i < count && op < cap; ++i){
            if(!need(w)) return false;
            quint32 v = quint32(acc) & mask; acc >>= w; nb -= w;
            if(v & sign) v |= ~mask;
            d[op++] = uchar(v);
        }
    }
    return true;
}

struct Obj { qint64 at = 0; int w = 0, h = 0, tw = 0, th = 0, bpp = 0, kind = 0; qint64 table = 0; };

bool readObj(const uchar* f, qint64 n, qint64 at, Obj& o){
    if(at < 0x13c || at + 0x40 > n) return false;
    o.at = at;
    o.w = int(le32(f + at)); o.h = int(le32(f + at + 4)); o.tw = int(le32(f + at + 8)); o.th = int(le32(f + at + 12));
    o.bpp = int(le32(f + at + 16)); o.kind = int(le32(f + at + 0x18));
    const quint32 hs = le32(f + at + 0x20);
    o.table = at + 0x3c + qint64(hs);
    if(o.w <= 0 || o.h <= 0 || o.w > 100000 || o.h > 100000 || qint64(o.w) * o.h > kMaxPixels) return false;
    if(o.tw <= 0 || o.th <= 0 || o.tw > 4096 || o.th > 4096) return false;
    const qint64 tiles = qint64((o.w + o.tw - 1) / o.tw) * ((o.h + o.th - 1) / o.th);
    return hs < quint32(n) && o.table + tiles * 8 <= n;
}

// One object's pixels: blue, green, red bytes (or one byte) per pixel, top row first. Empty when a tile can't be read.
QByteArray decodePlane(const uchar* f, qint64 n, const Obj& o, CptImportStats* st){
    const int ch = o.bpp / 8;
    if(ch != 1 && ch != 3){ if(st) ++st->unknownDepth; return QByteArray(); }
    const qint64 stride = (qint64(o.tw) * ch + 3) / 4 * 4, tileBytes = stride * o.th;
    const int cols = (o.w + o.tw - 1) / o.tw, rows = (o.h + o.th - 1) / o.th;
    QByteArray plane(int(qint64(o.w) * o.h * ch), '\0'), tile(int(tileBytes), '\0');
    uchar* P = reinterpret_cast<uchar*>(plane.data()); uchar* T = reinterpret_cast<uchar*>(tile.data());
    for(int i = 0; i < rows * cols; ++i){
        const quint32 pos = le32(f + o.table + i * 8), size = le32(f + o.table + i * 8 + 4);
        if(pos < 64 || size < 4 || qint64(pos) + size > n){ if(st) ++st->unknownTiles; continue; }
        const uchar* s = f + pos;
        int filter = 0; bool ok = true;
        if(qint64(size) == tileBytes || (size == 65536 && tileBytes <= 65536)){    // a whole buffer, padding and all
            std::memcpy(T, s, size_t(qMin<qint64>(size, tileBytes)));
        } else {
            const int kind = le16(s); filter = le16(s + 2);
            if(kind == 1){ std::memset(T, 0, size_t(tileBytes)); std::memcpy(T, s + 4, size_t(qMin<qint64>(size - 4, tileBytes))); }
            else if(kind == 2) ok = lzwDecode(s + 4, size - 4, T, tileBytes);
            else if(kind == 4){
                if(size < 12 + quint32(ch)){ ok = false; }
                else for(qint64 y = 0; y < o.th; ++y) for(int x = 0; x < o.tw; ++x) std::memcpy(T + y * stride + x * ch, s + 12, size_t(ch));
                filter = 0;
            }
            else if(kind == 5) ok = runDecode(s + 4, size - 4, T, tileBytes);
            else ok = false;
        }
        if(!ok){ if(st) ++st->unknownTiles; continue; }
        if(filter == 3){
            const int rowLen = o.tw * ch;
            for(int x = ch; x < rowLen; ++x) T[x] = uchar(T[x] + T[x - ch]);
            for(int y = 1; y < o.th; ++y){
                uchar* r = T + y * stride; const uchar* up = r - stride;
                for(int x = 0; x < rowLen; ++x) r[x] = uchar(r[x] + up[x]);
            }
        }
        // Stored rows run upward through the picture.
        const int ty = i / cols, tx = i % cols;
        const int x0 = tx * o.tw, w = qMin(o.tw, o.w - x0);
        for(int y = 0; y < o.th; ++y){
            const int sy = ty * o.th + y;
            if(sy >= o.h) break;
            std::memcpy(P + (qint64(o.h - 1 - sy) * o.w + x0) * ch, T + y * stride, size_t(w) * ch);
        }
    }
    return plane;
}

QImage toImage(const QByteArray& plane, const Obj& o, const QVector<QRgb>& palette){
    QImage img(o.w, o.h, QImage::Format_RGB32);
    if(img.isNull()) return img;
    const uchar* s = reinterpret_cast<const uchar*>(plane.constData());
    const bool rgb = o.bpp == 24;
    for(int y = 0; y < o.h; ++y){
        QRgb* d = reinterpret_cast<QRgb*>(img.scanLine(y));
        const uchar* r = s + qint64(y) * o.w * (rgb ? 3 : 1);
        for(int x = 0; x < o.w; ++x){
            if(rgb) d[x] = qRgb(r[x * 3 + 2], r[x * 3 + 1], r[x * 3]);
            else d[x] = r[x] < palette.size() ? palette[r[x]] : qRgb(r[x], r[x], r[x]);
        }
    }
    return img;
}

} // namespace

Document importCptDocument(const QByteArray& data, CptImportStats* st){
    Document doc;
    const uchar* f = reinterpret_cast<const uchar*>(data.constData()); const qint64 n = data.size();
    if(n < 0x140 || std::memcmp(f, "CPT7FILE", 8) != 0) return doc;
    const quint32 pal = le32(f + 12), count = le32(f + 0x28);
    if(pal > 768 || pal % 3 || count == 0 || count > 10000 || 0x13c + qint64(pal) + qint64(count) * 8 > n) return doc;
    QVector<QRgb> palette;
    for(quint32 i = 0; i + 2 < pal; i += 3) palette.push_back(qRgb(f[0x13c + i + 2], f[0x13c + i + 1], f[0x13c + i]));
    QVector<Obj> objs;
    for(quint32 i = 0; i < count; ++i){
        const qint64 at = le32(f + 0x13c + pal + i * 8);
        Obj o; if(readObj(f, n, at, o)) objs.push_back(o);
    }
    int W = 0, H = 0; QImage thumb;
    for(int i = 0; i < objs.size(); ++i){
        const Obj& o = objs[i];
        if(o.kind == 2){ if(st) ++st->savedMasks; continue; }
        if(o.kind == 16){ if(thumb.isNull()){ const QByteArray p = decodePlane(f, n, o, nullptr); if(!p.isEmpty()) thumb = toImage(p, o, palette); } continue; }
        if(o.kind != 1 && o.kind != 8) continue;
        if(o.kind == 8 && W == 0) continue;     // objects sit on the background
        const QByteArray plane = decodePlane(f, n, o, st);
        if(plane.isEmpty()) continue;
        QImage img = toImage(plane, o, palette);
        if(img.isNull()) continue;
        if(o.kind == 1){
            if(W) continue;
            W = o.w; H = o.h; doc.pageSize = QSizeF(W, H);
            doc.shapes.push_back(psdBitmapShape(img, 0, 0, QStringLiteral("Background")));
            continue;
        }
        const uchar* h = f + o.at;
        if(o.at + 0x6c + 32 > n) continue;
        const int left = int(qBound(qint64(-1000000), qint64(qint32(le32(h + 0x44))), qint64(1000000)));
        const int top = int(qBound(qint64(-1000000), qint64(qint32(le32(h + 0x48))), qint64(1000000)));
        if(i + 1 < objs.size() && objs[i + 1].kind == 4 && objs[i + 1].w == o.w && objs[i + 1].h == o.h && objs[i + 1].bpp == 8){
            const QByteArray m = decodePlane(f, n, objs[i + 1], st);
            if(!m.isEmpty()){
                img = img.convertToFormat(QImage::Format_ARGB32);
                const uchar* a = reinterpret_cast<const uchar*>(m.constData());
                for(int y = 0; y < o.h; ++y){
                    QRgb* d = reinterpret_cast<QRgb*>(img.scanLine(y));
                    for(int x = 0; x < o.w; ++x) d[x] = (d[x] & 0x00FFFFFFu) | quint32(a[qint64(y) * o.w + x]) << 24;
                }
            }
            ++i;
        }
        const char* nm = reinterpret_cast<const char*>(h + 0x6c);
        const QString name = QString::fromLatin1(nm, int(qstrnlen(nm, 32)));
        Shape s = psdBitmapShape(img, left, H - 1 - top, name.isEmpty() ? QStringLiteral("Object") : name);
        s.opacity = qBound(0, int(le32(h + 0x64)), 100) / 100.0;
        s.visible = le32(h + 0x5c) != 0;
        doc.shapes.push_back(s);
        if(st){ ++st->objects; if(!s.visible) ++st->hidden; }
    }
    if(doc.shapes.isEmpty() && !thumb.isNull()){
        doc.pageSize = thumb.size();
        doc.shapes.push_back(psdBitmapShape(thumb, 0, 0, QStringLiteral("Thumbnail")));
        if(st) st->thumbnailOnly = true;
    }
    return doc;
}
} // namespace xara
