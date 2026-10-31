// wmf.h — Windows Metafile (.wmf) vector import via libwmf. libwmf plays the metafile through its SVG
// device layer; we capture that SVG in memory and hand it to XarQ's own SVG importer, so a .wmf comes in
// as editable vectors. Linked, built only when libwmf is found (HAVE_WMF); otherwise wmfToSvg returns an
// empty QByteArray and wmfAvailable() is false.
#pragma once
#include <QByteArray>

namespace xara {
// Convert WMF bytes to a standard SVG document (empty on failure / when libwmf is absent).
QByteArray wmfToSvg(const QByteArray& data);
// True only when compiled against libwmf.
bool       wmfAvailable();
}
