// Paint.NET (.pdn), versions 3 to 5. A "PDN3" magic and a 3-byte little-endian length lead an XML header
// (size, layer count, a PNG thumbnail); then comes the document itself, Paint.NET's own classes as .NET's
// BinaryFormatter writes them (MS-NRBF), and after that each layer's pixels. The pixels are deferred out of
// the object graph: each memory block is written in the order the graph lists them, in numbered chunks that
// are gzip-compressed or raw. Pixels are 32-bit BGRA with straight alpha; layers are listed bottom first.
// Paint.NET 2 files are the same graph and pixels, gzip-compressed as one, with no header.
#include "prepress.h"
#include <QHash>
#include <QImage>
#include <cstring>
#include <zlib.h>

namespace xara {
namespace {

const qint64 kMaxPixels = qint64(1) << 28, kMaxTotalPixels = qint64(1) << 29;

// Just enough of MS-NRBF to walk an object graph: classes with their member names and values, strings,
// references and object arrays. Primitive arrays are skipped; nothing Paint.NET keeps in them is needed here.
struct Nrbf {
    struct Node { char kind = 'n'; qint64 i = 0; double d = 0; QString s, cls; QStringList names; QVector<int> vals; };
    struct Meta { QString cls; QStringList names; QByteArray types; QByteArray prims; };
    const uchar* p; qint64 n, o = 0; bool bad = false; int depth = 0; qint64 elements = 0;
    QVector<Node> nodes; QHash<qint32, int> ids; QHash<qint32, Meta> metas; qint32 root = 0; int nulls = 0;
    enum { End = -2, Nulls = -3 };

    Nrbf(const uchar* d, qint64 len) : p(d), n(len) {}
    bool need(qint64 k){ if(k < 0 || o + k > n){ bad = true; return false; } return true; }
    uchar u8(){ return need(1) ? p[o++] : 0; }
    qint32 i32(){ if(!need(4)) return 0; const quint32 v = quint32(p[o]) | quint32(p[o + 1]) << 8 | quint32(p[o + 2]) << 16 | quint32(p[o + 3]) << 24; o += 4; return qint32(v); }
    qint64 i64(){ const quint32 lo = quint32(i32()); const quint32 hi = quint32(i32()); return qint64(quint64(hi) << 32 | lo); }
    QString lps(){
        qint64 len = 0;
        for(int k = 0, sh = 0; k < 5; ++k, sh += 7){ const uchar c = u8(); len |= qint64(c & 0x7f) << sh; if(!(c & 0x80)) break; }
        if(!need(len)) return QString();
        const QString s = QString::fromUtf8(reinterpret_cast<const char*>(p + o), qsizetype(len)); o += len; return s;
    }
    int add(Node&& v){ if(nodes.size() >= (1 << 22)){ bad = true; return -1; } nodes.push_back(std::move(v)); return nodes.size() - 1; }
    void reg(qint32 id, int at){ if(at >= 0) ids.insert(id, at); }

    // A primitive value of type `t`; `keep` makes a node of it, otherwise it's only stepped over.
    int prim(int t, bool keep = true){
        Node v; v.kind = 'i';
        switch(t){
        case 1: case 2: v.i = u8(); break;
        case 10: v.i = qint8(u8()); break;
        case 3: { const uchar c = u8(); o += c < 0x80 ? 0 : c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3; need(0); v.i = c; break; }
        case 5: case 18: v.kind = 's'; v.s = lps(); break;
        case 6: { if(!need(8)) return -1; double d; std::memcpy(&d, p + o, 8); o += 8; v.kind = 'd'; v.d = d; break; }   // little-endian hosts
        case 11: { if(!need(4)) return -1; float f; std::memcpy(&f, p + o, 4); o += 4; v.kind = 'd'; v.d = f; break; }
        case 7: { if(!need(2)) return -1; v.i = qint16(p[o] | p[o + 1] << 8); o += 2; break; }
        case 14: { if(!need(2)) return -1; v.i = p[o] | p[o + 1] << 8; o += 2; break; }
        case 8: v.i = i32(); break;
        case 15: v.i = quint32(i32()); break;
        case 9: case 12: case 13: case 16: v.i = i64(); break;
        case 17: v.kind = 'n'; break;
        default: bad = true; return -1;
        }
        return keep && !bad ? add(std::move(v)) : -1;
    }
    bool primSize(int t, int* size){
        switch(t){ case 1: case 2: case 10: *size = 1; return true; case 7: case 14: *size = 2; return true;
                   case 8: case 11: case 15: *size = 4; return true; case 6: case 9: case 12: case 13: case 16: *size = 8; return true;
                   default: return false; }
    }
    void skipPrims(int t, qint64 count){
        int size = 0;
        if(primSize(t, &size)){ if(need(count * size)) o += count * size; return; }
        for(qint64 k = 0; k < count && !bad; ++k) prim(t, false);
    }
    // The extra type information a member's or array element's binary type carries.
    void typeExtra(int bt, QByteArray* prims){
        if(bt == 0 || bt == 7){ const uchar t = u8(); if(prims) prims->append(char(t)); return; }
        if(prims) prims->append(char(0));
        if(bt == 3) lps();
        else if(bt == 4){ lps(); i32(); }
        else if(bt > 7) bad = true;
    }
    Meta classInfo(qint32* oid){
        Meta m; *oid = i32(); m.cls = lps(); const qint32 count = i32();
        if(count < 0 || count > 4096 || !need(count)){ bad = true; return m; }
        for(int k = 0; k < count && !bad; ++k) m.names << lps();
        return m;
    }
    int object(qint32 oid, const Meta& m){
        Node v; v.kind = 'o'; v.cls = m.cls; v.names = m.names;
        const int at = add(std::move(v)); reg(oid, at);
        QVector<int> vals;
        for(int k = 0; k < m.names.size() && !bad; ++k){
            const int bt = k < m.types.size() ? uchar(m.types[k]) : 2;
            int r = bt == 0 ? prim(uchar(m.prims[k])) : record();
            if(r == End || r == Nulls){ bad = true; break; }
            vals << r;
        }
        if(at >= 0) nodes[at].vals = vals;
        return at;
    }
    void elementsOf(int at, qint64 count){
        QVector<int> vals;
        while(vals.size() < count && !bad){
            const int r = record();
            if(r == End){ bad = true; break; }
            if(r == Nulls){ if(nulls < 0 || nulls > count - vals.size()){ bad = true; break; } vals.insert(vals.size(), nulls, -1); }
            else vals << r;
        }
        if(at >= 0) nodes[at].vals = vals;
    }
    bool countOk(qint64 count){
        elements += count;
        if(count < 0 || count > (1 << 24) || elements > (qint64(1) << 25)){ bad = true; return false; }
        return true;
    }

    // One record; a value's node, -1 for null, End at the end of the stream, Nulls for a run of nulls.
    int record(){
        struct Guard { int& d; Guard(int& x) : d(x) { ++d; } ~Guard(){ --d; } } guard(depth);
        if(depth > 200){ bad = true; return -1; }
        while(!bad){
            const int t = u8();
            if(bad) return -1;
            switch(t){
            case 0: root = i32(); i32(); i32(); i32(); continue;              // stream header
            case 12: i32(); lps(); continue;                                  // library
            case 1: {                                                         // class, members as an earlier one's
                const qint32 oid = i32(), mid = i32();
                auto it = metas.find(mid);
                if(it == metas.end()){ bad = true; return -1; }
                const Meta m = it.value(); metas.insert(oid, m);
                return object(oid, m);
            }
            case 2: case 3: case 4: case 5: {                                 // class with its member names (and types)
                qint32 oid = 0; Meta m = classInfo(&oid);
                if(bad) return -1;
                if(t >= 4){
                    if(!need(m.names.size())) return -1;
                    m.types = QByteArray(reinterpret_cast<const char*>(p + o), m.names.size()); o += m.names.size();
                    for(int k = 0; k < m.types.size() && !bad; ++k) typeExtra(uchar(m.types[k]), &m.prims);
                } else { m.types = QByteArray(m.names.size(), char(2)); m.prims = QByteArray(m.names.size(), char(0)); }
                if(t == 3 || t == 5) i32();                                   // library id
                if(bad) return -1;
                metas.insert(oid, m);
                return object(oid, m);
            }
            case 6: { const qint32 oid = i32(); Node v; v.kind = 's'; v.s = lps(); const int at = add(std::move(v)); reg(oid, at); return at; }
            case 7: {                                                         // array of any rank and element type
                const qint32 oid = i32(); const int kind = u8(); const qint32 rank = i32();
                if(rank < 1 || rank > 32){ bad = true; return -1; }
                qint64 count = 1;
                for(int k = 0; k < rank && !bad; ++k){ const qint32 l = i32(); if(l < 0){ bad = true; break; } count *= l; if(count > (1 << 24)){ bad = true; break; } }
                if(kind >= 3 && kind <= 5) for(int k = 0; k < rank; ++k) i32();
                const int bt = u8(); QByteArray et; typeExtra(bt, &et);
                if(bad || !countOk(count)) return -1;
                Node v; v.kind = 'a'; const int at = add(std::move(v)); reg(oid, at);
                if(bt == 0) skipPrims(uchar(et[0]), count); else elementsOf(at, count);
                return at;
            }
            case 8: { const int pt = u8(); return prim(pt); }
            case 9: { Node v; v.kind = 'r'; v.i = i32(); return add(std::move(v)); }
            case 10: return -1;
            case 11: return End;
            case 13: nulls = u8(); return Nulls;
            case 14: nulls = i32(); return Nulls;
            case 15: {                                                        // array of one primitive type
                const qint32 oid = i32(), count = i32(); const int pt = u8();
                if(!countOk(count)) return -1;
                Node v; v.kind = 'a'; const int at = add(std::move(v)); reg(oid, at);
                skipPrims(pt, count);
                return at;
            }
            case 16: case 17: {                                               // array of objects or strings
                const qint32 oid = i32(), count = i32();
                if(!countOk(count)) return -1;
                Node v; v.kind = 'a'; const int at = add(std::move(v)); reg(oid, at);
                elementsOf(at, count);
                return at;
            }
            default: bad = true; return -1;
            }
        }
        return -1;
    }
    bool parse(){
        while(!bad){ const int r = record(); if(r == End) return true; if(r == Nulls) bad = true; }
        return false;
    }

    int res(int v) const {
        for(int hop = 0; v >= 0 && v < nodes.size() && nodes[v].kind == 'r' && hop < 64; ++hop) v = ids.value(qint32(nodes[v].i), -1);
        return v >= 0 && v < nodes.size() && nodes[v].kind != 'r' ? v : -1;
    }
    int member(int obj, const QString& name) const {
        obj = res(obj);
        if(obj < 0 || nodes[obj].kind != 'o') return -1;
        const int k = nodes[obj].names.indexOf(name);
        return k >= 0 && k < nodes[obj].vals.size() ? res(nodes[obj].vals[k]) : -1;
    }
    qint64 num(int v, qint64 def) const { v = res(v); return v >= 0 && nodes[v].kind == 'i' ? nodes[v].i : def; }
    QString str(int v) const { v = res(v); return v >= 0 && nodes[v].kind == 's' ? nodes[v].s : QString(); }
    QString cls(int v) const { v = res(v); return v >= 0 && nodes[v].kind == 'o' ? nodes[v].cls : QString(); }
};

QByteArray gunzipAll(const uchar* s, qint64 len){
    z_stream z; std::memset(&z, 0, sizeof z);
    if(inflateInit2(&z, 16 + MAX_WBITS) != Z_OK) return QByteArray();
    QByteArray out; const qint64 cap = qint64(1) << 31;
    z.next_in = const_cast<Bytef*>(s); z.avail_in = uInt(qMin<qint64>(len, 0x7fffffff));
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
// One gzip chunk into exactly `size` bytes at `d`.
bool gunzipInto(const uchar* s, qint64 len, uchar* d, qint64 size){
    z_stream z; std::memset(&z, 0, sizeof z);
    if(inflateInit2(&z, 16 + MAX_WBITS) != Z_OK) return false;
    z.next_in = const_cast<Bytef*>(s); z.avail_in = uInt(len);
    z.next_out = d; z.avail_out = uInt(size);
    const int rc = inflate(&z, Z_FINISH);
    const bool ok = (rc == Z_STREAM_END || rc == Z_BUF_ERROR || rc == Z_OK) && z.avail_out == 0;
    inflateEnd(&z);
    return ok;
}

QImage headerThumbnail(const QByteArray& xml){
    const int at = xml.indexOf("<thumb png=\"");
    if(at < 0) return QImage();
    const int from = at + 12, to = xml.indexOf('"', from);
    if(to < 0) return QImage();
    QImage img; img.loadFromData(QByteArray::fromBase64(xml.mid(from, to - from)), "PNG");
    return img;
}

// Paint.NET's blend modes, by LayerBlendMode value (Paint.NET 4 and 5) or blend op class name (3).
const char* const kBlendNames[] = { "Normal", "Multiply", "Additive", "ColorBurn", "ColorDodge", "Reflect", "Glow",
                                    "Overlay", "Difference", "Negation", "Lighten", "Darken", "Screen", "Xor" };
void applyPdnBlend(Shape& s, int mode){
    static const char* const keys[] = { "norm", "mul ", "lddg", "idiv", "div ", nullptr, nullptr, "over", "diff", nullptr, "lite", "dark", "scrn", nullptr };
    if(keys[mode]){ psdApplyBlend(s, keys[mode]); return; }
    // Reflect, Glow, Negation and Xor have no PSD peer; the renderer blends them per pixel.
    s.transp.type = mode == 5 ? 43 : mode == 6 ? 44 : mode == 9 ? 42 : 54;
    s.transp.geom = 0; s.transp.t0 = s.transp.t1 = 0.0;
}

} // namespace

Document importPdnDocument(const QByteArray& data, PdnImportStats* st){
    Document doc;
    const uchar* p = reinterpret_cast<const uchar*>(data.constData());
    qint64 n = data.size(), at = 0;
    QByteArray unpacked; QImage thumb;
    if(data.startsWith("PDN3")){
        if(n < 9) return doc;
        const qint64 hs = qint64(p[4]) | qint64(p[5]) << 8 | qint64(p[6]) << 16;
        if(7 + hs + 2 > n) return doc;
        thumb = headerThumbnail(data.mid(7, int(hs)));
        at = 7 + hs;
        if(p[at] == 0 && p[at + 1] == 1) at += 2;
        else if(p[at] == 0x1f && p[at + 1] == 0x8b){ unpacked = gunzipAll(p + at, n - at); p = reinterpret_cast<const uchar*>(unpacked.constData()); n = unpacked.size(); at = 0; }
        else return doc;
    } else if(n > 2 && p[0] == 0x1f && p[1] == 0x8b){
        unpacked = gunzipAll(p, n); p = reinterpret_cast<const uchar*>(unpacked.constData()); n = unpacked.size();
    } else return doc;

    auto thumbnailOnly = [&](int W, int H){
        if(thumb.isNull()) return;
        if(W < 1 || H < 1){ W = thumb.width(); H = thumb.height(); }
        doc.pageSize = QSizeF(W, H);
        Shape s = psdBitmapShape(thumb.convertToFormat(QImage::Format_ARGB32), 0, 0, QStringLiteral("Preview"));
        s.transform = QTransform::fromScale(double(W) / thumb.width(), double(H) / thumb.height());
        doc.shapes = { s };
        if(st) st->thumbnailOnly = true;
    };

    Nrbf g(p + at, n - at);
    const bool parsed = g.parse();
    const int docNode = g.res(g.ids.value(g.root, -1));
    const qint64 W = g.num(g.member(docNode, "width"), 0), H = g.num(g.member(docNode, "height"), 0);
    if(!parsed || W < 1 || H < 1 || W > 1000000 || H > 1000000 || W * H > kMaxPixels){ thumbnailOnly(int(qBound(qint64(0), W, qint64(1000000))), int(qBound(qint64(0), H, qint64(1000000)))); return doc; }
    doc.pageSize = QSizeF(W, H);

    // The layers, bottom first, and which memory block holds each one's pixels.
    const int list = g.member(docNode, "layers");
    const int items = g.member(list, "ArrayList+_items");
    if(items < 0 || g.nodes[items].kind != 'a'){ thumbnailOnly(int(W), int(H)); return doc; }
    const qint64 have = g.nodes[items].vals.size();
    const int count = int(qBound(qint64(0), g.num(g.member(list, "ArrayList+_size"), have), have));
    QHash<int, int> layerOfBlock;
    QVector<int> layerNodes;
    for(int k = 0; k < count && k < 4096; ++k){
        const int L = g.res(g.nodes[items].vals[k]);
        layerNodes << L;
        const int block = g.member(g.member(L, "surface"), "scan0");
        if(block >= 0 && !layerOfBlock.contains(block)) layerOfBlock.insert(block, k);
    }

    // The deferred blocks follow the graph in the order their records appeared in it.
    QVector<QByteArray> pixels(layerNodes.size());
    qint64 o = at + g.o, budget = kMaxTotalPixels * 4;
    bool streamOk = true;
    for(int b = 0; b < g.nodes.size() && streamOk; ++b){
        const Nrbf::Node& v = g.nodes[b];
        if(v.kind != 'o' || !v.cls.endsWith(QLatin1String("MemoryBlock"))) continue;
        if(!g.num(g.member(b, "deferred"), 0) || g.num(g.member(b, "hasParent"), 0)) continue;
        qint64 length = g.num(g.member(b, "length64"), -1);
        if(length < 0) length = g.num(g.member(b, "length"), -1);
        if(length < 0 || length > kMaxPixels * 4 || o + 5 > n){ streamOk = false; break; }
        const int fmt = p[o];
        const qint64 chunk = qint64(p[o + 1]) << 24 | qint64(p[o + 2]) << 16 | qint64(p[o + 3]) << 8 | p[o + 4];
        o += 5;
        if(chunk < 1 || (fmt != 0 && fmt != 1) || (length + chunk - 1) / chunk > (1 << 24)){ streamOk = false; break; }
        const qint64 chunks = (length + chunk - 1) / chunk;
        const int layer = layerOfBlock.value(b, -1);
        QByteArray buf;
        if(layer >= 0 && length <= budget){ buf = QByteArray(length, Qt::Uninitialized); budget -= length; std::memset(buf.data(), 0, size_t(length)); }
        QVector<bool> seen(buf.isEmpty() ? 0 : int(chunks), false);
        int good = 0;
        for(qint64 c = 0; c < chunks; ++c){
            if(o + 8 > n){ streamOk = false; break; }
            const quint32 num = quint32(p[o]) << 24 | quint32(p[o + 1]) << 16 | quint32(p[o + 2]) << 8 | p[o + 3];
            const qint64 size = qint64(p[o + 4]) << 24 | qint64(p[o + 5]) << 16 | qint64(p[o + 6]) << 8 | p[o + 7];
            o += 8;
            if(size > n - o){ streamOk = false; break; }
            if(!buf.isEmpty() && num < chunks && !seen[int(num)]){
                const qint64 off = qint64(num) * chunk, want = qMin(chunk, length - off);
                uchar* d = reinterpret_cast<uchar*>(buf.data()) + off;
                const bool ok = fmt == 0 ? gunzipInto(p + o, size, d, want) : (size >= want && (std::memcpy(d, p + o, size_t(want)), true));
                if(ok){ seen[int(num)] = true; ++good; }
            }
            o += size;
        }
        if(layer >= 0 && !buf.isEmpty()){
            if(good < chunks && st) st->badChunks += int(chunks - good);
            pixels[layer] = buf;
        }
    }

    for(int k = 0; k < layerNodes.size(); ++k){
        const int L = layerNodes[k];
        const int props = g.member(L, "Layer+properties"), surf = g.member(L, "surface");
        const qint64 sw = g.num(g.member(surf, "width"), W), sh = g.num(g.member(surf, "height"), H), stride = g.num(g.member(surf, "stride"), 0);
        const QByteArray& buf = pixels[k];
        if(sw != W || sh != H || stride < W * 4 || buf.size() < stride * (H - 1) + W * 4){ if(st) ++st->missing; continue; }
        QImage img(int(W), int(H), QImage::Format_ARGB32);
        if(img.isNull()){ if(st) ++st->missing; continue; }
        int x0 = int(W), y0 = int(H), x1 = -1, y1 = -1;
        for(int y = 0; y < H; ++y){
            const uchar* s = reinterpret_cast<const uchar*>(buf.constData()) + qint64(y) * stride;
            QRgb* d = reinterpret_cast<QRgb*>(img.scanLine(y));
            for(int x = 0; x < W; ++x, s += 4){
                d[x] = qRgba(s[2], s[1], s[0], s[3]);
                if(s[3]){ if(x < x0) x0 = x; if(x > x1) x1 = x; if(y < y0) y0 = y; y1 = y; }
            }
        }
        if(x1 < 0){ if(st) ++st->empty; continue; }   // nothing painted
        const QRect box(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
        QString name = g.str(g.member(props, "name"));
        if(name.isEmpty()) name = QStringLiteral("Layer %1").arg(k + 1);
        Shape s = psdBitmapShape(box.size() == img.size() ? img : img.copy(box), box.x(), box.y(), name);
        s.opacity = qBound(qint64(0), g.num(g.member(props, "opacity"), 255), qint64(255)) / 255.0;
        s.visible = g.num(g.member(props, "visible"), 1) != 0;
        int mode = 0;
        const int bm = g.member(props, "blendMode");
        if(bm >= 0) mode = int(g.nodes[bm].kind == 'o' ? g.num(g.member(bm, "value__"), 0) : g.num(bm, 0));
        else {
            const QString op = g.cls(g.member(g.member(L, "properties"), "blendOp"));
            for(int m = 0; m < 14; ++m) if(op.endsWith(QLatin1Char('+') + QLatin1String(kBlendNames[m]) + QLatin1String("BlendOp"))) mode = m;
        }
        if(mode < 0 || mode > 13){ if(st) ++st->unknownModes; mode = 0; }
        applyPdnBlend(s, mode);
        if(st){ ++st->layers; if(!s.visible) ++st->hidden; }
        doc.shapes << s;
    }
    if(doc.shapes.isEmpty() && (!streamOk || (st && st->missing))) thumbnailOnly(int(W), int(H));
    return doc;
}

} // namespace xara
