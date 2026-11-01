// Photoshop PSD / PSB import. Layers become native objects: pixel layers are bitmaps, shape and fill layers
// are real paths, text layers are live text when their font is installed, groups stay groups. What the model
// has no live equivalent for is baked: masks and clipping into alpha, adjustments into the layers beneath
// them, layer effects into bitmaps of their own stacked around the layer.
#include "prepress.h"
#include "icc.h"
#include "render.h"
#include "svgimport.h"

#include <QBuffer>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QHash>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <algorithm>
#include <functional>
#include <memory>
#include <cmath>
#include <cstring>
#include <zlib.h>

namespace xara {
namespace {

constexpr qint64 kMaxPsdPixels = 30000000;        // one PSD image or layer
constexpr qint64 kMaxPsbPixels = 200000000;       // one PSB image or layer
constexpr qint64 kMaxTotalPixels = 512000000;     // decoded across a file's layers
constexpr int    kMaxDepth = 32;                  // nesting in descriptors, engine data and groups
constexpr int    kMaxLayers = 65535;

struct Rd {
    const QByteArray& d; qint64 N;
    explicit Rd(const QByteArray& b) : d(b), N(b.size()) {}
    int u8(qint64 o) const { return (o < 0 || o >= N) ? 0 : quint8(d[o]); }
    int u16(qint64 o) const { return (o < 0 || o + 1 >= N) ? 0 : (quint8(d[o]) << 8) | quint8(d[o+1]); }
    int i16(qint64 o) const { return qint16(u16(o)); }
    qint64 u32(qint64 o) const { return (o < 0 || o + 3 >= N) ? 0 : (qint64(quint8(d[o])) << 24) | (quint8(d[o+1]) << 16) | (quint8(d[o+2]) << 8) | quint8(d[o+3]); }
    int i32(qint64 o) const { return int(qint32(quint32(u32(o)))); }
    qint64 u64(qint64 o) const { if(o < 0 || o + 7 >= N) return 0; quint64 v = 0; for(int i = 0; i < 8; ++i) v = (v << 8) | quint8(d[o+i]); return v > quint64(N) * 2 ? N * 2 : qint64(v); }
    double f64(qint64 o) const { if(o < 0 || o + 7 >= N) return 0.0; quint64 v = 0; for(int i = 0; i < 8; ++i) v = (v << 8) | quint8(d[o+i]); double r; memcpy(&r, &v, 8); return std::isfinite(r) ? r : 0.0; }
    float f32(qint64 o) const { quint32 v = quint32(u32(o)); float r; memcpy(&r, &v, 4); return std::isfinite(r) ? r : 0.f; }
};

// ---------------------------------------------------------------- generic tree values
// Both Photoshop's action descriptors and the text engine's PostScript-like data read into this.
struct Val {
    enum Kind { None, Num, Str, Obj, List } kind = None;
    double num = 0;
    QString str;
    QByteArray cls;
    QList<QPair<QByteArray, Val>> items;
    QList<Val> list;
    const Val* get(const char* k) const { for(const auto& kv : items) if(kv.first == k) return &kv.second; return nullptr; }
    double n(const char* k, double def = 0.0) const { const Val* v = get(k); return v && v->kind == Num ? v->num : def; }
    QString s(const char* k) const { const Val* v = get(k); return v ? v->str : QString(); }
};

struct DescReader {
    Rd r; qint64 p, end;
    DescReader(const QByteArray& d, qint64 o, qint64 e) : r(d), p(o), end(qMin(e, qint64(d.size()))) {}
    bool ok() const { return p <= end; }
    QByteArray key(){
        qint64 len = r.u32(p); p += 4;
        if(len == 0) len = 4;
        if(len < 0 || len > end - p){ p = end + 1; return {}; }
        QByteArray k = r.d.mid(p, len).trimmed(); p += len;
        return k;
    }
    QString uni(){
        qint64 len = r.u32(p); p += 4;
        if(len < 0 || len * 2 > end - p){ p = end + 1; return {}; }
        QString s; s.reserve(int(len));
        for(qint64 i = 0; i < len; ++i){ const int c = r.u16(p + i * 2); if(c) s += QChar(ushort(c)); }
        p += len * 2;
        return s;
    }
    Val desc(int depth){
        Val v; v.kind = Val::Obj;
        v.str = uni();
        v.cls = key();
        const qint64 count = r.u32(p); p += 4;
        for(qint64 i = 0; i < count && ok() && p < end; ++i){
            const QByteArray k = key();
            v.items.push_back({ k, value(depth + 1) });
        }
        return v;
    }
    Val value(int depth){
        Val v;
        if(depth > kMaxDepth || p + 4 > end){ p = end + 1; return v; }
        const QByteArray t = r.d.mid(p, 4); p += 4;
        if(t == "Objc" || t == "GlbO") return desc(depth);
        if(t == "VlLs"){
            v.kind = Val::List;
            const qint64 c = r.u32(p); p += 4;
            for(qint64 i = 0; i < c && ok() && p < end; ++i) v.list.push_back(value(depth + 1));
        } else if(t == "doub"){ v.kind = Val::Num; v.num = r.f64(p); p += 8; }
        else if(t == "UntF" || t == "UnFl"){
            v.kind = Val::Num; v.cls = r.d.mid(p, 4); p += 4;
            if(t == "UnFl"){ const qint64 c = r.u32(p); p += 4; v.num = r.f64(p); p += qMin(c, end) * 8; }
            else { v.num = r.f64(p); p += 8; }
        }
        else if(t == "long"){ v.kind = Val::Num; v.num = r.i32(p); p += 4; }
        else if(t == "comp"){ v.kind = Val::Num; v.num = double(r.u64(p)); p += 8; }
        else if(t == "bool"){ v.kind = Val::Num; v.num = r.u8(p); p += 1; }
        else if(t == "TEXT"){ v.kind = Val::Str; v.str = uni(); }
        else if(t == "enum"){ v.kind = Val::Str; v.cls = key(); v.str = QString::fromLatin1(key()); }
        else if(t == "type" || t == "GlbC"){ uni(); v.kind = Val::Str; v.str = QString::fromLatin1(key()); }
        else if(t == "tdta" || t == "alis" || t == "Pth "){ const qint64 l = r.u32(p); p += 4; if(l < 0 || l > end - p){ p = end + 1; return v; } v.kind = Val::Str; v.cls = r.d.mid(p, l); p += l; }
        else if(t == "obj "){
            const qint64 c = r.u32(p); p += 4;
            for(qint64 i = 0; i < c && ok(); ++i){
                const QByteArray rt = r.d.mid(p, 4); p += 4;
                if(rt == "prop"){ uni(); key(); key(); }
                else if(rt == "Clss"){ uni(); key(); }
                else if(rt == "Enmr"){ uni(); key(); key(); key(); }
                else if(rt == "rele"){ uni(); key(); p += 4; }
                else if(rt == "Idnt" || rt == "indx"){ p += 4; }
                else if(rt == "name"){ uni(); key(); uni(); }
                else { p = end + 1; }
            }
        }
        else if(t == "ObAr"){ p = end + 1; }             // only in fields read for nothing here; stop cleanly
        else p = end + 1;
        return v;
    }
};

// EngineData: "<< /Key value /Key [ ... ] >>", strings in parentheses, UTF-16 with a BOM.
struct EngineReader {
    const QByteArray& d; qint64 p = 0;
    explicit EngineReader(const QByteArray& b) : d(b) {}
    void ws(){ while(p < d.size() && (quint8(d[p]) <= 32)) ++p; }
    Val value(int depth){
        Val v;
        ws();
        if(p >= d.size() || depth > kMaxDepth){ p = d.size(); return v; }
        const char c = d[p];
        if(c == '<' && p + 1 < d.size() && d[p+1] == '<'){
            p += 2; v.kind = Val::Obj;
            while(true){
                ws();
                if(p >= d.size()) break;
                if(d[p] == '>'){ p += 2; break; }
                if(d[p] != '/'){ ++p; continue; }
                ++p;
                qint64 st = p;
                while(p < d.size() && quint8(d[p]) > 32 && d[p] != '/' && d[p] != '[' && d[p] != '<' && d[p] != '(') ++p;
                const QByteArray k = d.mid(st, p - st);
                v.items.push_back({ k, value(depth + 1) });
            }
            return v;
        }
        if(c == '['){
            ++p; v.kind = Val::List;
            while(true){
                ws();
                if(p >= d.size()) break;
                if(d[p] == ']'){ ++p; break; }
                const qint64 before = p;
                v.list.push_back(value(depth + 1));
                if(p == before) ++p;
            }
            return v;
        }
        if(c == '('){
            ++p; QByteArray raw;
            while(p < d.size() && d[p] != ')'){
                if(d[p] == '\\' && p + 1 < d.size()){ ++p; const char e = d[p]; raw += e == 'r' ? '\r' : e == 'n' ? '\n' : e; ++p; }
                else raw += d[p++];
            }
            ++p;
            v.kind = Val::Str;
            if(raw.startsWith("\xfe\xff")){
                for(qint64 i = 2; i + 1 < raw.size(); i += 2) v.str += QChar(ushort((quint8(raw[i]) << 8) | quint8(raw[i+1])));
            } else v.str = QString::fromLatin1(raw);
            return v;
        }
        qint64 st = p;
        while(p < d.size() && quint8(d[p]) > 32 && d[p] != '/' && d[p] != ']' && d[p] != '>' && d[p] != '[' && d[p] != '<' && d[p] != '(') ++p;
        const QByteArray tok = d.mid(st, p - st);
        if(tok == "true"){ v.kind = Val::Num; v.num = 1; }
        else if(tok == "false"){ v.kind = Val::Num; v.num = 0; }
        else { bool ok = false; const double x = tok.toDouble(&ok); if(ok && std::isfinite(x)){ v.kind = Val::Num; v.num = x; } if(p == st) ++p; }
        return v;
    }
};

// ---------------------------------------------------------------- pixels
QByteArray inflateAll(const char* src, qint64 n, qint64 want){
    QByteArray out(want, 0);
    z_stream z; memset(&z, 0, sizeof z);
    if(inflateInit(&z) != Z_OK) return {};
    z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(src)); z.avail_in = uInt(qMin(n, qint64(0x7fffffff)));
    z.next_out = reinterpret_cast<Bytef*>(out.data()); z.avail_out = uInt(want);
    inflate(&z, Z_FINISH);
    inflateEnd(&z);
    return out;
}

double linToSrgb(double v){ v = qBound(0.0, v, 1.0); return v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055; }

// One channel at [o,end) as 8 bits per sample, w*h bytes. Short or damaged data leaves zeros.
// comp 0 raw, 1 PackBits, 2 ZIP, 3 ZIP with per-row prediction. rleCounts >= 0 overrides where the counts live.
QByteArray decodePlane(const Rd& r, qint64 o, qint64 end, int comp, int w, int h, int depth, bool psb){
    const qint64 rowBytes = depth == 1 ? (qint64(w) + 7) / 8 : qint64(w) * (depth / 8);
    const qint64 total = rowBytes * h;
    end = qMin(end, r.N);
    QByteArray raw;
    if(comp == 0){
        raw = r.d.mid(o, qMin(total, qMax<qint64>(0, end - o)));
        raw.resize(total, 0);
    } else if(comp == 1){
        raw = QByteArray(total, 0);
        const int cs = psb ? 4 : 2;
        qint64 p = o + cs * qint64(h);
        for(int y = 0; y < h && p < end; ++y){
            const qint64 rc = psb ? r.u32(o + y * cs) : r.u16(o + y * cs);
            const qint64 rowEnd = qMin(p + rc, end);
            char* out = raw.data() + rowBytes * y;
            qint64 col = 0;
            while(p < rowEnd && col < rowBytes){
                const qint8 n = qint8(r.d[p++]);
                if(n >= 0){ const qint64 c = qMin<qint64>(n + 1, qMin(rowBytes - col, rowEnd - p)); if(c > 0){ memcpy(out + col, r.d.constData() + p, size_t(c)); col += c; } p += n + 1; }
                else if(n != -128 && p < rowEnd){ const qint64 c = qMin<qint64>(1 - n, rowBytes - col); memset(out + col, r.d[p++], size_t(c)); col += c; }
            }
            p = rowEnd;
        }
    } else if(comp == 2 || comp == 3){
        if(end <= o) return QByteArray(qint64(w) * h, 0);
        raw = inflateAll(r.d.constData() + o, end - o, total);
        if(raw.size() != total) raw = QByteArray(total, 0);
        if(comp == 3){
            uchar* b = reinterpret_cast<uchar*>(raw.data());
            if(depth == 16){
                for(int y = 0; y < h; ++y){ uchar* q = b + rowBytes * y;
                    for(int x = 1; x < w; ++x){ const int v = ((q[x*2-2] << 8 | q[x*2-1]) + (q[x*2] << 8 | q[x*2+1])) & 0xffff; q[x*2] = uchar(v >> 8); q[x*2+1] = uchar(v); } }
            } else if(depth == 32){
                // Byte-wise deltas over the row, then the bytes are planar: all MSBs first.
                QByteArray rowCopy(rowBytes, 0);
                for(int y = 0; y < h; ++y){ uchar* q = b + rowBytes * y;
                    for(qint64 i = 1; i < rowBytes; ++i) q[i] = uchar(q[i] + q[i-1]);
                    memcpy(rowCopy.data(), q, size_t(rowBytes));
                    for(int x = 0; x < w; ++x) for(int k = 0; k < 4; ++k) q[x*4+k] = uchar(rowCopy[qint64(k)*w + x]);
                }
            }
        }
    } else return QByteArray(qint64(w) * h, 0);
    if(depth == 8) return raw;
    QByteArray out(qint64(w) * h, 0);
    uchar* q = reinterpret_cast<uchar*>(out.data());
    const uchar* b = reinterpret_cast<const uchar*>(raw.constData());
    if(depth == 16){ for(qint64 i = 0; i < qint64(w) * h; ++i) q[i] = b[i*2]; }
    else if(depth == 1){ for(int y = 0; y < h; ++y) for(int x = 0; x < w; ++x) q[qint64(y)*w+x] = (b[rowBytes*y + x/8] >> (7 - x%8)) & 1 ? 0 : 255; }
    else if(depth == 32){
        for(qint64 i = 0; i < qint64(w) * h; ++i){
            quint32 v = (quint32(b[i*4]) << 24) | (b[i*4+1] << 16) | (b[i*4+2] << 8) | b[i*4+3];
            float f; memcpy(&f, &v, 4);
            q[i] = uchar(qBound(0, int(255.0 * (std::isfinite(f) ? double(f) : 0.0) + 0.5), 255));
        }
    }
    return out;
}

// 32-bit documents hold linear light; colour planes need gamma, alpha and masks must not get it.
void gammaPlane(QByteArray& raw8){
    static const QByteArray lut = []{ QByteArray t(256, 0); for(int i = 0; i < 256; ++i) t[i] = char(qBound(0, int(linToSrgb(i / 255.0) * 255.0 + 0.5), 255)); return t; }();
    uchar* q = reinterpret_cast<uchar*>(raw8.data());
    for(qint64 i = 0; i < raw8.size(); ++i) q[i] = uchar(lut[q[i]]);
}

QRgb labToRgb(int L8, int a8, int b8){
    const double L = L8 * 100.0 / 255.0, A = a8 - 128.0, B = b8 - 128.0;
    const double fy = (L + 16.0) / 116.0, fx = fy + A / 500.0, fz = fy - B / 200.0;
    auto g = [](double t){ return t > 6.0/29.0 ? t*t*t : 3.0*(6.0/29.0)*(6.0/29.0)*(t - 4.0/29.0); };
    const double X = 0.9642*g(fx), Y = g(fy), Z = 0.8249*g(fz);              // D50 white
    const double r = 3.1338561*X - 1.6168667*Y - 0.4906146*Z, gr = -0.9787684*X + 1.9161415*Y + 0.0334540*Z, b = 0.0719453*X - 0.2289914*Y + 1.4052427*Z;
    auto e = [](double v){ return qBound(0, int(linToSrgb(v) * 255.0 + 0.5), 255); };
    return qRgb(e(r), e(gr), e(b));
}

struct ColorInfo { int mode = 3; QByteArray table; std::shared_ptr<IccToSrgb> icc; };

// The profile an image declares, unless it is already sRGB, where a conversion could only add rounding.
std::shared_ptr<IccToSrgb> documentProfile(const Rd& r, qint64 resStart, qint64 resEnd, int mode){
    const int comps = (mode == 1) ? 1 : mode == 3 ? 3 : mode == 4 ? 4 : 0;
    if(!comps) return {};
    qint64 p = resStart;
    while(p + 12 <= resEnd && r.d.mid(p, 4) == "8BIM"){
        const int id = r.u16(p + 4), nl = r.u8(p + 6);
        qint64 q = p + 6 + ((nl + 2) / 2) * 2;
        const qint64 sz = r.u32(q); q += 4;
        if(sz < 0 || sz > resEnd - q) return {};
        if(id == 1039){
            const QByteArray pr = r.d.mid(q, sz);
            if(pr.contains("sRGB") || pr.contains(QByteArray::fromHex("0073005200470042"))) return {};
            auto xf = std::make_shared<IccToSrgb>(pr, comps);
            return xf->ok() ? xf : nullptr;
        }
        p = q + sz + (sz & 1);
    }
    return {};
}

// Stored channels to pixels. CMYK and multichannel samples are stored inverted (0 = full ink).
QImage planesToImage(const ColorInfo& ci, int w, int h, const QHash<int, QByteArray>& pl, const QByteArray* alpha){
    QImage img(w, h, QImage::Format_ARGB32);
    if(img.isNull()) return img;
    const QByteArray none;
    const QByteArray& p0 = pl.contains(0) ? pl[0] : none, &p1 = pl.contains(1) ? pl[1] : none, &p2 = pl.contains(2) ? pl[2] : none, &p3 = pl.contains(3) ? pl[3] : none;
    const qint64 n = qint64(w) * h;
    auto at = [n](const QByteArray& b, qint64 i)->int{ return b.size() >= n ? quint8(b[i]) : 0; };
    for(int y = 0; y < h; ++y){
        QRgb* row = reinterpret_cast<QRgb*>(img.scanLine(y));
        for(int x = 0; x < w; ++x){
            const qint64 i = qint64(y) * w + x;
            QRgb c;
            switch(ci.mode){
                case 0: case 1: case 8: { const int v = at(p0, i); c = qRgb(v, v, v); break; }
                case 2: { const int v = at(p0, i); c = ci.table.size() >= 768 ? qRgb(quint8(ci.table[v]), quint8(ci.table[256+v]), quint8(ci.table[512+v])) : qRgb(v, v, v); break; }
                case 4: { const int k = p3.isEmpty() ? 255 : at(p3, i); c = qRgb(at(p0,i)*k/255, at(p1,i)*k/255, at(p2,i)*k/255); break; }
                case 7: { c = qRgb(at(p0,i), p1.isEmpty() ? at(p0,i) : at(p1,i), p2.isEmpty() ? at(p0,i) : at(p2,i)); break; }
                case 9: c = labToRgb(at(p0,i), at(p1,i), at(p2,i)); break;
                default: c = qRgb(at(p0,i), at(p1,i), at(p2,i)); break;
            }
            const int a = alpha && alpha->size() >= n ? quint8((*alpha)[i]) : 255;
            row[x] = (c & 0xffffffu) | (uint(a) << 24);
        }
    }
    if(ci.icc && !pl.isEmpty()){
        const int comps = ci.mode == 1 ? 1 : ci.mode == 3 ? 3 : 4;
        QByteArray in(qint64(w) * comps, 0), out(qint64(w) * 3, 0);
        for(int y = 0; y < h; ++y){
            uchar* q = reinterpret_cast<uchar*>(in.data());
            const qint64 base = qint64(y) * w;
            for(int x = 0; x < w; ++x){
                if(comps == 1) q[x] = uchar(at(p0, base + x));
                else if(comps == 3){ q[x*3] = uchar(at(p0, base + x)); q[x*3+1] = uchar(at(p1, base + x)); q[x*3+2] = uchar(at(p2, base + x)); }
                else { q[x*4] = uchar(255 - at(p0, base + x)); q[x*4+1] = uchar(255 - at(p1, base + x)); q[x*4+2] = uchar(255 - at(p2, base + x)); q[x*4+3] = uchar(p3.isEmpty() ? 0 : 255 - at(p3, base + x)); }
            }
            ci.icc->convert(q, reinterpret_cast<uchar*>(out.data()), w);
            QRgb* row = reinterpret_cast<QRgb*>(img.scanLine(y));
            const uchar* o = reinterpret_cast<const uchar*>(out.constData());
            for(int x = 0; x < w; ++x) row[x] = (row[x] & 0xff000000u) | (uint(o[x*3]) << 16) | (uint(o[x*3+1]) << 8) | o[x*3+2];
        }
    }
    return img;
}
int baseChannels(int mode){ return mode == 3 || mode == 9 ? 3 : mode == 4 ? 4 : 1; }

// ---------------------------------------------------------------- layer records
struct Span { qint64 o = 0, len = 0; };
struct LChan { int id; qint64 len; };
struct Lay {
    int t = 0, l = 0, b = 0, r = 0;
    QVector<LChan> chans;
    QByteArray blend, sectBlend;
    int opacity = 255, fillOpacity = 255, section = 0;
    bool clipping = false, visible = true;
    int blendIf[8] = { 0, 0, 255, 255, 0, 0, 255, 255 };   // composite grey: this layer's range, then the backdrop's
    QString name;
    QHash<QByteArray, Span> blocks;
    QRect maskRect; int maskDefault = 0; bool maskOff = false; double maskDensity = 1.0, vecDensity = 1.0, maskFeather = 0.0, vecFeather = 0.0;
    QRect realRect; int realDefault = 0; bool maskRendered = false;
    QImage pix;             // ARGB32
    QImage native;          // CMYK documents: the stored channels as C,M,Y in RGB and K in alpha
    QByteArray mask;        // maskRect-sized 8-bit plane
};

bool bigKey(const QByteArray& k){
    static const char* const big[] = { "LMsk","Lr16","Lr32","Layr","Mt16","Mt32","Mtrn","Alph","FMsk","lnk2","lnk3","lnkE","FEid","FXid","PxSD","pths","extd","extn","cinf","artd" };
    for(const char* b : big) if(k == b) return true;
    return false;
}
void readBlocks(const Rd& r, qint64 o, qint64 end, bool psb, int pad, QHash<QByteArray, Span>& out){
    while(o + 12 <= end){
        const QByteArray sig = r.d.mid(o, 4);
        if(sig != "8BIM" && sig != "8B64"){ ++o; if(r.d.mid(o, 4) != "8BIM" && r.d.mid(o, 4) != "8B64") return; continue; }
        const QByteArray key = r.d.mid(o + 4, 4);
        const bool wide = psb && bigKey(key);
        const qint64 len = wide ? r.u64(o + 8) : r.u32(o + 8);
        const qint64 body = o + 8 + (wide ? 8 : 4);
        if(len < 0 || len > end - body) return;
        out.insert(key, { body, len });
        qint64 adv = len;
        if(pad > 1 && adv % pad) adv += pad - adv % pad;
        o = body + adv;
    }
}

// The layer records and their channels. False leaves whatever parsed in `out`.
bool readLayerInfo(const Rd& r, qint64 o, qint64 end, bool psb, int depth, const ColorInfo& ci, int maxDim, qint64 maxPix, int& rawCount, QVector<Lay>& out){
    const qint16 count = qint16(r.u16(o)); o += 2;
    rawCount = count;
    const int n = qMin(qAbs(int(count)), kMaxLayers);
    QVector<Lay> lays;
    for(int i = 0; i < n; ++i){
        if(o + 18 > end) return false;
        Lay L;
        L.t = r.i32(o); L.l = r.i32(o+4); L.b = r.i32(o+8); L.r = r.i32(o+12); o += 16;
        const int nc = r.u16(o); o += 2;
        if(nc > 64) return false;
        for(int c = 0; c < nc; ++c){
            LChan ch; ch.id = r.i16(o); ch.len = psb ? r.u64(o+2) : r.u32(o+2); o += psb ? 10 : 6;
            if(ch.len < 0 || ch.len > r.N) return false;
            L.chans.push_back(ch);
        }
        if(r.d.mid(o, 4) != "8BIM") return false;
        L.blend = r.d.mid(o+4, 4); o += 8;
        L.opacity = r.u8(o); L.clipping = r.u8(o+1) != 0;
        const int flags = r.u8(o+2); L.visible = !(flags & 2);
        o += 4;
        const qint64 extra = r.u32(o); o += 4;
        if(extra < 0 || extra > end - o) return false;
        const qint64 xEnd = o + extra;
        qint64 q = o;
        const qint64 ml = r.u32(q); q += 4;
        if(ml >= 20 && ml <= xEnd - q){
            L.maskRect = QRect(QPoint(r.i32(q+4), r.i32(q)), QPoint(r.i32(q+12) - 1, r.i32(q+8) - 1));
            L.maskDefault = r.u8(q+16);
            const int mf = r.u8(q+17);
            L.maskOff = mf & 2;
            L.maskRendered = mf & 8;
            qint64 m = q + 18;
            // The real mask's header exists only with a -3 channel, and comes before the parameters.
            if(ml >= 36 && std::any_of(L.chans.begin(), L.chans.end(), [](const LChan& c){ return c.id == -3; })){
                L.realDefault = r.u8(m + 1);
                L.realRect = QRect(QPoint(r.i32(m+6), r.i32(m+2)), QPoint(r.i32(m+14) - 1, r.i32(m+10) - 1));
                m += 18;
            }
            if(mf & 0x10){
                const int pf = r.u8(m); ++m;
                if(pf & 1){ L.maskDensity = r.u8(m) / 255.0; ++m; }
                if(pf & 2){ L.maskFeather = qBound(0.0, r.f64(m), 1000.0); m += 8; }
                if(pf & 4){ L.vecDensity = r.u8(m) / 255.0; ++m; }
                if(pf & 8){ L.vecFeather = qBound(0.0, r.f64(m), 1000.0); m += 8; }
            }
        }
        q += qMax<qint64>(0, ml);
        const qint64 br = r.u32(q);
        if(br >= 8) for(int k = 0; k < 8; ++k) L.blendIf[k] = r.u8(q + 4 + k);
        q += 4 + qMax<qint64>(0, br);
        const int nl = r.u8(q);
        L.name = QString::fromLatin1(r.d.mid(q + 1, nl));
        q += ((nl + 1 + 3) / 4) * 4;
        if(q <= xEnd) readBlocks(r, q, xEnd, psb, 1, L.blocks);
        o = xEnd;
        if(L.blocks.contains("luni")){
            const Span s = L.blocks["luni"];
            const qint64 c = r.u32(s.o);
            if(c > 0 && c * 2 + 4 <= s.len){ QString u; for(qint64 k = 0; k < c; ++k) u += QChar(ushort(r.u16(s.o + 4 + k * 2))); L.name = u; }
        }
        if(L.blocks.contains("iOpa")) L.fillOpacity = r.u8(L.blocks["iOpa"].o);
        Span sc = L.blocks.value("lsct"); if(!sc.len) sc = L.blocks.value("lsdk");
        if(sc.len >= 4){ L.section = int(r.u32(sc.o)); if(sc.len >= 12) L.sectBlend = r.d.mid(sc.o + 8, 4); }
        lays.push_back(L);
    }
    qint64 budget = kMaxTotalPixels;
    for(Lay& L : lays){
        const int lw = L.r - L.l, lh = L.b - L.t;
        const bool drawable = lw > 0 && lh > 0 && lw <= maxDim && lh <= maxDim && qint64(lw) * lh <= maxPix && (budget -= qint64(lw) * lh) >= 0;
        QHash<int, QByteArray> planes; QByteArray alpha;
        for(const LChan& ch : L.chans){
            const qint64 chEnd = qMin(r.N, o + ch.len);
            const int comp = r.u16(o);
            const bool isMask = ch.id == -2 || ch.id == -3;
            if(isMask){
                const QRect rc = ch.id == -3 ? L.realRect : L.maskRect;
                // With the render flag, -2 is only the vector mask drawn out; a user's own mask is then -3.
                const bool useThis = ch.id == -3 || (!L.maskRendered && std::none_of(L.chans.begin(), L.chans.end(), [](const LChan& c){ return c.id == -3; }));
                if(useThis && rc.isValid() && rc.width() <= maxDim && rc.height() <= maxDim && qint64(rc.width()) * rc.height() <= maxPix && (budget -= qint64(rc.width()) * rc.height()) >= 0){
                    L.mask = decodePlane(r, o + 2, chEnd, comp, rc.width(), rc.height(), depth, psb);
                    if(ch.id == -3){ L.maskRect = rc; L.maskDefault = L.realDefault; }
                }
            } else if(drawable && ch.id == -1){
                alpha = decodePlane(r, o + 2, chEnd, comp, lw, lh, depth, psb);
            } else if(drawable && ch.id >= 0 && ch.id < 4){
                QByteArray p = decodePlane(r, o + 2, chEnd, comp, lw, lh, depth, psb);
                if(depth == 32) gammaPlane(p);
                planes[ch.id] = p;
            }
            o = chEnd;
        }
        if(drawable && !planes.isEmpty()) L.pix = planesToImage(ci, lw, lh, planes, alpha.isEmpty() ? nullptr : &alpha);
        // Adjustments work on the document's own values; only what's shown goes through its profile.
        if(drawable && ci.icc && ci.mode != 4 && !L.pix.isNull() && planes.size() >= (ci.mode == 3 ? 3 : 1)){
            L.native = QImage(lw, lh, QImage::Format_ARGB32);
            const qint64 n = qint64(lw) * lh;
            const QByteArray& p0 = planes[0]; const QByteArray& p1 = ci.mode == 3 ? planes[1] : planes[0]; const QByteArray& p2 = ci.mode == 3 ? planes[2] : planes[0];
            if(!L.native.isNull() && p0.size() >= n && p1.size() >= n && p2.size() >= n)
                for(int y = 0; y < lh; ++y){ QRgb* q = reinterpret_cast<QRgb*>(L.native.scanLine(y));
                    for(int x = 0; x < lw; ++x){ const qint64 i = qint64(y) * lw + x; q[x] = qRgb(quint8(p0[i]), quint8(p1[i]), quint8(p2[i])); } }
            else L.native = QImage();
        }
        if(drawable && ci.mode == 4 && planes.size() >= 4 && !L.pix.isNull()){
            L.native = QImage(lw, lh, QImage::Format_ARGB32);
            const qint64 n = qint64(lw) * lh;
            if(!L.native.isNull() && planes[0].size() >= n && planes[1].size() >= n && planes[2].size() >= n && planes[3].size() >= n)
                for(int y = 0; y < lh; ++y){ QRgb* q = reinterpret_cast<QRgb*>(L.native.scanLine(y));
                    for(int x = 0; x < lw; ++x){ const qint64 i = qint64(y) * lw + x;
                        q[x] = qRgba(quint8(planes[0][i]), quint8(planes[1][i]), quint8(planes[2][i]), quint8(planes[3][i])); } }
            else L.native = QImage();
        }
    }
    out = lays;
    return true;
}

// ---------------------------------------------------------------- colours and gradients in descriptors
// A 32-bit document stores its colours in linear light, like its pixels.
bool g_linearDoc = false;
const ColorInfo* g_docColor = nullptr;      // the document being read, for its profile and mode
QColor colorRaw(const Val* c);
QColor colorOf(const Val* c){
    QColor v = colorRaw(c);
    if(g_linearDoc) return QColor::fromRgbF(linToSrgb(v.redF()), linToSrgb(v.greenF()), linToSrgb(v.blueF()));
    // A colour in the document's own space means what the profile says it means.
    if(c && g_docColor && g_docColor->icc){
        const int m = g_docColor->mode;
        uchar in[4], out[3];
        auto b = [](double x){ return uchar(qBound(0, int(x * 255.0 + 0.5), 255)); };
        if(m == 3 && c->cls == "RGBC"){ in[0] = uchar(v.red()); in[1] = uchar(v.green()); in[2] = uchar(v.blue()); }
        else if(m == 1 && c->cls == "Grsc") in[0] = uchar(v.red());
        else if(m == 4 && c->cls == "CMYC"){ in[0] = b(c->n("Cyn") / 100.0); in[1] = b(c->n("Mgnt") / 100.0); in[2] = b(c->n("Ylw") / 100.0); in[3] = b(c->n("Blck") / 100.0); }
        else return v;
        g_docColor->icc->convert(in, out, 1);
        return QColor(out[0], out[1], out[2]);
    }
    return v;
}
QColor colorRaw(const Val* c){
    if(!c) return Qt::black;
    if(c->cls == "RGBC") return QColor::fromRgbF(qBound(0.0, c->n("Rd") / 255.0, 1.0), qBound(0.0, c->n("Grn") / 255.0, 1.0), qBound(0.0, c->n("Bl") / 255.0, 1.0));
    if(c->cls == "Grsc"){ const double v = qBound(0.0, 1.0 - c->n("Gry") / 100.0, 1.0); return QColor::fromRgbF(v, v, v); }
    if(c->cls == "CMYC"){
        const double k = 1.0 - c->n("Blck") / 100.0;
        return QColor::fromRgbF(qBound(0.0, (1.0 - c->n("Cyn") / 100.0) * k, 1.0), qBound(0.0, (1.0 - c->n("Mgnt") / 100.0) * k, 1.0), qBound(0.0, (1.0 - c->n("Ylw") / 100.0) * k, 1.0));
    }
    if(c->cls == "HSBC") return QColor::fromHsvF(qBound(0.0, c->n("H") / 360.0, 1.0), qBound(0.0, c->n("Strt") / 100.0, 1.0), qBound(0.0, c->n("Brgh") / 100.0, 1.0));
    if(c->cls == "LbCl") return QColor::fromRgb(labToRgb(qBound(0, int(c->n("Lmnc") * 2.55 + 0.5), 255), qBound(0, int(c->n("A") + 128.5), 255), qBound(0, int(c->n("B") + 128.5), 255)));
    if(c->get("redFloat")) return QColor::fromRgbF(qBound(0.0, c->n("redFloat"), 1.0), qBound(0.0, c->n("greenFloat"), 1.0), qBound(0.0, c->n("blueFloat"), 1.0));
    return Qt::black;
}

struct GradStop { double pos; QColor col; };
// Colour and opacity stops live in separate lists, each with its own midpoints; merge them onto one ramp.
QVector<GradStop> gradStops(const Val* grad, bool reverse){
    QVector<GradStop> out;
    if(!grad) return out;
    struct CS { double p; QColor c; double mid; };
    QVector<CS> cs; QVector<QPair<double,double>> ts;
    if(const Val* l = grad->get("Clrs")) for(const Val& e : l->list){
        cs.push_back({ qBound(0.0, e.n("Lctn") / 4096.0, 1.0), colorOf(e.get("Clr")), qBound(0.05, e.n("Mdpn", 50.0) / 100.0, 0.95) });
    }
    if(const Val* l = grad->get("Trns")) for(const Val& e : l->list) ts.push_back({ qBound(0.0, e.n("Lctn") / 4096.0, 1.0), qBound(0.0, e.n("Opct", 100.0) / 100.0, 1.0) });
    if(cs.isEmpty()) return out;
    std::stable_sort(cs.begin(), cs.end(), [](const CS& a, const CS& b){ return a.p < b.p; });
    std::stable_sort(ts.begin(), ts.end(), [](const QPair<double,double>& a, const QPair<double,double>& b){ return a.first < b.first; });
    auto colAt = [&](double t)->QColor {
        if(t <= cs.first().p) return cs.first().c;
        for(int i = 1; i < cs.size(); ++i) if(t <= cs[i].p){
            const double span = cs[i].p - cs[i-1].p;
            double u = span > 1e-9 ? (t - cs[i-1].p) / span : 1.0;
            const double m = cs[i-1].mid;                    // the midpoint bends the ramp towards one end
            u = u < m ? 0.5 * u / m : 0.5 + 0.5 * (u - m) / (1.0 - m);
            const QColor a = cs[i-1].c, b = cs[i].c;
            return QColor::fromRgbF(a.redF() + (b.redF()-a.redF())*u, a.greenF() + (b.greenF()-a.greenF())*u, a.blueF() + (b.blueF()-a.blueF())*u);
        }
        return cs.last().c;
    };
    auto alpAt = [&](double t)->double {
        if(ts.isEmpty()) return 1.0;
        if(t <= ts.first().first) return ts.first().second;
        for(int i = 1; i < ts.size(); ++i) if(t <= ts[i].first){
            const double span = ts[i].first - ts[i-1].first;
            return ts[i-1].second + (ts[i].second - ts[i-1].second) * (span > 1e-9 ? (t - ts[i-1].first) / span : 1.0);
        }
        return ts.last().second;
    };
    const int N = 48;
    for(int i = 0; i <= N; ++i){
        const double t = double(i) / N;
        QColor c = colAt(reverse ? 1.0 - t : t);
        c.setAlphaF(alpAt(reverse ? 1.0 - t : t));
        out.push_back({ t, c });
    }
    return out;
}

QString hx(const QColor& c){ return c.name(QColor::HexRgb); }
QString fn(double v){ return QString::number(std::isfinite(v) ? v : 0.0, 'f', 4); }
QString xmlEsc(QString s){
    s.replace(QLatin1Char('&'), QLatin1String("&amp;")); s.replace(QLatin1Char('<'), QLatin1String("&lt;"));
    s.replace(QLatin1Char('>'), QLatin1String("&gt;")); s.replace(QLatin1Char('"'), QLatin1String("&quot;"));
    return s;
}
QString pathD(const QPainterPath& p){
    QString d;
    for(int i = 0; i < p.elementCount(); ++i){
        const QPainterPath::Element e = p.elementAt(i);
        if(e.type == QPainterPath::MoveToElement) d += QLatin1Char('M') + fn(e.x) + QLatin1Char(' ') + fn(e.y);
        else if(e.type == QPainterPath::LineToElement) d += QLatin1Char('L') + fn(e.x) + QLatin1Char(' ') + fn(e.y);
        else if(e.type == QPainterPath::CurveToElement && i + 2 < p.elementCount()){
            const auto c = p.elementAt(i+1), f = p.elementAt(i+2);
            d += QLatin1Char('C') + fn(e.x) + QLatin1Char(' ') + fn(e.y) + QLatin1Char(' ') + fn(c.x) + QLatin1Char(' ') + fn(c.y) + QLatin1Char(' ') + fn(f.x) + QLatin1Char(' ') + fn(f.y);
            i += 2;
        }
    }
    return d;
}

// ---------------------------------------------------------------- vector masks
// 26-byte records: positions are 8.24 fixed fractions of the document, y first. Each subpath's length record
// holds its operation (0 exclude, 1 combine, 2 subtract, 3 intersect, -1 continues the one before) and its
// fill rule (2 non-zero, else even-odd). A path that opens by subtracting starts from the whole canvas.
QPainterPath readVectorPath(const Rd& r, const Span& s, int W, int H, bool* disabled, bool* inverted, int* firstOp = nullptr){
    if(s.len < 8) return {};
    const quint32 flags = quint32(r.u32(s.o + 4));
    if(disabled) *disabled = flags & 4;
    if(inverted) *inverted = flags & 1;
    struct Comp { int op; QPainterPath path; };
    QVector<Comp> comps;
    QVector<QPointF> ctrl; bool closed = false; int pending = 0, op = 1; Qt::FillRule rule = Qt::OddEvenFill;
    auto pt = [&](qint64 o){ return QPointF(r.i32(o + 4) / 16777216.0 * W, r.i32(o) / 16777216.0 * H); };
    auto flushSub = [&]{
        if(ctrl.size() < 3){ ctrl.clear(); return; }
        const int n = ctrl.size() / 3;
        QPainterPath sub;
        sub.moveTo(ctrl[1]);
        for(int i = 1; i < n; ++i) sub.cubicTo(ctrl[(i-1)*3+2], ctrl[i*3], ctrl[i*3+1]);
        if(closed){ sub.cubicTo(ctrl[(n-1)*3+2], ctrl[0], ctrl[1]); sub.closeSubpath(); }
        if(op < 0 && !comps.isEmpty()) comps.last().path.addPath(sub);
        else { sub.setFillRule(rule); comps.push_back({ op < 0 ? 1 : op, sub }); }
        ctrl.clear();
    };
    const qint64 end = s.o + s.len;
    for(qint64 o = s.o + 8; o + 26 <= end; o += 26){
        const int sel = r.u16(o);
        if(sel == 0 || sel == 3){
            flushSub();
            closed = sel == 0;
            pending = r.u16(o + 2);
            op = r.i16(o + 4);
            rule = r.u16(o + 6) == 2 ? Qt::WindingFill : Qt::OddEvenFill;
        } else if(sel == 1 || sel == 2 || sel == 4 || sel == 5){
            if(pending > 0 && ctrl.size() < 3000000){ ctrl << pt(o + 2) << pt(o + 10) << pt(o + 18); --pending; }
        }
    }
    flushSub();
    if(comps.isEmpty()) return {};
    if(firstOp) *firstOp = comps.first().op;
    if(comps.size() == 1 && comps.first().op != 2) return comps.first().path;
    QPainterPath result;
    if(comps.first().op == 2){ result.addRect(0, 0, W, H); result = result.subtracted(comps.first().path); }
    else result = comps.first().path;
    for(int i = 1; i < comps.size() && i < 4096; ++i){
        const QPainterPath& c = comps[i].path;
        if(comps[i].op == 2) result = result.subtracted(c);
        else if(comps[i].op == 3) result = result.intersected(c);
        else if(comps[i].op == 0){ const QPainterPath both = result.intersected(c); result = result.united(c).subtracted(both); }
        else result = result.united(c);
    }
    return result;
}

// ---------------------------------------------------------------- bitmap helpers
QPainter::CompositionMode compOf(const QByteArray& b){
    if(b == "mul ") return QPainter::CompositionMode_Multiply;
    if(b == "scrn") return QPainter::CompositionMode_Screen;
    if(b == "over") return QPainter::CompositionMode_Overlay;
    if(b == "sLit") return QPainter::CompositionMode_SoftLight;
    if(b == "hLit") return QPainter::CompositionMode_HardLight;
    if(b == "dark") return QPainter::CompositionMode_Darken;
    if(b == "lite") return QPainter::CompositionMode_Lighten;
    if(b == "diff") return QPainter::CompositionMode_Difference;
    if(b == "smud") return QPainter::CompositionMode_Exclusion;
    if(b == "idiv") return QPainter::CompositionMode_ColorBurn;
    if(b == "div ") return QPainter::CompositionMode_ColorDodge;
    if(b == "lddg") return QPainter::CompositionMode_Plus;
    return QPainter::CompositionMode_SourceOver;
}
int transpTypeOf(const QByteArray& b){
    if(b == "hue ") return 31;
    if(b == "sat ") return 16;
    if(b == "lum ") return 28;
    if(b == "colr") return 40;
    if(b == "lbrn") return 45;
    if(b == "vLit") return 46;
    if(b == "lLit") return 47;
    if(b == "pLit") return 48;
    if(b == "hMix") return 49;
    if(b == "fsub") return 50;
    if(b == "fdiv") return 51;
    if(b == "dkCl") return 52;
    if(b == "lgCl") return 53;
    return 0;
}
void applyBlend(Shape& s, const QByteArray& b){
    s.blend = compOf(b);
    const int t = transpTypeOf(b);
    if(t > 0){ s.transp.type = t; s.transp.geom = 0; s.transp.t0 = s.transp.t1 = 0.0; }
}
QByteArray blendKeyOfEnum(const QString& e){
    static const QHash<QString, QByteArray> m = {
        {"Mltp","mul "},{"Scrn","scrn"},{"Ovrl","over"},{"SftL","sLit"},{"HrdL","hLit"},{"Drkn","dark"},{"Lghn","lite"},
        {"Dfrn","diff"},{"Xclu","smud"},{"CBrn","idiv"},{"CDdg","div "},{"linearDodge","lddg"},{"linearBurn","lbrn"},
        {"H   ","hue "},{"Hue","hue "},{"Strt","sat "},{"Clr ","colr"},{"Clr","colr"},{"Lmns","lum "},{"vividLight","vLit"},
        {"linearLight","lLit"},{"pinLight","pLit"},{"hardMix","hMix"},{"Sbtr","fsub"},{"divide","fdiv"},{"darkerColor","dkCl"},{"lighterColor","lgCl"} };
    return m.value(e.trimmed(), QByteArrayLiteral("norm"));
}

Shape bitmapShape(const QImage& img, int x, int y, const QString& name){
    Shape s; s.type = ShapeType::Rect; s.name = name; s.rect = QRectF(0, 0, img.width(), img.height());
    s.fill.type = FillType::Bitmap; s.fill.image = img; s.fill.origImage = img; s.fill.tiled = false;
    s.fill.p0 = QPointF(0, img.height()); s.fill.p1 = QPointF(img.width(), img.height()); s.fill.p2 = QPointF(0, 0);
    s.stroke.enabled = false;
    s.transform = QTransform::fromTranslate(x, y);
    return s;
}
bool isPixelShape(const Shape& s){ return s.type == ShapeType::Rect && s.fill.type == FillType::Bitmap && !s.fill.image.isNull(); }

// An object's coverage on a doc-space rectangle, for clipping and effects.
QImage coverageOf(const Shape& s, const QRect& area){
    QImage img(area.size(), QImage::Format_ARGB32_Premultiplied);
    if(img.isNull()) return img;
    img.fill(Qt::transparent);
    { QPainter g(&img); g.setRenderHint(QPainter::Antialiasing); g.translate(-area.topLeft()); paintShape(g, s); }
    return img;
}
void multiplyAlpha(QImage& img, int ox, int oy, const QImage& cov, const QRect& area){
    for(int y = 0; y < img.height(); ++y){
        QRgb* row = reinterpret_cast<QRgb*>(img.scanLine(y));
        const int cy = y + oy - area.top();
        const QRgb* c = (cy >= 0 && cy < area.height()) ? reinterpret_cast<const QRgb*>(cov.constScanLine(cy)) : nullptr;
        for(int x = 0; x < img.width(); ++x){
            const int cx = x + ox - area.left();
            const int a = (c && cx >= 0 && cx < area.width()) ? qAlpha(c[cx]) : 0;
            row[x] = (row[x] & 0xffffffu) | (uint(qAlpha(row[x]) * a / 255) << 24);
        }
    }
}
// A mask image in document space on any object: the renderer's bitmap transparency, luminance = opacity.
void setDocMask(Shape& s, const QImage& gray, const QRect& area){
    bool inv = false;
    const QTransform t = s.transform.inverted(&inv);
    if(!inv) return;
    s.transp.type = s.transp.type ? s.transp.type : 1; s.transp.geom = 4; s.transp.t0 = 1.0; s.transp.t1 = 0.0;
    s.transp.maskBmp = gray;
    s.transp.p0 = t.map(QPointF(area.left(), area.bottom() + 1));
    s.transp.p1 = t.map(QPointF(area.right() + 1, area.bottom() + 1));
    s.transp.p2 = t.map(QPointF(area.left(), area.top()));
}

// ---------------------------------------------------------------- adjustments
using PixFn = std::function<QRgb(QRgb)>;

QVector<int> curveLut(QVector<QPointF> pts){
    QVector<int> lut(256);
    std::sort(pts.begin(), pts.end(), [](const QPointF& a, const QPointF& b){ return a.x() < b.x(); });
    if(pts.size() < 2){ for(int i = 0; i < 256; ++i) lut[i] = i; return lut; }
    // Natural cubic spline, as Photoshop draws its curve.
    const int n = pts.size();
    QVector<double> a(n), b(n), c(n), r(n), y2(n, 0.0);
    for(int i = 1; i < n - 1; ++i){
        const double h0 = pts[i].x() - pts[i-1].x(), h1 = pts[i+1].x() - pts[i].x();
        a[i] = h0; b[i] = 2 * (h0 + h1); c[i] = h1;
        r[i] = 6 * ((pts[i+1].y() - pts[i].y()) / qMax(1e-9, h1) - (pts[i].y() - pts[i-1].y()) / qMax(1e-9, h0));
    }
    for(int i = 2; i < n - 1; ++i){ const double m = a[i] / qMax(1e-9, b[i-1]); b[i] -= m * c[i-1]; r[i] -= m * r[i-1]; }
    for(int i = n - 2; i >= 1; --i) y2[i] = (r[i] - c[i] * y2[i+1]) / qMax(1e-9, b[i]);
    for(int i = 0; i < 256; ++i){
        const double x = i;
        if(x <= pts.first().x()){ lut[i] = qBound(0, int(pts.first().y() + 0.5), 255); continue; }
        if(x >= pts.last().x()){ lut[i] = qBound(0, int(pts.last().y() + 0.5), 255); continue; }
        int k = 1; while(k < n - 1 && pts[k].x() < x) ++k;
        const double h = qMax(1e-9, pts[k].x() - pts[k-1].x()), A = (pts[k].x() - x) / h, B = (x - pts[k-1].x()) / h;
        const double v = A * pts[k-1].y() + B * pts[k].y() + ((A*A*A - A) * y2[k-1] + (B*B*B - B) * y2[k]) * h * h / 6.0;
        lut[i] = qBound(0, int(v + 0.5), 255);
    }
    return lut;
}
// Channel curves run first and the composite after them, which Photoshop's own results pin down.
PixFn lutFn(QVector<int> m, QVector<int> rr, QVector<int> gg, QVector<int> bb){
    return [m, rr, gg, bb](QRgb p){ return qRgba(m[rr[qRed(p)]], m[gg[qGreen(p)]], m[bb[qBlue(p)]], qAlpha(p)); };
}
QVector<int> identityLut(){ QVector<int> l(256); for(int i = 0; i < 256; ++i) l[i] = i; return l; }

struct Adjustment {
    PixFn fn;
    std::function<void(int*)> cmyk;       // on the four stored channels; null means go through RGB
};
// Per-channel tables by document mode. Records are the composite, then the document's channels in order.
Adjustment channelAdjustment(const QVector<QVector<int>>& t, int mode){
    Adjustment A;
    const QVector<int> id = identityLut();
    auto at = [&](int i){ return i < t.size() ? t[i] : id; };
    const QVector<int> m = at(0);
    if(mode == 4){
        const QVector<int> c = at(1), g = at(2), y = at(3), k = at(4);
        A.cmyk = [m, c, g, y, k](int* v){ v[0] = m[c[v[0]]]; v[1] = m[g[v[1]]]; v[2] = m[y[v[2]]]; v[3] = m[k[v[3]]]; };
        A.fn = lutFn(m, c, g, y);
    } else if(mode == 1 || mode == 8){
        const QVector<int> g = at(1);
        A.fn = lutFn(m, g, g, g);
    } else A.fn = lutFn(m, at(1), at(2), at(3));
    return A;
}

// Photoshop's saturation move: towards or away from the pixel's own lightness.
void saturate(double& r, double& g, double& b, double inc){
    const double mx = qMax(r, qMax(g, b)), mn = qMin(r, qMin(g, b)), delta = mx - mn;
    if(delta <= 0.0 || inc == 0.0) return;
    const double L = (mx + mn) / 2.0, S = L < 0.5 ? delta / (mx + mn) : delta / qMax(1e-9, 2.0 - mx - mn);
    double alpha;
    if(inc >= 0.0){ alpha = inc + S >= 1.0 ? S : 1.0 - inc; alpha = 1.0 / qMax(1e-9, alpha) - 1.0; r += (r - L) * alpha; g += (g - L) * alpha; b += (b - L) * alpha; }
    else { alpha = 1.0 + inc; r = L + (r - L) * alpha; g = L + (g - L) * alpha; b = L + (b - L) * alpha; }
}
QRgb packRgb(double r, double g, double b, int a){
    auto c = [](double v){ return qBound(0, int(v * 255.0 + 0.5), 255); };
    return qRgba(c(r), c(g), c(b), a);
}

// .cube text: one row per entry, red changing fastest. Empty if it isn't one.
PixFn cubeLut(const QByteArray& text){
    int size = 0; double lo[3] = { 0, 0, 0 }, hi[3] = { 1, 1, 1 };
    auto table = std::make_shared<QVector<float>>();
    qint64 pos = 0;
    while(pos < text.size()){
        qint64 e = text.indexOf('\n', pos);
        if(e < 0) e = text.size();
        const QByteArray line = text.mid(pos, e - pos).trimmed();
        pos = e + 1;
        if(line.isEmpty() || line.startsWith('#') || line.startsWith("TITLE")) continue;
        const QList<QByteArray> f = line.split(' ');
        if(line.startsWith("LUT_3D_SIZE") && f.size() >= 2){
            size = f.last().toInt();
            if(size < 2 || size > 129) return {};
            table->reserve(qsizetype(size) * size * size * 3);
            continue;
        }
        if(line.startsWith("LUT_1D_SIZE")) return {};
        if(line.startsWith("DOMAIN_MIN") && f.size() >= 4){ for(int i = 0; i < 3; ++i) lo[i] = f[i+1].toDouble(); continue; }
        if(line.startsWith("DOMAIN_MAX") && f.size() >= 4){ for(int i = 0; i < 3; ++i) hi[i] = f[i+1].toDouble(); continue; }
        if(size > 0 && f.size() >= 3 && table->size() < qsizetype(size) * size * size * 3){
            bool ok1, ok2, ok3;
            const float a = f[0].toFloat(&ok1), b = f[1].toFloat(&ok2), c = f[2].toFloat(&ok3);
            if(ok1 && ok2 && ok3 && std::isfinite(a) && std::isfinite(b) && std::isfinite(c)) table->append({ a, b, c });
        }
    }
    if(size < 2 || table->size() != qsizetype(size) * size * size * 3) return {};
    const double l0 = lo[0], l1 = lo[1], l2 = lo[2], s0 = qMax(1e-9, hi[0] - lo[0]), s1 = qMax(1e-9, hi[1] - lo[1]), s2 = qMax(1e-9, hi[2] - lo[2]);
    return [table, size, l0, l1, l2, s0, s1, s2](QRgb p){
        const double u[3] = { qBound(0.0, (qRed(p) / 255.0 - l0) / s0, 1.0) * (size - 1), qBound(0.0, (qGreen(p) / 255.0 - l1) / s1, 1.0) * (size - 1), qBound(0.0, (qBlue(p) / 255.0 - l2) / s2, 1.0) * (size - 1) };
        const int i0 = qMin(int(u[0]), size - 2), i1 = qMin(int(u[1]), size - 2), i2 = qMin(int(u[2]), size - 2);
        const double f0 = u[0] - i0, f1 = u[1] - i1, f2 = u[2] - i2;
        double out[3] = { 0, 0, 0 };
        const float* T = table->constData();
        for(int c = 0; c < 8; ++c){
            const double w = (c & 1 ? f0 : 1 - f0) * (c & 2 ? f1 : 1 - f1) * (c & 4 ? f2 : 1 - f2);
            const qsizetype at = 3 * (qsizetype(i0 + (c & 1)) + qsizetype(i1 + ((c >> 1) & 1)) * size + qsizetype(i2 + ((c >> 2) & 1)) * size * size);
            for(int k = 0; k < 3; ++k) out[k] += w * T[at + k];
        }
        return packRgb(out[0], out[1], out[2], qAlpha(p));
    };
}

// The layer's adjustment as a per-pixel function. False when it isn't an adjustment layer.
bool adjustmentOf(const Rd& r, const Lay& L, int mode, Adjustment& out){
    auto block = [&](const char* k, Span& s){ s = L.blocks.value(k); return L.blocks.contains(k); };
    Span s;
    if(block("nvrt", s)){
        out.fn = [](QRgb p){ return qRgba(255-qRed(p), 255-qGreen(p), 255-qBlue(p), qAlpha(p)); };
        if(mode == 4) out.cmyk = [](int* v){ for(int i = 0; i < 4; ++i) v[i] = 255 - v[i]; };
        return true;
    }
    if(block("CgEd", s) || block("brit", s)){
        double br, ct; bool legacy = false;
        if(L.blocks.contains("CgEd")){
            DescReader dr(r.d, s.o + 4, s.o + s.len);
            const Val v = dr.desc(0);
            br = v.n("Brgh"); ct = v.n("Cntr"); legacy = v.n("useLegacy") != 0;
        } else { br = r.i16(s.o); ct = r.i16(s.o + 2); legacy = true; }
        QVector<int> l(256);
        if(legacy){
            const double c = ct > 0 ? 255.0 / qMax(1.0, 255.0 - 2.0 * ct * 127.0 / 100.0) : 1.0 + ct / 100.0;
            for(int i = 0; i < 256; ++i) l[i] = qBound(0, int((i + br - 127.5) * c + 127.5 + 0.5), 255);
        } else {
            // A gamma followed by an S-curve. Both parameters are fitted to Photoshop's results over the
            // brightness and contrast settings in the test files; no formula for this mode is published.
            const double gamma = std::pow(2.0, -0.01281 * br + 3.12e-5 * br * br);
            const double a = std::exp(0.0048 * ct + 0.00711 * br);
            for(int i = 0; i < 256; ++i){
                const double t = std::pow(i / 255.0, gamma), A = std::pow(t, a), B = std::pow(1.0 - t, a);
                l[i] = qBound(0, int(A / qMax(1e-9, A + B) * 255.0 + 0.5), 255);
            }
        }
        out.fn = lutFn(identityLut(), l, l, l);
        return true;
    }
    if(block("levl", s)){
        QVector<QVector<int>> t;
        for(int rec = 0; rec < 5; ++rec){
            const qint64 o = s.o + 2 + rec * 10;
            if(o + 10 > s.o + s.len){ t.push_back(identityLut()); continue; }
            const double in0 = r.u16(o), in1 = qMax(in0 + 1.0, double(r.u16(o+2))), o0 = r.u16(o+4), o1 = r.u16(o+6), g = qMax(0.01, r.u16(o+8) / 100.0);
            QVector<int> l(256);
            for(int v = 0; v < 256; ++v){
                const double x = qBound(0.0, (v - in0) / (in1 - in0), 1.0);
                l[v] = qBound(0, int(o0 + (o1 - o0) * std::pow(x, 1.0 / g) + 0.5), 255);
            }
            t.push_back(l);
        }
        out = channelAdjustment(t, mode);
        return true;
    }
    if(block("curv", s)){
        const bool isMap = r.u8(s.o) != 0;
        qint64 o = s.o + 1;
        o += 2;
        const quint32 mask = quint32(r.u32(o)); o += 4;
        QVector<QVector<int>> t(5, identityLut());
        for(int ch = 0; ch < 32 && o < s.o + s.len; ++ch){
            if(!(mask & (1u << ch))) continue;
            if(isMap){
                if(ch < 5) for(int i = 0; i < 256; ++i) t[ch][i] = r.u8(o + i);
                o += 256;
                continue;
            }
            const int np = r.u16(o); o += 2;
            QVector<QPointF> pts;
            for(int i = 0; i < np && i < 64; ++i){ pts.push_back(QPointF(r.u16(o+2), r.u16(o))); o += 4; }
            if(ch < 5) t[ch] = curveLut(pts);
        }
        out = channelAdjustment(t, mode);
        return true;
    }
    if(block("hue2", s) || block("hueb", s)){
        const bool colorize = r.u8(s.o + 2) != 0;
        const int mh = r.i16(s.o + 10), ms = r.i16(s.o + 12), ml = r.i16(s.o + 14);
        const int ch = r.i16(s.o + 4), cs = r.i16(s.o + 6), cl = r.i16(s.o + 8);
        struct Range { int a, b, c, d, h, s, l; };
        QVector<Range> rg;
        for(int i = 0; i < 6; ++i){ const qint64 o = s.o + 16 + i * 14; rg.push_back({ r.i16(o), r.i16(o+2), r.i16(o+4), r.i16(o+6), r.i16(o+8), r.i16(o+10), r.i16(o+12) }); }
        // Everything here was measured against Photoshop's results. The master's lightness moves each channel
        // towards white or black. A range's lightness instead moves it to the pixel's own maximum or minimum,
        // so at +100 or -100 the colour goes grey. Where ranges overlap, the strongest setting wins.
        out.fn = [colorize, mh, ms, ml, ch, cs, cl, rg](QRgb p){
            float h, sa, li, al;
            QColor(p).getHslF(&h, &sa, &li, &al);
            if(colorize){
                const double d = cl / 100.0;
                const double L = d >= 0 ? li + (1.0 - li) * d : li * (1.0 + d);
                const QColor c = QColor::fromHslF((ch < 0 ? ch + 360 : ch) / 360.f, qBound(0.f, cs / 100.f, 1.f), float(qBound(0.0, L, 1.0)));
                return qRgba(c.red(), c.green(), c.blue(), qAlpha(p));
            }
            double R = qRed(p) / 255.0, G = qGreen(p) / 255.0, B = qBlue(p) / 255.0;
            double dh = 0, ds = 0, dl = 0;
            if(h >= 0.f && sa > 0.f){
                const double deg = h * 360.0;
                auto wrap = [](double v){ v = std::fmod(v, 360.0); return v < 0 ? v + 360.0 : v; };
                for(const Range& q : rg){
                    const double rel = wrap(deg - q.a), b1 = wrap(q.b - q.a), c1 = wrap(q.c - q.a), d1 = wrap(q.d - q.a);
                    double w = 0.0;
                    if(rel < b1) w = rel / b1;
                    else if(rel <= c1) w = 1.0;
                    else if(rel < d1) w = (d1 - rel) / qMax(1e-9, d1 - c1);
                    if(qAbs(w * q.h) > qAbs(dh)) dh = w * q.h;
                    if(qAbs(w * q.s) > qAbs(ds)) ds = w * q.s;
                    if(qAbs(w * q.l) > qAbs(dl)) dl = w * q.l;
                }
            }
            if(h >= 0.f && mh + dh != 0.0){
                double nh = std::fmod(h + (mh + dh) / 360.0, 1.0); if(nh < 0) nh += 1.0;
                const QColor c = QColor::fromHslF(float(nh), sa, li);
                R = c.redF(); G = c.greenF(); B = c.blueF();
            }
            if(dl != 0.0){
                const double top = qMax(R, qMax(G, B)), low = qMin(R, qMin(G, B)), d = qBound(-1.0, dl / 100.0, 1.0);
                auto to = [&](double v){ return d >= 0 ? v + (top - v) * d : v + (v - low) * d; };
                R = to(R); G = to(G); B = to(B);
            }
            const double m = qBound(-1.0, ml / 100.0, 1.0);
            auto light = [m](double v){ return m >= 0 ? v + (1.0 - v) * m : v * (1.0 + m); };
            R = light(R); G = light(G); B = light(B);
            saturate(R, G, B, qBound(-1.0, (ms + ds) / 100.0, 1.0));
            return packRgb(R, G, B, qAlpha(p));
        };
        return true;
    }
    if(block("blnc", s)){
        double a[3][3];
        for(int i = 0; i < 9; ++i) a[i/3][i%3] = r.i16(s.o + i * 2) / 100.0;
        const bool keepLum = r.u8(s.o + 18) != 0;
        QVector<double> sh(3), mi(3), hi(3);
        for(int i = 0; i < 3; ++i){ sh[i] = a[0][i]; mi[i] = a[1][i]; hi[i] = a[2][i]; }
        out.fn = [sh, mi, hi, keepLum](QRgb p){
            const int c[3] = { qRed(p), qGreen(p), qBlue(p) };
            const double L0 = 0.3*c[0] + 0.59*c[1] + 0.11*c[2];
            double o[3];
            for(int i = 0; i < 3; ++i){
                const double v = c[i] / 255.0;
                const double ws = qBound(0.0, (0.5 - v) * 2.0, 1.0), wh = qBound(0.0, (v - 0.5) * 2.0, 1.0), wm = 1.0 - ws - wh;
                o[i] = c[i] + 255.0 * 0.25 * (sh[i] * ws + mi[i] * wm + hi[i] * wh);
            }
            if(keepLum){ const double d = L0 - (0.3*o[0] + 0.59*o[1] + 0.11*o[2]); for(double& v : o) v += d; }
            return qRgba(qBound(0, int(o[0]+0.5), 255), qBound(0, int(o[1]+0.5), 255), qBound(0, int(o[2]+0.5), 255), qAlpha(p));
        };
        return true;
    }
    if(block("post", s)){
        const int n = qMax(2, r.u16(s.o));
        QVector<int> l(256);
        for(int i = 0; i < 256; ++i) l[i] = qBound(0, int(std::floor(qMin(i * n / 255.0, n - 1.0)) * 255.0 / (n - 1) + 0.5), 255);
        out.fn = lutFn(identityLut(), l, l, l);
        if(mode == 4) out.cmyk = [l](int* v){ for(int i = 0; i < 4; ++i) v[i] = l[v[i]]; };
        return true;
    }
    if(block("thrs", s)){
        const int t = r.u16(s.o);
        out.fn = [t](QRgb p){ const int v = (qRed(p)*299 + qGreen(p)*587 + qBlue(p)*114) / 1000 >= t ? 255 : 0; return qRgba(v, v, v, qAlpha(p)); };
        return true;
    }
    if(block("vibA", s)){
        DescReader dr(r.d, s.o + 4, s.o + s.len);
        const Val v = dr.desc(0);
        const double vib = v.n("vibrance") / 100.0, st = v.n("Strt") / 100.0;
        out.fn = [vib, st](QRgb p){
            QColor c(p);
            float h, ss, l, a; c.getHslF(&h, &ss, &l, &a);
            ss = float(qBound(0.0, ss + vib * (1.0 - ss) * (1.0 - ss) * 0.8 + st * (st > 0 ? 1.0 - ss : ss), 1.0));
            QColor o = QColor::fromHslF(h < 0 ? 0.f : h, ss, l);
            return qRgba(o.red(), o.green(), o.blue(), qAlpha(p));
        };
        return true;
    }
    if(block("expA", s)){
        const double ex = r.f32(s.o + 2), of = r.f32(s.o + 6), g = qMax(0.01f, r.f32(s.o + 10));
        QVector<int> l(256);
        for(int i = 0; i < 256; ++i){
            const double v = i / 255.0, lin = v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
            l[i] = qBound(0, int(linToSrgb(std::pow(qMax(0.0, lin * std::pow(2.0, ex) + of), 1.0 / g)) * 255.0 + 0.5), 255);
        }
        out.fn = lutFn(identityLut(), l, l, l);
        return true;
    }
    if(block("mixr", s)){
        const bool mono = r.u16(s.o + 2) != 0;
        double k[4][5];
        for(int i = 0; i < 4; ++i) for(int j = 0; j < 5; ++j) k[i][j] = r.i16(s.o + 4 + (i * 5 + j) * 2) / 100.0;
        QVector<double> K;
        for(int i = 0; i < 3; ++i) for(int j = 0; j < 5; ++j) K.push_back(k[mono ? 0 : i][j]);
        out.fn = [K](QRgb p){
            const double c[3] = { qRed(p) / 255.0, qGreen(p) / 255.0, qBlue(p) / 255.0 };
            double o[3];
            for(int i = 0; i < 3; ++i) o[i] = K[i*5] * c[0] + K[i*5+1] * c[1] + K[i*5+2] * c[2] + K[i*5+4];
            return packRgb(o[0], o[1], o[2], qAlpha(p));
        };
        return true;
    }
    if(block("selc", s)){
        const bool absolute = r.u16(s.o + 2) == 1;
        // Reds, yellows, greens, cyans, blues, magentas, whites, neutrals, blacks; record 0 is unused.
        QVector<double> T;
        for(int i = 1; i < 10; ++i) for(int j = 0; j < 4; ++j) T.push_back(r.i16(s.o + 4 + (i * 4 + j) * 2) / 100.0);
        out.fn = [T, absolute](QRgb p){
            const double R = qRed(p) / 255.0, G = qGreen(p) / 255.0, B = qBlue(p) / 255.0;
            const double mx = qMax(R, qMax(G, B)), mn = qMin(R, qMin(G, B)), mid = R + G + B - mx - mn;
            double w[9] = { 0 };
            if(mx > mn){
                const double prim = mx - mid, sec = mid - mn;
                const int top = R >= G && R >= B ? 0 : G >= B ? 2 : 4;            // reds, greens, blues
                w[top] = prim;
                const int low = R <= G && R <= B ? 3 : G <= B ? 5 : 1;            // cyans, magentas, yellows
                w[low] = sec;
            }
            w[6] = mn > 0.5 ? (mn - 0.5) * 2.0 : 0.0;
            w[8] = mx < 0.5 ? (0.5 - mx) * 2.0 : 0.0;
            w[7] = qMax(0.0, 1.0 - qAbs(mx - 0.5) - qAbs(mn - 0.5));
            double ink[3] = { 1.0 - R, 1.0 - G, 1.0 - B };
            double d[3] = { 0, 0, 0 };
            for(int i = 0; i < 9; ++i){
                if(w[i] <= 0.0) continue;
                for(int c = 0; c < 3; ++c){
                    const double a = T[i*4 + c], kAdj = T[i*4 + 3];
                    d[c] += w[i] * (absolute ? a : a * ink[c]) + w[i] * kAdj * (absolute ? 1.0 : ink[c]);
                }
            }
            return packRgb(1.0 - (ink[0] + d[0]), 1.0 - (ink[1] + d[1]), 1.0 - (ink[2] + d[2]), qAlpha(p));
        };
        return true;
    }
    if(block("phfl", s)){
        const int ver = r.u16(s.o);
        QColor f(Qt::white);
        qint64 o = s.o + 2;
        if(ver == 3){
            // XYZ as fixed point, scale 2^16 / 2 with D50 white near 0.96, 1, 0.82.
            const double X = r.u32(o) / 8192.0 / 8.0 / 100.0 * 100.0, Y = r.u32(o + 4) / 8192.0 / 8.0, Z = r.u32(o + 8) / 8192.0 / 8.0;
            auto e = [](double v){ return linToSrgb(v); };
            f = QColor::fromRgbF(e(3.1338561*X - 1.6168667*Y - 0.4906146*Z), e(-0.9787684*X + 1.9161415*Y + 0.0334540*Z), e(0.0719453*X - 0.2289914*Y + 1.4052427*Z));
            o += 12;
        } else {
            const int cs = r.u16(o); o += 2;
            if(cs == 7) f = QColor::fromRgb(labToRgb(qBound(0, int(r.u16(o) / 10000.0 * 255.0 + 0.5), 255), qBound(0, int(r.i16(o+2) / 100.0 + 128.5), 255), qBound(0, int(r.i16(o+4) / 100.0 + 128.5), 255)));
            else if(cs == 0) f = QColor::fromRgbF(r.u16(o) / 65535.f, r.u16(o+2) / 65535.f, r.u16(o+4) / 65535.f);
            o += 8;
        }
        const double density = qBound(0.0, r.u32(o) / 100.0, 1.0);
        const bool keep = r.u8(o + 4) != 0;
        auto toLin = [](double v){ return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
        const double fr = toLin(f.redF()), fg = toLin(f.greenF()), fb = toLin(f.blueF());
        // A coloured filter multiplies in linear light; "preserve luminosity" gives the lost light back evenly.
        out.fn = [fr, fg, fb, density, keep, toLin](QRgb p){
            const double R = toLin(qRed(p) / 255.0), G = toLin(qGreen(p) / 255.0), B = toLin(qBlue(p) / 255.0);
            double r2 = R * (1.0 + (fr - 1.0) * density), g2 = G * (1.0 + (fg - 1.0) * density), b2 = B * (1.0 + (fb - 1.0) * density);
            if(keep){
                const double d = 0.2126*R + 0.7152*G + 0.0722*B - (0.2126*r2 + 0.7152*g2 + 0.0722*b2);
                r2 += d; g2 += d; b2 += d;
            }
            return packRgb(linToSrgb(r2), linToSrgb(g2), linToSrgb(b2), qAlpha(p));
        };
        return true;
    }
    if(block("blwh", s)){
        DescReader dr(r.d, s.o + 4, s.o + s.len);
        const Val v = dr.desc(0);
        const double wr = v.n("Rd", 40) / 100.0, wy = v.n("Yllw", 60) / 100.0, wg = v.n("Grn", 40) / 100.0, wc = v.n("Cyn", 60) / 100.0, wb = v.n("Bl", 20) / 100.0, wm = v.n("Mgnt", 80) / 100.0;
        const bool tint = v.n("useTint") != 0;
        const QColor tc = colorOf(v.get("tintColor"));
        float th = 0.f, ts = 0.f, tl, ta;
        tc.getHslF(&th, &ts, &tl, &ta);
        out.fn = [wr, wy, wg, wc, wb, wm, tint, th, ts](QRgb p){
            const double R = qRed(p) / 255.0, G = qGreen(p) / 255.0, B = qBlue(p) / 255.0;
            const double mx = qMax(R, qMax(G, B)), mn = qMin(R, qMin(G, B)), mid = R + G + B - mx - mn;
            // The share of the top channel alone, of the top two together, and of grey.
            const double prim = R >= G && R >= B ? wr : G >= B ? wg : wb;
            const double sec = R <= G && R <= B ? wc : G <= B ? wm : wy;
            const double y = qBound(0.0, (mx - mid) * prim + (mid - mn) * sec + mn, 1.0);
            if(!tint) return packRgb(y, y, y, qAlpha(p));
            const QColor o = QColor::fromHslF(th < 0 ? 0.f : th, ts, float(y));
            return qRgba(o.red(), o.green(), o.blue(), qAlpha(p));
        };
        return true;
    }
    if(block("grdm", s)){
        const int ver = r.u16(s.o);
        const bool reversed = r.u8(s.o + 2) != 0;
        qint64 o = s.o + 4;
        if(ver == 3) o += 4;
        const qint64 nameLen = r.u32(o); o += 4 + qMax<qint64>(0, qMin(nameLen, s.len)) * 2;
        const int count = r.u16(o); o += 2;
        struct Stop { double p, mid; QColor c; };
        QVector<Stop> st;
        for(int i = 0; i < count && o + 20 <= s.o + s.len; ++i, o += 20){
            const int cm = r.u16(o + 8);
            QColor c = cm == 0 ? QColor::fromRgbF(r.u16(o+10) / 65535.f, r.u16(o+12) / 65535.f, r.u16(o+14) / 65535.f) : QColor::fromRgbF(r.u16(o+10) / 65535.f, r.u16(o+10) / 65535.f, r.u16(o+10) / 65535.f);
            st.push_back({ qBound(0.0, r.u32(o) / 4096.0, 1.0), qBound(0.05, r.u32(o + 4) / 100.0, 0.95), c });
        }
        if(st.isEmpty()) { out.fn = [](QRgb p){ return p; }; return true; }
        std::stable_sort(st.begin(), st.end(), [](const Stop& a, const Stop& b){ return a.p < b.p; });
        auto table = std::make_shared<QVector<QRgb>>(256);
        for(int i = 0; i < 256; ++i){
            double t = i / 255.0; if(reversed) t = 1.0 - t;
            QColor c = st.last().c;
            if(t <= st.first().p) c = st.first().c;
            else for(int k = 1; k < st.size(); ++k) if(t <= st[k].p){
                const double span = st[k].p - st[k-1].p;
                double u = span > 1e-9 ? (t - st[k-1].p) / span : 1.0;
                const double m = st[k-1].mid;
                u = u < m ? 0.5 * u / m : 0.5 + 0.5 * (u - m) / (1.0 - m);
                const QColor a = st[k-1].c, b = st[k].c;
                c = QColor::fromRgbF(a.redF() + (b.redF()-a.redF())*u, a.greenF() + (b.greenF()-a.greenF())*u, a.blueF() + (b.blueF()-a.blueF())*u);
                break;
            }
            (*table)[i] = c.rgb();
        }
        out.fn = [table](QRgb p){ const int v = qBound(0, (qRed(p)*299 + qGreen(p)*587 + qBlue(p)*114 + 500) / 1000, 255); return ((*table)[v] & 0xffffffu) | (uint(qAlpha(p)) << 24); };
        return true;
    }
    if(block("clrL", s)){
        DescReader dr(r.d, s.o + 6, s.o + s.len);
        const Val v = dr.desc(0);
        const Val* data = v.get("LUT3DFileData");
        if(!data || data->cls.isEmpty()){ out.fn = [](QRgb p){ return p; }; return true; }     // no table chosen
        out.fn = cubeLut(data->cls);
        return true;                                                                           // null fn: a .3dl or .look table
    }
    return false;
}

void adjustShapes(QVector<Shape>& list, const Adjustment& adj, const QImage* mask, const QRect& maskArea, double strength){
    const PixFn& fn = adj.fn;
    auto lerpTo = [&](QRgb p, QRgb q, int m){ const int k = int(m * strength + 0.5); auto mix = [k](int a, int b){ return a + (b - a) * k / 255; };
        return qRgba(mix(qRed(p), qRed(q)), mix(qGreen(p), qGreen(q)), mix(qBlue(p), qBlue(q)), qAlpha(p)); };
    for(Shape& s : list){
        if(s.type == ShapeType::Group){ adjustShapes(s.children, adj, mask, maskArea, strength); continue; }
        // A curve can't be written back into a gradient's stops, nor a mask into a flat colour: such a layer
        // takes the adjustment as pixels.
        if(!isPixelShape(s) && (mask || (s.fill.type != FillType::Solid && s.fill.type != FillType::None))){
            const QRect area = s.docBounds().toAlignedRect();
            if(!area.isEmpty() && qint64(area.width()) * area.height() <= 40000000){
                Shape flat = s; flat.opacity = 1.0; flat.blend = QPainter::CompositionMode_SourceOver; flat.transp = Transparency(); flat.visible = true;
                const QImage cov = coverageOf(flat, area).convertToFormat(QImage::Format_ARGB32);
                Shape b = bitmapShape(cov, area.left(), area.top(), s.name);
                b.opacity = s.opacity; b.blend = s.blend; b.transp = s.transp; b.visible = s.visible;
                s = b;
            }
        }
        if(isPixelShape(s)){
            QImage img = s.fill.image.convertToFormat(QImage::Format_ARGB32);
            // A layer that keeps its stored channels is adjusted there: K survives, and a profile applies after.
            QImage native = (s.fill.origImage.format() == QImage::Format_ARGB32 && s.fill.origImage.size() == img.size() && s.fill.origImage.cacheKey() != s.fill.image.cacheKey()) ? s.fill.origImage : QImage();
            const int ox = int(s.transform.dx()), oy = int(s.transform.dy());
            for(int y = 0; y < img.height(); ++y){
                QRgb* row = reinterpret_cast<QRgb*>(img.scanLine(y));
                QRgb* nat = native.isNull() ? nullptr : reinterpret_cast<QRgb*>(native.scanLine(y));
                const int my = y + oy - maskArea.top();
                const uchar* m = (mask && my >= 0 && my < maskArea.height()) ? mask->constScanLine(my) : nullptr;
                for(int x = 0; x < img.width(); ++x){
                    int mv = 255;
                    if(mask){ const int mx = x + ox - maskArea.left(); mv = (m && mx >= 0 && mx < maskArea.width()) ? m[mx] : 0; }
                    if(!mv) continue;
                    if(nat){
                        const QRgb n = nat[x];
                        int v[4] = { qRed(n), qGreen(n), qBlue(n), qAlpha(n) };
                        if(adj.cmyk) adj.cmyk(v);
                        else {
                            // Through RGB: in a CMYK document the result is held as pure C, M, Y.
                            const QRgb q = fn(qRgb(v[0]*v[3]/255, v[1]*v[3]/255, v[2]*v[3]/255));
                            v[0] = qRed(q); v[1] = qGreen(q); v[2] = qBlue(q); v[3] = 255;
                        }
                        const int k = int(mv * strength + 0.5);
                        auto mix = [k](int a, int b){ return a + (b - a) * k / 255; };
                        const int c0 = mix(qRed(n), v[0]), c1 = mix(qGreen(n), v[1]), c2 = mix(qBlue(n), v[2]), c3 = mix(qAlpha(n), v[3]);
                        nat[x] = qRgba(c0, c1, c2, c3);
                        row[x] = qRgba(c0*c3/255, c1*c3/255, c2*c3/255, qAlpha(row[x]));
                    } else if(qAlpha(row[x]) != 0) row[x] = lerpTo(row[x], fn(row[x]), mv);
                }
            }
            if(!native.isNull() && g_docColor && g_docColor->icc){
                const int mode = g_docColor->mode, comps = mode == 1 ? 1 : mode == 3 ? 3 : 4, w = img.width();
                QByteArray in(qint64(w) * comps, 0), out(qint64(w) * 3, 0);
                for(int y = 0; y < img.height(); ++y){
                    const QRgb* n = reinterpret_cast<const QRgb*>(native.constScanLine(y));
                    uchar* q = reinterpret_cast<uchar*>(in.data());
                    for(int x = 0; x < w; ++x){
                        if(comps == 1) q[x] = uchar(qRed(n[x]));
                        else if(comps == 3){ q[x*3] = uchar(qRed(n[x])); q[x*3+1] = uchar(qGreen(n[x])); q[x*3+2] = uchar(qBlue(n[x])); }
                        else { q[x*4] = uchar(255 - qRed(n[x])); q[x*4+1] = uchar(255 - qGreen(n[x])); q[x*4+2] = uchar(255 - qBlue(n[x])); q[x*4+3] = uchar(255 - qAlpha(n[x])); }
                    }
                    g_docColor->icc->convert(q, reinterpret_cast<uchar*>(out.data()), w);
                    QRgb* row = reinterpret_cast<QRgb*>(img.scanLine(y));
                    const uchar* o = reinterpret_cast<const uchar*>(out.constData());
                    for(int x = 0; x < w; ++x) row[x] = (row[x] & 0xff000000u) | (uint(o[x*3]) << 16) | (uint(o[x*3+1]) << 8) | o[x*3+2];
                }
            }
            s.fill.image = img;
            s.fill.origImage = native.isNull() ? img : native;
            continue;
        }
        if(s.fill.type == FillType::Solid && !mask){ const QColor c = s.fill.color; s.fill.color = QColor::fromRgba((lerpTo(c.rgba(), fn(c.rgba()), 255) & 0xffffffu) | (uint(c.alpha()) << 24)); }
        if(s.stroke.enabled && !mask){ const QColor c = s.stroke.color; s.stroke.color = QColor::fromRgba((lerpTo(c.rgba(), fn(c.rgba()), 255) & 0xffffffu) | (uint(c.alpha()) << 24)); }
    }
}
// Removes coverage from objects: what a knockout does to everything beneath it.
void cutUnder(QVector<Shape>& list, const QImage& cov, const QRect& area, bool keepFirst){
    for(int i = 0; i < list.size(); ++i){
        Shape& s = list[i];
        if(keepFirst && i == 0) continue;
        if(s.type == ShapeType::Group){ cutUnder(s.children, cov, area, false); continue; }
        if(!isPixelShape(s)){
            const QRect a = s.docBounds().toAlignedRect();
            if(a.isEmpty() || qint64(a.width()) * a.height() > 40000000) continue;
            Shape flat = s; flat.opacity = 1.0; flat.blend = QPainter::CompositionMode_SourceOver; flat.transp = Transparency(); flat.visible = true;
            Shape b = bitmapShape(coverageOf(flat, a).convertToFormat(QImage::Format_ARGB32), a.left(), a.top(), s.name);
            b.opacity = s.opacity; b.blend = s.blend; b.transp = s.transp; b.visible = s.visible;
            s = b;
        }
        QImage img = s.fill.image.convertToFormat(QImage::Format_ARGB32);
        const int ox = int(s.transform.dx()), oy = int(s.transform.dy());
        for(int y = 0; y < img.height(); ++y){
            QRgb* row = reinterpret_cast<QRgb*>(img.scanLine(y));
            const int cy = y + oy - area.top();
            const QRgb* c = (cy >= 0 && cy < area.height()) ? reinterpret_cast<const QRgb*>(cov.constScanLine(cy)) : nullptr;
            if(!c) continue;
            for(int x = 0; x < img.width(); ++x){
                const int cx = x + ox - area.left();
                if(cx >= 0 && cx < area.width()) row[x] = (row[x] & 0xffffffu) | (uint(qAlpha(row[x]) * (255 - qAlpha(c[cx])) / 255) << 24);
            }
        }
        s.fill.image = img;
        if(s.fill.origImage.size() != img.size() || s.fill.origImage.cacheKey() == img.cacheKey()) s.fill.origImage = img;
    }
}

// Photoshop's stored CMYK was only needed while adjustments ran; afterwards the original is the picture.
void dropNative(QVector<Shape>& list){
    for(Shape& s : list){
        if(s.type == ShapeType::Group) dropNative(s.children);
        else if(isPixelShape(s)) s.fill.origImage = s.fill.image;
    }
}

// ---------------------------------------------------------------- layer effects
QVector<float> blurPlane(const QVector<float>& src, int w, int h, double sigma){
    if(sigma < 0.3) return src;
    // Three box passes approximate the Gaussian in time independent of the radius.
    const int r = qMax(1, int(std::sqrt(4.0 * sigma * sigma + 1.0) / 2.0 + 0.5) - 0);
    QVector<float> a = src, b(src.size());
    for(int pass = 0; pass < 3; ++pass){
        for(int y = 0; y < h; ++y){
            double sum = 0; const float* in = a.constData() + qint64(y) * w; float* out = b.data() + qint64(y) * w;
            for(int x = -r; x <= r; ++x) sum += (x >= 0 && x < w) ? in[x] : 0.f;
            for(int x = 0; x < w; ++x){
                out[x] = float(sum / (2*r + 1));
                sum += (x + r + 1 < w ? in[x + r + 1] : 0.f) - (x - r >= 0 ? in[x - r] : 0.f);
            }
        }
        for(int x = 0; x < w; ++x){
            double sum = 0;
            for(int y = -r; y <= r; ++y) sum += (y >= 0 && y < h) ? b[qint64(y)*w + x] : 0.f;
            for(int y = 0; y < h; ++y){
                a[qint64(y)*w + x] = float(sum / (2*r + 1));
                sum += (y + r + 1 < h ? b[qint64(y+r+1)*w + x] : 0.f) - (y - r >= 0 ? b[qint64(y-r)*w + x] : 0.f);
            }
        }
    }
    return a;
}

// Euclidean distance to the nearest pixel where `in` holds (Felzenszwalb), for strokes.
QVector<float> distancePlane(const QVector<float>& cov, int w, int h, bool toInside){
    const float INF = 1e12f;
    QVector<float> d(cov.size());
    for(qint64 i = 0; i < d.size(); ++i){ const bool hit = toInside ? cov[i] >= 0.5f : cov[i] < 0.5f; d[i] = hit ? 0.f : INF; }
    auto pass = [&](float* f, int n, int stride){
        QVector<float> ff(n), out(n), z(n + 1); QVector<int> v(n);
        for(int i = 0; i < n; ++i) ff[i] = f[i * stride];
        int k = 0; v[0] = 0; z[0] = -INF; z[1] = INF;
        for(int q = 1; q < n; ++q){
            float s;
            while(true){
                s = ((ff[q] + q*q) - (ff[v[k]] + v[k]*v[k])) / (2.f*q - 2.f*v[k]);
                if(s <= z[k] && k > 0) --k; else break;
            }
            ++k; v[k] = q; z[k] = s; z[k+1] = INF;
        }
        k = 0;
        for(int q = 0; q < n; ++q){ while(z[k+1] < q) ++k; out[q] = (q - v[k])*(q - v[k]) + ff[v[k]]; }
        for(int i = 0; i < n; ++i) f[i * stride] = out[i];
    };
    for(int x = 0; x < w; ++x) pass(d.data() + x, h, w);
    for(int y = 0; y < h; ++y) pass(d.data() + qint64(y) * w, w, 1);
    for(float& v : d) v = std::sqrt(v);
    return d;
}

QImage colourImage(const QVector<float>& cov, int w, int h, const QColor& c){
    QImage img(w, h, QImage::Format_ARGB32);
    if(img.isNull()) return img;
    const QRgb rgb = c.rgb() & 0xffffffu;
    for(int y = 0; y < h; ++y){
        QRgb* row = reinterpret_cast<QRgb*>(img.scanLine(y));
        for(int x = 0; x < w; ++x) row[x] = rgb | (uint(qBound(0, int(cov[qint64(y)*w+x] * c.alphaF() * 255.0 + 0.5), 255)) << 24);
    }
    return img;
}

// A gradient drawn per pixel over `area`, so angle and diamond styles are exact.
QImage gradientImage(const QVector<GradStop>& stops, const QString& type, double angleDeg, double scale, const QRectF& box, QPointF offset, const QRect& area){
    QImage img(area.size(), QImage::Format_ARGB32);
    if(img.isNull() || stops.isEmpty()) return img;
    QRgb lut[1025];
    { int k = 0; for(int i = 0; i <= 1024; ++i){ const double t = i / 1024.0; while(k + 1 < stops.size() - 1 && stops[k+1].pos < t) ++k;
        const GradStop& a = stops[k]; const GradStop& b = stops[qMin(k+1, int(stops.size())-1)];
        const double u = b.pos > a.pos ? qBound(0.0, (t - a.pos) / (b.pos - a.pos), 1.0) : 0.0;
        auto mix = [u](double x, double y){ return qBound(0, int((x + (y - x) * u) * 255.0 + 0.5), 255); };
        lut[i] = qRgba(mix(a.col.redF(), b.col.redF()), mix(a.col.greenF(), b.col.greenF()), mix(a.col.blueF(), b.col.blueF()), mix(a.col.alphaF(), b.col.alphaF())); } }
    const QPointF c = box.center() + offset;
    const double rad = angleDeg * M_PI / 180.0, dx = std::cos(rad), dy = -std::sin(rad);
    const double half = qMax(0.5, 0.5 * scale * (qAbs(dx) * box.width() + qAbs(dy) * box.height()));
    const double radius = qMax(1.0, 0.5 * scale * std::hypot(box.width(), box.height()));
    for(int y = 0; y < img.height(); ++y){
        QRgb* row = reinterpret_cast<QRgb*>(img.scanLine(y));
        for(int x = 0; x < img.width(); ++x){
            const double px = area.left() + x + 0.5 - c.x(), py = area.top() + y + 0.5 - c.y();
            const double u = px * dx + py * dy, v = -px * dy + py * dx;
            double t;
            if(type == "Rdl") t = std::hypot(px, py) / radius;
            else if(type == "Dmnd") t = (qAbs(u) + qAbs(v)) / radius;
            else if(type == "Angl"){ t = std::atan2(v, u) / (2.0 * M_PI); if(t < 0) t += 1.0; t = 1.0 - t; }
            else if(type == "Rflc") t = qAbs(u) / half;
            else t = (u + half) / (2.0 * half);
            row[x] = lut[qBound(0, int(t * 1024.0 + 0.5), 1024)];
        }
    }
    return img;
}

using PatternMap = QHash<QString, QImage>;

// Patt / Pat2 / Pat3: pattern resources, each channel a small record of its own.
void readPatterns(const Rd& r, const Span& s, PatternMap& out){
    qint64 o = s.o; const qint64 end = s.o + s.len;
    while(o + 4 <= end && out.size() < 4096){
        const qint64 len = r.u32(o); o += 4;
        if(len <= 0 || len > end - o) return;
        const qint64 pe = o + len;
        qint64 p = o + 4;
        ColorInfo ci; ci.mode = int(r.u32(p)); p += 4;
        p += 4;                                                  // the point is repeated in the rectangle below
        const qint64 nameLen = r.u32(p); p += 4 + qMax<qint64>(0, nameLen) * 2;
        const int idLen = r.u8(p);
        const QString id = QString::fromLatin1(r.d.mid(p + 1, idLen)); p += 1 + idLen;
        if(ci.mode == 2){ ci.table = r.d.mid(p, 768); p += 772; }
        // Virtual memory array list.
        p += 4;
        const qint64 listLen = r.u32(p); p += 4;
        const qint64 listEnd = qMin(pe, p + qMax<qint64>(0, listLen));
        const int T = r.i32(p), L = r.i32(p+4), B = r.i32(p+8), R2 = r.i32(p+12); p += 16;
        const int nch = int(r.u32(p)); p += 4;
        const int w = R2 - L, h = B - T;
        o = pe + ((4 - (len & 3)) & 3);
        if(w <= 0 || h <= 0 || qint64(w) * h > 16000000) continue;
        QHash<int, QByteArray> planes; QByteArray alpha;
        const int base = baseChannels(ci.mode);
        int stored = 0;
        for(int c = 0; c < nch + 2 && p + 8 <= listEnd; ++c){
            const qint64 written = r.u32(p); p += 4;
            if(!written) continue;
            const qint64 l2 = r.u32(p); p += 4;
            if(l2 < 0 || l2 > listEnd - p) break;
            if(l2 == 0) continue;
            const qint64 be = p + l2;
            const int depth = int(r.u32(p));
            const int rt = r.i32(p+4), rl = r.i32(p+8), rb = r.i32(p+12), rr = r.i32(p+16);
            const int px = r.u16(p+20), comp = r.u8(p+22);
            const int cw = rr - rl, ch = rb - rt;
            Q_UNUSED(depth);
            if(cw == w && ch == h && (px == 8 || px == 16 || px == 32)){
                QByteArray pl = decodePlane(r, p + 23, be, comp, w, h, px, false);
                if(stored < base) planes[stored] = pl; else if(stored == base) alpha = pl;
                ++stored;
            }
            p = be;
        }
        if(planes.size() >= base) out.insert(id, planesToImage(ci, w, h, planes, alpha.isEmpty() ? nullptr : &alpha));
    }
}

QImage patternImage(const Val& e, const PatternMap& pats, const QRect& area){
    const Val* pt = e.get("Ptrn");
    if(!pt) return {};
    const QImage tile = pats.value(pt->s("Idnt"));
    if(tile.isNull()) return {};
    QImage img(area.size(), QImage::Format_ARGB32_Premultiplied);
    if(img.isNull()) return img;
    img.fill(Qt::transparent);
    const double sc = qBound(0.01, e.n("Scl", 100.0) / 100.0, 100.0);
    QPointF ph;
    if(const Val* v = e.get("phase")) ph = QPointF(v->n("Hrzn"), v->n("Vrtc"));
    QPainter g(&img);
    g.setRenderHint(QPainter::SmoothPixmapTransform);
    g.translate(-area.topLeft());
    QBrush br(tile);
    br.setTransform(QTransform::fromTranslate(ph.x(), ph.y()).scale(sc, sc));
    g.fillRect(area, br);
    g.end();
    return img.convertToFormat(QImage::Format_ARGB32);
}

struct FxOut { QVector<Shape> under, over; };

void buildEffects(const Val& fx, double globalAngle, const QImage& coverage, const QPoint& origin, const QRect& box, const QString& name, const PatternMap& pats, FxOut& out){
    if(fx.get("masterFXSwitch") && fx.n("masterFXSwitch") == 0) return;
    const int w = coverage.width(), h = coverage.height();
    if(w < 1 || h < 1) return;
    QVector<float> cov(qint64(w) * h);
    for(int y = 0; y < h; ++y){ const QRgb* row = reinterpret_cast<const QRgb*>(coverage.constScanLine(y)); for(int x = 0; x < w; ++x) cov[qint64(y)*w+x] = qAlpha(row[x]) / 255.f; }
    auto each = [&](const char* single, const char* multi, const std::function<void(const Val&)>& fn){
        if(const Val* v = fx.get(multi)){ for(const Val& e : v->list) if(e.n("enab", 1) != 0) fn(e); return; }
        if(const Val* v = fx.get(single)) if(v->n("enab", 1) != 0) fn(*v);
    };
    auto shape = [&](const QImage& img, const Val& e, const char* label, const char* modeKey = "Md  "){
        Shape s = bitmapShape(img, origin.x(), origin.y(), QStringLiteral("%1 (%2)").arg(name, QLatin1String(label)));
        const Val* md = e.get(QByteArray(modeKey).trimmed().constData());
        if(md) applyBlend(s, blendKeyOfEnum(md->str));
        return s;
    };
    auto colorOp = [](const Val& e, const char* colorKey = "Clr", const char* opKey = "Opct"){
        QColor c = colorOf(e.get(colorKey)); c.setAlphaF(qBound(0.0, e.n(opKey, 100.0) / 100.0, 1.0)); return c;
    };
    auto angleOf = [&](const Val& e){ return (e.n("uglg", 1) != 0 ? globalAngle : e.n("lagl", globalAngle)) * M_PI / 180.0; };

    // Photoshop's Size spreads the falloff over that many pixels; a sigma of half gives the same reach.
    each("DrSh", "dropShadowMulti", [&](const Val& e){
        const double size = qMax(0.0, e.n("blur")), ch = qBound(0.0, e.n("Ckmt") / 100.0, 1.0), a = angleOf(e), dist = e.n("Dstn");
        QVector<float> b = blurPlane(cov, w, h, size * (1.0 - ch) / 2.0);
        if(ch > 0) for(float& v : b) v = qMin(1.f, v / float(qMax(0.05, 1.0 - ch)));
        QVector<float> o(b.size(), 0.f);
        const int dx = int(std::lround(-dist * std::cos(a))), dy = int(std::lround(dist * std::sin(a)));
        for(int y = 0; y < h; ++y) for(int x = 0; x < w; ++x){ const int sx = x - dx, sy = y - dy; if(sx >= 0 && sy >= 0 && sx < w && sy < h) o[qint64(y)*w+x] = b[qint64(sy)*w+sx]; }
        out.under.push_back(shape(colourImage(o, w, h, colorOp(e)), e, "drop shadow"));
    });
    each("OrGl", "outerGlowMulti", [&](const Val& e){
        const double size = qMax(0.0, e.n("blur")), ch = qBound(0.0, e.n("Ckmt") / 100.0, 1.0);
        QVector<float> b = blurPlane(cov, w, h, size * (1.0 - ch) / 2.0);
        // Spreading the same mass over a wider area: rescale so the edge stays strong.
        for(float& v : b) v = qMin(1.f, v * 2.f / float(qMax(0.1, 2.0 - ch * 2.0 + ch)));
        out.under.push_back(shape(colourImage(b, w, h, colorOp(e)), e, "outer glow"));
    });
    auto interior = [&](QVector<float> v, const QColor& c, const Val& e, const char* label){
        for(qint64 i = 0; i < v.size(); ++i) v[i] *= cov[i];
        out.over.push_back(shape(colourImage(v, w, h, c), e, label));
    };
    if(const Val* e = fx.get("patternFill")) if(e->n("enab", 1) != 0){
        QImage img = patternImage(*e, pats, QRect(origin, QSize(w, h)));
        if(!img.isNull()){
            const double op = qBound(0.0, e->n("Opct", 100.0) / 100.0, 1.0);
            for(int y = 0; y < h; ++y){ QRgb* row = reinterpret_cast<QRgb*>(img.scanLine(y)); for(int x = 0; x < w; ++x) row[x] = (row[x] & 0xffffffu) | (uint(qAlpha(row[x]) * cov[qint64(y)*w+x] * op + 0.5) << 24); }
            out.over.push_back(shape(img, *e, "pattern overlay"));
        }
    }
    each("GrFl", "gradientFillMulti", [&](const Val& e){
        const QVector<GradStop> st = gradStops(e.get("Grad"), e.n("Rvrs") != 0);
        if(st.isEmpty()) return;
        QImage img = gradientImage(st, e.s("Type"), e.n("Angl", 90.0), qBound(0.1, e.n("Scl", 100.0) / 100.0, 1.5), box, QPointF(), QRect(origin, QSize(w, h)));
        if(img.isNull()) return;
        const double op = qBound(0.0, e.n("Opct", 100.0) / 100.0, 1.0);
        for(int y = 0; y < h; ++y){ QRgb* row = reinterpret_cast<QRgb*>(img.scanLine(y)); for(int x = 0; x < w; ++x) row[x] = (row[x] & 0xffffffu) | (uint(qAlpha(row[x]) * cov[qint64(y)*w+x] * op + 0.5) << 24); }
        out.over.push_back(shape(img, e, "gradient overlay"));
    });
    each("SoFi", "solidFillMulti", [&](const Val& e){ interior(QVector<float>(cov.size(), 1.f), colorOp(e), e, "colour overlay"); });
    each("ChFX", "satinMulti", [&](const Val& e){
        // The outline's blurred coverage moved both ways along the angle: their difference is the sheen.
        const double a = e.n("lagl", 19.0) * M_PI / 180.0, dist = e.n("Dstn", 10.0);
        const QVector<float> b = blurPlane(cov, w, h, qMax(0.0, e.n("blur", 14.0)) / 2.0);
        const int dx = int(std::lround(dist * std::cos(a))), dy = int(std::lround(-dist * std::sin(a)));
        const bool inv = e.n("Invr") != 0;
        QVector<float> o(b.size());
        auto at = [&](int x, int y){ return (x >= 0 && y >= 0 && x < w && y < h) ? b[qint64(y)*w+x] : 0.f; };
        for(int y = 0; y < h; ++y) for(int x = 0; x < w; ++x){
            const float v = qAbs(at(x - dx, y - dy) - at(x + dx, y + dy));
            o[qint64(y)*w+x] = inv ? 1.f - v : v;
        }
        interior(o, colorOp(e), e, "satin");
    });
    each("IrGl", "innerGlowMulti", [&](const Val& e){
        const double size = qMax(0.0, e.n("blur"));
        QVector<float> inv(cov.size());
        for(qint64 i = 0; i < cov.size(); ++i) inv[i] = 1.f - cov[i];
        QVector<float> b = blurPlane(inv, w, h, size / 2.0);
        for(float& v : b) v = qMin(1.f, v * 2.f);
        if(e.s("glwS") == "SrcC") for(float& v : b) v = 1.f - v;
        interior(b, colorOp(e), e, "inner glow");
    });
    each("IrSh", "innerShadowMulti", [&](const Val& e){
        const double size = qMax(0.0, e.n("blur")), a = angleOf(e), dist = e.n("Dstn");
        QVector<float> inv(cov.size());
        for(qint64 i = 0; i < cov.size(); ++i) inv[i] = 1.f - cov[i];
        const QVector<float> b = blurPlane(inv, w, h, size / 2.0);
        QVector<float> o(b.size(), 1.f);
        const int dx = int(std::lround(-dist * std::cos(a))), dy = int(std::lround(dist * std::sin(a)));
        for(int y = 0; y < h; ++y) for(int x = 0; x < w; ++x){ const int sx = x - dx, sy = y - dy; if(sx >= 0 && sy >= 0 && sx < w && sy < h) o[qint64(y)*w+x] = b[qint64(sy)*w+sx]; }
        interior(o, colorOp(e), e, "inner shadow");
    });
    each("FrFX", "frameFXMulti", [&](const Val& e){
        const double sz = qMax(0.0, e.n("Sz", 3.0));
        const QString st = e.s("Styl");
        const QVector<float> outD = distancePlane(cov, w, h, true), inD = distancePlane(cov, w, h, false);
        QVector<float> o(cov.size());
        const double reachOut = st == "InsF" ? 0.0 : st == "CtrF" ? sz / 2.0 : sz, reachIn = st == "OutF" ? 0.0 : st == "CtrF" ? sz / 2.0 : sz;
        for(qint64 i = 0; i < o.size(); ++i){
            const double sd = outD[i] > 0 ? outD[i] : -inD[i];
            // Distances run between pixel centres, so a pixel exactly at the reach is fully inside the stroke.
            float v = sd >= 0 ? float(qBound(0.0, reachOut - sd + 1.0, 1.0)) : float(qBound(0.0, reachIn + sd + 1.0, 1.0));
            if(reachOut <= 0.0 && sd >= 0) v = 0.f;
            if(reachIn <= 0.0 && sd < 0) v = 0.f;
            o[i] = v;
        }
        QImage img;
        const QVector<GradStop> st2 = e.s("PntT") == "GrFl" ? gradStops(e.get("Grad"), e.n("Rvrs") != 0) : QVector<GradStop>();
        if(!st2.isEmpty()){
            img = gradientImage(st2, e.s("Type"), e.n("Angl", 90.0), qBound(0.1, e.n("Scl", 100.0) / 100.0, 1.5), box, QPointF(), QRect(origin, QSize(w, h)));
            const double op = qBound(0.0, e.n("Opct", 100.0) / 100.0, 1.0);
            for(int y = 0; y < h; ++y){ QRgb* row = reinterpret_cast<QRgb*>(img.scanLine(y)); for(int x = 0; x < w; ++x) row[x] = (row[x] & 0xffffffu) | (uint(qAlpha(row[x]) * o[qint64(y)*w+x] * op + 0.5) << 24); }
        } else img = colourImage(o, w, h, colorOp(e));
        out.over.push_back(shape(img, e, "stroke"));
    });
    if(const Val* e = fx.get("ebbl")) if(e->n("enab", 1) != 0){
        // A height field from the blurred coverage, lit from the style's angle: highlights and shadows separately.
        const double size = qMax(1.0, e->n("blur", 5.0)), a = angleOf(*e), elev = e->n("Lald", 30.0) * M_PI / 180.0, depth = qBound(0.01, e->n("srgR", 100.0) / 100.0, 10.0);
        const QVector<float> hf = blurPlane(cov, w, h, size / 2.0);
        const double lx = std::cos(a) * std::cos(elev), ly = -std::sin(a) * std::cos(elev), lz = std::sin(elev);
        QVector<float> hi(cov.size(), 0.f), lo(cov.size(), 0.f);
        const double k = depth * size;
        for(int y = 1; y < h - 1; ++y) for(int x = 1; x < w - 1; ++x){
            const qint64 i = qint64(y)*w+x;
            if(cov[i] <= 0.f) continue;
            const double gx = (hf[i+1] - hf[i-1]) * k, gy = (hf[i+w] - hf[i-w]) * k;
            const double nl = (-gx*lx - gy*ly + lz) / std::sqrt(gx*gx + gy*gy + 1.0) - lz;
            if(nl > 0) hi[i] = float(qMin(1.0, nl / qMax(0.05, 1.0 - lz))); else lo[i] = float(qMin(1.0, -nl / qMax(0.05, lz)));
        }
        QColor hc = colorOf(e->get("hglC")); hc.setAlphaF(qBound(0.0, e->n("hglO", 75.0) / 100.0, 1.0));
        QColor lc = colorOf(e->get("sdwC")); lc.setAlphaF(qBound(0.0, e->n("sdwO", 75.0) / 100.0, 1.0));
        Shape sl = bitmapShape(colourImage(lo, w, h, lc), origin.x(), origin.y(), QStringLiteral("%1 (bevel shadow)").arg(name));
        if(const Val* m = e->get("sdwM")) applyBlend(sl, blendKeyOfEnum(m->str));
        Shape sh = bitmapShape(colourImage(hi, w, h, hc), origin.x(), origin.y(), QStringLiteral("%1 (bevel highlight)").arg(name));
        if(const Val* m = e->get("hglM")) applyBlend(sh, blendKeyOfEnum(m->str));
        out.over.push_back(sl); out.over.push_back(sh);
    }
}

// ---------------------------------------------------------------- text
QString installedFamilyFor(const QString& psName){
    static const QHash<QString, QString> have = []{
        QHash<QString, QString> h;
        const QStringList l = QFontDatabase::families();
        for(const QString& x : l) h.insert(QString(x).remove(QLatin1Char(' ')).toLower(), x);
        return h;
    }();
    QString base = psName.section(QLatin1Char('-'), 0, 0).toLower();
    if(base.endsWith(QLatin1String("psmt"))) base.chop(4);
    else if(base.endsWith(QLatin1String("mt")) || base.endsWith(QLatin1String("ps"))) base.chop(2);
    return have.value(base);
}

// The layer as live text, or empty to keep its pixels: a missing font or a warp can't be reproduced faithfully.
QString textSvg(const Rd& r, const Span& s){
    if(s.len < 60) return {};
    qint64 o = s.o + 2;
    double m[6]; for(int i = 0; i < 6; ++i){ m[i] = r.f64(o); o += 8; }
    o += 2 + 4;
    DescReader dr(r.d, o, s.o + s.len);
    const Val td = dr.desc(0);
    o = dr.p;
    { DescReader wr(r.d, o + 6, s.o + s.len); const Val warp = wr.desc(0); const QString st = warp.s("warpStyle"); if(!st.isEmpty() && st != "warpNone") return {}; }
    const Val* ed = td.get("EngineData");
    if(!ed || ed->cls.isEmpty()) return {};
    EngineReader er(ed->cls);
    const Val root = er.value(0);
    const Val* eng = root.get("EngineDict");
    const Val* res = root.get("ResourceDict");
    if(!eng || !res) return {};
    const Val* ed2 = eng->get("Editor");
    const QString text = ed2 ? ed2->s("Text") : QString();
    if(text.isEmpty()) return {};
    const Val* fontSet = res->get("FontSet");
    const Val* sr = eng->get("StyleRun");
    const Val* rl = sr ? sr->get("RunLengthArray") : nullptr;
    const Val* ra = sr ? sr->get("RunArray") : nullptr;
    if(!fontSet || !rl || !ra || rl->list.size() != ra->list.size()) return {};

    const Val* sheets = res->get("StyleSheetSet");
    const Val* defSheet = nullptr;
    if(sheets && !sheets->list.isEmpty()) defSheet = sheets->list[0].get("StyleSheetData");
    auto prop = [&](const Val* run, const char* k)->const Val* {
        if(run) if(const Val* v = run->get(k)) return v;
        return defSheet ? defSheet->get(k) : nullptr;
    };
    struct Piece { QString t, family; double size; QColor col; int weight; bool italic; double leading; };
    QVector<Piece> pieces;
    int pos = 0;
    for(int i = 0; i < ra->list.size(); ++i){
        const int len = int(qBound(0.0, rl->list[i].num, 1e7));
        const Val* ssd = nullptr;
        if(const Val* ss = ra->list[i].get("StyleSheet")) ssd = ss->get("StyleSheetData");
        const Val* fv = prop(ssd, "Font"), *sz = prop(ssd, "FontSize"), *fc = prop(ssd, "FillColor");
        const int fi = fv ? int(fv->num) : 0;
        if(fi < 0 || fi >= fontSet->list.size()) return {};
        const QString ps = fontSet->list[fi].s("Name");
        const QString fam = installedFamilyFor(ps);
        if(fam.isEmpty()) return {};
        QColor col(Qt::black);
        if(fc) if(const Val* vals = fc->get("Values")) if(vals->list.size() >= 4) col = QColor::fromRgbF(qBound(0.0, vals->list[1].num, 1.0), qBound(0.0, vals->list[2].num, 1.0), qBound(0.0, vals->list[3].num, 1.0));
        const bool bold = ps.contains(QLatin1String("Bold"), Qt::CaseInsensitive), it = ps.contains(QLatin1String("Italic"), Qt::CaseInsensitive) || ps.contains(QLatin1String("Oblique"), Qt::CaseInsensitive);
        pieces.push_back({ text.mid(pos, len), fam, sz ? qBound(0.1, sz->num, 5000.0) : 12.0, col, bold ? 700 : 400, it, (prop(ssd, "AutoLeading") && prop(ssd, "AutoLeading")->num == 0 && prop(ssd, "Leading")) ? qBound(0.1, prop(ssd, "Leading")->num, 10000.0) : 0.0 });
        pos += len;
    }
    double boxW = 0.0, boxTop = 0.0, boxLeft = 0.0; bool box = false;
    if(const Val* rend = eng->get("Rendered")) if(const Val* sh = rend->get("Shapes")) if(const Val* ch = sh->get("Children")) if(!ch->list.isEmpty()){
        if(const Val* cook = ch->list[0].get("Cookie")) if(const Val* ph = cook->get("Photoshop")){
            box = ph->n("ShapeType") == 1;
            if(ph->n("ShapeType") > 1) return {};          // on a path: no flat layout can stand in for it
            if(const Val* bb = ph->get("BoxBounds")) if(bb->list.size() >= 4){ boxLeft = bb->list[0].num; boxTop = bb->list[1].num; boxW = bb->list[2].num - bb->list[0].num; }
        }
    }
    int just = 0; double lead = 1.2;
    if(const Val* pr = eng->get("ParagraphRun")) if(const Val* a = pr->get("RunArray")) if(!a->list.isEmpty())
        if(const Val* ps = a->list[0].get("ParagraphSheet")) if(const Val* pp = ps->get("Properties")){ just = int(pp->n("Justification")); lead = qBound(0.1, pp->n("AutoLeading", 1.2), 10.0); }

    auto fontOf = [](const Piece& p){ QFont f(p.family); f.setPixelSize(100); f.setWeight(QFont::Weight(p.weight)); f.setItalic(p.italic); return f; };
    // Split into lines of same-styled pieces, wrapping in a box on spaces.
    struct Line { QVector<Piece> ps; double w = 0, size = 0, leading = 0; };
    QVector<Line> lines; Line cur;
    auto endLine = [&]{ lines.push_back(cur); cur = Line(); };
    for(const Piece& p : pieces){
        const QFontMetricsF fm(fontOf(p));
        const double unit = p.size / 100.0;
        Piece acc = p; acc.t.clear(); double accW = 0;
        auto flushAcc = [&]{ if(!acc.t.isEmpty()){ cur.ps.push_back(acc); cur.w += accW; cur.size = qMax(cur.size, p.size); cur.leading = qMax(cur.leading, p.leading); acc.t.clear(); accW = 0; } };
        QString word;
        auto putWord = [&]{
            if(word.isEmpty()) return;
            const double ww = fm.horizontalAdvance(word) * unit;
            if(box && boxW > 0 && cur.w + accW + ww > boxW && (cur.w + accW) > 0){ flushAcc(); while(!cur.ps.isEmpty() && cur.ps.last().t.endsWith(QLatin1Char(' '))){ cur.ps.last().t.chop(1); if(cur.ps.last().t.isEmpty()) cur.ps.removeLast(); } endLine(); }
            acc.t += word; accW += ww; word.clear();
        };
        for(const QChar c : p.t){
            if(c == QLatin1Char('\r') || c == QLatin1Char('\n') || c == QChar(3)){ putWord(); flushAcc(); if(cur.size <= 0) cur.size = p.size; endLine(); continue; }
            word += c;
            if(c.isSpace()) putWord();
        }
        putWord(); flushAcc();
    }
    if(!cur.ps.isEmpty()) endLine();
    while(!lines.isEmpty() && lines.last().ps.isEmpty()) lines.removeLast();
    if(lines.isEmpty()) return {};

    QString out = QStringLiteral("<g transform=\"matrix(%1 %2 %3 %4 %5 %6)\">").arg(fn(m[0]), fn(m[1]), fn(m[2]), fn(m[3]), fn(m[4]), fn(m[5]));
    double y = 0.0;
    if(box){ QFont f0 = fontOf(lines[0].ps.isEmpty() ? pieces[0] : lines[0].ps[0]); y = boxTop + QFontMetricsF(f0).ascent() * lines[0].size / 100.0; }
    for(const Line& ln : lines){
        if(!ln.ps.isEmpty()){
            double x = box ? boxLeft : 0.0;
            const char* anchor = "start";
            if(just == 1){ anchor = "end"; if(box) x += boxW; }
            else if(just == 2){ anchor = "middle"; if(box) x += boxW / 2.0; }
            out += QStringLiteral("<text xml:space=\"preserve\" x=\"%1\" y=\"%2\" text-anchor=\"%3\">").arg(fn(x), fn(y), QLatin1String(anchor));
            for(const Piece& p : ln.ps){
                QString a = QStringLiteral(" font-family=\"%1\" font-size=\"%2\" fill=\"%3\"").arg(xmlEsc(p.family), fn(p.size), hx(p.col));
                if(p.weight != 400) a += QStringLiteral(" font-weight=\"%1\"").arg(p.weight);
                if(p.italic) a += QStringLiteral(" font-style=\"italic\"");
                out += QStringLiteral("<tspan%1>%2</tspan>").arg(a, xmlEsc(p.t));
            }
            out += QStringLiteral("</text>");
        }
        y += ln.leading > 0 ? ln.leading : (ln.size > 0 ? ln.size : 12.0) * lead;
    }
    return out + QStringLiteral("</g>");
}

// ---------------------------------------------------------------- the document
struct Builder {
    const Rd& r;
    int W, H;
    double globalAngle = 120.0;
    PsdImportStats* stats;
    PatternMap patterns;

    QVector<Shape> fromSvg(const QString& inner) const {
        const QString svg = QStringLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xarq=\"urn:xarq\" width=\"%1\" height=\"%2\" viewBox=\"0 0 %1 %2\">%3</svg>").arg(W).arg(H).arg(inner);
        QString err;
        return parseSvg(svg.toUtf8(), &err, QString());
    }
    Val descAt(const Span& s, int skip) const { DescReader d(r.d, s.o + skip, s.o + s.len); return d.desc(0); }

    // A gradient spanning `box`, as SVG, with an id; Photoshop measures its length along the angle.
    QString gradientDef(const Val& g, const QRectF& box, const QString& id) const {
        QVector<GradStop> st = gradStops(g.get("Grad"), g.n("Rvrs") != 0);
        if(st.isEmpty()) return {};
        const QString type = g.s("Type");
        // Reflected: the ramp runs out from the centre both ways. Written out in full, so no spread method is needed.
        if(type == "Rflc"){
            QVector<GradStop> both;
            for(int i = st.size() - 1; i > 0; --i) both.push_back({ 0.5 - st[i].pos / 2.0, st[i].col });
            for(const GradStop& q : st) both.push_back({ 0.5 + q.pos / 2.0, q.col });
            st = both;
        }
        const double a = g.n("Angl", 90.0) * M_PI / 180.0, sc = qBound(0.1, g.n("Scl", 100.0) / 100.0, 1.5);
        QPointF off;
        if(const Val* of = g.get("Ofst")) off = QPointF(of->n("Hrzn") / 100.0 * box.width(), of->n("Vrtc") / 100.0 * box.height());
        const QPointF c = box.center() + off;
        const double dx = std::cos(a), dy = -std::sin(a), half = 0.5 * sc * (qAbs(dx) * box.width() + qAbs(dy) * box.height());
        QString stops;
        for(const GradStop& s : st) stops += QStringLiteral("<stop offset=\"%1\" stop-color=\"%2\"%3/>").arg(fn(s.pos), hx(s.col), s.col.alphaF() < 0.999 ? QStringLiteral(" stop-opacity=\"%1\"").arg(fn(s.col.alphaF())) : QString());
        if(type == "Rdl" || type == "Dmnd")
            return QStringLiteral("<radialGradient id=\"%1\" gradientUnits=\"userSpaceOnUse\" cx=\"%2\" cy=\"%3\" r=\"%4\">%5</radialGradient>").arg(id, fn(c.x()), fn(c.y()), fn(qMax(1.0, 0.5 * sc * std::hypot(box.width(), box.height()))), stops);
        const QPointF p0 = c - QPointF(dx, dy) * half, p1 = c + QPointF(dx, dy) * half;
        return QStringLiteral("<linearGradient id=\"%1\" gradientUnits=\"userSpaceOnUse\" x1=\"%2\" y1=\"%3\" x2=\"%4\" y2=\"%5\"%6>%7</linearGradient>")
            .arg(id, fn(p0.x()), fn(p0.y()), fn(p1.x()), fn(p1.y()), QString(), stops);
    }

    // What fills a shape or fill layer. A stroked shape keeps it in vscg, tagged with the fill kind.
    struct FillSrc { QByteArray kind; Val v; bool ok = false; };
    FillSrc fillSource(const Lay& L) const {
        FillSrc f;
        if(L.blocks.contains("vscg")){ const Span sp = L.blocks["vscg"]; f.kind = r.d.mid(sp.o, 4); f.v = descAt(sp, 8); f.ok = true; return f; }
        for(const char* k : { "SoCo", "GdFl", "PtFl" }) if(L.blocks.contains(k)){ f.kind = k; f.v = descAt(L.blocks[k], 4); f.ok = true; return f; }
        return f;
    }
    // Attributes that paint `path` with a fill source; defs gets whatever they refer to.
    QString fillAttrs(const FillSrc& f, const QPainterPath& path, QString& defs) const {
        const QRectF box = path.boundingRect();
        if(f.kind == "SoCo") return QStringLiteral(" fill=\"%1\"").arg(hx(colorOf(f.v.get("Clr"))));
        QImage tex; QRect area = box.toAlignedRect();
        if(f.kind == "GdFl"){
            const QString type = f.v.s("Type");
            if(type != "Angl" && type != "Dmnd"){
                const QString g = gradientDef(f.v, box, QStringLiteral("g"));
                if(g.isEmpty()) return {};
                defs += g;
                return QStringLiteral(" fill=\"url(#g)\"");
            }
            QPointF off;
            if(const Val* of = f.v.get("Ofst")) off = QPointF(of->n("Hrzn") / 100.0 * box.width(), of->n("Vrtc") / 100.0 * box.height());
            tex = gradientImage(gradStops(f.v.get("Grad"), f.v.n("Rvrs") != 0), type, f.v.n("Angl", 90.0), qBound(0.1, f.v.n("Scl", 100.0) / 100.0, 1.5), box, off, area);
        } else if(f.kind == "PtFl"){
            tex = patternImage(f.v, patterns, area);
        }
        if(tex.isNull() || area.isEmpty()) return {};
        QByteArray png; QBuffer buf(&png);
        if(!buf.open(QIODevice::WriteOnly) || !tex.save(&buf, "PNG")) return {};
        return QStringLiteral(" fill=\"#808080\" xarq:bitmap=\"data:image/png;base64,%1\" xarq:bitmap-xf=\"%2 0 0 %3 %4 %5\" xarq:bitmap-tiled=\"0\"")
            .arg(QString::fromLatin1(png.toBase64())).arg(area.width()).arg(-area.height()).arg(area.left()).arg(area.bottom() + 1);
    }

    // Shape and fill layers: real geometry with the layer's own fill and stroke.
    // With solidProbe, the opaque silhouette a layer effect sees: its outline however its fill is painted.
    QVector<Shape> vectorLayer(const Lay& L, bool& handled, bool& needVecMask, bool solidProbe = false) const {
        handled = false; needVecMask = false;
        const FillSrc fs = fillSource(L);
        if(!fs.ok && !L.blocks.contains("vstk")) return {};
        Span vm = L.blocks.value("vmsk"); if(!vm.len) vm = L.blocks.value("vsms");
        bool disabled = false, inverted = false; int firstOp = 1;
        QPainterPath path = vm.len ? readVectorPath(r, vm, W, H, &disabled, &inverted, &firstOp) : QPainterPath();
        const bool soft = !path.isEmpty() && !disabled && (L.vecDensity < 0.999 || L.vecFeather > 0.0);
        if(path.isEmpty() || disabled || inverted || soft){
            QPainterPath full; full.addRect(0, 0, W, H);
            if(!path.isEmpty() && inverted && !disabled && !soft) full = full.subtracted(path);
            path = full;
            needVecMask = soft;
        }
        if(path.isEmpty()) return {};
        QString defs, a;
        Val stroke; bool hasStroke = false, fillOn = true;
        if(L.blocks.contains("vstk")){
            stroke = descAt(L.blocks["vstk"], 4);
            hasStroke = stroke.n("strokeEnabled", 1) != 0;
            fillOn = stroke.n("fillEnabled", 1) != 0;
            // Measured: a path that opens by subtracting or intersecting shows no stroke of its own.
            if(firstOp == 2 || firstOp == 3) hasStroke = false;
        }
        if(solidProbe) a = QStringLiteral(" fill=\"#000000\"");
        else if(!fillOn || !fs.ok) a = QStringLiteral(" fill=\"none\"");
        else { a = fillAttrs(fs, path, defs); if(a.isEmpty()) return {}; }
        const double fop = L.fillOpacity / 255.0;
        if(fop < 0.999 && !solidProbe) a += QStringLiteral(" fill-opacity=\"%1\"").arg(fn(fop));
        QString body = QStringLiteral("<path d=\"%1\" fill-rule=\"%2\"%3/>").arg(pathD(path), path.fillRule() == Qt::OddEvenFill ? QStringLiteral("evenodd") : QStringLiteral("nonzero"), a);
        if(hasStroke){
            const double w = qMax(0.0, stroke.n("strokeStyleLineWidth", 1.0));
            QColor sc(Qt::black);
            if(const Val* ct = stroke.get("strokeStyleContent")){
                const QVector<GradStop> gs = gradStops(ct->get("Grad"), false);
                sc = gs.isEmpty() ? colorOf(ct->get("Clr")) : gs[gs.size() / 2].col;    // the model's stroke is one flat colour
            }
            const double opc = qBound(0.0, stroke.n("strokeStyleOpacity", 100.0) / 100.0, 1.0);
            const QString al = stroke.s("strokeStyleLineAlignment"), cap = stroke.s("strokeStyleLineCapType"), join = stroke.s("strokeStyleLineJoinType");
            QStringList dashes;
            if(const Val* ds = stroke.get("strokeStyleLineDashSet")) for(const Val& v : ds->list) dashes << fn(v.num * w);
            bool closed = true;
            for(int i = 0; i < path.elementCount() && closed; ++i) if(i + 1 == path.elementCount()) closed = path.currentPosition() == QPointF(path.elementAt(0));
            if(al.contains(QLatin1String("Center")) || !dashes.isEmpty() || !closed){
                QString sa = QStringLiteral(" fill=\"none\" stroke=\"%1\" stroke-width=\"%2\" stroke-miterlimit=\"%3\"").arg(hx(sc), fn(w), fn(qBound(1.0, stroke.n("strokeStyleMiterLimit", 4.0), 1000.0)));
                if(opc < 0.999) sa += QStringLiteral(" stroke-opacity=\"%1\"").arg(fn(opc));
                sa += cap.contains(QLatin1String("Round")) ? QStringLiteral(" stroke-linecap=\"round\"") : cap.contains(QLatin1String("Square")) ? QStringLiteral(" stroke-linecap=\"square\"") : QStringLiteral(" stroke-linecap=\"butt\"");
                sa += join.contains(QLatin1String("Round")) ? QStringLiteral(" stroke-linejoin=\"round\"") : join.contains(QLatin1String("Bevel")) ? QStringLiteral(" stroke-linejoin=\"bevel\"") : QStringLiteral(" stroke-linejoin=\"miter\"");
                if(!dashes.isEmpty()) sa += QStringLiteral(" stroke-dasharray=\"%1\"").arg(dashes.join(QLatin1Char(' ')));
                body += QStringLiteral("<path d=\"%1\"%2/>").arg(pathD(path), sa);
            } else {
                // Inside or outside: the half that shows, as geometry.
                QPainterPathStroker st;
                st.setWidth(2.0 * w);
                st.setCapStyle(Qt::FlatCap);
                st.setJoinStyle(join.contains(QLatin1String("Round")) ? Qt::RoundJoin : join.contains(QLatin1String("Bevel")) ? Qt::BevelJoin : Qt::MiterJoin);
                st.setMiterLimit(qBound(1.0, stroke.n("strokeStyleMiterLimit", 4.0), 1000.0));
                QPainterPath solid = path; solid.setFillRule(Qt::WindingFill);
                QPainterPath ring = st.createStroke(path); ring.setFillRule(Qt::WindingFill);
                const QPainterPath half = al.contains(QLatin1String("Inside")) ? ring.intersected(solid) : ring.subtracted(solid);
                body += QStringLiteral("<path d=\"%1\" fill-rule=\"evenodd\" fill=\"%2\"%3/>").arg(pathD(half), hx(sc), opc < 0.999 ? QStringLiteral(" fill-opacity=\"%1\"").arg(fn(opc)) : QString());
            }
        }
        handled = true;
        return fromSvg(QStringLiteral("<defs>%1</defs>%2").arg(defs, body));
    }

    // Combined soft masking for anything that can't be expressed as a plain path: a vector mask with density
    // or feather is rasterised onto the raster mask. Null when neither applies.
    QImage softMaskGray(const Lay& L) const {
        const bool hasRaster = !L.mask.isEmpty() && !L.maskOff && L.maskRect.isValid() && qint64(L.maskRect.width()) * L.maskRect.height() <= L.mask.size();
        Span vm = L.blocks.value("vmsk"); if(!vm.len) vm = L.blocks.value("vsms");
        bool off = false, inv = false;
        QPainterPath path = vm.len ? readVectorPath(r, vm, W, H, &off, &inv) : QPainterPath();
        const bool softVec = !path.isEmpty() && !off && (L.vecDensity < 0.999 || L.vecFeather > 0.0);
        if(!softVec && !(hasRaster && L.maskFeather > 0.0)) return hasRaster ? maskGray(L) : QImage();
        QVector<float> m(qint64(W) * H, 1.f);
        if(hasRaster){
            const QImage g = maskGray(L);
            for(int y = 0; y < H; ++y){ const QRgb* row = reinterpret_cast<const QRgb*>(g.constScanLine(y)); for(int x = 0; x < W; ++x) m[qint64(y)*W+x] = qRed(row[x]) / 255.f; }
            if(L.maskFeather > 0.0) m = blurPlane(m, W, H, L.maskFeather / 2.0);
        }
        if(softVec){
            QImage cov(W, H, QImage::Format_Grayscale8); cov.fill(0);
            { QPainter g(&cov); g.setRenderHint(QPainter::Antialiasing); g.setPen(Qt::NoPen); g.setBrush(Qt::white); if(inv){ QPainterPath full; full.addRect(0, 0, W, H); path = full.subtracted(path); } g.drawPath(path); }
            QVector<float> v(qint64(W) * H);
            for(int y = 0; y < H; ++y){ const uchar* row = cov.constScanLine(y); for(int x = 0; x < W; ++x) v[qint64(y)*W+x] = row[x] / 255.f; }
            if(L.vecFeather > 0.0) v = blurPlane(v, W, H, L.vecFeather / 2.0);
            const float d = float(L.vecDensity);
            for(qint64 i = 0; i < v.size(); ++i) m[i] *= 1.f - d * (1.f - v[i]);
        }
        QImage out(W, H, QImage::Format_ARGB32);
        for(int y = 0; y < H; ++y){ QRgb* row = reinterpret_cast<QRgb*>(out.scanLine(y)); for(int x = 0; x < W; ++x){ const int k = qBound(0, int(m[qint64(y)*W+x] * 255.f + 0.5f), 255); row[x] = qRgb(k, k, k); } }
        return out;
    }

    // Mask planes on the layer's own pixels; alpha outside the mask rect takes its default.
    static void bakeMask(QImage& img, int ox, int oy, const Lay& L){
        if(L.mask.isEmpty() || L.maskOff || !L.maskRect.isValid()) return;
        const QRect rc = L.maskRect;
        const qint64 need = qint64(rc.width()) * rc.height();
        if(L.mask.size() < need) return;
        const double dens = L.maskDensity;
        for(int y = 0; y < img.height(); ++y){
            QRgb* row = reinterpret_cast<QRgb*>(img.scanLine(y));
            const int my = y + oy - rc.top();
            for(int x = 0; x < img.width(); ++x){
                const int mx = x + ox - rc.left();
                int m = (mx >= 0 && my >= 0 && mx < rc.width() && my < rc.height()) ? quint8(L.mask[qint64(my) * rc.width() + mx]) : L.maskDefault;
                if(dens < 1.0) m = 255 - int((255 - m) * dens);
                row[x] = (row[x] & 0xffffffu) | (uint(qAlpha(row[x]) * m / 255) << 24);
            }
        }
    }
    QImage maskGray(const Lay& L) const {
        QImage g(W, H, QImage::Format_ARGB32);
        if(g.isNull()) return g;
        const QRect rc = L.maskRect;
        for(int y = 0; y < H; ++y){
            QRgb* row = reinterpret_cast<QRgb*>(g.scanLine(y));
            const int my = y - rc.top();
            for(int x = 0; x < W; ++x){
                const int mx = x - rc.left();
                int m = (mx >= 0 && my >= 0 && mx < rc.width() && my < rc.height()) ? quint8(L.mask[qint64(my) * rc.width() + mx]) : L.maskDefault;
                if(L.maskDensity < 1.0) m = 255 - int((255 - m) * L.maskDensity);
                row[x] = qRgb(m, m, m);
            }
        }
        return g;
    }

    // One leaf as native objects, empty if nothing is drawable.
    QVector<Shape> leaf(Lay& L) const {
        QVector<Shape> out;
        bool handled = false, vecMaskPending = false;
        QVector<Shape> vec = vectorLayer(L, handled, vecMaskPending);
        const bool hasMask = !L.mask.isEmpty() && !L.maskOff && L.maskRect.isValid() && qint64(L.maskRect.width()) * L.maskRect.height() <= L.mask.size();
        if(handled){
            out = vec;
            if((hasMask || vecMaskPending) && !out.isEmpty()){
                Shape g; g.type = ShapeType::Group; g.children = out; g.isolate = true;
                const QImage m = softMaskGray(L);
                if(!m.isNull()) setDocMask(g, m, QRect(0, 0, W, H));
                out = { g };
            }
            if(stats) ++stats->vectorLayers;
        } else if(L.blocks.contains("TySh")){
            const QString svg = textSvg(r, L.blocks["TySh"]);
            if(!svg.isEmpty()){
                out = fromSvg(svg);
                if(!out.isEmpty() && stats) ++stats->textLayers;
            } else if(stats) ++stats->rasterText;
        }
        if(out.isEmpty() && !L.pix.isNull()){
            QImage img = L.pix;
            Span vm = L.blocks.value("vmsk"); if(!vm.len) vm = L.blocks.value("vsms");
            bool disabled = false, inv = false;
            QPainterPath p = vm.len ? readVectorPath(r, vm, W, H, &disabled, &inv) : QPainterPath();
            const bool softVec = !p.isEmpty() && !disabled && (L.vecDensity < 0.999 || L.vecFeather > 0.0);
            if(softVec || (hasMask && L.maskFeather > 0.0)){
                const QImage m = softMaskGray(L);
                if(!m.isNull()) multiplyAlpha2(img, L.l, L.t, m);
            } else bakeMask(img, L.l, L.t, L);
            if(L.fillOpacity < 255){
                for(int y = 0; y < img.height(); ++y){ QRgb* row = reinterpret_cast<QRgb*>(img.scanLine(y)); for(int x = 0; x < img.width(); ++x) row[x] = (row[x] & 0xffffffu) | (uint(qAlpha(row[x]) * L.fillOpacity / 255) << 24); }
            }
            Shape s = bitmapShape(img, L.l, L.t, L.name);
            if(!L.native.isNull()) s.fill.origImage = L.native;
            if(!p.isEmpty() && !disabled && !softVec){
                if(inv){ QPainterPath full; full.addRect(0, 0, W, H); p = full.subtracted(p); }
                s.regionPath = QTransform::fromTranslate(-L.l, -L.t).map(p);
            }
            out = { s };
        }
        for(Shape& s : out){ if(s.name.isEmpty() || out.size() == 1) s.name = L.name; s.visible = L.visible; }
        return out;
    }
    // Pixels against a page-sized luminance mask.
    void multiplyAlpha2(QImage& img, int ox, int oy, const QImage& gray) const {
        for(int y = 0; y < img.height(); ++y){
            QRgb* row = reinterpret_cast<QRgb*>(img.scanLine(y));
            const int gy = y + oy;
            const QRgb* m = (gy >= 0 && gy < H) ? reinterpret_cast<const QRgb*>(gray.constScanLine(gy)) : nullptr;
            for(int x = 0; x < img.width(); ++x){
                const int gx = x + ox;
                const int k = (m && gx >= 0 && gx < W) ? qRed(m[gx]) : L_outside;
                row[x] = (row[x] & 0xffffffu) | (uint(qAlpha(row[x]) * k / 255) << 24);
            }
        }
    }
    static constexpr int L_outside = 0;
};

const Val* effectsOf(const Rd& r, const Lay& L, Val& storage){
    // Three keys, by age; lmfx is the one that can hold several of an effect.
    Span s = L.blocks.value("lmfx");
    if(!s.len) s = L.blocks.value("lfxs");
    if(!s.len) s = L.blocks.value("lfx2");
    if(s.len < 8) return nullptr;
    DescReader d(r.d, s.o + 8, s.o + s.len);
    storage = d.desc(0);
    return storage.kind == Val::Obj ? &storage : nullptr;
}

struct Scope { QVector<Shape> shapes; int baseIndex = -1; bool passThrough = false, baseIsAdjustment = false; const Lay* folder = nullptr; };

} // namespace

// ---------------------------------------------------------------- entry points
QImage importPsd(const QByteArray& d){
    if(d.size() < 26 || d.left(4) != "8BPS") return {};
    Rd r(d);
    const int version = r.u16(4);
    if(version != 1 && version != 2) return {};
    const bool psb = version == 2;
    const int channels = r.u16(12), H = int(r.u32(14)), W = int(r.u32(18)), depth = r.u16(22), mode = r.u16(24);
    const int maxDim = psb ? 300000 : 30000; const qint64 maxPix = psb ? kMaxPsbPixels : kMaxPsdPixels;
    if((depth != 1 && depth != 8 && depth != 16 && depth != 32) || W <= 0 || H <= 0 || W > maxDim || H > maxDim || qint64(W) * H > maxPix || channels < 1 || channels > 56) return {};
    ColorInfo ci; ci.mode = mode;
    qint64 o = 26;
    { const qint64 l = r.u32(o); if(l < 0 || l > r.N - o - 4) return {}; ci.table = d.mid(o + 4, l); o += 4 + l; }
    { const qint64 l = r.u32(o); if(l < 0 || l > r.N - o - 4) return {}; if(depth != 32) ci.icc = documentProfile(r, o + 4, o + 4 + l, mode); o += 4 + l; }
    int rawCount = 0;
    { const int sz = psb ? 8 : 4; const qint64 l = psb ? r.u64(o) : r.u32(o); if(l < 0 || l > r.N - o - sz) return {};
      if(l > sz){ const qint64 li = psb ? r.u64(o + sz) : r.u32(o + sz); if(li > 0) rawCount = qint16(r.u16(o + sz + sz)); }
      o += sz + l; }
    if(o + 2 > r.N) return {};
    const int comp = r.u16(o); o += 2;
    const int use = qMin(channels, 5);
    const qint64 rowBytes = depth == 1 ? (qint64(W) + 7) / 8 : qint64(W) * (depth / 8);
    QHash<int, QByteArray> planes;
    if(comp == 0){
        for(int c = 0; c < use; ++c){ if(o + rowBytes * H > r.N) return {}; planes[c] = decodePlane(r, o, o + rowBytes * H, 0, W, H, depth, psb); o += rowBytes * H; }
    } else if(comp == 1){
        const int cs = psb ? 4 : 2;
        const qint64 tableBytes = qint64(cs) * H * channels;
        if(o + tableBytes > r.N) return {};
        qint64 p = o + tableBytes;
        for(int c = 0; c < use; ++c){
            QByteArray raw(rowBytes * H, 0);
            for(int y = 0; y < H; ++y){
                const qint64 rc = psb ? r.u32(o + cs * (qint64(c) * H + y)) : r.u16(o + cs * (qint64(c) * H + y));
                const qint64 rowEnd = p + rc;
                if(rowEnd > r.N) return {};
                char* out = raw.data() + rowBytes * y; qint64 col = 0;
                while(p < rowEnd && col < rowBytes){
                    const qint8 n = qint8(d[p++]);
                    if(n >= 0){ const qint64 k = qMin<qint64>(n + 1, qMin(rowBytes - col, rowEnd - p)); if(k > 0){ memcpy(out + col, d.constData() + p, size_t(k)); col += k; } p += n + 1; }
                    else if(n != -128 && p < rowEnd){ const qint64 k = qMin<qint64>(1 - n, rowBytes - col); memset(out + col, d[p++], size_t(k)); col += k; }
                }
                p = rowEnd;
            }
            QByteArray pix(qint64(W) * H, 0);
            if(depth == 8) pix = raw;
            else {
                uchar* q = reinterpret_cast<uchar*>(pix.data()); const uchar* b = reinterpret_cast<const uchar*>(raw.constData());
                if(depth == 16) for(qint64 i = 0; i < pix.size(); ++i) q[i] = b[i*2];
                else if(depth == 1) for(int y = 0; y < H; ++y) for(int x = 0; x < W; ++x) q[qint64(y)*W+x] = (b[rowBytes*y + x/8] >> (7 - x%8)) & 1 ? 0 : 255;
                else for(qint64 i = 0; i < pix.size(); ++i){ quint32 v = (quint32(b[i*4]) << 24) | (b[i*4+1] << 16) | (b[i*4+2] << 8) | b[i*4+3]; float f; memcpy(&f, &v, 4); q[i] = uchar(qBound(0, int(255.0 * (std::isfinite(f) ? double(f) : 0.0) + 0.5), 255)); }
            }
            planes[c] = pix;
        }
    } else return {};
    const int base = baseChannels(mode);
    if(depth == 32) for(int c = 0; c < qMin(base, use); ++c) gammaPlane(planes[c]);
    // An extra channel is transparency only when the file says so (a negative layer count) or has no layers
    // to composite; otherwise it is a saved selection or a spot plate, which must not punch holes.
    QByteArray alpha;
    if(channels > base && use > base && rawCount <= 0) alpha = planes[base];
    if(!(mode == 7 && channels >= 3)) for(int c = base; c < use; ++c) planes.remove(c);
    return planesToImage(ci, W, H, planes, alpha.isEmpty() ? nullptr : &alpha);
}

Shape psdBitmapShape(const QImage& img, int x, int y, const QString& name){ return bitmapShape(img, x, y, name); }
void psdApplyBlend(Shape& s, const QByteArray& key){ applyBlend(s, key); }

Document importPsdDocument(const QByteArray& d, PsdImportStats* stats){
    Document doc;
    if(d.size() < 26 || d.left(4) != "8BPS") return doc;
    Rd r(d);
    const int version = r.u16(4);
    if(version != 1 && version != 2) return doc;
    const bool psb = version == 2;
    const int H = int(r.u32(14)), W = int(r.u32(18)), depth = r.u16(22), channels = r.u16(12);
    ColorInfo ci; ci.mode = r.u16(24);
    const int maxDim = psb ? 300000 : 30000; const qint64 maxPix = psb ? kMaxPsbPixels : kMaxPsdPixels;
    if((depth != 1 && depth != 8 && depth != 16 && depth != 32) || W <= 0 || H <= 0 || W > maxDim || H > maxDim || qint64(W) * H > maxPix || channels < 1 || channels > 56) return doc;
    doc.pageSize = QSizeF(W, H);

    qint64 o = 26;
    { const qint64 l = r.u32(o); if(l < 0 || l > r.N - o - 4) return doc; ci.table = d.mid(o + 4, l); o += 4 + l; }
    double globalAngle = 120.0;
    { const qint64 l = r.u32(o); if(l < 0 || l > r.N - o - 4) return doc;
      if(depth != 32) ci.icc = documentProfile(r, o + 4, o + 4 + l, ci.mode);
      qint64 p = o + 4; const qint64 end = p + l;
      while(p + 12 <= end && d.mid(p, 4) == "8BIM"){
          const int id = r.u16(p + 4);
          const int nl = r.u8(p + 6);
          qint64 q = p + 6 + ((nl + 2) / 2) * 2;
          const qint64 sz = r.u32(q); q += 4;
          if(sz < 0 || sz > end - q) break;
          if(id == 1037 && sz >= 4) globalAngle = r.i32(q);
          if(id == 1032 && stats && sz >= 16){
              const qint64 n = r.u32(q + 12);
              for(qint64 i = 0; i < n && i < 4096 && q + 16 + (i + 1) * 5 <= q + sz; ++i){
                  const double at = r.i32(q + 16 + i * 5) / 32.0;
                  if(r.u8(q + 20 + i * 5) == 1) stats->guidesH.push_back(at); else stats->guidesV.push_back(at);
              }
          }
          p = q + sz + (sz & 1);
      }
      o += 4 + l; }

    g_linearDoc = depth == 32;
    g_docColor = &ci;
    PatternMap patterns;
    QVector<Lay> lays;
    int rawCount = 0;
    const int lmSz = psb ? 8 : 4;
    if(o + lmSz <= r.N){
        const qint64 lmLen = psb ? r.u64(o) : r.u32(o);
        const qint64 lmEnd = (lmLen < 0 || lmLen > r.N - o - lmSz) ? r.N : o + lmSz + lmLen;
        qint64 q = o + lmSz;
        qint64 afterInfo = q;
        if(lmLen > 0 && q + lmSz <= lmEnd){
            const qint64 li = psb ? r.u64(q) : r.u32(q);
            q += lmSz;
            if(li > 0 && li <= lmEnd - q){
                readLayerInfo(r, q, q + li, psb, depth, ci, maxDim, maxPix, rawCount, lays);
                afterInfo = q + li + (li & 1);
            } else afterInfo = q;
            // Global mask info, then blocks: deep images keep their layers in Lr16 / Lr32 here.
            const qint64 gl = r.u32(afterInfo);
            qint64 b = afterInfo + 4 + qMax<qint64>(0, gl);
            QHash<QByteArray, Span> global;
            if(b < lmEnd) readBlocks(r, b, lmEnd, psb, 4, global);
            for(const char* k : { "Patt", "Pat2", "Pat3" }) if(global.contains(k)) readPatterns(r, global[k], patterns);
            if(lays.isEmpty()){
                Span deep = global.value("Lr16"); if(!deep.len) deep = global.value("Lr32"); if(!deep.len) deep = global.value("Layr");
                if(deep.len) readLayerInfo(r, deep.o, deep.o + deep.len, psb, depth, ci, maxDim, maxPix, rawCount, lays);
            }
        }
    }

    Builder B{ r, W, H, globalAngle, stats, patterns };
    QVector<Scope> stack(1);

    auto addClipped = [&](QVector<Shape>& list, Shape s, int baseIdx){
        if(baseIdx < 0 || baseIdx >= list.size()) return s;
        // Clipped to the base's coverage: baked for pixels, a live mask for anything else.
        const QRect area(0, 0, W, H);
        const QImage cov = coverageOf(list[baseIdx], area);
        if(cov.isNull()) return s;
        if(isPixelShape(s)){
            QImage img = s.fill.image.convertToFormat(QImage::Format_ARGB32);
            multiplyAlpha(img, int(s.transform.dx()), int(s.transform.dy()), cov, area);
            const bool native = s.fill.origImage.size() == img.size() && s.fill.origImage.cacheKey() != s.fill.image.cacheKey();
            s.fill.image = img; if(!native) s.fill.origImage = img;
        } else {
            QImage gray(W, H, QImage::Format_ARGB32);
            for(int y = 0; y < H; ++y){ const QRgb* c = reinterpret_cast<const QRgb*>(cov.constScanLine(y)); QRgb* g = reinterpret_cast<QRgb*>(gray.scanLine(y)); for(int x = 0; x < W; ++x){ const int a = qAlpha(c[x]); g[x] = qRgb(a, a, a); } }
            if(s.type == ShapeType::Group) s.isolate = true;
            setDocMask(s, gray, area);
        }
        return s;
    };

    // A deep knockout stops at a true background layer: the bottom one, with no transparency of its own.
    const bool bottomIsBackground = !lays.isEmpty() && std::none_of(lays[0].chans.begin(), lays[0].chans.end(), [](const LChan& c){ return c.id == -1; });
    // Takes a pixel layer's layer effects, and returns the group holding it and them.
    auto withEffects = [&](Shape layer, const Lay& L)->Shape {
        Val fxStore;
        const Val* fx = effectsOf(r, L, fxStore);
        if(!fx) return layer;
        const QRectF bb = layer.docBounds();
        const int margin = 64;
        const QRect area = bb.toAlignedRect().adjusted(-margin, -margin, margin, margin).intersected(QRect(-margin, -margin, W + margin * 2, H + margin * 2));
        if(area.isEmpty() || qint64(area.width()) * area.height() > 40000000) return layer;
        // Effects see the layer's outline: not its fill opacity or how its fill is painted, nor a mask on a shape.
        Shape probe = layer;
        bool h = false, pend = false;
        const QVector<Shape> solid = B.vectorLayer(L, h, pend, true);
        if(h && !solid.isEmpty()){
            if(solid.size() == 1) probe = solid[0];
            else { probe = Shape(); probe.type = ShapeType::Group; probe.children = solid; }
        } else if(L.fillOpacity < 255 && !L.pix.isNull()){
            Lay full = L; full.fillOpacity = 255;
            Builder quiet = B; quiet.stats = nullptr;
            const QVector<Shape> again = quiet.leaf(full);
            if(again.size() == 1) probe = again[0];
            else if(!again.isEmpty()){ probe = Shape(); probe.type = ShapeType::Group; probe.children = again; }
        }
        probe.opacity = 1.0; probe.blend = QPainter::CompositionMode_SourceOver; probe.transp = Transparency(); probe.visible = true;
        FxOut fo;
        buildEffects(*fx, globalAngle, coverageOf(probe, area), area.topLeft(), bb.toAlignedRect(), L.name, B.patterns, fo);
        // "Blend interior effects as group": then fill opacity does reach the interior ones.
        if(L.fillOpacity < 255 && L.blocks.contains("infx") && r.u8(L.blocks["infx"].o) != 0)
            for(Shape& os : fo.over) if(!os.name.endsWith(QLatin1String("(stroke)"))) os.opacity *= L.fillOpacity / 255.0;
        if(fo.under.isEmpty() && fo.over.isEmpty()) return layer;
        Shape g; g.type = ShapeType::Group; g.name = L.name; g.visible = L.visible;
        g.opacity = layer.opacity; layer.opacity = 1.0;
        g.isolate = g.opacity < 0.999;
        g.children = fo.under; layer.visible = true; g.children.push_back(layer); g.children += fo.over;
        if(stats) ++stats->effectLayers;
        return g;
    };

    // Knockout: the layer punches through what lies beneath it, to the bottom of its group (shallow) or down to
    // the background (deep), and its fill opacity then says how much of itself remains.
    auto applyKnockout = [&](const Shape& layer, const Lay& L){
        const int knock = L.blocks.contains("knko") ? r.u8(L.blocks["knko"].o) : 0;
        if(knock != 1 && knock != 2) return;
        Shape flat = layer; flat.opacity = 1.0; flat.visible = true; flat.blend = QPainter::CompositionMode_SourceOver; flat.transp = Transparency(); flat.isolate = false;
        bool h = false, pend = false;
        const QVector<Shape> solid = B.vectorLayer(L, h, pend, true);
        if(h && !solid.isEmpty()){ flat = solid[0]; if(solid.size() > 1){ flat = Shape(); flat.type = ShapeType::Group; flat.children = solid; } }
        const QRect area(0, 0, W, H);
        const QImage cov = coverageOf(flat, area);
        if(cov.isNull()) return;
        if(knock == 1) cutUnder(stack.last().shapes, cov, area, false);
        else for(int i = 0; i < stack.size(); ++i) cutUnder(stack[i].shapes, cov, area, i == 0 && bottomIsBackground);
    };

    // Records run bottom layer first: a divider opens a group, the folder record closes it.
    for(int li = 0; li < lays.size(); ++li){
        Lay& L = lays[li];
        if(L.section == 3){
            if(stack.size() < kMaxDepth){
                stack.push_back(Scope());
                // The folder record holding the group's blend mode comes after its contents, yet an adjustment
                // inside needs to know now whether it reaches past the group. Look ahead for it.
                int level = 1;
                for(int k = li + 1; k < lays.size() && level > 0; ++k){
                    if(lays[k].section == 3) ++level;
                    else if(lays[k].section == 1 || lays[k].section == 2){
                        if(--level == 0){ const QByteArray bm = lays[k].sectBlend.isEmpty() ? lays[k].blend : lays[k].sectBlend; stack.last().passThrough = bm == "pass" && lays[k].fillOpacity == 255; stack.last().folder = &lays[k]; }
                    }
                }
            }
            continue;
        }
        if(L.section == 1 || L.section == 2){
            if(stack.size() < 2) continue;                     // a stray folder record in a broken file
            Scope sc = stack.takeLast();
            const QByteArray bm = L.sectBlend.isEmpty() ? L.blend : L.sectBlend;
            // Fill below 100 % stops a group passing through: Photoshop composites it as a unit.
            const bool pass = bm == "pass" && L.fillOpacity == 255;
            const bool board = L.blocks.contains("artb") || L.blocks.contains("artd") || L.blocks.contains("abdd");
            if(sc.shapes.isEmpty() && !board) continue;
            Shape g; g.type = ShapeType::Group; g.name = L.name; g.children = sc.shapes; g.visible = L.visible;
            g.opacity = L.opacity / 255.0 * L.fillOpacity / 255.0;
            // A pass-through group's opacity simply reaches each child; only a real group is composited whole.
            if(!pass){ applyBlend(g, bm == "pass" ? QByteArrayLiteral("norm") : bm); if((bm != "norm" && bm != "pass") || g.opacity < 0.999) g.isolate = true; }
            if(L.blocks.contains("artb") || L.blocks.contains("artd") || L.blocks.contains("abdd")){
                Span ab = L.blocks.value("artb"); if(!ab.len) ab = L.blocks.value("artd"); if(!ab.len) ab = L.blocks.value("abdd");
                const Val v = B.descAt(ab, 4);
                const Val* rc = v.get("artboardRect");
                const int bt = int(v.n("artboardBackgroundType", 3));
                if(rc && bt != 3){
                    const QRectF rr(QPointF(rc->n("Left"), rc->n("Top")), QPointF(rc->n("Rght"), rc->n("Btom")));
                    QPainterPath clipTo; clipTo.addRect(rr);
                    g.regionPath = clipTo; g.isolate = !g.children.isEmpty();
                    const QColor bc = bt == 2 ? QColor(Qt::black) : bt == 1 ? QColor(Qt::white) : colorOf(v.get("Clr"));
                    QPainterPath pp; pp.addRect(rr);
                    QVector<Shape> bg = B.fromSvg(QStringLiteral("<path d=\"%1\" fill=\"%2\"/>").arg(pathD(pp), hx(bc)));
                    for(Shape& b : bg) b.name = QStringLiteral("%1 background").arg(L.name);
                    g.children = bg + g.children;
                }
            }
            { const QImage m = B.softMaskGray(L); if(!m.isNull()){ g.isolate = true; setDocMask(g, m, QRect(0, 0, W, H)); } }
            Span vm = L.blocks.value("vmsk"); if(!vm.len) vm = L.blocks.value("vsms");
            if(vm.len){ bool off = false, inv = false; QPainterPath p = readVectorPath(r, vm, W, H, &off, &inv); if(!off && !inv && !p.isEmpty() && L.vecDensity >= 0.999 && L.vecFeather <= 0.0){ g.regionPath = p; g.isolate = true; } }
            Scope& parent = stack.last();
            if(g.children.isEmpty()) continue;
            g = withEffects(g, L);
            applyKnockout(g, L);
            if(L.clipping && parent.baseIndex >= 0) parent.shapes.push_back(addClipped(parent.shapes, g, parent.baseIndex));
            else { parent.shapes.push_back(g); parent.baseIndex = parent.shapes.size() - 1; parent.baseIsAdjustment = false; }
            if(stats) ++stats->groups;
            continue;
        }
        // Adjustment layers work on everything beneath them, not on pixels of their own.
        Adjustment fn;
        if(adjustmentOf(r, L, ci.mode, fn)){
            if(!fn.fn){ if(stats) ++stats->skippedAdjustments; continue; }
            if(!L.visible) continue;
            auto toGray8 = [&](const QImage& g){
                QImage m(W, H, QImage::Format_Grayscale8);
                for(int y = 0; y < H; ++y){ const QRgb* src = reinterpret_cast<const QRgb*>(g.constScanLine(y)); uchar* q = m.scanLine(y); for(int x = 0; x < W; ++x) q[x] = uchar(qRed(src[x])); }
                return m;
            };
            QImage mask8; const QRect marea(0, 0, W, H);
            { const QImage own = B.softMaskGray(L); if(!own.isNull()) mask8 = toGray8(own); }
            double strength = L.opacity / 255.0 * L.fillOpacity / 255.0;
            // Clipped onto another adjustment, it reaches whatever that one does.
            if(!L.clipping) stack.last().baseIsAdjustment = true;
            if(L.clipping && stack.last().baseIndex >= 0 && !stack.last().baseIsAdjustment){
                QVector<Shape> one{ stack.last().shapes[stack.last().baseIndex] };
                adjustShapes(one, fn, mask8.isNull() ? nullptr : &mask8, marea, strength);
                stack.last().shapes[stack.last().baseIndex] = one[0];
            } else {
                // A pass-through group lets it reach the layers below the group, through the group's own
                // opacity and mask.
                for(int i = stack.size() - 1; i >= 0; --i){
                    adjustShapes(stack[i].shapes, fn, mask8.isNull() ? nullptr : &mask8, marea, strength);
                    if(!stack[i].passThrough) break;
                    if(const Lay* f = stack[i].folder){
                        strength *= f->opacity / 255.0 * f->fillOpacity / 255.0;
                        const QImage fm = B.softMaskGray(*f);
                        if(!fm.isNull()){
                            const QImage g8 = toGray8(fm);
                            if(mask8.isNull()) mask8 = g8;
                            else for(int y = 0; y < H; ++y){ uchar* q = mask8.scanLine(y); const uchar* v = g8.constScanLine(y); for(int x = 0; x < W; ++x) q[x] = uchar(q[x] * v[x] / 255); }
                        }
                    }
                }
            }
            if(stats) ++stats->adjustments;
            continue;
        }

        QVector<Shape> made = B.leaf(L);
        if(made.isEmpty()) continue;
        Shape layer;
        if(made.size() == 1) layer = made[0];
        else { layer.type = ShapeType::Group; layer.name = L.name; layer.children = made; layer.visible = L.visible; }
        layer.opacity = L.opacity / 255.0;
        applyBlend(layer, L.blend);
        if(layer.type == ShapeType::Group && (layer.opacity < 0.999 || L.blend != "norm")) layer.isolate = true;

        layer = withEffects(layer, L);

        // Blend If and Dissolve decide pixel by pixel, so such a layer becomes pixels.
        const bool blendIf = L.blendIf[0] != 0 || L.blendIf[1] != 0 || L.blendIf[2] != 255 || L.blendIf[3] != 255 || L.blendIf[4] != 0 || L.blendIf[5] != 0 || L.blendIf[6] != 255 || L.blendIf[7] != 255;
        if(blendIf || L.blend == "diss"){
            const QRect area = layer.docBounds().toAlignedRect().intersected(QRect(-W, -H, W * 3, H * 3));
            if(!area.isEmpty() && qint64(area.width()) * area.height() <= 40000000){
                const bool diss = L.blend == "diss";
                Shape flat = layer; flat.visible = true; flat.blend = QPainter::CompositionMode_SourceOver; flat.transp = Transparency();
                if(!diss) flat.opacity = 1.0;
                QImage img = coverageOf(flat, area).convertToFormat(QImage::Format_ARGB32);
                QImage under;
                if(blendIf){
                    under = QImage(area.size(), QImage::Format_ARGB32_Premultiplied); under.fill(Qt::white);
                    QPainter g(&under); g.setRenderHint(QPainter::Antialiasing); g.translate(-area.topLeft());
                    for(const Scope& sc : stack) for(const Shape& below : sc.shapes) paintShape(g, below);
                }
                auto ramp = [](int v, const int* q){
                    if(v < q[0]) return 0.0;
                    if(v < q[1]) return double(v - q[0]) / qMax(1, q[1] - q[0]);
                    if(v <= q[2]) return 1.0;
                    if(v < q[3]) return double(q[3] - v) / qMax(1, q[3] - q[2]);
                    return 0.0;
                };
                for(int y = 0; y < img.height(); ++y){
                    QRgb* row = reinterpret_cast<QRgb*>(img.scanLine(y));
                    const QRgb* u = under.isNull() ? nullptr : reinterpret_cast<const QRgb*>(under.constScanLine(y));
                    for(int x = 0; x < img.width(); ++x){
                        double a = qAlpha(row[x]) / 255.0;
                        if(blendIf){
                            a *= ramp(qGray(row[x]), L.blendIf);
                            if(u) a *= ramp(qGray(u[x]), L.blendIf + 4);
                        }
                        // Photoshop's noise belongs to the pixel's position, so the holes of stacked layers coincide.
                        if(diss){ quint32 h = quint32(x + area.left()) * 73856093u ^ quint32(y + area.top()) * 19349663u; h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15; a = (h & 0xffff) / 65535.0 < a ? 1.0 : 0.0; }
                        row[x] = (row[x] & 0xffffffu) | (uint(a * 255.0 + 0.5) << 24);
                    }
                }
                Shape b = bitmapShape(img, area.left(), area.top(), layer.name);
                b.visible = layer.visible; b.opacity = diss ? 1.0 : layer.opacity;
                if(!diss){ b.blend = layer.blend; b.transp = layer.transp; }
                layer = b;
            }
        }

        applyKnockout(layer, L);
        Scope& top = stack.last();
        if(L.clipping && top.baseIndex >= 0) top.shapes.push_back(addClipped(top.shapes, layer, top.baseIndex));
        else { top.shapes.push_back(layer); top.baseIndex = top.shapes.size() - 1; top.baseIsAdjustment = false; }
    }
    // A file with an unclosed divider still shows its contents.
    while(stack.size() > 1){ Scope sc = stack.takeLast(); stack.last().shapes += sc.shapes; }
    doc.shapes = stack[0].shapes;
    dropNative(doc.shapes);
    g_docColor = nullptr;

    if(doc.shapes.isEmpty()){
        const QImage comp = importPsd(d);
        if(!comp.isNull()){ Shape s = bitmapShape(comp, 0, 0, QStringLiteral("PSD")); doc.shapes.push_back(s); }
    }
    return doc;
}

} // namespace xara
