#include "photoops.h"
#include <QVector>
#include <QPainter>
#include <QRect>
#include <vector>
#include <algorithm>
#include <cmath>
#include <complex>

namespace xara {
namespace {

// Dual-gradient energy of each pixel (high at edges/detail, low in flat regions).
std::vector<float> energyMap(const QImage& im) {
    const int W=im.width(), H=im.height();
    std::vector<float> e(size_t(W)*H, 0.f);
    auto px=[&](int x,int y){ return im.pixel(qBound(0,x,W-1), qBound(0,y,H-1)); };
    for (int y=0;y<H;++y) for (int x=0;x<W;++x){
        QRgb l=px(x-1,y), r=px(x+1,y), u=px(x,y-1), d=px(x,y+1);
        float dx = std::abs(qRed(l)-qRed(r)) + std::abs(qGreen(l)-qGreen(r)) + std::abs(qBlue(l)-qBlue(r));
        float dy = std::abs(qRed(u)-qRed(d)) + std::abs(qGreen(u)-qGreen(d)) + std::abs(qBlue(u)-qBlue(d));
        e[size_t(y)*W+x] = dx+dy;
    }
    return e;
}

// Remove one minimum-energy vertical seam (image gets one column narrower).
QImage carveOneVerticalSeam(const QImage& im) {
    const int W=im.width(), H=im.height(); if (W<=1) return im;
    std::vector<float> e=energyMap(im);
    std::vector<float> M(size_t(W)*H);
    std::vector<int>   back(size_t(W)*H, 0);
    for (int x=0;x<W;++x) M[x]=e[x];
    for (int y=1;y<H;++y) for (int x=0;x<W;++x){
        float best=M[size_t(y-1)*W+x]; int bx=x;
        if (x>0   && M[size_t(y-1)*W+x-1]<best){ best=M[size_t(y-1)*W+x-1]; bx=x-1; }
        if (x<W-1 && M[size_t(y-1)*W+x+1]<best){ best=M[size_t(y-1)*W+x+1]; bx=x+1; }
        M[size_t(y)*W+x]=e[size_t(y)*W+x]+best; back[size_t(y)*W+x]=bx;
    }
    int minx=0; float minv=M[size_t(H-1)*W];
    for (int x=1;x<W;++x) if (M[size_t(H-1)*W+x]<minv){ minv=M[size_t(H-1)*W+x]; minx=x; }
    std::vector<int> seam(H); seam[H-1]=minx;
    for (int y=H-1;y>0;--y) seam[y-1]=back[size_t(y)*W+seam[y]];
    QImage out(W-1, H, QImage::Format_RGB32);
    for (int y=0;y<H;++y){ int s=seam[y], o=0;
        for (int x=0;x<W;++x){ if (x==s) continue; out.setPixel(o++, y, im.pixel(x,y)); } }
    return out;
}

} // namespace

namespace {
QImage grayDown(const QImage& src, int maxDim) {
    QImage g = src.convertToFormat(QImage::Format_Grayscale8);
    double sc = qMin(1.0, double(maxDim)/qMax(1, qMax(g.width(), g.height())));
    if (sc < 1.0) g = g.scaled(qMax(1,int(g.width()*sc)), qMax(1,int(g.height()*sc)), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    return g;
}
// Best (dx,dy) placing B relative to A (B to the right of A, overlapping), in A's full-res pixels.
// Searches a downsampled grayscale pair by minimising mean abs-difference over the overlap.
QPoint estimateOffset(const QImage& A, const QImage& B) {
    if (A.width()<8 || A.height()<4 || B.width()<2 || B.height()<2) return QPoint(A.width(), 0);   // too small to align: place side by side
    const int MD = 240;
    QImage ga = grayDown(A, MD), gb = grayDown(B, MD);
    const double sc = double(ga.width()) / qMax(1, A.width());
    const int W = ga.width(), H = ga.height(), BW = gb.width(), BH = gb.height();
    int bestDx = -1, bestDy = 0; double bestErr = 1e18;                                            // -1 = no valid match yet
    const int minDx = qMax(1, int(W*0.25)), maxDx = W-2, maxV = qMax(4, H/8);
    // Loop bounds keep x-dx in [0,BW) and y-dy in [0,BH): x1=min(W,dx+BW) and the area-check skip.
    for (int dx = minDx; dx <= maxDx; ++dx) {
        for (int dy = -maxV; dy <= maxV; ++dy) {
            const int x0=dx, x1=qMin(W, dx+BW), y0=qMax(0,dy), y1=qMin(H, dy+BH);
            if (x1-x0 < W*0.08 || y1-y0 < H*0.4) continue;
            double err=0; int cnt=0;
            for (int y=y0; y<y1; y+=2) { const uchar* ra=ga.scanLine(y); const uchar* rb=gb.scanLine(y-dy);
                for (int x=x0; x<x1; x+=2) { err += std::abs(int(ra[x]) - int(rb[x-dx])); ++cnt; } }
            if (cnt < 10) continue; err /= cnt;
            if (err < bestErr) { bestErr=err; bestDx=dx; bestDy=dy; }
        }
    }
    if (bestDx < 0) return QPoint(A.width(), 0);   // no overlap found → place side by side instead of guessing
    return QPoint(qRound(bestDx/qMax(1e-6,sc)), qRound(bestDy/qMax(1e-6,sc)));
}

// Shi-Tomasi corners (min eigenvalue of the 3x3 structure tensor) within roi, non-max suppressed.
QVector<QPoint> detectCorners(const QImage& g, QRect roi, int maxN) {
    const int W=g.width(), H=g.height();
    if (W<8 || H<8) return {};                       // too small for a 3x3 tensor window
    roi = roi.intersected(QRect(2,2,W-5,H-5));
    struct C { int x,y; float s; };
    QVector<C> cs;
    for (int y=roi.top(); y<=roi.bottom(); ++y)
      for (int x=roi.left(); x<=roi.right(); ++x) {
        float sxx=0,syy=0,sxy=0;
        for (int dy=-1;dy<=1;++dy) for (int dx=-1;dx<=1;++dx) {
            const int yy=y+dy, xx=x+dx;
            float gx=(g.scanLine(yy)[xx+1]-g.scanLine(yy)[xx-1])*0.5f;
            float gy=(g.scanLine(yy+1)[xx]-g.scanLine(yy-1)[xx])*0.5f;
            sxx+=gx*gx; syy+=gy*gy; sxy+=gx*gy; }
        float tr=sxx+syy, det=sxx*syy-sxy*sxy, disc=std::sqrt(qMax(0.f,tr*tr-4*det));
        float minEig=(tr-disc)*0.5f;
        if (minEig>2.0f) cs.push_back({x,y,minEig});
      }
    std::sort(cs.begin(),cs.end(),[](const C&a,const C&b){return a.s>b.s;});
    QVector<QPoint> out; const int D=6;
    for (const C&c: cs){ bool ok=true; for (const QPoint&p: out) if(std::abs(p.x()-c.x)<D&&std::abs(p.y()-c.y)<D){ok=false;break;}
        if(ok){ out.push_back(QPoint(c.x,c.y)); if(out.size()>=maxN) break; } }
    return out;
}
// Normalized cross-correlation of (2R+1)^2 patches; returns -1 if out of bounds / flat.
float nccPatch(const QImage& A, QPoint pa, const QImage& B, QPoint pb, int R) {
    double sa=0,sb=0,saa=0,sbb=0,sab=0; int n=0;
    for (int dy=-R;dy<=R;++dy) for (int dx=-R;dx<=R;++dx){
        int ax=pa.x()+dx,ay=pa.y()+dy,bx=pb.x()+dx,by=pb.y()+dy;
        if (ax<0||ay<0||ax>=A.width()||ay>=A.height()||bx<0||by<0||bx>=B.width()||by>=B.height()) return -1;
        double va=A.scanLine(ay)[ax], vb=B.scanLine(by)[bx];
        sa+=va; sb+=vb; saa+=va*va; sbb+=vb*vb; sab+=va*vb; ++n; }
    double na=saa-sa*sa/n, nb=sbb-sb*sb/n; if(na<=1e-6||nb<=1e-6) return -1;
    return float((sab-sa*sb/n)/std::sqrt(na*nb));
}
// Match A-corners to B-corners by NCC with a MUTUAL-BEST cross-check (each is the other's best),
// which eliminates most false matches on ambiguous texture. Returns (a,b) pairs.
QVector<QPair<QPointF,QPointF>> matchCorners(const QImage& gA, const QVector<QPoint>& fa, const QImage& gB, const QVector<QPoint>& fb, int R) {
    const int na=fa.size(), nb=fb.size();
    std::vector<float> S(size_t(na)*nb);
    for (int i=0;i<na;++i) for (int j=0;j<nb;++j) S[size_t(i)*nb+j] = nccPatch(gA,fa[i],gB,fb[j],R);
    std::vector<int> a2b(na,-1), b2a(nb,-1);
    std::vector<float> a2bs(na,-2.f);
    for (int i=0;i<na;++i){ float best=-2; int bi=-1; for(int j=0;j<nb;++j){ float s=S[size_t(i)*nb+j]; if(s>best){best=s;bi=j;} } a2b[i]=bi; a2bs[i]=best; }
    for (int j=0;j<nb;++j){ float best=-2; int bi=-1; for(int i=0;i<na;++i){ float s=S[size_t(i)*nb+j]; if(s>best){best=s;bi=i;} } b2a[j]=bi; }
    QVector<QPair<QPointF,QPointF>> m;
    for (int i=0;i<na;++i){ int j=a2b[i]; if(j>=0 && a2bs[i]>0.8f && b2a[j]==i) m.push_back({QPointF(fa[i]),QPointF(fb[j])}); }
    return m;
}
} // namespace

// RANSAC similarity fit (complex 2-point model + LSQ inlier refit): T maps src -> dst.
QTransform estimateSimilarityTransform(const QVector<QPointF>& src, const QVector<QPointF>& dst, int* inliers) {
    const int n = qMin(src.size(), dst.size());
    if (inliers) *inliers = 0;
    if (n < 2) return QTransform();
    typedef std::complex<double> cx;
    auto S=[&](int i){ return cx(src[i].x(),src[i].y()); };
    auto D=[&](int i){ return cx(dst[i].x(),dst[i].y()); };
    quint32 rng=2463534242u; auto rnd=[&](){ rng^=rng<<13; rng^=rng>>17; rng^=rng<<5; return rng; };   // deterministic xorshift
    const double thr=3.0; int bestIn=0; cx bestZ(1,0), bestW(0,0);
    for (int it=0; it<300; ++it) {
        int i=rnd()%n, j=rnd()%n; if(i==j) continue;
        cx db=S(j)-S(i); if(std::abs(db)<3.0) continue;   // need a real baseline for a stable 2-point similarity
        cx z=(D(j)-D(i))/db, w=D(i)-z*S(i);
        if(std::abs(z)<1e-6||std::abs(z)>1e6) continue;
        int in=0; for(int k=0;k<n;++k) if(std::abs(D(k)-(z*S(k)+w))<thr) ++in;
        if(in>bestIn){ bestIn=in; bestZ=z; bestW=w; }
    }
    if (bestIn>=2) {   // least-squares refit on the inlier set
        cx sbar(0,0),dbar(0,0); int cnt=0; QVector<int> inl;
        for(int k=0;k<n;++k) if(std::abs(D(k)-(bestZ*S(k)+bestW))<thr){ inl.push_back(k); sbar+=S(k); dbar+=D(k); ++cnt; }
        if (cnt>=2){ sbar/=cnt; dbar/=cnt; cx num(0,0); double den=0; double cxx=0,cyy=0,cxy=0;
            for(int k: inl){ num+=(D(k)-dbar)*std::conj(S(k)-sbar); den+=std::norm(S(k)-sbar);
                double dx=S(k).real()-sbar.real(), dy=S(k).imag()-sbar.imag(); cxx+=dx*dx; cyy+=dy*dy; cxy+=dx*dy; }
            if(den>1e-6){ bestZ=num/den; bestW=dbar-bestZ*sbar; }
            // reject a collinear inlier set: the thin-axis spread is degenerate → rotation/scale unstable
            double tr=cxx+cyy, dt=cxx*cyy-cxy*cxy, disc=std::sqrt(qMax(0.0,tr*tr-4*dt)), minEig=(tr-disc)*0.5;
            if(minEig < 4.0*cnt) bestIn = 0;   // spread < ~2px in the thin direction → force the caller to fall back
        }
    }
    if (inliers) *inliers = bestIn;
    const double zr=bestZ.real(), zi=bestZ.imag();
    return QTransform(zr, zi, -zi, zr, bestW.real(), bestW.imag());   // dst = z*src + w
}

namespace {
// Similarity transform mapping B's pixels into A's coordinate frame (rotation+scale+translation).
// Uses corner-feature matching + RANSAC; falls back to a pure-translation SAD offset when weak.
QTransform estimateAlign(const QImage& A, const QImage& B) {
    const QPoint off = estimateOffset(A,B);
    const QTransform tfall = QTransform::fromTranslate(off.x(), off.y());
    if (A.width()<24||A.height()<24||B.width()<24||B.height()<24) return tfall;
    const int MD=480;
    QImage gA=grayDown(A,MD), gB=grayDown(B,MD);
    const double scA=double(gA.width())/qMax(1,A.width()), scB=double(gB.width())/qMax(1,B.width());
    QRect roiA(int(gA.width()*0.40),0, gA.width()-int(gA.width()*0.40), gA.height());   // A's right ~60% (explicit, no OOB)
    QRect roiB(0,0, int(gB.width()*0.60), gB.height());                                 // B's left ~60%
    QVector<QPoint> fa=detectCorners(gA, roiA, 260), fb=detectCorners(gB, roiB, 260);
    if (fa.size()<4 || fb.size()<4) return tfall;
    QVector<QPair<QPointF,QPointF>> mm = matchCorners(gA, fa, gB, fb, 5);
    if (mm.size()<6) return tfall;
    QVector<QPointF> bs, as;    // map B-features -> A-features
    for (const auto& pr: mm){ as.push_back(pr.first); bs.push_back(pr.second); }
    int inl=0; QTransform Tds = estimateSimilarityTransform(bs, as, &inl);   // downsampled: a_ds = Tds(b_ds)
    // 6+ mutually-consistent correspondences to a single similarity is a reliable alignment
    if (inl < 6 || inl < mm.size()*0.15) return tfall;
    // lift to full-res: b_full --(*scB)--> b_ds --(Tds)--> a_ds --(*1/scA)--> a_full
    QTransform T = QTransform::fromScale(scB,scB) * Tds * QTransform::fromScale(1.0/scA, 1.0/scA);
    const double det=T.determinant(); if(det<0.25||det>4.0) return tfall;   // reject wild scale
    return T;
}
} // namespace

QTransform alignImagePair(const QImage& A, const QImage& B) { return estimateAlign(A, B); }

// ---- Burt–Adelson image pyramids (single-channel float), for multi-band blending ----
namespace pyr {
struct Lvl { std::vector<float> d; int w, h; };

std::vector<float> blur5(const std::vector<float>& s, int W, int H) {   // separable 5-tap binomial, replicate borders
    std::vector<float> t(size_t(W)*H), o(size_t(W)*H);
    static const float k[5]={1/16.f,4/16.f,6/16.f,4/16.f,1/16.f};
    for (int y=0;y<H;++y) for (int x=0;x<W;++x){ float v=0; for(int d=-2;d<=2;++d){ int xx=qBound(0,x+d,W-1); v+=s[size_t(y)*W+xx]*k[d+2]; } t[size_t(y)*W+x]=v; }
    for (int y=0;y<H;++y) for (int x=0;x<W;++x){ float v=0; for(int d=-2;d<=2;++d){ int yy=qBound(0,y+d,H-1); v+=t[size_t(yy)*W+x]*k[d+2]; } o[size_t(y)*W+x]=v; }
    return o;
}
Lvl reduce(const Lvl& s) {
    const std::vector<float> b=blur5(s.d,s.w,s.h); Lvl o; o.w=(s.w+1)/2; o.h=(s.h+1)/2; o.d.resize(size_t(o.w)*o.h);
    for (int y=0;y<o.h;++y) for (int x=0;x<o.w;++x) o.d[size_t(y)*o.w+x]=b[size_t(qMin(2*y,s.h-1))*s.w + qMin(2*x,s.w-1)];
    return o;
}
std::vector<float> expand(const Lvl& s, int W, int H) {                 // upsample (zero-insert ×4) then blur
    std::vector<float> up(size_t(W)*H, 0.f);
    for (int y=0;y<s.h;++y) for (int x=0;x<s.w;++x){ int Y=2*y,X=2*x; if(Y<H&&X<W) up[size_t(Y)*W+X]=s.d[size_t(y)*s.w+x]*4.f; }
    return blur5(up,W,H);
}
std::vector<Lvl> gaussian(std::vector<float> base, int W, int H, int levels) {
    std::vector<Lvl> g; g.push_back({std::move(base),W,H});
    for (int i=1;i<levels;++i){ if (g.back().w<2||g.back().h<2) break; g.push_back(reduce(g.back())); }
    return g;
}
std::vector<Lvl> laplacian(const std::vector<Lvl>& g) {                 // L[i]=G[i]-expand(G[i+1]); L[top]=G[top]
    std::vector<Lvl> l(g.size());
    for (int i=0;i<int(g.size());++i){ l[i].w=g[i].w; l[i].h=g[i].h;
        if (i+1<int(g.size())){ const std::vector<float> e=expand(g[i+1],g[i].w,g[i].h); l[i].d.resize(g[i].d.size());
            for (size_t j=0;j<g[i].d.size();++j) l[i].d[j]=g[i].d[j]-e[j]; }
        else l[i].d=g[i].d; }
    return l;
}
std::vector<float> collapse(const std::vector<Lvl>& l) {
    Lvl acc=l.back();
    for (int i=int(l.size())-2;i>=0;--i){ const std::vector<float> e=expand(acc,l[i].w,l[i].h);
        Lvl n; n.w=l[i].w; n.h=l[i].h; n.d.resize(l[i].d.size());
        for (size_t j=0;j<l[i].d.size();++j) n.d[j]=l[i].d[j]+e[j]; acc=std::move(n); }
    return acc.d;
}
int levelsFor(int W, int H, int cap) { int n=1, m=qMin(W,H); while (m>=4 && n<cap){ m/=2; ++n; } return n; }
} // namespace pyr

// Blend two equal-size images with a Laplacian pyramid: `maskA` (grayscale, 255=all A) weights A.
// Low-frequency bands blend over a wide region (seamless exposure), high-frequency bands stay sharp.
QImage multiBandBlend(const QImage& Ain, const QImage& Bin, const QImage& maskAin, int levels) {
    if (Ain.isNull() || Bin.isNull()) return Ain;
    const int W=Ain.width(), H=Ain.height();
    if (Bin.width()!=W || Bin.height()!=H || W<2 || H<2) return Ain;
    const QImage A=Ain.convertToFormat(QImage::Format_ARGB32), B=Bin.convertToFormat(QImage::Format_ARGB32);
    const QImage M=maskAin.convertToFormat(QImage::Format_Grayscale8).scaled(W,H);
    const int nl = qBound(1, levels, pyr::levelsFor(W,H,10));

    std::vector<float> mf(size_t(W)*H);
    for (int y=0;y<H;++y){ const uchar* mr=M.constScanLine(y); for(int x=0;x<W;++x) mf[size_t(y)*W+x]=mr[x]/255.f; }
    const std::vector<pyr::Lvl> gm = pyr::gaussian(mf, W, H, nl);

    QImage out(W,H,QImage::Format_ARGB32);
    std::vector<std::vector<float>> chan(3, std::vector<float>(size_t(W)*H));
    for (int c=0;c<3;++c){
        std::vector<float> a(size_t(W)*H), b(size_t(W)*H);
        for (int y=0;y<H;++y){ const QRgb* ar=(const QRgb*)A.constScanLine(y); const QRgb* br=(const QRgb*)B.constScanLine(y);
            for (int x=0;x<W;++x){ const size_t j=size_t(y)*W+x; const int sh=(c==0?16:c==1?8:0);
                a[j]=((ar[x]>>sh)&0xff); b[j]=((br[x]>>sh)&0xff); } }
        const std::vector<pyr::Lvl> la=pyr::laplacian(pyr::gaussian(std::move(a),W,H,nl));
        const std::vector<pyr::Lvl> lb=pyr::laplacian(pyr::gaussian(std::move(b),W,H,nl));
        std::vector<pyr::Lvl> blend(la.size());
        for (int i=0;i<int(la.size());++i){ blend[i].w=la[i].w; blend[i].h=la[i].h; blend[i].d.resize(la[i].d.size());
            for (size_t j=0;j<la[i].d.size();++j){ const float g=gm[i].d[j]; blend[i].d[j]=la[i].d[j]*g + lb[i].d[j]*(1.f-g); } }
        chan[c]=pyr::collapse(blend);
    }
    for (int y=0;y<H;++y){ QRgb* row=(QRgb*)out.scanLine(y);
        for (int x=0;x<W;++x){ const size_t j=size_t(y)*W+x;
            row[x]=qRgba(qBound(0,int(chan[0][j]+0.5f),255), qBound(0,int(chan[1][j]+0.5f),255), qBound(0,int(chan[2][j]+0.5f),255), 255); } }
    return out;
}

// N-image multi-band composite (Burt-Adelson mosaicing): per-band weighted average of each image's
// Laplacian pyramid, weighted by its Gaussian-pyramid feather weight. cbufs = warped ARGB on the union
// canvas, wbufs = feather weights. Generalises multiBandBlend to any image count; seamless + sharp.
static QImage multiBandCompositeN(const QVector<QImage>& cbufs, const QVector<QImage>& wbufs, int W, int H, int levels) {
    const int nI = cbufs.size(); if (nI==0) return QImage();
    const int nl = qBound(1, levels, pyr::levelsFor(W,H,8));
    const std::vector<pyr::Lvl> ref = pyr::gaussian(std::vector<float>(size_t(W)*H,0.f), W, H, nl);   // zero pyramid → level dims
    const int L = int(ref.size());
    std::vector<pyr::Lvl> accL[3] = { ref, ref, ref }, accW = ref;
    std::vector<float> cover(size_t(W)*H, 0.f);
    for (int i=0;i<nI;++i){
        std::vector<float> wf(size_t(W)*H);
        for (int y=0;y<H;++y){ const uchar* wr=wbufs[i].constScanLine(y); const QRgb* cr=(const QRgb*)cbufs[i].constScanLine(y);
            for (int x=0;x<W;++x){ const size_t j=size_t(y)*W+x; const float w=(qAlpha(cr[x])>=8)?(wr[x]+1.f):0.f; wf[j]=w; cover[j]+=w; } }
        const std::vector<pyr::Lvl> gw = pyr::gaussian(wf, W, H, nl);
        for (int l=0;l<L;++l) for (size_t j=0;j<accW[l].d.size();++j) accW[l].d[j]+=gw[l].d[j];
        for (int c=0;c<3;++c){ std::vector<float> ch(size_t(W)*H); const int sh=(c==0?16:c==1?8:0);
            for (int y=0;y<H;++y){ const QRgb* cr=(const QRgb*)cbufs[i].constScanLine(y);
                for (int x=0;x<W;++x) ch[size_t(y)*W+x]=float((cr[x]>>sh)&0xff); }
            const std::vector<pyr::Lvl> lap = pyr::laplacian(pyr::gaussian(std::move(ch), W, H, nl));
            for (int l=0;l<L;++l) for (size_t j=0;j<accL[c][l].d.size();++j) accL[c][l].d[j]+=lap[l].d[j]*gw[l].d[j]; }
    }
    std::vector<float> chan[3];
    for (int c=0;c<3;++c){ std::vector<pyr::Lvl> b=accL[c];
        for (int l=0;l<L;++l) for (size_t j=0;j<b[l].d.size();++j){ const float w=accW[l].d[j]; b[l].d[j] = (w>1e-6f)? b[l].d[j]/w : 0.f; }
        chan[c]=pyr::collapse(b); }
    QImage out(W,H,QImage::Format_ARGB32);
    for (int y=0;y<H;++y){ QRgb* row=(QRgb*)out.scanLine(y);
        for (int x=0;x<W;++x){ const size_t j=size_t(y)*W+x;
            row[x] = (cover[j]>0.f) ? qRgba(qBound(0,int(chan[0][j]+0.5f),255), qBound(0,int(chan[1][j]+0.5f),255), qBound(0,int(chan[2][j]+0.5f),255), 255) : 0u; } }
    return out;
}

QImage stitchPanorama(const QVector<QImage>& imgs) {
    if (imgs.isEmpty()) return QImage();
    if (imgs.size() == 1) return imgs[0];
    QVector<QTransform> xf; xf.push_back(QTransform());                       // frame 0 = identity
    for (int i=1;i<imgs.size();++i){ QTransform rel=estimateAlign(imgs[i-1], imgs[i]); xf.push_back(rel * xf[i-1]); }  // imgs[i] -> frame 0
    QRectF bounds;
    for (int i=0;i<imgs.size();++i){ QRectF r(0,0,imgs[i].width(),imgs[i].height()); QRectF mr=xf[i].mapRect(r); bounds = i==0?mr:bounds.united(mr); }
    if (!bounds.isValid()) return imgs[0];
    const int W=int(std::ceil(bounds.width())), H=int(std::ceil(bounds.height()));
    if (W<1 || H<1 || (qint64)W*H > 2e7) return imgs[0];
    const QTransform toCanvas = QTransform::fromTranslate(-bounds.left(), -bounds.top());
    QVector<QImage> cbufs, wbufs;                                             // each image + its feather weight, warped to the union canvas
    for (int i=0;i<imgs.size();++i){
        QImage im=imgs[i].convertToFormat(QImage::Format_ARGB32);
        const int iw=im.width(), ih=im.height();
        QImage wimg(iw,ih,QImage::Format_Grayscale8);                          // feather weight: distance to edge
        for (int y=0;y<ih;++y){ uchar* wr=wimg.scanLine(y); for(int x=0;x<iw;++x) wr[x]=uchar(qMin(255,qMin(qMin(x+1,iw-x),qMin(y+1,ih-y)))); }
        const QTransform T = xf[i]*toCanvas;
        QImage cbuf(W,H,QImage::Format_ARGB32); cbuf.fill(Qt::transparent);
        QImage wbuf(W,H,QImage::Format_Grayscale8); wbuf.fill(0);
        { QPainter p(&cbuf); p.setRenderHint(QPainter::SmoothPixmapTransform); p.setTransform(T); p.drawImage(0,0,im); }
        { QPainter p(&wbuf); p.setRenderHint(QPainter::SmoothPixmapTransform); p.setTransform(T); p.drawImage(0,0,wimg); }
        cbufs.push_back(cbuf); wbufs.push_back(wbuf);
    }

    // Exposure/gain compensation (Brown-Lowe): equalise per-image brightness before blending, so
    // auto-exposure differences between frames don't leave a visible step across the seam.
    {
        const int nI = cbufs.size();
        QVector<QVector<double>> Iij(nI, QVector<double>(nI,0.0)), Nij(nI, QVector<double>(nI,0.0));
        for (int i=0;i<nI;++i) for (int j=i+1;j<nI;++j){
            double si=0, sj=0; long cnt=0;
            for (int y=0;y<H;++y){ const QRgb* ri=(const QRgb*)cbufs[i].constScanLine(y); const QRgb* rj=(const QRgb*)cbufs[j].constScanLine(y);
                for (int x=0;x<W;++x) if (qAlpha(ri[x])>=8 && qAlpha(rj[x])>=8){
                    si += 0.299*qRed(ri[x])+0.587*qGreen(ri[x])+0.114*qBlue(ri[x]);
                    sj += 0.299*qRed(rj[x])+0.587*qGreen(rj[x])+0.114*qBlue(rj[x]); ++cnt; } }
            if (cnt>0){ Iij[i][j]=si/cnt; Iij[j][i]=sj/cnt; Nij[i][j]=Nij[j][i]=double(cnt); }
        }
        const QVector<double> gains = estimateGains(nI, Iij, Nij);
        for (int i=0;i<nI;++i){ const double gi=gains[i]; if (qAbs(gi-1.0)<0.01) continue;
            QImage& cb=cbufs[i];
            for (int y=0;y<H;++y){ QRgb* r=(QRgb*)cb.scanLine(y);
                for (int x=0;x<W;++x){ if(qAlpha(r[x])<8) continue;
                    r[x]=qRgba(qBound(0,int(qRed(r[x])*gi+0.5),255), qBound(0,int(qGreen(r[x])*gi+0.5),255),
                               qBound(0,int(qBlue(r[x])*gi+0.5),255), qAlpha(r[x])); } } }
    }

    // Multi-band (Laplacian-pyramid) blend for the common 2-image case on a bounded canvas:
    // low-frequency bands cross-fade over a wide region (exposure seams vanish), high frequencies stay sharp.
    if ((qint64)W*H <= 6000000)                                              // any image count → seamless N-image multi-band
        return multiBandCompositeN(cbufs, wbufs, W, H, pyr::levelsFor(W,H,7));

    // Fallback (huge canvas): flat feather-weighted average.
    std::vector<float> acc(size_t(W)*H*3, 0.f), wsum(size_t(W)*H, 0.f);
    for (int i=0;i<cbufs.size();++i){
        for (int y=0;y<H;++y){ const QRgb* cr=(const QRgb*)cbufs[i].scanLine(y); const uchar* wr=wbufs[i].scanLine(y);
            for (int x=0;x<W;++x){ QRgb c=cr[x]; int a=qAlpha(c); if(a<8) continue;
                float wgt=(wr[x]+1.0f)*(a/255.0f); size_t idx=size_t(y)*W+x;
                acc[idx*3]+=qRed(c)*wgt; acc[idx*3+1]+=qGreen(c)*wgt; acc[idx*3+2]+=qBlue(c)*wgt; wsum[idx]+=wgt; } }
    }
    QImage out(W,H,QImage::Format_ARGB32); out.fill(Qt::transparent);
    for (int y=0;y<H;++y){ QRgb* row=(QRgb*)out.scanLine(y);
        for (int x=0;x<W;++x){ size_t idx=size_t(y)*W+x; float w=wsum[idx]; if(w<=0) continue;
            row[x]=qRgba(qBound(0,int(acc[idx*3]/w+0.5),255), qBound(0,int(acc[idx*3+1]/w+0.5),255), qBound(0,int(acc[idx*3+2]/w+0.5),255), 255); } }
    return out;
}

// ---- destructive photo filters ----
namespace {
QImage boxBlurOnce(const QImage& in, int r) {                        // separable running-sum box blur, replicate borders
    const int W=in.width(), H=in.height(); if(W<1||H<1) return in;
    const int win=2*r+1;
    QImage tmp(W,H,QImage::Format_ARGB32), out(W,H,QImage::Format_ARGB32);
    for (int y=0;y<H;++y){ const QRgb* s=(const QRgb*)in.constScanLine(y); QRgb* d=(QRgb*)tmp.scanLine(y);
        long sr=0,sg=0,sb=0,sa=0;
        for (int k=-r;k<=r;++k){ QRgb p=s[qBound(0,k,W-1)]; sr+=qRed(p);sg+=qGreen(p);sb+=qBlue(p);sa+=qAlpha(p); }
        for (int x=0;x<W;++x){ d[x]=qRgba(int(sr/win),int(sg/win),int(sb/win),int(sa/win));
            QRgb po=s[qBound(0,x-r,W-1)], pn=s[qBound(0,x+r+1,W-1)];
            sr+=qRed(pn)-qRed(po); sg+=qGreen(pn)-qGreen(po); sb+=qBlue(pn)-qBlue(po); sa+=qAlpha(pn)-qAlpha(po); } }
    for (int x=0;x<W;++x){ long sr=0,sg=0,sb=0,sa=0;
        auto at=[&](int yy){ return ((const QRgb*)tmp.constScanLine(qBound(0,yy,H-1)))[x]; };
        for (int k=-r;k<=r;++k){ QRgb p=at(k); sr+=qRed(p);sg+=qGreen(p);sb+=qBlue(p);sa+=qAlpha(p); }
        for (int y=0;y<H;++y){ ((QRgb*)out.scanLine(y))[x]=qRgba(int(sr/win),int(sg/win),int(sb/win),int(sa/win));
            QRgb po=at(y-r), pn=at(y+r+1);
            sr+=qRed(pn)-qRed(po); sg+=qGreen(pn)-qGreen(po); sb+=qBlue(pn)-qBlue(po); sa+=qAlpha(pn)-qAlpha(po); } }
    return out;
}
} // namespace

QVector<double> estimateGains(int n, const QVector<QVector<double>>& Iij, const QVector<QVector<double>>& Nij) {
    QVector<double> g(qMax(0,n), 1.0);
    if (n<=1) return g;
    // Brown-Lowe: σN=10 intensity, σg=0.1 gain. The data term carries a factor of 2 because Nij is
    // symmetric — each overlap pair contributes as both (i,j) and (j,i) — so ∂E/∂g_i doubles the data
    // part but NOT the prior; folding the 2 into invN2 keeps the intended data:prior balance.
    const double invN2 = 2.0/(10.0*10.0), invG2 = 1.0/(0.1*0.1);
    std::vector<std::vector<double>> A(n, std::vector<double>(n,0.0)); std::vector<double> b(n,0.0);
    for (int i=0;i<n;++i){ double sumNi=0;
        for (int j=0;j<n;++j){ if(j==i) continue; const double Nij_=Nij[i][j]; if(Nij_<=0) continue;
            sumNi += Nij_;
            A[i][i] += Nij_ * Iij[i][j]*Iij[i][j] * invN2;
            A[i][j] -= Nij_ * Iij[i][j]*Iij[j][i] * invN2; }
        A[i][i] += sumNi*invG2; b[i] = sumNi*invG2; }
    for (int c=0;c<n;++c){                                          // Gaussian elimination, partial pivoting
        int piv=c; for(int r=c+1;r<n;++r) if(std::abs(A[r][c])>std::abs(A[piv][c])) piv=r;
        if (std::abs(A[piv][c])<1e-12) continue;                    // singular → leave those gains at 1
        std::swap(A[c],A[piv]); std::swap(b[c],b[piv]);
        for (int r=0;r<n;++r){ if(r==c) continue; const double f=A[r][c]/A[c][c];
            for(int k=c;k<n;++k) A[r][k]-=f*A[c][k]; b[r]-=f*b[c]; } }
    for (int i=0;i<n;++i) g[i] = (std::abs(A[i][i])>1e-12) ? qBound(0.3, b[i]/A[i][i], 3.0) : 1.0;
    return g;
}

QImage gaussianBlurImage(const QImage& src, double radius) {
    if (src.isNull() || radius < 0.5) return src;
    QImage im = src.convertToFormat(QImage::Format_ARGB32);
    const int r = qBound(1, int(radius+0.5), 128);
    for (int pass=0; pass<3; ++pass) im = boxBlurOnce(im, r);        // 3 box passes ≈ a Gaussian
    return im;
}

QImage unsharpMask(const QImage& src, double amount, double radius) {
    if (src.isNull() || amount <= 0) return src;
    const QImage s = src.convertToFormat(QImage::Format_ARGB32), b = gaussianBlurImage(s, radius);
    const int W=s.width(), H=s.height(); QImage out(W,H,QImage::Format_ARGB32);
    for (int y=0;y<H;++y){ const QRgb* sr=(const QRgb*)s.constScanLine(y); const QRgb* br=(const QRgb*)b.constScanLine(y); QRgb* o=(QRgb*)out.scanLine(y);
        for (int x=0;x<W;++x){ QRgb p=sr[x], q=br[x];               // out = src + amount·(src − blur)
            o[x]=qRgba(qBound(0,int(qRed(p)  +amount*(qRed(p)  -qRed(q))  +0.5),255),
                       qBound(0,int(qGreen(p)+amount*(qGreen(p)-qGreen(q))+0.5),255),
                       qBound(0,int(qBlue(p) +amount*(qBlue(p) -qBlue(q)) +0.5),255), qAlpha(p)); } }
    return out;
}

QImage vignette(const QImage& src, double strength) {
    if (src.isNull() || strength <= 0) return src;
    QImage s = src.convertToFormat(QImage::Format_ARGB32);
    const int W=s.width(), H=s.height(); const double cx=W/2.0, cy=H/2.0, maxd=std::hypot(cx,cy);
    for (int y=0;y<H;++y){ QRgb* r=(QRgb*)s.scanLine(y);
        for (int x=0;x<W;++x){ const double d=std::hypot(x-cx,y-cy)/qMax(1.0,maxd);
            const double f=qBound(0.0, 1.0 - strength*d*d, 1.0);     // quadratic falloff from centre
            QRgb p=r[x]; r[x]=qRgba(int(qRed(p)*f), int(qGreen(p)*f), int(qBlue(p)*f), qAlpha(p)); } }
    return s;
}

QImage lensCorrect(const QImage& src, double k1, double k2) {
    // Brown radial-distortion correction. For each output pixel we compute its normalised radius
    // (relative to the half-diagonal) and sample the source at the *distorted* radius r·(1+k1r²+k2r⁴),
    // so lines the lens bowed become straight. k>0 corrects barrel, k<0 corrects pincushion.
    if (src.isNull() || (qAbs(k1)<1e-6 && qAbs(k2)<1e-6)) return src;
    const QImage s = src.convertToFormat(QImage::Format_ARGB32);
    const int W=s.width(), H=s.height();
    QImage out(W,H,QImage::Format_ARGB32); out.fill(Qt::transparent);
    const double cx=(W-1)/2.0, cy=(H-1)/2.0, diag=std::hypot(cx,cy), norm= diag>1e-9 ? 1.0/diag : 0.0;
    for (int y=0;y<H;++y){ QRgb* o=(QRgb*)out.scanLine(y);
        for (int x=0;x<W;++x){
            const double dx=(x-cx)*norm, dy=(y-cy)*norm, r2=dx*dx+dy*dy;
            const double f=1.0 + k1*r2 + k2*r2*r2;             // corrected → distorted-source mapping
            const double sxf=cx + (x-cx)*f, syf=cy + (y-cy)*f;
            const int x0=(int)std::floor(sxf), y0=(int)std::floor(syf);
            if (x0<0||y0<0||x0+1>=W||y0+1>=H){                 // outside the source: leave transparent (nearest if just inside)
                if (x0>=0&&y0>=0&&x0<W&&y0<H) o[x]=((const QRgb*)s.constScanLine(y0))[x0]; continue; }
            const double fx=sxf-x0, fy=syf-y0;
            const QRgb* r0=(const QRgb*)s.constScanLine(y0); const QRgb* r1=(const QRgb*)s.constScanLine(y0+1);
            const QRgb a=r0[x0],b=r0[x0+1],c=r1[x0],d=r1[x0+1];
            auto lerp=[&](int ca,int cb,int cc,int cd){ const double t=ca+(cb-ca)*fx, u=cc+(cd-cc)*fx; return (int)(t+(u-t)*fy+0.5); };
            o[x]=qRgba(lerp(qRed(a),qRed(b),qRed(c),qRed(d)),   lerp(qGreen(a),qGreen(b),qGreen(c),qGreen(d)),
                       lerp(qBlue(a),qBlue(b),qBlue(c),qBlue(d)), lerp(qAlpha(a),qAlpha(b),qAlpha(c),qAlpha(d))); } }
    return out;
}

QImage tonemapHDR(const QImage& src, double exposure, double white) {
    // Extended Reinhard tone-map: compresses highlights and lifts shadows to give a single LDR photo
    // an "HDR" look. `exposure` scales input luminance (1=neutral); `white` (>0) is the luminance that
    // maps to pure white — larger is gentler. Colour ratios are preserved by scaling RGB uniformly.
    if (src.isNull()) return src;
    const QImage s = src.convertToFormat(QImage::Format_ARGB32);
    const int W=s.width(), H=s.height(); QImage out(W,H,QImage::Format_ARGB32);
    const double w2 = qMax(0.01, white*white);
    for (int y=0;y<H;++y){ const QRgb* sr=(const QRgb*)s.constScanLine(y); QRgb* o=(QRgb*)out.scanLine(y);
        for (int x=0;x<W;++x){ const QRgb p=sr[x];
            const double R=qRed(p)/255.0, G=qGreen(p)/255.0, B=qBlue(p)/255.0;
            const double L=0.2126*R+0.7152*G+0.0722*B;         // relative luminance
            const double Le=L*exposure, Lt=Le*(1.0+Le/w2)/(1.0+Le);
            const double scale = L>1e-6 ? Lt/L : 0.0;          // preserve hue: scale all channels by ΔL
            o[x]=qRgba(qBound(0,(int)(R*scale*255+0.5),255), qBound(0,(int)(G*scale*255+0.5),255),
                       qBound(0,(int)(B*scale*255+0.5),255), qAlpha(p)); } }
    return out;
}

QImage contentAwareResize(const QImage& src, int targetW, int targetH) {
    if (src.isNull()) return src;
    QImage im = src.convertToFormat(QImage::Format_RGB32);
    targetW = qBound(1, targetW, im.width());
    targetH = qBound(1, targetH, im.height());
    while (im.width()  > targetW) im = carveOneVerticalSeam(im);
    if (im.height() > targetH) {                       // carve horizontal seams via transpose
        im = im.transformed(QTransform().rotate(90));  // rotate 90: rows<->cols
        while (im.width() > targetH) im = carveOneVerticalSeam(im);
        im = im.transformed(QTransform().rotate(-90));
    }
    return im;
}

} // namespace xara
