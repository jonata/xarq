// cdrvector.cpp — CorelDRAW vector import via libcdr (Document Liberation Project) + librevenge.
// libcdr parses the proprietary .cdr/.cmx structure; librevenge's SVG drawing generator turns the
// resulting drawing calls into standard SVG, which we hand to XarQ's own SVG importer. This keeps all
// libcdr/librevenge headers confined to this single translation unit.
#include "cdrvector.h"
#include <zlib.h>

namespace xara {
// A .ccx / .cpx is a CMX whose body is deflated: RIFF "CDRX" holding a "pack" chunk of u32 size,
// "CPng", u16 1, u16 4, then a zlib stream of the chunks that would have followed. libcdr only reads the
// plain form, so rebuild it. Anything else comes back untouched.
QByteArray unpackCorelExchange(const QByteArray& d) {
    const int n = d.size();
    if (n < 12 || (qstrncmp(d.constData(), "RIFF", 4) != 0 && qstrncmp(d.constData(), "RIFX", 4) != 0)
        || qstrncmp(d.constData() + 8, "CDRX", 4) != 0) return d;
    const bool big = d[3] == 'X';
    const uchar* p = reinterpret_cast<const uchar*>(d.constData());
    auto u32 = [&](int o) -> quint32 {
        return big ? (quint32(p[o]) << 24 | p[o+1] << 16 | p[o+2] << 8 | p[o+3])
                   : (quint32(p[o+3]) << 24 | p[o+2] << 16 | p[o+1] << 8 | p[o]); };
    QByteArray body;
    qint64 pos = 12;
    while (pos + 8 <= n) {
        const quint32 len = u32(int(pos) + 4);
        if (len > quint32(n) || pos + 8 + qint64(len) > n) break;
        if (qstrncmp(d.constData() + pos, "pack", 4) == 0) {
            if (len < 13 || qstrncmp(d.constData() + pos + 12, "CPng", 4) != 0) return QByteArray();
            const quint32 want = u32(int(pos) + 8);
            if (want == 0 || want > (1u << 30)) return QByteArray();
            QByteArray out(int(want), Qt::Uninitialized);
            uLongf got = want;
            if (uncompress(reinterpret_cast<Bytef*>(out.data()), &got,
                           reinterpret_cast<const Bytef*>(d.constData() + pos + 20), len - 12) != Z_OK) return QByteArray();
            out.resize(int(got));
            body += out;
        } else {
            body.append(d.constData() + pos, int(8 + len));
            if (len & 1) body.append('\0');
        }
        pos += 8 + qint64(len) + (len & 1);
    }
    QByteArray r = d.left(4);
    const quint32 sz = quint32(body.size()) + 4;
    const char szb[4] = { char(big ? sz >> 24 : sz), char(big ? sz >> 16 : sz >> 8), char(big ? sz >> 8 : sz >> 16), char(big ? sz : sz >> 24) };
    r.append(szb, 4); r.append("CMX1", 4); r += body;
    return r;
}
}   // namespace xara

#ifdef HAVE_LIBCDR
#include <librevenge/librevenge.h>
#include <librevenge-stream/librevenge-stream.h>
#include <librevenge-generators/librevenge-generators.h>
#include <libcdr/libcdr.h>
#include <functional>

namespace xara {
bool cdrVectorAvailable() { return true; }

// Cheap defence-in-depth before handing bytes to libcdr: a crafted .cdr with deeply-nested RIFF/LIST
// containers can stack-overflow libcdr's own parser. Real CorelDRAW files nest only a few levels, so
// reject anything pathologically deep (our walk is itself depth-capped, so it can't overflow either).
static bool riffDepthOk(const QByteArray& d) {
    const int n = d.size();
    if (n < 12 || qstrncmp(d.constData(), "RIFF", 4) != 0) return true;   // not RIFF-shaped → let libcdr decide
    const uchar* p = reinterpret_cast<const uchar*>(d.constData());
    auto u32 = [&](int o)->quint32 { return (o>=0 && o+3<n) ? (quint32)(p[o]|(p[o+1]<<8)|(p[o+2]<<16)|((quint32)p[o+3]<<24)) : 0u; };
    std::function<bool(int,int,int)> walk = [&](int off,int end,int depth)->bool {
        if (depth > 100) return false;                                    // deeper than any real CDR → an attack
        while (off+8 <= end) {
            const char* cid = d.constData()+off; const quint32 size = u32(off+4); const int body = off+8;
            if (body+(qint64)size > end) break;
            if ((qstrncmp(cid,"RIFF",4)==0 || qstrncmp(cid,"LIST",4)==0) && !walk(body+4, body+(int)size, depth+1)) return false;
            off = body + (int)size + ((int)size & 1);
        }
        return true;
    };
    return walk(12, qMin(n, 8+(int)u32(4)), 0);
}

QStringList cdrToSvgPages(const QByteArray& data) {
    QStringList pages;
    if (data.isEmpty() || !riffDepthOk(data)) return pages;
    librevenge::RVNGStringStream input(reinterpret_cast<const unsigned char*>(data.constData()),
                                       static_cast<unsigned>(data.size()));
    if (!libcdr::CDRDocument::isSupported(&input)) return pages;
    librevenge::RVNGStringVector out;
    librevenge::RVNGSVGDrawingGenerator generator(out, "");   // empty namespace ⇒ plain <svg>, as the SVG importer expects
    if (!libcdr::CDRDocument::parse(&input, &generator)) return pages;
    for (unsigned i = 0; i < out.size(); ++i)
        if (!out[i].empty()) pages << QString::fromUtf8(out[i].cstr());
    return pages;
}

// Corel Presentation Exchange (.cmx) — same libcdr, different document class. CMX is also a RIFF
// container, so the same nesting-depth guard applies before we hand bytes to the parser.
QStringList cmxToSvgPages(const QByteArray& packed) {
    QStringList pages;
    const QByteArray data = unpackCorelExchange(packed);
    if (data.isEmpty() || !riffDepthOk(data)) return pages;
    librevenge::RVNGStringStream input(reinterpret_cast<const unsigned char*>(data.constData()),
                                       static_cast<unsigned>(data.size()));
    if (!libcdr::CMXDocument::isSupported(&input)) return pages;
    librevenge::RVNGStringVector out;
    librevenge::RVNGSVGDrawingGenerator generator(out, "");
    if (!libcdr::CMXDocument::parse(&input, &generator)) return pages;
    for (unsigned i = 0; i < out.size(); ++i)
        if (!out[i].empty()) pages << QString::fromUtf8(out[i].cstr());
    return pages;
}
}   // namespace xara

#else   // no libcdr at build time

namespace xara {
bool cdrVectorAvailable() { return false; }
QStringList cdrToSvgPages(const QByteArray&) { return {}; }
QStringList cmxToSvgPages(const QByteArray&) { return {}; }
}

#endif
