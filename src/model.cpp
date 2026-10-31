#include "model.h"
#include "hyphen.h"
#include "otshape.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QFontMetricsF>
#include <QtMath>
#include <QBuffer>

namespace xara {

QColor lerp(const QColor& a, const QColor& b, double t) {
    t = qBound(0.0, t, 1.0);
    return QColor::fromRgbF(a.redF()  + (b.redF()  - a.redF())  * t,
                            a.greenF()+ (b.greenF()- a.greenF())* t,
                            a.blueF() + (b.blueF() - a.blueF()) * t,
                            a.alphaF()+ (b.alphaF()- a.alphaF())* t);
}

QVector<QPair<QString, QPainter::CompositionMode>> blendModes() {
    return {
        {"Normal",     QPainter::CompositionMode_SourceOver},
        {"Multiply",   QPainter::CompositionMode_Multiply},
        {"Screen",     QPainter::CompositionMode_Screen},
        {"Overlay",    QPainter::CompositionMode_Overlay},
        {"Darken",     QPainter::CompositionMode_Darken},
        {"Lighten",    QPainter::CompositionMode_Lighten},
        {"Colour Dodge", QPainter::CompositionMode_ColorDodge},
        {"Colour Burn",  QPainter::CompositionMode_ColorBurn},
        {"Hard Light", QPainter::CompositionMode_HardLight},
        {"Soft Light", QPainter::CompositionMode_SoftLight},
        {"Difference", QPainter::CompositionMode_Difference},
        {"Exclusion",  QPainter::CompositionMode_Exclusion},
        {"Plus",       QPainter::CompositionMode_Plus},
    };
}
QString blendModeName(QPainter::CompositionMode m) {
    for (auto& p : blendModes()) if (p.second == m) return p.first;
    return "Normal";
}

// ---- parametric SmartShapes ------------------------------------------------
// Each returns a path in a nominal 0..100 box; `param` is the live count/teeth/points.
// Normalised (0..1) value of `param2` that reproduces each kind's original hardcoded secondary constant,
// so a SmartShape with the default smartParam2=-1 and one explicitly seeded to this value render identically.
double Shape::smartParam2Default(int kind) {
    switch (kind) {
    case 0: return (18.0-6.0)/40.0;   // chevron notch depth 18
    case 1: case 2: return (34.0-20.0)/24.0;  // gear inner radius 34
    case 3: return 0.0;               // banner swallowtail tip inset 0
    case 4: return (15.0-6.0)/24.0;   // arrow shaft half-thickness 15
    case 5: return (20.0-8.0)/38.0;   // star inner radius 20
    case 6: return 22.0/100.0;        // callout tail tip x 22
    default: return 0.0;
    }
}
QPainterPath Shape::smartPath(int kind, double param, double param2) {
    const int n = qBound(2, int(param+0.5), 64);
    // param2 in [0,1] shapes a secondary dimension; <0 => the kind's original constant (byte-identical).
    const double q2 = param2 >= 0.0 ? qBound(0.0, param2, 1.0) : -1.0;
    switch (kind) {
    case 0: { // chevron row — param2 = notch depth
        QPainterPath p; double notch = q2>=0 ? 6.0+q2*40.0 : 18.0, seg=100.0/qMax(1,n);
        for(int i=0;i<n;++i){ double x=i*seg; QPolygonF q; q<<QPointF(x,15)<<QPointF(x+seg-6,15)<<QPointF(x+seg-6+notch*0.4,50)<<QPointF(x+seg-6,85)<<QPointF(x,85)<<QPointF(x+notch*0.4,50);
            QPainterPath cp; cp.addPolygon(q); cp.closeSubpath(); p=p.united(cp); } return p; }
    case 1: case 2: { // gear / cog — param2 = inner (valley) radius
        int N=qMax(4,n); double inner = q2>=0 ? 20.0+q2*24.0 : 34.0;
        QPainterPath p; for(int i=0;i<N*2;++i){ double a=i*M_PI/N; double r=(i&1)?inner:48; QPointF v(50+r*std::cos(a),50+r*std::sin(a)); if(i==0)p.moveTo(v);else p.lineTo(v);} p.closeSubpath();
        QPainterPath h; h.addEllipse(QPointF(50,50),15,15); return p.subtracted(h); }
    case 3: { // banner with swallowtails — param2 = tail tip inset
        double ti = q2>=0 ? q2*12.0 : 0.0;
        QPainterPath p; p.addRect(12,28,76,44); QPainterPath l; l.moveTo(12,28); l.lineTo(ti,40); l.lineTo(12,52); l.closeSubpath();
        QPainterPath r; r.moveTo(88,28); r.lineTo(100-ti,40); r.lineTo(88,52); r.closeSubpath(); return p.united(l).united(r); }
    case 4: { // block arrow — param2 = shaft half-thickness
        double hf = q2>=0 ? 6.0+q2*24.0 : 15.0;
        QPainterPath p; QPolygonF q; q<<QPointF(8,50-hf)<<QPointF(62,50-hf)<<QPointF(62,18)<<QPointF(96,50)<<QPointF(62,82)<<QPointF(62,50+hf)<<QPointF(8,50+hf); p.addPolygon(q); p.closeSubpath(); return p; }
    case 5: { // N-point star — param2 = inner radius
        int m=qMax(3,n); double inner = q2>=0 ? 8.0+q2*38.0 : 20.0;
        QPainterPath p; for(int i=0;i<m*2;++i){ double a=-M_PI/2+i*M_PI/m; double r=(i&1)?inner:46; QPointF v(50+r*std::cos(a),50+r*std::sin(a)); if(i==0)p.moveTo(v);else p.lineTo(v);} p.closeSubpath(); return p; }
    case 6: { // speech callout — param2 = tail tip x
        double tx = q2>=0 ? q2*100.0 : 22.0;
        QPainterPath p; p.addRoundedRect(QRectF(8,10,84,56),10,10); QPainterPath t; t.moveTo(28,64); t.lineTo(tx,90); t.lineTo(48,64); t.closeSubpath(); return p.united(t); }
    default: { QPainterPath p; p.addRect(0,0,100,100); return p; }
    }
}

// ---- Table -----------------------------------------------------------------
Table Table::grid(int r, int c, double w, double h) {
    Table t; r=qMax(1,r); c=qMax(1,c);
    for (int i=0;i<c;++i) t.colW.append(w/c);
    for (int i=0;i<r;++i) t.rowH.append(h/r);
    t.cells.resize(r*c);
    return t;
}

// ---- geometry --------------------------------------------------------------
QPainterPath Shape::localPath() const {
    QPainterPath p;
    overflowStart = -1;   // reset per layout; only the frame word-wrap branch below may set it (linked/threaded frames)
    switch (type) {
    case ShapeType::Table:
        p.addRect(QRectF(0, 0, table.width(), table.height()));
        return p;
    case ShapeType::Rect:
        if (cornerRadius > 0.01) p.addRoundedRect(rect, cornerRadius, cornerRadius);
        else                     p.addRect(rect);
        break;
    case ShapeType::Ellipse:
        if (ellipseMode==0 || qAbs(ellipseSweep)>=360.0) {
            p.addEllipse(rect);                                          // whole ellipse (default) — byte-identical
        } else if (ellipseMode==1) {                                    // PIE / wedge to the centre
            p.moveTo(rect.center()); p.arcTo(rect, ellipseStart, ellipseSweep); p.closeSubpath();
        } else {                                                        // ARC (open) / SEGMENT (chord)
            p.arcMoveTo(rect, ellipseStart); p.arcTo(rect, ellipseStart, ellipseSweep);
            if (ellipseMode==3) p.closeSubpath();
        }
        break;
    case ShapeType::Polygon: {
        const QPointF c = rect.center();
        const double rx = rect.width()/2.0, ry = rect.height()/2.0;
        const int n = qMax(3, polySides);
        const double a0 = -M_PI/2.0;
        QVector<QPointF> pts;
        if (polyStar) {
            const double ri = polyStarDepth;
            for (int i = 0; i < n*2; ++i) { double a = a0 + i*M_PI/n; double r = (i&1)?ri:1.0;
                pts.push_back(QPointF(c.x()+qCos(a)*rx*r, c.y()+qSin(a)*ry*r)); }
        } else {
            for (int i = 0; i < n; ++i) { double a = a0 + i*2*M_PI/n;
                pts.push_back(QPointF(c.x()+qCos(a)*rx, c.y()+qSin(a)*ry)); }
        }
        const int m = pts.size();
        if (qAbs(polyCurvature) >= 1e-4) {                              // bowed edges → flower / gear (smooth closed curve)
            auto tang=[&](int i)->QPointF{ QPointF r=pts[i]-c; QPointF t(-r.y(),r.x()); double L=std::hypot(t.x(),t.y()); return L>1e-9? t/L : QPointF(0,0); };
            p.moveTo(pts[0]);
            for (int i=0;i<m;++i){ const QPointF& A=pts[i]; const QPointF& B=pts[(i+1)%m];
                const double h = polyCurvature * std::hypot(B.x()-A.x(), B.y()-A.y()) * 0.3642;
                p.cubicTo(A + tang(i)*h, B - tang((i+1)%m)*h, B); }
            p.closeSubpath();
        } else if (cornerRadius > 0.01) {                              // live rounded corners (quadratic fillet at each vertex)
            for (int i=0;i<m;++i){ const QPointF& V=pts[i];
                QPointF dp=pts[(i-1+m)%m]-V; double lp=std::hypot(dp.x(),dp.y()); if(lp>1e-9) dp/=lp;
                QPointF dn=pts[(i+1)%m]-V;   double ln=std::hypot(dn.x(),dn.y()); if(ln>1e-9) dn/=ln;
                const double t = qMin(cornerRadius, qMin(lp,ln)*0.5);
                const QPointF A=V+dp*t, Bc=V+dn*t;
                if (i==0) p.moveTo(A); else p.lineTo(A);
                p.quadTo(V, Bc); }
            p.closeSubpath();
        } else {                                                       // straight edges — byte-identical to before
            for (int i=0;i<m;++i){ if(i==0) p.moveTo(pts[i]); else p.lineTo(pts[i]); }
            p.closeSubpath();
        }
        break;
    }
    case ShapeType::Path:
        p = path;
        break;
    case ShapeType::Text: {
        // Local, possibly letter-spaced copy of the base font (tracking==0 => identical to the member).
        QFont font = this->font; if (tracking!=0.0) font.setLetterSpacing(QFont::AbsoluteSpacing, tracking);
        if(textAspect!=1.0) font.setStretch(qBound(1,int(textAspect*100.0+0.5),400));
        QPainterPath tp; QFontMetricsF fm(font);
        const double lineH = fm.height()*lineSpacing, asc = fm.ascent();
        if (!textPath.isEmpty()) {                          // TEXT ON A CURVE: flow glyphs along the baseline path
            const double len=textPath.length();
            // "Inside" flip: reverse the visual order and rotate each glyph 180° so the text
            // reads upright along the reverse (under) side of the path — Xara's flip-to-inside.
            QString seq; for (const QChar ch : text) if (ch!='\n') seq+=ch;
            if (textPathInside){ QString r; r.reserve(seq.size()); for (int i=seq.size()-1;i>=0;--i) r+=seq[i]; seq=r; }
            double x=textPathOffset;                        // start distance along the path (0 = at the start)
            for (const QChar ch : seq) {
                double adv=fm.horizontalAdvance(ch);
                double mid=x+adv/2; if (mid>len) break;
                double pct=textPath.percentAtLength(mid);
                QPointF pt=textPath.pointAtPercent(pct); double ang=textPath.angleAtPercent(pct);
                if (!(ch.isSpace())) { QPainterPath gp; gp.addText(0,0,font,QString(ch));
                    QTransform t; t.translate(pt.x(),pt.y());
                    if (textPathInside){ t.rotate(-ang+180); t.translate(-adv/2, asc); }   // hang glyph on the far side, upright
                    else               { t.rotate(-ang);     t.translate(-adv/2, 0);   }
                    tp.addPath(t.map(gp)); }
                x+=adv;
            }
            p=tp; p.setFillRule(fillRule); return p;
        }
        if (!textRuns.isEmpty()) {                          // RICH TEXT: mixed per-character fonts/colours
            QPainterPath u; for (const auto& pr : layoutRuns()) u.addPath(pr.first);
            u.setFillRule(fillRule); p=u; return p;
        }
        if (!otFeatures.isEmpty() && !textFrame && !smallCaps) {   // OPENTYPE (R9): HarfBuzz-shaped point text
            QPainterPath u; double yb=asc; bool missing=false;
            for (const QString& line : text.split('\n')) {
                if (!line.isEmpty()){ bool m=false; u.addPath(otShapeLine(line, font, otFeatures, tracking, nullptr, nullptr, &m).translated(0, yb)); missing = missing||m; }
                yb += lineH;
            }
            if (!missing) { u.setFillRule(fillRule); p=u; return p; }   // else fall through to Qt's layout (font substitution)
        }
        // Small-caps support: a smaller font for lowercase letters (rendered uppercased).
        QFont scFont=font; if(font.pointSizeF()>0) scFont.setPointSizeF(font.pointSizeF()*0.78); else scFont.setPixelSize(qMax(1,int((font.pixelSize()>0?font.pixelSize():12)*0.78)));
        QFontMetricsF scFm(scFont);
        auto runWidth=[&](const QString& s)->double{
            if(!smallCaps) return fm.horizontalAdvance(s);
            double wd=0; for(const QChar c: s){ if(c.isLetter()&&c.isLower()) wd+=scFm.horizontalAdvance(c.toUpper()); else wd+=fm.horizontalAdvance(c); } return wd; };
        auto emitRun=[&](double cx,double y,const QString& s){        // render a tab-free run at cx (honours small-caps)
            if(!smallCaps){ tp.addText(cx, y, font, s); return; }
            double sx=cx; for(const QChar c: s){ if(c.isLetter()&&c.isLower()){ tp.addText(sx,y,scFont,QString(c.toUpper())); sx+=scFm.horizontalAdvance(c.toUpper()); } else { tp.addText(sx,y,font,QString(c)); sx+=fm.horizontalAdvance(c); } } };
        auto place=[&](double x,double y,const QString& s,double w,bool justify=false){
            // Explicit tab stops (opt-in: tabStops non-empty). Each \t jumps to the next stop to the right and
            // aligns the following segment left/centre/right/decimal, filling the gap with the stop's leader glyph.
            if(!tabStops.isEmpty() && s.contains('\t')){
                double cx=x; const QStringList segs=s.split('\t');
                if(!segs[0].isEmpty()){ emitRun(cx,y,segs[0]); cx+=runWidth(segs[0]); }   // first segment: left at the line origin
                for(int i=1;i<segs.size();++i){
                    const double rel=cx-x; const TabStop* stop=nullptr;
                    for(const TabStop& ts: tabStops){ if(ts.pos>rel+0.01){ stop=&ts; break; } }
                    const double segW=runWidth(segs[i]);
                    double segStart=cx;
                    if(stop){ const double target=x+stop->pos;
                        if(stop->align==1) segStart=target-segW/2;                          // centre
                        else if(stop->align==2) segStart=target-segW;                       // right
                        else if(stop->align==3){ int dot=segs[i].indexOf('.'); if(dot<0) dot=segs[i].indexOf(','); if(dot<0) dot=segs[i].size();
                            segStart=target-runWidth(segs[i].left(dot)); }                  // decimal: align the point
                        else segStart=target;                                               // left
                        if(!stop->leader.isNull() && stop->leader.unicode()!=0 && segStart>cx+0.5){    // leader fill
                            const double lw=runWidth(QString(stop->leader));
                            if(lw>0.5){ const int nc=qMin(1000, int((segStart-cx)/lw)); for(int j=0;j<nc;++j) emitRun(cx+j*lw, y, QString(stop->leader)); } }
                    }
                    if(segStart<cx) segStart=cx;                                            // never run backwards over the previous segment
                    if(!segs[i].isEmpty()) emitRun(segStart,y,segs[i]);
                    cx=segStart+segW;
                }
                return;
            }
            // Regular tab stops (opt-in: tabWidth>0). Segments between \t advance to the next multiple of tabWidth
            // from the line origin. Left-origin only (alignment ignored on tabbed lines). tabWidth==0 → byte-identical.
            if(tabWidth>0.0 && s.contains('\t')){
                double cx=x; const QStringList segs=s.split('\t');
                for(int i=0;i<segs.size();++i){ if(!segs[i].isEmpty()){ emitRun(cx,y,segs[i]); cx+=runWidth(segs[i]); }
                    if(i<segs.size()-1){ double rel=cx-x; cx = x + (std::floor(rel/tabWidth)+1.0)*tabWidth; } }
                return;
            }
            double sw=runWidth(s); double off=0; if(!textFrame){} else if(textAlign==1) off=(w-sw)/2; else if(textAlign==2) off=w-sw;
            // Justified: on a non-last line of a paragraph, spread the slack across the inter-word gaps.
            if(justify && textAlign==3 && w>sw+0.5){
                const QStringList words=s.split(' ',Qt::SkipEmptyParts);
                if(words.size()>=2){
                    const double spaceW=runWidth(" "); double natural=(words.size()-1)*spaceW;
                    for(const QString& wd: words) natural+=runWidth(wd);
                    const double extra=(w-natural)/(words.size()-1);
                    double cx=x;
                    for(int wi=0; wi<words.size(); ++wi){
                        if(!smallCaps) tp.addText(cx,y,font,words[wi]);
                        else { double c2=cx; for(const QChar c: words[wi]){ if(c.isLetter()&&c.isLower()){ tp.addText(c2,y,scFont,QString(c.toUpper())); c2+=scFm.horizontalAdvance(c.toUpper()); } else { tp.addText(c2,y,font,QString(c)); c2+=fm.horizontalAdvance(c); } } }
                        cx+=runWidth(words[wi])+spaceW+extra;
                    }
                    return;
                }
            }
            if(!smallCaps){ tp.addText(x+off, y, font, s); return; }
            double cx=x+off; for(const QChar c: s){ if(c.isLetter()&&c.isLower()){ tp.addText(cx,y,scFont,QString(c.toUpper())); cx+=scFm.horizontalAdvance(c.toUpper()); } else { tp.addText(cx,y,font,QString(c)); cx+=fm.horizontalAdvance(c); } } };
        if (textFrame && rect.width()>4 && !textInlineBoxes.isEmpty()) {     // text AREA with INLINE anchored graphics
            p = layoutFrameInline(nullptr); p.setFillRule(fillRule); return p;
        }
        if (textFrame && rect.width()>4 && !textWrapObstacles.isEmpty()) {   // text AREA with REPEL: flow around obstacles
            const double W=rect.width();
            // free x-segments of a horizontal band [y0,y1): [0,W] minus obstacle x-ranges overlapping the band
            auto freeSegs=[&](double y0,double y1)->QVector<QPair<double,double>>{
                QVector<QPair<double,double>> occ;
                for(const QRectF& o: textWrapObstacles){ if(o.bottom()<=y0 || o.top()>=y1) continue;
                    double a=qMax(0.0,o.left()), b=qMin(W,o.right()); if(b>a) occ.push_back({a,b}); }
                std::sort(occ.begin(),occ.end());
                QVector<QPair<double,double>> segs; double cur=0;
                for(const auto& iv: occ){ if(iv.first>cur+0.5) segs.push_back({cur,iv.first}); cur=qMax(cur,iv.second); }
                if(cur<W-0.5) segs.push_back({cur,W});
                return segs; };
            const double colH=rect.height()>4?rect.height():1e9;
            double y=0;
            for(const QString& para: text.split('\n')){
                QStringList words = para.isEmpty()? QStringList() : para.split(' ');
                int wi=0; bool firstLineOfPara=true;
                do {
                    if(y+lineH>colH && y>0) { y=colH+lineH; break; }   // overflow past the frame: stop
                    QVector<QPair<double,double>> segs=freeSegs(y, y+lineH);
                    int wiAtLineStart=wi;
                    for(const auto& seg: segs){
                        double x=seg.first, segW=seg.second-seg.first; QString chunk;
                        while(wi<words.size()){ QString trial=chunk.isEmpty()?words[wi]:chunk+" "+words[wi];
                            if(runWidth(trial)<=segW || chunk.isEmpty()){ chunk=trial; ++wi; if(runWidth(chunk)>segW) break; }
                            else break; }
                        if(!chunk.isEmpty()) place(x, y+asc, chunk, segW);
                        if(wi>=words.size()) break;
                    }
                    if(wi==wiAtLineStart && wi<words.size()){ // no seg could take a word: force it (avoid infinite loop)
                        place(0, y+asc, words[wi], W); ++wi; }
                    y+=lineH; firstLineOfPara=false; (void)firstLineOfPara;
                } while(wi<words.size());
            }
            p=tp; p.setFillRule(fillRule); return p;
        }
        if (textFrame && !textShapePath.isEmpty()) {        // text INSIDE AN ARBITRARY SHAPE: scanline word-wrap that hugs the outline
            const QRectF bb=textShapePath.boundingRect();
            const double inset=lineH*0.18;                  // keep glyphs off the very edge
            // Widest continuous horizontal span of the shape across the line box [ytop, ytop+lineH].
            auto rowSpan=[&](double ytop, double& sx, double& sw)->bool{
                QPainterPath row; row.addRect(bb.left()-2, ytop, bb.width()+4, lineH);
                const QRectF r=textShapePath.intersected(row).boundingRect();
                if(r.width()<inset*2+2) return false; sx=r.left()+inset; sw=r.width()-2*inset; return true; };
            auto emitLine=[&](double sx,double sw,double baseY,const QString& s,bool justify){ place(sx,baseY,s,sw,justify); };
            double y=bb.top(); int guard=0, maxLines=int(bb.height()/qMax(1.0,lineH))+8;
            for(const QString& para: text.split('\n')){
                const QStringList words = para.isEmpty()? QStringList() : para.split(' ');
                int wi=0;
                do {
                    if(++guard>maxLines*4 || y+lineH>bb.bottom()+0.5){ wi=words.size(); break; }
                    double sx,sw; if(!rowSpan(y,sx,sw)){ y+=lineH; continue; }   // no room at this height — drop down
                    QString cur; int wStart=wi;
                    while(wi<words.size()){ QString trial=cur.isEmpty()?words[wi]:cur+" "+words[wi];
                        if(runWidth(trial)<=sw || cur.isEmpty()){ cur=trial; ++wi; } else break; }
                    const bool lastLine=(wi>=words.size());
                    emitLine(textAlign==1? sx+(sw-runWidth(cur))/2 : textAlign==2? sx+(sw-runWidth(cur)) : sx,
                             (textAlign==1||textAlign==2)?runWidth(cur):sw, y+asc, cur,
                             textAlign==3 && !lastLine && cur.contains(' '));
                    y+=lineH;
                    if(wi==wStart) ++wi;    // a single word wider than every row: force-advance to avoid a stall
                } while(wi<words.size());
                if(y+lineH>bb.bottom()+0.5) break;
            }
            p=tp; p.setFillRule(fillRule); return p;
        }
        if (textFrame && rect.width()>4) {                  // text AREA: word-wrap (+ columns)
            const int cols=qMax(1,textCols); const double colGap=textColGap;
            const double colW=(rect.width()-(cols-1)*colGap)/cols;
            const double colH=rect.height()>4?rect.height():1e9;
            // Drop cap: enlarge the first letter to span `dropCap` lines; flow text indents around it.
            QString flowText=text; int capLines=0; double capW=0;
            if(dropCap>1 && !text.isEmpty()){
                int fi=0; while(fi<flowText.size() && !flowText[fi].isLetterOrNumber()) fi++;
                if(fi<flowText.size()){ QChar capCh=flowText[fi]; QFont capFont=font;
                    if(font.pointSizeF()>0) capFont.setPointSizeF(font.pointSizeF()*dropCap*0.92); else capFont.setPixelSize(qMax(1,int((font.pixelSize()>0?font.pixelSize():12)*dropCap*0.92)));
                    QFontMetricsF cfm(capFont); capW=cfm.horizontalAdvance(capCh)+fm.horizontalAdvance(QChar(' '))*0.4;
                    tp.addText(0, asc+(dropCap-1)*lineH, capFont, QString(capCh));
                    flowText.remove(fi,1); capLines=dropCap; } }
            // Paragraph indents (all default 0 → every "-ri"/"+myIndent" below is a no-op, byte-identical).
            const double li=leftIndent, ri=rightIndent, fi=firstLineIndent;
            auto indentFor=[&](bool f){ return li + (f?fi:0); };
            QVector<QString> lines; QVector<bool> firstOf;   // firstOf[i]: is line i the first of its paragraph?
            QVector<int> lineStartChar;                      // lineStartChar[i]: char offset (into flowText) of line i's first char — used for linked-frame overflow
            int paraOff=0;
            for(const QString& para: flowText.split('\n')){
                if(para.isEmpty()){ lines.push_back(""); firstOf.push_back(true); lineStartChar.push_back(paraOff); paraOff+=1; continue; }
                QString cur; bool firstLine=true; int curStart=paraOff;
                auto pushLine=[&](const QString& s){ lines.push_back(s); firstOf.push_back(firstLine); lineStartChar.push_back(curStart); firstLine=false; };
                const QStringList words=para.split(' '); int wOff=paraOff;
                for(int wi=0; wi<words.size(); ++wi){ const QString& wd=words[wi]; QString trial=cur.isEmpty()?wd:cur+" "+wd;
                    double availW=((lines.size()<capLines)?(colW-capW):colW) - ri - indentFor(firstLine);
                    bool wasEmpty=cur.isEmpty();
                    if(runWidth(trial)<=availW || (cur.isEmpty() && !hyphenate)){ if(wasEmpty) curStart=wOff; cur=trial; }
                    else if(!hyphenate){ pushLine(cur); cur=wd; curStart=wOff; }                // (default path — byte-identical)
                    else {                                                                     // GATED: break the overflowing word at a syllable point
                        QString base=cur.isEmpty()?QString():cur+" ";
                        QVector<int> hp = wd.size()>4 ? Hyphenator::instance().points(wd) : QVector<int>();
                        int bestK=-1; for(int k: hp){ if(runWidth(base+wd.left(k+1)+"-")<=availW) bestK=k; else break; }
                        if(bestK>=0){ pushLine(base+wd.left(bestK+1)+"-"); cur=wd.mid(bestK+1); curStart=wOff+bestK+1; }
                        else if(!cur.isEmpty()){ pushLine(cur); cur=wd; curStart=wOff; }
                        else { if(wasEmpty) curStart=wOff; cur=trial; }                          // can't hyphenate a standalone long word: accept overflow
                    }
                    wOff+=wd.size()+1; }
                pushLine(cur); paraOff+=para.size()+1; }
            // A line is justified only if align==Justify, it has an inter-word gap, and it is NOT the last line
            // of its paragraph (the last line stays left-aligned, like every word processor).
            auto justifyLine=[&](int i){ return textAlign==3 && i<lines.size() && lines[i].contains(' ')
                && !(i==lines.size()-1 || (i+1<lines.size() && firstOf[i+1])); };
            const int linesPerCol=qMax(1,(int)(colH/lineH));
            // Bulleted / numbered lists (opt-in): each paragraph is an item — text hangs in from listInd and the
            // first line gets a marker in the gutter. listStyle==0 => listInd 0 + no marker => byte-identical.
            const bool listOn = listStyle>0;
            const double listInd = listOn ? (listIndent>0.0 ? listIndent : 18.0) : 0.0;
            int paraCount = 0;
            auto marker=[&](int n)->QString{ const int m=n+listStart-1;    // listStart=1 (default) → m=n → byte-identical
                if(listStyle==1) return QString(QChar(0x2022))+" ";
                if(listStyle==3) return QString(QChar('a'+(m-1)%26))+". "; return QString::number(m)+". "; };
            const bool paraSp=(spaceBefore>0.0 || spaceAfter>0.0) && cols==1;    // paragraph spacing: single-column cumulative-y layout
            if(paraSp){
                double y=0;
                for(int i=0;i<lines.size();++i){
                    if(firstOf[i] && i>0) y += spaceBefore + spaceAfter;         // gap between paragraphs (space-after + space-before)
                    if(y+lineH>colH && y>0){ if(overflowStart<0 && i<lineStartChar.size()) overflowStart=lineStartChar[i]; break; }
                    const double myIndent=li+listInd+(firstOf[i]?fi:0);
                    const double dcIndent=(i<capLines)?capW:0;
                    if(listOn && firstOf[i]){ ++paraCount; emitRun(li, y+asc, marker(paraCount)); }
                    place(dcIndent+myIndent, y+asc, lines[i], colW-dcIndent-myIndent-ri, justifyLine(i));
                    y += lineH; }
            } else {                                                             // fixed line grid (byte-identical when no paragraph spacing)
                const int cap=linesPerCol*cols;              // last visible line index+1; anything beyond overflows the frame
                if(lines.size()>cap && cap>=0 && cap<lineStartChar.size()) overflowStart=lineStartChar[cap];
                for(int i=0;i<lines.size();++i){ int col=i/linesPerCol; if(col>=cols) break; int rown=i%linesPerCol;
                    double dcIndent=(col==0 && i<capLines)?capW:0;
                    double myIndent=li+listInd+(firstOf[i]?fi:0);
                    const double colBase=col*(colW+colGap);
                    if(listOn && firstOf[i]){ ++paraCount; emitRun(colBase+li, rown*lineH+asc, marker(paraCount)); }
                    place(colBase+dcIndent+myIndent, rown*lineH+asc, lines[i], colW-dcIndent-myIndent-ri, justifyLine(i)); }
            }
        } else {                                            // POINT text: each \n is a new line (single line == before)
            double y=asc; bool firstLine=true;              // indents default 0 → place(0,…) exactly as before
            for(const QString& l: text.split('\n')){ place(leftIndent+(firstLine?firstLineIndent:0),y,l,0); y+=lineH; firstLine=false; }
        }
        p = tp;
        break;
    }
    case ShapeType::Smart:
        // Parametric SmartShape: regenerate the 0..100 box path from kind+param, then
        // map it into the shape's rect so resizing the bbox scales the generated geometry.
        p = smartPath(smartKind, smartParam, smartParam2);
        if (!rect.isNull() && rect.width()>0 && rect.height()>0) {
            QTransform fit = QTransform::fromTranslate(rect.x(), rect.y()) ;
            fit = QTransform::fromScale(rect.width()/100.0, rect.height()/100.0) * fit;
            p = fit.map(p);
        }
        break;
    case ShapeType::Group:
        for (const Shape& c : children) p.addPath(c.transform.map(c.localPath()));
        break;
    }
    p.setFillRule(fillRule);   // non-zero vs even-odd governs self-intersecting / holed fills
    return p;
}

void Shape::resolveChars(QVector<QFont>& fonts, QVector<QColor>& colors, QVector<QString>& urls) const {
    const int n = text.size();
    const QColor baseCol = (fill.type == FillType::Solid) ? fill.color : QColor(Qt::black);
    fonts  = QVector<QFont>(n, font);
    colors = QVector<QColor>(n, baseCol);
    urls   = QVector<QString>(n, QString());
    for (const TextRun& r : textRuns) {                     // later runs win; ranges clamped to text length
        const int a = qMax(0, r.start), b = qMin(n, r.start + r.len);
        for (int i = a; i < b; ++i) { fonts[i] = r.font; colors[i] = r.color; urls[i] = r.url;
            if (r.tracking != 0.0) fonts[i].setLetterSpacing(QFont::AbsoluteSpacing, r.tracking); }  // per-run tracking override
    }
}

QVector<double> Shape::resolveBaselines() const {
    QVector<double> bs(text.size(), 0.0);
    for (const TextRun& r : textRuns) {                     // per-run baseline shift (super/subscript)
        const int a = qMax(0, r.start), b = qMin(text.size(), r.start + r.len);
        for (int i = a; i < b; ++i) bs[i] = r.baseline;
    }
    return bs;
}

// One glyph placement (character index + local-space pen origin/baseline). Shared by
// layoutRuns (group by colour) and runLinkRects (group by URL) so both agree exactly.
namespace { struct GlyphPlace { int idx; double x, y; }; }
static QVector<GlyphPlace> layoutTextGlyphs(const Shape& s, const QVector<QFont>& fontsIn,
                                            const QVector<double>* bshift = nullptr) {
    const QString& text = s.text; const int n = text.size();
    QVector<QFont> fonts = fontsIn;   // apply the shape's letter-spacing to any char without a per-run override
    if (s.tracking!=0.0) for (QFont& f : fonts) if (f.letterSpacingType()!=QFont::AbsoluteSpacing) f.setLetterSpacing(QFont::AbsoluteSpacing, s.tracking);
    auto shiftOf = [&](int i){ return (bshift && i < bshift->size()) ? (*bshift)[i] : 0.0; };  // +up = smaller y
    // Single-char horizontalAdvance ignores letter-spacing (Qt applies it at shaping time), so add it
    // back per glyph — otherwise per-run/shape tracking wouldn't actually space out the glyphs.
    auto adv = [&](int i){ double a = QFontMetricsF(fonts[i]).horizontalAdvance(text[i]);
        if (fonts[i].letterSpacingType()==QFont::AbsoluteSpacing) a += fonts[i].letterSpacing(); return a; };
    auto asc = [&](int i){ return QFontMetricsF(fonts[i]).ascent(); };
    auto hgt = [&](int i){ return QFontMetricsF(fonts[i]).height()*s.lineSpacing; };
    QFont bf = s.font; if (s.tracking!=0.0) bf.setLetterSpacing(QFont::AbsoluteSpacing, s.tracking);
    if(s.textAspect!=1.0) bf.setStretch(qBound(1,int(s.textAspect*100.0+0.5),400));
    const QFontMetricsF bfm(bf);
    const double baseH = bfm.height()*s.lineSpacing, baseAsc = bfm.ascent();
    QVector<GlyphPlace> pls;
    const bool frame = s.textFrame && s.rect.width() > 4;
    if (!frame) {                                           // POINT text: split on '\n', left-aligned
        double cy = 0; int a = 0;
        for (int k = 0; k <= n; ++k) {
            if (k == n || text[k] == '\n') {
                double la = 0, lh = 0;
                for (int j = a; j < k; ++j) { la = qMax(la, asc(j)); lh = qMax(lh, hgt(j)); }
                if (lh <= 0) { lh = baseH; la = baseAsc; }
                double x = 0, base = cy + la;
                for (int j = a; j < k; ++j) { pls.push_back({j, x, base - shiftOf(j)}); x += adv(j); }
                cy += lh; a = k + 1;
            }
        }
    } else {                                                // text AREA: greedy word-wrap into columns
        const int cols = qMax(1, s.textCols); const double colGap = s.textColGap;
        const double colW = (s.rect.width()-(cols-1)*colGap)/cols;
        const double colH = s.rect.height() > 4 ? s.rect.height() : 1e9;
        QVector<QPair<int,int>> lines;                      // [start,end) char ranges
        int i = 0;
        while (i < n) {
            int lineStart = i; double x = 0; int lastSpace = -1; bool wrapped = false;
            while (i < n && text[i] != '\n') {
                if (text[i] == ' ') lastSpace = i;
                double a2 = adv(i);
                if (x + a2 > colW && i > lineStart) {
                    if (lastSpace > lineStart) { lines.push_back({lineStart, lastSpace}); i = lastSpace + 1; }
                    else lines.push_back({lineStart, i});
                    wrapped = true; break;
                }
                x += a2; ++i;
            }
            if (!wrapped) { lines.push_back({lineStart, i}); if (i < n && text[i] == '\n') ++i; }
        }
        int col = 0; double cy = 0;
        for (const auto& ln : lines) {
            double lh = baseH, la = baseAsc, lw = 0;
            if (ln.second > ln.first) { lh = 0; la = 0; }
            for (int j = ln.first; j < ln.second; ++j) { lh = qMax(lh, hgt(j)); la = qMax(la, asc(j)); lw += adv(j); }
            if (lh <= 0) lh = baseH;
            if (cy + lh > colH && cy > 0) { ++col; cy = 0; if (col >= cols) break; }
            double off = 0; if (s.textAlign == 1) off = (colW-lw)/2; else if (s.textAlign == 2) off = colW-lw;
            double x = col*(colW+colGap) + off, base = cy + la;
            for (int j = ln.first; j < ln.second; ++j) { pls.push_back({j, x, base - shiftOf(j)}); x += adv(j); }
            cy += lh;
        }
    }
    return pls;
}

QVector<QPair<QPainterPath,QColor>> Shape::layoutRuns() const {
    QVector<QPair<QPainterPath,QColor>> out;
    if (text.isEmpty() || textRuns.isEmpty()) return out;
    QVector<QFont> fonts; QVector<QColor> colors; QVector<QString> urls;
    resolveChars(fonts, colors, urls);
    const QVector<double> bs = resolveBaselines();
    const QVector<GlyphPlace> pls = layoutTextGlyphs(*this, fonts, &bs);
    // Coalesce consecutive same-colour glyphs into one path (each glyph keeps its own font).
    int i = 0;
    while (i < pls.size()) {
        const QColor c = colors[pls[i].idx];
        QPainterPath path;
        while (i < pls.size() && colors[pls[i].idx] == c) {
            const GlyphPlace& p = pls[i];
            if (!text[p.idx].isSpace()) path.addText(p.x, p.y, fonts[p.idx], QString(text[p.idx]));
            ++i;
        }
        if (!path.isEmpty()) out.push_back({path, c});
    }
    return out;
}

QVector<QPair<QRectF,QString>> Shape::runLinkRects() const {
    QVector<QPair<QRectF,QString>> out;
    if (text.isEmpty() || textRuns.isEmpty()) return out;
    QVector<QFont> fonts; QVector<QColor> colors; QVector<QString> urls;
    resolveChars(fonts, colors, urls);
    const QVector<double> bs = resolveBaselines();
    const QVector<GlyphPlace> pls = layoutTextGlyphs(*this, fonts, &bs);
    // Coalesce consecutive same-URL glyphs; the span's rect = union of its glyph cells
    // (x..x+advance across the font's ascent+descent band), which covers spaces too.
    int i = 0;
    while (i < pls.size()) {
        const QString u = urls[pls[i].idx];
        QRectF box;
        while (i < pls.size() && urls[pls[i].idx] == u) {
            const GlyphPlace& p = pls[i]; const QFontMetricsF fm(fonts[p.idx]);
            QRectF cell(p.x, p.y - fm.ascent(), fm.horizontalAdvance(text[p.idx]), fm.ascent() + fm.descent());
            box = box.isNull() ? cell : box.united(cell);
            ++i;
        }
        if (!u.isEmpty() && !box.isNull()) out.push_back({box, u});
    }
    return out;
}

QPainterPath Shape::layoutFrameInline(QVector<QRectF>* outBoxes) const {
    QPainterPath tp;
    QFont font = this->font; if (tracking!=0.0) font.setLetterSpacing(QFont::AbsoluteSpacing, tracking);
    if(textAspect!=1.0) font.setStretch(qBound(1,int(textAspect*100.0+0.5),400));
    const QFontMetricsF fm(font);
    const double lineH0 = fm.height()*lineSpacing, asc = fm.ascent(), spaceW = fm.horizontalAdvance(QChar(' '));
    const double W = rect.width() > 4 ? rect.width() : 1e9;
    QVector<QPair<int,QSizeF>> boxes = textInlineBoxes;
    std::sort(boxes.begin(), boxes.end(), [](const QPair<int,QSizeF>& a, const QPair<int,QSizeF>& b){ return a.first < b.first; });
    if (outBoxes) { outBoxes->clear(); outBoxes->resize(boxes.size()); }
    // Tokenise: word tokens (with char position) interleaved with box tokens at their positions.
    struct Tok { bool box; int boxIdx; QString s; double w, h; };
    QVector<Tok> toks; int bi = 0; const int n = text.size();
    auto flushBoxesUpTo = [&](int upto){ while (bi < boxes.size() && boxes[bi].first <= upto) {
        toks.push_back({true, bi, QString(), boxes[bi].second.width(), boxes[bi].second.height()}); ++bi; } };
    int i = 0;
    while (i < n) {
        if (text[i] == '\n') { flushBoxesUpTo(i); toks.push_back({false, -1, QStringLiteral("\n"), 0, 0}); ++i; continue; }
        if (text[i] == ' ')  { flushBoxesUpTo(i); ++i; continue; }
        int st = i; while (i < n && text[i] != ' ' && text[i] != '\n') ++i;
        flushBoxesUpTo(st);
        toks.push_back({false, -1, text.mid(st, i-st), fm.horizontalAdvance(text.mid(st, i-st)), 0});
    }
    flushBoxesUpTo(n);
    // Greedy line layout; a line's height/baseline grows to fit any inline box on it.
    struct Placed { Tok t; double x; };
    QVector<Placed> line; double x = 0, y = 0;
    auto flushLine = [&](){
        double maxBoxH = 0; for (const Placed& pl : line) if (pl.t.box) maxBoxH = qMax(maxBoxH, pl.t.h);
        const double baseline = y + qMax(asc, maxBoxH);
        for (const Placed& pl : line) {
            if (pl.t.box) { if (outBoxes && pl.t.boxIdx < outBoxes->size()) (*outBoxes)[pl.t.boxIdx] = QRectF(pl.x, baseline - pl.t.h, pl.t.w, pl.t.h); }
            else if (pl.t.s != QStringLiteral("\n")) tp.addText(pl.x, baseline, font, pl.t.s);
        }
        y += qMax(lineH0, maxBoxH + 6.0); line.clear(); x = 0;
    };
    for (const Tok& t : toks) {
        if (t.s == QStringLiteral("\n")) { flushLine(); continue; }
        const double adv = t.w;
        if (x > 0 && x + adv > W) flushLine();
        line.push_back({t, x});
        x += adv + (t.box ? 0.0 : spaceW);
    }
    flushLine();
    tp.setFillRule(fillRule);
    return tp;
}

// ---- In-canvas text-editing geometry (point text) ----
// The caret/hit maths mirror the point-text branch of localPath(): each '\n'-delimited line is placed
// left-aligned at x=leftIndent (+firstLineIndent on line 0), baseline y=ascent+line*lineH; run widths
// honour tracking (via the font's letter-spacing) and small-caps.
namespace {
    QFont s_textFont(const Shape& s){ QFont f=s.font; if(s.tracking!=0.0) f.setLetterSpacing(QFont::AbsoluteSpacing, s.tracking);
        if(s.textAspect!=1.0) f.setStretch(qBound(1,int(s.textAspect*100.0+0.5),400)); return f; }   // horizontal glyph scale
    QFont s_scFont(const QFont& f){ QFont sc=f; if(f.pointSizeF()>0) sc.setPointSizeF(f.pointSizeF()*0.78); else sc.setPixelSize(qMax(1,int((f.pixelSize()>0?f.pixelSize():12)*0.78))); return sc; }
    double s_runW(const QString& str, const QFontMetricsF& fm, const QFontMetricsF& scFm, bool smallCaps){
        if(!smallCaps) return fm.horizontalAdvance(str);
        double wd=0; for(const QChar c: str){ if(c.isLetter()&&c.isLower()) wd+=scFm.horizontalAdvance(c.toUpper()); else wd+=fm.horizontalAdvance(c); } return wd; }
    // Caret x of column `col` within a line, honouring a regular tab-stop interval (tabWidth). Mirrors
    // place()'s tabWidth branch (localPath 185-189): each '\t' jumps to the next multiple of tabWidth from
    // the line origin x0. tabWidth<=0 or no '\t' → plain advance sum (byte-identical to the old caret math).
    double s_tabX(double x0, const QString& s, int col, double tabWidth,
                  const QFontMetricsF& fm, const QFontMetricsF& scFm, bool smallCaps){
        if(tabWidth<=0.0 || !s.contains('\t')) return x0 + s_runW(s.left(col),fm,scFm,smallCaps);
        double cx=x0; int pos=0; const QStringList segs=s.split('\t');
        for(int i=0;i<segs.size();++i){ const QString& seg=segs[i];
            if(col<=pos+seg.size()) return cx + s_runW(seg.left(col-pos),fm,scFm,smallCaps);
            cx += s_runW(seg,fm,scFm,smallCaps); pos += seg.size();
            if(i<segs.size()-1){ const double rel=cx-x0; cx = x0 + (std::floor(rel/tabWidth)+1.0)*tabWidth; pos+=1; } }
        return cx; }
    // Caret x of column `col` within a line that uses explicit tab stops (align/leaders/decimal). Mirrors
    // place()'s tabStops branch (localPath 159-181). Leader glyphs don't consume character positions, so
    // they're ignored here — only the segment start x matters for the caret.
    double s_tabStopsX(double x0, const QString& s, int col, const QVector<TabStop>& tabStops,
                       const QFontMetricsF& fm, const QFontMetricsF& scFm, bool smallCaps){
        const QStringList segs=s.split('\t'); double cx=x0; int pos=0;
        for(int i=0;i<segs.size();++i){ const QString& seg=segs[i]; double segStart=cx;
            if(i>0){ const double rel=cx-x0; const TabStop* stop=nullptr;
                for(const TabStop& ts: tabStops){ if(ts.pos>rel+0.01){ stop=&ts; break; } }
                const double segW=s_runW(seg,fm,scFm,smallCaps);
                if(stop){ const double target=x0+stop->pos;
                    if(stop->align==1) segStart=target-segW/2;                                   // centre
                    else if(stop->align==2) segStart=target-segW;                                // right
                    else if(stop->align==3){ int dot=seg.indexOf('.'); if(dot<0) dot=seg.indexOf(','); if(dot<0) dot=int(seg.size());
                        segStart=target-s_runW(seg.left(dot),fm,scFm,smallCaps); }               // decimal
                    else segStart=target; }                                                      // left
                if(segStart<cx) segStart=cx; }
            if(col<=pos+seg.size()) return segStart + s_runW(seg.left(col-pos),fm,scFm,smallCaps);
            cx = segStart + s_runW(seg,fm,scFm,smallCaps); pos += seg.size();
            if(i<segs.size()-1) pos+=1; }
        return cx; }
    // Dispatch: explicit tab stops take precedence (as in place()); otherwise a regular tabWidth grid.
    double s_lineX(double x0, const QString& s, int col, double tabWidth, const QVector<TabStop>& tabStops,
                   const QFontMetricsF& fm, const QFontMetricsF& scFm, bool smallCaps){
        if(!tabStops.isEmpty() && s.contains('\t')) return s_tabStopsX(x0,s,col,tabStops,fm,scFm,smallCaps);
        return s_tabX(x0,s,col,tabWidth,fm,scFm,smallCaps); }
    // Drop-cap geometry: capIdx = index of the enlarged character (the first letter/number — any leading
    // punctuation is KEPT in the flow, per localPath 240), capW/capLines the reserved gutter. Returns false
    // (capIdx=-1) when no drop cap applies: dropCap<=1, non-frame, empty text, or all-punctuation text (no
    // letter/number to enlarge) — exactly localPath's `if(fi<flowText.size())` guard.
    bool s_dropCapGeom(const Shape& s, int& capIdx, double& capW, int& capLines){
        capIdx=-1; capW=0; capLines=0;
        if(!(s.dropCap>1) || !s.textFrame || s.text.isEmpty()) return false;
        int k=0; while(k<s.text.size() && !s.text[k].isLetterOrNumber()) ++k;
        if(k>=s.text.size()) return false;
        capIdx=k;
        const QFont font=s_textFont(s); const QFontMetricsF fm(font);
        QFont capFont=font;
        if(font.pointSizeF()>0) capFont.setPointSizeF(font.pointSizeF()*s.dropCap*0.92);
        else capFont.setPixelSize(qMax(1,int((font.pixelSize()>0?font.pixelSize():12)*s.dropCap*0.92)));
        const QFontMetricsF cfm(capFont);
        capW=cfm.horizontalAdvance(s.text[capIdx])+fm.horizontalAdvance(QChar(' '))*0.4; capLines=s.dropCap;
        return true; }
}
QRectF Shape::dropCapRectLocal() const {
    int capIdx=-1, capLines=0; double capW=0;
    if(!s_dropCapGeom(*this, capIdx, capW, capLines)) return QRectF();
    return QRectF(0, 0, capW, capLines * textLineH());   // cap spans capLines tall from the frame top
}
double Shape::textLineH() const { return QFontMetricsF(s_textFont(*this)).height()*lineSpacing; }
double Shape::textAscent() const { return QFontMetricsF(s_textFont(*this)).ascent(); }

// Word-wrap + place the visual lines of a simple text frame — a faithful PARALLEL of the frame branch
// of localPath() (non-hyphenate, no drop cap so flowText==text; the editor only enables inline editing
// for frames where that holds — see Canvas::canInlineEdit). Kept in sync with the renderer by tests.
QVector<Shape::TextVisualLine> Shape::frameVisualLines() const {
    QVector<TextVisualLine> out;
    if(type!=ShapeType::Text || !textFrame || rect.width()<=4) return out;
    const QFont font=s_textFont(*this); const QFontMetricsF fm(font), scFm(s_scFont(font));
    const double lineH=fm.height()*lineSpacing, asc=fm.ascent();
    const int cols=qMax(1,textCols); const double colGap=textColGap;
    const double colW=(rect.width()-(cols-1)*colGap)/cols;
    const double colH=rect.height()>4?rect.height():1e9;
    const double li=leftIndent, ri=rightIndent, fi=firstLineIndent;
    auto rw=[&](const QString& s){ return s_runW(s,fm,scFm,smallCaps); };
    // INLINE ANCHORED GRAPHICS: words flow with a reserved gap for each inline box. A faithful parallel of
    // layoutFrameInline() — words are placed as whole units, boxes reserve space but are NOT text, so the
    // caret map emits one visual line per WORD (mapping 1:1 to the source text) and lets the boxes only
    // shift the x-flow. Taken before repel/drop-cap to match localPath's branch order (inline at 194).
    if(!textInlineBoxes.isEmpty()){
        const double spaceW=fm.horizontalAdvance(QChar(' '));
        const double W=rect.width()>4?rect.width():1e9;
        QVector<QPair<int,QSizeF>> boxes=textInlineBoxes;
        std::sort(boxes.begin(),boxes.end(),[](const QPair<int,QSizeF>& a,const QPair<int,QSizeF>& b){ return a.first<b.first; });
        struct Tok{ bool box; int startChar; QString s; double w,h; };
        QVector<Tok> toks; int bi=0; const int n=int(text.size());
        auto flushBoxesUpTo=[&](int upto){ while(bi<boxes.size() && boxes[bi].first<=upto){ toks.push_back({true,boxes[bi].first,QString(),boxes[bi].second.width(),boxes[bi].second.height()}); ++bi; } };
        int i=0;
        while(i<n){
            if(text[i]=='\n'){ flushBoxesUpTo(i); toks.push_back({false,i,QStringLiteral("\n"),0,0}); ++i; continue; }
            if(text[i]==' '){ flushBoxesUpTo(i); ++i; continue; }
            int st=i; while(i<n && text[i]!=' ' && text[i]!='\n') ++i;
            flushBoxesUpTo(st);
            toks.push_back({false,st,text.mid(st,i-st),fm.horizontalAdvance(text.mid(st,i-st)),0});
        }
        flushBoxesUpTo(n);
        struct Placed{ Tok t; double x; };
        QVector<Placed> lineToks; double x=0,y=0;
        auto flushLine=[&](){
            double maxBoxH=0; for(const Placed& pl: lineToks) if(pl.t.box) maxBoxH=qMax(maxBoxH,pl.t.h);
            const double baseline=y+qMax(asc,maxBoxH);
            for(const Placed& pl: lineToks) if(!pl.t.box && pl.t.s!=QStringLiteral("\n")) out.push_back({pl.t.startChar, pl.t.s, pl.x, baseline, pl.t.w, pl.x});
            y+=qMax(lineH,maxBoxH+6.0); lineToks.clear(); x=0;
        };
        for(const Tok& t: toks){
            if(t.s==QStringLiteral("\n")){ bool hasWord=false; for(const Placed& pl: lineToks) if(!pl.t.box) hasWord=true;
                if(!hasWord) out.push_back({t.startChar, QString(), 0.0, y+asc, W, 0.0});   // blank line keeps a caret slot
                flushLine(); continue; }
            const double adv=t.w;
            if(x>0 && x+adv>W) flushLine();
            lineToks.push_back({t,x});
            x += adv + (t.box?0.0:spaceW);
        }
        flushLine();
        return out;
    }
    // REPEL: text flows around obstacle rectangles. A faithful parallel of localPath 197-231 — each
    // horizontal band is split into free x-segments (frame width minus the obstacles overlapping the band)
    // and wrapped chunks are placed left-aligned in each. Every placed chunk becomes one visual line and
    // maps 1:1 to the source text (single spaces), so per-chunk startChar tracking is exact. (Repel is a
    // separate render branch that ignores drop cap / lists, so we take it before that machinery below.)
    if(!textWrapObstacles.isEmpty()){
        const double W=rect.width();
        auto freeSegs=[&](double y0,double y1)->QVector<QPair<double,double>>{
            QVector<QPair<double,double>> occ;
            for(const QRectF& o: textWrapObstacles){ if(o.bottom()<=y0 || o.top()>=y1) continue;
                double a=qMax(0.0,o.left()), b=qMin(W,o.right()); if(b>a) occ.push_back({a,b}); }
            std::sort(occ.begin(),occ.end());
            QVector<QPair<double,double>> segs; double cur=0;
            for(const auto& iv: occ){ if(iv.first>cur+0.5) segs.push_back({cur,iv.first}); cur=qMax(cur,iv.second); }
            if(cur<W-0.5) segs.push_back({cur,W});
            return segs; };
        double y=0; int paraOff=0;
        for(const QString& para: text.split('\n')){
            if(para.isEmpty()){ out.push_back({paraOff, QString(), 0.0, y+asc, W, 0.0}); y+=lineH; paraOff+=1; continue; }
            const QStringList words = para.split(' ');
            QVector<int> wordOff; { int o=paraOff; for(const QString& wd: words){ wordOff.push_back(o); o+=wd.size()+1; } }
            int wi=0;
            do {
                const QVector<QPair<double,double>> segs=freeSegs(y, y+lineH);
                const int wiAtLineStart=wi;
                for(const auto& seg: segs){
                    const double x=seg.first, segW=seg.second-seg.first; QString chunk; const int chunkStart=(wi<words.size())?wordOff[wi]:paraOff;
                    while(wi<words.size()){ const QString trial=chunk.isEmpty()?words[wi]:chunk+" "+words[wi];
                        if(rw(trial)<=segW || chunk.isEmpty()){ chunk=trial; ++wi; if(rw(chunk)>segW) break; }
                        else break; }
                    if(!chunk.isEmpty()) out.push_back({chunkStart, chunk, x, y+asc, segW, x});
                    if(wi>=words.size()) break;
                }
                if(wi==wiAtLineStart && wi<words.size()){ out.push_back({wordOff[wi], words[wi], 0.0, y+asc, W, 0.0}); ++wi; }
                y+=lineH;
            } while(wi<words.size());
            paraOff+=para.size()+1;
        }
        return out;
    }
    // Drop cap (opt-in): the first LETTER/NUMBER is enlarged into the gutter, spanning `dropCap` lines; any
    // leading punctuation stays in the flow (localPath 240 removes flowText[capIdx], not [0]). The flow text
    // is that char removed, wrapped with the first capLines lines indented by capW. lineStartChar records
    // FLOW offsets; textCaretLocal/textIndexAtLocal convert text indices to/from flow offsets around the
    // removed cap. capLines/capW are 0 when off → every expression below is byte-identical to the plain
    // layout, and the flow offsets equal text indices (capIdx<0).
    int capIdx=-1, capLines=0; double capW=0;
    const bool dcOn = s_dropCapGeom(*this, capIdx, capW, capLines);
    const QString src = dcOn ? (text.left(capIdx)+text.mid(capIdx+1)) : text;
    auto indentFor=[&](bool f){ return li + (f?fi:0); };
    // --- wrap (mirrors localPath 249-271, INCLUDING the hyphenate branch) ---
    // lineReal[i] < 0 means the whole line maps 1:1 to text; a hyphenated line ends in a synthetic '-'
    // and stores its real-character count (str.size()-1) so the caret never steps onto the fake hyphen.
    QVector<QString> lines; QVector<bool> firstOf; QVector<int> lineStartChar, lineReal; int paraOff=0;
    for(const QString& para: src.split('\n')){
        if(para.isEmpty()){ lines.push_back(QString()); firstOf.push_back(true); lineStartChar.push_back(paraOff); lineReal.push_back(-1); paraOff+=1; continue; }
        QString cur; bool firstLine=true; int curStart=paraOff;
        auto pushLine=[&](const QString& s, int rl=-1){ lines.push_back(s); firstOf.push_back(firstLine); lineStartChar.push_back(curStart); lineReal.push_back(rl); firstLine=false; };
        const QStringList words=para.split(' '); int wOff=paraOff;
        for(int wi=0; wi<words.size(); ++wi){ const QString& wd=words[wi]; const QString trial=cur.isEmpty()?wd:cur+" "+wd;
            const double availW=((lines.size()<capLines)?(colW-capW):colW) - ri - indentFor(firstLine); const bool wasEmpty=cur.isEmpty();
            if(rw(trial)<=availW || (cur.isEmpty() && !hyphenate)){ if(wasEmpty) curStart=wOff; cur=trial; }
            else if(!hyphenate){ pushLine(cur); cur=wd; curStart=wOff; }
            else {                                                                     // break the overflowing word at a syllable point
                const QString base=cur.isEmpty()?QString():cur+" ";
                const QVector<int> hp = wd.size()>4 ? Hyphenator::instance().points(wd) : QVector<int>();
                int bestK=-1; for(int k: hp){ if(rw(base+wd.left(k+1)+"-")<=availW) bestK=k; else break; }
                if(bestK>=0){ const QString ln=base+wd.left(bestK+1)+"-"; pushLine(ln, int(ln.size())-1); cur=wd.mid(bestK+1); curStart=wOff+bestK+1; }
                else if(!cur.isEmpty()){ pushLine(cur); cur=wd; curStart=wOff; }
                else { if(wasEmpty) curStart=wOff; cur=trial; } }
            wOff+=wd.size()+1; }
        pushLine(cur); paraOff+=para.size()+1; }
    // --- place (mirrors localPath 272-300 for the VISIBLE lines; but unlike the renderer we do NOT drop
    // overflow lines — the caret must track every character. Single-column overflow continues straight
    // down; multi-column overflow spills into phantom columns to the right (both are below/right of the
    // clipped render, i.e. exactly where the un-rendered text conceptually sits). ---
    const int linesPerCol=qMax(1,(int)(colH/lineH));
    const bool paraSp=(spaceBefore>0.0||spaceAfter>0.0)&&cols==1;
    // Lists: the marker sits in the gutter at li and the text hangs at li+listIndent (matches localPath's
    // myIndent). The wrap above intentionally did NOT include listInd (nor does the renderer's wrap).
    const double listInd = listStyle>0 ? (listIndent>0.0 ? listIndent : 18.0) : 0.0;
    if(paraSp){ double y=0;
        for(int i=0;i<lines.size();++i){ if(firstOf[i]&&i>0) y+=spaceBefore+spaceAfter;
            const double myIndent=li+listInd+(firstOf[i]?fi:0);
            const double dcIndent=(i<capLines)?capW:0;
            out.push_back({lineStartChar[i], lines[i], dcIndent+myIndent, y+asc, colW-dcIndent-myIndent-ri, 0.0, lineReal[i]}); y+=lineH; }
    } else {
        for(int i=0;i<lines.size();++i){
            int col, rown; if(cols==1){ col=0; rown=i; } else { col=i/linesPerCol; rown=i%linesPerCol; }
            const double myIndent=li+listInd+(firstOf[i]?fi:0); const double colBase=col*(colW+colGap);
            const double dcIndent=(col==0 && i<capLines)?capW:0;
            out.push_back({lineStartChar[i], lines[i], colBase+dcIndent+myIndent, rown*lineH+asc, colW-dcIndent-myIndent-ri, colBase, lineReal[i]}); } }
    return out;
}

QPointF Shape::textCaretLocal(int idx) const {
    const QFont f=s_textFont(*this); const QFontMetricsF fm(f), scFm(s_scFont(f));
    const double lineH=fm.height()*lineSpacing, asc=fm.ascent();
    idx=qBound(0,idx,int(text.size()));
    if(!textPath.isEmpty()){                                        // TEXT ON A CURVE — mirror localPath 122-134
        const double len=textPath.length();
        double x=textPathOffset; for(int i=0;i<idx && i<text.size();++i){ if(text[i]=='\n') continue; x+=fm.horizontalAdvance(text[i]); }
        if(len<1e-6) return QPointF(0,asc);
        return textPath.pointAtPercent(textPath.percentAtLength(qBound(0.0,x,len)));   // caret sits ON the baseline path
    }
    if(textFrame && rect.width()>4 && textPath.isEmpty()){          // wrapped frame
        const QVector<TextVisualLine> vls=frameVisualLines();
        if(vls.isEmpty()) return QPointF(leftIndent, asc);
        int capIdx=-1, capLines=0; double capW=0; s_dropCapGeom(*this, capIdx, capW, capLines);
        if(capIdx>=0 && idx==capIdx) return QPointF(0, vls[0].baseY);   // caret at the drop cap sits at the gutter's left edge
        const int fidx = (capIdx>=0 && idx>capIdx) ? idx-1 : idx;       // text index → flow offset (the enlarged cap is out of the flow)
        int k=0; for(int j=0;j<vls.size();++j) if(vls[j].startChar<=fidx) k=j;
        const TextVisualLine& L=vls[k]; const int col=qBound(0, fidx-L.startChar, L.realCount());   // clamp off any synthetic hyphen
        const double sw=s_runW(L.str,fm,scFm,smallCaps);
        const bool tabbed = (tabWidth>0.0||!tabStops.isEmpty()) && L.str.contains('\t');   // place() ignores align on tabbed lines
        const double off=tabbed?0.0:(textAlign==1)?(L.width-sw)/2.0:(textAlign==2)?(L.width-sw):0.0;
        return QPointF(s_lineX(L.x0+off, L.str, col, tabWidth, tabStops, fm,scFm,smallCaps), L.baseY);
    }
    int line=0, lineStart=0;                                        // point text: split on '\n'
    for(int i=0;i<idx && i<text.size();++i) if(text[i]=='\n'){ ++line; lineStart=i+1; }
    int lineEnd=lineStart; while(lineEnd<text.size() && text[lineEnd]!='\n') ++lineEnd;
    const QString lineStr=text.mid(lineStart, lineEnd-lineStart);
    const double x0=leftIndent + (line==0? firstLineIndent : 0.0);
    return QPointF(s_lineX(x0, lineStr, idx-lineStart, tabWidth, tabStops, fm,scFm,smallCaps), asc + line*lineH);
}
int Shape::textIndexAtLocal(const QPointF& pt) const {
    const QFont f=s_textFont(*this); const QFontMetricsF fm(f), scFm(s_scFont(f));
    const double lineH=fm.height()*lineSpacing, asc=fm.ascent();
    if(!textPath.isEmpty()){                                        // TEXT ON A CURVE — nearest char boundary on the path
        const double len=textPath.length();
        if(len<1e-6) return 0;
        double best=1e18; int bestIdx=0; double x=textPathOffset;
        for(int i=0;i<=text.size();++i){
            const QPointF p=textPath.pointAtPercent(textPath.percentAtLength(qBound(0.0,x,len)));
            const double d=QLineF(p,pt).length(); if(d<best){ best=d; bestIdx=i; }
            if(i<text.size() && text[i]!='\n') x+=fm.horizontalAdvance(text[i]);
        }
        return bestIdx;
    }
    if(textFrame && rect.width()>4 && textPath.isEmpty()){          // wrapped frame
        const QVector<TextVisualLine> vls=frameVisualLines();
        if(vls.isEmpty()) return 0;
        int capIdx=-1, capLines=0; double capW=0; s_dropCapGeom(*this, capIdx, capW, capLines);
        if(capIdx>=0 && pt.x()>=0 && pt.x()<capW && pt.y()>=0 && pt.y()<capLines*lineH) return capIdx;   // click on the enlarged cap in the gutter
        const int cols=qMax(1,textCols); const double colW=(rect.width()-(cols-1)*textColGap)/cols;
        // For repel/inline there are several word-chunks per band, each at its own x; use each line's own
        // width as its hit extent so a click lands on the chunk under it. Otherwise the full column width.
        const bool repel = !textWrapObstacles.isEmpty() || !textInlineBoxes.isEmpty();
        int best=0; double bestScore=1e18;
        for(int k=0;k<vls.size();++k){ const TextVisualLine& L=vls[k];
            const double top=L.baseY-asc, bot=top+lineH;
            const double dy = pt.y()<top? top-pt.y() : pt.y()>bot? pt.y()-bot : 0.0;
            const double hw = repel ? L.width : colW;
            const double dx = pt.x()<L.colBase? L.colBase-pt.x() : pt.x()>L.colBase+hw? pt.x()-(L.colBase+hw) : 0.0;
            const double score=dx*1000.0+dy; if(score<bestScore){ bestScore=score; best=k; } }   // column/chunk first, then row
        const TextVisualLine& L=vls[best];
        const double sw=s_runW(L.str,fm,scFm,smallCaps);
        const bool tabbed = (tabWidth>0.0||!tabStops.isEmpty()) && L.str.contains('\t');
        const double x0=L.x0 + (tabbed?0.0:(textAlign==1)?(L.width-sw)/2.0:(textAlign==2)?(L.width-sw):0.0);
        double bestD=1e18; int bestCol=0;
        for(int c=0;c<=L.realCount();++c){ double xx=s_lineX(x0,L.str,c,tabWidth,tabStops,fm,scFm,smallCaps); double dd=qAbs(xx-pt.x()); if(dd<bestD){ bestD=dd; bestCol=c; } }
        const int flowOff = L.startChar+bestCol;
        return (capIdx>=0 && flowOff>=capIdx) ? flowOff+1 : flowOff;   // flow offset → text index (re-insert the removed cap)
    }
    const QStringList lines=text.split('\n');                       // point text
    if(lines.isEmpty()) return 0;
    int line=qBound(0, int(std::floor(pt.y()/qMax(1.0,lineH))), int(lines.size())-1);
    int off=0; for(int i=0;i<line;++i) off+=lines[i].size()+1;
    const QString& L=lines[line];
    const double x0=leftIndent + (line==0? firstLineIndent : 0.0);
    double bestD=1e18; int bestCol=0;
    for(int c=0;c<=L.size();++c){ double xx=s_lineX(x0,L,c,tabWidth,tabStops,fm,scFm,smallCaps); double dd=qAbs(xx-pt.x()); if(dd<bestD){ bestD=dd; bestCol=c; } }
    return off+bestCol;
}

QRectF Shape::localBounds() const { return localPath().boundingRect(); }

QRectF Shape::docBounds() const { return transform.map(localPath()).boundingRect(); }

bool Shape::hitTest(const QPointF& docPt, double tol) const {
    if (!visible) return false;
    QPainterPath dp = transform.map(localPath());
    if (fill.isVisible() && type != ShapeType::Path && type != ShapeType::Group) {
        if (dp.contains(docPt)) return true;
    } else if (fill.isVisible()) {
        if (dp.contains(docPt)) return true;
    }
    // stroke proximity
    QPainterPathStroker st;
    st.setWidth(qMax(stroke.width, 1.0) + tol*2);
    if (st.createStroke(dp).contains(docPt)) return true;
    // bbox fallback for thin/open paths
    return dp.boundingRect().adjusted(-tol,-tol,tol,tol).contains(docPt) &&
           (type == ShapeType::Path || type == ShapeType::Group) &&
           QPainterPathStroker().createStroke(dp).contains(docPt);
}

QPainterPath Document::docPath(const Shape& s) { return s.transform.map(s.localPath()); }

QRectF Document::contentBounds() const {
    QRectF b;
    for (const Shape& s : shapes) if (s.visible) {
        QRectF db = s.docBounds();
        if (s.shadow.on) db = db.united(db.translated(s.shadow.offset).adjusted(-s.shadow.blur,-s.shadow.blur,s.shadow.blur,s.shadow.blur));
        b = b.isNull() ? db : b.united(db);
    }
    return b;
}

// ---- serialization ---------------------------------------------------------
static QJsonObject colJson(const QColor& c){ return QJsonObject{{"r",c.red()},{"g",c.green()},{"b",c.blue()},{"a",c.alpha()}}; }
static QColor colFrom(const QJsonObject& o){ return QColor(o["r"].toInt(),o["g"].toInt(),o["b"].toInt(),o["a"].toInt(255)); }
static QJsonObject ptJson(const QPointF& p){ return QJsonObject{{"x",p.x()},{"y",p.y()}}; }
static QPointF ptFrom(const QJsonObject& o){ return QPointF(o["x"].toDouble(),o["y"].toDouble()); }

QString pathToStr(const QPainterPath& p);        // compact O(n) path (de)serialisation (defined below)
QPainterPath pathFromStr(const QString& str);

QJsonObject Shape::toJson() const {
    QJsonObject o;
    o["type"] = int(type);
    o["name"] = name; if(!url.isEmpty()) o["url"]=url; if(!webEmbed.isEmpty()) o["embed"]=webEmbed;
    o["m"] = QJsonArray{transform.m11(),transform.m12(),transform.m13(),transform.m21(),transform.m22(),transform.m23(),transform.m31(),transform.m32(),transform.m33()};
    // fill
    QJsonObject f; f["t"]=int(fill.type); f["c"]=colJson(fill.color); if(fill.colorRef>=0) f["cRef"]=fill.colorRef;
    if(fill.spread!=QGradient::PadSpread) f["spread"]=int(fill.spread);
    if(fill.rainbow) f["rainbow"]=true; if(fill.rainbowAlt) f["rainbowAlt"]=true;
    if(fill.fractal) f["fractal"]=true; if(fill.tiled) f["tiled"]=true;
    if(!fill.adjust.isIdentity()){ const Fill::Adjust& a=fill.adjust;
        f["adj"]=QJsonObject{{"b",a.brightness},{"c",a.contrast},{"s",a.saturation},{"bl",a.blur},{"sh",a.sharpen},{"vg",a.vignette},
            {"cl",a.cropL},{"cr",a.cropR},{"ct",a.cropT},{"cb",a.cropB},{"ln",a.lens},{"hd",a.hdr},{"g",a.gray},{"r",a.redeye},
            {"lvb",a.blackPt},{"lvw",a.whitePt},{"lvg",a.gamma},{"wbt",a.temp},{"wbg",a.tint}}; }
    f["p0"]=ptJson(fill.p0); f["p1"]=ptJson(fill.p1);
    if(fill.profileBias!=0.0||fill.profileGain!=0.0){ f["pgb"]=fill.profileBias; f["pgg"]=fill.profileGain; }
    QJsonArray st; for (auto& s : fill.stops){ st.append(QJsonObject{{"p",s.pos},{"c",colJson(s.color)}}); } f["stops"]=st;
    if (fill.type==FillType::Bitmap) {
        f["p2"]=ptJson(fill.p2);
        if (fill.contone){ f["contone"]=true; f["c2"]=colJson(fill.color2); }
        if (!fill.imageSrc.isEmpty()){                                     // reuse the original encoded bytes (no slow PNG re-encode)
            f["imgsrc"]=QString::fromLatin1(fill.imageSrc.toBase64()); }
        else if (!fill.image.isNull()){ QByteArray ba; QBuffer buf(&ba); buf.open(QIODevice::WriteOnly);
            fill.image.save(&buf,"PNG"); f["img"]=QString::fromLatin1(ba.toBase64()); }
    }
    if (fill.type==FillType::Mesh || fill.type==FillType::Square) {
        f["p2"]=ptJson(fill.p2);
        QJsonArray mc; for (auto& c : fill.meshColors) mc.append(colJson(c)); f["mesh"]=mc;
        if(fill.meshRows>=2 && fill.meshCols>=2){ f["mRows"]=fill.meshRows; f["mCols"]=fill.meshCols; }   // N×M grid mesh (gated)
    }
    o["fill"]=f;
    // stroke
    QJsonObject k; k["on"]=stroke.enabled; k["c"]=colJson(stroke.color); k["w"]=stroke.width; if(stroke.colorRef>=0) k["cRef"]=stroke.colorRef;
    k["cap"]=int(stroke.cap); k["join"]=int(stroke.join); if(stroke.transp>0) k["tr"]=stroke.transp;
    if(stroke.startArrow) k["sArr"]=stroke.startArrow; if(stroke.endArrow) k["eArr"]=stroke.endArrow;
    if(stroke.startArrowW!=1.0||stroke.startArrowH!=1.0){ k["sAW"]=stroke.startArrowW; k["sAH"]=stroke.startArrowH; }   // arrow size (gated: default 1×)
    if(stroke.endArrowW!=1.0||stroke.endArrowH!=1.0){ k["eAW"]=stroke.endArrowW; k["eAH"]=stroke.endArrowH; }
    if(stroke.calligraphic){ k["cal"]=true; k["calA"]=stroke.calligAngle; }
    if(!stroke.brushRotateToLine) k["bRotL"]=false; if(stroke.brushRotate!=0.0) k["bRot"]=stroke.brushRotate;   // Edit-Brush attrs (gated)
    if(stroke.brushScatter!=0.0) k["bScat"]=stroke.brushScatter; if(stroke.brushSizeVar!=0.0) k["bSzV"]=stroke.brushSizeVar;
    o["stroke"]=k;
    o["opacity"]=opacity; o["blend"]=int(blend); o["visible"]=visible; o["locked"]=locked; o["layer"]=layer; if(overprint) o["ovp"]=true;
    if(linkId) o["linkId"]=linkId; if(softGroup) o["soft"]=softGroup;
    if(anchorTextIndex>=0){ o["anchTo"]=anchorTextIndex; o["anchCh"]=anchorChar; }
    if(webStretch) o["wStretch"]=webStretch; if(webPin) o["wPin"]=webPin; if(webSticky) o["wSticky"]=true;
    if(webLightbox) o["wLightbox"]=true;
    if(webNewWindow) o["wNewWin"]=true; if(!webBuyUrl.isEmpty()) o["wBuy"]=webBuyUrl; if(webHeading>0) o["wH"]=webHeading;
    if(webAnim) o["wAnim"]=webAnim; if(webAnimDelay>0) o["wAnimDelay"]=webAnimDelay;
    if(webSlider) o["wSlider"]=webSlider;
    if(!webMediaData.isEmpty()){ o["media"]=QString::fromLatin1(webMediaData.toBase64()); o["mediaMime"]=webMediaMime; }
    if(webNavBar) o["wNav"]=true;
    if(webBuild>0) o["wBuild"]=webBuild;
    if(webPopupId>0) o["wPopId"]=webPopupId; if(webPopupTrigger>0) o["wPopTrig"]=webPopupTrigger;
    if(webBand) o["wBand"]=true;
    if(webHideBelow>0) o["wHideB"]=webHideBelow;
    if(webFlex) o["wFlex"]=webFlex;
    if(webVarBelow>0){ o["vB"]=webVarBelow; o["vX"]=webVarX; o["vY"]=webVarY; o["vW"]=webVarW; }
    if(!webMenuTree.isEmpty()) o["menu"]=webMenuTree;
    if(!webForm.isEmpty()) o["wForm"]=webForm;
    if (fillRule==Qt::WindingFill) o["wind"]=1;
    if (transp.type>0){ QJsonObject tj{{"ty",transp.type},{"g",transp.geom},{"t0",transp.t0},{"t1",transp.t1},
        {"p0",ptJson(transp.p0)},{"p1",ptJson(transp.p1)}};
        if (transp.geom>=2) tj["p2"]=ptJson(transp.p2);
        if (transp.geom==4 && !transp.maskBmp.isNull()){ QByteArray ba; QBuffer buf(&ba); buf.open(QIODevice::WriteOnly);
            transp.maskBmp.save(&buf,"PNG"); tj["mask"]=QString::fromLatin1(ba.toBase64()); }
        if (!transp.stops.isEmpty()){ QJsonArray ts; for(const TranspStop& st: transp.stops) ts.append(QJsonObject{{"p",st.pos},{"t",st.t}}); tj["stops"]=ts; }
        if (transp.profileBias!=0.0 || transp.profileGain!=0.0){ tj["pb"]=transp.profileBias; tj["pg"]=transp.profileGain; }
        o["transp"]=tj; }
    o["rect"]=QJsonObject{{"x",rect.x()},{"y",rect.y()},{"w",rect.width()},{"h",rect.height()}};
    o["cr"]=cornerRadius; o["sides"]=polySides; o["star"]=polyStar; o["depth"]=polyStarDepth;
    if(polyCurvature!=0.0) o["curv"]=polyCurvature;
    if(ellipseMode!=0){ o["elm"]=ellipseMode; o["els"]=ellipseStart; o["elw"]=ellipseSweep; }
    if(type==ShapeType::Smart){ o["smK"]=smartKind; o["smP"]=smartParam; if(smartParam2>=0.0) o["smP2"]=smartParam2; }
    o["text"]=text; o["font"]=font.toString(); if(!textStyleName.isEmpty()) o["tStyle"]=textStyleName;
    if(textFrame){ o["tFrame"]=true; o["tCols"]=textCols; o["tColGap"]=textColGap;
        if(!textShapePath.isEmpty()){ QJsonArray pa; for(int i=0;i<textShapePath.elementCount();++i){ auto e=textShapePath.elementAt(i); pa.append(QJsonArray{int(e.type),e.x,e.y}); } o["tShape"]=pa; } }
    if(textAlign) o["tAlign"]=textAlign;
    if(nextFrame>=0) o["tNext"]=nextFrame;
    if(smallCaps) o["tSC"]=true; if(dropCap>1) o["tDrop"]=dropCap; if(hyphenate) o["tHyph"]=true;
    if(listStyle>0){ o["tList"]=listStyle; if(listIndent>0.0) o["tListI"]=listIndent; if(listStart!=1) o["tListS"]=listStart; }
    if(!otFeatures.isEmpty()) o["tOT"]=QJsonArray::fromStringList(otFeatures);
    if(qAbs(lineSpacing-1.18)>1e-9) o["tLead"]=lineSpacing; if(tracking!=0.0) o["tTrack"]=tracking; if(textAspect!=1.0) o["tAsp"]=textAspect;
    if(spaceBefore!=0.0) o["tSpB"]=spaceBefore; if(spaceAfter!=0.0) o["tSpA"]=spaceAfter;
    if(firstLineIndent!=0.0) o["tInd1"]=firstLineIndent; if(leftIndent!=0.0) o["tIndL"]=leftIndent; if(rightIndent!=0.0) o["tIndR"]=rightIndent;
    if(tabWidth!=0.0) o["tTab"]=tabWidth;
    if(!tabStops.isEmpty()){ QJsonArray ta; for(const TabStop& t: tabStops) ta.append(QJsonArray{t.pos, t.align, int(t.leader.unicode())}); o["tStops"]=ta; }
    if(repel) o["repel"]=true;
    if(!textRuns.isEmpty()){ QJsonArray tr; for(const TextRun& r: textRuns){ QJsonObject ro{{"s",r.start},{"l",r.len},{"f",r.font.toString()},{"c",(qint64)r.color.rgba()}}; if(!r.url.isEmpty()) ro["u"]=r.url; if(r.baseline!=0.0) ro["bl"]=r.baseline; if(r.tracking!=0.0) ro["tk"]=r.tracking; tr.append(ro); } o["tRuns"]=tr; }
    if (shadow.on){ QJsonObject sj{{"o",ptJson(shadow.offset)},{"b",shadow.blur},{"c",colJson(shadow.color)}}; if(shadow.type) sj["ty"]=shadow.type;
        if(shadow.type==1){ sj["fsh"]=shadow.floorShear; sj["fsc"]=shadow.floorScale; } o["shadow"]=sj; }
    if (contour.on){ QJsonObject cn{{"w",contour.width},{"s",contour.steps},{"c",colJson(contour.color)}};
        if(contour.color2.isValid()) cn["c2"]=colJson(contour.color2); if(contour.transp!=0.0) cn["tr"]=contour.transp; o["contour"]=cn; }
    if (bevel.on){ QJsonObject bv{{"d",bevel.depth},{"l",bevel.light}};
        if(bevel.lightColor!=QColor(255,255,255)) bv["lc"]=colJson(bevel.lightColor); o["bevel"]=bv; }
    if (extrude.on){ QJsonObject ex{{"d",extrude.depth},{"a",extrude.angle},{"l",extrude.light}};
        if(extrude.color.isValid()) ex["c"]=colJson(extrude.color);
        if(extrude.rotX!=0.0) ex["rx"]=extrude.rotX; if(extrude.rotY!=0.0) ex["ry"]=extrude.rotY; if(extrude.bevel!=0.0) ex["bv"]=extrude.bevel;
        o["extr"]=ex; }
    if (blur>0) o["blur"]=blur; if(blurGain!=1.0) o["blurGain"]=blurGain;
    if (type==ShapeType::Path) o["pathd"]=pathToStr(path);   // compact O(n) encoding (was "pathel", O(n^2))
    if (!regionPath.isEmpty()){ QJsonArray pa; for (int i=0;i<regionPath.elementCount();++i){ auto e=regionPath.elementAt(i); pa.append(QJsonArray{int(e.type),e.x,e.y}); } o["region"]=pa; if(regionFeather>0) o["regFea"]=regionFeather; }
    if (type==ShapeType::Group){ QJsonArray ch; for (auto& c : children) ch.append(c.toJson()); o["children"]=ch;
        if(blendSteps>0){ o["blendSteps"]=blendSteps; if(blendProfile!=0.0) o["blendProf"]=blendProfile; if(blendAttrProfile!=0.0) o["blendAttrP"]=blendAttrProfile; if(blendNodeShift!=0.0) o["blendNS"]=blendNodeShift;
            if(!blendSpine.isEmpty()){ QJsonArray pa; for(int i=0;i<blendSpine.elementCount();++i){ auto e=blendSpine.elementAt(i); pa.append(QJsonArray{int(e.type),e.x,e.y}); } o["blendSpine"]=pa; } }
        if(isClipView){ o["clipView"]=true; o["clipSplit"]=clipSplit; }
        if(isolate) o["isolate"]=true; }
    if (type==ShapeType::Table){ QJsonObject t;
        QJsonArray cw; for(double w:table.colW) cw.append(w); t["cw"]=cw;
        QJsonArray rh; for(double h:table.rowH) rh.append(h); t["rh"]=rh;
        QJsonArray cs; for(const TableCell& c:table.cells){ QJsonObject co;
            if(!c.text.isEmpty()) co["t"]=c.text;
            if(c.bg.alpha()) co["bg"]=colJson(c.bg);
            if(c.textColor!=QColor(30,34,40)) co["fc"]=colJson(c.textColor);
            if(c.align) co["a"]=c.align;
            if(c.valign) co["va"]=c.valign; if(c.bold) co["b"]=true;
            if(c.rowSpan!=1) co["rs"]=c.rowSpan; if(c.colSpan!=1) co["cs"]=c.colSpan;
            cs.append(co); }
        t["cells"]=cs; t["bc"]=colJson(table.borderColor); t["bw"]=table.borderW; t["pad"]=table.pad;
        if(table.bg.alpha()) t["bg"]=colJson(table.bg); if(table.spacing>0) t["sp"]=table.spacing;
        if(table.headerRow) t["hdr"]=true;
        o["table"]=t; }
    if (type==ShapeType::Text && !textPath.isEmpty()){
        QJsonArray pa; for (int i=0;i<textPath.elementCount();++i){ auto e=textPath.elementAt(i); pa.append(QJsonArray{int(e.type),e.x,e.y}); }
        o["tpath"]=pa;
        if (textPathOffset!=0.0) o["tpoff"]=textPathOffset;
        if (textPathInside) o["tpin"]=true;
    }
    return o;
}

// Rebuild a QPainterPath from a serialized element array (shared by `path` and `textPath`).
// Compact, O(n) path (de)serialisation as a space-separated "type x y" element stream. The old form
// stored each element as a nested QJsonArray appended one at a time, which is O(n^2) to build for large
// paths (QJson/CBOR container reallocation) — pathologically slow when a document has many/large paths.
QString pathToStr(const QPainterPath& p) {
    QString s; s.reserve(p.elementCount()*20);
    for (int i=0;i<p.elementCount();++i){ const QPainterPath::Element e=p.elementAt(i);
        if(i) s+=QLatin1Char(' ');
        s+=QString::number(int(e.type)); s+=QLatin1Char(' ');
        s+=QString::number(e.x,'g',17); s+=QLatin1Char(' '); s+=QString::number(e.y,'g',17); }
    return s;
}
QPainterPath pathFromStr(const QString& str) {
    QPainterPath p; const QStringList t=str.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (int i=0;i+2<t.size();){ const int ty=t[i].toInt();
        if (ty==QPainterPath::MoveToElement){ p.moveTo(t[i+1].toDouble(),t[i+2].toDouble()); i+=3; }
        else if (ty==QPainterPath::CurveToElement && i+8<t.size()){
            p.cubicTo(t[i+1].toDouble(),t[i+2].toDouble(), t[i+4].toDouble(),t[i+5].toDouble(), t[i+7].toDouble(),t[i+8].toDouble()); i+=9; }
        else { p.lineTo(t[i+1].toDouble(),t[i+2].toDouble()); i+=3; } }
    return p;
}
static QPainterPath pathFromJsonArray(const QJsonArray& pa) {
    QPainterPath pp;
    for (int i=0;i<pa.size();){ auto el=pa[i].toArray(); int ty=el[0].toInt();
        if (ty==QPainterPath::MoveToElement){ pp.moveTo(el[1].toDouble(),el[2].toDouble()); ++i; }
        else if (ty==QPainterPath::LineToElement){ pp.lineTo(el[1].toDouble(),el[2].toDouble()); ++i; }
        else if (ty==QPainterPath::CurveToElement && i+2<pa.size()){ auto e1=pa[i+1].toArray(), e2=pa[i+2].toArray();
            pp.cubicTo(el[1].toDouble(),el[2].toDouble(),e1[1].toDouble(),e1[2].toDouble(),e2[1].toDouble(),e2[2].toDouble()); i+=3; }
        else ++i; }
    return pp;
}

Shape Shape::fromJson(const QJsonObject& o) {
    Shape s;
    s.type = ShapeType(o["type"].toInt());
    s.name = o["name"].toString(); s.url = o["url"].toString(); s.webEmbed = o["embed"].toString();
    auto m = o["m"].toArray();
    if (m.size()==9) s.transform = QTransform(m[0].toDouble(),m[1].toDouble(),m[2].toDouble(),m[3].toDouble(),m[4].toDouble(),m[5].toDouble(),m[6].toDouble(),m[7].toDouble(),m[8].toDouble());
    else if (m.size()==6) s.transform = QTransform(m[0].toDouble(),m[1].toDouble(),m[2].toDouble(),m[3].toDouble(),m[4].toDouble(),m[5].toDouble());
    auto f = o["fill"].toObject();
    s.fill.type=FillType(f["t"].toInt()); s.fill.color=colFrom(f["c"].toObject()); s.fill.colorRef=f["cRef"].toInt(-1);
    if(f.contains("spread")) s.fill.spread=QGradient::Spread(f["spread"].toInt());
    s.fill.rainbow=f["rainbow"].toBool(false); s.fill.rainbowAlt=f["rainbowAlt"].toBool(false);
    s.fill.fractal=f["fractal"].toBool(false); s.fill.tiled=f["tiled"].toBool(false);
    if(f.contains("adj")){ auto a=f["adj"].toObject(); Fill::Adjust& d=s.fill.adjust;
        d.brightness=a["b"].toInt(); d.contrast=a["c"].toInt(); d.saturation=a["s"].toInt(); d.blur=a["bl"].toInt(); d.sharpen=a["sh"].toInt(); d.vignette=a["vg"].toInt();
        d.cropL=a["cl"].toInt(); d.cropR=a["cr"].toInt(); d.cropT=a["ct"].toInt(); d.cropB=a["cb"].toInt(); d.lens=a["ln"].toInt(); d.hdr=a["hd"].toInt(); d.gray=a["g"].toBool(); d.redeye=a["r"].toBool();
        d.blackPt=a.value("lvb").toInt(0); d.whitePt=a.value("lvw").toInt(255); d.gamma=a.value("lvg").toDouble(1.0);
        d.temp=a.value("wbt").toInt(0); d.tint=a.value("wbg").toInt(0); }
    s.fill.p0=ptFrom(f["p0"].toObject()); s.fill.p1=ptFrom(f["p1"].toObject());
    s.fill.profileBias=f["pgb"].toDouble(0.0); s.fill.profileGain=f["pgg"].toDouble(0.0);
    s.fill.stops.clear(); for (auto v : f["stops"].toArray()){ auto so=v.toObject(); s.fill.stops.append({so["p"].toDouble(),colFrom(so["c"].toObject())}); }
    if (s.fill.type==FillType::Bitmap) {
        s.fill.p2=ptFrom(f["p2"].toObject());
        s.fill.contone=f["contone"].toBool(false); s.fill.color2=colFrom(f["c2"].toObject());
        if (f.contains("imgsrc")){ QByteArray raw=QByteArray::fromBase64(f["imgsrc"].toString().toLatin1());
            s.fill.image.loadFromData(raw); s.fill.imageSrc=raw; }        // auto-detect format; keep src so re-save stays fast
        else if (f.contains("img")){ QByteArray ba=QByteArray::fromBase64(f["img"].toString().toLatin1());
            s.fill.image.loadFromData(ba,"PNG"); }
    }
    if (s.fill.type==FillType::Mesh || s.fill.type==FillType::Square) {
        s.fill.p2=ptFrom(f["p2"].toObject());
        s.fill.meshColors.clear(); for (auto v : f["mesh"].toArray()) s.fill.meshColors.append(colFrom(v.toObject()));
        s.fill.meshRows=qBound(0,f["mRows"].toInt(0),64); s.fill.meshCols=qBound(0,f["mCols"].toInt(0),64);   // cap so a crafted file can't overflow R*C
    }
    auto k=o["stroke"].toObject();
    s.stroke.enabled=k["on"].toBool(true); s.stroke.color=colFrom(k["c"].toObject()); s.stroke.colorRef=k["cRef"].toInt(-1); s.stroke.width=k["w"].toDouble(1);
    s.stroke.cap=Qt::PenCapStyle(k["cap"].toInt(Qt::RoundCap)); s.stroke.join=Qt::PenJoinStyle(k["join"].toInt(Qt::RoundJoin));
    s.stroke.transp=k["tr"].toDouble(0);
    s.stroke.startArrow=k["sArr"].toInt(0); s.stroke.endArrow=k["eArr"].toInt(0);
    s.stroke.startArrowW=k["sAW"].toDouble(1.0); s.stroke.startArrowH=k["sAH"].toDouble(1.0);
    s.stroke.endArrowW=k["eAW"].toDouble(1.0); s.stroke.endArrowH=k["eAH"].toDouble(1.0);
    s.stroke.calligraphic=k["cal"].toBool(false); s.stroke.calligAngle=k["calA"].toDouble(30.0);
    s.stroke.brushRotateToLine=k["bRotL"].toBool(true); s.stroke.brushRotate=k["bRot"].toDouble(0.0);
    s.stroke.brushScatter=k["bScat"].toDouble(0.0); s.stroke.brushSizeVar=k["bSzV"].toDouble(0.0);
    s.opacity=o["opacity"].toDouble(1); s.blend=QPainter::CompositionMode(o["blend"].toInt()); s.visible=o["visible"].toBool(true);
    if (o.contains("transp")){ auto t=o["transp"].toObject(); s.transp.type=t["ty"].toInt(); s.transp.geom=t["g"].toInt();
        s.transp.t0=t["t0"].toDouble(); s.transp.t1=t["t1"].toDouble(); s.transp.p0=ptFrom(t["p0"].toObject()); s.transp.p1=ptFrom(t["p1"].toObject());
        if (t.contains("p2")) s.transp.p2=ptFrom(t["p2"].toObject());
        if (t.contains("mask")){ QByteArray ba=QByteArray::fromBase64(t["mask"].toString().toLatin1()); s.transp.maskBmp.loadFromData(ba,"PNG"); }
        if (s.transp.geom==4 && s.transp.maskBmp.isNull()) s.transp.geom=0;   // a mask that failed to load can't render as one
        for (const QJsonValue& v : t["stops"].toArray()){ auto j=v.toObject(); s.transp.stops.push_back({j["p"].toDouble(), j["t"].toDouble()}); }
        s.transp.profileBias=t["pb"].toDouble(0.0); s.transp.profileGain=t["pg"].toDouble(0.0); }
    s.locked=o["locked"].toBool(false); s.overprint=o["ovp"].toBool(false); s.layer=o["layer"].toInt(0); s.linkId=o["linkId"].toInt(0); s.softGroup=o["soft"].toInt(0);
    s.anchorTextIndex=o.contains("anchTo")?o["anchTo"].toInt(-1):-1; s.anchorChar=o["anchCh"].toInt(0);
    s.webStretch=o["wStretch"].toInt(0); s.webPin=o["wPin"].toInt(0); s.webSticky=o["wSticky"].toBool(false);
    s.webLightbox=o["wLightbox"].toBool(false);
    s.webNewWindow=o["wNewWin"].toBool(false); s.webBuyUrl=o["wBuy"].toString(); s.webHeading=o["wH"].toInt(0);
    s.webAnim=o["wAnim"].toInt(0); s.webAnimDelay=o["wAnimDelay"].toDouble(0.0);
    s.webSlider=o["wSlider"].toInt(0);
    if(o.contains("media")){ s.webMediaData=QByteArray::fromBase64(o["media"].toString().toLatin1()); s.webMediaMime=o["mediaMime"].toString(); }
    s.webNavBar=o["wNav"].toBool(false);
    s.webBuild=o["wBuild"].toInt(0);
    s.webPopupId=o["wPopId"].toInt(0); s.webPopupTrigger=o["wPopTrig"].toInt(0);
    s.webBand=o["wBand"].toBool(false);
    s.webHideBelow=o["wHideB"].toInt(0);
    s.webFlex=o["wFlex"].toInt(0);
    s.webVarBelow=o["vB"].toInt(0); s.webVarX=o["vX"].toDouble(); s.webVarY=o["vY"].toDouble(); s.webVarW=o["vW"].toDouble();
    s.webMenuTree=o["menu"].toString();
    s.webForm=o["wForm"].toString();
    s.fillRule=o["wind"].toInt(0) ? Qt::WindingFill : Qt::OddEvenFill;
    auto r=o["rect"].toObject(); s.rect=QRectF(r["x"].toDouble(),r["y"].toDouble(),r["w"].toDouble(),r["h"].toDouble());
    s.cornerRadius=o["cr"].toDouble(); s.polySides=o["sides"].toInt(5); s.polyStar=o["star"].toBool(); s.polyStarDepth=o["depth"].toDouble(0.5);
    s.polyCurvature=o["curv"].toDouble(0.0);
    if(o.contains("elm")){ s.ellipseMode=o["elm"].toInt(0); s.ellipseStart=o["els"].toDouble(0.0); s.ellipseSweep=o["elw"].toDouble(360.0); }
    s.smartKind=o["smK"].toInt(0); s.smartParam=o["smP"].toDouble(6); s.smartParam2=o["smP2"].toDouble(-1);
    s.text=o["text"].toString("Text"); if (o.contains("font")) s.font.fromString(o["font"].toString());
    s.textStyleName=o["tStyle"].toString();
    s.textFrame=o["tFrame"].toBool(false); s.textCols=o["tCols"].toInt(1); s.textColGap=o["tColGap"].toDouble(20); s.textAlign=o["tAlign"].toInt(0);
    if(o.contains("tShape")) s.textShapePath=pathFromJsonArray(o["tShape"].toArray());
    s.nextFrame=o["tNext"].toInt(-1);
    s.lineSpacing=o["tLead"].toDouble(1.18); s.tracking=o["tTrack"].toDouble(0.0); s.textAspect=o["tAsp"].toDouble(1.0);
    s.spaceBefore=o["tSpB"].toDouble(0.0); s.spaceAfter=o["tSpA"].toDouble(0.0);
    s.firstLineIndent=o["tInd1"].toDouble(0.0); s.leftIndent=o["tIndL"].toDouble(0.0); s.rightIndent=o["tIndR"].toDouble(0.0);
    s.tabWidth=o["tTab"].toDouble(0.0);
    if(o.contains("tStops")){ s.tabStops.clear(); for(const auto v: o["tStops"].toArray()){ const QJsonArray a=v.toArray();
        if(a.size()>=2){ TabStop t; t.pos=a[0].toDouble(); t.align=a[1].toInt(); int lc=a.size()>2?a[2].toInt():0; if(lc>0) t.leader=QChar(lc); s.tabStops.push_back(t); } } }
    s.repel=o["repel"].toBool(false);
    if(o.contains("tRuns")){ for(const QJsonValue& v: o["tRuns"].toArray()){ QJsonObject ro=v.toObject(); TextRun r; r.start=ro["s"].toInt(); r.len=ro["l"].toInt(); r.font.fromString(ro["f"].toString()); r.color=QColor::fromRgba((QRgb)ro["c"].toVariant().toLongLong()); r.url=ro["u"].toString(); r.baseline=ro["bl"].toDouble(0.0); r.tracking=ro["tk"].toDouble(0.0); s.textRuns.push_back(r); } }
    s.smallCaps=o["tSC"].toBool(false); s.dropCap=o["tDrop"].toInt(0); s.hyphenate=o["tHyph"].toBool(false);
    s.listStyle=o["tList"].toInt(0); s.listIndent=o["tListI"].toDouble(0.0); s.listStart=o["tListS"].toInt(1);
    s.otFeatures.clear(); for(const QJsonValue& v: o["tOT"].toArray()) s.otFeatures<<v.toString();
    if (o.contains("shadow")){ auto sh=o["shadow"].toObject(); s.shadow.on=true; s.shadow.offset=ptFrom(sh["o"].toObject()); s.shadow.blur=sh["b"].toDouble(); s.shadow.color=colFrom(sh["c"].toObject()); s.shadow.type=sh["ty"].toInt(0);
        s.shadow.floorShear=sh["fsh"].toDouble(0.7); s.shadow.floorScale=sh["fsc"].toDouble(0.5); }
    if (o.contains("contour")){ auto cn=o["contour"].toObject(); s.contour.on=true; s.contour.width=cn["w"].toDouble(); s.contour.steps=cn["s"].toInt(1); s.contour.color=colFrom(cn["c"].toObject());
        if(cn.contains("c2")) s.contour.color2=colFrom(cn["c2"].toObject()); s.contour.transp=cn["tr"].toDouble(0.0); }
    if (o.contains("bevel")){ auto bv=o["bevel"].toObject(); s.bevel.on=true; s.bevel.depth=bv["d"].toDouble(7); s.bevel.light=bv["l"].toDouble(45);
        if(bv.contains("lc")) s.bevel.lightColor=colFrom(bv["lc"].toObject()); }
    if (o.contains("extr")){ auto ex=o["extr"].toObject(); s.extrude.on=true; s.extrude.depth=ex["d"].toDouble(28); s.extrude.angle=ex["a"].toDouble(35); s.extrude.light=ex["l"].toInt(135);
        if(ex.contains("c")) s.extrude.color=colFrom(ex["c"].toObject());
        s.extrude.rotX=ex["rx"].toDouble(0); s.extrude.rotY=ex["ry"].toDouble(0); s.extrude.bevel=ex["bv"].toDouble(0); }
    s.blur = o["blur"].toDouble(); s.blurGain=o["blurGain"].toDouble(1.0);
    if (o.contains("pathd")) s.path=pathFromStr(o["pathd"].toString());   // compact form (faithful cubics)
    else if (o.contains("pathel")){ QPainterPath p; for (auto v : o["pathel"].toArray()){ auto e=v.toArray(); int t=e[0].toInt(); double x=e[1].toDouble(),y=e[2].toDouble(); if(t==0)p.moveTo(x,y); else if(t==1)p.lineTo(x,y); else p.lineTo(x,y);} s.path=p; }   // legacy
    if (o.contains("tpath")){ s.textPath=pathFromJsonArray(o["tpath"].toArray()); s.textPathOffset=qMax(0.0, o["tpoff"].toDouble(0.0)); s.textPathInside=o["tpin"].toBool(false); }   // clamp ≥0 (a corrupt negative would render glyphs before the path start)
    if (o.contains("region")){ s.regionPath=pathFromJsonArray(o["region"].toArray()); s.regionFeather=o["regFea"].toDouble(0); }
    if (o.contains("children")){ for (auto v : o["children"].toArray()) s.children.append(Shape::fromJson(v.toObject()));
        s.blendSteps=o["blendSteps"].toInt(0); s.blendProfile=o["blendProf"].toDouble(0.0); s.blendAttrProfile=o["blendAttrP"].toDouble(0.0); s.blendNodeShift=o["blendNS"].toDouble(0.0);
        if(o.contains("blendSpine")) s.blendSpine=pathFromJsonArray(o["blendSpine"].toArray());
        s.isClipView=o["clipView"].toBool(false); s.clipSplit=o["clipSplit"].toInt(0);
        s.isolate=o["isolate"].toBool(false); }
    if (o.contains("table")){ auto t=o["table"].toObject();
        for(auto v:t["cw"].toArray()) s.table.colW.append(v.toDouble());
        for(auto v:t["rh"].toArray()) s.table.rowH.append(v.toDouble());
        for(auto v:t["cells"].toArray()){ auto co=v.toObject(); TableCell c;
            c.text=co["t"].toString(); if(co.contains("bg")) c.bg=colFrom(co["bg"].toObject());
            c.textColor=co.contains("fc")?colFrom(co["fc"].toObject()):QColor(30,34,40);
            c.align=co["a"].toInt(0); c.valign=co["va"].toInt(0); c.bold=co["b"].toBool(false);
            c.rowSpan=co["rs"].toInt(1); c.colSpan=co["cs"].toInt(1);
            s.table.cells.append(c); }
        s.table.borderColor=colFrom(t["bc"].toObject()); s.table.borderW=t["bw"].toDouble(1); s.table.pad=t["pad"].toDouble(5);
        if(t.contains("bg")) s.table.bg=colFrom(t["bg"].toObject()); s.table.spacing=t["sp"].toDouble(0);
        s.table.headerRow=t["hdr"].toBool(false); }
    return s;
}

QColor Document::resolveNamed(int idx, int depth) const {
    if (idx<0 || idx>=namedColors.size() || depth>16) return Qt::black;
    const NamedColor& n = namedColors[idx];
    if (n.kind==0 || n.parent<0 || n.parent>=namedColors.size()) return n.color;
    QColor p = resolveNamed(n.parent, depth+1);
    switch (n.kind) {
        case 1: return lerp(p, Qt::white, n.amount);   // tint  -> toward white
        case 2: return lerp(p, Qt::black, n.amount);   // shade -> toward black
        case 3: return p;                              // linked -> same as parent
    }
    return n.color;
}

void Document::resyncNamedColorBindings() {
    std::function<void(Shape&)> walk = [&](Shape& s){
        if (s.fill.colorRef>=0 && s.fill.colorRef<namedColors.size()) s.fill.color = resolveNamed(s.fill.colorRef);
        if (s.stroke.colorRef>=0 && s.stroke.colorRef<namedColors.size()) s.stroke.color = resolveNamed(s.stroke.colorRef);
        for (Shape& c : s.children) walk(c);
    };
    for (Shape& s : shapes) walk(s);
}

void Document::reindexNamedColorsAfterDelete(int d) {
    if (d < 0) return;
    auto fix = [d](int& ref){ if(ref==d) ref=-1; else if(ref>d) --ref; };   // ==d unbind, >d shift down
    // Fill/Stroke colorRef bindings across every shape + nested group child.
    std::function<void(Shape&)> walk = [&](Shape& s){
        fix(s.fill.colorRef); fix(s.stroke.colorRef);
        for (Shape& c : s.children) walk(c);
    };
    for (Shape& s : shapes) walk(s);
    // Tint/shade/linked parent chains among the remaining palette entries.
    for (NamedColor& n : namedColors) {
        if (n.parent == d) { n.parent = -1; n.kind = 0; }   // parent gone → detach, keep own baked colour
        else if (n.parent > d) --n.parent;
    }
}

QJsonObject Document::toJson() const {
    QJsonObject o;
    { QJsonObject pg{{"w",pageSize.width()},{"h",pageSize.height()},{"count",pageCount},{"gap",pageGap}};
      if(!rulerOrigin.isNull()){ pg["rox"]=rulerOrigin.x(); pg["roy"]=rulerOrigin.y(); }   // ruler zero-point (gated: default 0,0)
      if(!scaleLineWidths) pg["noScaleLW"]=true;   // gated: default is ON (scale stroke width on resize)
      if(bleed>0) pg["bleed"]=bleed;
      auto nums=[](const QVector<double>& v){ QJsonArray a; for(double x: v) a.append(x); return a; };
      if(!guidesH.isEmpty()) pg["gh"]=nums(guidesH);
      if(!guidesV.isEmpty()) pg["gv"]=nums(guidesV);
      o["page"]=pg; }
    if(pageColor!=QColor(Qt::white)) o["pgcol"]=(qint64)pageColor.rgba();
    QJsonArray sh; for (auto& s : shapes) sh.append(s.toJson()); o["shapes"]=sh;
    if(!namedColors.isEmpty()){ QJsonArray nc; for(auto& n: namedColors)
        nc.append(QJsonObject{{"name",n.name},{"c",(qint64)n.color.rgba()},{"kind",n.kind},{"parent",n.parent},{"amount",n.amount},{"theme",n.theme},{"spot",n.spot}});
        o["namedColors"]=nc; }
    if(layers.size()>1){ QJsonArray ly; for(auto& l: layers){ QJsonObject lo{{"name",l.name},{"vis",l.visible},{"lock",l.locked},{"master",l.master}}; if(l.frameDelayMs>0) lo["fdly"]=l.frameDelayMs; if(!l.printable) lo["noprint"]=true; ly.append(lo); } o["layers"]=ly; }
    if(!textStyles.isEmpty()){ QJsonArray ts; for(auto& t: textStyles) ts.append(QJsonObject{{"name",t.name},{"font",t.font.toString()},{"c",(qint64)t.color.rgba()},{"a",t.align}}); o["textStyles"]=ts; }
    if(!breakpoints.isEmpty()){ QJsonArray bp; for(int w: breakpoints) bp.append(w); o["breakpoints"]=bp; }
    if(!webTitle.isEmpty()||!webDescription.isEmpty()||!webKeywords.isEmpty()||!webBaseUrl.isEmpty()||!webAuthor.isEmpty())
        o["seo"]=QJsonObject{{"t",webTitle},{"d",webDescription},{"k",webKeywords},{"url",webBaseUrl},{"au",webAuthor}};
    { bool any=false; for(const PageProps& pp: pageProps) if(!pp.empty()){ any=true; break; }
      if(any){ QJsonArray pa; for(const PageProps& pp: pageProps) pa.append(QJsonObject{{"n",pp.name},{"t",pp.title},{"d",pp.description},{"k",pp.keywords}}); o["pageProps"]=pa; } }
    if(!webPassword.isEmpty()) o["webPw"]=webPassword;
    if(fluidSite) o["fluid"]=true;
    if(presentSteps) o["present"]=true;
    if(!webHeadHtml.isEmpty()||!webBodyHtml.isEmpty()) o["webraw"]=QJsonObject{{"head",webHeadHtml},{"body",webBodyHtml}};
    return o;
}
Document Document::fromJson(const QJsonObject& o) {
    Document d;
    auto p=o["page"].toObject(); if(!p.isEmpty()){ d.pageSize=QSizeF(p["w"].toDouble(1123),p["h"].toDouble(794)); d.pageCount=qMax(1,p["count"].toInt(1)); d.pageGap=p["gap"].toDouble(50); d.rulerOrigin=QPointF(p["rox"].toDouble(0),p["roy"].toDouble(0)); d.scaleLineWidths=!p["noScaleLW"].toBool(false); }
    if(!p.isEmpty()){ d.bleed=qBound(0.0,p["bleed"].toDouble(0),1e5);
        for(const auto& v: p["gh"].toArray()) d.guidesH.push_back(v.toDouble());
        for(const auto& v: p["gv"].toArray()) d.guidesV.push_back(v.toDouble()); }
    if(o.contains("pgcol")) d.pageColor=QColor::fromRgba((QRgb)o["pgcol"].toVariant().toLongLong());
    d.shapes.clear(); for (auto v : o["shapes"].toArray()) d.shapes.append(Shape::fromJson(v.toObject()));
    for (auto v : o["namedColors"].toArray()){ auto j=v.toObject(); NamedColor n; n.name=j["name"].toString();
        n.color=QColor::fromRgba((QRgb)j["c"].toVariant().toLongLong()); n.kind=j["kind"].toInt(); n.parent=j["parent"].toInt(-1);
        n.amount=j["amount"].toDouble(0.5); n.theme=j["theme"].toBool(); n.spot=j["spot"].toBool(false); d.namedColors.append(n); }
    if(o.contains("layers")){ d.layers.clear(); for(auto v: o["layers"].toArray()){ auto j=v.toObject(); Layer l;
        l.name=j["name"].toString("Layer"); l.visible=j["vis"].toBool(true); l.locked=j["lock"].toBool(false); l.master=j["master"].toBool(false); l.printable=!j["noprint"].toBool(false); l.frameDelayMs=j["fdly"].toInt(0); d.layers.append(l); }
        if(d.layers.isEmpty()) d.layers.append(Layer{}); }
    for (auto v : o["textStyles"].toArray()){ auto j=v.toObject(); TextStyle t; t.name=j["name"].toString();
        t.font.fromString(j["font"].toString("DejaVu Sans,36")); t.color=QColor::fromRgba((QRgb)j["c"].toVariant().toLongLong()); t.align=j["a"].toInt(); d.textStyles.append(t); }
    for (auto v : o["breakpoints"].toArray()) d.breakpoints.append(v.toInt());
    if(o.contains("seo")){ auto s=o["seo"].toObject(); d.webTitle=s["t"].toString(); d.webDescription=s["d"].toString(); d.webKeywords=s["k"].toString(); d.webBaseUrl=s["url"].toString(); d.webAuthor=s["au"].toString(); }
    if(o.contains("pageProps")){ for(auto v: o["pageProps"].toArray()){ auto j=v.toObject(); PageProps pp; pp.name=j["n"].toString(); pp.title=j["t"].toString(); pp.description=j["d"].toString(); pp.keywords=j["k"].toString(); d.pageProps.append(pp); } }
    d.webPassword=o["webPw"].toString();
    d.fluidSite=o["fluid"].toBool(false);
    d.presentSteps=o["present"].toBool(false);
    if(o.contains("webraw")){ auto r=o["webraw"].toObject(); d.webHeadHtml=r["head"].toString(); d.webBodyHtml=r["body"].toString(); }
    return d;
}

// Morph outline A into outline B in `steps` even stages. Both outlines are sampled by ANGLE
// from their centroid so point k of each sits at the same bearing (a radial morph that doesn't
// fold), then interpolated point-for-point. Colours/stroke/opacity lerp alongside.
// Schlick bias/gain profile (Xara blend profiles). bias/gain are in [-1,+1]; 0,0 == identity.
static double pgBias(double b, double t){ double d=(1-2*b)*(1-t)+b; return qAbs(d)<1e-9? t : t*b/d; }
static double pgGain(double g, double t){ double cp=(1-2*g)*(1-2*t);
    if(t<0.5){ double d=cp+g; return qAbs(d)<1e-9? t : t*g/d; }
    double d=cp-g; return qAbs(d)<1e-9? t : (cp-t*g)/d; }
static double profileMap(double t, double bias, double gain){
    if(bias==0.0 && gain==0.0) return t;
    double b=qBound(1e-4,(bias+1)*0.5,1-1e-4), g=qBound(1e-4,(gain+1)*0.5,1-1e-4);
    return qBound(0.0, pgGain(g, pgBias(b, qBound(0.0,t,1.0))), 1.0);
}

QVector<Shape> blendMorph(const Shape& a, const Shape& b, int steps,
                          double objBias, double objGain, double attrBias, double attrGain, double nodeShift) {
    QVector<Shape> out;
    if (steps < 1) return out;
    // GROUP endpoints have no fill of their own — morphing their combined silhouette as one solid path
    // paints the model's default fill (a bright-blue blob) and collapses the children into a single fan.
    // Blend the two groups CHILD-BY-CHILD (recursing, so nested groups + each child's own fill/no-fill
    // survive). Falls through to the silhouette morph only when the structures don't line up.
    if (a.type == ShapeType::Group && b.type == ShapeType::Group &&
        a.children.size() == b.children.size() && !a.children.isEmpty()) {
        QVector<QVector<Shape>> perChild; perChild.reserve(a.children.size());
        for (int c = 0; c < a.children.size(); ++c) {
            Shape ca = a.children[c]; ca.transform = a.transform * ca.transform;   // bake parent xf -> doc space
            Shape cb = b.children[c]; cb.transform = b.transform * cb.transform;
            perChild.push_back(blendMorph(ca, cb, steps, objBias, objGain, attrBias, attrGain, nodeShift));
        }
        for (int i = 0; i < steps; ++i) {
            Shape g; g.type = ShapeType::Group; g.name = "Blend"; g.transform = QTransform();
            for (const QVector<Shape>& cm : perChild) if (i < cm.size()) g.children.push_back(cm[i]);
            if (!g.children.isEmpty()) out.push_back(g);
        }
        return out;
    }
    const QPainterPath pa = Document::docPath(a), pb = Document::docPath(b);
    if (pa.elementCount() < 2 || pb.elementCount() < 2) return out;
    // Subpath-aware ARC-LENGTH morph (mimics the Kernel GBlend): split each endpoint into its subpaths, pair
    // them by index, resample each by cumulative arc length from vertex[0], and lerp point-for-point. (The old
    // angle-from-centroid pick concatenated all subpaths and collapsed a multi-subpath gear outline —
    // rim/teeth/spokes/hub — into one fan with spurious radial spokes; Watch4's guilloché wheels need the
    // subpath structure kept.)
    const QList<QPolygonF> subA = pa.toSubpathPolygons(), subB = pb.toSubpathPolygons();
    if(subA.isEmpty() || subB.isEmpty()) return out;
    auto centroid=[](const QPolygonF& p)->QPointF{ QPointF c; for(const QPointF& q:p) c+=q; return p.size()? c/double(p.size()):QPointF(); };
    auto arcRes=[](const QPolygonF& poly, int K, bool open)->QVector<QPointF>{    // K points equally spaced by arc length, from poly[0]
        QVector<QPointF> r; const int M=poly.size();
        if(M<2){ r.fill(M? poly[0]:QPointF(), K); return r; }
        QVector<double> cum(M,0.0); double total=0;
        for(int j=1;j<M;++j){ total+=std::hypot(poly[j].x()-poly[j-1].x(),poly[j].y()-poly[j-1].y()); cum[j]=total; }
        if(open){    // OPEN stroke (e.g. a globe's meridian arc): sample [poly[0]..poly[M-1]] inclusive; NO wrap-around
            if(total<1e-9){ r.fill(poly[0],K); return r; } // segment back to poly[0] (that phantom edge was the "cage")
            r.reserve(K);
            for(int k=0;k<K;++k){ double d=(K>1)? total*double(k)/(K-1) : 0.0;
                int j=0; while(j+1<M && cum[j+1]<=d) ++j;
                if(j>M-2) j=M-2;   // last sample (d==total) would index poly[M]/cum[M] — out of bounds,
                                   // uninitialised: garbage*f(=0) is usually 0 but a NaN pattern makes the
                                   // vertex NaN, so the blend (baked at XAR load) renders non-deterministically.
                double seg=cum[j+1]-cum[j], f=seg>1e-9?(d-cum[j])/seg:0;
                r.push_back(poly[j]*(1.0-f)+poly[j+1]*f); }
            return r; }
        double closeLen=std::hypot(poly[0].x()-poly[M-1].x(),poly[0].y()-poly[M-1].y()), whole=total+closeLen;
        if(whole<1e-9){ r.fill(poly[0],K); return r; }
        r.reserve(K);
        for(int k=0;k<K;++k){ double d=whole*double(k)/K;
            if(d>=total){ double f=closeLen>1e-9?(d-total)/closeLen:0; r.push_back(poly[M-1]*(1.0-f)+poly[0]*f); continue; }
            int j=0; while(j+1<M && cum[j+1]<=d) ++j; double seg=cum[j+1]-cum[j], f=seg>1e-9?(d-cum[j])/seg:0;
            r.push_back(poly[j]*(1.0-f)+poly[j+1]*f); }
        return r; };
    // Per subpath, matched K-point samples. A subpath present in only ONE endpoint blends to/from the OTHER
    // endpoint's centroid (so a wheel keeps all of rim/teeth/spokes/hub instead of collapsing to a point).
    const int P=qMax(subA.size(), subB.size());
    QVector<QVector<QPointF>> ra(P), rb(P);
    QVector<bool> openK(P,false);   // an OPEN subpath (stroked arc, not a filled loop) morphs without a closing edge
    for(int k=0;k<P;++k){ bool hasA=k<subA.size(), hasB=k<subB.size();
        // Open iff every present endpoint's subpath is open (QPolygonF::isClosed() == first==last, set by closeSubpath()).
        // Closed subpaths keep the original wrap-around + closeSubpath() path => filled loops (gears/wheels) stay byte-identical.
        openK[k] = (!hasA || !subA[k].isClosed()) && (!hasB || !subB[k].isClosed());
        int K=qMax(64, 2*qMax(hasA?subA[k].size():0, hasB?subB[k].size():0));
        ra[k] = hasA? arcRes(subA[k],K,openK[k]) : QVector<QPointF>(K, centroid(subB[k]));
        rb[k] = hasB? arcRes(subB[k],K,openK[k]) : QVector<QPointF>(K, centroid(subA[k])); }
    const QColor ca=a.fill.representativeColor(), cb=b.fill.representativeColor();
    for(int i=1;i<=steps;++i){ double lin=double(i)/(steps+1);
        double tg=profileMap(lin, objBias, objGain);     // object profile reshapes step distribution (geometry)
        double ta=profileMap(tg, attrBias, attrGain);    // attribute profile reshapes colours/opacity on top
        QPainterPath m;
        for(int k=0;k<P;++k){ const QVector<QPointF>& A=ra[k]; const QVector<QPointF>& B=rb[k]; int K=qMin(A.size(),B.size());
            // Node correspondence: rotate the end-shape's samples by `nodeShift` of the perimeter (closed subpaths
            // only; an open arc has no wrap so the shift is meaningless there). nodeShift==0 => byte-identical.
            const int off = (!openK[k] && nodeShift!=0.0 && K>0) ? ((int(std::llround(nodeShift*K))%K)+K)%K : 0;
            for(int j=0;j<K;++j){ QPointF p=A[j]*(1.0-tg)+B[(j+off)%K]*tg; if(j==0) m.moveTo(p); else m.lineTo(p); }
            if(!openK[k]) m.closeSubpath(); }   // OPEN subpaths: leave un-closed (no spurious "cage" segment)
        m.setFillRule(a.fillRule);
        Shape sh; sh.type=ShapeType::Path; sh.path=m; sh.transform=QTransform(); sh.fillRule=a.fillRule;
        sh.fill=a.fill;
        const bool vaF=a.fill.isVisible(), vbF=b.fill.isVisible();
        if(vaF || vbF){ sh.fill.type=FillType::Solid; sh.fill.color=lerp(vaF?ca:cb, vbF?cb:ca, ta); }  // missing side reuses the present colour
        else sh.fill.type=FillType::None;                                                              // both unfilled -> outline-only step
        sh.stroke=a.stroke; sh.stroke.enabled=a.stroke.enabled || b.stroke.enabled;
        sh.stroke.color=lerp(a.stroke.color,b.stroke.color,ta);
        sh.opacity=a.opacity+(b.opacity-a.opacity)*ta; sh.name="Blend";
        // Carry GRADUATED transparency across the steps: a blend between two faded keys must itself fade.
        // (Dropping it made Watch4's two ~95%-transparent dark-blue backdrop paths a hard OPAQUE near-black
        // fan.) Only when BOTH endpoints are graduated — propagating a FLAT blend-mode would over-wash gold.
        { const Transparency& tA=a.transp; const Transparency& tB=b.transp;
          if(tA.type>0 && tA.geom!=0 && tB.type>0 && tB.geom!=0){
              sh.transp=tA;
              sh.transp.t0=tA.t0+(tB.t0-tA.t0)*ta; sh.transp.t1=tA.t1+(tB.t1-tA.t1)*ta;
              sh.transp.p0=tA.p0*(1.0-tg)+tB.p0*tg; sh.transp.p1=tA.p1*(1.0-tg)+tB.p1*tg; sh.transp.p2=tA.p2*(1.0-tg)+tB.p2*tg;
          } }
        out.push_back(sh);
    }
    return out;
}

} // namespace xara
