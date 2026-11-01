// Corel PHOTO-PAINT 7+ (.cpt, magic "CPT7FILE"). No official specification; this comes from the sample files
// and an unofficial draft. A palette follows the fixed header when bytes 12-15 give its size, then a field
// locates the image record: width, height, tile width, tile height, bit depth. Pixels sit in tiles listed as (position, size) pairs,
// each tile starting with u16 compression and u16 channel count. Compression 2 is GIF-style LZW with an
// early code-width change, over rows padded to four bytes with blue first; 4 is one flat colour.
// Compression 5, which most photographic files use, isn't understood, so those report that instead.
#include "prepress.h"
#include <QImage>
#include <cstring>

namespace xara {
namespace {

inline quint32 le32(const uchar* p){ return quint32(p[3]) << 24 | quint32(p[2]) << 16 | quint32(p[1]) << 8 | p[0]; }
inline int le16(const uchar* p){ return p[1] << 8 | p[0]; }
const qint64 kMaxPixels = qint64(1) << 28;

// LZW, least significant bit first, 8-bit symbols, clear = 256, end = 257.
bool lzwDecode(const uchar* s, qint64 n, QByteArray& out){
    uchar* d = reinterpret_cast<uchar*>(out.data()); const qint64 cap = out.size();
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

struct Rec { int w = 0, h = 0, bpp = 0; QVector<QRgb> palette; qint64 tableStart = 0; int tiles = 0; };

bool readRecord(const uchar* f, qint64 n, Rec& r){
    if(n < 0x140 || std::memcmp(f, "CPT7FILE", 8) != 0) return false;
    const quint32 pal = le32(f + 12);
    if(pal > 768 || pal % 3 || 0x140 + qint64(pal) > n) return false;
    for(quint32 i = 0; i + 2 < pal; i += 3) r.palette.push_back(qRgb(f[0x138 + i], f[0x138 + i + 1], f[0x138 + i + 2]));
    const quint32 off = le32(f + 0x13c + pal);
    if(off + 20 > quint64(n)) return false;
    r.w = int(le32(f + off)); r.h = int(le32(f + off + 4)); r.bpp = int(le32(f + off + 16));
    return r.w > 0 && r.h > 0 && qint64(r.w) * r.h <= kMaxPixels && (r.bpp == 8 || r.bpp == 24);
}

// The tile list isn't at a fixed place: find the longest run of (position, size) pairs that abut.
bool findTiles(const uchar* f, qint64 n, qint64 from, qint64 to, QVector<QPair<quint32,quint32>>& out){
    int bestLen = 0; qint64 bestAt = 0; quint32 bestSize = 0;
    for(qint64 s = from; s + 8 <= qMin(to, n); ++s){
        quint32 p = le32(f + s), z = le32(f + s + 4);
        if(p < 64 || z < 4 || qint64(p) + z > n) continue;
        const int kind = le16(f + p), chans = le16(f + p + 2);
        if(!(kind == 2 || kind == 4 || kind == 5) || chans > 5) continue;     // a real tile says what it is
        int run = 1; qint64 t = s + 8;
        while(t + 8 <= n){
            const quint32 p2 = le32(f + t), z2 = le32(f + t + 4);
            if(p2 != p + z || z2 == 0 || qint64(p2) + z2 > n) break;
            ++run; t += 8; p = p2; z = z2;
        }
        const quint32 first = le32(f + s + 4);
        if(run > bestLen || (run == bestLen && first > bestSize)){ bestLen = run; bestAt = s; bestSize = first; }  // a chance match is short and small
    }
    if(bestLen < 1) return false;
    for(int i = 0; i < bestLen; ++i) out.push_back({ le32(f + bestAt + i * 8), le32(f + bestAt + i * 8 + 4) });
    return true;
}

QImage decodeFile(const uchar* f, qint64 n, int depth, CptImportStats* st){
    Rec r;
    if(depth > 4 || !readRecord(f, n, r)) return QImage();
    // A file can hold a whole second document for its image.
    const qint64 scanEnd = qMin<qint64>(n, 8192);
    for(qint64 i = 16; i + 8 <= scanEnd; ++i)
        if(std::memcmp(f + i, "CPT7FILE", 8) == 0){ const QImage inner = decodeFile(f + i, n - i, depth + 1, st); if(!inner.isNull()) return inner; break; }
    const quint32 pal = le32(f + 12);
    const quint32 off = le32(f + 0x13c + pal);
    QVector<QPair<quint32,quint32>> tiles;
    if(!findTiles(f, n, off + 20, off + 1024, tiles)) return QImage();
    const int ch = r.bpp / 8;
    const qint64 stride = (qint64(r.w) * ch + 3) / 4 * 4;
    for(const auto& t : tiles){
        if(t.second < 4) continue;
        const int type = le16(f + t.first);
        if(type == 5){ if(st) ++st->unknownTiles; continue; }
        if(type != 2) continue;
        if(tiles.size() != 1){ if(st) ++st->unknownTiles; continue; }   // only whole-image LZW has been seen
        QByteArray px(int(stride * r.h), '\0');
        if(!lzwDecode(f + t.first + 4, t.second - 4, px)) return QImage();
        QImage img(r.w, r.h, QImage::Format_RGB32);
        if(img.isNull()) return QImage();
        const uchar* s = reinterpret_cast<const uchar*>(px.constData());
        for(int y = 0; y < r.h; ++y){
            QRgb* d = reinterpret_cast<QRgb*>(img.scanLine(y));
            const uchar* row = s + stride * y;
            for(int x = 0; x < r.w; ++x){
                if(ch == 3) d[x] = qRgb(row[x * 3 + 2], row[x * 3 + 1], row[x * 3]);
                else d[x] = row[x] < r.palette.size() ? r.palette[row[x]] : qRgb(row[x], row[x], row[x]);
            }
        }
        return img;
    }
    return QImage();
}
} // namespace

QImage importCpt(const QByteArray& d, CptImportStats* st){
    return decodeFile(reinterpret_cast<const uchar*>(d.constData()), d.size(), 0, st);
}
} // namespace xara
