// icc.cpp — see icc.h. A minimal dlopen wrapper over Little-CMS 2 (liblcms2.so.2). We declare the
// (ABI-stable) function signatures and the pixel-format constants ourselves, so no lcms2 dev headers
// are required. Everything is lazy + fail-safe: if the library or a CMYK profile is missing, available()
// is false and callers use the naive prepress transforms instead.
#include "icc.h"
#include <QDir>
#include <QFileInfo>
#include "dlcompat.h"

namespace xara {

// ---- lcms2 ABI (subset) ----------------------------------------------------
typedef void* cmsHPROFILE;
typedef void* cmsHTRANSFORM;
typedef unsigned int cmsU32;

// Pixel-format constants, computed from the lcms2 macros (stable across 2.x):
//   COLORSPACE_SH(s)=s<<16, SWAPFIRST_SH=s<<14, DOSWAP_SH=e<<10, EXTRA_SH=e<<7, CHANNELS_SH=c<<3, BYTES_SH=b
//   PT_GRAY=3, PT_RGB=4, PT_CMYK=6
static const cmsU32 TYPE_RGB_8  = (4u<<16)|(3u<<3)|1u;   // 0x40019 — 3 bytes R,G,B (contiguous)
static const cmsU32 TYPE_CMYK_8 = (6u<<16)|(4u<<3)|1u;   // 0x60021 — 4 bytes C,M,Y,K
static const cmsU32 INTENT_PERCEPTUAL = 0u;

typedef cmsHPROFILE (*fn_OpenFromFile)(const char*, const char*);
typedef cmsHPROFILE (*fn_Create_sRGB)(void);
typedef cmsHTRANSFORM (*fn_CreateTransform)(cmsHPROFILE, cmsU32, cmsHPROFILE, cmsU32, cmsU32, cmsU32);
typedef void (*fn_DoTransform)(cmsHTRANSFORM, const void*, void*, cmsU32);
typedef int  (*fn_CloseProfile)(cmsHPROFILE);
typedef void (*fn_DeleteTransform)(cmsHTRANSFORM);

struct Icc::Impl {
    void* lib = nullptr;
    fn_OpenFromFile    OpenFromFile = nullptr;
    fn_Create_sRGB     Create_sRGB  = nullptr;
    fn_CreateTransform CreateTransform = nullptr;
    fn_DoTransform     DoTransform  = nullptr;
    fn_CloseProfile    CloseProfile = nullptr;
    fn_DeleteTransform DeleteTransform = nullptr;

    cmsHPROFILE srgb = nullptr, cmyk = nullptr;
    cmsHTRANSFORM toCmyk = nullptr, toRgb = nullptr;   // sRGB→CMYK and CMYK→sRGB
    QString cmykPath;
    QStringList profiles;
    cmsU32 intent = 0;      // 0 perceptual, 1 relative-colorimetric, 2 saturation, 3 absolute-colorimetric
    cmsU32 flags = 0;       // 0x2000 = cmsFLAGS_BLACKPOINTCOMPENSATION

    bool loaded() const { return lib && OpenFromFile && Create_sRGB && CreateTransform && DoTransform; }
    void releaseTransforms(){
        if(toCmyk && DeleteTransform){ DeleteTransform(toCmyk); toCmyk=nullptr; }
        if(toRgb  && DeleteTransform){ DeleteTransform(toRgb);  toRgb=nullptr; }
        if(cmyk   && CloseProfile){ CloseProfile(cmyk); cmyk=nullptr; }
    }
    bool buildTransforms(const QString& path){
        if(!loaded() || !srgb) return false;
        // Build into temporaries first — only tear down the CURRENT working transforms once the new
        // profile is fully ready, so a failed switch (corrupt/non-CMYK profile) keeps ICC alive.
        cmsHPROFILE nc = OpenFromFile(path.toLocal8Bit().constData(), "r");
        if(!nc) return false;
        cmsHTRANSFORM tc = CreateTransform(srgb, TYPE_RGB_8, nc, TYPE_CMYK_8, intent, flags);
        cmsHTRANSFORM tr = CreateTransform(nc, TYPE_CMYK_8, srgb, TYPE_RGB_8, intent, flags);
        if(!tc || !tr){
            if(tc && DeleteTransform) DeleteTransform(tc);
            if(tr && DeleteTransform) DeleteTransform(tr);
            if(CloseProfile) CloseProfile(nc);
            return false;
        }
        releaseTransforms();                       // commit: drop the old, install the new
        cmyk=nc; toCmyk=tc; toRgb=tr; cmykPath=path; return true;
    }
};

// Scan the standard ICC directories for CMYK-colourspace profiles (header bytes 16..19 == "CMYK").
static QStringList scanCmykProfiles(){
    QStringList out;
    const QStringList dirs = { "/usr/share/color/icc", "/usr/share/color/icc/colord",
                               "/usr/local/share/color/icc", QDir::homePath()+"/.local/share/icc",
                               "/boot/system/data/color/icc", "/boot/home/config/non-packaged/data/color/icc" };
    for(const QString& dp : dirs){ QDir dir(dp); if(!dir.exists()) continue;
        for(const QFileInfo& fi : dir.entryInfoList({"*.icc","*.icm","*.ICC","*.ICM"}, QDir::Files)){
            QFile f(fi.absoluteFilePath()); if(!f.open(QIODevice::ReadOnly)) continue;
            const QByteArray head=f.read(20);
            if(head.size()>=20 && head.mid(16,4)=="CMYK") out << fi.absoluteFilePath();
        }
    }
    out.removeDuplicates(); return out;
}

namespace {
struct LcmsMem {
    typedef void* (*OpenMem)(const void*, cmsU32);
    typedef void* (*Srgb)();
    typedef void* (*Create)(void*, cmsU32, void*, cmsU32, cmsU32, cmsU32);
    typedef void (*Do)(void*, const void*, void*, cmsU32);
    typedef int (*Close)(void*);
    typedef void (*Del)(void*);
    OpenMem open = nullptr; Srgb srgb = nullptr; Create create = nullptr; Do run = nullptr; Close close = nullptr; Del del = nullptr;
    LcmsMem(){
        void* lib = dlopen("liblcms2.so.2", RTLD_NOW|RTLD_LOCAL);
        if(!lib) lib = dlopen("liblcms2.so", RTLD_NOW|RTLD_LOCAL);
        if(!lib) return;
        open = (OpenMem)dlsym(lib, "cmsOpenProfileFromMem"); srgb = (Srgb)dlsym(lib, "cmsCreate_sRGBProfile");
        create = (Create)dlsym(lib, "cmsCreateTransform"); run = (Do)dlsym(lib, "cmsDoTransform");
        close = (Close)dlsym(lib, "cmsCloseProfile"); del = (Del)dlsym(lib, "cmsDeleteTransform");
    }
    bool ok() const { return open && srgb && create && run && close && del; }
};
const LcmsMem& lcmsMem(){ static const LcmsMem l; return l; }
}

IccToSrgb::IccToSrgb(const QByteArray& profile, int components){
    const LcmsMem& l = lcmsMem();
    if(!l.ok() || profile.size() < 132 || profile.size() > (64 << 20)) return;
    // The profile's colour space, at byte 16, has to be the document's.
    const QByteArray space = profile.mid(16, 4);
    cmsU32 fmt = 0;
    if(components == 1 && space == "GRAY") fmt = (3u<<16)|(1u<<3)|1u;
    else if(components == 3 && space == "RGB ") fmt = TYPE_RGB_8;
    else if(components == 4 && space == "CMYK") fmt = TYPE_CMYK_8;
    if(!fmt) return;
    void* in = l.open(profile.constData(), cmsU32(profile.size()));
    if(!in) return;
    void* out = l.srgb();
    if(out) m_xf = l.create(in, fmt, out, TYPE_RGB_8, 1u /*relative colorimetric*/, 0x2000u /*black point compensation*/);
    l.close(in);
    if(out) l.close(out);
}
IccToSrgb::~IccToSrgb(){ if(m_xf) lcmsMem().del(m_xf); }
void IccToSrgb::convert(const uchar* in, uchar* rgb, int pixels) const { if(m_xf && pixels > 0) lcmsMem().run(m_xf, in, rgb, cmsU32(pixels)); }

Icc::Icc() : d(new Impl) {
    d->lib = dlopen("liblcms2.so.2", RTLD_NOW|RTLD_LOCAL);
    if(!d->lib) d->lib = dlopen("liblcms2.so", RTLD_NOW|RTLD_LOCAL);
    if(d->lib){
        d->OpenFromFile    = (fn_OpenFromFile)   dlsym(d->lib, "cmsOpenProfileFromFile");
        d->Create_sRGB     = (fn_Create_sRGB)    dlsym(d->lib, "cmsCreate_sRGBProfile");
        d->CreateTransform = (fn_CreateTransform)dlsym(d->lib, "cmsCreateTransform");
        d->DoTransform     = (fn_DoTransform)    dlsym(d->lib, "cmsDoTransform");
        d->CloseProfile    = (fn_CloseProfile)   dlsym(d->lib, "cmsCloseProfile");
        d->DeleteTransform = (fn_DeleteTransform)dlsym(d->lib, "cmsDeleteTransform");
    }
    if(d->loaded()){
        d->srgb = d->Create_sRGB();
        d->profiles = scanCmykProfiles();
        // default: prefer a "coated" commercial profile (SWOP/FOGRA/GRACoL), else the first CMYK found
        QString def;
        for(const QString& p : d->profiles){ const QString b=QFileInfo(p).fileName().toLower();
            if(b.contains("coated") && !b.contains("uncoated")){ def=p; break; } }
        if(def.isEmpty() && !d->profiles.isEmpty()) def=d->profiles.first();
        if(!def.isEmpty()) d->buildTransforms(def);
    }
}
Icc::~Icc(){
    if(d){ d->releaseTransforms();
        if(d->srgb && d->CloseProfile) d->CloseProfile(d->srgb);
        if(d->lib) dlclose(d->lib);
        delete d; d=nullptr; }
}

Icc& Icc::instance(){ static Icc inst; return inst; }

bool Icc::available() const { return d && d->loaded() && d->srgb && d->toCmyk && d->toRgb; }
QStringList Icc::cmykProfiles() const { return d? d->profiles : QStringList(); }
QString Icc::currentCmykProfile() const { return d? d->cmykPath : QString(); }
bool Icc::setCmykProfile(const QString& path){
    if(!d || !d->loaded() || !QFileInfo::exists(path)) return false;
    const bool ok=d->buildTransforms(path);
    if(ok && !d->profiles.contains(path)) d->profiles<<path;
    return ok;
}
void Icc::setRenderingIntent(int intent, bool bpc){
    if(!d) return;
    d->intent = (cmsU32)qBound(0,intent,3);
    d->flags  = bpc ? 0x2000u : 0u;                 // cmsFLAGS_BLACKPOINTCOMPENSATION
    if(!d->cmykPath.isEmpty() && d->loaded()) d->buildTransforms(d->cmykPath);   // re-derive with the new intent/BPC
}
int  Icc::renderingIntent() const { return d? int(d->intent) : 0; }
bool Icc::blackPointCompensation() const { return d && (d->flags & 0x2000u); }

QImage Icc::softProof(const QImage& src) const {
    if(!available() || src.isNull()) return QImage();
    const QImage a   = src.convertToFormat(QImage::Format_ARGB32);   // straight alpha (un-premultiplies)
    const QImage rgb = a.convertToFormat(QImage::Format_RGB888);     // straight R,G,B bytes (row-padded)
    const int W=rgb.width(), H=rgb.height();
    QImage out(W,H,QImage::Format_ARGB32);
    QVector<unsigned char> cmyk(size_t(W)*4), back(size_t(W)*3);
    for(int y=0;y<H;++y){
        const unsigned char* s=rgb.constScanLine(y);
        d->DoTransform(d->toCmyk, s, cmyk.data(), (cmsU32)W);
        d->DoTransform(d->toRgb, cmyk.data(), back.data(), (cmsU32)W);
        const QRgb* ar=(const QRgb*)a.constScanLine(y); QRgb* o=(QRgb*)out.scanLine(y);
        for(int x=0;x<W;++x) o[x]=qRgba(back[x*3], back[x*3+1], back[x*3+2], qAlpha(ar[x]));
    }
    return out;
}

bool Icc::toCmykPlanes(const QImage& src, QVector<QImage>& planes) const {
    if(!available() || src.isNull()) return false;
    const QImage rgb = src.convertToFormat(QImage::Format_RGB888);
    const int W=rgb.width(), H=rgb.height();
    planes.clear();
    for(int c=0;c<4;++c){ QImage p(W,H,QImage::Format_Grayscale8); p.fill(0); planes.append(p); }
    QVector<unsigned char> cmyk(size_t(W)*4);
    for(int y=0;y<H;++y){
        const unsigned char* s=rgb.constScanLine(y);
        d->DoTransform(d->toCmyk, s, cmyk.data(), (cmsU32)W);
        for(int c=0;c<4;++c){ unsigned char* pr=planes[c].scanLine(y); for(int x=0;x<W;++x) pr[x]=cmyk[x*4+c]; }
    }
    return true;
}

} // namespace xara
