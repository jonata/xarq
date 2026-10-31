#include "svgexport.h"
#include <QBuffer>
#include <QFontMetricsF>
#include <QRegularExpression>
#include <QtMath>
#include <functional>

namespace xara {
namespace {

// compact number: fixed 3dp, trailing zeros stripped
QString n(double v) {
    if (!std::isfinite(v)) v = 0;
    QString s = QString::number(v, 'f', 3);
    if (s.contains('.')) { while (s.endsWith('0')) s.chop(1); if (s.endsWith('.')) s.chop(1); }
    return s == "-0" ? "0" : s;
}
QString col(const QColor& c) {
    return QString("#%1%2%3").arg(c.red(),2,16,QChar('0')).arg(c.green(),2,16,QChar('0')).arg(c.blue(),2,16,QChar('0'));
}
QString esc(QString s) {
    s.replace('&',"&amp;").replace('<',"&lt;").replace('>',"&gt;").replace('"',"&quot;");
    static const QRegularExpression ctl("[\\x00-\\x08\\x0B\\x0C\\x0E-\\x1F]");   // XML 1.0 forbids C0 controls (bar tab/LF/CR)
    return s.remove(ctl);
}
QString xf(const QTransform& t) {   // " transform=matrix(...)" (omitted when identity / non-affine)
    if (t.isIdentity()) return QString();
    return QString(" transform=\"matrix(%1,%2,%3,%4,%5,%6)\"")
        .arg(n(t.m11()),n(t.m12()),n(t.m21()),n(t.m22()),n(t.dx()),n(t.dy()));
}
double fontSize(const QFont& f) { return f.pixelSize()>0 ? f.pixelSize() : (f.pointSizeF()>0 ? f.pointSizeF() : 16.0); }

// QPainterPath -> SVG path data (M / L / C, cubic beziers; subpath-back-lines reproduce closure)
QString pathData(const QPainterPath& p) {
    QString d;
    for (int i=0; i<p.elementCount(); ) {
        const auto e = p.elementAt(i);
        if (e.type == QPainterPath::MoveToElement) { d += "M"+n(e.x)+" "+n(e.y)+" "; ++i; }
        else if (e.type == QPainterPath::LineToElement) { d += "L"+n(e.x)+" "+n(e.y)+" "; ++i; }
        else if (e.type == QPainterPath::CurveToElement && i+2 < p.elementCount()) {
            const auto c2=p.elementAt(i+1), ep=p.elementAt(i+2);
            d += "C"+n(e.x)+" "+n(e.y)+" "+n(c2.x)+" "+n(c2.y)+" "+n(ep.x)+" "+n(ep.y)+" "; i+=3;
        } else ++i;
    }
    return d.trimmed();
}

QString spreadAttr(QGradient::Spread s) {
    return s==QGradient::RepeatSpread ? " spreadMethod=\"repeat\""
         : s==QGradient::ReflectSpread ? " spreadMethod=\"reflect\"" : QString();
}
QString stopsFor(const Fill& f) {
    QVector<GradStop> st = f.stops;
    if ((f.type==FillType::Mesh || f.type==FillType::Square) && !f.meshColors.isEmpty()) {   // approximate mesh/square as a ramp
        st.clear(); const int nc=f.meshColors.size();
        for (int i=0;i<nc;++i) st.push_back({ nc>1 ? double(i)/(nc-1) : 0.0, f.meshColors[i] });
    }
    if (st.isEmpty()) st.push_back({0.0, f.color});
    QString o;
    for (const GradStop& s : st) { o += "<stop offset=\""+n(qBound(0.0,s.pos,1.0))+"\" stop-color=\""+col(s.color)+"\"";
        if (s.color.alpha()<255) o += " stop-opacity=\""+n(s.color.alphaF())+"\""; o += "/>"; }
    return o;
}

// Fill -> paint attributes (appending any gradient to `defs`).
QString fillAttr(const Fill& f, QString& defs, int& gid) {
    auto solid = [&](const QColor& c)->QString {
        QString a = " fill=\""+col(c)+"\""; if (c.alpha()<255) a += " fill-opacity=\""+n(c.alphaF())+"\""; return a; };
    switch (f.type) {
        case FillType::None: return " fill=\"none\"";
        case FillType::Solid: return solid(f.color);
        case FillType::Linear: case FillType::Conical: case FillType::Mesh: {
            const int id = ++gid;
            defs += "<linearGradient id=\"g"+QString::number(id)+"\" gradientUnits=\"userSpaceOnUse\" x1=\""
                 +n(f.p0.x())+"\" y1=\""+n(f.p0.y())+"\" x2=\""+n(f.p1.x())+"\" y2=\""+n(f.p1.y())+"\""
                 +spreadAttr(f.spread)+">"+stopsFor(f)+"</linearGradient>";
            return " fill=\"url(#g"+QString::number(id)+")\"";
        }
        case FillType::Radial: case FillType::Square: {
            const int id = ++gid;
            const double r  = std::hypot(f.p1.x()-f.p0.x(), f.p1.y()-f.p0.y());
            const double r2 = std::hypot(f.p2.x()-f.p0.x(), f.p2.y()-f.p0.y());
            QString g = "<radialGradient id=\"g"+QString::number(id)+"\" gradientUnits=\"userSpaceOnUse\" cx=\""
                 +n(f.p0.x())+"\" cy=\""+n(f.p0.y())+"\" r=\""+n(r>1e-6?r:1)+"\"";
            if (r>1e-6 && qAbs(r2-r) > 0.01*r) {   // anisotropic radial: scale Y about the centre
                const double sy=r2/r; g += " gradientTransform=\"matrix(1,0,0,"+n(sy)+",0,"+n(f.p0.y()*(1-sy))+")\""; }
            g += spreadAttr(f.spread)+">"+stopsFor(f)+"</radialGradient>"; defs += g;
            return " fill=\"url(#g"+QString::number(id)+")\"";
        }
        case FillType::Bitmap: return solid(f.representativeColor());   // placed photos handled at shape level; this is the path-fill fallback
    }
    return solid(f.representativeColor());
}

QString strokeAttr(const Stroke& s) {
    if (!s.enabled) return QString();
    QString a = " stroke=\""+col(s.color)+"\" stroke-width=\""+n(qMax(0.0,s.width))+"\"";   // SVG forbids negative width
    const double so = (1.0-qBound(0.0,s.transp,1.0))*s.color.alphaF();
    if (so < 1.0) a += " stroke-opacity=\""+n(qBound(0.0,so,1.0))+"\"";
    if (s.cap  != Qt::FlatCap)  a += " stroke-linecap=\""+QString(s.cap==Qt::RoundCap?"round":"square")+"\"";
    if (s.join != Qt::MiterJoin) a += " stroke-linejoin=\""+QString(s.join==Qt::RoundJoin?"round":"bevel")+"\"";
    if (!s.dashes.isEmpty()) { QStringList d; for (qreal v : s.dashes) d << n(qMax(0.0, v*s.width));   // pen-width units, clamped >=0
        a += " stroke-dasharray=\""+d.join(",")+"\""; }
    return a;
}

// Effective object opacity: UI opacity folded with a FLAT rich-transparency (graduated transp is not expressible here).
QString opacityAttr(const Shape& s) {
    double o = s.opacity;
    if (s.transp.type != 0 && s.transp.geom == 0) o *= (1.0 - qBound(0.0, s.transp.t0, 1.0));
    return o < 0.999 ? " opacity=\""+n(qBound(0.0,o,1.0))+"\"" : QString();
}

// Live effects (drop shadow / feather soft-edge) → an SVG <filter>; returns the filter="url(#…)" attr (or empty).
QString effectFilter(const Shape& s, QString& defs, int& fid) {
    const bool sh = s.shadow.on, fe = s.feather > 0.1, bl = s.blur > 0.1;
    if (!sh && !fe && !bl) return QString();
    const int id = ++fid;
    QString f = "<filter id=\"fx"+QString::number(id)+"\" x=\"-60%\" y=\"-60%\" width=\"220%\" height=\"220%\">";
    QString graphic = "SourceGraphic", shadowSrc = "SourceAlpha";
    if (bl) {                                             // live blur: soft-focus the whole graphic (colour + alpha)
        f += "<feGaussianBlur in=\"SourceGraphic\" stdDeviation=\""+n(qMax(0.1,s.blur*0.5))+"\" result=\"blr\"/>";
        graphic = "blr"; shadowSrc = "blr";
    } else if (fe) {                                      // feather: soften the ALPHA edge, keep colour sharp
        f += "<feGaussianBlur in=\"SourceAlpha\" stdDeviation=\""+n(qMax(0.1,s.feather*0.5))+"\" result=\"fa\"/>";
        f += "<feComposite in=\"SourceGraphic\" in2=\"fa\" operator=\"in\" result=\"feathered\"/>";
        graphic = "feathered"; shadowSrc = "fa";
    }
    if (sh) {                                             // drop shadow: blurred, offset, tinted alpha behind the graphic
        f += "<feGaussianBlur in=\""+shadowSrc+"\" stdDeviation=\""+n(qMax(0.1,s.shadow.blur*0.4))+"\" result=\"sb\"/>";
        f += "<feOffset in=\"sb\" dx=\""+n(s.shadow.offset.x())+"\" dy=\""+n(s.shadow.offset.y())+"\" result=\"so\"/>";
        f += "<feFlood flood-color=\""+col(s.shadow.color)+"\" flood-opacity=\""+n(s.shadow.color.alphaF())+"\" result=\"sc\"/>";
        f += "<feComposite in=\"sc\" in2=\"so\" operator=\"in\" result=\"shadow\"/>";
        f += "<feMerge><feMergeNode in=\"shadow\"/><feMergeNode in=\""+graphic+"\"/></feMerge>";
    }
    f += "</filter>"; defs += f;
    return " filter=\"url(#fx"+QString::number(id)+")\"";
}

} // namespace

QByteArray writeSvg(const QVector<Shape>& shapes, QRectF vb) {
    QString defs, body; int gid=0, fid=0;
    if (vb.width()<=0 || vb.height()<=0) vb = QRectF(0,0,1123,794);
    const QString W = n(vb.width()), H = n(vb.height());

    std::function<void(const Shape&, int)> writeOne = [&](const Shape& s, int depth) {
        if (!s.visible) return;
        const QString ind(depth*2, ' ');

        if (s.type == ShapeType::Group) {
            body += ind+"<g"+opacityAttr(s)+effectFilter(s,defs,fid)+xf(s.transform)+">\n";
            for (const Shape& c : s.children) writeOne(c, depth+1);
            body += ind+"</g>\n";
            return;
        }
        // Placed photo: a bitmap fill becomes an embedded <image> mapped onto its parallelogram.
        if (s.fill.type == FillType::Bitmap && !s.fill.image.isNull()) {
            const QImage& img = s.fill.image; const double iw=img.width(), ih=img.height();
            QByteArray png; QBuffer buf(&png); buf.open(QIODevice::WriteOnly);
            if (!img.save(&buf, "PNG") || png.isEmpty()) return;   // encoder failed: skip rather than emit a broken data URI
            const Fill& f = s.fill;   // image px -> local: (0,0)->p0, (iw,0)->p1, (0,ih)->p2
            const QTransform it((f.p1.x()-f.p0.x())/qMax(1.0,iw), (f.p1.y()-f.p0.y())/qMax(1.0,iw),
                                (f.p2.x()-f.p0.x())/qMax(1.0,ih), (f.p2.y()-f.p0.y())/qMax(1.0,ih), f.p0.x(), f.p0.y());
            body += ind+"<image width=\""+n(iw)+"\" height=\""+n(ih)+"\""+opacityAttr(s)+effectFilter(s,defs,fid)+xf(it*s.transform)
                 +" xlink:href=\"data:image/png;base64,"+QString::fromLatin1(png.toBase64())+"\"/>\n";
            return;
        }
        // Projective (moulded, quadToQuad) transform: SVG's matrix() is AFFINE-only, so bake the
        // perspective warp into the geometry — emit the warped path with no transform attribute.
        if (!s.transform.isAffine()) {
            const QString d = pathData(s.transform.map(s.localPath()));
            if (!d.isEmpty()) {
                Fill bf = s.fill;   // gradient geometry lived in local space; move it to the (now baked) doc space
                bf.p0=s.transform.map(s.fill.p0); bf.p1=s.transform.map(s.fill.p1); bf.p2=s.transform.map(s.fill.p2);
                body += ind+"<path d=\""+d+"\" fill-rule=\""+(s.fillRule==Qt::WindingFill?"nonzero":"evenodd")+"\""
                     +fillAttr(bf,defs,gid)+strokeAttr(s.stroke)+opacityAttr(s)+effectFilter(s,defs,fid)+"/>\n";
            }
            return;
        }

        const QString fa = fillAttr(s.fill, defs, gid), sa = strokeAttr(s.stroke), op = opacityAttr(s)+effectFilter(s,defs,fid), tr = xf(s.transform);

        if (s.type == ShapeType::Rect) {
            body += ind+"<rect x=\""+n(s.rect.x())+"\" y=\""+n(s.rect.y())+"\" width=\""+n(s.rect.width())
                 +"\" height=\""+n(s.rect.height())+"\"";
            if (s.cornerRadius>0) body += " rx=\""+n(s.cornerRadius)+"\"";
            body += fa+sa+op+tr+"/>\n";
        } else if (s.type == ShapeType::Ellipse) {
            body += ind+"<ellipse cx=\""+n(s.rect.center().x())+"\" cy=\""+n(s.rect.center().y())+"\" rx=\""
                 +n(s.rect.width()/2)+"\" ry=\""+n(s.rect.height()/2)+"\""+fa+sa+op+tr+"/>\n";
        } else if (s.type == ShapeType::Text && !s.textRuns.isEmpty() && s.textPath.isEmpty() && !s.text.contains('\n')) {
            // Rich single-line text (T4): <text> with a <tspan> per run (colour/font), links wrapped in <a>.
            QVector<QFont> fonts; QVector<QColor> cols; QVector<QString> urls; s.resolveChars(fonts,cols,urls);
            const QVector<double> bl=s.resolveBaselines();
            double maxAsc=0; for(const QFont& fo: fonts) maxAsc=qMax(maxAsc, QFontMetricsF(fo).ascent());
            if(maxAsc<=0) maxAsc=QFontMetricsF(s.font).ascent();
            body += ind+"<text x=\"0\" y=\""+n(maxAsc)+"\""+op+tr+">";
            const int n2=s.text.size(); int i=0;
            while(i<n2){ int j=i+1; while(j<n2 && fonts[j]==fonts[i] && cols[j]==cols[i] && urls[j]==urls[i] && bl[j]==bl[i]) ++j;
                QString span="<tspan fill=\""+cols[i].name()+"\" font-family=\""+esc(fonts[i].family())+"\" font-size=\""+n(fontSize(fonts[i]))+"\"";
                if(fonts[i].weight()!=QFont::Normal) span+=" font-weight=\""+QString::number(fonts[i].weight())+"\"";   // numeric weight preserves Heavy/Black (900), not just Bold
                if(fonts[i].italic()) span+=" font-style=\"italic\"";
                if(bl[i]!=0.0) span+=" baseline-shift=\""+n(bl[i])+"\"";       // per-run super/subscript
                if(fonts[i].letterSpacingType()==QFont::AbsoluteSpacing && fonts[i].letterSpacing()!=0.0) span+=" letter-spacing=\""+n(fonts[i].letterSpacing())+"\"";
                span+=">"+esc(s.text.mid(i,j-i))+"</tspan>";
                if(!urls[i].isEmpty()){ QString u=urls[i].trimmed(); QString lo=u; lo.remove(QRegularExpression("[\\x00-\\x20]")); lo=lo.toLower();
                    if(lo.startsWith("javascript:")||lo.startsWith("vbscript:")||lo.startsWith("data:")||lo.startsWith("file:")) u="#";
                    span="<a href=\""+esc(u)+"\">"+span+"</a>"; }
                body+=span; i=j; }
            body += "</text>\n";
        } else if (s.type == ShapeType::Text && s.textPath.isEmpty() && !s.textFrame && !s.smallCaps && !s.text.contains('\n')) {
            QFontMetricsF fm(s.font);           // simple single-line point text stays editable <text>
            body += ind+"<text x=\"0\" y=\""+n(fm.ascent())+"\" font-family=\""+esc(s.font.family())
                 +"\" font-size=\""+n(fontSize(s.font))+"\"";
            if (s.font.weight()!=QFont::Normal) body += " font-weight=\""+QString::number(s.font.weight())+"\"";
            if (s.font.italic()) body += " font-style=\"italic\"";
            body += fa+op+tr+">"+esc(s.text)+"</text>\n";
        } else {                                 // Path / Polygon / Smart / complex Text -> outlined <path>
            const QString d = pathData(s.localPath());
            if (d.isEmpty()) return;
            body += ind+"<path d=\""+d+"\" fill-rule=\""+(s.fillRule==Qt::WindingFill?"nonzero":"evenodd")+"\""+fa+sa+op+tr+"/>\n";
        }
    };
    for (const Shape& s : shapes) writeOne(s, 1);

    QString svg = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" "
        "width=\""+W+"\" height=\""+H+"\" viewBox=\""+n(vb.x())+" "+n(vb.y())+" "+W+" "+H+"\">\n";
    if (!defs.isEmpty()) svg += "<defs>"+defs+"</defs>\n";
    svg += body + "</svg>\n";
    return svg.toUtf8();
}

} // namespace xara
