// exr.cpp — OpenEXR (.exr) decode via the vendored single-header tinyexr (MIT), reusing XarQ's
// already-linked zlib (TINYEXR_USE_MINIZ 0). First-party policy: we bundle an existing library's
// source, never shell out. The HDR float RGBA is tonemapped to 8-bit sRGB with the same Reinhard +
// gamma curve as the Radiance-HDR decoder, so EXR previews match HDR previews.
//
// Qt/std headers are included FIRST; tinyexr is included LAST because it defines short macros
// (e.g. getChar) that would otherwise clobber Qt method names like QIODevice::getChar.
#include <QImage>
#include <QFile>
#include <QByteArray>
#include <cmath>
#include <cstdlib>

#define TINYEXR_IMPLEMENTATION
#define TINYEXR_USE_MINIZ 0
#include <zlib.h>
#include "third_party/tinyexr.h"
#undef getChar

namespace xara {

QImage exrDecode(const QString& path){
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return {};
    const QByteArray data=f.read(512LL*1024*1024);
    if(data.size()<16) return {};
    float* rgba=nullptr; int W=0,H=0; const char* err=nullptr;
    const int rc=LoadEXRFromMemory(&rgba,&W,&H,
        reinterpret_cast<const unsigned char*>(data.constData()), size_t(data.size()), &err);
    if(rc!=TINYEXR_SUCCESS || !rgba){ if(err) FreeEXRErrorMessage(err); return {}; }
    QImage img;
    if((long long)W>0 && (long long)H>0 && (long long)W*H<=400LL*1000*1000){
        img=QImage(W,H,QImage::Format_ARGB32);
        if(!img.isNull()){
            auto enc=[](float v){ if(!(v>0.0f)) v=0.0f; v=v/(v+1.0f); v=std::pow(v,1.0f/2.2f);
                                  int q=int(v*255.0f+0.5f); return q<0?0:(q>255?255:q); };
            for(int y=0;y<H;y++){ QRgb* o=reinterpret_cast<QRgb*>(img.scanLine(y));
                const float* s=rgba+size_t(y)*W*4;
                for(int x=0;x<W;x++){ const float* p=s+size_t(x)*4;
                    float av=p[3]; av=av<0.0f?0.0f:(av>1.0f?1.0f:av);
                    o[x]=qRgba(enc(p[0]),enc(p[1]),enc(p[2]), int(av*255.0f+0.5f)); } }
        }
    }
    free(rgba);
    return img;
}

} // namespace xara
