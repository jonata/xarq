// Serif Affinity (.afdesign / .afphoto / .afpub / .aftemplate / .af) — first-party vector reader.
//
// Three layers, bottom-up:
//   1. CONTAINER.  16-byte file header (00 FF 4B 41, version, flag, class 4CC), a 52-byte "#Inf"
//      block (FAT offset, thumbnail offset, …), an optional "Prot" block, then a flat run of "#Fil"
//      chunks each terminated by 0xFFFFFFFF, and finally a linked chain of FAT blocks — one link per
//      saved revision — tagged #FAT / #FT2 / #FT3 / #FT4. A FAT entry names a stream and says where
//      its bytes are, how they are compressed (stored / zlib / zstd, optionally delta-predicted) and
//      what CRC32 the restored bytes must have. That checksum is an exact integrity gate and every
//      chunk is verified against it, so a mis-decode declines instead of producing plausible garbage.
//   2. OBJECT TREE.  The "doc.dat" stream is a self-describing tagged tree: every field carries a
//      type byte (bit 0x80 = array) and a 4CC tag, and every payload length is derivable from the
//      type alone — no schema database is needed. Classes come in three flavours: positional (0x30),
//      shareable (0x31, which registers an id that later links point back to — the tree is a DAG),
//      and plain (0x32). Fields are an ORDERED LIST that legitimately repeats tags, so they are kept
//      in order and looked up last-match-wins (a derived class overrides its base).
//   3. CONVERSION.  The document tree (DocN → Sprd spreads → Scop layers → drawables) is re-emitted
//      as one standard SVG per spread and handed to XarQ's SVG importer, which turns it into real
//      editable shapes. Affinity's coordinate system is SVG's: origin top-left, y down, and 'Xfrm' is
//      a 2x3 affine stored as two rows of three (x' = m0·x + m1·y + m2, y' = m3·x + m4·y + m5).
//
// Everything read out of the file is attacker-controlled: counts, lengths and record sizes all
// precede their data. Every read is bounds-checked against the remaining buffer BEFORE allocating,
// recursion is depth-capped, the shared-class DAG is walked with an on-path set so a crafted link
// cannot cycle, and every file-derived double is validated in the double domain before any cast.
#include "affvector.h"
#include "svgimport.h"

#include <QBuffer>
#include <QColor>
#include <QHash>
#include <QFont>
#include <QFontMetricsF>
#include <QImage>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QTransform>
#include <QSet>
#include <QVector>
#include <QPair>
#include <QPointF>
#include <QStringList>
#include <QRegularExpression>
#include <QtGlobal>
#include <algorithm>
#include <functional>
#include <cstring>
#include <cmath>
#include <zlib.h>
#ifdef HAVE_ZSTD
#include <zstd.h>
#endif

namespace xara {
namespace {

using u8 = uint8_t; using u16 = uint16_t; using u32 = uint32_t; using u64 = uint64_t; using i64 = int64_t;

inline u16 rdu16(const u8* p){ return u16(p[0]) | (u16(p[1]) << 8); }
inline u32 rdu32(const u8* p){ return u32(p[0]) | (u32(p[1])<<8) | (u32(p[2])<<16) | (u32(p[3])<<24); }
inline u64 rdu64(const u8* p){ u64 v=0; for(int i=7;i>=0;--i) v = (v<<8) | u64(p[i]); return v; }

// Object-tree 4CCs are stored byte-REVERSED (the little-endian u32 of the ASCII), so reading the u32
// little-endian and comparing against this big-endian packing of the readable name is exact.
constexpr u32 TG(char a, char b, char c, char d){
    return (u32(u8(a))<<24) | (u32(u8(b))<<16) | (u32(u8(c))<<8) | u32(u8(d));
}

// ---- hard limits (hostile input must decline, never hang or over-allocate) ----
constexpr i64  kMaxStream    = 256ll<<20;   // largest chunk we will decompress
constexpr i64  kMaxTree      =  64ll<<20;   // largest object-tree stream we will parse
constexpr int  kMaxNodes     = 500000;
constexpr int  kMaxFields    = 3000000;
constexpr int  kMaxArray     = 8000000;
constexpr i64  kMaxPayload   = 192ll<<20;   // total in-memory field payload for one tree
constexpr int  kMaxDepth     = 96;
constexpr int  kMaxWalkDepth = 64;
constexpr int  kMaxShapes    = 200000;
constexpr i64  kMaxSvg       = 96ll<<20;    // total SVG text budget per document
constexpr i64  kMaxImageBytes= 24ll<<20;    // per placed image
constexpr i64  kMaxImageTotal= 64ll<<20;
constexpr int  kTile         = 256;         // raster layers are stored as 256x256 single-channel planes
constexpr i64  kMaxRasterPx  = 64ll<<20;    // largest single raster layer we will assemble
constexpr i64  kMaxRasterTot = 128ll<<20;   // total raster pixels per document — see rasterImage

// ---------------------------------------------------------------- bounded reader ---
struct Rd {
    const u8* p = nullptr; i64 n = 0; i64 o = 0; bool bad = false;
    bool need(i64 k){ if(bad) return false; if(k < 0 || k > n - o){ bad = true; return false; } return true; }
    i64  left() const { return bad ? 0 : n - o; }
    u8   u8v (){ if(!need(1)) return 0; return p[o++]; }
    u16  u16v(){ if(!need(2)) return 0; u16 v = rdu16(p+o); o += 2; return v; }
    u32  u32v(){ if(!need(4)) return 0; u32 v = rdu32(p+o); o += 4; return v; }
    u64  u64v(){ if(!need(8)) return 0; u64 v = rdu64(p+o); o += 8; return v; }
    float  f32v(){ u32 b = u32v(); float  f; memcpy(&f, &b, 4); return f; }
    double f64v(){ u64 b = u64v(); double d; memcpy(&d, &b, 8); return d; }
    QByteArray raw(i64 k){
        if(k < 0 || !need(k)) return {};
        QByteArray b(reinterpret_cast<const char*>(p+o), qsizetype(k)); o += k; return b;
    }
};

inline double finite(double v){ return std::isfinite(v) ? v : 0.0; }

// ---------------------------------------------------------------- container ---
struct AffEntry {
    u32 id = 0; u8 flag = 0;
    u64 off = 0, size = 0, csize = 0;
    u32 crc = 0; u8 comp = 0;
    u64 rev = 0;
    QString name;
};

struct AffArchive {
    const u8* p = nullptr; i64 n = 0;
    int version = 0;
    u64 thmb = 0;
    QHash<QString, AffEntry> files;
    bool ok = false;
};

bool isContainerHeader(const u8* p, i64 n){
    return n >= 80 && p[0]==0x00 && p[1]==0xFF && p[2]=='K' && p[3]=='A' && memcmp(p+12, "#Inf", 4)==0;
}

bool parseContainer(const u8* p, i64 n, AffArchive& a){
    if(!isContainerHeader(p, n)) return false;
    const int ver = rdu16(p+4), flg = rdu16(p+6);
    // Bits 0-1 of the flag select a container variant we do not read (the reference reader refuses them
    // too). Higher bits are informational — a real Affinity 2.x .afphoto ships flag=4 and is ordinary.
    if((flg & 3) != 0) return false;
    if(ver < 7 || ver > 15) return false;
    a.p = p; a.n = n; a.version = ver;
    a.thmb = rdu64(p+24);
    const u64 fatOff = rdu64(p+16);
    // Compare by SUBTRACTION, never "off + k > n": these offsets are file-derived u64s and 0xFFFF…FF + 59
    // wraps to a small number, which would wave a wild pointer straight through the bounds check.
    if(fatOff < 16 || fatOff > u64(n) || u64(n) - fatOff < 59) return false;

    QSet<u64> seenFat;                                // the FAT is a revision CHAIN and can be crafted to loop
    QHash<u32, QString> idName;                       // flag==1 entries are unnamed revisions of a known id
    u64 off = fatOff;
    for(int link = 0; off != 0 && link < 64; ++link){
        if(off > u64(n) || u64(n) - off < 59 || seenFat.contains(off)) break;
        seenFat.insert(off);
        const u8* f = p + off;
        const bool isFat1 = memcmp(f, "#FAT", 4)==0;
        const bool isFat2 = memcmp(f, "#FT2", 4)==0;
        const bool isFat3 = memcmp(f, "#FT3", 4)==0;
        const bool isFat4 = memcmp(f, "#FT4", 4)==0;
        if(!isFat1 && !isFat2 && !isFat3 && !isFat4) break;
        const bool legacyComp = isFat1 || isFat2;     // #FAT/#FT2 store a compression INDEX, not the byte
        const u64 next  = rdu64(f+4);
        const u64 cdate = rdu64(f+12);
        const u32 filesCount = rdu32(f+44);
        const u32 tailLen    = rdu32(f+52);           // bytes of records after this 59-byte header
        const int dirsCount  = rdu16(f+56);

        u64 end = off + 59 + u64(tailLen);
        if(end > u64(n) || tailLen == 0) end = u64(n);
        if(u64(filesCount) > (end - off) / 5) break;  // impossible entry density → corrupt

        Rd r{ p, i64(end), i64(off + 59), false };
        for(u32 i = 0; i < filesCount && !r.bad; ++i){
            AffEntry e; e.rev = cdate;
            e.id = r.u32v(); e.flag = r.u8v();
            if(r.bad || e.flag > 2){ r.bad = true; break; }
            if(e.flag <= 1){
                e.off = r.u64v(); e.size = r.u64v(); e.csize = r.u64v();
                e.crc = r.u32v(); e.comp = r.u8v();
                if(!isFat1) r.u32v();                 // unknown6 — present on every FAT but #FAT
                if(isFat4)  r.u32v();                 // #FT4 adds a second per-entry word
                if(legacyComp){
                    switch(e.comp){
                        case 1: e.comp = 0x01; break; case 2: e.comp = 0x41; break;
                        case 3: e.comp = 0x81; break; case 4: e.comp = 0xC1; break;
                        default: e.comp = 0x00; break;
                    }
                }
            }
            if(e.flag == 0){
                const int ln = r.u16v();
                const QByteArray nm = r.raw(ln);
                if(r.bad) break;
                e.name = QString::fromUtf8(nm);
                if(!e.name.isEmpty()) idName.insert(e.id, e.name);
            } else {
                e.name = idName.value(e.id);
            }
            if(r.bad) break;
            if(e.flag != 2 && e.size > 0 && !e.name.isEmpty()){
                const auto it = a.files.constFind(e.name);
                // The head FAT link is the newest revision, so only a strictly newer date may replace it.
                if(it == a.files.constEnd() || cdate > it->rev) a.files.insert(e.name, e);
            }
        }
        for(int i = 0; i < dirsCount && !r.bad; ++i){
            const int sl = r.u16v();
            const int sm = r.u16v();
            r.u64v();
            r.raw(sl);
            if(sm != 0) break;                        // the reference reader hard-fails here; just stop
        }
        off = next;
    }
    a.ok = !a.files.isEmpty();
    return a.ok;
}

// Decompress + un-predict + CRC-verify one named stream. Returns empty on any failure.
QByteArray extractStream(const AffArchive& a, const AffEntry& e){
    if(e.size == 0 || e.size > u64(kMaxStream)) return {};
    if(e.off > u64(a.n) || u64(a.n) - e.off < 4) return {};     // subtraction, not addition — see above
    if(memcmp(a.p + e.off, "#Fil", 4) != 0) return {};
    const u8* src = a.p + e.off + 4;
    const u64 avail = u64(a.n) - (e.off + 4);

    const int alg = e.comp & 3;
    int pred = 0, pflag = (e.comp >> 5) & 1;
    switch(e.comp & 0xC0){
        case 0x40: pred = 1; pflag = 0; break;        // 0x40 selects byte-delta AND clears the suppress bit
        case 0x80: pred = 2; break;
        case 0xC0: pred = 3; break;
        default:   pred = 0; pflag = 0; break;
    }

    QByteArray out;
    if(alg == 1){                                      // raw zlib (not Qt's length-prefixed qUncompress form)
        if(e.csize == 0 || e.csize > avail || e.csize > u64(kMaxStream)) return {};
        out.resize(qsizetype(e.size));
        z_stream zs; memset(&zs, 0, sizeof zs);
        if(inflateInit(&zs) != Z_OK) return {};
        zs.next_in  = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(src));
        zs.avail_in = uInt(e.csize);
        zs.next_out = reinterpret_cast<Bytef*>(out.data());
        zs.avail_out= uInt(e.size);
        const int rc = inflate(&zs, Z_FINISH);
        const uLong done = zs.total_out;
        inflateEnd(&zs);
        if((rc != Z_STREAM_END && rc != Z_OK) || done != e.size) return {};
    } else if(alg == 2){
#ifdef HAVE_ZSTD
        if(e.csize == 0 || e.csize > avail || e.csize > u64(kMaxStream)) return {};
        out.resize(qsizetype(e.size));
        const size_t rc = ZSTD_decompress(out.data(), size_t(e.size), src, size_t(e.csize));
        if(ZSTD_isError(rc) || rc != size_t(e.size)) return {};
#else
        return {};                                     // container v11 / Affinity 2.x needs zstd
#endif
    } else {
        if(e.size > avail) return {};
        out = QByteArray(reinterpret_cast<const char*>(src), qsizetype(e.size));
    }

    // Predictors run AFTER decompression and BEFORE the checksum.
    u8* b = reinterpret_cast<u8*>(out.data());
    const i64 len = out.size();
    if(pflag == 0 && pred == 1){
        for(i64 i = 1; i < len; ++i) b[i] = u8(b[i] + b[i-1]);
    } else if(pflag == 0 && pred == 2){
        u16 prev = 0;
        for(i64 i = 0; i + 1 < len; i += 2){
            const u16 v = u16(rdu16(b+i) + prev);
            b[i] = u8(v & 0xFF); b[i+1] = u8(v >> 8); prev = v;
        }
    } else if(pflag == 1 && pred == 2){
        for(i64 i = 1; i < len; ++i) b[i] = u8(b[i] + b[i-1]);
        if(len == 0x10000){                            // 64 KiB de-interleave of two 32 KiB half-planes
            QByteArray t(out.size(), Qt::Uninitialized);
            u8* d = reinterpret_cast<u8*>(t.data());
            const u8* h0 = b; const u8* h1 = b + 0x8000;
            for(i64 i = 0; i < 0x4000; ++i){
                d[i*4+0] = h0[i*2+0]; d[i*4+1] = h0[i*2+1];
                d[i*4+2] = h1[i*2+0]; d[i*4+3] = h1[i*2+1];
            }
            out = t;
        }
    }

    const u32 crc = u32(crc32(crc32(0, nullptr, 0),
                              reinterpret_cast<const Bytef*>(out.constData()), uInt(out.size())));
    if(crc != e.crc) return {};                        // wrong algorithm/predictor → decline, never guess
    return out;
}

QByteArray namedStream(const AffArchive& a, const QString& name){
    const auto it = a.files.constFind(name);
    if(it == a.files.constEnd()) return {};
    return extractStream(a, *it);
}

// ---------------------------------------------------------------- object tree ---
struct AffField {
    u32  tag   = 0;
    u8   type  = 0;
    bool arr   = false;
    int  count = 0;          // element count
    int  comps = 1;          // numeric components per element
    int  recSize = 0;        // fixed-record byte size (0x2c and 0x35..0x74)
    u32  embTag = 0;         // 0x33 embedded-data tag
    int  ever  = 0;          // enum / flags version
    QVector<double> num;     // numeric payload, count*comps entries
    QByteArray bytes;        // string, blob or fixed-record payload
    QVector<QByteArray> strs;
    QVector<int> kids;       // child node indices (-1 = null / unresolved link)
};

struct AffNode {
    QVector<u32> types;      // inheritance chain, [0] = most derived
    u32 id = 0;
    QVector<AffField> fields;
};

int scalarBytes(u8 t){
    switch(t){
        case 0x01: case 0x05: return 1;
        case 0x02: case 0x06: return 2;
        case 0x03: case 0x07: case 0x09: case 0x2F: case 0x34: return 4;
        case 0x04: case 0x08: case 0x0A: return 8;
        default: return 0;
    }
}
int vecComps(u8 t){
    if(t >= 0x15 && t <= 0x19) return t - 0x15 + 2;   // int32 vec2..vec6
    if(t >= 0x1F && t <= 0x23) return t - 0x1F + 2;   // float vec2..vec6
    if(t >= 0x24 && t <= 0x28) return t - 0x24 + 2;   // double vec2..vec6
    return 0;
}
u8 vecElem(u8 t){
    if(t >= 0x15 && t <= 0x19) return 0x07;
    if(t >= 0x1F && t <= 0x23) return 0x09;
    return 0x0A;
}

class AffTree {
public:
    explicit AffTree(const QByteArray& d) : m_buf(d) {
        r.p = reinterpret_cast<const u8*>(m_buf.constData());
        r.n = m_buf.size();
    }
    bool load();
    void resolveMasters();
    const AffNode* node(int i) const { return (i >= 0 && i < nodes.size()) ? &nodes[i] : nullptr; }

    QVector<AffNode> nodes;
    int  root = -1;
    u32  typeTag = 0;
    bool failed = false;
    i64  consumed() const { return r.o; }
    i64  total() const { return m_buf.size(); }

private:
    QByteArray m_buf;
    Rd r{};
    QHash<u32,int> m_shared;
    int m_depth = 0;
    int m_fields = 0;
    i64 m_payload = 0;

    int  newNode();
    // Field payloads can AMPLIFY hugely: a bit-packed bool array is 1 byte of file per 8 elements but
    // 8 bytes of memory each (64x), so a small crafted tree could ask for gigabytes. Every allocation
    // is charged against one shared budget.
    bool spend(i64 bytes){ m_payload += bytes; if(m_payload > kMaxPayload){ failed = true; return false; } return true; }
    bool fieldList(int node, bool withTag);
    bool readValue(AffField& f);
    int  readClass(u8 t, bool haveHdr, u32 hdrTag);
    double readScalar(u8 t);
};

int AffTree::newNode(){
    if(nodes.size() >= kMaxNodes){ failed = true; return -1; }
    nodes.push_back(AffNode{});
    return int(nodes.size()) - 1;
}

double AffTree::readScalar(u8 t){
    switch(t){
        case 0x01: return double(r.u8v());
        case 0x02: return double(r.u16v());
        case 0x03: return double(r.u32v());
        case 0x04: return double(r.u64v());
        case 0x05: return double(int8_t(r.u8v()));
        case 0x06: return double(int16_t(r.u16v()));
        case 0x07: return double(int32_t(r.u32v()));
        case 0x08: return double(int64_t(r.u64v()));
        case 0x09: return finite(double(r.f32v()));
        case 0x0A: return finite(r.f64v());
        case 0x2F: case 0x34: return double(r.u32v());
        default: r.bad = true; return 0.0;
    }
}

bool AffTree::readValue(AffField& f){
    const u8 t = f.type; const bool arr = f.arr;

    if(const int sb = scalarBytes(t)){
        i64 c = 1;
        if(arr){ c = i64(r.u32v()); if(r.bad) return false; }
        if(c < 0 || c > kMaxArray || c > r.left() / sb) return false;
        if(!spend(c * 8)) return false;
        f.count = int(c); f.comps = 1;
        f.num.resize(qsizetype(c));
        for(i64 i = 0; i < c; ++i) f.num[qsizetype(i)] = readScalar(t);
        return !r.bad;
    }
    if(const int vc = vecComps(t)){
        const u8 et = vecElem(t);
        const int es = scalarBytes(et);
        i64 c = 1;
        if(arr){ c = i64(r.u32v()); if(r.bad) return false; }
        if(c < 0 || c > kMaxArray || c > r.left() / (i64(vc) * es)) return false;
        if(!spend(c * vc * 8)) return false;
        f.count = int(c); f.comps = vc;
        f.num.resize(qsizetype(c * vc));
        for(i64 i = 0; i < c * vc; ++i) f.num[qsizetype(i)] = readScalar(et);
        return !r.bad;
    }
    switch(t){
        case 0x29: {                                   // bool — ARRAYS are bit-packed, 8 per byte, LSB first
            if(arr){
                const i64 c = i64(r.u32v());
                if(r.bad || c < 0 || c > kMaxArray || (c + 7) / 8 > r.left()) return false;
                if(!spend(c * 8)) return false;
                const QByteArray raw = r.raw((c + 7) / 8);
                if(r.bad) return false;
                f.count = int(c); f.num.resize(qsizetype(c));
                for(i64 i = 0; i < c; ++i)
                    f.num[qsizetype(i)] = (u8(raw[qsizetype(i >> 3)]) >> (i & 7)) & 1;
            } else { f.count = 1; f.num.resize(1); f.num[0] = r.u8v() ? 1.0 : 0.0; }
            return !r.bad;
        }
        case 0x2A: {                                   // enum: id + version (arrays share ONE version)
            if(arr){
                const i64 c = i64(r.u32v());
                if(r.bad || c < 0 || c > kMaxArray || c * 2 > r.left() - 2) return false;
                if(!spend(c * 8)) return false;
                f.ever = r.u16v();
                f.count = int(c); f.num.resize(qsizetype(c));
                for(i64 i = 0; i < c; ++i) f.num[qsizetype(i)] = double(r.u16v());
            } else { f.count = 1; f.num.resize(1); f.num[0] = double(r.u16v()); f.ever = r.u16v(); }
            return !r.bad;
        }
        case 0x2B: case 0x2E: {                        // string: u32 BYTE length (arrays add a total-size prefix)
            if(arr){
                r.u32v();                              // total size (not the element count)
                const i64 c = i64(r.u32v());
                if(r.bad || c < 0 || c > kMaxArray || c * 4 > r.left()) return false;
                if(!spend(c * 32)) return false;                // per-element QByteArray overhead
                f.count = int(c); f.strs.reserve(int(qMin<i64>(c, 65536)));
                for(i64 i = 0; i < c; ++i){
                    const i64 ln = i64(r.u32v());
                    if(r.bad || ln < 0 || ln > r.left() || !spend(ln)) return false;
                    f.strs.push_back(r.raw(ln));
                }
            } else {
                const i64 ln = i64(r.u32v());
                if(r.bad || ln < 0 || ln > r.left() || !spend(ln)) return false;
                f.count = 1; f.bytes = r.raw(ln);
            }
            return !r.bad;
        }
        case 0x2C: {                                   // fixed-size record list; the size u16 comes AFTER the count
            i64 c = 1;
            if(arr){ c = i64(r.u32v()); if(r.bad) return false; }
            const int size = r.u16v();
            if(r.bad || c < 0 || size <= 0 || c > kMaxArray || c * size > r.left()) return false;
            if(!spend(c * size)) return false;
            f.count = int(c); f.recSize = size; f.bytes = r.raw(c * size);
            return !r.bad;
        }
        case 0x2D: {                                   // binary blob
            const i64 c = arr ? i64(r.u32v()) : 1;
            if(r.bad || c < 0 || c > kMaxArray) return false;
            for(i64 i = 0; i < c; ++i){
                const i64 ln = i64(r.u32v());
                if(r.bad || ln < 0 || ln > r.left()) return false;
                if(i == 0 && !spend(ln)) return false;
                const QByteArray b = r.raw(ln);
                if(i == 0) f.bytes = b;
            }
            f.count = int(c);
            return !r.bad;
        }
        case 0x33: {                                   // embedded data: tag + size + bytes
            const i64 c = arr ? i64(r.u32v()) : 1;
            if(r.bad || c < 0 || c > kMaxArray) return false;
            for(i64 i = 0; i < c; ++i){
                const u32 tg = r.u32v();
                const i64 ln = i64(r.u32v());
                if(r.bad || ln < 0 || ln > r.left()) return false;
                if(i == 0 && !spend(ln)) return false;
                const QByteArray b = r.raw(ln);
                if(i == 0){ f.embTag = tg; f.bytes = b; }
            }
            f.count = int(c);
            return !r.bad;
        }
        case 0x75: {                                   // flags: version + byte count + little-endian flag bytes
            f.ever = r.u16v();
            const int c = r.u8v();
            if(r.bad || c < 0 || c > 8 || c > r.left()) return false;
            const QByteArray b = r.raw(c);
            u64 v = 0; for(int i = c - 1; i >= 0; --i) v = (v << 8) | u8(b[i]);
            f.count = 1; f.num.resize(1); f.num[0] = double(v);
            return !r.bad;
        }
        default: break;
    }
    if(t >= 0x35 && t <= 0x74){                        // fixed struct of (type - 0x34) bytes
        const int size = t - 0x34;
        i64 c = 1;
        if(arr){ c = i64(r.u32v()); if(r.bad) return false; }
        if(c < 0 || c > kMaxArray || c * size > r.left()) return false;
        if(!spend(c * size)) return false;
        f.count = int(c); f.recSize = size; f.bytes = r.raw(c * size);
        return !r.bad;
    }
    failed = true;                                     // 0x76/0x77 and anything unknown: decline, never guess a length
    return false;
}

int AffTree::readClass(u8 t, bool haveHdr, u32 hdrTag){
    if(m_depth > kMaxDepth){ failed = true; return -1; }
    if(t == 0x30){                                     // positional (untagged) field list
        const int idx = newNode();
        if(idx < 0) return -1;
        ++m_depth; const bool ok = fieldList(idx, false); --m_depth;
        return ok ? idx : -1;
    }
    if(t == 0x31){                                     // shareable class — the DAG's definitions and links
        const u8 flag = r.u8v();
        if(r.bad) return -1;
        if(flag == 0) return -1;                       // null
        if(flag == 2){
            const u32 id = r.u32v();
            if(r.bad) return -1;
            return m_shared.value(id, -1);             // a link to an unknown id is corrupt, not something to follow
        }
        if(flag != 1){
            failed = true; return -1; }
        const u32 id = r.u32v();
        if(r.bad) return -1;
        const int idx = newNode();
        if(idx < 0) return -1;
        nodes[idx].id = id;
        m_shared.insert(id, idx);
        ++m_depth;
        bool ok = true;
        for(int g = 0; ; ++g){                         // inheritance chain: base classes contribute fields inline
            if(g > 64){ failed = true; ok = false; break; }
            const u8 tf = r.u8v();
            if(r.bad){ ok = false; break; }
            if(tf == 1){ const u32 tg = r.u32v(); if(r.bad){ ok = false; break; } nodes[idx].types.push_back(tg); break; }
            if(tf == 2) break;
            if(tf == 0){
                const u32 tg = r.u32v(); r.u16v();
                if(r.bad){ ok = false; break; }
                nodes[idx].types.push_back(tg);
                if(!fieldList(idx, true)){ ok = false; break; }
                continue;
            }
            failed = true; ok = false; break;
        }
        if(ok) ok = fieldList(idx, true);
        --m_depth;
        return ok ? idx : -1;
    }
    // 0x32 — plain class; for ARRAYS the tag+id are read once before the elements
    const u8 flag = r.u8v();
    if(r.bad) return -1;
    if(flag == 0) return -1;
    if(flag != 1){
        failed = true; return -1; }
    u32 tg = hdrTag;
    if(!haveHdr){ tg = r.u32v(); r.u16v(); if(r.bad) return -1; }
    const int idx = newNode();
    if(idx < 0) return -1;
    nodes[idx].types.push_back(tg);
    ++m_depth; const bool ok = fieldList(idx, true); --m_depth;
    return ok ? idx : -1;
}

bool AffTree::fieldList(int node, bool withTag){
    for(;;){
        if(r.bad || failed) return false;
        u8 t = r.u8v();
        if(r.bad) return false;
        const bool arr = (t & 0x80) != 0;
        t &= 0x7F;
        if(t == 0) return true;                        // end of list
        if(t > 0x75){
            failed = true; return false; }   // 0x76/0x77 are genuinely unknown
        if(++m_fields > kMaxFields){ failed = true; return false; }

        AffField f; f.type = t; f.arr = arr;
        if(withTag){ f.tag = r.u32v(); if(r.bad) return false; }

        if(t == 0x30 || t == 0x31 || t == 0x32){
            i64 c = 1; bool haveHdr = false; u32 hdrTag = 0;
            if(arr){
                const i64 cc = i64(r.u32v());
                if(r.bad || cc < 0 || cc > kMaxArray || cc > r.left()){ failed = true; return false; }
                c = cc;
                // A 0x32 ARRAY carries its class tag+id ONCE, before the elements — and it is present
                // even for an EMPTY array, so it must be consumed unconditionally or the stream desyncs.
                if(t == 0x32){ hdrTag = r.u32v(); r.u16v(); haveHdr = true; if(r.bad) return false; }
            }
            if(!spend(c * 4)) return false;
            f.count = int(c);
            f.kids.reserve(int(qMin<i64>(c, 4096)));
            for(i64 i = 0; i < c; ++i){
                const int k = readClass(t, haveHdr, hdrTag);
                if(failed || r.bad) return false;
                f.kids.push_back(k);
            }
        } else if(!readValue(f)){
            failed = true; return false;
        }
        if(node >= 0 && node < nodes.size()) nodes[node].fields.push_back(f);
    }
}

bool AffTree::load(){
    if(m_buf.size() < 12 || m_buf.size() > kMaxTree) return false;
    if(r.u32v() != 0x534BFF00u) return false;          // 00 FF 4B 53 — note KS, not KA
    const int fver = r.u16v();
    typeTag = r.u32v();
    r.u16v();                                          // type version
    if(fver > 2 || r.bad) return false;
    if(fver == 2) r.u32v();                            // the extra version word exists only for file_ver 2
    if(r.bad) return false;
    root = newNode();
    if(root < 0) return false;
    nodes[root].types.push_back(typeTag);
    if(!fieldList(root, true)) return false;
    if(failed || r.bad) return false;
    resolveMasters();
    return true;
}

// A symbol instance or master-page copy is a sparse node: it stores only what it overrides and points
// at the object it was made from (ShpM on a shape, MCrM on a curve, StMa/TxFM on text). A copy that has
// not been edited therefore holds no geometry and no paint of its own, and reads as an empty object
// unless what it does not override is taken from the master. Masters can chain, so resolve depth-first;
// the on-stack mark stops a crafted cycle.
void AffTree::resolveMasters(){
    const u32 links[4] = { TG('S','h','p','M'), TG('M','C','r','M'), TG('S','t','M','a'), TG('T','x','F','M') };
    QVector<char> state(nodes.size(), 0);
    std::function<void(int,int)> go = [&](int i, int depth){
        if(i < 0 || i >= nodes.size() || state[i] != 0 || depth > 64) return;
        state[i] = 1;
        for(u32 l : links){
            for(int f = 0; f < nodes[i].fields.size(); ++f){
                if(nodes[i].fields[f].tag != l) continue;
                const QVector<int> kids = nodes[i].fields[f].kids;
                for(int m : kids){
                    if(m < 0 || m >= nodes.size() || m == i) continue;
                    go(m, depth + 1);
                    if(state[m] == 1) continue;                  // a cycle: leave it unresolved
                    QVector<AffField> add;
                    for(const AffField& mf : nodes[m].fields){
                        bool have = false;
                        for(const AffField& own : nodes[i].fields) if(own.tag == mf.tag){ have = true; break; }
                        if(!have) add.push_back(mf);
                    }
                    if(!add.isEmpty() && spend(i64(add.size()) * 64)){
                        add += nodes[i].fields;
                        nodes[i].fields = add;
                    }
                    break;
                }
                break;
            }
        }
        state[i] = 2;
    };
    for(int i = 0; i < nodes.size(); ++i) go(i, 0);
}

// Fields are an ordered list that legitimately repeats tags (a derived class overrides its base, and
// ShSt carries IRad twice). Last match wins; callers that need every match walk the vector directly.
const AffField* fieldOf(const AffNode& n, u32 tag){
    for(int i = n.fields.size() - 1; i >= 0; --i)
        if(n.fields[i].tag == tag) return &n.fields[i];
    return nullptr;
}
double numOf(const AffNode& n, u32 tag, double def){
    const AffField* f = fieldOf(n, tag);
    return (f && !f->num.isEmpty()) ? f->num[0] : def;
}
int firstKid(const AffNode& n, u32 tag){
    const AffField* f = fieldOf(n, tag);
    if(!f) return -1;
    for(int k : f->kids) if(k >= 0) return k;
    return -1;
}

// ---------------------------------------------------------------- SVG helpers ---
QString fnum(double v){
    if(!std::isfinite(v)) v = 0.0;
    v = qBound(-1e9, v, 1e9);
    QString s = QString::number(v, 'f', 4);
    if(s.contains(QLatin1Char('.'))){
        while(s.endsWith(QLatin1Char('0'))) s.chop(1);
        if(s.endsWith(QLatin1Char('.'))) s.chop(1);
    }
    if(s == QLatin1String("-0")) s = QStringLiteral("0");
    return s;
}
QString xmlEsc(const QString& s){
    QString o; o.reserve(s.size() + 8);
    for(QChar c : s){
        const ushort u = c.unicode();
        if(u == '&') o += QLatin1String("&amp;");
        else if(u == '<') o += QLatin1String("&lt;");
        else if(u == '>') o += QLatin1String("&gt;");
        else if(u == '"') o += QLatin1String("&quot;");
        else if(u == '\'') o += QLatin1String("&apos;");
        else if(u < 0x20 && u != '\t') o += QLatin1Char(' ');
        else o += c;
    }
    return o;
}
QString colHex(const QColor& c){
    return QStringLiteral("#%1%2%3").arg(c.red(),2,16,QLatin1Char('0'))
                                    .arg(c.green(),2,16,QLatin1Char('0'))
                                    .arg(c.blue(),2,16,QLatin1Char('0'));
}
// Affinity stores a 2x3 affine as TWO ROWS OF THREE: x' = m0·x + m1·y + m2, y' = m3·x + m4·y + m5.
// SVG's matrix(a,b,c,d,e,f) is x' = a·x + c·y + e, y' = b·x + d·y + f.
QString affMatrix(const double* m){
    return QStringLiteral("matrix(%1,%2,%3,%4,%5,%6)")
        .arg(fnum(m[0]), fnum(m[3]), fnum(m[1]), fnum(m[4]), fnum(m[2]), fnum(m[5]));
}

// The blend enum self-labels in layer_mode.afdesign (each layer is named after its mode).
// The full id -> mode table, read off layer_mode.afdesign, which names every mode in its layer
// descriptions: v0 1 Darken, 2 Multiply, 3 Colour Burn, 4 Lighten, 5 Screen, 6 Colour Dodge, 7 Add,
// 8 Overlay, 9 Soft Light, 10 Hard Light, 11 Vivid Light, 12 Pin Light, 13 Hard Mix, 14 Difference,
// 15 Exclusion, 16 Subtract, 17 Hue, 18 Saturation, 19 Luminosity, 20 Colour, 21 Average,
// 22 Negation, 23 Reflect, 24 Glow, 25 Erase; v1 2 Darken Colour, 6 Lighten Colour, 15 Linear Light.
// Of the 28, CSS can express 16. The rest (Vivid/Pin Light, Hard Mix, Subtract, Average, Negation,
// Reflect, Glow, Erase and the three v1 modes) have no peer and fall back to Normal.
QString blendCss(int id, int ver){
    if(ver != 0) return QString();                     // v1: Darken Colour / Lighten Colour / Linear Light — no CSS peer
    switch(id){
        case 1:  return QStringLiteral("darken");
        case 2:  return QStringLiteral("multiply");
        case 3:  return QStringLiteral("color-burn");
        case 4:  return QStringLiteral("lighten");
        case 5:  return QStringLiteral("screen");
        case 6:  return QStringLiteral("color-dodge");
        case 7:  return QStringLiteral("plus-lighter");   // Add — the one mode CSS gained late
        case 8:  return QStringLiteral("overlay");
        case 9:  return QStringLiteral("soft-light");
        case 10: return QStringLiteral("hard-light");
        case 14: return QStringLiteral("difference");
        case 15: return QStringLiteral("exclusion");
        case 17: return QStringLiteral("hue");
        case 18: return QStringLiteral("saturation");
        case 19: return QStringLiteral("luminosity");
        case 20: return QStringLiteral("color");
        case 21: return QStringLiteral("xarq-average");    // no CSS peer: per-pixel in the renderer
        case 22: return QStringLiteral("xarq-negation");
        case 23: return QStringLiteral("xarq-reflect");
        case 24: return QStringLiteral("xarq-glow");
        default: return QString();                     // Add/Subtract/Vivid/Pin/Hard Mix/… have no CSS equivalent
    }
}

void rgbToLab(const QColor& c, double& L, double& A, double& B){      // sRGB -> CIELAB D50
    auto lin = [](double v){ return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    const double r = lin(c.redF()), g = lin(c.greenF()), b = lin(c.blueF());
    const double X = 0.4360747*r + 0.3850649*g + 0.1430804*b, Y = 0.2225045*r + 0.7168786*g + 0.0606169*b, Z = 0.0139322*r + 0.0971045*g + 0.7141733*b;
    auto f = [](double t){ return t > 216.0/24389.0 ? std::cbrt(t) : (24389.0/27.0*t + 16.0)/116.0; };
    const double fx = f(X/0.96422), fy = f(Y), fz = f(Z/0.82521);
    L = 116.0*fy - 16.0; A = 500.0*(fx-fy); B = 200.0*(fy-fz);
}

double labF(double t){ return t > 6.0/29.0 ? t*t*t : 3.0*(6.0/29.0)*(6.0/29.0)*(t - 4.0/29.0); }
QColor labToRgb(double L, double A, double B, double alpha){
    const double fy = (L + 16.0) / 116.0, fx = fy + A / 500.0, fz = fy - B / 200.0;
    const double X = 0.95047 * labF(fx), Y = 1.00000 * labF(fy), Z = 1.08883 * labF(fz);
    double r =  3.2406*X - 1.5372*Y - 0.4986*Z;
    double g = -0.9689*X + 1.8758*Y + 0.0415*Z;
    double b =  0.0557*X - 0.2040*Y + 1.0570*Z;
    auto gam = [](double v){ v = qBound(0.0, v, 1.0); return v <= 0.0031308 ? 12.92*v : 1.055*std::pow(v, 1.0/2.4) - 0.055; };
    return QColor::fromRgbF(qBound(0.0, gam(r), 1.0), qBound(0.0, gam(g), 1.0), qBound(0.0, gam(b), 1.0),
                            qBound(0.0, alpha, 1.0));
}

// Exact extremum of one cubic-Bezier component — used to reproduce a path's bounding box and check it
// against the file's own stored CvsB. That agreement is a free, exact self-check on the point model
// (anchor / out-handle / in-handle roles), so it is counted and surfaced to the self-test.
void cubicExt(double p0, double p1, double p2, double p3, double& lo, double& hi){
    lo = qMin(lo, qMin(p0, p3)); hi = qMax(hi, qMax(p0, p3));
    const double a = -p0 + 3*p1 - 3*p2 + p3, b = 2*(p0 - 2*p1 + p2), c = -p0 + p1;
    double ts[2]; int nt = 0;
    if(std::fabs(a) > 1e-12){
        const double disc = b*b - 4*a*c;
        if(disc >= 0){ const double sd = std::sqrt(disc); ts[nt++] = (-b + sd)/(2*a); ts[nt++] = (-b - sd)/(2*a); }
    } else if(std::fabs(b) > 1e-12) ts[nt++] = -c / b;
    for(int i = 0; i < nt; ++i){
        const double t = ts[i];
        if(!(t > 0.0) || !(t < 1.0)) continue;
        const double mt = 1.0 - t;
        const double v = mt*mt*mt*p0 + 3*mt*mt*t*p1 + 3*mt*t*t*p2 + t*t*t*p3;
        lo = qMin(lo, v); hi = qMax(hi, v);
    }
}

// ---------------------------------------------------------------- converter ---
// ---- adjustment layers -------------------------------------------------------------------------
// Every Affinity adjustment is a per-pixel colour transform, so applied to vector content it can be
// baked exactly into the colours (gradients are re-sampled, because the transform is not linear between
// stops). An adjustment applies to what lies BELOW it in its group; as an AdCh child, to its own layer.
struct Spline {   // natural cubic: Vals is x[n], y[n], y''[n]
    QVector<double> x, y, d;
    double at(double v) const {
        const int n = x.size();
        if(n < 2) return v;
        if(v <= x[0]) return y[0];
        if(v >= x[n-1]) return y[n-1];
        int i = 0; while(i + 2 < n && v > x[i+1]) ++i;
        const double h = x[i+1] - x[i];
        if(!(h > 0.0)) return y[i];
        const double a = (x[i+1] - v) / h, b = (v - x[i]) / h;
        return a*y[i] + b*y[i+1] + ((a*a*a - a)*d[i] + (b*b*b - b)*d[i+1]) * h*h / 6.0;
    }
};
struct Adjust {
    enum Kind { Levels, Curves, HSL, BC, BW, Invert, Recolour, Selective,
                ColourBalance, Exposure, Vibrance, Posterize, Threshold, ShadowsHighlights, LensFilter, ChannelMixer, GradientMap } kind = Invert;
    double a = 0, b = 0, c = 0, d = 0, e = 0;
    double wt[6] = {1,1,1,1,1,1};
    double sel[36] = {0};
    bool relative = true;
    Spline master, chan[3];
    QVector<QRgb> map;             // GradientMap: 256 colours by luminance
};
void rgb2hsl(double r, double g, double b, double& h, double& s, double& l){
    const double mx = qMax(r, qMax(g, b)), mn = qMin(r, qMin(g, b));
    l = (mx + mn) / 2.0; h = s = 0.0;
    const double dl = mx - mn;
    if(dl < 1e-9) return;
    s = l > 0.5 ? dl / (2.0 - mx - mn) : dl / (mx + mn);
    if(mx == r) h = (g - b) / dl + (g < b ? 6.0 : 0.0); else if(mx == g) h = (b - r) / dl + 2.0; else h = (r - g) / dl + 4.0;
    h /= 6.0;
}
double h2c(double p, double q, double t){
    if(t < 0) t += 1; if(t > 1) t -= 1;
    if(t < 1.0/6) return p + (q-p)*6*t; if(t < 0.5) return q; if(t < 2.0/3) return p + (q-p)*(2.0/3-t)*6; return p;
}
void hsl2rgb(double h, double s, double l, double& r, double& g, double& b){
    if(s <= 0.0){ r = g = b = l; return; }
    const double q = l < 0.5 ? l*(1+s) : l+s-l*s, p = 2*l-q;
    r = h2c(p,q,h+1.0/3); g = h2c(p,q,h); b = h2c(p,q,h-1.0/3);
}
QRgb labToRgbQ(double L, double A, double B){
    const double fy = (L + 16.0) / 116.0, fx = fy + A / 500.0, fz = fy - B / 200.0;
    auto g = [](double t){ return t > 6.0/29.0 ? t*t*t : 3.0*(6.0/29.0)*(6.0/29.0)*(t - 4.0/29.0); };
    const double X = 0.9642*g(fx), Y = g(fy), Z = 0.8249*g(fz);
    auto e = [](double v){ v = qBound(0.0, v, 1.0); return qBound(0, int((v <= 0.0031308 ? 12.92*v : 1.055*std::pow(v, 1.0/2.4) - 0.055) * 255.0 + 0.5), 255); };
    return qRgb(e(3.1338561*X - 1.6168667*Y - 0.4906146*Z), e(-0.9787684*X + 1.9161415*Y + 0.0334540*Z), e(0.0719453*X - 0.2289914*Y + 1.4052427*Z));
}

// These later kinds come from the format's field names; no available document has real values for them.
void applyAdjust(const Adjust& A, double& r, double& g, double& b){
    auto cl = [](double v){ return qBound(0.0, v, 1.0); };
    auto toLin = [](double v){ return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    auto toGam = [](double v){ v = qBound(0.0, v, 1.0); return v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055; };
    switch(A.kind){
    case Adjust::ColourBalance: {
        // sel: shadows, midtones, highlights, each cyan-red, magenta-green, yellow-blue, in -1..1.
        const double L0 = 0.2126*r + 0.7152*g + 0.0722*b;
        double* ch[3] = { &r, &g, &b };
        for(int i = 0; i < 3; ++i){
            const double v = *ch[i], ws = qBound(0.0, (0.5 - v) * 2.0, 1.0), wh = qBound(0.0, (v - 0.5) * 2.0, 1.0), wm = 1.0 - ws - wh;
            *ch[i] = v + 0.25 * (A.sel[i] * ws + A.sel[3 + i] * wm + A.sel[6 + i] * wh);
        }
        if(A.relative){ const double d = L0 - (0.2126*r + 0.7152*g + 0.0722*b); r += d; g += d; b += d; }
        r = cl(r); g = cl(g); b = cl(b); break; }
    case Adjust::Exposure: {
        const double k = std::pow(2.0, qBound(-20.0, A.a, 20.0));
        r = toGam(toLin(r) * k); g = toGam(toLin(g) * k); b = toGam(toLin(b) * k); break; }
    case Adjust::Vibrance: {
        const double mx = qMax(r, qMax(g, b)), mn = qMin(r, qMin(g, b)), L = (r + g + b) / 3.0;
        const double k = 1.0 + A.a + A.b * (1.0 - (mx - mn));     // vibrance favours what isn't saturated yet
        r = cl(L + (r - L) * k); g = cl(L + (g - L) * k); b = cl(L + (b - L) * k); break; }
    case Adjust::Posterize: {
        const int n = qBound(2, int(A.a), 255);
        auto f = [n](double v){ return qMin(std::floor(v * n), double(n - 1)) / (n - 1); };
        r = f(r); g = f(g); b = f(b); break; }
    case Adjust::Threshold: {
        const double v = 0.2126*r + 0.7152*g + 0.0722*b >= A.a ? A.b : A.c;
        r = g = b = cl(v); break; }
    case Adjust::ShadowsHighlights: {
        auto f = [&](double v){ return cl(v + A.b * (1.0 - v) * (1.0 - v) * v + A.a * v * v * (1.0 - v)); };
        r = f(r); g = f(g); b = f(b); break; }
    case Adjust::LensFilter: {
        const double R = toLin(r), G = toLin(g), B = toLin(b), d = qBound(0.0, A.a, 1.0);
        double r2 = R * (1.0 + (A.wt[0] - 1.0) * d), g2 = G * (1.0 + (A.wt[1] - 1.0) * d), b2 = B * (1.0 + (A.wt[2] - 1.0) * d);
        if(A.relative){ const double k = 0.2126*R + 0.7152*G + 0.0722*B - (0.2126*r2 + 0.7152*g2 + 0.0722*b2); r2 += k; g2 += k; b2 += k; }
        r = toGam(r2); g = toGam(g2); b = toGam(b2); break; }
    case Adjust::ChannelMixer: {
        const int w = qBound(3, int(A.a), 12);
        const double R = r, G = g, B = b;
        auto row = [&](int i){ const double* k = A.sel + i * w; return cl(k[0]*R + k[1]*G + k[2]*B + (w > 3 ? k[3] : 0.0)); };
        r = row(0); g = row(1); b = row(2); break; }
    case Adjust::GradientMap: {
        if(A.map.size() != 256) break;
        const QRgb c = A.map[qBound(0, int((0.2126*r + 0.7152*g + 0.0722*b) * 255.0 + 0.5), 255)];
        r = qRed(c) / 255.0; g = qGreen(c) / 255.0; b = qBlue(c) / 255.0; break; }
    case Adjust::Levels: {
        auto lv = [&](double v){
            // Measured on the reference ramp: the input range is NOT clamped before the output range, so
            // black 0.2 / white 0.8 / output 0.1-0.9 still reaches 0 and 255, and gamma is an exponent
            // on the normalised value inside [0,1] only (0.5 -> 104 with gamma 1.5).
            const double t = (v - A.a) / qMax(1e-6, A.b - A.a);
            const double g = (t > 0.0 && t < 1.0) ? std::pow(t, qMax(1e-3, A.c)) : t;
            return cl(A.d + (A.e - A.d) * g);
        };
        r = lv(r); g = lv(g); b = lv(b); break; }
    case Adjust::Curves:
        r = cl(A.chan[0].at(cl(A.master.at(r)))); g = cl(A.chan[1].at(cl(A.master.at(g)))); b = cl(A.chan[2].at(cl(A.master.at(b)))); break;
    case Adjust::HSL: {
        double h, s, l; rgb2hsl(r, g, b, h, s, l);
        h += A.a; h -= std::floor(h);                  // in turns: -0.25 takes red to violet, as rendered
        const double s0 = s;
        s = cl(s * (1.0 + A.b));
        l = l + A.c * (1.0 - s0 / 2.0);                // +0.1 lifts a grey by 0.1 but a pure hue by 0.05
        hsl2rgb(h, s, cl(l), r, g, b); break; }
    case Adjust::BC:
    {   // Fitted to the +20% / +40% reference ramp, not a known formula: brightness is a gamma lift, then
        // contrast pivots at mid-grey. Within about 5/255 of Affinity's own output on that ramp.
        const double ex = 1.0 / (1.0 + A.a * 1.67);
        auto bc = [&](double v){ return cl((std::pow(cl(v), ex) - 0.5) * A.b + 0.5); };
        r = bc(r); g = bc(g); b = bc(b); break; }
    case Adjust::BW: {
        // Weighted by hue sector: the distance from the smallest channel to the middle one takes the
        // secondary's weight, from the middle to the largest the primary's.
        const double mx = qMax(r, qMax(g, b)), mn = qMin(r, qMin(g, b)), md = r + g + b - mx - mn;
        const int pi = mx == r ? 0 : (mx == g ? 1 : 2);        // R G B
        const int si = mn == b ? 5 : (mn == r ? 3 : 4);        // Y C M
        const double v = cl(mn + (md - mn) * A.wt[si] + (mx - md) * A.wt[pi]);
        r = g = b = v; break; }
    case Adjust::Invert: r = 1.0 - r; g = 1.0 - g; b = 1.0 - b; break;
    case Adjust::Recolour: {
        double h, s, l; rgb2hsl(r, g, b, h, s, l);
        hsl2rgb(A.a, A.b, cl(l + A.c), r, g, b); break; }
    case Adjust::Selective: {
        const double mx = qMax(r, qMax(g, b)), mn = qMin(r, qMin(g, b)), md = r + g + b - mx - mn;
        const double chroma[3] = { mx - md, md - mn, 0.0 };
        double w[9] = {0};
        if(mx == r) w[0] = chroma[0]; if(mx == g) w[2] = chroma[0]; if(mx == b) w[4] = chroma[0];
        if(mn == b) w[1] = chroma[1]; if(mn == r) w[3] = chroma[1]; if(mn == g) w[5] = chroma[1];
        w[6] = qMax(0.0, 2.0 * mn - 1.0); w[8] = qMax(0.0, 1.0 - 2.0 * mx); w[7] = 1.0 - qAbs(mx + mn - 1.0);
        double dc = 0, dm = 0, dy = 0, dk = 0;
        for(int i = 0; i < 9; ++i){ dc += w[i]*A.sel[i*4]; dm += w[i]*A.sel[i*4+1]; dy += w[i]*A.sel[i*4+2]; dk += w[i]*A.sel[i*4+3]; }
        // Relative mode scales the ink already there, so a saturated primary barely moves.
        auto ink = [&](double v, double dv){ const double i = 1.0 - v; return cl(1.0 - (A.relative ? i * (1.0 + dv + dk) : i + dv + dk)); };
        r = ink(r, dc); g = ink(g, dm); b = ink(b, dy); break; }
    }
}

// sRGB <-> other interpolation spaces. A document's own colour format decides how its gradients blend.
double srgbToLin(double v){ return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); }
double linToSrgb(double v){ v = qBound(0.0, v, 1.0); return v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1.0/2.4) - 0.055; }

QString affPathData(const QPainterPath& p);
QImage rasterImage(const AffArchive& ar, const AffTree& tr, const AffNode& dybm, i64& pixelBudget, QPoint* originOut);

// LSty.Data is a 12-byte record: f64 miter limit, then cap, join and line style. The style byte decides
// whether a stroke exists at all: 0 none, 1 solid, 2 dashed, 3 brush.
struct LineData { bool valid = false; int cap = 0, join = 0, style = 1; double miter = 2.0; };
LineData lineData(const AffNode& lsty){
    LineData d;
    const AffField* f = fieldOf(lsty, TG('D','a','t','a'));
    if(!f || f->bytes.size() < 11) return d;
    const u8* b = reinterpret_cast<const u8*>(f->bytes.constData());
    double m; memcpy(&m, b, 8);
    d.valid = true; d.miter = finite(m); d.cap = b[8]; d.join = b[9]; d.style = b[10];
    return d;
}

// A layer's enabled effects (FiEf) and live blur filter, as far as each maps onto something native.
struct LayerFx {
    bool shadow = false, glow = false, bevel = false, inner = false, innerGlow = false, outline = false, colour = false;
    double shX = 0, shY = 0, shR = 0, glR = 0, glComp = 1, igComp = 1, blur = 0;
    QColor shC, glC, bvHi, bvLo;
    QString glBlend, bvHiBlend, bvLoBlend;
    double bvRad = 0, bvAzim = 135, bvOp = 0.75, bvShOp = 0.75, phAmb = 1, phDiff = 0, phSpec = 0, phExp = 1, phElev = 45;
    bool pillow = false, phong = false, bvOuter = false, bvInner = true;
    double inX = 0, inY = 0, inR = 0, igR = 0, olR = 0, coOp = 1;
    QColor inC, igC, olC, coC;
    QString inBlend, igBlend, coBlend, grBlend;
    int grFill = -1; double grOp = 1;
    double noise = 0;
};

struct TxStyle { double size = 12.0; QString family; int weight = 400; bool italic = false; QColor col = Qt::black; double track = 0.0; };

struct Paint {
    enum Kind { None, Solid, Gradient, Bitmap } kind = None;
    QColor col;
    QString url;
    QString bitmapAttrs;   // xarq:bitmap* attributes for a texture fill
    double alpha = 1.0;
};

class Converter {
public:
    Converter(const AffArchive& a, const AffTree& t) : m_ar(a), m_tr(t) {}
    QStringList run(AffinityStats* stats);

private:
    const AffArchive& m_ar;
    const AffTree& m_tr;
    QString m_defs, m_body;
    int m_gradSeq = 0, m_depth = 0, m_clipSeq = 0;
    QString m_idPrefix;
    QHash<QString,QString> m_gradCache;   // one <linearGradient> per (gradient node, transform) per page
    i64 m_imageBytes = 0;
    i64 m_rasterPixels = kMaxRasterTot;   // remaining raster canvas for this document
    struct CachedRaster { QPoint origin; QSize size; QString b64; };
    QHash<int,CachedRaster> m_rasterCache;   // one assembled+encoded image per DyBm node
    qsizetype m_parked = 0;
    AffinityStats m_st;
    QSet<int> m_onPath;

    const AffNode* node(int i) const { return m_tr.node(i); }
    // m_parked carries the real body while maskGeometry borrows m_body for clip geometry, so the
    // cap still measures the whole document instead of just the clip buffer.
    bool budget() const { return m_body.size() + m_defs.size() + m_parked < kMaxSvg && m_st.shapes < kMaxShapes; }

    void walk(int idx);
    void shapeNode(const AffNode& n);
    void curveNode(const AffNode& n);
    void textNode(const AffNode& n, QString* outlines = nullptr);
    TxStyle runStyle(const AffNode& item, const TxStyle& base);
    QString blockText(const AffNode& glyp) const;
    QString curveData(const AffNode& pcvd) const;
    int m_curPage = 0;
    QString m_fileName;
    void imageNode(const AffNode& n);
    void embedNode(const AffNode& n);
    int  firstSpread(QString& body, QString& defs);
    int m_nest = 0, m_embedSeq = 0;
    bool placedOriginal(const AffNode& bm);
    void rasterNode(const AffNode& n);
    void compoundNode(const AffNode& n);
    void fillLayerNode(const AffNode& n);
    QPainterPath compoundOutline(const AffNode& n, int depth);
    QHash<int, int> m_flowLines;       // linked text frames: lines of a story already placed
    int m_compoundVisits = 0;          // across the document: nodes are shareable, so depth alone doesn't bound this
    QString pixelMaskAttrs(const AffNode& n);
    QString maskGeometry(const AffNode& n);
    void spreadBox(const AffNode& sp, const AffNode& doc, double& px, double& py, double& pw, double& ph) const;

    bool colourOf(int idx, QColor& out) const;
    bool rawColourOf(int idx, QColor& out) const;
    QColor docColour(const QColor& c, u32 cls) const;
    bool readAdjust(const AffNode& n, Adjust& out) const;
    void walkChildren(const AffField& ch, int selfIdx);
    QString eraseClip(const QVector<int>& erasers);
    int m_docFmt = 0;
    double m_ptScale = 1.0;      // stroke weights are in points; the page is in pixels at UVCn.UPPI
    QVector<Adjust> m_adj;
    QString m_adjKey;
    Paint paintOf(const AffNode& n, u32 tag);
    Paint paintAt(int fdscOrFillIdx);
    Paint paintFromFill(int fillIdx, const double* xf, bool hasXf);
    int   slotKid(const AffNode& n, u32 listTag, u32 indexTag) const;
    QString gradientDef(int gradIdx, int type, const double* xf, bool hasXf, QColor* singleOut);
    QString styleFor(const AffNode& n, int fillSlot = -1, int strokeSlot = -1);
    void    emitPainted(const AffNode& n, const QString& element);
    void    emitPaintStack(const AffNode& n, const QString& element);
    LayerFx readFx(const AffNode& n);
    QString fxShapeAttrs() const;
    QString pressureAttr(const AffNode& ldsc) const;
    LayerFx m_fx;
    QString transpAttr(const AffNode& n);
    QPainterPath elementPath(const QString& element) const;
    void put(const QString& s){ if(budget()) m_body += s; }
};

// What a stored colour looks like in this document, and after the adjustments in force. Colours are kept
// as authored; a greyscale, CMYK or linear document shows them through its own space. Pinned against
// the 12-* charts' own renders:
//   DDRF 2 grey: Y from linear sRGB with D50 weights, re-encoded at gamma 2.2 (red -> 129, exactly).
//   DDRF 4 CMYK: through the print gamut. No profile bytes are stored for it in these files, so this is
//     a trilinear table over the RGB cube, whose corners are what SWOP v2 renders them as.
//   DDRF 9 RGB/32: the stored values are LINEAR; encode to sRGB (0.905 -> 244, the page colour).
QColor Converter::docColour(const QColor& c, u32 cls) const {
    double r = c.redF(), g = c.greenF(), b = c.blueF();
    const bool ink = cls == TG('C','M','Y','K');
    if(!ink){
        if(m_docFmt == 9){ r = linToSrgb(r); g = linToSrgb(g); b = linToSrgb(b); }
        else if(m_docFmt == 2){
            const double y = 0.2225 * srgbToLin(r) + 0.7169 * srgbToLin(g) + 0.0606 * srgbToLin(b);
            r = g = b = std::pow(qBound(0.0, y, 1.0), 1.0 / 2.2);
        } else if(m_docFmt == 4){
            static const double K[8][3] = { {35,31,32}, {58,83,164}, {106,189,69}, {111,204,220}, {237,30,36}, {186,81,158}, {247,236,19}, {255,255,255} };
            double o[3] = {0,0,0};
            for(int i = 0; i < 8; ++i){
                const double w = ((i & 4) ? r : 1-r) * ((i & 2) ? g : 1-g) * ((i & 1) ? b : 1-b);
                for(int k = 0; k < 3; ++k) o[k] += w * K[i][k] / 255.0;
            }
            // A trilinear blend of eight saturated corners goes brown along the grey axis; a neutral stays
            // neutral in print, so fade towards the K-to-paper ramp as the input loses chroma.
            const double mx = qMax(r, qMax(g, b)), mn = qMin(r, qMin(g, b));
            const double sat = qBound(0.0, (mx - mn) * 3.0, 1.0);
            const double lum = (r + g + b) / 3.0;
            const double nr = (35.0 + 220.0*lum)/255.0, ng = (31.0 + 224.0*lum)/255.0, nb = (32.0 + 223.0*lum)/255.0;
            r = nr + (o[0]-nr)*sat; g = ng + (o[1]-ng)*sat; b = nb + (o[2]-nb)*sat;
        }
    }
    for(const Adjust& a : m_adj) applyAdjust(a, r, g, b);
    return QColor::fromRgbF(qBound(0.0,r,1.0), qBound(0.0,g,1.0), qBound(0.0,b,1.0), c.alphaF());
}

bool Converter::colourOf(int idx, QColor& out) const {
    if(!rawColourOf(idx, out)) return false;
    const AffNode* n = node(idx);
    out = docColour(out, n && !n->types.isEmpty() ? n->types[0] : 0);
    return true;
}

bool Converter::rawColourOf(int idx, QColor& out) const {
    const AffNode* n = node(idx);
    if(!n || n->types.isEmpty()) return false;
    const u32 cls = n->types[0];
    const AffField* f = fieldOf(*n, TG('_','c','o','l'));
    if(!f) return false;
    const QByteArray& b = f->bytes;
    auto F = [&](int i)->double {
        if(qsizetype(i+1)*4 > b.size()) return 0.0;
        const u32 raw = rdu32(reinterpret_cast<const u8*>(b.constData()) + i*4);
        float v; memcpy(&v, &raw, 4);
        return std::isfinite(v) ? double(v) : 0.0;
    };
    auto C = [](double v){ return qBound(0.0, v, 1.0); };
    if(cls == TG('R','G','B','A') && b.size() >= 16){ out = QColor::fromRgbF(C(F(0)), C(F(1)), C(F(2)), C(F(3))); return true; }
    if(cls == TG('H','S','L','A') && b.size() >= 16){
        double h = F(0); h = std::isfinite(h) ? h - std::floor(h) : 0.0;
        out = QColor::fromHslF(qBound(0.0, h, 0.99999), C(F(1)), C(F(2)), C(F(3)));
        return true;
    }
    if(cls == TG('C','M','Y','K') && b.size() >= 20){
        const double c = C(F(0)), m = C(F(1)), y = C(F(2)), k = C(F(3));
        // Neugebauer over the SWOP solids rather than (1-c)(1-k), which puts 100% magenta at (255,0,255)
        // where print shows (236,0,140).
        static const double P[8][3] = { {255,255,255}, {255,242,0}, {236,0,140}, {237,28,36}, {0,174,239}, {0,166,81}, {46,49,146}, {54,54,57} };
        double o[3] = {0,0,0};
        for(int i = 0; i < 8; ++i){
            const double wg = ((i & 4) ? c : 1-c) * ((i & 2) ? m : 1-m) * ((i & 1) ? y : 1-y);
            for(int q = 0; q < 3; ++q) o[q] += wg * P[i][q] / 255.0;
        }
        const double kk[3] = { 35/255.0, 31/255.0, 32/255.0 };
        for(int q = 0; q < 3; ++q) o[q] = o[q] * (1.0 - k) + o[q] * kk[q] * k;
        out = QColor::fromRgbF(C(o[0]), C(o[1]), C(o[2]), C(F(4)));
        return true;
    }
    if(cls == TG('G','R','A','Y') && b.size() >= 8){ const double g = C(F(0)); out = QColor::fromRgbF(g, g, g, C(F(1))); return true; }
    if(cls == TG('L','A','B','A') && b.size() >= 8){
        const u8* q = reinterpret_cast<const u8*>(b.constData());
        const double L = rdu16(q)   / 65535.0 * 100.0;
        const double A = rdu16(q+2) / 65535.0 * 255.0 - 128.0;
        const double B = rdu16(q+4) / 65535.0 * 255.0 - 128.0;
        out = labToRgb(L, A, B, rdu16(q+6) / 65535.0);
        return true;
    }
    return false;
}

QString Converter::gradientDef(int gradIdx, int type, const double* xf, bool hasXf, QColor* singleOut){
    const AffNode* g = node(gradIdx);
    if(!g) return QString();
    // One <linearGradient> per (gradient node, transform): a shared FilS/FilG is referenced by many
    // shapes, and re-emitting a def per reference would bloat the SVG on real documents.
    const QString key = QStringLiteral("%1%2|%3|%4").arg(m_adjKey).arg(gradIdx).arg(type)
                            .arg(hasXf && xf ? affMatrix(xf) : QString());
    const auto cached = m_gradCache.constFind(key);
    if(cached != m_gradCache.constEnd()) return *cached;
    const AffField* posn = fieldOf(*g, TG('P','o','s','n'));
    const AffField* cols = fieldOf(*g, TG('C','o','l','s'));
    if(!cols || cols->kids.isEmpty()) return QString();

    // Posn is a vec2 PER STOP: [position, midpoint]. The midpoint is where the blend to the NEXT
    // stop is 50/50 — 0.5 is a straight ramp, and anything else bends it. Real documents lean on
    // this hard (real2.afphoto's background uses 0.000381, which turns a purple->black ramp into
    // black with a small purple glow; read as a straight ramp it washes the whole panel purple).
    QVector<QPair<double,QColor>> stops;
    QVector<double> mids;
    QVector<QPair<double,QColor>> rawByOff;
    const int n = cols->kids.size();
    for(int i = 0; i < n && stops.size() < 512; ++i){
        QColor c, rc;
        if(!colourOf(cols->kids[i], c)) continue;
        rawColourOf(cols->kids[i], rc);
        double off = (n > 1) ? double(i) / double(n - 1) : 0.0;
        double mid = 0.5;
        if(posn && posn->comps >= 2 && qsizetype(i)*posn->comps < posn->num.size()){
            off = posn->num[qsizetype(i) * posn->comps];
            if(qsizetype(i)*posn->comps + 1 < posn->num.size())
                mid = qBound(0.001, finite(posn->num[qsizetype(i) * posn->comps + 1]), 0.999);
        }
        stops.push_back({ qBound(0.0, finite(off), 1.0), c });
        mids.push_back(mid);
        rawByOff.push_back({ qBound(0.0, finite(off), 1.0), rc });
    }
    if(stops.isEmpty()) return QString();
    if(stops.size() == 1){ if(singleOut) *singleOut = stops[0].second; return QString(); }
    if(type == 3){
        // Affinity sweeps a conical gradient clockwise on screen, QConicalGradient counter-clockwise.
        // Mirror the ramp; the midpoint belongs to the segment after a stop, so it moves one along.
        QVector<QPair<double,QColor>> rs; QVector<double> rm;
        for(int i = stops.size() - 1; i >= 0; --i){
            rs.push_back({ 1.0 - stops[i].first, stops[i].second });
            rm.push_back(i > 0 && i - 1 < mids.size() ? 1.0 - mids[i - 1] : 0.5);
        }
        stops = rs; mids = rm;
    }
    std::stable_sort(stops.begin(), stops.end(),
                     [](const QPair<double,QColor>& a, const QPair<double,QColor>& b){ return a.first < b.first; });

    if(m_defs.size() >= kMaxSvg) return QString();      // out of budget: let the caller fall back to a flat colour
    const QString id = QStringLiteral("%1afg%2").arg(m_idPrefix).arg(m_gradSeq++);
    const bool radial = (type == 1 || type == 2);
    const bool conical = (type == 3);                   // SVG has no conical gradient; XarQ's model does
    QString d = radial
        ? QStringLiteral("<radialGradient id=\"%1\" gradientUnits=\"userSpaceOnUse\" cx=\"0\" cy=\"0\" r=\"1\"").arg(id)
        : QStringLiteral("<linearGradient id=\"%1\" gradientUnits=\"userSpaceOnUse\" x1=\"0\" y1=\"0\" x2=\"1\" y2=\"0\"%2").arg(id, conical ? QStringLiteral(" xarq:conical=\"1\"") : QString());
    if(hasXf) d += QStringLiteral(" gradientTransform=\"%1\"").arg(affMatrix(xf));
    d += QLatin1Char('>');
    // SVG has no midpoint, so a bent segment is approximated by sampling it. The curve that puts the
    // 50/50 blend at m is t' = t^(ln 0.5 / ln m), which is exactly linear when m == 0.5 — so a
    // straight segment still emits its two endpoints and nothing more, and existing output is
    // unchanged for every document that does not use the feature.
    auto emitStop = [&](double off, const QColor& c){
        d += QStringLiteral("<stop offset=\"%1\" stop-color=\"%2\" stop-opacity=\"%3\"/>")
                .arg(fnum(qBound(0.0, off, 1.0)), colHex(c), fnum(c.alphaF()));
    };
    QVector<QColor> rawStops;
    std::stable_sort(rawByOff.begin(), rawByOff.end(), [](const QPair<double,QColor>& x, const QPair<double,QColor>& y){ return x.first < y.first; });
    if(type == 3) std::reverse(rawByOff.begin(), rawByOff.end());
    for(const auto& r : rawByOff) rawStops.push_back(r.second);
    for(int i = 0; i < stops.size(); ++i){
        emitStop(stops[i].first, stops[i].second);
        if(i + 1 >= stops.size()) continue;
        const double m = (i < mids.size()) ? mids[i] : 0.5;
        const double p0 = stops[i].first, p1 = stops[i+1].first;
        // A straight sRGB segment needs nothing between its ends. Otherwise sample: for a bent midpoint,
        // for an adjustment (not linear between stops), or because the document blends in another space.
        const bool sampled = !m_adj.isEmpty() || m_docFmt == 9 || m_docFmt == 5;
        if((qAbs(m - 0.5) < 0.002 && !sampled) || p1 - p0 < 1e-6) continue;
        const double e = qAbs(m - 0.5) < 0.002 ? 1.0 : std::log(0.5) / std::log(m);
        if(!std::isfinite(e) || e <= 0.0) continue;
        const QColor& ca = rawStops[i]; const QColor& cb = rawStops[i+1];
        const int steps = sampled ? 24 : 12;
        for(int k = 1; k < steps; ++k){
            const double t = double(k) / double(steps);
            const double u = std::pow(t, e);                       // colour parameter at distance t
            double rr, gg, bb;
            if(m_docFmt == 5){                                     // CIELAB: blend in Lab
                double La, Aa, Ba, Lb, Ab, Bb;
                rgbToLab(ca, La, Aa, Ba); rgbToLab(cb, Lb, Ab, Bb);
                const QColor q = labToRgb(La + u*(Lb-La), Aa + u*(Ab-Aa), Ba + u*(Bb-Ba), 1.0);
                rr = q.redF(); gg = q.greenF(); bb = q.blueF();
            } else {
                rr = ca.redF() + u*(cb.redF()-ca.redF()); gg = ca.greenF() + u*(cb.greenF()-ca.greenF()); bb = ca.blueF() + u*(cb.blueF()-ca.blueF());
            }
            const QColor q = docColour(QColor::fromRgbF(qBound(0.0,rr,1.0),qBound(0.0,gg,1.0),qBound(0.0,bb,1.0)), 0);
            QColor out(q); out.setAlphaF(qBound(0.0, ca.alphaF() + u*(cb.alphaF()-ca.alphaF()), 1.0));
            emitStop(p0 + t * (p1 - p0), out);
        }
    }
    d += radial ? QStringLiteral("</radialGradient>") : QStringLiteral("</linearGradient>");
    m_defs += d;
    m_gradCache.insert(key, id);
    return id;
}

Paint Converter::paintFromFill(int fillIdx, const double* xf, bool hasXf){
    Paint p;
    const AffNode* f = node(fillIdx);
    if(!f || f->types.isEmpty()) return p;
    const u32 cls = f->types[0];
    if(cls == TG('F','i','l','S')){
        QColor c;
        if(colourOf(firstKid(*f, TG('C','o','l','r')), c)){ p.kind = Paint::Solid; p.col = c; p.alpha = c.alphaF(); }
        return p;
    }
    if(cls == TG('F','i','l','G')){
        const AffField* ty = fieldOf(*f, TG('T','y','p','e'));
        const int type = ty && !ty->num.isEmpty() ? int(qBound(0.0, ty->num[0], 64.0)) : 0;
        QColor single;
        const QString id = gradientDef(firstKid(*f, TG('G','r','a','d')), type, xf, hasXf, &single);
        if(!id.isEmpty()){
            p.kind = Paint::Gradient; p.url = id;
            if(const AffNode* g = node(firstKid(*f, TG('G','r','a','d'))))
                if(const AffField* cs = fieldOf(*g, TG('C','o','l','s')))
                    if(!cs->kids.isEmpty()) colourOf(cs->kids[0], p.col);   // what a gradient STROKE flattens to
            return p;
        }
        if(single.isValid()){ p.kind = Paint::Solid; p.col = single; p.alpha = single.alphaF(); }
        return p;
    }
    if(cls == TG('F','i','l','B')){
        // A bitmap fill: the texture is an ordinary DyBm, FDeX places it by centre and half-extent (it maps
        // [-1,1]², so a 32 px tile with a 130 scale spans 260 units), and ExtT 4 is the repeating extend
        // mode (the only one the samples use).
        const AffNode* bm = node(firstKid(*f, TG('B','i','t','m')));
        if(!bm) return p;
        QPoint origin(0, 0);
        // A placed texture keeps its file in the container (Bckg); only an edited one has real tiles.
        QImage img;
        if(const AffField* bg = fieldOf(*bm, TG('B','c','k','g'))){
            if(!bg->bytes.isEmpty() && bg->bytes.size() <= 256){
                AffTree bt(namedStream(m_ar, QString::fromUtf8(bg->bytes)));
                if(bt.load() && bt.root >= 0)
                    if(const AffNode* r0 = bt.node(bt.root))
                        if(const AffField* dat = fieldOf(*r0, TG('D','a','t','a')))
                            if(dat->bytes.size() <= kMaxImageBytes) img = QImage::fromData(dat->bytes);
            }
        }
        if(img.isNull()) img = rasterImage(m_ar, m_tr, *bm, m_rasterPixels, &origin);
        if(img.isNull() || !hasXf) return p;
        QByteArray png; QBuffer buf(&png);
        if(!buf.open(QIODevice::WriteOnly) || !img.save(&buf, "PNG")) return p;
        if(png.size() > kMaxImageBytes || m_imageBytes + png.size() > kMaxImageTotal) return p;
        m_imageBytes += png.size();
        const AffField* ex = fieldOf(*f, TG('E','x','t','T'));
        const bool tiled = ex && !ex->num.isEmpty() && ex->num[0] == 4.0;
        p.kind = Paint::Bitmap;
        p.bitmapAttrs = QStringLiteral(" xarq:bitmap=\"data:image/png;base64,%1\" xarq:bitmap-xf=\"%2 %3 %4 %5 %6 %7\" xarq:bitmap-tiled=\"%8\"")
            .arg(QString::fromLatin1(png.toBase64()))
            // The model's p2 is the image's TOP edge, and FDeX's +y runs down the image.
            .arg(fnum(2.0*xf[0]), fnum(2.0*xf[3]), fnum(-2.0*xf[1]), fnum(-2.0*xf[4]), fnum(xf[2] - xf[0] + xf[1]), fnum(xf[5] - xf[3] + xf[4]))
            .arg(tiled ? 1 : 0);
        return p;
    }
    return p;                                          // FilN (none) and friends
}

// One element of a multi-slot field (Affinity 2.x keeps fills and lines in arrays with a companion
// "current slot" index: BFFl/BFCr for the fill, LIFl+LILn/LICr for the stroke).
int Converter::slotKid(const AffNode& n, u32 listTag, u32 indexTag) const {
    const AffField* f = fieldOf(n, listTag);
    if(!f || f->kids.isEmpty()) return -1;
    int want = 0;
    if(const AffField* ix = fieldOf(n, indexTag))
        if(!ix->num.isEmpty()) want = int(qBound(0.0, finite(ix->num[0]), 4096.0));
    if(want >= 0 && want < f->kids.size() && f->kids[want] >= 0) return f->kids[want];
    for(int c : f->kids) if(c >= 0) return c;
    return -1;
}

// BFil is the FILL and PFil is the STROKE colour; both are Fill objects and look identical in a dump —
// reading PFil as the fill renders every document in its outline colour. Either may instead be an FDsc
// wrapper carrying the fill (FDeF) plus its gradient transform (FDeX).
Paint Converter::paintAt(int k){
    const AffNode* w = node(k);
    if(!w || w->types.isEmpty()) return Paint{};
    if(w->types[0] == TG('F','D','s','c')){
        const AffField* x = fieldOf(*w, TG('F','D','e','X'));
        const bool hasXf = x && x->num.size() >= 6;
        double xf[6] = {1,0,0,0,1,0};
        if(hasXf) for(int i = 0; i < 6; ++i) xf[i] = finite(x->num[i]);
        return paintFromFill(firstKid(*w, TG('F','D','e','F')), xf, hasXf);
    }
    return paintFromFill(k, nullptr, false);
}

Paint Converter::paintOf(const AffNode& n, u32 tag){
    const AffField* f = fieldOf(n, tag);
    if(!f) return Paint{};
    int k = -1;
    for(int c : f->kids) if(c >= 0){ k = c; break; }
    return paintAt(k);
}

// fill / stroke / stroke-width for one drawable, as SVG presentation attributes. Every attribute is
// written exactly once — a duplicate attribute name is a hard XML error in the consuming parser.
// LDeP: the pressure curve, cubic records over x = position along the stroke, y = width multiplier.
// A level curve is what every stroke without pressure carries, and says nothing.
QString Converter::pressureAttr(const AffNode& ldsc) const {
    const AffNode* cd = node(firstKid(ldsc, TG('L','D','e','P')));
    const AffNode* data = cd ? node(firstKid(*cd, TG('D','a','t','a'))) : nullptr;
    if(!data || data->fields.size() < 3) return {};
    const AffField& fp = data->fields[2];
    if(fp.recSize != 18 || fp.count < 4 || qsizetype(fp.count) * 18 > fp.bytes.size()) return {};
    const u8* rec = reinterpret_cast<const u8*>(fp.bytes.constData());
    auto at = [&](int i, int ofs){ u64 b = rdu64(rec + qsizetype(i)*18 + ofs); double v; memcpy(&v, &b, 8); return finite(v); };
    QStringList out;
    bool level = true;
    double lastX = -1.0;
    auto add = [&](double x, double y){
        x = qBound(0.0, x, 1.0); y = qBound(0.0, y, 16.0);
        // A curve may open with a vertical run at x = 0: the value leaving it is the one that counts.
        if(x <= lastX){ if(!out.isEmpty() && x == lastX){ out.last() = fnum(x) + QLatin1Char(':') + fnum(y); } return; }
        lastX = x;
        if(qAbs(y - 1.0) > 1e-3) level = false;
        out << fnum(x) + QLatin1Char(':') + fnum(y);
    };
    add(at(0, 0), at(0, 8));
    for(int j = 0; j + 3 < fp.count && j < 3000; j += 3)
        for(int k = 1; k <= 8; ++k){
            const double t = k / 8.0, u = 1.0 - t, a = u*u*u, b = 3*u*u*t, c = 3*u*t*t, d = t*t*t;
            add(a*at(j,0) + b*at(j+1,0) + c*at(j+2,0) + d*at(j+3,0), a*at(j,8) + b*at(j+1,8) + c*at(j+2,8) + d*at(j+3,8));
        }
    if(level || out.size() < 2) return {};
    return QStringLiteral(" xarq:pressure=\"%1\"").arg(out.join(QLatin1Char(',')));
}

QString Converter::styleFor(const AffNode& n, int fillSlot, int strokeSlot){
    QString out;
    auto kidAt = [&](u32 tag, int i){
        const AffField* f = fieldOf(n, tag);
        return (f && i >= 0 && i < f->kids.size()) ? f->kids[i] : -1;
    };
    // Affinity 1.x names them BFil/PFil/LSty; 2.x moved to indexed lists BFFl/LIFl/LILn (+BFCr/LICr).
    // A slot of -1 means "the current one", -2 means "this element does not draw that paint".
    Paint fill;
    if(fillSlot >= 0) fill = paintAt(kidAt(TG('B','F','F','l'), fillSlot));
    else if(fillSlot == -1){
        fill = paintOf(n, TG('B','F','i','l'));
        if(fill.kind == Paint::None) fill = paintAt(slotKid(n, TG('B','F','F','l'), TG('B','F','C','r')));
    }
    // FOpc is a SEPARATE fill-opacity control (as in the Affinity UI); it multiplies the fill's own alpha.
    double fillAlpha = 1.0;
    const AffField* fo = fieldOf(n, TG('F','O','p','c'));
    if(fo && !fo->num.isEmpty()) fillAlpha = qBound(0.0, finite(fo->num[0]), 1.0);
    if(fill.kind == Paint::Solid){
        out += QStringLiteral(" fill=\"%1\"").arg(colHex(fill.col));
        fillAlpha *= fill.col.alphaF();
    } else if(fill.kind == Paint::Gradient){
        out += QStringLiteral(" fill=\"url(#%1)\"").arg(fill.url);
    } else if(fill.kind == Paint::Bitmap){
        out += QStringLiteral(" fill=\"#808080\"") + fill.bitmapAttrs;
    } else {
        out += QStringLiteral(" fill=\"none\"");
        fillAlpha = 1.0;
    }
    if(fill.kind != Paint::None && fillAlpha < 0.999)
        out += QStringLiteral(" fill-opacity=\"%1\"").arg(fnum(fillAlpha));

    Paint stroke;
    if(strokeSlot >= 0) stroke = paintAt(kidAt(TG('L','I','F','l'), strokeSlot));
    else if(strokeSlot == -1){
        stroke = paintOf(n, TG('P','F','i','l'));
        if(stroke.kind == Paint::None) stroke = paintAt(slotKid(n, TG('L','I','F','l'), TG('L','I','C','r')));
    }
    if(stroke.kind == Paint::Gradient){ stroke.kind = Paint::Solid; }   // the model's Stroke is one flat colour
    if(stroke.kind == Paint::Solid && stroke.col.isValid()){
        // No LSty means no outline at all: a shape can carry a stroke COLOUR (PFil) with the stroke
        // switched off, and the four quadrants of color.afdesign prove it renders unstroked.
        double w = 0.0; bool brush = false;
        QString pressure;
        LineData ld;
        int ls = firstKid(n, TG('L','S','t','y'));
        if(ls < 0) ls = strokeSlot >= 0 ? kidAt(TG('L','I','L','n'), strokeSlot) : slotKid(n, TG('L','I','L','n'), TG('L','I','C','r'));
        if(const AffNode* l = node(ls)){
            if(!l->types.isEmpty() && l->types[0] == TG('L','D','s','c')){
                ls = firstKid(*l, TG('L','D','e','L'));
                pressure = pressureAttr(*l);
            }
        }
        if(const AffNode* l = node(ls)){
            const AffField* wg = fieldOf(*l, TG('W','g','h','t'));
            if(wg && !wg->num.isEmpty()) w = finite(wg->num[0]) * m_ptScale;
            brush = firstKid(*l, TG('B','r','u','s')) >= 0;
            ld = lineData(*l);
        }
        w = qBound(0.0, w, 1e6);
        if(ld.valid && ld.style == 0) w = 0.0;          // a weight with line style "none" draws nothing
        if(w > 0.0){
            out += (stroke.kind == Paint::Gradient)
                 ? QStringLiteral(" stroke=\"url(#%1)\"").arg(stroke.url)
                 : QStringLiteral(" stroke=\"%1\"").arg(colHex(stroke.col));
            if(stroke.kind == Paint::Solid && stroke.col.alphaF() < 0.999)
                out += QStringLiteral(" stroke-opacity=\"%1\"").arg(fnum(stroke.col.alphaF()));
            out += QStringLiteral(" stroke-width=\"%1\"").arg(fnum(w)) + pressure;
            // LSty.Data is the authority on cap, join and dash. CnCp/CnJn are only consulted without it.
            const AffField* cp = fieldOf(n, TG('C','n','C','p'));
            const AffField* cj = fieldOf(n, TG('C','n','J','n'));
            const int capId = ld.valid ? ld.cap  : (cp && !cp->num.isEmpty() ? int(qBound(0.0, cp->num[0], 8.0)) : (brush ? 1 : -1));
            const int jnId  = ld.valid ? ld.join : (cj && !cj->num.isEmpty() ? int(qBound(0.0, cj->num[0], 8.0)) : (brush ? 1 : -1));
            if(capId == 1) out += QStringLiteral(" stroke-linecap=\"round\"");
            else if(capId == 2) out += QStringLiteral(" stroke-linecap=\"square\"");
            if(jnId == 0) out += QStringLiteral(" stroke-linejoin=\"bevel\"");
            else if(jnId == 1) out += QStringLiteral(" stroke-linejoin=\"round\"");
            else if(jnId == 2) out += QStringLiteral(" stroke-linejoin=\"miter\"");
            if(ld.valid && ld.style == 2)
                out += QStringLiteral(" stroke-dasharray=\"%1 %1\"").arg(fnum(w * 2.0));
        }
    }
    return out;
}

// PCrv → Crvs (PCvD) → Data: a positional struct of u8, u32 subpath-count, then per subpath a bool
// "closed" and an array of 18-byte records (double x, double y, u8 handle-count, u8 role). The list is
// a flat cubic chain A,(out,in,A)…; a closed subpath repeats its first anchor as the last one, so the
// closing segment carries real handles and the flag only asks for a "Z".
// Corner Tool data. CnrD: record index of each node, radius, type: 1 rounded, 2 concave, 3 straight,
// 4 inset. The stored curve stays the original sharp one, so applying this is on the reader. Only a corner
// between straight segments is rebuilt; a curved one keeps its sharp node.
static QString cornerPath(const u8* rec, int cnt, bool closed, const QHash<int,QPair<double,int>>& corners){
    auto P = [&](int i){ u64 x = rdu64(rec + qsizetype(i)*18), y = rdu64(rec + qsizetype(i)*18 + 8); double a, b; memcpy(&a,&x,8); memcpy(&b,&y,8); return QPointF(finite(a), finite(b)); };
    const int nodes = (cnt - 1) / 3;
    const int unique = closed ? nodes : nodes + 1;
    if(unique < 2) return {};
    auto anchor = [&](int k){ return P((k % nodes) * 3); };
    auto straightOut = [&](int k){ return P(k*3) == P(k*3+1) && P(k*3+2) == P(k*3+3); };
    QPainterPath pp;
    for(int k = 0; k < unique; ++k){
        const QPointF a = anchor(k);
        const auto it = corners.constFind(k * 3);
        const bool hasPrev = closed || k > 0, hasNext = closed || k + 1 < unique;
        const double r = it != corners.constEnd() ? it->first : 0.0;
        const int type = it != corners.constEnd() ? it->second : 0;
        const int kp = (k + nodes - 1) % nodes;
        const bool ok = r > 0.0 && hasPrev && hasNext && straightOut(kp) && straightOut(k % nodes);
        QPointF in = a, out = a;
        if(ok){
            const QPointF pv = anchor(kp), nx = anchor(k + 1);
            const double lp = QLineF(a, pv).length(), ln = QLineF(a, nx).length();
            if(lp > 1e-9 && ln > 1e-9){
                const QPointF up = (pv - a) / lp, un = (nx - a) / ln;
                const double cosA = qBound(-1.0, up.x()*un.x() + up.y()*un.y(), 1.0);
                const double ang = std::acos(cosA);
                double cut = r;
                if(type == 1 && ang > 1e-3 && ang < M_PI - 1e-3) cut = r / std::tan(ang / 2.0);
                cut = qMin(cut, qMin(lp, ln) * 0.5);
                in = a + up * cut; out = a + un * cut;
                k == 0 ? pp.moveTo(in) : pp.lineTo(in);
                if(type == 1 && ang > 1e-3 && ang < M_PI - 1e-3){
                    const double kk = 4.0/3.0 * std::tan((M_PI - ang) / 4.0) * (cut * std::tan(ang / 2.0));
                    pp.cubicTo(in - up * kk, out - un * kk, out);
                } else if(type == 2){
                    // A bite of radius r centred on the corner itself.
                    const double kk = 4.0/3.0 * std::tan(ang / 4.0) * cut;
                    const QPointF tin(-up.y(), up.x()), tout(-un.y(), un.x());
                    const double sgn = (un.x()*up.y() - un.y()*up.x()) > 0 ? -1.0 : 1.0;
                    pp.cubicTo(in + tin * (kk * sgn), out - tout * (kk * sgn), out);
                } else if(type == 4){
                    pp.lineTo(in + un * cut); pp.lineTo(out);
                } else pp.lineTo(out);
                continue;
            }
        }
        if(k == 0) pp.moveTo(a);
        else {
            const int s0 = (k - 1) % nodes;
            if(straightOut(s0)) pp.lineTo(a); else pp.cubicTo(P(s0*3+1), P(s0*3+2), a);
        }
    }
    if(closed) pp.closeSubpath();
    return affPathData(pp) + (closed ? QStringLiteral("Z") : QString());
}

// The SVG path of a PCvD, for text on a path.
QString Converter::curveData(const AffNode& pcvd) const {
    const AffNode* data = node(firstKid(pcvd, TG('D','a','t','a')));
    if(!data || data->fields.size() < 4) return {};
    const AffField& fp = data->fields[3];
    if(fp.recSize != 18 || fp.count < 4 || qsizetype(fp.count)*18 > fp.bytes.size()) return {};
    const u8* rec = reinterpret_cast<const u8*>(fp.bytes.constData());
    auto px = [&](int i){ u64 b = rdu64(rec + qsizetype(i)*18);     double v; memcpy(&v,&b,8); return finite(v); };
    auto py = [&](int i){ u64 b = rdu64(rec + qsizetype(i)*18 + 8); double v; memcpy(&v,&b,8); return finite(v); };
    QString d = QStringLiteral("M%1 %2").arg(fnum(px(0)), fnum(py(0)));
    for(int j = 0; j + 3 < fp.count && j < 300000; j += 3)
        d += QStringLiteral("C%1 %2 %3 %4 %5 %6").arg(fnum(px(j+1)), fnum(py(j+1)), fnum(px(j+2)), fnum(py(j+2)), fnum(px(j+3)), fnum(py(j+3)));
    return d;
}

void Converter::curveNode(const AffNode& n){
    const int ci = firstKid(n, TG('C','r','v','s'));
    const AffNode* pcvd = node(ci);
    if(!pcvd) return;
    const int di = firstKid(*pcvd, TG('D','a','t','a'));
    const AffNode* data = node(di);
    if(!data || data->fields.size() < 2) return;
    if(data->fields[1].type != 0x03 || data->fields[1].num.isEmpty()) return;
    const double rawSub = data->fields[1].num[0];
    if(!(rawSub >= 0.0) || rawSub > 1e6) return;
    const int nsub = int(rawSub);

    // Corner Tool radii, keyed by record index.
    QHash<int,QPair<double,int>> corners;
    if(const AffNode* cd = node(firstKid(*pcvd, TG('C','n','r','D')))){
        if(cd->fields.size() >= 4){
            const QVector<double>& ix = cd->fields[1].num; const QVector<double>& rd = cd->fields[2].num; const QVector<double>& ty = cd->fields[3].num;
            for(int i = 0; i < ix.size() && i < rd.size() && i < ty.size(); ++i)
                if(finite(rd[i]) > 0.0 && finite(ty[i]) > 0.0) corners.insert(int(qBound(-1.0, finite(ix[i]), 1e8)), { finite(rd[i]), int(qBound(0.0, finite(ty[i]), 16.0)) });
        }
    }
    QString d;
    int emitted = 0;
    double bx0 = 1e300, by0 = 1e300, bx1 = -1e300, by1 = -1e300;
    for(int s = 0; s < nsub; ++s){
        const int fi = 2 + s*2;
        if(fi + 1 >= data->fields.size()) break;
        const AffField& fc = data->fields[fi];
        const AffField& fp = data->fields[fi+1];
        if(fp.recSize != 18) continue;
        const int cnt = fp.count;
        if(cnt < 2) continue;
        if(qsizetype(cnt)*18 > fp.bytes.size()) continue;
        const bool closed = !fc.num.isEmpty() && fc.num[0] != 0.0;
        const u8* rec = reinterpret_cast<const u8*>(fp.bytes.constData());
        auto px = [&](int i){ u64 b = rdu64(rec + qsizetype(i)*18);     double v; memcpy(&v,&b,8); return finite(v); };
        auto py = [&](int i){ u64 b = rdu64(rec + qsizetype(i)*18 + 8); double v; memcpy(&v,&b,8); return finite(v); };
        if(cnt < 4){                                   // a stray anchor pair: draw it as a line
            d += QStringLiteral("M%1 %2L%3 %4").arg(fnum(px(0)), fnum(py(0)), fnum(px(1)), fnum(py(1)));
            if(closed) d += QLatin1Char('Z');
            ++emitted;
            continue;
        }
        if(!corners.isEmpty() && nsub == 1){
            const QString cp = cornerPath(rec, cnt, closed, corners);
            if(!cp.isEmpty()){
                d += cp;
                for(int j = 0; j + 3 < cnt; j += 3){ cubicExt(px(j), px(j+1), px(j+2), px(j+3), bx0, bx1); cubicExt(py(j), py(j+1), py(j+2), py(j+3), by0, by1); }
                ++emitted;
                continue;
            }
        }
        d += QStringLiteral("M%1 %2").arg(fnum(px(0)), fnum(py(0)));
        for(int j = 0; j + 3 < cnt; j += 3){
            d += QStringLiteral("C%1 %2 %3 %4 %5 %6")
                    .arg(fnum(px(j+1)), fnum(py(j+1)), fnum(px(j+2)), fnum(py(j+2)), fnum(px(j+3)), fnum(py(j+3)));
            cubicExt(px(j), px(j+1), px(j+2), px(j+3), bx0, bx1);
            cubicExt(py(j), py(j+1), py(j+2), py(j+3), by0, by1);
            if(d.size() > kMaxSvg) break;
        }
        if(closed) d += QLatin1Char('Z');
        ++emitted;
        if(d.size() > kMaxSvg) break;
    }
    if(!emitted || d.isEmpty()) return;
    // Free integrity check: the computed cubic bounds must reproduce the file's own CvsB.
    const AffField* cb = fieldOf(n, TG('C','v','s','B'));
    if(cb && cb->num.size() >= 4 && bx1 > -1e299){
        const double sc = qMax(1.0, qMax(qAbs(cb->num[0]), qMax(qAbs(cb->num[1]), qMax(qAbs(cb->num[2]), qAbs(cb->num[3])))));
        const double err = qMax(qMax(qAbs(bx0 - cb->num[0]), qAbs(by0 - cb->num[1])),
                                qMax(qAbs(bx1 - cb->num[2]), qAbs(by1 - cb->num[3])));
        if(err < 1e-9 * sc) ++m_st.bboxOk; else ++m_st.bboxBad;
    }
    emitPainted(n, QStringLiteral("<path d=\"%1\"\x01/>").arg(d));
    ++m_st.shapes;
}

// The outline of an element we are about to emit, for the operations SVG cannot express (a stroke aligned
// inside or outside the shape has to be computed from the geometry).
QPainterPath Converter::elementPath(const QString& el) const {
    auto attr = [&](const char* name){
        const int a = el.indexOf(QStringLiteral(" %1=\"").arg(QLatin1String(name)));
        if(a < 0) return QString();
        const int b = el.indexOf(QLatin1Char('"'), a + int(strlen(name)) + 3);
        return b < 0 ? QString() : el.mid(a + int(strlen(name)) + 3, b - a - int(strlen(name)) - 3);
    };
    QPainterPath p;
    if(el.startsWith(QLatin1String("<path"))) return parseSvgPathData(attr("d"));
    if(el.startsWith(QLatin1String("<rect"))){
        const double x = attr("x").toDouble(), y = attr("y").toDouble(), w = attr("width").toDouble(), h = attr("height").toDouble();
        const double r = attr("rx").toDouble();
        if(r > 0.0) p.addRoundedRect(QRectF(x, y, w, h), r, r); else p.addRect(QRectF(x, y, w, h));
        return p;
    }
    if(el.startsWith(QLatin1String("<ellipse"))){
        p.addEllipse(QPointF(attr("cx").toDouble(), attr("cy").toDouble()), attr("rx").toDouble(), attr("ry").toDouble());
        return p;
    }
    return p;
}

// Trns: the Transparency Tool. It is an FDsc holding a gradient of which only the alpha matters.
QString Converter::transpAttr(const AffNode& n){
    const AffNode* w = node(firstKid(n, TG('T','r','n','s')));
    if(!w || w->types.isEmpty() || w->types[0] != TG('F','D','s','c')) return {};
    const AffNode* f = node(firstKid(*w, TG('F','D','e','F')));
    if(!f || f->types.isEmpty() || f->types[0] != TG('F','i','l','G')) return {};
    const AffField* x = fieldOf(*w, TG('F','D','e','X'));
    const AffNode* g = node(firstKid(*f, TG('G','r','a','d')));
    if(!x || x->num.size() < 6 || !g) return {};
    const AffField* ty = fieldOf(*f, TG('T','y','p','e'));
    const int type = ty && !ty->num.isEmpty() ? int(qBound(0.0, ty->num[0], 64.0)) : 0;
    if(type == 3) return {};                           // conical transparency has no model equivalent
    const AffField* posn = fieldOf(*g, TG('P','o','s','n'));
    const AffField* cols = fieldOf(*g, TG('C','o','l','s'));
    if(!cols || cols->kids.size() < 2) return {};
    QString stops;
    const int nc = qMin(cols->kids.size(), 64);
    for(int i = 0; i < nc; ++i){
        QColor c; if(!colourOf(cols->kids[i], c)) continue;
        double off = double(i) / double(nc - 1);
        if(posn && posn->comps >= 2 && qsizetype(i) * posn->comps < posn->num.size()) off = finite(posn->num[qsizetype(i) * posn->comps]);
        if(!stops.isEmpty()) stops += QLatin1Char(',');
        stops += QStringLiteral("%1:%2").arg(fnum(qBound(0.0, off, 1.0)), fnum(1.0 - c.alphaF()));
    }
    const double a = finite(x->num[0]), b = finite(x->num[1]), c = finite(x->num[2]), d = finite(x->num[3]), e = finite(x->num[4]), ff = finite(x->num[5]);
    return QStringLiteral(" xarq:transp=\"%1 %2 %3 %4 %5 %6 %7 %8\"")
        .arg(type == 0 ? 1 : 2).arg(fnum(c), fnum(ff), fnum(c + a), fnum(ff + d), fnum(c + b), fnum(ff + e)).arg(stops);
}

// A shape's appearance is a STACK of paints. DrwO lists it bottom to top, consuming the fill and line
// lists in order: 0 next fill, 1 next line, 2 a hidden fill, 3 a hidden line. BFCr and LICr only say
// which entry is selected in the Appearance panel — reading them as "the" paint lost every other one.
// The ordinary fill-under-one-line case stays a single element, so it remains one editable object.
// FiEf holds one entry per effect type, most of them switched off. Radii are in page units unless SclO.
LayerFx Converter::readFx(const AffNode& n){
    LayerFx fx = m_fx;
    auto col = [&](const AffNode& e, double op, QColor& out){
        QColor c(Qt::black);
        colourOf(firstKid(e, TG('C','o','l','r')), c);
        c.setAlphaF(qBound(0.0, c.alphaF() * op, 1.0));
        out = c;
    };
    auto blendOf = [&](const AffNode& e){
        const AffField* b = fieldOf(e, TG('B','l','n','M'));
        return (b && !b->num.isEmpty()) ? blendCss(int(qBound(0.0, finite(b->num[0]), 1000.0)), b->ever) : QString();
    };
    if(const AffField* fe = fieldOf(n, TG('F','i','E','f'))){
        for(int k : fe->kids){
            const AffNode* e = node(k);
            if(!e || e->types.isEmpty() || numOf(*e, TG('E','n','a','b'), 0.0) == 0.0) continue;
            const double op = qBound(0.0, numOf(*e, TG('O','p','a','c'), 1.0), 1.0);
            const double r = qBound(0.0, numOf(*e, TG('R','a','d','i'), 0.0), 1e4);
            const double off = qBound(-1e5, numOf(*e, TG('O','f','f','s'), 0.0), 1e5);
            const double ang = numOf(*e, TG('A','n','g','l'), 0.0);
            const u32 c = e->types[0];
            if(c == TG('S','h','a','d')){ fx.shadow = true; fx.shX = off * std::cos(ang); fx.shY = off * std::sin(ang); fx.shR = r; col(*e, op, fx.shC); }
            else if(c == TG('O','u','t','G')){ fx.glow = true; fx.glR = r; col(*e, op, fx.glC); fx.glComp = qBound(0.05, numOf(*e, TG('C','o','m','p'), 1.0), 1.0); fx.glBlend = blendOf(*e); }
            else if(c == TG('G','a','u','s')) fx.blur = qMax(fx.blur, r);
            else if(c == TG('I','n','n','S')){ fx.inner = true; fx.inX = off * std::cos(ang); fx.inY = off * std::sin(ang); fx.inR = r; col(*e, op, fx.inC); fx.inBlend = blendOf(*e); }
            else if(c == TG('I','n','n','G')){ fx.innerGlow = true; fx.igR = r; col(*e, op, fx.igC); fx.igBlend = blendOf(*e); fx.igComp = qBound(0.05, numOf(*e, TG('C','o','m','p'), 1.0), 1.0); }
            else if(c == TG('S','t','r','k')){ fx.outline = r > 0.0; fx.olR = r; col(*e, op, fx.olC); }
            else if(c == TG('C','o','l','O')){ fx.colour = true; col(*e, op, fx.coC); fx.coBlend = blendOf(*e); }
            else if(c == TG('G','r','d','O')){ fx.grFill = firstKid(*e, TG('G','r','F','l')); fx.grOp = op; fx.grBlend = blendOf(*e); }
            else if(c == TG('B','e','v','E')){
                fx.bevel = true; fx.phong = false; fx.bvRad = r;
                fx.bvAzim = numOf(*e, TG('A','z','i','m'), 2.3562);
                fx.bvOp = op; fx.bvShOp = qBound(0.0, numOf(*e, TG('S','h','O','p'), 0.75), 1.0);
                // Beve: 0 inner, 1 outer, 2 emboss, 3 pillow, as in the panel's list. Only pillow has a sample.
                const int style = int(qBound(0.0, numOf(*e, TG('B','e','v','e'), 0.0), 8.0));
                fx.bvOuter = style != 0; fx.bvInner = style != 1;
                fx.bvHi = Qt::white; fx.bvLo = Qt::black;
                fx.bvHiBlend = blendOf(*e);
                if(const AffField* sb = fieldOf(*e, TG('S','h','B','M'))) if(!sb->num.isEmpty()) fx.bvLoBlend = blendCss(int(qBound(0.0, finite(sb->num[0]), 1000.0)), sb->ever);
                if(numOf(*e, TG('I','n','v','t'), 0.0) != 0.0) fx.bvAzim += M_PI;
            }
            else if(c == TG('P','h','g','B')){
                fx.bevel = true; fx.phong = true; fx.bvRad = r; fx.bvOuter = false; fx.bvInner = true;
                fx.phAmb = numOf(*e, TG('A','m','b','i'), 0.2); fx.phDiff = numOf(*e, TG('D','i','f','f'), 1.0);
                fx.phSpec = numOf(*e, TG('S','p','e','c'), 0.5); fx.phExp = qBound(0.1, numOf(*e, TG('E','x','p','o'), 5.0), 200.0);
                fx.bvAzim = 0.7854; fx.phElev = 0.7854;
                if(const AffField* ls = fieldOf(*e, TG('L','i','t','s'))) if(!ls->kids.isEmpty()) if(const AffNode* la = node(ls->kids[0]))
                    if(const AffField* pl = fieldOf(*la, TG('P','L','i','s'))) if(!pl->kids.isEmpty()) if(const AffNode* li = node(pl->kids[0])){
                        fx.bvAzim = numOf(*li, TG('A','l','p','h'), fx.bvAzim); fx.phElev = numOf(*li, TG('B','e','t','a'), fx.phElev); }
            }
        }
    }
    // A live Gaussian blur layer nested in the object.
    if(const AffField* ad = fieldOf(n, TG('A','d','C','h')))
        for(int k : ad->kids){
            const AffNode* m = node(k);
            if(!m || m->types.isEmpty() || m->types[0] != TG('F','l','R','N')) continue;
            const AffNode* f = node(firstKid(*m, TG('F','i','l','t')));
            if(f && !f->types.isEmpty() && f->types[0] == TG('R','G','B','C')) fx.blur = qMax(fx.blur, qBound(0.0, numOf(*f, TG('R','a','d','i'), 0.0), 1e4));
            if(f && !f->types.isEmpty() && f->types[0] == TG('R','A','N','C')) fx.noise = qBound(0.0, numOf(*f, TG('I','n','t','e'), 0.0), 1.0);
            if(f && !f->types.isEmpty() && f->types[0] != TG('R','G','B','C') && f->types[0] != TG('R','A','N','C')) ++m_st.ignoredFilters;
        }
    return fx;
}

// Affinity's radius is the visible reach of the falloff, about 2.5 sigma; Shape::blur is 1.8 sigma.
static double fxBlur(double radius){ return radius / 2.5 * 1.8; }

QString Converter::fxShapeAttrs() const {
    if(!m_fx.shadow) return {};
    return QStringLiteral(" xarq:shadow=\"%1 %2 %3 %4 %5 0\"").arg(fnum(m_fx.shX), fnum(m_fx.shY), fnum(m_fx.shR), m_fx.shC.name(QColor::HexRgb), fnum(m_fx.shC.alphaF()));
}

void Converter::emitPainted(const AffNode& n, const QString& element){
    static const QString kMark = QStringLiteral("\x01");
    if(m_parked > 0){ emitPaintStack(n, element); return; }
    const LayerFx& fx = m_fx;
    const QString blurA = fx.blur > 0.0 ? QStringLiteral(" xarq:blur=\"%1\"").arg(fnum(fxBlur(fx.blur))) : QString();
    auto overlay = [&](const QString& paint){ QString e = element; put(e.replace(kMark, paint + blurA)); };
    const QPainterPath geo = (fx.outline || fx.inner || fx.innerGlow || fx.glow || fx.bevel) ? elementPath(element) : QPainterPath();
    // A blurred copy of the outline, behind or over the object. Intensity (Comp) divides what the falloff
    // has to reach: at 0.5 a glow's edge is at full strength rather than half.
    auto softCopy = [&](const QPainterPath& p, const QColor& c, double radius, double gain, const QString& blend){
        if(p.isEmpty() || !budget()) return;
        QString a = QStringLiteral(" fill=\"%1\" fill-opacity=\"%2\" xarq:blur=\"%3\"").arg(c.name(QColor::HexRgb), fnum(c.alphaF()), fnum(fxBlur(radius)));
        if(gain != 1.0) a += QStringLiteral(" xarq:blur-gain=\"%1\"").arg(fnum(gain));
        if(!blend.isEmpty()) a += QStringLiteral(" style=\"mix-blend-mode:%1\"").arg(blend);
        m_body += QStringLiteral("<path d=\"%1\"%2/>").arg(affPathData(p), a);
    };
    if(fx.glow) softCopy(geo, fx.glC, fx.glR, 1.0 / fx.glComp, fx.glBlend);
    // The direction towards the light, screen y down.
    const QPointF toLight(std::cos(fx.bvAzim), -std::sin(fx.bvAzim));
    // A pillow's outer half: the surface falls away from the object, so its lit side faces the other way.
    if(fx.bevel && fx.bvOuter){
        QColor lo = fx.bvLo; lo.setAlphaF(fx.bvShOp);
        QColor hi = fx.bvHi; hi.setAlphaF(fx.bvOp);
        const QPointF o = toLight * fx.bvRad * 0.5;
        softCopy(geo.translated(o), lo, fx.bvRad, 1.0, fx.bvLoBlend);
        softCopy(geo.translated(-o), hi, fx.bvRad, 1.0, fx.bvHiBlend);
    }

    // Outline effect: behind the object, so only its outer half shows.
    if(fx.outline)
        overlay(QStringLiteral(" fill=\"none\" stroke=\"%1\" stroke-width=\"%2\" stroke-opacity=\"%3\"").arg(fx.olC.name(QColor::HexRgb), fnum(2.0 * fx.olR), fnum(fx.olC.alphaF())));

    const qsizetype at = m_body.size();
    emitPaintStack(n, element);
    // One caster per object: a shadow on each paint layer would double up.
    const QString first = fxShapeAttrs() + blurA;
    if(!first.isEmpty() && m_body.size() > at){
        const qsizetype sp = m_body.indexOf(QLatin1Char(' '), at);
        if(sp > 0) m_body.insert(sp, first);
    }
    auto tint = [&](const QColor& c, const QString& blend){
        QString a = QStringLiteral(" fill=\"%1\" fill-opacity=\"%2\"").arg(c.name(QColor::HexRgb), fnum(c.alphaF()));
        if(!blend.isEmpty()) a += QStringLiteral(" style=\"mix-blend-mode:%1\"").arg(blend);
        overlay(a);
    };
    if(fx.grFill >= 0){
        // This gradient has no Xfrm of its own: it spans the object's box, unlike a layer's own fill.
        const QRectF bb = elementPath(element).boundingRect();
        const double unit[6] = { bb.width(), 0.0, bb.left(), 0.0, bb.height(), bb.top() };
        const AffNode* gd = node(fx.grFill);
        const int gf = gd ? firstKid(*gd, TG('F','D','e','F')) : -1;
        const Paint p = paintFromFill(gf >= 0 ? gf : fx.grFill, unit, true);
        if(p.kind == Paint::Gradient){
            QString a = QStringLiteral(" fill=\"url(#%1)\" fill-opacity=\"%2\"").arg(p.url, fnum(fx.grOp));
            if(!fx.grBlend.isEmpty()) a += QStringLiteral(" style=\"mix-blend-mode:%1\"").arg(fx.grBlend);
            overlay(a);
        } else if(p.kind == Paint::Solid) tint(p.col, fx.grBlend);
    }
    if(fx.colour) tint(fx.coC, fx.coBlend);
    // Live noise: a repeatable zero-mean tile through Linear Light, which adds it to what is beneath. Against
    // Affinity's render, intensity 0.5 has a standard deviation of 21 levels.
    if(fx.noise > 0.0){
        static QHash<int, QByteArray> tiles;
        const int key = int(fx.noise * 1000.0);
        if(!tiles.contains(key)){
            QImage img(256, 256, QImage::Format_RGB32);
            quint32 st = 0x2545F491u;
            auto rnd = [&]{ st ^= st << 13; st ^= st >> 17; st ^= st << 5; return (st & 0xffffu) / 65535.0; };
            const double amp = 0.248 * fx.noise;
            for(int y = 0; y < 256; ++y)
                for(int x = 0; x < 256; ++x){
                    const double v = (rnd() + rnd() + rnd()) / 3.0 * 2.0 - 1.0;      // sigma 1/3
                    const int c = qBound(0, int((0.5 + v * amp) * 255.0 + 0.5), 255);
                    img.setPixel(x, y, qRgb(c, c, c));
                }
            QByteArray png; QBuffer b(&png); b.open(QIODevice::WriteOnly); img.save(&b, "PNG");
            tiles.insert(key, png.toBase64());
        }
        overlay(QStringLiteral(" fill=\"#808080\" style=\"mix-blend-mode:xarq-linear-light\" xarq:bitmap=\"data:image/png;base64,%1\" xarq:bitmap-xf=\"256 0 0 -256 0 256\" xarq:bitmap-tiled=\"1\"")
                    .arg(QString::fromLatin1(tiles[key])));
    }
    // Inner shadow and glow: a blurred inverse of the outline, clipped to it.
    auto innerFx = [&](double dx, double dy, double r, const QColor& c, const QString& blend, double gain = 1.0){
        if(geo.isEmpty() || !budget()) return;
        const QString id = QStringLiteral("%1affx%2").arg(m_idPrefix).arg(++m_clipSeq);
        m_defs += QStringLiteral("<clipPath id=\"%1\"><path d=\"%2\"/></clipPath>").arg(id, affPathData(geo));
        QPainterPath inv;
        const double grow = 4.0 * r + qAbs(dx) + qAbs(dy) + 1.0;
        inv.addRect(geo.boundingRect().adjusted(-grow, -grow, grow, grow));
        inv.addPath(geo.translated(dx, dy));
        QString a = QStringLiteral(" fill=\"%1\" fill-opacity=\"%2\" fill-rule=\"evenodd\" xarq:blur=\"%3\"").arg(c.name(QColor::HexRgb), fnum(c.alphaF()), fnum(fxBlur(r)));
        if(gain != 1.0) a += QStringLiteral(" xarq:blur-gain=\"%1\"").arg(fnum(gain));
        if(!blend.isEmpty()) a += QStringLiteral(" style=\"mix-blend-mode:%1\"").arg(blend);
        m_body += QStringLiteral("<g clip-path=\"url(#%1)\"><path d=\"%2\"%3/></g>").arg(id, affPathData(inv), a);
    };
    if(fx.inner) innerFx(fx.inX, fx.inY, fx.inR, fx.inC, fx.inBlend);
    if(fx.innerGlow) innerFx(0.0, 0.0, fx.igR, fx.igC, fx.igBlend, 1.0 / fx.igComp);
    if(fx.bevel && fx.phong){
        // Phong on the flat top: colour * (ambient + diffuse N.L) + specular (N.L)^exponent. The rim tilts
        // towards the light on one side and away on the other.
        const double nl = qBound(0.0, std::sin(fx.phElev), 1.0);
        const double k = fx.phAmb + fx.phDiff * nl, sp = fx.phSpec * std::pow(nl, fx.phExp);
        if(k < 0.999) overlay(QStringLiteral(" fill=\"#000000\" fill-opacity=\"%1\"").arg(fnum(qBound(0.0, 1.0 - k, 1.0))));
        if(sp > 0.001){ const int v = qBound(0, int(sp * 255.0 + 0.5), 255); overlay(QStringLiteral(" fill=\"%1\" style=\"mix-blend-mode:plus-lighter\"").arg(QColor(v, v, v).name(QColor::HexRgb))); }
        const QPointF o = toLight * fx.bvRad * 0.5;
        QColor dark(Qt::black); dark.setAlphaF(qBound(0.0, 1.0 - fx.phAmb / qMax(0.01, k), 1.0));
        QColor lit(Qt::white); lit.setAlphaF(qBound(0.0, fx.phSpec + (fx.phAmb + fx.phDiff - k) * 0.5, 1.0));
        innerFx(-o.x(), -o.y(), fx.bvRad * 1.4, lit, QString());
        innerFx(o.x(), o.y(), fx.bvRad * 1.4, dark, QString());
    } else if(fx.bevel && fx.bvInner){
        // Rim softness is fitted to Affinity's render of the pillow sample.
        QColor lo = fx.bvLo; lo.setAlphaF(fx.bvShOp);
        QColor hi = fx.bvHi; hi.setAlphaF(fx.bvOp);
        const QPointF o = toLight * fx.bvRad * 0.5;
        innerFx(-o.x(), -o.y(), fx.bvRad * 1.4, hi, fx.bvHiBlend);
        innerFx(o.x(), o.y(), fx.bvRad * 1.4, lo, fx.bvLoBlend);
    }
}

void Converter::emitPaintStack(const AffNode& n, const QString& element){
    static const QString kMark = QStringLiteral("\x01");
    const QString extra = m_parked > 0 ? QString() : transpAttr(n);
    auto single = [&](int fs, int ss){
        QString e = element;
        put(e.replace(kMark, styleFor(n, fs, ss) + extra));
    };
    const AffField* order = fieldOf(n, TG('D','r','w','O'));
    if(m_parked > 0 || !order || order->num.isEmpty() || !fieldOf(n, TG('B','F','F','l'))){ single(-1, -1); return; }

    struct Layer { bool stroke; int slot; };
    QVector<Layer> layers;
    int fi = 0, si = 0;
    for(double c : order->num){
        if(c == 0.0) layers.push_back({ false, fi++ });
        else if(c == 1.0) layers.push_back({ true, si++ });
        else if(c == 2.0) ++fi;
        else if(c == 3.0) ++si;
    }
    auto alignOf = [&](int slot){
        const AffField* f = fieldOf(n, TG('L','I','L','n'));
        const AffNode* l = (f && slot < f->kids.size()) ? node(f->kids[slot]) : nullptr;
        const AffField* a = l ? fieldOf(*l, TG('L','D','S','a')) : nullptr;
        return a && !a->num.isEmpty() ? int(qBound(0.0, a->num[0], 2.0)) : 0;
    };
    auto hasLine = [&](int slot){
        const QString st = styleFor(n, -2, slot);
        return st.contains(QLatin1String(" stroke=\""));
    };
    if(layers.size() <= 2 && (layers.isEmpty() || !layers[0].stroke) && (layers.size() < 2 || (layers[1].stroke && alignOf(layers[1].slot) == 0))){
        single(layers.isEmpty() ? -2 : layers[0].slot, layers.size() > 1 ? layers[1].slot : -2);
        return;
    }
    for(const Layer& l : layers){
        if(!l.stroke){ QString e = element; put(e.replace(kMark, styleFor(n, l.slot, -2) + extra)); continue; }
        if(!hasLine(l.slot)) continue;
        const int al = alignOf(l.slot);
        if(al == 0){ QString e = element; put(e.replace(kMark, styleFor(n, -2, l.slot) + extra)); continue; }
        // Inside (1) or outside (2): stroke at twice the weight and keep the half on the right side.
        const AffField* lf = fieldOf(n, TG('L','I','L','n'));
        const AffNode* d = (lf && l.slot < lf->kids.size()) ? node(lf->kids[l.slot]) : nullptr;
        const AffNode* ls = d ? node(firstKid(*d, TG('L','D','e','L'))) : nullptr;
        const AffField* wg = ls ? fieldOf(*ls, TG('W','g','h','t')) : nullptr;
        const double w = wg && !wg->num.isEmpty() ? qBound(0.0, finite(wg->num[0]) * m_ptScale, 1e6) : 0.0;
        const QPainterPath geo = elementPath(element);
        if(!(w > 0.0) || geo.isEmpty()){ QString e = element; put(e.replace(kMark, styleFor(n, -2, l.slot) + extra)); continue; }
        QPainterPathStroker st;
        st.setWidth(w * 2.0);
        st.setJoinStyle(Qt::MiterJoin);
        const QPainterPath ring = st.createStroke(geo).simplified();
        const QPainterPath part = al == 1 ? ring.intersected(geo) : ring.subtracted(geo);
        const Paint pc = [&]{
            const AffField* pf = fieldOf(n, TG('L','I','F','l'));
            return (pf && l.slot < pf->kids.size()) ? paintAt(pf->kids[l.slot]) : Paint{};
        }();
        if(part.isEmpty() || !pc.col.isValid()) continue;
        QString a = QStringLiteral("<path d=\"%1\" fill=\"%2\" fill-rule=\"evenodd\"").arg(affPathData(part), colHex(pc.col));
        if(pc.col.alphaF() < 0.999) a += QStringLiteral(" fill-opacity=\"%1\"").arg(fnum(pc.col.alphaF()));
        put(a + extra + QStringLiteral("/>"));
    }
}

// ShpN carries a live "Shpe" child whose CLASS tag is the shape kind, plus ShpB (the local bounds the
// shape is drawn inside). Unknown kinds fall back to their ShpB rectangle so position, size and colour
// survive even when the outline does not.
void Converter::shapeNode(const AffNode& n){
    const AffField* sb = fieldOf(n, TG('S','h','p','B'));
    if(!sb || sb->num.size() < 4) return;
    double x0 = finite(sb->num[0]), y0 = finite(sb->num[1]), x1 = finite(sb->num[2]), y1 = finite(sb->num[3]);
    if(x1 < x0) std::swap(x0, x1);
    if(y1 < y0) std::swap(y0, y1);
    const double w = x1 - x0, h = y1 - y0;
    if(!(w > 0.0) || !(h > 0.0) || w > 1e9 || h > 1e9) return;
    const QString style = QStringLiteral("\x01");       // emitPainted fills in each paint layer

    const int si = firstKid(n, TG('S','h','p','e'));
    const AffNode* sh = node(si);
    const u32 kind = (sh && !sh->types.isEmpty()) ? sh->types[0] : 0;
    const double cx = x0 + w/2.0, cy = y0 + h/2.0, rx = w/2.0, ry = h/2.0;

    auto poly = [&](const QVector<QPointF>& pts){
        if(pts.size() < 3) return;
        QString d = QStringLiteral("M%1 %2").arg(fnum(pts[0].x()), fnum(pts[0].y()));
        for(int i = 1; i < pts.size(); ++i) d += QStringLiteral("L%1 %2").arg(fnum(pts[i].x()), fnum(pts[i].y()));
        d += QLatin1Char('Z');
        emitPainted(n, QStringLiteral("<path d=\"%1\"%2/>").arg(d, style));
        ++m_st.shapes;
    };

    if(kind == TG('S','h','p','E')){
        emitPainted(n, QStringLiteral("<ellipse cx=\"%1\" cy=\"%2\" rx=\"%3\" ry=\"%4\"%5/>")
              .arg(fnum(cx), fnum(cy), fnum(rx), fnum(ry), style));
        ++m_st.shapes; return;
    }
    if(kind == TG('S','h','N','R')){
        // ShCR holds four corner-radius fractions of half the shorter side. The plain Rectangle tool
        // also stores 0.25 while drawing square corners; only a rectangle that actually carries the
        // per-corner CTyp list renders rounded (verified against the documents' own previews).
        const AffField* cr = fieldOf(*sh, TG('S','h','C','R'));
        const bool rounded = sh && fieldOf(*sh, TG('C','T','y','p')) != nullptr;
        double r = 0.0;
        // ShCR is a fraction of the FULL shorter side, with a domain of [0, 0.5] — 0.5 means fully
        // rounded, which is the geometric maximum. Measured on shape_test.afdesign's 45x45 rounded
        // rectangle (ShCR = 0.24449) against the document's own 512px preview: the corner radius is
        // ~11 units, i.e. ShCR * min(w,h); halving it again drew the corners at half their size.
        if(rounded && cr && cr->num.size() >= 1)
            r = numOf(*sh, TG('A','b','S','z'), 0.0) != 0.0   // AbSz: the radius is in document units, not a fraction
                ? qBound(0.0, finite(cr->num[0]), qMin(w, h) / 2.0)
                : qBound(0.0, finite(cr->num[0]), 0.5) * qMin(w, h);
        if(r > 0.0)
            emitPainted(n, QStringLiteral("<rect x=\"%1\" y=\"%2\" width=\"%3\" height=\"%4\" rx=\"%5\" ry=\"%5\"%6/>")
                  .arg(fnum(x0), fnum(y0), fnum(w), fnum(h), fnum(r), style));
        else
            emitPainted(n, QStringLiteral("<rect x=\"%1\" y=\"%2\" width=\"%3\" height=\"%4\"%5/>")
                  .arg(fnum(x0), fnum(y0), fnum(w), fnum(h), style));
        ++m_st.shapes; return;
    }
    if(kind == TG('S','h','S','t') || kind == TG('S','h','P','y')){
        const bool star = (kind == TG('S','h','S','t'));
        // A star counts its points in Pnts; a POLYGON counts its sides in Side and has no Pnts field
        // at all, so reading Pnts there always fell through to the default and drew a pentagon
        // whatever the user asked for.
        const u32 kCount = star ? TG('P','n','t','s') : TG('S','i','d','e');
        double rawPts = numOf(*sh, kCount, -1.0);
        if(!(rawPts >= 3.0)) rawPts = numOf(*sh, star ? TG('S','i','d','e') : TG('P','n','t','s'), 5.0);
        int pts = int(qBound(3.0, finite(rawPts), 200.0));
        double ir = qBound(0.02, numOf(*sh, TG('I','R','a','d'), 0.382), 1.0);   // last IRad wins = the real one
        QVector<QPointF> v;
        const int steps = star ? pts*2 : pts;
        v.reserve(steps);
        for(int i = 0; i < steps; ++i){
            const double ang = -M_PI/2.0 + double(i) * M_PI * (star ? 1.0 : 2.0) / double(pts);
            const double f = (star && (i & 1)) ? ir : 1.0;
            v.push_back(QPointF(cx + rx*f*std::cos(ang), cy + ry*f*std::sin(ang)));
        }
        poly(v); return;
    }
    if(kind == TG('S','h','p','T')){                   // triangle; 'Pos ' is the apex position across the box
        const double pos = qBound(0.0, numOf(*sh, TG('P','o','s',' '), 0.5), 1.0);
        poly(QVector<QPointF>{ QPointF(x0 + w*pos, y0), QPointF(x1, y1), QPointF(x0, y1) });
        return;
    }
    if(kind == TG('S','h','D','i')){                   // diamond
        poly(QVector<QPointF>{ QPointF(cx, y0), QPointF(x1, cy), QPointF(cx, y1), QPointF(x0, cy) });
        return;
    }
    if(kind == TG('S','h','H','t')){                   // heart; Sprd deepens the top notch
        const double sp = qBound(0.0, numOf(*sh, TG('S','p','r','d'), 0.2), 1.0);
        const double notch = 0.06 + 0.40 * sp;
        auto X = [&](double u){ return x0 + u * w; };
        auto Y = [&](double u){ return y0 + u * h; };
        const QString d = QStringLiteral(
            "M%1 %2C%3 %4 %5 %6 %7 %8C%9 %10 %11 %12 %13 %14C%15 %16 %17 %18 %19 %20"
            "C%21 %22 %23 %24 %25 %26C%27 %28 %29 %30 %31 %32C%33 %34 %35 %36 %37 %38Z")
            .arg(fnum(X(0.50)), fnum(Y(1.00)))
            .arg(fnum(X(0.10)), fnum(Y(0.72)), fnum(X(0.00)), fnum(Y(0.44)), fnum(X(0.00)), fnum(Y(0.30)))
            .arg(fnum(X(0.00)), fnum(Y(0.12)), fnum(X(0.16)), fnum(Y(0.02)), fnum(X(0.32)), fnum(Y(0.02)))
            .arg(fnum(X(0.42)), fnum(Y(0.02)), fnum(X(0.48)), fnum(Y(notch*0.55)), fnum(X(0.50)), fnum(Y(notch)))
            .arg(fnum(X(0.52)), fnum(Y(notch*0.55)), fnum(X(0.58)), fnum(Y(0.02)), fnum(X(0.68)), fnum(Y(0.02)))
            .arg(fnum(X(0.84)), fnum(Y(0.02)), fnum(X(1.00)), fnum(Y(0.12)), fnum(X(1.00)), fnum(Y(0.30)))
            .arg(fnum(X(1.00)), fnum(Y(0.44)), fnum(X(0.90)), fnum(Y(0.72)), fnum(X(0.50)), fnum(Y(1.00)));
        emitPainted(n, QStringLiteral("<path d=\"%1\"%2/>").arg(d, style));
        ++m_st.shapes; return;
    }
    if(kind == TG('S','h','C','l')){                   // cloud: Bubl bubbles on a ring of radius IRad
        const int nb = int(qBound(3.0, numOf(*sh, TG('B','u','b','l'), 12.0), 200.0));
        const double ir = qBound(0.05, numOf(*sh, TG('I','R','a','d'), 0.8165), 0.99);
        const double half = M_PI / double(nb);
        // IRad is the VALLEY radius as a fraction of the peak radius: the bubbles peak at 1 and cross
        // each other at ir. Solving  rc*cos h + sqrt(rb^2 - (rc*sin h)^2) = ir  with rb = 1 - rc gives
        // rc in closed form, so the scallop depth comes straight out of the file.
        const double rc = (1.0 - ir*ir) / (2.0 * (1.0 - ir*std::cos(half)));
        const double rb = 1.0 - rc;
        const double side = rc * std::sin(half);
        // With many bubbles IRad no longer lies on the outer crossing (Affinity keeps the value when the
        // bubble count changes), so the ring radius it gives is used and the valley follows from it.
        if(rb > side + 1e-9 && rc > 0.0){
            // Where consecutive bubbles cross, on the bisector between their centres.
            const double cross = rc * std::cos(half) + std::sqrt(qMax(0.0, rb*rb - side*side));
            QString d;
            for(int i = 0; i < nb; ++i){
                const double a = -M_PI/2.0 + (2.0*double(i) + 1.0) * half;   // bisector i -> i+1
                const QPointF p(cx + rx*cross*std::cos(a), cy + ry*cross*std::sin(a));
                d += (i == 0) ? QStringLiteral("M%1 %2").arg(fnum(p.x()), fnum(p.y()))
                              : QStringLiteral("A%1 %2 0 0 1 %3 %4").arg(fnum(rx*rb), fnum(ry*rb), fnum(p.x()), fnum(p.y()));
            }
            const double a0 = -M_PI/2.0 + half;
            d += QStringLiteral("A%1 %2 0 0 1 %3 %4Z")
                   .arg(fnum(rx*rb), fnum(ry*rb),
                        fnum(cx + rx*cross*std::cos(a0)), fnum(cy + ry*cross*std::sin(a0)));
            emitPainted(n, QStringLiteral("<path d=\"%1\"%2/>").arg(d, style));
            ++m_st.shapes; return;
        }
    }
    if(kind == TG('S','h','S','S')){                   // "square star": Side bars of constant width
        // The arms are cut from a regular polygon inscribed in the box: each arm's flat end lies on one
        // edge (at the apothem, cos(pi/n)), COut is how much of that edge is cut away, and neighbouring
        // arms meet at radius 1-COut. Measured on a 6-arm star with COut 0.6 against its reference:
        // bar half-width 0.2 and tip 0.866 of the radius. The axes are the polygon's edge normals.
        const int nA = int(qBound(3.0, numOf(*sh, TG('S','i','d','e'), 5.0), 200.0));
        const double co = qBound(0.0, numOf(*sh, TG('C','O','u','t'), 0.5), 0.98);
        const double half = M_PI / double(nA);
        const double wArm = (1.0 - co) * std::sin(half);
        const double tipL = std::cos(half);
        const double rIn  = 1.0 - co;
        QVector<QPointF> v;
        v.reserve(nA * 3);
        for(int i = 0; i < nA; ++i){
            const double a = -M_PI/2.0 + (2.0*double(i) + 1.0) * half;
            const double ca = std::cos(a), sa = std::sin(a);
            const double nx = -sa, ny = ca;
            v.push_back(QPointF(cx + rx*(ca*tipL - wArm*nx), cy + ry*(sa*tipL - wArm*ny)));
            v.push_back(QPointF(cx + rx*(ca*tipL + wArm*nx), cy + ry*(sa*tipL + wArm*ny)));
            const double b = a + half;
            v.push_back(QPointF(cx + rx*rIn*std::cos(b), cy + ry*rIn*std::sin(b)));
        }
        poly(v); return;
    }
    auto pathEl = [&](const QPainterPath& pp){
        emitPainted(n, QStringLiteral("<path d=\"%1\"%2/>").arg(affPathData(pp), style));
        ++m_st.shapes;
    };
    auto U = [&](double u, double v){ return QPointF(x0 + u*w, y0 + v*h); };
    auto E = [&](double a, double r){ return QPointF(cx + rx*r*std::cos(a), cy + ry*r*std::sin(a)); };   // y-down angle
    if(kind == TG('S','h','p','D')){                   // diamond; Pos is the height of the widest point
        const double pos = qBound(0.0, numOf(*sh, TG('P','o','s',' '), 0.5), 1.0);
        poly(QVector<QPointF>{ U(0.5,0), U(1,pos), U(0.5,1), U(0,pos) });
        return;
    }
    if(kind == TG('S','h','T','z')){                   // trapezoid; the top edge runs from PosL to PosR
        const double l = qBound(0.0, numOf(*sh, TG('P','o','s','L'), 0.25), 1.0), r = qBound(0.0, numOf(*sh, TG('P','o','s','R'), 0.75), 1.0);
        poly(QVector<QPointF>{ U(l,0), U(r,0), U(1,1), U(0,1) });
        return;
    }
    if(kind == TG('S','h','D','S')){                   // double star: tips alternate 1 and PRad, valleys IRad
        const int np = int(qBound(3.0, numOf(*sh, TG('P','n','t','s'), 5.0), 200.0));
        const double ir = qBound(0.02, numOf(*sh, TG('I','R','a','d'), 0.6), 1.0), pr = qBound(0.02, numOf(*sh, TG('P','R','a','d'), 0.9), 1.0);
        QVector<QPointF> v; v.reserve(np * 4);
        for(int i = 0; i < np * 4; ++i){
            const double f = (i & 1) ? ir : ((i % 4) == 0 ? 1.0 : pr);
            v.push_back(E(-M_PI/2.0 + double(i) * M_PI / (2.0 * np), f));
        }
        poly(v); return;
    }
    if(kind == TG('S','h','P','i')){                   // pie / donut: AngS..AngE counter-clockwise, hole IRad
        const double as = finite(numOf(*sh, TG('A','n','g','S'), 0.0)), ae = finite(numOf(*sh, TG('A','n','g','E'), 0.0));
        const double ir = qBound(0.0, numOf(*sh, TG('I','R','a','d'), 0.0), 0.999);
        double sweep = as - ae;                        // angles count upward from +x; on screen that is clockwise from -AngS
        while(sweep <= 0.0) sweep += 2.0 * M_PI;
        const bool full = qAbs(as - ae) < 1e-6;
        if(full) sweep = 2.0 * M_PI;
        QPainterPath pp;
        const int steps = qMax(16, int(sweep * 32.0));
        // Sweep the solid part: from -AngS round through the left of the circle back to -AngE.
        const double a0 = -as;
        const double dir = -1.0;
        const double span = 2.0 * M_PI - (full ? 0.0 : std::fmod(as - ae + 4.0 * M_PI, 2.0 * M_PI));
        Q_UNUSED(sweep);
        const double total = full ? 2.0 * M_PI : span;
        pp.moveTo(E(a0, 1.0));
        for(int i = 1; i <= steps; ++i) pp.lineTo(E(a0 + dir * total * double(i) / steps, 1.0));
        if(ir > 0.0){
            if(full){ pp.closeSubpath(); pp.moveTo(E(a0, ir)); } else pp.lineTo(E(a0 + dir * total, ir));
            for(int i = steps - 1; i >= 0; --i) pp.lineTo(E(a0 + dir * total * double(i) / steps, ir));
        } else if(!full) pp.lineTo(QPointF(cx, cy));
        pp.closeSubpath();
        pathEl(pp); return;
    }
    if(kind == TG('S','h','S','g')){                   // segment: a disc between two parallel cuts
        const double ang = finite(numOf(*sh, TG('A','n','g','l'), 0.0));
        const double p0 = 2.0 * qBound(0.0, numOf(*sh, TG('P','o','s','0'), 0.0), 1.0) - 1.0;
        const double p1 = 2.0 * qBound(0.0, numOf(*sh, TG('P','o','s','1'), 1.0), 1.0) - 1.0;
        const double ax = std::cos(ang), ay = -std::sin(ang);   // the cut axis, angle counted upward from +x
        const double lo = qMin(p0, p1), hi = qMax(p0, p1);
        const double a1 = std::atan2(ay, ax);
        const double dl = std::acos(qBound(-1.0, lo, 1.0)), dh = std::acos(qBound(-1.0, hi, 1.0));
        QPainterPath pp;
        const int steps = 96;
        for(int i = 0; i <= steps; ++i){               // the far arc, above the upper cut... through the axis
            const double t = a1 + dl - (dl - dh) * double(i) / steps;
            i == 0 ? pp.moveTo(E(t, 1.0)) : pp.lineTo(E(t, 1.0));
        }
        for(int i = 0; i <= steps; ++i){
            const double t = a1 - dh - (dl - dh) * double(i) / steps;
            pp.lineTo(E(t, 1.0));
        }
        pp.closeSubpath();
        pathEl(pp); return;
    }
    if(kind == TG('S','h','C','r')){                   // crescent: two arcs between the tips at top/bottom centre
        const double al = qBound(-1.0, numOf(*sh, TG('A','r','c','L'), -1.0), 1.0), ar = qBound(-1.0, numOf(*sh, TG('A','r','c','R'), -0.3), 1.0);
        QPainterPath pp; const int steps = 64;
        pp.moveTo(cx, y0);
        for(int i = 1; i <= steps; ++i){ const double t = M_PI * double(i) / steps; pp.lineTo(cx + rx * al * std::sin(t), cy - ry * std::cos(t)); }
        for(int i = steps - 1; i >= 0; --i){ const double t = M_PI * double(i) / steps; pp.lineTo(cx + rx * ar * std::sin(t), cy - ry * std::cos(t)); }
        pp.closeSubpath();
        pathEl(pp); return;
    }
    if(kind == TG('S','h','T','r')){                   // tear: a round belly drawn up to a point; Curv bows the flanks
        const double cv = qBound(-1.0, numOf(*sh, TG('C','u','r','v'), 0.3), 1.0);
        const double yc = y1 - qMin(rx, h / 2.0);
        const double ky = 0.5522847498 * (y1 - yc), kx = 0.5522847498 * rx;
        const double fl = (yc - y0) * 0.45;
        QPainterPath pp; pp.moveTo(cx, y0);
        pp.cubicTo(cx + rx * cv * 0.3, y0 + fl, x1, yc - fl, x1, yc);
        pp.cubicTo(x1, yc + ky, cx + kx, y1, cx, y1);
        pp.cubicTo(cx - kx, y1, x0, yc + ky, x0, yc);
        pp.cubicTo(x0, yc - fl, cx - rx * cv * 0.3, y0 + fl, cx, y0);
        pp.closeSubpath();
        pathEl(pp); return;
    }
    if(kind == TG('S','h','C','g')){                   // cog: Teth teeth on a root circle IRad, hole Hole
        const int nt = int(qBound(3.0, numOf(*sh, TG('T','e','t','h'), 12.0), 200.0));
        const double ir = qBound(0.05, numOf(*sh, TG('I','R','a','d'), 0.8), 1.0);
        const double hole = qBound(0.0, numOf(*sh, TG('H','o','l','e'), 0.3), 0.98);
        const double tt = qBound(0.01, numOf(*sh, TG('T','t','S','z'), 0.3), 1.0), nn = qBound(0.01, numOf(*sh, TG('N','t','S','z'), 0.45), 1.0);
        const double pitch = 2.0 * M_PI / nt;
        QPainterPath pp;
        for(int i = 0; i < nt; ++i){
            const double a = -M_PI/2.0 + i * pitch;
            const double a1 = a - pitch*tt/2.0, a2 = a + pitch*tt/2.0;      // tip
            const double a3 = a + pitch/2.0 - pitch*nn/2.0, a4 = a + pitch/2.0 + pitch*nn/2.0;   // the notch floor
            i == 0 ? pp.moveTo(E(a1, 1.0)) : pp.lineTo(E(a1, 1.0));
            for(int k = 1; k <= 4; ++k) pp.lineTo(E(a1 + (a2 - a1) * k / 4.0, 1.0));
            for(int k = 0; k <= 4; ++k) pp.lineTo(E(a3 + (a4 - a3) * k / 4.0, ir));
        }
        pp.closeSubpath();
        if(hole > 0.0) pp.addEllipse(QPointF(cx, cy), rx * hole, ry * hole);
        emitPainted(n, QStringLiteral("<path d=\"%1\" fill-rule=\"evenodd\"%2/>").arg(affPathData(pp), style));
        ++m_st.shapes; return;
    }
    if(kind == TG('S','h','C','R')){                   // rounded-rectangle speech bubble
        const double hg = qBound(0.0, numOf(*sh, TG('T','l','H','g'), 0.2), 0.95), wd = qBound(0.0, numOf(*sh, TG('T','l','W','d'), 0.1), 1.0);
        const double rp = qBound(0.0, numOf(*sh, TG('T','l','R','P'), 0.5), 1.0), ep = qBound(0.0, numOf(*sh, TG('T','l','E','P'), 0.4), 1.0);
        const AffField* cr = fieldOf(*sh, TG('S','h','C','R'));
        const double bh = h * (1.0 - hg);
        const double r = (cr && !cr->num.isEmpty()) ? qBound(0.0, finite(cr->num[0]), 0.5) * qMin(w, bh) : 0.0;
        QPainterPath body; body.addRoundedRect(QRectF(x0, y0, w, bh), r, r);
        QPainterPath tail;
        tail.moveTo(x0 + (rp - wd/2.0) * w, y0 + bh - 1.0); tail.lineTo(x0 + (rp + wd/2.0) * w, y0 + bh - 1.0); tail.lineTo(x0 + ep * w, y1); tail.closeSubpath();
        pathEl(body.united(tail).simplified()); return;
    }
    if(kind == TG('S','h','C','E')){                   // elliptical speech bubble
        const double hg = qBound(0.0, numOf(*sh, TG('T','l','H','g'), 0.25), 0.95), ep = qBound(0.0, numOf(*sh, TG('T','l','E','P'), 0.2), 1.0);
        const double tw = qBound(0.0, numOf(*sh, TG('T','l','A','n'), 0.26), 3.0);
        const double bh = h * (1.0 - hg), ery = bh / 2.0, ecy = y0 + ery;
        const QPointF tip(x0 + ep * w, y1);
        const double ta = std::atan2((tip.y() - ecy) / ery, (tip.x() - cx) / rx);   // the tail's direction, in ellipse parameter
        QPainterPath body; body.addEllipse(QPointF(cx, ecy), rx, ery);
        QPainterPath tail;
        tail.moveTo(cx + rx * 0.98 * std::cos(ta - tw/2.0), ecy + ery * 0.98 * std::sin(ta - tw/2.0));
        tail.lineTo(cx + rx * 0.98 * std::cos(ta + tw/2.0), ecy + ery * 0.98 * std::sin(ta + tw/2.0));
        tail.lineTo(tip); tail.closeSubpath();
        pathEl(body.united(tail).simplified()); return;
    }
    if(kind == TG('S','h','D','A')){                   // double arrow: a shaft and an end style each side
        const double th = qBound(0.01, numOf(*sh, TG('T','h','c','k'), 0.35), 1.0);
        const int ls = int(qBound(0.0, numOf(*sh, TG('L','S','t','y'), 0.0), 64.0)), rs = int(qBound(0.0, numOf(*sh, TG('R','S','t','y'), 0.0), 64.0));
        const double l1 = qBound(0.0, numOf(*sh, TG('L','P','r','1'), 0.5), 8.0) * h, l2 = qBound(0.0, numOf(*sh, TG('L','P','r','2'), 0.0), 8.0) * h;
        const double r1 = qBound(0.0, numOf(*sh, TG('R','P','r','1'), 0.5), 8.0) * h, r2 = qBound(0.0, numOf(*sh, TG('R','P','r','2'), 0.0), 8.0) * h;
        // End styles pinned so far: 8 a square bar Pr1 wide, 10 a chevron head Pr1 long swept back Pr2.
        // Anything else draws a plain shaft end, and is counted so the status line says so.
        if((ls != 0 && ls != 8 && ls != 10) || (rs != 0 && rs != 8 && rs != 10)) ++m_st.skippedShape;
        auto endPath = [&](int st, double tipX, double dirX, double p1, double p2){
            QPainterPath e;
            if(st == 8){ const double a = qMin(tipX, tipX + dirX * p1); e.addRect(QRectF(a, y0, p1, h)); }
            else if(st == 10){
                const double bx = tipX + dirX * p1;
                e.moveTo(tipX, cy); e.lineTo(bx, y0); e.lineTo(bx + dirX * p2, y0); e.lineTo(tipX + dirX * p2, cy); e.lineTo(bx + dirX * p2, y1); e.lineTo(bx, y1); e.closeSubpath();   // the back edge runs parallel to the front
            }
            return e;
        };
        const double sx0 = (ls == 10) ? x0 + l2 : x0, sx1 = (rs == 10) ? x1 - r2 : x1;
        QPainterPath pp; pp.addRect(QRectF(sx0, cy - h*th/2.0, qMax(0.0, sx1 - sx0), h*th));
        pp = pp.united(endPath(ls, x0, 1.0, l1, l2)).united(endPath(rs, x1, -1.0, r1, r2)).simplified();
        pathEl(pp); return;
    }
    // Unknown live shape: keep its box so the page still reads, and count it.
    ++m_st.skippedShape;
    emitPainted(n, QStringLiteral("<rect x=\"%1\" y=\"%2\" width=\"%3\" height=\"%4\"%5/>")
          .arg(fnum(x0), fnum(y0), fnum(w), fnum(h), style));
    ++m_st.shapes;
}

// TxtA/TxtF: the characters live in Glyp.Utf8 (U+2029 PARAGRAPH SEPARATOR between lines, trailing NUL),
// the run attributes in GAtt (Doub[0] = size, Objs[0] = the fill, DFnt/RFnt = the font). Affinity stores
// no laid-out glyph positions, so the baselines here are reconstructed, not read — text will not match
// Affinity's own line breaking.
QString affPathData(const QPainterPath& p);
QPainterPath affTextOutline(const QString& text, const QString& family, double size,
                            int weight, bool italic, double x, double y);

// One character run's style. Runs end at Indx (exclusive, in characters); a block's last run covers its
// trailing NUL. Doub[0] is the size, Objs[0] the fill, RFnt/DFnt the face.

static QFont txFont(const TxStyle& st){
    QFont f;
    if(!st.family.isEmpty()) f.setFamily(st.family);
    f.setPixelSize(100);                               // measured at a fixed size and scaled: no screen-DPI dependence
    f.setWeight(QFont::Weight(qBound(1, st.weight, 1000)));
    f.setItalic(st.italic);
    return f;
}

TxStyle Converter::runStyle(const AffNode& item, const TxStyle& base){
    TxStyle st = base;
    if(const AffField* db = fieldOf(item, TG('D','o','u','b'))){
        if(!db->num.isEmpty()) st.size = qBound(0.01, finite(db->num[0]), 4000.0);
    }
    if(const AffField* objs = fieldOf(item, TG('O','b','j','s'))){
        if(!objs->kids.isEmpty()){
            const AffNode* w = node(objs->kids[0]);
            Paint p;
            if(w && !w->types.isEmpty())
                p = w->types[0] == TG('F','D','s','c') ? paintFromFill(firstKid(*w, TG('F','D','e','F')), nullptr, false)
                                                       : paintFromFill(objs->kids[0], nullptr, false);
            if(p.kind == Paint::Solid) st.col = p.col;
        }
    }
    // A run carries both an RFnt and a DFnt slot, and RFnt is routinely present but EMPTY while DFnt holds
    // the actual face. Take the family, weight and style together from whichever slot names a family.
    const AffNode* rf = node(firstKid(item, TG('R','F','n','t')));
    const AffNode* df = node(firstKid(item, TG('D','F','n','t')));
    auto famOf = [](const AffNode* f)->QString {
        if(!f) return {};
        const AffField* x = fieldOf(*f, TG('F','a','m','y'));
        return (x && !x->bytes.isEmpty()) ? QString::fromUtf8(x->bytes) : QString();
    };
    const QString rFam = famOf(rf), dFam = famOf(df);
    const AffNode* fnt = !rFam.isEmpty() ? rf : (!dFam.isEmpty() ? df : (rf ? rf : df));
    if(fnt){
        const QString fam = famOf(fnt);
        if(!fam.isEmpty()) st.family = fam;
        st.weight = int(qBound(1.0, numOf(*fnt, TG('W','e','g','t'), 400.0), 1000.0));
        st.italic = numOf(*fnt, TG('I','t','a','l'), 0.0) != 0.0;
    }
    return st;
}

// The characters of a block. Plain text is Glyp.Utf8; text with live fields is Mixd, a list of segments
// whose Utf8 holds a NUL where a field's Glys entry goes: PgNG the page number, DcFG a document field
// (Type 12, the file name), LLGl a list label.
QString Converter::blockText(const AffNode& glyp) const {
    if(const AffField* utf = fieldOf(glyp, TG('U','t','f','8'))){
        QByteArray raw = utf->bytes;
        while(!raw.isEmpty() && raw.endsWith('\0')) raw.chop(1);
        return QString::fromUtf8(raw);
    }
    const AffField* mx = fieldOf(glyp, TG('M','i','x','d'));
    if(!mx) return {};
    QString out;
    for(int k : mx->kids){
        const AffNode* seg = node(k);
        if(!seg) continue;
        const AffField* u = fieldOf(*seg, TG('U','t','f','8'));
        const AffField* gl = fieldOf(*seg, TG('G','l','y','s'));
        const QString t = u ? QString::fromUtf8(u->bytes) : QString();
        int gi = 0;
        for(const QChar c : t){
            if(c != QChar(0)){ out += c; continue; }
            const AffNode* fld = (gl && gi < gl->kids.size()) ? node(gl->kids[gi]) : nullptr;
            ++gi;
            if(!fld || fld->types.isEmpty()) continue;
            if(fld->types[0] == TG('P','g','N','G')) out += QString::number(m_curPage + 1);
            else if(fld->types[0] == TG('D','c','F','G') && int(numOf(*fld, TG('T','y','p','e'), -1.0)) == 12) out += m_fileName;
        }
    }
    while(out.endsWith(QChar(0))) out.chop(1);
    return out;
}

// TxtA/TxtF/TxtS/CPTx. Affinity stores no laid-out glyph positions, so lines are reconstructed from the
// frame: artistic text has one baseline per hard break, starting ArtV below the frame top; frame text
// wraps to its columns (CoFr: ColW, GutW, BalC balances them) or, for TxtS, to its shape's outline.
QPainterPath affTextOutline(const QString& text, const QString& family, double size, int weight, bool italic, double x, double y);
void Converter::textNode(const AffNode& n, QString* outlines){
    // Linked frames share one story: the later ones continue where the earlier ran out of room.
    int flowKey = -1;
    const AffNode* story = node(firstKid(n, TG('S','t','S','t')));
    if(const AffNode* fl = node(firstKid(n, TG('F','l','o','w')))){
        const AffField* nods = fieldOf(*fl, TG('N','o','d','s'));
        if(nods && nods->kids.size() > 1){
            flowKey = nods->kids[0];
            if(!story) if(const AffNode* head = node(flowKey)) story = node(firstKid(*head, TG('S','t','S','t')));
        }
    }
    if(!story) return;
    const AffField* blok = fieldOf(*story, TG('B','l','o','k'));
    if(!blok || blok->kids.isEmpty()) return;

    double fx = 0.0, fy = 0.0, fr = 0.0, fb = 0.0, artV = -1.0, gut = 0.0;
    bool haveBox = false, balance = false, framed = false;
    QVector<double> colW;
    QString pathD; double pathOff = 0.0;
    const AffNode* frm = node(firstKid(n, TG('T','x','t','H')));
    if(frm){
        if(const AffField* b = fieldOf(*frm, TG('F','r','m','B'))){
            if(b->num.size() >= 4){ fx = finite(b->num[0]); fy = finite(b->num[1]); fr = finite(b->num[2]); fb = finite(b->num[3]); haveBox = true; }
        }
        artV = numOf(*frm, TG('A','r','t','V'), -1.0);
        if(const AffField* cw = fieldOf(*frm, TG('C','o','l','W'))){
            for(double v : cw->num) if(finite(v) > 0.0 && colW.size() < 64) colW.push_back(finite(v));
            framed = !colW.isEmpty();
            gut = qBound(0.0, numOf(*frm, TG('G','u','t','W'), 0.0), 1e6);
            balance = numOf(*frm, TG('B','a','l','C'), 0.0) != 0.0;
        }
        // Text on a path: PaFr holds the curve; Strt is where along it the text begins, as a fraction.
        if(const AffNode* pc = node(firstKid(*frm, TG('P','C','r','v')))){
            pathD = curveData(*pc);
            const AffField* st = fieldOf(*frm, TG('S','t','r','t'));
            if(st && !st->num.isEmpty()) pathOff = qBound(0.0, finite(st->num[0]), 1.0);
        }
    }
    if(!haveBox){
        const AffField* bb = fieldOf(n, TG('b','b','o','x'));
        if(bb && bb->num.size() >= 4){ fx = finite(bb->num[0]); fy = finite(bb->num[1]); fr = finite(bb->num[2]); fb = finite(bb->num[3]); haveBox = true; }
    }
    if(!haveBox) return;

    // A frame's Xfrm resizes the box, not its glyphs: the columns are stored unscaled, and the text keeps
    // its point size however far the frame was stretched. Lay out in the scaled box, then undo the scale.
    double sx = 1.0, sy = 1.0;
    if(framed){
        const AffField* xf = fieldOf(n, TG('X','f','r','m'));
        if(xf && xf->num.size() >= 6){
            sx = qHypot(finite(xf->num[0]), finite(xf->num[3]));
            sy = qHypot(finite(xf->num[1]), finite(xf->num[4]));
        }
        if(sx < 1e-6 || sy < 1e-6){ sx = 1.0; sy = 1.0; }
        fx *= sx; fr *= sx; fy *= sy; fb *= sy;
        for(double& w : colW) w *= sx;
    }

    // A shaped frame: each line is as wide as the outline allows at its height.
    QPainterPath shapeGeo;
    if(framed && fieldOf(n, TG('S','h','p','e')) && m_parked == 0){
        QString held; held.swap(m_body);
        const qsizetype parked = m_parked; m_parked = held.size() + 1;
        const int before = m_st.shapes;
        shapeNode(n);
        m_st.shapes = before;
        QString geo; geo.swap(m_body); m_body.swap(held); m_parked = parked;
        const QPainterPath local = elementPath(geo);
        shapeGeo = QTransform::fromScale(sx, sy).map(local);
        // The frame's own outline, drawn behind its text.
        if(!outlines && !local.isEmpty()) put(QStringLiteral("<path d=\"%1\"%2/>").arg(affPathData(local), styleFor(n, -2, -1)));
    }

    struct Piece { QString text; TxStyle st; };
    struct Line { QVector<Piece> pieces; double size = 12.0, asc = 10.0, adv = 14.0, width = 0.0, x = 0.0, w = 0.0, y = 0.0; int align = 0; };
    QVector<Line> lines;
    const double wrapW = framed ? colW[0] : 0.0;
    auto measure = [](const QString& t, const TxStyle& st){ return QFontMetricsF(txFont(st)).horizontalAdvance(t) * st.size / 100.0; };
    // The outline's clear span at a line's baseline.
    double shapeY = fy;
    auto bandSpan = [&](double asc, double& l, double& w){
        QPainterPath b; b.addRect(QRectF(fx - 1e5, shapeY + asc, 2e5 + (fr - fx), 0.5));
        const QRectF a = shapeGeo.intersected(b).boundingRect();
        if(a.isEmpty()) return false;
        l = a.left(); w = a.width();
        return true;
    };

    for(int bi = 0; bi < blok->kids.size() && bi < 4096; ++bi){
        const AffNode* blk = node(blok->kids[bi]);
        if(!blk) continue;
        const AffNode* glyp = node(firstKid(*blk, TG('G','l','y','p')));
        if(!glyp) continue;
        const QString text = blockText(*glyp);
        if(text.isEmpty()) continue;

        // Per-character style from the run list.
        QVector<QPair<int,TxStyle>> runs;
        TxStyle cur;
        if(const AffNode* gatt = node(firstKid(*blk, TG('G','A','t','t')))){
            if(const AffField* rs = fieldOf(*gatt, TG('R','u','n','s'))){
                for(int k : rs->kids){
                    const AffNode* r = node(k);
                    if(!r) continue;
                    if(const AffNode* item = node(firstKid(*r, TG('I','t','e','m')))) cur = runStyle(*item, cur);
                    runs.push_back({ int(qBound(0.0, numOf(*r, TG('I','n','d','x'), 1e9), 1e9)), cur });
                }
            }
        }
        if(runs.isEmpty()) runs.push_back({ int(1e9), cur });
        auto styleAt = [&](int i)->const TxStyle& {
            for(const auto& r : runs) if(i < r.first) return r.second;
            return runs.last().second;
        };

        double lead = 1.0; bool leadOfSize = false; int align = 0;
        if(const AffNode* patt = node(firstKid(*blk, TG('P','A','t','t')))){
            const AffField* rs = fieldOf(*patt, TG('R','u','n','s'));
            const AffNode* r0 = (rs && !rs->kids.isEmpty()) ? node(rs->kids[0]) : nullptr;
            const AffNode* item = r0 ? node(firstKid(*r0, TG('I','t','e','m'))) : nullptr;
            if(item){
                const AffField* db = fieldOf(*item, TG('D','o','u','b'));
                if(db && !db->num.isEmpty()){ const double v = finite(db->num[0]); if(v >= 0.1 && v <= 10.0) lead = v; }
                // Ints[0] is the paragraph alignment: 0 left, 1 centre, 2 right, 3 justify.
                const AffField* iv = fieldOf(*item, TG('I','n','t','s'));
                if(iv && !iv->num.isEmpty()) align = int(qBound(0.0, finite(iv->num[0]), 3.0));
                // Ints[1] picks the leading rule: 0 the font's natural height, 1 a multiple of the size.
                if(iv && iv->num.size() > 1) leadOfSize = iv->num[1] == 1.0;
            }
        }
        if(frm && artV >= 0.0) if(const AffField* aa = fieldOf(*frm, TG('A','r','t','A'))) if(!aa->num.isEmpty()) align = int(qBound(0.0, finite(aa->num[0]), 3.0));

        // Split on hard breaks; wrap on spaces when there is a width to wrap to.
        int pos = 0;
        static const QRegularExpression kBreak(QStringLiteral("[\\x{2029}\\x{2028}\\n\\r]"));
        const QStringList paras = text.split(kBreak);
        for(const QString& para : paras){
            const int start = pos;
            pos += para.size() + 1;
            double spanX = fx, spanW = fr - fx;
            auto flush = [&](int i0, int i1){
                Line ln; ln.align = align;
                double mx = 0.0, asc = 0.0, adv = 0.0;
                for(int i = i0; i < i1; ){
                    const TxStyle& st = styleAt(start + i);
                    int j = i + 1;
                    while(j < i1 && &styleAt(start + j) == &st) ++j;
                    ln.pieces.push_back({ para.mid(i, j - i), st });
                    ln.width += measure(para.mid(i, j - i), st);
                    const QFontMetricsF fm(txFont(st));
                    mx = qMax(mx, st.size);
                    asc = qMax(asc, fm.ascent() * st.size / 100.0);
                    adv = qMax(adv, fm.height() * st.size / 100.0);
                    i = j;
                }
                if(mx <= 0.0){
                    const TxStyle& st = styleAt(start);
                    const QFontMetricsF fm(txFont(st));
                    mx = st.size; asc = fm.ascent() * st.size / 100.0; adv = fm.height() * st.size / 100.0;
                }
                ln.size = mx; ln.asc = asc; ln.adv = leadOfSize ? mx * lead : adv * lead;
                if(!shapeGeo.isEmpty()){ ln.x = spanX; ln.w = spanW; ln.y = shapeY + asc; shapeY += ln.adv; }
                lines.push_back(ln);
            };
            // Move down to the next band wide enough to hold a line at all.
            // Affinity leaves the outline's narrow cap empty; 60 % of the frame width reproduces where its
            // first line lands in the sample ellipse.
            auto nextSpan = [&](const TxStyle& st){
                const QFontMetricsF fm(txFont(st));
                const double h = fm.height() * st.size / 100.0 * lead, asc = fm.ascent() * st.size / 100.0;
                const double minW = qMax(st.size * 2.0, (lines.isEmpty() ? 0.6 : 0.0) * (fr - fx));
                for(int g = 0; g < 512 && shapeY < fb; ++g){
                    if(bandSpan(asc, spanX, spanW) && spanW >= minW) return;
                    shapeY += qMax(1.0, h * 0.25);
                }
                spanX = fx; spanW = fr - fx;
            };
            const bool shaped = !shapeGeo.isEmpty();
            if(para.isEmpty()){ flush(0, 0); continue; }
            if(wrapW <= 0.0){ flush(0, para.size()); continue; }
            if(shaped) nextSpan(styleAt(start));
            int ls = 0, lastSpace = -1; double w = 0.0;
            for(int i = 0; i < para.size(); ++i){
                const TxStyle& st = styleAt(start + i);
                w += measure(para.mid(i, 1), st);
                if(para[i].isSpace()) lastSpace = i;
                const double avail = shaped ? spanW : wrapW;
                if(w > avail && i > ls){
                    const int cut = lastSpace > ls ? lastSpace : i;
                    flush(ls, cut);
                    if(shaped) nextSpan(st);
                    ls = (cut == lastSpace) ? cut + 1 : cut;
                    i = ls - 1; w = 0.0; lastSpace = -1;
                }
            }
            if(ls < para.size()) flush(ls, para.size());
        }
    }
    // A closing paragraph mark is not a line: it would skew how balanced columns divide the text.
    while(!lines.isEmpty() && lines.last().pieces.isEmpty()) lines.removeLast();
    if(lines.isEmpty()) return;

    // Mask mode: real curves, because a <text> inside a <clipPath> contributes no geometry downstream.
    double y0 = artV >= 0.0 ? fy + artV : fy + lines[0].asc;
    if(outlines){
        double y = y0;
        for(const Line& ln : lines){
            for(const Piece& pc : ln.pieces){
                const QPainterPath gp = QTransform::fromScale(1.0 / sx, 1.0 / sy).map(affTextOutline(pc.text, pc.st.family, pc.st.size, pc.st.weight, pc.st.italic, fx, y));
                if(!gp.isEmpty()) *outlines += QStringLiteral("<path d=\"%1\"/>").arg(affPathData(gp));
            }
            y += ln.adv;
        }
        return;
    }

    auto attrs = [&](const TxStyle& st){
        QString a = QStringLiteral(" font-size=\"%1\" fill=\"%2\"").arg(fnum(st.size), colHex(st.col));
        if(!st.family.isEmpty()) a += QStringLiteral(" font-family=\"%1\"").arg(xmlEsc(st.family));
        if(st.weight != 400) a += QStringLiteral(" font-weight=\"%1\"").arg(st.weight);
        if(st.italic) a += QStringLiteral(" font-style=\"italic\"");
        if(st.col.alphaF() < 0.999) a += QStringLiteral(" fill-opacity=\"%1\"").arg(fnum(st.col.alphaF()));
        return a;
    };
    auto emitLine = [&](const Line& ln, double lx, double lw, double y){
        if(!budget()) return;
        QString body;
        for(const Piece& pc : ln.pieces) body += pc.text;
        if(body.trimmed().isEmpty()) return;
        double ax = lx; const char* anchor = nullptr;
        if(lw > 0.0){
            if(ln.align == 1){ ax = lx + lw / 2.0; anchor = "middle"; }
            else if(ln.align == 2){ ax = lx + lw; anchor = "end"; }
        }
        QString e = QStringLiteral("<text x=\"%1\" y=\"%2\"").arg(fnum(ax), fnum(y));
        if(anchor) e += QStringLiteral(" text-anchor=\"%1\"").arg(QLatin1String(anchor));
        e += fxShapeAttrs();
        if(m_fx.blur > 0.0) e += QStringLiteral(" xarq:blur=\"%1\"").arg(fnum(fxBlur(m_fx.blur)));
        if(!pathD.isEmpty()) e += QStringLiteral(" xarq:path=\"%1\" xarq:path-start=\"%2\"").arg(pathD, fnum(pathOff));
        if(ln.pieces.size() == 1){
            e += attrs(ln.pieces[0].st) + QLatin1Char('>') + xmlEsc(ln.pieces[0].text) + QStringLiteral("</text>");
        } else {
            e += attrs(ln.pieces[0].st) + QLatin1Char('>');
            for(const Piece& pc : ln.pieces) e += QStringLiteral("<tspan%1>%2</tspan>").arg(attrs(pc.st), xmlEsc(pc.text));
            e += QStringLiteral("</text>");
        }
        put(e);
        ++m_st.shapes; ++m_st.texts;
    };

    if(!pathD.isEmpty()){
        // The text's ascent, not its baseline, rides on the curve: move the baseline in along the normal.
        const QPainterPath curve = elementPath(QStringLiteral("<path d=\"%1\"/>").arg(pathD));
        if(!curve.isEmpty()){
            QPainterPath shifted;
            for(int i = 0; i <= 200; ++i){
                const double t = i / 200.0;
                const QPointF a = curve.pointAtPercent(t);
                const double ang = qDegreesToRadians(curve.angleAtPercent(t));
                const QPointF q = a + QPointF(std::sin(ang), std::cos(ang)) * lines[0].asc;
                if(i == 0) shifted.moveTo(q); else shifted.lineTo(q);
            }
            pathD = affPathData(shifted);
        }
        emitLine(lines[0], 0.0, 0.0, 0.0);
        return;
    }

    // Algn places the text in the frame: 0 top, 1 middle, 2 bottom.
    if(framed && frm && shapeGeo.isEmpty()){
        const int va = int(numOf(*frm, TG('A','l','g','n'), 0.0));
        if(va == 1 || va == 2){
            double tot = 0.0;
            for(const Line& ln : lines) tot += ln.adv;
            if(colW.size() > 1 && balance) tot /= colW.size();
            const double spare = (fb - fy) - tot;
            if(spare > 0.0) y0 += va == 1 ? spare / 2.0 : spare;
        }
    }
    if(!framed){
        double y = y0;
        for(const Line& ln : lines){ emitLine(ln, fx, fr > fx ? fr - fx : 0.0, y); y += ln.adv; }
        return;
    }
    const bool unscale = (sx != 1.0 || sy != 1.0) && budget();
    if(unscale) m_body += QStringLiteral("<g transform=\"scale(%1 %2)\">").arg(fnum(1.0 / sx), fnum(1.0 / sy));
    if(!shapeGeo.isEmpty()){
        for(const Line& ln : lines) emitLine(ln, ln.x, ln.w, ln.y);
    } else {
        // Columns. Balanced text splits the line count evenly; otherwise a column fills to the frame's foot.
        const int nc = colW.size();
        const int perCol = (nc > 1 && balance) ? (lines.size() + nc - 1) / nc : lines.size();
        int li = flowKey >= 0 ? m_flowLines.value(flowKey, 0) : 0;
        double colX = fx;
        for(int c = 0; c < nc && li < lines.size(); ++c){
            double y = y0;
            int inCol = 0;
            while(li < lines.size()){
                if(c + 1 < nc && ((balance && inCol >= perCol) || (!balance && y > fb && inCol > 0))) break;
                // Text that doesn't fit is hidden, or carries on in the next linked frame.
                if(c + 1 == nc && y > fb + 1.0 && inCol > 0) { c = nc; break; }
                const Line& ln = lines[li];
                emitLine(ln, colX, colW[c], y);
                y += ln.adv; ++li; ++inCol;
            }
            if(c < nc) colX += colW[c] + gut;
        }
        if(flowKey >= 0) m_flowLines.insert(flowKey, li);
    }
    if(unscale) m_body += QStringLiteral("</g>");
}

// QPainterPath -> SVG path data. Used to turn a text layer into outlines when it is acting as a
// clipping mask: a <text> element inside a <clipPath> contributes no glyph geometry downstream, so
// the letters have to be real curves or the clip silently degrades to nothing.
QString affPathData(const QPainterPath& p){
    QString d;
    const int n = p.elementCount();
    for(int i = 0; i < n; ){
        const QPainterPath::Element e = p.elementAt(i);
        if(e.type == QPainterPath::MoveToElement){ d += QStringLiteral("M%1 %2").arg(fnum(e.x), fnum(e.y)); ++i; }
        else if(e.type == QPainterPath::LineToElement){ d += QStringLiteral("L%1 %2").arg(fnum(e.x), fnum(e.y)); ++i; }
        else if(e.type == QPainterPath::CurveToElement && i + 2 < n){
            const QPainterPath::Element c2 = p.elementAt(i+1), ep = p.elementAt(i+2);
            d += QStringLiteral("C%1 %2 %3 %4 %5 %6")
                   .arg(fnum(e.x), fnum(e.y), fnum(c2.x), fnum(c2.y), fnum(ep.x), fnum(ep.y));
            i += 3;
        } else ++i;
    }
    return d;
}

// Glyph outlines for one line, at a baseline of (x, y) in document units. The path is built at a
// fixed 100px size and scaled down, so the result never depends on the screen DPI that QFont would
// otherwise use to turn a point size into pixels.
QPainterPath affTextOutline(const QString& text, const QString& family, double size,
                            int weight, bool italic, double x, double y){
    QPainterPath out;
    if(text.isEmpty() || !(size > 0.0)) return out;
    QFont f;
    if(!family.isEmpty()) f.setFamily(family);
    f.setPixelSize(100);
    f.setWeight(QFont::Weight(qBound(1, weight, 1000)));
    f.setItalic(italic);
    QPainterPath glyphs;
    glyphs.addText(0.0, 0.0, f, text);
    if(glyphs.isEmpty()) return out;
    QTransform t;
    t.translate(x, y);
    t.scale(size / 100.0, size / 100.0);
    out = t.map(glyphs);
    return out;
}

// ---------------------------------------------------------------- raster layers ---
// A DyBm keeps a pixel layer as 256x256 SINGLE-CHANNEL planes — one container stream per channel per
// tile — laid out in raster order over a ceil(W/256) x ceil(H/256) grid:
//
//   Sta1..Sta4   per-tile state of channels 1..4, one entry per grid cell. 4 means a plane is listed
//                for that tile; 2 means the whole cell is the constant 0xFF; every other state (0, 1,
//                5) means the cell is the constant 0x00 and no plane is stored. The two constants are
//                what make a flat photo cheap: an opaque placed image stores no alpha plane at all,
//                just 2 in every cell. Both readings are pinned against the documents' own previews —
//                a placed screenshot whose alpha is all 2 must come out opaque, and a black brush
//                stroke whose R,G,B are all 1 must come out black.
//   Idx1..Idx4   the Blck nodes for exactly the state-4 cells, in the same raster order. A Blck's
//                Data blob holds the FAT entry name ("d/1", "d/01" — little-endian hex digits), and
//                identical planes are shared through the tree's DAG, so two channels routinely point
//                at one block. Blck also carries Rect/IRct, a dirty-rect hint the full-res read
//                does not need — the plane itself is already positioned within its tile.
//
// Channels are R,G,B,A with STRAIGHT (unpremultiplied) alpha: in the sample documents colour channels
// exceed alpha on 43% of the antialiased edge pixels, which premultiplied data cannot do.
//
// Idx*/Sta* is mip 0. The MI*/MT* groups that follow repeat the identical layout for each smaller mip
// level; a full-resolution import ignores them. The plane bytes themselves need no unpacking here —
// the container's byte-delta predictor already ran during extraction, and the CRC32 it is checked
// against covers the restored plane, so a wrong guess anywhere upstream declines instead of guessing.
// One tile's 256 x 256 bytes from its d/ stream, bare or wrapped in a one-field object stream.
static QByteArray tileBytes(const AffArchive& ar, const AffTree& tr, int blkIdx){
    const AffNode* blk = tr.node(blkIdx);
    const AffField* nm = blk ? fieldOf(*blk, TG('D','a','t','a')) : nullptr;
    if(!nm || nm->bytes.isEmpty() || nm->bytes.size() > 256) return {};
    QByteArray plane = namedStream(ar, QString::fromUtf8(nm->bytes));
    if(plane.size() == qsizetype(kTile) * kTile || plane.isEmpty()) return plane;
    if(plane.size() > qsizetype(kTile) * kTile + 4096) return {};
    AffTree pt(plane);
    if(!pt.load() || pt.root < 0) return {};
    const AffNode* pr = pt.node(pt.root);
    if(!pr) return {};
    for(const AffField& pf : pr->fields) if(pf.bytes.size() == qsizetype(kTile) * kTile) return pf.bytes;
    return {};
}

// One channel of a bitmap at any sample size. Tiles cut BYTES, so a 16-bit plane's grid is twice as wide,
// which is what TWi/THi say. States as in rasterImage: 4 stored, 2 constant 0xFF, others constant 0.
static QByteArray bitmapPlane(const AffArchive& ar, const AffTree& tr, const AffNode& dybm, int ch, int W, int H, int bps){
    const u32 idxTag = TG('I','d','x', char('1' + ch)), staTag = TG('S','t','a', char('1' + ch));
    const AffField* sta = fieldOf(dybm, staTag);
    const AffField* idx = fieldOf(dybm, idxTag);
    const qint64 rowBytes = qint64(W) * bps;
    const int tx = int((rowBytes + kTile - 1) / kTile), ty = (H + kTile - 1) / kTile;
    if(!sta || sta->num.size() != qsizetype(qint64(tx) * ty)) return {};
    QByteArray out(rowBytes * H, 0);
    int next = 0;
    for(qint64 cell = 0; cell < qint64(tx) * ty; ++cell){
        const double st = sta->num[qsizetype(cell)];
        if(st != 2.0 && st != 4.0) continue;
        const qint64 ox = (cell % tx) * kTile; const int oy = int(cell / tx) * kTile;
        const int cw = int(qMin<qint64>(kTile, rowBytes - ox)), chh = qMin(kTile, H - oy);
        QByteArray t;
        if(st == 4.0){
            if(!idx || next >= idx->kids.size()) return {};
            t = tileBytes(ar, tr, idx->kids[next++]);
            if(t.size() != qsizetype(kTile) * kTile) continue;
        }
        for(int r = 0; r < chh; ++r){
            char* dst = out.data() + (oy + r) * rowBytes + ox;
            if(st == 2.0) memset(dst, 0xFF, size_t(cw));
            else memcpy(dst, t.constData() + qsizetype(r) * kTile, size_t(cw));
        }
    }
    return out;
}

// A grey mask bitmap (Frmt 6, or 7 at 16 bits little-endian) as 8-bit grey. Null for anything else.
static QImage bitmapGrey(const AffArchive& ar, const AffTree& tr, const AffNode& dybm, i64& pixelBudget){
    const double fmt = numOf(dybm, TG('F','r','m','t'), -1.0);
    if(fmt != 6.0 && fmt != 7.0) return {};
    const double dw = numOf(dybm, TG('B','m','p','W'), 0.0), dh = numOf(dybm, TG('B','m','p','H'), 0.0);
    if(!(dw >= 1.0) || !(dh >= 1.0) || dw > 1e6 || dh > 1e6) return {};
    const int W = int(dw), H = int(dh);
    const i64 px = i64(W) * H;
    if(px > kMaxRasterPx || px > pixelBudget) return {};
    pixelBudget -= px;
    const int bps = fmt == 7.0 ? 2 : 1;
    const QByteArray p = bitmapPlane(ar, tr, dybm, 0, W, H, bps);
    if(p.isEmpty()) return {};
    QImage img(W, H, QImage::Format_Grayscale8);
    if(img.isNull()) return {};
    for(int y = 0; y < H; ++y){
        uchar* d = img.scanLine(y);
        const u8* q = reinterpret_cast<const u8*>(p.constData()) + qint64(y) * W * bps;
        for(int x = 0; x < W; ++x) d[x] = bps == 2 ? q[x*2+1] : q[x];
    }
    return img;
}

// 16-bit RGBA (Frmt 1, little-endian samples) and 8-bit CMYK + alpha (Frmt 4) pixel layers.
static QImage rasterOtherFormat(const AffArchive& ar, const AffTree& tr, const AffNode& dybm, int fmt, i64& pixelBudget){
    const double dw = numOf(dybm, TG('B','m','p','W'), 0.0), dh = numOf(dybm, TG('B','m','p','H'), 0.0);
    if(!(dw >= 1.0) || !(dh >= 1.0) || dw > 1e6 || dh > 1e6) return {};
    const int W = int(dw), H = int(dh);
    const i64 px = i64(W) * H;
    if(px > kMaxRasterPx || px > pixelBudget) return {};
    pixelBudget -= px;
    const int nch = fmt == 4 ? 5 : 4, bps = fmt == 1 ? 2 : 1;
    QByteArray pl[5];
    for(int c = 0; c < nch; ++c) pl[c] = bitmapPlane(ar, tr, dybm, c, W, H, bps);
    for(int c = 0; c < nch - 1; ++c) if(pl[c].isEmpty()) return {};
    QImage img(W, H, QImage::Format_RGBA8888);
    if(img.isNull()) return {};
    for(int y = 0; y < H; ++y){
        u8* d = img.scanLine(y);
        for(int x = 0; x < W; ++x, d += 4){
            const qint64 i = (qint64(y) * W + x) * bps + (bps - 1);       // the high byte
            auto v = [&](int c){ return pl[c].isEmpty() ? 255 : int(u8(pl[c][i])); };
            if(fmt == 4){ const int k = 255 - v(3); d[0] = u8((255 - v(0)) * k / 255); d[1] = u8((255 - v(1)) * k / 255); d[2] = u8((255 - v(2)) * k / 255); d[3] = u8(v(4)); }
            else { d[0] = u8(v(0)); d[1] = u8(v(1)); d[2] = u8(v(2)); d[3] = u8(v(3)); }
        }
    }
    return img;
}

QImage rasterImage(const AffArchive& ar, const AffTree& tr, const AffNode& dybm,
                   i64& pixelBudget, QPoint* originOut){
    const double rasterFmt = numOf(dybm, TG('F','r','m','t'), -1.0);
    if(rasterFmt == 1.0 || rasterFmt == 4.0){
        if(originOut) *originOut = QPoint(0, 0);
        return rasterOtherFormat(ar, tr, dybm, int(rasterFmt), pixelBudget);
    }
    if(rasterFmt != 0.0) return {};
    const double dw = numOf(dybm, TG('B','m','p','W'), 0.0);
    const double dh = numOf(dybm, TG('B','m','p','H'), 0.0);
    if(!(dw >= 1.0) || !(dh >= 1.0) || dw > 1e6 || dh > 1e6) return {};   // clamp before the cast, not after
    const int W = int(dw), H = int(dh);
    const i64 px = i64(W) * i64(H);
    if(px > kMaxRasterPx || px > pixelBudget) return {};
    // Charge the canvas BEFORE allocating it, and never refund: the cost of a raster layer is paid in
    // the allocate-and-fill, which happens whether or not any plane turns out to be readable. Metering
    // only what we successfully emit would let a tiny file (a few KB of near-empty raster nodes, which
    // compress to nothing) demand a 256 MB allocation and memset thousands of times over.
    pixelBudget -= px;

    const int tx = (W + kTile - 1) / kTile, ty = (H + kTile - 1) / kTile;
    const i64 cells = i64(tx) * i64(ty);

    static const u32 kIdx[4] = { TG('I','d','x','1'), TG('I','d','x','2'), TG('I','d','x','3'), TG('I','d','x','4') };
    static const u32 kSta[4] = { TG('S','t','a','1'), TG('S','t','a','2'), TG('S','t','a','3'), TG('S','t','a','4') };

    QImage img(W, H, QImage::Format_RGBA8888);
    if(img.isNull()) return {};
    // No alpha state array at all means the layer simply has no alpha channel; the pixels it does
    // carry are opaque. None of the sample documents take this branch — every one of them stores an
    // alpha channel, if only as the constant 2 — so it is a safety net, not a verified reading.
    img.fill(fieldOf(dybm, kSta[3]) ? QColor(Qt::transparent) : QColor(0, 0, 0, 255));

    bool any = false;
    for(int c = 0; c < 4; ++c){
        const AffField* idx = fieldOf(dybm, kIdx[c]);
        const AffField* sta = fieldOf(dybm, kSta[c]);
        // A channel that is constant everywhere (a fully opaque photo's alpha) stores no blocks at
        // all, so an empty index list is not a reason to skip the channel — only a missing state
        // array is.
        if(!sta) continue;
        if(sta->num.size() != qsizetype(cells)) return {};   // layout disagrees with W/H: decline, never guess
        int next = 0;
        for(i64 cell = 0; cell < cells; ++cell){
            const double st = sta->num[qsizetype(cell)];
            if(st != 2.0 && st != 4.0) continue;             // 0/1/5 leave the cell at 0x00
            const int ox = int(cell % tx) * kTile, oy = int(cell / tx) * kTile;
            const int cw = qMin(kTile, W - ox), chh = qMin(kTile, H - oy);
            if(st == 2.0){                                   // constant 0xFF over the whole cell
                for(int r = 0; r < chh; ++r){
                    u8* dst = img.scanLine(oy + r) + qsizetype(ox) * 4 + c;
                    for(int x = 0; x < cw; ++x) dst[qsizetype(x) * 4] = 0xFF;
                }
                any = true;
                continue;
            }
            if(!idx || next >= idx->kids.size()) return {};  // fewer blocks than states-with-data
            const AffNode* blk = tr.node(idx->kids[next++]);
            if(!blk) continue;
            const AffField* nm = fieldOf(*blk, TG('D','a','t','a'));
            if(!nm || nm->bytes.isEmpty() || nm->bytes.size() > 256) continue;
            QByteArray plane = namedStream(ar, QString::fromUtf8(nm->bytes));
            if(plane.isEmpty()) continue;
            if(plane.size() != qsizetype(kTile) * kTile){
                // Some revisions wrap the plane in its own one-field object stream instead of storing
                // it bare; both forms appear in the same document generation. The wrapper adds a
                // couple of dozen bytes, so anything much larger is not a plane — refuse to re-parse
                // it as a tree rather than paying a whole nested parse per referencing cell (a
                // crafted Blck can otherwise name doc.dat itself).
                if(plane.size() > qsizetype(kTile) * kTile + 4096) continue;
                AffTree pt(plane);
                if(!pt.load() || pt.root < 0) continue;
                const AffNode* pr = pt.node(pt.root);
                if(!pr) continue;
                // "Data" is the wrapper's CLASS name; the field inside it is tagged DatI. Looking up
                // a field called Data therefore never matched and this whole branch could not fire,
                // which demoted a wrapped-plane document to its 512px preview. Take the field by the
                // only property that actually identifies a plane — being exactly one tile of bytes —
                // so neither spelling can break it again.
                const AffField* pd = nullptr;
                for(const AffField& pf : pr->fields)
                    if(pf.bytes.size() == qsizetype(kTile) * kTile){ pd = &pf; break; }
                if(!pd) continue;
                plane = pd->bytes;
            }
            if(plane.size() != qsizetype(kTile) * kTile) continue;
            const u8* src = reinterpret_cast<const u8*>(plane.constData());
            for(int r = 0; r < chh; ++r){
                u8* dst = img.scanLine(oy + r) + qsizetype(ox) * 4 + c;
                const u8* s = src + qsizetype(r) * kTile;
                for(int x = 0; x < cw; ++x) dst[qsizetype(x) * 4] = s[x];
            }
            any = true;
        }
    }
    if(!any) return {};

    // A layer's grid spans the whole canvas even when the paint covers a corner of it. Emitting the
    // full box would hand XarQ a mostly transparent image that still enlarges the document bounds, so
    // trim to the pixels that can actually show. The offset travels back with the image because the
    // caller places it in the same layer-pixel space the planes were assembled in.
    int x0 = W, y0 = H, x1 = -1, y1 = -1;
    for(int y = 0; y < H; ++y){
        const u8* row = img.constScanLine(y) + 3;
        int lo = -1, hi = -1;
        for(int x = 0; x < W; ++x){
            if(row[qsizetype(x) * 4] == 0) continue;
            if(lo < 0) lo = x;
            hi = x;
        }
        if(lo < 0) continue;
        if(y < y0) y0 = y;
        y1 = y;
        if(lo < x0) x0 = lo;
        if(hi > x1) x1 = hi;
    }
    if(x1 < x0 || y1 < y0) return {};                   // every pixel fully transparent
    if(originOut) *originOut = QPoint(x0, y0);
    if(x0 == 0 && y0 == 0 && x1 == W-1 && y1 == H-1) return img;
    return img.copy(QRect(x0, y0, x1 - x0 + 1, y1 - y0 + 1));
}

// ImgN → Bitm (DyBm) → Bckg names a container entry holding the placed file VERBATIM (its own little
// "Blck" stream with a Data blob). Those bytes go straight into a data: URI, so placed images import at
// full fidelity. Images whose pixels live only as d/* raster tiles are counted and skipped.
bool Converter::placedOriginal(const AffNode& bm){
    const double bw = numOf(bm, TG('B','m','p','W'), 0.0);
    const double bh = numOf(bm, TG('B','m','p','H'), 0.0);
    const AffField* bg = fieldOf(bm, TG('B','c','k','g'));
    if(!bg || bg->bytes.isEmpty() || !(bw > 0.0) || !(bh > 0.0) || bw > 1e6 || bh > 1e6) return false;

    const QString entry = QString::fromUtf8(bg->bytes);
    if(entry.isEmpty() || entry.size() > 256) return false;
    const QByteArray blk = namedStream(m_ar, entry);
    if(blk.isEmpty()) return false;
    AffTree bt(blk);
    if(!bt.load() || bt.root < 0) return false;
    QByteArray orig;
    if(const AffNode* r0 = bt.node(bt.root))
        if(const AffField* dat = fieldOf(*r0, TG('D','a','t','a'))) orig = dat->bytes;
    if(orig.size() < 8 || orig.size() > kMaxImageBytes || m_imageBytes + orig.size() > kMaxImageTotal) return false;
    const u8* q = reinterpret_cast<const u8*>(orig.constData());
    const char* mime = nullptr;
    if(!memcmp(q, "\x89PNG\r\n\x1a\n", 8)) mime = "image/png";
    else if(q[0]==0xFF && q[1]==0xD8) mime = "image/jpeg";
    else if(!memcmp(q, "GIF8", 4)) mime = "image/gif";
    else if(!memcmp(q, "RIFF", 4) && orig.size() > 12 && !memcmp(q+8, "WEBP", 4)) mime = "image/webp";
    else if(!memcmp(q, "II*\0", 4) || !memcmp(q, "MM\0*", 4)) mime = "image/tiff";
    else if(q[0]=='B' && q[1]=='M') mime = "image/bmp";
    if(!mime || !budget()) return false;

    m_imageBytes += orig.size();
    put(QStringLiteral("<image x=\"0\" y=\"0\" width=\"%1\" height=\"%2\" xlink:href=\"data:%3;base64,%4\"/>")
          .arg(fnum(bw), fnum(bh), QLatin1String(mime), QString::fromLatin1(orig.toBase64())));
    ++m_st.shapes; ++m_st.images;
    return true;
}

// EmbN: a placed document embedded whole. EmbR -> EmCn -> EmbC names a stream holding "EmDc", four zero
// bytes, and then a complete Affinity container (Affinity converts a placed SVG or PDF to its own format
// on placement), so this is one level of recursion over the same reader. Nesting is capped, and every
// id the inner document defines is prefixed so it cannot collide with the host page.
void Converter::embedNode(const AffNode& n){
    const AffNode* er = node(firstKid(n, TG('B','i','t','m')));
    const AffNode* ec = er ? node(firstKid(*er, TG('E','m','C','n'))) : nullptr;
    const AffField* nm = ec ? fieldOf(*ec, TG('E','m','b','C')) : nullptr;
    if(!nm || nm->bytes.isEmpty() || nm->bytes.size() > 256 || m_nest >= 3){ ++m_st.skippedRaster; return; }
    QByteArray doc = namedStream(m_ar, QString::fromUtf8(nm->bytes));
    if(doc.size() < 88 || !doc.startsWith("EmDc")){ ++m_st.skippedRaster; return; }
    doc.remove(0, 8);
    AffArchive inner;
    if(!parseContainer(reinterpret_cast<const u8*>(doc.constData()), doc.size(), inner)){ ++m_st.skippedRaster; return; }
    const QByteArray dat = namedStream(inner, QStringLiteral("doc.dat"));
    AffTree tree(dat);
    if(dat.isEmpty() || !tree.load()){ ++m_st.skippedRaster; return; }
    Converter c(inner, tree);
    c.m_nest = m_nest + 1;
    c.m_idPrefix = m_idPrefix + QStringLiteral("e%1").arg(++m_embedSeq);
    c.m_rasterPixels = m_rasterPixels;
    QString body, defs;
    const int made = c.firstSpread(body, defs);
    m_rasterPixels = c.m_rasterPixels;
    if(made <= 0){ ++m_st.skippedRaster; return; }
    if(m_body.size() + m_defs.size() + m_parked + body.size() + defs.size() >= kMaxSvg){ ++m_st.skippedRaster; return; }
    // The placed document's origin is its CENTRE: at Xfrm (800,230) scale 1.1 a 400x200 page spans
    // (580,120)..(1020,340) in Affinity's own export.
    double px = 0, py = 0, pw = 0, ph = 0;
    {
        const AffNode* r = tree.node(tree.root);
        const AffNode* d = r ? tree.node(firstKid(*r, TG('D','o','c','R'))) : nullptr;
        const AffField* sps = d ? fieldOf(*d, TG('C','h','l','d')) : nullptr;
        const AffNode* sp = (sps && !sps->kids.isEmpty()) ? tree.node(sps->kids[0]) : nullptr;
        if(d && sp) c.spreadBox(*sp, *d, px, py, pw, ph);
    }
    m_defs += defs;
    m_body += QStringLiteral("<g transform=\"translate(%1,%2)\">").arg(fnum(-(px + pw / 2.0)), fnum(-(py + ph / 2.0))) + body + QStringLiteral("</g>");
    m_st.shapes += made;
}

void Converter::imageNode(const AffNode& n){
    const AffNode* bm = node(firstKid(n, TG('B','i','t','m')));
    if(!bm){ ++m_st.skippedRaster; return; }
    if(placedOriginal(*bm)) return;
    // No embedded original — an image placed and then edited keeps its pixels only as d/* tiles,
    // exactly like a pixel layer, so assemble it the same way.
    rasterNode(n);
}

// AdCh lists the layers attached to this one. An Affinity VECTOR MASK lives there as an ordinary
// shape carrying CLiT = 1 ("clip"): it crops the pixels instead of painting. Its Xfrm maps it into the
// same layer-pixel space the planes are assembled in, so re-emitting its geometry under that transform
// gives a <clipPath> that can hang directly on the raster. A mask we cannot turn into geometry leaves
// the layer uncropped rather than cropping it to a guessed box.
// Whether the node carries a clipping child at all, regardless of whether we can turn it into
// geometry — the difference between "no mask" and "a mask we failed to express".
bool hasClipChild(const AffTree& tree, const AffNode& n){
    const AffField* ad = fieldOf(n, TG('A','d','C','h'));
    if(!ad) return false;
    // Filter and adjustment children (FlRN, *RA) sit in this same list and also carry CLiT = 1, but they
    // modify the layer rather than crop it. Treating one as an inexpressible clip made the whole layer
    // vanish; only an object that has an outline can be a clip.
    for(int k : ad->kids){
        const AffNode* m = tree.node(k);
        if(!m || m->types.isEmpty()) continue;
        const u32 c = m->types[0];
        if(c == TG('P','C','r','v') || c == TG('S','h','p','N') || c == TG('S','h','R','N') ||
           c == TG('T','x','t','A') || c == TG('T','x','t','F') || c == TG('T','x','t','S') || c == TG('C','P','T','x')) return true;
    }
    return false;
}

// An MRst on this node: attached in AdCh, or a mask layer among its children, which masks the parent.
// White shows the layer and black hides it. The image gets a black border because the renderer holds
// its edge pixels outward, and past a mask's edge the layer is hidden.
QString Converter::pixelMaskAttrs(const AffNode& n){
    QVector<int> found;
    for(u32 list : { TG('A','d','C','h'), TG('C','h','l','d') })
        if(const AffField* f = fieldOf(n, list))
            for(int k : f->kids){
                const AffNode* m = node(k);
                if(!m || m->types.isEmpty() || m->types[0] != TG('M','R','s','t')) continue;
                const AffField* vis = fieldOf(*m, TG('V','i','s','i'));
                if(vis && !vis->num.isEmpty() && vis->num[0] == 0.0) continue;
                found.push_back(k);
            }
    if(found.isEmpty()) return {};
    m_st.extraMasks += found.size() - 1;
    const AffNode* m = node(found[0]);
    const AffNode* bm = node(firstKid(*m, TG('B','i','t','m')));
    if(!bm){ ++m_st.skippedRaster; return {}; }
    const QImage grey = bitmapGrey(m_ar, m_tr, *bm, m_rasterPixels);
    if(grey.isNull()){ ++m_st.skippedRaster; return {}; }
    QImage bordered(grey.width() + 2, grey.height() + 2, QImage::Format_Grayscale8);
    bordered.fill(0);
    for(int y = 0; y < grey.height(); ++y) memcpy(bordered.scanLine(y + 1) + 1, grey.constScanLine(y), size_t(grey.width()));
    QByteArray png; QBuffer buf(&png);
    if(!buf.open(QIODevice::WriteOnly) || !bordered.save(&buf, "PNG")){ ++m_st.skippedRaster; return {}; }
    if(png.size() > kMaxImageBytes || m_imageBytes + png.size() > kMaxImageTotal){ ++m_st.skippedRaster; return {}; }
    m_imageBytes += png.size();
    double x[6] = { 1, 0, 0, 0, 1, 0 };
    if(const AffField* xf = fieldOf(*m, TG('X','f','r','m'))) if(xf->num.size() >= 6) for(int i = 0; i < 6; ++i) x[i] = finite(xf->num[i]);
    // Image pixels to this node's space: Xfrm is a b tx c d ty; the border moves the origin back by one.
    const double e = x[2] - x[0] - x[1], f = x[5] - x[3] - x[4];
    ++m_st.pixelMasks;
    return QStringLiteral(" xarq:pmask=\"%1\" xarq:pmask-xf=\"%2 %3 %4 %5 %6 %7\"")
        .arg(QString::fromLatin1(png.toBase64()), fnum(x[0]), fnum(x[3]), fnum(x[1]), fnum(x[4]), fnum(e), fnum(f));
}

// A compound's operands combine in order, each by its own ComO: 0 add, 1 intersect, 2 subtract, 3 exclude.
// The reference reader confirms 0 and 2 only; 1 and 3 are the remaining pair, assigned by the panel's wording.
QPainterPath Converter::compoundOutline(const AffNode& n, int depth){
    QPainterPath out;
    const AffField* ch = fieldOf(n, TG('C','h','l','d'));
    if(!ch || depth > 32) return out;
    bool first = true;
    for(int k : ch->kids){
        const AffNode* m = node(k);
        if(!m || m->types.isEmpty()) continue;
        if(++m_compoundVisits > 20000) return out;
        const u32 c = m->types[0];
        QPainterPath geo;
        if(c == TG('C','o','m','p')) geo = compoundOutline(*m, depth + 1);
        else if(c == TG('P','C','r','v') || c == TG('S','h','p','N') || c == TG('S','h','R','N') || c == TG('S','N','E','N') || c == TG('S','N','R','R')){
            QString held; held.swap(m_body);
            const qsizetype parked = m_parked; m_parked = held.size() + 1;
            const int before = m_st.shapes;
            if(c == TG('P','C','r','v')) curveNode(*m); else shapeNode(*m);
            m_st.shapes = before;
            QString g; g.swap(m_body); m_body.swap(held); m_parked = parked;
            geo = elementPath(g);
        } else continue;
        if(const AffField* xf = fieldOf(*m, TG('X','f','r','m'))) if(xf->num.size() >= 6)
            geo = QTransform(finite(xf->num[0]), finite(xf->num[3]), finite(xf->num[1]), finite(xf->num[4]), finite(xf->num[2]), finite(xf->num[5])).map(geo);
        geo.setFillRule(Qt::WindingFill);
        if(first){ out = geo; first = false; continue; }
        const int op = int(qBound(0.0, numOf(*m, TG('C','o','m','O'), 0.0), 8.0));
        if(op == 2) out = out.subtracted(geo);
        else if(op == 1) out = out.intersected(geo);
        else if(op == 3){ const QPainterPath both = out.intersected(geo); out = out.united(geo).subtracted(both); }
        else out = out.united(geo);
    }
    return out;
}

void Converter::compoundNode(const AffNode& n){
    const QPainterPath p = compoundOutline(n, 0);
    if(p.isEmpty()){ ++m_st.skippedShape; return; }
    emitPainted(n, QStringLiteral("<path d=\"%1\" fill-rule=\"evenodd\"\x01/>").arg(affPathData(p)));
    ++m_st.shapes;
}

// A fill layer paints the whole of its bitmap's area.
void Converter::fillLayerNode(const AffNode& n){
    const AffNode* bm = node(firstKid(n, TG('B','i','t','m')));
    const double w = bm ? numOf(*bm, TG('B','m','p','W'), 0.0) : 0.0, h = bm ? numOf(*bm, TG('B','m','p','H'), 0.0) : 0.0;
    if(!(w > 0.0) || !(h > 0.0) || w > 1e6 || h > 1e6){ ++m_st.skippedShape; return; }
    emitPainted(n, QStringLiteral("<rect x=\"0\" y=\"0\" width=\"%1\" height=\"%2\"\x01/>").arg(fnum(w), fnum(h)));
    ++m_st.shapes;
}

QString Converter::maskGeometry(const AffNode& n){
    const AffField* ad = fieldOf(n, TG('A','d','C','h'));
    if(!ad || ad->kids.isEmpty()) return {};
    QString out;
    for(int k : ad->kids){
        const AffNode* m = node(k);
        if(!m || m->types.isEmpty()) continue;
        if(int(qBound(-1e6, numOf(*m, TG('C','L','i','T'), 0.0), 1e6)) != 1) continue;
        const AffField* vis = fieldOf(*m, TG('V','i','s','i'));
        if(vis && !vis->num.isEmpty() && vis->num[0] == 0.0) continue;

        // shapeNode/curveNode write through put(); borrow m_body so their geometry lands here instead.
        QString held;
        held.swap(m_body);
        m_parked = held.size();
        const int shapesBefore = m_st.shapes;
        const u32 cls = m->types[0];
        if(cls == TG('P','C','r','v'))                                    curveNode(*m);
        else if(cls == TG('S','h','p','N') || cls == TG('S','h','R','N')) shapeNode(*m);
        else if(cls == TG('T','x','t','A') || cls == TG('T','x','t','F') || cls == TG('T','x','t','S') || cls == TG('C','P','T','x')){
            // A text layer used as a mask has to become outlines; real2.afphoto's whole icon is a
            // "V" cut out of a gradient panel this way.
            QString glyphs;
            textNode(*m, &glyphs);
            m_body += glyphs;
        }
        m_st.shapes = shapesBefore;                    // a clip outline is not a drawable
        QString geo;
        geo.swap(m_body);
        m_body.swap(held);
        m_parked = 0;
        if(geo.isEmpty()) continue;

        const AffField* xf = fieldOf(*m, TG('X','f','r','m'));
        if(xf && xf->num.size() >= 6){
            double mm[6];
            for(int i = 0; i < 6; ++i) mm[i] = finite(xf->num[i]);
            geo = QStringLiteral("<g transform=\"%1\">%2</g>").arg(affMatrix(mm), geo);
        }
        out += geo;
    }
    return out;
}

// Rstr/MRst/SRst/EncR — a pixel layer. Its assembled planes go out as a PNG in a data: URI covering
// the layer's own BmpW x BmpH pixel box, which the node's Xfrm (written by walk) maps onto the page,
// exactly as for a placed image. Anything we cannot decode is counted, never silently dropped.
void Converter::rasterNode(const AffNode& n){
    const AffNode* bm = node(firstKid(n, TG('B','i','t','m')));
    if(!bm || !budget()){ ++m_st.skippedRaster; return; }
    // The same DyBm is routinely referenced by several layers (real.afdesign points six ImgN at one
    // 1920x1080 node), and assembling + PNG-encoding + base64-ing it once per reference cost seconds
    // and hundreds of MB, and charged the raster pixel budget six times over for one image.
    const int bmIdx = firstKid(n, TG('B','i','t','m'));
    const auto hit = m_adj.isEmpty() ? m_rasterCache.constFind(bmIdx) : m_rasterCache.constEnd();
    QPoint origin(0, 0);
    QString b64;
    QSize dim;
    if(hit != m_rasterCache.constEnd()){
        origin = hit->origin; b64 = hit->b64; dim = hit->size;
    } else {
        QImage img = rasterImage(m_ar, m_tr, *bm, m_rasterPixels, &origin);
        if(img.isNull()){ ++m_st.skippedRaster; return; }
        if(!m_adj.isEmpty()){
            QImage baked = img;
            for(int y = 0; y < baked.height(); ++y){
                u8* row = baked.scanLine(y);
                for(int x = 0; x < baked.width(); ++x, row += 4){
                    double r = row[0]/255.0, g = row[1]/255.0, b = row[2]/255.0;
                    for(const Adjust& a : m_adj) applyAdjust(a, r, g, b);
                    row[0] = u8(qBound(0.0, r, 1.0)*255.0+0.5); row[1] = u8(qBound(0.0, g, 1.0)*255.0+0.5); row[2] = u8(qBound(0.0, b, 1.0)*255.0+0.5);
                }
            }
            img = baked;
        }
        QByteArray png;
        QBuffer buf(&png);
        if(!buf.open(QIODevice::WriteOnly) || !img.save(&buf, "PNG")){ ++m_st.skippedRaster; return; }
        buf.close();
        if(png.size() > kMaxImageBytes || m_imageBytes + png.size() > kMaxImageTotal){ ++m_st.skippedRaster; return; }
        m_imageBytes += png.size();
        b64 = QString::fromLatin1(png.toBase64());
        dim = img.size();
        if(bmIdx >= 0 && m_rasterCache.size() < 256) m_rasterCache.insert(bmIdx, { origin, dim, b64 });
    }

    // The AdCh clip, if any, is written by walk() as a group around this element.
    put(QStringLiteral("<image x=\"%1\" y=\"%2\" width=\"%3\" height=\"%4\" xlink:href=\"data:image/png;base64,%5\"/>")
          .arg(fnum(origin.x()), fnum(origin.y()), fnum(dim.width()), fnum(dim.height()), b64));
    ++m_st.shapes; ++m_st.images; ++m_st.rasters;
}

bool Converter::readAdjust(const AffNode& n, Adjust& A) const {
    if(n.types.isEmpty()) return false;
    const u32 c = n.types[0];
    const AffNode* p = node(firstKid(n, TG('A','d','j','P')));
    auto nv = [&](const char* t, double d){ return p ? numOf(*p, TG(t[0],t[1],t[2],t[3]), d) : d; };
    if(c == TG('L','e','R','A')){ if(!p) return false; A.kind = Adjust::Levels; A.a = nv("Blac",0); A.b = nv("Whit",1); A.c = nv("Gamm",1); A.d = nv("OutB",0); A.e = nv("OutW",1); return true; }
    if(c == TG('B','C','R','A')){ if(!p) return false; A.kind = Adjust::BC; A.a = nv("Brig",0); A.b = nv("Ctrs",1); return true; }
    if(c == TG('H','s','R','A')){ if(!p) return false; A.kind = Adjust::HSL; A.a = nv("HueA",0); A.b = nv("SatA",0); A.c = nv("LumA",0); return true; }
    if(c == TG('I','n','R','A')){ A.kind = Adjust::Invert; return true; }
    if(c == TG('R','c','R','A')){ if(!p) return false; A.kind = Adjust::Recolour; A.a = nv("RecH",0); A.b = nv("RecS",1); A.c = nv("RecL",0); return true; }
    if(c == TG('B','W','R','A')){
        if(!p) return false; A.kind = Adjust::BW;
        A.wt[0] = nv("RedC",1); A.wt[1] = nv("Gree",1); A.wt[2] = nv("Blue",1); A.wt[3] = nv("Cyan",1); A.wt[4] = nv("Mage",1); A.wt[5] = nv("Yell",1);
        return true;
    }
    if(c == TG('S','C','R','A')){
        if(!p) return false; A.kind = Adjust::Selective;
        const AffField* w = fieldOf(*p, TG('W','e','i','g'));
        if(w) for(int i = 0; i < 36 && i < w->num.size(); ++i) A.sel[i] = finite(w->num[i]);
        A.relative = nv("Rela",1) != 0.0;
        return true;
    }
    if(c == TG('C','r','R','A')){
        if(!p) return false; A.kind = Adjust::Curves;
        auto sp = [&](u32 tag, Spline& out){
            const AffNode* s = node(firstKid(*p, tag));
            const AffField* v = s ? fieldOf(*s, TG('V','a','l','s')) : nullptr;
            const int cn = s ? int(qBound(0.0, numOf(*s, TG('C','n','t',' '), 0.0), 4096.0)) : 0;
            if(!v || cn < 2 || v->num.size() < qsizetype(cn) * 3) return;
            for(int i = 0; i < cn; ++i){ out.x.push_back(finite(v->num[i])); out.y.push_back(finite(v->num[cn+i])); out.d.push_back(finite(v->num[2*cn+i])); }
        };
        sp(TG('M','a','s','t'), A.master); sp(TG('C','1','S','p'), A.chan[0]); sp(TG('C','2','S','p'), A.chan[1]); sp(TG('C','3','S','p'), A.chan[2]);
        return true;
    }
    if(c == TG('C','B','R','A')){
        if(!p) return false; A.kind = Adjust::ColourBalance;
        static const char* const keys[9] = { "ShCR","ShMG","ShYB","MiCR","MiMG","MiYB","HiCR","HiMG","HiYB" };
        for(int i = 0; i < 9; ++i) A.sel[i] = qBound(-1.0, nv(keys[i], 0), 1.0);
        A.relative = nv("PeLu", 1) != 0.0;
        return true;
    }
    if(c == TG('E','x','R','A')){ if(!p) return false; A.kind = Adjust::Exposure; A.a = nv("Expo", 0); return true; }
    if(c == TG('V','b','R','A')){ if(!p) return false; A.kind = Adjust::Vibrance; A.a = qBound(-1.0, nv("Satu", 0), 1.0); A.b = qBound(-1.0, nv("Vibr", 0), 1.0); return true; }
    if(c == TG('P','o','R','A')){ if(!p) return false; A.kind = Adjust::Posterize; A.a = nv("Post", 8); return true; }
    if(c == TG('T','h','R','A')){ if(!p) return false; A.kind = Adjust::Threshold; A.a = nv("Thre", 0.5); A.b = nv("True", 1); A.c = nv("Fals", 0); return true; }
    if(c == TG('S','H','R','A')){ if(!p) return false; A.kind = Adjust::ShadowsHighlights; A.a = qBound(-1.0, nv("HigA", 0), 1.0); A.b = qBound(-1.0, nv("ShdA", 0), 1.0); return true; }
    if(c == TG('P','f','R','A')){
        if(!p) return false; A.kind = Adjust::LensFilter;
        // Lab in ICC 16-bit encoding.
        const QRgb f = labToRgbQ(nv("L_", 65535) / 65535.0 * 100.0, nv("A_", 32896) / 257.0 - 128.0, nv("B_", 32896) / 257.0 - 128.0);
        auto toLin = [](double v){ return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
        A.wt[0] = toLin(qRed(f) / 255.0); A.wt[1] = toLin(qGreen(f) / 255.0); A.wt[2] = toLin(qBlue(f) / 255.0);
        A.a = nv("Dens", 0.25); A.relative = nv("Pres", 1) != 0.0;
        return true;
    }
    if(c == TG('C','M','R','A')){
        if(!p) return false;
        const AffField* w = fieldOf(*p, TG('W','e','i','g'));
        if(!w || (w->num.size() % 3) != 0 || w->num.size() < 9 || w->num.size() > 36) return false;
        A.kind = Adjust::ChannelMixer; A.a = w->num.size() / 3;
        for(int i = 0; i < w->num.size(); ++i) A.sel[i] = finite(w->num[i]);
        return true;
    }
    if(c == TG('G','r','R','A')){
        const AffNode* np = node(firstKid(n, TG('N','A','j','P')));
        const AffNode* g = np ? node(firstKid(*np, TG('G','r','a','d'))) : nullptr;
        const AffField* cols = g ? fieldOf(*g, TG('C','o','l','s')) : nullptr;
        if(!cols || cols->kids.isEmpty()) return false;
        const AffField* posn = fieldOf(*g, TG('P','o','s','n'));
        struct St { double p, m; QColor c; };
        QVector<St> st;
        const int cnt = qMin(int(cols->kids.size()), 512);
        for(int i = 0; i < cnt; ++i){
            QColor col;
            if(!rawColourOf(cols->kids[i], col)) continue;
            double off = cnt > 1 ? double(i) / (cnt - 1) : 0.0, mid = 0.5;
            if(posn && posn->comps >= 2 && qsizetype(i) * posn->comps + 1 < posn->num.size()){ off = finite(posn->num[qsizetype(i) * posn->comps]); mid = qBound(0.001, finite(posn->num[qsizetype(i) * posn->comps + 1]), 0.999); }
            st.push_back({ qBound(0.0, off, 1.0), mid, col });
        }
        if(st.isEmpty()) return false;
        std::stable_sort(st.begin(), st.end(), [](const St& x, const St& y){ return x.p < y.p; });
        A.kind = Adjust::GradientMap; A.map.resize(256);
        for(int i = 0; i < 256; ++i){
            const double t = i / 255.0;
            QColor col = st.last().c;
            if(t <= st.first().p) col = st.first().c;
            else for(int k = 1; k < st.size(); ++k) if(t <= st[k].p){
                const double span = st[k].p - st[k-1].p;
                double u = span > 1e-9 ? (t - st[k-1].p) / span : 1.0;
                const double m = st[k-1].m;
                u = u < m ? 0.5 * u / m : 0.5 + 0.5 * (u - m) / (1.0 - m);
                const QColor x = st[k-1].c, y = st[k].c;
                col = QColor::fromRgbF(x.redF() + (y.redF()-x.redF())*u, x.greenF() + (y.greenF()-x.greenF())*u, x.blueF() + (y.blueF()-x.blueF())*u);
                break;
            }
            A.map[i] = col.rgb();
        }
        return true;
    }
    return false;
}

// Soft proof changes nothing in the artwork; everything else of this family that isn't read is reported.
bool isIgnoredAdjustment(const AffNode& n){
    if(n.types.isEmpty()) return false;
    const u32 c = n.types[0];
    if(c == TG('S','P','R','A')) return false;
    return (c & 0xffffu) == (u32('R') | (u32('A') << 8)) || (c & 0xffffu) == (u32('R') << 8 | u32('A')) || c == TG('3','D','L','A');
}

// A clip that keeps everything except the given shapes, in the space of the list that holds them.
QString Converter::eraseClip(const QVector<int>& erasers){
    QPainterPath keep;
    keep.addRect(QRectF(-1e6, -1e6, 2e6, 2e6));
    for(int k : erasers){
        const AffNode* m = node(k);
        if(!m || m->types.isEmpty()) continue;
        QString held; held.swap(m_body);
        const qsizetype parked = m_parked; m_parked = held.size() + 1;
        const int shapesBefore = m_st.shapes;
        const u32 c = m->types[0];
        if(c == TG('P','C','r','v')) curveNode(*m);
        else if(c == TG('S','h','p','N') || c == TG('S','h','R','N')) shapeNode(*m);
        m_st.shapes = shapesBefore;
        QString geo; geo.swap(m_body); m_body.swap(held); m_parked = parked;
        QPainterPath p = elementPath(geo);
        const AffField* x = fieldOf(*m, TG('X','f','r','m'));
        if(x && x->num.size() >= 6)
            p = QTransform(finite(x->num[0]), finite(x->num[3]), finite(x->num[1]), finite(x->num[4]), finite(x->num[2]), finite(x->num[5])).map(p);
        if(!p.isEmpty()) keep = keep.subtracted(p);
    }
    const QString id = QStringLiteral("%1aferase%2").arg(m_idPrefix).arg(++m_clipSeq);
    m_defs += QStringLiteral("<clipPath id=\"%1\"><path d=\"%2\" fill-rule=\"evenodd\"/></clipPath>").arg(id, affPathData(keep));
    return id;
}

// Walk a layer list bottom to top. An adjustment layer modifies everything below it in the same list, so
// each sibling is drawn with the adjustments that come AFTER it in force.
void Converter::walkChildren(const AffField& ch, int selfIdx){
    m_onPath.insert(selfIdx);
    ++m_depth;
    const int n = ch.kids.size();
    QVector<Adjust> after;                             // parsed once; [i] applies to siblings below i
    QVector<int> adjAt;
    for(int i = 0; i < n; ++i){
        const AffNode* k = node(ch.kids[i]);
        Adjust A;
        if(k && !k->types.isEmpty() && !readAdjust(*k, A) && isIgnoredAdjustment(*k)) ++m_st.ignoredAdjustments;
        if(k && !k->types.isEmpty() && readAdjust(*k, A)){
            const AffField* vis = fieldOf(*k, TG('V','i','s','i'));
            if(!vis || vis->num.isEmpty() || vis->num[0] != 0.0){ after.push_back(A); adjAt.push_back(i); }
        }
    }
    // Erase (blend 25) punches its shape out of what lies below it in the same list. It paints nothing.
    QVector<int> eraseAt;
    for(int i = 0; i < n; ++i){
        const AffNode* k = node(ch.kids[i]);
        const AffField* bl = k ? fieldOf(*k, TG('B','l','n','d')) : nullptr;
        if(bl && !bl->num.isEmpty() && bl->num[0] == 25.0 && bl->ever == 0) eraseAt.push_back(i);
    }
    const int base = m_adj.size();
    const QString baseKey = m_adjKey;
    for(int i = 0; i < n; ++i){
        if(eraseAt.contains(i)) continue;
        QVector<int> erasers;
        for(int e : eraseAt) if(e > i) erasers.push_back(ch.kids[e]);
        const QString ec = erasers.isEmpty() ? QString() : eraseClip(erasers);
        if(!ec.isEmpty()) m_body += QStringLiteral("<g clip-path=\"url(#%1)\">").arg(ec);
        m_adj.resize(base);
        m_adjKey = baseKey;
        for(int a = 0; a < after.size(); ++a)
            if(adjAt[a] > i){ m_adj.push_back(after[a]); m_adjKey += QStringLiteral("%1:").arg(ch.kids[adjAt[a]]); }
        walk(ch.kids[i]);
        if(!ec.isEmpty()) m_body += QStringLiteral("</g>");
        if(!budget()) break;
    }
    m_adj.resize(base);
    m_adjKey = baseKey;
    --m_depth;
    m_onPath.remove(selfIdx);
}

void Converter::walk(int idx){
    const AffNode* np = node(idx);
    if(!np || np->types.isEmpty()) return;
    if(m_depth > kMaxWalkDepth || m_onPath.contains(idx)) return;   // the tree is a DAG: never re-enter
    if(!budget()) return;
    const AffNode& n = *np;

    const AffField* vis = fieldOf(n, TG('V','i','s','i'));
    if(vis && !vis->num.isEmpty() && vis->num[0] == 0.0) return;    // hidden layers stay hidden

    QString attrs;
    const AffField* xf = fieldOf(n, TG('X','f','r','m'));
    if(xf && xf->num.size() >= 6){
        double m[6];
        for(int i = 0; i < 6; ++i) m[i] = finite(xf->num[i]);
        attrs += QStringLiteral(" transform=\"%1\"").arg(affMatrix(m));
    }
    const AffField* op = fieldOf(n, TG('O','p','a','c'));
    if(op && !op->num.isEmpty() && op->num[0] < 0.999)
        attrs += QStringLiteral(" opacity=\"%1\"").arg(fnum(qBound(0.0, finite(op->num[0]), 1.0)));
    const AffField* bl = fieldOf(n, TG('B','l','n','d'));
    if(bl && !bl->num.isEmpty()){
        const QString b = blendCss(int(qBound(0.0, bl->num[0], 1000.0)), bl->ever);
        if(!b.isEmpty()) attrs += QStringLiteral(" style=\"mix-blend-mode:%1\"").arg(b);
    }
    // Desc is the layer's name in Affinity's Layers panel — 1119 of them across the sample documents,
    // every one of them previously dropped, so a 940-object import arrived entirely anonymous.
    const AffField* de = fieldOf(n, TG('D','e','s','c'));
    if(de && !de->bytes.isEmpty() && de->bytes.size() <= 512){
        const QString nm = QString::fromUtf8(de->bytes).simplified();
        if(!nm.isEmpty()) attrs += QStringLiteral(" inkscape:label=\"%1\"").arg(xmlEsc(nm));
    }
    // The budget can stop output mid-document; the opening and closing tags must go together or the
    // SVG ends up unbalanced and the consuming parser throws the whole page away.
    const bool wroteGroup = budget();
    if(wroteGroup) m_body += QStringLiteral("<g%1>").arg(attrs);

    // An AdCh clip crops this node's own content AND its children, in the space Xfrm establishes — so
    // it goes on an inner group, where the clip coordinates are unambiguously post-transform. Affinity
    // hangs these off vector layers far more often than off pixel layers (66 of the 67 in the sample
    // documents), which is what keeps a framed mock-up inside its frame.
    QString clipClose;
    if(wroteGroup){
        const QString clip = maskGeometry(n);
        if(!clip.isEmpty()){
            const QString id = QStringLiteral("%1afclip%2").arg(m_idPrefix).arg(++m_clipSeq);
            m_defs += QStringLiteral("<clipPath id=\"%1\">%2</clipPath>").arg(id, clip);
            m_body += QStringLiteral("<g clip-path=\"url(#%1)\">").arg(id);
            clipClose = QStringLiteral("</g>");
            ++m_st.masks;
        } else if(hasClipChild(m_tr, n)){
            // The layer IS clipped but the mask produced no geometry we can express. Drawing its
            // content unclipped is far worse than drawing nothing: the whole point of the mask is
            // that most of the content is hidden, so an unclipped panel typically paints straight
            // over the artwork beneath it (real2.afphoto's gradient panel buries the entire icon).
            // Skip the subtree and account for it so the user is told.
            ++m_st.skippedShape;
            m_body += QStringLiteral("</g>");
            return;
        }
    }

    const QString pixelMask = wroteGroup ? pixelMaskAttrs(n) : QString();
    if(!pixelMask.isEmpty()){ m_body += QStringLiteral("<g%1>").arg(pixelMask); clipClose += QStringLiteral("</g>"); }

    // Effects carry down a group: each child casts its own, unless it sets that effect itself.
    struct FxScope { LayerFx& ref; LayerFx saved; ~FxScope(){ ref = saved; } } fxScope{ m_fx, m_fx };
    m_fx = readFx(n);

    // Adjustments attached to this layer itself (AdCh) apply to it and everything in it.
    const int adjBase = m_adj.size();
    const QString adjKeyBase = m_adjKey;
    if(const AffField* ad = fieldOf(n, TG('A','d','C','h')))
        for(int k : ad->kids){
            const AffNode* m = node(k);
            Adjust A;
            if(m && readAdjust(*m, A)){ m_adj.push_back(A); m_adjKey += QStringLiteral("%1:").arg(k); }
            else if(m && isIgnoredAdjustment(*m)) ++m_st.ignoredAdjustments;
        }
    const u32 cls = n.types[0];
    if(cls == TG('P','C','r','v'))                                        curveNode(n);
    else if(cls == TG('S','h','p','N') || cls == TG('S','h','R','N') ||
            cls == TG('S','N','E','N') || cls == TG('S','N','R','R'))     shapeNode(n);
    else if(cls == TG('C','o','m','p'))                                   compoundNode(n);
    else if(cls == TG('F','R','s','t'))                                   fillLayerNode(n);
    else if(cls == TG('T','x','t','C')){ if(firstKid(n, TG('S','t','S','t')) >= 0) textNode(n); else curveNode(n); }
    else if(cls == TG('T','x','t','A') || cls == TG('T','x','t','F') ||
            cls == TG('T','x','t','S') || cls == TG('C','P','T','x'))     textNode(n);
    else if(cls == TG('I','m','g','N'))                                   imageNode(n);
    else if(cls == TG('E','m','b','N'))                                   embedNode(n);
    else if(cls == TG('M','R','s','t')) {}                                  // its parent reads it as a mask
    else if(cls == TG('R','s','t','r') || cls == TG('S','R','s','t') || cls == TG('E','n','c','R')) rasterNode(n);

    // A compound's children are its operands, already consumed.
    const AffField* ch = cls == TG('C','o','m','p') ? nullptr : fieldOf(n, TG('C','h','l','d'));
    if(ch && !ch->kids.isEmpty()) walkChildren(*ch, idx);
    m_adj.resize(adjBase);
    m_adjKey = adjKeyBase;
    m_body += clipClose;
    if(wroteGroup) m_body += QStringLiteral("</g>");
}

// A DyBm holds its pixels as 256x256 tiles listed in Idx1..Idx4; an empty index list means the node
// exists but carries no pixels (every document has one of these whether or not it has raster content).
bool bitmapHasTiles(const AffNode& dybm){
    static const u32 kIdx[4] = { TG('I','d','x','1'), TG('I','d','x','2'), TG('I','d','x','3'), TG('I','d','x','4') };
    for(u32 t : kIdx){
        const AffField* f = fieldOf(dybm, t);
        if(f && !f->kids.isEmpty()) return true;
    }
    return false;
}

// The page rectangle of one spread. Affinity 1.x/2.x store it directly as SprB. Affinity 3 (container
// v12) dropped SprB: the page now lives in the spread model, Sprd.SpMd -> PagR -> PgIn.rctp, one PgIn per
// page, with the document default DfSz on DocN as a last resort. Missing it meant every v12 document
// fell through to a hardcoded 1000x1000 page, so nothing on it lined up with what the file draws.
void Converter::spreadBox(const AffNode& sp, const AffNode& doc, double& px, double& py, double& pw, double& ph) const {
    px = py = pw = ph = 0.0;
    auto take = [&](const AffField* f) -> bool {
        if(!f || f->num.size() < 4) return false;
        const double x0 = finite(f->num[0]), y0 = finite(f->num[1]), x1 = finite(f->num[2]), y1 = finite(f->num[3]);
        if(!(x1 - x0 > 0.0) || !(y1 - y0 > 0.0) || x1 - x0 > 1e7 || y1 - y0 > 1e7) return false;
        px = x0; py = y0; pw = x1 - x0; ph = y1 - y0;
        return true;
    };
    if(take(fieldOf(sp, TG('S','p','r','B')))) return;
    if(const AffNode* md = node(firstKid(sp, TG('S','p','M','d')))){
        if(const AffField* pr = fieldOf(*md, TG('P','a','g','R'))){
            // Several pages in one spread (a facing pair): the spread box is their union.
            double ux0 = 0, uy0 = 0, ux1 = 0, uy1 = 0; bool any = false;
            for(int k : pr->kids){
                const AffNode* pg = node(k);
                if(!pg) continue;
                const AffField* r = fieldOf(*pg, TG('r','c','t','p'));
                if(!r || r->num.size() < 4) continue;
                const double a0 = finite(r->num[0]), b0 = finite(r->num[1]), a1 = finite(r->num[2]), b1 = finite(r->num[3]);
                if(!(a1 > a0) || !(b1 > b0)) continue;
                if(!any){ ux0 = a0; uy0 = b0; ux1 = a1; uy1 = b1; any = true; }
                else { ux0 = qMin(ux0, a0); uy0 = qMin(uy0, b0); ux1 = qMax(ux1, a1); uy1 = qMax(uy1, b1); }
            }
            if(any && ux1 - ux0 < 1e7 && uy1 - uy0 < 1e7){ px = ux0; py = uy0; pw = ux1 - ux0; ph = uy1 - uy0; return; }
        }
    }
    if(const AffField* ds = fieldOf(doc, TG('D','f','S','z'))){
        if(ds->num.size() >= 2){
            const double w = finite(ds->num[0]), h = finite(ds->num[1]);
            if(w > 0.0 && h > 0.0 && w < 1e7 && h < 1e7){ pw = w; ph = h; return; }
        }
    }
    pw = ph = 1000.0;                                  // nothing usable anywhere: a neutral square
}

// The artwork of a document's first spread, for placing it inside another one.
int Converter::firstSpread(QString& body, QString& defs){
    const AffNode* root = node(m_tr.root);
    const AffNode* doc = root ? node(firstKid(*root, TG('D','o','c','R'))) : nullptr;
    const AffField* spreads = doc ? fieldOf(*doc, TG('C','h','l','d')) : nullptr;
    if(!spreads) return 0;
    for(int k : spreads->kids){
        const AffNode* sp = node(k);
        const AffField* ch = sp ? fieldOf(*sp, TG('C','h','l','d')) : nullptr;
        if(!ch) continue;
        m_body.clear(); m_defs.clear();
        for(int c : ch->kids) walk(c);
        if(m_st.shapes > 0){ body = m_body; defs = m_defs; return m_st.shapes; }
    }
    return 0;
}

QStringList Converter::run(AffinityStats* stats){
    QStringList pages;
    const AffNode* root = node(m_tr.root);
    if(!root) return pages;
    // A photo document keeps its pixels in a document-level raster (Pers.RasS), outside the spread tree.
    // Those tiles are not decoded yet, so count them — the caller reports the omission rather than
    // quietly presenting a page that is missing its main content.
    if(const AffNode* ras = node(firstKid(*root, TG('R','a','s','S'))))
        if(const AffNode* bm = node(firstKid(*ras, TG('B','i','t','m'))))
            if(bitmapHasTiles(*bm)) ++m_st.skippedRaster;
    m_docFmt = int(qBound(0.0, numOf(*root, TG('D','D','R','F'), 0.0), 64.0));
    if(const AffNode* uv = node(firstKid(*root, TG('U','V','C','n')))){
        const double ppi = numOf(*uv, TG('U','P','P','I'), 72.0);
        if(ppi >= 1.0 && ppi <= 100000.0) m_ptScale = ppi / 72.0;
    }
    const AffNode* doc = node(firstKid(*root, TG('D','o','c','R')));
    if(!doc) return pages;
    if(const AffNode* md = node(firstKid(*doc, TG('D','C','M','D'))))
        if(const AffField* nm = fieldOf(*md, TG('F','l','N','m'))) m_fileName = QString::fromUtf8(nm->bytes);
    const AffField* spreads = fieldOf(*doc, TG('C','h','l','d'));
    if(!spreads || spreads->kids.isEmpty()) return pages;

    // Artboards become pages of their own. XarQ pages share one size, so every page of such a document
    // takes the largest artboard's, with its artwork from the top left.
    auto isBoard = [&](const AffNode& k){
        if(numOf(k, TG('A','B','E','n'), 0.0) != 0.0) return true;
        const AffNode* pr = node(firstKid(k, TG('p','h','r','p')));
        return pr && !pr->types.isEmpty() && pr->types[0] == TG('a','p','r','p');
    };
    auto boardBox = [&](const AffNode& k, QRectF& out){
        const AffField* b = fieldOf(k, TG('S','h','p','B'));
        if(!b || b->num.size() < 4) return false;
        double m[6] = { 1, 0, 0, 0, 1, 0 };
        if(const AffField* xf = fieldOf(k, TG('X','f','r','m'))) if(xf->num.size() >= 6) for(int i = 0; i < 6; ++i) m[i] = finite(xf->num[i]);
        double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
        for(int c = 0; c < 4; ++c){
            const double x = finite(b->num[c & 1 ? 2 : 0]), y = finite(b->num[c & 2 ? 3 : 1]);
            const double qx = m[0]*x + m[1]*y + m[2], qy = m[3]*x + m[4]*y + m[5];
            x0 = qMin(x0, qx); y0 = qMin(y0, qy); x1 = qMax(x1, qx); y1 = qMax(y1, qy);
        }
        const QRectF r(QPointF(x0, y0), QPointF(x1, y1));
        if(!(r.width() >= 1.0 && r.height() >= 1.0 && r.width() < 1e7 && r.height() < 1e7)) return false;
        out = r;
        return true;
    };
    double maxW = 0.0, maxH = 0.0; bool anyBoard = false;
    for(int si = 0; si < spreads->kids.size() && si < 4096; ++si){
        const AffNode* sp = node(spreads->kids[si]);
        const AffField* ch = sp ? fieldOf(*sp, TG('C','h','l','d')) : nullptr;
        if(!ch) continue;
        bool here = false;
        for(int k : ch->kids){
            const AffNode* kn = node(k);
            QRectF r;
            if(kn && isBoard(*kn) && boardBox(*kn, r)){ here = anyBoard = true; maxW = qMax(maxW, r.width()); maxH = qMax(maxH, r.height()); }
        }
        if(!here){ double x, y, w, h; spreadBox(*sp, *doc, x, y, w, h); maxW = qMax(maxW, w); maxH = qMax(maxH, h); }
    }

    auto finishPage = [&](double px, double py, double pw, double ph, const AffinityPageInfo& info){
        QString svg;
        svg.reserve(m_body.size() + m_defs.size() + 256);
        svg += QStringLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" "
                  "xmlns:inkscape=\"http://www.inkscape.org/namespaces/inkscape\" xmlns:xarq=\"urn:xarq\" "
                              "viewBox=\"%1 %2 %3 %4\" width=\"%3\" height=\"%4\">")
                  .arg(fnum(px), fnum(py), fnum(pw), fnum(ph));
        if(!m_defs.isEmpty()) svg += QStringLiteral("<defs>") + m_defs + QStringLiteral("</defs>");
        svg += m_body;
        svg += QStringLiteral("</svg>");
        pages << svg;
        m_st.pageInfo.push_back(info);
    };
    // Margins and bleed are stored left, top, right, bottom: on the page in Affinity 2+, on the spread before.
    auto spreadInfo = [&](const AffNode& sp, double px, double py, double pw, double ph){
        AffinityPageInfo info;
        info.width = pw; info.height = ph;
        if(const AffField* g = fieldOf(sp, TG('G','d','s','H'))) for(double v : g->num) if(info.guidesH.size() < 4096) info.guidesH.push_back(finite(v) - py);
        if(const AffField* g = fieldOf(sp, TG('G','d','s','V'))) for(double v : g->num) if(info.guidesV.size() < 4096) info.guidesV.push_back(finite(v) - px);
        const AffField* mr = fieldOf(sp, TG('M','r','g','n'));
        if(const AffNode* md = node(firstKid(sp, TG('S','p','M','d')))){
            if(const AffNode* pg = node(firstKid(*md, TG('P','a','g','R')))) if(numOf(*pg, TG('u','s','m','g'), 0.0) != 0.0) mr = fieldOf(*pg, TG('m','r','g','n'));
        }
        if(mr && mr->num.size() >= 4){
            info.marginL = qBound(0.0, finite(mr->num[0]), pw); info.marginT = qBound(0.0, finite(mr->num[1]), ph);
            info.marginR = qBound(0.0, finite(mr->num[2]), pw); info.marginB = qBound(0.0, finite(mr->num[3]), ph);
        }
        if(const AffField* bl = fieldOf(sp, TG('b','l','e','d'))) if(bl->num.size() >= 4)
            for(int i = 0; i < 4; ++i) m_st.bleed = qMax(m_st.bleed, qBound(0.0, finite(bl->num[i]), 1e5));
        return info;
    };

    for(int si = 0; si < spreads->kids.size() && si < 4096; ++si){
        const AffNode* sp = node(spreads->kids[si]);
        if(!sp) continue;
        double px = 0, py = 0, pw = 0, ph = 0;
        spreadBox(*sp, *doc, px, py, pw, ph);
        const AffField* ch = fieldOf(*sp, TG('C','h','l','d'));

        QVector<int> boards, loose;
        if(ch) for(int k : ch->kids){
            const AffNode* kn = node(k);
            QRectF r;
            (kn && isBoard(*kn) && boardBox(*kn, r) ? boards : loose).push_back(k);
        }
        if(!boards.isEmpty()){
            spreadInfo(*sp, px, py, pw, ph);                 // for the bleed; guides on a spread of boards belong to no one page
            m_onPath.insert(spreads->kids[si]);
            ++m_depth;
            for(int k : boards){
                QRectF r;
                boardBox(*node(k), r);
                m_body.clear(); m_defs.clear(); m_gradCache.clear();
                m_curPage = pages.size();
                const int before = m_st.shapes;
                // Artboards clip what they hold; the import drops the clip for whatever already fits.
                const QString id = QStringLiteral("%1board%2").arg(m_idPrefix).arg(++m_clipSeq);
                m_defs += QStringLiteral("<clipPath id=\"%1\"><rect x=\"%2\" y=\"%3\" width=\"%4\" height=\"%5\"/></clipPath>").arg(id, fnum(r.left()), fnum(r.top()), fnum(r.width()), fnum(r.height()));
                m_body += QStringLiteral("<g clip-path=\"url(#%1)\">").arg(id);
                walk(k);
                m_body += QStringLiteral("</g>");
                if(m_st.shapes == before) continue;
                AffinityPageInfo info;
                info.width = maxW; info.height = maxH;
                finishPage(r.left(), r.top(), maxW, maxH, info);
                ++m_st.artboards;
            }
            --m_depth;
            m_onPath.remove(spreads->kids[si]);
            // Objects outside every artboard don't belong to a page in Affinity; they keep one of their own.
            if(!loose.isEmpty()){
                m_body.clear(); m_defs.clear(); m_gradCache.clear();
                m_curPage = pages.size();
                const int before = m_st.shapes;
                m_onPath.insert(spreads->kids[si]); ++m_depth;
                for(int k : loose) walk(k);
                --m_depth; m_onPath.remove(spreads->kids[si]);
                if(m_st.shapes != before){ AffinityPageInfo info; info.width = maxW; info.height = maxH; finishPage(px, py, maxW, maxH, info); }
            }
            continue;
        }

        m_body.clear(); m_defs.clear(); m_gradCache.clear();
        m_curPage = pages.size();
        const int before = m_st.shapes;
        if(ch) walkChildren(*ch, spreads->kids[si]);
        if(m_st.shapes == before) continue;
        if(anyBoard){ pw = maxW; ph = maxH; }
        finishPage(px, py, pw, ph, spreadInfo(*sp, px, py, pw, ph));
    }
    m_st.pages = pages.size();
    if(stats) *stats = m_st;
    return pages;
}

}   // namespace

// ---------------------------------------------------------------- public API ---
bool affinityIsContainer(const uint8_t* p, int64_t n){
    return p && isContainerHeader(p, n);
}

QByteArray affinityPreviewPng(const uint8_t* p, int64_t n){
    if(!p || !isContainerHeader(p, n)) return {};
    const u64 t = rdu64(p + 24);
    if(t == 0 || t > u64(n) || u64(n) - t < 29) return {};
    const u8* h = p + t;
    if(rdu32(h) != 0xFFFFFFFFu || memcmp(h + 4, "Thmb", 4) != 0) return {};
    if(rdu32(h + 16) != 29) return {};                       // header length
    const u32 pngLen = rdu32(h + 24);
    if(pngLen < 8 || u64(pngLen) > u64(n) - (t + 29)) return {};
    return QByteArray(reinterpret_cast<const char*>(p + t + 29), qsizetype(pngLen));
}

QStringList affinityToSvgPages(const uint8_t* p, int64_t n, AffinityStats* stats){
    if(stats) *stats = AffinityStats{};
    if(!p || n < 80) return {};
    AffArchive ar;
    if(!parseContainer(p, n, ar)) return {};
    const QByteArray doc = namedStream(ar, QStringLiteral("doc.dat"));
    if(doc.isEmpty()) return {};
    AffTree tree(doc);
    const bool loaded = tree.load();
    if(stats){ stats->treeBytes = doc.size(); stats->treeConsumed = tree.consumed(); stats->exactParse = loaded && tree.consumed() == doc.size(); }
    if(!loaded) return {};
    Converter conv(ar, tree);
    const AffinityStats pre = stats ? *stats : AffinityStats{};
    const QStringList pages = conv.run(stats);
    if(stats){ stats->treeBytes = pre.treeBytes; stats->treeConsumed = pre.treeConsumed; stats->exactParse = pre.exactParse; }
    return pages;
}

QStringList affinityToSvgPages(const uint8_t* p, int64_t n){
    return affinityToSvgPages(p, n, nullptr);
}

bool affinityVerifyStreams(const uint8_t* p, int64_t n, int* okOut, int* badOut){
    if(okOut) *okOut = 0;
    if(badOut) *badOut = 0;
    if(!p || n < 80) return false;
    AffArchive ar;
    if(!parseContainer(p, n, ar)) return false;
    int ok = 0, bad = 0;
    for(auto it = ar.files.constBegin(); it != ar.files.constEnd(); ++it){
        if(extractStream(ar, *it).isEmpty()) ++bad; else ++ok;
    }
    if(okOut) *okOut = ok;
    if(badOut) *badOut = bad;
    return true;
}

bool affinityZstdAvailable(){
#ifdef HAVE_ZSTD
    return true;
#else
    return false;
#endif
}

}   // namespace xara
