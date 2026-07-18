// gif.cpp — minimal animated GIF89a writer. Quantises every frame to the 216-colour
// "web safe" palette (6×6×6), then GIF-LZW compresses each frame.
#include "gif.h"
#include <QFile>
#include <QByteArray>
#include <QHash>

namespace xara {

// nearest web-216 index for an (r,g,b): 6 levels per channel (0,51,…,255)
static inline int webIndex(int r,int g,int b){
    auto q=[](int c){ return (c*5+127)/255; };   // 0..5
    return q(r)*36 + q(g)*6 + q(b);
}

// GIF variable-width LZW for one frame's index buffer (min code size = 8).
static QByteArray lzwCompress(const QVector<quint8>& px){
    const int minCode=8;
    const int clearCode=1<<minCode;          // 256
    const int eoiCode=clearCode+1;           // 257
    QByteArray out;
    // bit packer → packed into sub-blocks of ≤255 bytes
    QByteArray block; int bitBuf=0, bitCnt=0;
    auto flushBlocks=[&](bool force){
        while(block.size()>=255){ out.append((char)255); out.append(block.left(255)); block.remove(0,255); }
        if(force && !block.isEmpty()){ out.append((char)block.size()); out.append(block); block.clear(); }
    };
    auto emitCode=[&](int code,int width){ bitBuf |= (code<<bitCnt); bitCnt+=width;
        while(bitCnt>=8){ block.append((char)(bitBuf&0xFF)); bitBuf>>=8; bitCnt-=8; flushBlocks(false); } };

    QHash<QByteArray,int> table; table.reserve(4096);
    auto reset=[&]{ table.clear(); for(int i=0;i<clearCode;++i) table.insert(QByteArray(1,(char)i), i); };
    int codeWidth=minCode+1, next=eoiCode+1;
    reset(); emitCode(clearCode,codeWidth);
    if(px.isEmpty()){ emitCode(eoiCode,codeWidth); }
    else {
        QByteArray cur; cur.append((char)px[0]);
        for(int i=1;i<px.size();++i){ char k=(char)px[i]; QByteArray nk=cur; nk.append(k);
            if(table.contains(nk)) cur=nk;
            else { emitCode(table.value(cur),codeWidth); table.insert(nk,next++);
                   if(next == (1<<codeWidth) && codeWidth<12) codeWidth++;
                   if(next>=4096){ emitCode(clearCode,codeWidth); reset(); codeWidth=minCode+1; next=eoiCode+1; }
                   cur=QByteArray(1,k); } }
        emitCode(table.value(cur),codeWidth);
        emitCode(eoiCode,codeWidth);
    }
    if(bitCnt>0){ block.append((char)(bitBuf&0xFF)); }     // flush remaining bits
    flushBlocks(true);
    out.append((char)0);                                    // block terminator
    return out;
}

static void put16(QByteArray& b, int v){ b.append((char)(v&0xFF)); b.append((char)((v>>8)&0xFF)); }

bool writeAnimatedGif(const QString& path, const QVector<QImage>& frames, int delayMs,
                      int loops, const QVector<int>& perFrameMs){
    if(frames.isEmpty()) return false;
    const int W=frames[0].width(), H=frames[0].height();
    QByteArray g;
    g.append("GIF89a");
    put16(g,W); put16(g,H);
    g.append((char)0xF7);   // global colour table, 256 entries (2^(7+1)), 8-bit colour
    g.append((char)0);      // background colour index
    g.append((char)0);      // pixel aspect ratio
    // global colour table: 216 web colours then pad to 256
    for(int i=0;i<256;++i){ if(i<216){ int r=i/36, gg=(i/6)%6, bb=i%6; g.append((char)(r*51)); g.append((char)(gg*51)); g.append((char)(bb*51)); }
        else { g.append((char)0); g.append((char)0); g.append((char)0); } }
    // Netscape looping extension (loops = repeat count; 0 = forever)
    g.append((char)0x21); g.append((char)0xFF); g.append((char)0x0B); g.append("NETSCAPE2.0");
    g.append((char)0x03); g.append((char)0x01); put16(g, qMax(0,loops)); g.append((char)0);
    const bool perFrame = perFrameMs.size()==frames.size();
    for(int fi=0; fi<frames.size(); ++fi){
        const QImage& f0 = frames[fi];
        QImage f = f0.convertToFormat(QImage::Format_ARGB32);
        if(f.width()!=W||f.height()!=H) f=f.scaled(W,H);
        const int delayCs = qMax(2, (perFrame ? perFrameMs[fi] : delayMs)/10);   // centiseconds
        // graphic control extension (per-frame delay)
        g.append((char)0x21); g.append((char)0xF9); g.append((char)0x04); g.append((char)0x04);   // disposal=1 (no transparency)
        put16(g,delayCs); g.append((char)0); g.append((char)0);
        // image descriptor
        g.append((char)0x2C); put16(g,0); put16(g,0); put16(g,W); put16(g,H); g.append((char)0);   // no local table
        // index buffer (composite on white)
        QVector<quint8> px; px.resize(W*H);
        for(int y=0;y<H;++y){ const QRgb* r=(const QRgb*)f.constScanLine(y);
            for(int x=0;x<W;++x){ QRgb p=r[x]; int a=qAlpha(p);
                int rr=(qRed(p)*a+255*(255-a))/255, gg=(qGreen(p)*a+255*(255-a))/255, bb=(qBlue(p)*a+255*(255-a))/255;
                px[y*W+x]=(quint8)webIndex(rr,gg,bb); } }
        g.append((char)0x08);                 // LZW minimum code size
        g.append(lzwCompress(px));
    }
    g.append((char)0x3B);                     // trailer
    QFile out(path); if(!out.open(QIODevice::WriteOnly)) return false; out.write(g); out.close();
    return true;
}

} // namespace xara
