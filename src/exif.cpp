#include "exif.h"
#include <QFile>
#include <cstring>

namespace xara {

namespace {
struct Tiff {
    const uchar* d = nullptr; int n = 0; bool be = false;   // be = big-endian (MM)
    quint32 u16(int o) const { if(o<0||o+2>n) return 0;
        return be ? (quint32(d[o])<<8)|d[o+1] : (quint32(d[o+1])<<8)|d[o]; }
    quint32 u32(int o) const { if(o<0||o+4>n) return 0;
        return be ? (quint32(d[o])<<24)|(quint32(d[o+1])<<16)|(quint32(d[o+2])<<8)|d[o+3]
                  : (quint32(d[o+3])<<24)|(quint32(d[o+2])<<16)|(quint32(d[o+1])<<8)|d[o]; }
};
int typeSize(int t){ switch(t){case 1:case 2:case 6:case 7:return 1;case 3:case 8:return 2;
    case 4:case 9:case 11:return 4;case 5:case 10:case 12:return 8;default:return 1;} }

QString asciiVal(const Tiff& t,int valOff,int count){
    int len=qMin(count,t.n-valOff); if(valOff<0||len<=0) return QString();
    QByteArray b(reinterpret_cast<const char*>(t.d+valOff),len);
    int z=b.indexOf('\0'); if(z>=0) b.truncate(z); return QString::fromLatin1(b).trimmed();
}
// Parse one IFD; fill `out` with selected tags; return the Exif sub-IFD pointer (0 if none).
quint32 parseIFD(const Tiff& t,int ifdOff,QMap<QString,QString>& out){
    quint32 exifPtr=0; if(ifdOff<=0||ifdOff+2>t.n) return 0;
    int cnt=t.u16(ifdOff); int p=ifdOff+2;
    for(int i=0;i<cnt && p+12<=t.n;++i,p+=12){
        int tag=t.u16(p), type=t.u16(p+2); quint32 count=t.u32(p+4);
        int bytes=int(count)*typeSize(type);
        int valOff = bytes<=4 ? p+8 : int(t.u32(p+8));
        auto ascii=[&]{ return asciiVal(t,valOff,int(count)); };
        // A numeric value as a double, robust to how the writer typed it:
        // RATIONAL(5/10) = num/den at an offset; SHORT(3) count 2 = num/den inline (PIL quirk);
        // SHORT/LONG scalar = the integer itself.
        auto numeric=[&]()->double{
            if(type==5||type==10){ int o=int(t.u32(p+8)); quint32 nu=t.u32(o),de=t.u32(o+4); return de?double(nu)/de:0.0; }
            if(type==3 && count>=2){ quint32 nu=t.u16(p+8),de=t.u16(p+10); return de?double(nu)/de:double(nu); }
            if(type==3) return t.u16(p+8);
            if(type==4||type==9) return t.u32(p+8);
            return 0.0; };
        switch(tag){
            case 0x010F: out["Camera Make"]=ascii(); break;
            case 0x0110: out["Camera Model"]=ascii(); break;
            case 0x0131: out["Software"]=ascii(); break;
            case 0x0132: if(!out.contains("Date Taken")) out["Date Taken"]=ascii(); break;
            case 0x013B: out["Artist"]=ascii(); break;
            case 0x8298: out["Copyright"]=ascii(); break;
            case 0x0112: { int o=t.u16(valOff); static const char* on[]={"","Normal","Mirror H","Rotate 180","Mirror V","Mirror+90CW","Rotate 90CW","Mirror+90CCW","Rotate 90CCW"};
                           if(o>=1&&o<=8) out["Orientation"]=on[o]; } break;
            case 0x8769: exifPtr=t.u32(valOff); break;   // Exif sub-IFD pointer
            case 0x829A: { double e=numeric(); if(e>0) out["Exposure"]= e<1? QString("1/%1 s").arg(qRound(1.0/e)) : QString("%1 s").arg(e,0,'g',3); } break;
            case 0x829D: { double f=numeric(); if(f>0) out["Aperture"]=QString("f/%1").arg(f,0,'g',3); } break;
            case 0x8827: out["ISO"]=QString::number(qRound(numeric())); break;
            case 0x9003: out["Date Taken"]=ascii(); break;   // DateTimeOriginal (preferred)
            case 0x920A: { double fl=numeric(); if(fl>0) out["Focal Length"]=QString("%1 mm").arg(fl,0,'g',4); } break;
            case 0xA002: out["Width"]=QString::number(t.u32(valOff)); break;
            case 0xA003: out["Height"]=QString::number(t.u32(valOff)); break;
            default: break;
        }
    }
    return exifPtr;
}
} // namespace

QMap<QString,QString> readExifFromData(const QByteArray& jpeg){
    QMap<QString,QString> out;
    const uchar* p=reinterpret_cast<const uchar*>(jpeg.constData()); int n=jpeg.size();
    if(n<4 || p[0]!=0xFF || p[1]!=0xD8) return out;   // not a JPEG (SOI)
    int i=2;
    while(i+4<=n){
        if(p[i]!=0xFF) { ++i; continue; }
        int marker=p[i+1];
        if(marker==0xD9 || marker==0xDA) break;        // EOI / SOS — no more metadata
        int len=(p[i+2]<<8)|p[i+3];
        if(len<2 || i+2+len>n) break;
        if(marker==0xE1 && len>=8 && memcmp(p+i+4,"Exif\0\0",6)==0){
            Tiff t; t.d=p+i+10; t.n=len-8;             // TIFF block starts after "Exif\0\0"
            if(t.n>8){
                if(t.d[0]=='M'&&t.d[1]=='M') t.be=true; else if(t.d[0]=='I'&&t.d[1]=='I') t.be=false; else return out;
                quint32 ifd0=t.u32(4);
                quint32 exifPtr=parseIFD(t,int(ifd0),out);
                if(exifPtr) parseIFD(t,int(exifPtr),out);
            }
            return out;
        }
        i+=2+len;
    }
    return out;
}

QMap<QString,QString> readExif(const QString& jpegPath){
    QFile f(jpegPath); if(!f.open(QIODevice::ReadOnly)) return {};
    return readExifFromData(f.read(256*1024));   // EXIF lives in the first APP1 segment
}

} // namespace xara
