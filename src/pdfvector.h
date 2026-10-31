#pragma once
// PDF import through libpoppler: every page becomes one SVG string that the shared SVG importer
// (Canvas::importVectorSvgPages) turns into editable shapes, like the other vector formats.
#include <QString>
#include <QStringList>

namespace xara {

struct PdfVectorStats {
    int pages = 0, totalPages = 0;
    int softMasks = 0;       // soft masks drawn without their mask
    bool encrypted = false;
};

bool pdfVectorAvailable();
QStringList pdfToSvgPages(const QString& path, PdfVectorStats* stats = nullptr);

}
