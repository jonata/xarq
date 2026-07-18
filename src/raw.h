// raw.h — camera RAW import (Batch R8). Loads libraw.so at RUNTIME via dlopen (no dev headers /
// link dependency), decoding CR2/CR3/NEF/ARW/DNG/ORF/RW2/RAF/… into an sRGB QImage. Degrades
// gracefully (decode() returns a null image) when LibRaw is absent.
#pragma once
#include <QImage>
#include <QString>
#include <QStringList>

namespace xara {

class Raw {
public:
    static bool available();                        // libraw loaded
    static bool isRawFile(const QString& path);     // extension looks like a camera RAW
    static QStringList extensions();                // lower-case RAW extensions (no dot)
    static QImage decode(const QString& path);      // RAW file → sRGB QImage (null on failure)
};

} // namespace xara
