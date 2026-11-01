// Adobe XD → SVG. An .xd is a ZIP: a manifest listing the artboards, one graphicContent.agc (JSON scene
// graph) per artboard, shared gradients / clip paths / symbols in resources/graphics, images in resources/.
// Artboard contents are written in the pasteboard's coordinates, so each page shifts by its artboard's
// origin. Each artboard takes a page; since a XarQ document has one page size, all pages use the largest.
#include "xdvector.h"
#include "svgimport.h"
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainterPath>
#include <QTransform>
#include <QHash>
#include <QtMath>
#include <algorithm>

namespace xara {
namespace {

QString num(double v){ return QString::number(v,'g',12); }
QString esc(QString s){ s.replace('&',"&amp;"); s.replace('<',"&lt;"); s.replace('>',"&gt;"); s.replace('"',"&quot;"); return s; }

struct Rgba { int r=0,g=0,b=0; double a=1; };
Rgba colorOf(const QJsonObject& c){
    Rgba o; const QJsonObject v=c.value("value").toObject();
    o.r=qBound(0,v.value("r").toInt(),255); o.g=qBound(0,v.value("g").toInt(),255); o.b=qBound(0,v.value("b").toInt(),255);
    o.a=c.contains("alpha")? qBound(0.0,c.value("alpha").toDouble(1),1.0) : 1.0;
    return o;
}
QString hex(const Rgba& c){ return QStringLiteral("#%1%2%3").arg(c.r,2,16,QLatin1Char('0')).arg(c.g,2,16,QLatin1Char('0')).arg(c.b,2,16,QLatin1Char('0')); }
QTransform xfOf(const QJsonObject& n){
    const QJsonObject t=n.value("transform").toObject();
    if(t.isEmpty()) return QTransform();
    return QTransform(t.value("a").toDouble(1),t.value("b").toDouble(0),t.value("c").toDouble(0),t.value("d").toDouble(1),t.value("tx").toDouble(0),t.value("ty").toDouble(0));
}
QString svgMatrix(const QTransform& t){
    return QStringLiteral("matrix(%1 %2 %3 %4 %5 %6)").arg(num(t.m11()),num(t.m12()),num(t.m21()),num(t.m22()),num(t.dx()),num(t.dy()));
}
int weightOf(const QString& style){
    const QString s=style.toLower().remove(' ').remove('-');
    if(s.contains("thin")||s.contains("hairline")) return 100;
    if(s.contains("extralight")||s.contains("ultralight")) return 200;
    if(s.contains("light")) return 300;
    if(s.contains("medium")) return 500;
    if(s.contains("semibold")||s.contains("demibold")) return 600;
    if(s.contains("extrabold")||s.contains("ultrabold")) return 800;
    if(s.contains("black")||s.contains("heavy")) return 900;
    if(s.contains("bold")) return 700;
    return 400;
}
const int kMaxDepth=200;

struct Geom { QString open; QRectF box; QPainterPath path; };   // open = the element, attributes still open

class Conv {
public:
    const QMap<QString,QByteArray>& zip;
    QJsonObject gradients, clipPaths;
    QHash<QString,QJsonObject> symbols;
    QString defs;
    int nextId=0;
    QRectF bounds;

    explicit Conv(const QMap<QString,QByteArray>& z) : zip(z) {}

    void children(const QJsonArray& kids, const QTransform& ctm, QString& out, int depth){
        for(const QJsonValue& v : kids) node(v.toObject(), ctm, out, depth+1);
    }

    void note(const QRectF& r, const QTransform& ctm){
        const QRectF d=ctm.mapRect(r);
        if(bounds.isNull()) bounds=d;
        else { const double x0=qMin(bounds.left(),d.left()), y0=qMin(bounds.top(),d.top()), x1=qMax(bounds.right(),d.right()), y1=qMax(bounds.bottom(),d.bottom());
               bounds=QRectF(x0,y0,x1-x0,y1-y0); }
    }
    QString fillOnly(const QJsonObject& fill, const QRectF& b){
        const QString t=fill.value("type").toString();
        if(t=="solid"){
            const Rgba c=colorOf(fill.value("color").toObject());
            return QStringLiteral(" fill=\"%1\"%2").arg(hex(c), c.a<1?QStringLiteral(" fill-opacity=\"%1\"").arg(num(c.a)):QString());
        }
        if(t=="gradient"){ const QString id=gradient(fill.value("gradient").toObject()); if(!id.isEmpty()) return QStringLiteral(" fill=\"url(#%1)\"").arg(id); }
        if(t=="pattern") return patternFill(fill.value("pattern").toObject(), b);
        return QStringLiteral(" fill=\"none\"");
    }
    QString paintUrl(const QJsonObject& s){
        const QString t=s.value("type").toString();
        if(t=="solid"){
            const Rgba c=colorOf(s.value("color").toObject());
            return QStringLiteral(" stroke=\"%1\"%2").arg(hex(c), c.a<1?QStringLiteral(" stroke-opacity=\"%1\"").arg(num(c.a)):QString());
        }
        if(t=="gradient"){ const QString id=gradient(s.value("gradient").toObject()); if(!id.isEmpty()) return QStringLiteral(" stroke=\"url(#%1)\"").arg(id); }
        return QString();
    }

    void node(const QJsonObject& n, const QTransform& ctm, QString& out, int depth){
        if(depth>kMaxDepth || n.value("visible")==QJsonValue(false)) return;
        const QString t=n.value("type").toString();
        const QTransform local=xfOf(n), inner=local*ctm;
        const QJsonObject style=n.value("style").toObject();
        if(t=="group"||t=="syncRef"){
            QJsonArray kids=n.value("group").toObject().value("children").toArray();
            if(kids.isEmpty()){   // an instance with nothing inline takes the symbol master's
                const QString sid=n.value("syncSourceGuid").toString();
                QString key=sid; if(key.isEmpty()) key=n.value("meta").toObject().value("ux").toObject().value("symbolId").toString();
                if(symbols.contains(key)) kids=symbols.value(key).value("group").toObject().value("children").toArray();
            }
            QString body; children(kids, inner, body, depth);
            if(body.isEmpty()) return;
            out+=QStringLiteral("<g transform=\"%1\"%2>\n%3</g>\n").arg(svgMatrix(local), groupAttrs(style, depth), body);
        } else if(t=="shape"){
            shape(n, style, local, ctm, out, depth);
        } else if(t=="text"){
            text(n, style, local, ctm, out, depth);
        }
    }

private:
    QString newId(const char* b){ return QStringLiteral("%1%2").arg(QLatin1String(b)).arg(++nextId); }

    QString effects(const QJsonObject& style){
        QString s;
        for(const QJsonValue& fv : style.value("filters").toArray()){
            const QJsonObject f=fv.toObject();
            if(f.value("visible")==QJsonValue(false)) continue;
            const QString ty=f.value("type").toString();
            const QJsonObject p=f.value("params").toObject();
            if(ty=="dropShadow"){
                const QJsonArray ds=p.value("dropShadows").toArray();
                if(!ds.isEmpty() && !s.contains("xarq:shadow")){
                    const QJsonObject d=ds.at(0).toObject(); const Rgba c=colorOf(d.value("color").toObject());
                    s+=QStringLiteral(" xarq:shadow=\"%1 %2 %3 %4 %5 0\"").arg(num(d.value("dx").toDouble()),num(d.value("dy").toDouble()),num(d.value("r").toDouble()),hex(c),num(c.a));
                }
            } else if(ty=="uxdesign#blur" && !p.value("backgroundEffect").toBool()){
                s+=QStringLiteral(" xarq:blur=\"%1\"").arg(num(p.value("blurAmount").toDouble()*0.9));
            }
        }
        return s;
    }
    // Background blur reads what lies behind, which the model has no way to do; keep the shape's see-through amount.
    double backgroundFillOpacity(const QJsonObject& style){
        for(const QJsonValue& fv : style.value("filters").toArray()){
            const QJsonObject f=fv.toObject();
            if(f.value("visible")==QJsonValue(false) || f.value("type").toString()!="uxdesign#blur") continue;
            const QJsonObject p=f.value("params").toObject();
            if(p.value("backgroundEffect").toBool() && p.contains("fillOpacity")) return qBound(0.0,p.value("fillOpacity").toDouble(1),1.0);
        }
        return 1.0;
    }
    QString groupAttrs(const QJsonObject& style, int depth){
        QString s;
        if(style.contains("opacity") && style.value("opacity").toDouble(1)<1) s+=QStringLiteral(" opacity=\"%1\"").arg(num(qBound(0.0,style.value("opacity").toDouble(1),1.0)));
        const QString bm=style.value("blendMode").toString();
        if(!bm.isEmpty() && bm!="normal" && bm!="pass-through") s+=QStringLiteral(" style=\"mix-blend-mode:%1\"").arg(esc(bm));
        s+=effects(style);
        const QString ref=style.value("clipPath").toObject().value("ref").toString();
        if(!ref.isEmpty()) s+=clipAttr(clipPaths.value(ref).toObject().value("children").toArray(), depth);
        return s;
    }
    QString clipAttr(const QJsonArray& kids, int depth){
        if(kids.isEmpty()) return QString();
        QString inner; const QRectF saved=bounds;
        children(kids, QTransform(), inner, depth);
        bounds=saved;
        if(inner.isEmpty()) return QString();
        const QString id=newId("clip");
        defs+=QStringLiteral("<clipPath id=\"%1\">%2</clipPath>\n").arg(id,inner);
        return QStringLiteral(" clip-path=\"url(#%1)\"").arg(id);
    }

    QString gradient(const QJsonObject& g){
        const QString ref=g.value("ref").toString();
        const QJsonObject res=gradients.value(ref).toObject();
        if(res.isEmpty()) return QString();
        const bool radial=res.value("type").toString()=="radial";
        QString st;
        for(const QJsonValue& sv : res.value("stops").toArray()){
            const QJsonObject so=sv.toObject(); const Rgba c=colorOf(so.value("color").toObject());
            st+=QStringLiteral("<stop offset=\"%1\" stop-color=\"%2\" stop-opacity=\"%3\"/>").arg(num(so.value("offset").toDouble()),hex(c),num(c.a));
        }
        const QString units=g.value("units").toString()=="userSpaceOnUse"? QStringLiteral("userSpaceOnUse"):QStringLiteral("objectBoundingBox");
        const QString id=newId("grad");
        if(radial){
            QString extra;
            if(g.contains("fx")) extra+=QStringLiteral(" fx=\"%1\"").arg(num(g.value("fx").toDouble()));
            if(g.contains("fy")) extra+=QStringLiteral(" fy=\"%1\"").arg(num(g.value("fy").toDouble()));
            const QJsonObject m=g.value("transform").toObject();
            if(!m.isEmpty()) extra+=QStringLiteral(" gradientTransform=\"matrix(%1 %2 %3 %4 %5 %6)\"").arg(num(m.value("a").toDouble(1)),num(m.value("b").toDouble()),num(m.value("c").toDouble()),num(m.value("d").toDouble(1)),num(m.value("tx").toDouble()),num(m.value("ty").toDouble()));
            defs+=QStringLiteral("<radialGradient id=\"%1\" gradientUnits=\"%2\" cx=\"%3\" cy=\"%4\" r=\"%5\"%6>%7</radialGradient>\n")
                .arg(id,units,num(g.value("cx").toDouble(0.5)),num(g.value("cy").toDouble(0.5)),num(g.value("r").toDouble(0.5)),extra,st);
        } else {
            defs+=QStringLiteral("<linearGradient id=\"%1\" gradientUnits=\"%2\" x1=\"%3\" y1=\"%4\" x2=\"%5\" y2=\"%6\">%7</linearGradient>\n")
                .arg(id,units,num(g.value("x1").toDouble()),num(g.value("y1").toDouble()),num(g.value("x2").toDouble(1)),num(g.value("y2").toDouble()),st);
        }
        return id;
    }

    // An image fill: "cover" keeps the aspect and crops, centred on the shape; "fill" stretches it.
    QString patternFill(const QJsonObject& pat, const QRectF& b){
        const QJsonObject ux=pat.value("meta").toObject().value("ux").toObject();
        const QByteArray bytes=zip.value(QStringLiteral("resources/")+ux.value("uid").toString());
        if(bytes.isEmpty()) return QStringLiteral(" fill=\"#c8c8c8\"");
        double w=pat.value("width").toDouble(), h=pat.value("height").toDouble();
        if(w<=0||h<=0){ QImage im; im.loadFromData(bytes); w=im.width(); h=im.height(); }
        if(w<=0||h<=0||b.isEmpty()) return QStringLiteral(" fill=\"#c8c8c8\"");
        const bool cover=ux.value("scaleBehavior").toString()!="fill";
        const double user=ux.contains("scale")? qMax(0.001,ux.value("scale").toDouble(1)) : 1.0;
        double sx= cover? qMax(b.width()/w,b.height()/h)*user : b.width()/w, sy= cover? sx : b.height()/h;
        double x=b.left()+(b.width()-w*sx)/2 + ux.value("offsetX").toDouble()*b.width();
        double y=b.top()+(b.height()-h*sy)/2 + ux.value("offsetY").toDouble()*b.height();
        QPointF p0(x,y+h*sy), p1(x+w*sx,y+h*sy), p2(x,y);
        if(ux.value("flipX").toBool()){ std::swap(p0,p1); p2=QPointF(x+w*sx,y); }
        if(ux.value("flipY").toBool()){ const QPointF px=p1-p0; p2=p0; p0=p0+QPointF(0,-h*sy); p1=p0+px; }
        const char* mime= bytes.startsWith("\xff\xd8")?"image/jpeg": bytes.startsWith("GIF")?"image/gif":"image/png";
        return QStringLiteral(" fill=\"#c8c8c8\" xarq:bitmap=\"data:%1;base64,%2\" xarq:bitmap-xf=\"%3 %4 %5 %6 %7 %8\"")
            .arg(QLatin1String(mime),QString::fromLatin1(bytes.toBase64()))
            .arg(num(p1.x()-p0.x()),num(p1.y()-p0.y()),num(p2.x()-p0.x()),num(p2.y()-p0.y()),num(p0.x()),num(p0.y()));
    }

    bool geometry(const QJsonObject& sh, Geom& g){
        const QString t=sh.value("type").toString();
        if(t=="rect"){
            const double x=sh.value("x").toDouble(), y=sh.value("y").toDouble(), w=sh.value("width").toDouble(), h=sh.value("height").toDouble();
            if(w<=0||h<=0) return false;
            g.box=QRectF(x,y,w,h);
            double r[4]={0,0,0,0};
            const QJsonValue rv=sh.value("r");
            if(rv.isArray()){ const QJsonArray a=rv.toArray(); for(int i=0;i<4&&i<a.size();++i) r[i]=qBound(0.0,a.at(i).toDouble(),qMin(w,h)/2); }
            else if(rv.isDouble()) r[0]=r[1]=r[2]=r[3]=qBound(0.0,rv.toDouble(),qMin(w,h)/2);
            if(r[0]==r[1]&&r[1]==r[2]&&r[2]==r[3]){
                g.open=QStringLiteral("<rect x=\"%1\" y=\"%2\" width=\"%3\" height=\"%4\"").arg(num(x),num(y),num(w),num(h));
                if(r[0]>0) g.open+=QStringLiteral(" rx=\"%1\" ry=\"%1\"").arg(num(r[0]));
            } else {
                g.open=QStringLiteral("<path d=\"M%1 %2 L%3 %2 A%4 %4 0 0 1 %5 %6 L%5 %7 A%8 %8 0 0 1 %9 %10 ")
                    .arg(num(x+r[0]),num(y),num(x+w-r[1]),num(r[1]),num(x+w),num(y+r[1]),num(y+h-r[2]),num(r[2]),num(x+w-r[2]))
                    .arg(num(y+h));
                g.open+=QStringLiteral("L%1 %2 A%3 %3 0 0 1 %4 %5 L%4 %6 A%7 %7 0 0 1 %8 %9 Z\"")
                    .arg(num(x+r[3]),num(y+h),num(r[3]),num(x),num(y+h-r[3]),num(y+r[0]),num(r[0]),num(x+r[0]),num(y));
            }
            return true;
        }
        if(t=="circle"){
            const double cx=sh.value("cx").toDouble(), cy=sh.value("cy").toDouble(), r=sh.value("r").toDouble();
            if(r<=0) return false;
            g.box=QRectF(cx-r,cy-r,2*r,2*r);
            g.open=QStringLiteral("<circle cx=\"%1\" cy=\"%2\" r=\"%3\"").arg(num(cx),num(cy),num(r));
            return true;
        }
        if(t=="ellipse"){
            const double cx=sh.value("cx").toDouble(), cy=sh.value("cy").toDouble(), rx=sh.value("rx").toDouble(), ry=sh.value("ry").toDouble();
            if(rx<=0||ry<=0) return false;
            g.box=QRectF(cx-rx,cy-ry,2*rx,2*ry);
            g.open=QStringLiteral("<ellipse cx=\"%1\" cy=\"%2\" rx=\"%3\" ry=\"%4\"").arg(num(cx),num(cy),num(rx),num(ry));
            return true;
        }
        if(t=="line"){
            const double x1=sh.value("x1").toDouble(), y1=sh.value("y1").toDouble(), x2=sh.value("x2").toDouble(), y2=sh.value("y2").toDouble();
            g.box=QRectF(QPointF(x1,y1),QPointF(x2,y2)).normalized();
            g.open=QStringLiteral("<line x1=\"%1\" y1=\"%2\" x2=\"%3\" y2=\"%4\"").arg(num(x1),num(y1),num(x2),num(y2));
            return true;
        }
        QString d=sh.value("path").toString();
        if(d.isEmpty() && sh.contains("points")){
            const QJsonArray pts=sh.value("points").toArray();
            for(int i=0;i<pts.size();++i){ const QJsonObject p=pts.at(i).toObject(); d+=QStringLiteral("%1%2 %3 ").arg(i?"L":"M",num(p.value("x").toDouble()),num(p.value("y").toDouble())); }
            if(!d.isEmpty()) d+=QStringLiteral("Z");
        }
        if(d.isEmpty()) return false;
        g.box=parseSvgPathData(d).boundingRect();
        const QString w=sh.value("winding").toString();
        g.open=QStringLiteral("<path d=\"%1\"%2").arg(esc(d), w=="evenodd"?QStringLiteral(" fill-rule=\"evenodd\""):QString());
        return true;
    }

    void shape(const QJsonObject& n, const QJsonObject& style, const QTransform& local, const QTransform& ctm, QString& out, int depth){
        Geom g; if(!geometry(n.value("shape").toObject(), g)) return;
        note(g.box, local*ctm);
        const QJsonObject st=style.value("stroke").toObject();
        const bool hasStroke=st.value("type").toString()!="none" && !st.isEmpty() && st.value("width").toDouble(1)>0;
        QString fill=fillOnly(style.value("fill").toObject(), g.box);
        const double bfo=backgroundFillOpacity(style);
        if(bfo<1 && !fill.contains("fill-opacity")) fill+=QStringLiteral(" fill-opacity=\"%1\"").arg(num(bfo));
        QString strokeCommon;
        double w=0;
        if(hasStroke){
            w=st.value("width").toDouble(1);
            const QJsonArray dash=st.value("dash").toArray();
            if(!dash.isEmpty()){ QStringList ds; for(const QJsonValue& v : dash) ds<<num(v.toDouble()); strokeCommon+=QStringLiteral(" stroke-dasharray=\"%1\"").arg(ds.join(' ')); }
            const QString cap=st.value("cap").toString(), join=st.value("join").toString();
            if(cap=="round"||cap=="square"||cap=="butt") strokeCommon+=QStringLiteral(" stroke-linecap=\"%1\"").arg(cap);
            if(join=="round"||join=="bevel"||join=="miter") strokeCommon+=QStringLiteral(" stroke-linejoin=\"%1\"").arg(join);
            if(st.contains("miterLimit")) strokeCommon+=QStringLiteral(" stroke-miterlimit=\"%1\"").arg(num(qMax(1.0,st.value("miterLimit").toDouble())));
        }
        const QString strokeUrl=hasStroke? paintUrl(st) : QString();
        const QString align=st.value("align").toString();
        const QString attrs=groupAttrs(style, depth);
        const QString xf=svgMatrix(local);
        const bool isLine=n.value("shape").toObject().value("type").toString()=="line";
        if(strokeUrl.isEmpty() || isLine || align.isEmpty() || align=="center"){
            out+=g.open+QStringLiteral(" transform=\"%1\"%2%3%4%5/>\n")
                .arg(xf, fill, strokeUrl.isEmpty()?QStringLiteral(" stroke=\"none\""):strokeUrl+QStringLiteral(" stroke-width=\"%1\"").arg(num(w))+strokeCommon, attrs, QString());
        } else if(align=="inside"){
            // SVG strokes straddle the outline: draw it twice as wide and let the shape cut off the outer half.
            const QString id=newId("clip");
            defs+=QStringLiteral("<clipPath id=\"%1\">%2 transform=\"%3\"/></clipPath>\n").arg(id,g.open,xf);
            out+=g.open+QStringLiteral(" transform=\"%1\"%2 stroke=\"none\"%3/>\n").arg(xf,fill,attrs);
            out+=g.open+QStringLiteral(" transform=\"%1\" fill=\"none\"%2 stroke-width=\"%3\"%4 clip-path=\"url(#%5)\"%6/>\n").arg(xf,strokeUrl,num(2*w),strokeCommon,id,attrs);
        } else {
            // Outside: the doubled stroke goes under the fill, which hides the inner half.
            out+=g.open+QStringLiteral(" transform=\"%1\" fill=\"none\"%2 stroke-width=\"%3\"%4%5/>\n").arg(xf,strokeUrl,num(2*w),strokeCommon,attrs);
            out+=g.open+QStringLiteral(" transform=\"%1\"%2 stroke=\"none\"%3/>\n").arg(xf,fill,attrs);
        }
    }

    struct Run { QString family; QString style; double size=12, spacing=0; bool underline=false, strike=false; QString transform; Rgba color; bool hasColor=false; };
    static void readFont(const QJsonObject& s, Run& r){
        const QJsonObject f=s.value("font").toObject();
        if(f.contains("family")) r.family=f.value("family").toString();
        if(f.contains("style"))  r.style=f.value("style").toString();
        if(f.contains("size"))   r.size=f.value("size").toDouble(12);
        if(s.contains("fill")){
            const QJsonObject fl=s.value("fill").toObject();
            if(fl.value("type").toString()=="solid"){ r.color=colorOf(fl.value("color").toObject()); r.hasColor=true; }
        }
        const QJsonObject ta=s.value("textAttributes").toObject();
        if(ta.contains("letterSpacing")) r.spacing=ta.value("letterSpacing").toDouble()/1000.0*r.size;
        const QString dec=ta.value("decoration").toString();
        if(dec=="underline") r.underline=true;
    }

    void text(const QJsonObject& n, const QJsonObject& style, const QTransform& local, const QTransform& ctm, QString& out, int depth){
        const QJsonObject tx=n.value("text").toObject();
        const QString raw=tx.value("rawText").toString();
        if(raw.isEmpty()) return;
        Run base; base.color=Rgba(); readFont(style, base);
        // rangedStyles are what the designer chose; each line's own font can be a stand-in XD used on the
        // machine that last saved it. Only trust the line's font when there are no ranges.
        struct Range { int from,to; Run r; };
        QVector<Range> ranges;
        const QJsonArray rs=n.value("meta").toObject().value("ux").toObject().value("rangedStyles").toArray();
        int cursor=0;
        for(const QJsonValue& v : rs){
            const QJsonObject o=v.toObject(); Range rg; rg.from=cursor; rg.to=cursor+qMax(0,o.value("length").toInt());
            rg.r=base;
            if(o.contains("fontFamily")) rg.r.family=o.value("fontFamily").toString();
            if(o.contains("fontStyle"))  rg.r.style=o.value("fontStyle").toString();
            if(o.contains("fontSize"))   rg.r.size=o.value("fontSize").toDouble(12);
            if(o.contains("charSpacing")) rg.r.spacing=o.value("charSpacing").toDouble()/1000.0*rg.r.size;
            if(o.contains("underline"))  rg.r.underline=o.value("underline").toBool();
            if(o.contains("strikethrough")) rg.r.strike=o.value("strikethrough").toBool();
            if(o.contains("textTransform")) rg.r.transform=o.value("textTransform").toString();
            if(o.value("fill").toObject().contains("value")){
                const quint32 argb=quint32(o.value("fill").toObject().value("value").toDouble());
                rg.r.color.r=(argb>>16)&255; rg.r.color.g=(argb>>8)&255; rg.r.color.b=argb&255; rg.r.color.a=((argb>>24)&255)/255.0; rg.r.hasColor=true;
            }
            cursor=rg.to; ranges.push_back(rg);
        }
        const QJsonObject frame=tx.value("frame").toObject();
        const bool area=frame.value("type").toString()=="area"||frame.contains("width");
        const double fw=frame.value("width").toDouble();
        const QString al=style.value("textAttributes").toObject().value("paragraphAlign").toString();
        QString anchor; double anchorX=0;
        if(al=="center"){ anchor=QStringLiteral("middle"); anchorX=area?fw/2:0; }
        else if(al=="right"){ anchor=QStringLiteral("end"); anchorX=area?fw:0; }
        const double op=style.contains("opacity")? style.value("opacity").toDouble(1) : 1.0;
        QString body; QRectF tb;
        for(const QJsonValue& pv : tx.value("paragraphs").toArray()){
            for(const QJsonValue& lv : pv.toObject().value("lines").toArray()){
                const QJsonArray pieces=lv.toArray();
                if(pieces.isEmpty()) continue;
                const QJsonObject first=pieces.at(0).toObject();
                const double lx=first.value("x").toDouble(), ly=first.value("y").toDouble();
                QString spans; double maxSize=0, lineSpacing=0; int chars=0;
                for(const QJsonValue& gv : pieces){
                    const QJsonObject g=gv.toObject();
                    int from=qBound(0,g.value("from").toInt(),raw.size()), to=qBound(from,g.value("to").toInt(),raw.size());
                    Run gr=base; if(ranges.isEmpty()) readFont(g.value("style").toObject(), gr);
                    int i=from;
                    while(i<to){
                        // Split the piece wherever the ranged style changes.
                        const Run* rr=&gr; int stop=to;
                        for(const Range& rg : ranges) if(i>=rg.from && i<rg.to){ rr=&rg.r; stop=qMin(stop,rg.to); break; }
                        QString seg=raw.mid(i,stop-i); i=stop;
                        seg.remove(QLatin1Char('\n')); seg.remove(QLatin1Char('\r'));
                        if(seg.isEmpty()) continue;
                        if(seg.trimmed().isEmpty()) seg.replace(QLatin1Char(' '),QChar(0x00A0));   // the SVG route drops a run of bare spaces
                        if(spans.isEmpty()) lineSpacing=rr->spacing;
                        if(rr->transform=="uppercase") seg=seg.toUpper();
                        else if(rr->transform=="lowercase") seg=seg.toLower();
                        chars+=seg.size(); maxSize=qMax(maxSize,rr->size);
                        const QString lower=rr->style.toLower();
                        const bool italic=lower.contains("italic")||lower.contains("oblique");
                        QString dec; if(rr->underline) dec=QStringLiteral(" text-decoration=\"underline\""); else if(rr->strike) dec=QStringLiteral(" text-decoration=\"line-through\"");
                        spans+=QStringLiteral("<tspan font-family=\"%1\" font-size=\"%2\" font-weight=\"%3\" font-style=\"%4\" fill=\"%5\"%6%7%8>%9</tspan>")
                            .arg(esc(rr->family),num(rr->size)).arg(weightOf(rr->style)).arg(italic?"italic":"normal",hex(rr->color),
                                 rr->color.a<1?QStringLiteral(" fill-opacity=\"%1\"").arg(num(rr->color.a)):QString(),
                                 QString(),dec,esc(seg));
                    }
                }
                if(spans.isEmpty()) continue;
                const double x=anchor.isEmpty()? lx : anchorX;
                // Tracking belongs to the whole text object here, so the line takes its first run's.
                body+=QStringLiteral("<text xml:space=\"preserve\" x=\"%1\" y=\"%2\"%3%4>%5</text>\n")
                    .arg(num(x),num(ly),anchor.isEmpty()?QString():QStringLiteral(" text-anchor=\"%1\"").arg(anchor),
                         lineSpacing!=0?QStringLiteral(" letter-spacing=\"%1\"").arg(num(lineSpacing)):QString(),spans);
                const QRectF lineBox(lx,ly-maxSize,maxSize*0.6*chars,maxSize*1.2);
                tb=tb.isNull()?lineBox:tb.united(lineBox);
            }
        }
        if(body.isEmpty()) return;
        if(!tb.isNull()) note(tb, local*ctm);
        Q_UNUSED(op);
        out+=QStringLiteral("<g transform=\"%1\"%2>\n%3</g>\n").arg(svgMatrix(local),groupAttrs(style,depth),body);
    }
};

} // namespace

QStringList xdToSvgPages(const QMap<QString,QByteArray>& zip, XdStats* stats){
    QJsonParseError pe;
    const QJsonObject res=QJsonDocument::fromJson(zip.value("resources/graphics/graphicContent.agc"),&pe).object();
    const QJsonObject artboards=res.value("artboards").toObject();
    const QJsonObject resR=res.value("resources").toObject();
    const QJsonObject manifest=QJsonDocument::fromJson(zip.value("manifest")).object();
    if(manifest.isEmpty() && !zip.contains("mimetype")) return {};

    // The manifest keeps the designer's artboard order; its bounds are the fallback for an older file.
    struct Ab { QString dir, name; QRectF box; };
    QVector<Ab> abs;
    for(const QJsonValue& cv : manifest.value("children").toArray()){
        const QJsonObject c=cv.toObject();
        if(c.value("path").toString()!="artwork") continue;
        for(const QJsonValue& av : c.value("children").toArray()){
            const QJsonObject a=av.toObject();
            const QString p=a.value("path").toString();
            if(!p.startsWith("artboard")) continue;
            Ab ab; ab.dir=p; ab.name=a.value("name").toString();
            const QJsonObject b=a.value("uxdesign#bounds").toObject();
            ab.box=QRectF(b.value("x").toDouble(),b.value("y").toDouble(),b.value("width").toDouble(),b.value("height").toDouble());
            abs.push_back(ab);
        }
    }
    if(abs.isEmpty()){
        for(auto it=zip.begin(); it!=zip.end(); ++it){
            const QStringList parts=it.key().split('/');
            if(parts.size()>=3 && parts[0]=="artwork" && parts[1].startsWith("artboard") && it.key().endsWith("graphicContent.agc")){
                Ab ab; ab.dir=parts[1]; ab.name=parts[1]; abs.push_back(ab); }
        }
    }

    Conv conv(zip);
    conv.gradients=resR.value("gradients").toObject();
    conv.clipPaths=resR.value("clipPaths").toObject();
    for(const QJsonValue& sv : resR.value("meta").toObject().value("ux").toObject().value("symbols").toArray()){
        const QJsonObject so=sv.toObject();
        const QString key=so.value("meta").toObject().value("ux").toObject().value("symbolId").toString();
        if(!key.isEmpty()) conv.symbols.insert(key,so);
    }

    struct Pg { QString name, body; QRectF box; QRectF content; };
    QVector<Pg> pages;
    for(const Ab& ab : abs){
        const QJsonObject doc=QJsonDocument::fromJson(zip.value(QStringLiteral("artwork/%1/graphics/graphicContent.agc").arg(ab.dir))).object();
        const QJsonArray top=doc.value("children").toArray();
        if(top.isEmpty()) continue;
        Pg pg; pg.name=ab.name; pg.box=ab.box;
        for(const QJsonValue& tv : top){
            const QJsonObject t=tv.toObject();
            const QJsonObject ar=t.value("artboard").toObject();
            const QString ref=ar.value("ref").toString();
            if(artboards.contains(ref)){
                const QJsonObject m=artboards.value(ref).toObject();
                pg.box=QRectF(m.value("x").toDouble(),m.value("y").toDouble(),m.value("width").toDouble(),m.value("height").toDouble());
                if(m.contains("name")) pg.name=m.value("name").toString();
            }
            conv.bounds=QRectF();
            // The artboard's own fill sits behind everything on it.
            const QJsonObject style=t.value("style").toObject();
            if(!pg.box.isEmpty()){
                const QJsonObject fill=style.value("fill").toObject();
                if(!fill.isEmpty() && fill.value("type").toString()!="none")
                    pg.body+=QStringLiteral("<rect x=\"%1\" y=\"%2\" width=\"%3\" height=\"%4\" stroke=\"none\"%5/>\n")
                        .arg(num(pg.box.left()),num(pg.box.top()),num(pg.box.width()),num(pg.box.height()),conv.fillOnly(fill,pg.box));
            }
            conv.children(ar.value("children").toArray(), QTransform(), pg.body, 0);
            pg.content=conv.bounds;
        }
        if(pg.box.isEmpty() && !pg.content.isNull()) pg.box=pg.content;
        if(pg.box.isEmpty()) continue;
        pages.push_back(pg);
    }

    int pasteboardObjects=0;
    const QJsonObject pb=QJsonDocument::fromJson(zip.value("artwork/pasteboard/graphics/graphicContent.agc")).object();
    const QJsonArray pbKids=pb.value("children").toArray();
    pasteboardObjects=pbKids.size();
    if(pages.isEmpty() && !pbKids.isEmpty()){     // nothing on an artboard: the pasteboard is the drawing
        Pg pg; pg.name=QStringLiteral("Pasteboard");
        conv.bounds=QRectF();
        conv.children(pbKids, QTransform(), pg.body, 0);
        pg.box=conv.bounds;
        if(!pg.box.isEmpty()) pages.push_back(pg);
        pasteboardObjects=0;
    }
    if(pages.isEmpty()) return {};

    double W=0,H=0;
    for(const Pg& p : pages){ W=qMax(W,p.box.width()); H=qMax(H,p.box.height()); }
    QStringList out;
    if(stats){ stats->artboards=pages.size(); stats->pasteboardObjects=pasteboardObjects; }
    for(int i=0;i<pages.size();++i){
        const Pg& p=pages[i];
        if(stats) stats->names<<p.name;
        // An artboard shows only what's on it, so anything hanging over its edge is cut there.
        const bool overflow=!p.content.isNull() && !p.box.adjusted(-0.5,-0.5,0.5,0.5).contains(p.content);
        QString clipDef, clipAttr;
        if(overflow){
            const QString id=QStringLiteral("board%1").arg(i);
            clipDef=QStringLiteral("<clipPath id=\"%1\"><rect x=\"%2\" y=\"%3\" width=\"%4\" height=\"%5\"/></clipPath>\n").arg(id,num(p.box.left()),num(p.box.top()),num(p.box.width()),num(p.box.height()));
            clipAttr=QStringLiteral(" clip-path=\"url(#%1)\"").arg(id);
        }
        out<<QStringLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xarq=\"urn:xarq\" width=\"%1\" height=\"%2\" viewBox=\"0 0 %1 %2\">\n<defs>\n%3%4</defs>\n<g transform=\"translate(%5 %6)\"%7>\n%8</g>\n</svg>\n")
            .arg(num(W),num(H),conv.defs,clipDef,num(-p.box.left()),num(-p.box.top()),clipAttr,p.body);
    }
    return out;
}

} // namespace xara
