// wmf.cpp — WMF → SVG via libwmf's SVG device layer. See wmf.h. libwmf headers/symbols confined here.
#include "wmf.h"

#ifdef HAVE_WMF
#include <cstdio>
#include <cstdlib>
#include <cmath>
extern "C" {
#include <libwmf/api.h>
#include <libwmf/svg.h>
}

namespace xara {
bool wmfAvailable(){ return true; }

QByteArray wmfToSvg(const QByteArray& data){
    if(data.isEmpty()) return {};

    // libwmf's SVG device writes to a FILE*; open_memstream gives us a memory-backed FILE we can read back,
    // so nothing touches the filesystem.
    char* svgbuf = nullptr; size_t svgsize = 0;
    FILE* memf = open_memstream(&svgbuf, &svgsize);
    if(!memf){ std::free(svgbuf); return {}; }

    wmfAPI* API = nullptr;
    wmfAPI_Options opts; opts.function = wmf_svg_function;                 // route playback through the SVG device
    const unsigned long flags = WMF_OPT_FUNCTION | WMF_OPT_IGNORE_NONFATAL;

    auto bail = [&](wmfAPI* a, bool memOpen)->QByteArray{
        if(a){ if(memOpen) wmf_mem_close(a); wmf_api_destroy(a); }
        fclose(memf); std::free(svgbuf); return {};
    };

    if(wmf_api_create(&API, flags, &opts) != wmf_E_None) return bail(API, false);
    if(wmf_mem_open(API, reinterpret_cast<unsigned char*>(const_cast<char*>(data.constData())),
                    (long)data.size()) != wmf_E_None) return bail(API, false);
    wmfD_Rect bbox;
    if(wmf_scan(API, 0, &bbox) != wmf_E_None) return bail(API, true);

    unsigned int dw = 0, dh = 0;
    if(wmf_display_size(API, &dw, &dh, 96.0, 96.0) != wmf_E_None || dw==0 || dh==0){
        dw = (unsigned int)std::ceil((double)(bbox.BR.x - bbox.TL.x));     // fall back to the bounding box
        dh = (unsigned int)std::ceil((double)(bbox.BR.y - bbox.TL.y));
    }
    if(dw==0 || dh==0 || (long long)dw*dh > 400000000LL) return bail(API, true);

    wmf_svg_t* dd = WMF_SVG_GetData(API);
    dd->out    = wmf_stream_create(API, memf);
    dd->bbox   = bbox;
    dd->width  = dw;
    dd->height = dh;

    const wmf_error_t perr = wmf_play(API, 0, &bbox);
    wmf_mem_close(API);
    wmf_api_destroy(API);
    fclose(memf);                                                          // flush the memstream → svgbuf/svgsize

    QByteArray out;
    if(perr == wmf_E_None && svgbuf && svgsize > 0) out = QByteArray(svgbuf, (int)svgsize);
    std::free(svgbuf);
    return out;
}
}   // namespace xara

#else   // no libwmf at build time

namespace xara {
bool       wmfAvailable(){ return false; }
QByteArray wmfToSvg(const QByteArray&){ return {}; }
}

#endif
