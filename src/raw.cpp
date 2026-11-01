// raw.cpp — see raw.h. A minimal dlopen wrapper over LibRaw's C API. We declare the (ABI-stable)
// function signatures + the libraw_processed_image_t layout ourselves, so no LibRaw dev headers are
// needed. Fully fail-safe: any missing symbol / decode error yields a null QImage and the caller
// falls back (or reports "couldn't load").
#include "raw.h"
#include <QFileInfo>
#include "dlcompat.h"
#include <cstring>

namespace xara {

// LibRaw processed-image struct (stable layout): int type; ushort h,w,colors,bits; uint data_size; data[].
struct RawImg { int type; unsigned short height, width, colors, bits; unsigned int data_size; unsigned char data[1]; };

namespace {
struct Loader {
    void* lib = nullptr; bool ok = false;
    void* (*init)(unsigned int) = nullptr;
    int   (*open_file)(void*, const char*) = nullptr;
    int   (*unpack)(void*) = nullptr;
    int   (*process)(void*) = nullptr;
    RawImg* (*make_mem)(void*, int*) = nullptr;
    void  (*clear_mem)(RawImg*) = nullptr;
    void  (*close)(void*) = nullptr;
    Loader(){
        lib = dlopen("libraw.so.23", RTLD_NOW|RTLD_LOCAL);
        if(!lib) lib = dlopen("libraw.so", RTLD_NOW|RTLD_LOCAL);
        if(!lib) lib = dlopen("libraw_r.so.23", RTLD_NOW|RTLD_LOCAL);
        if(!lib) return;
        init      = (void*(*)(unsigned int))     dlsym(lib, "libraw_init");
        open_file = (int(*)(void*,const char*))  dlsym(lib, "libraw_open_file");
        unpack    = (int(*)(void*))              dlsym(lib, "libraw_unpack");
        process   = (int(*)(void*))              dlsym(lib, "libraw_dcraw_process");
        make_mem  = (RawImg*(*)(void*,int*))     dlsym(lib, "libraw_dcraw_make_mem_image");
        clear_mem = (void(*)(RawImg*))           dlsym(lib, "libraw_dcraw_clear_mem");
        close     = (void(*)(void*))             dlsym(lib, "libraw_close");
        ok = init && open_file && unpack && process && make_mem && clear_mem && close;
    }
};
static Loader& loader(){ static Loader l; return l; }
} // namespace

bool Raw::available(){ return loader().ok; }

QStringList Raw::extensions(){
    return { "cr2","cr3","crw","nef","nrw","arw","srf","sr2","dng","orf","rw2","raf",
             "pef","srw","kdc","dcr","mrw","x3f","3fr","mef","mos","iiq","rwl","raw","erf",
             // Further formats LibRaw's decoder already handles by content — just route the extensions.
             "fff","ptx","cap","bay","dcs","drf","k25","kc2","ori","pxn","mdc","arq","qtk" };
}
bool Raw::isRawFile(const QString& path){
    return extensions().contains(QFileInfo(path).suffix().toLower());
}

QImage Raw::decode(const QString& path){
    Loader& L = loader();
    if(!L.ok) return QImage();
    void* h = L.init(0);
    if(!h) return QImage();
    QImage out;
    if(L.open_file(h, path.toLocal8Bit().constData())==0 && L.unpack(h)==0){
        L.process(h);                                  // demosaic + white-balance + sRGB (LibRaw defaults)
        int err=0; RawImg* pi = L.make_mem(h, &err);
        if(pi){
            const int W=pi->width, Hh=pi->height;
            if(pi->type==2 /*BITMAP*/ && pi->colors==3 && W>0 && Hh>0 &&
               (quint64)pi->data_size >= (quint64)W*Hh*3*(pi->bits/8)){
                QImage im(W, Hh, QImage::Format_RGB888);
                if(pi->bits==8){
                    for(int y=0;y<Hh;++y) std::memcpy(im.scanLine(y), pi->data + (size_t)y*W*3, (size_t)W*3);
                    out = im.convertToFormat(QImage::Format_ARGB32);
                } else if(pi->bits==16){
                    const unsigned short* s0=(const unsigned short*)pi->data;
                    for(int y=0;y<Hh;++y){ unsigned char* d=im.scanLine(y); const unsigned short* s=s0+(size_t)y*W*3;
                        for(int i=0;i<W*3;++i) d[i]=(unsigned char)(s[i]>>8); }
                    out = im.convertToFormat(QImage::Format_ARGB32);
                }
            } else if(pi->type==1 /*embedded JPEG*/){
                out.loadFromData(pi->data, pi->data_size);
            }
            L.clear_mem(pi);
        }
    }
    L.close(h);
    return out;
}

} // namespace xara
