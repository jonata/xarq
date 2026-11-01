// qr.cpp — QR generation via a dlopen wrapper over libqrencode (guaranteed-correct, scannable output).
// Mirrors the icc/raw/hyphen pattern: the ABI-stable C symbols are declared locally so we don't need the
// dev headers, and everything degrades to an empty image if the runtime library isn't present.
#include "qr.h"
#include <QPainter>
#include "dlcompat.h"

namespace xara {
namespace {
struct QRcode { int version; int width; unsigned char* data; };   // libqrencode's public struct
typedef QRcode* (*EncFn)(const char*, int, int, int, int);        // QRcode_encodeString(str,ver,ecl,hint,cs)
typedef void    (*FreeFn)(QRcode*);                               // QRcode_free

struct Lib {
    void* h=nullptr; EncFn enc=nullptr; FreeFn free=nullptr;
    Lib(){
        h=dlopen("libqrencode.so.4", RTLD_NOW|RTLD_LOCAL);
        if(!h) h=dlopen("libqrencode.so.3", RTLD_NOW|RTLD_LOCAL);
        if(!h) h=dlopen("libqrencode.so",   RTLD_NOW|RTLD_LOCAL);
        if(h){ enc=(EncFn)dlsym(h,"QRcode_encodeString"); free=(FreeFn)dlsym(h,"QRcode_free"); }
    }
    bool ok() const { return enc && free; }
};
Lib& lib(){ static Lib l; return l; }
}

bool qrAvailable(){ return lib().ok(); }

QImage makeQrCode(const QString& text, int scale){
    if(!lib().ok() || text.isEmpty()) return {};
    const QByteArray utf8 = text.toUtf8();
    // version 0 = auto-size; QR_ECLEVEL_M = 1; QR_MODE_8 (byte) = 2; case-sensitive = 1.
    QRcode* qr = lib().enc(utf8.constData(), 0, 1, 2, 1);
    if(!qr || qr->width<=0 || !qr->data){ if(qr) lib().free(qr); return {}; }
    const int w=qr->width, quiet=4; scale=qMax(1,scale);
    const int size=(w+2*quiet)*scale;
    QImage img(size,size,QImage::Format_ARGB32); img.fill(Qt::white);
    { QPainter g(&img); g.setPen(Qt::NoPen);
      for(int y=0;y<w;++y){ const unsigned char* row=qr->data+y*w;
        for(int x=0;x<w;++x) if(row[x]&1) g.fillRect((x+quiet)*scale,(y+quiet)*scale,scale,scale,Qt::black); } }
    lib().free(qr);
    return img;
}
}
