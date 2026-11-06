// OpenRaster (.ora): a ZIP whose stack.xml nests <stack> and <layer> elements, topmost first. A layer is a
// PNG placed at x, y; layers and stacks have opacity, visibility and a composite-op, and a stack is
// composited on its own when its isolation says so or when its opacity or blend need it.
#include "prepress.h"
#include <QHash>
#include <QImage>
#include <QXmlStreamReader>
#include <functional>

namespace xara {
namespace {

// SVG compositing names (and Krita's and MyPaint's own) as the PSD route's blend keys; empty for no match.
QByteArray opKey(const QString& op){
    static const QHash<QString, QByteArray> m = {
        {"svg:src-over","norm"},{"svg:multiply","mul "},{"svg:screen","scrn"},{"svg:overlay","over"},{"svg:darken","dark"},
        {"svg:lighten","lite"},{"svg:color-dodge","div "},{"svg:color-burn","idiv"},{"svg:hard-light","hLit"},
        {"svg:soft-light","sLit"},{"svg:difference","diff"},{"svg:exclusion","smud"},{"svg:hue","hue "},
        {"svg:saturation","sat "},{"svg:color","colr"},{"svg:luminosity","lum "},{"svg:plus","lddg"},
        {"krita:linear_burn","lbrn"},{"krita:vivid_light","vLit"},{"krita:pin_light","pLit"},{"krita:linear_light","lLit"},
        {"krita:hard_mix","hMix"},{"krita:divide","fdiv"},{"krita:subtract","fsub"} };
    return op.isEmpty() ? QByteArrayLiteral("norm") : m.value(op);
}
bool porterDuff(const QString& op, QPainter::CompositionMode* mode){
    if(op == QLatin1String("svg:dst-out")){ *mode = QPainter::CompositionMode_DestinationOut; return true; }
    if(op == QLatin1String("svg:dst-in")){ *mode = QPainter::CompositionMode_DestinationIn; return true; }
    if(op == QLatin1String("svg:src-atop")){ *mode = QPainter::CompositionMode_SourceAtop; return true; }
    if(op == QLatin1String("svg:dst-atop")){ *mode = QPainter::CompositionMode_DestinationAtop; return true; }
    return false;
}

struct Ctx { const QMap<QString, QByteArray>& zip; OraImportStats* st; qint64 pixels = 0; int W = 0, H = 0; };

QByteArray entry(const Ctx& c, const QString& src){
    auto it = c.zip.find(src);
    if(it != c.zip.end()) return it.value();
    for(auto i = c.zip.begin(); i != c.zip.end(); ++i) if(i.key().compare(src, Qt::CaseInsensitive) == 0) return i.value();
    return QByteArray();
}

void applyCommon(Shape& s, const QXmlStreamAttributes& a, Ctx& c){
    bool ok = false; const double op = a.value("opacity").toDouble(&ok);
    s.opacity = (ok && qIsFinite(op)) ? qBound(0.0, op, 1.0) : 1.0;
    s.visible = a.value("visibility") != QLatin1String("hidden");
    const QString cop = a.value("composite-op").toString();
    QPainter::CompositionMode pd;
    if(porterDuff(cop, &pd)){ s.blend = pd; return; }
    QByteArray key = opKey(cop);
    if(key.isEmpty()){ key = "norm"; if(c.st) ++c.st->unknownModes; }
    psdApplyBlend(s, key);
}

// The children of a <stack>, topmost first in the file, returned bottom first.
QVector<Shape> readStack(QXmlStreamReader& r, Ctx& c, int depth){
    QVector<Shape> out;
    while(r.readNextStartElement()){
        const QXmlStreamAttributes a = r.attributes();
        if(r.name() == QLatin1String("stack")){
            Shape g; g.type = ShapeType::Group; g.name = a.value("name").toString();
            if(g.name.isEmpty()) g.name = QStringLiteral("Group");
            g.children = depth < 64 ? readStack(r, c, depth + 1) : (r.skipCurrentElement(), QVector<Shape>());
            applyCommon(g, a, c);
            const QString cop = a.value("composite-op").toString();
            g.isolate = a.value("isolation") == QLatin1String("isolate") || g.opacity < 0.999 || (!cop.isEmpty() && cop != QLatin1String("svg:src-over"));
            if(c.st) ++c.st->groups;
            out.prepend(g);
        } else if(r.name() == QLatin1String("layer")){
            const QString src = a.value("src").toString();
            r.skipCurrentElement();
            QImage img; const QByteArray png = src.isEmpty() ? QByteArray() : entry(c, src);
            if(png.isEmpty() || !img.loadFromData(png) || img.isNull()){ if(c.st) ++c.st->missing; continue; }
            c.pixels += qint64(img.width()) * img.height();
            if(c.pixels > (qint64(1) << 28)){ if(c.st) ++c.st->missing; continue; }   // a budget for many huge layers
            bool okx = false, oky = false;
            const double x = a.value("x").toDouble(&okx), y = a.value("y").toDouble(&oky);
            const int ix = (okx && qIsFinite(x)) ? int(qBound(-1e7, x, 1e7)) : 0, iy = (oky && qIsFinite(y)) ? int(qBound(-1e7, y, 1e7)) : 0;
            QString name = a.value("name").toString(); if(name.isEmpty()) name = QStringLiteral("Layer");
            Shape s = psdBitmapShape(img.convertToFormat(QImage::Format_ARGB32), ix, iy, name);
            // MyPaint's background is one tile, repeated over the whole canvas.
            if((a.hasAttribute("background_tile") || a.hasAttribute("mypaint:background-tile")) && (img.width() < c.W || img.height() < c.H)){
                s.rect = QRectF(0, 0, c.W, c.H); s.fill.tiled = true; s.transform = QTransform();
            }
            applyCommon(s, a, c);
            if(c.st) ++c.st->layers;
            out.prepend(s);
        } else r.skipCurrentElement();
    }
    return out;
}

} // namespace

Document importOraDocument(const QMap<QString, QByteArray>& zip, OraImportStats* st){
    Document doc;
    Ctx c{ zip, st };
    const QByteArray xml = entry(c, QStringLiteral("stack.xml"));
    if(xml.isEmpty()) return doc;
    QXmlStreamReader r(xml);
    if(!r.readNextStartElement() || r.name() != QLatin1String("image")) return doc;
    bool okw = false, okh = false;
    const double w = r.attributes().value("w").toDouble(&okw), h = r.attributes().value("h").toDouble(&okh);
    if(!okw || !okh || !qIsFinite(w) || !qIsFinite(h) || w < 1 || h < 1 || w > 1e6 || h > 1e6 || w * h > double(qint64(1) << 28)) return doc;
    doc.pageSize = QSizeF(w, h); c.W = int(w); c.H = int(h);
    while(r.readNextStartElement()){
        if(r.name() != QLatin1String("stack")){ r.skipCurrentElement(); continue; }
        // The root stack holds the picture; its own settings, rarely anything but the defaults, wrap it.
        const QXmlStreamAttributes a = r.attributes();
        QVector<Shape> kids = readStack(r, c, 0);
        Shape root; root.children = kids; applyCommon(root, a, c);
        if(root.opacity < 0.999 || !root.visible || root.blend != QPainter::CompositionMode_SourceOver){
            root.type = ShapeType::Group; root.name = a.value("name").toString(); root.isolate = true;
            doc.shapes = { root };
        } else doc.shapes = kids;
        break;
    }
    return doc;
}
} // namespace xara
