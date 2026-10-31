// dicom.h — DICOM (.dcm) medical-image import via GDCM (Grassroots DICOM). GDCM handles the container,
// the transfer-syntax decompression (raw / RLE / JPEG / JPEG-LS / JPEG 2000) and the pixel format; we
// convert the decoded pixels to an 8-bit QImage, auto-windowed by min/max (a sensible default for a
// general importer). Linked, not dlopen'd (GDCM is a C++ API). Built only when GDCM is found (HAVE_GDCM);
// otherwise dicomDecode returns a null QImage and dicomAvailable() is false.
#pragma once
#include <QImage>
#include <QString>

namespace xara {
// Decode the first frame of a DICOM file to an 8-bit QImage (grayscale auto-windowed, or RGB), or null.
QImage dicomDecode(const QString& path);
// True only when compiled against GDCM.
bool   dicomAvailable();
}
