// cdrvector.h — optional CorelDRAW (.cdr / .cmx) vector import via libcdr + librevenge.
// Built only when libcdr is found at configure time (HAVE_LIBCDR); otherwise the stubs report
// unavailable and importCdr falls back to extracting the embedded preview bitmap.
#pragma once
#include <QByteArray>
#include <QStringList>

namespace xara {
// True when compiled with libcdr support.
bool cdrVectorAvailable();
// Parse a CorelDRAW document (raw .cdr/.cmx bytes) into one standard SVG string per page.
// Returns an empty list if libcdr is unavailable, the data isn't a supported CDR, or parsing fails.
QStringList cdrToSvgPages(const QByteArray& data);
// Parse a Corel Presentation Exchange document (raw .cmx bytes) via libcdr's CMXDocument.
QStringList cmxToSvgPages(const QByteArray& data);
}
