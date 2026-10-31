// gisvector.cpp — GDAL/OGR (geospatial vector cluster) + Graphviz (DOT) → SVG document string,
// fed to XarQ's SVG importer (importVectorSvgPages → parseSvg → editable Shapes). Both libraries are
// heavyweight and optional; all their headers are confined to this single translation unit and each
// block compiles to a stub when its -dev package is absent (HAVE_GDAL / HAVE_GRAPHVIZ).
#include "gisvector.h"
#include <QStringList>

// ============================ GDAL / OGR (geospatial vector) ============================
#ifdef HAVE_GDAL
#include <ogrsf_frmts.h>     // GDALDataset / OGRLayer / OGRFeature  (pulls in gdal_priv.h)
#include <ogr_geometry.h>    // OGRGeometry / OGRPoint / OGRLineString / OGRPolygon / OGRMulti*
#include <ogr_core.h>        // wkb* enums + wkbFlatten
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace xara {
namespace {

// ---- caps: a hostile input must not OOM or hang ----
static const long long MAX_FEATURES = 200000;
static const long long MAX_POINTS   = 4000000;
static const long long MAX_PRIMS    = 500000;
static const long long MAX_OUTPUT   = 64LL * 1024 * 1024;   // 64 MB of SVG text
static const int       MAX_DEPTH    = 32;                    // nested collections / curves

// Reject NaN/Inf AND finite-but-absurd magnitudes: coordinates near DBL_MAX would make the bbox
// subtraction (maxX-minX) or the Y-flip (maxY-y) overflow to +Inf, which slips past the `!(W>0)`
// clamp (Inf>0 is true) and emits "inf" tokens into the SVG. 1e15 is far beyond any real projected
// coordinate (Earth in millimetres ≈ 1e11), so every difference below stays finite.
inline bool finite2(double x, double y){
    return std::isfinite(x) && std::isfinite(y) && std::fabs(x) < 1e15 && std::fabs(y) < 1e15;
}

// Compact, locale-independent number formatting (callers pre-check finiteness).
inline QString num(double v){ return QString::number(v, 'g', 10); }

struct Ring { std::vector<double> x, y; };
struct Prim { int kind; std::vector<Ring> rings; };  // kind: 0=point, 1=line, 2=polygon

struct Accum {
    std::vector<Prim> prims;
    long long points = 0;
    double minX =  std::numeric_limits<double>::infinity();
    double minY =  std::numeric_limits<double>::infinity();
    double maxX = -std::numeric_limits<double>::infinity();
    double maxY = -std::numeric_limits<double>::infinity();
    bool haveBounds = false;
    bool full() const { return points >= MAX_POINTS || (long long)prims.size() >= MAX_PRIMS; }
    void see(double x, double y){
        if(x<minX)minX=x; if(y<minY)minY=y; if(x>maxX)maxX=x; if(y>maxY)maxY=y; haveBounds=true;
    }
};

void addPoint(const OGRPoint* p, Accum& a){
    if(!p || a.full()) return;
    double x=p->getX(), y=p->getY(); if(!finite2(x,y)) return;
    Prim pr; pr.kind=0; Ring r; r.x.push_back(x); r.y.push_back(y); pr.rings.push_back(std::move(r));
    a.prims.push_back(std::move(pr)); a.points++; a.see(x,y);
}
void addCurve(const OGRSimpleCurve* c, int kind, Accum& a){
    if(!c || a.full()) return;
    int n=c->getNumPoints(); if(n<=0) return;
    Ring r;
    for(int i=0;i<n && !a.full();++i){
        double x=c->getX(i), y=c->getY(i); if(!finite2(x,y)) continue;
        r.x.push_back(x); r.y.push_back(y); a.points++; a.see(x,y);
    }
    if(r.x.size()<2) return;              // need a real segment
    Prim pr; pr.kind=kind; pr.rings.push_back(std::move(r)); a.prims.push_back(std::move(pr));
}
void addPolygon(const OGRPolygon* poly, Accum& a){
    if(!poly || a.full()) return;
    Prim pr; pr.kind=2;
    auto readRing=[&](const OGRLinearRing* ring){
        if(!ring) return; int n=ring->getNumPoints(); if(n<=0) return;
        Ring r;
        for(int i=0;i<n && !a.full();++i){
            double x=ring->getX(i), y=ring->getY(i); if(!finite2(x,y)) continue;
            r.x.push_back(x); r.y.push_back(y); a.points++; a.see(x,y);
        }
        if(r.x.size()>=3) pr.rings.push_back(std::move(r));
    };
    readRing(poly->getExteriorRing());
    int nh=poly->getNumInteriorRings();
    for(int k=0;k<nh && !a.full();++k) readRing(poly->getInteriorRing(k));
    if(!pr.rings.empty()) a.prims.push_back(std::move(pr));
}

void appendGeom(const OGRGeometry* g, int depth, Accum& a){
    if(!g || depth>MAX_DEPTH || a.full()) return;
    OGRwkbGeometryType t = wkbFlatten(g->getGeometryType());
    switch(t){
        case wkbPoint:      addPoint(g->toPoint(), a); break;
        case wkbLineString: addCurve(g->toLineString(), 1, a); break;
        case wkbPolygon:    addPolygon(g->toPolygon(), a); break;
        case wkbMultiPoint:
        case wkbMultiLineString:
        case wkbMultiPolygon:
        case wkbGeometryCollection: {
            const OGRGeometryCollection* gc = g->toGeometryCollection();
            int n = gc->getNumGeometries();
            for(int i=0;i<n && !a.full();++i) appendGeom(gc->getGeometryRef(i), depth+1, a);
            break;
        }
        default:
            // Curved geometry (CircularString / CurvePolygon / MultiCurve …): linearise.
            // getLinearGeometry() returns a NEW geometry the caller must delete.
            if(OGR_GT_IsCurve(t) || g->hasCurveGeometry(TRUE)){
                OGRGeometry* lin = g->getLinearGeometry();
                if(lin){ appendGeom(lin, depth+1, a); delete lin; }
            }
            break;
    }
}

void ensureRegistered(){
    static std::atomic<bool> done{false};
    if(!done.exchange(true)) GDALAllRegister();   // register drivers once
}

} // anonymous namespace

bool ogrAvailable(){ return true; }

QString ogrToSvg(const QString& path){
    if(path.isEmpty()) return {};
    ensureRegistered();

    GDALDataset* ds = static_cast<GDALDataset*>(
        GDALOpenEx(path.toUtf8().constData(),
                   GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr));
    if(!ds) return {};

    Accum a;
    long long features = 0;
    int nLayers = ds->GetLayerCount();
    for(int li=0; li<nLayers && !a.full() && features<MAX_FEATURES; ++li){
        OGRLayer* L = ds->GetLayer(li);
        if(!L) continue;
        L->ResetReading();
        OGRFeature* f;
        while(features<MAX_FEATURES && !a.full() && (f = L->GetNextFeature())){
            const OGRGeometry* g = f->GetGeometryRef();     // owned by feature — do NOT free
            if(g) appendGeom(g, 0, a);
            OGRFeature::DestroyFeature(f);                  // free every feature
            features++;
        }
    }
    GDALClose(ds);                       // free dataset on the success path

    if(a.prims.empty() || !a.haveBounds) return {};
    if(!(std::isfinite(a.minX) && std::isfinite(a.minY) &&
         std::isfinite(a.maxX) && std::isfinite(a.maxY))) return {};

    double W = a.maxX - a.minX, H = a.maxY - a.minY;
    if(!(W>0)) W = 1.0;                   // degenerate (single point / vertical extent)
    if(!(H>0)) H = 1.0;
    double span = std::max(W, H);
    double r  = span * 0.004; if(!(r>0)) r = 1.0;        // point radius
    double sw = span * 0.002; if(!(sw>0)) sw = 0.5;      // stroke width
    double maxY = a.maxY;                                 // screen_y = maxY - y  (Y-flip)

    // viewBox in screen space: x in [minX,maxX], screenY in [0,H]
    QString out;
    out.reserve(1<<16);
    out += "<svg xmlns='http://www.w3.org/2000/svg' viewBox='";
    out += num(a.minX); out += ' '; out += "0"; out += ' ';
    out += num(W);      out += ' '; out += num(H); out += "'>";

    const QString strokeW = num(sw);
    const QString rad     = num(r);

    for(const Prim& p : a.prims){
        if((long long)out.size() > MAX_OUTPUT) break;
        if(p.kind==0){                                   // point -> <circle>
            const Ring& rg = p.rings.front();
            out += "<circle cx='"; out += num(rg.x[0]);
            out += "' cy='";       out += num(maxY - rg.y[0]);
            out += "' r='";        out += rad;
            out += "' fill='#3778c2' stroke='#204a78' stroke-width='";
            out += strokeW; out += "'/>";
        } else if(p.kind==1){                            // line -> <polyline>
            const Ring& rg = p.rings.front();
            out += "<polyline fill='none' stroke='#204a78' stroke-width='";
            out += strokeW; out += "' points='";
            for(size_t i=0;i<rg.x.size();++i){
                if(i) out += ' ';
                out += num(rg.x[i]); out += ','; out += num(maxY - rg.y[i]);
            }
            out += "'/>";
        } else {                                         // polygon -> filled <path> (even-odd for holes)
            out += "<path fill='#8fb8e0' fill-rule='evenodd' stroke='#204a78' stroke-width='";
            out += strokeW; out += "' d='";
            for(const Ring& rg : p.rings){
                for(size_t i=0;i<rg.x.size();++i){
                    out += (i==0 ? 'M' : 'L');
                    out += num(rg.x[i]); out += ' '; out += num(maxY - rg.y[i]); out += ' ';
                }
                out += "Z ";
            }
            out += "'/>";
        }
    }
    out += "</svg>";
    if((long long)out.size() > MAX_OUTPUT + (1<<20)) return {};   // paranoia
    return out;
}

} // namespace xara

#else  // ---- HAVE_GDAL not defined: stub ----

namespace xara {
bool ogrAvailable(){ return false; }
QString ogrToSvg(const QString&){ return {}; }
}

#endif


// ============================ Graphviz (DOT graph description) ============================
#ifdef HAVE_GRAPHVIZ

// Graphviz public C API. Include cgraph first (defines Agraph_t) then gvc.
#include <graphviz/cgraph.h>
#include <graphviz/gvc.h>
#include <graphviz/gvplugin.h>

#include <string>
#include <type_traits>

extern "C" {
extern gvplugin_library_t gvplugin_dot_layout_LTX_library;
extern gvplugin_library_t gvplugin_core_LTX_library;
}

// Deduces gvRenderData's 5th parameter type (unsigned* before graphviz 12, size_t* after).
template <class R, class A0, class A1, class A2, class A3, class A4>
A4 gvRenderDataLenArg(R (*)(A0, A1, A2, A3, A4));


namespace xara {

// Hard cap on the rendered SVG so a pathological graph can't OOM the parser.
static const unsigned kMaxSvgBytes = 64u * 1024u * 1024u; // ~64 MB

// Reject absurd inputs up-front (agmemread would still try to parse them).
static const int kMaxDotBytes = 32 * 1024 * 1024; // 32 MB of DOT source

bool dotAvailable() { return true; }

QString dotToSvg(const QByteArray& data)
{
    if (data.isEmpty() || data.size() > kMaxDotBytes)
        return QString();

    // Copy into a NUL-terminated std::string; agmemread wants a C string and
    // this guards against a QByteArray that embeds NULs / isn't terminated.
    std::string src(data.constData(), static_cast<size_t>(data.size()));
    if (src.find('\0') != std::string::npos)
        src.resize(src.find('\0')); // agmemread reads to first NUL anyway

    // Preload the two plugins this needs instead of discovering them. gvContext() finds plugins through
    // a registry file (config8) that `dot -c` writes into /usr/lib as root; a distro upgrade that drops
    // the graphviz package but keeps its libraries leaves no registry, and then there is no layout
    // engine and no SVG renderer — DOT import silently produced nothing. Preloading is graphviz's own
    // embedding pattern and makes this independent of whatever the system has registered.
    static const lt_symlist_t kBuiltins[] = {
        { "gvplugin_dot_layout_LTX_library", &gvplugin_dot_layout_LTX_library },
        { "gvplugin_core_LTX_library",       &gvplugin_core_LTX_library },
        { nullptr, nullptr }
    };
    GVC_t* gvc = gvContextPlugins(kBuiltins, 0);
    if (!gvc)
        return QString();

    // agmemread takes a non-const char* in this headers' signature.
    Agraph_t* g = agmemread(const_cast<char*>(src.c_str()));
    if (!g) {                       // syntax error -> null
        gvFreeContext(gvc);
        return QString();
    }

    if (gvLayout(gvc, g, "dot") != 0) {
        // Layout failed: gvFreeLayout is NOT valid here, but graph+context are.
        agclose(g);
        gvFreeContext(gvc);
        return QString();
    }

    char* out = nullptr;
    // graphviz widened gvRenderData's length out-param from `unsigned*` to `size_t*` (12.x), so a
    // hardcoded type stops compiling the moment the distro bumps the package. Deduce it from the
    // declaration instead and the call builds against either.
    using GvLen = std::remove_pointer_t<decltype(gvRenderDataLenArg(&gvRenderData))>;
    GvLen len = 0;
    int rc = gvRenderData(gvc, g, "svg", &out, &len);

    QString svg;
    if (rc == 0 && out && len > 0 && len <= kMaxSvgBytes) {
        svg = QString::fromUtf8(out, static_cast<int>(len));
    }
    // out is allocated by gvRenderData even on some partial paths — always free.
    if (out)
        gvFreeRenderData(out);

    gvFreeLayout(gvc, g);
    agclose(g);
    gvFreeContext(gvc);

    return svg;
}

} // namespace xara

#else // !HAVE_GRAPHVIZ  ---- stub ----

namespace xara {

bool dotAvailable() { return false; }

QString dotToSvg(const QByteArray& /*data*/) { return QString(); }

} // namespace xara

#endif // HAVE_GRAPHVIZ
