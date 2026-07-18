#include "svgimport.h"
#include <QXmlStreamReader>
#include <QRegularExpression>
#include <QColor>
#include <QHash>
#include <QSet>
#include <QtMath>
#include <QPainterPath>
#include <QDir>
#include <QUrl>
#include <algorithm>
#include <functional>

namespace xara {
namespace {

double num(const QString& s) {   // leading number + optional unit → px (em/pt/pc/mm/cm/in scaled at 96dpi; %/px/unitless kept)
    int i=0, n=s.size(); if(i<n && (s[i]=='+'||s[i]=='-')) ++i;
    while(i<n && s[i].isDigit()) ++i;
    if(i<n && s[i]=='.'){ ++i; while(i<n && s[i].isDigit()) ++i; }
    // exponent — only when 'e' is followed by a digit/sign, so "2em" is a unit, not a broken exponent
    if(i<n && (s[i]=='e'||s[i]=='E') && i+1<n && (s[i+1].isDigit()||s[i+1]=='+'||s[i+1]=='-')){
        ++i; if(i<n&&(s[i]=='+'||s[i]=='-'))++i; while(i<n&&s[i].isDigit())++i; }
    double v=s.left(i).toDouble();
    const QString u=s.mid(i).trimmed().toLower();
    if(u=="em"||u=="rem") v*=16.0;              // default font-size 16px
    else if(u=="pt") v*=96.0/72.0;
    else if(u=="pc") v*=16.0;
    else if(u=="mm") v*=96.0/25.4;
    else if(u=="cm") v*=96.0/2.54;
    else if(u=="in") v*=96.0;
    return v;
}

// ---- SVG transform list -> QTransform ----
QTransform parseTransform(const QString& str) {
    QTransform t;
    static const QRegularExpression re("(\\w+)\\s*\\(([^)]*)\\)");
    auto it = re.globalMatch(str);
    while (it.hasNext()) {
        auto mm = it.next(); const QString fn = mm.captured(1);
        const QStringList a = mm.captured(2).split(QRegularExpression("[\\s,]+"), Qt::SkipEmptyParts);
        auto g = [&](int i){ return i<a.size()? a[i].toDouble() : 0.0; };
        QTransform u;
        if      (fn=="translate") u.translate(g(0), a.size()>1?g(1):0.0);
        else if (fn=="scale")     { double sx=a.size()>0?g(0):1.0; u.scale(sx, a.size()>1?g(1):sx); }   // no-args => identity, not 0-scale
        else if (fn=="rotate")    { if(a.size()>=3){ u.translate(g(1),g(2)); u.rotate(g(0)); u.translate(-g(1),-g(2)); } else u.rotate(g(0)); }
        else if (fn=="matrix" && a.size()>=6) u = QTransform(g(0),g(1),g(2),g(3),g(4),g(5));
        else if (fn=="skewX")     u.shear(std::tan(qDegreesToRadians(g(0))), 0);
        else if (fn=="skewY")     u.shear(0, std::tan(qDegreesToRadians(g(0))));
        else continue;
        t = u * t;   // SVG applies the list left-to-right (rightmost transform closest to the geometry)
    }
    return t;
}

// ---- colour ("none" / #hex / named / rgb(...)) ----
bool parseColor(QString s, QColor& out) {   // false => "none"/invalid (no paint)
    s = s.trimmed();
    if (s.isEmpty() || s=="none" || s=="transparent") return false;
    if (s.startsWith("rgb")) { auto in=s.indexOf('('), en=s.indexOf(')');
        if(in>0&&en>in){ auto p=s.mid(in+1,en-in-1).split(',');
            if(p.size()>=3){ auto ch=[&](int i){ QString v=p[i].trimmed(); return v.endsWith('%')? qRound(v.left(v.size()-1).toDouble()*2.55) : v.toInt(); };
                out=QColor(qBound(0,ch(0),255),qBound(0,ch(1),255),qBound(0,ch(2),255)); return true; } }
        return false; }
    QColor c; c.setNamedColor(s); if(c.isValid()){ out=c; return true; }
    return false;
}

// ---- SVG path data ("d") -> QPainterPath ----
void arcSegment(QPainterPath& p, QPointF c0, double rx, double ry, double rot, bool large, bool sweep, QPointF c1) {
    if (!std::isfinite(c0.x())||!std::isfinite(c0.y())||!std::isfinite(c1.x())||!std::isfinite(c1.y())){   // guard NaN/inf endpoints
        if (std::isfinite(c1.x())&&std::isfinite(c1.y())) p.lineTo(c1); return; }
    rx=std::abs(rx); ry=std::abs(ry);
    if (rx<1e-6||ry<1e-6){ p.lineTo(c1); return; }
    const double phi=qDegreesToRadians(rot), cp=std::cos(phi), sp=std::sin(phi);
    const double dx=(c0.x()-c1.x())/2, dy=(c0.y()-c1.y())/2;
    const double x1= cp*dx+sp*dy, y1=-sp*dx+cp*dy;
    double lam=(x1*x1)/(rx*rx)+(y1*y1)/(ry*ry); if(lam>1){ double s=std::sqrt(lam); rx*=s; ry*=s; }
    double sign = (large!=sweep)?1:-1;
    double num_ = rx*rx*ry*ry - rx*rx*y1*y1 - ry*ry*x1*x1, den = rx*rx*y1*y1 + ry*ry*x1*x1;
    double co = sign*std::sqrt(qMax(0.0, num_/qMax(1e-12,den)));
    double cxp= co*rx*y1/ry, cyp=-co*ry*x1/rx;
    double cx = cp*cxp - sp*cyp + (c0.x()+c1.x())/2, cy = sp*cxp + cp*cyp + (c0.y()+c1.y())/2;
    auto ang=[&](double ux,double uy,double vx,double vy){ double d=std::sqrt((ux*ux+uy*uy)*(vx*vx+vy*vy)); double a=std::acos(qBound(-1.0,(ux*vx+uy*vy)/qMax(1e-12,d),1.0)); return (ux*vy-uy*vx<0)?-a:a; };
    double th1=ang(1,0,(x1-cxp)/rx,(y1-cyp)/ry);
    double dth=ang((x1-cxp)/rx,(y1-cyp)/ry,(-x1-cxp)/rx,(-y1-cyp)/ry);
    if(!sweep && dth>0) dth-=2*M_PI; else if(sweep && dth<0) dth+=2*M_PI;
    int seg=qMax(1,int(std::ceil(std::abs(dth)/(M_PI/2))));
    for(int i=0;i<seg;++i){ double a0=th1+dth*i/seg, a1=th1+dth*(i+1)/seg, t=4.0/3.0*std::tan((a1-a0)/4);
        auto pt=[&](double a){ double x=rx*std::cos(a),y=ry*std::sin(a); return QPointF(cp*x-sp*y+cx, sp*x+cp*y+cy); };
        auto dv=[&](double a){ double x=-rx*std::sin(a),y=ry*std::cos(a); return QPointF(cp*x-sp*y, sp*x+cp*y); };
        QPointF p0=pt(a0), p1=pt(a1), d0=dv(a0), d1=dv(a1);
        p.cubicTo(p0+d0*t, p1-d1*t, p1); }
}

QPainterPath parsePath(const QString& d) {
    QPainterPath p; const int n=d.size(); int i=0;
    QPointF cur(0,0), sub(0,0), prevC(0,0); QChar cmd, last;
    auto ws=[&](){ while(i<n && (d[i].isSpace()||d[i]==',')) ++i; };
    auto rd=[&]()->double{ ws(); int s=i; if(i<n&&(d[i]=='+'||d[i]=='-'))++i; while(i<n&&d[i].isDigit())++i;
        if(i<n&&d[i]=='.'){++i;while(i<n&&d[i].isDigit())++i;} if(i<n&&(d[i]=='e'||d[i]=='E')){++i;if(i<n&&(d[i]=='+'||d[i]=='-'))++i;while(i<n&&d[i].isDigit())++i;} return d.mid(s,i-s).toDouble(); };
    auto has=[&]()->bool{ int j=i; while(j<n&&(d[j].isSpace()||d[j]==','))++j; return j<n && (d[j].isDigit()||d[j]=='.'||d[j]=='+'||d[j]=='-'); };
    bool rel=false;   // set per command; captured by rp()
    // Read a coordinate pair in a GUARANTEED order (x then y). NB: `pt(rd(),rd())` is a trap —
    // C++ leaves function-argument evaluation order unspecified (gcc evaluates right-to-left),
    // which silently swaps x and y. Sequenced local reads fix that.
    auto rp=[&]()->QPointF{ double x=rd(); double y=rd(); return rel?QPointF(cur.x()+x,cur.y()+y):QPointF(x,y); };
    while (i<n) { ws(); if(i>=n) break;
        const int guard=i;
        if (d[i].isLetter()){ cmd=d[i]; ++i; }
        rel=cmd.isLower(); const QChar C=cmd.toUpper();
        if (C=='M'){ cur=rp(); sub=cur; p.moveTo(cur); cmd = rel?QChar('l'):QChar('L');
            while(has()){ cur=rp(); p.lineTo(cur); } }
        else if (C=='L'){ do{ cur=rp(); p.lineTo(cur); }while(has()); }
        else if (C=='H'){ do{ double x=rd(); cur=QPointF(rel?cur.x()+x:x, cur.y()); p.lineTo(cur); }while(has()); }
        else if (C=='V'){ do{ double y=rd(); cur=QPointF(cur.x(), rel?cur.y()+y:y); p.lineTo(cur); }while(has()); }
        else if (C=='C'){ do{ QPointF c1=rp(), c2=rp(), e=rp(); p.cubicTo(c1,c2,e); prevC=c2; cur=e; }while(has()); }
        else if (C=='S'){ do{ QPointF c1=(last=='C'||last=='S')?2*cur-prevC:cur, c2=rp(), e=rp(); p.cubicTo(c1,c2,e); prevC=c2; cur=e; last='C'; }while(has()); }
        else if (C=='Q'){ do{ QPointF c1=rp(), e=rp(); p.quadTo(c1,e); prevC=c1; cur=e; }while(has()); }
        else if (C=='T'){ do{ QPointF c1=(last=='Q'||last=='T')?2*cur-prevC:cur, e=rp(); p.quadTo(c1,e); prevC=c1; cur=e; last='Q'; }while(has()); }
        else if (C=='A'){ do{ double rx=rd(),ry=rd(),rot=rd(),la=rd(),sw=rd(); QPointF e=rp(); arcSegment(p,cur,rx,ry,rot,la!=0,sw!=0,e); cur=e; }while(has()); }
        else if (C=='Z'){ p.closeSubpath(); cur=sub; }
        else { ++i; continue; }
        if (C!='S' && C!='T') last=C;
        if (i==guard) ++i;   // malformed data consumed nothing this iteration → advance to avoid an infinite loop
    }
    return p;
}

struct Ctx { QTransform xf; QColor fill=Qt::black; bool fillOn=true; QColor stroke; double strokeW=1; bool strokeOn=false; double opacity=1; QString fillUrl;
    QString fontFamily; double fontSize=16; bool bold=false, italic=false; int anchor=0; };   // anchor: 0 start,1 middle,2 end

// Decode an <image> href: only "data:image/*;base64,…" is supported (external URLs/paths are skipped in v1).
QImage decodeImageHref(const QString& href, const QString& baseDir=QString()) {
    if (href.startsWith("data:")) {
        const int c = href.indexOf("base64,");
        if (c < 0) return QImage();
        QImage img; img.loadFromData(QByteArray::fromBase64(href.mid(c+7).toLatin1()));
        return img;
    }
    // External/relative <image>: load a LOCAL file only (never fetch http/https — no network from an import).
    if (href.startsWith("http://") || href.startsWith("https://") || href.startsWith("ftp://")) return QImage();
    QString path = href;
    if (path.startsWith("file://")) path = QUrl(path).toLocalFile();
    if (!QDir::isAbsolutePath(path)) {
        if (baseDir.isEmpty()) return QImage();
        path = QDir(baseDir).filePath(path);
    }
    QImage img; img.load(path); return img;
}

// stop offset ("50%" or "0.5") and the id from "url(#id)".
double stopOffset(const QString& s){ return s.contains('%') ? num(s)/100.0 : num(s); }
QString urlId(const QString& s){ int h=s.indexOf('#'); if(h<0) return QString(); int e=s.indexOf(')',h); return s.mid(h+1, (e<0?s.size():e)-h-1).trimmed(); }

// Apply one CSS/presentation declaration (key already lower-cased, value trimmed).
void applyDecl(Ctx& c, const QString& k, const QString& v) {
    if(k=="fill"){ if(v.startsWith("url(")){ c.fillUrl=urlId(v); c.fillOn=true; }
        else { QColor col; if(parseColor(v,col)){ c.fill=col; c.fillOn=true; c.fillUrl.clear(); } else { c.fillOn=false; c.fillUrl.clear(); } } }
    else if(k=="stroke"){ QColor col; if(!v.startsWith("url(") && parseColor(v,col)){ c.stroke=col; c.strokeOn=true; } else if(!v.startsWith("url(")) c.strokeOn=false; }
    else if(k=="stroke-width") c.strokeW=num(v);
    else if(k=="opacity"||k=="fill-opacity") c.opacity=v.toDouble();
    else if(k=="font-size") c.fontSize=num(v);
    else if(k=="font-family"){ QString f=v.section(',',0,0).trimmed(); f.remove('"'); f.remove('\''); if(!f.isEmpty()) c.fontFamily=f; }
    else if(k=="font-weight") c.bold=(v=="bold"||v.toInt()>=600);
    else if(k=="font-style") c.italic=(v=="italic"||v=="oblique");
    else if(k=="text-anchor") c.anchor=(v=="middle"?1:(v=="end"?2:0));
}

// SVG presentation attributes (lower priority than author CSS, per the cascade).
void applyPresentation(Ctx& c, const QXmlStreamAttributes& a) {
    for (const QString& k : {QStringLiteral("fill"),QStringLiteral("stroke"),QStringLiteral("stroke-width"),
             QStringLiteral("opacity"),QStringLiteral("fill-opacity"),QStringLiteral("font-size"),
             QStringLiteral("font-family"),QStringLiteral("font-weight"),QStringLiteral("font-style"),QStringLiteral("text-anchor")})
        if (a.hasAttribute(k)) applyDecl(c, k, a.value(k).toString());
}
// The style="" attribute (highest priority).
void applyInlineStyle(Ctx& c, const QXmlStreamAttributes& a) {
    if (a.hasAttribute("style")) for (const QString& decl : a.value("style").toString().split(';', Qt::SkipEmptyParts)) {
        int c2=decl.indexOf(':'); if(c2>0) applyDecl(c, decl.left(c2).trimmed().toLower(), decl.mid(c2+1).trimmed());
    }
}

// ---- CSS <style> rules ----
struct CssRule { QStringList sels; QVector<QPair<QString,QString>> decls; };

QVector<CssRule> parseCss(const QByteArray& xml) {
    QVector<CssRule> rules;
    QRegularExpression styleRe("<style[^>]*>(.*?)</style>",
        QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
    auto it = styleRe.globalMatch(QString::fromUtf8(xml));
    while (it.hasNext()) {
        QString css = it.next().captured(1);
        css.remove("<![CDATA["); css.remove("]]>");
        css.remove(QRegularExpression("/\\*.*?\\*/", QRegularExpression::DotMatchesEverythingOption));
        int pos=0;
        while (true) {
            int ob=css.indexOf('{',pos); if(ob<0) break;
            int cb=css.indexOf('}',ob); if(cb<0) break;
            CssRule ru;
            for (const QString& sp : css.mid(pos,ob-pos).split(',',Qt::SkipEmptyParts)) { QString t=sp.trimmed(); if(!t.isEmpty()) ru.sels<<t; }
            for (const QString& d : css.mid(ob+1,cb-ob-1).split(';',Qt::SkipEmptyParts)) { int c2=d.indexOf(':'); if(c2>0) ru.decls.push_back({d.left(c2).trimmed().toLower(), d.mid(c2+1).trimmed()}); }
            if (!ru.sels.isEmpty() && !ru.decls.isEmpty()) rules.push_back(ru);
            pos=cb+1;
        }
    }
    return rules;
}

// Match one simple selector (tag / .class / #id / tag.class / tag#id / *) against an element.
bool selMatch(const QString& sel, const QString& tag, const QString& id, const QStringList& classes) {
    const QString s=sel.trimmed();
    if (s.isEmpty()) return false;
    static const QRegularExpression seR("[.#]");
    int p=s.indexOf(seR);
    if (p<0) return s=="*" || s==tag;
    QString base=s.left(p); QChar kind=s[p]; QString name=s.mid(p+1);
    static const QRegularExpression endR("[.#\\s]");
    int q=name.indexOf(endR); if(q>=0) name=name.left(q);              // v1: first simple component only
    bool baseOk = base.isEmpty() || base=="*" || base==tag;
    return kind=='.' ? (baseOk && classes.contains(name)) : (baseOk && name==id);
}

// Specificity of the (first simple component of a) selector: #id=100, .class=10, tag=1, *=0.
int specificity(const QString& sel) {
    const QString s=sel.trimmed();
    if (s.isEmpty() || s=="*") return 0;
    static const QRegularExpression seR("[.#]");
    int p=s.indexOf(seR);
    if (p<0) return 1;                                   // bare tag
    int base = s.left(p).isEmpty() ? 0 : 1;              // optional leading tag
    return base + (s[p]=='#' ? 100 : 10);
}

// Apply author CSS in ascending specificity (doc order as tiebreak), between presentation attrs and inline style.
void applyCss(Ctx& c, const QVector<CssRule>& rules, const QString& tag, const QXmlStreamAttributes& a) {
    if (rules.isEmpty()) return;
    const QString id = a.value("id").toString();
    const QStringList classes = a.value("class").toString().split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    struct M { int spec; int order; const CssRule* rule; };
    QVector<M> matches;
    for (int ri=0; ri<rules.size(); ++ri) {
        int best=-1;
        for (const QString& sel : rules[ri].sels) if (selMatch(sel, tag, id, classes)) best=qMax(best, specificity(sel));
        if (best>=0) matches.push_back({best, ri, &rules[ri]});
    }
    std::stable_sort(matches.begin(), matches.end(), [](const M&x, const M&y){ return x.spec!=y.spec ? x.spec<y.spec : x.order<y.order; });
    for (const M& m : matches) for (const auto& kv : m.rule->decls) applyDecl(c, kv.first, kv.second);
}

void styleShape(Shape& s, const Ctx& c) {
    if (c.fillOn){ s.fill.type=FillType::Solid; s.fill.color=c.fill; } else s.fill.type=FillType::None;
    s.stroke.enabled=c.strokeOn; if(c.strokeOn){ s.stroke.color=c.stroke; s.stroke.width=c.strokeW; }
    s.opacity=qBound(0.0,c.opacity,1.0); s.transform=c.xf;
}

struct SvgStop { double off; QColor col; };
struct SvgGrad {
    bool radial=false, userSpace=false, hasGT=false;
    double x1=0,y1=0,x2=1,y2=0;              // linear (objectBoundingBox defaults)
    double cx=0.5,cy=0.5,rr=0.5;             // radial
    QTransform gt; QString href;
    QVector<SvgStop> stops;
};

// Resolve a url(#id) gradient onto a shape's fill (linear/radial), in the shape's LOCAL space.
void resolveGradient(Shape& s, const QString& id, const QHash<QString,SvgGrad>& grads, int depth=0) {
    if (depth>8 || !grads.contains(id)) return;
    const SvgGrad& g = grads[id];
    QVector<SvgStop> stops = g.stops;
    if (stops.isEmpty() && !g.href.isEmpty() && grads.contains(g.href)) stops = grads[g.href].stops;  // href inherits stops
    if (stops.size() < 2) { if(stops.size()==1){ s.fill.type=FillType::Solid; s.fill.color=stops[0].col; } return; }
    std::sort(stops.begin(), stops.end(), [](const SvgStop&a,const SvgStop&b){ return a.off<b.off; });
    s.fill.stops.clear();
    for (const SvgStop& st : stops) s.fill.stops.push_back({ qBound(0.0,st.off,1.0), st.col });
    const QRectF bb = s.localBounds();
    if (!g.userSpace && (!bb.isValid() || bb.width()<1e-6 || bb.height()<1e-6)) return;   // objectBBox needs a real bbox; keep the solid fallback
    auto toLocal = [&](QPointF raw)->QPointF {
        if (g.hasGT) raw = g.gt.map(raw);
        return g.userSpace ? raw : QPointF(bb.left()+raw.x()*bb.width(), bb.top()+raw.y()*bb.height());
    };
    if (g.radial) {
        s.fill.type = FillType::Radial;
        QPointF ctr = toLocal(QPointF(g.cx,g.cy));
        double rw = qMax(1e-3, g.userSpace ? g.rr : g.rr*bb.width()), rh = qMax(1e-3, g.userSpace ? g.rr : g.rr*bb.height());
        s.fill.p0 = ctr; s.fill.p1 = ctr+QPointF(rw,0); s.fill.p2 = ctr+QPointF(0,rh);
    } else {
        s.fill.type = FillType::Linear;
        s.fill.p0 = toLocal(QPointF(g.x1,g.y1)); s.fill.p1 = toLocal(QPointF(g.x2,g.y2));
    }
}

// ---- <use>/<symbol>/<defs> instantiation (Approach B: def-span record + post-walk clone) ----
// A referenced element's output shapes occupy a contiguous span of the flat 'out' (or 'defsBuf')
// list; E is that element's CTM captured at its StartElement. Cloning the span and rebasing each
// shape's transform by  origT * E.inverted() * placement  re-expresses it under the <use> site.
struct Def { int start=0, end=0; bool inDefs=false; QTransform E;   // E = the element's PARENT CTM (its own transform is preserved on <use>)
    int seqStart=0, seqEnd=0;                                        // document-order range (nesting-detection, independent of shape emission)
    bool isSymbol=false, hasVB=false; double vbX=0,vbY=0,vbW=0,vbH=0; };
struct PendUse { QString refId; QTransform useXf; double x=0,y=0,w=0,h=0; bool hasWH=false; bool inDefs=false; int seq=0; };
struct Frame { QString id; int spanStart=0, seqStart=0; bool inDefs=false, container=false;
    QTransform E; bool isSymbol=false, hasVB=false; double vbX=0,vbY=0,vbW=0,vbH=0; };
struct GPend { bool inDefs; int idx; QString grad; };

// Is this element a non-rendering container whose content is only shown when <use>d?
bool isSuppressContainer(const QString& el){
    return el=="symbol"||el=="defs"||el=="clipPath"||el=="mask"||el=="marker"||el=="pattern";
}

} // namespace

QVector<Shape> parseSvg(const QByteArray& xml, QString* error, const QString& baseDir) {
    QVector<Shape> out, defsBuf;                       // defsBuf: shapes inside a non-rendering container (<defs>/<symbol>/…)
    QHash<QString,SvgGrad> grads; QString curGradId;
    QVector<GPend> pend;                               // gradient url() to resolve after the walk (records which buffer)
    QHash<QString,Def> defs; QVector<PendUse> pend2;   // <use> def-spans + pending instantiations
    int suppress=0, seq=0; QVector<Frame> idstk;       // idstk mirrors stk 1:1; suppress>0 => inside a container; seq = document-order counter
    const QVector<CssRule> cssRules = parseCss(xml);   // author <style> rules (parsed up-front, position-independent)
    QXmlStreamReader r(xml);
    QVector<Ctx> stk; stk.push_back(Ctx{});
    auto emitShape = [&](Shape& s){ (suppress? defsBuf : out).push_back(s); };   // route to defsBuf while suppressed
    while (!r.atEnd()) {
        auto tok = r.readNext();
        if (tok == QXmlStreamReader::StartElement) {
            Ctx c = stk.back();                        // inherit from the parent
            const QTransform parentXf = c.xf;          // CTM excluding this element's own transform (used as Def::E)
            const auto a = r.attributes();
            const QString el = r.name().toString();
            const int mySeq = ++seq;                   // this element's document-order index
            // --- gradient defs (emit no geometry; keep idstk lockstep on their continue-pushes) ---
            if (el=="linearGradient" || el=="radialGradient") {
                auto GC=[&](const char* k, double def){ if(!a.hasAttribute(k)) return def; QString v=a.value(k).toString(); return v.contains('%')?num(v)/100.0:num(v); };
                SvgGrad g; g.radial=(el=="radialGradient");
                g.userSpace = a.value("gradientUnits").toString()=="userSpaceOnUse";
                if (a.hasAttribute("gradientTransform")){ g.gt=parseTransform(a.value("gradientTransform").toString()); g.hasGT=true; }
                QString href = a.hasAttribute("href")? a.value("href").toString() : a.value("xlink:href").toString();
                if (!href.isEmpty()) g.href = href.startsWith('#')? href.mid(1) : href;
                g.x1=GC("x1",0); g.y1=GC("y1",0); g.x2=GC("x2",1); g.y2=GC("y2",0);
                g.cx=GC("cx",0.5); g.cy=GC("cy",0.5); g.rr=GC("r",0.5);
                curGradId = a.value("id").toString(); if(!curGradId.isEmpty()) grads.insert(curGradId, g);
                stk.push_back(c); idstk.push_back(Frame{}); continue;
            }
            if (el=="stop") {
                if (!curGradId.isEmpty() && grads.contains(curGradId)) {
                    SvgStop st; st.off=a.hasAttribute("offset")?stopOffset(a.value("offset").toString()):0.0;
                    QString sc="black"; double so=1.0;
                    if(a.hasAttribute("stop-color")) sc=a.value("stop-color").toString();
                    if(a.hasAttribute("stop-opacity")) so=a.value("stop-opacity").toDouble();
                    if(a.hasAttribute("style")) for(const QString& d: a.value("style").toString().split(';',Qt::SkipEmptyParts)){ int p=d.indexOf(':'); if(p<0)continue; QString k=d.left(p).trimmed(),v=d.mid(p+1).trimmed(); if(k=="stop-color")sc=v; else if(k=="stop-opacity")so=v.toDouble(); }
                    QColor col; if(!parseColor(sc,col)) col=Qt::black; col.setAlphaF(qBound(0.0,so,1.0)); st.col=col;
                    grads[curGradId].stops.push_back(st);
                }
                stk.push_back(c); idstk.push_back(Frame{}); continue;
            }
            applyPresentation(c, a);                   // cascade: presentation attrs < author CSS < inline style
            applyCss(c, cssRules, el, a);
            applyInlineStyle(c, a);
            if (a.hasAttribute("transform")) c.xf = parseTransform(a.value("transform").toString()) * c.xf;
            auto D=[&](const char* k){ return a.hasAttribute(k)? num(a.value(k).toString()) : 0.0; };
            // --- <svg> viewBox → viewport: scale content so a viewBox that differs from width/height imports at the right size ---
            if (el=="svg" && a.hasAttribute("viewBox")) {
                const auto vb=a.value("viewBox").toString().split(QRegularExpression("[\\s,]+"),Qt::SkipEmptyParts);
                if (vb.size()==4) { const double vx=vb[0].toDouble(),vy=vb[1].toDouble(),vw=vb[2].toDouble(),vh=vb[3].toDouble();
                    const double W = a.hasAttribute("width") ? num(a.value("width").toString())  : vw;
                    const double H = a.hasAttribute("height")? num(a.value("height").toString()) : vh;
                    if (vw>0 && vh>0 && W>0 && H>0) {
                        QTransform vbT;
                        if (a.value("preserveAspectRatio").toString().startsWith("none"))   // stretch to fill
                            vbT = QTransform(W/vw,0,0,H/vh, -vx*W/vw, -vy*H/vh);
                        else { const double s=qMin(W/vw,H/vh);                               // default xMidYMid meet: uniform + centre
                            vbT = QTransform(s,0,0,s, (W-vw*s)/2 - vx*s, (H-vh*s)/2 - vy*s); }
                        c.xf = vbT * c.xf;
                    } }
            }
            // --- def bookkeeping: capture this element's span-start + CTM (E) BEFORE any child emits ---
            const bool container = isSuppressContainer(el);
            const bool childInDefs = suppress>0 || container;      // where THIS element's children get emitted
            Frame fr; fr.id=a.value("id").toString(); fr.container=container; fr.inDefs=childInDefs;
            fr.E=parentXf; fr.seqStart=mySeq;                      // E is the PARENT CTM so the element's own transform survives instantiation
            fr.spanStart=(childInDefs? defsBuf : out).size();
            if (el=="symbol") { fr.isSymbol=true;
                if (a.hasAttribute("viewBox")) { const auto vb=a.value("viewBox").toString().split(QRegularExpression("[\\s,]+"),Qt::SkipEmptyParts);
                    if (vb.size()==4){ fr.hasVB=true; fr.vbX=vb[0].toDouble(); fr.vbY=vb[1].toDouble(); fr.vbW=vb[2].toDouble(); fr.vbH=vb[3].toDouble(); } } }
            if (container) ++suppress;                 // children now route to defsBuf
            // --- text: content via readElementText (folds in <tspan> children); baseline + anchor corrected ---
            if (el=="text") {
                const double tx=D("x"), ty=D("y");
                const QString content = r.readElementText(QXmlStreamReader::IncludeChildElements).simplified();
                if (!content.isEmpty()) {
                    Shape s; s.type=ShapeType::Text; s.text=content;
                    QFont f(c.fontFamily.isEmpty()? QStringLiteral("DejaVu Sans") : c.fontFamily);
                    f.setPixelSize(qMax(1,qRound(c.fontSize))); f.setBold(c.bold); f.setItalic(c.italic);
                    s.font=f;
                    if (c.fillOn){ s.fill.type=FillType::Solid; s.fill.color=c.fill; } else s.fill.type=FillType::None;
                    s.stroke.enabled=c.strokeOn; if(c.strokeOn){ s.stroke.color=c.stroke; s.stroke.width=c.strokeW; }
                    s.opacity=qBound(0.0,c.opacity,1.0);
                    QFontMetricsF fm(f);
                    double w=0; for(const QString& ln : content.split('\n')) w=qMax(w, fm.horizontalAdvance(ln));
                    const double xoff = c.anchor==1? -w/2 : (c.anchor==2? -w : 0.0);   // text-anchor middle/end
                    s.transform = QTransform::fromTranslate(tx+xoff, ty-fm.ascent()) * c.xf;  // SVG y is the baseline
                    s.name="text"; emitShape(s);
                }
                continue;   // readElementText consumed </text>; no stack push, so no matching pop (text is not a <use> target)
            }
            // --- use: record a deferred instantiation (forward refs OK); emits no geometry now ---
            if (el=="use") {
                QString href = a.hasAttribute("href")? a.value("href").toString() : a.value("xlink:href").toString();
                QString refId = href.startsWith('#')? href.mid(1) : href;
                if (!refId.isEmpty()) { PendUse pu; pu.refId=refId; pu.useXf=c.xf; pu.x=D("x"); pu.y=D("y");
                    pu.hasWH=a.hasAttribute("width")||a.hasAttribute("height"); pu.w=D("width"); pu.h=D("height");
                    pu.inDefs=suppress>0; pu.seq=mySeq; pend2.push_back(pu); }
                stk.push_back(c); idstk.push_back(fr); continue;
            }
            // --- image: a data:URI raster → a Rect with a Bitmap fill (parallelogram = the image box in local space) ---
            if (el=="image") {
                QString href = a.hasAttribute("href")? a.value("href").toString() : a.value("xlink:href").toString();
                QImage img = decodeImageHref(href, baseDir);
                if (!img.isNull()) {
                    const double x=D("x"), y=D("y");
                    const double w = a.hasAttribute("width")  ? num(a.value("width").toString())  : img.width();
                    const double h = a.hasAttribute("height") ? num(a.value("height").toString()) : img.height();
                    if (w>0 && h>0) {
                        Shape s; s.type=ShapeType::Rect; s.rect=QRectF(x,y,w,h);
                        s.fill.type=FillType::Bitmap; s.fill.image=img;
                        s.fill.p0=QPointF(x,y); s.fill.p1=QPointF(x+w,y); s.fill.p2=QPointF(x,y+h);   // image (0,0)/(W,0)/(0,H)
                        s.stroke.enabled=false; s.opacity=qBound(0.0,c.opacity,1.0); s.transform=c.xf; s.name="image";
                        emitShape(s);
                    }
                }
                stk.push_back(c); idstk.push_back(fr); continue;
            }
            Shape s; bool made=false;
            if (el=="rect"){ s.type=ShapeType::Rect; s.rect=QRectF(D("x"),D("y"),D("width"),D("height")); s.cornerRadius=D("rx"); made=s.rect.width()>0&&s.rect.height()>0; }
            else if (el=="circle"){ double cx=D("cx"),cy=D("cy"),rr=D("r"); s.type=ShapeType::Ellipse; s.rect=QRectF(cx-rr,cy-rr,2*rr,2*rr); made=rr>0; }
            else if (el=="ellipse"){ double cx=D("cx"),cy=D("cy"),rx=D("rx"),ry=D("ry"); s.type=ShapeType::Ellipse; s.rect=QRectF(cx-rx,cy-ry,2*rx,2*ry); made=rx>0&&ry>0; }
            else if (el=="line"){ s.type=ShapeType::Path; QPainterPath p; p.moveTo(D("x1"),D("y1")); p.lineTo(D("x2"),D("y2")); s.path=p; if(!c.strokeOn){c.strokeOn=true;c.stroke=Qt::black;} c.fillOn=false; made=true; }
            else if (el=="polyline"||el=="polygon"){ const auto pts=a.value("points").toString().split(QRegularExpression("[\\s,]+"),Qt::SkipEmptyParts);
                if(pts.size()>=4){ QPainterPath p; p.moveTo(pts[0].toDouble(),pts[1].toDouble()); for(int k=2;k+1<pts.size();k+=2) p.lineTo(pts[k].toDouble(),pts[k+1].toDouble());
                    if(el=="polygon") p.closeSubpath(); s.type=ShapeType::Path; s.path=p; if(el=="polyline"){ if(!c.strokeOn){c.strokeOn=true;c.stroke=Qt::black;} c.fillOn=false; } made=true; } }
            else if (el=="path"){ QPainterPath p=parsePath(a.value("d").toString()); if(p.elementCount()>0){ s.type=ShapeType::Path; s.path=p; made=true; } }
            if (made){ styleShape(s, c); s.name=el; emitShape(s);
                if (!c.fillUrl.isEmpty()) pend.push_back({ suppress>0, int((suppress? defsBuf : out).size())-1, c.fillUrl }); }
            stk.push_back(c); idstk.push_back(fr);
        } else if (tok == QXmlStreamReader::EndElement) {
            const QString en = r.name().toString();
            if (en=="linearGradient" || en=="radialGradient") curGradId.clear();
            if (stk.size()>1) {
                stk.pop_back();
                Frame f = idstk.back(); idstk.pop_back();
                if (f.container && suppress>0) --suppress;
                if (!f.id.isEmpty()) defs.insert(f.id, Def{ f.spanStart, int((f.inDefs? defsBuf : out).size()), f.inDefs, f.E, f.seqStart, seq, f.isSymbol, f.hasVB, f.vbX, f.vbY, f.vbW, f.vbH });
            }
        }
    }
    // 1) resolve gradients on the ORIGINALS (both buffers), in local space, before cloning
    for (const GPend& pr : pend) { QVector<Shape>& buf = pr.inDefs? defsBuf : out;
        if (pr.idx>=0 && pr.idx<buf.size()) resolveGradient(buf[pr.idx], pr.grad, grads); }
    // 2) instantiate <use> (deferred → forward refs resolve; nested use + path-scoped cycle/depth guard)
    auto placement = [&](const QString& refId, const QTransform& useXf, double x,double y,bool hasWH,double w,double h)->QTransform {
        QTransform vb;                                  // symbol viewBox → viewport (xMidYMid meet); identity otherwise
        if (defs.contains(refId)) { const Def& t=defs.value(refId);
            if (t.isSymbol && t.hasVB && hasWH && t.vbW>0 && t.vbH>0 && w>0 && h>0) {
                double sc=qMin(w/t.vbW, h/t.vbH);
                double tx=(w-t.vbW*sc)/2 - t.vbX*sc, ty=(h-t.vbH*sc)/2 - t.vbY*sc;
                vb = QTransform(sc,0,0,sc,tx,ty); } }
        return vb * QTransform::fromTranslate(x,y) * useXf;
    };
    std::function<void(const QString&, const QTransform&, int, QSet<QString>)> instantiate =
      [&](const QString& refId, const QTransform& U, int depth, QSet<QString> visited){
        if (depth>12 || visited.contains(refId) || !defs.contains(refId)) return;
        visited.insert(refId);
        const Def d = defs.value(refId);
        const QTransform Einv = d.E.inverted();         // computed once; a singular parent CTM (e.g. scale(0)) yields identity → clone collapses harmlessly
        const QVector<Shape>& src = d.inDefs? defsBuf : out;
        QVector<Shape> clones;                          // snapshot BEFORE appending (out may reallocate; src aliases out)
        for (int k=d.start; k<d.end && k<src.size(); ++k) { Shape copy=src[k];
            copy.transform = src[k].transform * Einv * U; clones.push_back(copy); }
        for (Shape& cs : clones) out.push_back(cs);
        for (const PendUse& pu : pend2)                 // nested <use> that lived inside this def's subtree (document-order range)
            if (pu.seq>d.seqStart && pu.seq<=d.seqEnd)
                instantiate(pu.refId, placement(pu.refId, pu.useXf, pu.x,pu.y,pu.hasWH,pu.w,pu.h) * Einv * U, depth+1, visited);
      };
    for (const PendUse& pu : pend2)                     // top-level: a use renders at its own site iff not inside a container
        if (!pu.inDefs) instantiate(pu.refId, placement(pu.refId, pu.useXf, pu.x,pu.y,pu.hasWH,pu.w,pu.h), 0, {});
    if (r.hasError() && error) *error = r.errorString();
    return out;
}

} // namespace xara
