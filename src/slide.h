// slide.h — whole-slide microscopy import via OpenSlide (Aperio .svs, Hamamatsu .ndpi/.vms/.vmu,
// Leica .scn, MIRAX .mrxs, Ventana .bif, Sakura .svslide, generic tiled BigTIFF). Whole-slide images
// are gigapixel pyramids, so we read a page-sized overview level rather than the full resolution.
// Linked, built only when OpenSlide is found (HAVE_OPENSLIDE); otherwise slideDecode returns a null
// QImage and slideAvailable() is false.
#pragma once
#include <QImage>
#include <QString>

namespace xara {
// Decode a bounded overview of a whole-slide image to an ARGB32 QImage, or a null QImage on failure.
QImage slideDecode(const QString& path);
// True only when compiled against OpenSlide.
bool   slideAvailable();
}
