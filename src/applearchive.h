// Readers for two Apple formats that several design apps' files are built on: binary property lists as
// written by NSKeyedArchiver, and Apple's framed LZ4.
#pragma once
#include <QByteArray>
#include <QHash>
#include <QString>
#include <QVector>
#include <QtEndian>
#include <cstring>
#include <memory>

namespace xara {

// --- binary property list -----------------------------------------------------------------------
struct PObj {
    enum T { Null, Bool, Int, Real, Str, Data, Arr, Dict, Uid } t = Null;
    qint64 i = 0; double d = 0; QString s; QByteArray b;
    QVector<int> arr;                 // object numbers
    QHash<QString,int> dict;          // key text → object number
};
class BPlist {
public:
    bool open(const QByteArray& f){
        d = f; n = f.size();
        if(n < 40 || !f.startsWith("bplist0")) return false;
        const uchar* t = u() + n - 32;
        off = t[6]; ref = t[7];
        count = qFromBigEndian<quint64>(t + 8); top = qFromBigEndian<quint64>(t + 16);
        table = qFromBigEndian<quint64>(t + 24);
        if(off < 1 || off > 8 || ref < 1 || ref > 8 || count > quint64(n) || top >= count) return false;
        if(table + count * off > quint64(n)) return false;
        memo.resize(int(count));
        return true;
    }
    int topObject() const { return int(top); }
    const PObj& obj(int k){
        static const PObj none;
        if(k < 0 || k >= memo.size()) return none;
        if(!memo[k]) memo[k] = std::make_shared<PObj>(parse(k));
        return *memo[k];
    }
private:
    const uchar* u() const { return reinterpret_cast<const uchar*>(d.constData()); }
    quint64 be(qint64 o, int w) const { quint64 v = 0; if(o < 0 || o + w > n) return 0; for(int i = 0; i < w; ++i) v = v << 8 | u()[o + i]; return v; }
    PObj parse(int k) const {
        PObj r;
        const quint64 o = be(qint64(table) + qint64(k) * off, off);
        if(o >= quint64(n)) return r;
        const uchar m = u()[o]; const int hi = m >> 4; qint64 len = m & 15; qint64 q = qint64(o) + 1;
        auto longLen = [&](){ if(len == 15){ if(q >= n) return false; const int ib = u()[q]; if((ib >> 4) != 1) return false; const int w = 1 << (ib & 15); if(w > 8) return false; len = qint64(be(q + 1, w)); q += 1 + w; } return len >= 0 && len <= n; };
        switch(hi){
            case 0: if(m == 8 || m == 9){ r.t = PObj::Bool; r.i = m == 9; } break;
            case 1: { const int w = 1 << (m & 15); if(w <= 8){ r.t = PObj::Int; r.i = qint64(be(q, w)); } break; }
            case 2: { const int w = 1 << (m & 15); r.t = PObj::Real;
                if(w == 4){ quint32 x = quint32(be(q, 4)); float f; std::memcpy(&f, &x, 4); r.d = f; }
                else if(w == 8){ quint64 x = be(q, 8); std::memcpy(&r.d, &x, 8); } break; }
            case 4: if(longLen() && q + len <= n){ r.t = PObj::Data; r.b = d.mid(int(q), int(len)); } break;
            case 5: if(longLen() && q + len <= n){ r.t = PObj::Str; r.s = QString::fromLatin1(d.constData() + q, int(len)); } break;
            case 6: if(longLen() && q + len * 2 <= n){ r.t = PObj::Str; r.s.resize(int(len)); for(qint64 i = 0; i < len; ++i) r.s[int(i)] = QChar(ushort(be(q + i * 2, 2))); } break;
            case 7: if(longLen() && q + len <= n){ r.t = PObj::Str; r.s = QString::fromUtf8(d.constData() + q, int(len)); } break;
            case 8: { r.t = PObj::Uid; r.i = qint64(be(q, int(len) + 1)); break; }
            case 10: case 12: if(longLen() && q + len * ref <= n){ r.t = PObj::Arr; r.arr.reserve(int(len)); for(qint64 i = 0; i < len; ++i) r.arr.push_back(int(be(q + i * ref, ref))); } break;
            case 13: if(longLen() && q + 2 * len * ref <= n){
                r.t = PObj::Dict;
                for(qint64 i = 0; i < len; ++i){
                    const int key = int(be(q + i * ref, ref)), val = int(be(q + (len + i) * ref, ref));
                    r.dict.insert(keyText(key), val);
                }
            } break;
            default: break;
        }
        return r;
    }
    QString keyText(int k) const {
        if(k < 0 || k >= int(count)) return QString();
        const quint64 o = be(qint64(table) + qint64(k) * off, off);
        if(o >= quint64(n)) return QString();
        const uchar m = u()[o]; const int hi = m >> 4; qint64 len = m & 15; qint64 q = qint64(o) + 1;
        if(hi != 5 && hi != 7) return QString();
        if(len == 15){ if(q >= n) return QString(); const int w = 1 << (u()[q] & 15); if(w > 8) return QString(); len = qint64(be(q + 1, w)); q += 1 + w; }
        if(len < 0 || q + len > n) return QString();
        return QString::fromUtf8(d.constData() + q, int(len));
    }
    QByteArray d; qint64 n = 0; int off = 0, ref = 0; quint64 count = 0, top = 0, table = 0;
    mutable QVector<std::shared_ptr<PObj>> memo;
};

// NSKeyedArchiver: everything is an index into $objects, and index 0 means nil.
struct KeyedArchive {
    BPlist pl; QVector<int> objects;
    bool open(const QByteArray& f, int* root){
        if(!pl.open(f)) return false;
        const PObj t = pl.obj(pl.topObject());
        if(t.t != PObj::Dict) return false;
        const PObj arr = pl.obj(t.dict.value("$objects", -1));
        if(arr.t != PObj::Arr) return false;
        objects = arr.arr;
        const PObj tops = pl.obj(t.dict.value("$top", -1));
        const PObj rootUid = pl.obj(tops.dict.value("root", -1));
        if(rootUid.t != PObj::Uid) return false;
        *root = int(rootUid.i);
        return true;
    }
    const PObj& at(int uid){ static const PObj none; return (uid > 0 && uid < objects.size()) ? pl.obj(objects[uid]) : none; }
    // A dictionary entry, following a reference if it holds one.
    PObj field(const PObj& dict, const char* key){
        const auto it = dict.dict.constFind(QLatin1String(key));
        if(it == dict.dict.constEnd()) return PObj();
        const PObj& v = pl.obj(it.value());
        return v.t == PObj::Uid ? at(int(v.i)) : v;
    }
    int fieldUid(const PObj& dict, const char* key){
        const auto it = dict.dict.constFind(QLatin1String(key));
        if(it == dict.dict.constEnd()) return 0;
        const PObj& v = pl.obj(it.value());
        return v.t == PObj::Uid ? int(v.i) : 0;
    }
    QString str(const PObj& dict, const char* key){ const PObj v = field(dict, key); return (v.t == PObj::Str && v.s != QLatin1String("$null")) ? v.s : QString(); }
    double num(const PObj& dict, const char* key, double def = 0){ const PObj v = field(dict, key); return v.t == PObj::Real ? v.d : (v.t == PObj::Int || v.t == PObj::Bool) ? double(v.i) : def; }
    bool flag(const PObj& dict, const char* key){ return num(dict, key) != 0; }
    QVector<int> arrayUids(const PObj& ns){
        QVector<int> out;
        const PObj items = field(ns, "NS.objects");
        for(int k : items.arr){ const PObj& e = pl.obj(k); if(e.t == PObj::Uid) out.push_back(int(e.i)); }
        return out;
    }
};

// --- LZ4 ---------------------------------------------------------------------------------------
inline bool lz4Block(const uchar* s, qint64 sn, uchar* d, qint64 dn, qint64& op){
    qint64 ip = 0;
    while(ip < sn){
        const int token = s[ip++];
        qint64 lit = token >> 4;
        if(lit == 15){ int b; do { if(ip >= sn) return false; b = s[ip++]; lit += b; } while(b == 255); }
        if(ip + lit > sn || op + lit > dn) return false;
        std::memcpy(d + op, s + ip, size_t(lit)); ip += lit; op += lit;
        if(ip >= sn) return true;                 // the last sequence is literals only
        if(ip + 2 > sn) return false;
        const int offset = s[ip] | s[ip + 1] << 8; ip += 2;
        qint64 match = token & 15;
        if(match == 15){ int b; do { if(ip >= sn) return false; b = s[ip++]; match += b; } while(b == 255); }
        match += 4;
        if(offset == 0 || offset > op || op + match > dn) return false;
        for(qint64 i = 0; i < match; ++i, ++op) d[op] = d[op - offset];    // may overlap on purpose
    }
    return true;
}
// Apple's framing: "bv41" (encoded), "bv4-" (stored), ended by "bv4$".
inline bool appleLz4(const QByteArray& in, QByteArray& out){
    const uchar* s = reinterpret_cast<const uchar*>(in.constData()); const qint64 n = in.size();
    uchar* d = reinterpret_cast<uchar*>(out.data()); const qint64 dn = out.size();
    qint64 o = 0, op = 0;
    while(o + 4 <= n){
        if(std::memcmp(s + o, "bv4$", 4) == 0) return op == dn;
        if(o + 12 > n) return false;
        const quint32 dec = qFromLittleEndian<quint32>(s + o + 4), enc = qFromLittleEndian<quint32>(s + o + 8);
        if(std::memcmp(s + o, "bv41", 4) == 0){
            if(o + 12 + enc > n || op + dec > dn) return false;
            qint64 p = op;
            if(!lz4Block(s + o + 12, enc, d, op + dec, p)) return false;
            op += dec; o += 12 + qint64(enc);
        } else if(std::memcmp(s + o, "bv4-", 4) == 0){
            // A stored block has the size once, so the data starts one field earlier.
            if(o + 8 + dec > n || op + dec > dn) return false;
            std::memcpy(d + op, s + o + 8, dec); op += dec; o += 8 + qint64(dec);
        } else return false;
    }
    return op == dn;
}

} // namespace xara
