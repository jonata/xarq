// extvector.cpp — WordPerfect Graphics / Visio / FreeHand / Publisher vector import via the Document
// Liberation Project libraries (libwpg / libvisio / libfreehand / libmspub) + librevenge's SVG drawing
// generator. Each library parses its proprietary structure; librevenge turns the resulting drawing
// calls into standard SVG, which we hand to XarQ's own SVG importer. All library headers are confined
// to this single translation unit; each format is independently optional (its -dev package need not be
// installed for the others to build).
#include "extvector.h"

#if defined(HAVE_LIBWPG) || defined(HAVE_LIBVISIO) || defined(HAVE_LIBFREEHAND) || defined(HAVE_LIBMSPUB) || \
    defined(HAVE_LIBPAGEMAKER) || defined(HAVE_LIBZMF) || defined(HAVE_LIBQXP) || \
    defined(HAVE_LIBSTAROFFICE) || defined(HAVE_LIBETONYEK) || defined(HAVE_LIBMWAW)
#include <librevenge/librevenge.h>
#include <librevenge-stream/librevenge-stream.h>
#include <librevenge-generators/librevenge-generators.h>
#include <functional>

namespace {
// Shared librevenge → SVG driver: feed raw bytes through a DLP document parser and collect one standard
// SVG string per page. isSup/parse are the library's static isSupported/parse entry points. Empty list
// on any failure (not that format, or parse error).
[[maybe_unused]] QStringList runGenerator(const QByteArray& data,
        const std::function<bool(librevenge::RVNGInputStream*)>& isSup,
        const std::function<bool(librevenge::RVNGInputStream*, librevenge::RVNGDrawingInterface*)>& parse){
    QStringList pages;
    if(data.isEmpty()) return pages;
    librevenge::RVNGStringStream input(reinterpret_cast<const unsigned char*>(data.constData()),
                                       static_cast<unsigned>(data.size()));
    if(!isSup(&input)) return pages;
    librevenge::RVNGStringVector out;
    librevenge::RVNGSVGDrawingGenerator generator(out, "");   // empty namespace ⇒ plain <svg>, as our SVG importer expects
    if(!parse(&input, &generator)) return pages;
    for(unsigned i=0; i<out.size(); ++i)
        if(!out[i].empty()) pages << QString::fromUtf8(out[i].cstr());
    return pages;
}
// Presentation variant: some DLP formats (Keynote, StarImpress) are slide documents, which librevenge
// renders via RVNGSVGPresentationGenerator (one SVG per slide) rather than the drawing generator.
[[maybe_unused]] QStringList runPresentationGenerator(const QByteArray& data,
        const std::function<bool(librevenge::RVNGInputStream*)>& isSup,
        const std::function<bool(librevenge::RVNGInputStream*, librevenge::RVNGPresentationInterface*)>& parse){
    QStringList pages;
    if(data.isEmpty()) return pages;
    librevenge::RVNGStringStream input(reinterpret_cast<const unsigned char*>(data.constData()),
                                       static_cast<unsigned>(data.size()));
    if(!isSup(&input)) return pages;
    librevenge::RVNGStringVector out;
    librevenge::RVNGSVGPresentationGenerator generator(out);
    if(!parse(&input, &generator)) return pages;
    for(unsigned i=0; i<out.size(); ++i)
        if(!out[i].empty()) pages << QString::fromUtf8(out[i].cstr());
    return pages;
}
// Text variants: Pages is a word processor and Numbers a spreadsheet — librevenge renders those
// through RVNGTextTextGenerator / RVNGTextSpreadsheetGenerator (plain text), which is what XarQ can
// place as an editable text frame.
[[maybe_unused]] QString runTextGenerator(const QByteArray& d,
        const std::function<bool(librevenge::RVNGInputStream*)>& isSup,
        const std::function<bool(librevenge::RVNGInputStream*, librevenge::RVNGTextInterface*)>& parse){
    if(d.isEmpty()) return {};
    librevenge::RVNGStringStream input(reinterpret_cast<const unsigned char*>(d.constData()),
                                       static_cast<unsigned>(d.size()));
    if(!isSup(&input)) return {};
    librevenge::RVNGString out;
    librevenge::RVNGTextTextGenerator gen(out, false);   // false = body text, NOT the metadata dump
    if(!parse(&input, &gen)) return {};
    return QString::fromUtf8(out.cstr());
}
[[maybe_unused]] QString runSpreadsheetTextGenerator(const QByteArray& d,
        const std::function<bool(librevenge::RVNGInputStream*)>& isSup,
        const std::function<bool(librevenge::RVNGInputStream*, librevenge::RVNGSpreadsheetInterface*)>& parse){
    if(d.isEmpty()) return {};
    librevenge::RVNGStringStream input(reinterpret_cast<const unsigned char*>(d.constData()),
                                       static_cast<unsigned>(d.size()));
    if(!isSup(&input)) return {};
    librevenge::RVNGStringVector sheets;                 // one plain-text block per sheet
    librevenge::RVNGTextSpreadsheetGenerator gen(sheets, false);
    if(!parse(&input, &gen)) return {};
    QStringList out;
    for(unsigned i=0;i<sheets.size();++i) if(!sheets[i].empty()) out << QString::fromUtf8(sheets[i].cstr());
    return out.join(QStringLiteral("\n\n"));
}
}   // anonymous namespace
#endif

// ---- WordPerfect Graphics (.wpg) ----------------------------------------------------------------
#ifdef HAVE_LIBWPG
#include <libwpg/libwpg.h>
namespace xara {
bool wpgVectorAvailable(){ return true; }
QStringList wpgToSvgPages(const QByteArray& d){
    return runGenerator(d,
        [](librevenge::RVNGInputStream* i){ return libwpg::WPGraphics::isSupported(i); },
        [](librevenge::RVNGInputStream* i, librevenge::RVNGDrawingInterface* g){ return libwpg::WPGraphics::parse(i, g); });
}
}
#else
namespace xara { bool wpgVectorAvailable(){ return false; } QStringList wpgToSvgPages(const QByteArray&){ return {}; } }
#endif

// ---- Microsoft Visio (.vsd / .vsdx / .vdx / .vss / .vst) -----------------------------------------
#ifdef HAVE_LIBVISIO
#include <libvisio/libvisio.h>
namespace xara {
bool visioVectorAvailable(){ return true; }
QStringList visioToSvgPages(const QByteArray& d){
    return runGenerator(d,
        [](librevenge::RVNGInputStream* i){ return libvisio::VisioDocument::isSupported(i); },
        [](librevenge::RVNGInputStream* i, librevenge::RVNGDrawingInterface* g){ return libvisio::VisioDocument::parse(i, g); });
}
}
#else
namespace xara { bool visioVectorAvailable(){ return false; } QStringList visioToSvgPages(const QByteArray&){ return {}; } }
#endif

// ---- Adobe / Macromedia FreeHand (.fh / .fh7 … .fh11 / .ft*) -------------------------------------
#ifdef HAVE_LIBFREEHAND
#include <libfreehand/libfreehand.h>
namespace xara {
bool freehandVectorAvailable(){ return true; }
QStringList freehandToSvgPages(const QByteArray& d){
    return runGenerator(d,
        [](librevenge::RVNGInputStream* i){ return libfreehand::FreeHandDocument::isSupported(i); },
        [](librevenge::RVNGInputStream* i, librevenge::RVNGDrawingInterface* g){ return libfreehand::FreeHandDocument::parse(i, g); });
}
}
#else
namespace xara { bool freehandVectorAvailable(){ return false; } QStringList freehandToSvgPages(const QByteArray&){ return {}; } }
#endif

// ---- Microsoft Publisher (.pub) -----------------------------------------------------------------
#ifdef HAVE_LIBMSPUB
#include <libmspub/libmspub.h>
namespace xara {
bool mspubVectorAvailable(){ return true; }
QStringList mspubToSvgPages(const QByteArray& d){
    return runGenerator(d,
        [](librevenge::RVNGInputStream* i){ return libmspub::MSPUBDocument::isSupported(i); },
        [](librevenge::RVNGInputStream* i, librevenge::RVNGDrawingInterface* g){ return libmspub::MSPUBDocument::parse(i, g); });
}
}
#else
namespace xara { bool mspubVectorAvailable(){ return false; } QStringList mspubToSvgPages(const QByteArray&){ return {}; } }
#endif

// ---- Adobe/Aldus PageMaker (.pmd/.p65) — drawing ----------------------------------------------------
#ifdef HAVE_LIBPAGEMAKER
#include <libpagemaker/libpagemaker.h>
namespace xara {
bool pagemakerVectorAvailable(){ return true; }
QStringList pagemakerToSvgPages(const QByteArray& d){
    return runGenerator(d,
        [](librevenge::RVNGInputStream* i){ return libpagemaker::PMDocument::isSupported(i); },
        [](librevenge::RVNGInputStream* i, librevenge::RVNGDrawingInterface* g){ return libpagemaker::PMDocument::parse(i, g); });
}
}
#else
namespace xara { bool pagemakerVectorAvailable(){ return false; } QStringList pagemakerToSvgPages(const QByteArray&){ return {}; } }
#endif


// ---- Zoner Draw / Callisto (.zmf) — drawing (KIND_DRAW only) ----------------------------------------------------
#ifdef HAVE_LIBZMF
#include <libzmf/libzmf.h>
namespace xara {
bool zmfVectorAvailable(){ return true; }
QStringList zmfToSvgPages(const QByteArray& d){
    return runGenerator(d,
        [](librevenge::RVNGInputStream* i){
            libzmf::ZMFDocument::Type type = libzmf::ZMFDocument::TYPE_UNKNOWN;
            libzmf::ZMFDocument::Kind kind = libzmf::ZMFDocument::KIND_UNKNOWN;
            if (!libzmf::ZMFDocument::isSupported(i, &type, &kind))
                return false;
            // Only accept vector-drawing documents; decline raster/paint (Zoner Bitmap) kinds.
            return kind == libzmf::ZMFDocument::KIND_DRAW;
        },
        [](librevenge::RVNGInputStream* i, librevenge::RVNGDrawingInterface* g){ return libzmf::ZMFDocument::parse(i, g); });
}
}
#else
namespace xara { bool zmfVectorAvailable(){ return false; } QStringList zmfToSvgPages(const QByteArray&){ return {}; } }
#endif


// ---- QuarkXPress (.qxp/.qxd) — page-layout drawing ----------------------------------------------------
#ifdef HAVE_LIBQXP
#include <libqxp/libqxp.h>
namespace xara {
bool qxpVectorAvailable(){ return true; }
QStringList qxpToSvgPages(const QByteArray& d){
    return runGenerator(d,
        [](librevenge::RVNGInputStream* i){ return libqxp::QXPDocument::isSupported(i); },
        [](librevenge::RVNGInputStream* i, librevenge::RVNGDrawingInterface* g){ return libqxp::QXPDocument::parse(i, g) == libqxp::QXPDocument::RESULT_OK; });
}
}
#else
namespace xara { bool qxpVectorAvailable(){ return false; } QStringList qxpToSvgPages(const QByteArray&){ return {}; } }
#endif


// ---- StarOffice StarDraw/StarImpress (.sda/.sdd) — drawing + presentation ----------------------------------------------------
#ifdef HAVE_LIBSTAROFFICE
#include <libstaroffice/libstaroffice.hxx>
namespace xara {
bool starofficeVectorAvailable(){ return true; }
QStringList starofficeToSvgPages(const QByteArray& d){
    // Vector DRAW / gallery GRAPHIC kinds -> one SVG per drawing page.
    QStringList pages = runGenerator(d,
        [](librevenge::RVNGInputStream* i){
            STOFFDocument::Kind k = STOFFDocument::STOFF_K_UNKNOWN;
            // Accept SUPPORTED_ENCRYPTION as well as EXCELLENT: real StarOffice files use the format's
            // built-in scrambling and report SUPPORTED_ENCRYPTION though libstaroffice parses them fine
            // (parse() below is the real gate — it declines if a password we don't have is required).
            return STOFFDocument::isFileFormatSupported(i, k) >= STOFFDocument::STOFF_C_SUPPORTED_ENCRYPTION
                && (k == STOFFDocument::STOFF_K_DRAW || k == STOFFDocument::STOFF_K_GRAPHIC);
        },
        [](librevenge::RVNGInputStream* i, librevenge::RVNGDrawingInterface* g){
            return STOFFDocument::parse(i, g, nullptr) == STOFFDocument::STOFF_R_OK;
        });
    if (!pages.isEmpty()) return pages;
    // PRESENTATION kind (StarImpress) -> one SVG per slide.
    return runPresentationGenerator(d,
        [](librevenge::RVNGInputStream* i){
            STOFFDocument::Kind k = STOFFDocument::STOFF_K_UNKNOWN;
            // Accept SUPPORTED_ENCRYPTION as well as EXCELLENT: real StarOffice files use the format's
            // built-in scrambling and report SUPPORTED_ENCRYPTION though libstaroffice parses them fine
            // (parse() below is the real gate — it declines if a password we don't have is required).
            return STOFFDocument::isFileFormatSupported(i, k) >= STOFFDocument::STOFF_C_SUPPORTED_ENCRYPTION
                && k == STOFFDocument::STOFF_K_PRESENTATION;
        },
        [](librevenge::RVNGInputStream* i, librevenge::RVNGPresentationInterface* g){
            return STOFFDocument::parse(i, g, nullptr) == STOFFDocument::STOFF_R_OK;
        });
}
}
#else
namespace xara { bool starofficeVectorAvailable(){ return false; } QStringList starofficeToSvgPages(const QByteArray&){ return {}; } }
#endif


// ---- Apple iWork: Keynote (.key) slides + Pages (.pages) / Numbers (.numbers) documents ----------
#ifdef HAVE_LIBETONYEK
#include <libetonyek/libetonyek.h>
namespace xara {
bool iworkVectorAvailable(){ return true; }
// Classify an iWork package once: 0 = not iWork, 1 = Keynote, 2 = Pages, 3 = Numbers.
// NOTE: only CONFIDENCE_EXCELLENT counts. libetonyek's CONFIDENCE_SUPPORTED_PART means "this is a
// FRAGMENT of a package" (not a whole document) — unlike libstaroffice/libmwaw's SUPPORTED_ENCRYPTION,
// accepting it here would import partial garbage.
int iworkDocumentKind(const QByteArray& d){
    using ED = libetonyek::EtonyekDocument;
    if(d.isEmpty()) return 0;
    librevenge::RVNGStringStream input(reinterpret_cast<const unsigned char*>(d.constData()),
                                       static_cast<unsigned>(d.size()));
    ED::Type t = ED::TYPE_UNKNOWN;
    if(ED::isSupported(&input, &t) != ED::CONFIDENCE_EXCELLENT) return 0;
    switch(t){ case ED::TYPE_KEYNOTE: return 1; case ED::TYPE_PAGES: return 2; case ED::TYPE_NUMBERS: return 3; default: return 0; }
}
QStringList iworkToSvgPages(const QByteArray& d){
    return runPresentationGenerator(d,
        [](librevenge::RVNGInputStream* i){
            libetonyek::EtonyekDocument::Type t = libetonyek::EtonyekDocument::TYPE_UNKNOWN;
            libetonyek::EtonyekDocument::Confidence c = libetonyek::EtonyekDocument::isSupported(i, &t);
            return c == libetonyek::EtonyekDocument::CONFIDENCE_EXCELLENT
                && t == libetonyek::EtonyekDocument::TYPE_KEYNOTE;
        },
        [](librevenge::RVNGInputStream* i, librevenge::RVNGPresentationInterface* g){
            return libetonyek::EtonyekDocument::parse(i, g);
        });
}
// Pages / Numbers → plain body text (Keynote returns empty here; it goes through iworkToSvgPages).
QString iworkToText(const QByteArray& d, QString* kindOut){
    using ED = libetonyek::EtonyekDocument;
    const int kind = iworkDocumentKind(d);
    if(kind==2){
        if(kindOut) *kindOut = QStringLiteral("Pages");
        return runTextGenerator(d,
            [](librevenge::RVNGInputStream* i){ ED::Type t=ED::TYPE_UNKNOWN;
                return ED::isSupported(i,&t)==ED::CONFIDENCE_EXCELLENT && t==ED::TYPE_PAGES; },
            [](librevenge::RVNGInputStream* i, librevenge::RVNGTextInterface* g){ return ED::parse(i,g); });
    }
    if(kind==3){
        if(kindOut) *kindOut = QStringLiteral("Numbers");
        return runSpreadsheetTextGenerator(d,
            [](librevenge::RVNGInputStream* i){ ED::Type t=ED::TYPE_UNKNOWN;
                return ED::isSupported(i,&t)==ED::CONFIDENCE_EXCELLENT && t==ED::TYPE_NUMBERS; },
            [](librevenge::RVNGInputStream* i, librevenge::RVNGSpreadsheetInterface* g){ return ED::parse(i,g); });
    }
    return {};
}
}
#else
namespace xara { bool iworkVectorAvailable(){ return false; } QStringList iworkToSvgPages(const QByteArray&){ return {}; }
                 int iworkDocumentKind(const QByteArray&){ return 0; }
                 QString iworkToText(const QByteArray&, QString*){ return {}; } }
#endif


// ---- legacy Mac MacDraw/ClarisDraw (.drw/.cwk) — drawing + presentation ----------------------------------------------------
#ifdef HAVE_LIBMWAW
#include <libmwaw/libmwaw.hxx>
namespace xara {
bool mwawVectorAvailable(){ return true; }
QStringList mwawToSvgPages(const QByteArray& d){
    // Legacy Mac vector documents (MacDraw/ClarisDraw/…). isFileFormatSupported returns a
    // Confidence enum and fills Type/Kind out-params; accept only the vector kinds. Accept
    // SUPPORTED_ENCRYPTION too (like StarOffice above) — parse() is the real gate.
    // Try the drawing kind first (one SVG per page), fall back to the presentation kind.
    QStringList pages = runGenerator(d,
        [](librevenge::RVNGInputStream* i){
            MWAWDocument::Type type = MWAWDocument::MWAW_T_UNKNOWN;
            MWAWDocument::Kind kind = MWAWDocument::MWAW_K_UNKNOWN;
            return MWAWDocument::isFileFormatSupported(i, type, kind) >= MWAWDocument::MWAW_C_SUPPORTED_ENCRYPTION
                && kind == MWAWDocument::MWAW_K_DRAW;
        },
        [](librevenge::RVNGInputStream* i, librevenge::RVNGDrawingInterface* g){
            return MWAWDocument::parse(i, g) == MWAWDocument::MWAW_R_OK;
        });
    if(!pages.isEmpty()) return pages;
    return runPresentationGenerator(d,
        [](librevenge::RVNGInputStream* i){
            MWAWDocument::Type type = MWAWDocument::MWAW_T_UNKNOWN;
            MWAWDocument::Kind kind = MWAWDocument::MWAW_K_UNKNOWN;
            return MWAWDocument::isFileFormatSupported(i, type, kind) >= MWAWDocument::MWAW_C_SUPPORTED_ENCRYPTION
                && kind == MWAWDocument::MWAW_K_PRESENTATION;
        },
        [](librevenge::RVNGInputStream* i, librevenge::RVNGPresentationInterface* g){
            return MWAWDocument::parse(i, g) == MWAWDocument::MWAW_R_OK;
        });
}
}
#else
namespace xara { bool mwawVectorAvailable(){ return false; } QStringList mwawToSvgPages(const QByteArray&){ return {}; } }
#endif

