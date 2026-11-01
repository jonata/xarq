// Procreate (.procreate) → a Document of layers. The file is a ZIP: Document.archive, a binary plist written
// by NSKeyedArchiver, describes the layers, and each layer's pixels are square tiles named
// "{UUID}/{col}~{row}.chunk" (LZO) or ".lz4" (Apple's framed LZ4), RGBA and premultiplied.
#include "prepress.h"
#include "applearchive.h"
#include <QImage>
#include <QHash>
#include <QPainter>
#include <QRegularExpression>
#include <QSet>
#include <QTransform>
#include <QtEndian>
#include <cstring>
#include <memory>

namespace xara {
namespace {

bool lzo1x(const QByteArray& in, QByteArray& out){
    const uchar* s = reinterpret_cast<const uchar*>(in.constData()); const qint64 n = in.size();
    uchar* d = reinterpret_cast<uchar*>(out.data()); const qint64 cap = out.size();
    qint64 ip = 0, op = 0, t = 0, m = 0;
    const auto copyLit = [&](qint64 c){ if(ip + c > n || op + c > cap) return false; std::memcpy(d + op, s + ip, size_t(c)); ip += c; op += c; return true; };
    const auto copyMatch = [&](qint64 from, qint64 c){ if(from < 0 || from >= op || op + c > cap) return false; for(qint64 i = 0; i < c; ++i) d[op + i] = d[from + i]; op += c; return true; };
    if(n < 1) return false;
    if(s[0] > 17){
        t = s[ip++] - 17;
        if(t < 4) goto match_next;
        if(!copyLit(t)) return false;
        goto first_literal_run;
    }
    for(;;){
        if(ip >= n) return false;
        t = s[ip++];
        if(t >= 16) goto match;
        if(t == 0){
            while(ip < n && s[ip] == 0){ t += 255; ++ip; }
            if(ip >= n) return false;
            t += 15 + s[ip++];
        }
        if(!copyLit(t + 3)) return false;
first_literal_run:
        if(ip >= n) return false;
        t = s[ip++];
        if(t >= 16) goto match;
        if(ip >= n) return false;
        m = op - 1 - 0x800 - (t >> 2) - (qint64(s[ip++]) << 2);
        if(!copyMatch(m, 3)) return false;
        goto match_done;
        for(;;){
match:
            if(t >= 64){
                if(ip >= n) return false;
                m = op - 1 - ((t >> 2) & 7) - (qint64(s[ip++]) << 3);
                t = (t >> 5) - 1;
            } else if(t >= 32){
                t &= 31;
                if(t == 0){
                    while(ip < n && s[ip] == 0){ t += 255; ++ip; }
                    if(ip >= n) return false;
                    t += 31 + s[ip++];
                }
                if(ip + 2 > n) return false;
                m = op - 1 - (s[ip] >> 2) - (qint64(s[ip + 1]) << 6);
                ip += 2;
            } else if(t >= 16){
                m = op - (qint64(t & 8) << 11);
                t &= 7;
                if(t == 0){
                    while(ip < n && s[ip] == 0){ t += 255; ++ip; }
                    if(ip >= n) return false;
                    t += 7 + s[ip++];
                }
                if(ip + 2 > n) return false;
                m -= (s[ip] >> 2) + (qint64(s[ip + 1]) << 6);
                ip += 2;
                if(m == op) return op == cap;      // the end marker
                m -= 0x4000;
            } else {
                if(ip >= n) return false;
                m = op - 1 - (t >> 2) - (qint64(s[ip++]) << 2);
                if(!copyMatch(m, 2)) return false;
                goto match_done;
            }
            if(!copyMatch(m, t + 2)) return false;
match_done:
            if(ip < 2) return false;
            t = s[ip - 2] & 3;
            if(t == 0) break;
match_next:
            if(!copyLit(t)) return false;
            if(ip >= n) return false;
            t = s[ip++];
        }
    }
}

// --- the document ---------------------------------------------------------------------------------
QByteArray blendKey(qint64 m){
    switch(m){
        case 1: return "mul "; case 2: return "scrn"; case 3: return "lddg"; case 4: return "lite"; case 5: return "smud";
        case 6: return "diff"; case 7: return "fsub"; case 8: return "lbrn"; case 9: return "div "; case 10: return "idiv";
        case 11: return "over"; case 12: return "hLit"; case 13: return "colr"; case 14: return "lum "; case 15: return "hue ";
        case 16: return "sat "; case 17: return "sLit"; case 19: return "dark"; case 20: return "hMix"; case 21: return "vLit";
        case 22: return "lLit"; case 23: return "pLit"; case 24: return "lgCl"; case 25: return "dkCl"; case 26: return "fdiv";
        default: return "norm";
    }
}
struct Canvas {
    int W = 0, H = 0, tile = 0, cols = 0, rows = 0;
    QHash<QString,QVector<QString>> tilesByLayer;    // uuid → archive entry names
    const QMap<QString,QByteArray>* zip = nullptr;
};
bool parseTileName(const QString& name, int& col, int& row){
    const int slash = name.lastIndexOf(QLatin1Char('/')), tilde = name.indexOf(QLatin1Char('~'), slash + 1);
    const int dot = name.indexOf(QLatin1Char('.'), tilde + 1);
    if(slash < 0 || tilde < 0 || dot < 0) return false;
    bool a = false, b = false;
    col = name.mid(slash + 1, tilde - slash - 1).toInt(&a);
    row = name.mid(tilde + 1, dot - tilde - 1).toInt(&b);
    return a && b;
}
// A layer's tiles, cropped to the ones it has. Masks are one byte per pixel.
bool layerImage(const Canvas& c, const QString& uuid, bool isMask, QImage& img, int& X, int& Y, ProcreateImportStats* stats){
    const QVector<QString> names = c.tilesByLayer.value(uuid);
    if(names.isEmpty()) return false;
    int c0 = c.cols, c1 = -1, r0 = c.rows, r1 = -1;
    for(const QString& nm : names){
        int col, row; if(!parseTileName(nm, col, row) || col < 0 || row < 0 || col >= c.cols || row >= c.rows) continue;
        c0 = qMin(c0, col); c1 = qMax(c1, col); r0 = qMin(r0, row); r1 = qMax(r1, row);
    }
    if(c1 < 0) return false;
    X = c0 * c.tile; Y = r0 * c.tile;
    const int iw = qMin(c.W, (c1 + 1) * c.tile) - X, ih = qMin(c.H, (r1 + 1) * c.tile) - Y;
    if(iw <= 0 || ih <= 0) return false;
    img = QImage(iw, ih, QImage::Format_ARGB32_Premultiplied);
    if(img.isNull()) return false;
    img.fill(Qt::transparent);
    const int ch = isMask ? 1 : 4;
    bool any = false;
    for(const QString& nm : names){
        int col, row; if(!parseTileName(nm, col, row) || col < c0 || col > c1 || row < r0 || row > r1) continue;
        const int tw = qMin(c.tile, c.W - col * c.tile), th = qMin(c.tile, c.H - row * c.tile);
        if(tw <= 0 || th <= 0) continue;
        QByteArray raw(int(qint64(tw) * th * ch), '\0');
        const QByteArray in = c.zip->value(nm);
        const bool ok = nm.endsWith(QLatin1String(".lz4")) ? appleLz4(in, raw) : lzo1x(in, raw);
        if(!ok){ if(stats) ++stats->badTiles; continue; }
        any = true;
        const uchar* s = reinterpret_cast<const uchar*>(raw.constData());
        const int ox = col * c.tile - X, oy = row * c.tile - Y;
        for(int y = 0; y < th; ++y){
            QRgb* d = reinterpret_cast<QRgb*>(img.scanLine(oy + y)) + ox;
            for(int x = 0; x < tw; ++x){
                const qint64 i = qint64(y) * tw + x;
                if(isMask){ const int v = s[i]; d[x] = qRgba(v, v, v, v); }
                else d[x] = qRgba(s[i * 4], s[i * 4 + 1], s[i * 4 + 2], s[i * 4 + 3]);
            }
        }
    }
    return any;
}
void multiplyAlpha(QImage& img, int X, int Y, const QImage& mask, int mx, int my){
    for(int y = 0; y < img.height(); ++y){
        QRgb* d = reinterpret_cast<QRgb*>(img.scanLine(y));
        for(int x = 0; x < img.width(); ++x){
            const int u = X + x - mx, v = Y + y - my;
            const int a = (u >= 0 && v >= 0 && u < mask.width() && v < mask.height()) ? qAlpha(mask.pixel(u, v)) : 0;
            d[x] = qRgba(qRed(d[x]), qGreen(d[x]), qBlue(d[x]), qAlpha(d[x]) * a / 255);
        }
    }
}
struct Builder {
    KeyedArchive& a; const Canvas& c; ProcreateImportStats* st; QSet<int> seen;
    QTransform view;      // stored canvas → what the artist sees
    // Procreate lists layers top first; the document wants the bottom first.
    QVector<Shape> list(const QVector<int>& uids, int depth){
        QVector<Shape> out;
        QImage base; int bx = 0, by = 0; bool haveBase = false;
        for(int k = uids.size() - 1; k >= 0 && depth < 64; --k){
            const int uid = uids[k];
            if(seen.contains(uid)) continue;
            seen.insert(uid);
            const PObj& o = a.at(uid);
            const PObj cls = a.field(o, "$class");
            const QString className = a.str(cls, "$classname");
            Shape s; bool ok = false;
            if(className == QLatin1String("SilicaGroup")){
                const QVector<Shape> kids = list(a.arrayUids(a.field(o, "children")), depth + 1);
                if(!kids.isEmpty()){
                    s.type = ShapeType::Group; s.name = a.str(o, "name"); s.children = kids; s.visible = !a.flag(o, "isHidden");
                    ok = true;
                    if(st) ++st->groups;
                }
            } else {
                QImage img; int X = 0, Y = 0;
                const QString uuid = a.str(o, "UUID");
                if(layerImage(c, uuid, false, img, X, Y, st)){
                    const PObj mask = a.field(o, "mask");
                    if(mask.t == PObj::Dict){
                        QImage mi; int mx = 0, my = 0;
                        if(layerImage(c, a.str(mask, "UUID"), true, mi, mx, my, st)){
                            if(!a.flag(mask, "hidden")){ multiplyAlpha(img, X, Y, mi, mx, my); if(st) ++st->masks; }
                        } else if(!a.flag(mask, "hidden") && c.tilesByLayer.contains(a.str(mask, "UUID")) == false){
                            img.fill(Qt::transparent);    // a mask with no tiles has painted nothing, so hides everything
                        }
                    }
                    const bool clipped = a.flag(o, "clipped");
                    if(clipped && haveBase){ multiplyAlpha(img, X, Y, base, bx, by); if(st) ++st->clipped; }
                    else if(!clipped){ base = img; bx = X; by = Y; haveBase = true; }
                    // Bake the view into the pixels, so a layer stays an upright, unmirrored bitmap to edit.
                    const QRectF placed = view.mapRect(QRectF(X, Y, img.width(), img.height()));
                    const QTransform linear(view.m11(), view.m12(), view.m21(), view.m22(), 0, 0);
                    s = psdBitmapShape((linear.isIdentity() ? img : img.transformed(linear)).convertToFormat(QImage::Format_ARGB32), qRound(placed.left()), qRound(placed.top()), a.str(o, "name"));
                    s.opacity = qBound(0.0, a.num(o, "opacity", 1), 1.0);
                    s.visible = !a.flag(o, "hidden");
                    const PObj ext = a.field(o, "extendedBlend");
                    const qint64 mode = (ext.t == PObj::Int) ? ext.i : qint64(a.num(o, "blend"));
                    psdApplyBlend(s, blendKey(mode));
                    ok = true;
                    if(st) ++st->layers;
                }
            }
            if(ok) out.push_back(s);
        }
        return out;
    }
};

} // namespace

Document importProcreateDocument(const QMap<QString,QByteArray>& zip, ProcreateImportStats* st){
    Document doc;
    const QByteArray arc = zip.value(QStringLiteral("Document.archive"));
    if(arc.isEmpty()) return doc;
    KeyedArchive a; int root = 0;
    if(!a.open(arc, &root)) return doc;
    const PObj r = a.at(root);
    if(r.t != PObj::Dict) return doc;
    const QRegularExpressionMatch sm = QRegularExpression(QStringLiteral("\\{\\s*(\\d+)\\s*,\\s*(\\d+)\\s*\\}")).match(a.str(r, "size"));
    if(!sm.hasMatch()) return doc;
    Canvas c;
    c.W = sm.captured(1).toInt(); c.H = sm.captured(2).toInt(); c.tile = int(a.num(r, "tileSize"));
    if(c.W <= 0 || c.H <= 0 || c.tile <= 0 || qint64(c.W) * c.H > (qint64(1) << 28)) return doc;
    c.cols = (c.W + c.tile - 1) / c.tile; c.rows = (c.H + c.tile - 1) / c.tile;
    c.zip = &zip;
    for(auto it = zip.begin(); it != zip.end(); ++it){
        const QString& nm = it.key();
        if(!(nm.endsWith(QLatin1String(".chunk")) || nm.endsWith(QLatin1String(".lz4")))) continue;
        const int slash = nm.indexOf(QLatin1Char('/'));
        if(slash > 0) c.tilesByLayer[nm.left(slash)].push_back(nm);
    }
    // Tiles run bottom-up, and the canvas may have been turned or flipped since it was started.
    const double W = c.W, H = c.H;
    QTransform flip;
    if(a.flag(r, "flippedHorizontally")) flip = flip * QTransform(-1, 0, 0, 1, W, 0);
    if(a.flag(r, "flippedVertically")) flip = flip * QTransform(1, 0, 0, -1, 0, H);
    QTransform turn(1, 0, 0, -1, 0, H);                   // 1: as drawn
    const qint64 orientation = qint64(a.num(r, "orientation", 1));
    if(orientation == 2) turn = QTransform(-1, 0, 0, 1, W, 0);
    else if(orientation == 3) turn = QTransform(0, -1, -1, 0, H, W);
    else if(orientation == 4) turn = QTransform(0, 1, 1, 0, 0, 0);
    Builder b{ a, c, st, {}, flip * turn };
    const QSizeF page = (orientation == 3 || orientation == 4) ? QSizeF(H, W) : QSizeF(W, H);
    QVector<Shape> layers = b.list(a.arrayUids(a.field(r, "unwrappedLayers")), 0);
    // The background is a setting, not a layer.
    const QByteArray bg = a.field(r, "backgroundColor").b;
    if(bg.size() >= 16 && !a.flag(r, "backgroundHidden")){
        float v[4]; std::memcpy(v, bg.constData(), 16);
        QImage solid(int(page.width()), int(page.height()), QImage::Format_ARGB32);
        if(!solid.isNull()){
            solid.fill(QColor::fromRgbF(qBound(0.f, v[0], 1.f), qBound(0.f, v[1], 1.f), qBound(0.f, v[2], 1.f)).rgb());
            layers.prepend(psdBitmapShape(solid, 0, 0, QStringLiteral("Background")));
        }
    }
    doc.pageSize = page;
    doc.shapes = layers;
    return doc;
}
} // namespace xara
