// extcodecs.cpp — see extcodecs.h. dlopen wrappers for JPEG 2000 (OpenJPEG) and JPEG XL (libjxl).
// We declare the ABI-stable subset of each C API ourselves so no dev headers are needed; every path
// is fail-safe (missing lib / bad file → null QImage).
#include "extcodecs.h"
#include <QFileInfo>
#include <QFile>
#include "dlcompat.h"
#include <cstring>
#include <vector>

namespace xara { QImage exrDecode(const QString& path); }   // exr.cpp (vendored tinyexr)
namespace xara { QImage jxrDecode(const QString& path); bool jxrAvailable(); }   // jxr.cpp (linked jxrlib)
namespace xara { QImage dicomDecode(const QString& path); bool dicomAvailable(); }   // dicom.cpp (linked GDCM)
namespace xara { QImage slideDecode(const QString& path); bool slideAvailable(); }   // slide.cpp (linked OpenSlide)

namespace xara { namespace ExtCodec {
namespace {
using u8=uint8_t; using u32=uint32_t;
constexpr long long kMaxPixels = 400ll*1000*1000;

// ============================================================ OpenJPEG (JPEG 2000) ===
// OPJ_CODEC_FORMAT
enum { OPJ_CODEC_J2K=0, OPJ_CODEC_JPT=1, OPJ_CODEC_JP2=2 };
// opj_image_comp_t / opj_image_t — layout stable across OpenJPEG 2.x (verified against 2.5.0).
struct opj_image_comp { u32 dx,dy,w,h,x0,y0,prec,bpp,sgnd,resno_decoded,factor; int* data; unsigned short alpha; };
struct opj_image { u32 x0,y0,x1,y1,numcomps; int color_space; opj_image_comp* comps; u8* icc_buf; u32 icc_len; };

struct Opj {
    void* lib=nullptr; bool ok=false;
    void*(*create_decompress)(int)=nullptr;
    int  (*setup_decoder)(void*,void*)=nullptr;
    void (*set_default_params)(void*)=nullptr;
    void*(*stream_file)(const char*,int)=nullptr;
    int  (*read_header)(void*,void*,opj_image**)=nullptr;
    int  (*decode)(void*,void*,opj_image*)=nullptr;
    int  (*end_decompress)(void*,void*)=nullptr;
    void (*stream_destroy)(void*)=nullptr;
    void (*destroy_codec)(void*)=nullptr;
    void (*image_destroy)(opj_image*)=nullptr;
    Opj(){
        lib=dlopen("libopenjp2.so.7",RTLD_NOW|RTLD_LOCAL);
        if(!lib) lib=dlopen("libopenjp2.so",RTLD_NOW|RTLD_LOCAL);
        if(!lib) return;
        create_decompress =(void*(*)(int))          dlsym(lib,"opj_create_decompress");
        setup_decoder     =(int(*)(void*,void*))     dlsym(lib,"opj_setup_decoder");
        set_default_params=(void(*)(void*))          dlsym(lib,"opj_set_default_decoder_parameters");
        stream_file       =(void*(*)(const char*,int))dlsym(lib,"opj_stream_create_default_file_stream");
        read_header       =(int(*)(void*,void*,opj_image**))dlsym(lib,"opj_read_header");
        decode            =(int(*)(void*,void*,opj_image*))dlsym(lib,"opj_decode");
        end_decompress    =(int(*)(void*,void*))     dlsym(lib,"opj_end_decompress");
        stream_destroy    =(void(*)(void*))          dlsym(lib,"opj_stream_destroy");
        destroy_codec     =(void(*)(void*))          dlsym(lib,"opj_destroy_codec");
        image_destroy     =(void(*)(opj_image*))     dlsym(lib,"opj_image_destroy");
        ok = create_decompress&&setup_decoder&&set_default_params&&stream_file&&read_header
           &&decode&&end_decompress&&stream_destroy&&destroy_codec&&image_destroy;
    }
};
Opj& opj(){ static Opj o; return o; }

QImage decodeJP2(const QString& path){
    Opj& O=opj(); if(!O.ok) return {};
    // JP2 boxed signature vs raw J2K codestream → pick the codec.
    int codec=OPJ_CODEC_JP2;
    { QFile f(path); if(f.open(QIODevice::ReadOnly)){ QByteArray h=f.read(12);
        if(h.size()>=4 && (u8)h[0]==0xFF && (u8)h[1]==0x4F && (u8)h[2]==0xFF && (u8)h[3]==0x51) codec=OPJ_CODEC_J2K; } }
    void* stream=O.stream_file(path.toLocal8Bit().constData(),1);
    if(!stream) return {};
    void* dec=O.create_decompress(codec);
    if(!dec){ O.stream_destroy(stream); return {}; }
    std::vector<char> params(0x4000,0);               // opj_dparameters_t is ~8KB; over-allocate, zero, fill.
    O.set_default_params(params.data());
    QImage out;
    opj_image* image=nullptr;
    if(O.setup_decoder(dec,params.data()) && O.read_header(stream,dec,&image) && image
       && O.decode(dec,stream,image) && O.end_decompress(dec,stream)){
        const u32 nc=image->numcomps;
        if(nc>=1 && image->comps && image->comps[0].data){
            const opj_image_comp* c=image->comps;
            const long long W=c[0].w, H=c[0].h;
            if(W>0 && H>0 && W*H<=kMaxPixels){
                out=QImage(int(W),int(H),QImage::Format_ARGB32);
                if(!out.isNull()){
                    auto samp=[&](u32 ci,long long x,long long y)->int{
                        const opj_image_comp& cc=image->comps[ci];
                        if(!cc.data || cc.w==0 || cc.h==0) return 0;
                        const int prec=(cc.prec>=1 && cc.prec<=31)?int(cc.prec):8;   // guard UB shift on a crafted prec
                        long long sx=(cc.w==(u32)W)?x:(x*cc.w/W), sy=(cc.h==(u32)H)?y:(y*cc.h/H);
                        if(sx<0)sx=0; if(sy<0)sy=0; if(sx>=cc.w)sx=cc.w-1; if(sy>=cc.h)sy=cc.h-1;
                        long long v=cc.data[sy*cc.w+sx];
                        if(cc.sgnd) v += (1LL<<(prec-1));                      // recentre signed samples
                        const long long mx=(1LL<<prec)-1;
                        v = mx>0 ? (v*255/mx) : v; return v<0?0:(v>255?255:int(v)); };
                    const bool gray=(nc<3);
                    for(long long y=0;y<H;y++){ QRgb* o=reinterpret_cast<QRgb*>(out.scanLine(int(y)));
                        for(long long x=0;x<W;x++){
                            if(gray){ int g=samp(0,x,y); int a=(nc==2)?samp(1,x,y):255; o[x]=qRgba(g,g,g,a); }
                            else { int r=samp(0,x,y),gg=samp(1,x,y),b=samp(2,x,y); int a=(nc>=4)?samp(3,x,y):255;
                                   o[x]=qRgba(r,gg,b,a); } } }
                }
            }
        }
    }
    if(image) O.image_destroy(image);
    O.destroy_codec(dec);
    O.stream_destroy(stream);
    return out;
}

// ==================================================================== libjxl (JPEG XL) ===
// Minimal one-shot decode API (libjxl 0.7+). Enums & structs from jxl/decode.h & codestream_header.h.
enum { JXL_DEC_SUCCESS=0, JXL_DEC_ERROR=1, JXL_DEC_NEED_MORE_INPUT=2,
       JXL_DEC_NEED_IMAGE_OUT_BUFFER=5, JXL_DEC_BASIC_INFO=0x40, JXL_DEC_FULL_IMAGE=0x1000 };
enum { JXL_TYPE_UINT8=2 };                             // JxlDataType
enum { JXL_LITTLE_ENDIAN=0 };                          // JxlEndianness
struct JxlPixelFormat { u32 num_channels; int data_type; int endianness; size_t align; };
// JxlBasicInfo (0.7 layout). We only read xsize/ysize/alpha_bits; keep the full trailing region so
// JxlDecoderGetBasicInfo has room to write.
struct JxlBasicInfo {
    int have_container; u32 xsize; u32 ysize; u32 bits_per_sample; u32 exponent_bits_per_sample;
    float intensity_target; float min_nits; int relative_to_max_display; float linear_below;
    int uses_original_profile; u32 have_preview; u32 have_animation; int orientation;
    u32 num_color_channels; u32 num_extra_channels; u32 alpha_bits; u32 alpha_exponent_bits;
    int alpha_premultiplied; struct { u32 num,den; } preview; struct { u32 num,den; } animation_tps;
    u32 animation_num_loops; int animation_have_timecodes; u32 intrinsic_xsize; u32 intrinsic_ysize;
    u8 padding[100];                                   // slack for ABI drift / trailing fields
};
struct Jxl {
    void* lib=nullptr; bool ok=false;
    int   (*sig)(const u8*,size_t)=nullptr;
    void* (*create)(void*)=nullptr;
    void  (*destroy)(void*)=nullptr;
    int   (*subscribe)(void*,int)=nullptr;
    int   (*set_input)(void*,const u8*,size_t)=nullptr;
    int   (*process)(void*)=nullptr;
    int   (*basic_info)(void*,JxlBasicInfo*)=nullptr;
    int   (*out_size)(void*,const JxlPixelFormat*,size_t*)=nullptr;
    int   (*set_out)(void*,const JxlPixelFormat*,void*,size_t)=nullptr;
    Jxl(){
        lib=dlopen("libjxl.so.0.7",RTLD_NOW|RTLD_LOCAL);
        if(!lib) lib=dlopen("libjxl.so.0.8",RTLD_NOW|RTLD_LOCAL);
        if(!lib) lib=dlopen("libjxl.so.0.9",RTLD_NOW|RTLD_LOCAL);
        if(!lib) lib=dlopen("libjxl.so.0.10",RTLD_NOW|RTLD_LOCAL);
        if(!lib) lib=dlopen("libjxl.so",RTLD_NOW|RTLD_LOCAL);
        if(!lib) return;
        sig       =(int(*)(const u8*,size_t))     dlsym(lib,"JxlSignatureCheck");
        create    =(void*(*)(void*))              dlsym(lib,"JxlDecoderCreate");
        destroy   =(void(*)(void*))               dlsym(lib,"JxlDecoderDestroy");
        subscribe =(int(*)(void*,int))            dlsym(lib,"JxlDecoderSubscribeEvents");
        set_input =(int(*)(void*,const u8*,size_t))dlsym(lib,"JxlDecoderSetInput");
        process   =(int(*)(void*))                dlsym(lib,"JxlDecoderProcessInput");
        basic_info=(int(*)(void*,JxlBasicInfo*))  dlsym(lib,"JxlDecoderGetBasicInfo");
        out_size  =(int(*)(void*,const JxlPixelFormat*,size_t*))dlsym(lib,"JxlDecoderImageOutBufferSize");
        set_out   =(int(*)(void*,const JxlPixelFormat*,void*,size_t))dlsym(lib,"JxlDecoderSetImageOutBuffer");
        ok = sig&&create&&destroy&&subscribe&&set_input&&process&&basic_info&&out_size&&set_out;
    }
};
Jxl& jxl(){ static Jxl j; return j; }

QImage decodeJXL(const QString& path){
    Jxl& J=jxl(); if(!J.ok) return {};
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return {};
    const QByteArray data=f.read(512ll*1024*1024);
    const u8* p=reinterpret_cast<const u8*>(data.constData()); const size_t n=data.size();
    if(n<12) return {};
    void* dec=J.create(nullptr); if(!dec) return {};
    QImage out;
    if(J.subscribe(dec, JXL_DEC_BASIC_INFO|JXL_DEC_FULL_IMAGE)==JXL_DEC_SUCCESS
       && J.set_input(dec,p,n)==JXL_DEC_SUCCESS){
        JxlPixelFormat fmt{4,JXL_TYPE_UINT8,JXL_LITTLE_ENDIAN,0};   // RGBA8, little-endian
        JxlBasicInfo info; memset(&info,0,sizeof(info));
        std::vector<u8> pix; long long W=0,H=0; bool done=false;
        for(;;){
            const int st=J.process(dec);
            if(st==JXL_DEC_ERROR || st==JXL_DEC_NEED_MORE_INPUT) break;
            if(st==JXL_DEC_SUCCESS){ done=true; break; }
            else if(st==JXL_DEC_BASIC_INFO){ if(J.basic_info(dec,&info)!=JXL_DEC_SUCCESS) break;
                W=info.xsize; H=info.ysize; if(W<=0||H<=0||W*H>kMaxPixels) break; }
            else if(st==JXL_DEC_NEED_IMAGE_OUT_BUFFER){         // only NOW may we set the output buffer
                size_t need=0; if(J.out_size(dec,&fmt,&need)!=JXL_DEC_SUCCESS) break;
                if(W<=0||H<=0 || need<size_t(W*H*4)) break;
                pix.resize(need);
                if(J.set_out(dec,&fmt,pix.data(),pix.size())!=JXL_DEC_SUCCESS) break; }
            else if(st==JXL_DEC_FULL_IMAGE){ /* frame done; loop until SUCCESS */ }
        }
        if(done && W>0 && H>0 && pix.size()>=size_t(W*H*4)){
            out=QImage(int(W),int(H),QImage::Format_ARGB32);
            if(!out.isNull()) for(long long y=0;y<H;y++){ QRgb* o=reinterpret_cast<QRgb*>(out.scanLine(int(y)));
                const u8* s=pix.data()+size_t(y*W*4);
                for(long long x=0;x<W;x++){ const u8* q=s+x*4; o[x]=qRgba(q[0],q[1],q[2],q[3]); } }
        }
    }
    J.destroy(dec);
    return out;
}
// ================================================================= libmobi (MOBI/KF8) ===
// We extract the book's cover (first decodable embedded image) as a bitmap. Each resource part's
// data/size is a raw JPEG/PNG/GIF that QImage decodes directly, so we sidestep the nested MOBIRawml
// struct entirely and only touch MOBIPart (uid, type, data, size, next).
// libmobi MOBIPart layout: uid, type, SIZE, DATA, next (size precedes data — verified against libmobi.so.0).
struct MOBIPart { size_t uid; int type; size_t size; unsigned char* data; void* next; };
struct Mobi {
    void* lib=nullptr; bool ok=false;
    void* (*init)()=nullptr;
    int   (*load_filename)(void*,const char*)=nullptr;
    bool  (*is_encrypted)(const void*)=nullptr;    // returns C `bool` (1 byte) — must NOT be read as int
    void* (*init_rawml)(const void*)=nullptr;
    int   (*parse_rawml)(void*,const void*)=nullptr;
    MOBIPart* (*resource_by_uid)(const void*,size_t)=nullptr;
    void  (*free_rawml)(void*)=nullptr;
    void  (*free_)(void*)=nullptr;
    Mobi(){
        lib=dlopen("libmobi.so.0",RTLD_NOW|RTLD_LOCAL);
        if(!lib) lib=dlopen("libmobi.so",RTLD_NOW|RTLD_LOCAL);
        if(!lib) return;
        init           =(void*(*)())                dlsym(lib,"mobi_init");
        load_filename  =(int(*)(void*,const char*)) dlsym(lib,"mobi_load_filename");
        is_encrypted   =(bool(*)(const void*))      dlsym(lib,"mobi_is_encrypted");
        init_rawml     =(void*(*)(const void*))     dlsym(lib,"mobi_init_rawml");
        parse_rawml    =(int(*)(void*,const void*)) dlsym(lib,"mobi_parse_rawml");
        resource_by_uid=(MOBIPart*(*)(const void*,size_t))dlsym(lib,"mobi_get_resource_by_uid");
        free_rawml     =(void(*)(void*))            dlsym(lib,"mobi_free_rawml");
        free_          =(void(*)(void*))            dlsym(lib,"mobi_free");
        ok = init&&load_filename&&init_rawml&&parse_rawml&&resource_by_uid&&free_rawml&&free_;
    }
};
Mobi& mobi(){ static Mobi m; return m; }

QImage decodeMOBI(const QString& path){
    Mobi& M=mobi(); if(!M.ok) return {};
    void* m=M.init(); if(!m) return {};
    QImage out;
    if(M.load_filename(m, path.toLocal8Bit().constData())==0 && !(M.is_encrypted && M.is_encrypted(m))){
        void* rawml=M.init_rawml(m);
        if(rawml && M.parse_rawml(rawml,m)==0){
            // The cover is the largest image among the early resources; scan a bounded window and keep it.
            long long bestArea=0; int seen=0, misses=0;
            for(size_t uid=0; uid<4096 && misses<64 && seen<48; ++uid){
                MOBIPart* part=M.resource_by_uid(rawml,uid);
                if(!part){ ++misses; continue; }
                misses=0;
                if(part->data && part->size>0 && part->size<256ll*1024*1024){
                    QImage img=QImage::fromData(part->data,int(part->size));  // JPEG/PNG/GIF via Qt
                    if(!img.isNull()){ ++seen; const long long a=(long long)img.width()*img.height();
                        if(a>bestArea){ bestArea=a; out=img.convertToFormat(QImage::Format_ARGB32); } }  // deep copy before free
                }
            }
        }
        if(rawml) M.free_rawml(rawml);
    }
    M.free_(m);
    return out;
}
// ================================================================= DjVuLibre (DjVu) ===
// Render the first page to RGB24 via the async ddjvu API (opaque handles + a message pump).
struct ddjvu_rect { int x, y; unsigned int w, h; };
struct DjVu {
    void* lib=nullptr; bool ok=false;
    void* (*context_create)(const char*)=nullptr;
    void* (*document_create_by_filename)(void*,const char*,int)=nullptr;
    int   (*job_status)(void*)=nullptr;   // ddjvu_{document,page}_decoding_status are header MACROS → use ddjvu_job_status (doc/page ARE jobs)
    void* (*message_wait)(void*)=nullptr;
    void* (*message_peek)(void*)=nullptr;
    void  (*message_pop)(void*)=nullptr;
    void* (*page_create_by_pageno)(void*,int)=nullptr;
    int   (*page_get_width)(void*)=nullptr;
    int   (*page_get_height)(void*)=nullptr;
    void* (*format_create)(int,int,unsigned int*)=nullptr;
    void  (*format_set_row_order)(void*,int)=nullptr;
    int   (*page_render)(void*,int,const ddjvu_rect*,const ddjvu_rect*,const void*,unsigned long,char*)=nullptr;
    void  (*format_release)(void*)=nullptr;
    void  (*job_release)(void*)=nullptr;   // ddjvu_{document,page}_release are header MACROS → ddjvu_job_release
    void  (*context_release)(void*)=nullptr;
    DjVu(){
        lib=dlopen("libdjvulibre.so.21",RTLD_NOW|RTLD_LOCAL);
        if(!lib) lib=dlopen("libdjvulibre.so",RTLD_NOW|RTLD_LOCAL);
        if(!lib) return;
        context_create             =(void*(*)(const char*))          dlsym(lib,"ddjvu_context_create");
        document_create_by_filename=(void*(*)(void*,const char*,int)) dlsym(lib,"ddjvu_document_create_by_filename");
        job_status                 =(int(*)(void*))                  dlsym(lib,"ddjvu_job_status");
        message_wait               =(void*(*)(void*))                dlsym(lib,"ddjvu_message_wait");
        message_peek               =(void*(*)(void*))                dlsym(lib,"ddjvu_message_peek");
        message_pop                =(void(*)(void*))                 dlsym(lib,"ddjvu_message_pop");
        page_create_by_pageno      =(void*(*)(void*,int))            dlsym(lib,"ddjvu_page_create_by_pageno");
        page_get_width             =(int(*)(void*))                  dlsym(lib,"ddjvu_page_get_width");
        page_get_height            =(int(*)(void*))                  dlsym(lib,"ddjvu_page_get_height");
        format_create              =(void*(*)(int,int,unsigned int*))dlsym(lib,"ddjvu_format_create");
        format_set_row_order       =(void(*)(void*,int))             dlsym(lib,"ddjvu_format_set_row_order");
        page_render                =(int(*)(void*,int,const ddjvu_rect*,const ddjvu_rect*,const void*,unsigned long,char*))dlsym(lib,"ddjvu_page_render");
        format_release             =(void(*)(void*))                 dlsym(lib,"ddjvu_format_release");
        job_release                =(void(*)(void*))                 dlsym(lib,"ddjvu_job_release");
        context_release            =(void(*)(void*))                 dlsym(lib,"ddjvu_context_release");
        ok = context_create&&document_create_by_filename&&job_status&&message_wait&&message_peek
           &&message_pop&&page_create_by_pageno&&page_get_width&&page_get_height
           &&format_create&&format_set_row_order&&page_render&&format_release&&job_release&&context_release;
    }
};
DjVu& djvu(){ static DjVu d; return d; }

QImage decodeDjVu(const QString& path){
    DjVu& D=djvu(); if(!D.ok) return {};
    void* ctx=D.context_create("xarq"); if(!ctx) return {};
    QImage out;
    // Pump the async message queue until the given status reaches DDJVU_JOB_OK(2); bail on FAILED(3)/STOPPED(4).
    auto pump=[&](void* obj,int(*statusFn)(void*))->int{
        int s=0; for(int i=0;i<200000;i++){ s=statusFn(obj); if(s>=2) return s;
            D.message_wait(ctx); while(D.message_peek(ctx)) D.message_pop(ctx); } return -1; };
    void* doc=D.document_create_by_filename(ctx, path.toLocal8Bit().constData(), 1);
    if(doc){
        if(pump(doc,D.job_status)==2){
            void* page=D.page_create_by_pageno(doc,0);
            if(page){
                if(pump(page,D.job_status)==2){
                    const int w=D.page_get_width(page), h=D.page_get_height(page);
                    if(w>0 && h>0 && (long long)w*h<=kMaxPixels){
                        void* fmt=D.format_create(1 /*DDJVU_FORMAT_RGB24*/,0,nullptr);
                        if(fmt){
                            D.format_set_row_order(fmt,1);                    // top-to-bottom
                            ddjvu_rect pr{0,0,(unsigned)w,(unsigned)h}, rr=pr;
                            std::vector<char> buf((size_t)w*h*3, 0);
                            if(D.page_render(page,0 /*DDJVU_RENDER_COLOR*/,&pr,&rr,fmt,(unsigned long)w*3,buf.data())){
                                out=QImage(w,h,QImage::Format_ARGB32);
                                if(!out.isNull()) for(int y=0;y<h;y++){ QRgb* o=reinterpret_cast<QRgb*>(out.scanLine(y));
                                    const u8* s=reinterpret_cast<const u8*>(buf.data())+(size_t)y*w*3;
                                    for(int x=0;x<w;x++){ const u8* p=s+x*3; o[x]=qRgb(p[0],p[1],p[2]); } }
                            }
                            D.format_release(fmt);
                        }
                    }
                }
                D.job_release(page);
            }
        }
        D.job_release(doc);
    }
    D.context_release(ctx);
    return out;
}

// ============================================================ jbig2dec (JBIG2 / ITU T.88) ===
// Bilevel scanned-document codec. No -dev package ships a jbig2.h, so we declare the ABI subset we
// use; layout verified against libjbig2dec.so.0 (0.20): Jbig2Image is 32 bytes — w@0, h@4, stride@8,
// data@16, refcount@24 — 1 bit/pixel packed MSB-first, `1` = BLACK, `stride` bytes per row.
enum { JBIG2_OPTIONS_EMBEDDED=1 };
struct Jbig2Image { u32 width, height, stride; u8* data; int refcount; };
typedef int (*Jbig2ErrorCallback)(void* data, const char* msg, int severity, u32 seg_idx);
int jbig2Silent(void*, const char*, int, u32){ return 0; }   // jbig2dec's built-in callback chats to stderr

struct Jb2 {
    void* lib=nullptr; bool ok=false;
    void* (*ctx_new)(void*,int,void*,Jbig2ErrorCallback,void*)=nullptr;   // the 5-arg entry, NOT *_imp
    int   (*data_in)(void*,const unsigned char*,size_t)=nullptr;
    int   (*complete_page)(void*)=nullptr;
    Jbig2Image* (*page_out)(void*)=nullptr;
    void  (*release_page)(void*,Jbig2Image*)=nullptr;
    void  (*ctx_free)(void*)=nullptr;
    Jb2(){
        lib=dlopen("libjbig2dec.so.0",RTLD_NOW|RTLD_LOCAL);
        if(!lib) lib=dlopen("libjbig2dec.so",RTLD_NOW|RTLD_LOCAL);
        if(!lib) return;
        ctx_new      =(void*(*)(void*,int,void*,Jbig2ErrorCallback,void*))dlsym(lib,"jbig2_ctx_new");
        data_in      =(int(*)(void*,const unsigned char*,size_t)) dlsym(lib,"jbig2_data_in");
        complete_page=(int(*)(void*))                             dlsym(lib,"jbig2_complete_page");
        page_out     =(Jbig2Image*(*)(void*))                     dlsym(lib,"jbig2_page_out");
        release_page =(void(*)(void*,Jbig2Image*))                dlsym(lib,"jbig2_release_page");
        ctx_free     =(void(*)(void*))                            dlsym(lib,"jbig2_ctx_free");
        ok = ctx_new&&data_in&&complete_page&&page_out&&release_page&&ctx_free;
    }
};
Jb2& jb2(){ static Jb2 j; return j; }

u32 jbRd32(const u8* p){ return (u32(p[0])<<24)|(u32(p[1])<<16)|(u32(p[2])<<8)|p[3]; }
// Pre-flight: walk the segment headers for the first page-information segment (type 48) and read the
// page size, so a tiny file can't make jbig2dec allocate a huge page buffer inside data_in (a 104-byte
// file declaring 65536x32767 costs the bare library ~268 MB). Returns false when the size can't be
// established — then jbig2dec's own limit applies.
bool jbig2PageSize(const u8* p, long long n, long long off, bool seq, long long& W, long long& H){
    long long i=off; int guard=0;
    while(i+11<=n && guard++<4096){
        const u32 num=jbRd32(p+i); const u8 flags=p[i+4];
        const int type=flags&0x3f; const bool pa4=(flags&0x40)!=0;   // bit6: 4-byte page association
        long long j=i+5; if(j>=n) return false;
        const u8 rt=p[j]; long long cnt;                              // referred-to count + retain bits
        if((rt>>5)==7){ if(j+4>n) return false; cnt=(long long)(jbRd32(p+j)&0x1FFFFFFFu); j+=4+(cnt+8)/8; }
        else { cnt=rt>>5; j+=1; }
        if(cnt<0 || cnt>1000000) return false;
        j += cnt * (num<=256?1:(num<=65536?2:4));                     // referred-to numbers
        j += pa4?4:1;                                                 // page association
        if(j+4>n) return false;
        const u32 dlen=jbRd32(p+j); j+=4;                             // segment data length
        if(type==48){ if(!seq || j+8>n) return false; W=jbRd32(p+j); H=jbRd32(p+j+4); return true; }
        if(!seq){ i=j; continue; }                                    // random access: headers are contiguous
        if(dlen==0xFFFFFFFFu) return false;                           // unknown length — give up
        i = j + (long long)dlen;
    }
    return false;
}

QImage decodeJBIG2(const QString& path){
    Jb2& J=jb2(); if(!J.ok) return {};
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return {};
    const QByteArray data=f.read(256ll*1024*1024);
    const u8* p=reinterpret_cast<const u8*>(data.constData());
    const long long n=data.size(); if(n<11) return {};
    // A standalone .jb2 starts with the 8-byte file header; a raw embedded stream (as in a PDF) has
    // none. jbig2dec parses the header ITSELF when the context is not EMBEDDED, and rejects a
    // header-less stream in that mode, so the sniff picks the mode and we always feed the WHOLE file.
    static const u8 kMagic[8]={0x97,0x4A,0x42,0x32,0x0D,0x0A,0x1A,0x0A};
    const bool standalone = !memcmp(p,kMagic,8);
    { long long W=0,H=0, off=0; bool seq=true;
      if(standalone){ const u8 fl=p[8]; seq=(fl&1)!=0; off = 9 + ((fl&2)?0:4); }   // bit0 sequential, bit1 npages-unknown
      // Bound each dimension BEFORE multiplying: both come from u32 fields, so W*H can reach ~1.8e19
      // and overflow a signed 64-bit multiply (UB, and this project's UBSan build traps on it).
      const bool known = jbig2PageSize(p,n,off,seq,W,H);
      // A page height of 0xFFFFFFFF is the spec's "striped, height unknown" marker — legal, and the
      // real height arrives with the end-of-stripe segment, so don't reject it on the declared value.
      const bool striped = known && H==0xFFFFFFFFll;
      if(known && !striped &&
         (W<=0 || H<=0 || W>kMaxPixels || H>kMaxPixels || W>kMaxPixels/H)) return {};
      // If the size could NOT be established (random-access organisation, unknown segment length),
      // jbig2dec would allocate the page buffer inside data_in with only its own 2 GB ceiling. Require
      // the file to be large enough to plausibly describe such a page before handing it over.
      if(!known && n < 1024) return {}; }
    void* ctx=J.ctx_new(nullptr, standalone?0:JBIG2_OPTIONS_EMBEDDED, nullptr, &jbig2Silent, nullptr);
    if(!ctx) return {};
    QImage out;
    if(J.data_in(ctx,p,size_t(n))>=0){
        Jbig2Image* im=J.page_out(ctx);                       // standalone + end-of-page: already complete
        if(!im){ J.complete_page(ctx); im=J.page_out(ctx); }  // embedded / truncated: force the page out
        if(im){
            const long long W=im->width, H=im->height, S=im->stride;
            if(W>0 && H>0 && S>0 && im->data && W*H<=kMaxPixels && S>=(W+7)/8){
                out=QImage(int(W),int(H),QImage::Format_ARGB32);
                if(!out.isNull()){
                    const QRgb black=qRgb(0,0,0), white=qRgb(255,255,255);
                    for(long long y=0;y<H;y++){
                        QRgb* o=reinterpret_cast<QRgb*>(out.scanLine(int(y)));
                        const u8* s=im->data + y*S;
                        for(long long x=0;x<W;x++) o[x] = ((s[x>>3]>>(7-(x&7)))&1) ? black : white;
                    }
                }
            }
            J.release_page(ctx,im);                           // page_out returns a REFERENCED image
        }
    }
    J.ctx_free(ctx);
    return out;
}

} // anon namespace

QImage decode(const QString& path){
    const QString e=QFileInfo(path).suffix().toLower();
    if(e=="jp2"||e=="jpm"||e=="jpx"||e=="jpf"||e=="j2k"||e=="j2c"||e=="jpc"||e=="jpx"||e=="jpf") return decodeJP2(path);
    if(e=="jxl") return decodeJXL(path);
    if(e=="mobi"||e=="azw"||e=="azw3"||e=="prc") return decodeMOBI(path);
    if(e=="exr"||e=="sxr"||e=="mxr") return xara::exrDecode(path);
    if(e=="jxr"||e=="wdp"||e=="hdp"||e=="wmp") return xara::jxrDecode(path);
    if(e=="dcm"||e=="dicom") return xara::dicomDecode(path);
    if(e=="svs"||e=="ndpi"||e=="scn"||e=="mrxs"||e=="vms"||e=="vmu"||e=="svslide"||e=="bif")
        return xara::slideDecode(path);
    if(e=="djvu"||e=="djv") return decodeDjVu(path);
    if(e=="jb2"||e=="jbig2") return decodeJBIG2(path);
    return {};
}
QStringList extensions(){
    QStringList e;
    e<<"exr"<<"sxr"<<"mxr";                                 // OpenEXR via vendored tinyexr (always compiled in)
    if(jp2Available())  e<<"jp2"<<"j2k"<<"j2c"<<"jpc"<<"jpx"<<"jpf";
    if(jxlAvailable())  e<<"jxl";
    if(jxrAvailable())  e<<"jxr"<<"wdp"<<"hdp"<<"wmp";      // JPEG XR / HD Photo via linked jxrlib
    if(dicomAvailable())e<<"dcm"<<"dicom";                  // DICOM medical images via linked GDCM
    if(slideAvailable())e<<"svs"<<"ndpi"<<"scn"<<"mrxs"<<"vms"<<"vmu"<<"svslide"<<"bif";  // whole-slide via OpenSlide
    if(mobiAvailable()) e<<"mobi"<<"azw"<<"azw3"<<"prc";
    if(djvuAvailable()) e<<"djvu"<<"djv";
    if(jbig2Available()) e<<"jb2"<<"jbig2";                  // JBIG2 bilevel scans via jbig2dec
    return e;
}
bool jp2Available(){ return opj().ok; }
bool jxlAvailable(){ return jxl().ok; }
bool jxrAvailable(){ return xara::jxrAvailable(); }
bool dicomAvailable(){ return xara::dicomAvailable(); }
bool slideAvailable(){ return xara::slideAvailable(); }
bool mobiAvailable(){ return mobi().ok; }
bool djvuAvailable(){ return djvu().ok; }
bool jbig2Available(){ return jb2().ok; }

}} // namespace xara::ExtCodec
