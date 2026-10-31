// jxr.cpp — JPEG XR / HD Photo decode via jxrlib (JXRGlue). See jxr.h. All jxrlib headers/symbols are
// confined to this translation unit; the rest of the app only sees xara::jxrDecode / xara::jxrAvailable.
#include "jxr.h"

#ifdef HAVE_JXR
#include <QFile>
#include <vector>
#include <cstring>
// jxrlib's public umbrella header (self-guards extern "C"); pulls in JXRMeta.h + guiddef.h. The
// -D__ANSI__ / -DDISABLE_PERF_MEASUREMENT the header expects are applied to this file by CMake.
#include <JXRGlue.h>

namespace xara {
bool jxrAvailable(){ return true; }

QImage jxrDecode(const QString& path){
    QFile f(path);
    if(!f.open(QIODevice::ReadOnly)) return {};
    QByteArray data = f.readAll(); f.close();          // jxrlib reads from a memory stream over these bytes;
    if(data.isEmpty()) return {};                      // `data` must (and does) outlive the whole decode below.

    PKFactory*         factory = nullptr;
    PKCodecFactory*    codecs  = nullptr;
    PKImageDecode*     decoder = nullptr;
    PKFormatConverter* conv    = nullptr;
    struct WMPStream*  stream  = nullptr;
    QImage out;

    auto teardown = [&]{
        if(conv)    conv->Release(&conv);
        Bool streamOwned = decoder ? decoder->fStreamOwner : (Bool)0;   // decoder frees the stream only if it owns it
        if(decoder) decoder->Release(&decoder);
        if(stream && !streamOwned) stream->Close(&stream);              // ...otherwise we must, or it leaks
        if(codecs)  codecs->Release(&codecs);
        if(factory) factory->Release(&factory);
    };

    if(Failed(PKCreateFactory(&factory, PK_SDK_VERSION)))            { teardown(); return {}; }
    if(Failed(PKCreateCodecFactory(&codecs, WMP_SDK_VERSION)))       { teardown(); return {}; }
    if(Failed(factory->CreateStreamFromMemory(&stream, data.data(), (size_t)data.size()))) { teardown(); return {}; }
    if(Failed(PKImageDecode_Create_WMP(&decoder)))                  { teardown(); return {}; }
    if(Failed(decoder->Initialize(decoder, stream)))                { teardown(); return {}; }

    I32 w = 0, h = 0;
    if(Failed(decoder->GetSize(decoder, &w, &h)) ||
       w <= 0 || h <= 0 || (qint64)w * h > 400000000LL)             { teardown(); return {}; }   // cap ~400 Mpix

    const bool hasAlpha = decoder->WMP.bHasAlpha != 0;
    // JPEG XR stores alpha as a SEPARATE (planar) plane; the decoder skips it unless told to decode
    // "image + alpha" (uAlphaMode 2). Without this the format converter fills the alpha byte with zeros
    // (matches the reference JxrDecApp, which sets the same field). Harmless when the file has no alpha.
    if(hasAlpha) decoder->WMP.wmiSCP.uAlphaMode = 2;

    // SAFETY-CRITICAL: jxrlib's format-converter Copy decodes the file's NATIVE pixel format into the caller
    // buffer FIRST (at the stride we pass) and only THEN converts IN PLACE. So the buffer/stride must be sized
    // for the WIDER of source and destination bytes-per-pixel — otherwise a 48/96/128-bpp HDR JPEG XR, which
    // won't convert to 32bppBGRA but WILL convert to 24bppRGB, would have the decoder write srcBpp*w bytes into
    // a 3*w-strided row and overrun the heap. Look up the native source width (fall back to the widest jxrlib
    // unit, 16 B, if the lookup fails) so the allocation always covers the native write.
    U32 srcBpp = 16;
    { PKPixelInfo pi; std::memset(&pi, 0, sizeof(pi)); pi.pGUIDPixFmt = &decoder->guidPixFormat;
      if(!Failed(PixelFormatLookup(&pi, LOOKUP_FORWARD)) && pi.cbitUnit) srcBpp = (pi.cbitUnit + 7) / 8; }

    // Convert into a buffer/stride wide enough for both source and destination, then lift each destination row
    // (destBpp*w bytes, at the widened stride) into a QImage scanline. `conv` must already be Initialize()d.
    auto copyOut = [&](int destBpp, QImage::Format qfmt) -> QImage {
        const qint64 unit     = qMax<qint64>(srcBpp, destBpp);
        const qint64 stride64 = unit * (qint64)w;
        const qint64 bytes    = stride64 * (qint64)h;
        if(stride64 > 0xFFFFFFFFLL || bytes > (qint64)2 * 1024 * 1024 * 1024) return {};   // U32 stride + 2 GB cap
        const U32 stride = (U32)stride64;
        std::vector<U8> buf((size_t)bytes);
        PKRect rect = { 0, 0, w, h };
        if(Failed(conv->Copy(conv, &rect, buf.data(), stride))) return {};
        QImage img(w, h, qfmt);
        const size_t rowBytes = (size_t)destBpp * (size_t)w;
        for(int y = 0; y < h; ++y) std::memcpy(img.scanLine(y), buf.data() + (size_t)y * stride, rowBytes);
        return img.copy();                                                                  // detach from buf
    };

    // Preferred: 32bpp BGRA, whose byte order matches Qt's ARGB32/RGB32 in memory.
    if(Failed(codecs->CreateFormatConverter(&conv)))                { teardown(); return {}; }
    if(!Failed(conv->Initialize(conv, decoder, nullptr, GUID_PKPixelFormat32bppBGRA))){
        out = copyOut(4, hasAlpha ? QImage::Format_ARGB32 : QImage::Format_RGB32);
        teardown();
        return out;
    }
    // Fallback: 24bpp RGB (for gray/HDR sources jxrlib won't convert to BGRA). Fresh converter required
    // because Initialize already partially bound the last one.
    conv->Release(&conv); conv = nullptr;
    if(Failed(codecs->CreateFormatConverter(&conv)))                { teardown(); return {}; }
    if(Failed(conv->Initialize(conv, decoder, nullptr, GUID_PKPixelFormat24bppRGB))) { teardown(); return {}; }
    out = copyOut(3, QImage::Format_RGB888);
    if(!out.isNull()) out = out.convertToFormat(QImage::Format_RGB32);
    teardown();
    return out;
}
}   // namespace xara

#else   // no libjxr at build time

namespace xara {
bool   jxrAvailable(){ return false; }
QImage jxrDecode(const QString&){ return {}; }
}

#endif
