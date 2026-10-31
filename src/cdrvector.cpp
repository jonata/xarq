// cdrvector.cpp — CorelDRAW vector import via libcdr (Document Liberation Project) + librevenge.
// libcdr parses the proprietary .cdr/.cmx structure; librevenge's SVG drawing generator turns the
// resulting drawing calls into standard SVG, which we hand to XarQ's own SVG importer. This keeps all
// libcdr/librevenge headers confined to this single translation unit.
#include "cdrvector.h"

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
QStringList cmxToSvgPages(const QByteArray& data) {
    QStringList pages;
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
