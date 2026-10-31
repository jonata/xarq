#include "vecextra.h"
#include <QXmlStreamReader>
#include <QLatin1Char>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QJsonParseError>
#include <QStringList>
#include <QHash>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <vector>
#include <algorithm>
#include <zlib.h>

namespace xara {
namespace {
using u8=uint8_t; using u16=uint16_t; using u32=uint32_t; using i64=int64_t;
inline u16 rd16be(const u8* p){ return (u16(p[0])<<8)|u16(p[1]); }
inline u16 rd16le(const u8* p){ return (u16(p[1])<<8)|u16(p[0]); }
inline u32 rd32be(const u8* p){ return (u32(p[0])<<24)|(u32(p[1])<<16)|(u32(p[2])<<8)|u32(p[3]); }
inline u32 rd32le(const u8* p){ return (u32(p[3])<<24)|(u32(p[2])<<16)|(u32(p[1])<<8)|u32(p[0]); }
inline uint64_t rd64be(const u8* p){ uint64_t v=0; for(int i=0;i<8;i++) v=(v<<8)|(uint64_t)p[i]; return v; }

// ---- minimal ZIP reader (central-directory walk; stored + raw-deflate) for .lottie/.sketch ----
inline const u8* zipFindEOCD(const u8* p, i64 n){
    if(n < 22) return nullptr;
    i64 back = n < 65557 ? n : 65557;
    for(i64 i = n - 22; i >= n - back; --i){
        if(i < 0) break;
        if(p[i]==0x50 && p[i+1]==0x4b && p[i+2]==0x05 && p[i+3]==0x06) return p+i;
    }
    return nullptr;
}
template<class F> inline void zipWalkCD(const u8* p, i64 n, F fn){
    const u8* e = zipFindEOCD(p,n); if(!e) return;
    i64 eocdOff = e - p; if(eocdOff + 20 > n) return;
    u32 cdOff = rd32le(e+16), cdSize = rd32le(e+12); int count = rd16le(e+10);
    if((i64)cdOff > n) return;
    i64 pos = cdOff, end = (i64)cdOff + (i64)cdSize; if(end > eocdOff) end = eocdOff;
    int seen = 0;
    while(seen < count && pos + 46 <= end && pos + 46 <= n){
        if(!(p[pos]==0x50 && p[pos+1]==0x4b && p[pos+2]==0x01 && p[pos+3]==0x02)) break;
        int method = rd16le(p+pos+10);
        u32 comp   = rd32le(p+pos+20);
        int nameLen = rd16le(p+pos+28), extraLen = rd16le(p+pos+30), cmtLen = rd16le(p+pos+32);
        u32 localOff = rd32le(p+pos+42);
        i64 nameStart = pos + 46; if(nameStart + nameLen > n) break;
        QString nm = QString::fromUtf8(reinterpret_cast<const char*>(p+nameStart), nameLen);
        fn(nm, method, comp, localOff);
        pos = nameStart + (i64)nameLen + extraLen + cmtLen; seen++;
    }
}
inline QStringList xqUnzipList(const u8* p, i64 n){
    QStringList out;
    zipWalkCD(p, n, [&](const QString& nm, int, u32, u32){ if(out.size() < 100000) out << nm; });
    return out;
}
inline bool xqUnzip(const u8* p, i64 n, const QString& name, QByteArray& out){
    out.clear();
    if(!p || n < 22 || name.isEmpty()) return false;
    const QString base = name.section('/', -1);
    int foundMethod = -1; u32 foundComp = 0, foundLocal = 0; bool found=false;
    zipWalkCD(p, n, [&](const QString& nm, int method, u32 comp, u32 localOff){
        if(found) return;
        if(nm == name || nm.compare(name, Qt::CaseInsensitive)==0 ||
           nm.section('/', -1).compare(base, Qt::CaseInsensitive)==0){
            found=true; foundMethod=method; foundComp=comp; foundLocal=localOff;
        }
    });
    if(!found) return false;
    i64 lh = (i64)foundLocal;
    if(lh < 0 || lh + 30 > n) return false;
    if(!(p[lh]==0x50 && p[lh+1]==0x4b && p[lh+2]==0x03 && p[lh+3]==0x04)) return false;
    int lnameLen = rd16le(p+lh+26), lextraLen = rd16le(p+lh+28);
    i64 dataStart = lh + 30 + (i64)lnameLen + lextraLen;
    i64 comp = (i64)foundComp;
    if(comp < 0 || dataStart < 0 || dataStart + comp > n) return false;
    const size_t kMaxOut = 128u*1024u*1024u;
    if(foundMethod == 0){ if((size_t)comp > kMaxOut) return false;
        out = QByteArray(reinterpret_cast<const char*>(p+dataStart), (int)comp); return true; }
    if(foundMethod != 8) return false;
    z_stream s; std::memset(&s,0,sizeof(s));
    if(inflateInit2(&s, -15) != Z_OK) return false;
    s.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(p+dataStart));
    s.avail_in = (uInt)((comp > (i64)0x7fffffff) ? 0x7fffffff : comp);
    const size_t CH = 1u<<16; std::vector<char> buf(CH);
    int ret=Z_OK; bool ok=true;
    do{
        s.next_out = reinterpret_cast<Bytef*>(buf.data()); s.avail_out=(uInt)CH;
        ret = inflate(&s, Z_NO_FLUSH);
        // Anything other than progress (Z_OK) or completion (Z_STREAM_END) is fatal here — we feed all
        // input up front, so Z_BUF_ERROR means the stream is truncated. Treating only the "hard" error
        // codes as fatal would spin forever on Z_BUF_ERROR (no progress, loop condition still true).
        if(ret!=Z_OK && ret!=Z_STREAM_END){ ok=false; break; }
        size_t got = CH - s.avail_out;
        if(got){ if((size_t)out.size()+got > kMaxOut){ ok=false; break; } out.append(buf.data(),(int)got); }
    } while(ret != Z_STREAM_END);
    inflateEnd(&s);
    if(!ok || ret!=Z_STREAM_END){ out.clear(); return false; }
    return true;
}
} // anon

// file-scope helper: read an 8-byte IEEE-754 double, little-endian
static inline double shpRdF64le(const u8* q){
    uint64_t v=0; for(int i=0;i<8;i++) v |= (uint64_t)q[i] << (8*i);
    double d; memcpy(&d,&v,sizeof(d)); return d;
}

// ESRI Shapefile (.shp) geometry -> SVG. Spec: ESRI Shapefile Technical
// Description (July 1998), pp.2-4 (main file header) & pp.4-9 (record layout).
QString shapefileToSvg(const u8* p, i64 n){
    if(!p || n < 100) return QString();               // no room for the 100-byte header
    // --- main file header (big-endian file code, little-endian body) ---
    if(rd32be(p+0) != 9994u) return QString();         // @0  file code == 9994
    if(rd32le(p+28) != 1000u) return QString();        // @28 version == 1000
    // @32 shapeType (LE); @36..67 bbox Xmin,Ymin,Xmax,Ymax (LE doubles)
    double hXmin = shpRdF64le(p+36);
    double hYmin = shpRdF64le(p+44);
    double hXmax = shpRdF64le(p+52);
    double hYmax = shpRdF64le(p+60);
    // Flip reference: screen y = Ymax - y  (geographic Y points up).
    double ymaxRef = std::isfinite(hYmax) ? hYmax : 0.0;

    // stroke/point sizing from header extents (known up-front)
    double hW = (std::isfinite(hXmax)&&std::isfinite(hXmin)) ? (hXmax-hXmin) : 0.0;
    double hH = (std::isfinite(hYmax)&&std::isfinite(hYmin)) ? (hYmax-hYmin) : 0.0;
    double span = std::max(std::fabs(hW), std::fabs(hH));
    if(!(span > 0.0) || !std::isfinite(span)) span = 1.0;
    double sw = span / 500.0; if(!(sw>0.0)) sw = 1.0;
    double pr = sw * 1.5;                               // point/circle radius

    QString swS = QString::number(sw,'f',4);
    QString prS = QString::number(pr,'f',4);

    // transformed-geometry bbox (accumulated while emitting)
    bool have=false; double gxmin=0,gymin=0,gxmax=0,gymax=0;
    auto acc=[&](double x,double y){
        if(!std::isfinite(x)||!std::isfinite(y)) return;
        if(!have){ gxmin=gxmax=x; gymin=gymax=y; have=true; }
        else { if(x<gxmin)gxmin=x; if(x>gxmax)gxmax=x; if(y<gymin)gymin=y; if(y>gymax)gymax=y; }
    };

    QString geo;
    const i64 MAX_PTS  = 4000000;                       // OOM guard: total emitted vertices
    const i64 MAX_ELEM = 500000;                         // OOM guard: total emitted elements
    i64 totalPts = 0, totalElem = 0;

    i64 off = 100;                                       // records begin at byte 100
    while(off + 8 <= n){
        // record header: recordNumber i32 BE @off, contentLength i32 BE @off+4 (16-bit words)
        u32 clen = rd32be(p+off+4);
        i64 clenB = (i64)clen * 2;
        i64 rs = off + 8;                                // record content start
        if(clenB < 4) break;                             // content must hold at least the shapeType
        if(rs + clenB > n) break;                        // truncated record -> stop
        i64 rend = rs + clenB;                           // one-past-end of this record's content
        const u8* c = p + rs;
        u32 st = rd32le(c);                              // shape type (LE) at content start

        if(st==1 || st==11 || st==21){
            // Point / PointZ / PointM : x @ +4, y @ +12 (LE doubles)
            if(rs + 20 <= rend && rs + 20 <= n){
                double x = shpRdF64le(c+4);
                double y = ymaxRef - shpRdF64le(c+12);
                if(std::isfinite(x)&&std::isfinite(y) && totalElem<MAX_ELEM){
                    acc(x,y);
                    geo += QStringLiteral("<circle cx='%1' cy='%2' r='%3' fill='#cccccc' stroke='#000000' stroke-width='%4'/>")
                            .arg(QString::number(x,'f',3), QString::number(y,'f',3), prS, swS);
                    totalElem++; totalPts++;
                }
            }
        }
        else if(st==8 || st==18 || st==28){
            // MultiPoint : box[4 dbl] @+4, numPoints i32 @+36, points @+40
            if(rs + 40 <= rend && rs + 40 <= n){
                int32_t np = (int32_t)rd32le(c+36);
                i64 base = rs + 40;
                if(np > 0 && base + (i64)np*16 <= rend && base + (i64)np*16 <= n){
                    for(int i=0;i<np;i++){
                        if(totalPts>=MAX_PTS || totalElem>=MAX_ELEM) break;
                        const u8* pp = p + base + (i64)i*16;
                        double x = shpRdF64le(pp);
                        double y = ymaxRef - shpRdF64le(pp+8);
                        if(!std::isfinite(x)||!std::isfinite(y)) continue;
                        acc(x,y);
                        geo += QStringLiteral("<circle cx='%1' cy='%2' r='%3' fill='#cccccc' stroke='#000000' stroke-width='%4'/>")
                                .arg(QString::number(x,'f',3), QString::number(y,'f',3), prS, swS);
                        totalElem++; totalPts++;
                    }
                }
            }
        }
        else if(st==3 || st==5 || st==13 || st==15 || st==23 || st==25){
            // PolyLine(3)/Polygon(5) & Z/M variants:
            // box[4 dbl] @+4, numParts i32 @+36, numPoints i32 @+40,
            // parts[numParts] i32 @+44, points[numPoints] @ +44+numParts*4
            bool polygon = (st==5 || st==15 || st==25);
            if(rs + 44 <= rend && rs + 44 <= n){
                int32_t nParts  = (int32_t)rd32le(c+36);
                int32_t nPoints = (int32_t)rd32le(c+40);
                if(nParts > 0 && nPoints > 0 &&
                   nParts <= 1000000 && (i64)nPoints <= MAX_PTS){
                    i64 partsOff = rs + 44;
                    i64 ptsOff   = partsOff + (i64)nParts*4;
                    i64 ptsEnd   = ptsOff + (i64)nPoints*16;
                    if(ptsOff <= rend && ptsOff <= n && ptsEnd <= rend && ptsEnd <= n){
                        for(int part=0; part<nParts; part++){
                            if(totalElem>=MAX_ELEM || totalPts>=MAX_PTS) break;
                            int32_t startIdx = (int32_t)rd32le(p + partsOff + (i64)part*4);
                            int32_t endIdx   = (part+1<nParts)
                                ? (int32_t)rd32le(p + partsOff + (i64)(part+1)*4)
                                : nPoints;
                            if(startIdx < 0 || endIdx > nPoints || startIdx >= endIdx) continue;
                            QString d;                       // path data (polygon)
                            QString poly;                    // polyline points
                            int emittedInPart = 0;
                            for(int k=startIdx; k<endIdx; k++){
                                if(totalPts>=MAX_PTS) break;
                                const u8* pp = p + ptsOff + (i64)k*16;
                                double x = shpRdF64le(pp);
                                double y = ymaxRef - shpRdF64le(pp+8);
                                if(!std::isfinite(x)||!std::isfinite(y)) continue;
                                acc(x,y); totalPts++;
                                QString xs = QString::number(x,'f',3);
                                QString ys = QString::number(y,'f',3);
                                if(polygon){
                                    d += (emittedInPart==0 ? QStringLiteral("M") : QStringLiteral("L"));
                                    d += xs; d += QLatin1Char(' '); d += ys; d += QLatin1Char(' ');
                                } else {
                                    if(emittedInPart) poly += QLatin1Char(' ');
                                    poly += xs; poly += QLatin1Char(','); poly += ys;
                                }
                                emittedInPart++;
                            }
                            if(emittedInPart < 2) continue;   // nothing drawable
                            if(polygon){
                                d += QStringLiteral("Z");
                                geo += QStringLiteral("<path d='%1' fill='#cccccc' stroke='#000000' stroke-width='%2'/>")
                                        .arg(d, swS);
                            } else {
                                geo += QStringLiteral("<polyline points='%1' fill='none' stroke='#000000' stroke-width='%2'/>")
                                        .arg(poly, swS);
                            }
                            totalElem++;
                        }
                    }
                }
            }
        }
        // Null shape (0) and anything else: skip.
        off = rend;                                      // strictly advances (clenB>=4)
    }

    if(geo.isEmpty()) return QString();                  // nothing decodable -> fall through

    // viewBox from transformed geometry extents (fall back to header if needed)
    double vx, vy, vw, vh;
    if(have && gxmax>gxmin && gymax>gymin){
        vx=gxmin; vy=gymin; vw=gxmax-gxmin; vh=gymax-gymin;
    } else if(std::isfinite(hXmin)&&hW>0&&hH>0){
        vx=hXmin; vy=0.0; vw=hW; vh=hH;                  // transformed header box: y in [0,H]
    } else {
        vx=(have?gxmin:0.0); vy=(have?gymin:0.0); vw=1.0; vh=1.0;
    }
    QString head = QStringLiteral("<svg xmlns='http://www.w3.org/2000/svg' viewBox='%1 %2 %3 %4'>")
        .arg(QString::number(vx,'f',3), QString::number(vy,'f',3),
             QString::number(vw,'f',3), QString::number(vh,'f',3));
    return head + geo + QStringLiteral("</svg>");
}

// GPX (GPS Exchange Format) 1.1 importer -> SVG document string.
// Spec: http://www.topografix.com/GPX/1/1 (GPX 1.1 schema). Also compatible with GPX 1.0.
// Structure (all elements unprefixed in the default namespace):
//   <gpx> root.
//   <wpt lat="F" lon="F">     : waypoint      -> emitted as <circle fill='#f00'>
//   <rte><rtept lat lon>...    : route         -> one <polyline> per <rte>
//   <trk><trkseg><trkpt lat lon>...</trkseg>...: track -> one <polyline> per <trkseg>
// lat/lon are decimal degrees (WGS84). lat in [-90,90], lon in [-180,180].
// Geographic Y (lat) increases NORTH/up, SVG y increases DOWN -> FLIP: y = (maxLat - lat).
// x = lon directly. viewBox = (minLon, 0, lonSpan, latSpan).
QString gpxToSvg(const u8* p, i64 n)
{
    if (!p || n < 5) return QString();

    // Cheap sniff: the buffer must mention "<gpx" somewhere near the start,
    // otherwise this is not our format (fall through fast, avoid parsing junk).
    {
        const i64 sniffLen = n < 4096 ? n : 4096;
        bool sawGpx = false;
        for (i64 i = 0; i + 4 <= sniffLen; ++i) {
            if (p[i] == '<' && (p[i+1]=='g'||p[i+1]=='G')
                && (p[i+2]=='p'||p[i+2]=='P') && (p[i+3]=='x'||p[i+3]=='X')) {
                sawGpx = true; break;
            }
        }
        if (!sawGpx) return QString();
    }

    // Point + polyline collection with a hard safety cap.
    const int kMaxPoints = 4000000; // reject/truncate hostile files (~ tens of MB of SVG max)
    const long long kMaxPolys = 1000000; // cap the polyline COUNT (empty segments add no points)

    struct Pt { double lon; double lat; };
    struct Poly { std::vector<Pt> pts; };

    std::vector<Poly> polys;   // tracks + routes (open polylines)
    std::vector<Pt>   wpts;    // standalone waypoints
    long long totalPts = 0;

    double minLon = 1e300, maxLon = -1e300;
    double minLat = 1e300, maxLat = -1e300;
    bool haveBounds = false;

    auto accumBounds = [&](double lon, double lat) {
        if (lon < minLon) minLon = lon;
        if (lon > maxLon) maxLon = lon;
        if (lat < minLat) minLat = lat;
        if (lat > maxLat) maxLat = lat;
        haveBounds = true;
    };

    // Parse lat/lon attributes of the current start element; returns false if invalid/out of range.
    // QXmlStreamReader handles all XML decoding, entity refs, encoding & bounds internally.
    QByteArray xml(reinterpret_cast<const char*>(p), int(n < (i64)0x7fffffff ? n : (i64)0x7fffffff));
    QXmlStreamReader xr(xml);
    xr.setNamespaceProcessing(false); // keep element names literal (GPX is normally unprefixed)

    Poly* cur = nullptr;  // current open track-seg / route being filled
    bool aborted = false;

    auto localName = [](const QStringView& qn) -> QStringView {
        int c = qn.lastIndexOf(QLatin1Char(':'));
        if (c < 0) return qn;
        return qn.mid(c + 1);
    };

    auto readLatLon = [&](const QXmlStreamAttributes& at, double& lon, double& lat) -> bool {
        bool okA = false, okB = false;
        // Attribute names are unprefixed per schema; match by localName defensively.
        QStringView latRef, lonRef;
        for (const QXmlStreamAttribute& a : at) {
            QStringView ln = a.qualifiedName();
            int c = ln.lastIndexOf(QLatin1Char(':'));
            if (c >= 0) ln = ln.mid(c + 1);
            if (ln == QLatin1String("lat")) latRef = a.value();
            else if (ln == QLatin1String("lon")) lonRef = a.value();
        }
        if (latRef.isEmpty() || lonRef.isEmpty()) return false;
        lat = latRef.toString().toDouble(&okA);
        lon = lonRef.toString().toDouble(&okB);
        if (!okA || !okB) return false;
        if (!std::isfinite(lat) || !std::isfinite(lon)) return false;
        if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) return false;
        return true;
    };

    while (!xr.atEnd()) {
        QXmlStreamReader::TokenType t = xr.readNext();
        if (xr.hasError()) break;
        if (t == QXmlStreamReader::StartElement) {
            QStringView name = localName(xr.qualifiedName());

            if (name == QLatin1String("trkseg") || name == QLatin1String("rte")) {
                // Begin a new open polyline. (A <rte> holds <rtept> directly;
                // a <trk> holds one or more <trkseg>, each a separate polyline.)
                // Cap the polyline COUNT too: a hostile file of millions of empty
                // <trkseg/> tags adds no points (so kMaxPoints never trips) but would
                // otherwise allocate an unbounded number of Poly structs (OOM/DoS).
                if ((long long)polys.size() >= kMaxPolys) { aborted = true; break; }
                polys.push_back(Poly());
                cur = &polys.back();
            }
            else if (name == QLatin1String("trkpt") || name == QLatin1String("rtept")) {
                double lon, lat;
                if (readLatLon(xr.attributes(), lon, lat)) {
                    if (totalPts >= kMaxPoints) { aborted = true; break; }
                    // A trkpt/rtept should be inside a seg/route; if malformed and
                    // none is open, start an implicit one so we still capture geometry.
                    if (!cur) {
                        if ((long long)polys.size() >= kMaxPolys) { aborted = true; break; }
                        polys.push_back(Poly()); cur = &polys.back();
                    }
                    cur->pts.push_back(Pt{lon, lat});
                    accumBounds(lon, lat);
                    ++totalPts;
                }
            }
            else if (name == QLatin1String("wpt")) {
                double lon, lat;
                if (readLatLon(xr.attributes(), lon, lat)) {
                    if (totalPts >= kMaxPoints) { aborted = true; break; }
                    wpts.push_back(Pt{lon, lat});
                    accumBounds(lon, lat);
                    ++totalPts;
                }
            }
        }
        else if (t == QXmlStreamReader::EndElement) {
            QStringView name = localName(xr.qualifiedName());
            if (name == QLatin1String("trkseg") || name == QLatin1String("rte")) {
                cur = nullptr; // close the current polyline
            }
        }
    }

    (void)aborted; // if we truncated, we still emit what we gathered (partial is fine)

    if (!haveBounds) return QString(); // no valid geometry -> not usable / not our format

    // Degenerate bbox guard: give a small non-zero span so viewBox stays valid.
    double lonSpan = maxLon - minLon;
    double latSpan = maxLat - minLat;
    if (!(lonSpan > 0.0)) lonSpan = 1e-4;
    if (!(latSpan > 0.0)) latSpan = 1e-4;
    double diag = std::sqrt(lonSpan*lonSpan + latSpan*latSpan);
    if (!std::isfinite(diag) || diag <= 0.0) diag = 1.0;

    // Stroke / marker sizes proportional to the data extent so it renders sensibly
    // regardless of whether the track spans a city block or a continent.
    double stroke = diag * 0.003; if (stroke <= 0.0) stroke = 1e-6;
    double wr     = diag * 0.006; if (wr     <= 0.0) wr     = 1e-6;

    auto num = [](double v) { return QString::number(v, 'f', 6); };
    // FLIP Y for geographic data: SVG y grows downward, latitude grows upward (north).
    auto yflip = [&](double lat) { return maxLat - lat; };

    QString svg;
    svg.reserve(qsizetype(64) + qsizetype(totalPts) * 20 + qsizetype(polys.size()+wpts.size()) * 32);
    svg += QStringLiteral("<svg xmlns='http://www.w3.org/2000/svg' viewBox='");
    svg += num(minLon);  svg += QLatin1Char(' ');
    svg += num(0.0);     svg += QLatin1Char(' ');
    svg += num(lonSpan); svg += QLatin1Char(' ');
    svg += num(latSpan);
    svg += QStringLiteral("'>\n");

    // Tracks & routes -> open polylines (fill:none, blue stroke).
    QString strokeStr = num(stroke);
    for (const Poly& poly : polys) {
        if (poly.pts.empty()) continue;
        if (poly.pts.size() == 1) {
            // A one-point segment can't be a polyline; render as a small dot.
            const Pt& q = poly.pts[0];
            svg += QStringLiteral("<circle cx='"); svg += num(q.lon);
            svg += QStringLiteral("' cy='");       svg += num(yflip(q.lat));
            svg += QStringLiteral("' r='");        svg += num(stroke * 1.5);
            svg += QStringLiteral("' fill='#0000ff'/>\n");
            continue;
        }
        svg += QStringLiteral("<polyline fill='none' stroke='#0000ff' stroke-width='");
        svg += strokeStr;
        svg += QStringLiteral("' stroke-linejoin='round' stroke-linecap='round' points='");
        bool first = true;
        for (const Pt& q : poly.pts) {
            if (!first) svg += QLatin1Char(' ');
            first = false;
            svg += num(q.lon);
            svg += QLatin1Char(',');
            svg += num(yflip(q.lat));
        }
        svg += QStringLiteral("'/>\n");
    }

    // Waypoints -> red dots.
    QString wrStr = num(wr);
    for (const Pt& q : wpts) {
        svg += QStringLiteral("<circle cx='"); svg += num(q.lon);
        svg += QStringLiteral("' cy='");       svg += num(yflip(q.lat));
        svg += QStringLiteral("' r='");        svg += wrStr;
        svg += QStringLiteral("' fill='#ff0000' stroke='#800000' stroke-width='");
        svg += num(stroke * 0.5);
        svg += QStringLiteral("'/>\n");
    }

    svg += QStringLiteral("</svg>\n");
    return svg;
}

// ---- Xfig (.fig) ASCII vector importer -> SVG bridge ----
// Spec: Xfig FORMAT 3.2 ("The FIG file format 3.2", XFIG distribution doc).
// Line-oriented ASCII. Header: "#FIG 3.2" then parameter lines, the last of
// which is "resolution coord_system" (two integers, e.g. "1200 2"). Then object
// records, each keyed by a leading integer object code:
//   0 colour-def, 1 ellipse, 2 polyline, 3 spline, 4 text, 5 arc, 6/-6 compound.
// Coords are 1/1200 inch, Y-down (upper-left origin) -> used directly, no flip.

static std::vector<QString> xfigTokens(const QString& s){
    std::vector<QString> out;
    int i=0, L=s.size();
    while(i<L){
        while(i<L && s[i].isSpace()) i++;
        int st=i;
        while(i<L && !s[i].isSpace()) i++;
        if(i>st) out.push_back(s.mid(st,i-st));
    }
    return out;
}

static QString xfigEsc(const QString& in){
    QString o; o.reserve(in.size());
    for(int k=0;k<in.size();k++){
        QChar c=in[k];
        ushort u=c.unicode();
        if(u=='&') o+=QStringLiteral("&amp;");
        else if(u=='<') o+=QStringLiteral("&lt;");
        else if(u=='>') o+=QStringLiteral("&gt;");
        else if(u=='"') o+=QStringLiteral("&quot;");
        else if(u=='\'') o+=QStringLiteral("&#39;");
        else if(u<0x20) o+=QChar(' ');
        else o+=c;
    }
    return o;
}

QString xfigToSvg(const u8* p, i64 n){
    if(!p || n<=0 || n>(i64)80*1024*1024) return QString();
    const double XPI = 3.14159265358979323846;

    QString content = QString::fromLatin1((const char*)p,(int)n);

    // Split into lines without relying on QStringList/QRegularExpression headers.
    std::vector<QString> lines;
    {
        int i=0,L=content.size(),st=0;
        while(i<=L){
            if(i==L || content[i]==QChar('\n')){ lines.push_back(content.mid(st,i-st)); st=i+1; }
            i++;
        }
    }
    if(lines.empty()) return QString();

    // Header must announce FIG.
    if(!lines[0].trimmed().startsWith(QStringLiteral("#FIG"))) return QString();

    // Locate resolution line: first non-comment line that is exactly two integers.
    int objStart=-1;
    for(int i=1;i<(int)lines.size() && i<400;i++){
        QString tr=lines[i].trimmed();
        if(tr.isEmpty()||tr.startsWith(QChar('#'))) continue;
        std::vector<QString> tk=xfigTokens(lines[i]);
        if(tk.size()==2){
            bool a=false,b=false; tk[0].toLong(&a); tk[1].toLong(&b);
            if(a&&b){ objStart=i+1; break; }
        }
    }
    if(objStart<0) return QString();

    static const char* PAL[32]={
      "#000000","#0000ff","#00ff00","#00ffff","#ff0000","#ff00ff","#ffff00","#ffffff",
      "#000090","#0000b0","#0000d0","#87ceff","#009000","#00b000","#00d000","#009090",
      "#00b0b0","#00d0d0","#900000","#b00000","#d00000","#900090","#b000b0","#d000d0",
      "#803000","#a04000","#c06000","#ff8080","#ffa0a0","#ffc0c0","#ffe0e0","#ffd700"
    };
    std::vector<int> ucNum; std::vector<QString> ucHex; // user-defined colours

    auto colr=[&](int idx, bool isFill)->QString{
        if(idx==-1) return isFill? QString("#cccccc") : QString("#000000");
        if(idx>=0 && idx<32) return QString(PAL[idx]);
        for(size_t j=0;j<ucNum.size();j++) if(ucNum[j]==idx) return ucHex[j];
        return QString("#000000");
    };
    auto swidth=[&](int t)->double{ if(t<0)t=0; double w=(double)t*15.0; if(w<1.0) w=7.5; return w; };

    double minx=1e18,miny=1e18,maxx=-1e18,maxy=-1e18; bool haveBox=false;
    auto upd=[&](double x,double y){
        if(!std::isfinite(x)||!std::isfinite(y)) return;
        if(x<minx)minx=x; if(y<miny)miny=y; if(x>maxx)maxx=x; if(y>maxy)maxy=y; haveBox=true;
    };

    QString body;
    long emitted=0, totalPts=0;
    const long MAXELEM=200000, MAXPTS=2000000;
    int li=objStart, loopGuard=0; const int LOOPMAX=20000000;

    auto collectNums=[&](int need)->std::vector<double>{
        std::vector<double> v;
        if(need<=0) return v;
        v.reserve((size_t)need);
        int guard=0;
        while((int)v.size()<need && li<(int)lines.size() && guard<need+2000){
            guard++;
            QString ln=lines[li]; li++;
            std::vector<QString> tk=xfigTokens(ln);
            for(size_t j=0;j<tk.size();j++){ bool ok=false; double d=tk[j].toDouble(&ok); if(ok){ v.push_back(d); if((int)v.size()>=need) break; } }
        }
        return v;
    };

    while(li<(int)lines.size()){
        if(++loopGuard>LOOPMAX) break;
        if(emitted>=MAXELEM || totalPts>=MAXPTS) break;
        QString ln=lines[li];
        QString tr=ln.trimmed();
        if(tr.isEmpty()||tr.startsWith(QChar('#'))){ li++; continue; }
        std::vector<QString> tk=xfigTokens(ln);
        if(tk.empty()){ li++; continue; }
        bool okc=false; long code=tk[0].toLong(&okc);
        if(!okc){ li++; continue; }

        if(code==6 || code==-6){ li++; continue; } // compound begin/end (single line)

        if(code==0){ // colour pseudo-object: "0 <num> #rrggbb"
            if(tk.size()>=3 && tk[2].startsWith(QChar('#'))){
                bool ok=false; long cn=tk[1].toLong(&ok);
                // Validate the hex token strictly ('#' + 3/4/6/8 hex digits) before it is
                // stored: tokens are split only on whitespace, so an unvalidated value could
                // contain ' / < > and break out of the fill='…'/stroke='…' attribute it is
                // later substituted into (SVG attribute injection).
                const QString& hx=tk[2];
                bool hexOk = (hx.size()==4||hx.size()==5||hx.size()==7||hx.size()==9);
                for(int j=1; hexOk && j<hx.size(); ++j){ QChar ch=hx[j];
                    if(!((ch>='0'&&ch<='9')||(ch>='a'&&ch<='f')||(ch>='A'&&ch<='F'))) hexOk=false; }
                if(ok && cn>=0 && cn<1000000 && hexOk){ ucNum.push_back((int)cn); ucHex.push_back(hx); }
            }
            li++; continue;
        }

        if(code==1){ // ELLIPSE (all 20 fields on one line)
            if(tk.size()>=20){
                int thickness=tk[3].toInt(), pen=tk[4].toInt(), fill=tk[5].toInt(), areaFill=tk[8].toInt();
                double angle=tk[11].toDouble();
                double cx=tk[12].toDouble(), cy=tk[13].toDouble();
                double rx=std::fabs(tk[14].toDouble()), ry=std::fabs(tk[15].toDouble());
                if(std::isfinite(cx)&&std::isfinite(cy)&&std::isfinite(rx)&&std::isfinite(ry)){
                    upd(cx-rx,cy-ry); upd(cx+rx,cy+ry);
                    QString fillc=(areaFill==-1)?QString("none"):colr(fill,true);
                    QString penc=colr(pen,false);
                    QString e=QString("<ellipse cx='%1' cy='%2' rx='%3' ry='%4' fill='%5' stroke='%6' stroke-width='%7'")
                        .arg(QString::number(cx,'f',3),QString::number(cy,'f',3),
                             QString::number(rx,'f',3),QString::number(ry,'f',3),
                             fillc,penc,QString::number(swidth(thickness),'f',3));
                    double deg=angle*180.0/XPI;
                    if(std::isfinite(deg)&&std::fabs(deg)>0.01)
                        e+=QString(" transform='rotate(%1 %2 %3)'").arg(QString::number(deg,'f',3),QString::number(cx,'f',3),QString::number(cy,'f',3));
                    e+=QStringLiteral("/>\n");
                    body+=e; emitted++;
                }
            }
            li++; continue;
        }

        if(code==2){ // POLYLINE: first line 16 fields ending in npoints; pts follow
            if(tk.size()>=16){
                int sub=tk[1].toInt(), thickness=tk[3].toInt(), pen=tk[4].toInt(), fill=tk[5].toInt(), areaFill=tk[8].toInt();
                int fwd=tk[13].toInt(), bwd=tk[14].toInt();
                long npoints=tk[15].toLong();
                li++;
                if(fwd!=0 && li<(int)lines.size()) li++;   // forward arrow sub-line
                if(bwd!=0 && li<(int)lines.size()) li++;   // backward arrow sub-line
                if(sub==5 && li<(int)lines.size()) li++;   // imported-picture "flipped file" line
                if(npoints<=0 || npoints>500000) return QString();
                std::vector<double> nums=collectNums((int)(npoints*2));
                long have=(long)(nums.size()/2);
                if(have>=2){
                    totalPts+=have;
                    QString pts;
                    for(long k=0;k<have;k++){
                        double x=nums[2*k], y=nums[2*k+1];
                        if(!std::isfinite(x)||!std::isfinite(y)) continue;
                        upd(x,y);
                        if(!pts.isEmpty()) pts+=QChar(' ');
                        pts+=QString::number(x,'f',3)+QChar(',')+QString::number(y,'f',3);
                    }
                    bool closed=(sub==2||sub==3||sub==4||sub==5);
                    QString fillc=(areaFill==-1)?QString("none"):colr(fill,true);
                    QString penc=colr(pen,false);
                    if(closed)
                        body+=QString("<polygon points='%1' fill='%2' stroke='%3' stroke-width='%4'/>\n")
                            .arg(pts,fillc,penc,QString::number(swidth(thickness),'f',3));
                    else
                        body+=QString("<polyline points='%1' fill='none' stroke='%2' stroke-width='%3'/>\n")
                            .arg(pts,penc,QString::number(swidth(thickness),'f',3));
                    emitted++;
                }
            } else li++;
            continue;
        }

        if(code==3){ // SPLINE: approximate as polyline through control points
            if(tk.size()>=14){
                int thickness=tk[3].toInt(), pen=tk[4].toInt();
                int fwd=tk[11].toInt(), bwd=tk[12].toInt();
                long npoints=tk[13].toLong();
                li++;
                if(fwd!=0 && li<(int)lines.size()) li++;
                if(bwd!=0 && li<(int)lines.size()) li++;
                if(npoints<=0 || npoints>500000) return QString();
                std::vector<double> nums=collectNums((int)(npoints*2));
                long have=(long)(nums.size()/2);
                collectNums((int)npoints); // consume npoints shape-factor floats
                if(have>=2){
                    totalPts+=have;
                    QString pts;
                    for(long k=0;k<have;k++){
                        double x=nums[2*k], y=nums[2*k+1];
                        if(!std::isfinite(x)||!std::isfinite(y)) continue;
                        upd(x,y);
                        if(!pts.isEmpty()) pts+=QChar(' ');
                        pts+=QString::number(x,'f',3)+QChar(',')+QString::number(y,'f',3);
                    }
                    QString penc=colr(pen,false);
                    body+=QString("<polyline points='%1' fill='none' stroke='%2' stroke-width='%3'/>\n")
                        .arg(pts,penc,QString::number(swidth(thickness),'f',3));
                    emitted++;
                }
            } else li++;
            continue;
        }

        if(code==4){ // TEXT: 13 leading fields then string terminated by 0x01
            if(tk.size()>=13){
                int sub=tk[1].toInt(), color=tk[2].toInt();
                double fontSize=tk[6].toDouble(), angle=tk[7].toDouble();
                double height=tk[9].toDouble(), length=tk[10].toDouble();
                double x=tk[11].toDouble(), y=tk[12].toDouble();
                // Locate string: skip 13 whitespace-separated tokens in the raw line.
                int pos=0,L=ln.size(),cnt=0;
                while(cnt<13 && pos<L){
                    while(pos<L && ln[pos].isSpace()) pos++;
                    while(pos<L && !ln[pos].isSpace()) pos++;
                    cnt++;
                }
                if(pos<L && ln[pos].isSpace()) pos++;
                QString raw=(pos<L)?ln.mid(pos):QString();
                int term=raw.indexOf(QChar(0x01));
                if(term>=0) raw=raw.left(term);
                // Un-escape FIG backslash-octal sequences.
                QString str;
                for(int q=0;q<raw.size();q++){
                    QChar c=raw[q];
                    if(c==QChar('\\') && q+1<raw.size()){
                        QChar d=raw[q+1];
                        if(d>=QChar('0') && d<=QChar('7')){
                            int val=0,cc=0;
                            while(cc<3 && q+1<raw.size() && raw[q+1]>=QChar('0') && raw[q+1]<=QChar('7')){ val=val*8+(raw[q+1].unicode()-'0'); q++; cc++; }
                            if(val>0 && val<256) str+=QChar(val);
                        } else { str+=d; q++; }
                    } else str+=c;
                }
                if(std::isfinite(x)&&std::isfinite(y) && !str.trimmed().isEmpty()){
                    upd(x,y);
                    double fs=(fontSize>0?fontSize:12.0)*1200.0/72.0; // points -> fig units
                    upd(x,y-fs);
                    if(std::isfinite(length)) upd(x+length,y);
                    if(std::isfinite(height)) upd(x,y-height);
                    QString anchor=(sub==1)?QString("middle"):(sub==2?QString("end"):QString("start"));
                    QString e=QString("<text x='%1' y='%2' font-size='%3' fill='%4' text-anchor='%5'")
                        .arg(QString::number(x,'f',3),QString::number(y,'f',3),QString::number(fs,'f',3),colr(color,false),anchor);
                    double deg=angle*180.0/XPI;
                    if(std::isfinite(deg)&&std::fabs(deg)>0.01)
                        e+=QString(" transform='rotate(%1 %2 %3)'").arg(QString::number(-deg,'f',3),QString::number(x,'f',3),QString::number(y,'f',3));
                    e+=QChar('>')+xfigEsc(str)+QStringLiteral("</text>\n");
                    body+=e; emitted++;
                }
            }
            li++; continue;
        }

        if(code==5){ // ARC: emit a 3-point polyline through its defining points
            if(tk.size()>=22){
                int thickness=tk[3].toInt(), pen=tk[4].toInt();
                int fwd=tk[12].toInt(), bwd=tk[13].toInt();
                double x1=tk[16].toDouble(),y1=tk[17].toDouble();
                double x2=tk[18].toDouble(),y2=tk[19].toDouble();
                double x3=tk[20].toDouble(),y3=tk[21].toDouble();
                li++;
                if(fwd!=0 && li<(int)lines.size()) li++;
                if(bwd!=0 && li<(int)lines.size()) li++;
                if(std::isfinite(x1)&&std::isfinite(y1)&&std::isfinite(x2)&&std::isfinite(y2)&&std::isfinite(x3)&&std::isfinite(y3)){
                    upd(x1,y1);upd(x2,y2);upd(x3,y3);
                    body+=QString("<polyline points='%1,%2 %3,%4 %5,%6' fill='none' stroke='%7' stroke-width='%8'/>\n")
                        .arg(QString::number(x1,'f',3),QString::number(y1,'f',3),
                             QString::number(x2,'f',3),QString::number(y2,'f',3),
                             QString::number(x3,'f',3),QString::number(y3,'f',3),
                             colr(pen,false),QString::number(swidth(thickness),'f',3));
                    emitted++;
                }
            } else li++;
            continue;
        }

        li++; // unknown code
    }

    if(!haveBox || emitted==0) return QString();
    double w=maxx-minx, h=maxy-miny;
    if(w<=0) w=1; if(h<=0) h=1;
    double pad=0.02*std::sqrt(w*h); if(!std::isfinite(pad)||pad<1) pad=1;
    double vx=minx-pad, vy=miny-pad, vw=w+2*pad, vh=h+2*pad;
    QString svg=QString("<svg xmlns='http://www.w3.org/2000/svg' viewBox='%1 %2 %3 %4'>\n")
        .arg(QString::number(vx,'f',3),QString::number(vy,'f',3),QString::number(vw,'f',3),QString::number(vh,'f',3));
    svg+=body;
    svg+=QStringLiteral("</svg>\n");
    return svg;
}

// ---- CGM (ISO 8632-3, binary encoding) -> SVG bridge -----------------------
// Command header: one 16-bit BIG-ENDIAN word.
//   bits 15-12 = element class (4)   bits 11-5 = element id (7)   bits 4-0 = param length (5)
// If param-length field == 31 -> long form: next 16-bit word is the partition length,
//   its high bit (0x8000) = "not last partition" (another length word + data follows).
// Parameter data is padded to an even octet count. VDC (point) values default to
// 16-bit signed BIG-ENDIAN integers; VDC INTEGER PRECISION (class 3 id 1) may change this.

static inline long cgmRdInt(const u8* d, i64 sz, i64& c, int bytes, bool& ok){
    if(bytes<1 || bytes>4 || c<0 || c+bytes>sz){ ok=false; return 0; }
    long v = (long)(int8_t)d[c];               // sign-extend high byte
    for(int i=1;i<bytes;i++) v = (v<<8) | (long)d[c+i];
    c += bytes;
    return v;
}

static inline QString cgmXmlEsc(const QString& s){
    QString o; o.reserve(s.size()+8);
    for(QChar ch : s){
        unsigned short u = ch.unicode();
        if(u=='&') o += QStringLiteral("&amp;");
        else if(u=='<') o += QStringLiteral("&lt;");
        else if(u=='>') o += QStringLiteral("&gt;");
        else if(u=='"') o += QStringLiteral("&quot;");
        else if(u=='\'') o += QStringLiteral("&#39;");
        else if(u>=32 || u=='\n' || u=='\t') o += ch;
    }
    return o;
}

QString cgmToSvg(const u8* p, i64 n){
    if(!p || n < 4) return QString();
    // Magic: the very first command must be BEGIN METAFILE (class 0, id 1).
    {
        u16 h0 = rd16be(p);
        int c0 = (h0>>12)&0xF, id0 = (h0>>5)&0x7F;
        if(!(c0==0 && id0==1)) return QString();
    }

    struct Prim { int kind; std::vector<double> pts; double r=0.0, r2=0.0; QString text; };
    // kind: 0 polyline, 1 polygon(filled), 2 rect, 3 circle, 4 ellipse, 5 text,
    //       6 polymarker, 7 disjoint-polyline
    std::vector<Prim> prims;

    const size_t MAXPTS = 5000000, MAXPRIM = 200000;
    const i64    MAXPARAM = 40000000;
    size_t totalPts = 0;

    int  vdcBytes  = 2;      // default INTEGER precision for VDC = 16-bit
    bool vdcReal   = false;  // VDC TYPE real -> we skip primitives (best-effort)
    bool extentSet = false;
    double exMinX=0,exMinY=0,exMaxX=0,exMaxY=0;

    i64 pos = 0;
    std::vector<u8> params;
    params.reserve(256);

    while(pos + 2 <= n){
        u16 hdr = rd16be(p+pos); pos += 2;
        int cls = (hdr>>12)&0xF;
        int eid = (hdr>>5)&0x7F;
        int len = hdr & 0x1F;

        params.clear();
        if(len != 31){
            if(pos + len > n) break;
            if(len>0) params.insert(params.end(), p+pos, p+pos+len);
            pos += len;
            if((len & 1) && pos < n) pos += 1;           // even padding
        } else {
            // Long form: one or more partitions.
            bool more = true;
            while(more){
                if(pos + 2 > n){ more=false; break; }
                u16 lw = rd16be(p+pos); pos += 2;
                more = (lw & 0x8000) != 0;
                int plen = lw & 0x7FFF;
                if(plen < 0) plen = 0;
                if(pos + plen > n){ plen = (int)(n - pos); more = false; }
                if(plen < 0) plen = 0;
                if(plen > 0){ params.insert(params.end(), p+pos, p+pos+plen); pos += plen; }
                if((plen & 1) && pos < n) pos += 1;       // pad each partition to even
                if((i64)params.size() > MAXPARAM){ more=false; }
            }
        }

        const u8* d = params.data();
        i64 sz = (i64)params.size();

        if(cls == 0){                 // delimiter
            if(eid == 2) break;       // END METAFILE
            continue;
        }
        if(cls == 1){                 // metafile descriptor
            if(eid == 3 && sz >= 2){  // VDC TYPE: 0=integer, 1=real
                int v = (int)(int16_t)rd16be(d);
                vdcReal = (v == 1);
            }
            continue;
        }
        if(cls == 3){                 // control elements
            if(eid == 1 && sz >= 2){  // VDC INTEGER PRECISION (bits)
                int bits = (int)(int16_t)rd16be(d);
                if(bits==8) vdcBytes=1; else if(bits==16) vdcBytes=2;
                else if(bits==24) vdcBytes=3; else if(bits==32) vdcBytes=4;
            }
            continue;
        }
        if(cls == 2){                 // picture descriptor
            if(eid == 6 && !vdcReal){  // VDC EXTENT = two points
                i64 c=0; bool ok=true;
                long x1=cgmRdInt(d,sz,c,vdcBytes,ok);
                long y1=cgmRdInt(d,sz,c,vdcBytes,ok);
                long x2=cgmRdInt(d,sz,c,vdcBytes,ok);
                long y2=cgmRdInt(d,sz,c,vdcBytes,ok);
                if(ok){
                    exMinX = (x1<x2)?(double)x1:(double)x2;
                    exMaxX = (x1<x2)?(double)x2:(double)x1;
                    exMinY = (y1<y2)?(double)y1:(double)y2;
                    exMaxY = (y1<y2)?(double)y2:(double)y1;
                    extentSet = true;
                }
            }
            continue;
        }
        if(cls == 4 && !vdcReal){     // graphical primitives
            if(prims.size() >= MAXPRIM) continue;
            int stride = 2 * vdcBytes;
            if(stride <= 0) continue;

            if(eid==1 || eid==2 || eid==3 || eid==7){   // polyline / disjoint / polymarker / polygon
                Prim pr; pr.kind = (eid==1?0 : eid==2?7 : eid==3?6 : 1);
                i64 c=0;
                while(c + stride <= sz && totalPts < MAXPTS){
                    bool ok=true;
                    long x=cgmRdInt(d,sz,c,vdcBytes,ok);
                    long y=cgmRdInt(d,sz,c,vdcBytes,ok);
                    if(!ok) break;
                    pr.pts.push_back((double)x); pr.pts.push_back((double)y);
                    totalPts++;
                }
                if(pr.pts.size()>=4 || (pr.kind==6 && pr.pts.size()>=2)) prims.push_back(pr);
            }
            else if(eid==8){                            // POLYGON SET: point + 2-byte edge flag
                Prim pr; pr.kind = 1;
                i64 c=0;
                while(c + stride + 2 <= sz && totalPts < MAXPTS){
                    bool ok=true;
                    long x=cgmRdInt(d,sz,c,vdcBytes,ok);
                    long y=cgmRdInt(d,sz,c,vdcBytes,ok);
                    if(!ok) break;
                    c += 2; // edge-out flag (enumerated) - ignored, approximate as one contour
                    pr.pts.push_back((double)x); pr.pts.push_back((double)y);
                    totalPts++;
                }
                if(pr.pts.size()>=4) prims.push_back(pr);
            }
            else if(eid==11){                           // RECTANGLE: two corner points
                i64 c=0; bool ok=true;
                long x1=cgmRdInt(d,sz,c,vdcBytes,ok);
                long y1=cgmRdInt(d,sz,c,vdcBytes,ok);
                long x2=cgmRdInt(d,sz,c,vdcBytes,ok);
                long y2=cgmRdInt(d,sz,c,vdcBytes,ok);
                if(ok){ Prim pr; pr.kind=2; pr.pts={(double)x1,(double)y1,(double)x2,(double)y2}; prims.push_back(pr); }
            }
            else if(eid==12){                           // CIRCLE: centre + radius
                i64 c=0; bool ok=true;
                long cx=cgmRdInt(d,sz,c,vdcBytes,ok);
                long cy=cgmRdInt(d,sz,c,vdcBytes,ok);
                long rr=cgmRdInt(d,sz,c,vdcBytes,ok);
                if(ok && rr>=0){ Prim pr; pr.kind=3; pr.pts={(double)cx,(double)cy}; pr.r=(double)rr; prims.push_back(pr); }
            }
            else if(eid==17){                           // ELLIPSE: centre + 2 conjugate-diameter endpoints
                i64 c=0; bool ok=true;
                long cx=cgmRdInt(d,sz,c,vdcBytes,ok);
                long cy=cgmRdInt(d,sz,c,vdcBytes,ok);
                long ax=cgmRdInt(d,sz,c,vdcBytes,ok);
                long ay=cgmRdInt(d,sz,c,vdcBytes,ok);
                long bx=cgmRdInt(d,sz,c,vdcBytes,ok);
                long by=cgmRdInt(d,sz,c,vdcBytes,ok);
                if(ok){
                    double rx=std::sqrt((double)(ax-cx)*(double)(ax-cx)+(double)(ay-cy)*(double)(ay-cy));
                    double ry=std::sqrt((double)(bx-cx)*(double)(bx-cx)+(double)(by-cy)*(double)(by-cy));
                    Prim pr; pr.kind=4; pr.pts={(double)cx,(double)cy}; pr.r=rx; pr.r2=ry; prims.push_back(pr);
                }
            }
            else if(eid==4){                            // TEXT: point, final-flag(enum,2B), string
                i64 c=0; bool ok=true;
                long tx=cgmRdInt(d,sz,c,vdcBytes,ok);
                long ty=cgmRdInt(d,sz,c,vdcBytes,ok);
                if(ok){
                    c += 2; // final/not-final flag (enumerated)
                    QString str;
                    if(c < sz){
                        int cnt = d[c]; c++;
                        if(cnt == 255){                 // long string form
                            if(c+2 <= sz){ cnt = (((int)(d[c]&0x7F))<<8) | (int)d[c+1]; c += 2; }
                            else cnt = 0;
                        }
                        for(int i=0;i<cnt && c<sz;i++){ str += QChar((unsigned short)d[c]); c++; }
                    }
                    Prim pr; pr.kind=5; pr.pts={(double)tx,(double)ty}; pr.text=str; prims.push_back(pr);
                }
            }
            // unrecognised primitives: already advanced past their params, skip.
            continue;
        }
        // other classes / attributes: ignore (params already consumed).
    }

    if(prims.empty()) return QString();

    // ---- compute data bounding box -----------------------------------------
    double dMinX=1e300,dMinY=1e300,dMaxX=-1e300,dMaxY=-1e300;
    auto inc=[&](double x,double y){ if(x<dMinX)dMinX=x; if(x>dMaxX)dMaxX=x; if(y<dMinY)dMinY=y; if(y>dMaxY)dMaxY=y; };
    for(const Prim& pr : prims){
        if(pr.kind==3){ inc(pr.pts[0]-pr.r,pr.pts[1]-pr.r); inc(pr.pts[0]+pr.r,pr.pts[1]+pr.r); }
        else if(pr.kind==4){ inc(pr.pts[0]-pr.r,pr.pts[1]-pr.r2); inc(pr.pts[0]+pr.r,pr.pts[1]+pr.r2); }
        else { for(size_t i=0;i+1<pr.pts.size();i+=2) inc(pr.pts[i],pr.pts[i+1]); }
    }

    double vbMinX,vbMinY,vbMaxX,vbMaxY;
    if(extentSet && exMaxX>exMinX && exMaxY>exMinY){ vbMinX=exMinX;vbMinY=exMinY;vbMaxX=exMaxX;vbMaxY=exMaxY; }
    else { vbMinX=dMinX;vbMinY=dMinY;vbMaxX=dMaxX;vbMaxY=dMaxY; }
    if(!(vbMaxX>vbMinX)) vbMaxX = vbMinX + 1.0;
    if(!(vbMaxY>vbMinY)) vbMaxY = vbMinY + 1.0;
    double W = vbMaxX - vbMinX, H = vbMaxY - vbMinY;
    if(!std::isfinite(W) || !std::isfinite(H) || W<=0.0 || H<=0.0) return QString();

    double flipC = vbMinY + vbMaxY;                   // CGM VDC Y-up -> SVG Y-down
    double big = (W>H)?W:H;
    double sw = big/500.0; if(sw<=0.0 || !std::isfinite(sw)) sw = 1.0;
    double fs = big/40.0;  if(fs<=0.0 || !std::isfinite(fs)) fs = 1.0;

    auto num=[&](double v){ return QString::number(v,'f',3); };
    auto fy =[&](double y){ return flipC - y; };

    QString body;
    for(const Prim& pr : prims){
        if(pr.kind==0){                               // polyline
            QString pts;
            for(size_t i=0;i+1<pr.pts.size();i+=2){
                double x=pr.pts[i], y=fy(pr.pts[i+1]);
                if(!std::isfinite(x)||!std::isfinite(y)) continue;
                if(!pts.isEmpty()) pts += QLatin1Char(' ');
                pts += num(x)+QStringLiteral(",")+num(y);
            }
            if(!pts.isEmpty())
                body += QStringLiteral("<polyline points='%1' fill='none' stroke='#000000' stroke-width='%2'/>").arg(pts,num(sw));
        }
        else if(pr.kind==6){                          // polymarker -> small dots
            for(size_t i=0;i+1<pr.pts.size();i+=2){
                double x=pr.pts[i], y=fy(pr.pts[i+1]);
                if(!std::isfinite(x)||!std::isfinite(y)) continue;
                body += QStringLiteral("<circle cx='%1' cy='%2' r='%3' fill='#000000'/>").arg(num(x),num(y),num(sw*2.0));
            }
        }
        else if(pr.kind==7){                          // disjoint polyline -> segment pairs
            QString dstr;
            for(size_t i=0;i+3<pr.pts.size();i+=4){
                double x1=pr.pts[i],y1=fy(pr.pts[i+1]),x2=pr.pts[i+2],y2=fy(pr.pts[i+3]);
                if(!std::isfinite(x1)||!std::isfinite(y1)||!std::isfinite(x2)||!std::isfinite(y2)) continue;
                dstr += QStringLiteral("M%1,%2L%3,%4").arg(num(x1),num(y1),num(x2),num(y2));
            }
            if(!dstr.isEmpty())
                body += QStringLiteral("<path d='%1' fill='none' stroke='#000000' stroke-width='%2'/>").arg(dstr,num(sw));
        }
        else if(pr.kind==1){                          // polygon (filled, closed)
            QString dstr; bool first=true;
            for(size_t i=0;i+1<pr.pts.size();i+=2){
                double x=pr.pts[i], y=fy(pr.pts[i+1]);
                if(!std::isfinite(x)||!std::isfinite(y)) continue;
                dstr += QStringLiteral("%1%2,%3").arg(first?QStringLiteral("M"):QStringLiteral("L"), num(x), num(y));
                first=false;
            }
            if(!dstr.isEmpty()){
                dstr += QStringLiteral("Z");
                body += QStringLiteral("<path d='%1' fill='#cccccc' stroke='#333333' stroke-width='%2'/>").arg(dstr,num(sw));
            }
        }
        else if(pr.kind==2){                          // rectangle
            double x1=pr.pts[0],y1=fy(pr.pts[1]),x2=pr.pts[2],y2=fy(pr.pts[3]);
            double rx=(x1<x2)?x1:x2, ry=(y1<y2)?y1:y2;
            double rw=(x2>x1)?(x2-x1):(x1-x2), rh=(y2>y1)?(y2-y1):(y1-y2);
            if(std::isfinite(rx)&&std::isfinite(ry)&&rw>0.0&&rh>0.0)
                body += QStringLiteral("<rect x='%1' y='%2' width='%3' height='%4' fill='#cccccc' stroke='#333333' stroke-width='%5'/>")
                        .arg(num(rx),num(ry),num(rw),num(rh),num(sw));
        }
        else if(pr.kind==3){                          // circle
            double cx=pr.pts[0], cy=fy(pr.pts[1]), r=pr.r;
            if(std::isfinite(cx)&&std::isfinite(cy)&&r>0.0)
                body += QStringLiteral("<circle cx='%1' cy='%2' r='%3' fill='#cccccc' stroke='#333333' stroke-width='%4'/>")
                        .arg(num(cx),num(cy),num(r),num(sw));
        }
        else if(pr.kind==4){                          // ellipse (axis-aligned approximation)
            double cx=pr.pts[0], cy=fy(pr.pts[1]), rx=pr.r, ry=pr.r2;
            if(std::isfinite(cx)&&std::isfinite(cy)&&rx>0.0&&ry>0.0)
                body += QStringLiteral("<ellipse cx='%1' cy='%2' rx='%3' ry='%4' fill='#cccccc' stroke='#333333' stroke-width='%5'/>")
                        .arg(num(cx),num(cy),num(rx),num(ry),num(sw));
        }
        else if(pr.kind==5){                          // text
            double x=pr.pts[0], y=fy(pr.pts[1]);
            QString t=cgmXmlEsc(pr.text);
            if(!t.isEmpty()&&std::isfinite(x)&&std::isfinite(y))
                body += QStringLiteral("<text x='%1' y='%2' font-size='%3' fill='#000000'>%4</text>")
                        .arg(num(x),num(y),num(fs),t);
        }
    }

    if(body.isEmpty()) return QString();
    QString svg = QStringLiteral("<svg xmlns='http://www.w3.org/2000/svg' viewBox='%1 %2 %3 %4'>")
                    .arg(num(vbMinX),num(vbMinY),num(W),num(H));
    svg += body;
    svg += QStringLiteral("</svg>");
    return svg;
}

// SVGZ = gzip-compressed SVG (RFC 1952). We detect the gzip magic, inflate
// the stream with zlib (gzip window via inflateInit2(15+16)), cap the output
// to guard against decompression bombs, and hand the raw bytes back as a
// UTF-8 SVG document. The caller pipes the result straight to the SVG parser.
QByteArray gunzipBytes(const u8* p, i64 n) {
    // Minimum gzip file: 10-byte header + 8-byte trailer.
    if (!p || n < 18) return QByteArray();
    // RFC 1952 section 2.3.1: ID1=0x1f, ID2=0x8b, CM=8 (DEFLATE).
    if (p[0] != 0x1f || p[1] != 0x8b || p[2] != 0x08) return QByteArray();

    // Decompression-bomb guard: refuse to produce more than this many bytes.
    const size_t kMaxOut = 256u * 1024u * 1024u; // 256 MB

    z_stream strm;
    std::memset(&strm, 0, sizeof(strm));
    strm.zalloc = Z_NULL;
    strm.zfree  = Z_NULL;
    strm.opaque = Z_NULL;
    strm.next_in  = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(p));
    strm.avail_in = 0;

    // 15 = max window bits; +16 selects gzip-only decoding (zlib manual).
    if (inflateInit2(&strm, 15 + 16) != Z_OK) return QByteArray();

    QByteArray out;
    const size_t CHUNK = 1u << 16; // 64 KiB output chunk
    std::vector<char> buf(CHUNK);

    // Feed the whole input at once; zlib will consume it across inflate() calls.
    // Guard the i64->uInt narrowing: process the input in <=uInt-sized slices.
    i64 inPos = 0;
    int ret = Z_OK;
    bool ok = true;

    do {
        if (strm.avail_in == 0) {
            if (inPos >= n) {
                // Ran out of input before Z_STREAM_END => truncated/corrupt.
                ok = false;
                break;
            }
            i64 remain = n - inPos;
            uInt take = (remain > (i64)0x40000000) ? (uInt)0x40000000 : (uInt)remain;
            strm.next_in  = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(p + inPos));
            strm.avail_in = take;
            inPos += take;
        }

        do {
            strm.next_out  = reinterpret_cast<Bytef*>(buf.data());
            strm.avail_out = (uInt)CHUNK;
            ret = inflate(&strm, Z_NO_FLUSH);
            if (ret == Z_NEED_DICT || ret == Z_DATA_ERROR ||
                ret == Z_MEM_ERROR || ret == Z_STREAM_ERROR) {
                ok = false;
                break;
            }
            size_t produced = CHUNK - strm.avail_out;
            if (produced) {
                if ((size_t)out.size() + produced > kMaxOut) { ok = false; break; }
                out.append(buf.data(), (int)produced);
            }
            // Keep draining while zlib is filling full chunks.
        } while (strm.avail_out == 0 && ret != Z_STREAM_END && ok);

        if (!ok) break;
    } while (ret != Z_STREAM_END);

    inflateEnd(&strm);

    if (!ok || ret != Z_STREAM_END) return QByteArray();
    if (out.isEmpty()) return QByteArray();

    return out;
}

QString svgzToSvg(const u8* p, i64 n) {
    const QByteArray out = gunzipBytes(p, n);
    if (out.isEmpty()) return QString();
    // Sanity: the payload should actually look like SVG/XML, otherwise treat
    // it as "not our format" so the caller can fall through cleanly.
    {
        // Skip a UTF-8 BOM and leading whitespace when probing.
        int i = 0;
        if (out.size() >= 3 &&
            (u8)out[0] == 0xEF && (u8)out[1] == 0xBB && (u8)out[2] == 0xBF)
            i = 3;
        while (i < out.size() &&
               (out[i] == ' ' || out[i] == '\t' || out[i] == '\r' || out[i] == '\n'))
            ++i;
        if (i >= out.size() || out[i] != '<') return QString();
    }

    return QString::fromUtf8(out);
}


// ============================= GERBER RS-274X ================================
// Ucamco "The Gerber Format Specification" (rev 2021.02+). ASCII PCB image
// format: stream of '*'-terminated blocks; extended commands wrapped in '%..%'.
// Emits a monochrome (black) SVG with a data-bounds viewBox. Y is flipped
// (Gerber Y is up) so SVG y = -y consistently.
QString gerberToSvg(const u8* p, i64 n){
    if(!p || n<=0) return QString();

    struct GerAp{ int code; int type; double a,b,c; QString trace; };
    const double GERPI = 3.14159265358979323846;

    // ---- parser state ----
    int    fmtIntD = 0, fmtDecD = 6;   // FS integer/decimal digit counts
    bool   trailingZ = false;          // trailing zero suppression (legacy 'T')
    double unitScale = 1.0;            // MM=1.0, IN=25.4  -> everything in mm
    std::vector<GerAp> aps;
    int    curIdx = -1;                // selected aperture index
    double curX = 0.0, curY = 0.0;     // current point, gerber mm (y up)
    bool   region = false;
    QString regionPath, regionElems, flashElems;
    long long prims = 0;
    const long long MAXPRIM = 3000000;
    bool haveBB=false; double minX=0,minY=0,maxX=0,maxY=0;
    bool sawFS=false, sawMO=false, sawAD=false;

    // isfinite-guard every emitted number: a huge aperture modifier (e.g. 1e308 in inch units)
    // can overflow to inf, and QString::number(inf) yields the literal "inf" — invalid in an SVG attr.
    auto fmt = [](double v)->QString{ return std::isfinite(v) ? QString::number(v,'f',4) : QStringLiteral("0"); };

    auto addBB = [&](double sx,double sy,double r){
        if(!std::isfinite(sx)||!std::isfinite(sy)||!std::isfinite(r)) return;
        double x0=sx-r,x1=sx+r,y0=sy-r,y1=sy+r;
        if(!haveBB){ minX=x0;maxX=x1;minY=y0;maxY=y1;haveBB=true; }
        else{ if(x0<minX)minX=x0; if(x1>maxX)maxX=x1; if(y0<minY)minY=y0; if(y1>maxY)maxY=y1; }
    };

    // parse a coordinate integer token -> mm double (implied decimal via fmt)
    auto pcoord = [&](const QByteArray& raw)->double{
        if(raw.isEmpty()) return 0.0;
        bool neg=false; int st=0;
        if(raw[0]=='+') st=1; else if(raw[0]=='-'){ neg=true; st=1; }
        QByteArray d;
        for(int k=st;k<raw.size();k++){ char ch=raw[k]; if(ch>='0'&&ch<='9') d.append(ch); }
        if(d.isEmpty()) return 0.0;
        if(trailingZ){ int total=fmtIntD+fmtDecD; while(d.size()<total) d.append('0'); }
        double val = d.toDouble();
        double r = (fmtDecD>0)? val/std::pow(10.0,(double)fmtDecD) : val;
        r *= unitScale;
        return neg? -r : r;
    };

    // ------- extended command dispatch ('%...%' sub-blocks) -------
    auto processExt = [&](const QByteArray& sub){
        QString s = QString::fromLatin1(sub).trimmed();
        if(s.isEmpty()) return;
        if(s.startsWith("FS")){                 // %FSLAX46Y46*%  format spec
            int xi = s.indexOf('X');
            if(xi>=0 && xi+2 < s.size()){
                int di = s.at(xi+1).digitValue();
                int md = s.at(xi+2).digitValue();
                if(di>=0) fmtIntD=di;
                if(md>=0) fmtDecD=md;
            }
            trailingZ = (s.size()>2 && s.at(2)==QChar('T'));   // L=leading, T=trailing
            sawFS=true;
        } else if(s.startsWith("MO")){          // %MOMM*% / %MOIN*%  units
            if(s.contains("IN")) unitScale=25.4;
            else if(s.contains("MM")) unitScale=1.0;
            sawMO=true;
        } else if(s.startsWith("ADD")){         // %ADD<code><template>,<mods>*%
            int k=3; QByteArray code;
            while(k<s.size() && s.at(k).isDigit()){ code.append(s.at(k).toLatin1()); k++; }
            if(code.isEmpty()) return;
            int cd = code.toInt();
            QString tmpl;
            while(k<s.size() && s.at(k)!=QChar(',')){ tmpl.append(s.at(k)); k++; }
            QString mods;
            if(k<s.size() && s.at(k)==QChar(',')){ k++; mods=s.mid(k); }
            QStringList mp = mods.split('X');
            auto md = [&](int idx)->double{
                if(idx<mp.size()){ bool ok=false; double v=mp[idx].trimmed().toDouble(&ok); double r=v*unitScale; return (ok && std::isfinite(r))? r : 0.0; }
                return 0.0;
            };
            GerAp ap; ap.code=cd; ap.type=0; ap.a=ap.b=ap.c=0;
            char t0 = tmpl.isEmpty()? '?' : tmpl.at(0).toLatin1();
            if(t0=='C'){ ap.type=0; ap.a=md(0); }                    // circle: dia
            else if(t0=='R'){ ap.type=1; ap.a=md(0); ap.b=md(1); }   // rect: x X y
            else if(t0=='O'){ ap.type=2; ap.a=md(0); ap.b=md(1); }   // obround: x X y
            else if(t0=='P'){                                        // poly: dia X verts X rot
                ap.type=3; ap.a=md(0);                              // outer diameter (scaled)
                ap.b = (mp.size()>1)? mp[1].trimmed().toDouble() : 3; // vertices (unscaled)
                ap.c = (mp.size()>2)? mp[2].trimmed().toDouble() : 0; // rotation deg (unscaled)
            } else { ap.type=0; ap.a=md(0); }                      // macro/unknown -> circle-ish
            if(aps.size() < 200000) aps.push_back(ap);
            sawAD=true;
        }
        // LP/IP/OF/AM/AB/etc. best-effort ignored (polarity, macros, blocks)
    };

    // ------- standard block dispatch (D/G/M + coordinates) -------
    auto processStd = [&](const QByteArray& blk){
        int gC=-1,dC=-1,mC=-1; bool hX=false,hY=false; double nX=curX,nY=curY;
        int idx=0; int L=blk.size();
        while(idx<L){
            char ch=blk[idx];
            if((ch>='A'&&ch<='Z')||(ch>='a'&&ch<='z')){
                char letter=ch; idx++;
                int st=idx;
                while(idx<L){ char d=blk[idx]; if((d>='0'&&d<='9')||d=='+'||d=='-') idx++; else break; }
                QByteArray num=blk.mid(st, idx-st);
                switch(letter){
                    case 'G': gC=num.toInt(); break;
                    case 'D': dC=num.toInt(); break;
                    case 'M': mC=num.toInt(); break;
                    case 'X': hX=true; nX=pcoord(num); break;
                    case 'Y': hY=true; nY=pcoord(num); break;
                    case 'I': case 'J': break;   // arc offsets: arcs approximated as lines
                    default: break;
                }
            } else idx++;
        }

        if(gC==4) return;                                   // G04 comment
        if(gC==36){ region=true; regionPath.clear(); return; }   // region begin
        if(gC==37){                                         // region end
            if(region && !regionPath.isEmpty() && prims<MAXPRIM){
                regionElems += "<path d='"+regionPath+" Z' fill='#000000' stroke='none'/>";
                prims++;
            }
            region=false; regionPath.clear(); return;
        }
        if(gC==70) unitScale=25.4; else if(gC==71) unitScale=1.0;   // deprecated unit set
        // G01/G02/G03 interpolation modes: arcs drawn as line-to-endpoint (spec-permitted approx)

        double oX=curX, oY=curY;
        if(hX) curX=nX; if(hY) curY=nY;                     // coordinates are modal

        if(dC>=10){                                          // select aperture
            int fi=-1;
            for(size_t a=0;a<aps.size();a++){ if(aps[a].code==dC){ fi=(int)a; break; } }
            curIdx=fi;
        }
        else if(dC==2){                                      // D02 move (pen up)
            addBB(curX,-curY,0);
            if(region && prims<MAXPRIM){ regionPath += QString(" M ")+fmt(curX)+" "+fmt(-curY); prims++; }
        }
        else if(dC==1){                                      // D01 draw / interpolate
            if(region){
                if(prims<MAXPRIM){
                    if(regionPath.isEmpty()) regionPath += QString("M ")+fmt(oX)+" "+fmt(-oY);
                    regionPath += QString(" L ")+fmt(curX)+" "+fmt(-curY);
                    addBB(curX,-curY,0); addBB(oX,-oY,0); prims++;
                }
            } else if(curIdx>=0 && prims<MAXPRIM){
                GerAp& ap=aps[curIdx];
                double w = (ap.type==0)? ap.a : ((ap.type==1||ap.type==2)? (ap.a>ap.b?ap.a:ap.b) : ap.a);
                if(!(w>0)) w=0.001;
                ap.trace += "M "+fmt(oX)+" "+fmt(-oY)+" L "+fmt(curX)+" "+fmt(-curY)+" ";
                double r=w/2.0;
                addBB(oX,-oY,r); addBB(curX,-curY,r);
                prims++;
            }
        }
        else if(dC==3){                                      // D03 flash aperture
            if(curIdx>=0 && prims<MAXPRIM){
                GerAp& ap=aps[curIdx];
                double cx=curX, cy=-curY;
                if(ap.type==0){
                    double r=ap.a/2.0;
                    if(r>0){ flashElems += "<circle cx='"+fmt(cx)+"' cy='"+fmt(cy)+"' r='"+fmt(r)+"' fill='#000000'/>"; addBB(cx,cy,r); prims++; }
                } else if(ap.type==1){
                    double w=ap.a,h=ap.b;
                    if(w>0&&h>0){ flashElems += "<rect x='"+fmt(cx-w/2)+"' y='"+fmt(cy-h/2)+"' width='"+fmt(w)+"' height='"+fmt(h)+"' fill='#000000'/>"; addBB(cx,cy,(w>h?w:h)/2); prims++; }
                } else if(ap.type==2){
                    double w=ap.a,h=ap.b;
                    if(w>0&&h>0){ double rx=(w<h?w:h)/2; flashElems += "<rect x='"+fmt(cx-w/2)+"' y='"+fmt(cy-h/2)+"' width='"+fmt(w)+"' height='"+fmt(h)+"' rx='"+fmt(rx)+"' fill='#000000'/>"; addBB(cx,cy,(w>h?w:h)/2); prims++; }
                } else if(ap.type==3){
                    double r=ap.a/2.0; int nv=(int)ap.b; double rot=ap.c;
                    if(r>0 && nv>=3 && nv<=200){
                        QString pts;
                        for(int v=0;v<nv;v++){
                            double ang=(rot + v*360.0/nv)*GERPI/180.0;
                            double px=cx + r*std::cos(ang);
                            double py=cy - r*std::sin(ang);      // y flipped
                            pts += (v==0? QString("M "):QString("L "))+fmt(px)+" "+fmt(py)+" ";
                        }
                        flashElems += "<path d='"+pts+"Z' fill='#000000'/>"; addBB(cx,cy,r); prims++;
                    }
                }
            }
        }
        (void)mC;   // M00/M01/M02 = end; nothing to emit
    };

    // ------- top-level scanner -------
    i64 i=0;
    while(i<n){
        u8 c=p[i];
        if(c=='%'){                                  // extended command group
            i++;
            i64 start=i;
            while(i<n && p[i]!='%') i++;
            // Bound the length before narrowing to int: a >2GB block would wrap negative and make
            // QByteArray(ptr,negative) do a strlen scan past the buffer. Real %..% commands are tiny.
            i64 elen=i-start; if(elen<0) elen=0; if(elen>16*1024*1024) elen=16*1024*1024;
            QByteArray ext((const char*)(p+start), (int)elen);
            if(i<n) i++;                             // skip closing %
            int bs=0;
            for(int k=0;k<=ext.size();k++){          // split by '*' into sub-blocks
                if(k==ext.size() || ext[k]=='*'){
                    if(k>bs) processExt(ext.mid(bs, k-bs));
                    bs=k+1;
                }
            }
        } else if(c=='*'){ i++; }
        else if(c==' '||c=='\n'||c=='\r'||c=='\t'){ i++; }
        else {                                       // standard block, up to '*' (or '%')
            i64 start=i;
            while(i<n && p[i]!='*' && p[i]!='%') i++;
            QByteArray blk((const char*)(p+start), (int)(i-start));
            if(i<n && p[i]=='*') i++;                // consume terminator; '%' left for next loop
            processStd(blk);
        }
        if(prims>=MAXPRIM && flashElems.size()>60000000) break;   // hard runaway guard
    }

    // ------- emit -------
    if(!haveBB) return QString();                    // no geometry -> not this format
    if(!(sawFS||sawMO||sawAD)) return QString();     // no gerber directive seen
    if(prims==0) return QString();

    double w=maxX-minX, h=maxY-minY;
    if(!(w>0)) w=1; if(!(h>0)) h=1;
    QString vb = fmt(minX)+" "+fmt(minY)+" "+fmt(w)+" "+fmt(h);
    QString out = "<svg xmlns='http://www.w3.org/2000/svg' viewBox='"+vb+"'>";
    out += regionElems;
    for(size_t a=0;a<aps.size();a++){
        GerAp& ap=aps[a];
        if(ap.trace.isEmpty()) continue;
        double w2 = (ap.type==0)? ap.a : ((ap.type==1||ap.type==2)? (ap.a>ap.b?ap.a:ap.b) : ap.a);
        if(!(w2>0)) w2=0.001;
        out += "<path d='"+ap.trace+"' fill='none' stroke='#000000' stroke-width='"+fmt(w2)+
               "' stroke-linecap='round' stroke-linejoin='round'/>";
    }
    out += flashElems;
    out += "</svg>";
    return out;
}


// --- Excellon / XNC drill (.drl) -> SVG --------------------------------------
// Parse a signed number at s[i]; on success advance i, set val, hasDot, ndig
// (digit count, excluding sign/dot). For integer (no-dot) form val is the raw
// integer value (with sign); caller applies implied decimals.
static bool excellonNum(const QString& s, int& i, double& val, bool& hasDot, int& ndig){
    int n = s.size();
    bool neg = false;
    if(i < n && (s[i] == QLatin1Char('+') || s[i] == QLatin1Char('-'))){ neg = (s[i] == QLatin1Char('-')); i++; }
    long long intPart = 0; double frac = 0.0, fscale = 1.0;
    hasDot = false; ndig = 0; bool any = false;
    while(i < n){
        ushort u = s[i].unicode();
        if(u == '.'){ hasDot = true; i++; continue; }
        if(u >= '0' && u <= '9'){
            any = true; ndig++;
            if(hasDot){ fscale *= 10.0; frac += (double)(u - '0') / fscale; }
            else { intPart = intPart * 10 + (long long)(u - '0'); }
            i++;
            if(ndig >= 15) break; // overflow / runaway guard
        } else break;
    }
    if(!any) return false;
    val = hasDot ? ((double)intPart + frac) : (double)intPart;
    if(neg) val = -val;
    return true;
}

QString excellonToSvg(const u8* p, i64 n){
    if(!p || n < 4) return QString();
    // Decode as text (cap size so a hostile file can't blow up memory).
    i64 cap = n < (i64)64 * 1024 * 1024 ? n : (i64)64 * 1024 * 1024;
    QString text = QString::fromUtf8((const char*)p, (int)cap);
    if(text.isEmpty()) return QString();

    // Signature sniff: must look like an Excellon/XNC drill program.
    bool sig = text.contains(QLatin1String("M48"))
            || text.contains(QLatin1String("FMAT"))
            || ((text.contains(QLatin1String("METRIC")) || text.contains(QLatin1String("INCH")))
                 && text.contains(QLatin1Char('T')));
    if(!sig) return QString();

    bool inch = false, metric = false;
    bool leadingSuppressed = true; // default: leading zeros omitted -> value = raw/10^dec

    std::vector<std::pair<int,double>> tools; // (code, diameter)
    double curDia = -1.0;

    struct EHole { double x, y, r; };
    std::vector<EHole> holes;
    double lastX = 0.0, lastY = 0.0;
    const int MAXHOLES = 2000000;

    int len = text.size();
    int lineStart = 0;
    for(int idx = 0; idx <= len; idx++){
        if(idx < len){ ushort u = text[idx].unicode(); if(u != '\n' && u != '\r') continue; }
        QString line = text.mid(lineStart, idx - lineStart).trimmed();
        lineStart = idx + 1;
        if(line.isEmpty()) continue;
        if(line[0] == QLatin1Char(';')) continue; // comment

        // Units / zero-suppression tokens (may share a line, e.g. "METRIC,TZ").
        if(line.contains(QLatin1String("METRIC"))){ metric = true; inch = false; }
        if(line.contains(QLatin1String("INCH"))){ inch = true; metric = false; }
        if(line.contains(QLatin1String("LZ"))) leadingSuppressed = false; // leading zeros present -> pad right
        if(line.contains(QLatin1String("TZ"))) leadingSuppressed = true;  // trailing zeros present -> pad left

        ushort c0 = line[0].unicode();

        // Tool definition (T..C..) or selection (T..).
        if(c0 == 'T'){
            int i = 1; long long code = 0; bool anyc = false;
            while(i < line.size() && line[i].unicode() >= '0' && line[i].unicode() <= '9'){
                code = code * 10 + (long long)(line[i].unicode() - '0'); anyc = true; i++;
                if(code > 100000000LL) break;
            }
            if(anyc){
                double dia = -1.0;
                int ci = i;
                while(ci < line.size() && line[ci].unicode() != 'C') ci++;
                if(ci < line.size() && line[ci].unicode() == 'C'){
                    int j = ci + 1; bool dot; int nd; double v;
                    if(excellonNum(line, j, v, dot, nd)) dia = v;
                }
                if(dia > 0){
                    bool found = false;
                    for(auto& t : tools){ if(t.first == (int)code){ t.second = dia; found = true; break; } }
                    // Cap the tool table: without a bound, a file of millions of distinct T-defs makes the
                    // linear-scan dedup O(N^2) → multi-hour CPU hang (holes are already capped at MAXHOLES).
                    if(!found && tools.size() < 100000) tools.push_back(std::make_pair((int)code, dia));
                } else {
                    curDia = -1.0;
                    for(auto& t : tools){ if(t.first == (int)code){ curDia = t.second; break; } }
                }
            }
            continue;
        }

        // Coordinate line: flash a hole with the current tool.
        if(line.contains(QLatin1Char('X')) || line.contains(QLatin1Char('Y'))){
            double X = lastX, Y = lastY; bool gotX = false, gotY = false;
            int i = 0, L = line.size();
            while(i < L){
                ushort u = line[i].unicode();
                if(u == 'X' || u == 'Y'){
                    int j = i + 1; bool dot; int nd; double v;
                    if(excellonNum(line, j, v, dot, nd)){
                        double coord;
                        if(dot) coord = v;
                        else {
                            int intDig = inch ? 2 : 3;
                            int decDig = inch ? 4 : 3;
                            if(leadingSuppressed) coord = v / std::pow(10.0, decDig);
                            else { int total = intDig + decDig; int missing = total - nd; if(missing < 0) missing = 0;
                                   coord = v * std::pow(10.0, (double)missing) / std::pow(10.0, decDig); }
                        }
                        if(u == 'X'){ X = coord; gotX = true; } else { Y = coord; gotY = true; }
                        i = j; continue;
                    }
                }
                i++;
            }
            if(gotX) lastX = X;
            if(gotY) lastY = Y;
            if((gotX || gotY) && curDia > 0){
                double f = inch ? 25.4 : 1.0; // normalise to mm
                EHole h; h.x = X * f; h.y = Y * f; h.r = (curDia * f) / 2.0;
                if(std::isfinite(h.x) && std::isfinite(h.y) && std::isfinite(h.r) && h.r > 0){
                    if((int)holes.size() < MAXHOLES) holes.push_back(h);
                }
            }
            continue;
        }
        // M30/M00 (end) and other M/G codes: nothing further to do.
    }

    if(holes.empty()) return QString();

    double minX = 1e18, minY = 1e18, maxX = -1e18, maxY = -1e18;
    for(auto& h : holes){
        minX = qMin(minX, h.x - h.r); maxX = qMax(maxX, h.x + h.r);
        minY = qMin(minY, h.y - h.r); maxY = qMax(maxY, h.y + h.r);
    }
    if(!(std::isfinite(minX) && std::isfinite(maxX) && std::isfinite(minY) && std::isfinite(maxY)))
        return QString();
    double w = maxX - minX, hh = maxY - minY;
    if(w <= 0) w = 1.0; if(hh <= 0) hh = 1.0;

    QString svg;
    svg.reserve(holes.size() * 64 + 128);
    svg += QStringLiteral("<svg xmlns='http://www.w3.org/2000/svg' viewBox='%1 %2 %3 %4'>")
             .arg(QString::number(minX, 'f', 4), QString::number(minY, 'f', 4),
                  QString::number(w, 'f', 4), QString::number(hh, 'f', 4));
    double flip = minY + maxY; // reflect Y so PCB origin (bottom-left) reads upright
    for(auto& h : holes){
        double cyf = flip - h.y;
        svg += QStringLiteral("<circle cx='%1' cy='%2' r='%3' fill='#333333'/>")
                 .arg(QString::number(h.x, 'f', 4), QString::number(cyf, 'f', 4),
                      QString::number(h.r, 'f', 4));
    }
    svg += QStringLiteral("</svg>");
    return svg;
}


// ---- Lottie / Bodymovin animation JSON -> static first-frame SVG snapshot ----
// Schema: airbnb/lottie (bodymovin). Root: {"v","w","h","fr","ip","op","layers":[...]}.
// Shape layer ty==4 has "ks" transform + "shapes":[...]. Shape items carry a string
// "ty": "gr"(group,"it":[...]), "rc"(rect p/s/r), "el"(ellipse p/s), "sh"(path
// "ks"->{v,i,o,c}), "fl"(fill "c"), "st"(stroke "c","w"), "tr"(group transform).
// Property object {"a":0,"k":val} static | {"a":1,"k":[{"s":[...]}]} animated->k[0].s.
// Best-effort translation only (position - anchor); scale/rotation skipped.

static QString lottieNumFmt(double v){
    if(!std::isfinite(v)) return QString();
    return QString::number(v,'f',3);
}

// [r,g,b(,a)] in 0..1 -> validated "#rrggbb" (only hex digits emitted; safe in attr).
static QString lottieColHex(const QVector<double>& c){
    if(c.size()<3) return QString();
    if(!std::isfinite(c[0])||!std::isfinite(c[1])||!std::isfinite(c[2])) return QString();
    auto cv=[](double x)->int{ int v=(int)std::lround(x*255.0); if(v<0)v=0; if(v>255)v=255; return v; };
    return QStringLiteral("#%1%2%3")
        .arg(cv(c[0]),2,16,QLatin1Char('0'))
        .arg(cv(c[1]),2,16,QLatin1Char('0'))
        .arg(cv(c[2]),2,16,QLatin1Char('0'));
}

// Resolve a Lottie animated/static property to its first-frame value.
static QJsonValue lottiePropVal(const QJsonObject& prop){
    QJsonValue k = prop.value(QStringLiteral("k"));
    int a = prop.value(QStringLiteral("a")).toInt(0);
    if(a==1 && k.isArray()){
        QJsonArray arr=k.toArray();
        if(arr.isEmpty()) return QJsonValue();
        QJsonValue first=arr.at(0);
        if(first.isObject()){
            // keyframe object -> "s" (start value) is the first-frame value
            return first.toObject().value(QStringLiteral("s"));
        }
        return first;
    }
    return k;
}

// Resolve a property to a flat array of doubles (scalars become 1-element arrays).
static QVector<double> lottiePropArr(const QJsonObject& prop){
    QVector<double> out;
    QJsonValue v=lottiePropVal(prop);
    if(v.isArray()){
        QJsonArray a=v.toArray();
        for(int i=0;i<a.size() && out.size()<8; ++i) out.push_back(a.at(i).toDouble());
    } else if(v.isDouble()){
        out.push_back(v.toDouble());
    }
    return out;
}

// Safe [x,y] from a vertex/tangent array element.
static bool lottieXY(const QJsonValue& v, double& x, double& y){
    if(!v.isArray()) return false;
    QJsonArray a=v.toArray();
    if(a.size()<2) return false;
    x=a.at(0).toDouble(); y=a.at(1).toDouble();
    return std::isfinite(x)&&std::isfinite(y);
}

struct LottieCtx { QString out; long long shapeCount=0; long long vertCount=0; };
static const long long kLottieMaxShapes = 200000;
static const long long kLottieMaxVerts  = 2000000;

// Recursively walk a shape-item array. dx/dy = accumulated translation.
// Inherited fill/stroke propagate into nested groups.
static void lottieWalk(const QJsonArray& items, double dx, double dy,
                       QString inhFill, QString inhStroke, double inhSW,
                       LottieCtx& cx){
    if(cx.shapeCount>=kLottieMaxShapes || cx.vertCount>=kLottieMaxVerts) return;

    // First pass: collect this group's own transform (tr), fill (fl), stroke (st).
    double gx=0.0, gy=0.0;
    QString fill=inhFill, stroke=inhStroke; double sw=inhSW;
    for(const QJsonValue& itv : items){
        if(!itv.isObject()) continue;
        QJsonObject it=itv.toObject();
        QString ty=it.value(QStringLiteral("ty")).toString();
        if(ty==QStringLiteral("tr")){
            QVector<double> p=lottiePropArr(it.value(QStringLiteral("p")).toObject());
            QVector<double> an=lottiePropArr(it.value(QStringLiteral("a")).toObject());
            if(p.size()>=2){ gx+=p[0]; gy+=p[1]; }
            if(an.size()>=2){ gx-=an[0]; gy-=an[1]; }
        } else if(ty==QStringLiteral("fl")){
            QString h=lottieColHex(lottiePropArr(it.value(QStringLiteral("c")).toObject()));
            if(!h.isEmpty()) fill=h;
        } else if(ty==QStringLiteral("st")){
            QString h=lottieColHex(lottiePropArr(it.value(QStringLiteral("c")).toObject()));
            if(!h.isEmpty()) stroke=h;
            QVector<double> w=lottiePropArr(it.value(QStringLiteral("w")).toObject());
            if(!w.isEmpty() && std::isfinite(w[0]) && w[0]>0) sw=w[0];
        }
    }
    double ndx=dx+gx, ndy=dy+gy;
    if(!std::isfinite(ndx)||!std::isfinite(ndy)) return;

    // Effective style: default to opaque black if a shape has neither fill nor stroke.
    QString effFill=fill, effStroke=stroke;
    auto styleFor=[&](bool isPath)->QString{
        QString f=effFill, s=effStroke;
        if(f.isEmpty() && s.isEmpty()) f=QStringLiteral("#000000");
        QString a;
        a += f.isEmpty() ? QStringLiteral(" fill='none'") : (QStringLiteral(" fill='")+f+QStringLiteral("'"));
        if(!s.isEmpty()){
            double w = (sw>0.0 && std::isfinite(sw)) ? sw : 1.0;
            a += QStringLiteral(" stroke='")+s+QStringLiteral("'");
            a += QStringLiteral(" stroke-width='")+lottieNumFmt(w)+QStringLiteral("'");
        }
        (void)isPath;
        return a;
    };

    // Second pass: draw primitives / recurse into nested groups.
    for(const QJsonValue& itv : items){
        if(cx.shapeCount>=kLottieMaxShapes || cx.vertCount>=kLottieMaxVerts) return;
        if(!itv.isObject()) continue;
        QJsonObject it=itv.toObject();
        QString ty=it.value(QStringLiteral("ty")).toString();

        if(ty==QStringLiteral("gr")){
            lottieWalk(it.value(QStringLiteral("it")).toArray(), ndx, ndy,
                       fill, stroke, sw, cx);
        }
        else if(ty==QStringLiteral("rc")){
            QVector<double> p=lottiePropArr(it.value(QStringLiteral("p")).toObject());
            QVector<double> s=lottiePropArr(it.value(QStringLiteral("s")).toObject());
            if(p.size()<2 || s.size()<2) continue;
            double w=s[0], h=s[1];
            double x=p[0]-w/2.0+ndx, y=p[1]-h/2.0+ndy;
            double r=0.0;
            { QVector<double> rr=lottiePropArr(it.value(QStringLiteral("r")).toObject());
              if(!rr.isEmpty() && std::isfinite(rr[0]) && rr[0]>0) r=rr[0]; }
            QString sx=lottieNumFmt(x), sy=lottieNumFmt(y),
                    sw2=lottieNumFmt(w), sh2=lottieNumFmt(h);
            if(sx.isEmpty()||sy.isEmpty()||sw2.isEmpty()||sh2.isEmpty()) continue;
            if(w<=0||h<=0) continue;
            cx.out += QStringLiteral("<rect x='")+sx+QStringLiteral("' y='")+sy
                    + QStringLiteral("' width='")+sw2+QStringLiteral("' height='")+sh2+QStringLiteral("'");
            if(r>0){ QString sr=lottieNumFmt(r); if(!sr.isEmpty())
                cx.out += QStringLiteral(" rx='")+sr+QStringLiteral("'"); }
            cx.out += styleFor(false)+QStringLiteral("/>\n");
            cx.shapeCount++;
        }
        else if(ty==QStringLiteral("el")){
            QVector<double> p=lottiePropArr(it.value(QStringLiteral("p")).toObject());
            QVector<double> s=lottiePropArr(it.value(QStringLiteral("s")).toObject());
            if(p.size()<2 || s.size()<2) continue;
            double cxp=p[0]+ndx, cyp=p[1]+ndy, rx=s[0]/2.0, ry=s[1]/2.0;
            QString scx=lottieNumFmt(cxp), scy=lottieNumFmt(cyp),
                    srx=lottieNumFmt(rx), sry=lottieNumFmt(ry);
            if(scx.isEmpty()||scy.isEmpty()||srx.isEmpty()||sry.isEmpty()) continue;
            if(rx<=0||ry<=0) continue;
            cx.out += QStringLiteral("<ellipse cx='")+scx+QStringLiteral("' cy='")+scy
                    + QStringLiteral("' rx='")+srx+QStringLiteral("' ry='")+sry+QStringLiteral("'")
                    + styleFor(false)+QStringLiteral("/>\n");
            cx.shapeCount++;
        }
        else if(ty==QStringLiteral("sh")){
            QJsonValue shv=lottiePropVal(it.value(QStringLiteral("ks")).toObject());
            if(!shv.isObject()) continue;
            QJsonObject sh=shv.toObject();
            QJsonArray v =sh.value(QStringLiteral("v")).toArray();
            QJsonArray in=sh.value(QStringLiteral("i")).toArray();
            QJsonArray ou=sh.value(QStringLiteral("o")).toArray();
            bool closed=sh.value(QStringLiteral("c")).toBool();
            int nv=v.size();
            if(nv<2) continue;
            if(nv>500000) nv=500000; // hard cap per path
            double x0,y0;
            if(!lottieXY(v.at(0), x0, y0)) continue;
            QString d0x=lottieNumFmt(x0+ndx), d0y=lottieNumFmt(y0+ndy);
            if(d0x.isEmpty()||d0y.isEmpty()) continue;
            QString d=QStringLiteral("M ")+d0x+QStringLiteral(" ")+d0y;
            int segs = closed ? nv : nv-1;
            bool ok=true;
            for(int j=0;j<segs;j++){
                if(cx.vertCount>=kLottieMaxVerts){ ok=false; break; }
                int a=j, b=(j+1)%nv;
                double ax,ay,bx,by;
                if(!lottieXY(v.at(a),ax,ay) || !lottieXY(v.at(b),bx,by)){ ok=false; break; }
                double aox=0,aoy=0,bix=0,biy=0;
                if(a<ou.size()) lottieXY(ou.at(a),aox,aoy);
                if(b<in.size()) lottieXY(in.at(b),bix,biy);
                double c1x=ax+aox+ndx, c1y=ay+aoy+ndy;
                double c2x=bx+bix+ndx, c2y=by+biy+ndy;
                double ex=bx+ndx, ey=by+ndy;
                QString s1=lottieNumFmt(c1x), s2=lottieNumFmt(c1y),
                        s3=lottieNumFmt(c2x), s4=lottieNumFmt(c2y),
                        s5=lottieNumFmt(ex),  s6=lottieNumFmt(ey);
                if(s1.isEmpty()||s2.isEmpty()||s3.isEmpty()||s4.isEmpty()||s5.isEmpty()||s6.isEmpty()){ ok=false; break; }
                d += QStringLiteral(" C ")+s1+QStringLiteral(" ")+s2+QStringLiteral(" ")
                   + s3+QStringLiteral(" ")+s4+QStringLiteral(" ")+s5+QStringLiteral(" ")+s6;
                cx.vertCount++;
            }
            if(!ok || d.isEmpty()) continue;
            if(closed) d += QStringLiteral(" Z");
            cx.out += QStringLiteral("<path d=\"")+d+QStringLiteral("\"")+styleFor(true)+QStringLiteral("/>\n");
            cx.shapeCount++;
        }
    }
}

QString lottieToSvg(const u8* p, i64 n){
    if(!p || n<2) return QString();

    // dotLottie (.lottie) = ZIP (PK\x03\x04) containing animations/*.json.
    QByteArray jsonBytes;
    if(n>=4 && p[0]=='P' && p[1]=='K' && p[2]==0x03 && p[3]==0x04){
        QByteArray entry;
        bool got=false;
        QStringList names = xqUnzipList(p, n);
        // Prefer animations/*.json, then any *.json.
        for(const QString& nm : names){
            QString low=nm.toLower();
            if(low.startsWith(QStringLiteral("animations/")) && low.endsWith(QStringLiteral(".json"))){
                if(xqUnzip(p,n,nm,entry) && !entry.isEmpty()){ got=true; break; }
            }
        }
        if(!got){
            for(const QString& nm : names){
                if(nm.toLower().endsWith(QStringLiteral(".json"))){
                    if(xqUnzip(p,n,nm,entry) && !entry.isEmpty()){ got=true; break; }
                }
            }
        }
        if(!got) return QString();
        jsonBytes=entry;
    } else {
        // Cheap sniff: JSON object that plausibly mentions lottie fields.
        i64 sl = n<4096 ? n : 4096;
        bool brace=false, sawLayers=false;
        for(i64 i=0;i<sl;i++){ if(p[i]=='{'){ brace=true; break; } if(p[i]!=' '&&p[i]!='\t'&&p[i]!='\r'&&p[i]!='\n'&&p[i]!=(char)0xEF&&p[i]!=(char)0xBB&&p[i]!=(char)0xBF) break; }
        if(!brace) return QString();
        for(i64 i=0;i+6<=sl;i++){
            if(p[i]=='l'&&p[i+1]=='a'&&p[i+2]=='y'&&p[i+3]=='e'&&p[i+4]=='r'&&p[i+5]=='s'){ sawLayers=true; break; }
        }
        if(!sawLayers) return QString();
        jsonBytes=QByteArray(reinterpret_cast<const char*>(p),
                             int(n < (i64)0x7fffffff ? n : (i64)0x7fffffff));
    }

    QJsonParseError perr;
    QJsonDocument doc=QJsonDocument::fromJson(jsonBytes, &perr);
    if(perr.error!=QJsonParseError::NoError || !doc.isObject()) return QString();
    QJsonObject root=doc.object();

    if(!root.contains(QStringLiteral("layers"))) return QString();
    double W=root.value(QStringLiteral("w")).toDouble(0);
    double H=root.value(QStringLiteral("h")).toDouble(0);
    if(!std::isfinite(W)||!std::isfinite(H)||W<=0||H<=0||W>200000||H>200000) return QString();

    QJsonArray layers=root.value(QStringLiteral("layers")).toArray();
    if(layers.isEmpty()) return QString();

    LottieCtx cx;
    // Draw bottom layer first so index 0 (topmost in Lottie) ends up on top.
    for(int li=layers.size()-1; li>=0; --li){
        if(cx.shapeCount>=kLottieMaxShapes) break;
        QJsonValue lv=layers.at(li);
        if(!lv.isObject()) continue;
        QJsonObject layer=lv.toObject();
        if(layer.value(QStringLiteral("ty")).toInt(-1)!=4) continue; // shape layers only

        // Layer transform: translation = position - anchor (best effort).
        double lx=0.0, ly=0.0;
        QJsonObject ks=layer.value(QStringLiteral("ks")).toObject();
        { QVector<double> pp=lottiePropArr(ks.value(QStringLiteral("p")).toObject());
          QVector<double> aa=lottiePropArr(ks.value(QStringLiteral("a")).toObject());
          if(pp.size()>=2){ lx+=pp[0]; ly+=pp[1]; }
          if(aa.size()>=2){ lx-=aa[0]; ly-=aa[1]; } }

        QJsonArray shapes=layer.value(QStringLiteral("shapes")).toArray();
        if(shapes.isEmpty()) continue;
        lottieWalk(shapes, lx, ly, QString(), QString(), 0.0, cx);
    }

    if(cx.out.isEmpty()) return QString(); // nothing renderable -> fall through

    QString svg=QStringLiteral("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 ")
              + lottieNumFmt(W)+QStringLiteral(" ")+lottieNumFmt(H)+QStringLiteral("'>\n")
              + cx.out
              + QStringLiteral("</svg>\n");
    return svg;
}


// ===================== Sketch (.sketch) -> SVG bridge =====================
// Spec: Sketch file format (developer.sketch.com/file-format). A .sketch file
// is a ZIP archive: "document.json", "pages/<uuid>.json" (one per page),
// "meta.json". A page JSON is a layer tree: {"_class":"page","layers":[...]}.
// Layer nodes carry "_class" ("rectangle","oval","shapePath","text","group",
// "artboard","shapeGroup","symbolMaster"), a "frame":{x,y,width,height}
// (relative to the parent's frame origin), and a "style":{"fills":[{"color":
// {red,green,blue,alpha}}...],"borders":[...]}. shapePath "points" are strings
// "{px, py}" normalised 0..1 within the frame. We accumulate parent offsets,
// emit <rect>/<ellipse>/<path>/<text>, and compute the viewBox from all frames.

// Clamp a Sketch 0..1 colour channel to a 0..255 int.
static inline int sketchChan(double v){
    if(!std::isfinite(v)) return 0;
    int i = (int)std::lround(v*255.0);
    if(i<0) i=0; if(i>255) i=255; return i;
}
// A colour object {red,green,blue,...} 0..1 -> "#rrggbb", or empty if absent.
static QString sketchColorHex(const QJsonObject& col){
    if(col.isEmpty()) return QString();
    if(!col.contains("red") && !col.contains("green") && !col.contains("blue")) return QString();
    int r=sketchChan(col.value("red").toDouble());
    int g=sketchChan(col.value("green").toDouble());
    int b=sketchChan(col.value("blue").toDouble());
    static const char* hx="0123456789abcdef";
    QString s="#";
    s+=hx[(r>>4)&0xf]; s+=hx[r&0xf];
    s+=hx[(g>>4)&0xf]; s+=hx[g&0xf];
    s+=hx[(b>>4)&0xf]; s+=hx[b&0xf];
    return s;
}
// Validate a colour string we are about to inject into an attribute: accept
// only "#" + 6 hex digits (all we ever produce). Anything else -> empty.
static QString sketchSafeColor(const QString& c){
    if(c.size()!=7 || c[0]!=QLatin1Char('#')) return QString();
    for(int i=1;i<7;i++){ QChar ch=c[i];
        bool ok=(ch>=QLatin1Char('0')&&ch<=QLatin1Char('9'))||(ch>=QLatin1Char('a')&&ch<=QLatin1Char('f'));
        if(!ok) return QString();
    }
    return c;
}
// XML-escape text destined for character data / attribute values.
static QString sketchXmlEsc(const QString& in){
    QString o; o.reserve(in.size());
    for(QChar c : in){
        if(c==QLatin1Char('&')) o+=QStringLiteral("&amp;");
        else if(c==QLatin1Char('<')) o+=QStringLiteral("&lt;");
        else if(c==QLatin1Char('>')) o+=QStringLiteral("&gt;");
        else if(c==QLatin1Char('"')) o+=QStringLiteral("&quot;");
        else if(c==QLatin1Char('\'')) o+=QStringLiteral("&#39;");
        else if(c==QLatin1Char('\n')||c==QLatin1Char('\r')||c==QLatin1Char('\t')||c.unicode()>=0x20) o+=c;
        // drop other control chars
    }
    return o;
}
// Parse a Sketch point string "{px, py}" -> two doubles. false on malformed.
static bool sketchParsePoint(const QString& s, double& x, double& y){
    int a=s.indexOf(QLatin1Char('{')); int b=s.indexOf(QLatin1Char('}'));
    if(a<0||b<0||b<=a) return false;
    QString inner=s.mid(a+1,b-a-1);
    int comma=inner.indexOf(QLatin1Char(','));
    if(comma<0) return false;
    bool ok1=false, ok2=false;
    x=inner.left(comma).trimmed().toDouble(&ok1);
    y=inner.mid(comma+1).trimmed().toDouble(&ok2);
    return ok1 && ok2 && std::isfinite(x) && std::isfinite(y);
}
static inline QString sketchNum(double v){ return QString::number(v,'f',3); }

// Shared recursion state.
struct SketchCtx {
    QStringList body;
    double minx, miny, maxx, maxy;
    bool anyBox;      // did we see any bounded shape (for viewBox)
    bool anyDraw;     // did we emit any drawable element
    int layers;       // total layers visited (cap guard)
    int points;       // total points emitted (cap guard)
};

// Pull the first enabled fill colour ("#rrggbb") from a style object.
static QString sketchFill(const QJsonObject& node){
    QJsonObject style=node.value("style").toObject();
    QJsonArray fills=style.value("fills").toArray();
    for(const QJsonValue& fv : fills){
        QJsonObject f=fv.toObject();
        if(f.contains("isEnabled") && !f.value("isEnabled").toBool(true)) continue;
        QString c=sketchSafeColor(sketchColorHex(f.value("color").toObject()));
        if(!c.isEmpty()) return c;
    }
    return QString();
}
// Pull the first enabled border {colour,width}.
static bool sketchStroke(const QJsonObject& node, QString& col, double& w){
    QJsonObject style=node.value("style").toObject();
    QJsonArray bs=style.value("borders").toArray();
    for(const QJsonValue& bv : bs){
        QJsonObject b=bv.toObject();
        if(b.contains("isEnabled") && !b.value("isEnabled").toBool(true)) continue;
        QString c=sketchSafeColor(sketchColorHex(b.value("color").toObject()));
        if(c.isEmpty()) continue;
        col=c;
        double t=b.value("thickness").toDouble(1.0);
        w=(std::isfinite(t)&&t>0)?t:1.0;
        return true;
    }
    return false;
}
// Extract the text content of a "text" layer.
static QString sketchTextString(const QJsonObject& node){
    QJsonValue as=node.value("attributedString");
    if(as.isObject()){
        QString s=as.toObject().value("string").toString();
        if(!s.isEmpty()) return s;
    }
    if(as.isString() && !as.toString().isEmpty()) return as.toString();
    return node.value("string").toString();
}

static void sketchAddBox(SketchCtx& c, double x, double y, double w, double h){
    if(!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(w)||!std::isfinite(h)) return;
    double x2=x+w, y2=y+h;
    if(!c.anyBox){ c.minx=std::min(x,x2); c.miny=std::min(y,y2); c.maxx=std::max(x,x2); c.maxy=std::max(y,y2); c.anyBox=true; }
    else { c.minx=std::min(c.minx,std::min(x,x2)); c.miny=std::min(c.miny,std::min(y,y2));
           c.maxx=std::max(c.maxx,std::max(x,x2)); c.maxy=std::max(c.maxy,std::max(y,y2)); }
}

static void sketchWalk(const QJsonObject& node, double ox, double oy, SketchCtx& c, int depth){
    if(depth>200) return;                       // hostile deep-nesting guard
    if(c.layers>=200000) return;                // layer cap
    c.layers++;

    QString cls=node.value("_class").toString();
    QJsonObject fr=node.value("frame").toObject();
    double fx=fr.value("x").toDouble(0.0), fy=fr.value("y").toDouble(0.0);
    double fw=fr.value("width").toDouble(0.0), fh=fr.value("height").toDouble(0.0);
    if(!std::isfinite(fx)) fx=0; if(!std::isfinite(fy)) fy=0;
    if(!std::isfinite(fw)) fw=0; if(!std::isfinite(fh)) fh=0;
    double ax=ox+fx, ay=oy+fy;                  // absolute top-left
    // Nested groups with huge finite frame offsets can overflow the accumulated ax/ay to inf; clamp so
    // no leaf branch emits x='inf'/cx='inf' into the SVG (sketchNum would print the literal "inf").
    if(!std::isfinite(ax)) ax=0; if(!std::isfinite(ay)) ay=0;

    // Container classes: recurse into "layers" with an accumulated offset.
    if(cls==QLatin1String("page")||cls==QLatin1String("group")||cls==QLatin1String("artboard")||
       cls==QLatin1String("shapeGroup")||cls==QLatin1String("symbolMaster")||cls==QLatin1String("symbolInstance")){
        // Artboards define a visible box; count it toward the bbox.
        if((cls==QLatin1String("artboard")) && fw>0 && fh>0) sketchAddBox(c,ax,ay,fw,fh);
        QJsonArray kids=node.value("layers").toArray();
        for(const QJsonValue& kv : kids){
            if(c.layers>=200000) break;
            if(kv.isObject()) sketchWalk(kv.toObject(), ax, ay, c, depth+1);
        }
        return;
    }

    if(cls==QLatin1String("rectangle")){
        if(fw<=0||fh<=0) return;
        sketchAddBox(c,ax,ay,fw,fh);
        QString fill=sketchFill(node);
        QString scol; double sw=0; bool hasStroke=sketchStroke(node,scol,sw);
        QString e="<rect x='"+sketchNum(ax)+"' y='"+sketchNum(ay)+"' width='"+sketchNum(fw)+
                  "' height='"+sketchNum(fh)+"' fill='"+(fill.isEmpty()?QStringLiteral("none"):fill)+"'";
        if(hasStroke) e+=" stroke='"+scol+"' stroke-width='"+sketchNum(sw)+"'";
        e+="/>";
        c.body<<e; c.anyDraw=true;
        return;
    }
    if(cls==QLatin1String("oval")){
        if(fw<=0||fh<=0) return;
        sketchAddBox(c,ax,ay,fw,fh);
        QString fill=sketchFill(node);
        QString scol; double sw=0; bool hasStroke=sketchStroke(node,scol,sw);
        QString e="<ellipse cx='"+sketchNum(ax+fw/2.0)+"' cy='"+sketchNum(ay+fh/2.0)+
                  "' rx='"+sketchNum(fw/2.0)+"' ry='"+sketchNum(fh/2.0)+
                  "' fill='"+(fill.isEmpty()?QStringLiteral("none"):fill)+"'";
        if(hasStroke) e+=" stroke='"+scol+"' stroke-width='"+sketchNum(sw)+"'";
        e+="/>";
        c.body<<e; c.anyDraw=true;
        return;
    }
    if(cls==QLatin1String("text")){
        if(fw>0&&fh>0) sketchAddBox(c,ax,ay,fw,fh); else sketchAddBox(c,ax,ay,1,1);
        QString txt=sketchXmlEsc(sketchTextString(node));
        if(txt.isEmpty()) return;
        QString fill=sketchFill(node);
        double fontSize=(fh>0)? std::min(fh, 96.0) : 12.0;
        // baseline: put text near the top of its frame box
        double by=ay + (fh>0? std::min(fh*0.8, fontSize) : fontSize);
        QString e="<text x='"+sketchNum(ax)+"' y='"+sketchNum(by)+
                  "' font-size='"+sketchNum(fontSize)+"' fill='"+
                  (fill.isEmpty()?QStringLiteral("#000000"):fill)+"'>"+txt+"</text>";
        c.body<<e; c.anyDraw=true;
        return;
    }
    if(cls==QLatin1String("shapePath")||cls==QLatin1String("polygon")||cls==QLatin1String("star")||
       cls==QLatin1String("triangle")){
        QJsonArray pts=node.value("points").toArray();
        if(pts.isEmpty()) return;
        bool closed=node.value("isClosed").toBool(false);
        QString d; int emitted=0;
        for(const QJsonValue& pv : pts){
            if(c.points>=3000000) break;
            QString ps=pv.toObject().value("point").toString();
            double px,py;
            if(!sketchParsePoint(ps,px,py)) continue;
            double X=ax+px*fw, Y=ay+py*fh;
            if(!std::isfinite(X)||!std::isfinite(Y)) continue;
            d += (emitted==0? QStringLiteral("M "):QStringLiteral(" L "));
            d += sketchNum(X)+QLatin1Char(' ')+sketchNum(Y);
            sketchAddBox(c,X,Y,0,0);
            emitted++; c.points++;
        }
        if(emitted<2) return;
        if(closed) d+=QStringLiteral(" Z");
        QString fill = closed? sketchFill(node) : QString();
        QString scol; double sw=0; bool hasStroke=sketchStroke(node,scol,sw);
        QString e="<path d=\""+d+"\" fill='"+(fill.isEmpty()?QStringLiteral("none"):fill)+"'";
        if(hasStroke) e+=" stroke='"+scol+"' stroke-width='"+sketchNum(sw)+"'";
        else if(fill.isEmpty()) e+=" stroke='#000000' stroke-width='1'";
        e+="/>";
        c.body<<e; c.anyDraw=true;
        return;
    }
    // Unhandled class (slice, bitmap, hotspot, ...): skip silently.
}

QString sketchToSvg(const u8* p, i64 n){
    if(!p || n<22) return QString();
    if(p[0]!='P'||p[1]!='K') return QString();          // ZIP local/central sig starts 'PK'

    QStringList names=xqUnzipList(p,n);
    if(names.isEmpty()) return QString();

    // Locate the first "pages/*.json" entry.
    QString pageEntry;
    for(const QString& nm : names){
        QString low=nm.toLower();
        if(low.startsWith(QLatin1String("pages/")) && low.endsWith(QLatin1String(".json"))){ pageEntry=nm; break; }
    }
    if(pageEntry.isEmpty()) return QString();

    QByteArray pg;
    if(!xqUnzip(p,n,pageEntry,pg) || pg.isEmpty()) return QString();
    if(pg.size() > 128*1024*1024) return QString();     // sanity cap

    QJsonParseError perr;
    QJsonDocument doc=QJsonDocument::fromJson(pg,&perr);
    if(perr.error!=QJsonParseError::NoError || !doc.isObject()) return QString();
    QJsonObject root=doc.object();

    SketchCtx c; c.minx=c.miny=c.maxx=c.maxy=0; c.anyBox=false; c.anyDraw=false; c.layers=0; c.points=0;
    sketchWalk(root, 0.0, 0.0, c, 0);

    if(!c.anyDraw || !c.anyBox) return QString();

    double w=c.maxx-c.minx, h=c.maxy-c.miny;
    if(!(w>0)) w=1; if(!(h>0)) h=1;
    if(!std::isfinite(c.minx)||!std::isfinite(c.miny)||!std::isfinite(w)||!std::isfinite(h)) return QString();

    QString svg="<svg xmlns='http://www.w3.org/2000/svg' viewBox='"+
        sketchNum(c.minx)+QLatin1Char(' ')+sketchNum(c.miny)+QLatin1Char(' ')+
        sketchNum(w)+QLatin1Char(' ')+sketchNum(h)+"'>";
    for(const QString& e : c.body) svg+=e;
    svg+="</svg>";
    return svg;
}


// ---------------------------------------------------------------------------
// TGS  (Telegram animated sticker)
//
// A .tgs file is nothing more than a GZIP-compressed Lottie/Bodymovin JSON
// animation.  Telegram's own tooling documents this: stickers are exported
// from Adobe After Effects via the Bodymovin/Lottie plugin and then gzip'd
// with the ".tgs" extension.  (See Telegram "Animated Stickers" developer
// note; also the dotLottie / TGS convention: a raw gzip stream, RFC 1952,
// whose inflated payload is a standard Bodymovin JSON object with the usual
// "v"/"fr"/"ip"/"op"/"w"/"h"/"layers" keys.)
//
// We therefore do the minimum: detect the gzip magic (0x1f 0x8b), inflate the
// whole stream with zlib's gzip window (inflateInit2(15+16)), cap the output
// against a decompression bomb, and DELEGATE to the sibling lottieToSvg()
// which already turns Bodymovin JSON into an editable SVG.  We do not
// reimplement any Lottie parsing here.  If the inflated bytes are not Lottie,
// lottieToSvg() returns an empty QString and we propagate it (fall through).
QString lottieToSvg(const u8* p, i64 n);   // sibling decoder, same TU

QString tgsToSvg(const u8* p, i64 n) {
    // Minimum gzip file = 10-byte header + 8-byte trailer.
    if (!p || n < 18) return QString();
    // RFC 1952 2.3.1: ID1=0x1f, ID2=0x8b, CM=8 (DEFLATE).  This is the sole
    // TGS signature — reject anything else so the caller can try other formats.
    if (p[0] != 0x1f || p[1] != 0x8b || p[2] != 0x08) return QString();

    // Decompression-bomb guard: TGS payloads are tiny JSON; cap at ~64 MB.
    const size_t kTgsMaxOut = 64u * 1024u * 1024u;

    z_stream strm;
    std::memset(&strm, 0, sizeof(strm));
    strm.zalloc = Z_NULL;
    strm.zfree  = Z_NULL;
    strm.opaque = Z_NULL;
    strm.next_in  = Z_NULL;
    strm.avail_in = 0;

    // 15 = max window bits; +16 selects gzip-only decoding (zlib manual).
    if (inflateInit2(&strm, 15 + 16) != Z_OK) return QString();

    QByteArray tgsOut;
    const size_t kChunk = 1u << 16; // 64 KiB
    std::vector<char> tgsBuf(kChunk);

    i64 inPos = 0;
    int ret = Z_OK;
    bool ok = true;

    do {
        if (strm.avail_in == 0) {
            if (inPos >= n) { ok = false; break; }          // truncated
            i64 remain = n - inPos;
            uInt take = (remain > (i64)0x40000000) ? (uInt)0x40000000 : (uInt)remain;
            strm.next_in  = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(p + inPos));
            strm.avail_in = take;
            inPos += take;
        }
        do {
            strm.next_out  = reinterpret_cast<Bytef*>(tgsBuf.data());
            strm.avail_out = (uInt)kChunk;
            ret = inflate(&strm, Z_NO_FLUSH);
            // Break on any error; only Z_OK / Z_STREAM_END continue, so a
            // truncated or corrupt stream can never infinite-loop.
            if (ret != Z_OK && ret != Z_STREAM_END) { ok = false; break; }
            size_t produced = kChunk - strm.avail_out;
            if (produced) {
                if ((size_t)tgsOut.size() + produced > kTgsMaxOut) { ok = false; break; }
                tgsOut.append(tgsBuf.data(), (int)produced);
            }
        } while (strm.avail_out == 0 && ret != Z_STREAM_END && ok);
        if (!ok) break;
    } while (ret != Z_STREAM_END);

    inflateEnd(&strm);

    if (!ok || ret != Z_STREAM_END || tgsOut.isEmpty()) return QString();

    // Hand the inflated Bodymovin JSON to the shared Lottie decoder.  It emits
    // the SVG (with a data-bounds viewBox) or an empty QString if not Lottie.
    return lottieToSvg(reinterpret_cast<const u8*>(tgsOut.constData()),
                       (i64)tgsOut.size());
}


// ===================== TopoJSON (.topojson) -> SVG =====================
// Spec: TopoJSON Format Specification v1.0.0
//   github.com/topojson/topojson-specification/blob/master/README.md
// A Topology object (§2) has: optional "transform"
//   {"scale":[sx,sy],"translate":[tx,ty]} (§2.1.2); an "arcs" array (§2.1.3)
//   where each arc is a list of [x,y] positions -- when a transform is present
//   the positions are QUANTIZED + DELTA-encoded integers (first position is
//   absolute in quantized space, the rest are deltas; accumulate, then
//   dequantize realX = x*sx+tx, realY = y*sy+ty); "objects" (§2.2), a map of
//   named geometry objects. Geometry (§2.2.x): Point/MultiPoint carry
//   "coordinates" (transformed like an arc's first position); LineString/
//   MultiLineString/Polygon/MultiPolygon carry "arcs" indices. An arc index
//   i>=0 = arc i forward; a NEGATIVE index means arc ~i (= -i-1) REVERSED
//   (§2.1.4). A ring/line is the concatenation of its arcs, dropping the shared
//   duplicated endpoint. Coordinates are geographic -> Y-flip (screen_y = maxY-y).
struct topojsonPt { double x, y; };

struct topojsonCtx {
    std::vector<std::vector<topojsonPt>> arcs;                    // dequantized real coords
    std::vector<int> kinds;                                       // 0 polyline,1 polygon,2 point
    std::vector<std::vector<std::vector<topojsonPt>>> prims;      // rings per primitive
    double minX, minY, maxX, maxY; bool any;
    qint64 ptBudget; int primBudget;
    void see(double x, double y){
        if(x<minX)minX=x; if(x>maxX)maxX=x;
        if(y<minY)minY=y; if(y>maxY)maxY=y; any=true;
    }
};

static inline bool topojsonGood(double v){ return std::isfinite(v) && v>-1e12 && v<1e12; }

static QString topojsonFmt(double v){
    if(!std::isfinite(v)) v=0;
    QString s=QString::number(v,'f',4);
    if(s.contains(QLatin1Char('.'))){
        int i=s.size();
        while(i>0 && s.at(i-1)==QLatin1Char('0')) --i;
        if(i>0 && s.at(i-1)==QLatin1Char('.')) --i;
        s.truncate(i);
    }
    if(s==QLatin1String("-0")) s=QStringLiteral("0");
    return s;
}

// Resolve a list of arc indices into one concatenated point list.
static std::vector<topojsonPt> topojsonResolve(const QJsonArray& idxs, topojsonCtx& cx){
    std::vector<topojsonPt> out;
    for(const QJsonValue& iv : idxs){
        if(!iv.isDouble()) continue;
        // Validate BEFORE the double→long long cast: casting a non-finite or out-of-range double to an
        // integer is UB (traps under UBSan). Any real arc index is tiny; ±9e18 is well inside LLONG range.
        double dv=iv.toDouble();
        if(!std::isfinite(dv) || dv < -9.0e18 || dv > 9.0e18) continue;
        long long idx=(long long)dv;
        bool rev = idx<0;
        long long a = rev ? (-idx-1) : idx;                       // ~i == -i-1
        if(a<0 || a>=(long long)cx.arcs.size()) continue;
        const std::vector<topojsonPt>& arc=cx.arcs[(size_t)a];
        size_t na=arc.size();
        if(na==0) continue;
        for(size_t k=0;k<na;k++){
            if(!out.empty() && k==0) continue;                    // drop shared endpoint
            if(cx.ptBudget<=0) return out;
            out.push_back(rev ? arc[na-1-k] : arc[k]);
            cx.ptBudget--;
        }
    }
    return out;
}

static void topojsonWalk(const QJsonObject& g, double sx,double sy,double tx,double ty,
                         bool hasT, topojsonCtx& cx){
    if(cx.primBudget<=0) return;
    QString type=g.value(QStringLiteral("type")).toString();

    if(type==QLatin1String("GeometryCollection")){
        const QJsonArray gs=g.value(QStringLiteral("geometries")).toArray();
        for(const QJsonValue& gv: gs){
            if(cx.primBudget<=0) return;
            if(gv.isObject()) topojsonWalk(gv.toObject(),sx,sy,tx,ty,hasT,cx);
        }
        return;
    }
    if(type==QLatin1String("Point")){
        const QJsonArray c=g.value(QStringLiteral("coordinates")).toArray();
        if(c.size()>=2 && c.at(0).isDouble() && c.at(1).isDouble()){
            double x=c.at(0).toDouble(), y=c.at(1).toDouble();
            if(hasT){ x=x*sx+tx; y=y*sy+ty; }
            if(topojsonGood(x)&&topojsonGood(y)){
                cx.see(x,y);
                std::vector<topojsonPt> one; one.push_back({x,y});
                std::vector<std::vector<topojsonPt>> pr; pr.push_back(std::move(one));
                cx.kinds.push_back(2); cx.prims.push_back(std::move(pr)); cx.primBudget--;
            }
        }
        return;
    }
    if(type==QLatin1String("MultiPoint")){
        const QJsonArray cs=g.value(QStringLiteral("coordinates")).toArray();
        double ax=0, ay=0;                                        // MultiPoint coords are delta-encoded
        for(const QJsonValue& cv: cs){
            if(cx.primBudget<=0) return;
            const QJsonArray c=cv.toArray();
            if(c.size()>=2 && c.at(0).isDouble() && c.at(1).isDouble()){
                double x=c.at(0).toDouble(), y=c.at(1).toDouble(), rx, ry;
                if(hasT){ ax+=x; ay+=y; rx=ax*sx+tx; ry=ay*sy+ty; } else { rx=x; ry=y; }
                if(topojsonGood(rx)&&topojsonGood(ry)){
                    cx.see(rx,ry);
                    std::vector<topojsonPt> one; one.push_back({rx,ry});
                    std::vector<std::vector<topojsonPt>> pr; pr.push_back(std::move(one));
                    cx.kinds.push_back(2); cx.prims.push_back(std::move(pr)); cx.primBudget--;
                }
            }
        }
        return;
    }
    if(type==QLatin1String("LineString")){
        std::vector<topojsonPt> pts=topojsonResolve(g.value(QStringLiteral("arcs")).toArray(),cx);
        std::vector<topojsonPt> clean; clean.reserve(pts.size());
        for(const topojsonPt& pt: pts) if(topojsonGood(pt.x)&&topojsonGood(pt.y)){ cx.see(pt.x,pt.y); clean.push_back(pt); }
        if(clean.size()>=2){
            std::vector<std::vector<topojsonPt>> pr; pr.push_back(std::move(clean));
            cx.kinds.push_back(0); cx.prims.push_back(std::move(pr)); cx.primBudget--;
        }
        return;
    }
    if(type==QLatin1String("MultiLineString")){
        const QJsonArray outer=g.value(QStringLiteral("arcs")).toArray();
        for(const QJsonValue& lv: outer){
            if(cx.primBudget<=0) return;
            std::vector<topojsonPt> pts=topojsonResolve(lv.toArray(),cx);
            std::vector<topojsonPt> clean;
            for(const topojsonPt& pt: pts) if(topojsonGood(pt.x)&&topojsonGood(pt.y)){ cx.see(pt.x,pt.y); clean.push_back(pt); }
            if(clean.size()>=2){
                std::vector<std::vector<topojsonPt>> pr; pr.push_back(std::move(clean));
                cx.kinds.push_back(0); cx.prims.push_back(std::move(pr)); cx.primBudget--;
            }
        }
        return;
    }
    if(type==QLatin1String("Polygon")){
        const QJsonArray rings=g.value(QStringLiteral("arcs")).toArray();
        std::vector<std::vector<topojsonPt>> ringPts;
        for(const QJsonValue& rv: rings){
            std::vector<topojsonPt> pts=topojsonResolve(rv.toArray(),cx);
            std::vector<topojsonPt> clean;
            for(const topojsonPt& pt: pts) if(topojsonGood(pt.x)&&topojsonGood(pt.y)){ cx.see(pt.x,pt.y); clean.push_back(pt); }
            if(clean.size()>=3) ringPts.push_back(std::move(clean));
        }
        if(!ringPts.empty()){ cx.kinds.push_back(1); cx.prims.push_back(std::move(ringPts)); cx.primBudget--; }
        return;
    }
    if(type==QLatin1String("MultiPolygon")){
        const QJsonArray polys=g.value(QStringLiteral("arcs")).toArray();
        for(const QJsonValue& pv: polys){
            if(cx.primBudget<=0) return;
            const QJsonArray rings=pv.toArray();
            std::vector<std::vector<topojsonPt>> ringPts;
            for(const QJsonValue& rv: rings){
                std::vector<topojsonPt> pts=topojsonResolve(rv.toArray(),cx);
                std::vector<topojsonPt> clean;
                for(const topojsonPt& pt: pts) if(topojsonGood(pt.x)&&topojsonGood(pt.y)){ cx.see(pt.x,pt.y); clean.push_back(pt); }
                if(clean.size()>=3) ringPts.push_back(std::move(clean));
            }
            if(!ringPts.empty()){ cx.kinds.push_back(1); cx.prims.push_back(std::move(ringPts)); cx.primBudget--; }
        }
        return;
    }
    // unknown geometry type -> ignore
}

static void topojsonBuildArcs(const QJsonArray& arcsArr, double sx,double sy,double tx,double ty,
                              bool hasT, topojsonCtx& cx){
    int na=arcsArr.size();
    for(int i=0;i<na;i++){
        std::vector<topojsonPt> arc;
        const QJsonArray positions=arcsArr.at(i).toArray();
        double ax=0, ay=0;
        for(const QJsonValue& pv: positions){
            if(cx.ptBudget<=0) break;
            const QJsonArray pos=pv.toArray();
            if(pos.size()<2 || !pos.at(0).isDouble() || !pos.at(1).isDouble()) continue;
            double x=pos.at(0).toDouble(), y=pos.at(1).toDouble(), rx, ry;
            if(hasT){ ax+=x; ay+=y; rx=ax*sx+tx; ry=ay*sy+ty; } else { rx=x; ry=y; }
            arc.push_back({rx,ry}); cx.ptBudget--;
        }
        cx.arcs.push_back(std::move(arc));
    }
}

QString topojsonToSvg(const u8* p, i64 n){
    if(!p || n<12 || n>(i64)300*1024*1024) return QString();
    // Cheap sniff: the mandatory type token must appear, else this isn't ours.
    bool sawTop=false;
    for(i64 i=0;i+8<=n;i++){
        if(p[i]=='T'&&p[i+1]=='o'&&p[i+2]=='p'&&p[i+3]=='o'&&
           p[i+4]=='l'&&p[i+5]=='o'&&p[i+6]=='g'&&p[i+7]=='y'){ sawTop=true; break; }
    }
    if(!sawTop) return QString();

    QByteArray jb(reinterpret_cast<const char*>(p), int(n<(i64)0x7fffffff?n:(i64)0x7fffffff));
    QJsonParseError perr; QJsonDocument doc=QJsonDocument::fromJson(jb,&perr);
    if(perr.error!=QJsonParseError::NoError || !doc.isObject()) return QString();
    QJsonObject root=doc.object();
    if(root.value(QStringLiteral("type")).toString()!=QLatin1String("Topology")) return QString();
    QJsonValue objsV=root.value(QStringLiteral("objects"));
    if(!objsV.isObject()) return QString();

    bool hasT=false; double sx=1,sy=1,tx=0,ty=0;
    if(root.value(QStringLiteral("transform")).isObject()){
        QJsonObject tr=root.value(QStringLiteral("transform")).toObject();
        QJsonArray sc=tr.value(QStringLiteral("scale")).toArray();
        QJsonArray tl=tr.value(QStringLiteral("translate")).toArray();
        if(sc.size()>=2){ sx=sc.at(0).toDouble(1); sy=sc.at(1).toDouble(1); }
        if(tl.size()>=2){ tx=tl.at(0).toDouble(0); ty=tl.at(1).toDouble(0); }
        if(!(std::isfinite(sx)&&std::isfinite(sy)&&std::isfinite(tx)&&std::isfinite(ty))) return QString();
        hasT=true;
    }

    topojsonCtx cx;
    cx.minX=cx.minY=1e300; cx.maxX=cx.maxY=-1e300; cx.any=false;
    cx.ptBudget=4000000; cx.primBudget=200000;

    QJsonArray arcsArr=root.value(QStringLiteral("arcs")).toArray();
    if(arcsArr.size()>600000) return QString();
    topojsonBuildArcs(arcsArr,sx,sy,tx,ty,hasT,cx);

    QJsonObject objs=objsV.toObject();
    for(auto it=objs.begin(); it!=objs.end(); ++it){
        if(cx.primBudget<=0) break;
        if(it.value().isObject()) topojsonWalk(it.value().toObject(),sx,sy,tx,ty,hasT,cx);
    }

    if(!cx.any || cx.kinds.empty()) return QString();
    double W=cx.maxX-cx.minX, H=cx.maxY-cx.minY;
    if(!(W>0)) W=1; if(!(H>0)) H=1;
    if(!std::isfinite(W)||!std::isfinite(H)) return QString();
    double r = 0.005*(W>H?W:H); if(r<0.25) r=0.25;

    QString body; body.reserve(int(cx.kinds.size())*48+256);
    for(size_t i=0;i<cx.kinds.size();i++){
        int kind=cx.kinds[i];
        const std::vector<std::vector<topojsonPt>>& rings=cx.prims[i];
        if(kind==2){
            if(rings.empty()||rings[0].empty()) continue;
            const topojsonPt& c=rings[0][0];
            body += QStringLiteral("<circle cx='")+topojsonFmt(c.x)
                  + QStringLiteral("' cy='")+topojsonFmt(cx.maxY-c.y)
                  + QStringLiteral("' r='")+topojsonFmt(r)
                  + QStringLiteral("' fill='#e34a33'/>\n");
        } else if(kind==0){
            if(rings.empty()) continue;
            QString pts;
            for(const topojsonPt& pt: rings[0]){
                pts += topojsonFmt(pt.x); pts += QLatin1Char(',');
                pts += topojsonFmt(cx.maxY-pt.y); pts += QLatin1Char(' ');
            }
            body += QStringLiteral("<polyline fill='none' stroke='#333333' stroke-width='0.5' points='")
                  + pts.trimmed() + QStringLiteral("'/>\n");
        } else {
            QString d;
            for(const std::vector<topojsonPt>& ring: rings){
                if(ring.empty()) continue;
                bool first=true;
                for(const topojsonPt& pt: ring){
                    d += first ? QStringLiteral("M") : QStringLiteral("L");
                    d += topojsonFmt(pt.x); d += QLatin1Char(',');
                    d += topojsonFmt(cx.maxY-pt.y); d += QLatin1Char(' ');
                    first=false;
                }
                d += QStringLiteral("Z ");
            }
            body += QStringLiteral("<path fill-rule='evenodd' fill='#9ecae1' stroke='#3182bd' stroke-width='0.5' d='")
                  + d.trimmed() + QStringLiteral("'/>\n");
        }
    }
    if(body.isEmpty()) return QString();

    QString svg=QStringLiteral("<svg xmlns='http://www.w3.org/2000/svg' viewBox='")
        + topojsonFmt(cx.minX) + QStringLiteral(" 0 ")
        + topojsonFmt(W) + QStringLiteral(" ") + topojsonFmt(H)
        + QStringLiteral("'>\n") + body + QStringLiteral("</svg>\n");
    return svg;
}


// ---- VML (Vector Markup Language) -> SVG ---------------------------------
// Spec: W3C NOTE-VML-19980513 "Vector Markup Language (VML)" + the Office VML
// namespace 'urn:schemas-microsoft-com:vml'. We parse with QXmlStreamReader and
// key off element LOCAL names (prefix 'v:' ignored via name()):
//   <rect>/<oval>  : box from style "left/top/width/height" or same-named attrs
//   <line from="x1,y1" to="x2,y2">
//   <polyline points="x1,y1 x2,y2 ...">
//   <shape path="m X Y l X Y c ... x e" ...> : VML path verbs
//        m=moveto(abs) l=lineto(abs) c=cubic(abs, 3 pts) x=close e=end
//        t=rmoveto r=rlineto v=rcurveto (relative). Unknown verbs are skipped.
// Paint attrs: fillcolor/strokecolor, filled/stroked ("t"/"f"); VML defaults are
// filled=true(white) stroked=true(black) (NOTE-VML "Fill"/"Stroke" defaults).
// We map to SVG rect/ellipse/line/polyline/path and derive the viewBox from all
// emitted coordinates. Colours are validated; the SVG parser does the rest.
static double vmlNum(const QString& s){
    QString t=s.trimmed(); if(t.isEmpty()) return std::nan("");
    int i=0,n=t.size(); QString num; bool any=false;
    if(i<n && (t[i]==QLatin1Char('+')||t[i]==QLatin1Char('-'))){ num+=t[i]; ++i; }
    while(i<n && t[i].isDigit()){ num+=t[i]; ++i; any=true; }
    if(i<n && t[i]==QLatin1Char('.')){ num+=t[i]; ++i; while(i<n&&t[i].isDigit()){num+=t[i];++i;any=true;} }
    if(!any) return std::nan("");
    bool ok=false; double v=num.toDouble(&ok); return ok? v : std::nan("");
}
static double vmlStyleVal(const QString& style,const char* key){
    const QStringList parts=style.split(QLatin1Char(';'),Qt::SkipEmptyParts);
    for(const QString& pr:parts){ int c=pr.indexOf(QLatin1Char(':')); if(c<0)continue;
        if(pr.left(c).trimmed().compare(QLatin1String(key),Qt::CaseInsensitive)==0) return vmlNum(pr.mid(c+1)); }
    return std::nan("");
}
static QString vmlValidColor(QString c){
    c=c.trimmed().toLower(); if(c.isEmpty()) return QString();
    if(c.startsWith(QLatin1Char('#'))){
        QString h=c.mid(1); if(h.size()!=3 && h.size()!=6) return QString();
        for(QChar ch:h){ if(!((ch>=QLatin1Char('0')&&ch<=QLatin1Char('9'))||(ch>=QLatin1Char('a')&&ch<=QLatin1Char('f')))) return QString(); }
        return QLatin1Char('#')+h;
    }
    static const char* kw[]={"black","white","red","green","blue","yellow","cyan","magenta",
        "gray","grey","silver","maroon","olive","lime","aqua","teal","navy","fuchsia","purple",
        "orange","gold","pink","brown","darkblue","darkred","darkgreen","lightgray","lightgrey",nullptr};
    for(int i=0;kw[i];++i) if(c==QLatin1String(kw[i])) return c;
    return QString();
}
template<class F>
static QString vmlPathToSvg(const QString& d, int& emitted, F upd){
    auto okc=[](double v){ return std::isfinite(v)&&std::fabs(v)<1e12; };
    auto ns=[](double v){ return QString::number(v,'f',2); };
    QString out; double curX=0,curY=0; int mode=0,arity=0,bc=0; double b[6]={0,0,0,0,0,0};
    int i=0,nn=d.size(); long guard=0;
    auto flush=[&](){
        if(mode=='M'||mode=='m'){
            double x=(mode=='m'?curX:0.0)+b[0], y=(mode=='m'?curY:0.0)+b[1];
            if(okc(x)&&okc(y)){ out+=QLatin1Char('M')+ns(x)+QLatin1Char(' ')+ns(y)+QLatin1Char(' ');
                curX=x;curY=y;upd(x,y);emitted++; }
            mode=(mode=='m')?'l':'L';   // implicit line for extra coord pairs after a moveto
        } else if(mode=='L'||mode=='l'){
            double x=(mode=='l'?curX:0.0)+b[0], y=(mode=='l'?curY:0.0)+b[1];
            if(okc(x)&&okc(y)){ out+=QLatin1Char('L')+ns(x)+QLatin1Char(' ')+ns(y)+QLatin1Char(' ');
                curX=x;curY=y;upd(x,y);emitted++; }
        } else if(mode=='C'||mode=='c'){
            bool rel=(mode=='c'); double bx=rel?curX:0.0, by=rel?curY:0.0;
            double x1=bx+b[0],y1=by+b[1],x2=bx+b[2],y2=by+b[3],ex=bx+b[4],ey=by+b[5];
            if(okc(x1)&&okc(y1)&&okc(x2)&&okc(y2)&&okc(ex)&&okc(ey)){
                out+=QLatin1Char('C')+ns(x1)+QLatin1Char(' ')+ns(y1)+QLatin1Char(' ')+ns(x2)+QLatin1Char(' ')+ns(y2)+QLatin1Char(' ')+ns(ex)+QLatin1Char(' ')+ns(ey)+QLatin1Char(' ');
                upd(x1,y1);upd(x2,y2);upd(ex,ey); curX=ex;curY=ey; emitted++; }
        }
    };
    while(i<nn){
        if(++guard>500000) break;
        if(emitted>100000) break;
        QChar ch=d[i];
        if(ch.isSpace()||ch==QLatin1Char(',')){ ++i; continue; }
        if(ch.isLetter()){
            QString cmd; while(i<nn && d[i].isLetter()){ cmd+=d[i].toLower(); ++i; }
            bc=0;
            if(cmd==QLatin1String("m")){mode='M';arity=2;}
            else if(cmd==QLatin1String("l")){mode='L';arity=2;}
            else if(cmd==QLatin1String("c")){mode='C';arity=6;}
            else if(cmd==QLatin1String("t")){mode='m';arity=2;}
            else if(cmd==QLatin1String("r")){mode='l';arity=2;}
            else if(cmd==QLatin1String("v")){mode='c';arity=6;}
            else if(cmd==QLatin1String("x")){ out+=QLatin1String("Z "); mode=0;arity=0; }
            else if(cmd==QLatin1String("e")){ break; }        // end of path
            else { mode=0; arity=0; }                          // ignore unknown verb + its args
            continue;
        }
        QString num; if(ch==QLatin1Char('+')||ch==QLatin1Char('-')){num+=ch;++i;}
        while(i<nn && (d[i].isDigit()||d[i]==QLatin1Char('.'))){ num+=d[i]; ++i; }
        if(num.isEmpty()||num==QLatin1String("+")||num==QLatin1String("-")||num==QLatin1String(".")){
            if(i<nn && !d[i].isLetter() && !d[i].isDigit()) ++i;  // strictly advance past a stray char
            continue; }
        bool ok=false; double v=num.toDouble(&ok);
        if(!ok||!std::isfinite(v)) continue;
        if(arity==0) continue;
        if(bc<6) b[bc++]=v;
        if(bc==arity){ flush(); bc=0; }
    }
    return out;
}

QString vmlToSvg(const u8* p, i64 n){
    if(!p || n<8) return QString();
    i64 cap = n < (i64)64*1024*1024 ? n : (i64)64*1024*1024;
    QByteArray raw=QByteArray::fromRawData(reinterpret_cast<const char*>(p),(int)cap);
    QXmlStreamReader xr(raw);
    QString body; int emitted=0; long elems=0;
    bool hasBox=false; double minX=0,minY=0,maxX=0,maxY=0;
    auto okc=[](double v){ return std::isfinite(v)&&std::fabs(v)<1e12; };
    auto upd=[&](double x,double y){ if(!okc(x)||!okc(y))return;
        if(!hasBox){minX=maxX=x;minY=maxY=y;hasBox=true;}
        else{ if(x<minX)minX=x; if(x>maxX)maxX=x; if(y<minY)minY=y; if(y>maxY)maxY=y; } };
    auto ns=[](double v){ return QString::number(v,'f',2); };
    bool firstEl=true;
    while(!xr.atEnd()){
        QXmlStreamReader::TokenType t=xr.readNext();
        if(xr.hasError()) break;
        if(t!=QXmlStreamReader::StartElement) continue;
        QString name=xr.name().toString().toLower();
        if(firstEl){ firstEl=false; if(name==QLatin1String("svg")) return QString(); } // that's SVG, not VML
        if(++elems>200000) break;
        if(emitted>100000) break;
        const QXmlStreamAttributes ats=xr.attributes();
        auto attr=[&](const char* key)->QString{
            for(const QXmlStreamAttribute& at:ats)
                if(at.name().toString().compare(QLatin1String(key),Qt::CaseInsensitive)==0) return at.value().toString();
            return QString(); };
        QString style=attr("style");
        auto boxVal=[&](const char* an,const char* sk)->double{
            QString av=attr(an); if(!av.isEmpty()){ double v=vmlNum(av); if(std::isfinite(v))return v; }
            return vmlStyleVal(style,sk); };
        auto paint=[&](bool defaultFilled)->QString{
            QString f=vmlValidColor(attr("fillcolor"));
            QString s=vmlValidColor(attr("strokecolor"));
            QString filled=attr("filled").trimmed().toLower();
            QString stroked=attr("stroked").trimmed().toLower();
            bool isFilled=defaultFilled;
            if(filled==QLatin1String("f")||filled==QLatin1String("false")||filled==QLatin1String("0")) isFilled=false;
            else if(filled==QLatin1String("t")||filled==QLatin1String("true")||filled==QLatin1String("1")) isFilled=true;
            bool isStroked=true;
            if(stroked==QLatin1String("f")||stroked==QLatin1String("false")||stroked==QLatin1String("0")) isStroked=false;
            QString r;
            r+=QLatin1String(" fill='")+(isFilled? (f.isEmpty()? QStringLiteral("#ffffff"):f) : QStringLiteral("none"))+QLatin1Char('\'');
            r+=QLatin1String(" stroke='")+(isStroked? (s.isEmpty()? QStringLiteral("#000000"):s) : QStringLiteral("none"))+QLatin1Char('\'');
            double sw=vmlNum(attr("strokeweight"));
            if(std::isfinite(sw)&&sw>0&&sw<1e6) r+=QLatin1String(" stroke-width='")+ns(sw)+QLatin1Char('\'');
            return r; };

        if(name==QLatin1String("rect")){
            double x=boxVal("left","left"),y=boxVal("top","top"),w=boxVal("width","width"),h=boxVal("height","height");
            if(okc(x)&&okc(y)&&okc(w)&&okc(h)&&w>=0&&h>=0){
                body+=QLatin1String("<rect x='")+ns(x)+QLatin1String("' y='")+ns(y)+QLatin1String("' width='")+ns(w)+QLatin1String("' height='")+ns(h)+QLatin1Char('\'')+paint(true)+QLatin1String("/>\n");
                upd(x,y);upd(x+w,y+h);emitted++;
            }
        } else if(name==QLatin1String("oval")){
            double x=boxVal("left","left"),y=boxVal("top","top"),w=boxVal("width","width"),h=boxVal("height","height");
            if(okc(x)&&okc(y)&&okc(w)&&okc(h)&&w>=0&&h>=0){
                double cx=x+w/2,cy=y+h/2,rx=w/2,ry=h/2;
                body+=QLatin1String("<ellipse cx='")+ns(cx)+QLatin1String("' cy='")+ns(cy)+QLatin1String("' rx='")+ns(rx)+QLatin1String("' ry='")+ns(ry)+QLatin1Char('\'')+paint(true)+QLatin1String("/>\n");
                upd(x,y);upd(x+w,y+h);emitted++;
            }
        } else if(name==QLatin1String("line")){
            auto pr=[&](const QString& v,double& px,double& py)->bool{
                QStringList c=v.split(QLatin1Char(','),Qt::SkipEmptyParts); if(c.size()<2)return false;
                px=vmlNum(c[0]); py=vmlNum(c[1]); return okc(px)&&okc(py); };
            double x1,y1,x2,y2;
            if(pr(attr("from"),x1,y1)&&pr(attr("to"),x2,y2)){
                body+=QLatin1String("<line x1='")+ns(x1)+QLatin1String("' y1='")+ns(y1)+QLatin1String("' x2='")+ns(x2)+QLatin1String("' y2='")+ns(y2)+QLatin1Char('\'')+paint(false)+QLatin1String("/>\n");
                upd(x1,y1);upd(x2,y2);emitted++;
            }
        } else if(name==QLatin1String("polyline")){
            QString norm=attr("points"); norm.replace(QLatin1Char(','),QLatin1Char(' '));
            const QStringList toks=norm.simplified().split(QLatin1Char(' '),Qt::SkipEmptyParts);
            QString pts; int npair=0; double px=0; bool have=false;
            for(const QString& tk:toks){ double v=vmlNum(tk); if(!okc(v)) continue;
                if(!have){ px=v; have=true; }
                else { if(!pts.isEmpty())pts+=QLatin1Char(' '); pts+=ns(px)+QLatin1Char(',')+ns(v); upd(px,v); have=false; if(++npair>50000)break; } }
            if(npair>=2){ body+=QLatin1String("<polyline points='")+pts+QLatin1Char('\'')+paint(false)+QLatin1String("/>\n"); emitted++; }
        } else if(name==QLatin1String("shape")){
            QString path=attr("path");
            if(!path.isEmpty() && path.size()<2000000){
                int before=emitted;
                QString dd=vmlPathToSvg(path,emitted,upd);
                QString dt=dd.trimmed();
                if(!dt.isEmpty() && emitted>before)
                    body+=QLatin1String("<path d='")+dt+QLatin1Char('\'')+paint(true)+QLatin1String("/>\n");
            }
        }
    }
    if(!hasBox || emitted==0 || body.isEmpty()) return QString();
    double w=maxX-minX,h=maxY-minY;
    if(!std::isfinite(w)||!std::isfinite(h)) return QString();
    if(w<=0)w=1; if(h<=0)h=1;
    double pad=0.02*std::sqrt(w*h); if(!std::isfinite(pad)||pad<1)pad=1;
    double vx=minX-pad,vy=minY-pad,vw=w+2*pad,vh=h+2*pad;
    if(!okc(vx)||!okc(vy)||!std::isfinite(vw)||!std::isfinite(vh)) return QString();
    return QLatin1String("<svg xmlns='http://www.w3.org/2000/svg' viewBox='")+ns(vx)+QLatin1Char(' ')+ns(vy)+QLatin1Char(' ')+ns(vw)+QLatin1Char(' ')+ns(vh)+QLatin1String("'>\n")+body+QLatin1String("</svg>\n");
}


// ---- XPS / OpenXPS (ECMA-388, Microsoft XML Paper Specification) -> SVG ----
// An XPS file is an OPC (Open Packaging Conventions) ZIP package. The drawable
// content lives in FixedPage parts "*.fpage" (typically Documents/1/Pages/1.fpage),
// which are XAML-like XML in namespace http://schemas.microsoft.com/xps/2005/06.
// We locate the first *.fpage, inflate it, and stream it with QXmlStreamReader:
//   <FixedPage Width Height>            -> viewBox 0 0 W H (device units = 1/96")
//   <Path Data Fill Stroke StrokeThickness RenderTransform>  (Data = XAML path
//        mini-language, near-identical to the SVG 'd' grammar; strip a leading
//        F0/F1 fill-rule token, sanitise, pass through)
//   <Glyphs OriginX OriginY FontRenderingEmSize UnicodeString Fill> -> <text>
//   <Canvas RenderTransform> may nest (best-effort matrix as an SVG <g>)
// Refs: ECMA-388 "Open XML Paper Specification"; MS XPS Specification 1.0,
//        secs. on FixedPage / Path.Data / Glyphs; XAML path grammar.

// --- small formatters / validators (uniquely xps*-prefixed) ---
static QString xpsFmt(double v){
    if(!std::isfinite(v)) return QStringLiteral("0");
    return QString::number(v, 'g', 8);
}
static QString xpsEsc(const QString& s){
    QString o; o.reserve(s.size()+8);
    for(QChar c : s){ ushort u=c.unicode();
        if(u=='&') o+=QLatin1String("&amp;");
        else if(u=='<') o+=QLatin1String("&lt;");
        else if(u=='>') o+=QLatin1String("&gt;");
        else if(u=='"') o+=QLatin1String("&quot;");
        else if(u=='\'') o+=QLatin1String("&apos;");
        else o+=c;
    }
    return o;
}
// Accept only a safe colour token: '#rrggbb', '#rgb', '#aarrggbb', '#argb', or a
// purely-alphabetic CSS keyword. Anything else (scRGB, ContextColor, hostile
// strings) -> empty. alpha255 set from the alpha nibble/byte when present.
static QString xpsColor(const QString& raw, int& alpha255){
    alpha255 = 255;
    QString s = raw.trimmed();
    if(s.isEmpty()) return QString();
    if(s.startsWith(QLatin1Char('#'))){
        QString hex = s.mid(1);
        if(hex.isEmpty() || hex.size()>8) return QString();
        for(QChar c : hex){ ushort u=c.unicode();
            bool ok=(u>='0'&&u<='9')||(u>='a'&&u<='f')||(u>='A'&&u<='F');
            if(!ok) return QString(); }
        hex = hex.toLower();
        int L = hex.size();
        if(L==6) return QLatin1Char('#')+hex;
        if(L==3) return QLatin1Char('#')+hex;
        if(L==8){ bool ok; int a=hex.left(2).toInt(&ok,16); if(ok) alpha255=a;
                  return QLatin1Char('#')+hex.mid(2); }
        if(L==4){ bool ok; int a=hex.left(1).toInt(&ok,16); if(ok) alpha255=a*17;
                  return QLatin1Char('#')+hex.mid(1); }
        return QString();
    }
    if(s.startsWith(QLatin1String("sc#"), Qt::CaseInsensitive)) return QString();
    if(s.size()>32) return QString();
    for(QChar c : s){ if(!c.isLetter()) return QString(); }
    return s.toLower();
}
// XAML "m11,m12,m21,m22,dx,dy" -> validated SVG matrix() or empty.
static QString xpsMatrix(const QString& raw){
    QString s = raw.trimmed();
    if(s.isEmpty()) return QString();
    s.replace(QLatin1Char(','), QLatin1Char(' '));
    const QStringList parts = s.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if(parts.size()!=6) return QString();
    double m[6];
    for(int k=0;k<6;k++){ bool ok; m[k]=parts[k].toDouble(&ok);
        if(!ok || !std::isfinite(m[k]) || std::fabs(m[k])>=1e12) return QString(); }
    return QStringLiteral("matrix(%1,%2,%3,%4,%5,%6)")
        .arg(xpsFmt(m[0]),xpsFmt(m[1]),xpsFmt(m[2]),xpsFmt(m[3]),xpsFmt(m[4]),xpsFmt(m[5]));
}
// Translate a XAML Path "Data" string to an SVG 'd'. Strips a leading F0/F1
// fill-rule token, keeps ONLY path-grammar characters (so no quote can escape
// the attribute), and requires a leading move command.
static QString xpsPathData(const QString& in, bool* nonZeroOut = nullptr){
    if(nonZeroOut) *nonZeroOut = false;            // XPS/XAML's default fill rule is EvenOdd, not nonzero
    if(in.isEmpty() || in.size() > 2000000) return QString();
    int N = in.size(), i = 0;
    while(i<N && in[i].isSpace()) i++;
    if(i<N && (in[i]==QLatin1Char('F') || in[i]==QLatin1Char('f'))){
        i++;
        while(i<N && in[i].isSpace()) i++;
        // F0 = EvenOdd, F1 = Nonzero. The digit was consumed and discarded, which was harmless only
        // while the SVG importer happened to default to even-odd as well.
        if(i<N && in[i]==QLatin1Char('1') && nonZeroOut) *nonZeroOut = true;
        while(i<N && in[i].isDigit()) i++;
    }
    static const QString kCmds = QStringLiteral("MLCQHVASZTmlcqhvaszt");
    QString out; out.reserve(N);
    for(; i<N; i++){
        QChar c = in[i]; ushort u = c.unicode();
        if((u>='0'&&u<='9') || u=='.' || u=='-' || u=='+' || u=='e' || u=='E'
           || u==',' || u==' ') out += c;
        else if(u=='\t' || u=='\n' || u=='\r') out += QLatin1Char(' ');
        else if(kCmds.contains(c)) out += c;
        // else: silently drop
        if(out.size() > 4000000) break;
    }
    QString t = out.trimmed();
    if(t.isEmpty()) return QString();
    QChar f = t[0];
    if(f!=QLatin1Char('M') && f!=QLatin1Char('m')) return QString();
    return t;
}
// Crude fallback bbox: walk the numbers of an SVG 'd', alternating x/y.
static void xpsBboxD(const QString& d, double& mnx,double& mny,double& mxx,double& mxy,bool& any){
    int N=d.size(), i=0, axis=0;
    while(i<N){
        QChar c=d[i]; ushort u=c.unicode();
        if((u>='0'&&u<='9')||u=='.'||u=='-'||u=='+'){
            int j=i; QString num;
            while(j<N){ QChar q=d[j]; ushort v=q.unicode();
                if((v>='0'&&v<='9')||v=='.'||v=='-'||v=='+'||v=='e'||v=='E'){ num+=q; j++; }
                else break; }
            if(j<=i) { i++; continue; }
            bool ok; double val=num.toDouble(&ok);
            if(ok && std::isfinite(val) && std::fabs(val)<1e12){
                if(axis==0){ if(val<mnx)mnx=val; if(val>mxx)mxx=val; }
                else       { if(val<mny)mny=val; if(val>mxy)mxy=val; }
                any=true; axis^=1;
            }
            i=j;
        } else i++;
    }
}

QString xpsToSvg(const u8* p, i64 n){
    if(!p || n < 22) return QString();
    if(p[0]!='P' || p[1]!='K') return QString();          // OPC package is a ZIP

    // Find the first FixedPage part "*.fpage".
    QStringList names = xqUnzipList(p, n);
    if(names.isEmpty()) return QString();
    QString fpage;
    for(const QString& nm : names){
        if(nm.endsWith(QLatin1String(".fpage"), Qt::CaseInsensitive)){ fpage=nm; break; }
    }
    if(fpage.isEmpty()) return QString();

    QByteArray xml;
    if(!xqUnzip(p, n, fpage, xml) || xml.isEmpty()) return QString();
    if(xml.size() > 128*1024*1024) return QString();

    double pageW=0, pageH=0; bool fpageSeen=false;
    double mnx=1e300, mny=1e300, mxx=-1e300, mxy=-1e300; bool bboxAny=false;
    QString body;
    int elems = 0; const int kMaxElems = 200000;
    std::vector<bool> canvasGrp;                          // one entry per open <Canvas>

    QXmlStreamReader xr(xml);
    long guard = 0;
    while(!xr.atEnd()){
        if(++guard > 5000000) break;
        QXmlStreamReader::TokenType t = xr.readNext();
        if(t == QXmlStreamReader::StartElement){
            const QStringView nm = xr.name();
            auto at=[&](const char* a)->QString{
                return xr.attributes().value(QLatin1String(a)).toString(); };

            if(nm == QLatin1String("FixedPage")){
                fpageSeen = true;
                bool ok; double w=at("Width").toDouble(&ok);
                if(ok && std::isfinite(w) && w>0 && w<1e9) pageW=w;
                double h=at("Height").toDouble(&ok);
                if(ok && std::isfinite(h) && h>0 && h<1e9) pageH=h;
            }
            else if(nm == QLatin1String("Canvas")){
                QString rt = xpsMatrix(at("RenderTransform"));
                if(!rt.isEmpty() && elems < kMaxElems){
                    body += QLatin1String("<g transform='")+rt+QLatin1String("'>");
                    canvasGrp.push_back(true);
                } else canvasGrp.push_back(false);
            }
            else if(nm == QLatin1String("Path")){
                if(elems >= kMaxElems) continue;
                bool xpsNonZero = false;
                QString d = xpsPathData(at("Data"), &xpsNonZero);
                if(d.isEmpty()) continue;
                int fa=255; QString fill = xpsColor(at("Fill"), fa);
                int sa=255; QString stroke = xpsColor(at("Stroke"), sa);
                QString rt = xpsMatrix(at("RenderTransform"));
                QString el = QLatin1String("<path d='")+d+QLatin1Char('\'')
                           + QLatin1String(" fill-rule='") + QLatin1String(xpsNonZero?"nonzero":"evenodd") + QLatin1Char('\'');
                if(!fill.isEmpty()){
                    el += QLatin1String(" fill='")+fill+QLatin1Char('\'');
                    if(fa<255) el += QLatin1String(" fill-opacity='")+xpsFmt(fa/255.0)+QLatin1Char('\'');
                } else if(stroke.isEmpty()){
                    el += QLatin1String(" fill='#000000'");   // both absent -> visible default
                } else {
                    el += QLatin1String(" fill='none'");
                }
                if(!stroke.isEmpty()){
                    el += QLatin1String(" stroke='")+stroke+QLatin1Char('\'');
                    if(sa<255) el += QLatin1String(" stroke-opacity='")+xpsFmt(sa/255.0)+QLatin1Char('\'');
                    bool tok; double tw=at("StrokeThickness").toDouble(&tok);
                    if(!tok || !(tw>0) || !std::isfinite(tw)) tw=1;
                    el += QLatin1String(" stroke-width='")+xpsFmt(tw)+QLatin1Char('\'');
                }
                if(!rt.isEmpty()) el += QLatin1String(" transform='")+rt+QLatin1Char('\'');
                el += QLatin1String("/>");
                body += el; elems++;
                xpsBboxD(d, mnx,mny,mxx,mxy, bboxAny);
            }
            else if(nm == QLatin1String("Glyphs")){
                if(elems >= kMaxElems) continue;
                QString ss = at("UnicodeString");
                if(ss.isEmpty()) continue;
                bool okx, oky, oke;
                double x=at("OriginX").toDouble(&okx);
                double y=at("OriginY").toDouble(&oky);
                double sz=at("FontRenderingEmSize").toDouble(&oke);
                if(!okx || !oky || !std::isfinite(x) || !std::isfinite(y)) continue;
                if(std::fabs(x)>=1e12 || std::fabs(y)>=1e12) continue;
                if(!oke || !(sz>0) || !std::isfinite(sz)) sz=12;
                int ga=255; QString fill = xpsColor(at("Fill"), ga);
                if(fill.isEmpty()) fill = QStringLiteral("#000000");
                QString rt = xpsMatrix(at("RenderTransform"));
                QString el = QLatin1String("<text x='")+xpsFmt(x)+QLatin1String("' y='")+xpsFmt(y)
                    +QLatin1String("' font-size='")+xpsFmt(sz)+QLatin1String("' fill='")+fill+QLatin1Char('\'');
                if(ga<255) el += QLatin1String(" fill-opacity='")+xpsFmt(ga/255.0)+QLatin1Char('\'');
                if(!rt.isEmpty()) el += QLatin1String(" transform='")+rt+QLatin1Char('\'');
                el += QLatin1Char('>')+xpsEsc(ss)+QLatin1String("</text>");
                body += el; elems++;
                if(x<mnx)mnx=x; if(x>mxx)mxx=x; if(y<mny)mny=y; if(y>mxy)mxy=y;
                double xe=x+sz*ss.size(); if(std::isfinite(xe)&&std::fabs(xe)<1e12&&xe>mxx)mxx=xe;
                bboxAny=true;
            }
        }
        else if(t == QXmlStreamReader::EndElement){
            if(xr.name() == QLatin1String("Canvas") && !canvasGrp.empty()){
                bool opened = canvasGrp.back(); canvasGrp.pop_back();
                if(opened) body += QLatin1String("</g>");
            }
        }
    }
    // Balance any groups left open by a truncated / malformed stream.
    for(auto it=canvasGrp.rbegin(); it!=canvasGrp.rend(); ++it)
        if(*it) body += QLatin1String("</g>");

    if(!fpageSeen || body.isEmpty()) return QString();

    // viewBox: prefer FixedPage W/H (device units), else the data bbox.
    double vx=0, vy=0, vw=pageW, vh=pageH;
    if(!(vw>0) || !(vh>0)){
        if(!bboxAny) return QString();
        if(!std::isfinite(mnx)||!std::isfinite(mny)||!std::isfinite(mxx)||!std::isfinite(mxy))
            return QString();
        vx=mnx; vy=mny; vw=mxx-mnx; vh=mxy-mny;
        if(!(vw>0)) vw=1; if(!(vh>0)) vh=1;
    }
    if(!std::isfinite(vx)||!std::isfinite(vy)||!std::isfinite(vw)||!std::isfinite(vh)
       || vw<=0 || vh<=0 || vw>=1e12 || vh>=1e12) return QString();

    QString svg = QLatin1String("<svg xmlns='http://www.w3.org/2000/svg' viewBox='")
        + xpsFmt(vx)+QLatin1Char(' ')+xpsFmt(vy)+QLatin1Char(' ')
        + xpsFmt(vw)+QLatin1Char(' ')+xpsFmt(vh)+QLatin1String("'>")
        + body + QLatin1String("</svg>");
    return svg;
}


// ---- SCRIBUS .sla / .scd (open XML DTP; wiki.scribus.net "File Format Specification") ----
// Structure: <SCRIBUSUTF8NEW><DOCUMENT ...>
//   <COLOR NAME=".." RGB="#rrggbb"|"r g b" | CMYK="#ccmmyykk"/> ...
//   <PAGEOBJECT PTYPE="n" XPOS ".." YPOS ".." WIDTH ".." HEIGHT ".." ROT ".."
//               FCOLOR="name" PCOLOR="name" PWIDTH=".." path="M .. L .. Z"|POCOOR="..">
// We build a name->#rrggbb colour map, then for each PAGEOBJECT translate its local
// path (or POCOOR polyline, or bounding rect) by (XPOS,YPOS) and emit an <svg> whose
// viewBox spans all object rects. Colours are validated to #hex/none. QXmlStreamReader
// handles all XML decoding/entities/bounds; every parse loop strictly advances.

// Split on whitespace / commas without needing QRegExp (Qt6-safe).
static QStringList scribusTokenize(const QString& s){
    QStringList out; QString cur;
    for(QChar c: s){
        ushort u=c.unicode();
        if(u==' '||u=='\t'||u=='\n'||u=='\r'||u==','){
            if(!cur.isEmpty()){ out<<cur; cur.clear(); }
        } else cur+=c;
    }
    if(!cur.isEmpty()) out<<cur;
    return out;
}
static QString scribusFmt(double v){
    if(!std::isfinite(v)) return QStringLiteral("0");
    QString s=QString::number(v,'f',4);
    if(s.contains('.')){ while(s.endsWith('0')) s.chop(1); if(s.endsWith('.')) s.chop(1); }
    if(s==QLatin1String("-0")) s=QStringLiteral("0");
    return s;
}
// Build a validated, clamped "#rrggbb" (lowercase).
static QString scribusHex6(int r,int g,int b){
    auto cl=[](int v){ return v<0?0:(v>255?255:v); };
    return QString::asprintf("#%02x%02x%02x", cl(r), cl(g), cl(b));
}
// Parse a COLOR's RGB / CMYK attribute value into #rrggbb; empty QString on failure.
static QString scribusParseColorVal(const QString& rgb, const QString& cmyk){
    QString v=rgb.trimmed();
    if(!v.isEmpty()){
        if(v.startsWith('#')){
            QString h=v.mid(1);
            if(h.size()>=6){
                bool o1,o2,o3;
                int r=h.mid(0,2).toInt(&o1,16), g=h.mid(2,2).toInt(&o2,16), b=h.mid(4,2).toInt(&o3,16);
                if(o1&&o2&&o3) return scribusHex6(r,g,b);
            }
            return QString();
        }
        // "r g b" 0-255 triple
        QStringList t=scribusTokenize(v);
        if(t.size()>=3){
            bool o1,o2,o3;
            int r=t[0].toInt(&o1), g=t[1].toInt(&o2), b=t[2].toInt(&o3);
            if(o1&&o2&&o3) return scribusHex6(r,g,b);
        }
        return QString();
    }
    // CMYK: "#ccmmyykk" (4 bytes 0-255) or "c m y k" (0-1 floats or 0-255 ints)
    QString ck=cmyk.trimmed();
    if(ck.isEmpty()) return QString();
    int c=0,m=0,y=0,k=0; bool ok=false;
    if(ck.startsWith('#')){
        QString h=ck.mid(1);
        if(h.size()>=8){
            bool a,b2,d,e;
            c=h.mid(0,2).toInt(&a,16); m=h.mid(2,2).toInt(&b2,16);
            y=h.mid(4,2).toInt(&d,16); k=h.mid(6,2).toInt(&e,16);
            ok=a&&b2&&d&&e;
        }
    } else {
        QStringList t=scribusTokenize(ck);
        if(t.size()>=4){
            bool a,b2,d,e;
            double cf=t[0].toDouble(&a), mf=t[1].toDouble(&b2), yf=t[2].toDouble(&d), kf=t[3].toDouble(&e);
            if(a&&b2&&d&&e && std::isfinite(cf)&&std::isfinite(mf)&&std::isfinite(yf)&&std::isfinite(kf)){
                double sc=(cf<=1.0&&mf<=1.0&&yf<=1.0&&kf<=1.0)?255.0:1.0;
                // Clamp in the DOUBLE domain before narrowing: casting a huge finite double (e.g. 1e300)
                // to int is UB (UBSan-trap). Clamp to [0,255] first so the cast operand is always in range.
                auto q=[&](double v){ v*=sc; return (int)(v<0.0?0.0:(v>255.0?255.0:v)); };
                c=q(cf); m=q(mf); y=q(yf); k=q(kf); ok=true;
            }
        }
    }
    if(!ok) return QString();
    auto cl=[](int v){ return v<0?0:(v>255?255:v); };
    c=cl(c); m=cl(m); y=cl(y); k=cl(k);
    int r=255-((c*(255-k))/255 + k);
    int g=255-((m*(255-k))/255 + k);
    int b=255-((y*(255-k))/255 + k);
    return scribusHex6(r,g,b);
}
// Sanitise an SVG-path 'd' string: allow only path-grammar chars; empty if hostile.
static QString scribusSanitizePathD(const QString& in){
    if(in.isEmpty() || in.size()>200000) return QString();
    for(QChar c: in){
        ushort u=c.unicode();
        bool ok = (u>='0'&&u<='9') || (u>='a'&&u<='z') || (u>='A'&&u<='Z')
               || u=='.'||u=='-'||u=='+'||u==','||u==' '||u=='\t'||u=='\n'||u=='\r';
        if(!ok) return QString();
    }
    return in; // restricted to safe chars; contains no XML metacharacters
}

QString scribusToSvg(const u8* p, i64 n){
    if(!p || n < 24) return QString();
    // Cheap sniff: must mention the Scribus root somewhere near the start.
    {
        const char* magic="SCRIBUSUTF8NEW";
        const i64 mlen=14, sniff = n < 8192 ? n : 8192;
        bool saw=false;
        for(i64 i=0; i+mlen<=sniff; ++i){
            if(p[i]==(u8)'S' && std::memcmp(p+i,magic,(size_t)mlen)==0){ saw=true; break; }
        }
        if(!saw) return QString();
    }

    QByteArray xml(reinterpret_cast<const char*>(p), (qsizetype)n);
    QXmlStreamReader xr(xml);

    // name -> #rrggbb. Hash-indexed (exact + case-folded) so lookup is O(1): a file with N colours and N
    // objects referencing undefined names must not become O(N²) linear scans (CPU-exhaustion DoS).
    QHash<QString,QString> colorExact, colorFold;
    auto lookupColor=[&](const QString& name)->QString{
        QString nm=name.trimmed();
        if(nm.isEmpty()) return QString();
        if(nm.compare(QLatin1String("None"),Qt::CaseInsensitive)==0) return QStringLiteral("none");
        auto it=colorExact.constFind(nm); if(it!=colorExact.constEnd()) return it.value();
        auto jt=colorFold.constFind(nm.toCaseFolded()); if(jt!=colorFold.constEnd()) return jt.value();
        return QString();
    };

    QStringList body;
    bool haveBox=false; double minX=0,minY=0,maxX=0,maxY=0;
    auto acc=[&](double x0,double y0,double x1,double y1){
        if(!std::isfinite(x0)||!std::isfinite(y0)||!std::isfinite(x1)||!std::isfinite(y1)) return;
        if(!haveBox){ minX=x0;minY=y0;maxX=x1;maxY=y1; haveBox=true; }
        else { if(x0<minX)minX=x0; if(y0<minY)minY=y0; if(x1>maxX)maxX=x1; if(y1>maxY)maxY=y1; }
    };

    const int kMaxObj=200000, kMaxColor=200000;
    int objCount=0;
    bool sawRoot=false;

    while(!xr.atEnd()){
        QXmlStreamReader::TokenType t=xr.readNext();
        if(xr.hasError()) break;
        if(t!=QXmlStreamReader::StartElement) continue;
        QStringView en=xr.name();
        if(en==QLatin1String("SCRIBUSUTF8NEW")) sawRoot=true;
        else if(en==QLatin1String("COLOR")){
            if(colorExact.size()>=kMaxColor) continue;
            const QXmlStreamAttributes& a=xr.attributes();
            QString nm=a.value(QLatin1String("NAME")).toString();
            if(nm.isEmpty()) continue;
            QString hex=scribusParseColorVal(a.value(QLatin1String("RGB")).toString(),
                                             a.value(QLatin1String("CMYK")).toString());
            if(!hex.isEmpty()){ colorExact.insert(nm,hex); colorFold.insert(nm.toCaseFolded(),hex); }
        }
        else if(en==QLatin1String("PAGEOBJECT") || en==QLatin1String("ITEM")){
            if(objCount>=kMaxObj) continue;
            const QXmlStreamAttributes& a=xr.attributes();
            double xp=a.value(QLatin1String("XPOS")).toDouble();
            double yp=a.value(QLatin1String("YPOS")).toDouble();
            double w =a.value(QLatin1String("WIDTH")).toDouble();
            double h =a.value(QLatin1String("HEIGHT")).toDouble();
            double rot=a.value(QLatin1String("ROT")).toDouble();
            if(!std::isfinite(xp)||!std::isfinite(yp)) continue;
            if(qAbs(xp)>=1e12||qAbs(yp)>=1e12||qAbs(w)>=1e12||qAbs(h)>=1e12) continue;
            if(!std::isfinite(w)) w=0; if(!std::isfinite(h)) h=0;
            if(!std::isfinite(rot)) rot=0;

            QString fcol=lookupColor(a.value(QLatin1String("FCOLOR")).toString());
            QString pcol=lookupColor(a.value(QLatin1String("PCOLOR")).toString());
            if(fcol.isEmpty()) fcol=QStringLiteral("none");
            if(pcol.isEmpty()) pcol=QStringLiteral("none");
            double pw=a.value(QLatin1String("PWIDTH")).toDouble();
            if(!std::isfinite(pw)||pw<0||pw>=1e6) pw=0;

            QString transform=QStringLiteral("translate(%1 %2)").arg(scribusFmt(xp),scribusFmt(yp));
            if(std::isfinite(rot) && qAbs(rot)>1e-9)
                transform+=QStringLiteral(" rotate(%1)").arg(scribusFmt(rot));

            QString shape;
            // 1) explicit SVG-ish path / copath (object-local coords)
            QString pathAttr=a.value(QLatin1String("path")).toString();
            if(pathAttr.isEmpty()) pathAttr=a.value(QLatin1String("copath")).toString();
            QString d=scribusSanitizePathD(pathAttr);
            if(!d.isEmpty()){
                shape=QStringLiteral("<path d=\"%1\" fill=\"%2\" stroke=\"%3\" stroke-width=\"%4\"/>")
                        .arg(d, fcol, pcol, scribusFmt(pw));
            } else {
                // 2) POCOOR flat double list -> polyline (every 2nd pair = on-curve node)
                QString po=a.value(QLatin1String("POCOOR")).toString();
                if(po.isEmpty()) po=a.value(QLatin1String("COCOOR")).toString();
                std::vector<double> nums;
                if(!po.isEmpty() && po.size()<=400000){
                    QStringList tk=scribusTokenize(po);
                    nums.reserve(tk.size()<40000?(size_t)tk.size():40000);
                    for(int i=0;i<tk.size() && (int)nums.size()<40000;i++){
                        bool ok; double v=tk[i].toDouble(&ok);
                        if(ok && std::isfinite(v) && qAbs(v)<1e12) nums.push_back(v);
                    }
                }
                if(nums.size()>=4){
                    QString dd; int emitted=0;
                    size_t pairs=nums.size()/2;
                    for(size_t i=0;i<pairs;i+=2){
                        double x=nums[i*2], y=nums[i*2+1];
                        dd += (emitted==0?QLatin1String("M "):QLatin1String("L "));
                        dd += scribusFmt(x); dd+=QLatin1Char(' '); dd+=scribusFmt(y); dd+=QLatin1Char(' ');
                        emitted++;
                        if(emitted>=20000) break;
                    }
                    if(emitted>=2){
                        shape=QStringLiteral("<path d=\"%1\" fill=\"%2\" stroke=\"%3\" stroke-width=\"%4\"/>")
                                .arg(dd.trimmed(), fcol, pcol, scribusFmt(pw));
                    }
                }
                // 3) fallback: bounding rect (rect-like / image objects)
                if(shape.isEmpty() && w>0 && h>0){
                    shape=QStringLiteral("<rect x=\"0\" y=\"0\" width=\"%1\" height=\"%2\" fill=\"%3\" stroke=\"%4\" stroke-width=\"%5\"/>")
                            .arg(scribusFmt(w),scribusFmt(h),fcol,pcol,scribusFmt(pw));
                }
            }

            if(!shape.isEmpty()){
                body << QStringLiteral("<g transform=\"%1\">%2</g>").arg(transform,shape);
                objCount++;
                double x0=xp, y0=yp, x1=xp+(w>0?w:0), y1=yp+(h>0?h:0);
                if(x1<x0) std::swap(x0,x1);
                if(y1<y0) std::swap(y0,y1);
                if(x1-x0<1) x1=x0+1;
                if(y1-y0<1) y1=y0+1;
                acc(x0,y0,x1,y1);
            }
        }
    }

    if(!sawRoot) return QString();
    if(body.isEmpty()) return QString();

    if(!haveBox){ minX=0;minY=0;maxX=100;maxY=100; }
    const double pad=2.0;
    minX-=pad; minY-=pad; maxX+=pad; maxY+=pad;
    double vw=maxX-minX, vh=maxY-minY;
    if(!(vw>0)||!std::isfinite(vw)) vw=1;
    if(!(vh>0)||!std::isfinite(vh)) vh=1;
    if(vw>1e12) vw=1e12; if(vh>1e12) vh=1e12;

    QString out;
    out.reserve(256 + body.join(QString()).size());
    out += QStringLiteral(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"%1 %2 %3 %4\" width=\"%3\" height=\"%4\">")
        .arg(scribusFmt(minX),scribusFmt(minY),scribusFmt(vw),scribusFmt(vh));
    out += body.join(QLatin1Char('\n'));
    out += QStringLiteral("</svg>");
    return out;
}

} // namespace xara
