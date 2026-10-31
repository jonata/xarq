#include "svgimport.h"
#include <QXmlStreamReader>
#include <QRegularExpression>
#include <QColor>
#include <QHash>
#include <QSet>
#include <QtMath>
#include <QPainterPath>
#include <QPainter>
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
    // fill-opacity and stroke-opacity are PER-PAINT and multiply into that paint's alpha; 'opacity' is
    // the whole-element one and COMPOUNDS down the tree. Folding all three into one slot meant a
    // translucent fill also faded the stroke, and <g opacity=.5><g opacity=.5> rendered at .5.
    double fillOpacity=1, strokeOpacity=1;
    Qt::PenCapStyle  cap  = Qt::FlatCap;
    Qt::PenJoinStyle join = Qt::RoundJoin;
    QVector<qreal>   dashes;
    Qt::FillRule fillRule = Qt::WindingFill;   // SVG's default is nonzero, not even-odd
    QString label;   // inkscape:label / data-name, inherited so a named LAYER names the drawable inside it
    // 'opacity' is per-element and MULTIPLIES down the tree, but within one element the cascade
    // must let the winning declaration replace the others. Keeping the element's own value in its
    // own slot and folding it into `opacity` once, after the cascade, does both.
    double elemOpacity = 1.0;
    // XarQ-native appearance that SVG has no words for, written by our own importers under the xarq:
    // namespace. Per-element: never inherited.
    QString xqTransp, xqBitmap, xqBitmapXf; bool xqBitmapTiled = false;
    QString xqPath; double xqPathStart = 0.0;
    QString xqShadow, xqPressure; double xqBlur = 0.0, xqBlurGain = 1.0;
    QImage xqMask; QTransform xqMaskXf;     // pixel mask, inherited: image pixels to document
    int xqBlendType = 0;      // inherited, like the composition mode it stands in for
    QString fontFamily; double fontSize=16; bool bold=false, italic=false; int fontWeight=400; int anchor=0;   // fontWeight: CSS 100–900 (Heavy/Black=900, distinct from Bold=700); anchor: 0 start,1 middle,2 end
    QString clipId; QTransform clipCTM;   // clip-path="url(#id)" active on/inherited by this element; clipCTM = its user-space ref frame
    QString filterId;   // filter="url(#id)" — a pure feGaussianBlur filter → Shape::blur (whole-object soft focus)
    QPainter::CompositionMode blend = QPainter::CompositionMode_SourceOver; };   // CSS mix-blend-mode → Shape::blend

// Decode an <image> href: only "data:image/*;base64,…" is supported (external URLs/paths are skipped in v1).
// `srcOut` (optional) receives the ORIGINAL encoded file bytes so the writer can store them verbatim (no re-encode).
QImage decodeImageHref(const QString& href, const QString& baseDir=QString(), QByteArray* srcOut=nullptr) {
    if (href.startsWith("data:")) {
        const int c = href.indexOf("base64,");
        if (c < 0) return QImage();
        const QByteArray raw = QByteArray::fromBase64(href.mid(c+7).toLatin1());
        QImage img; img.loadFromData(raw);
        if (srcOut && !img.isNull()) *srcOut = raw;
        return img;
    }
    // External/relative <image>: load a LOCAL file only (never fetch http/https — no network from an import).
    if (href.startsWith("http://") || href.startsWith("https://") || href.startsWith("ftp://")) return QImage();
    QString path = href;
    if (path.startsWith("file://")) path = QUrl(path).toLocalFile();
    if (baseDir.isEmpty()) return QImage();                       // no base → can't safely resolve a filesystem path
    // Confine the read to baseDir: an absolute href, or a '../' that escapes the SVG's own directory, must
    // NOT let a crafted document read arbitrary local image files. Resolve + normalise, then require containment.
    const QString base = QDir(baseDir).absolutePath();
    const QString resolved = QDir::cleanPath(QDir::isAbsolutePath(path) ? path : base + QLatin1Char('/') + path);
    if (resolved != base && !resolved.startsWith(base + QLatin1Char('/'))) return QImage();   // outside baseDir → refuse
    QImage img; img.load(resolved);
    if (srcOut && !img.isNull()){ QFile ff(resolved); if(ff.open(QIODevice::ReadOnly)) *srcOut = ff.readAll(); }
    return img;
}

// stop offset ("50%" or "0.5") and the id from "url(#id)".
double stopOffset(const QString& s){ return s.contains('%') ? num(s)/100.0 : num(s); }
QString urlId(const QString& s){ int h=s.indexOf('#'); if(h<0) return QString(); int e=s.indexOf(')',h); return s.mid(h+1, (e<0?s.size():e)-h-1).trimmed(); }

// Apply one CSS/presentation declaration (key already lower-cased, value trimmed).
// CSS mix-blend-mode keyword → QPainter composition mode (Shape::blend). The non-separable modes
// (hue/saturation/color/luminosity) have no QPainter equivalent → fall back to Normal.
QPainter::CompositionMode cssBlend(const QString& v) {
    const QString k=v.trimmed().toLower();
    if(k=="plus-lighter"||k=="plus") return QPainter::CompositionMode_Plus;   // CSS plus-lighter == Affinity "Add"
    if(k=="multiply")    return QPainter::CompositionMode_Multiply;
    if(k=="screen")      return QPainter::CompositionMode_Screen;
    if(k=="overlay")     return QPainter::CompositionMode_Overlay;
    if(k=="darken")      return QPainter::CompositionMode_Darken;
    if(k=="lighten")     return QPainter::CompositionMode_Lighten;
    if(k=="color-dodge"||k=="colour-dodge") return QPainter::CompositionMode_ColorDodge;
    if(k=="color-burn" ||k=="colour-burn")  return QPainter::CompositionMode_ColorBurn;
    if(k=="hard-light")  return QPainter::CompositionMode_HardLight;
    if(k=="soft-light")  return QPainter::CompositionMode_SoftLight;
    if(k=="difference")  return QPainter::CompositionMode_Difference;
    if(k=="exclusion")   return QPainter::CompositionMode_Exclusion;
    return QPainter::CompositionMode_SourceOver;   // normal + unsupported
}

void applyDecl(Ctx& c, const QString& k, const QString& v) {
    if(k=="mix-blend-mode"){
        c.blend=cssBlend(v);
        // Modes QPainter has no composition for go through the renderer's per-pixel path.
        const QString m=v.trimmed().toLower();
        c.xqBlendType = m=="hue"?31 : m=="saturation"?16 : m=="luminosity"?28 : m=="xarq-linear-light"?47 : m=="color"||m=="colour"?40
                      : m=="xarq-average"?41 : m=="xarq-negation"?42 : m=="xarq-reflect"?43 : m=="xarq-glow"?44 : 0;
        return; }
    if(k=="fill"){ if(v.startsWith("url(")){ c.fillUrl=urlId(v); c.fillOn=true; }
        else { QColor col; if(parseColor(v,col)){ c.fill=col; c.fillOn=true; c.fillUrl.clear(); } else { c.fillOn=false; c.fillUrl.clear(); } } }
    else if(k=="stroke"){ QColor col; if(!v.startsWith("url(") && parseColor(v,col)){ c.stroke=col; c.strokeOn=true; } else if(!v.startsWith("url(")) c.strokeOn=false; }
    else if(k=="stroke-width") c.strokeW=num(v);
    else if(k=="opacity") c.elemOpacity = qBound(0.0, v.toDouble(), 1.0);      // assigned here; folded in after the cascade
    else if(k=="fill-opacity") c.fillOpacity = qBound(0.0, v.toDouble(), 1.0);
    else if(k=="stroke-opacity") c.strokeOpacity = qBound(0.0, v.toDouble(), 1.0);
    else if(k=="stroke-linecap") c.cap = (v=="round"?Qt::RoundCap : v=="square"?Qt::SquareCap : Qt::FlatCap);
    else if(k=="stroke-linejoin") c.join = (v=="bevel"?Qt::BevelJoin : v=="miter"?Qt::MiterJoin : Qt::RoundJoin);
    else if(k=="fill-rule") c.fillRule = (v.trimmed()=="evenodd" ? Qt::OddEvenFill : Qt::WindingFill);
    else if(k=="stroke-dasharray"){
        c.dashes.clear();
        if(v!="none"){
            const QStringList parts = v.split(QRegularExpression("[,\\s]+"), Qt::SkipEmptyParts);
            for(const QString& d : parts){ const double x = num(d); if(x >= 0.0) c.dashes.push_back(x); }
            // Qt's dash pattern is in PEN WIDTHS, SVG's is in user units; and an odd count repeats.
            if(c.dashes.size() & 1) c.dashes += c.dashes;
        }
    }
    else if(k=="font-size") c.fontSize=num(v);
    else if(k=="font-family"){ QString f=v.section(',',0,0).trimmed(); f.remove('"'); f.remove('\''); if(!f.isEmpty()) c.fontFamily=f; }
    else if(k=="font-weight"){   // carry the FULL CSS weight (100–900), not just a bold flag, so 900 → Heavy/Black (≠ Bold 700)
        if(v=="normal") c.fontWeight=400;
        else if(v=="bold") c.fontWeight=700;
        else if(v=="bolder") c.fontWeight=qMin(900, c.fontWeight+300);   // relative to inherited weight
        else if(v=="lighter") c.fontWeight=qMax(100, c.fontWeight-300);
        else { bool ok=false; const int w=v.toInt(&ok); if(ok && w>0) c.fontWeight=qBound(1,w,1000); }
        c.bold = c.fontWeight>=600; }   // keep the legacy flag in sync for any bold() reader
    else if(k=="font-style") c.italic=(v=="italic"||v=="oblique");
    else if(k=="text-anchor") c.anchor=(v=="middle"?1:(v=="end"?2:0));
}

// SVG presentation attributes (lower priority than author CSS, per the cascade).
void applyPresentation(Ctx& c, const QXmlStreamAttributes& a) {
    for (const QString& k : {QStringLiteral("fill"),QStringLiteral("stroke"),QStringLiteral("stroke-width"),
             QStringLiteral("opacity"),QStringLiteral("fill-opacity"),QStringLiteral("stroke-opacity"),
             QStringLiteral("stroke-linecap"),QStringLiteral("stroke-linejoin"),QStringLiteral("stroke-dasharray"),
             QStringLiteral("fill-rule"),QStringLiteral("font-size"),
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
    if (c.fillOn){ s.fill.type=FillType::Solid; s.fill.color=c.fill;
                   if(c.fillOpacity < 0.999) s.fill.color.setAlphaF(s.fill.color.alphaF()*c.fillOpacity); }
    else s.fill.type=FillType::None;
    s.stroke.enabled=c.strokeOn;
    if(c.strokeOn){ s.stroke.color=c.stroke; s.stroke.width=c.strokeW;
                    s.stroke.cap=c.cap; s.stroke.join=c.join;
                    // SVG's dash lengths are USER UNITS; QPen multiplies its pattern by the pen width,
                    // so a 10-unit dash on a 10-wide stroke came out 10x too long. Convert here, where
                    // the width is known.
                    s.stroke.dashes.clear();
                    if(!c.dashes.isEmpty()){
                        const double w = qMax(1e-6, c.strokeW);
                        for(qreal d : c.dashes) s.stroke.dashes.push_back(qMax(qreal(0.01), qreal(d / w)));
                    }
                    s.stroke.transp=qBound(0.0, 1.0-c.strokeOpacity, 1.0); }
    s.fillRule=c.fillRule;
    if(c.xqBlendType>0){ s.transp.type=c.xqBlendType; s.transp.geom=0; s.transp.t0=s.transp.t1=0.0; }
    if(c.xqBlur>0.0){ s.blur=qMin(c.xqBlur, 1000.0); s.blurGain=c.xqBlurGain; }
    if(!c.xqPressure.isEmpty() && s.stroke.enabled){      // x:y,x:y… along the stroke
        for(const QString& kv : c.xqPressure.split(QLatin1Char(','), Qt::SkipEmptyParts)){
            const int m = kv.indexOf(QLatin1Char(':'));
            if(m > 0) s.stroke.widthProfile.push_back(QPointF(kv.left(m).toDouble(), kv.mid(m+1).toDouble()));
        }
        s.stroke.varWidth = !s.stroke.widthProfile.isEmpty();
    }
    if(!c.xqShadow.isEmpty()){                  // dx dy blur #rgb alpha type
        const QStringList t = c.xqShadow.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if(t.size() >= 6){
            s.shadow.on = true; s.shadow.offset = QPointF(t[0].toDouble(), t[1].toDouble());
            s.shadow.blur = qBound(0.0, t[2].toDouble(), 1000.0);
            QColor sc(t[3]); sc.setAlphaF(qBound(0.0, t[4].toDouble(), 1.0));
            s.shadow.color = sc; s.shadow.type = qBound(0, t[5].toInt(), 2);
        }
    }
    if(!c.xqTransp.isEmpty()){                  // geom p0 p1 p2 stops(pos:transparency,…)
        const QStringList t = c.xqTransp.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if(t.size() >= 8){
            s.transp.type = 1; s.transp.geom = t[0].toInt();
            s.transp.p0 = QPointF(t[1].toDouble(), t[2].toDouble());
            s.transp.p1 = QPointF(t[3].toDouble(), t[4].toDouble());
            s.transp.p2 = QPointF(t[5].toDouble(), t[6].toDouble());
            s.transp.elliptical = s.transp.geom==2 && qAbs(QLineF(s.transp.p0,s.transp.p1).length()-QLineF(s.transp.p0,s.transp.p2).length())>1e-3;
            QVector<QPair<double,double>> st;
            for(const QString& e : t[7].split(QLatin1Char(','), Qt::SkipEmptyParts)){
                const int k=e.indexOf(QLatin1Char(':')); if(k>0) st.push_back({ qBound(0.0,e.left(k).toDouble(),1.0), qBound(0.0,e.mid(k+1).toDouble(),1.0) }); }
            if(st.size()>=2){
                s.transp.t0=st.first().second; s.transp.t1=st.last().second;
                for(int i=1;i+1<st.size();++i) s.transp.stops.push_back(TranspStop{ st[i].first, st[i].second });
            } else s.transp.type=0;
        }
    }
    if(!c.xqBitmap.isEmpty()){                  // a textured fill; a,b,c,d,e,f map the image's unit square
        QByteArray src; const QImage img = decodeImageHref(c.xqBitmap, QString(), &src);
        const QStringList m = c.xqBitmapXf.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if(!img.isNull() && m.size()==6){
            const double ma=m[0].toDouble(),mb=m[1].toDouble(),mc=m[2].toDouble(),md=m[3].toDouble(),me=m[4].toDouble(),mf=m[5].toDouble();
            s.fill.type=FillType::Bitmap; s.fill.image=img; s.fill.origImage=img; s.fill.imageSrc=src; s.fill.tiled=c.xqBitmapTiled;
            s.fill.p0=QPointF(me,mf); s.fill.p1=QPointF(me+ma,mf+mb); s.fill.p2=QPointF(me+mc,mf+md);
        }
    }
    s.opacity=qBound(0.0,c.opacity,1.0); s.transform=c.xf; s.blend=c.blend;
}

struct SvgStop { double off; QColor col; };
struct SvgGrad {
    bool radial=false, conical=false, userSpace=false, hasGT=false;
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
        // The radius endpoints go through the SAME mapping as the centre. Mapping only the centre
        // silently drops gradientTransform from the radii, which matters because a writer is free to
        // put ALL the geometry there: Affinity emits cx=0 cy=0 r=1 plus a gradientTransform, so a
        // centre-only mapping leaves a one-unit gradient — a flat slab of the last stop covering the
        // shape. Mapping the endpoints also makes rotated and elliptical radials come out right,
        // which p1/p2 can express but the old axis-aligned rw/rh could not.
        const QPointF ctr = toLocal(QPointF(g.cx,g.cy));
        QPointF px = toLocal(QPointF(g.cx+g.rr, g.cy)), py = toLocal(QPointF(g.cx, g.cy+g.rr));
        if (QLineF(ctr,px).length() < 1e-6) px = ctr + QPointF(1e-3, 0);   // degenerate r: keep a usable axis
        if (QLineF(ctr,py).length() < 1e-6) py = ctr + QPointF(0, 1e-3);
        s.fill.p0 = ctr; s.fill.p1 = px; s.fill.p2 = py;
    } else if (g.conical) {
        s.fill.type = FillType::Conical;
        s.fill.p0 = toLocal(QPointF(0,0)); s.fill.p1 = toLocal(QPointF(1,0));
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
    bool isSymbol=false, hasVB=false; double vbX=0,vbY=0,vbW=0,vbH=0; bool isClip=false; };  // isClip: this def-span is a <clipPath>'s geometry
struct PendUse { QString refId; QTransform useXf; double x=0,y=0,w=0,h=0; bool hasWH=false; bool inDefs=false; int seq=0; };
struct Frame { QString id; int spanStart=0, seqStart=0; bool inDefs=false, container=false;
    QTransform E; bool isSymbol=false, hasVB=false; double vbX=0,vbY=0,vbW=0,vbH=0; bool isClip=false;
    int groupId=-1; bool pushedGroup=false; };   // <g> preserved as a real Group: this frame opened group `groupId`
struct GPend { bool inDefs; int idx; QString grad; double fillOpacity = 1.0; };

// Fold a flat shape list into a nested Group tree, preserving each shape's <g> nesting path (`gp[i]` = the list of
// group ids the shape is inside, outermost first). Groups get an IDENTITY transform and the children keep their
// already-baked doc-space transforms, so the render is pixel-identical to the flattened output — only the STRUCTURE
// becomes real, selectable/enterable Groups. Shapes with an empty path stay top-level. Because a <g>'s descendants
// are emitted contiguously (depth-first walk), a simple open-groups stack rebuilds the tree in one pass.
static QVector<Shape> foldSvgGroups(const QVector<Shape>& flat, const QVector<QVector<int>>& gp){
    struct Open { int gid; Shape g; };
    QVector<Shape> roots; QVector<Open> open;
    auto commitTop = [&](){ Shape g = open.back().g; open.pop_back();
        if(open.isEmpty()) roots.push_back(g); else open.back().g.children.push_back(g); };
    for(int i=0;i<flat.size();++i){
        const QVector<int>& path = (i<gp.size()) ? gp[i] : QVector<int>{};
        int common=0; while(common<open.size() && common<path.size() && open[common].gid==path[common]) ++common;
        while(open.size()>common) commitTop();                       // leave groups this shape isn't in
        for(int k=common;k<path.size();++k){ Shape g; g.type=ShapeType::Group; g.name="g"; open.push_back({ path[k], g }); }
        if(open.isEmpty()) roots.push_back(flat[i]); else open.back().g.children.push_back(flat[i]);
    }
    while(!open.isEmpty()) commitTop();
    return roots;
}

// Is this element a non-rendering container whose content is only shown when <use>d?
bool isSuppressContainer(const QString& el){
    return el=="symbol"||el=="defs"||el=="clipPath"||el=="mask"||el=="marker"||el=="pattern"||el=="filter";
}

} // namespace

QSizeF svgViewportSize(const QByteArray& xml){
    QXmlStreamReader r(xml);
    while(!r.atEnd()){
        if(r.readNext() != QXmlStreamReader::StartElement) continue;
        if(r.name().toString() != QLatin1String("svg")) continue;
        const auto a = r.attributes();
        double vw = 0, vh = 0;
        if(a.hasAttribute("viewBox")){
            const auto vb = a.value("viewBox").toString().split(QRegularExpression("[\\s,]+"), Qt::SkipEmptyParts);
            if(vb.size() == 4){ vw = vb[2].toDouble(); vh = vb[3].toDouble(); }
        }
        // Same precedence parseSvg uses to build the viewBox transform.
        const double W = a.hasAttribute("width")  ? num(a.value("width").toString())  : vw;
        const double H = a.hasAttribute("height") ? num(a.value("height").toString()) : vh;
        if(W > 0 && H > 0) return QSizeF(W, H);
        return QSizeF();
    }
    return QSizeF();
}

QVector<Shape> parseSvg(const QByteArray& xml, QString* error, const QString& baseDir, QVector<QRectF>* pagesOut) {
    QVector<Shape> out, defsBuf;                       // defsBuf: shapes inside a non-rendering container (<defs>/<symbol>/…)
    QHash<QString,SvgGrad> grads; QString curGradId;
    QVector<GPend> pend;                               // gradient url() to resolve after the walk (records which buffer)
    QHash<QString,Def> defs; QVector<PendUse> pend2;   // <use> def-spans + pending instantiations
    int suppress=0, seq=0; QVector<Frame> idstk;       // idstk mirrors stk 1:1; suppress>0 => inside a container; seq = document-order counter
    const QVector<CssRule> cssRules = parseCss(xml);   // author <style> rules (parsed up-front, position-independent)
    QXmlStreamReader r(xml);
    struct ClipPend { bool inDefs; int idx; QString clipId; QTransform clipCTM; };
    QVector<ClipPend> clipPend;                        // clip-path url() to resolve after the walk (like gradients)
    struct BlurPend { bool inDefs; int idx; QString filterId; };
    QVector<BlurPend> blurPend;                        // filter=url() (feGaussianBlur) → Shape::blur, resolved after the walk
    QHash<QString,double> blurDefs; QString curFilterId;   // <filter id> → its feGaussianBlur stdDeviation (local user units)
    QVector<Ctx> stk; stk.push_back(Ctx{});
    QVector<QVector<int>> outGP;                        // parallel to `out`: each shape's <g> group-id nesting path (outermost first)
    QVector<int> groupStack; int nextGid=0;             // ids of the visible <g> elements currently open (empty ⇒ top-level)
    auto emitShape = [&](Shape& s){ if(suppress) defsBuf.push_back(s); else { out.push_back(s); outGP.push_back(groupStack); } };
    auto recClip = [&](const Ctx& cc){
        if(!cc.xqMask.isNull() && !(suppress? defsBuf : out).isEmpty()){
            Shape& sh = (suppress? defsBuf : out).last();
            bool inv = false; const QTransform t = sh.transform.inverted(&inv);
            if(inv){
                const QTransform m = cc.xqMaskXf * t;
                const double W = cc.xqMask.width(), H = cc.xqMask.height();
                if(!sh.transp.type) sh.transp.type = 1;
                sh.transp.geom = 4; sh.transp.t0 = 1.0; sh.transp.t1 = 0.0; sh.transp.maskBmp = cc.xqMask;
                sh.transp.p0 = m.map(QPointF(0, H)); sh.transp.p1 = m.map(QPointF(W, H)); sh.transp.p2 = m.map(QPointF(0, 0));
            }
        }
        if(!cc.clipId.isEmpty()) clipPend.push_back({ suppress>0, int((suppress? defsBuf : out).size())-1, cc.clipId, cc.clipCTM }); };
    auto recBlur = [&](const Ctx& cc){ if(!cc.filterId.isEmpty()) blurPend.push_back({ suppress>0, int((suppress? defsBuf : out).size())-1, cc.filterId }); };
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
                g.conical = a.value("xarq:conical").toString()==QLatin1String("1");
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
            // A label on a LAYER describes the layer, not each object in it — stamping it on every
            // leaf gives a thousand identically-named entries and throws away the per-element hint.
            // Only an object-level label is inherited. (Affinity has no groupmode, so its Desc still
            // reaches the drawable inside the layer group, which is what it describes.)
            if(a.value("inkscape:groupmode").toString() == QLatin1String("layer")) c.label.clear();
            else if(a.hasAttribute("inkscape:label"))  c.label = a.value("inkscape:label").toString();
            else if(a.hasAttribute("data-name"))       c.label = a.value("data-name").toString();
            c.xqTransp = a.value("xarq:transp").toString();
            c.xqPath = a.value("xarq:path").toString();
            if(a.hasAttribute("xarq:pmask")){
                const QByteArray b = a.value("xarq:pmask").toString().toLatin1();
                const QImage im = QImage::fromData(QByteArray::fromBase64(b), "PNG");
                const QStringList m = a.value("xarq:pmask-xf").toString().split(QLatin1Char(' '), Qt::SkipEmptyParts);
                if(!im.isNull() && m.size() == 6){
                    c.xqMask = im;
                    c.xqMaskXf = QTransform(m[0].toDouble(), m[1].toDouble(), m[2].toDouble(), m[3].toDouble(), m[4].toDouble(), m[5].toDouble()) * c.xf;
                }
            }
            c.xqShadow = a.value("xarq:shadow").toString();
            c.xqPressure = a.value("xarq:pressure").toString();
            c.xqBlur = a.value("xarq:blur").toString().toDouble();
            c.xqBlurGain = a.hasAttribute("xarq:blur-gain") ? qBound(0.01, a.value("xarq:blur-gain").toString().toDouble(), 100.0) : 1.0;
            c.xqPathStart = a.value("xarq:path-start").toString().toDouble();
            c.xqBitmap = a.value("xarq:bitmap").toString();
            c.xqBitmapXf = a.value("xarq:bitmap-xf").toString();
            c.xqBitmapTiled = a.value("xarq:bitmap-tiled").toString() == QLatin1String("1");
            const double inheritedOpacity = c.opacity;   // the product of every ancestor's opacity
            c.elemOpacity = 1.0;                          // NOT inherited: each element starts at fully opaque
            applyPresentation(c, a);                   // cascade: presentation attrs < author CSS < inline style
            applyCss(c, cssRules, el, a);
            applyInlineStyle(c, a);
            c.opacity = qBound(0.0, inheritedOpacity * c.elemOpacity, 1.0);
            if (a.hasAttribute("transform")) c.xf = parseTransform(a.value("transform").toString()) * c.xf;
            // clip-path="url(#id)" (attribute or inline style): the clip is resolved in THIS element's user space,
            // so remember the id + the CTM now in effect. Inherited by children (there is no group object to hold it
            // once the tree is flattened), so a clip on a <g> reaches every descendant shape. A child's own clip-path
            // replaces the inherited one (v1: nearest wins; nested intersection is not modelled).
            { QString cp = a.hasAttribute("clip-path") ? a.value("clip-path").toString() : QString();
              if (a.hasAttribute("style")) for (const QString& d : a.value("style").toString().split(';',Qt::SkipEmptyParts)){ int p=d.indexOf(':'); if(p>0 && d.left(p).trimmed()=="clip-path") cp=d.mid(p+1).trimmed(); }
              cp=cp.trimmed();
              if (cp.startsWith("url(")) { c.clipId=urlId(cp); c.clipCTM=c.xf; }
              else if (cp=="none") c.clipId.clear(); }
            // A <clipPath>'s geometry is interpreted in the referencing element's user space (default
            // clipPathUnits=userSpaceOnUse), NOT its DOM ancestors' — re-base its subtree to identity (+ its own
            // transform) so we capture clip-LOCAL coordinates, then re-project by clipCTM when a shape references it.
            if (el=="clipPath") c.xf = a.hasAttribute("transform") ? parseTransform(a.value("transform").toString()) : QTransform();
            // filter="url(#id)" (attribute or style): a pure feGaussianBlur filter → whole-object blur (Shape::blur).
            { QString fl = a.hasAttribute("filter") ? a.value("filter").toString() : QString();
              if (a.hasAttribute("style")) for (const QString& d : a.value("style").toString().split(';',Qt::SkipEmptyParts)){ int p=d.indexOf(':'); if(p>0 && d.left(p).trimmed()=="filter") fl=d.mid(p+1).trimmed(); }
              fl=fl.trimmed();
              if (fl.startsWith("url(")) c.filterId=urlId(fl); else if (fl=="none") c.filterId.clear(); }
            // <filter> def: remember the current filter id, and capture a feGaussianBlur's stdDeviation (local user
            // units; scaled to doc space at resolve). filters with only feColorMatrix/etc. never enter blurDefs → skipped.
            if (el=="filter") curFilterId = a.value("id").toString();
            else if (el=="feGaussianBlur" && !curFilterId.isEmpty()) {
                const auto parts = a.value("stdDeviation").toString().split(QRegularExpression("[\\s,]+"),Qt::SkipEmptyParts);
                double sd=0; if(parts.size()>=2) sd=(num(parts[0])+num(parts[1]))/2.0; else if(parts.size()==1) sd=num(parts[0]);
                if (sd>0) blurDefs[curFilterId] = qMax(blurDefs.value(curFilterId,0.0), sd);
            }
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
            Frame fr; fr.id=a.value("id").toString(); fr.container=container; fr.inDefs=childInDefs; fr.isClip=(el=="clipPath");
            fr.E=parentXf; fr.seqStart=mySeq;                      // E is the PARENT CTM so the element's own transform survives instantiation
            fr.spanStart=(childInDefs? defsBuf : out).size();
            // Preserve a visible <g> as a real Group: open a group id so every shape emitted inside it is tagged
            // (folded into a Group after the walk). A <g> inside <defs>/<symbol> (suppress>0) is a template, not folded.
            // Cap the nesting depth (mirrors the <use> depth cap): a crafted SVG of tens of thousands of nested <g>
            // would otherwise fold into an equally deep Group tree that overflows the stack in render/bounds/teardown
            // (all recurse per level). Past the cap, deeper <g>s don't open a group — their shapes flatten into the
            // deepest allowed group. 64 is far beyond any real artwork yet trivially safe to recurse.
            if (el=="g" && suppress==0 && groupStack.size()<64) { fr.groupId=nextGid++; fr.pushedGroup=true; groupStack.push_back(fr.groupId); }
            if (el=="symbol") { fr.isSymbol=true;
                if (a.hasAttribute("viewBox")) { const auto vb=a.value("viewBox").toString().split(QRegularExpression("[\\s,]+"),Qt::SkipEmptyParts);
                    if (vb.size()==4){ fr.hasVB=true; fr.vbX=vb[0].toDouble(); fr.vbY=vb[1].toDouble(); fr.vbW=vb[2].toDouble(); fr.vbH=vb[3].toDouble(); } } }
            if (container) ++suppress;                 // children now route to defsBuf
            // --- text: emit ONE shape per styled RUN, grouped into CHUNKS. A <tspan> with an absolute x (or a new
            //     y) opens a chunk; runs inside it flow left→right and share a baseline, and text-anchor centres/ends
            //     the WHOLE chunk (sum of run widths). This preserves each run's own colour, font and style — e.g.
            //     documento-2.svg's green "•" inside a white Arson-Pro title, and a normal run under an italic <text>.
            //     A style STACK tracks nested tspans (a nested run inherits its parent tspan, not the <text>).
            if (el=="text") {
                struct Run { QString txt; QFont font; QColor fill; bool fillOn; bool strokeOn; QColor stroke;
                             double strokeW; double opacity; QPainter::CompositionMode blend; };
                auto mkFont=[](const Ctx& cc){ QFont f(cc.fontFamily.isEmpty()? QStringLiteral("DejaVu Sans") : cc.fontFamily);
                    f.setPixelSize(qMax(1,qRound(cc.fontSize))); f.setWeight(QFont::Weight(qBound(1,cc.fontWeight,1000))); f.setItalic(cc.italic); return f; };
                const double tx=D("x"), ty=D("y");
                double penX=tx, penY=ty;
                QVector<Run> chunk; double chunkX=tx, chunkY=ty; int chunkAnchor=c.anchor;
                auto flushChunk=[&](){
                    if(chunk.isEmpty()) return;
                    double total=0; for(const Run& rn: chunk) total += QFontMetricsF(rn.font).horizontalAdvance(rn.txt);
                    const double px = chunkX + (chunkAnchor==1? -total/2.0 : (chunkAnchor==2? -total : 0.0));   // anchor the whole chunk
                    if(chunk.size()==1){
                        // A uniform-style line → a plain Text shape (no runs), byte-identical to the classic path.
                        const Run& rn=chunk[0];
                        Shape s; s.type=ShapeType::Text; s.text=rn.txt; s.font=rn.font; s.blend=rn.blend;
                        if(rn.fillOn){ s.fill.type=FillType::Solid; s.fill.color=rn.fill; } else s.fill.type=FillType::None;
                        s.stroke.enabled=rn.strokeOn; if(rn.strokeOn){ s.stroke.color=rn.stroke; s.stroke.width=rn.strokeW; }
                        s.opacity=qBound(0.0,rn.opacity,1.0);
                        s.transform = QTransform::fromTranslate(px, chunkY-QFontMetricsF(rn.font).ascent()) * c.xf;   // baseline at chunkY
                        s.name=c.label.isEmpty()?QStringLiteral("text"):c.label;
                        if(!c.xqPath.isEmpty()){ s.textPath=parsePath(c.xqPath); s.textPathOffset=c.xqPathStart*s.textPath.length(); }
                        emitShape(s); recClip(c); recBlur(c);
                    } else {
                        // Several styled runs sharing one baseline (e.g. a coloured word inside a title) → ONE Text shape
                        // carrying per-run textRuns, so it stays a single selectable/editable object (like Inkscape) rather
                        // than N fragments. layoutRuns() lays the mixed fonts/colours on the shared baseline.
                        double maxAsc=0; for(const Run& rn: chunk) maxAsc=qMax(maxAsc, QFontMetricsF(rn.font).ascent());
                        Shape s; s.type=ShapeType::Text; s.blend=chunk[0].blend;
                        QString full; QVector<TextRun> runs; int pos=0;
                        for(const Run& rn: chunk){
                            TextRun tr; tr.start=pos; tr.len=rn.txt.size(); tr.font=rn.font;
                            tr.color = rn.fillOn ? rn.fill : QColor(0,0,0,0);   // a no-fill run → transparent glyphs
                            runs.push_back(tr); full += rn.txt; pos += rn.txt.size();
                        }
                        s.text=full; s.textRuns=runs;
                        s.font=chunk[0].font;                                   // base (uncovered chars fall back; here all are covered)
                        if(chunk[0].fillOn){ s.fill.type=FillType::Solid; s.fill.color=chunk[0].fill; } else s.fill.type=FillType::None;
                        s.stroke.enabled=chunk[0].strokeOn;                     // text stroke is uniform-per-line (not per-run in the model)
                        if(chunk[0].strokeOn){ s.stroke.color=chunk[0].stroke; s.stroke.width=chunk[0].strokeW; }
                        s.opacity=qBound(0.0,chunk[0].opacity,1.0);
                        s.transform = QTransform::fromTranslate(px, chunkY-maxAsc) * c.xf;   // shared baseline at chunkY
                        s.name=c.label.isEmpty()?QStringLiteral("text"):c.label;
                        if(!c.xqPath.isEmpty()){ s.textPath=parsePath(c.xqPath); s.textPathOffset=c.xqPathStart*s.textPath.length(); }
                        emitShape(s); recClip(c); recBlur(c);
                    }
                    chunk.clear();
                };
                auto addRun=[&](const QString& raw, const Ctx& cc){ if(raw.isEmpty()) return;
                    const QFont f=mkFont(cc); const double op=qBound(0.0,cc.opacity,1.0);
                    if(!chunk.isEmpty()){ Run& last=chunk.back();   // merge consecutive runs with identical style into one shape
                        if(last.font==f && last.fillOn==cc.fillOn && last.fill==cc.fill && last.strokeOn==cc.strokeOn
                           && (!cc.strokeOn || (last.stroke==cc.stroke && last.strokeW==cc.strokeW))
                           && last.blend==cc.blend && qAbs(last.opacity-op)<1e-9){ last.txt+=raw; return; } }
                    // Text builds its fill directly rather than through styleShape, so fill-opacity has
                    // to be folded in here or it is dropped for <text> alone.
                    QColor rf = cc.fill;
                    if(cc.fillOpacity < 0.999) rf.setAlphaF(rf.alphaF()*cc.fillOpacity);
                    chunk.push_back(Run{ raw, f, rf, cc.fillOn, cc.strokeOn, cc.stroke, cc.strokeW, op, cc.blend }); };
                QVector<Ctx> st; st.push_back(c);   // style stack (top = current run style; nested tspans inherit their parent)
                int depth=1;
                while(!r.atEnd() && depth>0){
                    const auto t=r.readNext();
                    if(t==QXmlStreamReader::StartElement){ ++depth;
                        Ctx tc = st.back();
                        if(r.name()==QStringLiteral("tspan")){
                            const auto ta=r.attributes();
                            applyPresentation(tc,ta); applyCss(tc,cssRules,QStringLiteral("tspan"),ta); applyInlineStyle(tc,ta);
                            const bool hasX=ta.hasAttribute("x"), hasY=ta.hasAttribute("y");
                            double nx=penX, ny=penY;
                            if(hasX) nx=num(ta.value("x").toString());
                            if(ta.hasAttribute("dx")) nx+=num(ta.value("dx").toString());
                            if(hasY) ny=num(ta.value("y").toString());
                            if(ta.hasAttribute("dy")) ny+=num(ta.value("dy").toString());
                            // A new absolute position (fresh x, or a y that moves the baseline) opens a new chunk/line.
                            if((hasX && qAbs(nx-chunkX)>0.001) || (hasY && qAbs(ny-chunkY)>0.5)){ flushChunk(); chunkX=nx; chunkY=ny; chunkAnchor=tc.anchor; }
                            penX=nx; penY=ny;
                        }
                        st.push_back(tc);
                    } else if(t==QXmlStreamReader::Characters){ if(!r.isWhitespace()) addRun(r.text().toString(), st.back()); }
                    else if(t==QXmlStreamReader::EndElement){ --depth; if(st.size()>1) st.pop_back(); }
                }
                flushChunk();
                continue;   // the walk above consumed </text>; no stack push, so no matching pop (text is not a <use> target)
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
                QByteArray imgSrc; QImage img = decodeImageHref(href, baseDir, &imgSrc);
                if (!img.isNull()) {
                    const double x=D("x"), y=D("y");
                    const double w = a.hasAttribute("width")  ? num(a.value("width").toString())  : img.width();
                    const double h = a.hasAttribute("height") ? num(a.value("height").toString()) : img.height();
                    if (w>0 && h>0) {
                        Shape s; s.type=ShapeType::Rect; s.rect=QRectF(x,y,w,h);
                        s.fill.type=FillType::Bitmap; s.fill.image=img; s.fill.origImage=img; s.fill.imageSrc=imgSrc;
                        // Bitmap-fill convention (see render.cpp bitmapTexXf): image top-left(0,0)->p2, bottom-left(0,H)->p0.
                        // SVG places an image y-DOWN (its 0,0 at the rect's top-left = (x,y)), so p2=top-left, p0=bottom-left.
                        s.fill.p0=QPointF(x,y+h); s.fill.p1=QPointF(x+w,y+h); s.fill.p2=QPointF(x,y);
                        s.stroke.enabled=false; s.opacity=qBound(0.0,c.opacity,1.0); s.transform=c.xf; s.name=c.label.isEmpty()?QStringLiteral("image"):c.label; s.blend=c.blend;
                        emitShape(s); recClip(c); recBlur(c);
                    }
                }
                stk.push_back(c); idstk.push_back(fr); continue;
            }
            // Inkscape 1.2+ multi-page: <inkscape:page x y width height> inside <sodipodi:namedview>. Record the
            // page rect in document space (c.xf carries the root viewBox transform, so it matches the shapes).
            if (el=="page" && a.hasAttribute("width") && a.hasAttribute("height")) {
                const double pw=D("width"), ph=D("height");
                if (pw>0 && ph>0 && pagesOut) pagesOut->push_back(c.xf.mapRect(QRectF(D("x"),D("y"),pw,ph)));
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
            if (made){ styleShape(s, c);
                // inkscape:label (and Adobe/Sketch's data-name) carry the layer's real name. Without
                // this every imported object is called after its SVG element, so a document with a
                // thousand named layers arrives completely anonymous in the Name Gallery.
                s.name = c.label.isEmpty() ? el : c.label;
                emitShape(s);
                if (!c.fillUrl.isEmpty()) pend.push_back({ suppress>0, int((suppress? defsBuf : out).size())-1, c.fillUrl, c.fillOpacity });
                recClip(c); recBlur(c); }
            stk.push_back(c); idstk.push_back(fr);
        } else if (tok == QXmlStreamReader::EndElement) {
            const QString en = r.name().toString();
            if (en=="linearGradient" || en=="radialGradient") curGradId.clear();
            if (en=="filter") curFilterId.clear();
            if (stk.size()>1) {
                stk.pop_back();
                Frame f = idstk.back(); idstk.pop_back();
                if (f.container && suppress>0) --suppress;
                if (f.pushedGroup && !groupStack.isEmpty()) groupStack.pop_back();   // close the <g>'s group scope
                if (!f.id.isEmpty()) defs.insert(f.id, Def{ f.spanStart, int((f.inDefs? defsBuf : out).size()), f.inDefs, f.E, f.seqStart, seq, f.isSymbol, f.hasVB, f.vbX, f.vbY, f.vbW, f.vbH, f.isClip });
            }
        }
    }
    // 1) resolve gradients on the ORIGINALS (both buffers), in local space, before cloning
    for (const GPend& pr : pend) { QVector<Shape>& buf = pr.inDefs? defsBuf : out;
        if (pr.idx<0 || pr.idx>=buf.size()) continue;
        resolveGradient(buf[pr.idx], pr.grad, grads);
        // fill-opacity applies to a gradient fill too, and the renderer builds its brush from the
        // STOPS — folding it into Fill::color (which a gradient never reads) silently dropped it.
        if (pr.fillOpacity < 0.999) {
            for (auto& st : buf[pr.idx].fill.stops) st.color.setAlphaF(st.color.alphaF()*pr.fillOpacity);
            if (buf[pr.idx].fill.type==FillType::Solid)
                buf[pr.idx].fill.color.setAlphaF(buf[pr.idx].fill.color.alphaF()*pr.fillOpacity);   // single-stop fallback
        } }
    // 1b) resolve clip-path → Shape::regionPath (local space). Build each <clipPath>'s geometry from its def-span
    //     (clip-local coords), then for each clipped shape: clip in doc space (clipCTM·clipGeom) mapped back into
    //     the shape's local space. Done BEFORE <use> instantiation so clones inherit the resolved region.
    { QHash<QString,QPainterPath> clipGeom;
      for (auto it=defs.constBegin(); it!=defs.constEnd(); ++it) { const Def& d=it.value(); if(!d.isClip) continue;
          const QVector<Shape>& src = d.inDefs? defsBuf : out;
          QPainterPath cpp; cpp.setFillRule(Qt::WindingFill);
          for (int k=d.start; k<d.end && k<src.size(); ++k){ cpp.addPath(src[k].transform.map(src[k].localPath()));
              if(d.end-d.start==1 && src[k].fillRule==Qt::OddEvenFill) cpp.setFillRule(Qt::OddEvenFill); }
          if (!cpp.isEmpty()) clipGeom.insert(it.key(), cpp); }
      for (const ClipPend& cpn : clipPend) { QVector<Shape>& buf = cpn.inDefs? defsBuf : out;
          if (cpn.idx<0 || cpn.idx>=buf.size() || !clipGeom.contains(cpn.clipId)) continue;
          bool inv=false; const QTransform tinv = buf[cpn.idx].transform.inverted(&inv); if(!inv) continue;
          const QPainterPath clipDoc = cpn.clipCTM.map(clipGeom.value(cpn.clipId));
          // Wholly inside a plain rectangle, the clip does nothing, and leaving it off keeps the object simple.
          if(clipDoc.elementCount() <= 6 && clipDoc.contains(clipDoc.boundingRect().adjusted(0.01, 0.01, -0.01, -0.01))
             && clipDoc.boundingRect().adjusted(-0.5, -0.5, 0.5, 0.5).contains(buf[cpn.idx].docBounds())) continue;
          buf[cpn.idx].regionPath = tinv.map(clipDoc); } }
    // 1c) resolve feGaussianBlur filters → Shape::blur. stdDeviation is in the shape's local user units; Shape::blur
    //     is in DOC units (paintBlurred scales it by the doc→device transform), so multiply by the shape transform's scale.
    //     paintBlurred's gaussBlur uses an effective sigma of blur/2 (tuned for Xara feather spread), so an SVG blur of
    //     stdDeviation σ would render at only ~σ/2 — half of what Inkscape/librosvg draw (a true gaussian of sigma σ).
    //     Multiply by BLUR_STDDEV_TO_XARQ so the rendered spread matches: measured against Inkscape, ~1.8× lands the
    //     visually-dominant (larger) blurs within a few % across stdDeviation 5–31.
    constexpr double BLUR_STDDEV_TO_XARQ = 1.8;
    for (const BlurPend& bp : blurPend) { QVector<Shape>& buf = bp.inDefs? defsBuf : out;
        if (bp.idx<0 || bp.idx>=buf.size() || !blurDefs.contains(bp.filterId)) continue;
        const double sc = std::sqrt(std::abs(buf[bp.idx].transform.determinant()));
        const double b = blurDefs.value(bp.filterId) * sc * BLUR_STDDEV_TO_XARQ;
        if (b>0.4) buf[bp.idx].blur = qMax(buf[bp.idx].blur, b); }
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
    // Hard budgets independent of the per-branch depth/cycle guards: `visited` is passed BY VALUE, so a
    // chained fan-out (g0 has 8 <use> of g1, g1 has 8 of g2, …) has distinct ids per branch and would
    // otherwise expand ~8^12 clones from a tiny file → CPU hang + OOM. Bound total calls AND total emitted.
    int useCalls=0;
    std::function<void(const QString&, const QTransform&, int, QSet<QString>)> instantiate =
      [&](const QString& refId, const QTransform& U, int depth, QSet<QString> visited){
        if (++useCalls>200000 || out.size()>200000 || depth>12 || visited.contains(refId) || !defs.contains(refId)) return;
        visited.insert(refId);
        const Def d = defs.value(refId);
        const QTransform Einv = d.E.inverted();         // computed once; a singular parent CTM (e.g. scale(0)) yields identity → clone collapses harmlessly
        const QVector<Shape>& src = d.inDefs? defsBuf : out;
        QVector<Shape> clones;                          // snapshot BEFORE appending (out may reallocate; src aliases out)
        for (int k=d.start; k<d.end && k<src.size(); ++k) { Shape copy=src[k];
            copy.transform = src[k].transform * Einv * U; clones.push_back(copy); }
        for (Shape& cs : clones) out.push_back(cs);
        for (const PendUse& pu : pend2) {               // nested <use> that lived inside this def's subtree (document-order range)
            if (useCalls>200000 || out.size()>200000) break;
            if (pu.seq>d.seqStart && pu.seq<=d.seqEnd)
                instantiate(pu.refId, placement(pu.refId, pu.useXf, pu.x,pu.y,pu.hasWH,pu.w,pu.h) * Einv * U, depth+1, visited);
        }
      };
    for (const PendUse& pu : pend2)                     // top-level: a use renders at its own site iff not inside a container
        if (!pu.inDefs) instantiate(pu.refId, placement(pu.refId, pu.useXf, pu.x,pu.y,pu.hasWH,pu.w,pu.h), 0, {});
    if (r.hasError() && error) *error = r.errorString();
    // Fold <g> membership into real Group shapes (identity groups + baked children ⇒ render unchanged). <use> clones
    // were appended after the walk without a group path, so they stay top-level (padded empty).
    bool anyGroup=false; for(const auto& p : outGP) if(!p.isEmpty()){ anyGroup=true; break; }
    if (anyGroup) {
        // An outermost <g> enclosing the ENTIRE document is a layer/wrapper (Illustrator & Inkscape both emit one),
        // not a grabbable group — folding it whole means a single click grabs the entire artwork. Strip the group-id
        // prefix common to every walk-emitted shape so a click lands on an individual object / its own sub-group.
        int lcp=-1;
        for(const auto& p : outGP){ if(lcp<0){ lcp=p.size(); continue; }
            int c=0; while(c<lcp && c<p.size() && outGP[0][c]==p[c]) ++c; lcp=c; if(!lcp) break; }
        if(lcp>0) for(auto& p : outGP) p.remove(0, qMin(lcp,int(p.size())));
        while(outGP.size()<out.size()) outGP.push_back({});
        return foldSvgGroups(out, outGP);
    }
    return out;
}

// Public wrapper (external linkage) over the anonymous-namespace parsePath, so the ODF importer can
// reuse the same SVG path-data parser for <draw:path svg:d="…">.
QPainterPath parseSvgPathData(const QString& d){ return parsePath(d); }

} // namespace xara
