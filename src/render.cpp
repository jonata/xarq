#include "render.h"
#include <cmath>
#include <QPainter>
#include <QImage>
#include <QPainterPathStroker>
#include <QtMath>

namespace xara {

// Outline (wireframe) view mode: when set, paintShape draws each object's outline only, no fills or
// effects. It's a screen-only view aid — the canvas sets it around its paintEvent; --shot/PDF/PNG export
// never touches it, so on-disk renders stay byte-identical.
static bool g_outlineView = false;
void setOutlineRenderMode(bool on){ g_outlineView = on; }
static bool g_screenRender = false;   // true only while painting to the on-screen canvas (never during export/print)
void setScreenRenderMode(bool on){ g_screenRender = on; }
// Viewport culling (canvas only): skip shapes wholly outside the visible doc rect. The canvas sets this
// per frame and clears it afterwards, so export / thumbnails (which never set it) always render everything.
static QRectF g_cullRect;   // invalid ⇒ culling disabled
void setRenderCullRect(const QRectF& r){ g_cullRect = r; }

// Visible viewport rect (in the painter's DEVICE space — the widget in logical px) for clipping EFFECT buffers
// (blur/feather/bevel/noise/grad-transp/bitmap-mask). Each of those rasterises the shape into a device buffer
// sized to the shape's FULL device bounds; at high zoom that buffer (and its per-pixel work) explodes and every
// zoom step re-renders → the UI freezes. Clipping the buffer to the visible viewport (grown by the effect's
// spread) makes the cost O(screen), not O(shape). This is stored in the SAME space the effect functions compute
// their devB in (world-transform mapped, i.e. viewXf·groupXf applied to doc coords) — which for the canvas is
// always the widget's logical-pixel rect, INDEPENDENT of any group transform on the shape. (An earlier version
// stored a doc-space rect and re-mapped it through the per-shape painter transform; inside a transformed group
// that displaced the clip and dropped visible effect pixels.) Set on the canvas paint path only; invalid on
// export/thumbnail ⇒ effects render at full extent (so those outputs stay byte-identical). See clipEffectToViewport().
static QRectF g_viewportRect;   // invalid ⇒ no clip (render full extent)
void setRenderViewport(const QRectF& r){ g_viewportRect = r; }

// Intersect an effect's device-space bounds with the visible viewport, grown by `margin` device px so a
// blur/feather sampling just past the screen edge still has its source pixels. g_viewportRect is already in the
// same device space as devB (the widget's logical-pixel rect), so NO per-shape transform is applied here — that
// keeps the clip correct for shapes nested inside a transformed group. No-op when g_viewportRect is unset
// (export) so exported/thumbnail renders are unchanged.
static inline QRect clipEffectToViewport(const QRect& devB, int margin){
    if(!g_viewportRect.isValid()) return devB;
    return devB.intersected(g_viewportRect.toRect().adjusted(-margin,-margin,margin,margin));
}
// Conservative upper bound (doc px) on how far a shape's rendering can extend beyond its geometry bounds
// via effects — used as the cull margin. Over-estimating only reduces culling; under-estimating would
// drop visible pixels, so every extent-adding effect is included and padded.
static double effectReach(const Shape& s){
    double r = 2.0;
    if (s.stroke.enabled)  r = qMax(r, s.stroke.width * 0.5 + 2.0);
    if (s.shadow.on)       r = qMax(r, std::hypot(s.shadow.offset.x(), s.shadow.offset.y()) + s.shadow.blur * 3.0 + 4.0);
    if (s.contour.on && s.contour.width > 0) r = qMax(r, s.contour.width + 4.0);
    if (s.extrude.on)      r = qMax(r, std::abs(s.extrude.depth) * 2.0 + 4.0);
    r = qMax(r, s.feather * 3.0);
    r = qMax(r, s.blur * 3.0);
    return r + 8.0;
}
double shapeRenderReach(const Shape& s){ return effectReach(s); }
static inline bool cullShape(const Shape& s){
    if (!g_cullRect.isValid()) return false;
    const double m = qMax(256.0, effectReach(s));   // generous floor covers any small effect not itemised above
    return !s.docBounds().adjusted(-m, -m, m, m).intersects(g_cullRect);
}

// Drag-proxy shape filters (canvas only). Address-keyed so no re-indexing is needed. Both empty in every
// export/print/thumbnail render ⇒ renderFiltered() short-circuits to false ⇒ byte-identical output.
static QSet<const void*> g_skipSet;   // draw everything except these
static QSet<const void*> g_onlySet;   // if non-empty, draw ONLY these
void setRenderSkipSet(const QSet<const void*>& s){ g_skipSet = s; }
void setRenderOnlySet(const QSet<const void*>& s){ g_onlySet = s; }
static inline bool renderFiltered(const Shape& s){
    if (!g_skipSet.isEmpty() && g_skipSet.contains(&s)) return true;
    if (!g_onlySet.isEmpty() && !g_onlySet.contains(&s)) return true;
    return false;
}

// Apply a fill's colour stops to a gradient, plus its repeat-spread. A RAINBOW fill-effect cycles
// each adjacent stop pair through the HSV hue wheel: short way (rainbow) or long way (alt-rainbow),
// chosen exactly as Xara's engine (Kernel/gradtbl.cpp BuildPaletteInternalHSV): the wrap is decided
// by the simple hue distance, and S/V are interpolated per step so achromatic ends stay grey.
static double profileBiasGain(double t, double biasP, double gainP);   // fwd (defined below)
// Interpolate the fill's stop colours at parameter u∈[0,1] (stops assumed sorted by pos).
static QColor colorAtStops(const QVector<GradStop>& st, double u){
    if(st.isEmpty()) return Qt::black;
    if(u<=st.first().pos) return st.first().color;
    if(u>=st.last().pos)  return st.last().color;
    for(int i=0;i+1<st.size();++i){ if(u>=st[i].pos && u<=st[i+1].pos){
        double span=st[i+1].pos-st[i].pos; double w = span>1e-9? (u-st[i].pos)/span : 0.0;
        const QColor&a=st[i].color;const QColor&b=st[i+1].color;
        return QColor::fromRgbF(a.redF()+(b.redF()-a.redF())*w, a.greenF()+(b.greenF()-a.greenF())*w,
                                a.blueF()+(b.blueF()-a.blueF())*w, a.alphaF()+(b.alphaF()-a.alphaF())*w); } }
    return st.last().color;
}
static void applyStops(QGradient& g, const Fill& f) {
    g.setSpread(f.spread);
    // Profile (bias/gain): resample the graduation through the Schlick curve. Default (0,0) falls through
    // to the plain stop path below, so a profile-less gradient is byte-identical.
    if (!f.rainbow && (f.profileBias!=0.0 || f.profileGain!=0.0) && f.stops.size()>=2) {
        const int N=24;
        for(int i=0;i<=N;++i){ double t=double(i)/N; g.setColorAt(t, colorAtStops(f.stops, profileBiasGain(t, f.profileBias, f.profileGain))); }
        return;
    }
    if (f.rainbow && f.stops.size() >= 2) {
        auto rainbowSegment = [&](const QColor& a, double posA, const QColor& b, double posB) {
            double h0 = a.hueF(), h1 = b.hueF();                 // -1 for achromatic colours
            const double s0 = a.hsvSaturationF(), s1 = b.hsvSaturationF();
            const double v0 = a.valueF(),         v1 = b.valueF();
            if (h0 < 0) h0 = (h1 < 0 ? 0.0 : h1);                // borrow the other end's hue (S stays 0 -> grey)
            if (h1 < 0) h1 = h0;
            const double dist = std::fabs(h0 - h1);             // wrap: normal if dist<=0.5 for rainbow / >0.5 for alt
            const bool blendNormally = (dist <= 0.5) ? !f.rainbowAlt : f.rainbowAlt;
            double sh0 = h0, sh1 = h1;
            if (!blendNormally) { if (sh0 > sh1) sh1 += 1.0; else sh0 += 1.0; }   // go the long way round
            const int N = 16;
            for (int i = 0; i <= N; ++i) {
                const double u = double(i)/N;
                double h = sh0 + (sh1 - sh0)*u; if (h > 1.0) h -= 1.0;
                const double s = s0 + (s1 - s0)*u, v = v0 + (v1 - v0)*u;
                const double t = posA + (posB - posA)*u;
                g.setColorAt(qBound(0.0, t, 1.0), QColor::fromHsvF(qBound(0.0,h,1.0), qBound(0.0,s,1.0), qBound(0.0,v,1.0)));
            }
        };
        for (int i = 0; i+1 < f.stops.size(); ++i)
            rainbowSegment(f.stops[i].color, f.stops[i].pos, f.stops[i+1].color, f.stops[i+1].pos);
    } else {
        for (auto& s : f.stops) g.setColorAt(qBound(0.0, s.pos, 1.0), s.color);
    }
}

QBrush fillBrush(const Fill& f, const QRectF& lb) {
    switch (f.type) {
    case FillType::None:  return Qt::NoBrush;
    case FillType::Solid: return QBrush(f.color);
    case FillType::Linear: { QLinearGradient g(f.p0, f.p1); applyStops(g, f); return QBrush(g); }
    case FillType::Radial: {
        double r = QLineF(f.p0, f.p1).length();
        QRadialGradient g(f.p0, qMax(r, 0.001)); applyStops(g, f); return QBrush(g);
    }
    case FillType::Conical: {
        double ang = -QLineF(f.p0, f.p1).angle();
        QConicalGradient g(f.p0, ang); applyStops(g, f); return QBrush(g);
    }
    case FillType::Bitmap: case FillType::Mesh: case FillType::Square: break;   // handled in paintShape (texture brush)
    }
    Q_UNUSED(lb); return Qt::NoBrush;
}

// Affine that maps the bitmap's pixel rectangle onto the fill parallelogram (local space).
// p0 = origin, p1 = origin + X-axis, p2 = origin + Y-axis. The image is y-down (row 0 at top)
// while the parallelogram's Y-axis (p2-p0) points the doc "up" way, so the image's BOTTOM edge
// sits at p0 and its TOP edge at p2 (i.e. image (0,0) -> p2, (0,H) -> p0, (W,0) -> p2+X-axis).
static QTransform bitmapTexXf(const Fill& f, QSize sz) {
    const double W = qMax(1, sz.width()), H = qMax(1, sz.height());
    const QPointF ax = (f.p1 - f.p0) / W;     // per-pixel step along image +X
    const QPointF ay = (f.p0 - f.p2) / H;     // per-pixel step along image +Y (top->bottom = p2->p0)
    return QTransform(ax.x(), ax.y(), ay.x(), ay.y(), f.p2.x(), f.p2.y());
}

// 3-/4-colour mesh fill rendered as a small image, sampled bilinearly by the parallelogram map.
// Corners (matching bitmapTexXf): image bottom-left -> p0=c0, bottom-right -> p1=c1, top-left ->
// p2=c2, top-right -> far=c3. A 3-colour fill is planar: its implied 4th corner is c1+c2-c0.
static QImage makeMeshImage(const QVector<QColor>& m) {
    auto at = [&](int i, const QColor& def){ return i < m.size() ? m[i] : def; };
    const QColor c0 = at(0, Qt::black), c1 = at(1, c0), c2 = at(2, c0);
    const QColor c3 = (m.size() >= 4) ? m[3]
        : QColor(qBound(0, c1.red()  +c2.red()  -c0.red(),  255),
                 qBound(0, c1.green()+c2.green()-c0.green(),255),
                 qBound(0, c1.blue() +c2.blue() -c0.blue(), 255));
    const int N = 64;
    QImage img(N, N, QImage::Format_ARGB32);
    for (int y = 0; y < N; ++y) {
        QRgb* line = reinterpret_cast<QRgb*>(img.scanLine(y));
        const double b = 1.0 - double(y)/(N-1);          // 0 at bottom (p0/p1), 1 at top (p2/far)
        for (int x = 0; x < N; ++x) {
            const double a = double(x)/(N-1);            // 0 at left (p0/p2), 1 at right (p1/far)
            auto bl = [&](int v0,int v1,int v2,int v3){ return int((1-a)*(1-b)*v0 + a*(1-b)*v1 + (1-a)*b*v2 + a*b*v3 + 0.5); };
            line[x] = qRgba(bl(c0.red(),c1.red(),c2.red(),c3.red()),
                            bl(c0.green(),c1.green(),c2.green(),c3.green()),
                            bl(c0.blue(),c1.blue(),c2.blue(),c3.blue()), 255);
        }
    }
    return img;
}

// Square/diamond fill: concentric L-infinity contours, colour c0 at the centre fading to c1 at
// the edges. Mapped onto the full square (centre +/- both axes) by the caller.
// Arbitrary R×C mesh gradient: bilinear interpolation across a grid of colour nodes (row-major in `m`,
// row 0 = TOP, col 0 = left — the image's (0,0) maps to the fill parallelogram origin p2). rows,cols >= 2.
static QImage makeMeshGridImage(const QVector<QColor>& m, int rows, int cols) {
    rows = qMax(2, rows); cols = qMax(2, cols);
    auto node = [&](int r,int c)->QColor{ int i=r*cols+c; return (i>=0 && i<m.size()) ? m[i] : Qt::black; };
    const int N = 128;
    QImage img(N, N, QImage::Format_ARGB32);
    for (int y = 0; y < N; ++y) {
        QRgb* line = reinterpret_cast<QRgb*>(img.scanLine(y));
        double fr = (double(y)/(N-1))*(rows-1); int r0 = qBound(0, int(fr), rows-2); double ty = fr - r0;
        for (int x = 0; x < N; ++x) {
            const double a = double(x)/(N-1);
            double fc = a*(cols-1); int c0 = qBound(0, int(fc), cols-2); double tx = fc - c0;
            const QColor q00=node(r0,c0), q01=node(r0,c0+1), q10=node(r0+1,c0), q11=node(r0+1,c0+1);
            auto bl=[&](int a0,int a1,int b0,int b1){ return int((1-ty)*((1-tx)*a0+tx*a1) + ty*((1-tx)*b0+tx*b1) + 0.5); };
            line[x] = qRgba(bl(q00.red(),q01.red(),q10.red(),q11.red()),
                            bl(q00.green(),q01.green(),q10.green(),q11.green()),
                            bl(q00.blue(),q01.blue(),q10.blue(),q11.blue()), 255);
        }
    }
    return img;
}

static QImage makeSquareImage(const QColor& c0, const QColor& c1) {
    const int N = 64;
    QImage img(N, N, QImage::Format_ARGB32);
    for (int y = 0; y < N; ++y) {
        QRgb* line = reinterpret_cast<QRgb*>(img.scanLine(y));
        const double cv = 2.0*y/(N-1) - 1.0;
        for (int x = 0; x < N; ++x) {
            const double cu = 2.0*x/(N-1) - 1.0;
            const double t = qBound(0.0, qMax(qAbs(cu), qAbs(cv)), 1.0);
            line[x] = qRgba(int(c0.red()  +(c1.red()  -c0.red())  *t + 0.5),
                            int(c0.green()+(c1.green()-c0.green())*t + 0.5),
                            int(c0.blue() +(c1.blue() -c0.blue()) *t + 0.5), 255);
        }
    }
    return img;
}

// Contone: recolour a (greyscale) bitmap so luminance 0 -> c0 and 255 -> c1, keeping alpha.
static QImage contoneTint(const QImage& src, const QColor& c0, const QColor& c1) {
    QImage img = src.convertToFormat(QImage::Format_ARGB32);
    const int w = img.width(), h = img.height();
    for (int y = 0; y < h; ++y) {
        QRgb* line = reinterpret_cast<QRgb*>(img.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const QRgb p = line[x]; const double t = qGray(p) / 255.0;
            line[x] = qRgba(int(c0.red()  +(c1.red()  -c0.red()  )*t),
                            int(c0.green()+(c1.green()-c0.green())*t),
                            int(c0.blue() +(c1.blue() -c0.blue() )*t), qAlpha(p));
        }
    }
    return img;
}

// Separable 3-pass box blur approximating a gaussian of total radius `rad` px, over an
// ARGB image including its alpha. Three passes spread content by ~3*per, so we use
// per = rad/3 to make `rad` the actual blur radius — callers must leave a margin >= rad
// of transparent pixels around the content, else the blur tail clips to a hard edge.
static void boxBlur(QImage& img, int rad) {
    rad = qBound(0, rad, 64); if (rad < 1) return;
    const int per = qMax(1, (rad + 2) / 3);
    const int w = img.width(), h = img.height();
    for (int pass = 0; pass < 3; ++pass) {
        QImage tmp = img;
        for (int y=0;y<h;++y){ const QRgb* src=(const QRgb*)tmp.constScanLine(y); QRgb* dst=(QRgb*)img.scanLine(y);
            for (int x=0;x<w;++x){ int a=0,r=0,gg=0,b=0,n=0;
                for (int k=-per;k<=per;++k){ int xx=x+k; if(xx<0||xx>=w)continue; QRgb px=src[xx]; a+=qAlpha(px); r+=qRed(px); gg+=qGreen(px); b+=qBlue(px); ++n; }
                dst[x]=qRgba(r/n,gg/n,b/n,a/n); } }
        tmp = img;
        for (int x=0;x<w;++x) for (int y=0;y<h;++y){ int a=0,r=0,gg=0,b=0,n=0;
            for (int k=-per;k<=per;++k){ int yy=y+k; if(yy<0||yy>=h)continue; QRgb px=((const QRgb*)tmp.constScanLine(yy))[x]; a+=qAlpha(px); r+=qRed(px); gg+=qGreen(px); b+=qBlue(px); ++n; }
            ((QRgb*)img.scanLine(y))[x]=qRgba(r/n,gg/n,b/n,a/n); }
    }
}

// True separable gaussian blur of total radius `rad` px over a premultiplied-ARGB image (alpha + colour).
// Smoother, more accurate falloff than the 3-pass box approximation — used for feathered edges. Edges
// are clamped (the caller leaves a transparent margin >= rad, so this is just replicating transparent).
// One separable box-blur pass (radius br) with clamp-to-edge, via a running sum → O(W·H), INDEPENDENT of radius.
// Operates on ARGB32 (premultiplied for effect buffers). Horizontal then vertical.
static void boxBlurPass(QImage& img, int br) {
    if (br < 1) return;
    const int w = img.width(), h = img.height();
    const int win = 2*br + 1;
    QImage tmp = img;
    // horizontal
    for (int y=0; y<h; ++y) {
        const QRgb* src=(const QRgb*)tmp.constScanLine(y); QRgb* dst=(QRgb*)img.scanLine(y);
        int sa=0,sr=0,sg=0,sb=0;
        for (int k=-br;k<=br;++k){ QRgb p=src[qBound(0,k,w-1)]; sa+=qAlpha(p);sr+=qRed(p);sg+=qGreen(p);sb+=qBlue(p); }
        for (int x=0;x<w;++x){
            dst[x]=qRgba(sr/win,sg/win,sb/win,sa/win);
            QRgb pin=src[qBound(0,x+br+1,w-1)], pout=src[qBound(0,x-br,w-1)];
            sa+=qAlpha(pin)-qAlpha(pout); sr+=qRed(pin)-qRed(pout); sg+=qGreen(pin)-qGreen(pout); sb+=qBlue(pin)-qBlue(pout);
        }
    }
    tmp = img;
    // vertical
    for (int x=0; x<w; ++x) {
        int sa=0,sr=0,sg=0,sb=0;
        auto at=[&](int yy)->QRgb{ return ((const QRgb*)tmp.constScanLine(qBound(0,yy,h-1)))[x]; };
        for (int k=-br;k<=br;++k){ QRgb p=at(k); sa+=qAlpha(p);sr+=qRed(p);sg+=qGreen(p);sb+=qBlue(p); }
        for (int y=0;y<h;++y){
            ((QRgb*)img.scanLine(y))[x]=qRgba(sr/win,sg/win,sb/win,sa/win);
            QRgb pin=at(y+br+1), pout=at(y-br);
            sa+=qAlpha(pin)-qAlpha(pout); sr+=qRed(pin)-qRed(pout); sg+=qGreen(pin)-qGreen(pout); sb+=qBlue(pin)-qBlue(pout);
        }
    }
}
// Three box passes ≈ a gaussian (central-limit) of the same variance, at O(W·H) regardless of radius. Used ONLY
// on the interactive canvas for big blurs — the exact gaussian's O(W·H·radius) cost explodes at high zoom (the
// radius scales with zoom up to the 200px cap), which is what makes zooming into a blurred object slow.
static void boxBlur3(QImage& img, int rad) {
    const double sigma = qMax(0.5, rad/2.0);                 // match gaussBlur's spread (sigma = rad/2)
    const int br = qMax(1, (int)std::lround((std::sqrt(1.0 + 4.0*sigma*sigma) - 1.0) / 2.0));   // 3 boxes, variance br²+br = sigma²
    boxBlurPass(img, br); boxBlurPass(img, br); boxBlurPass(img, br);
}

static void gaussBlur(QImage& img, int rad, int maxRad = 96) {
    rad = qBound(0, rad, maxRad); if (rad < 1) return;
    // Interactive canvas + a big blur: use the fast box-blur approximation (radius-independent). Export/print
    // (g_screenRender false) always take the exact gaussian below, so exported/corpus renders are byte-identical.
    if (g_screenRender && rad > 24) { boxBlur3(img, rad); return; }
    const double sigma = qMax(0.5, rad/2.0);   // Xara's feather spread is ~rad/2 (wider than the rad/3 first cut)
    const int K = qMin(rad, (int)std::ceil(sigma*3.0));
    QVector<float> ker(2*K+1); double sum=0;
    for (int i=-K;i<=K;++i){ double v=std::exp(-(double)(i*i)/(2.0*sigma*sigma)); ker[i+K]=(float)v; sum+=v; }
    for (float& v : ker) v=(float)(v/sum);
    const int w = img.width(), h = img.height();
    QImage tmp = img;                                   // horizontal pass: tmp -> img
    for (int y=0;y<h;++y){ const QRgb* src=(const QRgb*)tmp.constScanLine(y); QRgb* dst=(QRgb*)img.scanLine(y);
        for (int x=0;x<w;++x){ float a=0,r=0,g=0,b=0;
            for (int k=-K;k<=K;++k){ int xx=qBound(0,x+k,w-1); QRgb p=src[xx]; float wt=ker[k+K];
                a+=qAlpha(p)*wt; r+=qRed(p)*wt; g+=qGreen(p)*wt; b+=qBlue(p)*wt; }
            dst[x]=qRgba((int)(r+0.5f),(int)(g+0.5f),(int)(b+0.5f),(int)(a+0.5f)); } }
    tmp = img;                                          // vertical pass: tmp -> img
    for (int x=0;x<w;++x) for (int y=0;y<h;++y){ float a=0,r=0,g=0,b=0;
        for (int k=-K;k<=K;++k){ int yy=qBound(0,y+k,h-1); QRgb p=((const QRgb*)tmp.constScanLine(yy))[x]; float wt=ker[k+K];
            a+=qAlpha(p)*wt; r+=qRed(p)*wt; g+=qGreen(p)*wt; b+=qBlue(p)*wt; }
        ((QRgb*)img.scanLine(y))[x]=qRgba((int)(r+0.5f),(int)(g+0.5f),(int)(b+0.5f),(int)(a+0.5f)); }
}

// Render a blurred drop shadow of a document-space path into the painter.
// 3-D extrude: project the outline back along the extrusion vector, drawing the back face,
// then shaded side walls (lit by `e.light`), then the caller paints the front face on top.
// True-3-D tilt (rotX/rotY): the front-face in-plane 2x2 map and the projected z (depth) offset,
// both about the shape centre. Orthographic projection, screen y-down. See G-G derivation.
static QTransform extrudeFrontMap(const Extrude& e, const QPointF& c) {
    const double a = e.rotX*M_PI/180.0, b = e.rotY*M_PI/180.0;
    // (x,y,0) -> X = x cosβ ; Y = x sinβ sinα + y cosα  (orthographic; screen y-down)
    const QTransform m(std::cos(b), std::sin(b)*std::sin(a),   // m11, m12
                       0.0,         std::cos(a),                // m21, m22
                       0.0, 0.0);
    // Apply the 2x2 map about the shape centre c: translate(-c) * m * translate(+c).
    return QTransform::fromTranslate(-c.x(),-c.y()) * m * QTransform::fromTranslate(c.x(),c.y());
}
static QPointF extrudeDepthOffset(const Extrude& e) {
    const double a = e.rotX*M_PI/180.0, b = e.rotY*M_PI/180.0, d = e.depth;
    return QPointF(-d*std::sin(b), d*std::cos(b)*std::sin(a));   // projection of (0,0,-d)
}
// True only when paintExtrude actually paints the (rotated) front face itself — i.e. a 3-D tilt WITH a
// positive depth. At depth<=0 paintExtrude draws nothing, so the caller must NOT skip the normal fill.
static bool extrudeDrawsFront(const Extrude& e){ return e.on && e.is3D() && e.depth>0.0; }

// Shade + draw the extruded side walls and back face for outline `face`, offset by `off`.
static void paintExtrudeWalls(QPainter& g, const QPainterPath& face, const QPointF& off, const QColor& base, const Extrude& e) {
    g.setPen(Qt::NoPen); g.setBrush(base.darker(190)); g.drawPath(face.translated(off));   // back face
    const double la=e.light*M_PI/180.0; const QPointF L(std::cos(la), std::sin(la));
    for (const QPolygonF& poly : face.toSubpathPolygons()) {
        const int n=poly.size(); if (n<2) continue;
        for (int i=0;i<n;++i) {
            QPointF p0=poly[i], p1=poly[(i+1)%n], edge=p1-p0;
            double len=std::hypot(edge.x(),edge.y()); if (len<1e-6) continue;
            QPointF nrm(edge.y()/len, -edge.x()/len);
            double lit = 0.5 + 0.5*(nrm.x()*L.x()+nrm.y()*L.y());   // 0..1
            QColor wc = base.darker(int(130 + (1.0-lit)*110));
            QPolygonF quad; quad << p0 << p1 << (p1+off) << (p0+off);
            g.setBrush(wc); g.setPen(QPen(wc,0.6)); g.drawPolygon(quad);
        }
    }
}
static void paintExtrude(QPainter& g, const QPainterPath& dp, const Fill& fill, const Extrude& e) {
    if (e.depth <= 0) return;
    const QColor base = e.color.isValid() ? e.color : fill.representativeColor();   // explicit wall colour, else the fill
    g.save(); g.setRenderHint(QPainter::Antialiasing);
    if (e.is3D()) {
        // Orthographic 3-D solid: rotate the front outline, draw back + walls, then a flat-shaded front
        // face (loses gradient/bitmap detail under strong tilt — an accepted 3-D limitation).
        const QPointF c = dp.boundingRect().center();
        const QTransform fm = extrudeFrontMap(e, c);
        const QPainterPath fp = fm.map(dp);
        const QPointF off = extrudeDepthOffset(e);
        paintExtrudeWalls(g, fp, off, base, e);
        if (e.bevel > 0.0) {                              // chamfer rim: a slightly darker inset ring
            g.setPen(Qt::NoPen); g.setBrush(base.darker(120)); g.drawPath(fp);
            const double s = qMax(0.0, 1.0 - 2.0*e.bevel/qMax(1.0,fp.boundingRect().width()));
            QTransform ins = QTransform::fromTranslate(c.x(),c.y()); ins.scale(s,s); ins.translate(-c.x(),-c.y());
            g.setBrush(base); g.drawPath(ins.map(fp));
        } else { g.setPen(Qt::NoPen); g.setBrush(base); g.drawPath(fp); }
        g.restore(); return;
    }
    const double a = e.angle*M_PI/180.0;
    const QPointF off(e.depth*std::cos(a), e.depth*std::sin(a));    // doc-space extrusion offset (classic path)
    paintExtrudeWalls(g, dp, off, base, e);
    g.restore();
}

static void paintShadow(QPainter& g, const QPainterPath& dp, const Shadow& sh) {
    if (sh.type==2) {   // GLOW: no offset, symmetric coloured halo
        const double rad=qMax(4.0, sh.blur);
        QRectF br=dp.boundingRect().adjusted(-rad-6,-rad-6,rad+6,rad+6);
        if(br.width()<1||br.height()<1||br.width()*br.height()>8e6) return;
        QImage img(br.size().toSize(),QImage::Format_ARGB32_Premultiplied); img.fill(Qt::transparent);
        { QPainter p(&img); p.setRenderHint(QPainter::Antialiasing); p.translate(-br.topLeft()); p.fillPath(dp, sh.color); }
        boxBlur(img,int(rad)); g.drawImage(br.topLeft(),img); return;
    }
    if (sh.type==1) {   // FLOOR: squashed, sheared ground shadow anchored at the object's baseline, fading up
        const QRectF b=dp.boundingRect(); const double baseY=b.bottom();
        QTransform t; t.translate(sh.offset.x(),baseY); t.scale(1.0,sh.floorScale); t.shear(sh.floorShear,0.0); t.translate(0,-baseY);
        const QPainterPath fp=t.map(dp);
        QRectF br=fp.boundingRect().adjusted(-sh.blur-6,-sh.blur-6,sh.blur+6,sh.blur+6);
        if(br.width()<1||br.height()<1||br.width()*br.height()>8e6) return;
        QImage img(br.size().toSize(),QImage::Format_ARGB32_Premultiplied); img.fill(Qt::transparent);
        { QPainter p(&img); p.setRenderHint(QPainter::Antialiasing); p.translate(-br.topLeft()); p.fillPath(fp, sh.color);
          QLinearGradient fade(0, baseY-br.top(), 0, b.top()-br.top());     // opaque at base -> transparent far
          fade.setColorAt(0.0, QColor(0,0,0,255)); fade.setColorAt(1.0, QColor(0,0,0,0));
          p.setCompositionMode(QPainter::CompositionMode_DestinationIn); p.fillRect(img.rect(), fade); }
        boxBlur(img,int(sh.blur)); g.drawImage(br.topLeft(),img); return;
    }
    QRectF br = dp.boundingRect().translated(sh.offset).adjusted(-sh.blur-4,-sh.blur-4,sh.blur+4,sh.blur+4);
    if (br.width() < 1 || br.height() < 1 || br.width()*br.height() > 8e6) return;
    QImage img(br.size().toSize(), QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    { QPainter p(&img); p.setRenderHint(QPainter::Antialiasing);
      p.translate(-br.topLeft() + sh.offset);
      p.fillPath(dp, sh.color); }
    boxBlur(img, int(sh.blur));
    g.drawImage(br.topLeft(), img);
}

// Stepped contour rings inset (width<0) or outset (width>0) of a doc-space path.
static void paintContour(QPainter& g, const QPainterPath& dp, const Contour& c) {
    if (!c.on || c.steps < 1) return;
    for (int i = c.steps; i >= 1; --i) {
        double w = c.width * i / c.steps;
        QPainterPathStroker st; st.setWidth(qAbs(w)*2); st.setJoinStyle(Qt::RoundJoin);
        QPainterPath ring = st.createStroke(dp);
        QColor col;
        if (c.color2.isValid()) {                                 // graduate inner(color) → outer(color2)
            const double f = c.steps>1 ? double(i-1)/(c.steps-1) : 0.0;   // 0 = innermost, 1 = outermost
            col = QColor::fromRgbF(c.color.redF()+(c.color2.redF()-c.color.redF())*f,
                                   c.color.greenF()+(c.color2.greenF()-c.color.greenF())*f,
                                   c.color.blueF()+(c.color2.blueF()-c.color.blueF())*f,
                                   c.color.alphaF()+(c.color2.alphaF()-c.color.alphaF())*f);
        } else {
            col = c.color; col.setAlphaF(col.alphaF() * (1.0 - 0.4*(i-1.0)/qMax(1,c.steps)));   // classic fade (byte-identical)
        }
        if (c.transp > 0.001) col.setAlphaF(col.alphaF() * qBound(0.0, 1.0 - c.transp, 1.0));   // independent contour transparency (default 0 = no change)
        g.fillPath(ring, col);
    }
}

// Inward distance transform: for each pixel inside the alpha mask, the approximate distance (px) to the
// nearest outside pixel; 0 outside. Two-pass chamfer with 8-connectivity weights (1, √2).
static void insideDistanceTransform(const QImage& mask, QVector<float>& dist, int W, int H) {
    const float INF = 1e9f, d1 = 1.0f, d2 = 1.41421356f;
    dist.resize(W*H);
    for (int y=0;y<H;++y){ const QRgb* m=(const QRgb*)mask.constScanLine(y);
        for (int x=0;x<W;++x) dist[y*W+x] = (qAlpha(m[x])>=128) ? INF : 0.0f; }
    for (int y=0;y<H;++y) for (int x=0;x<W;++x){ float& v=dist[y*W+x]; if (v==0) continue;
        if (x>0)            v=qMin(v, dist[y*W+x-1]+d1);
        if (y>0){           v=qMin(v, dist[(y-1)*W+x]+d1);
            if (x>0)        v=qMin(v, dist[(y-1)*W+x-1]+d2);
            if (x<W-1)      v=qMin(v, dist[(y-1)*W+x+1]+d2); } }
    for (int y=H-1;y>=0;--y) for (int x=W-1;x>=0;--x){ float& v=dist[y*W+x]; if (v==0) continue;
        if (x<W-1)          v=qMin(v, dist[y*W+x+1]+d1);
        if (y<H-1){         v=qMin(v, dist[(y+1)*W+x]+d1);
            if (x<W-1)      v=qMin(v, dist[(y+1)*W+x+1]+d2);
            if (x>0)        v=qMin(v, dist[(y+1)*W+x-1]+d2); } }
}

// True 3-D lit bevel. Render the shape silhouette, distance-transform it to a height ramp across the bevel
// band, derive a per-pixel surface normal (the distance gradient tilted by the profile slope), light it with
// the directional light (azimuth + elevation from BEVEL 4052), and composite the highlight/shadow. Replaces
// the old flat sheen/rim fake. Strengths follow Kernel nodebev.cpp (highlight linear / shadow quadratic in
// contrast); inner bevels of the flip-types are lit from the opposite side (beveler.cpp aFlipLightAngle).
static void paintBevel(QPainter& g, const QPainterPath& dp, const Bevel& b) {
    if (!b.on) return;
    const QRectF docB = dp.boundingRect(); if (docB.isEmpty()) return;
    const QTransform xf = g.transform();                       // doc -> device
    const double scale = std::sqrt(qAbs(xf.determinant()));
    const double depthPx = qMax(1.5, b.depth*scale);
    QRect devB = xf.mapRect(docB).toRect().adjusted(-2,-2,2,2);
    // margin ≥ the bevel's device reach; clamp in the DOUBLE domain before the int cast (a file-derived depthPx can be
    // huge/non-finite via an unclamped JSON bevel depth × a large group scale → int() would be out-of-range UB).
    const int bevMargin = (int)qBound(0.0, depthPx*2.0 + 4.0, 1e6);
    devB = clipEffectToViewport(devB, bevMargin);   // high-zoom: bound the effect buffer to the visible viewport
    const int W=devB.width(), H=devB.height();
    if (W<3 || H<3 || (qint64)W*H > (qint64)24e6) return;       // skip degenerate / enormous shapes
    // silhouette mask in device space
    QImage mask(W,H,QImage::Format_ARGB32_Premultiplied); mask.fill(Qt::transparent);
    { QPainter mp(&mask); mp.setRenderHint(QPainter::Antialiasing);
      mp.translate(-devB.topLeft()); mp.setTransform(xf,true); mp.fillPath(dp, Qt::white); }
    QVector<float> dist; insideDistanceTransform(mask, dist, W, H);
    // soften the chamfer ridges (medial-axis artifacts) so the normals don't streak: one 3×3 box pass over
    // the interior, averaging only inside neighbours to keep the edge crisp.
    { QVector<float> src=dist;
      for (int y=0;y<H;++y) for (int x=0;x<W;++x){ if (src[y*W+x]<=0.0f) continue;
          float sum=0; int n=0;
          for (int dy=-1;dy<=1;++dy){ int yy=y+dy; if(yy<0||yy>=H) continue;
              for (int dx=-1;dx<=1;++dx){ int xx=x+dx; if(xx<0||xx>=W) continue;
                  float v=src[yy*W+xx]; if(v>0.0f){ sum+=v; ++n; } } }
          if (n>0) dist[y*W+x]=sum/n; } }
    // light vector (screen space: x right, y down, z toward viewer). Same azimuth convention as the old dir.
    double ang = b.light*M_PI/180.0;
    auto flipType=[](int t){ return t==1||t==2||t==3||t==8||t==9||t==10||t==11||t==12; };
    if (!b.outer && flipType(b.type)) ang += M_PI;
    const double elev = qBound(2.0, 90.0-b.tilt, 88.0)*M_PI/180.0;
    const double Lx=qCos(elev)*qCos(ang), Ly=-qCos(elev)*qSin(ang), Lz=qSin(elev), base=Lz;
    const double light01 = qBound(0.0, b.contrast*2.5599900/255.0, 1.0);
    const double dark01  = qBound(0.0, b.contrast*(double)b.contrast*0.0255999/255.0, 1.0);
    const int lr=b.lightColor.red(), lg=b.lightColor.green(), lb=b.lightColor.blue();  // highlight tint (white=255 => neutral)
    const double theta0 = 55.0*M_PI/180.0;                      // nominal bevel slope angle
    auto slopeAngle=[&](double t)->double{                      // profile: edge(t=0) -> flat top(t=1)
        switch(b.type){ case 1: case 2: return theta0*qCos(t*M_PI/2.0);   // round/half-round: steep edge
                        case 6: case 7: return theta0*(1.0-t)*(1.0-t);    // smooth: eased to flat
                        default:        return theta0*(1.0-0.15*t); } };  // flat & others: near-constant
    QImage overlay(W,H,QImage::Format_ARGB32_Premultiplied); overlay.fill(Qt::transparent);
    for (int y=0;y<H;++y){
        QRgb* o=(QRgb*)overlay.scanLine(y); const QRgb* m=(const QRgb*)mask.constScanLine(y);
        for (int x=0;x<W;++x){
            const int ma=qAlpha(m[x]); if (ma<8) continue;
            const float d=dist[y*W+x]; if (d<=0.0f || d>=depthPx) continue;   // outside or flat top
            const double t=d/depthPx;
            // 3×3 Sobel gradient of the distance field (smoother than a 2-px difference -> less chamfer streaking)
            auto D=[&](int xx,int yy)->float{ xx=qBound(0,xx,W-1); yy=qBound(0,yy,H-1); return dist[yy*W+xx]; };
            const float dxv=(D(x+1,y-1)+2*D(x+1,y)+D(x+1,y+1)-D(x-1,y-1)-2*D(x-1,y)-D(x-1,y+1))*0.125f;
            const float dyv=(D(x-1,y+1)+2*D(x,y+1)+D(x+1,y+1)-D(x-1,y-1)-2*D(x,y-1)-D(x+1,y-1))*0.125f;
            const double gl=std::hypot(dxv,dyv); if (gl<1e-3) continue;
            const double gx=dxv/gl, gy=dyv/gl;                  // unit inward direction
            const double th=slopeAngle(t), s=qSin(th), c=qCos(th);
            const double Nx=-gx*s, Ny=-gy*s, Nz=c;              // normal tilts downhill (outward)
            const double delta=(Nx*Lx+Ny*Ly+Nz*Lz)-base;       // shade relative to the flat top
            const double aHi=qBound(0.0, delta*light01*1.8, 1.0), aLo=qBound(0.0, -delta*dark01*1.8, 1.0);
            const double fade=ma/255.0;
            if (aHi>=aLo){ int a=(int)qRound(aHi*205.0*fade); if(a>0) o[x]=qRgba(lr*a/255, lg*a/255, lb*a/255, a); }  // light colour, premult (white => a,a,a,a)
            else         { int a=(int)qRound(aLo*205.0*fade); if(a>0) o[x]=qRgba(0,0,0,a); }           // black, premult
        }
    }
    g.save(); g.resetTransform(); g.drawImage(devB.topLeft(), overlay); g.restore();
}

// Map a Xara transparency type (Kernel/fillval.h) to the nearest separable QPainter blend mode.
// Mix=1, Bleach=2, Stainedglass=3, Additive=10, Subtractive=11, Contrast=13, Darken=19,
// Lighten=22, Brightness=25. The non-separable HSL types — Saturation=16, Luminosity=28, Hue=31 —
// have no Qt equivalent and are composited per-pixel (see isHslBlend / paintHslBlend).
static QPainter::CompositionMode xaraBlend(int type) {
    switch (type) {
    case 2:  return QPainter::CompositionMode_Screen;     // Bleach
    case 3:  return QPainter::CompositionMode_Multiply;   // Stained glass
    case 10: return QPainter::CompositionMode_Plus;       // Additive
    case 11: return QPainter::CompositionMode_Difference; // Subtractive (approx)
    case 13: return QPainter::CompositionMode_HardLight;  // Contrast
    case 19: return QPainter::CompositionMode_Darken;     // Darken
    case 22: return QPainter::CompositionMode_Lighten;    // Lighten
    case 25: return QPainter::CompositionMode_Screen;     // Brightness (lighten approx)
    default: return QPainter::CompositionMode_SourceOver; // Mix(1) / HSL (handled per-pixel) / unsupported
    }
}
static inline bool isHslBlend(int type) { return type==16 || type==28 || type==31 || (type>=40 && type<=53); }  // Saturation / Luminosity / Hue / Colour + imported modes Qt has no composition for

static void paintFeathered(QPainter& g, const Shape& s);
static void paintBlurred(QPainter& g, const Shape& s);
static void paintNoiseMasked(QPainter& g, const Shape& s);
static void paintBitmapMasked(QPainter& g, const Shape& s);
static void paintIsolatedGroup(QPainter& g, const Shape& s);
static void compositeHsl(QPainter& g, const QImage& src, const QRect& devB, int type, double strength);
static void paintGradTranspFill(QPainter& g, const Shape& s);
static void paintBrush(QPainter& g, const QPainterPath& dp, const Stroke& st);
static void paintVarWidth(QPainter& g, const QPainterPath& dp, const Stroke& st);
static void paintVectorStroke(QPainter& g, const QPainterPath& dp, const Stroke& st);

// Brush for a graduated-transparency gradient (used as fill alpha or as a DestinationIn mask), in the
// transparency's own coordinate space (p0=centre/start). For an ELLIPTICAL radial transparency the
// gradient is a unit circle mapped onto the (p1,p2) axes via the brush transform, so anisotropic skies
// fade as an ellipse instead of collapsing to a circle (which over-darkened wedges between light rays).
// `extra` post-composes any shape local->doc transform (identity when p0/p1/p2 are already doc-space).
static QBrush transpGradBrush(const Transparency& t, const QColor& c0, const QColor& c1, const QTransform& extra) {
    // Endpoints c0..c1 carry the same RGB, alpha = 1-t0 / 1-t1. Multi-stage: add each intermediate
    // stop at its position with the same RGB and alpha = 1-stop.t. Empty stops => the classic 2-stop
    // ramp (byte-identical for XAR-loaded transparency, which never carries stops).
    auto ramp = [&](QGradient& gr){
        // Non-linear profile (Schlick bias/gain) on the plain 2-endpoint fade: resample alpha through the
        // curve. Default (0,0) or any multi-stage ramp falls through to the classic path (byte-identical for
        // XAR-loaded transparency, which never carries a profile).
        if ((t.profileBias!=0.0 || t.profileGain!=0.0) && t.stops.isEmpty()) {
            const int N=24; const double a0=c0.alphaF(), a1=c1.alphaF();
            for(int i=0;i<=N;++i){ double u=double(i)/N; QColor sc=c0;
                sc.setAlphaF(qBound(0.0, a0 + (a1-a0)*profileBiasGain(u, t.profileBias, t.profileGain), 1.0));
                gr.setColorAt(u, sc); }
            return;
        }
        gr.setColorAt(0, c0); gr.setColorAt(1, c1);
        for (const TranspStop& st : t.stops){ QColor sc = c0; sc.setAlphaF(qBound(0.0, 1.0 - st.t, 1.0));
            gr.setColorAt(qBound(0.0, st.pos, 1.0), sc); } };
    QBrush b;
    if (t.geom == 2 && t.elliptical) {
        QRadialGradient rg(QPointF(0,0), 1.0); ramp(rg);
        b = QBrush(rg);
        const QPointF e1 = t.p1 - t.p0, e2 = t.p2 - t.p0;     // unit circle -> ellipse(p0; axis1=e1, axis2=e2)
        b.setTransform(QTransform(e1.x(), e1.y(), e2.x(), e2.y(), t.p0.x(), t.p0.y()) * extra);
    } else if (t.geom == 2) {
        const double r = QLineF(t.p0, t.p1).length();
        QRadialGradient rg(t.p0, qMax(r, 0.001)); ramp(rg);
        b = QBrush(rg); b.setTransform(extra);
    } else if (t.geom == 5) {                                 // conical (angular sweep around p0, start angle p0→p1)
        QConicalGradient cg(t.p0, -QLineF(t.p0, t.p1).angle()); ramp(cg);   // negate like the conical FILL (render.cpp:72) so fill+transparency on the same axis sweep together
        b = QBrush(cg); b.setTransform(extra);
    } else {
        QLinearGradient lg(t.p0, t.p1); ramp(lg);
        b = QBrush(lg); b.setTransform(extra);
    }
    return b;
}
static void paintHslBlend(QPainter& g, const Shape& s);

// Stock arrowhead outlines, ported from Kernel/arrows.cpp CreateStockArrow (coords in millipoints
// -> /750 = doc px). Tip on +X, neck at origin. Negative ArrowId: -2 straight, -3 angled, -4 rounded,
// -5 spot/blob, -6 diamond, -7 feather, -9 hollow-diamond; others fall back to straight. centreOut and
// evenOddOut are set for shapes that need a non-zero origin / odd-even fill (hollow diamond).
static QPainterPath stockArrowPath(int id, QPointF& centreOut, bool& evenOddOut) {
    centreOut = QPointF(0,0); evenOddOut = false;
    const double k = 1.0/750.0;
    QPainterPath p;
    auto M=[&](double x,double y){ p.moveTo(x*k,y*k); };
    auto L=[&](double x,double y){ p.lineTo(x*k,y*k); };
    auto C=[&](double a,double b,double c,double d,double e,double f){ p.cubicTo(a*k,b*k,c*k,d*k,e*k,f*k); };
    switch (id) {
        case -3:  // ANGLED
            M(-27000,54000); L(-9000,0); L(-27000,-54000); L(135000,0); p.closeSubpath(); break;
        case -4:  // ROUNDED
            M(-9000,0); L(-9000,-45000); C(-9000,-51708, 2808,-56580, 9000,-54000);
            L(117000,-9000); C(120916,-7369, 126000,-4242, 126000,0);
            C(126000,4242, 120916,7369, 117000,9000); L(9000,54000);
            C(2808,56580, -9000,51708, -9000,45000); p.closeSubpath(); break;
        case -5:  // SPOT / blob: circle r=54000 at origin
            p.addEllipse(QPointF(0,0), 54000*k, 54000*k); break;
        case -6:  // DIAMOND
            M(-63000,0); L(0,63000); L(63000,0); L(0,-63000); p.closeSubpath(); break;
        case -7:  // FEATHER
            M(18000,-54000); L(108000,-54000); L(63000,0); L(108000,54000); L(18000,54000); L(-36000,0);
            p.closeSubpath(); break;
        case -9:  // HOLLOW DIAMOND (even-odd: outer + inner), centre offset -45000
            M(0,45000); L(-45000,0); L(0,-45000); L(45000,0); p.closeSubpath();
            M(0,63000); L(-63000,0); L(0,-63000); L(63000,0); p.closeSubpath();
            centreOut = QPointF(-45000*k, 0); evenOddOut = true; break;
        case -2: default:  // STRAIGHT (also fallback: -8 feather2, custom +ids, unknown)
            M(-9000,54000); L(-9000,-54000); L(117000,0); p.closeSubpath(); break;
    }
    return p;
}

// A Table: table background, per-cell background + word-wrapped text (honouring merge spans),
// then the border grid over the top. Painter enters in doc space; we apply the shape transform.
static void paintTable(QPainter& g, const Shape& s) {
    const Table& t = s.table; if (!t.valid()) return;
    g.save();
    g.setTransform(s.transform, true);
    const double sp = qMax(0.0, t.spacing);
    if (t.bg.alpha()) g.fillRect(QRectF(0, 0, t.width(), t.height()), t.bg);
    QFont cf = s.font;
    for (int r=0;r<t.rows();++r) for (int c=0;c<t.cols();++c) {
        const TableCell& cell = t.at(r,c);
        if (cell.rowSpan==0 || cell.colSpan==0) continue;          // covered by a merge
        const int rs=qMin(qMax(1,cell.rowSpan), t.rows()-r), cs=qMin(qMax(1,cell.colSpan), t.cols()-c);
        QRectF cr(t.colX(c), t.rowY(r), t.colX(c+cs)-t.colX(c), t.rowY(r+rs)-t.rowY(r));
        if (sp>0) cr = cr.adjusted(sp/2, sp/2, -sp/2, -sp/2);
        const bool hdr = t.headerRow && r==0;
        if (cell.bg.alpha()) g.fillRect(cr, cell.bg);
        else if (hdr) g.fillRect(cr, QColor(228,231,236));     // header row shading (only when no explicit cell bg)
        if (!cell.text.isEmpty()) {
            QFont ccf=cf; if (cell.bold || hdr) ccf.setBold(true);
            g.setPen(cell.textColor); g.setFont(ccf);
            const int va = (cell.valign==1?Qt::AlignTop:cell.valign==2?Qt::AlignBottom:Qt::AlignVCenter);
            const int fl = (cell.align==1?Qt::AlignHCenter:cell.align==2?Qt::AlignRight:Qt::AlignLeft)
                         | va | Qt::TextWordWrap;
            g.drawText(cr.adjusted(t.pad, t.pad, -t.pad, -t.pad), fl, cell.text);
        }
    }
    if (t.borderW > 0.01) {
        QPen bp(t.borderColor, t.borderW); bp.setJoinStyle(Qt::MiterJoin);
        g.setPen(bp); g.setBrush(Qt::NoBrush);
        for (int r=0;r<t.rows();++r) for (int c=0;c<t.cols();++c) {
            const TableCell& cell = t.at(r,c);
            if (cell.rowSpan==0 || cell.colSpan==0) continue;
            const int rs=qMin(qMax(1,cell.rowSpan), t.rows()-r), cs=qMin(qMax(1,cell.colSpan), t.cols()-c);
            QRectF cr(t.colX(c), t.rowY(r), t.colX(c+cs)-t.colX(c), t.rowY(r+rs)-t.rowY(r));
            if (sp>0) cr = cr.adjusted(sp/2, sp/2, -sp/2, -sp/2);
            g.drawRect(cr);
        }
    }
    g.restore();
}

// Photo Region with a SOFT (feathered) edge: render the object un-clipped into a device buffer, then
// multiply in a gaussian-blurred alpha mask of the region so the clip boundary fades out.
static void paintSoftRegion(QPainter& g, const Shape& s) {
    Shape s2=s; s2.regionPath=QPainterPath(); s2.regionFeather=0; s2.opacity=1.0;   // render fully, apply opacity on composite
    s2.shadow.on=s2.extrude.on=s2.contour.on=false;   // under-effects are painted at full extent by paintShape() — don't re-clip them here
    const QTransform xf=g.transform();
    const double scale=std::sqrt(qAbs(xf.determinant()));
    const int rad=qBound(1, int(s.regionFeather*scale), 96);
    const QPainterPath regDoc = s.transform.map(s.regionPath);
    QRectF docB = s.docBounds().intersected(regDoc.boundingRect()); if (docB.isEmpty()) docB=regDoc.boundingRect();
    const int margin=rad+4;
    QRect devB = xf.mapRect(docB).toRect().adjusted(-margin,-margin,margin,margin);
    devB = clipEffectToViewport(devB, margin);   // high-zoom: bound the effect buffer to the visible viewport
    if (devB.width()<1 || devB.height()<1 || (qint64)devB.width()*devB.height()>3e7) {   // fall back to a hard clip
        g.save(); g.setClipPath(regDoc, Qt::IntersectClip); paintShape(g, s2); g.restore(); return; }
    QImage img(devB.size(), QImage::Format_ARGB32_Premultiplied); img.fill(Qt::transparent);
    { QPainter ip(&img); ip.setRenderHint(QPainter::Antialiasing); ip.translate(-devB.topLeft()); ip.setTransform(xf,true); paintShape(ip, s2); }
    QImage mask(devB.size(), QImage::Format_ARGB32_Premultiplied); mask.fill(Qt::transparent);
    { QPainter mp(&mask); mp.setRenderHint(QPainter::Antialiasing); mp.translate(-devB.topLeft()); mp.setTransform(xf,true);
      mp.setPen(Qt::NoPen); mp.setBrush(Qt::white); mp.drawPath(regDoc); }
    gaussBlur(mask, rad);
    { QPainter ip(&img); ip.setCompositionMode(QPainter::CompositionMode_DestinationIn); ip.drawImage(0,0,mask); }
    g.save(); const double op=g.opacity(); g.resetTransform(); g.setOpacity(op*s.opacity);
    g.drawImage(devB.topLeft(), img); g.restore();
}

void paintShape(QPainter& g, const Shape& s) {
    if (!s.visible || s.opacity <= 0.001) return;
    // Under-effects (drop shadow, 3-D extrude, contour) render BENEATH the object. Every special render
    // path below (blur/feather/mask/HSL/grad-transp/soft-region) `return`s before the normal path paints
    // these, so a shape with e.g. a feather AND a drop shadow would silently lose its shadow. Paint them
    // here for those shapes (same order as the normal path). No-op — hence byte-identical — for the normal
    // path and for shapes without any under-effect.
    auto paintUnderEffects = [&]{
        if (!s.shadow.on && !s.extrude.on && !s.contour.on) return;
        const QPainterPath dpu = s.transform.map(s.localPath());
        if (s.shadow.on)  paintShadow(g, dpu, s.shadow);
        if (s.extrude.on) paintExtrude(g, dpu, s.fill, s.extrude);
        paintContour(g, dpu, s.contour);
    };
    if (s.isolate && s.type == ShapeType::Group) { paintIsolatedGroup(g, s); return; }
    if (!s.regionPath.isEmpty() && s.regionFeather > 0.4) { paintUnderEffects(); paintSoftRegion(g, s); return; }   // soft-edged region
    if (s.blur > 0.4)    { paintUnderEffects(); paintBlurred(g, s);   return; }   // live blur: render -> gaussian-blur RGBA -> composite
    if (s.feather > 0.4) { paintUnderEffects(); paintFeathered(g, s); return; }   // soft edge: render -> blur alpha -> composite
    if (s.transp.type > 0 && s.transp.geom == 3) { paintUnderEffects(); paintNoiseMasked(g, s); return; }   // fractal/noise alpha mask
    if (s.transp.geom == 4 && !s.transp.maskBmp.isNull()) { paintUnderEffects(); paintBitmapMasked(g, s); return; }   // bitmap luminance alpha mask
    if (isHslBlend(s.transp.type)) { paintUnderEffects(); paintHslBlend(g, s); return; }   // Hue/Saturation/Luminosity: per-pixel
    // graduated transparency on a non-solid fill: real gradient fade (not the flat mean-opacity approximation)
    if (s.transp.type > 0 && s.transp.isGraduated() && s.fill.isVisible() && s.fill.type != FillType::Solid) {
        paintUnderEffects(); paintGradTranspFill(g, s); return; }

    g.save();
    // Photo Region: clip this object to the painted region (local → doc space).
    if (!s.regionPath.isEmpty()) g.setClipPath(s.transform.map(s.regionPath), Qt::IntersectClip);
    if (s.opacity < 0.999) g.setOpacity(g.opacity() * s.opacity);
    // Object transparency: blend mode + flat opacity (graduated handled per-fill below).
    if (s.transp.type > 0) {
        g.setCompositionMode(xaraBlend(s.transp.type));
        if (s.transp.geom == 0 && s.transp.t0 > 0.001)
            g.setOpacity(g.opacity() * (1.0 - s.transp.t0));
    } else {
        g.setCompositionMode(s.blend);
    }

    // Groups composite their children in their own coordinate space (recurse). A group from a
    // SHADOW/CONTOUR/BEVEL controller applies the effect to the combined silhouette of its children.
    if (s.type == ShapeType::Group) {
        const QPainterPath silDoc = s.transform.map(s.localPath());
        if (s.shadow.on) paintShadow(g, silDoc, s.shadow);
        paintContour(g, silDoc, s.contour);                 // under children
        g.setTransform(s.transform, true);
        if (s.blendSteps > 0 && s.children.size() == 2) {
            // LIVE blend: draw end0, the regenerated morphed in-betweens, then end1 (all in group-local space).
            paintShape(g, s.children[0]);
            QVector<Shape> mids = blendMorph(s.children[0], s.children[1], s.blendSteps, s.blendProfile,0.0, s.blendAttrProfile,0.0, s.blendNodeShift);
            const bool alongPath = !s.blendSpine.isEmpty() && s.blendSpine.length() > 0.01;   // live blend-along-curve
            for (int i=0;i<mids.size();++i){ Shape mid=mids[i];
                if (alongPath){ const QPointF tgt=s.blendSpine.pointAtPercent(qBound(0.0,(i+1.0)/(s.blendSteps+1.0),1.0));
                    const QPointF cur=mid.docBounds().center(); const QTransform tr=QTransform::fromTranslate(tgt.x()-cur.x(), tgt.y()-cur.y());
                    if (mid.type==ShapeType::Path) mid.path=tr.map(mid.path); else mid.transform=mid.transform*tr; }
                paintShape(g, mid); }
            paintShape(g, s.children[1]);
        } else if (s.isClipView && s.clipSplit > 0 && s.clipSplit < s.children.size()) {
            // ClipView (Kernel NodeClipViewController): the keyhole child/children render as ordinary
            // visible art first, then their unioned filled outline clips the remaining content children.
            for (int i = 0; i < s.clipSplit; ++i) paintShape(g, s.children[i]);
            QPainterPath clip; clip.setFillRule(Qt::WindingFill);
            for (int i = 0; i < s.clipSplit; ++i)
                clip.addPath(s.children[i].transform.map(s.children[i].localPath()));
            g.save();
            g.setClipPath(clip, Qt::IntersectClip);          // content masked to the keyhole interior (nests)
            for (int i = s.clipSplit; i < s.children.size(); ++i) paintShape(g, s.children[i]);
            g.restore();
        } else {
            for (const Shape& c : s.children) paintShape(g, c);
        }
        paintBevel(g, s.localPath(), s.bevel);              // over children (painter is in group-local space)
        g.restore();
        return;
    }

    if (s.type == ShapeType::Table) { paintTable(g, s); g.restore(); return; }

    const QPainterPath local = s.localPath();
    const QPainterPath dp = s.transform.map(local);

    if (g_outlineView) {                                  // wireframe view: hairline outline, no fill/effects
        QPen op(QColor(40,90,210), 0); op.setCosmetic(true);
        g.setPen(op); g.setBrush(Qt::NoBrush); g.drawPath(dp); g.restore(); return;
    }

    if (s.shadow.on) paintShadow(g, dp, s.shadow);
    if (s.extrude.on) paintExtrude(g, dp, s.fill, s.extrude);
    // A 3-D-tilted extrude paints its own (rotated, flat-shaded) front face inside paintExtrude — the
    // upright fill/stroke below would draw over it un-rotated, so skip them. Classic extrude (rotX=rotY=0)
    // never takes this branch, so it stays byte-identical.
    if (extrudeDrawsFront(s.extrude)) { g.restore(); return; }
    paintContour(g, dp, s.contour);

    // Rich text: each per-character run painted with its own solid colour (glyphs already
    // carry their own font via layoutRuns). Only taken when textRuns is non-empty — XAR text
    // (always empty runs) falls through to the classic single-brush fill below, byte-identical.
    if (s.type == ShapeType::Text && !s.textRuns.isEmpty()) {
        g.setPen(Qt::NoPen);
        g.setRenderHint(QPainter::Antialiasing, true);
        for (const auto& pr : s.layoutRuns()) { g.setBrush(pr.second); g.drawPath(s.transform.map(pr.first)); }
    }
    // Fill (gradient geometry is in local space -> map the brush via the transform)
    else if (s.fill.isVisible()) {
        const bool gradTransp = s.transp.type > 0 && s.transp.isGraduated();
        const bool fractal = s.fill.fractal && !s.fill.image.isNull();
        const bool bitmap = s.fill.type == FillType::Bitmap && !fractal && !s.fill.image.isNull();
        const bool mesh   = s.fill.type == FillType::Mesh   && !s.fill.meshColors.isEmpty();
        const bool square = s.fill.type == FillType::Square &&  s.fill.meshColors.size() >= 2;
        QBrush br;
        bool clipToTile = false; QPainterPath tileClip;
        if (fractal) {
            // Fractal/noise cloud texture: stretch the generated tile across the shape's local bounds
            // (fills the whole path, unlike a parallelogram bitmap tile).
            const QImage& img = s.fill.image; QRectF lb = local.boundingRect();
            QTransform tex; tex.translate(lb.left(), lb.top());
            tex.scale(lb.width()/qMax(1,img.width()), lb.height()/qMax(1,img.height()));
            br = QBrush(img); br.setTransform(tex * s.transform);
            if (gradTransp) g.setOpacity(g.opacity() * qBound(0.0, 1.0 - (s.transp.t0 + s.transp.t1) / 2.0, 1.0));
        } else if (bitmap || mesh || square) {
            // Textured fill (bitmap or a procedurally-generated mesh/square gradient): map the image
            // into the parallelogram (local), then local -> doc.
            QImage img; Fill mapFill = s.fill;     // mapFill carries the parallelogram used for mapping + clip
            if (bitmap)     img = s.fill.contone ? contoneTint(s.fill.image, s.fill.color, s.fill.color2) : s.fill.image;
            else if (mesh)  img = (s.fill.meshRows>=2 && s.fill.meshCols>=2)   // N×M grid mesh, else legacy 3/4-corner (byte-identical)
                                    ? makeMeshGridImage(s.fill.meshColors, s.fill.meshRows, s.fill.meshCols)
                                    : makeMeshImage(s.fill.meshColors);
            else {          img = makeSquareImage(s.fill.meshColors[0], s.fill.meshColors[1]);
                const QPointF a1 = s.fill.p1 - s.fill.p0, a2 = s.fill.p2 - s.fill.p0;   // expand p0/p1/p2 to the FULL square
                mapFill.p0 = s.fill.p0 - a1 - a2; mapFill.p1 = s.fill.p0 + a1 - a2; mapFill.p2 = s.fill.p0 - a1 + a2; }
            br = QBrush(img);
            br.setTransform(bitmapTexXf(mapFill, img.size()) * s.transform);
            // The image maps to ONE parallelogram; clip to it so the (opaque) texture isn't tiled
            // across the rest of the path — outside stays transparent (e.g. sky shows through).
            QPolygonF para; para << mapFill.p0 << mapFill.p1 << (mapFill.p1 + mapFill.p2 - mapFill.p0) << mapFill.p2;
            tileClip.addPolygon(s.transform.map(para)); tileClip.closeSubpath();
            clipToTile = !(bitmap && s.fill.tiled);   // a tiled bitmap FILL repeats across the whole path; a placed bitmap + mesh/square clip to one parallelogram
            if (gradTransp)   // textured + graduated transparency: approximate with mean opacity
                g.setOpacity(g.opacity() * qBound(0.0, 1.0 - (s.transp.t0 + s.transp.t1) / 2.0, 1.0));
        } else if (gradTransp && s.fill.type == FillType::Solid) {
            // Solid fill + graduated transparency: bake the alpha fade into the fill colour.
            QColor c0 = s.fill.color; c0.setAlphaF(qBound(0.0, 1.0 - s.transp.t0, 1.0));
            QColor c1 = s.fill.color; c1.setAlphaF(qBound(0.0, 1.0 - s.transp.t1, 1.0));
            br = transpGradBrush(s.transp, c0, c1, s.transform);   // linear / circular / ELLIPTICAL
        } else {
            br = fillBrush(s.fill, local.boundingRect());
            if (s.fill.type != FillType::Solid && s.fill.type != FillType::None) br.setTransform(s.transform);
            if (gradTransp)   // gradient fill + graduated transparency: approximate with mean opacity
                g.setOpacity(g.opacity() * qBound(0.0, 1.0 - (s.transp.t0 + s.transp.t1) / 2.0, 1.0));
        }
        g.setBrush(br);
        g.setPen(Qt::NoPen);
        g.setRenderHint(QPainter::Antialiasing, true);
        g.setRenderHint(QPainter::SmoothPixmapTransform, bitmap || mesh || square || fractal);
        if (clipToTile) { g.save(); g.setClipPath(tileClip, Qt::IntersectClip); g.drawPath(dp); g.restore(); }
        else g.drawPath(dp);
    }

    paintBevel(g, dp, s.bevel);

    // Vector / stroke-shape stroke: warp a vector shape along the path (repeated or stretched).
    if (s.stroke.enabled && !s.stroke.strokeShapePaths.isEmpty() && s.stroke.width > 0.001) {
        paintVectorStroke(g, dp, s.stroke);
    }
    // Art/textured brush: stamp the ink along the path instead of a plain pen stroke.
    else if (s.stroke.enabled && !s.stroke.brushInk.isNull() && s.stroke.width > 0.001) {
        paintBrush(g, dp, s.stroke);
    }
    // Variable-width / pressure stroke: a filled outline whose half-width follows the profile.
    else if (s.stroke.enabled && s.stroke.varWidth && !s.stroke.widthProfile.isEmpty() && s.stroke.width > 0.001) {
        paintVarWidth(g, dp, s.stroke);
    }
    // Calligraphic (flat/angled nib): sweep a fixed-angle nib along the path (Minkowski sum with a
    // segment) — thick where the path runs perpendicular to the nib, thin where parallel.
    else if (s.stroke.enabled && s.stroke.calligraphic && s.stroke.width > 0.001) {
        const double th=s.stroke.calligAngle*M_PI/180.0;
        const QPointF v(s.stroke.width*0.5*std::cos(th), s.stroke.width*0.5*std::sin(th));
        QColor sc=s.stroke.color; if(s.stroke.transp>0.001) sc.setAlphaF(sc.alphaF()*qBound(0.0,1.0-s.stroke.transp,1.0));
        QPainterPath fill;
        for(const QPolygonF& poly : dp.toSubpathPolygons()){
            if(poly.size()<2) continue;
            QPolygonF rib; rib.reserve(poly.size()*2);
            for(const QPointF& p: poly) rib << p+v;
            for(int i=poly.size()-1;i>=0;--i) rib << poly[i]-v;
            fill.addPolygon(rib); fill.closeSubpath();
        }
        fill.setFillRule(Qt::WindingFill);
        g.save(); g.setRenderHint(QPainter::Antialiasing,true); g.setPen(Qt::NoPen); g.setBrush(sc); g.drawPath(fill); g.restore();
    }
    // Stroke (constant width in document units; map to device via the painter)
    else if (s.stroke.enabled && s.stroke.width > 0.001) {
        QColor sc = s.stroke.color;
        if (s.stroke.transp > 0.001) sc.setAlphaF(sc.alphaF() * qBound(0.0, 1.0 - s.stroke.transp, 1.0));
        QPen pen(sc);
        pen.setWidthF(s.stroke.width);
        pen.setCapStyle(s.stroke.cap);
        pen.setJoinStyle(s.stroke.join);
        if (!s.stroke.dashes.isEmpty()) pen.setDashPattern(s.stroke.dashes);
        g.setPen(pen);
        g.setBrush(Qt::NoBrush);
        g.drawPath(dp);

        // Arrowheads: stock shapes (Kernel/arrows.cpp) filled in the stroke colour at each OPEN end,
        // scaled by the file's FIXED16 factor × the line width (k = scale·width/144, ports GetArrowMatrix),
        // rotated so the tip points outward along the tangent. Closed subpaths get no arrows.
        if ((s.stroke.startArrow || s.stroke.endArrow) && dp.length() > 0.5) {
            const double len = dp.length();
            const bool closed = (dp.pointAtPercent(0.0) - dp.pointAtPercent(1.0)).manhattanLength() < 0.5;
            if (!closed) {
                const double w = (s.stroke.width > 0.001) ? s.stroke.width : 1.5;   // ZEROWIDTH guard (1125mp/750)
                const double delta = qBound(0.0005, 3.0/qMax(len,1.0), 0.05);
                auto drawArrow = [&](int id, double sw, double sh, const QPointF& P, const QPointF& D){
                    if (id == 0) return;
                    const QPointF out = P - D; if (std::hypot(out.x(), out.y()) < 1e-6) return;  // outward tangent
                    QPointF centre; bool eo = false;
                    QPainterPath stock = stockArrowPath(id, centre, eo);
                    const double kw = qMax(0.02, sw) * w / 144.0, kh = qMax(0.02, sh) * w / 144.0;
                    QTransform T;
                    T.translate(P.x(), P.y());
                    T.rotate(qRadiansToDegrees(std::atan2(out.y(), out.x())));   // aim +X (tip) outward
                    T.scale(kw, kh);
                    T.translate(-centre.x(), -centre.y());
                    QPainterPath ap = T.map(stock);
                    if (eo) ap.setFillRule(Qt::OddEvenFill);
                    g.fillPath(ap, sc);
                };
                if (s.stroke.endArrow)
                    drawArrow(s.stroke.endArrow, s.stroke.endArrowW, s.stroke.endArrowH,
                              dp.pointAtPercent(1.0), dp.pointAtPercent(1.0 - delta));
                if (s.stroke.startArrow)
                    drawArrow(s.stroke.startArrow, s.stroke.startArrowW, s.stroke.startArrowH,
                              dp.pointAtPercent(0.0), dp.pointAtPercent(delta));
            }
        }
    }
    g.restore();
}

// Schlick bias/gain (CProfileBiasGain): params in [-1,+1], 0 = identity. Same mapping as the fractal/
// feather profile in xar.cpp. Bias applied first, then gain.
static inline double schlickBiasR(double b, double t){ return t/((1.0/b - 2.0)*(1.0-t) + 1.0); }
static double profileBiasGain(double t, double biasP, double gainP){
    if (biasP==0.0 && gainP==0.0) return t;
    double b=qBound(0.001,(biasP+1.0)*0.49999+0.00001,0.999), gn=qBound(0.001,(gainP+1.0)*0.49999+0.00001,0.999);
    t = schlickBiasR(b, t);
    t = (t<0.5) ? schlickBiasR(1.0-gn, 2.0*t)/2.0 : 1.0 - schlickBiasR(1.0-gn, 2.0-2.0*t)/2.0;
    return qBound(0.0, t, 1.0);
}
// Feathered (soft) edge. A plain gaussian blur of the whole shape washes out thin features (a star's
// points lose their solid core); Xara instead keeps the interior SOLID and softens only across the
// boundary. So we use a signed distance transform: the alpha ramps over ±half px around the edge (the
// interior stays opaque), the halo colour comes from a blurred copy, the Schlick bias/gain profile
// reshapes the ramp, and a light final blur eases the linear ramp toward Xara's gaussian falloff.
static void paintFeathered(QPainter& g, const Shape& s) {
    Shape s2 = s; s2.feather = 0; s2.shadow.on=s2.extrude.on=s2.contour.on=false;   // under-effects at full extent (paintShape)
    const QRectF docB = s.docBounds();
    if (docB.isEmpty()) { paintShape(g, s2); return; }
    const QTransform xf = g.transform();                       // doc -> device
    const double scale = std::sqrt(qAbs(xf.determinant()));
    const int rad = qBound(0, int(s.feather * scale), 96);
    if (rad < 1) { paintShape(g, s2); return; }
    const int margin = rad + 4;
    QRect devB = xf.mapRect(docB).toRect().adjusted(-margin, -margin, margin, margin);
    // Clip margin covers BOTH gaussian passes: the alpha/colour blur (rad) AND the final easing blur (rad/3), so a
    // visible pixel near the screen edge still has every source pixel those two blurs sample (else a thin band by
    // the edge would render a slightly wrong feather vs the unclipped export).
    devB = clipEffectToViewport(devB, rad + rad/3 + 4);   // high-zoom: bound the effect buffer to the visible viewport
    if (devB.width() < 1 || devB.height() < 1 || (qint64)devB.width()*devB.height() > 3e7) { paintShape(g, s2); return; }
    const int W = devB.width(), H = devB.height();
    QImage img(devB.size(), QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    { QPainter ip(&img); ip.setRenderHint(QPainter::Antialiasing);
      ip.translate(-devB.topLeft()); ip.setTransform(xf, true);   // draw in doc coords -> device buffer
      paintShape(ip, s2); }
    // A shape whose interior is intentionally FAINT (a graduated/flat transparency, e.g. Watch4's vignetted
    // grid backdrop) cannot derive its feather silhouette from its own low alpha: the whole interior reads as
    // empty (alpha<128) and the edge-ramp wipes it to transparent. For those we rasterise an OPAQUE geometry
    // silhouette so the ramp softens only the OUTER boundary and the faint interior is kept. Opaque feathered
    // shapes (stars, photos) keep the proven content-alpha silhouette — byte-identical to before.
    const bool faintInterior = (s.transp.type > 0);
    QImage sil;
    if (faintInterior) {
        sil = QImage(devB.size(), QImage::Format_ARGB32_Premultiplied); sil.fill(Qt::transparent);
        Shape s3=s2; s3.transp=Transparency(); s3.opacity=1.0; s3.fillRule=Qt::WindingFill;
        s3.fill=Fill(); s3.fill.type=FillType::Solid; s3.fill.color=Qt::black; s3.stroke.enabled=false;
        QPainter sp(&sil); sp.setRenderHint(QPainter::Antialiasing); sp.translate(-devB.topLeft()); sp.setTransform(xf,true); paintShape(sp, s3);
    }
    const QImage& silSrc = faintInterior ? sil : img;         // silhouette source for the distance transforms
    QVector<float> dIn, dOut;
    insideDistanceTransform(silSrc, dIn, W, H);               // interior: dist to edge (0 outside)
    QImage inv(W, H, QImage::Format_ARGB32_Premultiplied);
    for (int y=0;y<H;++y){ const QRgb* sr=(const QRgb*)silSrc.constScanLine(y); QRgb* dr=(QRgb*)inv.scanLine(y);
        for (int x=0;x<W;++x) dr[x] = (qAlpha(sr[x])<128) ? 0xFF000000u : 0u; }
    insideDistanceTransform(inv, dOut, W, H);                  // exterior: dist to edge (0 inside)
    const QImage orig = img;                                   // sharp copy -> interior colour (no blur)
    QImage col = img; gaussBlur(col, rad);                     // blurred copy -> halo colour (edge bled outward)
    const double half = rad * 0.9;                             // ramp half-width around the boundary (±half)
    const double bias=s.featherBias, gain=s.featherGain;
    for (int y=0;y<H;++y){ QRgb* dr=(QRgb*)img.scanLine(y); const QRgb* cr=(const QRgb*)col.constScanLine(y);
        const QRgb* orr=(const QRgb*)orig.constScanLine(y);
        for (int x=0;x<W;++x){ int i=y*W+x;
            double d = (dIn[i]>0.0f) ? -(double)dIn[i] : (double)dOut[i];   // signed: − inside, + outside
            double t = qBound(0.0, 0.5 - d/(2.0*half), 1.0);               // coverage ramp over ±half
            t = 1.0 - profileBiasGain(1.0 - t, bias, gain);                // bias/gain profile (identity at 0,0)
            if (t<=0.0){ dr[x]=0; continue; }
            // source colour: SHARP interior (orig) blended with the blurred halo (col) by orig's own coverage,
            // so a feathered BITMAP/gradient keeps its interior detail and only the halo is blurred (the old
            // code sourced every pixel from the fully-blurred copy, mushing feathered photos).
            int oa=qAlpha(orr[x]), ka=qAlpha(cr[x]);
            if (oa==0 && ka==0){ dr[x]=0; continue; }
            double w = oa/255.0;
            double oR=oa>0?qRed(orr[x])*255.0/oa:0, oG=oa>0?qGreen(orr[x])*255.0/oa:0, oB=oa>0?qBlue(orr[x])*255.0/oa:0;
            double cR=ka>0?qRed(cr[x]) *255.0/ka:0, cG=ka>0?qGreen(cr[x]) *255.0/ka:0, cB=ka>0?qBlue(cr[x]) *255.0/ka:0;
            double sR=w*oR+(1-w)*cR, sG=w*oG+(1-w)*cG, sB=w*oB+(1-w)*cB;
            // Opaque shapes: alpha = the geometric ramp t (the ramp IS the soft edge; the content's own AA at
            // the boundary would double-attenuate). Faint-interior shapes: alpha = the content's OWN alpha
            // (sharp interior / blurred halo) attenuated by t, so e.g. Watch4's vignetted grid stays faint
            // instead of being forced opaque (t*255) — and the geometry silhouette above kept it from vanishing.
            double af = faintInterior ? ((dIn[i]>0.0f ? (double)oa : (double)ka)/255.0) * t : t;
            dr[x] = qRgba(qBound(0,(int)(sR*af+0.5),255), qBound(0,(int)(sG*af+0.5),255),
                          qBound(0,(int)(sB*af+0.5),255), qBound(0,(int)(af*255.0+0.5),255)); } }
    gaussBlur(img, qMax(1, rad/3));                            // ease the linear ramp toward a gaussian falloff
    g.save(); const double op = g.opacity(); g.resetTransform(); g.setOpacity(op);
    g.drawImage(devB.topLeft(), img); g.restore();
}

// Live Gaussian blur of a whole object: rasterise sharp into a padded device buffer, blur the full
// premultiplied RGBA, and composite. Non-destructive; gated on s.blur>0 so all XAR content is untouched.
static void paintBlurred(QPainter& g, const Shape& s) {
    Shape s2 = s; s2.blur = 0;                                 // draw the object sharp, then blur the buffer
    s2.shadow.on=s2.extrude.on=s2.contour.on=false;            // under-effects at full extent (paintShape) — not object-blurred
    // A clip and a blend mode apply to the blurred result. Clipping inside the buffer would blur the clip's
    // hard edge outward, and a blend mode there has only transparency to blend with.
    s2.regionPath = QPainterPath(); s2.blend = QPainter::CompositionMode_SourceOver;
    const QRectF docB = s.docBounds();
    if (docB.isEmpty()) { paintShape(g, s2); return; }
    const QTransform xf = g.transform();
    const double scale = std::sqrt(qAbs(xf.determinant()));
    const int rad = qBound(0, int(s.blur * scale), 200);
    if (rad < 1) { paintShape(g, s2); return; }
    const int margin = rad + 4;
    QRect devB = xf.mapRect(docB).toRect().adjusted(-margin,-margin,margin,margin);
    devB = clipEffectToViewport(devB, margin);   // high-zoom: bound the effect buffer to the visible viewport
    if (devB.width()<1 || devB.height()<1 || (qint64)devB.width()*devB.height() > 3e7) { paintShape(g, s2); return; }
    QImage img(devB.size(), QImage::Format_ARGB32_Premultiplied); img.fill(Qt::transparent);
    { QPainter ip(&img); ip.setRenderHint(QPainter::Antialiasing);
      ip.translate(-devB.topLeft()); ip.setTransform(xf, true); paintShape(ip, s2); }
    gaussBlur(img, rad, 200);
    if (s.blurGain != 1.0) {                                   // premultiplied, so scaling all four channels is one step
        const double gain = s.blurGain;
        for (int y = 0; y < img.height(); ++y) { QRgb* row = reinterpret_cast<QRgb*>(img.scanLine(y));
            for (int x = 0; x < img.width(); ++x) {
                const QRgb p = row[x]; const int a = qAlpha(p); if (!a) continue;
                const double k = qMin(gain, 255.0 / a);
                row[x] = qRgba(int(qRed(p)*k+0.5), int(qGreen(p)*k+0.5), int(qBlue(p)*k+0.5), qMin(255, int(a*k+0.5))); } } }
    // blur colour + alpha (premultiplied → correct edges); allow a big radius so large SVG blurs at high zoom don't clamp (feather/shadow keep the default 96 cap → corpus unaffected)
    g.save(); const double op = g.opacity() * (s.opacity<0.999 ? s.opacity : 1.0);
    if (!s.regionPath.isEmpty()) g.setClipPath(s.transform.map(s.regionPath), Qt::IntersectClip);
    g.setCompositionMode(s.blend);
    g.resetTransform(); g.setOpacity(op); g.drawImage(devB.topLeft(), img); g.restore();
}

// Multiply a premultiplied-ARGB buffer's alpha by a per-pixel fractal (fBm) mask, so the opacity
// mottles between (1-t0) and (1-t1). Scaling all four channels keeps the premultiplied invariant.
static void applyNoiseMask(QImage& img, double t0, double t1) {
    const int w = img.width(), h = img.height(); if (w < 1 || h < 1) return;
    const double o0 = qBound(0.0, 1.0 - t0, 1.0), o1 = qBound(0.0, 1.0 - t1, 1.0);
    auto hash = [](int x, int y){ unsigned u=(unsigned)(x*73856093) ^ (unsigned)(y*19349663);
        u^=u>>13; u*=0x5bd1e995u; u^=u>>15; return (u & 0xffffffu)/double(0xffffffu); };
    auto smooth = [&](double fx, double fy){ int x0=(int)std::floor(fx), y0=(int)std::floor(fy);
        auto sc=[](double t){ return t*t*(3-2*t); }; double sx=sc(fx-x0), sy=sc(fy-y0);
        double a=hash(x0,y0), b=hash(x0+1,y0), c=hash(x0,y0+1), d=hash(x0+1,y0+1);
        double tp=a+(b-a)*sx, bt=c+(d-c)*sx; return tp+(bt-tp)*sy; };
    const double base = 10.0/qMax(1, qMin(w,h));   // ~10 cloud features across the shape
    for (int y=0;y<h;++y){ QRgb* row=(QRgb*)img.scanLine(y);
        for (int x=0;x<w;++x){ if (qAlpha(row[x])==0) continue;
            double v=0, amp=0.5, freq=base, norm=0;
            for (int o=0;o<5;++o){ v+=amp*smooth(x*freq+o*11.3, y*freq+o*7.7); norm+=amp; amp*=0.5; freq*=2; }
            v = qBound(0.0, v/norm, 1.0);
            double f = o0 + (o1-o0)*v;
            QRgb p=row[x]; row[x]=qRgba(int(qRed(p)*f+0.5), int(qGreen(p)*f+0.5), int(qBlue(p)*f+0.5), int(qAlpha(p)*f+0.5)); } }
}

// Fractal/noise transparency: render the shape (mask cleared) into a buffer, multiply its alpha by a
// per-pixel fractal mask, then composite — a mottled translucency rather than a flat mean opacity.
static void paintNoiseMasked(QPainter& g, const Shape& s) {
    Shape s2 = s; s2.transp = Transparency(); s2.shadow.on=s2.extrude.on=s2.contour.on=false;   // under-effects at full extent (paintShape)
    const QRectF docB = s.docBounds();
    if (docB.isEmpty()) { paintShape(g, s2); return; }
    const QTransform xf = g.transform();
    QRect devB = xf.mapRect(docB).toRect().adjusted(-2, -2, 2, 2);
    devB = clipEffectToViewport(devB, 4);   // high-zoom: bound the effect buffer to the visible viewport
    if (devB.width() < 1 || devB.height() < 1 || (qint64)devB.width()*devB.height() > 3e7) { paintShape(g, s2); return; }
    QImage img(devB.size(), QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    { QPainter ip(&img); ip.setRenderHint(QPainter::Antialiasing);
      ip.translate(-devB.topLeft()); ip.setTransform(xf, true);
      paintShape(ip, s2); }
    applyNoiseMask(img, s.transp.t0, s.transp.t1);
    g.save(); const double op = g.opacity(); g.resetTransform(); g.setOpacity(op);
    g.setCompositionMode(xaraBlend(s.transp.type));
    g.drawImage(devB.topLeft(), img); g.restore();
}

// Bitmap MASK transparency (BITMAPTRANSPARENTFILL, geom=4): render the shape, then multiply its alpha by the
// mask bitmap's luminance sampled over the parallelogram (p0/p1/p2, local space). Xara stores e.g. a soft
// drop-shadow's falloff as a greyscale bitmap; without it the shape is a flat semi-opaque slab. Luminance v=0
// (black) -> transparency t0, v=1 (white) -> transparency t1; opacity factor = 1 - that. Edge-clamped outside.
static void paintBitmapMasked(QPainter& g, const Shape& s) {
    Shape s2 = s; s2.transp = Transparency(); s2.shadow.on=s2.extrude.on=s2.contour.on=false;   // under-effects at full extent (paintShape)
    const QRectF docB = s.docBounds();
    if (docB.isEmpty() || s.transp.maskBmp.isNull()) { paintShape(g, s2); return; }
    const QTransform xf = g.transform();
    QRect devB = xf.mapRect(docB).toRect().adjusted(-2, -2, 2, 2);
    devB = clipEffectToViewport(devB, 4);   // high-zoom: bound the effect buffer to the visible viewport
    if (devB.width() < 1 || devB.height() < 1 || (qint64)devB.width()*devB.height() > 3e7) { paintShape(g, s2); return; }
    QImage img(devB.size(), QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    { QPainter ip(&img); ip.setRenderHint(QPainter::Antialiasing);
      ip.translate(-devB.topLeft()); ip.setTransform(xf, true);
      paintShape(ip, s2); }
    const QImage m = s.transp.maskBmp.convertToFormat(QImage::Format_ARGB32);
    const int mw = m.width(), mh = m.height();
    // image->local (matches bitmapTexXf): img(0,0)->p2, img(W,0)->p2+(p1-p0), img(0,H)->p0
    const QPointF ax = (s.transp.p1 - s.transp.p0) / qMax(1, mw);
    const QPointF ay = (s.transp.p0 - s.transp.p2) / qMax(1, mh);
    const QTransform localToImg = QTransform(ax.x(), ax.y(), ay.x(), ay.y(), s.transp.p2.x(), s.transp.p2.y()).inverted();
    const QTransform devToLocal = (s.transform * xf).inverted();          // device -> shape-local
    const double o0 = qBound(0.0, 1.0 - s.transp.t0, 1.0), o1 = qBound(0.0, 1.0 - s.transp.t1, 1.0);
    for (int y = 0; y < img.height(); ++y) { QRgb* row = (QRgb*)img.scanLine(y);
        for (int x = 0; x < img.width(); ++x) { if (qAlpha(row[x]) == 0) continue;
            QPointF loc = devToLocal.map(QPointF(devB.left()+x+0.5, devB.top()+y+0.5));
            QPointF ip = localToImg.map(loc);
            int bx = qBound(0, (int)std::floor(ip.x()), mw-1), by = qBound(0, (int)std::floor(ip.y()), mh-1);
            QRgb mp = m.pixel(bx, by);
            double v = (0.299*qRed(mp) + 0.587*qGreen(mp) + 0.114*qBlue(mp)) / 255.0;
            double f = o0 + (o1 - o0) * v;                                // luminance-mapped opacity factor
            QRgb p = row[x]; row[x] = qRgba(int(qRed(p)*f+0.5), int(qGreen(p)*f+0.5), int(qBlue(p)*f+0.5), int(qAlpha(p)*f+0.5)); } }
    if (isHslBlend(s.transp.type)) { compositeHsl(g, img, devB, s.transp.type, g.opacity()); return; }
    g.save(); const double op = g.opacity(); g.resetTransform(); g.setOpacity(op);
    g.setCompositionMode(s.transp.type > 1 ? xaraBlend(s.transp.type) : s.blend);
    g.drawImage(devB.topLeft(), img); g.restore();
}

// A group composited as one object: children blend among themselves on a transparent buffer, then its
// mask, clip, opacity and blend mode apply to the result (a Photoshop group that isn't pass-through).
static void paintIsolatedGroup(QPainter& g, const Shape& s) {
    Shape s2 = s; s2.isolate = false; s2.opacity = 1.0; s2.blend = QPainter::CompositionMode_SourceOver;
    const int hsl = isHslBlend(s.transp.type) ? s.transp.type : 0;
    if (hsl) s2.transp.type = s2.transp.geom == 4 ? 1 : 0;
    const QTransform xf = g.transform();
    QRect devB = xf.mapRect(s.docBounds()).toRect().adjusted(-2, -2, 2, 2);
    if (QPaintDevice* dev = g.device()) devB = devB.intersected(QRect(0, 0, dev->width(), dev->height()));
    if (devB.width() < 1 || devB.height() < 1 || (qint64)devB.width()*devB.height() > 3e7) { paintShape(g, s2); return; }
    QImage img(devB.size(), QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    { QPainter ip(&img); ip.setRenderHint(QPainter::Antialiasing); ip.setRenderHint(QPainter::SmoothPixmapTransform);
      ip.translate(-devB.topLeft()); ip.setTransform(xf, true);
      if (!s.regionPath.isEmpty()) { ip.setClipPath(s.transform.map(s.regionPath), Qt::IntersectClip); s2.regionPath = QPainterPath(); }
      paintShape(ip, s2); }
    if (hsl) { compositeHsl(g, img, devB, hsl, g.opacity() * s.opacity); return; }
    g.save(); const double op = g.opacity() * s.opacity; g.resetTransform(); g.setOpacity(op);
    g.setCompositionMode(s.blend);
    g.drawImage(devB.topLeft(), img); g.restore();
}

// Graduated transparency on a non-solid fill (gradient/textured): render the fill to a buffer, then multiply
// its alpha by the transparency gradient (DestinationIn) — a real fade, not the flat mean-opacity approximation
// (which turned SimpleSphere's gradient facets into uniform haze instead of crisp fading highlights).
static void paintGradTranspFill(QPainter& g, const Shape& s) {
    Shape s2 = s; s2.transp = Transparency(); s2.shadow.on=s2.extrude.on=s2.contour.on=false;   // under-effects at full extent (paintShape)
    const QRectF docB = s.docBounds();
    if (docB.isEmpty()) { paintShape(g, s2); return; }
    const QTransform xf = g.transform();
    QRect devB = xf.mapRect(docB).toRect().adjusted(-2, -2, 2, 2);
    devB = clipEffectToViewport(devB, 4);   // high-zoom: bound the effect buffer to the visible viewport
    if (devB.width() < 1 || devB.height() < 1 || (qint64)devB.width()*devB.height() > 3e7) { paintShape(g, s2); return; }
    QImage img(devB.size(), QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    { QPainter ip(&img); ip.setRenderHint(QPainter::Antialiasing);
      ip.translate(-devB.topLeft()); ip.setTransform(xf, true);
      paintShape(ip, s2);
      // mask: alpha *= transparency gradient (opacity = 1 - t along p0..p1, in doc space)
      const QColor a0(0,0,0, qBound(0,(int)qRound((1.0-s.transp.t0)*255),255));
      const QColor a1(0,0,0, qBound(0,(int)qRound((1.0-s.transp.t1)*255),255));
      // p0/p1/p2 are doc-space here (no shape transform), so extra = identity
      QBrush mask = transpGradBrush(s.transp, a0, a1, QTransform());
      ip.setCompositionMode(QPainter::CompositionMode_DestinationIn);
      ip.setPen(Qt::NoPen); ip.setBrush(mask);
      ip.drawRect(docB.adjusted(-4,-4,4,4)); }
    g.save(); double op = g.opacity() * (s.opacity < 0.999 ? s.opacity : 1.0);
    g.resetTransform(); g.setOpacity(op);
    g.setCompositionMode(xaraBlend(s.transp.type));
    g.drawImage(devB.topLeft(), img); g.restore();
}

// ---- W3C non-separable blend modes (Hue/Saturation/Luminosity), per-pixel on 0..1 RGB ----
static inline double hslLum(double r,double g,double b){ return 0.3*r + 0.59*g + 0.11*b; }
static void hslClip(double& r,double& g,double& b){
    double L=hslLum(r,g,b), n=qMin(r,qMin(g,b)), x=qMax(r,qMax(g,b));
    if(n<0 && L>n){ double d=L-n; r=L+(r-L)*L/d; g=L+(g-L)*L/d; b=L+(b-L)*L/d; }
    if(x>1 && x>L){ double d=x-L; r=L+(r-L)*(1-L)/d; g=L+(g-L)*(1-L)/d; b=L+(b-L)*(1-L)/d; }
}
static void hslSetLum(double& r,double& g,double& b,double L){ double d=L-hslLum(r,g,b); r+=d;g+=d;b+=d; hslClip(r,g,b); }
static double hslSat(double r,double g,double b){ return qMax(r,qMax(g,b)) - qMin(r,qMin(g,b)); }
static void hslSetSat(double& r,double& g,double& b,double s){
    double* p[3]={&r,&g,&b};
    if(*p[0]>*p[1]) std::swap(p[0],p[1]);
    if(*p[1]>*p[2]) std::swap(p[1],p[2]);
    if(*p[0]>*p[1]) std::swap(p[0],p[1]);   // p[0]=min p[1]=mid p[2]=max
    if(*p[2] > *p[0]){ *p[1]=(*p[1]-*p[0])*s/(*p[2]-*p[0]); *p[2]=s; } else { *p[1]=0; *p[2]=0; }
    *p[0]=0;
}
// type 16=Saturation, 28=Luminosity, 31=Hue. Backdrop (br,bg,bb) is overwritten with the blend result.
static void hslBlend(int type, double& br,double& bg,double& bb, double sr,double sg,double sb){
    if(type==52 || type==53){      // Darker / Lighter Colour: whichever whole colour has the lower / higher luminance
        const bool srcWins = type==52 ? hslLum(sr,sg,sb)<hslLum(br,bg,bb) : hslLum(sr,sg,sb)>hslLum(br,bg,bb);
        if(srcWins){ br=sr; bg=sg; bb=sb; }
        return;
    }
    if(type>=45 && type<=51){      // Linear Burn, Vivid, Linear Light, Pin, Hard Mix, Subtract, Divide (separable)
        auto burn=[](double b,double s){ return s<=0.0?0.0:qMax(0.0,1.0-(1.0-b)/s); };
        auto dodge=[](double b,double s){ return s>=1.0?1.0:qMin(1.0,b/(1.0-s)); };
        auto f=[&](double b,double s)->double{
            switch(type){
            case 45: return qMax(0.0,b+s-1.0);
            case 46: return s<0.5?burn(b,2.0*s):dodge(b,2.0*s-1.0);
            case 47: return qBound(0.0,b+2.0*s-1.0,1.0);
            case 48: return s<0.5?qMin(b,2.0*s):qMax(b,2.0*s-1.0);
            case 49: return (s<0.5?burn(b,2.0*s):dodge(b,2.0*s-1.0))<0.5?0.0:1.0;
            case 50: return qMax(0.0,b-s);
            default: return s<=0.0?1.0:qMin(1.0,b/s);
            } };
        br=f(br,sr); bg=f(bg,sg); bb=f(bb,sb); return;
    }
    if(type>=41 && type<=44){      // Average, Negation, Reflect, Glow (separable; imported from Affinity)
        auto f=[type](double b,double s){
            switch(type){
            case 41: return (b+s)/2.0;
            case 42: return 1.0-qAbs(1.0-b-s);
            case 43: return s>=1.0?1.0:qMin(1.0,b*b/(1.0-s));
            default: return b>=1.0?1.0:qMin(1.0,s*s/(1.0-b));
            } };
        br=f(br,sr); bg=f(bg,sg); bb=f(bb,sb); return;
    }
    if(type==40){ const double L=hslLum(br,bg,bb); br=sr;bg=sg;bb=sb; hslSetLum(br,bg,bb,L); return; }   // Colour
    if(type==28){ hslSetLum(br,bg,bb, hslLum(sr,sg,sb)); }                                   // Luminosity
    else if(type==16){ double L=hslLum(br,bg,bb),S=hslSat(sr,sg,sb); hslSetSat(br,bg,bb,S); hslSetLum(br,bg,bb,L); } // Saturation
    else { double L=hslLum(br,bg,bb),S=hslSat(br,bg,bb); br=sr;bg=sg;bb=sb; hslSetSat(br,bg,bb,S); hslSetLum(br,bg,bb,L); } // Hue
}
// A per-pixel blend over a backdrop that isn't opaque. A blend only applies where there is something to blend
// with: over nothing the source shows as itself, and the result gains the source's coverage.
static QRgb hslOverPartial(QRgb back, QRgb src, double rr, double rg, double rb, double a){
    const double ab=qAlpha(back)/255.0, ao=a+ab*(1.0-a);
    if(ao<=0.0) return back;
    auto mix=[&](double cb,double cs,double blended){ const double c=((1.0-a)*ab*cb + a*((1.0-ab)*cs + ab*blended))/ao; return qBound(0,int(c*255.0+0.5),255); };
    return qRgba(mix(qRed(back)/255.0,qRed(src)/255.0,rr), mix(qGreen(back)/255.0,qGreen(src)/255.0,rg), mix(qBlue(back)/255.0,qBlue(src)/255.0,rb), qBound(0,int(ao*255.0+0.5),255));
}

// An already rendered buffer through a per-pixel blend mode. Needs the backdrop, so only when painting to an image.
static void compositeHsl(QPainter& g, const QImage& srcIn, const QRect& devB0, int type, double strength){
    QPaintDevice* dev = g.device();
    if(!dev || dev->devType()!=QInternal::Image){ g.save(); g.resetTransform(); g.setOpacity(strength); g.drawImage(devB0.topLeft(), srcIn); g.restore(); return; }
    QImage* target = static_cast<QImage*>(dev);
    const QRect devB = devB0.intersected(QRect(0,0,target->width(),target->height()));
    if(devB.width()<1 || devB.height()<1) return;
    const QImage src = srcIn.copy(devB.translated(-devB0.topLeft())).convertToFormat(QImage::Format_ARGB32);
    QImage bd = target->copy(devB).convertToFormat(QImage::Format_ARGB32);
    for(int y=0;y<devB.height();++y){ const QRgb* sR=(const QRgb*)src.constScanLine(y); QRgb* bR=(QRgb*)bd.scanLine(y);
        for(int x=0;x<devB.width();++x){ const int sa=qAlpha(sR[x]); if(sa==0) continue;
            const double br=qRed(bR[x])/255.0, bg=qGreen(bR[x])/255.0, bb=qBlue(bR[x])/255.0;
            double rr=br,rg=bg,rb=bb; hslBlend(type, rr,rg,rb, qRed(sR[x])/255.0,qGreen(sR[x])/255.0,qBlue(sR[x])/255.0);
            const double a=(sa/255.0)*strength;
            const int ba=qAlpha(bR[x]);
            if(ba<255){ bR[x]=hslOverPartial(bR[x],sR[x],rr,rg,rb,a); continue; }
            bR[x]=qRgba(qBound(0,int((br+(rr-br)*a)*255+0.5),255),qBound(0,int((bg+(rg-bg)*a)*255+0.5),255),qBound(0,int((bb+(rb-bb)*a)*255+0.5),255),qAlpha(bR[x])); } }
    g.save(); g.resetTransform(); g.setClipping(false); g.setOpacity(1.0);
    g.setCompositionMode(QPainter::CompositionMode_Source);
    g.drawImage(devB.topLeft(), bd); g.restore();
}

// Hue/Saturation/Luminosity transparency: blend the shape's colour with the backdrop per-pixel.
// Needs the backdrop, which is only available when painting to a QImage (export) — else plain over.
static void paintHslBlend(QPainter& g, const Shape& s){
    Shape s2=s; s2.transp=Transparency(); s2.shadow.on=s2.extrude.on=s2.contour.on=false;   // under-effects at full extent (paintShape)
    const QRectF docB=s.docBounds(); if(docB.isEmpty()){ paintShape(g,s2); return; }
    s2.opacity=1.0;      // the opacity is applied once, in the blend's strength below
    const QTransform xf=g.transform();
    QRect devB = xf.mapRect(docB).toRect().adjusted(-1,-1,1,1);
    QPaintDevice* dev=g.device();
    if(!dev || dev->devType()!=QInternal::Image || (qint64)devB.width()*devB.height()>3e7){ paintShape(g,s2); return; }
    QImage* target=static_cast<QImage*>(dev);
    devB = devB.intersected(QRect(0,0,target->width(),target->height()));
    if(devB.width()<1 || devB.height()<1){ paintShape(g,s2); return; }
    QImage src(devB.size(),QImage::Format_ARGB32); src.fill(Qt::transparent);
    { QPainter ip(&src); ip.setRenderHint(QPainter::Antialiasing); ip.translate(-devB.topLeft()); ip.setTransform(xf,true); paintShape(ip,s2); }
    QImage bd = target->copy(devB).convertToFormat(QImage::Format_ARGB32);   // backdrop (straight RGBA)
    const double strength = qBound(0.0, 1.0 - (s.transp.geom==0? s.transp.t0 : (s.transp.t0+s.transp.t1)/2.0), 1.0) * qBound(0.0,s.opacity,1.0);
    const int w=devB.width(), h=devB.height();
    for(int y=0;y<h;++y){ const QRgb* sR=(const QRgb*)src.constScanLine(y); QRgb* bR=(QRgb*)bd.scanLine(y);
        for(int x=0;x<w;++x){ int sa=qAlpha(sR[x]); if(sa==0) continue;
            double br=qRed(bR[x])/255.0, bg=qGreen(bR[x])/255.0, bb=qBlue(bR[x])/255.0;
            double rr=br,rg=bg,rb=bb; hslBlend(s.transp.type, rr,rg,rb, qRed(sR[x])/255.0,qGreen(sR[x])/255.0,qBlue(sR[x])/255.0);
            double a=(sa/255.0)*strength;
            if(qAlpha(bR[x])<255){ bR[x]=hslOverPartial(bR[x],sR[x],rr,rg,rb,a); continue; }
            int R=(int)((br+(rr-br)*a)*255+0.5), G=(int)((bg+(rg-bg)*a)*255+0.5), B=(int)((bb+(rb-bb)*a)*255+0.5);
            bR[x]=qRgba(qBound(0,R,255),qBound(0,G,255),qBound(0,B,255),qAlpha(bR[x])); } }
    g.save(); g.resetTransform(); g.setClipping(false); g.setOpacity(1.0);
    g.setCompositionMode(QPainter::CompositionMode_Source);
    g.drawImage(devB.topLeft(), bd); g.restore();
}

// Recolour an ink to a flat colour, preserving its alpha shape (a "coloured" art brush).
static QImage tintInk(const QImage& ink, const QColor& c) {
    QImage out = ink.convertToFormat(QImage::Format_ARGB32);
    const int w=out.width(), h=out.height(), cr=c.red(), cg=c.green(), cb=c.blue(); const double ca=c.alphaF();
    for (int y=0;y<h;++y){ QRgb* row=(QRgb*)out.scanLine(y);
        for (int x=0;x<w;++x) row[x]=qRgba(cr,cg,cb,int(qAlpha(row[x])*ca+0.5)); }
    return out;
}

// Art/textured brush: stamp the ink bitmap along the path, scaled so its height = the stroke width,
// rotated to the local tangent, spaced by brushSpacing×ink-width.
static void paintBrush(QPainter& g, const QPainterPath& dp, const Stroke& st) {
    const QImage ink = st.brushTint ? tintInk(st.brushInk, st.color) : st.brushInk;
    if (ink.isNull() || ink.height() < 1) return;
    const double len = dp.length(); if (len < 0.5) return;
    // brushScale>0: real Xara nib placed at natural size (explicit px->doc scale), absolute doc-unit pitch.
    // brushScale==0: app's own ink bitmap, auto-fit its height to the stroke width, pitch = fraction of ink width.
    const double scale = (st.brushScale > 0.0) ? st.brushScale : qMax(0.01, st.width) / ink.height();
    const double step  = (st.brushScale > 0.0) ? qMax(1.0, st.brushSpacing)
                                               : qMax(1.0, qMax(0.05, st.brushSpacing) * ink.width() * scale);
    const int n = qMax(1, int(len/step));
    g.save();
    g.setRenderHint(QPainter::SmoothPixmapTransform, true);
    if (st.transp > 0.001) g.setOpacity(g.opacity() * qBound(0.0, 1.0 - st.transp, 1.0));
    // Deterministic per-stamp pseudo-random in [0,1) (keeps corpus renders reproducible — no Math.random).
    auto rnd = [](int i, int salt){ uint32_t h = uint32_t(i)*2654435761u ^ uint32_t(salt)*40503u; h ^= h>>13; h *= 1274126177u; h ^= h>>16; return (h & 0xffffu)/65535.0; };
    const bool scat = st.brushScatter>0.001 || st.brushSizeVar>0.001 || st.brushRotate!=0.0 || !st.brushRotateToLine;
    for (int i=0;i<=n;++i) {
        const double pct = double(i)/n;
        QPointF p = dp.pointAtPercent(pct);
        double ang = st.brushRotateToLine ? dp.angleAtPercent(pct) : 0.0;   // tangent, degrees (Qt: CCW from +x, y-down)
        double sc = scale;
        if (scat) {
            ang += st.brushRotate;
            if (st.brushScatter>0.001) p += QPointF((rnd(i,1)*2-1)*st.brushScatter, (rnd(i,3)*2-1)*st.brushScatter);
            if (st.brushSizeVar>0.001)  sc *= qMax(0.05, 1.0 + (rnd(i,2)*2-1)*st.brushSizeVar);
        }
        g.save();
        g.translate(p); g.rotate(-ang); g.scale(sc, sc);
        g.drawImage(QPointF(-ink.width()/2.0, -ink.height()/2.0), ink);
        g.restore();
    }
    g.restore();
}

// Variable-width / pressure stroke: sample the path by arc length, offset ±(width/2)·profile(t) along the
// path normal on each side, fill the closed outline in the stroke colour (QPen can't vary width). Ports
// Kernel/pathstrk.cpp PathStroker (MaxWidth = LineWidth/2 = radius; offset = Centre ± Normal·Width).
static void paintVarWidth(QPainter& g, const QPainterPath& dp, const Stroke& st) {
    const double len = dp.length(); if (len < 0.5) return;
    const int N = qBound(32, int(len/2.0), 2000);           // ~1 sample / 2 doc px
    const double halfBase = 0.5 * st.width;                  // == Kernel MaxWidth (radius)
    const double dt = 0.5 / N;
    QVector<QPointF> fwd, bwd; fwd.reserve(N+1); bwd.reserve(N+1);
    for (int i = 0; i <= N; ++i) {
        const double t = double(i)/N;
        const QPointF p  = dp.pointAtPercent(t);
        const QPointF pa = dp.pointAtPercent(qMax(0.0, t-dt));
        const QPointF pb = dp.pointAtPercent(qMin(1.0, t+dt));
        QPointF tang = pb - pa; const double tl = std::hypot(tang.x(), tang.y());
        if (tl < 1e-9) { if (!fwd.isEmpty()) { fwd.push_back(fwd.back()); bwd.push_back(bwd.back()); } continue; }
        tang /= tl; const QPointF nrm(-tang.y(), tang.x());   // left normal
        const double hw = qMax(0.0, halfBase * st.widthMultAt(t));
        fwd.push_back(p + nrm*hw);
        bwd.push_back(p - nrm*hw);
    }
    if (fwd.size() < 2) return;
    QPainterPath outline; outline.moveTo(fwd.front());
    for (int i = 1; i < fwd.size(); ++i)  outline.lineTo(fwd[i]);
    for (int i = bwd.size()-1; i >= 0; --i) outline.lineTo(bwd[i]);   // -normal side back to start
    outline.closeSubpath();
    QColor sc = st.color;
    if (st.transp > 0.001) sc.setAlphaF(sc.alphaF() * qBound(0.0, 1.0 - st.transp, 1.0));
    g.save();
    g.setRenderHint(QPainter::Antialiasing, true);
    g.setPen(Qt::NoPen); g.setBrush(sc);
    g.drawPath(outline.simplified());    // winding union: removes inner-corner self-intersections
    g.restore();
}

// Vector / stroke-shape stroke (Kernel pathstrk.cpp): map the shape's bbox X-axis onto the path's arc-length
// 0..1 and its Y-axis onto ±(width/2) perpendicular to the spine, warping the shape to follow the path. The
// shape is repeated (strokeRepeating; count = strokeRepeats or computed from length) or stretched once.
static void paintVectorStroke(QPainter& g, const QPainterPath& dp, const Stroke& st) {
    const double len = dp.length(); if (len < 0.5 || st.strokeShapePaths.isEmpty()) return;
    QRectF bb; for (const QPainterPath& p : st.strokeShapePaths) bb = bb.united(p.boundingRect());
    if (bb.width() < 0.01 || bb.height() < 0.01) return;
    const double cy = bb.center().y(), hscale = st.width / bb.height();   // Y -> ±half-line-width (GetScaleFactor)
    int N = 1;
    if (st.strokeRepeating) {
        const double stampW = bb.width() * hscale;                        // shape width when scaled to the line width
        N = st.strokeRepeats > 0 ? st.strokeRepeats : qMax(1, int(qRound(len / qMax(1.0, stampW))));
    }
    const double dtp = 0.5 / qMax(64, N*8);
    auto warp = [&](double ug, double off) -> QPointF {
        ug = qBound(0.0, ug, 1.0);
        const QPointF P = dp.pointAtPercent(ug);
        const QPointF pa = dp.pointAtPercent(qMax(0.0, ug-dtp)), pb = dp.pointAtPercent(qMin(1.0, ug+dtp));
        QPointF t = pb - pa; const double tl = std::hypot(t.x(), t.y()); if (tl < 1e-9) return P;
        t /= tl; return P + QPointF(-t.y(), t.x())*off;                    // offset along the spine normal
    };
    g.save(); g.setRenderHint(QPainter::Antialiasing, true); g.setPen(Qt::NoPen);
    for (int k = 0; k < N; ++k) {
        for (int li = 0; li < st.strokeShapePaths.size(); ++li) {
            QColor col = st.strokeOverrideFill ? st.color : st.strokeShapeCols.value(li, st.color);
            if (st.transp > 0.001) col.setAlphaF(col.alphaF() * qBound(0.0, 1.0 - st.transp, 1.0));
            QPainterPath out;
            for (const QPolygonF& poly : st.strokeShapePaths[li].toSubpathPolygons()) {
                if (poly.isEmpty()) continue;
                QPolygonF wp; wp.reserve(poly.size());
                for (const QPointF& pt : poly) {
                    const double u = (pt.x() - bb.left()) / bb.width(), off = (pt.y() - cy) * hscale;
                    wp.push_back(warp((k + u) / N, off));
                }
                out.addPolygon(wp); out.closeSubpath();
            }
            out.setFillRule(Qt::WindingFill);
            g.fillPath(out, col);
        }
    }
    g.restore();
}

void paintDocument(QPainter& g, const Document& d) {
    // Single-layer docs (every XAR import, the default new doc): paint the flat z-order verbatim —
    // byte-identical to the pre-layer renderer, so render fidelity is untouched.
    if (d.layers.size() <= 1) {
        if (d.layers.isEmpty() || (d.layers[0].visible && (d.layers[0].printable || g_screenRender)))
            for (const Shape& s : d.shapes) { if (cullShape(s) || renderFiltered(s)) continue; paintShape(g, s); }
        return;
    }
    // Multi-layer: paint bottom layer first; skip hidden layers; preserve z-order within each layer.
    const int nL = d.layers.size();
    const double adv = d.pageSize.height() + d.pageGap;
    for (int L = 0; L < nL; ++L) {
        if (!d.layers[L].visible) continue;
        if (!d.layers[L].printable && !g_screenRender) continue;   // non-printing layer: on screen only, not in print/export
        if (d.layers[L].master && d.pageCount > 1) {           // master page: draw each object on EVERY page
            for (const Shape& s : d.shapes) { if (qBound(0, s.layer, nL-1) != L || renderFiltered(s)) continue;
                int home = qBound(0, int(s.docBounds().center().y()/adv), d.pageCount-1);
                for (int pg = 0; pg < d.pageCount; ++pg) { g.save(); g.translate(0, (pg-home)*adv); paintShape(g, s); g.restore(); } }
        } else {
            for (const Shape& s : d.shapes) if (qBound(0, s.layer, nL-1) == L && !cullShape(s) && !renderFiltered(s)) paintShape(g, s);
        }
    }
}

} // namespace xara
