// Pixelmator Pro (.pxd) → a Document. The file is a ZIP (or, straight off a Mac, a folder): metadata.info is an
// SQLite database of layers and their settings, data/<uuid> holds each raster layer's pixels. Layout facts
// come from the sample files and two open descriptions of the format. Coordinates are in points with y up;
// backingScale says what a point is.
#include "prepress.h"
#include "applearchive.h"
#include "icc.h"
#include "sqlitemini.h"
#include "svgimport.h"
#include <QFontMetricsF>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPainterPath>
#include <QTransform>
#include <QtEndian>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <functional>
#include <zlib.h>

namespace xara {
namespace {

const qint64 kMaxPixels = qint64(1) << 28;
const int kMaskIndex = 32767;      // a mask is a child row of its layer, parked at this index

// --- tagged scalars: "4-tP", a reversed tag, a length, the payload. Doubles big-endian, integers little. ---
struct Vercon { QByteArray tag; const uchar* p = nullptr; int n = 0; };
Vercon vercon(const QByteArray& b){
    Vercon v;
    if(b.size() < 12 || !b.startsWith("4-tP")) return v;
    v.tag = b.mid(4, 4); std::reverse(v.tag.begin(), v.tag.end());
    const quint32 len = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(b.constData()) + 8);
    v.p = reinterpret_cast<const uchar*>(b.constData()) + 12;
    v.n = int(qMin<qint64>(len, b.size() - 12));
    return v;
}
double beDouble(const uchar* p){ const quint64 x = qFromBigEndian<quint64>(p); double d; std::memcpy(&d, &x, 8); return qIsFinite(d) ? d : 0.0; }
double vDouble(const QByteArray& b, double def){ const Vercon v = vercon(b); return v.n >= 8 ? beDouble(v.p) : def; }
QPointF vPoint(const QByteArray& b, const QPointF& def = QPointF()){ const Vercon v = vercon(b); return v.n >= 16 ? QPointF(beDouble(v.p), beDouble(v.p + 8)) : def; }
quint64 vU64(const QByteArray& b, quint64 def){ const Vercon v = vercon(b); return v.n >= 8 ? qFromLittleEndian<quint64>(v.p) : def; }
qint32 vI32(const QByteArray& b, qint32 def){ const Vercon v = vercon(b); return v.n >= 4 ? qFromLittleEndian<qint32>(v.p) : def; }
QString vString(const QByteArray& b){
    const Vercon v = vercon(b);
    if(v.n < 4) return QString();
    const quint32 len = qFromLittleEndian<quint32>(v.p);
    return QString::fromUtf8(reinterpret_cast<const char*>(v.p + 4), int(qMin<qint64>(len, v.n - 4)));
}
QByteArray vFourcc(const QByteArray& b){
    const Vercon v = vercon(b);
    if(v.n < 4) return QByteArray();
    QByteArray c(reinterpret_cast<const char*>(v.p), 4); std::reverse(c.begin(), c.end());
    return c;
}
QTransform vTransform(const QByteArray& b){
    const Vercon v = vercon(b);
    if(v.n < 48) return QTransform();
    return QTransform(beDouble(v.p), beDouble(v.p + 8), beDouble(v.p + 16), beDouble(v.p + 24), beDouble(v.p + 32), beDouble(v.p + 40));
}

// --- a raster layer's pixels ------------------------------------------------------------------------------
// "PTBitmapBuffer__", version, timestamp, width, height, row bytes, compression (1 deflate, 4 Apple LZ4),
// format record length, 8 bytes of FF, the format record, then the compressed rows: BGRA, straight alpha.
QImage decodeBitmap(const QByteArray& b){
    if(b.size() < 56 || !b.startsWith("PTBitmapBuffer__")) return QImage();
    const uchar* p = reinterpret_cast<const uchar*>(b.constData());
    const int w = qFromLittleEndian<qint32>(p + 28), h = qFromLittleEndian<qint32>(p + 32), stride = qFromLittleEndian<qint32>(p + 36);
    const int comp = qFromLittleEndian<qint32>(p + 40); const qint64 fmt = qFromLittleEndian<quint32>(p + 44);
    if(w <= 0 || h <= 0 || qint64(w) * h > kMaxPixels || stride < w || 56 + fmt > b.size()) return QImage();
    const qint64 total = qint64(stride) * h;
    if(total > (qint64(1) << 31) - 64) return QImage();
    QByteArray raw(int(total), '\0');
    const QByteArray in = b.mid(int(56 + fmt));
    if(comp == 4){ if(!appleLz4(in, raw)) return QImage(); }
    else if(comp == 1){
        z_stream z; std::memset(&z, 0, sizeof z);
        if(inflateInit2(&z, -15) != Z_OK) return QImage();
        z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.constData())); z.avail_in = uInt(in.size());
        z.next_out = reinterpret_cast<Bytef*>(raw.data()); z.avail_out = uInt(total);
        const int rc = inflate(&z, Z_FINISH); const bool done = z.avail_out == 0 || rc == Z_STREAM_END;
        inflateEnd(&z);
        if(!done) return QImage();
    } else return QImage();
    QImage img(w, h, QImage::Format_ARGB32);
    if(img.isNull()) return QImage();
    const uchar* s = reinterpret_cast<const uchar*>(raw.constData());
    const int bytes = stride / w;            // 8: 16 bits a channel, little-endian, so keep the high bytes; 1: a mask's grey
    for(int y = 0; y < h; ++y){
        QRgb* d = reinterpret_cast<QRgb*>(img.scanLine(y));
        const uchar* r = s + qint64(stride) * y;
        for(int x = 0; x < w; ++x){
            if(bytes >= 8) d[x] = qRgba(r[x*8 + 5], r[x*8 + 3], r[x*8 + 1], r[x*8 + 7]);
            else if(bytes >= 4) d[x] = qRgba(r[x*4 + 2], r[x*4 + 1], r[x*4], r[x*4 + 3]);
            else if(bytes == 2) d[x] = qRgba(r[x*2 + 1], r[x*2 + 1], r[x*2 + 1], 255);
            else d[x] = qRgba(r[x], r[x], r[x], 255);
        }
    }
    return img;
}
// The ICC profile nested in a format record: the bytes around its "acsp" signature, which sits 36 in.
QByteArray iccInRecord(const QByteArray& rec){
    const int a = rec.indexOf("acsp");
    if(a < 36) return QByteArray();
    const int start = a - 36;
    const quint32 len = qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(rec.constData()) + start);
    if(len < 128 || qint64(start) + len > rec.size()) return QByteArray();
    return rec.mid(start, int(len));
}
void convertToSrgb(QImage& img, const QByteArray& icc){
    if(icc.isEmpty()) return;
    IccToSrgb conv(icc, 3);
    if(!conv.ok()) return;
    QVector<uchar> in(img.width() * 3), out(img.width() * 3);
    for(int y = 0; y < img.height(); ++y){
        QRgb* d = reinterpret_cast<QRgb*>(img.scanLine(y));
        for(int x = 0; x < img.width(); ++x){ in[x*3] = uchar(qRed(d[x])); in[x*3 + 1] = uchar(qGreen(d[x])); in[x*3 + 2] = uchar(qBlue(d[x])); }
        conv.convert(in.constData(), out.data(), img.width());
        for(int x = 0; x < img.width(); ++x) d[x] = qRgba(out[x*3], out[x*3 + 1], out[x*3 + 2], qAlpha(d[x]));
    }
}

// --- layer records ----------------------------------------------------------------------------------------
struct Row { qint64 id = 0; QString uuid, parent; int index = 0, type = 0; QHash<QString,QByteArray> info; };

struct Place { QSizeF extent; QPointF topLeft; QTransform turn; };   // turn acts about the box's centre, box at the origin
Place placeOf(const Row& r, double H){
    Place pl;
    const double back = vDouble(r.info.value("backingScale"), 1.0);
    const QPointF pos = vPoint(r.info.value("position")) * back;
    const QPointF sz = vPoint(r.info.value("size"));
    pl.extent = QSizeF(sz.x() * back, sz.y() * back);
    pl.topLeft = QPointF(pos.x(), H - pos.y()) - QPointF(pl.extent.width() / 2, pl.extent.height() / 2);
    const QPointF scale = vPoint(r.info.value("scale"), QPointF(1, 1));
    const double angle = vDouble(r.info.value("angle"), 0.0);
    const QTransform stored = vTransform(r.info.value("transform"));
    QTransform local;
    if(scale != QPointF(1, 1)) local = local * QTransform::fromScale(scale.x(), scale.y());
    if(angle != 0){ QTransform rot; rot.rotate(angle); local = local * rot; }   // matched against Pixelmator's own render
    if(!stored.isIdentity()) local = local * stored;
    if(!local.isIdentity()){
        const QPointF c(pl.extent.width() / 2, pl.extent.height() / 2);
        pl.turn = QTransform::fromTranslate(-c.x(), -c.y()) * local * QTransform::fromTranslate(c.x(), c.y());
    }
    return pl;
}
QJsonObject unwrapped(const QJsonValue& v){
    if(v.isArray()){ const QJsonArray a = v.toArray(); return a.size() >= 2 ? a.at(1).toObject() : QJsonObject(); }
    return v.toObject();
}
QString num(double v){ return QString::number(v, 'g', 12); }
QString xmlEsc(QString s){ s.replace('&',"&amp;"); s.replace('<',"&lt;"); s.replace('>',"&gt;"); s.replace('"',"&quot;"); return s; }
QColor colorOf(const QJsonValue& v, const QColor& def){
    const QJsonArray c = unwrapped(v).value("c").toArray();
    if(c.size() < 3) return def;
    QColor o; o.setRgbF(float(qBound(0.0, c.at(0).toDouble(), 1.0)), float(qBound(0.0, c.at(1).toDouble(), 1.0)), float(qBound(0.0, c.at(2).toDouble(), 1.0)), float(qBound(0.0, c.size() > 3 ? c.at(3).toDouble(1) : 1.0, 1.0)));
    return o;
}
QVector<QJsonObject> enabledStyles(const QJsonObject& styles, const char* key){
    QVector<QJsonObject> out;
    for(const QJsonValue& v : styles.value(QLatin1String(key)).toArray()){
        const QJsonObject e = unwrapped(v);
        if(e.value("E").toDouble(1) >= 0.5) out.push_back(e);
    }
    return out;
}
QJsonObject stylesOf(const Row& r){ return unwrapped(QJsonValue(QJsonDocument::fromJson(r.info.value("styles-data")).array())); }

// A shape's bezier: a 10-byte "3.2.0" header in UTF-16, then element codes followed by big-endian doubles.
QPainterPath parseCGPath(const QByteArray& data, bool closeSubpaths){
    QPainterPath path; bool open = false;
    const uchar* p = reinterpret_cast<const uchar*>(data.constData()); const qint64 n = data.size();
    qint64 i = 10;
    auto pt = [&](qint64 at){ return QPointF(beDouble(p + at), beDouble(p + at + 8)); };
    auto finish = [&]{ if(closeSubpaths && open) path.closeSubpath(); open = false; };
    while(i < n){
        const char op = char(p[i++]);
        if(op == 'M' && i + 16 <= n){ finish(); path.moveTo(pt(i)); i += 16; }
        else if(op == 'L' && i + 16 <= n){ path.lineTo(pt(i)); i += 16; open = true; }
        else if(op == 'Q' && i + 32 <= n){ path.quadTo(pt(i), pt(i + 16)); i += 32; open = true; }
        else if(op == 'C' && i + 48 <= n){ path.cubicTo(pt(i), pt(i + 16), pt(i + 32)); i += 48; open = true; }
        else if(op == 'Z'){ path.closeSubpath(); open = false; }
        else break;
    }
    finish();
    return path;
}
// The bezier sits at a different depth per kind of shape, so it's found by walking.
QString findPathData(const QJsonValue& v, int depth = 0){
    if(depth > 64) return QString();
    if(v.isObject()){
        const QJsonObject o = v.toObject();
        if(o.contains("dataFromCGPath")) return o.value("dataFromCGPath").toString();
        for(const QJsonValue& c : o){ const QString f = findPathData(c, depth + 1); if(!f.isEmpty()) return f; }
    } else if(v.isArray()){
        for(const QJsonValue& c : v.toArray()){ const QString f = findPathData(c, depth + 1); if(!f.isEmpty()) return f; }
    }
    return QString();
}
QPainterPath shapePath(const QByteArray& blob){
    QPainterPath all;
    const QJsonObject root = QJsonDocument::fromJson(blob).object().value("versionSpecifiContainer").toObject();
    for(const QJsonValue& w : root.value("pathCodableWrappers").toArray()){
        const QJsonObject c = w.toObject().value("versionSpecifiContainer").toObject();
        const bool closed = c.value("type").toString() != QLatin1String("base");      // only a pen path is really open
        for(const QJsonValue& s : c.value("shapePathUnkeyedContainer").toArray()){
            const QString enc = findPathData(s);
            if(!enc.isEmpty()) all.addPath(parseCGPath(QByteArray::fromBase64(enc.toLatin1()), closed));
        }
    }
    return all;
}
QString pathD(const QPainterPath& p){
    QString d;
    for(int i = 0; i < p.elementCount(); ++i){
        const QPainterPath::Element e = p.elementAt(i);
        if(e.isMoveTo()) d += QStringLiteral("M%1 %2 ").arg(num(e.x), num(e.y));
        else if(e.isLineTo()) d += QStringLiteral("L%1 %2 ").arg(num(e.x), num(e.y));
        else if(e.isCurveTo() && i + 2 < p.elementCount()){
            const QPainterPath::Element c = p.elementAt(i + 1), z = p.elementAt(i + 2);
            d += QStringLiteral("C%1 %2 %3 %4 %5 %6 ").arg(num(e.x), num(e.y), num(c.x), num(c.y), num(z.x), num(z.y));
            i += 2;
        }
    }
    return d;
}

int strokeWhere(const QJsonObject& k){ return int(unwrapped(k.value("S")).value("p").toDouble(k.value("sP").toDouble(1))); }   // 0 inside, 1 centre, 2 outside

// Three box passes stand in for a gaussian of this radius.
void blurPlane(QVector<float>& a, int w, int h, double radius){
    const int r = qBound(0, qRound(radius / 2.0), 512);
    if(r <= 0) return;
    QVector<float> t(a.size());
    for(int pass = 0; pass < 3; ++pass){
        for(int y = 0; y < h; ++y){
            float sum = 0; const float* in = a.constData() + qint64(y) * w; float* out = t.data() + qint64(y) * w;
            for(int x = -r; x <= r; ++x) sum += in[qBound(0, x, w - 1)];
            for(int x = 0; x < w; ++x){ out[x] = sum / (2 * r + 1); sum += in[qMin(x + r + 1, w - 1)] - in[qMax(x - r, 0)]; }
        }
        for(int x = 0; x < w; ++x){
            float sum = 0;
            for(int y = -r; y <= r; ++y) sum += t[qint64(qBound(0, y, h - 1)) * w + x];
            for(int y = 0; y < h; ++y){ a[qint64(y) * w + x] = sum / (2 * r + 1); sum += t[qint64(qMin(y + r + 1, h - 1)) * w + x] - t[qint64(qMax(y - r, 0)) * w + x]; }
        }
    }
}
// Exact squared distance to the nearest set pixel, then its root (Felzenszwalb and Huttenlocher).
QVector<float> distanceTo(const QVector<uchar>& set, int w, int h){
    const float inf = 1e15f;
    QVector<float> d(set.size()), f(qMax(w, h)), res(qMax(w, h)), z(qMax(w, h) + 1); QVector<int> v(qMax(w, h));
    for(int i = 0; i < set.size(); ++i) d[i] = set[i] ? 0.f : inf;
    const auto pass = [&](int n){
        int k = 0; v[0] = 0; z[0] = -inf; z[1] = inf;
        for(int q = 1; q < n; ++q){
            float t = 0;
            for(;;){
                t = ((f[q] + float(q) * q) - (f[v[k]] + float(v[k]) * v[k])) / (2.f * (q - v[k]));
                if(t > z[k] || k == 0) break;
                --k;
            }
            ++k; v[k] = q; z[k] = t; z[k + 1] = inf;
        }
        k = 0;
        for(int q = 0; q < n; ++q){ while(z[k + 1] < q) ++k; const float g = float(q - v[k]); res[q] = g * g + f[v[k]]; }
    };
    for(int x = 0; x < w; ++x){ for(int y = 0; y < h; ++y) f[y] = d[qint64(y) * w + x]; pass(h); for(int y = 0; y < h; ++y) d[qint64(y) * w + x] = res[y]; }
    for(int y = 0; y < h; ++y){ for(int x = 0; x < w; ++x) f[x] = d[qint64(y) * w + x]; pass(w); for(int x = 0; x < w; ++x) d[qint64(y) * w + x] = std::sqrt(qMin(res[x], inf)); }
    return d;
}
inline QRgb over(QRgb top, double ta, QRgb under){    // straight alpha
    const double ua = qAlpha(under) / 255.0, oa = ta + ua * (1 - ta);
    if(oa <= 0) return 0u;
    const auto mix = [&](int t, int u){ return qBound(0, qRound((t * ta + u * ua * (1 - ta)) / oa), 255); };
    return qRgba(mix(qRed(top), qRed(under)), mix(qGreen(top), qGreen(under)), mix(qBlue(top), qBlue(under)), qRound(oa * 255));
}
// Pixelmator draws a raster layer's fill, inner shadow, stroke and shadow from its alpha. The model has no such
// thing for a bitmap, so they go into the pixels. Returns how far the image grew on each side.
int bakeStyles(QImage& img, const QJsonObject& styles, double toImage){
    const QVector<QJsonObject> fills = enabledStyles(styles, "f"), strokes = enabledStyles(styles, "s"), shadows = enabledStyles(styles, "S"), inner = enabledStyles(styles, "i");
    if(fills.isEmpty() && strokes.isEmpty() && shadows.isEmpty() && inner.isEmpty()) return 0;
    double reach = 0;
    if(!strokes.isEmpty()) reach = qMax(reach, strokes[0].value("W").toDouble(3) * toImage);
    if(!shadows.isEmpty()) reach = qMax(reach, (shadows[0].value("d").toDouble(2) + shadows[0].value("b").toDouble(5) * 2) * toImage);
    const int m = qBound(0, int(std::ceil(reach)) + 2, 2048);
    const int W = img.width() + 2 * m, H = img.height() + 2 * m;
    if(qint64(W) * H > (qint64(1) << 26)) return 0;
    QImage out(W, H, QImage::Format_ARGB32);
    if(out.isNull()) return 0;
    out.fill(0u);
    for(int y = 0; y < img.height(); ++y){ const QRgb* d = reinterpret_cast<const QRgb*>(img.constScanLine(y)); QRgb* o = reinterpret_cast<QRgb*>(out.scanLine(y + m)) + m; std::memcpy(o, d, size_t(img.width()) * 4); }
    QVector<float> alpha(qint64(W) * H);
    for(int y = 0; y < H; ++y){ const QRgb* d = reinterpret_cast<const QRgb*>(out.constScanLine(y)); for(int x = 0; x < W; ++x) alpha[qint64(y) * W + x] = qAlpha(d[x]) / 255.f; }
    const QVector<float> shape = alpha;
    const auto offsetOf = [toImage](const QJsonObject& s, double sign){ const double a = s.value("a").toDouble(), d = s.value("d").toDouble(2); return QPointF(sign * d * qCos(a), -sign * d * qSin(a)) * toImage; };
    const auto at = [&](const QVector<float>& p, int x, int y, float outside){ return (x >= 0 && y >= 0 && x < W && y < H) ? p[qint64(y) * W + x] : outside; };
    if(!fills.isEmpty()){
        const QColor c = colorOf(fills[0].value("c"), Qt::white); const double op = fills[0].value("o").toDouble(1) * c.alphaF();
        for(int y = 0; y < H; ++y){ QRgb* d = reinterpret_cast<QRgb*>(out.scanLine(y)); for(int x = 0; x < W; ++x) if(qAlpha(d[x])){ const QRgb base = d[x]; const auto mix = [&](int u, int v){ return qRound(u + (v - u) * op); }; d[x] = qRgba(mix(qRed(base), c.red()), mix(qGreen(base), c.green()), mix(qBlue(base), c.blue()), qAlpha(base)); } }
    }
    if(!inner.isEmpty()){
        const QJsonObject& s = inner[0]; QVector<float> inv(alpha.size());
        const QPointF off = offsetOf(s, -1);       // the cut-out slides against the light, so the far edge darkens
        const int ox = qRound(off.x()), oy = qRound(off.y());
        for(int y = 0; y < H; ++y) for(int x = 0; x < W; ++x) inv[qint64(y) * W + x] = 1.f - at(shape, x - ox, y - oy, 0.f);
        blurPlane(inv, W, H, s.value("b").toDouble(5) * toImage);
        const QColor c = colorOf(s.value("c"), Qt::black); const double op = s.value("o").toDouble(0.5) * c.alphaF();
        for(int y = 0; y < H; ++y){ QRgb* d = reinterpret_cast<QRgb*>(out.scanLine(y)); for(int x = 0; x < W; ++x) if(qAlpha(d[x])){ const double t = op * inv[qint64(y) * W + x]; const QRgb base = d[x]; const auto mix = [&](int u, int v){ return qRound(u + (v - u) * t); }; d[x] = qRgba(mix(qRed(base), c.red()), mix(qGreen(base), c.green()), mix(qBlue(base), c.blue()), qAlpha(base)); } }
    }
    if(!strokes.isEmpty()){
        const QJsonObject& k = strokes[0]; const double w = k.value("W").toDouble(3) * toImage; const int where = strokeWhere(k);
        const QColor c = colorOf(k.value("c"), Qt::black); const double op = k.value("o").toDouble(1) * c.alphaF();
        QVector<uchar> in(alpha.size()), outside(alpha.size());
        for(int i = 0; i < alpha.size(); ++i){ in[i] = shape[i] >= 0.5f; outside[i] = !in[i]; }
        const QVector<float> dOut = distanceTo(in, W, H), dIn = distanceTo(outside, W, H);
        for(int y = 0; y < H; ++y){
            QRgb* d = reinterpret_cast<QRgb*>(out.scanLine(y));
            for(int x = 0; x < W; ++x){
                const qint64 i = qint64(y) * W + x;
                const double sd = dOut[i] > 0 ? dOut[i] - 0.5 : 0.5 - dIn[i];     // negative inside
                const double lo = where == 0 ? -w : where == 2 ? 0 : -w / 2, hi = where == 0 ? 0 : where == 2 ? w : w / 2;
                const double cov = qBound(0.0, qMin(sd - lo, hi - sd) + 0.5, 1.0);
                if(cov > 0){ d[x] = over(c.rgb(), cov * op, d[x]); alpha[i] = qAlpha(d[x]) / 255.f; }
            }
        }
    }
    if(!shadows.isEmpty()){
        const QJsonObject& s = shadows[0]; QVector<float> sh(alpha.size());
        const QPointF off = offsetOf(s, 1); const int ox = qRound(off.x()), oy = qRound(off.y());
        for(int y = 0; y < H; ++y) for(int x = 0; x < W; ++x) sh[qint64(y) * W + x] = at(alpha, x - ox, y - oy, 0.f);
        blurPlane(sh, W, H, s.value("b").toDouble(5) * toImage);
        const QColor c = colorOf(s.value("c"), Qt::black); const double op = s.value("o").toDouble(0.5) * c.alphaF();
        for(int y = 0; y < H; ++y){
            QRgb* d = reinterpret_cast<QRgb*>(out.scanLine(y));
            for(int x = 0; x < W; ++x){ const double t = op * sh[qint64(y) * W + x]; if(t > 0) d[x] = over(d[x], qAlpha(d[x]) / 255.0, qRgba(c.red(), c.green(), c.blue(), qRound(t * 255))); }
        }
    }
    img = out;
    return m;
}

struct Builder {
    const QMap<QString,QByteArray>& files; const QString prefix; QHash<QString,QVector<Row>> kids;
    double W, H; QByteArray docIcc; PxdImportStats* st;
    int nextId = 0, guard = 0;
    QString defs;

    QByteArray file(const QString& name) const { return files.value(prefix + name); }
    QVector<Shape> svg(const QString& body){
        const QString s = QStringLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xarq=\"urn:xarq\" width=\"%1\" height=\"%2\" viewBox=\"0 0 %1 %2\">\n<defs>%3</defs>\n%4</svg>\n").arg(num(W), num(H), defs, body);
        defs.clear();
        return parseSvg(s.toUtf8());
    }
    QString matrix(const QTransform& t){ return QStringLiteral("matrix(%1 %2 %3 %4 %5 %6)").arg(num(t.m11()), num(t.m12()), num(t.m21()), num(t.m22()), num(t.dx()), num(t.dy())); }

    QString shadowAttr(const QJsonObject& styles){
        const QVector<QJsonObject> sh = enabledStyles(styles, "S");
        if(!enabledStyles(styles, "i").isEmpty() && st) ++st->innerShadows;     // only reached for shapes and text
        if(sh.isEmpty()) return QString();
        const QJsonObject& s = sh[0];
        const double a = s.value("a").toDouble(), d = s.value("d").toDouble(2);
        const QColor c = colorOf(s.value("c"), Qt::black);
        return QStringLiteral(" xarq:shadow=\"%1 %2 %3 %4 %5 0\"").arg(num(d * qCos(a)), num(-d * qSin(a)), num(s.value("b").toDouble(5)), c.name(), num(s.value("o").toDouble(1) * c.alphaF()));
    }
    QString paint(const QJsonObject& e, bool stroke, const QRectF& box){
        const char* attr = stroke ? "stroke" : "fill";
        const char* opAttr = stroke ? "stroke-opacity" : "fill-opacity";
        const double op = e.value("o").toDouble(1);
        if(e.value("fT").toDouble() > 0.5){
            const QJsonObject g = unwrapped(e.value("g"));
            QString stops; const QJsonArray ss = g.value("s").toArray();
            for(int i = 0; i < ss.size(); ++i){
                const QJsonArray pair = ss.at(i).toArray().at(1).toArray();
                const QJsonArray col = pair.at(0).toArray();
                QColor c; c.setRgbF(float(qBound(0.0, col.at(0).toDouble(), 1.0)), float(qBound(0.0, col.at(1).toDouble(), 1.0)), float(qBound(0.0, col.at(2).toDouble(), 1.0)));
                stops += QStringLiteral("<stop offset=\"%1\" stop-color=\"%2\" stop-opacity=\"%3\"/>").arg(num(pair.at(1).toDouble(ss.size() > 1 ? double(i) / (ss.size() - 1) : 0)), c.name(), num(col.at(3).toDouble(1) * op));
            }
            if(!stops.isEmpty()){
                const QJsonArray a = e.value("gSP").toArray(), z = e.value("gEP").toArray();
                const QPointF p0(box.left() + a.at(0).toDouble(0) * box.width(), box.bottom() - a.at(1).toDouble(0.5) * box.height());
                const QPointF p1(box.left() + z.at(0).toDouble(1) * box.width(), box.bottom() - z.at(1).toDouble(0.5) * box.height());
                const QString id = QStringLiteral("g%1").arg(++nextId);
                if(g.value("t").toInt() == 1)
                    defs += QStringLiteral("<radialGradient id=\"%1\" gradientUnits=\"userSpaceOnUse\" cx=\"%2\" cy=\"%3\" r=\"%4\">%5</radialGradient>").arg(id, num(p0.x()), num(p0.y()), num(QLineF(p0, p1).length()), stops);
                else
                    defs += QStringLiteral("<linearGradient id=\"%1\" gradientUnits=\"userSpaceOnUse\" x1=\"%2\" y1=\"%3\" x2=\"%4\" y2=\"%5\">%6</linearGradient>").arg(id, num(p0.x()), num(p0.y()), num(p1.x()), num(p1.y()), stops);
                return QStringLiteral(" %1=\"url(#%2)\"").arg(QLatin1String(attr), id);
            }
        }
        const QColor c = colorOf(e.value("c"), Qt::black);
        const double a = c.alphaF() * op;
        return QStringLiteral(" %1=\"%2\"%3").arg(QLatin1String(attr), c.name(), a < 1 ? QStringLiteral(" %1=\"%2\"").arg(QLatin1String(opAttr), num(a)) : QString());
    }

    bool shape(const Row& r, const Place& pl, Shape& s, const QString& name){
        QPainterPath path = shapePath(r.info.value("shape-shapeData"));
        if(path.isEmpty()) return false;
        // The bezier's y runs up from the bottom of the geometry box.
        path = QTransform(1, 0, 0, -1, 0, pl.extent.height()).map(path);
        const QJsonObject styles = stylesOf(r);
        const QRectF box(QPointF(0, 0), pl.extent);
        const QString d = pathD(path);
        const QVector<QJsonObject> fills = enabledStyles(styles, "f"), strokes = enabledStyles(styles, "s");
        const QString fill = fills.isEmpty() ? QStringLiteral(" fill=\"none\"") : paint(fills[0], false, box);
        const QString shadow = shadowAttr(styles);
        QString body = QStringLiteral("<path d=\"%1\"%2 stroke=\"none\"%3/>\n");
        if(strokes.isEmpty()){
            body = body.arg(d, fill, shadow);
        } else {
            const QJsonObject& k = strokes[0];
            const double w = k.value("W").toDouble(k.value("sW").toDouble(1));
            const QString sp = paint(k, true, box);
            const int where = strokeWhere(k);
            if(where == 0){        // inside: twice as wide, cut by the shape
                const QString id = QStringLiteral("c%1").arg(++nextId);
                defs += QStringLiteral("<clipPath id=\"%1\"><path d=\"%2\"/></clipPath>").arg(id, d);
                body = body.arg(d, fill, shadow) + QStringLiteral("<path d=\"%1\" fill=\"none\"%2 stroke-width=\"%3\" clip-path=\"url(#%4)\"/>\n").arg(d, sp, num(2 * w), id);
            } else if(where == 2){ // outside: twice as wide, under the fill
                body = QStringLiteral("<path d=\"%1\" fill=\"none\"%2 stroke-width=\"%3\"%4/>\n").arg(d, sp, num(2 * w), shadow) + QStringLiteral("<path d=\"%1\"%2 stroke=\"none\"/>\n").arg(d, fill);
            } else
                body = QStringLiteral("<path d=\"%1\"%2%3 stroke-width=\"%4\"%5/>\n").arg(d, fill, sp, num(w), shadow);
        }
        const QTransform m = pl.turn * QTransform::fromTranslate(pl.topLeft.x(), pl.topLeft.y());
        const QVector<Shape> out = svg(QStringLiteral("<g transform=\"%1\">\n%2</g>\n").arg(matrix(m), body));
        if(out.isEmpty()) return false;
        if(out.size() == 1){ s = out[0]; }
        else { s.type = ShapeType::Group; s.children = out; }
        s.name = name;
        if(st) ++st->vectorLayers;
        return true;
    }

    bool text(const Row& r, const Place& pl, Shape& s, const QString& name){
        const QJsonObject root = QJsonDocument::fromJson(r.info.value("text-stringData")).object().value("versionSpecifiContainer").toObject();
        const QByteArray arc = QByteArray::fromBase64(root.value("stringNSCodingData").toString().toLatin1());
        KeyedArchive a; int rootUid = 0;
        if(!a.open(arc, &rootUid)) return false;
        const PObj rootObj = a.at(rootUid);
        PObj strObj = a.field(rootObj, "NSString");
        if(strObj.t == PObj::Dict) strObj = a.field(strObj, "NS.string");
        const QString raw = strObj.s;
        if(raw.isEmpty()) return false;

        struct Sty { QString family = QStringLiteral("Helvetica"); int weight = 400; bool italic = false; double size = 24; QColor color = Qt::black; bool underline = false; int align = 0; };
        const double back = vDouble(r.info.value("backingScale"), 1.0);
        auto readDict = [&](int dictUid){
            Sty y;
            const PObj d = a.at(dictUid);
            QVector<int> keys, vals;
            for(int k : a.field(d, "NS.keys").arr){ const PObj& e = a.pl.obj(k); keys.push_back(e.t == PObj::Uid ? int(e.i) : 0); }
            for(int k : a.field(d, "NS.objects").arr){ const PObj& e = a.pl.obj(k); vals.push_back(e.t == PObj::Uid ? int(e.i) : 0); }
            for(int i = 0; i < keys.size() && i < vals.size(); ++i){
                const QString key = a.at(keys[i]).s;
                const PObj v = a.at(vals[i]);
                if(key.endsWith(QLatin1String(".font-style-data"))){
                    const QByteArray bytes = v.t == PObj::Data ? v.b : a.field(v, "NS.data").b;
                    const QJsonObject f = unwrapped(QJsonValue(QJsonDocument::fromJson(bytes).array()));
                    if(f.contains("s")) y.size = f.value("s").toDouble(24);
                    if(f.contains("n")){
                        const QString nm = f.value("n").toString();
                        const int dash = nm.lastIndexOf(QLatin1Char('-'));
                        y.family = dash > 0 ? nm.left(dash) : nm;
                        const QString face = dash > 0 ? nm.mid(dash + 1).toLower() : QString();
                        if(face.contains("thin")) y.weight = 100; else if(face.contains("extralight") || face.contains("ultralight")) y.weight = 200;
                        else if(face.contains("light")) y.weight = 300; else if(face.contains("medium")) y.weight = 500;
                        else if(face.contains("semibold") || face.contains("demibold")) y.weight = 600; else if(face.contains("extrabold") || face.contains("heavy")) y.weight = 800;
                        else if(face.contains("black")) y.weight = 900; else if(face.contains("bold")) y.weight = 700;
                        if(face.contains("italic") || face.contains("oblique")) y.italic = true;
                    }
                    if(f.value("b").toInt() != 0) y.weight = 700;
                    if(f.value("i").toInt() != 0) y.italic = true;
                } else if(key.endsWith(QLatin1String(".color"))){
                    const QByteArray comp = a.field(v, "NSComponents").b;
                    const QList<QByteArray> parts = comp.split(' ');
                    if(parts.size() >= 3){ y.color.setRgbF(float(qBound(0.0, parts[0].toDouble(), 1.0)), float(qBound(0.0, parts[1].toDouble(), 1.0)), float(qBound(0.0, parts[2].toDouble(), 1.0)), float(qBound(0.0, parts.size() > 3 ? parts[3].toDouble() : 1.0, 1.0))); }
                } else if(key.endsWith(QLatin1String(".paragraph-style"))){
                    y.align = int(a.num(v, "NSAlignment", 0));
                } else if(key.endsWith(QLatin1String(".underline"))){
                    y.underline = (v.t == PObj::Int ? v.i : 0) != 0;
                }
            }
            return y;
        };
        struct R { int from, len; Sty sty; };
        QVector<R> runs;
        const int attrsUid = a.fieldUid(rootObj, "NSAttributes");
        const PObj attrs = a.at(attrsUid);
        if(attrs.dict.contains("NS.keys") || !attrs.dict.contains("NS.objects")){
            runs.push_back({ 0, int(raw.size()), attrsUid ? readDict(attrsUid) : Sty() });
        } else {
            QVector<Sty> table;
            for(int k : a.field(attrs, "NS.objects").arr){ const PObj& e = a.pl.obj(k); table.push_back(readDict(e.t == PObj::Uid ? int(e.i) : 0)); }
            const PObj infoObj = a.field(rootObj, "NSAttributeInfo");
            const QByteArray info = infoObj.t == PObj::Data ? infoObj.b : a.field(infoObj, "NS.data").b;
            int i = 0, cursor = 0;
            const auto var = [&](){ int v = 0, shift = 0; while(i < info.size() && shift < 28){ const uchar c = uchar(info[i++]); v |= (c & 0x7f) << shift; shift += 7; if(!(c & 0x80)) break; } return v; };
            while(i < info.size()){
                const int len = var(), idx = var();
                if(idx >= 0 && idx < table.size()) runs.push_back({ cursor, len, table[idx] });
                cursor += len;
            }
            if(runs.isEmpty() && !table.isEmpty()) runs.push_back({ 0, int(raw.size()), table[0] });
        }
        if(runs.isEmpty()) runs.push_back({ 0, int(raw.size()), Sty() });
        const auto fontOf = [&](const Sty& y){ QFont f(y.family); f.setPixelSize(qMax(1, qRound(y.size))); f.setWeight(QFont::Weight(y.weight)); f.setItalic(y.italic); return f; };

        // Words into lines. A layer whose width isn't autosized wraps at its box.
        const QPointF inset = vPoint(r.info.value("text-insets"), QPointF(4, 4)) * back;
        const bool wrap = vI32(r.info.value("text-widthAutosizable"), 1) == 0 || vI32(r.info.value("text-layerType"), 0) == 1;
        const double avail = pl.extent.width() - 2 * inset.x();
        struct Piece { QString t; const Sty* y; };
        QVector<QVector<Piece>> lines; lines.push_back({});
        double lw = 0;
        for(const R& run : runs){
            const QString seg = raw.mid(run.from, run.len);
            const QFontMetricsF fm(fontOf(run.sty));
            int i = 0;
            while(i < seg.size()){
                if(seg[i] == QLatin1Char('\n') || seg[i] == QChar(0x2028)){ lines.push_back({}); lw = 0; ++i; continue; }
                int j = i; while(j < seg.size() && seg[j] != QLatin1Char(' ') && seg[j] != QLatin1Char('\n') && seg[j] != QChar(0x2028)) ++j;
                while(j < seg.size() && seg[j] == QLatin1Char(' ')) ++j;
                const QString w = seg.mid(i, qMax(1, j - i));
                const double wd = fm.horizontalAdvance(w), wt = fm.horizontalAdvance(w.trimmed());
                if(wrap && avail > 0 && lw + wt > avail && !lines.back().isEmpty()){ lines.push_back({}); lw = 0; }
                lines.back().push_back({ w, &run.sty }); lw += wd;
                i += w.size();
            }
        }
        struct LineGeo { double asc, step, width; };
        QVector<LineGeo> geo; double block = 0;
        for(const auto& ln : lines){
            LineGeo g{ 0, 0, 0 };
            for(const Piece& q : ln){ const QFontMetricsF fm(fontOf(*q.y)); g.asc = qMax(g.asc, fm.ascent()); g.step = qMax(g.step, fm.height()); g.width += fm.horizontalAdvance(q.t); }
            if(ln.isEmpty()){ const QFontMetricsF fm(fontOf(runs[0].sty)); g.asc = fm.ascent(); g.step = fm.height(); }
            geo.push_back(g); block += g.step;
        }
        const int vAlign = vI32(r.info.value("text-verticalAlignment"), 0);
        double y = inset.y();
        if(vAlign == 1) y = (pl.extent.height() - block) / 2; else if(vAlign == 2) y = pl.extent.height() - inset.y() - block;
        QString body;
        for(int i = 0; i < lines.size(); ++i){
            const auto& ln = lines[i];
            if(!ln.isEmpty()){
                const int al = ln[0].y->align;       // NSTextAlignment: 0 left, 1 right on these files' platform-neutral table, 2 centre
                double x = inset.x();
                if(al == 2 || al == 1){
                    const double room = pl.extent.width() - 2 * inset.x() - geo[i].width;
                    x += al == 2 ? room / 2 : room;
                }
                body += QStringLiteral("<text xml:space=\"preserve\" x=\"%1\" y=\"%2\">").arg(num(x), num(y + geo[i].asc));
                int a0 = 0;
                while(a0 < ln.size()){
                    int b0 = a0; QString t;
                    while(b0 < ln.size() && ln[b0].y == ln[a0].y){ t += ln[b0].t; ++b0; }
                    if(t.trimmed().isEmpty()) t.replace(QLatin1Char(' '), QChar(0x00A0));
                    const Sty& z = *ln[a0].y;
                    body += QStringLiteral("<tspan font-family=\"%1\" font-size=\"%2\" font-weight=\"%3\" font-style=\"%4\" fill=\"%5\"%6%7>%8</tspan>")
                        .arg(xmlEsc(z.family), num(z.size)).arg(z.weight).arg(z.italic ? "italic" : "normal", z.color.name(),
                             z.color.alphaF() < 1 ? QStringLiteral(" fill-opacity=\"%1\"").arg(num(z.color.alphaF())) : QString(),
                             z.underline ? QStringLiteral(" text-decoration=\"underline\"") : QString(), xmlEsc(t));
                    a0 = b0;
                }
                body += QStringLiteral("</text>\n");
            }
            y += geo[i].step;
        }
        if(body.isEmpty()) return false;
        const QTransform m = pl.turn * QTransform::fromTranslate(pl.topLeft.x(), pl.topLeft.y());
        const QVector<Shape> out = svg(QStringLiteral("<g transform=\"%1\"%2>\n%3</g>\n").arg(matrix(m), shadowAttr(stylesOf(r)), body));
        if(out.isEmpty()) return false;
        if(out.size() == 1) s = out[0]; else { s.type = ShapeType::Group; s.children = out; }
        s.name = name;
        if(st) ++st->textLayers;
        return true;
    }

    bool raster(const Row& r, const Place& pl, Shape& s, const QString& name){
        const QByteArray blob = file(QStringLiteral("data/") + r.uuid);
        QImage img = decodeBitmap(blob);
        bool source = false;
        if(img.isNull()){
            // A placed image keeps its pixels in its source file, whose opaque part fills the layer's box.
            const QByteArray src = file(QStringLiteral("data/") + r.uuid + QStringLiteral("-OriginalContentSource"));
            QImage pic;
            if(src.isEmpty() || !pic.loadFromData(src)) return false;
            pic = pic.convertToFormat(QImage::Format_ARGB32);
            int x0 = pic.width(), x1 = -1, y0 = pic.height(), y1 = -1;
            for(int y = 0; y < pic.height(); ++y){
                const QRgb* d = reinterpret_cast<const QRgb*>(pic.constScanLine(y));
                for(int x = 0; x < pic.width(); ++x) if(qAlpha(d[x])){ x0 = qMin(x0, x); x1 = qMax(x1, x); y0 = qMin(y0, y); y1 = qMax(y1, y); }
            }
            if(x1 >= x0 && (x1 - x0 + 1 != pic.width() || y1 - y0 + 1 != pic.height())) pic = pic.copy(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
            const QSize ext = pl.extent.toSize();
            if(ext.width() > 0 && ext.height() > 0 && qint64(ext.width()) * ext.height() <= kMaxPixels && pic.size() != ext)
                pic = pic.scaled(ext, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            img = pic; source = true;
        }
        if(!source){
            QByteArray icc = iccInRecord(blob.left(1 << 20));
            if(icc.isEmpty()) icc = docIcc;
            convertToSrgb(img, icc);
        }
        const QPointF sc = vPoint(r.info.value("scale"), QPointF(1, 1));
        const double mean = (qAbs(sc.x()) + qAbs(sc.y())) / 2;
        const int grown = bakeStyles(img, stylesOf(r), mean > 1e-3 ? 1.0 / mean : 1.0);
        s = psdBitmapShape(img, 0, 0, name);
        s.transform = QTransform::fromTranslate(-grown, -grown) * pl.turn * QTransform::fromTranslate(pl.topLeft.x(), pl.topLeft.y());
        if(st) ++st->rasterLayers;
        return true;
    }

    // Multiply a canvas-sized coverage into every bitmap under this shape; other shapes get the outline when it has one.
    void applyCoverage(Shape& s, const QImage& cov, const QPainterPath& outline, int depth){
        if(s.type == ShapeType::Group){ if(depth < 64) for(Shape& c : s.children) applyCoverage(c, cov, outline, depth + 1); return; }
        if(s.fill.type == FillType::Bitmap && !s.fill.image.isNull()){
            QImage im = s.fill.image.convertToFormat(QImage::Format_ARGB32);
            for(int y = 0; y < im.height(); ++y){
                QRgb* d = reinterpret_cast<QRgb*>(im.scanLine(y));
                for(int x = 0; x < im.width(); ++x){
                    const QPointF p = s.transform.map(QPointF(x + 0.5, y + 0.5));
                    const int u = int(std::floor(p.x())), v = int(std::floor(p.y()));
                    const int m = (u >= 0 && v >= 0 && u < cov.width() && v < cov.height()) ? cov.constScanLine(v)[u] : 0;
                    d[x] = (d[x] & 0x00ffffffu) | quint32(qAlpha(d[x]) * m / 255) << 24;
                }
            }
            s.fill.image = im; s.fill.origImage = im;
        } else if(!outline.isEmpty()) {
            s.regionPath = s.transform.inverted().map(outline);
        } else if(st) ++st->unappliedMasks;
    }
    void applyMask(const Row& r, Shape& s){
        for(const Row& m : kids.value(r.uuid)){
            if(m.index != kMaskIndex) continue;
            if(int(W) <= 0 || int(H) <= 0) return;
            QImage cov(int(W), int(H), QImage::Format_Indexed8);
            if(cov.isNull()) return;
            QVector<QRgb> grey(256); for(int i = 0; i < 256; ++i) grey[i] = qRgb(i, i, i);
            cov.setColorTable(grey);
            const Place mp = placeOf(m, H);
            QPainterPath outline;
            if(m.type == 1){
                const QImage px = decodeBitmap(file(QStringLiteral("data/") + m.uuid));
                if(px.isNull()) return;
                // A reveal-all mask paints over white, a hide-all over black.
                cov.fill(uint(qBound(0.0, vDouble(m.info.value("maskBackgroundValue"), 1.0), 1.0) * 255));
                const int ox = qRound(mp.topLeft.x()), oy = qRound(mp.topLeft.y());
                for(int y = 0; y < px.height(); ++y){
                    const int v = oy + y; if(v < 0 || v >= cov.height()) continue;
                    const QRgb* d = reinterpret_cast<const QRgb*>(px.constScanLine(y));
                    uchar* c = cov.scanLine(v);
                    for(int x = 0; x < px.width(); ++x){ const int u = ox + x; if(u >= 0 && u < cov.width()){ const int g = qGray(d[x]), a = qAlpha(d[x]); c[u] = uchar((g * a + c[u] * (255 - a)) / 255); } }
                }
            } else if(m.type == 3){
                outline = shapePath(m.info.value("shape-shapeData"));
                outline = QTransform(1, 0, 0, -1, 0, mp.extent.height()).map(outline);
                outline.translate(mp.topLeft);
                QImage a8(int(W), int(H), QImage::Format_ARGB32_Premultiplied);
                if(a8.isNull()) return;
                a8.fill(Qt::black);
                { QPainter g(&a8); g.setRenderHint(QPainter::Antialiasing); g.fillPath(outline, Qt::white); }
                for(int y = 0; y < a8.height(); ++y){ const QRgb* d = reinterpret_cast<const QRgb*>(a8.constScanLine(y)); uchar* c = cov.scanLine(y); for(int x = 0; x < a8.width(); ++x) c[x] = uchar(qRed(d[x])); }
            } else return;
            applyCoverage(s, cov, outline, 0);
            if(st) ++st->masks;
            return;
        }
    }

    // A bitmap layer's alpha, as a canvas-sized plane.
    QImage alphaOnCanvas(const Shape& base){
        QImage cov(int(W), int(H), QImage::Format_Indexed8);
        if(cov.isNull()) return cov;
        QVector<QRgb> grey(256); for(int i = 0; i < 256; ++i) grey[i] = qRgb(i, i, i);
        cov.setColorTable(grey); cov.fill(0u);
        const QImage im = base.fill.image.convertToFormat(QImage::Format_ARGB32);
        for(int y = 0; y < im.height(); ++y){
            const QRgb* d = reinterpret_cast<const QRgb*>(im.constScanLine(y));
            for(int x = 0; x < im.width(); ++x){
                const QPointF p = base.transform.map(QPointF(x + 0.5, y + 0.5));
                const int u = int(std::floor(p.x())), v = int(std::floor(p.y()));
                if(u >= 0 && v >= 0 && u < cov.width() && v < cov.height()) cov.scanLine(v)[u] = uchar(qAlpha(d[x]));
            }
        }
        return cov;
    }
    QVector<Shape> list(QVector<Row> rows, int depth){
        QVector<Shape> out;
        if(depth > 64) return out;
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b){ return a.index > b.index; });  // index 0 is the top
        int baseIndex = -1;       // the bitmap below, which a clipping layer cuts to
        for(const Row& r : rows){
            if(r.index == kMaskIndex || ++guard > 100000) continue;
            const Place pl = placeOf(r, H);
            const QString name = vString(r.info.value("name"));
            Shape s; bool ok = false;
            const QByteArray blend = vFourcc(r.info.value("blendMode"));
            const double opacity = r.info.contains("opct-nrm") ? qBound(0.0, vDouble(r.info.value("opct-nrm"), 1.0), 1.0)
                                 : qBound(0.0, double(qFromLittleEndian<quint16>(vercon(r.info.value("opacity")).n >= 2 ? vercon(r.info.value("opacity")).p : reinterpret_cast<const uchar*>("\x64\0"))) / 100.0, 1.0);
            if(r.type == 1) ok = raster(r, pl, s, name);
            else if(r.type == 2) ok = text(r, pl, s, name);
            else if(r.type == 3) ok = shape(r, pl, s, name);
            else if(r.type == 4){
                const QVector<Shape> sub = list(kids.value(r.uuid), depth + 1);
                if(!sub.isEmpty()){
                    s.type = ShapeType::Group; s.name = name; s.children = sub;
                    s.isolate = blend != "pass";
                    ok = true;
                    if(st) ++st->groups;
                }
            } else if(st){
                if(r.type == 6) ++st->adjustmentLayers; else ++st->unknownLayers;
            }
            if(!ok) continue;
            if(r.info.contains("color-adjustments") && st){
                const QJsonObject adj = unwrapped(QJsonValue(QJsonDocument::fromJson(r.info.value("color-adjustments")).array()));
                bool any = false;
                for(auto it = adj.begin(); it != adj.end(); ++it){ const QJsonObject e = unwrapped(it.value()); if(e.value("E").toDouble() > 0.5) any = true; }
                if(any) ++st->adjustedLayers;
            }
            // A blur effect is the one effect the model can hold.
            if(r.info.contains("effects-data")){
                const QJsonObject eff = unwrapped(QJsonValue(QJsonDocument::fromJson(r.info.value("effects-data")).array()));
                for(const QJsonValue& w : eff.value("w").toArray()){
                    const QJsonObject o = w.toObject().value("versionSpecifiContainer").toObject();
                    const QString type = o.value("type").toString();
                    const QJsonObject filter = o.value("effectUnkeyedContainer").toArray().at(0).toObject().value("versionSpecifiContainer").toObject();
                    KeyedArchive fa; int fr = 0; double radius = 0;
                    if(fa.open(QByteArray::fromBase64(filter.value("CIFilterNSCodingData").toString().toLatin1()), &fr)){
                        const PObj fo = fa.at(fr);
                        radius = fa.num(fo, "CI_inputRadius", fa.num(fo, "inputRadius", 0));
                    }
                    if(type == QLatin1String("PTGaussianBlur") && radius > 0){ s.blur = radius * 1.8; }
                    else if(st) ++st->otherEffects;
                }
            }
            applyMask(r, s);
            const quint64 flags = vU64(r.info.value("flags"), 1);
            if(flags & 8){
                if(baseIndex >= 0){
                    const QImage cov = alphaOnCanvas(out[baseIndex]);
                    if(!cov.isNull()){ applyCoverage(s, cov, QPainterPath(), 0); if(st) ++st->clipped; }
                }
            } else baseIndex = (s.type != ShapeType::Group && s.fill.type == FillType::Bitmap) ? int(out.size()) : -1;
            s.opacity = opacity;
            s.visible = (flags & 1) != 0;
            if(blend != "pass" && !blend.isEmpty()) psdApplyBlend(s, blend);
            out.push_back(s);
        }
        return out;
    }
};

} // namespace

Document importPxdDocument(const QMap<QString,QByteArray>& files, PxdImportStats* st){
    Document doc;
    // Entries may sit under a wrapping folder.
    QString prefix;
    for(auto it = files.begin(); it != files.end(); ++it)
        if(it.key().endsWith(QLatin1String("metadata.info"))){ prefix = it.key().chopped(13); break; }
    SqliteMini db;
    if(!db.open(files.value(prefix + QStringLiteral("metadata.info")))) return doc;
    double W = 0, H = 0; QByteArray icc;
    for(const SqliteMini::Row& r : db.rows("document_info")){
        const QString k = r.value("key").toString();
        const QByteArray v = r.value("value").toByteArray();
        if(k == QLatin1String("size")){ const Vercon c = vercon(v); if(c.n >= 16){ W = double(qFromLittleEndian<quint64>(c.p)); H = double(qFromLittleEndian<quint64>(c.p + 8)); } }
        else if(k == QLatin1String("format")) icc = iccInRecord(v);
    }
    if(!(W >= 1 && H >= 1) || W * H > double(kMaxPixels)) return doc;
    QHash<qint64,QHash<QString,QByteArray>> infos;
    for(const SqliteMini::Row& r : db.rows("layer_info")) infos[r.value("layer_id").toLongLong()].insert(r.value("key").toString(), r.value("value").toByteArray());
    Builder b{ files, prefix, {}, W, H, icc, st };
    QVector<Row> top;
    for(const SqliteMini::Row& r : db.rows("document_layers")){
        Row row; row.id = r.value("id").toLongLong(); row.uuid = r.value("identifier").toString(); row.parent = r.value("parent_identifier").toString();
        row.index = r.value("index_at_parent").toInt(); row.type = r.value("type").toInt(); row.info = infos.value(row.id);
        if(row.parent.isEmpty()) top.push_back(row); else b.kids[row.parent].push_back(row);
    }
    doc.pageSize = QSizeF(W, H);
    doc.shapes = b.list(top, 0);
    return doc;
}
} // namespace xara
