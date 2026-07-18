#include "icons.h"
#include <QPainter>
#include <QPixmap>
#include <QPainterPath>
#include <QtMath>

namespace xara {

static QPixmap makePix(int s, std::function<void(QPainter&,double)> draw, const QColor& col) {
    QPixmap pm(s*2, s*2); pm.setDevicePixelRatio(2.0); pm.fill(Qt::transparent);
    QPainter g(&pm); g.setRenderHint(QPainter::Antialiasing);
    QPen pen(col, 1.7); pen.setCapStyle(Qt::RoundCap); pen.setJoinStyle(Qt::RoundJoin);
    g.setPen(pen); g.setBrush(Qt::NoBrush);
    draw(g, s);
    return pm;
}

QIcon toolIcon(Tool t, int size, const QColor& col) {
    auto pm = makePix(size, [&](QPainter& g, double s){
        const double m=s*0.2, M=s*0.8, c=s*0.5;
        switch (t) {
        case Tool::Select: { // arrow cursor
            QPainterPath p; p.moveTo(m,m); p.lineTo(m,M); p.lineTo(s*0.42,s*0.62); p.lineTo(s*0.56,s*0.86);
            p.lineTo(s*0.66,s*0.8); p.lineTo(s*0.52,s*0.56); p.lineTo(M,s*0.5); p.closeSubpath();
            g.setBrush(col); g.drawPath(p); break; }
        case Tool::Node: { // path with nodes
            g.drawLine(QPointF(m,M),QPointF(c,m)); g.drawLine(QPointF(c,m),QPointF(M,M));
            g.setBrush(Qt::NoBrush); for(QPointF v:{QPointF(m,M),QPointF(c,m),QPointF(M,M)}) g.drawRect(QRectF(v.x()-2,v.y()-2,4,4)); break; }
        case Tool::Rect: g.drawRoundedRect(QRectF(m,m+s*0.06,M-m,M-m-s*0.12),2,2); break;
        case Tool::Ellipse: g.drawEllipse(QRectF(m,m+s*0.05,M-m,M-m-s*0.1)); break;
        case Tool::Polygon: { QPainterPath p; for(int i=0;i<5;++i){ double a=-M_PI/2+i*2*M_PI/5; QPointF v(c+qCos(a)*(c-m),c+qSin(a)*(c-m)); if(i==0)p.moveTo(v);else p.lineTo(v);} p.closeSubpath(); g.drawPath(p); break; }
        case Tool::Pen: { QPainterPath p; p.moveTo(m,M); p.cubicTo(m,m, M,m, M,M); g.drawPath(p); for(QPointF v:{QPointF(m,M),QPointF(M,M)}){ g.setBrush(Qt::NoBrush); g.drawRect(QRectF(v.x()-2,v.y()-2,4,4)); } break; }
        case Tool::Line: g.drawLine(QPointF(m,M),QPointF(M,m)); break;
        case Tool::Freehand: { QPainterPath p; p.moveTo(m,M*0.9); p.cubicTo(s*0.3,m, s*0.55,M, M,s*0.3); g.drawPath(p); break; }
        case Tool::Text: { QFont f("DejaVu Sans", int(s*0.62), QFont::Bold); g.setFont(f); g.setPen(col); g.drawText(QRectF(0,0,s,s),Qt::AlignCenter,"T"); break; }
        case Tool::Fill: { g.drawEllipse(QRectF(m,m,M-m,M-m)); QLinearGradient lg(m,m,M,M); lg.setColorAt(0,col); lg.setColorAt(1,QColor(col.red(),col.green(),col.blue(),40)); g.setBrush(lg); g.setPen(Qt::NoPen); g.drawEllipse(QRectF(m+1,m+1,M-m-2,M-m-2)); break; }
        case Tool::Transparency: { g.setBrush(Qt::NoBrush); for(int i=0;i<3;++i){ QColor cc=col; cc.setAlpha(220-i*70); g.setPen(QPen(cc,1.7)); g.drawEllipse(QPointF(c-s*0.12+i*s*0.12,c),s*0.22,s*0.22);} break; }
        case Tool::Shadow: { g.setBrush(QColor(col.red(),col.green(),col.blue(),70)); g.setPen(Qt::NoPen); g.drawEllipse(QRectF(m+s*0.16,m+s*0.16,M-m-s*0.1,M-m-s*0.1)); g.setBrush(Qt::NoBrush); g.setPen(QPen(col,1.7)); g.drawRect(QRectF(m,m,M-m-s*0.16,M-m-s*0.16)); break; }
        case Tool::Contour: { for(int i=0;i<3;++i){ double e=i*s*0.1; g.drawEllipse(QRectF(m+e,m+e,M-m-2*e,M-m-2*e)); } break; }
        case Tool::Blend: { g.drawEllipse(QRectF(m,c-s*0.12,s*0.3,s*0.3)); g.drawEllipse(QRectF(M-s*0.3,c-s*0.12,s*0.3,s*0.3)); g.drawLine(QPointF(m+s*0.3,c),QPointF(M-s*0.3,c)); break; }
        case Tool::Mould: { QPolygonF q; q<<QPointF(m+s*0.06,m)<<QPointF(M,m+s*0.1)<<QPointF(M-s*0.06,M)<<QPointF(m,M-s*0.1); g.drawPolygon(q);
            g.setBrush(col); for(QPointF v:q){ g.setBrush(Qt::NoBrush); g.drawRect(QRectF(v.x()-2,v.y()-2,4,4)); } break; }
        case Tool::Zoom: { g.drawEllipse(QPointF(c-s*0.06,c-s*0.06),s*0.26,s*0.26); g.drawLine(QPointF(c+s*0.14,c+s*0.14),QPointF(M,M)); g.drawLine(QPointF(c-s*0.2,c-s*0.06),QPointF(c+s*0.08,c-s*0.06)); g.drawLine(QPointF(c-s*0.06,c-s*0.2),QPointF(c-s*0.06,c+s*0.08)); break; }
        case Tool::Pan: { QPainterPath p; p.moveTo(c,m); p.lineTo(c,M); g.drawPath(p); g.drawLine(QPointF(m,c),QPointF(M,c)); break; }
        case Tool::Bevel: { QPolygonF o; o<<QPointF(m,m)<<QPointF(M,m)<<QPointF(M,M)<<QPointF(m,M); g.drawPolygon(o);
            QPolygonF in; in<<QPointF(m+s*0.18,m+s*0.18)<<QPointF(M-s*0.18,m+s*0.18)<<QPointF(M-s*0.18,M-s*0.18)<<QPointF(m+s*0.18,M-s*0.18); g.drawPolygon(in);
            for(int i=0;i<4;++i) g.drawLine(o[i],in[i]); break; }
        case Tool::Feather: { QPainterPath p; p.addEllipse(QRectF(m,m,M-m,M-m)); QRadialGradient rg(QPointF(c,c),(M-m)/2); rg.setColorAt(0,col); rg.setColorAt(1,QColor(col.red(),col.green(),col.blue(),0)); g.setBrush(rg); g.setPen(Qt::NoPen); g.drawPath(p); break; }
        case Tool::Eraser: { QPolygonF q; q<<QPointF(m,M-s*0.06)<<QPointF(c-s*0.04,m+s*0.14)<<QPointF(M-s*0.1,m+s*0.34)<<QPointF(c+s*0.06,M-s*0.06); g.setBrush(QColor(col.red(),col.green(),col.blue(),60)); g.drawPolygon(q); g.drawLine(QPointF(m,M),QPointF(M,M)); break; }
        case Tool::Extrude: { double e=s*0.2;   // a simple cube (front face + depth edges)
            g.drawRect(QRectF(m,m+e,M-m-e,M-m-e));
            g.drawLine(QPointF(m,m+e),QPointF(m+e,m)); g.drawLine(QPointF(M-e,m+e),QPointF(M,m));
            g.drawLine(QPointF(M-e,M),QPointF(M,M-e)); g.drawLine(QPointF(m+e,m),QPointF(M,m)); g.drawLine(QPointF(M,m),QPointF(M,M-e));
            break; }
        case Tool::Clone: {   // a rubber-stamp: handle + head + base
            g.setBrush(Qt::NoBrush);
            g.drawLine(QPointF(c,m+s*0.05),QPointF(c,c+s*0.02));                 // handle
            g.drawRect(QRectF(c-s*0.16,m+s*0.02,s*0.32,s*0.1));                  // grip
            QPolygonF head; head<<QPointF(c-s*0.04,c-s*0.06)<<QPointF(c+s*0.04,c-s*0.06)<<QPointF(c+s*0.16,c+s*0.16)<<QPointF(c-s*0.16,c+s*0.16);
            g.drawPolygon(head);                                                // stamp head (trapezoid)
            g.drawLine(QPointF(m+s*0.06,M-s*0.04),QPointF(M-s*0.06,M-s*0.04));   // base line
            break; }
        case Tool::Table: {   // a 3×3 grid
            g.setBrush(Qt::NoBrush); QRectF rc(m,m+s*0.03,M-m,M-m-s*0.06); g.drawRect(rc);
            for(int i=1;i<3;++i){ double x=rc.left()+rc.width()*i/3.0; g.drawLine(QPointF(x,rc.top()),QPointF(x,rc.bottom())); }
            for(int i=1;i<3;++i){ double y=rc.top()+rc.height()*i/3.0; g.drawLine(QPointF(rc.left(),y),QPointF(rc.right(),y)); }
            break; }
        case Tool::Region: {   // a photo frame with a marching-ants selection region
            g.setBrush(Qt::NoBrush); g.drawRect(QRectF(m,m+s*0.04,M-m,M-m-s*0.08));
            QPen dp(col,1.3); dp.setStyle(Qt::DashLine); g.setPen(dp);
            g.drawEllipse(QRectF(c-s*0.19,c-s*0.15,s*0.38,s*0.30));
            break; }
        }
    }, col);
    return QIcon(pm);
}

QIcon actionIcon(const QString& name, int size, const QColor& col) {
    auto pm = makePix(size, [&](QPainter& g, double s){
        const double c=s*0.5, r=s*0.055;   // corner radius unit for a soft, modern feel
        auto rrect=[&](double x0,double y0,double x1,double y1,double rad){ g.drawRoundedRect(QRectF(x0,y0,x1-x0,y1-y0),rad,rad); };
        if (name=="new") {                 // document with a folded corner
            const double x0=s*0.26,y0=s*0.14,x1=s*0.74,y1=s*0.86,f=s*0.17;
            QPainterPath p; p.moveTo(x0,y0); p.lineTo(x1-f,y0); p.lineTo(x1,y0+f); p.lineTo(x1,y1); p.lineTo(x0,y1); p.closeSubpath();
            g.drawPath(p); g.drawPolyline(QPolygonF({QPointF(x1-f,y0),QPointF(x1-f,y0+f),QPointF(x1,y0+f)})); }
        else if (name=="open"){            // folder
            const double y=s*0.34;
            QPainterPath p; p.moveTo(s*0.16,y); p.lineTo(s*0.16,s*0.82); p.lineTo(s*0.84,s*0.82); p.lineTo(s*0.84,s*0.3);
            p.lineTo(s*0.48,s*0.3); p.lineTo(s*0.4,s*0.22); p.lineTo(s*0.16,s*0.22); p.closeSubpath(); g.drawPath(p); }
        else if (name=="save"){            // floppy disk
            QPainterPath p; p.moveTo(s*0.2,s*0.18); p.lineTo(s*0.66,s*0.18); p.lineTo(s*0.82,s*0.34); p.lineTo(s*0.82,s*0.82);
            p.lineTo(s*0.2,s*0.82); p.closeSubpath(); g.drawPath(p);
            rrect(s*0.34,s*0.18,s*0.62,s*0.36,r);                    // top slider
            rrect(s*0.3,s*0.52,s*0.7,s*0.82,r); }                    // label
        else if (name=="export"){          // tray with an up-arrow (send out)
            g.drawLine(QPointF(c,s*0.62),QPointF(c,s*0.16));
            g.drawPolyline(QPolygonF({QPointF(c-s*0.15,s*0.32),QPointF(c,s*0.15),QPointF(c+s*0.15,s*0.32)}));
            g.drawPolyline(QPolygonF({QPointF(s*0.2,s*0.62),QPointF(s*0.2,s*0.82),QPointF(s*0.8,s*0.82),QPointF(s*0.8,s*0.62)})); }
        else if (name=="undo"){            // curved arrow left
            QPainterPath a; a.moveTo(s*0.22,s*0.5); a.lineTo(s*0.72,s*0.5); a.arcTo(QRectF(s*0.5,s*0.5,s*0.3,s*0.3),90,-180);
            g.drawPath(a);
            g.drawPolyline(QPolygonF({QPointF(s*0.34,s*0.36),QPointF(s*0.18,s*0.5),QPointF(s*0.34,s*0.64)})); }
        else if (name=="redo"){            // curved arrow right (mirror of undo)
            QPainterPath a; a.moveTo(s*0.78,s*0.5); a.lineTo(s*0.28,s*0.5); a.arcTo(QRectF(s*0.2,s*0.5,s*0.3,s*0.3),90,180);
            g.drawPath(a);
            g.drawPolyline(QPolygonF({QPointF(s*0.66,s*0.36),QPointF(s*0.82,s*0.5),QPointF(s*0.66,s*0.64)})); }
        else if (name=="delete"){          // trash can
            g.drawLine(QPointF(s*0.18,s*0.28),QPointF(s*0.82,s*0.28));
            g.drawPolyline(QPolygonF({QPointF(s*0.4,s*0.28),QPointF(s*0.42,s*0.18),QPointF(s*0.58,s*0.18),QPointF(s*0.6,s*0.28)}));
            QPainterPath b; b.moveTo(s*0.24,s*0.28); b.lineTo(s*0.28,s*0.84); b.lineTo(s*0.72,s*0.84); b.lineTo(s*0.76,s*0.28); g.drawPath(b);
            g.drawLine(QPointF(s*0.42,s*0.4),QPointF(s*0.43,s*0.74)); g.drawLine(QPointF(s*0.58,s*0.4),QPointF(s*0.57,s*0.74)); }
        else if (name=="dup"){             // two overlapping rounded squares
            rrect(s*0.16,s*0.16,s*0.6,s*0.6,r); rrect(s*0.4,s*0.4,s*0.84,s*0.84,r); }
        else if (name=="group"){           // dashed bounding box + solid corner handles
            g.setPen(QPen(col,1.3,Qt::DashLine)); rrect(s*0.18,s*0.18,s*0.82,s*0.82,0);
            g.setPen(Qt::NoPen); g.setBrush(col);
            for(QPointF v:{QPointF(s*0.18,s*0.18),QPointF(s*0.82,s*0.18),QPointF(s*0.18,s*0.82),QPointF(s*0.82,s*0.82)})
                g.drawRect(QRectF(v.x()-s*0.06,v.y()-s*0.06,s*0.12,s*0.12)); }
        else { g.drawEllipse(QPointF(c,c),s*0.25,s*0.25); }
    }, col);
    return QIcon(pm);
}

} // namespace xara
