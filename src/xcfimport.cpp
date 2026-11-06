// GIMP's XCF (.xcf, and gzip-compressed .xcf.gz / .xcfgz), versions 0 to 12+, after GIMP's own format
// documentation (devel-docs/xcf.txt). Everything is big-endian: a header ("gimp xcf " + version tag), the
// canvas size, colour mode and (version 4+) precision, a property list, then pointers to the layers, topmost
// first (pointers are 64-bit from version 11). A layer has its size, type, name, properties and pointers to
// its pixels and mask; groups (version 3+) are layers flagged as such, their members found by item path.
// Pixels sit in 64×64 tiles, stored as is, zlib-compressed, or run-length coded one byte-plane at a time.
#include "prepress.h"
#include <QHash>
#include <QImage>
#include <algorithm>
#include <cmath>
#include <functional>
#include <cstring>
#include <zlib.h>

namespace xara {
namespace {

const qint64 kMaxPixels = qint64(1) << 28;

struct Rd {
    const uchar* p; qint64 n;
    quint32 u32(qint64 o) const { return o >= 0 && o + 4 <= n ? quint32(p[o]) << 24 | quint32(p[o + 1]) << 16 | quint32(p[o + 2]) << 8 | p[o + 3] : 0; }
    qint32 i32(qint64 o) const { return qint32(u32(o)); }
};

struct Xcf {
    Rd r; int ver = 0, W = 0, H = 0, base = 0, prec = 150, comp = 0, psz = 4;
    QVector<QRgb> cmap;
    qint64 ptr(qint64 o) const {
        if(psz == 4) return r.u32(o);
        const quint64 v = quint64(r.u32(o)) << 32 | r.u32(o + 4);
        return v > quint64(r.n) ? -1 : qint64(v);
    }
    int bpc() const {
        switch(prec){ case 200: case 250: case 500: case 550: return 2; case 300: case 350: case 600: case 650: return 4; case 700: case 750: return 8; default: return 1; }
    }
    bool isFloat() const { return prec >= 500; }
    bool linear() const { return (prec / 50) % 2 == 0; }
};

using Props = QHash<quint32, QByteArray>;

bool readProps(const Rd& r, qint64& o, Props& props){
    for(int guard = 0; guard < 100000; ++guard){
        if(o + 8 > r.n) return false;
        const quint32 type = r.u32(o), len = r.u32(o + 4); o += 8;
        if(type == 0) return true;
        if(qint64(len) > r.n - o) return false;
        props.insert(type, QByteArray(reinterpret_cast<const char*>(r.p + o), int(len)));
        o += len;
    }
    return false;
}
QString readString(const Rd& r, qint64& o){
    const quint32 len = r.u32(o); o += 4;
    if(qint64(len) > r.n - o) { o = r.n; return QString(); }
    const QString s = QString::fromUtf8(reinterpret_cast<const char*>(r.p + o), int(qstrnlen(reinterpret_cast<const char*>(r.p + o), len)));
    o += len;
    return s;
}
quint32 be32(const QByteArray& b, int at){
    if(at < 0 || at + 4 > b.size()) return 0;
    const uchar* d = reinterpret_cast<const uchar*>(b.constData()) + at;
    return quint32(d[0]) << 24 | quint32(d[1]) << 16 | quint32(d[2]) << 8 | d[3];
}
quint32 propU32(const Props& p, quint32 type, quint32 dflt){ const QByteArray b = p.value(type); return b.size() >= 4 ? be32(b, 0) : dflt; }

bool rleTile(const uchar* s, qint64 n, uchar* d, int pixels, int bpp){
    qint64 q = 0;
    for(int k = 0; k < bpp; ++k){
        int j = 0;
        while(j < pixels){
            if(q >= n) return false;
            const int v = s[q++];
            int len; bool same;
            if(v < 127){ len = v + 1; same = true; }
            else if(v == 127){ if(q + 2 > n) return false; len = s[q] * 256 + s[q + 1]; q += 2; same = true; }
            else if(v == 128){ if(q + 2 > n) return false; len = s[q] * 256 + s[q + 1]; q += 2; same = false; }
            else { len = 256 - v; same = false; }
            if(len > pixels - j) return false;
            if(same){ if(q >= n) return false; const uchar c = s[q++]; for(int i = 0; i < len; ++i) d[qint64(j + i) * bpp + k] = c; }
            else { if(q + len > n) return false; for(int i = 0; i < len; ++i) d[qint64(j + i) * bpp + k] = s[q + i]; q += len; }
            j += len;
        }
    }
    return true;
}
bool zlibTile(const uchar* s, qint64 n, uchar* d, qint64 size){
    z_stream z; std::memset(&z, 0, sizeof z);
    if(inflateInit(&z) != Z_OK) return false;
    z.next_in = const_cast<uchar*>(s); z.avail_in = uInt(qMin<qint64>(n, 0x7fffffff));
    z.next_out = d; z.avail_out = uInt(size);
    const int rc = inflate(&z, Z_FINISH);
    const bool ok = (rc == Z_STREAM_END || rc == Z_OK || rc == Z_BUF_ERROR) && z.avail_out == 0;
    inflateEnd(&z);
    return ok;
}

// A hierarchy's first level: `bpp`-byte pixels, top row first. Empty when the structure itself is broken.
QByteArray readPixels(const Xcf& x, qint64 hp, int w, int h, int bpp, int* badTiles){
    const Rd& r = x.r;
    if(hp <= 0 || hp + 12 + x.psz > r.n) return QByteArray();
    if(int(r.u32(hp)) != w || int(r.u32(hp + 4)) != h || int(r.u32(hp + 8)) != bpp) return QByteArray();
    const qint64 lp = x.ptr(hp + 12);
    if(lp <= 0 || lp + 8 > r.n || int(r.u32(lp)) != w || int(r.u32(lp + 4)) != h) return QByteArray();
    const int cols = (w + 63) / 64, rows = (h + 63) / 64;
    const qint64 count = qint64(cols) * rows;
    if(lp + 8 + (count + 1) * x.psz > r.n) return QByteArray();
    QVector<qint64> tiles(int(count) + 1);
    for(qint64 i = 0; i < count; ++i) tiles[int(i)] = x.ptr(lp + 8 + i * x.psz);
    tiles[int(count)] = x.ptr(lp + 8 + count * x.psz);
    QByteArray out(int(qint64(w) * h * bpp), '\0');
    if(out.isEmpty() && w * h) return QByteArray();
    QByteArray tile(64 * 64 * bpp, '\0');
    uchar* O = reinterpret_cast<uchar*>(out.data()); uchar* T = reinterpret_cast<uchar*>(tile.data());
    for(qint64 i = 0; i < count; ++i){
        const int ty = int(i / cols), tx = int(i % cols);
        const int tw = qMin(64, w - tx * 64), th = qMin(64, h - ty * 64), px = tw * th;
        const qint64 at = tiles[int(i)];
        qint64 end = tiles[int(i) + 1] > at ? tiles[int(i) + 1] : r.n;   // the next tile, else the file's end
        end = qMin(end, r.n);
        bool ok = at > 0 && at < r.n;
        if(ok){
            if(x.comp == 0) ok = end - at >= qint64(px) * bpp && (std::memcpy(T, r.p + at, size_t(px) * bpp), true);
            else if(x.comp == 1) ok = rleTile(r.p + at, end - at, T, px, bpp);
            else if(x.comp == 2) ok = zlibTile(r.p + at, end - at, T, qint64(px) * bpp);
            else ok = false;
        }
        if(!ok){ if(badTiles) ++*badTiles; continue; }
        for(int y = 0; y < th; ++y)
            std::memcpy(O + (qint64(ty * 64 + y) * w + tx * 64) * bpp, T + qint64(y) * tw * bpp, size_t(tw) * bpp);
    }
    return out;
}

// One channel sample as 0..1.
inline float sample(const Xcf& x, const uchar* s){
    switch(x.bpc()){
    case 1: return s[0] / 255.0f;
    case 2: {
        const quint16 v = quint16(s[0] << 8 | s[1]);
        if(!x.isFloat()) return v / 65535.0f;
        const int e = (v >> 10) & 31, m = v & 1023; float f;
        if(e == 0) f = std::ldexp(float(m), -24); else if(e == 31) f = m ? 0.0f : 65504.0f; else f = std::ldexp(float(m | 1024), e - 25);
        return (v & 0x8000) ? -f : f; }
    case 4: {
        const quint32 v = quint32(s[0]) << 24 | quint32(s[1]) << 16 | quint32(s[2]) << 8 | s[3];
        if(!x.isFloat()) return float(v / 4294967295.0);
        float f; std::memcpy(&f, &v, 4); return std::isfinite(f) ? f : 0.0f; }
    default: {
        quint64 v = 0; for(int i = 0; i < 8; ++i) v = v << 8 | s[i];
        double f; std::memcpy(&f, &v, 8); return std::isfinite(f) ? float(f) : 0.0f; }
    }
}

struct Lut { uchar enc[4097]; Lut(){ for(int i = 0; i <= 4096; ++i){ const double c = i / 4096.0; enc[i] = uchar(qRound(255.0 * (c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1 / 2.4) - 0.055))); } } };
inline uchar toByte(float v, bool encode){
    static const Lut lut;
    v = qBound(0.0f, v, 1.0f);
    return encode ? lut.enc[int(v * 4096.0f + 0.5f)] : uchar(v * 255.0f + 0.5f);
}

// Layer types: 0 RGB, 1 RGBA, 2 grey, 3 grey + alpha, 4 indexed, 5 indexed + alpha.
QImage layerImage(const Xcf& x, const QByteArray& px, int w, int h, int type){
    const int ch = type == 0 ? 3 : type == 1 ? 4 : (type == 2 || type == 4) ? 1 : 2;
    const int bpc = (type >= 4) ? 1 : x.bpc(), bpp = ch * bpc;
    QImage img(w, h, QImage::Format_ARGB32);
    if(img.isNull()) return img;
    const uchar* s = reinterpret_cast<const uchar*>(px.constData());
    const bool enc = x.linear() && type < 4;
    for(int y = 0; y < h; ++y){
        QRgb* d = reinterpret_cast<QRgb*>(img.scanLine(y));
        for(int xx = 0; xx < w; ++xx){
            const uchar* p = s + (qint64(y) * w + xx) * bpp;
            if(type >= 4){
                const QRgb c = p[0] < x.cmap.size() ? x.cmap[p[0]] : qRgb(p[0], p[0], p[0]);
                d[xx] = (c & 0xffffffu) | quint32(type == 5 ? p[1] : 255) << 24;
            } else if(bpc == 1 && !enc){
                if(ch >= 3) d[xx] = qRgba(p[0], p[1], p[2], ch == 4 ? p[3] : 255);
                else d[xx] = qRgba(p[0], p[0], p[0], ch == 2 ? p[1] : 255);
            } else {
                const float a = (ch == 4 || ch == 2) ? sample(x, p + (ch - 1) * bpc) : 1.0f;
                const uchar A = toByte(a, false);
                if(ch >= 3) d[xx] = qRgba(toByte(sample(x, p), enc), toByte(sample(x, p + bpc), enc), toByte(sample(x, p + 2 * bpc), enc), A);
                else { const uchar g = toByte(sample(x, p), enc); d[xx] = qRgba(g, g, g, A); }
            }
        }
    }
    return img;
}

// A layer or group mask (a channel structure): coverage as a greyscale image.
QImage maskImage(const Xcf& x, qint64 mp, int w, int h, int* badTiles){
    const Rd& r = x.r;
    if(mp <= 0 || mp + 8 > r.n || int(r.u32(mp)) != w || int(r.u32(mp + 4)) != h) return QImage();
    qint64 o = mp + 8; readString(r, o);
    Props props; if(!readProps(r, o, props)) return QImage();
    const QByteArray px = readPixels(x, x.ptr(o), w, h, x.bpc(), badTiles);
    if(px.isEmpty()) return QImage();
    QImage m(w, h, QImage::Format_Grayscale8);
    if(m.isNull()) return m;
    const uchar* s = reinterpret_cast<const uchar*>(px.constData());
    for(int y = 0; y < h; ++y){
        uchar* d = m.scanLine(y);
        for(int xx = 0; xx < w; ++xx) d[xx] = toByte(sample(x, s + (qint64(y) * w + xx) * x.bpc()), false);
    }
    return m;
}

// GIMP layer modes, legacy (0-22) and 2.10's (23-61), as the PSD route's blend keys; empty where there's no match.
QByteArray modeKey(quint32 m){
    switch(m){
    case 0: case 28: case 1: return "norm";
    case 3: case 30: return "mul ";
    case 4: case 31: return "scrn";
    case 23: return "over";
    case 5: case 19: case 45: return "sLit";
    case 6: case 32: return "diff";
    case 7: case 33: return "lddg";
    case 8: case 34: return "fsub";
    case 9: case 35: return "dark";
    case 10: case 36: return "lite";
    case 11: case 24: case 37: return "hue ";
    case 12: case 25: case 38: return "sat ";
    case 13: case 26: case 39: return "colr";
    case 14: case 27: case 40: case 56: return "lum ";
    case 15: case 41: return "fdiv";
    case 16: case 42: return "div ";
    case 17: case 43: return "idiv";
    case 18: case 44: return "hLit";
    case 48: return "vLit";
    case 49: return "pLit";
    case 50: return "lLit";
    case 51: return "hMix";
    case 52: return "smud";
    case 53: return "lbrn";
    case 54: return "dkCl";
    case 55: return "lgCl";
    case 61: return "pass";
    default: return QByteArray();       // behind, grain extract/merge, colour erase, erase, merge, split
    }
}

struct Node {
    Shape shape; bool group = false; QVector<quint32> path; QVector<int> kids;   // kids topmost first
    QByteArray mode; double opacity = 1; bool visible = true;
    QImage groupMask; QRect maskArea;
};

QByteArray gunzip(const QByteArray& in){
    z_stream z; std::memset(&z, 0, sizeof z);
    if(inflateInit2(&z, 16 + MAX_WBITS) != Z_OK) return QByteArray();
    QByteArray out; const qint64 cap = qint64(1) << 31;
    z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.constData())); z.avail_in = uInt(in.size());
    char buf[1 << 16]; int rc;
    do {
        z.next_out = reinterpret_cast<Bytef*>(buf); z.avail_out = sizeof buf;
        rc = inflate(&z, Z_NO_FLUSH);
        if(rc != Z_OK && rc != Z_STREAM_END){ out.clear(); break; }
        out.append(buf, int(sizeof buf - z.avail_out));
        if(out.size() > cap - qint64(sizeof buf)){ out.clear(); break; }
    } while(rc != Z_STREAM_END);
    inflateEnd(&z);
    return out;
}

} // namespace

Document importXcfDocument(const QByteArray& raw, XcfImportStats* st){
    Document doc;
    QByteArray data = raw;
    if(raw.size() > 2 && uchar(raw[0]) == 0x1f && uchar(raw[1]) == 0x8b) data = gunzip(raw);
    else if(raw.startsWith("BZh")){ if(st) st->bzip2 = true; return doc; }
    if(data.size() < 30 || !data.startsWith("gimp xcf ")) return doc;
    Xcf x; x.r = { reinterpret_cast<const uchar*>(data.constData()), data.size() };
    const Rd& r = x.r;
    const QByteArray tag = data.mid(9, 4);
    if(tag == "file") x.ver = 0;
    else if(tag.size() == 4 && tag[0] == 'v'){ bool ok = false; x.ver = tag.mid(1).toInt(&ok); if(!ok) return doc; }
    else return doc;
    if(x.ver >= 100) return doc;                         // CinePaint's XCF, a different format
    qint64 o = 14;
    x.W = int(r.u32(o)); x.H = int(r.u32(o + 4)); x.base = int(r.u32(o + 8)); o += 12;
    if(x.W <= 0 || x.H <= 0 || x.W > 524288 || x.H > 524288) return doc;
    if(x.ver >= 4){
        x.prec = int(r.u32(o)); o += 4;
        if(x.ver == 4){ static const int m[] = { 150, 250, 300, 500, 600 }; x.prec = (x.prec >= 0 && x.prec < 5) ? m[x.prec] : 150; }
        else if(x.ver <= 6){ if(x.prec == 400) x.prec = 500; else if(x.prec == 450) x.prec = 550; else if(x.prec == 500) x.prec = 600; else if(x.prec == 550) x.prec = 650; }
    }
    static const int precs[] = { 100, 150, 200, 250, 300, 350, 500, 550, 600, 650, 700, 750 };
    if(std::find(std::begin(precs), std::end(precs), x.prec) == std::end(precs)) return doc;
    x.psz = x.ver >= 11 ? 8 : 4;
    Props ip; if(!readProps(r, o, ip)) return doc;
    x.comp = ip.contains(17) && !ip.value(17).isEmpty() ? uchar(ip.value(17)[0]) : 0;
    if(ip.contains(1) && ip.value(1).size() >= 4){
        const QByteArray cm = ip.value(1); const uchar* c = reinterpret_cast<const uchar*>(cm.constData());
        const int n = int(qMin<quint32>(256, quint32(c[0]) << 24 | quint32(c[1]) << 16 | quint32(c[2]) << 8 | c[3]));
        for(int i = 0; i < n && 4 + i * 3 + 2 < cm.size(); ++i) x.cmap.push_back(qRgb(c[4 + i * 3], c[5 + i * 3], c[6 + i * 3]));
    }
    if(st) st->linear = x.linear();
    doc.pageSize = QSizeF(x.W, x.H);

    QVector<qint64> lptrs;
    for(int i = 0; i < 100000; ++i){
        const qint64 p = x.ptr(o); o += x.psz;
        if(p <= 0) break;
        lptrs.push_back(p);
    }
    QVector<Node> nodes; QVector<int> root; QHash<QVector<quint32>, int> groups;
    int badTiles = 0;
    for(const qint64 lp : lptrs){
        if(lp + 12 > r.n) continue;
        const int w = int(r.u32(lp)), h = int(r.u32(lp + 4)), type = int(r.u32(lp + 8));
        qint64 q = lp + 12;
        const QString name = readString(r, q);
        Props lpr; if(!readProps(r, q, lpr)) continue;
        if(q + 2 * x.psz > r.n) continue;
        const qint64 hp = x.ptr(q), mp = x.ptr(q + x.psz);
        Node nd; nd.group = lpr.contains(29);
        nd.visible = propU32(lpr, 8, 1) != 0;
        nd.opacity = propU32(lpr, 6, 255) / 255.0;
        if(lpr.value(33).size() == 4){ const quint32 v = propU32(lpr, 33, 0); float f; std::memcpy(&f, &v, 4); if(std::isfinite(f)) nd.opacity = f; }
        nd.opacity = qBound(0.0, nd.opacity, 1.0);
        const quint32 mode = propU32(lpr, 7, 0);
        nd.mode = modeKey(mode);
        if(nd.mode.isEmpty()){ nd.mode = "norm"; if(st) ++st->unknownModes; }
        if(mode == 1 && st) ++st->dissolve;
        { const QByteArray pb = lpr.value(30); for(int i = 0; i + 4 <= pb.size() && i < 256; i += 4) nd.path.push_back(be32(pb, i)); }
        const int ox = qBound(-10000000, int(qint32(be32(lpr.value(15), 0))), 10000000);
        const int oy = qBound(-10000000, int(qint32(be32(lpr.value(15), 4))), 10000000);
        const bool applyMask = propU32(lpr, 11, 1) != 0 && mp > 0;
        if(w <= 0 || h <= 0 || qint64(w) * h > kMaxPixels || type < 0 || type > 5) continue;
        if(nd.group){
            nd.shape.type = ShapeType::Group; nd.shape.name = name;
            if(applyMask){ nd.groupMask = maskImage(x, mp, w, h, &badTiles); nd.maskArea = QRect(ox, oy, w, h); if(st && !nd.groupMask.isNull()) ++st->masks; }
            if(st) ++st->groups;
        } else {
            const int ch = type == 0 ? 3 : type == 1 ? 4 : (type == 2 || type == 4) ? 1 : 2;
            const int bpp = ch * (type >= 4 ? 1 : x.bpc());
            const QByteArray px = readPixels(x, hp, w, h, bpp, &badTiles);
            if(px.isEmpty()){ ++badTiles; continue; }
            QImage img = layerImage(x, px, w, h, type);
            if(img.isNull()) continue;
            if(applyMask){
                const QImage m = maskImage(x, mp, w, h, &badTiles);
                if(!m.isNull()){
                    for(int y = 0; y < h; ++y){
                        QRgb* d = reinterpret_cast<QRgb*>(img.scanLine(y)); const uchar* a = m.constScanLine(y);
                        for(int xx = 0; xx < w; ++xx) d[xx] = (d[xx] & 0xffffffu) | quint32(qAlpha(d[xx]) * a[xx] / 255) << 24;
                    }
                    if(st) ++st->masks;
                }
            }
            nd.shape = psdBitmapShape(img, ox, oy, name);
            if(st) ++st->layers;
        }
        const int idx = nodes.size();
        nodes.push_back(nd);
        int parent = -1;
        if(nd.path.size() > 1){ const QVector<quint32> pp = nd.path.mid(0, nd.path.size() - 1); parent = groups.value(pp, -1); }
        if(parent >= 0) nodes[parent].kids.push_back(idx); else root.push_back(idx);
        if(nd.group && !nd.path.isEmpty()) groups.insert(nd.path, idx);
    }
    if(st) st->badTiles = badTiles;

    // GIMP draws the bottommost visible layer as Normal whatever its mode.
    for(int i = root.size() - 1; i >= 0; --i)
        if(nodes[root[i]].visible){ if(!nodes[root[i]].group) nodes[root[i]].mode = "norm"; break; }

    std::function<Shape(int, int)> build = [&](int i, int depth) -> Shape {
        Node& nd = nodes[i];
        Shape s = nd.shape;
        if(nd.group){
            s.children.clear();
            if(depth < 64) for(int k = nd.kids.size() - 1; k >= 0; --k) s.children.push_back(build(nd.kids[k], depth + 1));
            const bool pass = nd.mode == "pass";
            psdApplyBlend(s, pass ? QByteArrayLiteral("norm") : nd.mode);
            s.isolate = (!pass && nd.mode != "norm") || nd.opacity < 0.999;
            if(!nd.groupMask.isNull()){ s.isolate = true; psdDocMask(s, nd.groupMask, nd.maskArea); }
        } else psdApplyBlend(s, nd.mode);
        s.opacity = nd.opacity;
        s.visible = nd.visible;
        return s;
    };
    for(int k = root.size() - 1; k >= 0; --k) doc.shapes.push_back(build(root[k], 0));
    return doc;
}
} // namespace xara
