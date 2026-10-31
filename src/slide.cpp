// slide.cpp — whole-slide import via OpenSlide. See slide.h. OpenSlide headers/symbols confined here.
#include "slide.h"

#ifdef HAVE_OPENSLIDE
#include <openslide/openslide.h>
#include <vector>
#include <cstdint>

namespace xara {
bool slideAvailable(){ return true; }

QImage slideDecode(const QString& path){
    openslide_t* osr = openslide_open(path.toLocal8Bit().constData());
    if(!osr) return {};
    if(openslide_get_error(osr)){ openslide_close(osr); return {}; }

    int64_t w0 = 0, h0 = 0;
    openslide_get_level0_dimensions(osr, &w0, &h0);
    if(w0 <= 0 || h0 <= 0){ openslide_close(osr); return {}; }

    // Whole-slide images are gigapixel pyramids — pick the pyramid level closest to a page-sized overview
    // (longest side ~4096 px) rather than reading full resolution.
    const int32_t levels = openslide_get_level_count(osr);
    const double MAXDIM = 4096.0;
    const double downsample = qMax(1.0, qMax((double)w0 / MAXDIM, (double)h0 / MAXDIM));
    int32_t level = openslide_get_best_level_for_downsample(osr, downsample);
    if(level < 0 || level >= levels) level = (levels > 0) ? levels - 1 : 0;

    int64_t lw = 0, lh = 0;
    openslide_get_level_dimensions(osr, level, &lw, &lh);
    // Bound each dimension BEFORE the product (dims are attacker-controlled file metadata): a per-side cap of
    // 1e6 keeps lw*lh <= 1e12, so the signed int64 product below can't overflow and skip the ~64 Mpix ceiling.
    if(lw <= 0 || lh <= 0 || lw > 1000000LL || lh > 1000000LL || lw * lh > 64000000LL){ openslide_close(osr); return {}; }

    std::vector<uint32_t> buf((size_t)lw * (size_t)lh);
    // x/y are always in level-0 coordinates; (0,0) is the same point at every level.
    openslide_read_region(osr, buf.data(), 0, 0, level, lw, lh);
    const bool err = (openslide_get_error(osr) != nullptr);
    openslide_close(osr);
    if(err) return {};

    // OpenSlide delivers pre-multiplied ARGB (A in the high byte) — exactly Qt's ARGB32_Premultiplied.
    QImage img(reinterpret_cast<const uchar*>(buf.data()), (int)lw, (int)lh, (int)(lw * 4),
               QImage::Format_ARGB32_Premultiplied);
    return img.convertToFormat(QImage::Format_ARGB32);      // un-premultiply + deep-copy out of buf
}
}   // namespace xara

#else   // no OpenSlide at build time

namespace xara {
bool   slideAvailable(){ return false; }
QImage slideDecode(const QString&){ return {}; }
}

#endif
