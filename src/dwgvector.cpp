// dwgvector.cpp — AutoCAD DWG import via GNU LibreDWG. See dwgvector.h.
//
// Strategy: libredwg parses the DWG into an in-memory object model; we walk the MODEL-SPACE entity list and
// re-emit each supported 2D entity (LINE / CIRCLE / ARC / LWPOLYLINE / 2D POLYLINE) as DXF group-code text,
// then feed that to the project's existing importDxf(). This deliberately reuses importDxf's coordinate
// handling (y-flip, bbox-normalisation), its non-finite/robustness guards, and its Shape construction —
// keeping the DWG binding thin and all the geometry logic in one audited place.
//
// Scope: the same 2D primitive set importDxf understands. TEXT/MTEXT, INSERT (block references), ELLIPSE,
// SPLINE, HATCH, DIMENSION and 3D solids are not yet emitted (they'd need importDxf support first) — a DWG
// using only those imports empty rather than wrong. Coordinates are taken in WCS/plan (OCS extrusion is not
// applied), which matches the common 2D-drawing case.
#include "dwgvector.h"
#include "prepress.h"     // importDxf
#include <QByteArray>
#include <QFile>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <QPointF>
#include <QVector>
#include <QString>
#include <QRegularExpression>

#ifdef HAVE_LIBREDWG
#include <dwg.h>
#include <dwg_api.h>

namespace {
// Append one DXF group-code line pair: "<code>\n<value>\n". Values use %g with generous precision so large
// CAD coordinates round-trip through importDxf's QString::toDouble() without loss.
inline void gc(QByteArray& o, int code, double v){
    o += QByteArray::number(code); o += '\n';
    o += QByteArray::number(v, 'g', 15); o += '\n';
}
inline void gc(QByteArray& o, int code, const char* v){
    o += QByteArray::number(code); o += '\n';
    o += v; o += '\n';
}
inline bool finite2(double a, double b){ return std::isfinite(a) && std::isfinite(b); }
} // namespace

namespace xara {

bool dwgAvailable(){ return true; }

Document dwgToDocument(const QString& path){
    Document empty;
    Dwg_Data dwg;
    memset(&dwg, 0, sizeof(dwg));

    const QByteArray fn = QFile::encodeName(path);
    const int err = dwg_read_file(fn.constData(), &dwg);
    // dwg_read_file returns an error bitmask; anything at/above DWG_ERR_CRITICAL means the model is unusable.
    if (err >= DWG_ERR_CRITICAL) { dwg_free(&dwg); return empty; }

    Dwg_Object_Ref* mspace = dwg_model_space_ref(&dwg);
    if (!mspace || !mspace->obj) { dwg_free(&dwg); return empty; }

    QByteArray dxf;
    dxf.reserve(1 << 16);
    dxf += "0\nSECTION\n2\nENTITIES\n";

    // Tessellation helpers. importDxf (prepress.cpp) understands only LINE/CIRCLE/ARC/(LW)POLYLINE with
    // no bulges, so curved DWG geometry is FLATTENED here into LWPOLYLINE vertices rather than emitted
    // as DXF groups the reader would silently drop. A global vertex budget keeps a hostile file from
    // turning tessellation into a memory bomb.
    qint64 vbudget = 4000000;
    auto putPt = [&](QByteArray& out, double x, double y)->bool{
        if(vbudget <= 0 || !finite2(x,y)) return false;
        gc(out, 10, x); gc(out, 20, y); --vbudget; return true;
    };
    // DWG strings: R2007 and later store UTF-16LE in the same char* field (BITCODE_T is BITCODE_TV).
    // Newlines/control characters must never reach the synthesised DXF — a bare '\n' inside a group-1
    // value would be read back as a group code and corrupt the entity stream.
    auto dwgText = [](const Dwg_Data& dd, const char* raw)->QString{
        if(!raw) return {};
        QString t = (dd.header.version >= R_2007)
            ? QString::fromUtf16(reinterpret_cast<const char16_t*>(raw))
            : QString::fromUtf8(raw);
        t.replace(QChar('\r'), QChar(' '));
        t.replace(QChar('\n'), QChar(' '));
        t.remove(QRegularExpression("[\\x00-\\x1F]"));
        if(t.size() > 4096) t.truncate(4096);
        return t.trimmed();
    };

    // Arc through a DXF/DWG "bulge" (tan of a quarter of the included angle) between P0 and P1.
    // Emits only the INTERIOR samples; the endpoints are written by the vertex walk itself.
    auto bulgeArc = [&](QByteArray& out, double x0,double y0,double x1,double y1,double b){
        if(!std::isfinite(b) || qAbs(b) < 1e-9 || !finite2(x0,y0) || !finite2(x1,y1)) return;
        const double dx=x1-x0, dy=y1-y0;
        const double mx=(x0+x1)/2.0, my=(y0+y1)/2.0;
        const double f=(1.0-b*b)/(4.0*b);
        const double cx=mx - f*dy, cy=my + f*dx;
        const double r=std::hypot(x0-cx, y0-cy);
        if(!std::isfinite(r) || r<=0) return;
        const double a0=std::atan2(y0-cy, x0-cx);
        const double sweep=4.0*std::atan(b);
        if(!std::isfinite(sweep)) return;
        const int N=qBound(2, int(std::ceil(qAbs(sweep)/(M_PI/16.0))), 128);
        for(int i=1;i<N;++i){ const double t=a0 + sweep*(double(i)/N);
            if(!putPt(out, cx + r*std::cos(t), cy + r*std::sin(t))) return; }
    };

    // Bound the walk: cap the number of emitted entities (a corrupt/hostile file must not exhaust memory) and
    // the loop iterations (guards against a cyclic owned-entity list).
    const int   kMaxEntities = 500000;
    const qint64 kMaxIters   = 5000000;
    int    emitted = 0;
    qint64 iters   = 0;

    for (Dwg_Object* obj = get_first_owned_entity(mspace->obj);
         obj && iters < kMaxIters && emitted < kMaxEntities;
         obj = get_next_owned_entity(mspace->obj, obj)) {
        ++iters;
        if (obj->supertype != DWG_SUPERTYPE_ENTITY || !obj->tio.entity) continue;

        switch (obj->fixedtype) {
        case DWG_TYPE_LINE: {
            Dwg_Entity_LINE* l = obj->tio.entity->tio.LINE;
            if (!l || !finite2(l->start.x, l->start.y) || !finite2(l->end.x, l->end.y)) break;
            gc(dxf, 0, "LINE");
            gc(dxf, 10, l->start.x); gc(dxf, 20, l->start.y);
            gc(dxf, 11, l->end.x);   gc(dxf, 21, l->end.y);
            ++emitted;
        } break;
        case DWG_TYPE_CIRCLE: {
            Dwg_Entity_CIRCLE* c = obj->tio.entity->tio.CIRCLE;
            if (!c || !finite2(c->center.x, c->center.y) || !std::isfinite(c->radius) || c->radius <= 0) break;
            gc(dxf, 0, "CIRCLE");
            gc(dxf, 10, c->center.x); gc(dxf, 20, c->center.y); gc(dxf, 40, c->radius);
            ++emitted;
        } break;
        case DWG_TYPE_ARC: {
            Dwg_Entity_ARC* a = obj->tio.entity->tio.ARC;
            if (!a || !finite2(a->center.x, a->center.y) || !std::isfinite(a->radius) || a->radius <= 0
                || !finite2(a->start_angle, a->end_angle)) break;
            gc(dxf, 0, "ARC");
            gc(dxf, 10, a->center.x); gc(dxf, 20, a->center.y); gc(dxf, 40, a->radius);
            gc(dxf, 50, a->start_angle * 180.0 / M_PI);   // libredwg arc angles are radians; DXF wants degrees
            gc(dxf, 51, a->end_angle   * 180.0 / M_PI);
            ++emitted;
        } break;
        case DWG_TYPE_LWPOLYLINE: {
            Dwg_Entity_LWPOLYLINE* p = obj->tio.entity->tio.LWPOLYLINE;
            if (!p || !p->points || p->num_points == 0) break;
            const bool closed = (p->flag & 512) != 0;     // DWG flag bit 512 = closed
            gc(dxf, 0, "LWPOLYLINE");
            gc(dxf, 70, closed ? "1" : "0");
            for (BITCODE_BL k = 0; k < p->num_points; ++k) {
                const double x = p->points[k].x, y = p->points[k].y;
                if (!finite2(x, y)) continue;
                if(!putPt(dxf, x, y) && vbudget <= 0) break;   // out of budget stops; a bad vertex just skips
                // A bulge on segment k curves the span to the NEXT vertex (wrapping when closed).
                const bool haveNext = (k + 1 < p->num_points) || closed;
                if (p->bulges && k < p->num_bulges && haveNext) {
                    const BITCODE_BL nx = (k + 1 < p->num_points) ? k + 1 : 0;
                    bulgeArc(dxf, x, y, p->points[nx].x, p->points[nx].y, p->bulges[k]);
                }
            }
            ++emitted;
        } break;
        case DWG_TYPE_POLYLINE_2D: {
            Dwg_Entity_POLYLINE_2D* p = obj->tio.entity->tio.POLYLINE_2D;
            if (!p) break;
            int gerr = 0;
            const BITCODE_RL n = dwg_object_polyline_2d_get_numpoints(obj, &gerr);
            if (gerr || n == 0) break;
            dwg_point_2d* pts = dwg_object_polyline_2d_get_points(obj, &gerr);
            // get_points may still return a calloc'd buffer WITH *error set (a dangling/mistyped vertex handle in
            // a malformed-but-loadable file) — free it before bailing or it leaks.
            if (gerr || !pts) { free(pts); break; }
            gc(dxf, 0, "POLYLINE");
            gc(dxf, 70, (p->flag & 1) ? "1" : "0");        // POLYLINE_2D flag bit 1 = closed
            for (BITCODE_RL k = 0; k < n; ++k) {
                if (!finite2(pts[k].x, pts[k].y)) continue;
                gc(dxf, 0, "VERTEX");
                gc(dxf, 10, pts[k].x); gc(dxf, 20, pts[k].y);
            }
            gc(dxf, 0, "SEQEND");
            free(pts);
            ++emitted;
        } break;
        case DWG_TYPE_ELLIPSE: {
            Dwg_Entity_ELLIPSE* e = obj->tio.entity->tio.ELLIPSE;
            if (!e || !finite2(e->center.x, e->center.y) || !finite2(e->sm_axis.x, e->sm_axis.y)) break;
            const double majorR = std::hypot(e->sm_axis.x, e->sm_axis.y);
            if (!std::isfinite(majorR) || majorR <= 0) break;
            const double ux = e->sm_axis.x/majorR, uy = e->sm_axis.y/majorR;     // major-axis unit vector
            const double ratio = std::isfinite(e->axis_ratio) ? qBound(1e-9, (double)e->axis_ratio, 1.0) : 1.0;
            const double minorR = majorR * ratio;
            double t0 = e->start_angle, t1 = e->end_angle;
            if (!finite2(t0, t1)) { t0 = 0; t1 = 2*M_PI; }
            // Clamp in the DOUBLE domain before any cast: t1-t0 can be a huge finite negative (or
            // overflow to infinity), and a single "+= 2*M_PI" does not bring it back into range —
            // int(ceil(...)) on that value is an out-of-range cast (UB; the UBSan build traps).
            double sweep = t1 - t0;
            if (!std::isfinite(sweep)) sweep = 2*M_PI;
            if (sweep <= 0) sweep = std::fmod(sweep, 2*M_PI) + 2*M_PI;
            sweep = qBound(1e-9, sweep, 2*M_PI);
            const bool closed = sweep >= 2*M_PI - 1e-6;
            const double steps = std::ceil(sweep/(M_PI/32.0));
            const int N = qBound(16, int(qBound(1.0, steps, 512.0)), 512);
            gc(dxf, 0, "LWPOLYLINE");
            gc(dxf, 70, closed ? "1" : "0");
            for (int i = 0; i <= N; ++i) {
                if (closed && i == N) break;                                     // don't duplicate the seam
                const double t = t0 + sweep*(double(i)/N);
                const double c = std::cos(t)*majorR, sN = std::sin(t)*minorR;
                if(!putPt(dxf, e->center.x + c*ux - sN*uy, e->center.y + c*uy + sN*ux)) break;
            }
            ++emitted;
        } break;
        case DWG_TYPE_SPLINE: {
            Dwg_Entity_SPLINE* sp = obj->tio.entity->tio.SPLINE;
            if (!sp) break;
            QVector<QPointF> out;
            // Do NOT trust sp->degree/sp->scenario (a fit-point spline reads back with both zero);
            // branch on which arrays are actually populated.
            if (sp->num_ctrl_pts > 0 && sp->ctrl_pts && sp->knots
                && sp->num_knots >= sp->num_ctrl_pts + 2) {                       // NURBS: de Boor
                const int nc = int(sp->num_ctrl_pts), nk = int(sp->num_knots);
                const int p = qBound(1, nk - nc - 1, 7);
                const double u0 = sp->knots[p], u1 = sp->knots[nk-1-p];
                if (std::isfinite(u0) && std::isfinite(u1) && u1 > u0) {
                    const int N = qBound(32, (nc > 64 ? 1024 : nc*16), 1024);
                    for (int i = 0; i <= N; ++i) {
                        const double u = u0 + (u1-u0)*(double(i)/N);
                        int k = p; while (k < nc-1 && sp->knots[k+1] <= u) ++k;
                        QVector<double> dx(p+1), dy(p+1), dw(p+1);
                        bool bad=false;
                        for (int j = 0; j <= p; ++j) {
                            const int idx = k - p + j;
                            if (idx < 0 || idx >= nc) { bad=true; break; }
                            const double w = (sp->ctrl_pts[idx].w > 0) ? sp->ctrl_pts[idx].w : 1.0;
                            dx[j]=sp->ctrl_pts[idx].x*w; dy[j]=sp->ctrl_pts[idx].y*w; dw[j]=w;
                        }
                        if (bad) break;
                        for (int r = 1; r <= p; ++r)
                            for (int j = p; j >= r; --j) {
                                const double den = sp->knots[k+1+j-r] - sp->knots[k-p+j];
                                const double a2 = (den > 1e-12) ? (u - sp->knots[k-p+j])/den : 0.0;
                                dx[j]=(1-a2)*dx[j-1]+a2*dx[j]; dy[j]=(1-a2)*dy[j-1]+a2*dy[j]; dw[j]=(1-a2)*dw[j-1]+a2*dw[j];
                            }
                        if (dw[p] > 1e-12) out.push_back(QPointF(dx[p]/dw[p], dy[p]/dw[p]));
                    }
                }
            } else if (sp->num_fit_pts >= 2 && sp->fit_pts) {                     // Catmull-Rom through fit points
                const int n = int(sp->num_fit_pts);
                auto FP=[&](int i)->QPointF{ i=qBound(0,i,n-1); return QPointF(sp->fit_pts[i].x, sp->fit_pts[i].y); };
                for (int i = 0; i < n-1 && out.size() < vbudget && out.size() < 200000; ++i) {
                    const QPointF p0=FP(i-1), p1=FP(i), p2=FP(i+1), p3=FP(i+2);
                    for (int j = 0; j < 16; ++j) {
                        const double t=double(j)/16.0, t2=t*t, t3=t2*t;
                        out.push_back(0.5*((2*p1) + (-p0+p2)*t + (2*p0-5*p1+4*p2-p3)*t2 + (-p0+3*p1-3*p2+p3)*t3));
                    }
                }
                out.push_back(FP(n-1));
            }
            if (out.size() < 2) break;
            gc(dxf, 0, "LWPOLYLINE");
            gc(dxf, 70, (sp->closed_b ? "1" : "0"));
            for (const QPointF& q : out) if(!putPt(dxf, q.x(), q.y())) break;
            ++emitted;
        } break;
        case DWG_TYPE_TEXT: {
            Dwg_Entity_TEXT* t = obj->tio.entity->tio.TEXT;
            if (!t || !t->text_value || !finite2(t->ins_pt.x, t->ins_pt.y)) break;   // ins_pt is 2D here (no .z)
            const QString txt = dwgText(dwg, (const char*)t->text_value);
            if (txt.isEmpty()) break;
            gc(dxf, 0, "TEXT");
            gc(dxf, 10, t->ins_pt.x); gc(dxf, 20, t->ins_pt.y);
            if (std::isfinite(t->height) && t->height > 0) gc(dxf, 40, t->height);
            if (std::isfinite(t->rotation)) gc(dxf, 50, t->rotation * 180.0 / M_PI);
            gc(dxf, 1, txt.toUtf8().constData());
            ++emitted;
        } break;
        case DWG_TYPE_MTEXT: {
            Dwg_Entity_MTEXT* t = obj->tio.entity->tio.MTEXT;
            if (!t || !t->text || !finite2(t->ins_pt.x, t->ins_pt.y)) break;
            const QString txt = dwgText(dwg, (const char*)t->text);
            if (txt.isEmpty()) break;
            gc(dxf, 0, "MTEXT");
            gc(dxf, 10, t->ins_pt.x); gc(dxf, 20, t->ins_pt.y);
            if (std::isfinite(t->text_height) && t->text_height > 0) gc(dxf, 40, t->text_height);
            gc(dxf, 1, txt.toUtf8().constData());
            ++emitted;
        } break;
        default:
            break;   // unsupported entity types are silently skipped (see header)
        }
    }

    dwg_free(&dwg);
    if (emitted == 0) return empty;
    dxf += "0\nENDSEC\n0\nEOF\n";
    return importDxf(dxf);
}

} // namespace xara

#else   // !HAVE_LIBREDWG — stub so the app still builds/links without libredwg.

namespace xara {
bool dwgAvailable(){ return false; }
Document dwgToDocument(const QString&){ return Document(); }
}

#endif
