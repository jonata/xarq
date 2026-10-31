// extvector.h — optional vector import for the Document Liberation Project sibling formats:
// WordPerfect Graphics (.wpg), Microsoft Visio (.vsd/.vsdx/.vdx…), Adobe/Macromedia FreeHand (.fh*),
// and Microsoft Publisher (.pub). Each is parsed by its own DLP library (libwpg/libvisio/libfreehand/
// libmspub) and rendered to standard SVG via librevenge's SVG drawing generator, which XarQ's own SVG
// importer then turns into editable shapes — exactly the bridge cdrvector.cpp uses for CorelDRAW.
// Built only when the matching -dev package is present at configure time (HAVE_LIBWPG/…); otherwise the
// stubs report unavailable and the importer reports the format as unsupported.
#pragma once
#include <QByteArray>
#include <QStringList>

namespace xara {
// Each *Available() is true when compiled with that library; each *ToSvgPages() returns one standard
// SVG string per page, or an empty list if the library is absent, the data isn't that format, or the
// parse fails.
bool wpgVectorAvailable();       QStringList wpgToSvgPages(const QByteArray& data);
bool visioVectorAvailable();     QStringList visioToSvgPages(const QByteArray& data);
bool freehandVectorAvailable();  QStringList freehandToSvgPages(const QByteArray& data);
bool mspubVectorAvailable();     QStringList mspubToSvgPages(const QByteArray& data);
// More DLP siblings (drawing + presentation via the same bridge).
bool pagemakerVectorAvailable(); QStringList pagemakerToSvgPages(const QByteArray& data);  // Adobe/Aldus PageMaker
bool zmfVectorAvailable();       QStringList zmfToSvgPages(const QByteArray& data);         // Zoner Draw / Callisto
bool qxpVectorAvailable();       QStringList qxpToSvgPages(const QByteArray& data);         // QuarkXPress
bool starofficeVectorAvailable();QStringList starofficeToSvgPages(const QByteArray& data);  // StarOffice StarDraw/StarImpress
bool iworkVectorAvailable();     QStringList iworkToSvgPages(const QByteArray& data);       // Apple iWork Keynote (.key) → one SVG per slide
int  iworkDocumentKind(const QByteArray& data);                                            // 0=not iWork, 1=Keynote, 2=Pages, 3=Numbers
QString iworkToText(const QByteArray& data, QString* kindOut = nullptr);                    // Pages/Numbers → plain body text
bool mwawVectorAvailable();      QStringList mwawToSvgPages(const QByteArray& data);        // legacy Mac (MacDraw/ClarisDraw)
}
