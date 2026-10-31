// dicom.cpp — DICOM decode via GDCM. See dicom.h. All GDCM headers/symbols are confined to this TU.
#include "dicom.h"

#ifdef HAVE_GDCM
#include <gdcmImageReader.h>
#include <gdcmImage.h>
#include <gdcmImageApplyLookupTable.h>
#include <gdcmImageChangePhotometricInterpretation.h>
#include <gdcmPhotometricInterpretation.h>
#include <gdcmPixelFormat.h>
#include <vector>
#include <cstdint>
#include <cstring>

namespace xara {
bool dicomAvailable(){ return true; }

QImage dicomDecode(const QString& path){
    gdcm::ImageReader reader;
    reader.SetFileName(path.toLocal8Bit().constData());
    if(!reader.Read()) return {};

    using PI = gdcm::PhotometricInterpretation;
    const gdcm::Image* img = &reader.GetImage();
    gdcm::PhotometricInterpretation pi = img->GetPhotometricInterpretation();

    // GDCM's GetBuffer() returns the STORED pixels: for PALETTE COLOR those are palette indices (not colour),
    // and for uncompressed/RLE YBR they are Y/Cb/Cr (not RGB). Normalise both to real RGB with GDCM's own
    // filters first (kept alive here so their GetOutput() references stay valid).
    gdcm::ImageApplyLookupTable lut;
    gdcm::ImageChangePhotometricInterpretation toRgb;
    if(pi.GetType() == PI::PALETTE_COLOR){
        lut.SetInput(*img);
        if(lut.Apply()){ img = &lut.GetOutput(); pi = img->GetPhotometricInterpretation(); }
    } else if(pi.GetType()==PI::YBR_FULL || pi.GetType()==PI::YBR_FULL_422 || pi.GetType()==PI::YBR_PARTIAL_422 || pi.GetType()==PI::YBR_PARTIAL_420){
        toRgb.SetInput(*img);
        toRgb.SetPhotometricInterpretation(PI::RGB);
        if(toRgb.Change()){ img = &toRgb.GetOutput(); pi = img->GetPhotometricInterpretation(); }
    }

    const qint64 cols = (qint64)img->GetColumns(), rows = (qint64)img->GetRows();
    if(cols<=0 || rows<=0 || cols*rows > 100000000LL) return {};          // cap ~100 Mpix
    const qint64 npix = cols*rows;

    const gdcm::PixelFormat& pf = img->GetPixelFormat();
    const int  spp = pf.GetSamplesPerPixel();
    const int  ba  = pf.GetBitsAllocated();
    const bool sgn = pf.GetPixelRepresentation()!=0;
    const bool isFloat = (pf.GetScalarType()==gdcm::PixelFormat::FLOAT32);
    const bool mono1 = (pi.GetType() == PI::MONOCHROME1);
    const double slope = img->GetSlope(), intercept = img->GetIntercept();

    std::vector<char> buf(img->GetBufferLength());
    if(buf.empty() || !img->GetBuffer(buf.data())) return {};
    const unsigned char* p = reinterpret_cast<const unsigned char*>(buf.data());
    const qint64 bufSz = (qint64)buf.size();

    // ---- Colour (RGB, 8- or 16-bit per channel) ---------------------------------------------------
    if(spp==3 && (ba==8 || ba==16)){
        const int bps = ba/8;
        if(bufSz < npix*3*bps) return {};
        const bool planar = (img->GetPlanarConfiguration()==1);            // 1 = RRR…GGG…BBB, 0 = RGBRGB…
        auto ch=[&](qint64 s)->uchar{ return (bps==1) ? p[s] : p[s*2+1]; };// 16-bit LE → high byte
        std::vector<uchar> rgb((size_t)npix*3);
        for(qint64 i=0;i<npix;i++){
            if(planar){ rgb[i*3]=ch(i); rgb[i*3+1]=ch(npix+i); rgb[i*3+2]=ch(2*npix+i); }
            else      { rgb[i*3]=ch(i*3); rgb[i*3+1]=ch(i*3+1); rgb[i*3+2]=ch(i*3+2); }
        }
        QImage out((const uchar*)rgb.data(), int(cols), int(rows), int(cols*3), QImage::Format_RGB888);
        return out.convertToFormat(QImage::Format_ARGB32);                 // deep-copies out of rgb
    }

    // ---- Grayscale (8/16/32-bit int, or 32-bit float) → auto-windowed 8-bit -----------------------
    if(spp!=1) return {};
    const int bps = ba/8; if(bps<1 || bps>4) return {};
    if(bufSz < npix*bps) return {};
    auto sample=[&](qint64 i)->double{
        const unsigned char* q = p + i*bps;
        if(bps==1) return sgn ? (double)(int8_t)q[0] : (double)q[0];
        if(bps==2){ int v = q[0] | (q[1]<<8); if(sgn && (v&0x8000)) v -= 0x10000; return (double)v; }
        const uint32_t u = (uint32_t)q[0] | ((uint32_t)q[1]<<8) | ((uint32_t)q[2]<<16) | ((uint32_t)q[3]<<24);
        if(isFloat){ float f; std::memcpy(&f, &u, 4); return (double)f; }   // FLOAT32 parametric maps
        return sgn ? (double)(int32_t)u : (double)u;
    };
    double lo = 1e300, hi = -1e300;
    for(qint64 i=0;i<npix;i++){ const double v = slope*sample(i)+intercept; if(v<lo)lo=v; if(v>hi)hi=v; }
    const double range = (hi>lo) ? (hi-lo) : 1.0;
    QImage out(int(cols), int(rows), QImage::Format_Grayscale8);
    if(out.isNull()) return {};
    for(qint64 y=0;y<rows;y++){
        uchar* row = out.scanLine(int(y));
        for(qint64 x=0;x<cols;x++){
            const double v = slope*sample(y*cols+x)+intercept;
            int g = (int)((v-lo)/range*255.0 + 0.5); g = g<0?0:(g>255?255:g);
            row[x] = (uchar)(mono1 ? 255-g : g);
        }
    }
    return out.convertToFormat(QImage::Format_ARGB32);
}
}   // namespace xara

#else   // no GDCM at build time

namespace xara {
bool   dicomAvailable(){ return false; }
QImage dicomDecode(const QString&){ return {}; }
}

#endif
