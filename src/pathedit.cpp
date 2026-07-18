#include "pathedit.h"
#include <QLineF>
#include <QPolygonF>
#include <QtGlobal>
#include <cmath>

namespace xara {

QVector<SubPath> pathToNodes(const QPainterPath& path) {
    QVector<SubPath> subs;
    SubPath cur;
    auto flush=[&]{ if(!cur.nodes.isEmpty()) subs.push_back(cur); cur=SubPath(); };

    const int n = path.elementCount();
    for (int i=0;i<n;++i) {
        const auto e = path.elementAt(i);
        if (e.type==QPainterPath::MoveToElement) {
            flush();
            PathNode nd; nd.anchor=QPointF(e.x,e.y); cur.nodes.push_back(nd);
        } else if (e.type==QPainterPath::LineToElement) {
            // A closing line back onto the subpath's start marks it closed.
            if (!cur.nodes.isEmpty() &&
                QLineF(QPointF(e.x,e.y), cur.nodes.first().anchor).length()<1e-6 && cur.nodes.size()>1) {
                cur.closed=true;
            } else {
                PathNode nd; nd.anchor=QPointF(e.x,e.y); cur.nodes.push_back(nd);
            }
        } else if (e.type==QPainterPath::CurveToElement) {
            // cubic: e = c1, i+1 = c2, i+2 = end
            QPointF c1(e.x,e.y);
            const auto e2 = path.elementAt(i+1);
            const auto e3 = path.elementAt(i+2);
            QPointF c2(e2.x,e2.y), end(e3.x,e3.y);
            if (!cur.nodes.isEmpty()) { cur.nodes.last().out=c1; cur.nodes.last().hasOut=true; }
            // closing cubic onto the start anchor?
            if (!cur.nodes.isEmpty() &&
                QLineF(end, cur.nodes.first().anchor).length()<1e-6 && cur.nodes.size()>1) {
                cur.nodes.first().in=c2; cur.nodes.first().hasIn=true; cur.closed=true;
            } else {
                PathNode nd; nd.anchor=end; nd.in=c2; nd.hasIn=true; cur.nodes.push_back(nd);
            }
            i+=2;
        }
    }
    flush();

    // Tag nodes whose in/out handles are roughly collinear as "smooth".
    for (auto& s : subs)
        for (auto& nd : s.nodes)
            if (nd.hasIn && nd.hasOut) {
                QPointF a=nd.anchor-nd.in, b=nd.out-nd.anchor;
                double la=std::hypot(a.x(),a.y()), lb=std::hypot(b.x(),b.y());
                if (la>1e-3 && lb>1e-3) {
                    double cross=(a.x()*b.y()-a.y()*b.x())/(la*lb);
                    if (std::fabs(cross)<0.08) {
                        nd.smooth=true;
                        // Equal-length collinear handles => symmetric (point-mirror) rather than merely smooth.
                        if (std::fabs(la-lb) <= 0.02*(la+lb)) nd.symmetric=true;
                    }
                }
            }
    return subs;
}

QPainterPath nodesToPath(const QVector<SubPath>& subs) {
    QPainterPath p;
    for (const auto& s : subs) {
        if (s.nodes.isEmpty()) continue;
        p.moveTo(s.nodes.first().anchor);
        auto seg=[&](const PathNode& a, const PathNode& b){
            if (a.hasOut || b.hasIn) {
                QPointF c1 = a.hasOut ? a.out : a.anchor;
                QPointF c2 = b.hasIn  ? b.in  : b.anchor;
                p.cubicTo(c1,c2,b.anchor);
            } else p.lineTo(b.anchor);
        };
        for (int i=1;i<s.nodes.size();++i) seg(s.nodes[i-1], s.nodes[i]);
        if (s.closed) { seg(s.nodes.last(), s.nodes.first()); p.closeSubpath(); }
    }
    return p;
}

// ---- Envelope / mould warp (Coons patch over a 4-sided cubic-bezier cage) ----
static QPointF s_cubicAt(const QPointF& p0,const QPointF& c1,const QPointF& c2,const QPointF& p3,double t){
    double m=1-t, a=m*m*m, b=3*m*m*t, c=3*m*t*t, e=t*t*t;
    return QPointF(a*p0.x()+b*c1.x()+c*c2.x()+e*p3.x(), a*p0.y()+b*c1.y()+c*c2.y()+e*p3.y());
}
QPointF coonsWarp(const Envelope& e, double u, double v){
    auto E=[&](int i,double t){ return s_cubicAt(e.cor[i], e.ec1[i], e.ec2[i], e.cor[(i+1)&3], t); };
    QPointF L=E(0,v), T=E(1,u), R=E(2,1-v), B=E(3,1-u);   // left/top/right/bottom rails
    const QPointF&c0=e.cor[0]; const QPointF&c1=e.cor[1]; const QPointF&c2=e.cor[2]; const QPointF&c3=e.cor[3];
    double x=(1-v)*B.x()+v*T.x()+(1-u)*L.x()+u*R.x()
            -((1-u)*(1-v)*c0.x()+u*(1-v)*c3.x()+(1-u)*v*c1.x()+u*v*c2.x());
    double y=(1-v)*B.y()+v*T.y()+(1-u)*L.y()+u*R.y()
            -((1-u)*(1-v)*c0.y()+u*(1-v)*c3.y()+(1-u)*v*c1.y()+u*v*c2.y());
    return QPointF(x,y);
}
QPainterPath warpThroughEnvelope(const QPainterPath& docPath, const QRectF& src, const Envelope& e){
    double sw=src.width()>1?src.width():1, sh=src.height()>1?src.height():1;
    double step=qMax(2.0, qMin(sw,sh)/40.0);
    auto map=[&](const QPointF& p){ double u=qBound(0.0,(p.x()-src.left())/sw,1.0),
                                           v=qBound(0.0,(src.bottom()-p.y())/sh,1.0); return coonsWarp(e,u,v); };
    QPainterPath out;
    for(const QPolygonF& poly : docPath.toSubpathPolygons()){
        if(poly.isEmpty()) continue;
        QPolygonF w; w<<map(poly[0]);
        for(int i=1;i<poly.size();++i){
            QPointF a=poly[i-1], b=poly[i]; double len=QLineF(a,b).length();
            int sub=qBound(1,(int)(len/step),64);
            for(int k=1;k<=sub;++k) w<<map(a+(b-a)*(double(k)/sub));
        }
        out.addPolygon(w); out.closeSubpath();
    }
    return out;
}
bool envelopeIsCurved(const Envelope& e, double tol){
    for(int i=0;i<4;++i){ QLineF chord(e.cor[i],e.cor[(i+1)&3]); double L=chord.length(); if(L<1) continue;
        auto off=[&](const QPointF& c){ return qAbs((c.x()-chord.x1())*(chord.y2()-chord.y1())-(c.y()-chord.y1())*(chord.x2()-chord.x1()))/L; };
        if(off(e.ec1[i])>tol||off(e.ec2[i])>tol) return true; }
    return false;
}

} // namespace xara
