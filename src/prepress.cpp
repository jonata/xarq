// prepress.cpp — Module 3 importers/exporters. See prepress.h.
#include "prepress.h"
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QtMath>
#include <cstring>
#include <functional>
#include <zlib.h>

namespace xara {

// zlib deflate (zlib-wrapped, i.e. PDF /FlateDecode compatible). Returns {} on failure.
QByteArray deflateZlib(const QByteArray& src){
    if(src.isEmpty()) return {};
    z_stream zs; std::memset(&zs,0,sizeof(zs));
    if(deflateInit(&zs,Z_BEST_SPEED)!=Z_OK) return {};
    zs.next_in=(Bytef*)src.data(); zs.avail_in=(uInt)src.size();
    QByteArray out; char buf[32768]; int ret;
    do{ zs.next_out=(Bytef*)buf; zs.avail_out=sizeof(buf); ret=deflate(&zs,Z_FINISH);
        out.append(buf,int(sizeof(buf)-zs.avail_out)); } while(ret==Z_OK);
    deflateEnd(&zs); return (ret==Z_STREAM_END)? out : QByteArray();
}

void rgbToCmyk(const QColor& c, double& C, double& M, double& Y, double& K){
    double r=c.redF(), g=c.greenF(), b=c.blueF();
    K = 1.0 - qMax(r, qMax(g,b));
    if (K >= 0.9999) { C=M=Y=0; return; }
    C = (1.0-r-K)/(1.0-K); M = (1.0-g-K)/(1.0-K); Y = (1.0-b-K)/(1.0-K);
}
static QColor cmykToRgb(double c,double m,double y,double k){
    return QColor::fromRgbF(qBound(0.0,(1.0-c)*(1.0-k),1.0), qBound(0.0,(1.0-m)*(1.0-k),1.0), qBound(0.0,(1.0-y)*(1.0-k),1.0));
}

// Approximate CMYK soft-proof (no ICC — liblcms2 dev headers unavailable). Simulates how the colour
// would look printed on coated offset stock: our own rgbToCmyk separation, plus midtone dot-gain,
// a slightly-cream paper white and a gamut compression toward luminance (CMYK can't hit sRGB's most
// saturated primaries). Directionally faithful — the proof reads a touch darker and duller than screen,
// which is the real-world truth — but it is an approximation, not a colour-managed proof.
QColor softProofCmyk(const QColor& in){
    double C,M,Y,K; rgbToCmyk(in,C,M,Y,K);
    auto gain=[](double v){ return qBound(0.0, v + 0.12*v*(1.0-v), 1.0); };   // dot gain swells midtone dots
    C=gain(C); M=gain(M); Y=gain(Y);
    const double pr=252/255.0, pg=250/255.0, pb=244/255.0;                    // uncoated stock isn't pure white
    double r=pr*(1.0-C)*(1.0-K), g=pg*(1.0-M)*(1.0-K), b=pb*(1.0-Y)*(1.0-K);
    const double L=0.299*r+0.587*g+0.114*b, s=0.85;                           // pull saturated colours into gamut
    r=L+(r-L)*s; g=L+(g-L)*s; b=L+(b-L)*s;
    return QColor(qBound(0,int(r*255+0.5),255), qBound(0,int(g*255+0.5),255), qBound(0,int(b*255+0.5),255), in.alpha());
}

// Apply the soft-proof to every pixel of an ARGB image in place (leaves the alpha channel untouched).
void softProofImage(QImage& im){
    if (im.isNull()) return;
    const bool premul = im.format()==QImage::Format_ARGB32_Premultiplied;
    if (premul) im = im.convertToFormat(QImage::Format_ARGB32);               // un-premultiply so the proof sees true colour
    else if (im.format()!=QImage::Format_ARGB32) im = im.convertToFormat(QImage::Format_ARGB32);
    for (int y=0;y<im.height();++y){ QRgb* r=(QRgb*)im.scanLine(y);
        for (int x=0;x<im.width();++x){ const int a=qAlpha(r[x]); if(a==0) continue;
            const QColor p = softProofCmyk(QColor(qRed(r[x]),qGreen(r[x]),qBlue(r[x])));
            r[x]=qRgba(p.red(),p.green(),p.blue(),a); } }
    if (premul) im = im.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

// Flag every pixel whose colour lies OUTSIDE the CMYK gamut (the proof pulls it far from the original)
// a flat neutral grey — a view-only "gamut warning" so out-of-gamut art is visible before print.
void gamutWarnImage(QImage& im){
    if (im.isNull()) return;
    const bool premul = im.format()==QImage::Format_ARGB32_Premultiplied;
    if (im.format()!=QImage::Format_ARGB32) im = im.convertToFormat(QImage::Format_ARGB32);
    for (int y=0;y<im.height();++y){ QRgb* r=(QRgb*)im.scanLine(y);
        for (int x=0;x<im.width();++x){ const int a=qAlpha(r[x]); if(a==0) continue;
            const QColor in(qRed(r[x]),qGreen(r[x]),qBlue(r[x]));
            const QColor p = softProofCmyk(in);
            const int d = qAbs(in.red()-p.red())+qAbs(in.green()-p.green())+qAbs(in.blue()-p.blue());
            if (d>48) r[x]=qRgba(150,150,150,a); } }
    if (premul) im = im.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

// Total-area-coverage (ink limit): sum of the four CMYK channels, as a percentage 0..400.
int totalAreaCoverage(const QColor& c){
    double C,M,Y,K; rgbToCmyk(c, C,M,Y,K);
    return int((C+M+Y+K)*100.0 + 0.5);
}
// View-only prepress aid: paint every pixel whose ink coverage exceeds `limitPct` a warning colour, so a
// designer can see where the press's total-ink limit (typically ~300% coated, ~240% newsprint) is blown.
void tacWarnImage(QImage& im, int limitPct){
    if (im.isNull()) return;
    const bool premul = im.format()==QImage::Format_ARGB32_Premultiplied;
    if (im.format()!=QImage::Format_ARGB32) im = im.convertToFormat(QImage::Format_ARGB32);
    for (int y=0;y<im.height();++y){ QRgb* r=(QRgb*)im.scanLine(y);
        for (int x=0;x<im.width();++x){ const int a=qAlpha(r[x]); if(a==0) continue;
            if (totalAreaCoverage(QColor(qRed(r[x]),qGreen(r[x]),qBlue(r[x]))) > limitPct) r[x]=qRgba(0,220,120,a); } }
    if (premul) im = im.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

// ============================================================================
// ============================================================================
// Shared PostScript-family path interpreter. EPS (postfix names) and PDF (short
// operators) both drive it: they tokenise differently but map to the same ops.
// ============================================================================
namespace {
// Hard cap on shapes any single vector import may emit. A malformed PDF/EPS content stream (e.g. a
// FlateDecode body of millions of 're f' ops) must not push an unbounded number of heavyweight Shapes
// and OOM the app. Chosen far above any real document.
static constexpr int kMaxImportShapes = 500000;
// True only if every path point is finite — a crafted 'nan'/'inf' coordinate must not enter the model.
static bool pathAllFinite(const QPainterPath& p){
    const int n=p.elementCount();
    for(int i=0;i<n;++i){ const QPainterPath::Element e=p.elementAt(i);
        if(!qIsFinite(e.x)||!qIsFinite(e.y)) return false; }
    return true;
}
struct GState { QTransform ctm; QColor fill = Qt::black; QColor stroke = Qt::black; double lineW = 1.0; bool evenOdd=false; };

struct Interp {
    Document doc;
    QTransform base;                 // device(EPS/PDF) → doc space (y-flip etc.)
    GState gs;
    QVector<GState> stack;
    QPainterPath path;               // current path, in DEVICE space
    QPointF curUser{0,0};            // current point, USER space (for r* ops)
    QPointF startUser{0,0};

    QPointF dev(const QPointF& user){ return gs.ctm.map(user); }
    void moveTo(double x,double y){ curUser=startUser=QPointF(x,y); path.moveTo(dev(curUser)); }
    void lineTo(double x,double y){ curUser=QPointF(x,y); path.lineTo(dev(curUser)); }
    void curveTo(double x1,double y1,double x2,double y2,double x3,double y3){
        path.cubicTo(dev({x1,y1}), dev({x2,y2}), dev({x3,y3})); curUser=QPointF(x3,y3); }
    void rmoveTo(double dx,double dy){ moveTo(curUser.x()+dx, curUser.y()+dy); }
    void rlineTo(double dx,double dy){ lineTo(curUser.x()+dx, curUser.y()+dy); }
    void rcurveTo(double dx1,double dy1,double dx2,double dy2,double dx3,double dy3){
        double x=curUser.x(), y=curUser.y();
        curveTo(x+dx1,y+dy1, x+dx2,y+dy2, x+dx3,y+dy3); }
    void rectAbs(double x,double y,double w,double h){    // PDF 're'
        moveTo(x,y); lineTo(x+w,y); lineTo(x+w,y+h); lineTo(x,y+h); path.closeSubpath(); }
    void closeSub(){ path.closeSubpath(); curUser=startUser; }

    double ctmScale() const { double d=qAbs(gs.ctm.determinant()); return d>0? std::sqrt(d):1.0; }

    void emitPath(bool doFill, bool doStroke){
        if(path.isEmpty()){ path=QPainterPath(); return; }
        // Robustness: never emit past the cap (malformed streams can request millions of fills), and
        // drop paths with non-finite coordinates (crafted 'nan'/'inf' operands) so they can't enter the model.
        if(doc.shapes.size() >= kMaxImportShapes || !pathAllFinite(path)){ path=QPainterPath(); return; }
        Shape s; s.type=ShapeType::Path;
        s.path = base.map(path);
        s.fillRule = gs.evenOdd ? Qt::OddEvenFill : Qt::WindingFill;
        if(doFill){ s.fill.type=FillType::Solid; s.fill.color=gs.fill; } else s.fill.type=FillType::None;
        if(doStroke){ s.stroke.enabled=true; s.stroke.color=gs.stroke; s.stroke.width=qMax(0.2, gs.lineW*ctmScale()); }
        else s.stroke.enabled=false;
        if(doFill||doStroke) doc.shapes.push_back(s);
        path = QPainterPath();
    }
    void gsave(){ stack.push_back(gs); }
    void grestore(){ if(!stack.isEmpty()){ gs=stack.back(); stack.pop_back(); } }
};
} // namespace

// ---- EPS tokeniser + evaluator ---------------------------------------------
Document importEps(const QByteArray& data){
    Interp in;
    QRectF bbox(0,0,600,800);
    // %%BoundingBox (prefer the exact one; fall back to (atend) default).
    { QRegularExpression re("%%BoundingBox:\\s*([-0-9.]+)\\s+([-0-9.]+)\\s+([-0-9.]+)\\s+([-0-9.]+)");
      auto m=re.match(QString::fromLatin1(data));
      if(m.hasMatch()){ double a=m.captured(1).toDouble(),b=m.captured(2).toDouble(),c=m.captured(3).toDouble(),d=m.captured(4).toDouble();
          if(c>a && d>b) bbox=QRectF(a,b,c-a,d-b); } }
    in.base = QTransform(1,0, 0,-1, -bbox.left(), bbox.top()+bbox.height());   // EPS(y-up) → doc(y-down)
    in.doc.pageSize = QSizeF(bbox.width(), bbox.height());

    QVector<double> st;                       // operand stack
    auto pop=[&]()->double{ if(st.isEmpty())return 0; double v=st.back(); st.pop_back(); return v; };
    const char* p=data.constData(); const char* end=p+data.size();
    while(p<end){
        char ch=*p;
        if(ch=='%'){ while(p<end && *p!='\n' && *p!='\r') ++p; continue; }               // comment
        if(ch=='(' ){ int depth=1; ++p; while(p<end&&depth){ if(*p=='\\')++p; else if(*p=='(')++depth; else if(*p==')')--depth; ++p; } continue; } // string
        if(ch=='{'){ int depth=1; ++p; while(p<end&&depth){ if(*p=='{')++depth; else if(*p=='}')--depth; ++p; } continue; }   // proc: skip
        if(ch=='}'){ ++p; continue; }   // stray/unbalanced '}' at top level — skip it (else the token scanner below stalls, hanging forever)
        if(ch=='['||ch==']'){ ++p; continue; }
        if(isspace((unsigned char)ch)){ ++p; continue; }
        // token
        const char* s0=p; while(p<end && !isspace((unsigned char)*p) && *p!='%' && *p!='(' && *p!='{' && *p!='}' && *p!='[' && *p!=']') ++p;
        QByteArray tok(s0, int(p-s0));
        bool isNum=false; double num=tok.toDouble(&isNum);
        if(isNum){ st.push_back(num); continue; }
        const char* t=tok.constData();
        auto eq=[&](const char* n){ return std::strcmp(t,n)==0; };
        if(eq("moveto")){ double y=pop(),x=pop(); in.moveTo(x,y); }
        else if(eq("lineto")){ double y=pop(),x=pop(); in.lineTo(x,y); }
        else if(eq("rmoveto")){ double y=pop(),x=pop(); in.rmoveTo(x,y); }
        else if(eq("rlineto")){ double y=pop(),x=pop(); in.rlineTo(x,y); }
        else if(eq("curveto")){ double y3=pop(),x3=pop(),y2=pop(),x2=pop(),y1=pop(),x1=pop(); in.curveTo(x1,y1,x2,y2,x3,y3); }
        else if(eq("rcurveto")){ double y3=pop(),x3=pop(),y2=pop(),x2=pop(),y1=pop(),x1=pop(); in.rcurveTo(x1,y1,x2,y2,x3,y3); }
        else if(eq("closepath")){ in.closeSub(); }
        else if(eq("newpath")){ in.path=QPainterPath(); }
        else if(eq("fill")||eq("eofill")){ in.gs.evenOdd = eq("eofill"); in.emitPath(true,false); }
        else if(eq("stroke")){ in.emitPath(false,true); }
        else if(eq("gsave")){ in.gsave(); }
        else if(eq("grestore")){ in.grestore(); }
        else if(eq("translate")){ double y=pop(),x=pop(); in.gs.ctm.translate(x,y); }
        else if(eq("scale")){ double y=pop(),x=pop(); in.gs.ctm.scale(x,y); }
        else if(eq("rotate")){ double a=pop(); in.gs.ctm.rotate(a); }
        else if(eq("concat")){ double f=pop(),e=pop(),d=pop(),c=pop(),b=pop(),a=pop(); in.gs.ctm = QTransform(a,b,c,d,e,f) * in.gs.ctm; }
        else if(eq("setlinewidth")){ in.gs.lineW=pop(); }
        else if(eq("setgray")){ double g=pop(); QColor c=QColor::fromRgbF(g,g,g); in.gs.fill=in.gs.stroke=c; }
        else if(eq("setrgbcolor")){ double b=pop(),g=pop(),r=pop(); QColor c=QColor::fromRgbF(qBound(0.0,r,1.0),qBound(0.0,g,1.0),qBound(0.0,b,1.0)); in.gs.fill=in.gs.stroke=c; }
        else if(eq("sethsbcolor")){ double b=pop(),s=pop(),h=pop(); QColor c=QColor::fromHsvF(qBound(0.0,h,1.0),qBound(0.0,s,1.0),qBound(0.0,b,1.0)); in.gs.fill=in.gs.stroke=c; }
        else if(eq("setcmykcolor")){ double k=pop(),y=pop(),m=pop(),c=pop(); QColor col=cmykToRgb(c,m,y,k); in.gs.fill=in.gs.stroke=col; }
        // showpage/clip/eoclip/setmatrix/etc: ignored (operands already consumed as needed by later ops)
    }
    return in.doc;
}

// ---- PDF content-stream import ---------------------------------------------
static QByteArray inflateZlib(const QByteArray& src){
    if(src.isEmpty()) return {};
    z_stream zs; std::memset(&zs,0,sizeof(zs));
    if(inflateInit(&zs)!=Z_OK) return {};
    zs.next_in=(Bytef*)src.data(); zs.avail_in=(uInt)src.size();
    QByteArray out; char buf[16384]; int ret=Z_OK;
    const int kMaxOut=64*1024*1024;              // cap output: a decompression bomb otherwise exhausts memory
    do{ zs.next_out=(Bytef*)buf; zs.avail_out=sizeof(buf);
        ret=inflate(&zs,Z_NO_FLUSH);
        if(ret!=Z_OK && ret!=Z_STREAM_END){ inflateEnd(&zs); return {}; }
        out.append(buf, int(sizeof(buf)-zs.avail_out));
        if(out.size()>kMaxOut){ inflateEnd(&zs); return {}; }
    } while(ret==Z_OK);
    inflateEnd(&zs); return out;
}

// Decode a PDF image XObject (/Subtype/Image) → QImage. Handles DCTDecode (JPEG via Qt), and raw or
// FlateDecode 8-bit DeviceRGB / DeviceGray samples. Returns a null image for anything else.
static QImage decodePdfImage(const QByteArray& obj){
    const int sp=obj.indexOf("stream"); if(sp<0) return {};
    const QString ds=QString::fromLatin1(obj.left(sp));
    if(!ds.contains(QRegularExpression("/Subtype\\s*/Image"))) return {};
    auto intOf=[&](const char* key)->int{ QRegularExpression re(QString("/")+key+"\\s+(\\d+)"); auto m=re.match(ds); return m.hasMatch()? m.captured(1).toInt():0; };
    const int W=intOf("Width"), H=intOf("Height"); if(W<=0||H<=0||W>20000||H>20000) return {};
    int cs=sp+6; if(cs<obj.size()&&obj[cs]=='\r')++cs; if(cs<obj.size()&&obj[cs]=='\n')++cs;
    int e=obj.indexOf("endstream", cs); if(e<0) e=obj.size();
    const QByteArray raw=obj.mid(cs, e-cs);
    if(ds.contains("DCTDecode")){ QImage im; if(im.loadFromData(raw,"JPG")||im.loadFromData(raw)) return im; return {}; }
    const QByteArray data = ds.contains("FlateDecode")? inflateZlib(raw) : raw;
    if(data.isEmpty()) return {};
    int bpc=intOf("BitsPerComponent"); if(bpc==0) bpc=8; if(bpc!=8) return {};
    const bool gray = ds.contains("DeviceGray") || (!ds.contains("RGB"));
    if(!gray){ if(qint64(data.size()) < qint64(W)*H*3) return {};
        QImage im(W,H,QImage::Format_RGB888);
        for(int y=0;y<H;++y) std::memcpy(im.scanLine(y), data.constData()+qint64(y)*W*3, size_t(W)*3);
        return im.convertToFormat(QImage::Format_ARGB32); }
    if(qint64(data.size()) < qint64(W)*H) return {};
    QImage im(W,H,QImage::Format_Grayscale8);
    for(int y=0;y<H;++y) std::memcpy(im.scanLine(y), data.constData()+qint64(y)*W, size_t(W));
    return im.convertToFormat(QImage::Format_ARGB32);
}

static void interpretPdfContent(Interp& in, const QByteArray& c, const std::function<QImage(const QString&)>& resolveXObject = {}){
    QVector<double> st; auto pop=[&]()->double{ if(st.isEmpty())return 0.0; double v=st.back(); st.pop_back(); return v; };
    bool inText=false; QByteArray lastName;   // most-recent /Name operand (for the Do operator)
    const char* p=c.constData(); const char* end=p+c.size();
    auto clampf=[](double v){ return qBound(0.0,v,1.0); };
    // --- text state (BT..ET): show Tj/TJ/'/" strings as editable Text shapes -------------------
    QTransform tm, tlm;          // text matrix + text line matrix (PDF text-space → user-space)
    double tfs=0.0, tl=0.0;      // font size (Tf) and leading (TL)
    QByteArray curStr;           // decoded bytes accumulated for the pending text-show operator
    auto emitText=[&](){
        QString txt=QString::fromLatin1(curStr); curStr.clear();
        if(txt.trimmed().isEmpty()) return;
        QTransform T = tm * in.gs.ctm * in.base;             // text-space (0,0) → doc space (base has unit scale)
        QPointF O = T.map(QPointF(0,0));
        double dsc=qAbs((tm*in.gs.ctm).determinant()); double sc = dsc>0? std::sqrt(dsc):1.0;
        double sizeDoc=(tfs>0?tfs:12.0)*sc; if(sizeDoc<1.0) sizeDoc=1.0;
        Shape t; t.type=ShapeType::Text; t.text=txt;
        QFont f("DejaVu Sans"); f.setPixelSize(qMax(1,int(sizeDoc+0.5))); t.font=f;
        t.fill.type=FillType::Solid; t.fill.color=in.gs.fill.isValid()?in.gs.fill:QColor(Qt::black); t.stroke.enabled=false;
        t.transform=QTransform::fromTranslate(O.x(), O.y()-0.8*sizeDoc);   // baseline ≈ 0.8·size below the box top
        in.doc.shapes.push_back(t);
        tm.translate(txt.length()*0.5*(tfs>0?tfs:12.0), 0.0);              // approx advance so successive Tj don't fully overlap
    };
    while(p<end){
        char ch=*p;
        if(isspace((unsigned char)ch)){ ++p; continue; }
        if(ch=='%'){ while(p<end&&*p!='\n'&&*p!='\r')++p; continue; }
        if(ch=='('){ ++p; QByteArray inner; int d=1;                                    // literal string operand — decode when showing text
            while(p<end&&d){ char cc=*p;
                if(cc=='\\'){ ++p; if(p>=end)break; char e=*p;
                    switch(e){ case 'n':inner+='\n';break; case 'r':inner+='\r';break; case 't':inner+='\t';break;
                               case 'b':inner+='\b';break; case 'f':inner+='\f';break;
                               case '(':inner+='(';break; case ')':inner+=')';break; case '\\':inner+='\\';break;
                               default: if(e>='0'&&e<='7'){ int v=e-'0'; for(int k=0;k<2&&p+1<end&&p[1]>='0'&&p[1]<='7';++k){ ++p; v=v*8+(*p-'0'); } inner+=char(v&0xff); }
                                        else inner+=e; }
                    ++p; continue; }
                if(cc=='('){ ++d; inner+=cc; ++p; continue; }
                if(cc==')'){ --d; if(!d){ ++p; break; } inner+=cc; ++p; continue; }
                inner+=cc; ++p; }
            if(inText) curStr+=inner; continue; }
        if(ch=='<'){ if(p+1<end&&p[1]=='<'){ int d=1; p+=2; while(p<end&&d){ if(p+1<end&&p[0]=='<'&&p[1]=='<'){d++;p+=2;} else if(p+1<end&&p[0]=='>'&&p[1]=='>'){d--;p+=2;} else ++p; } }
                     else { ++p; QByteArray hex; while(p<end&&*p!='>'){ if(isxdigit((unsigned char)*p)) hex+=*p; ++p; } if(p<end)++p;   // hex string
                            if(inText){ if(hex.size()&1) hex+='0'; curStr+=QByteArray::fromHex(hex); } } continue; }
        if(ch=='['){ if(!inText){ while(p<end&&*p!=']')++p; if(p<end)++p; }   // outside text: skip arrays (dash patterns etc.)
                     else ++p;                                               // inside text (TJ): let inner (…)/<…> strings accumulate into curStr
                     continue; }
        if(ch==']'){ ++p; continue; }                                        // TJ array close
        if(ch=='/'){ const char* s0=++p; while(p<end && !isspace((unsigned char)*p) && *p!='/'&&*p!='['&&*p!='('&&*p!='<') ++p; lastName=QByteArray(s0,int(p-s0)); continue; }
        const char* s0=p; while(p<end && !isspace((unsigned char)*p) && *p!='/'&&*p!='['&&*p!=']'&&*p!='('&&*p!='<'&&*p!='%') ++p;
        QByteArray tok(s0,int(p-s0)); bool isNum=false; double num=tok.toDouble(&isNum);
        if(isNum){ st.push_back(num); continue; }
        const char* t=tok.constData(); auto eq=[&](const char* n){ return std::strcmp(t,n)==0; };
        if(inText){                                              // BT..ET text object: emit Tj/TJ/'/" strings as editable Text shapes
            if(eq("ET")) inText=false;
            else if(eq("Tf")){ tfs=pop(); }
            else if(eq("Td")){ double ty=pop(),tx=pop(); tlm.translate(tx,ty); tm=tlm; }
            else if(eq("TD")){ double ty=pop(),tx=pop(); tl=-ty; tlm.translate(tx,ty); tm=tlm; }
            else if(eq("Tm")){ double f=pop(),e=pop(),d=pop(),cc=pop(),b=pop(),a=pop(); tlm=QTransform(a,b,cc,d,e,f); tm=tlm; }
            else if(eq("TL")){ tl=pop(); }
            else if(eq("T*")){ tlm.translate(0,-tl); tm=tlm; }
            else if(eq("Tj")||eq("TJ")){ emitText(); }
            else if(eq("'")){ tlm.translate(0,-tl); tm=tlm; emitText(); }
            else if(eq("\"")){ tlm.translate(0,-tl); tm=tlm; emitText(); }   // aw ac string " (spacing operands ignored)
            st.clear(); continue;
        }
        if(eq("BT")){ inText=true; tm=tlm=QTransform(); curStr.clear(); }
        else if(eq("m")){ double y=pop(),x=pop(); in.moveTo(x,y); }
        else if(eq("l")){ double y=pop(),x=pop(); in.lineTo(x,y); }
        else if(eq("c")){ double y3=pop(),x3=pop(),y2=pop(),x2=pop(),y1=pop(),x1=pop(); in.curveTo(x1,y1,x2,y2,x3,y3); }
        else if(eq("v")){ double y3=pop(),x3=pop(),y2=pop(),x2=pop(); in.curveTo(in.curUser.x(),in.curUser.y(),x2,y2,x3,y3); }
        else if(eq("y")){ double y3=pop(),x3=pop(),y1=pop(),x1=pop(); in.curveTo(x1,y1,x3,y3,x3,y3); }
        else if(eq("re")){ double h=pop(),w=pop(),y=pop(),x=pop(); in.rectAbs(x,y,w,h); }
        else if(eq("h")){ in.closeSub(); }
        else if(eq("n")){ in.path=QPainterPath(); }
        else if(eq("W")||eq("W*")){ /* clip — ignore path paint */ }
        else if(eq("f")||eq("F")||eq("f*")){ in.gs.evenOdd=eq("f*"); in.emitPath(true,false); }
        else if(eq("S")){ in.emitPath(false,true); }
        else if(eq("s")){ in.closeSub(); in.emitPath(false,true); }
        else if(eq("B")||eq("B*")){ in.gs.evenOdd=eq("B*"); in.emitPath(true,true); }
        else if(eq("b")||eq("b*")){ in.gs.evenOdd=eq("b*"); in.closeSub(); in.emitPath(true,true); }
        else if(eq("q")){ in.gsave(); }
        else if(eq("Q")){ in.grestore(); }
        else if(eq("cm")){ double f=pop(),e=pop(),d=pop(),cc=pop(),b=pop(),a=pop(); in.gs.ctm=QTransform(a,b,cc,d,e,f)*in.gs.ctm; }
        else if(eq("w")){ in.gs.lineW=pop(); }
        else if(eq("rg")){ double b=pop(),g=pop(),r=pop(); in.gs.fill=QColor::fromRgbF(clampf(r),clampf(g),clampf(b)); }
        else if(eq("RG")){ double b=pop(),g=pop(),r=pop(); in.gs.stroke=QColor::fromRgbF(clampf(r),clampf(g),clampf(b)); }
        else if(eq("g")){ double v=clampf(pop()); in.gs.fill=QColor::fromRgbF(v,v,v); }
        else if(eq("G")){ double v=clampf(pop()); in.gs.stroke=QColor::fromRgbF(v,v,v); }
        else if(eq("k")){ double kk=pop(),y=pop(),m=pop(),cc=pop(); in.gs.fill=cmykToRgb(cc,m,y,kk); }
        else if(eq("K")){ double kk=pop(),y=pop(),m=pop(),cc=pop(); in.gs.stroke=cmykToRgb(cc,m,y,kk); }
        else if(eq("sc")||eq("scn")){ int nc=st.size(); if(nc>=4){ double kk=pop(),y=pop(),m=pop(),cc=pop(); in.gs.fill=cmykToRgb(cc,m,y,kk); }
                     else if(nc==3){ double b=pop(),g=pop(),r=pop(); in.gs.fill=QColor::fromRgbF(clampf(r),clampf(g),clampf(b)); }
                     else if(nc>=1){ double v=clampf(pop()); in.gs.fill=QColor::fromRgbF(v,v,v); } }
        else if(eq("SC")||eq("SCN")){ int nc=st.size(); if(nc>=4){ double kk=pop(),y=pop(),m=pop(),cc=pop(); in.gs.stroke=cmykToRgb(cc,m,y,kk); }
                     else if(nc==3){ double b=pop(),g=pop(),r=pop(); in.gs.stroke=QColor::fromRgbF(clampf(r),clampf(g),clampf(b)); }
                     else if(nc>=1){ double v=clampf(pop()); in.gs.stroke=QColor::fromRgbF(v,v,v); } }
        else if(eq("Do") && resolveXObject){                              // draw an image XObject in the unit square via the CTM
            const QImage img=resolveXObject(QString::fromLatin1(lastName));
            if(!img.isNull() && img.width()>0 && img.height()>0){
                const QTransform& M=in.gs.ctm;                            // unit square → user; then in.base → doc
                QPolygonF dst; dst<<in.base.map(M.map(QPointF(0,1)))<<in.base.map(M.map(QPointF(1,1)))
                                  <<in.base.map(M.map(QPointF(1,0)))<<in.base.map(M.map(QPointF(0,0)));   // TL,TR,BR,BL
                const QRectF bb=dst.boundingRect();
                if(bb.width()>=0.5 && bb.height()>=0.5){
                    const int iw=img.width(), ih=img.height();
                    Shape s; s.type=ShapeType::Rect; s.name=QStringLiteral("Image"); s.rect=QRectF(0,0,iw,ih);
                    s.fill.type=FillType::Bitmap; s.fill.image=img; s.fill.origImage=img; s.fill.tiled=false;
                    s.fill.p0=QPointF(0,ih); s.fill.p1=QPointF(iw,ih); s.fill.p2=QPointF(0,0); s.stroke.enabled=false;  // same convention as the PSD importer
                    QTransform T; T.translate(bb.left(),bb.top()); T.scale(bb.width()/iw, bb.height()/ih); s.transform=T;
                    in.doc.shapes.push_back(s); } } }
        st.clear();   // any operator fully consumes its operands
    }
}

// Shift a parsed shape (and its children) down the page stack by dy doc units.
static void translatePdfShape(Shape& s, double dy){
    if(s.type==ShapeType::Path) s.path.translate(0,dy);
    else s.transform = s.transform * QTransform::fromTranslate(0,dy);
    for(Shape& c: s.children) translatePdfShape(c,dy);
}

// Parse a PDF by walking the /Type/Pages → /Type/Page tree so EVERY page imports (each onto its
// own document page). Resolves each page's /Contents (single ref or array), inflates + interprets
// them. Falls back to the classic first-drawing-stream scan when there is no usable page tree.
Document importPdf(const QByteArray& data){
    // Object table: number → body bytes between 'obj' and 'endobj'. LAST definition wins (so an
    // incremental update's re-defined object overrides the stale original), and 'N G obj' headers that
    // fall inside a binary stream body are ignored (so stream bytes can't forge a fake object).
    QHash<int,QByteArray> objs;
    { const int n=data.size();
      const int kMaxObjs=500000;                                                  // reject pathological object counts
      QVector<QPair<int,int>> streamSpans;                                        // [stream keyword .. endstream end), position-ordered
      { int q=0; while(q<n){ int sPos=data.indexOf("stream", q); if(sPos<0) break;
          if(sPos>=3 && data.mid(sPos-3,3)=="end"){ q=sPos+6; continue; }          // this is 'endstream', not a stream start
          int e=data.indexOf("endstream", sPos+6); int end=(e<0)? n : e+9;
          streamSpans.push_back({sPos, end}); q=end; } }
      // Spans are ascending + non-overlapping → binary search membership (was a linear scan per match: O(matches*spans)).
      auto inStream=[&](int pos)->bool{ int lo=0,hi=streamSpans.size();
          while(lo<hi){ int mid=(lo+hi)/2; if(streamSpans[mid].first<=pos) lo=mid+1; else hi=mid; }
          return lo>0 && pos < streamSpans[lo-1].second; };
      QRegularExpression re("\\b(\\d+)\\s+\\d+\\s+obj\\b"); const QString s=QString::fromLatin1(data);
      // Collect header positions first (capped), then bound each object body by the NEXT header. A missing
      // 'endobj' must not make every object deep-copy the whole tail-to-EOF (O(N^2) memory bomb from a tiny file).
      struct Hdr{ int num; int bodyStart; int matchStart; };
      QVector<Hdr> hdrs;
      auto it=re.globalMatch(s);
      while(it.hasNext() && hdrs.size()<kMaxObjs){ auto m=it.next(); if(inStream(m.capturedStart(0))) continue;
          hdrs.push_back({ m.captured(1).toInt(), int(m.capturedEnd(0)), int(m.capturedStart(0)) }); }
      for(int k=0;k<hdrs.size();++k){ const int bs=hdrs[k].bodyStart;
          const int limit=(k+1<hdrs.size())? hdrs[k+1].matchStart : n;
          int e=data.indexOf("endobj", bs); if(e<0 || e>limit) e=limit;           // clamp to the next header when 'endobj' is absent/misnested
          objs.insert(hdrs[k].num, data.mid(bs, qMax(0, e-bs))); } }               // insert (not insert-if-absent) → last def wins

    QRegularExpression refRe("(\\d+)\\s+\\d+\\s+R");
    QRegularExpression pagesRe("/Type\\s*/Pages\\b"), pageRe("/Type\\s*/Page\\b"), catRe("/Type\\s*/Catalog\\b");
    auto refIn=[&](const QByteArray& dict, const char* key)->int{
        QRegularExpression re(QString("/")+key+"\\s+(\\d+)\\s+\\d+\\s+R");
        auto m=re.match(QString::fromLatin1(dict)); return m.hasMatch()? m.captured(1).toInt() : -1; };
    auto mediaBoxIn=[&](const QByteArray& dict, QRectF& out)->bool{
        QRegularExpression re("/MediaBox\\s*\\[\\s*([-0-9.]+)\\s+([-0-9.]+)\\s+([-0-9.]+)\\s+([-0-9.]+)");
        auto m=re.match(QString::fromLatin1(dict)); if(!m.hasMatch()) return false;
        double a=m.captured(1).toDouble(),b=m.captured(2).toDouble(),c=m.captured(3).toDouble(),d=m.captured(4).toDouble();
        if(c>a&&d>b){ out=QRectF(a,b,c-a,d-b); return true; } return false; };
    auto streamOf=[&](int num)->QByteArray{
        if(!objs.contains(num)) return {}; const QByteArray& b=objs[num];
        int s=b.indexOf("stream"); if(s<0) return {}; int cs=s+6;
        if(cs<b.size()&&b[cs]=='\r')++cs; if(cs<b.size()&&b[cs]=='\n')++cs;
        int e=b.indexOf("endstream", cs); if(e<0) e=b.size();
        QByteArray raw=b.mid(cs,e-cs); QByteArray inf=inflateZlib(raw); return inf.isEmpty()? raw : inf; };
    auto contentsOf=[&](const QByteArray& pageDict)->QByteArray{
        QRegularExpression arr("/Contents\\s*\\[([^\\]]*)\\]"); auto am=arr.match(QString::fromLatin1(pageDict));
        if(am.hasMatch()){ QByteArray out; auto it=refRe.globalMatch(am.captured(1));
            while(it.hasNext()){ out+=streamOf(it.next().captured(1).toInt()); out+="\n"; } return out; }
        int one=refIn(pageDict,"Contents"); return one>=0? streamOf(one) : QByteArray(); };
    // Build a page's XObject name→object-number map from its /Resources /XObject dict (inline or via ref).
    auto xobjectsOf=[&](const QByteArray& pageDict)->QMap<QString,int>{
        QMap<QString,int> map;
        QByteArray res; int rr=refIn(pageDict,"Resources"); if(rr>=0 && objs.contains(rr)) res=objs[rr]; else res=pageDict;
        int xi=res.indexOf("/XObject"); if(xi<0) return map;
        int b=res.indexOf("<<", xi); if(b<0) return map;
        int depth=0, i=b, endp=-1;                       // balanced-scan the XObject sub-dictionary
        while(i+1<res.size()){ if(res[i]=='<'&&res[i+1]=='<'){depth++;i+=2;} else if(res[i]=='>'&&res[i+1]=='>'){depth--;i+=2; if(depth==0){endp=i;break;}} else ++i; }
        if(endp<=b) return map;
        QRegularExpression pr("/([^\\s/<>\\[\\]]+)\\s+(\\d+)\\s+\\d+\\s+R"); auto it=pr.globalMatch(QString::fromLatin1(res.mid(b,endp-b)));
        while(it.hasNext()){ auto m=it.next(); map.insert(m.captured(1), m.captured(2).toInt()); }
        return map; };

    // Ordered page dicts + their (possibly inherited) MediaBox, via a DFS of the Pages tree.
    QVector<QPair<QByteArray,QRectF>> pages;
    const QRectF deflt(0,0,612,792);
    int catalog=-1; { QList<int> ck=objs.keys(); std::sort(ck.begin(),ck.end());       // lowest object number → deterministic (QHash order is not)
        for(int k: ck) if(QString::fromLatin1(objs[k]).contains(catRe)){ catalog=k; break; } }
    QList<QPair<int,QRectF>> work;
    if(catalog>=0){ int rp=refIn(objs[catalog],"Pages"); if(rp>=0) work.append({rp,deflt}); }
    QSet<int> seen; int guard=0;
    while(!work.isEmpty() && guard++<5000){
        auto pr=work.takeFirst(); int num=pr.first; QRectF inh=pr.second;
        if(seen.contains(num)||!objs.contains(num)) continue; seen.insert(num);
        const QByteArray d=objs[num]; const QString ds=QString::fromLatin1(d);
        QRectF box=inh; mediaBoxIn(d,box);
        if(ds.contains(pagesRe)){
            QRegularExpression kids("/Kids\\s*\\[([^\\]]*)\\]"); auto km=kids.match(ds);
            if(km.hasMatch()){ QVector<int> kl; auto it=refRe.globalMatch(km.captured(1));
                while(it.hasNext()) kl.push_back(it.next().captured(1).toInt());
                for(int i=kl.size()-1;i>=0;--i) work.prepend({kl[i],box}); } }   // DFS pre-order
        else if(ds.contains(pageRe)) pages.push_back({d,box});
    }
    if(pages.isEmpty()){   // no catalog/tree: take any /Type/Page objects in ascending number order
        QList<int> keys=objs.keys(); std::sort(keys.begin(),keys.end());
        for(int k: keys){ const QString ds=QString::fromLatin1(objs[k]);
            if(ds.contains(pageRe) && !ds.contains(pagesRe)){ QRectF box=deflt; mediaBoxIn(objs[k],box); pages.push_back({objs[k],box}); } } }

    if(!pages.isEmpty()){
        Document doc; const int N=qMin(pages.size(), 500);
        const QRectF first=pages[0].second; doc.pageSize=QSizeF(first.width(),first.height());
        // Stack pages by the CUMULATIVE sum of their own heights (equals k*stride when uniform, but
        // pages of differing MediaBox heights no longer overlap). Note: the model stores one pageSize,
        // so pageRect() still uses page 0's size — mixed-size PDFs page approximately.
        double yOff=0;
        for(int k=0;k<N;++k){ const QByteArray pd=pages[k].first; const QRectF box=pages[k].second;
            const QByteArray content=contentsOf(pd);
            if(!content.isEmpty()){
                Interp in; in.base=QTransform(1,0,0,-1,-box.left(), box.top()+box.height());
                const QMap<QString,int> xo=xobjectsOf(pd);
                interpretPdfContent(in, content, [&](const QString& nm)->QImage{
                    auto it=xo.find(nm); if(it==xo.end()||!objs.contains(*it)) return {}; return decodePdfImage(objs[*it]); });
                for(Shape& s: in.doc.shapes){ translatePdfShape(s, yOff); doc.shapes.push_back(s); } }
            yOff += box.height()+doc.pageGap; }
        doc.pageCount=qMax(1,N);
        if(!doc.shapes.isEmpty()) return doc;
    }

    // Fallback: classic single-page scan (first content stream that carries drawing operators).
    Interp in;
    QRectF box(0,0,612,792);
    { QRegularExpression re("/MediaBox\\s*\\[\\s*([-0-9.]+)\\s+([-0-9.]+)\\s+([-0-9.]+)\\s+([-0-9.]+)");
      auto m=re.match(QString::fromLatin1(data.left(qMin(data.size(),20000))));
      if(m.hasMatch()){ double a=m.captured(1).toDouble(),b=m.captured(2).toDouble(),c=m.captured(3).toDouble(),d=m.captured(4).toDouble();
          if(c>a && d>b) box=QRectF(a,b,c-a,d-b); } }
    in.base=QTransform(1,0, 0,-1, -box.left(), box.top()+box.height());
    in.doc.pageSize=QSizeF(box.width(),box.height());
    int idx=0;
    while(true){
        int s=data.indexOf("stream", idx); if(s<0) break;
        int cs=s+6; if(cs<data.size()&&data[cs]=='\r')++cs; if(cs<data.size()&&data[cs]=='\n')++cs;
        int e=data.indexOf("endstream", cs); if(e<0) break;
        QByteArray raw=data.mid(cs, e-cs); idx=e+9;
        QByteArray content=inflateZlib(raw); if(content.isEmpty()) content=raw;
        if(content.contains(" re")||content.contains(" m\n")||content.contains(" l\n")||content.contains(" cm")
           ||content.contains(" c\n")||content.contains(" f")||content.contains(" S")||content.contains("BT")){
            interpretPdfContent(in, content);
            if(!in.doc.shapes.isEmpty()) break;
        }
    }
    return in.doc;
}

// Modern .ai files are PDF-compatible; route to the PDF interpreter when a %PDF header exists.
Document importAi(const QByteArray& data){
    const int p=data.indexOf("%PDF");
    if(p>=0 && p<65536){ Document d=importPdf(data.mid(p)); if(!d.shapes.isEmpty()) return d; }
    return importEps(data);   // classic Illustrator (PostScript)
}

// AutoCAD ASCII DXF → stroked Path shapes from the ENTITIES section.
// ---- raster-only PostScript written by ImageMagick's "DisplayImage" prologue ----------------------
// Such a file carries no drawing operators at all: after %%EndProlog it calls DisplayImage and then
// feeds it a fixed run of parameter lines followed by the pixels as ASCII hex. Rasterising a general
// PostScript image would need an interpreter, but this one shape is regular enough to read directly,
// and it is what `convert x.png x.eps` (and .ps/.eps2/.eps3/.epsf/.epi) produces.
//   DisplayImage / "x y" / "x y" / pointsize / "columns rows" / class / compression / <hex data>
// class 0 = DirectClass (RGB triples). compression 0 = raw triples, 1 = packets of RGB + a repeat
// count byte (the run covers count+1 pixels).
// ---- raster-only PDF: the page content is one image XObject -------------------------------------
// importPdf reads content-stream vectors; a PDF whose page is just a photograph has none. Rather
// than a full PDF engine, find an image XObject, decode the stream through the filters we already
// have, and infer the component count from the payload size (the ColorSpace is usually an indirect
// reference we would otherwise have to resolve).
QImage pdfRasterImage(const QByteArray& data){
    int scan = 0, guard = 0;
    while(guard++ < 256){
        int i = data.indexOf("/Subtype", scan);
        if(i < 0) break;
        const int imgTok = data.indexOf("/Image", i);
        if(imgTok < 0 || imgTok > i + 12){ scan = i + 8; continue; }
        const int dictStart = data.lastIndexOf("<<", i);
        const int strTok    = data.indexOf("stream", imgTok);
        if(dictStart < 0 || strTok < 0){ scan = i + 8; continue; }
        const QByteArray dict = data.mid(dictStart, strTok - dictStart);
        scan = strTok;
        auto intOf=[&](const char* key)->int{
            const int k = dict.indexOf(key); if(k < 0) return -1;
            int j = k + int(qstrlen(key));
            while(j < dict.size() && (dict[j]==' '||dict[j]=='\t')) ++j;
            int v = 0; bool any=false;
            while(j < dict.size() && isdigit(static_cast<unsigned char>(dict[j]))){ v = v*10 + (dict[j]-'0'); ++j; any=true;
                if(v > 100000) return -1; }
            return any ? v : -1; };
        const int W = intOf("/Width"), H = intOf("/Height"), bpc = intOf("/BitsPerComponent");
        if(W <= 0 || H <= 0 || bpc != 8 || qint64(W)*H > 100000000ll) continue;
        // Stream payload: skip the EOL after the `stream` keyword.
        int sp = strTok + 6;
        if(sp < data.size() && data[sp]=='\r') ++sp;
        if(sp < data.size() && data[sp]=='\n') ++sp;
        int ep = data.indexOf("endstream", sp);
        if(ep < 0) ep = data.size();
        const QByteArray raw = data.mid(sp, ep - sp);
        if(raw.isEmpty()) continue;
        const bool flate = dict.contains("FlateDecode");
        const bool dct   = dict.contains("DCTDecode");
        if(dct){                                            // the stream is a JPEG as-is
            QImage j; if(j.loadFromData(raw, "JPEG") && !j.isNull()) return j;
            continue;
        }
        const QByteArray px = flate ? inflateZlib(raw) : raw;
        const qint64 need = qint64(W)*H;
        int comps = 0;
        if(px.size() >= need*3 && px.size() < need*4) comps = 3;
        else if(px.size() >= need*4) comps = 4;             // CMYK
        else if(px.size() >= need)   comps = 1;
        if(!comps) continue;
        QImage img(W, H, QImage::Format_ARGB32);
        if(img.isNull()) continue;
        for(qint64 k = 0; k < need; ++k){
            const unsigned char* s2 = reinterpret_cast<const unsigned char*>(px.constData()) + k*comps;
            QRgb v;
            if(comps == 1)      v = qRgb(s2[0], s2[0], s2[0]);
            else if(comps == 3) v = qRgb(s2[0], s2[1], s2[2]);
            else { const int c=s2[0], m=s2[1], y2=s2[2], kk=s2[3];   // CMYK, additive-inverted
                   v = qRgb((255-c)*(255-kk)/255, (255-m)*(255-kk)/255, (255-y2)*(255-kk)/255); }
            reinterpret_cast<QRgb*>(img.scanLine(int(k / W)))[k % W] = v;
        }
        return img;
    }
    return QImage();
}

QImage psDisplayImageRaster(const QByteArray& data){
    const int di = data.indexOf("\nDisplayImage");
    if(di < 0) return QImage();
    // The parameter block differs by PostScript level (Level 3 interleaves `false` tokens), but in
    // every variant the THIRD "<int> <int>" line is the pixel size, so key off that rather than a
    // fixed line count. Everything after the last parameter line is the payload, whose encoding is
    // then sniffed: zlib (Level 3 Flate), ASCII hex (Level 1) or binary RunLength (Level 2).
    int W=0, H=0, pairs=0, pos=di+1, guard=0;
    while(pos < data.size() && guard++ < 24){
        const int nl = data.indexOf('\n', pos);
        // A binary payload may contain no newline at all; that simply means the parameters have
        // ended and everything from here on is data.
        if(nl < 0) break;
        const QByteArray line = data.mid(pos, nl-pos).trimmed();
        const int next = nl + 1;
        if(line.isEmpty() || line == "DisplayImage" || line.startsWith('%')
           || line == "false" || line == "true"){ pos = next; continue; }
        const QList<QByteArray> tk = line.split(' ');
        bool allNum = !tk.isEmpty();
        for(const QByteArray& t : tk){ if(t.isEmpty()) continue;
            for(char c : t) if(!isdigit(static_cast<unsigned char>(c))) { allNum=false; break; }
            if(!allNum) break; }
        if(!allNum) break;                                  // first non-parameter line: payload starts here
        if(tk.size() >= 2){
            bool a1=false,b1=false; const int a=tk[0].toInt(&a1), b=tk[1].toInt(&b1);
            if(a1&&b1){ ++pairs; if(pairs==3){ W=a; H=b; } }
        }
        pos = next;
        if(pairs>=3 && guard>=12) break;
    }
    if(W <= 0 || H <= 0 || W > 20000 || H > 20000 || qint64(W)*H > 100000000ll) return QImage();
    auto makeImage=[&](const QByteArray& rgb)->QImage{
        if(rgb.size() < 3) return QImage();
        QImage img(W, H, QImage::Format_ARGB32);
        if(img.isNull()) return QImage();
        img.fill(Qt::white);
        const qint64 tot = qint64(W)*H;
        for(qint64 k = 0; k < tot && (k*3+2) < rgb.size(); ++k)
            reinterpret_cast<QRgb*>(img.scanLine(int(k / W)))[k % W] =
                qRgb(static_cast<unsigned char>(rgb[k*3]), static_cast<unsigned char>(rgb[k*3+1]),
                     static_cast<unsigned char>(rgb[k*3+2]));
        return img;
    };
    // (a) Level 3: Flate-compressed binary RGB.
    for(int z = pos; z < qMin(data.size()-1, pos+96); ++z){
        if(static_cast<unsigned char>(data[z]) != 0x78) continue;
        const unsigned char z1 = static_cast<unsigned char>(data[z+1]);
        if(z1!=0xDA && z1!=0x9C && z1!=0x01) continue;
        const QImage img = makeImage(inflateZlib(data.mid(z)));
        if(!img.isNull()) return img;
        break;
    }
    // (b) Level 1: ASCII hex, either raw RGB triples or RGB + a repeat-count byte.
    {   QByteArray hex; hex.reserve(data.size()-pos);
        for(int i = pos; i < data.size(); ++i){
            const char c = data[i];
            if(isxdigit(static_cast<unsigned char>(c))) hex.append(c);
            else if(c=='\n'||c=='\r'||c==' '||c=='\t') continue;
            else break;
        }
        if(hex.size() >= 6){
            auto hv=[](char c)->int{ return (c<='9') ? c-'0' : ((c|0x20)-'a'+10); };
            for(int mode = 0; mode < 2; ++mode){          // 0 = raw triples, 1 = RGB + count
                const int per = mode? 8 : 6;
                QByteArray rgb; rgb.reserve(qint64(W)*H*3);
                int hp = 0; qint64 px = 0; const qint64 tot = qint64(W)*H;
                while(px < tot && hp + per <= hex.size()){
                    const char r=char(hv(hex[hp])*16+hv(hex[hp+1]));
                    const char g=char(hv(hex[hp+2])*16+hv(hex[hp+3]));
                    const char b=char(hv(hex[hp+4])*16+hv(hex[hp+5]));
                    hp += 6;
                    qint64 run = 1;
                    if(mode){ run = hv(hex[hp])*16 + hv(hex[hp+1]) + 1; hp += 2; }
                    for(qint64 k=0;k<run && px<tot;++k,++px){ rgb.append(r); rgb.append(g); rgb.append(b); }
                }
                if(px == tot) return makeImage(rgb);           // only accept a complete image
            }
        }
    }
    // (c) Level 2: binary RGB through a RunLengthDecode filter.
    {   QByteArray rgb; rgb.reserve(qint64(W)*H*3);
        int i = pos;
        while(i < data.size() && rgb.size() < qint64(W)*H*3){
            const int nb = static_cast<unsigned char>(data[i++]);
            if(nb == 128) break;
            if(nb < 128){ const int cnt = nb+1; if(i+cnt > data.size()) break;
                rgb.append(data.constData()+i, cnt); i += cnt; }
            else { if(i >= data.size()) break; const char v = data[i++]; rgb.append(QByteArray(257-nb, v)); }
        }
        if(rgb.size() >= qint64(W)*H*3) return makeImage(rgb);
    }
    return QImage();
}

Document importDxf(const QByteArray& data){
    Document doc;
    const QStringList lines=QString::fromLatin1(data).split(QRegularExpression("\\r\\n|\\r|\\n"));
    struct Pair{ int code; QString val; };
    QVector<Pair> toks;
    for(int i=0;i+1<lines.size();i+=2){ bool ok; const int code=lines[i].trimmed().toInt(&ok); if(!ok) continue; toks.push_back({code, lines[i+1].trimmed()}); }
    int start=-1, end=toks.size();
    for(int i=0;i<toks.size();++i) if(toks[i].code==2 && toks[i].val=="ENTITIES"){ start=i+1; break; }
    if(start<0) return doc;
    for(int i=start;i<toks.size();++i) if(toks[i].code==0 && toks[i].val=="ENDSEC"){ end=i; break; }
    auto flip=[](double y){ return -y; };   // DXF is y-up; flip to doc y-down
    QVector<Shape> shapes;
    auto emitPath=[&](const QPainterPath& p, const QString& nm){ if(p.isEmpty())return; Shape s; s.type=ShapeType::Path; s.name=nm; s.path=p;
        s.fill.type=FillType::None; s.stroke.enabled=true; s.stroke.color=QColor(30,30,30); s.stroke.width=1.0; shapes.push_back(s); };
    QPainterPath poly; bool inPoly=false; int polyFlags=0;
    int i=start;
    while(i<end){
        if(toks[i].code!=0){ ++i; continue; }
        const QString type=toks[i].val; ++i;
        QVector<Pair> ent; while(i<end && toks[i].code!=0){ ent.push_back(toks[i]); ++i; }
        auto val=[&](int code,double def=0.0)->double{ for(const Pair& p:ent) if(p.code==code) return p.val.toDouble(); return def; };
        auto vals=[&](int code)->QVector<double>{ QVector<double> v; for(const Pair& p:ent) if(p.code==code) v.push_back(p.val.toDouble()); return v; };
        auto ival=[&](int code)->int{ for(const Pair& p:ent) if(p.code==code) return p.val.toInt(); return 0; };
        if(type=="POLYLINE"){ inPoly=true; poly=QPainterPath(); polyFlags=ival(70); continue; }
        if(type=="VERTEX" && inPoly){ QPointF pt(val(10),flip(val(20))); if(poly.elementCount()==0) poly.moveTo(pt); else poly.lineTo(pt); continue; }
        if(type=="SEQEND"){ if(inPoly){ if(polyFlags&1 && poly.elementCount()>1) poly.closeSubpath(); emitPath(poly,"Polyline"); } inPoly=false; continue; }
        QPainterPath path;
        if(type=="LINE"){ path.moveTo(val(10),flip(val(20))); path.lineTo(val(11),flip(val(21))); }
        else if(type=="CIRCLE"){ path.addEllipse(QPointF(val(10),flip(val(20))), val(40), val(40)); }
        else if(type=="ARC"){ const double cx=val(10),cy=flip(val(20)),r=val(40),a0=val(50),a1=val(51);
            // Guard non-finite operands, and normalise the span with fmod: `while(span<=0) span+=360`
            // never terminates for a huge start angle (360 falls below the ULP near 1e300 → infinite loop).
            if(qIsFinite(cx)&&qIsFinite(cy)&&qIsFinite(r)&&r>0 && qIsFinite(a0)&&qIsFinite(a1)){
                QRectF rc(cx-r,cy-r,2*r,2*r); double span=std::fmod(a1-a0,360.0); if(span<=0) span+=360;
                const double s0=std::fmod(a0,360.0);                                  // DXF CCW; flipped y → negate angles
                path.arcMoveTo(rc,-s0); path.arcTo(rc,-s0,-span); } }
        else if(type=="LWPOLYLINE"){ const QVector<double> xs=vals(10), ys=vals(20); const int flags=ival(70);
            for(int k=0;k<xs.size()&&k<ys.size();++k){ QPointF p(xs[k],flip(ys[k])); if(k==0)path.moveTo(p); else path.lineTo(p); }
            if(flags&1) path.closeSubpath(); }
        else if(type=="TEXT"||type=="MTEXT"){
            // Real text, not outlines: DXF group 1 is the string (MTEXT continues in 3), 10/20 the
            // insertion point, 40 the cap height, 50 the rotation in degrees (CCW on a y-up page).
            QString txt; for(const Pair& p:ent){ if(p.code==3) txt+=p.val; }
            for(const Pair& p:ent){ if(p.code==1){ txt+=p.val; break; } }
            txt.replace("\\P","\n").replace("\\~"," ");
            static const QRegularExpression mtextFmt("\\\\[A-Za-z][^;\\\\]*;|[{}]");   // strip MTEXT inline codes
            txt.remove(mtextFmt);
            txt=txt.trimmed();
            if(!txt.isEmpty()){
                // Clamp in the double domain: group 40 is file-supplied, and int() on an
                // out-of-range double is UB (the UBSan build traps) — a post-cast qBound is too late.
                const double raw40 = val(40,10.0);
                const double h = (qIsFinite(raw40) && raw40 > 0) ? qBound(1.0, raw40, 4000.0) : 10.0;
                Shape t; t.type=ShapeType::Text; t.name=type; t.text=txt;
                t.font=QFont("DejaVu Sans"); t.font.setPixelSize(int(h+0.5));
                t.fill.type=FillType::Solid; t.fill.color=QColor(30,30,30); t.stroke.enabled=false;
                t.rect=QRectF(0,0, qMax(1.0,h*0.62*txt.length()), h*1.25);
                // DXF rotates about the INSERTION point (the baseline-left), so translate there
                // first, rotate, and only then lift by the cap height to the frame's top-left.
                QTransform tr; tr.translate(val(10), flip(val(20)));
                const double rot=val(50); if(qIsFinite(rot) && qAbs(rot)>1e-9) tr.rotate(-rot);
                tr.translate(0, -h);
                t.transform=tr;
                shapes.push_back(t);
            }
        }
        emitPath(path, type);
    }
    if(shapes.isEmpty()) return doc;
    QRectF bb;
    auto bounds=[](const Shape& s){ return s.type==ShapeType::Text ? s.transform.mapRect(s.rect) : s.path.boundingRect(); };
    for(const Shape& s:shapes){ const QRectF r=bounds(s); if(r.isNull()) continue; bb = bb.isNull()? r : bb.united(r); }
    const QTransform norm=QTransform::fromTranslate(-bb.left(),-bb.top());   // shift bbox to origin
    for(Shape& s:shapes){ if(s.type==ShapeType::Text) s.transform = s.transform * norm; else s.path=norm.map(s.path); }
    doc.pageSize=QSizeF(qMax(1.0,bb.width()), qMax(1.0,bb.height()));
    doc.shapes=shapes;
    return doc;
}

// Windows Metafile (WMF) → editable Path shapes. Handles the optional placeable header, window
// org/ext mapping, pen/brush object table, and the common drawing records. Ignores records it
// doesn't understand (advancing by rdSize), so unusual metafiles still import their basic geometry.
Document importWmf(const QByteArray& data){
    Document doc;
    const uchar* p=reinterpret_cast<const uchar*>(data.constData()); const int n=data.size();
    auto u16=[&](int o)->quint16{ return (o>=0&&o+1<n)? quint16(p[o]|(p[o+1]<<8)) : 0; };
    auto s16=[&](int o)->qint16{ return qint16(u16(o)); };
    auto u32=[&](int o)->quint32{ return (o>=0&&o+3<n)? (quint32(p[o])|(quint32(p[o+1])<<8)|(quint32(p[o+2])<<16)|(quint32(p[o+3])<<24)) : 0; };
    auto colorAt=[&](int o)->QColor{ return (o+2<n)? QColor(p[o],p[o+1],p[o+2]) : QColor(Qt::black); };

    int off=0; QRectF bbox(0,0,1000,1000);
    if(u32(0)==0x9AC6CDD7u){                                     // placeable (Aldus) header
        int l=s16(6),t=s16(8),r=s16(10),b=s16(12);
        if(r>l && b>t) bbox=QRectF(l,t,r-l,b-t);
        off=22;
    }
    if(off+18>n) return doc;
    off+=18;                                                     // skip the standard METAHEADER

    double winOrgX=bbox.left(),winOrgY=bbox.top(),winExtX=bbox.width(),winExtY=bbox.height();
    auto mapPt=[&](double x,double y)->QPointF{
        double sx = winExtX!=0? bbox.width()/winExtX : 1.0, sy = winExtY!=0? bbox.height()/winExtY : 1.0;
        return QPointF((x-winOrgX)*sx, (y-winOrgY)*sy); };
    auto mapScale=[&]()->double{ double sx=winExtX!=0?qAbs(bbox.width()/winExtX):1.0, sy=winExtY!=0?qAbs(bbox.height()/winExtY):1.0; return (sx+sy)*0.5; };

    struct WObj{ bool used=false, isPen=false, none=false; QColor color=Qt::black; double w=1; };
    QVector<WObj> objs;
    QColor curPen(Qt::black), curBrush(Qt::white); double curPenW=1; bool penNone=false, brushNone=false;
    QPolygonF line;

    auto addShape=[&](const QPainterPath& pp, bool fill){
        if(pp.elementCount()==0) return; Shape s; s.type=ShapeType::Path; s.path=pp;
        if(fill && !brushNone){ s.fill.type=FillType::Solid; s.fill.color=curBrush; } else s.fill.type=FillType::None;
        if(!penNone){ s.stroke.enabled=true; s.stroke.color=curPen; s.stroke.width=qMax(0.2,curPenW*mapScale()); } else s.stroke.enabled=false;
        if(s.fill.type==FillType::None && !s.stroke.enabled) return;
        doc.shapes.push_back(s); };
    auto flushLine=[&](){ if(line.size()>=2){ QPainterPath pp; pp.addPolygon(line); addShape(pp,false); } line.clear(); };
    auto addObj=[&](const WObj& o){ for(int i=0;i<objs.size();++i) if(!objs[i].used){ objs[i]=o; return; } objs.push_back(o); };

    int guard=0;
    while(off+6<=n && guard++<200000){
        quint32 rsize=u32(off); quint16 func=u16(off+4);
        if(rsize<3) break;
        const int pr=off+6;                                      // rdParm start
        switch(func){
            case 0x0000: off=n; break;                                                       // META_EOF
            case 0x020B: winOrgY=s16(pr); winOrgX=s16(pr+2); break;                           // SETWINDOWORG (Y,X)
            case 0x020C: { int ey=s16(pr), ex=s16(pr+2); if(ex) winExtX=ex; if(ey) winExtY=ey; } break; // SETWINDOWEXT (Y,X)
            case 0x0214: flushLine(); line.clear(); line<<mapPt(s16(pr+2), s16(pr)); break;   // MOVETO (Y,X)
            case 0x0213: line<<mapPt(s16(pr+2), s16(pr)); break;                              // LINETO (Y,X)
            case 0x0324: case 0x0325: {                                                       // POLYGON / POLYLINE
                int cnt=u16(pr); const int fit=int((quint64(off)+quint64(rsize)*2 - quint64(pr+2))/4);  // points that actually fit in the record
                cnt=qBound(0, cnt, qMax(0,fit)); QPolygonF poly;
                for(int k=0;k<cnt;++k){ int o=pr+2+k*4; poly<<mapPt(s16(o), s16(o+2)); }      // points are (X,Y)
                if(poly.size()>=2){ QPainterPath pp; pp.addPolygon(poly); const bool isPolygon=(func==0x0324);
                    if(isPolygon) pp.closeSubpath(); addShape(pp, isPolygon); } } break;
            case 0x041B: { double b=s16(pr), r=s16(pr+2), t=s16(pr+4), l=s16(pr+6);           // RECTANGLE (B,R,T,L)
                QPainterPath pp; pp.addPolygon(QPolygonF()<<mapPt(l,t)<<mapPt(r,t)<<mapPt(r,b)<<mapPt(l,b)); pp.closeSubpath();
                addShape(pp,true); } break;
            case 0x0418: { double b=s16(pr), r=s16(pr+2), t=s16(pr+4), l=s16(pr+6);           // ELLIPSE (B,R,T,L)
                QPainterPath pp; pp.addEllipse(QRectF(mapPt(l,t), mapPt(r,b)).normalized()); addShape(pp,true); } break;
            case 0x02FA: { WObj o; o.isPen=true; o.used=true; int style=u16(pr); o.w=s16(pr+2); o.color=colorAt(pr+6); o.none=(style==5); addObj(o); } break; // CREATEPENINDIRECT
            case 0x02FC: { WObj o; o.isPen=false; o.used=true; int style=u16(pr); o.color=colorAt(pr+2); o.none=(style==1); addObj(o); } break;             // CREATEBRUSHINDIRECT
            case 0x012D: { int idx=u16(pr); if(idx>=0 && idx<objs.size() && objs[idx].used){ const WObj& o=objs[idx];  // SELECTOBJECT
                    if(o.isPen){ curPen=o.color; curPenW=o.w; penNone=o.none; } else { curBrush=o.color; brushNone=o.none; } } } break;
            case 0x01F0: { int idx=u16(pr); if(idx>=0 && idx<objs.size()) objs[idx]=WObj(); } break;                    // DELETEOBJECT
            default: break;                                                                   // ignore other records
        }
        if(off>=n) break;
        const quint64 next = quint64(off) + quint64(rsize)*2;
        if(next<=quint64(off) || next>quint64(n)) break;         // no progress / overrun → stop
        off = int(next);
    }
    flushLine();
    if(doc.shapes.isEmpty()) return doc;
    QRectF bb; for(const Shape& s: doc.shapes) bb = bb.isNull()? s.path.boundingRect() : bb.united(s.path.boundingRect());
    if(bb.isValid()){ const QTransform norm=QTransform::fromTranslate(-bb.left(),-bb.top());
        for(Shape& s: doc.shapes) s.path=norm.map(s.path);
        doc.pageSize=QSizeF(qMax(1.0,bb.width()), qMax(1.0,bb.height())); }
    return doc;
}

// Enhanced Metafile (EMF) → editable Path shapes. Iterates 32-bit records (iType,nSize header) and
// handles the common vector primitives + pen/brush objects (incl. GDI stock objects). POINTL/POINTS
// coordinates are taken as device space and normalised to content bounds. Unknown records are skipped.
Document importEmf(const QByteArray& data){
    Document doc;
    const uchar* p=reinterpret_cast<const uchar*>(data.constData()); const int n=data.size();
    auto u16=[&](int o)->quint16{ return (o>=0&&o+1<n)? quint16(p[o]|(p[o+1]<<8)) : 0; };
    auto s16=[&](int o)->qint16{ return qint16(u16(o)); };
    auto u32=[&](int o)->quint32{ return (o>=0&&o+3<n)? (quint32(p[o])|(quint32(p[o+1])<<8)|(quint32(p[o+2])<<16)|(quint32(p[o+3])<<24)) : 0; };
    auto s32=[&](int o)->qint32{ return qint32(u32(o)); };
    auto colorAt=[&](int o)->QColor{ return (o+2<n)? QColor(p[o],p[o+1],p[o+2]) : QColor(Qt::black); };
    if(n<88 || u32(0)!=1u) return doc;               // must start with EMR_HEADER

    struct WObj{ bool used=false, isPen=false, none=false; QColor color=Qt::black; double w=1; };
    QHash<int,WObj> objs;
    QColor curPen(Qt::black), curBrush(Qt::white); double curPenW=1; bool penNone=false, brushNone=false;
    QPolygonF line;
    auto addShape=[&](const QPainterPath& pp, bool fill){
        if(pp.elementCount()==0) return; Shape s; s.type=ShapeType::Path; s.path=pp;
        if(fill && !brushNone){ s.fill.type=FillType::Solid; s.fill.color=curBrush; } else s.fill.type=FillType::None;
        if(!penNone){ s.stroke.enabled=true; s.stroke.color=curPen; s.stroke.width=qMax(0.2,curPenW); } else s.stroke.enabled=false;
        if(s.fill.type==FillType::None && !s.stroke.enabled) return;
        doc.shapes.push_back(s); };
    auto flushLine=[&](){ if(line.size()>=2){ QPainterPath pp; pp.addPolygon(line); addShape(pp,false); } line.clear(); };

    int off=0, guard=0;
    while(off+8<=n && guard++<500000){
        const quint32 type=u32(off), size=u32(off+4);
        if(size<8 || quint64(off)+size>quint64(n)) break;
        const int pr=off+8;
        switch(type){
            case 14: off=n; break;                                                            // EMR_EOF
            case 27: flushLine(); line.clear(); line<<QPointF(s32(pr), s32(pr+4)); break;      // MOVETOEX (POINTL)
            case 54: line<<QPointF(s32(pr), s32(pr+4)); break;                                 // LINETO
            case 43: { double l=s32(pr),t=s32(pr+4),r=s32(pr+8),b=s32(pr+12);                  // RECTANGLE (rclBox l,t,r,b)
                QPainterPath pp; pp.addPolygon(QPolygonF()<<QPointF(l,t)<<QPointF(r,t)<<QPointF(r,b)<<QPointF(l,b)); pp.closeSubpath();
                addShape(pp,true); } break;
            case 42: { double l=s32(pr),t=s32(pr+4),r=s32(pr+8),b=s32(pr+12);                  // ELLIPSE (rclBox)
                QPainterPath pp; pp.addEllipse(QRectF(QPointF(l,t),QPointF(r,b)).normalized()); addShape(pp,true); } break;
            case 3: case 4: {                                                                 // POLYGON / POLYLINE (POINTL)
                int cnt=int(u32(pr+16)); const int fit=int((quint64(off)+size - quint64(pr+20))/8);   // points that fit in the record (8 bytes each)
                cnt=qBound(0, cnt, qMin(65536, qMax(0,fit))); QPolygonF poly;                  // rclBounds(16) then count(4) then points
                for(int k=0;k<cnt;++k){ int o=pr+20+k*8; poly<<QPointF(s32(o), s32(o+4)); }
                if(poly.size()>=2){ QPainterPath pp; pp.addPolygon(poly); const bool isPolygon=(type==3);
                    if(isPolygon) pp.closeSubpath(); addShape(pp,isPolygon); } } break;
            case 86: case 87: {                                                               // POLYGON16 / POLYLINE16 (POINTS)
                int cnt=int(u32(pr+16)); const int fit=int((quint64(off)+size - quint64(pr+20))/4);   // 4 bytes each
                cnt=qBound(0, cnt, qMin(65536, qMax(0,fit))); QPolygonF poly;
                for(int k=0;k<cnt;++k){ int o=pr+20+k*4; poly<<QPointF(s16(o), s16(o+2)); }
                if(poly.size()>=2){ QPainterPath pp; pp.addPolygon(poly); const bool isPolygon=(type==86);
                    if(isPolygon) pp.closeSubpath(); addShape(pp,isPolygon); } } break;
            case 38: { int ih=int(u32(pr)); int style=int(u32(pr+4));                          // CREATEPEN: ih, LOGPEN{style, width POINTL, color}
                WObj o; o.used=true; o.isPen=true; o.w=s32(pr+8); o.color=colorAt(pr+16); o.none=(style==5); objs.insert(ih,o); } break;
            case 39: { int ih=int(u32(pr)); int style=int(u32(pr+4));                          // CREATEBRUSHINDIRECT: ih, LOGBRUSH{style, color, hatch}
                WObj o; o.used=true; o.isPen=false; o.color=colorAt(pr+8); o.none=(style==1); objs.insert(ih,o); } break;
            case 37: { quint32 ih=u32(pr);                                                     // SELECTOBJECT (incl. stock objects)
                if(ih & 0x80000000u){ switch(ih){
                        case 0x80000000u: curBrush=Qt::white; brushNone=false; break;          // WHITE_BRUSH
                        case 0x80000004u: curBrush=Qt::black; brushNone=false; break;          // BLACK_BRUSH
                        case 0x80000005u: brushNone=true; break;                               // NULL_BRUSH
                        case 0x80000006u: curPen=Qt::white; penNone=false; break;              // WHITE_PEN
                        case 0x80000007u: curPen=Qt::black; penNone=false; break;              // BLACK_PEN
                        case 0x80000008u: penNone=true; break;                                 // NULL_PEN
                        default: break; } }
                else if(objs.contains(int(ih))){ const WObj& o=objs[int(ih)];
                    if(o.isPen){ curPen=o.color; curPenW=o.w; penNone=o.none; } else { curBrush=o.color; brushNone=o.none; } } } break;
            case 40: objs.remove(int(u32(pr))); break;                                         // DELETEOBJECT
            default: break;
        }
        if(off>=n) break;
        const quint64 next=quint64(off)+size; if(next<=quint64(off) || next>quint64(n)) break;
        off=int(next);
    }
    flushLine();
    if(doc.shapes.isEmpty()) return doc;
    QRectF bb; for(const Shape& s: doc.shapes) bb = bb.isNull()? s.path.boundingRect() : bb.united(s.path.boundingRect());
    if(bb.isValid()){ const QTransform norm=QTransform::fromTranslate(-bb.left(),-bb.top());
        for(Shape& s: doc.shapes) s.path=norm.map(s.path);
        doc.pageSize=QSizeF(qMax(1.0,bb.width()), qMax(1.0,bb.height())); }
    return doc;
}

} // namespace xara
