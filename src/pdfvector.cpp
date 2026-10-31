#include "pdfvector.h"

#ifndef HAVE_POPPLER
namespace xara {
bool pdfVectorAvailable(){ return false; }
QStringList pdfToSvgPages(const QString&, PdfVectorStats*){ return {}; }
}
#else

#include <QBuffer>
#include <QFontDatabase>
#include <QHash>
#include <QImage>
#include <QPainterPath>
#include <QSet>
#include <cmath>
#include <memory>

#include <GfxFont.h>
#include <GfxState.h>
#include <GlobalParams.h>
#include <PDFDoc.h>
#include <Stream.h>
#include <SplashOutputDev.h>
#include <goo/GooString.h>
#include <splash/SplashFont.h>
#include <splash/SplashPath.h>
#include <splash/SplashTypes.h>

namespace xara {
namespace {

constexpr qsizetype kMaxPageBytes = 256 * 1024 * 1024;      // one page's SVG
constexpr qint64    kMaxImageBytes = 512 * 1024 * 1024;     // decoded PNG across the document
constexpr qint64    kMaxImagePixels = 200 * 1000 * 1000;    // one image
constexpr int       kMaxPages = 5000;

QString num(double v){
    if(!std::isfinite(v)) v = 0.0;
    QString t = QString::number(v, 'f', 3);
    int e = t.size();
    while(e > 0 && t[e-1] == QLatin1Char('0')) --e;
    if(e > 0 && t[e-1] == QLatin1Char('.')) --e;
    t.truncate(e);
    return t.isEmpty() || t == QLatin1String("-") ? QStringLiteral("0") : t;
}
QString esc(QString s){
    s.replace(QLatin1Char('&'), QLatin1String("&amp;")); s.replace(QLatin1Char('<'), QLatin1String("&lt;"));
    s.replace(QLatin1Char('>'), QLatin1String("&gt;")); s.replace(QLatin1Char('"'), QLatin1String("&quot;"));
    return s;
}
QString pathData(const QPainterPath& p){
    QString d;
    d.reserve(p.elementCount() * 16);
    for(int i = 0; i < p.elementCount(); ++i){
        const QPainterPath::Element e = p.elementAt(i);
        if(e.type == QPainterPath::MoveToElement) d += QLatin1Char('M') + num(e.x) + QLatin1Char(' ') + num(e.y);
        else if(e.type == QPainterPath::LineToElement) d += QLatin1Char('L') + num(e.x) + QLatin1Char(' ') + num(e.y);
        else if(e.type == QPainterPath::CurveToElement && i + 2 < p.elementCount()){
            const QPainterPath::Element c = p.elementAt(i + 1), f = p.elementAt(i + 2);
            d += QLatin1Char('C') + num(e.x) + QLatin1Char(' ') + num(e.y) + QLatin1Char(' ') + num(c.x) + QLatin1Char(' ') + num(c.y)
               + QLatin1Char(' ') + num(f.x) + QLatin1Char(' ') + num(f.y);
            i += 2;
        }
    }
    return d;
}
QString hexOf(const GfxRGB& c){
    auto b = [](GfxColorComp v){ return qBound(0, int(colToDbl(v) * 255.0 + 0.5), 255); };
    return QColor(b(c.r), b(c.g), b(c.b)).name(QColor::HexRgb);
}
const char* blendName(GfxBlendMode m){
    switch(m){
        case gfxBlendMultiply: return "multiply";     case gfxBlendScreen: return "screen";
        case gfxBlendOverlay: return "overlay";       case gfxBlendDarken: return "darken";
        case gfxBlendLighten: return "lighten";       case gfxBlendColorDodge: return "color-dodge";
        case gfxBlendColorBurn: return "color-burn";  case gfxBlendHardLight: return "hard-light";
        case gfxBlendSoftLight: return "soft-light";  case gfxBlendDifference: return "difference";
        case gfxBlendExclusion: return "exclusion";   case gfxBlendHue: return "hue";
        case gfxBlendSaturation: return "saturation"; case gfxBlendColor: return "color";
        case gfxBlendLuminosity: return "luminosity"; default: return nullptr;
    }
}

// SplashOutputDev is the base only so that a glyph's outline can come from Splash's font engine, which
// already resolves every embedded font type and PDF encoding to a glyph. All painting is overridden.
class SvgDev : public SplashOutputDev {
public:
    SvgDev() : SplashOutputDev(splashModeMono1, 4, false, m_paper) {}

    QString body, defs;
    PdfVectorStats* stats = nullptr;
    qint64 imageBytes = 0;
    bool full = false;

    void beginPage(){ body.clear(); defs.clear(); m_clip = {}; m_clipStack.clear(); m_openClip.clear(); m_seq = 0; m_groupAt.clear(); m_suppress = 0; m_textClip = QPainterPath(); m_run = {}; }
    void finishPage(){ flushRun(); syncClip(QString()); while(!m_groupAt.isEmpty()) m_groupAt.removeLast(); }

    bool interpretType3Chars() override { return true; }
    bool beginType3Char(GfxState*, double, double, double, double, CharCode, const Unicode*, int) override { return false; }
    void type3D0(GfxState*, double, double) override {}
    void type3D1(GfxState*, double, double, double, double, double, double) override {}
    bool useShadedFills(int type) override { return type == 2 || type == 3; }
    bool useTilingPatternFill() override { return false; }

    void saveState(GfxState* s) override { SplashOutputDev::saveState(s); m_clipStack.push_back(m_clip); }
    void restoreState(GfxState* s) override {
        SplashOutputDev::restoreState(s);
        if(!m_clipStack.isEmpty()) m_clip = m_clipStack.takeLast();
    }
    void endTextObject(GfxState*) override {
        flushRun();
        if(!m_textClip.isEmpty()){ intersectClip(m_textClip); m_textClip = QPainterPath(); }
    }

    void stroke(GfxState* s) override { paintPath(s, false, false, true); }
    void fill(GfxState* s) override { paintPath(s, true, false, false); }
    void eoFill(GfxState* s) override { paintPath(s, true, true, false); }
    void clip(GfxState* s) override { intersectClip(toPath(s, s->getPath(), false)); }
    void eoClip(GfxState* s) override { intersectClip(toPath(s, s->getPath(), true)); }
    void clipToStrokePath(GfxState* s) override {
        QPainterPathStroker st; st.setWidth(qMax(0.1, deviceWidth(s)));
        intersectClip(st.createStroke(toPath(s, s->getPath(), false)));
    }

    void drawChar(GfxState* s, double x, double y, double dx, double dy, double ox, double oy, CharCode code, int nBytes, const Unicode* u, int uLen) override;
    bool axialShadedFill(GfxState* s, GfxAxialShading* sh, double, double) override;
    bool radialShadedFill(GfxState* s, GfxRadialShading* sh, double, double) override;
    void drawImage(GfxState* s, Object*, Stream* str, int w, int h, GfxImageColorMap* cm, bool, const int* maskColors, bool) override;
    void drawImageMask(GfxState* s, Object*, Stream* str, int w, int h, bool invert, bool, bool) override;
    void drawMaskedImage(GfxState* s, Object*, Stream* str, int w, int h, GfxImageColorMap* cm, bool, Stream* mask, int mw, int mh, bool minv, bool) override;
    void drawSoftMaskedImage(GfxState* s, Object*, Stream* str, int w, int h, GfxImageColorMap* cm, bool, Stream* mask, int mw, int mh, GfxImageColorMap* mcm, bool) override;
    void setSoftMaskFromImageMask(GfxState*, Object*, Stream*, int, int, bool, bool, double*) override {}
    void unsetSoftMaskFromImageMask(GfxState*, double*) override {}

    // A soft mask's own content renders only into the mask. A group's alpha and blend mode arrive after
    // its contents, so the wrapper goes in behind them at paint time.
    void beginTransparencyGroup(GfxState*, const std::array<double, 4>&, GfxColorSpace*, bool, bool, bool forSoftMask) override {
        flushRun();
        if(forSoftMask){ ++m_suppress; m_groupAt.push_back(-1); return; }
        syncClip(QString());
        m_groupAt.push_back(body.size());
    }
    void endTransparencyGroup(GfxState*) override {
        flushRun();
        if(!m_groupAt.isEmpty() && m_groupAt.last() < 0){ --m_suppress; m_groupAt.removeLast(); m_lastGroup = -1; return; }
        syncClip(QString());
        m_lastGroup = m_groupAt.isEmpty() ? -1 : m_groupAt.takeLast();
    }
    void paintTransparencyGroup(GfxState* s, const std::array<double, 4>&) override {
        if(m_lastGroup < 0 || m_suppress) return;
        QString a;
        if(s->getFillOpacity() < 0.999) a += QStringLiteral(" opacity=\"%1\"").arg(num(s->getFillOpacity()));
        if(const char* b = blendName(s->getBlendMode())) a += QStringLiteral(" style=\"mix-blend-mode:%1\"").arg(QLatin1String(b));
        if(a.isEmpty() && m_clip.isEmpty()) return;
        const QString id = clipIdFor();
        QString open = QStringLiteral("<g%1").arg(a);
        if(!id.isEmpty()) open += QStringLiteral(" clip-path=\"url(#%1)\"").arg(id);
        body.insert(m_lastGroup, open + QLatin1Char('>'));
        body += QStringLiteral("</g>");
        m_lastGroup = -1;
    }
    void setSoftMask(GfxState*, const std::array<double, 4>&, bool, Function*, GfxColor*) override { if(stats) ++stats->softMasks; }
    void clearSoftMask(GfxState*) override {}

private:
    struct Run {
        QString text, family, fill;
        double size = 0, x = 0, y = 0, endX = 0, endY = 0, opacity = 1, a = 1, b = 0, c = 0, d = 1;
        int weight = 400; bool italic = false;
        const char* blend = nullptr;
        bool active = false;
    };

    bool budget(){ if(body.size() + defs.size() > kMaxPageBytes) full = true; return !full; }
    static QPointF dev(GfxState* s, double x, double y){ double dx, dy; s->transform(x, y, &dx, &dy); return { dx, dy }; }
    static double deviceScale(GfxState* s){ const double* m = s->getCTM(); return std::sqrt(qAbs(m[0]*m[3] - m[1]*m[2])); }
    static double deviceWidth(GfxState* s){ return s->getLineWidth() * deviceScale(s); }

    static QPainterPath toPath(GfxState* s, const GfxPath* gp, bool eo){
        QPainterPath p;
        p.setFillRule(eo ? Qt::OddEvenFill : Qt::WindingFill);
        for(int i = 0; i < gp->getNumSubpaths(); ++i){
            const GfxSubpath* sp = gp->getSubpath(i);
            const int n = sp->getNumPoints();
            if(n < 1) continue;
            p.moveTo(dev(s, sp->getX(0), sp->getY(0)));
            for(int j = 1; j < n; ){
                if(sp->getCurve(j) && j + 2 < n){
                    p.cubicTo(dev(s, sp->getX(j), sp->getY(j)), dev(s, sp->getX(j+1), sp->getY(j+1)), dev(s, sp->getX(j+2), sp->getY(j+2)));
                    j += 3;
                } else { p.lineTo(dev(s, sp->getX(j), sp->getY(j))); ++j; }
            }
            if(sp->isClosed()) p.closeSubpath();
        }
        return p;
    }

    void intersectClip(const QPainterPath& add){
        m_clip.path = m_clip.path.isEmpty() && !m_clip.set ? add : m_clip.path.intersected(add);
        m_clip.set = true;
        m_clip.bounds = m_clip.path.boundingRect();
        m_clip.rect = m_clip.path.elementCount() <= 6 && m_clip.path.contains(m_clip.bounds.adjusted(0.01, 0.01, -0.01, -0.01));
        m_clip.id.clear();
    }
    QString clipIdFor(){
        if(!m_clip.set) return {};
        if(m_clip.id.isEmpty()){
            m_clip.id = QStringLiteral("pc%1").arg(++m_seq);
            defs += QStringLiteral("<clipPath id=\"%1\"><path d=\"%2\"/></clipPath>").arg(m_clip.id, pathData(m_clip.path));
        }
        return m_clip.id;
    }
    void syncClip(const QString& id){
        if(m_openClip == id) return;
        if(!m_openClip.isEmpty()) body += QStringLiteral("</g>");
        if(!id.isEmpty()) body += QStringLiteral("<g clip-path=\"url(#%1)\">").arg(id);
        m_openClip = id;
    }
    // A shape wholly inside a rectangular clip needs no clip at all, which keeps it plainly editable.
    bool useClipFor(const QRectF& bounds){
        if(m_clip.set && !bounds.intersects(m_clip.bounds.adjusted(-0.5, -0.5, 0.5, 0.5))) return false;    // nothing would show
        if(m_clip.set && m_clip.rect && m_clip.bounds.adjusted(-0.5, -0.5, 0.5, 0.5).contains(bounds)){ syncClip(QString()); return true; }
        syncClip(clipIdFor());
        return true;
    }

    QString blendAttr(GfxState* s) const {
        const char* b = blendName(s->getBlendMode());
        return b ? QStringLiteral(" style=\"mix-blend-mode:%1\"").arg(QLatin1String(b)) : QString();
    }
    QString fillAttrs(GfxState* s, bool eo) const {
        GfxRGB rgb; s->getFillRGB(&rgb);
        QString a = QStringLiteral(" fill=\"%1\"").arg(hexOf(rgb));
        if(s->getFillOpacity() < 0.999) a += QStringLiteral(" fill-opacity=\"%1\"").arg(num(s->getFillOpacity()));
        if(eo) a += QStringLiteral(" fill-rule=\"evenodd\"");
        return a;
    }
    QString strokeAttrs(GfxState* s) const {
        GfxRGB rgb; s->getStrokeRGB(&rgb);
        const double sc = deviceScale(s);
        QString a = QStringLiteral(" stroke=\"%1\" stroke-width=\"%2\"").arg(hexOf(rgb), num(qMax(0.1, deviceWidth(s))));
        if(s->getStrokeOpacity() < 0.999) a += QStringLiteral(" stroke-opacity=\"%1\"").arg(num(s->getStrokeOpacity()));
        if(s->getLineCap() == GfxState::LineCapRound) a += QStringLiteral(" stroke-linecap=\"round\"");
        else if(s->getLineCap() == GfxState::LineCapProjecting) a += QStringLiteral(" stroke-linecap=\"square\"");
        if(s->getLineJoin() == GfxState::LineJoinRound) a += QStringLiteral(" stroke-linejoin=\"round\"");
        else if(s->getLineJoin() == GfxState::LineJoinBevel) a += QStringLiteral(" stroke-linejoin=\"bevel\"");
        else a += QStringLiteral(" stroke-miterlimit=\"%1\"").arg(num(s->getMiterLimit()));
        double start = 0.0;
        const std::vector<double>& dash = s->getLineDash(&start);
        if(!dash.empty()){
            QStringList ds;
            bool any = false;
            for(double v : dash){ ds << num(v * sc); any = any || v > 0.0; }
            if(any) a += QStringLiteral(" stroke-dasharray=\"%1\"").arg(ds.join(QLatin1Char(' ')));
        }
        return a;
    }

    void paintPath(GfxState* s, bool doFill, bool eo, bool doStroke){
        flushRun();
        if(m_suppress || !budget()) return;
        const QPainterPath p = toPath(s, s->getPath(), eo);
        if(p.isEmpty()) return;
        QRectF b = p.boundingRect();
        if(doStroke){ const double w = deviceWidth(s); b.adjust(-w, -w, w, w); }
        if(!useClipFor(b)) return;
        QString a = doFill ? fillAttrs(s, eo) : QStringLiteral(" fill=\"none\"");
        if(doStroke) a += strokeAttrs(s);
        body += QStringLiteral("<path d=\"%1\"%2%3/>").arg(pathData(p), a, blendAttr(s));
    }

    void flushRun();
    void placeImage(GfxState* s, const QImage& img);
    QImage decode(Stream* str, int w, int h, GfxImageColorMap* cm, const int* maskColors);
    static QImage decodeMask(Stream* str, int w, int h, bool invert);
    static QImage decodeSoft(Stream* str, int w, int h, GfxImageColorMap* cm);
    bool gradientShape(GfxState* s, GfxUnivariateShading* sh, const QString& open, const QString& close, const QString& gradAttrs, bool flip);

    struct ClipState { QPainterPath path; QRectF bounds; QString id; bool set = false, rect = false; bool isEmpty() const { return !set; } };
    SplashColor m_paper = { 255, 255, 255, 255 };
    ClipState m_clip;
    QVector<ClipState> m_clipStack;
    QString m_openClip;
    int m_seq = 0, m_suppress = 0;
    QVector<qsizetype> m_groupAt;
    qsizetype m_lastGroup = -1;
    QPainterPath m_textClip;
    Run m_run;
};

// An installed family keeps text as live text. A subset's custom encoding, or a family this machine
// lacks, would come back as wrong glyphs or a different face, so those are drawn from the embedded font.
QString fontName(const GfxFont* f){ return f && f->getName() ? QString::fromLatin1(*f->getName()).section(QLatin1Char('+'), -1) : QString(); }

QString installedFamily(const GfxFont* f){
    if(!f) return {};
    // Keyed without spaces, because PDF names run the words together: TimesNewRomanPSMT, Arial-BoldMT.
    static const QHash<QString, QString> have = []{
        QHash<QString, QString> h;
        const QStringList l = QFontDatabase::families();
        for(const QString& x : l) h.insert(QString(x).remove(QLatin1Char(' ')).toLower(), x);
        return h;
    }();
    static QHash<QString, QString> cache;
    const QString name = fontName(f);
    const QString key = (f->getFamily() ? QString::fromLatin1(f->getFamily()->toStr()) : QString()) + QLatin1Char('/') + name;
    const auto it = cache.constFind(key);
    if(it != cache.constEnd()) return *it;
    QString found;
    if(f->getFamily()) found = have.value(QString::fromLatin1(f->getFamily()->toStr()).remove(QLatin1Char(' ')).toLower());
    if(found.isEmpty()){
        QString base = name.section(QLatin1Char(','), 0, 0).section(QLatin1Char('-'), 0, 0).toLower();
        if(base.endsWith(QLatin1String("psmt"))) base.chop(4);
        else if(base.endsWith(QLatin1String("mt")) || base.endsWith(QLatin1String("ps"))) base.chop(2);
        found = have.value(base);
    }
    cache.insert(key, found);
    return found;
}

void SvgDev::drawChar(GfxState* s, double x, double y, double dx, double dy, double ox, double oy, CharCode code, int nBytes, const Unicode* u, int uLen){
    const int mode = s->getRender() & 3, clipMode = s->getRender() & 4;
    if(m_suppress || (mode == 3 && !clipMode) || !budget()) return;       // mode 3 is the invisible OCR layer
    GfxRGB rgb; s->getFillRGB(&rgb);
    const QPointF pen = dev(s, x - ox, y - oy);
    const std::shared_ptr<GfxFont>& font = s->getFont();
    const double* tm = s->getTextMat();
    const double* ctm = s->getCTM();
    const double fs = s->getFontSize();
    const double hs = s->getHorizScaling();
    // Text space to device, without the translation.
    const double a = (tm[0]*ctm[0] + tm[1]*ctm[2]) * fs * hs, b = (tm[0]*ctm[1] + tm[1]*ctm[3]) * fs * hs;
    const double c = -(tm[2]*ctm[0] + tm[3]*ctm[2]) * fs, d = -(tm[2]*ctm[1] + tm[3]*ctm[3]) * fs;

    const QString family = (mode == 0 && !clipMode && uLen > 0 && font && font->getType() != fontType3) ? installedFamily(font.get()) : QString();
    if(!family.isEmpty()){
        QString ch;
        for(int i = 0; i < uLen; ++i) ch += QString::fromUcs4(reinterpret_cast<const char32_t*>(&u[i]), 1);
        const double size = std::hypot(c, d);
        const double tol = qMax(0.05, size * 0.02);
        const QString fill = hexOf(rgb);
        const bool same = m_run.active && m_run.family == family && m_run.fill == fill && qAbs(m_run.size - size) < 1e-3
            && qAbs(m_run.a - a) < 1e-3 && qAbs(m_run.b - b) < 1e-3 && qAbs(m_run.c - c) < 1e-3 && qAbs(m_run.d - d) < 1e-3
            && qAbs(m_run.opacity - s->getFillOpacity()) < 1e-3;
        const double gapX = pen.x() - m_run.endX, gapY = pen.y() - m_run.endY;
        const double adv = std::hypot(a, b) > 1e-9 ? (gapX*a + gapY*b) / std::hypot(a, b) : 0.0;
        const double off = std::hypot(a, b) > 1e-9 ? qAbs(gapX*b - gapY*a) / std::hypot(a, b) : 1e9;
        if(!(same && off < tol && adv > -tol && adv < size * 0.6)){
            flushRun();
            m_run = {};
            m_run.active = true; m_run.family = family; m_run.fill = fill; m_run.size = size;
            m_run.x = pen.x(); m_run.y = pen.y(); m_run.a = a; m_run.b = b; m_run.c = c; m_run.d = d;
            m_run.opacity = s->getFillOpacity(); m_run.blend = blendName(s->getBlendMode());
            const QString nm = fontName(font.get());
            m_run.weight = (font->isBold() || nm.contains(QLatin1String("Bold"), Qt::CaseInsensitive)) ? 700 : 400;
            m_run.italic = font->isItalic() || nm.contains(QLatin1String("Italic"), Qt::CaseInsensitive) || nm.contains(QLatin1String("Oblique"), Qt::CaseInsensitive);
        } else if(adv > size * 0.15 && !m_run.text.endsWith(QLatin1Char(' ')) && ch != QLatin1String(" ")){
            m_run.text += QLatin1Char(' ');      // a word gap made by positioning rather than a space glyph
        }
        m_run.text += ch;
        const QPointF end = dev(s, x - ox + dx, y - oy + dy);
        m_run.endX = end.x(); m_run.endY = end.y();
        return;
    }

    // Outline from the embedded font. The base call is what selects the font for this state.
    flushRun();
    SplashOutputDev::drawChar(s, x, y, dx, dy, ox, oy, code, nBytes, u, uLen);
    SplashFont* sf = getCurrentFont();
    if(!sf) return;
    std::unique_ptr<SplashPath> gp(sf->getGlyphPath(int(code)));
    if(!gp || gp->getLength() < 2) return;
    // Splash builds the outline in text space; the page's CTM still has to be applied.
    auto glyph = [&](double gx, double gy){ return QPointF(ctm[0]*gx + ctm[2]*gy, ctm[1]*gx + ctm[3]*gy); };
    QPainterPath p;
    p.setFillRule(Qt::WindingFill);
    for(int i = 0; i < gp->getLength(); ){
        double px, py; unsigned char fl;
        gp->getPoint(i, &px, &py, &fl);
        const QPointF q = pen + glyph(px, py);
        if(fl & splashPathFirst){ p.moveTo(q); ++i; continue; }
        if((fl & splashPathCurve) && i + 2 < gp->getLength()){
            double x2, y2, x3, y3; unsigned char f2, f3;
            gp->getPoint(i + 1, &x2, &y2, &f2); gp->getPoint(i + 2, &x3, &y3, &f3);
            p.cubicTo(q, pen + glyph(x2, y2), pen + glyph(x3, y3));
            i += 3;
            continue;
        }
        p.lineTo(q);
        ++i;
    }
    if(clipMode) m_textClip.addPath(p);
    if(mode == 3) return;
    QRectF bb = p.boundingRect();
    if(!useClipFor(bb)) return;
    QString at = (mode == 0 || mode == 2) ? fillAttrs(s, false) : QStringLiteral(" fill=\"none\"");
    if(mode == 1 || mode == 2) at += strokeAttrs(s);
    body += QStringLiteral("<path d=\"%1\"%2%3/>").arg(pathData(p), at, blendAttr(s));
}

void SvgDev::flushRun(){
    if(!m_run.active) return;
    Run r = m_run;
    m_run = {};
    while(r.text.endsWith(QLatin1Char(' '))) r.text.chop(1);
    if(r.text.isEmpty() || r.size < 1e-6) return;
    // Font size in the element; the rest of the text matrix (rotation, stretch) in its transform.
    const double sc = 1.0 / r.size;
    const QRectF approx(r.x, r.y - r.size, std::hypot(r.a, r.b) * r.text.size(), r.size * 1.3);
    if(!useClipFor(approx)) return;
    QString e = QStringLiteral("<text transform=\"matrix(%1 %2 %3 %4 %5 %6)\" x=\"0\" y=\"0\" font-size=\"%7\" font-family=\"%8\" fill=\"%9\"")
        .arg(num(r.a * sc), num(r.b * sc), num(r.c * sc), num(r.d * sc), num(r.x), num(r.y), num(r.size), esc(r.family), r.fill);
    if(r.weight != 400) e += QStringLiteral(" font-weight=\"%1\"").arg(r.weight);
    if(r.italic) e += QStringLiteral(" font-style=\"italic\"");
    if(r.opacity < 0.999) e += QStringLiteral(" fill-opacity=\"%1\"").arg(num(r.opacity));
    if(r.blend) e += QStringLiteral(" style=\"mix-blend-mode:%1\"").arg(QLatin1String(r.blend));
    body += e + QStringLiteral(" xml:space=\"preserve\">") + esc(r.text) + QStringLiteral("</text>");
}

// ---------------------------------------------------------------- shadings
// An unextended end paints nothing past it: stop just short, then fall to transparent.
QString stopsOf(GfxUnivariateShading* sh){
    GfxColorSpace* cs = sh->getColorSpace();
    const double t0 = sh->getDomain0(), t1 = sh->getDomain1();
    QString out;
    const int n = 32;
    for(int i = 0; i <= n; ++i){
        GfxColor col; GfxRGB rgb;
        sh->getColor(t0 + (t1 - t0) * i / n, &col);
        cs->getRGB(&col, &rgb);
        if(i == 0 && !sh->getExtend0()) out += QStringLiteral("<stop offset=\"0\" stop-color=\"%1\" stop-opacity=\"0\"/>").arg(hexOf(rgb));
        const double at = i == 0 && !sh->getExtend0() ? 0.001 : i == n && !sh->getExtend1() ? 0.999 : double(i) / n;
        out += QStringLiteral("<stop offset=\"%1\" stop-color=\"%2\"/>").arg(num(at), hexOf(rgb));
        if(i == n && !sh->getExtend1()) out += QStringLiteral("<stop offset=\"1\" stop-color=\"%1\" stop-opacity=\"0\"/>").arg(hexOf(rgb));
    }
    return out;
}

bool SvgDev::axialShadedFill(GfxState* s, GfxAxialShading* sh, double, double){
    flushRun();
    if(m_suppress || !budget()) return true;
    double x0, y0, x1, y1;
    sh->getCoords(&x0, &y0, &x1, &y1);
    const QPointF a = dev(s, x0, y0), b = dev(s, x1, y1);
    const QString id = QStringLiteral("pg%1").arg(++m_seq);
    defs += QStringLiteral("<linearGradient id=\"%1\" gradientUnits=\"userSpaceOnUse\" x1=\"%2\" y1=\"%3\" x2=\"%4\" y2=\"%5\">%6</linearGradient>")
        .arg(id, num(a.x()), num(a.y()), num(b.x()), num(b.y()), stopsOf(sh));
    // The clip is the shaded area: draw it as the shape, so the gradient lands on real geometry.
    syncClip(QString());
    if(m_clip.set && m_clip.path.isEmpty()) return true;
    QPainterPath area = m_clip.path;
    if(area.isEmpty()){ double xm, ym, xM, yM; s->getClipBBox(&xm, &ym, &xM, &yM); area.addRect(QRectF(dev(s, xm, ym), dev(s, xM, yM)).normalized()); }
    QString at = QStringLiteral(" fill=\"url(#%1)\"").arg(id);
    if(s->getFillOpacity() < 0.999) at += QStringLiteral(" fill-opacity=\"%1\"").arg(num(s->getFillOpacity()));
    body += QStringLiteral("<path d=\"%1\"%2%3/>").arg(pathData(area), at, blendAttr(s));
    return true;
}

bool SvgDev::radialShadedFill(GfxState* s, GfxRadialShading* sh, double, double){
    flushRun();
    if(m_suppress || !budget()) return true;
    double x0, y0, r0, x1, y1, r1;
    sh->getCoords(&x0, &y0, &r0, &x1, &y1, &r1);
    const QPointF a = dev(s, x0, y0), b = dev(s, x1, y1);
    const double sc = deviceScale(s);
    const QString id = QStringLiteral("pg%1").arg(++m_seq);
    defs += QStringLiteral("<radialGradient id=\"%1\" gradientUnits=\"userSpaceOnUse\" cx=\"%2\" cy=\"%3\" r=\"%4\" fx=\"%5\" fy=\"%6\">%7</radialGradient>")
        .arg(id, num(b.x()), num(b.y()), num(qMax(r0, r1) * sc), num(a.x()), num(a.y()), stopsOf(sh));
    syncClip(QString());
    if(m_clip.set && m_clip.path.isEmpty()) return true;
    QPainterPath area = m_clip.path;
    if(area.isEmpty()){ double xm, ym, xM, yM; s->getClipBBox(&xm, &ym, &xM, &yM); area.addRect(QRectF(dev(s, xm, ym), dev(s, xM, yM)).normalized()); }
    QString at = QStringLiteral(" fill=\"url(#%1)\"").arg(id);
    if(s->getFillOpacity() < 0.999) at += QStringLiteral(" fill-opacity=\"%1\"").arg(num(s->getFillOpacity()));
    body += QStringLiteral("<path d=\"%1\"%2%3/>").arg(pathData(area), at, blendAttr(s));
    return true;
}

// ---------------------------------------------------------------- images
QImage SvgDev::decode(Stream* str, int w, int h, GfxImageColorMap* cm, const int* maskColors){
    if(w < 1 || h < 1 || qint64(w) * h > kMaxImagePixels) return {};
    QImage img(w, h, QImage::Format_ARGB32);
    if(img.isNull()) return {};
    ImageStream is(str, w, cm->getNumPixelComps(), cm->getBits());
    if(!is.rewind()) return {};
    const int nc = cm->getNumPixelComps();
    for(int y = 0; y < h; ++y){
        unsigned char* p = is.getLine();
        QRgb* out = reinterpret_cast<QRgb*>(img.scanLine(y));
        if(!p){ memset(out, 0, size_t(w) * 4); continue; }
        for(int x = 0; x < w; ++x, p += nc){
            GfxRGB rgb; cm->getRGB(p, &rgb);
            int alpha = 255;
            if(maskColors){
                bool hit = true;
                for(int k = 0; k < nc; ++k) if(p[k] < maskColors[2*k] || p[k] > maskColors[2*k+1]){ hit = false; break; }
                if(hit) alpha = 0;
            }
            out[x] = qRgba(colToByte(rgb.r), colToByte(rgb.g), colToByte(rgb.b), alpha);
        }
    }
    is.close();
    return img;
}
QImage SvgDev::decodeMask(Stream* str, int w, int h, bool invert){
    if(w < 1 || h < 1 || qint64(w) * h > kMaxImagePixels) return {};
    QImage img(w, h, QImage::Format_Alpha8);
    if(img.isNull()) return {};
    ImageStream is(str, w, 1, 1);
    if(!is.rewind()) return {};
    for(int y = 0; y < h; ++y){
        unsigned char* p = is.getLine();
        uchar* out = img.scanLine(y);
        for(int x = 0; x < w; ++x) out[x] = (p && (p[x] != 0) == invert) ? 255 : 0;
    }
    is.close();
    return img;
}
QImage SvgDev::decodeSoft(Stream* str, int w, int h, GfxImageColorMap* cm){
    if(w < 1 || h < 1 || qint64(w) * h > kMaxImagePixels) return {};
    QImage img(w, h, QImage::Format_Alpha8);
    if(img.isNull()) return {};
    ImageStream is(str, w, cm->getNumPixelComps(), cm->getBits());
    if(!is.rewind()) return {};
    const int nc = cm->getNumPixelComps();
    for(int y = 0; y < h; ++y){
        unsigned char* p = is.getLine();
        uchar* out = img.scanLine(y);
        for(int x = 0; x < w; ++x){
            if(!p){ out[x] = 0; continue; }
            GfxGray g; cm->getGray(p + x * nc, &g);
            out[x] = colToByte(g);
        }
    }
    is.close();
    return img;
}

void SvgDev::placeImage(GfxState* s, const QImage& img){
    if(img.isNull() || imageBytes >= kMaxImageBytes || !budget()) return;
    const double* m = s->getCTM();
    // Blown-up pixels stay square, as they do in a PDF viewer: the canvas would smooth them.
    const int k = int(qBound(1.0, qMin(std::hypot(m[0], m[1]) / img.width(), std::hypot(m[2], m[3]) / img.height()), 16.0));
    const QImage shown = (k >= 2 && qint64(img.width()) * img.height() * k * k <= 4000000) ? img.scaled(img.size() * k, Qt::IgnoreAspectRatio, Qt::FastTransformation) : img;
    QByteArray png; QBuffer buf(&png);
    if(!buf.open(QIODevice::WriteOnly) || !shown.save(&buf, "PNG")) return;
    imageBytes += png.size();
    // PDF's image space is the unit square with its top edge at y = 1.
    const QTransform t(m[0], m[1], -m[2], -m[3], m[4] + m[2], m[5] + m[3]);
    if(!useClipFor(t.mapRect(QRectF(0, 0, 1, 1)))) return;
    QString at;
    if(s->getFillOpacity() < 0.999) at += QStringLiteral(" opacity=\"%1\"").arg(num(s->getFillOpacity()));
    body += QStringLiteral("<image x=\"0\" y=\"0\" width=\"1\" height=\"1\" preserveAspectRatio=\"none\" transform=\"matrix(%1 %2 %3 %4 %5 %6)\"%7%8 xlink:href=\"data:image/png;base64,%9\"/>")
        .arg(QString::number(t.m11(), 'f', 4), QString::number(t.m12(), 'f', 4), QString::number(t.m21(), 'f', 4), QString::number(t.m22(), 'f', 4), num(t.m31()), num(t.m32()), at, blendAttr(s))
        .arg(QString::fromLatin1(png.toBase64()));
}

void SvgDev::drawImage(GfxState* s, Object*, Stream* str, int w, int h, GfxImageColorMap* cm, bool, const int* maskColors, bool){
    flushRun();
    if(m_suppress) return;
    placeImage(s, decode(str, w, h, cm, maskColors));
}
void SvgDev::drawImageMask(GfxState* s, Object*, Stream* str, int w, int h, bool invert, bool, bool){
    flushRun();
    if(m_suppress) return;
    const QImage mask = decodeMask(str, w, h, invert);
    if(mask.isNull()) return;
    GfxRGB rgb; s->getFillRGB(&rgb);
    QImage img(w, h, QImage::Format_ARGB32);
    const QRgb c = qRgb(colToByte(rgb.r), colToByte(rgb.g), colToByte(rgb.b));
    for(int y = 0; y < h; ++y){
        const uchar* a = mask.constScanLine(y);
        QRgb* o = reinterpret_cast<QRgb*>(img.scanLine(y));
        for(int x = 0; x < w; ++x) o[x] = (c & 0xffffffu) | (uint(a[x]) << 24);
    }
    placeImage(s, img);
}
// A mask at its own resolution is resampled onto the larger of the two grids, so neither loses detail.
static QImage withAlpha(QImage img, const QImage& mask){
    if(img.isNull() || mask.isNull()) return img;
    const QSize sz(qMax(img.width(), mask.width()), qMax(img.height(), mask.height()));
    if(qint64(sz.width()) * sz.height() > kMaxImagePixels) return img;
    if(img.size() != sz) img = img.scaled(sz, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    const QImage m = mask.size() == sz ? mask : mask.scaled(sz, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    for(int y = 0; y < sz.height(); ++y){
        const uchar* a = m.constScanLine(y);
        QRgb* o = reinterpret_cast<QRgb*>(img.scanLine(y));
        for(int x = 0; x < sz.width(); ++x) o[x] = (o[x] & 0xffffffu) | (uint(qAlpha(o[x]) * a[x] / 255) << 24);
    }
    return img;
}
void SvgDev::drawMaskedImage(GfxState* s, Object*, Stream* str, int w, int h, GfxImageColorMap* cm, bool, Stream* mask, int mw, int mh, bool minv, bool){
    flushRun();
    if(m_suppress) return;
    const QImage a = decodeMask(mask, mw, mh, minv);
    placeImage(s, withAlpha(decode(str, w, h, cm, nullptr), a));
}
void SvgDev::drawSoftMaskedImage(GfxState* s, Object*, Stream* str, int w, int h, GfxImageColorMap* cm, bool, Stream* mask, int mw, int mh, GfxImageColorMap* mcm, bool){
    flushRun();
    if(m_suppress) return;
    const QImage a = decodeSoft(mask, mw, mh, mcm);
    placeImage(s, withAlpha(decode(str, w, h, cm, nullptr), a));
}

void quietErrors(ErrorCategory, Goffset, const char*){}

} // namespace

bool pdfVectorAvailable(){ return true; }

QStringList pdfToSvgPages(const QString& path, PdfVectorStats* stats){
    static GlobalParamsIniter params(quietErrors);
    QStringList out;
    auto doc = std::make_unique<PDFDoc>(std::make_unique<GooString>(path.toLocal8Bit().constData()));
    if(!doc->isOk()){
        if(stats) stats->encrypted = doc->getErrorCode() == errEncrypted;
        return out;
    }
    if(stats) stats->totalPages = doc->getNumPages();
    SvgDev dev;
    dev.stats = stats;
    dev.setFontAntialias(false);
    dev.startDoc(doc.get());
    const int n = qMin(doc->getNumPages(), kMaxPages);
    for(int p = 1; p <= n; ++p){
        dev.beginPage();
        doc->displayPage(&dev, p, 72.0, 72.0, 0, false, true, false);
        dev.finishPage();
        const double w = doc->getPageCropWidth(p), h = doc->getPageCropHeight(p);
        const bool swap = doc->getPageRotate(p) == 90 || doc->getPageRotate(p) == 270;
        const double pw = swap ? h : w, ph = swap ? w : h;
        out << QStringLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" width=\"%1\" height=\"%2\" viewBox=\"0 0 %1 %2\"><defs>%3</defs>%4</svg>")
                   .arg(num(pw), num(ph), dev.defs, dev.body);
        if(stats) ++stats->pages;
        if(dev.full){ dev.full = false; }
    }
    return out;
}

} // namespace xara
#endif
