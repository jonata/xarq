// exif.h — minimal EXIF metadata reader for imported JPEG photos.
// Parses the APP1/TIFF IFD0 + Exif sub-IFD and returns friendly key→value strings
// (Camera, Date Taken, Exposure, Aperture, ISO, Focal Length, …). Returns an empty
// map for files without EXIF. Self-contained — no external image library needed.
#pragma once
#include <QString>
#include <QMap>

namespace xara {
QMap<QString,QString> readExif(const QString& jpegPath);
QMap<QString,QString> readExifFromData(const QByteArray& jpeg);
}
