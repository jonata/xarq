#include "trace.h"
#include <QPainterPath>
#include <QLineF>
#include <QtMath>
#include <algorithm>
#include <functional>
#include <vector>
#include <climits>

namespace xara {
namespace {

// ---- median-cut colour quantisation ----
QColor avgBox(const std::vector<QRgb>& px, int lo, int hi) {
    long r=0,g=0,b=0; const int nn=qMax(1,hi-lo);
    for (int i=lo;i<hi;++i){ r+=qRed(px[i]); g+=qGreen(px[i]); b+=qBlue(px[i]); }
    return QColor(int(r/nn), int(g/nn), int(b/nn));
}
QVector<QColor> medianCut(std::vector<QRgb>& px, int nColors) {
    struct Seg { int lo, hi; };
    QVector<Seg> boxes; boxes.push_back({0, int(px.size())});
    if (px.empty()) return { QColor(Qt::black) };
    while (boxes.size() < nColors) {
        int bi=-1, bestRange=-1, bestAxis=0;
        for (int i=0;i<boxes.size();++i) { const Seg& b=boxes[i]; if (b.hi-b.lo < 2) continue;
            int rmin=255,rmax=0,gmin=255,gmax=0,bmin=255,bmax=0;
            for (int j=b.lo;j<b.hi;++j){ int r=qRed(px[j]),g=qGreen(px[j]),bl=qBlue(px[j]);
                rmin=qMin(rmin,r);rmax=qMax(rmax,r);gmin=qMin(gmin,g);gmax=qMax(gmax,g);bmin=qMin(bmin,bl);bmax=qMax(bmax,bl); }
            const int rr=rmax-rmin, gr=gmax-gmin, br=bmax-bmin;
            const int axis = (rr>=gr&&rr>=br)?0:(gr>=br?1:2), range=qMax(rr,qMax(gr,br));
            if (range>bestRange){ bestRange=range; bi=i; bestAxis=axis; } }
        if (bi<0) break;                                          // all boxes uniform → done
        const Seg b=boxes[bi]; const int axis=bestAxis;
        auto av=[axis](QRgb c){ return axis==0?qRed(c):axis==1?qGreen(c):qBlue(c); };
        long sum=0; for (int j=b.lo;j<b.hi;++j) sum+=av(px[j]);
        const int mean=int(sum/qMax(1,b.hi-b.lo));                 // split at the axis MEAN (threshold) — separates bimodal clusters
        auto it=std::partition(px.begin()+b.lo, px.begin()+b.hi, [&](QRgb c){ return av(c)<mean; });
        int mid=int(it-px.begin());
        if (mid<=b.lo || mid>=b.hi) mid=(b.lo+b.hi)/2;             // all-equal box → arbitrary split (harmless duplicate colour)
        boxes[bi]={b.lo,mid}; boxes.push_back({mid,b.hi});
    }
    QVector<QColor> pal; for (const Seg& b : boxes) pal.push_back(avgBox(px,b.lo,b.hi));
    return pal;
}

// ---- Ramer–Douglas–Peucker on a closed polygon ----
double distToSeg(const QPointF& p, const QPointF& a, const QPointF& b) {
    const double dx=b.x()-a.x(), dy=b.y()-a.y(), l2=dx*dx+dy*dy;
    if (l2 < 1e-9) return QLineF(p,a).length();
    double t=((p.x()-a.x())*dx+(p.y()-a.y())*dy)/l2; t=qBound(0.0,t,1.0);
    return QLineF(p, QPointF(a.x()+t*dx, a.y()+t*dy)).length();
}
QPolygonF simplify(QPolygonF poly, double eps) {
    if (poly.size()>1 && poly.first()==poly.last()) poly.removeLast();
    const int n=poly.size(); if (n<4) return poly;
    QVector<bool> keep(n,false); keep[0]=true; keep[n-1]=true;
    std::function<void(int,int)> rec=[&](int lo,int hi){
        double dmax=0; int idx=-1;
        for (int i=lo+1;i<hi;++i){ double d=distToSeg(poly[i],poly[lo],poly[hi]); if (d>dmax){dmax=d;idx=i;} }
        if (dmax>eps && idx>lo){ keep[idx]=true; rec(lo,idx); rec(idx,hi); } };
    rec(0,n-1);
    QPolygonF out; for (int i=0;i<n;++i) if (keep[i]) out<<poly[i]; return out;
}

// ---- append a closed subpath (Catmull-Rom → cubic beziers, or a polyline) ----
void addSubpath(QPainterPath& path, const QPolygonF& p, bool smooth) {
    const int n=p.size(); if (n<3) return;
    path.moveTo(p[0]);
    if (!smooth) { for (int i=1;i<n;++i) path.lineTo(p[i]); path.closeSubpath(); return; }
    for (int i=0;i<n;++i) {
        const QPointF p0=p[(i-1+n)%n], p1=p[i], p2=p[(i+1)%n], p3=p[(i+2)%n];
        path.cubicTo(p1+(p2-p0)/6.0, p2-(p3-p1)/6.0, p2);        // Catmull-Rom spline through p1..p2
    }
    path.closeSubpath();
}

} // namespace

QVector<Shape> traceBitmap(const QImage& src, const TraceOptions& opt) {
    if (src.isNull()) return {};
    QImage img = src.convertToFormat(QImage::Format_RGB32);
    const int oW=img.width(), oH=img.height();
    const int md=qMax(8, opt.maxDim);
    if (qMax(oW,oH) > md) img = img.scaled(md, md, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    const int W=img.width(), H=img.height(); if (W<1||H<1) return {};
    const double sx = double(oW)/W, sy = double(oH)/H;           // downscale → original pixel space

    std::vector<QRgb> px; px.reserve(size_t(W)*H);
    for (int y=0;y<H;++y){ const QRgb* row=reinterpret_cast<const QRgb*>(img.constScanLine(y)); for (int x=0;x<W;++x) px.push_back(row[x]); }
    const QVector<QColor> pal = medianCut(px, qBound(2,opt.colors,64));

    // label every pixel to the nearest palette colour
    QVector<int> label(W*H, 0);
    for (int y=0;y<H;++y){ const QRgb* row=reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x=0;x<W;++x){ const int r=qRed(row[x]),g=qGreen(row[x]),b=qBlue(row[x]);
            int best=0; long bd=LONG_MAX;
            for (int k=0;k<pal.size();++k){ const long dr=r-pal[k].red(),dg=g-pal[k].green(),db=b-pal[k].blue();
                const long d=dr*dr+dg*dg+db*db; if (d<bd){bd=d;best=k;} }
            label[y*W+x]=best; } }

    struct Region { double area; Shape shape; };
    QVector<Region> regions;
    for (int k=0;k<pal.size();++k) {
        long area=0; for (int i=0;i<W*H;++i) if (label[i]==k) ++area;
        if (area*sx*sy < opt.minAreaPx) continue;
        // region outline: union of horizontal runs (fast) → simplified() gives outer + hole loops
        QPainterPath runs;
        for (int y=0;y<H;++y){ int x=0; while (x<W){ if (label[y*W+x]==k){ int x0=x; while (x<W && label[y*W+x]==k) ++x; runs.addRect(x0,y,x-x0,1); } else ++x; } }
        const QList<QPolygonF> loops = runs.simplified().toSubpathPolygons();
        QPainterPath fp;
        for (const QPolygonF& loop : loops){ const QPolygonF s=simplify(loop, opt.epsilon); if (s.size()>=3) addSubpath(fp, s, opt.smooth); }
        if (fp.elementCount()==0) continue;
        fp.setFillRule(Qt::OddEvenFill);
        Shape sh; sh.type=ShapeType::Path; sh.path=fp; sh.name="trace";
        sh.fill.type=FillType::Solid; sh.fill.color=pal[k]; sh.stroke.enabled=false;
        sh.transform=QTransform::fromScale(sx,sy);                // undo the downscale → original pixel coords
        regions.push_back({ double(area), sh });
    }
    std::stable_sort(regions.begin(), regions.end(), [](const Region& a, const Region& b){ return a.area > b.area; });  // background first
    QVector<Shape> out; for (const Region& r : regions) out.push_back(r.shape);
    return out;
}

QVector<QColor> extractPalette(const QImage& src, int colors) {
    if (src.isNull()) return {};
    QImage img = src.convertToFormat(QImage::Format_RGB32);
    const int md=160; if (qMax(img.width(),img.height())>md) img = img.scaled(md,md,Qt::KeepAspectRatio,Qt::SmoothTransformation);
    const int W=img.width(), H=img.height(); if (W<1||H<1) return {};
    std::vector<QRgb> px; px.reserve(size_t(W)*H);
    for (int y=0;y<H;++y){ const QRgb* r=reinterpret_cast<const QRgb*>(img.constScanLine(y)); for(int x=0;x<W;++x) px.push_back(r[x]); }
    const std::vector<QRgb> orig = px;                          // medianCut reorders px in place → keep a copy for frequency
    const QVector<QColor> pal = medianCut(px, qBound(2, colors, 32));
    QVector<int> cnt(pal.size(), 0);
    for (QRgb p : orig){ const int r=qRed(p),g=qGreen(p),b=qBlue(p); int best=0; long bd=LONG_MAX;
        for (int k=0;k<pal.size();++k){ const long dr=r-pal[k].red(),dg=g-pal[k].green(),db=b-pal[k].blue(); const long d=dr*dr+dg*dg+db*db; if(d<bd){bd=d;best=k;} }
        ++cnt[best]; }
    QVector<int> idx; for (int k=0;k<pal.size();++k) idx.push_back(k);
    std::stable_sort(idx.begin(), idx.end(), [&](int a,int b){ return cnt[a]>cnt[b]; });   // dominant colours first
    QVector<QColor> outp; for (int k : idx) if (cnt[k]>0) outp.push_back(pal[k]);
    return outp;
}

} // namespace xara
