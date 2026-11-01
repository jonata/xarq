// Adobe FXG 1.0/2.0 → SVG. Written from the FXG 2.0 specification; the gradient geometry follows the
// Flex rules (a gradient with no x/y sits centred on the shape, one with x/y starts there).
#include "fxgvector.h"
#include "svgimport.h"
#include <QXmlStreamReader>
#include <QFontMetricsF>
#include <QFileInfo>
#include <QFile>
#include <QImage>
#include <QHash>
#include <QMap>
#include <QPainterPath>
#include <QTransform>
#include <QtMath>
#include <memory>
#include <vector>

namespace xara {
namespace {

struct Node {
    QString name;                                 // local name, namespace dropped
    QString ns;
    QMap<QString,QString> attr;
    std::vector<std::unique_ptr<Node>> kids;
    QString text;                                 // text directly inside (only kept for RichText content)
    std::vector<std::pair<int,QString>> runs;     // position among kids → the text before that kid
    const Node* child(const QString& n) const { for(const auto& k : kids) if(k->name==n) return k.get(); return nullptr; }
    QString a(const QString& n, const QString& def=QString()) const { auto it=attr.find(n); return it==attr.end()?def:it.value(); }
    double d(const QString& n, double def=0.0) const {
        auto it=attr.find(n); if(it==attr.end()) return def;
        bool ok=false; const double v=it.value().toDouble(&ok); return (ok&&qIsFinite(v))?v:def; }
};

const int kMaxDepth = 200, kMaxNodes = 2000000, kMaxInstances = 200000;

std::unique_ptr<Node> parseXml(const QByteArray& data){
    QXmlStreamReader x(data);
    std::unique_ptr<Node> root; std::vector<Node*> st; int count=0;
    while(!x.atEnd()){
        const auto t=x.readNext();
        if(t==QXmlStreamReader::StartElement){
            if(++count>kMaxNodes || (int)st.size()>=kMaxDepth) return nullptr;
            auto n=std::make_unique<Node>(); n->name=x.name().toString(); n->ns=x.namespaceUri().toString();
            for(const QXmlStreamAttribute& a : x.attributes()) if(a.namespaceUri().isEmpty()) n->attr.insert(a.name().toString(), a.value().toString());
            Node* raw=n.get();
            if(!st.empty()) st.back()->kids.push_back(std::move(n)); else if(!root) root=std::move(n); else return nullptr;
            st.push_back(raw);
        } else if(t==QXmlStreamReader::EndElement){ if(!st.empty()) st.pop_back(); }
        else if(t==QXmlStreamReader::Characters && !st.empty()){
            Node* p=st.back(); p->runs.push_back({ (int)p->kids.size(), x.text().toString() }); }
    }
    if(x.hasError() && !root) return nullptr;
    return root;
}

QString num(double v){ return QString::number(v,'g',12); }
QString esc(QString s){ s.replace('&',"&amp;"); s.replace('<',"&lt;"); s.replace('>',"&gt;"); s.replace('"',"&quot;"); return s; }

// Colours are "#RRGGBB" (or, in older files, "0xRRGGBB").
struct ColorXf {
    double rm=1,gm=1,bm=1,am=1, ro=0,go=0,bo=0,ao=0;
    ColorXf then(const ColorXf& o) const {       // o is the inner transform; this one applies after it
        ColorXf r;
        r.rm=rm*o.rm; r.gm=gm*o.gm; r.bm=bm*o.bm; r.am=am*o.am;
        r.ro=rm*o.ro+ro; r.go=gm*o.go+go; r.bo=bm*o.bo+bo; r.ao=am*o.ao+ao;
        return r; }
};
QRgb parseColor(const QString& s, QRgb def=0xff000000u){
    QString t=s.trimmed(); if(t.isEmpty()) return def;
    if(t.startsWith('#')) t=t.mid(1); else if(t.startsWith("0x",Qt::CaseInsensitive)) t=t.mid(2);
    bool ok=false; const uint v=t.toUInt(&ok,16); if(!ok) return def;
    return 0xff000000u | (v & 0xffffffu);
}
QString colorStr(QRgb c, const ColorXf& x){
    const int r=qBound(0,qRound(qRed(c)*x.rm+x.ro),255), g=qBound(0,qRound(qGreen(c)*x.gm+x.go),255), b=qBound(0,qRound(qBlue(c)*x.bm+x.bo),255);
    return QStringLiteral("#%1%2%3").arg(r,2,16,QLatin1Char('0')).arg(g,2,16,QLatin1Char('0')).arg(b,2,16,QLatin1Char('0'));
}
double alphaOf(double a, const ColorXf& x){ return qBound(0.0, a*x.am + x.ao/255.0, 1.0); }

QTransform matrixOf(const Node* m){
    return QTransform(m->d("a",1),m->d("b",0),m->d("c",0),m->d("d",1),m->d("tx",0),m->d("ty",0));
}
// An element's placement: either the attributes, or a <transform><Transform><matrix> child. (Flash
// applies scale, then rotation, then translation.)
QTransform placementOf(const Node& n){
    QTransform t; t.scale(1,1);
    QTransform xf;
    xf.translate(n.d("x"),n.d("y"));
    xf.rotate(n.d("rotation"));
    xf.scale(n.d("scaleX",1),n.d("scaleY",1));
    t=xf;
    if(const Node* tr=n.child("transform")) if(const Node* T=tr->child("Transform")) if(const Node* m=T->child("matrix")) if(const Node* M=m->child("Matrix"))
        t=matrixOf(M)*t;
    return t;
}
ColorXf colorXfOf(const Node& n){
    ColorXf c;
    const Node* src=nullptr;
    if(const Node* tr=n.child("transform")) if(const Node* T=tr->child("Transform")) if(const Node* m=T->child("colorTransform")) src=m->child("ColorTransform");
    if(!src) return c;
    c.rm=src->d("redMultiplier",1); c.gm=src->d("greenMultiplier",1); c.bm=src->d("blueMultiplier",1); c.am=src->d("alphaMultiplier",1);
    c.ro=src->d("redOffset"); c.go=src->d("greenOffset"); c.bo=src->d("blueOffset"); c.ao=src->d("alphaOffset");
    return c;
}
QString svgMatrix(const QTransform& t){
    return QStringLiteral("matrix(%1 %2 %3 %4 %5 %6)").arg(num(t.m11()),num(t.m12()),num(t.m21()),num(t.m22()),num(t.dx()),num(t.dy()));
}
QString blendKeyword(const QString& b){
    static const QHash<QString,QString> m{
        {"multiply","multiply"},{"screen","screen"},{"overlay","overlay"},{"darken","darken"},{"lighten","lighten"},
        {"difference","difference"},{"hardlight","hard-light"},{"softlight","soft-light"},{"colordodge","color-dodge"},
        {"colorburn","color-burn"},{"exclusion","exclusion"},{"hue","hue"},{"saturation","saturation"},{"color","color"},
        {"luminosity","luminosity"},{"add","xarq-add"},{"subtract","xarq-subtract"} };
    return m.value(b);
}

class Conv {
public:
    QString baseDir;
    QHash<QString,const Node*> defs;
    QString body, defsOut;
    QRectF bounds;                                // where the artwork lands, for files with no view size
    int nextId=0, instances=0;

    void graphicKids(const Node& g, const QTransform& ctm, const ColorXf& cx, int depth){
        for(const auto& k : g.kids) element(*k, ctm, cx, depth+1);
    }

    void element(const Node& n, const QTransform& ctm, const ColorXf& cx, int depth){
        if(depth>kMaxDepth || n.a("visible")==QLatin1String("false")) return;
        const QString& t=n.name;
        if(t=="Group")                     group(n, &n, ctm, cx, depth);
        else if(t=="Rect"||t=="Ellipse"||t=="Line"||t=="Path") shape(n, ctm, cx);
        else if(t=="BitmapImage"||t=="BitmapGraphic") bitmap(n, ctm, cx);
        else if(t=="RichText"||t=="TextGraphic") text(n, ctm, cx);
        else if(defs.contains(t)){
            if(++instances>kMaxInstances) return;
            group(n, defs.value(t), ctm, cx, depth);      // a placed symbol: the instance places the definition's group
        }
    }

private:
    QString newId(const char* base){ return QStringLiteral("%1%2").arg(QLatin1String(base)).arg(++nextId); }
    void note(const QRectF& r, const QTransform& ctm){
        const QRectF d=ctm.mapRect(r);
        if(!d.isValid() && d.width()<=0 && d.height()<=0 && d.isNull()) return;
        if(bounds.isNull()) bounds=d;
        else { const double x0=qMin(bounds.left(),d.left()), y0=qMin(bounds.top(),d.top()), x1=qMax(bounds.right(),d.right()), y1=qMax(bounds.bottom(),d.bottom());
               bounds=QRectF(x0,y0,x1-x0,y1-y0); }
    }

    QString commonAttrs(const Node& n, const QString& extra=QString()){
        QString s;
        const double alpha=n.d("alpha",1);
        if(alpha<1) s+=QStringLiteral(" opacity=\"%1\"").arg(num(qBound(0.0,alpha,1.0)));
        const QString bm=blendKeyword(n.a("blendMode"));
        if(!bm.isEmpty()) s+=QStringLiteral(" style=\"mix-blend-mode:%1\"").arg(bm);
        s+=effectAttrs(n);
        s+=extra;
        return s;
    }
    // <filters>: a drop shadow and a glow become the object's shadow, a blur its feather.
    QString effectAttrs(const Node& n){
        const Node* fs=n.child("filters"); if(!fs) return QString();
        QString s; bool hasShadow=false;
        for(const auto& f : fs->kids){
            if(f->name=="DropShadowFilter" && !hasShadow && f->a("inner")!=QLatin1String("true")){
                const double ang=qDegreesToRadians(f->d("angle",45)), dist=f->d("distance",4);
                const QString col=colorStr(parseColor(f->a("color")),ColorXf());
                s+=QStringLiteral(" xarq:shadow=\"%1 %2 %3 %4 %5 0\"").arg(num(dist*qCos(ang)),num(dist*qSin(ang)),num(qMax(f->d("blurX",4),f->d("blurY",4))),col,num(f->d("alpha",1)));
                hasShadow=true;
            } else if(f->name=="GlowFilter" && !hasShadow && f->a("inner")!=QLatin1String("true")){
                const QString col=colorStr(parseColor(f->a("color"),0xffff0000u),ColorXf());
                s+=QStringLiteral(" xarq:shadow=\"0 0 %1 %2 %3 1\"").arg(num(qMax(f->d("blurX",4),f->d("blurY",4))),col,num(f->d("alpha",1)));
                hasShadow=true;
            } else if(f->name=="BlurFilter"){
                s+=QStringLiteral(" xarq:blur=\"%1\"").arg(num(qMax(f->d("blurX",4),f->d("blurY",4))*0.9));
            }
        }
        return s;
    }

    // A <mask> child: its outline clips. FXG's alpha and luminosity masks become clips too, since the
    // SVG route has no soft masks.
    QString maskAttr(const Node& n, const QTransform& ctm, const ColorXf& cx, int depth){
        const Node* m=n.child("mask"); if(!m) return QString();
        const QString id=newId("clip");
        QString saved=body; body.clear();
        const QRectF savedB=bounds;
        for(const auto& k : m->kids) element(*k, ctm, cx, depth+1);
        const QString inner=body; body=saved; bounds=savedB;
        if(inner.trimmed().isEmpty()) return QString();
        defsOut+=QStringLiteral("<clipPath id=\"%1\">%2</clipPath>\n").arg(id,inner);
        return QStringLiteral(" clip-path=\"url(#%1)\"").arg(id);
    }

    void group(const Node& placed, const Node* content, const QTransform& ctm, const ColorXf& cx, int depth){
        if(depth>kMaxDepth) return;
        const QTransform local=placementOf(placed), inner=local*ctm;
        const ColorXf icx=cx.then(colorXfOf(placed));
        const QString clip=maskAttr(placed, QTransform(), icx, depth);
        body+=QStringLiteral("<g transform=\"%1\"%2>\n").arg(svgMatrix(local), commonAttrs(placed, clip));
        for(const auto& k : content->kids) element(*k, inner, icx, depth+1);
        body+=QStringLiteral("</g>\n");
    }

    // --- paint ----------------------------------------------------------------------------------
    QString stops(const Node& g, const ColorXf& cx){
        QString s; int i=0; const int n=[&]{ int c=0; for(const auto& k : g.kids) if(k->name=="GradientEntry") ++c; return c; }();
        for(const auto& k : g.kids) if(k->name=="GradientEntry"){
            const double r=k->attr.contains("ratio")? k->d("ratio") : (n>1? double(i)/(n-1) : 0.0);
            s+=QStringLiteral("<stop offset=\"%1\" stop-color=\"%2\" stop-opacity=\"%3\"/>")
                .arg(num(qBound(0.0,r,1.0)), colorStr(parseColor(k->a("color")),cx), num(alphaOf(k->d("alpha",1),cx)));
            ++i;
        }
        return s;
    }
    static QString spreadAttr(const Node& g){
        const QString m=g.a("spreadMethod");
        return (m=="reflect"||m=="repeat")? QStringLiteral(" spreadMethod=\"%1\"").arg(m) : QString();
    }
    QString linearGrad(const Node& g, const QRectF& b, const ColorXf& cx){
        const QString id=newId("grad");
        QString geom;
        if(const Node* mx=g.child("matrix")){            // the raw matrix maps Flash's ±819.2 square
            if(const Node* M=mx->child("Matrix")){
                const QTransform m=matrixOf(M);
                const QPointF a=m.map(QPointF(-819.2,0)), z=m.map(QPointF(819.2,0));
                geom=QStringLiteral("x1=\"%1\" y1=\"%2\" x2=\"%3\" y2=\"%4\"").arg(num(a.x()),num(a.y()),num(z.x()),num(z.y()));
            }
        }
        if(geom.isEmpty()){
            const double rot=g.d("rotation");
            double len;
            if(g.attr.contains("scaleX")) len=g.d("scaleX");
            else if(std::fmod(rot,90.0)!=0.0){
                double na=std::fmod(rot,360.0); if(na<0) na+=360; na=std::fmod(na,180.0); if(na>90) na=180-na;
                double side=b.width(); const double hyp=std::hypot(b.width(),b.height());
                const double hypA= hyp>0? qRadiansToDegrees(std::acos(b.width()/hyp)) : 0;
                if(na>hypA){ na=90-na; side=b.height(); }
                len=side/qMax(1e-6,qCos(qDegreesToRadians(na)));
            } else len=(std::fmod(rot,180.0)==0.0)? b.width() : b.height();
            const double ra=qDegreesToRadians(rot), ux=qCos(ra), uy=qSin(ra);
            QPointF p0,p1;
            if(g.attr.contains("x")||g.attr.contains("y")){      // starts at x,y
                p0=QPointF(g.d("x"),g.d("y")); p1=p0+QPointF(ux*len,uy*len);
            } else {                                            // centred on the shape
                const QPointF c=b.center(); p0=c-QPointF(ux*len/2,uy*len/2); p1=c+QPointF(ux*len/2,uy*len/2);
            }
            geom=QStringLiteral("x1=\"%1\" y1=\"%2\" x2=\"%3\" y2=\"%4\"").arg(num(p0.x()),num(p0.y()),num(p1.x()),num(p1.y()));
        }
        defsOut+=QStringLiteral("<linearGradient id=\"%1\" gradientUnits=\"userSpaceOnUse\" %2%3>%4</linearGradient>\n").arg(id,geom,spreadAttr(g),stops(g,cx));
        return id;
    }
    QString radialGrad(const Node& g, const QRectF& b, const ColorXf& cx){
        const QString id=newId("grad");
        QTransform m;
        if(const Node* mx=g.child("matrix")){
            if(const Node* M=mx->child("Matrix")){ m=matrixOf(M); m=QTransform::fromScale(819.2,819.2)*m; }
        } else {
            const double w=g.attr.contains("scaleX")?g.d("scaleX"):b.width(), h=g.attr.contains("scaleY")?g.d("scaleY"):b.height();
            const double x=g.attr.contains("x")?g.d("x"):b.center().x(), y=g.attr.contains("y")?g.d("y"):b.center().y();
            m.translate(x,y); m.rotate(g.d("rotation")); m.scale(w/2,h/2);
        }
        const double f=qBound(-0.99,g.d("focalPointRatio"),0.99);
        defsOut+=QStringLiteral("<radialGradient id=\"%1\" gradientUnits=\"userSpaceOnUse\" cx=\"0\" cy=\"0\" r=\"1\" fx=\"%2\" fy=\"0\" gradientTransform=\"%3\"%4>%5</radialGradient>\n")
            .arg(id,num(f),svgMatrix(m),spreadAttr(g),stops(g,cx));
        return id;
    }
    QString embedSource(const QString& src){
        QString s=src.trimmed();                         // "@Embed('a/b.png')" or a bare path
        const int q=s.indexOf(QLatin1Char('\'')); const int r=s.lastIndexOf(QLatin1Char('\''));
        if(q>=0 && r>q) s=s.mid(q+1,r-q-1);
        else { const int dq=s.indexOf(QLatin1Char('"')), dr=s.lastIndexOf(QLatin1Char('"')); if(dq>=0&&dr>dq) s=s.mid(dq+1,dr-dq-1); }
        if(s.startsWith("data:")) return s;
        if(baseDir.isEmpty()) return QString();
        // Only files beside the document: an @Embed path never reaches outside its folder.
        const QString name=QFileInfo(s).isAbsolute()||s.contains("..")? QFileInfo(s).fileName() : s;
        QFile f(baseDir+QLatin1Char('/')+name);
        if(!f.open(QIODevice::ReadOnly) || f.size()>(256<<20)) return QString();
        const QByteArray b=f.readAll();
        QImage im; if(!im.loadFromData(b)) return QString();
        const char* mime= b.startsWith("\x89PNG")?"image/png": b.startsWith("\xff\xd8")?"image/jpeg": b.startsWith("GIF")?"image/gif":"image/png";
        return QStringLiteral("data:%1;base64,%2").arg(QLatin1String(mime),QString::fromLatin1(b.toBase64()));
    }
    static QSize dataUriSize(const QString& uri){
        const int c=uri.indexOf(QLatin1Char(',')); if(c<0) return QSize();
        QImage im; im.loadFromData(QByteArray::fromBase64(uri.mid(c+1).toLatin1())); return im.size();
    }
    QString fillAttrs(const Node& n, const QRectF& b, const ColorXf& cx){
        const Node* fw=n.child("fill");
        const Node* f=nullptr; if(fw) for(const auto& k : fw->kids){ f=k.get(); break; }
        if(!f) return QStringLiteral(" fill=\"none\"");
        if(f->name=="SolidColor"){
            const double a=alphaOf(f->d("alpha",1),cx);
            return QStringLiteral(" fill=\"%1\"%2").arg(colorStr(parseColor(f->a("color")),cx), a<1?QStringLiteral(" fill-opacity=\"%1\"").arg(num(a)):QString());
        }
        if(f->name=="LinearGradient") return QStringLiteral(" fill=\"url(#%1)\"").arg(linearGrad(*f,b,cx));
        if(f->name=="RadialGradient") return QStringLiteral(" fill=\"url(#%1)\"").arg(radialGrad(*f,b,cx));
        if(f->name=="BitmapFill"){
            const QString uri=embedSource(f->a("source"));
            const QSize sz=dataUriSize(uri);
            if(sz.isEmpty()) return QStringLiteral(" fill=\"#808080\"");
            QString mode=f->a("fillMode");
            if(mode.isEmpty()) mode=f->a("repeat")==QLatin1String("false")?QStringLiteral("clip"):QStringLiteral("scale");
            QTransform m;
            if(const Node* mx=f->child("matrix")){ if(const Node* M=mx->child("Matrix")) m=matrixOf(M); }
            else {
                m.translate(f->d("x"),f->d("y")); m.rotate(f->d("rotation"));
                double sx=f->d("scaleX",1), sy=f->d("scaleY",1);
                if(mode=="scale" && !f->attr.contains("scaleX") && !f->attr.contains("scaleY")){
                    m.reset(); m.translate(b.left(),b.top()); sx=b.width()/sz.width(); sy=b.height()/sz.height(); }
                m.scale(sx,sy);
            }
            // The bridge maps the unit square with image-top at p2, so the v axis runs upward.
            const QPointF p0=m.map(QPointF(0,sz.height())), p1=m.map(QPointF(sz.width(),sz.height())), p2=m.map(QPointF(0,0));
            return QStringLiteral(" fill=\"#808080\" xarq:bitmap=\"%1\" xarq:bitmap-xf=\"%2 %3 %4 %5 %6 %7\" xarq:bitmap-tiled=\"%8\"")
                .arg(uri).arg(num(p1.x()-p0.x()),num(p1.y()-p0.y()),num(p2.x()-p0.x()),num(p2.y()-p0.y()),num(p0.x()),num(p0.y()))
                .arg(mode=="repeat"?1:0);
        }
        return QStringLiteral(" fill=\"none\"");
    }
    QString strokeAttrs(const Node& n, const QRectF& b, const ColorXf& cx){
        const Node* sw=n.child("stroke");
        const Node* f=nullptr; if(sw) for(const auto& k : sw->kids){ f=k.get(); break; }
        if(!f) return QStringLiteral(" stroke=\"none\"");
        QString s;
        if(f->name=="SolidColorStroke"){
            const double a=alphaOf(f->d("alpha",1),cx);
            s=QStringLiteral(" stroke=\"%1\"%2").arg(colorStr(parseColor(f->a("color")),cx), a<1?QStringLiteral(" stroke-opacity=\"%1\"").arg(num(a)):QString());
        } else if(f->name=="LinearGradientStroke") s=QStringLiteral(" stroke=\"url(#%1)\"").arg(linearGrad(*f,b,cx));
        else if(f->name=="RadialGradientStroke")   s=QStringLiteral(" stroke=\"url(#%1)\"").arg(radialGrad(*f,b,cx));
        else return QStringLiteral(" stroke=\"none\"");
        s+=QStringLiteral(" stroke-width=\"%1\"").arg(num(qMax(0.0,f->d("weight",1))));
        const QString caps=f->a("caps","round");
        s+=QStringLiteral(" stroke-linecap=\"%1\"").arg(caps=="none"?QStringLiteral("butt"):caps=="square"?QStringLiteral("square"):QStringLiteral("round"));
        const QString joints=f->a("joints","round");
        s+=QStringLiteral(" stroke-linejoin=\"%1\"").arg(joints=="miter"?QStringLiteral("miter"):joints=="bevel"?QStringLiteral("bevel"):QStringLiteral("round"));
        s+=QStringLiteral(" stroke-miterlimit=\"%1\"").arg(num(qMax(1.0,f->d("miterLimit",3))));
        return s;
    }

    // --- shapes ---------------------------------------------------------------------------------
    void shape(const Node& n, const QTransform& ctm, const ColorXf& cx0){
        const ColorXf cx=cx0.then(colorXfOf(n));
        QString geom; QRectF b;
        const QString& t=n.name;
        if(t=="Rect"){
            const double w=n.d("width"), h=n.d("height");
            if(w<=0||h<=0) return;
            b=QRectF(0,0,w,h);
            const bool perCorner=n.attr.contains("topLeftRadiusX")||n.attr.contains("topRightRadiusX")||n.attr.contains("bottomLeftRadiusX")||n.attr.contains("bottomRightRadiusX");
            if(perCorner){
                const double rx=n.d("radiusX"), ry=n.d("radiusY",rx);
                auto cr=[&](const char* c){ const double x=n.d(QStringLiteral("%1RadiusX").arg(QLatin1String(c)),rx), y=n.d(QStringLiteral("%1RadiusY").arg(QLatin1String(c)),x?x:ry);
                    return QPointF(qBound(0.0,x,w/2),qBound(0.0,y,h/2)); };
                const QPointF tl=cr("topLeft"),tr=cr("topRight"),bl=cr("bottomLeft"),br=cr("bottomRight");
                geom=QStringLiteral("<path d=\"M%1 0 L%2 0 A%3 %4 0 0 1 %5 %6 L%7 %8 A%9 %10 0 0 1 %11 %12 ")
                    .arg(num(tl.x()),num(w-tr.x()),num(tr.x()),num(tr.y()),num(w),num(tr.y()),num(w),num(h-br.y()),num(br.x()))
                    .arg(num(br.y()),num(w-br.x()),num(h));
                geom+=QStringLiteral("L%1 %2 A%3 %4 0 0 1 0 %5 L0 %6 A%7 %8 0 0 1 %9 0 Z\"")
                    .arg(num(bl.x()),num(h),num(bl.x()),num(bl.y()),num(h-bl.y()),num(tl.y()),num(tl.x()),num(tl.y()),num(tl.x()));
            } else {
                const double rx=n.d("radiusX"), ry=n.d("radiusY",rx);
                geom=QStringLiteral("<rect x=\"0\" y=\"0\" width=\"%1\" height=\"%2\"").arg(num(w),num(h));
                if(rx>0||ry>0) geom+=QStringLiteral(" rx=\"%1\" ry=\"%2\"").arg(num(rx),num(ry));
            }
        } else if(t=="Ellipse"){
            const double w=n.d("width"), h=n.d("height");
            if(w<=0||h<=0) return;
            b=QRectF(0,0,w,h);
            geom=QStringLiteral("<ellipse cx=\"%1\" cy=\"%2\" rx=\"%3\" ry=\"%4\"").arg(num(w/2),num(h/2),num(w/2),num(h/2));
        } else if(t=="Line"){
            const double x0=n.d("xFrom"),y0=n.d("yFrom"),x1=n.d("xTo"),y1=n.d("yTo");
            b=QRectF(QPointF(x0,y0),QPointF(x1,y1)).normalized();
            geom=QStringLiteral("<line x1=\"%1\" y1=\"%2\" x2=\"%3\" y2=\"%4\"").arg(num(x0),num(y0),num(x1),num(y1));
        } else {
            const QString d=n.a("data").trimmed();
            if(d.isEmpty()) return;
            b=parseSvgPathData(d).boundingRect();
            geom=QStringLiteral("<path d=\"%1\" fill-rule=\"%2\"").arg(esc(d), n.a("winding")==QLatin1String("nonZero")?QStringLiteral("nonzero"):QStringLiteral("evenodd"));
        }
        const QTransform local=placementOf(n);
        note(b, local*ctm);
        const QString clip=maskAttr(n, QTransform(), cx, 0);
        const QString paint=fillAttrs(n,b,cx)+strokeAttrs(n,b,cx);
        body+=geom+QStringLiteral(" transform=\"%1\"%2%3/>\n").arg(svgMatrix(local),paint,commonAttrs(n,clip));
    }

    void bitmap(const Node& n, const QTransform& ctm, const ColorXf& cx){
        Q_UNUSED(cx);
        const QString uri=embedSource(n.a("source"));
        const QSize sz=dataUriSize(uri);
        if(sz.isEmpty()) return;
        const double w=n.attr.contains("width")?n.d("width"):sz.width(), h=n.attr.contains("height")?n.d("height"):sz.height();
        const QTransform local=placementOf(n);
        note(QRectF(0,0,w,h), local*ctm);
        QString mode=n.a("fillMode");
        if(mode.isEmpty()) mode=n.a("repeat")==QLatin1String("true")?QStringLiteral("repeat"):QStringLiteral("scale");
        if(mode=="scale"){
            body+=QStringLiteral("<image x=\"0\" y=\"0\" width=\"%1\" height=\"%2\" transform=\"%3\" href=\"%4\"%5/>\n")
                .arg(num(w),num(h),svgMatrix(local),uri,commonAttrs(n));
        } else {
            // clip: natural size, cut to the box; repeat: tiled over it. Either way a filled rectangle.
            body+=QStringLiteral("<rect x=\"0\" y=\"0\" width=\"%1\" height=\"%2\" transform=\"%3\" stroke=\"none\" fill=\"#808080\" xarq:bitmap=\"%4\" xarq:bitmap-xf=\"%5 0 0 %6 0 %7\" xarq:bitmap-tiled=\"%8\"%9/>\n")
                .arg(num(w),num(h),svgMatrix(local),uri,num(sz.width()),num(-sz.height()),num(sz.height())).arg(mode=="repeat"?1:0).arg(commonAttrs(n));
        }
    }

    // --- text -----------------------------------------------------------------------------------
    struct TStyle {
        QString family=QStringLiteral("Arial"); double size=12; QString weight=QStringLiteral("normal"), italic=QStringLiteral("normal");
        QRgb color=0xff000000u; double alpha=1; QString lineHeight=QStringLiteral("120%"), decoration=QStringLiteral("none");
        double tracking=0;
    };
    struct TPara { QString align; double indent=0, startIndent=0, endIndent=0, before=0, after=0; };
    struct Span { QString text; TStyle st; };
    struct ParaOut { TPara p; QVector<Span> spans; };
    static void readStyle(const Node& n, TStyle& s){
        if(n.attr.contains("fontFamily")) { QString f=n.a("fontFamily"); const int c=f.indexOf(','); if(c>0) f=f.left(c); s.family=f.trimmed(); }
        if(n.attr.contains("fontSize")) s.size=qBound(0.1,n.d("fontSize",12),4000.0);
        if(n.attr.contains("fontWeight")) s.weight=n.a("fontWeight");
        if(n.attr.contains("fontStyle")) s.italic=n.a("fontStyle");
        if(n.attr.contains("color")) s.color=parseColor(n.a("color"));
        if(n.attr.contains("textAlpha")) s.alpha=qBound(0.0,n.d("textAlpha",1),1.0);
        if(n.attr.contains("lineHeight")) s.lineHeight=n.a("lineHeight");
        if(n.attr.contains("textDecoration")) s.decoration=n.a("textDecoration");
        if(n.attr.contains("trackingRight")){ const QString t=n.a("trackingRight"); s.tracking=t.endsWith('%')? t.chopped(1).toDouble()/100.0*s.size : t.toDouble(); }
    }
    static void readPara(const Node& n, TPara& p){
        if(n.attr.contains("textAlign")) p.align=n.a("textAlign");
        if(n.attr.contains("textIndent")) p.indent=n.d("textIndent");
        if(n.attr.contains("paragraphStartIndent")) p.startIndent=n.d("paragraphStartIndent");
        if(n.attr.contains("paragraphEndIndent")) p.endIndent=n.d("paragraphEndIndent");
        if(n.attr.contains("paragraphSpaceBefore")) p.before=n.d("paragraphSpaceBefore");
        if(n.attr.contains("paragraphSpaceAfter")) p.after=n.d("paragraphSpaceAfter");
    }
    static QString collapsed(QString t){
        // XML layout whitespace between elements is not text.
        t.replace(QLatin1Char('\t'),QLatin1Char(' ')); t.replace(QLatin1Char('\r'),QLatin1Char(' ')); t.replace(QLatin1Char('\n'),QLatin1Char(' '));
        QString o; bool sp=false; for(const QChar c : t){ if(c==QLatin1Char(' ')){ if(!sp) o+=c; sp=true; } else { o+=c; sp=false; } }
        return o;
    }
    void inlineRuns(const Node& n, const TStyle& st, ParaOut& out, QVector<ParaOut>& all, int depth){
        if(depth>kMaxDepth) return;
        int runIdx=0;
        auto flushTo=[&](int kid){
            while(runIdx<(int)n.runs.size() && n.runs[runIdx].first<=kid){
                const QString t=collapsed(n.runs[runIdx].second); ++runIdx;
                if(!t.trimmed().isEmpty() || (!t.isEmpty() && !out.spans.isEmpty())) out.spans.push_back({t,st}); } };
        for(int i=0;i<(int)n.kids.size();++i){
            flushTo(i);
            const Node& k=*n.kids[i];
            if(k.name=="br"){ TPara keep=out.p; all.push_back(out); out=ParaOut(); out.p=keep; }
            else if(k.name=="tab"){ out.spans.push_back({QStringLiteral("    "),st}); }
            else if(k.name=="span"||k.name=="a"||k.name=="tcy"){ TStyle s2=st; readStyle(k,s2); inlineRuns(k,s2,out,all,depth+1); }
        }
        flushTo((int)n.kids.size());
    }
    void blocks(const Node& n, TStyle st, TPara pa, QVector<ParaOut>& all, int depth){
        if(depth>kMaxDepth) return;
        readStyle(n,st); readPara(n,pa);
        bool sawBlock=false;
        for(const auto& k : n.kids) if(k->name=="p"||k->name=="div") sawBlock=true;
        if(!sawBlock){
            ParaOut po; po.p=pa; inlineRuns(n,st,po,all,depth+1); if(!po.spans.isEmpty()||!all.isEmpty()) all.push_back(po);
            return;
        }
        for(const auto& k : n.kids){
            if(k->name=="div") blocks(*k,st,pa,all,depth+1);
            else if(k->name=="p"){
                TStyle s2=st; TPara p2=pa; readStyle(*k,s2); readPara(*k,p2);
                ParaOut po; po.p=p2; inlineRuns(*k,s2,po,all,depth+1); all.push_back(po);
            }
        }
    }
    static QFont fontOf(const TStyle& s){
        QFont f(s.family); f.setPixelSize(qMax(1,qRound(s.size))); f.setBold(s.weight==QLatin1String("bold")); f.setItalic(s.italic==QLatin1String("italic"));
        return f;
    }
    static double lineStep(const TStyle& s){
        const QString l=s.lineHeight.trimmed();
        if(l.endsWith('%')) return l.chopped(1).toDouble()/100.0*s.size;
        const double v=l.toDouble(); return v>0?v:1.2*s.size;
    }
    void text(const Node& n, const QTransform& ctm, const ColorXf& cx0){
        const ColorXf cx=cx0.then(colorXfOf(n));
        TStyle base; TPara pa;
        QVector<ParaOut> paras;
        if(const Node* c=n.child("content")){ readStyle(n,base); readPara(n,pa); blocks(*c,base,pa,paras,0); }
        else { readStyle(n,base); readPara(n,pa);
            ParaOut po; po.p=pa; inlineRuns(n,base,po,paras,0); paras.push_back(po); }
        if(paras.isEmpty()) return;
        const double boxW=n.d("width",0), boxH=n.d("height",0);
        const bool wrap= boxW>0 && n.a("lineBreak")!=QLatin1String("explicit");
        const QTransform local=placementOf(n);
        struct Word { QString t; TStyle st; double w; };
        QString out; double y=0; bool first=true; double maxW=0;
        for(const ParaOut& po : paras){
            y+=po.p.before;
            QVector<QVector<Word>> lines; lines.push_back({});
            double lw=po.p.indent; double avail=boxW-po.p.startIndent-po.p.endIndent;
            for(const Span& sp : po.spans){
                const QFontMetricsF fm(fontOf(sp.st));
                // Break after each space so a run keeps its own inner spacing.
                int i=0; const QString& t=sp.text;
                while(i<t.size()){
                    int j=i; while(j<t.size() && t[j]!=QLatin1Char(' ')) ++j; while(j<t.size() && t[j]==QLatin1Char(' ')) ++j;
                    const QString w=t.mid(i,j-i); const double wd=fm.horizontalAdvance(w)+sp.st.tracking*w.size();
                    const double wdTrim=fm.horizontalAdvance(w.trimmed())+sp.st.tracking*w.trimmed().size();
                    if(wrap && lw+wdTrim>avail && !lines.back().isEmpty()){ lines.push_back({}); lw=0; }
                    lines.back().push_back({w,sp.st,wd}); lw+=wd; i=j;
                }
            }
            bool firstLine=true;
            for(const auto& ln : lines){
                double step=0, asc=0, lineW=0;
                for(const Word& w : ln){ step=qMax(step,lineStep(w.st)); asc=qMax(asc,QFontMetricsF(fontOf(w.st)).ascent()); lineW+=w.w; }
                if(ln.isEmpty()){ step=lineStep(base); asc=QFontMetricsF(fontOf(base)).ascent(); }
                // Flash places the first baseline at the font's ascent, then steps by the leading.
                const double baseline= first? y+asc : y+step;
                y=baseline; first=false;
                double x=po.p.startIndent+(firstLine?po.p.indent:0);
                const QString al=po.p.align;
                if(boxW>0){
                    if(al=="center") x=po.p.startIndent+(avail-lineW)/2;
                    else if(al=="end"||al=="right") x=boxW-po.p.endIndent-lineW;
                }
                maxW=qMax(maxW,x+lineW);
                if(!ln.isEmpty()){
                    // Merge same-styled neighbours so each run stays one editable text object.
                    out+=QStringLiteral("<text xml:space=\"preserve\" x=\"%1\" y=\"%2\"%3>").arg(num(x),num(baseline),
                        ln[0].st.tracking!=0?QStringLiteral(" letter-spacing=\"%1\"").arg(num(ln[0].st.tracking)):QString());
                    int a=0;
                    while(a<ln.size()){
                        int b=a; QString tx;
                        while(b<ln.size() && sameStyle(ln[b].st,ln[a].st)){ tx+=ln[b].t; ++b; }
                        if(tx.trimmed().isEmpty()) tx.replace(QLatin1Char(' '),QChar(0x00A0));   // the SVG route drops a run of bare spaces
                        const TStyle& s=ln[a].st;
                        const double al2=alphaOf(s.alpha,cx);
                        out+=QStringLiteral("<tspan font-family=\"%1\" font-size=\"%2\" font-weight=\"%3\" font-style=\"%4\" fill=\"%5\"%6%7%8>%9</tspan>")
                            .arg(esc(s.family),num(s.size),s.weight==QLatin1String("bold")?QStringLiteral("bold"):QStringLiteral("normal"),
                                 s.italic==QLatin1String("italic")?QStringLiteral("italic"):QStringLiteral("normal"),colorStr(s.color,cx),
                                 al2<1?QStringLiteral(" fill-opacity=\"%1\"").arg(num(al2)):QString(),
                                 s.decoration==QLatin1String("underline")?QStringLiteral(" text-decoration=\"underline\""):QString(),
                                 QString(),esc(tx));
                        a=b;
                    }
                    out+=QStringLiteral("</text>\n");
                }
                firstLine=false;
            }
            y+=po.p.after;
        }
        if(out.isEmpty()) return;
        note(QRectF(0,0,qMax(boxW,maxW),qMax(boxH,y)), local*ctm);
        body+=QStringLiteral("<g transform=\"%1\"%2>\n%3</g>\n").arg(svgMatrix(local),commonAttrs(n),out);
    }
    static bool sameStyle(const TStyle& a, const TStyle& b){
        return a.family==b.family && a.size==b.size && a.weight==b.weight && a.italic==b.italic && a.color==b.color && a.alpha==b.alpha && a.decoration==b.decoration && a.tracking==b.tracking;
    }
};

} // namespace

QString fxgToSvg(const QByteArray& data, const QString& baseDir){
    std::unique_ptr<Node> root=parseXml(data);
    if(!root || root->name!=QLatin1String("Graphic")) return QString();
    Conv c; c.baseDir=baseDir;
    // A Library holds Definitions; each is placed by writing its name as an element.
    if(const Node* lib=root->child("Library"))
        for(const auto& k : lib->kids) if(k->name=="Definition" && k->attr.contains("name")){
            const Node* g=k->child("Group");
            if(g) c.defs.insert(k->a("name"), g);
        }
    QString clip;
    const QTransform I;
    // Top-level <mask> is the view's own clip.
    if(const Node* m=root->child("mask")){
        QString inner; { Conv mc; mc.baseDir=baseDir; mc.defs=c.defs; for(const auto& k : m->kids) mc.element(*k,I,ColorXf(),0); inner=mc.body; c.defsOut+=mc.defsOut; c.nextId+=0; }
        if(!inner.trimmed().isEmpty()){ c.defsOut+=QStringLiteral("<clipPath id=\"viewclip\">%1</clipPath>\n").arg(inner); clip=QStringLiteral(" clip-path=\"url(#viewclip)\""); }
    }
    for(const auto& k : root->kids){
        const QString& t=k->name;
        if(t=="Library"||t=="mask"||t=="Private"||t=="filters"||t=="transform") continue;
        c.element(*k, I, ColorXf(), 0);
    }
    double w=root->d("viewWidth"), h=root->d("viewHeight");
    double x0=0, y0=0;
    if(w<=0||h<=0){
        if(c.bounds.isNull()) return QString();
        x0=c.bounds.left(); y0=c.bounds.top(); w=c.bounds.width(); h=c.bounds.height();
    }
    if(c.body.trimmed().isEmpty() && c.bounds.isNull()) return QString();
    return QStringLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xarq=\"urn:xarq\" width=\"%1\" height=\"%2\" viewBox=\"%3 %4 %5 %6\">\n<defs>\n%7</defs>\n<g%8>\n%9</g>\n</svg>\n")
        .arg(num(w),num(h),num(x0),num(y0),num(w),num(h),c.defsOut,clip,c.body);
}

} // namespace xara
