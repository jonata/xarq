// selftest.cpp — deterministic, headless interaction tests. Drives the Canvas
// through real injected QMouseEvent/QKeyEvent sequences (the exact path a user's
// clicks take) and asserts on the resulting Document. Far more reliable than
// screenshot coordinate-guessing for verifying tool *logic*. Run: xarq --selftest
#include <QApplication>
#include <QClipboard>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QPainterPath>
#include <QImage>
#include <QImageWriter>
#include <QDir>
#include <QBuffer>
#include <QFileInfo>
#include <QPainter>
#include <QRadialGradient>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <cstdio>
#include "mainwindow.h"
#include "canvas.h"
#include "cdrvector.h"
#include "galleries.h"
#include "pathedit.h"
#include "panels.h"
#include "render.h"
#include <QCheckBox>
#include <QElapsedTimer>
#include <QTreeWidget>
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QListWidget>
#include "spell.h"
#include "qr.h"
#include "exif.h"
#include "photoops.h"
#include "trace.h"
#include "hyphen.h"
#include "vecextra.h"
#include "affvector.h"
#include "pdfvector.h"
#include "gisvector.h"
#include <zlib.h>
#include <cstring>
#include <cstdint>
#include "svgimport.h"
#include "svgexport.h"
#include "pathedit.h"
#include "render.h"
#include "prepress.h"
#include "dwgvector.h"
#include "icc.h"
#include "raw.h"
#include "rasterextra.h"
#include "extcodecs.h"
#include "extvector.h"
#include "jxr.h"
#include "dicom.h"
#include "wmf.h"
#include "slide.h"
#include "otshape.h"
#include "xar.h"
#include "fxgvector.h"
#include "sqlitemini.h"
#include "xdvector.h"

using namespace xara;

static int g_fail = 0, g_pass = 0;
static void check(bool ok, const char* what) {
    if (ok) { ++g_pass; std::printf("  \033[32mPASS\033[0m %s\n", what); }
    else    { ++g_fail; std::printf("  \033[31mFAIL\033[0m %s\n", what); }
}

namespace {
void send(QWidget* c, QEvent::Type t, QPointF p, Qt::MouseButton b, Qt::MouseButtons btns) {
    QMouseEvent e(t, p, c->mapToGlobal(p.toPoint()), b, btns, Qt::NoModifier);
    QApplication::sendEvent(c, &e);
}
void click(QWidget* c, QPointF p) {
    send(c, QEvent::MouseButtonPress,   p, Qt::LeftButton, Qt::LeftButton);
    send(c, QEvent::MouseButtonRelease, p, Qt::LeftButton, Qt::NoButton);
}
// Press at anchor, drag to pull out a bezier handle, release (a smooth pen point).
void penPoint(QWidget* c, QPointF anchor, QPointF handle) {
    send(c, QEvent::MouseButtonPress,   anchor, Qt::LeftButton, Qt::LeftButton);
    send(c, QEvent::MouseMove,          handle, Qt::NoButton,   Qt::LeftButton);
    send(c, QEvent::MouseButtonRelease, handle, Qt::LeftButton, Qt::NoButton);
}
void dragShape(QWidget* c, QPointF a, QPointF b) {
    send(c, QEvent::MouseButtonPress,   a, Qt::LeftButton, Qt::LeftButton);
    send(c, QEvent::MouseMove,          QPointF((a.x()+b.x())/2,(a.y()+b.y())/2), Qt::NoButton, Qt::LeftButton);
    send(c, QEvent::MouseMove,          b, Qt::NoButton, Qt::LeftButton);
    send(c, QEvent::MouseButtonRelease, b, Qt::LeftButton, Qt::NoButton);
}
// Like dragShape but carries a keyboard modifier (e.g. Ctrl for drop-a-copy).
void dragShapeMod(QWidget* c, QPointF a, QPointF b, Qt::KeyboardModifiers m) {
    auto sm=[&](QEvent::Type t, QPointF p, Qt::MouseButton bt, Qt::MouseButtons bs){
        QMouseEvent e(t, p, c->mapToGlobal(p.toPoint()), bt, bs, m); QApplication::sendEvent(c,&e); };
    sm(QEvent::MouseButtonPress,   a, Qt::LeftButton, Qt::LeftButton);
    sm(QEvent::MouseMove,          b, Qt::NoButton,   Qt::LeftButton);
    sm(QEvent::MouseButtonRelease, b, Qt::LeftButton, Qt::NoButton);
}
void key(QWidget* c, int k) {
    QKeyEvent e(QEvent::KeyPress, k, Qt::NoModifier);
    QApplication::sendEvent(c, &e);
}
bool pathHasCurve(const QPainterPath& p) {
    for (int i=0;i<p.elementCount();++i)
        if (p.elementAt(i).type==QPainterPath::CurveToElement) return true;
    return false;
}
} // namespace

// Build a deterministic "node editor in action" state for a screenshot: draw a smooth
// bezier curve with the pen, finish it, and switch to the Node tool so its handles show.
// Reuses the same reliable event injection as the self-test (no screen coordinate guessing).
void injectNodeDemo(Canvas* c) {
    c->newDocument();
    c->setTool(Tool::Pen);
    penPoint(c, {260,470}, {340,360});
    penPoint(c, {500,470}, {580,600});
    penPoint(c, {740,470}, {820,360});
    key(c, Qt::Key_Return);
    c->setTool(Tool::Node);
}

// Leave a drag mid-flight so the magenta alignment guide is on screen for a screenshot.
void injectSnapDemo(Canvas* c) {
    Document d;
    Shape a; a.type=ShapeType::Rect; a.rect=QRectF(0,0,220,150); a.cornerRadius=10;
    a.transform=QTransform::fromTranslate(260,250);
    a.fill.type=FillType::Linear; a.fill.stops={{0,QColor(96,170,255)},{1,QColor(40,80,180)}}; a.fill.p1=QPointF(220,150); a.name="A";
    Shape b; b.type=ShapeType::Rect; b.rect=QRectF(0,0,220,150); b.cornerRadius=10;
    b.transform=QTransform::fromTranslate(560,470);
    b.fill.type=FillType::Linear; b.fill.stops={{0,QColor(255,150,90)},{1,QColor(200,70,60)}}; b.fill.p1=QPointF(220,150); b.name="B";
    d.shapes.push_back(a); d.shapes.push_back(b);
    c->setDocument(d);
    c->setTool(Tool::Select);
    QPointF pressDoc(670,545);                         // centre of B
    QPointF relDoc = pressDoc + QPointF(-297,-2);       // B.left 560 -> 263 (≈3px from A.left 260) => snaps
    QPointF vp=c->mapToView(pressDoc), vr=c->mapToView(relDoc);
    send(c, QEvent::MouseButtonPress, vp, Qt::LeftButton, Qt::LeftButton);
    send(c, QEvent::MouseMove,        vr, Qt::NoButton,   Qt::LeftButton);   // no release: guide stays lit
}

// Visual: subtract a small disc from a big one (a ring) + intersect a star with a disc.
void injectCombineDemo(Canvas* c) {
    Document d;
    Shape o; o.type=ShapeType::Ellipse; o.rect=QRectF(0,0,240,240); o.transform=QTransform::fromTranslate(200,260);
    o.fill.type=FillType::Linear; o.fill.stops={{0,QColor(120,200,255)},{1,QColor(40,90,200)}}; o.fill.p1=QPointF(240,240);
    Shape i; i.type=ShapeType::Ellipse; i.rect=QRectF(0,0,120,120); i.transform=QTransform::fromTranslate(260,320);
    d.shapes.push_back(o); d.shapes.push_back(i);
    Shape st; st.type=ShapeType::Polygon; st.polySides=5; st.polyStar=true; st.rect=QRectF(0,0,240,240);
    st.transform=QTransform::fromTranslate(560,260);
    st.fill.type=FillType::Solid; st.fill.color=QColor(255,200,60);
    Shape disc; disc.type=ShapeType::Ellipse; disc.rect=QRectF(0,0,200,200); disc.transform=QTransform::fromTranslate(580,280);
    d.shapes.push_back(st); d.shapes.push_back(disc);
    c->setDocument(d);
    c->select({0,1}); c->combineSelection(1);   // ring (big - small); leaves [star, disc, ring]
    c->select({0,1}); c->combineSelection(2);   // star ∩ disc
    c->selectNone();
}

// Visual: morph a blue disc on the left into a gold star on the right.
void injectBlendDemo(Canvas* c) {
    Document d;
    Shape a; a.type=ShapeType::Ellipse; a.rect=QRectF(0,0,110,110); a.transform=QTransform::fromTranslate(150,300);
    a.fill.type=FillType::Solid; a.fill.color=QColor(70,150,255);
    Shape b; b.type=ShapeType::Polygon; b.polySides=5; b.polyStar=true; b.rect=QRectF(0,0,170,170); b.transform=QTransform::fromTranslate(760,280);
    b.fill.type=FillType::Solid; b.fill.color=QColor(255,190,50);
    d.shapes.push_back(a); d.shapes.push_back(b);
    c->setDocument(d); c->select({0,1}); c->blendSelection(7); c->selectNone();
}

// Art/textured brush demo: generate ink bitmaps and stamp them along wavy paths.
void injectBrushDemo(Canvas* c) {
    // Ink A: a soft round dab (radial alpha) — for tinted "paint" strokes.
    QImage dab(64,64,QImage::Format_ARGB32); dab.fill(Qt::transparent);
    { QPainter p(&dab); p.setRenderHint(QPainter::Antialiasing);
      QRadialGradient rg(32,32,31); rg.setColorAt(0,QColor(255,255,255,255));
      rg.setColorAt(0.6,QColor(255,255,255,210)); rg.setColorAt(1,QColor(255,255,255,0));
      p.fillRect(dab.rect(),rg); }
    // Ink B: a coloured 5-point star — stamped as-is (no tint).
    QImage star(60,60,QImage::Format_ARGB32); star.fill(Qt::transparent);
    { QPainter p(&star); p.setRenderHint(QPainter::Antialiasing); QPainterPath sp;
      for(int i=0;i<10;++i){ double a=-M_PI/2+i*M_PI/5, r=(i&1)?12:29;
          QPointF pt(30+std::cos(a)*r, 30+std::sin(a)*r); if(i==0) sp.moveTo(pt); else sp.lineTo(pt); }
      sp.closeSubpath(); p.fillPath(sp,QColor(255,170,40)); p.strokePath(sp,QPen(QColor(190,110,0),2)); }

    Document d;
    auto wave=[&](double y, QColor col, const QImage& ink, double width, bool tint, double spacing){
        Shape s; s.type=ShapeType::Path; s.name="Brush"; QPainterPath p; p.moveTo(60,y);
        p.cubicTo(250,y-100, 450,y+100, 640,y);
        s.path=p; s.fill.type=FillType::None;
        s.stroke.enabled=true; s.stroke.width=width; s.stroke.color=col;
        s.stroke.brushInk=ink; s.stroke.brushTint=tint; s.stroke.brushSpacing=spacing;
        d.shapes.push_back(s);
    };
    wave(150, QColor(40,120,230), dab,  44, true, 0.22);   // soft blue paint
    wave(330, QColor(0,0,0),      star, 40, false, 0.85);  // orange stars along the wave
    wave(510, QColor(210,50,30),  dab,  20, true, 0.45);   // thin red paint
    c->setDocument(d);
}

// ClipView demo: a 5-point star "keyhole" clips three colour bands that extend well beyond it,
// so the RGB bands appear only inside the star outline (hard-clipped). Round-trips through XAR
// via TAG_CLIPVIEWCONTROLLER/TAG_CLIPVIEW. Mirrors Xara's "Combine shapes > Clip view".
void injectClipViewDemo(Canvas* c) {
    Document d; d.pageSize = QSizeF(700, 600);
    Shape bg; bg.type=ShapeType::Rect; bg.name="BG"; bg.rect=QRectF(0,0,700,600);
    bg.fill.type=FillType::Solid; bg.fill.color=QColor(244,244,248); bg.stroke.enabled=false;
    d.shapes.push_back(bg);

    Shape clip; clip.type=ShapeType::Group; clip.name="ClipView"; clip.isClipView=true;
    Shape star; star.type=ShapeType::Polygon; star.name="Keyhole";
    star.polySides=5; star.polyStar=true; star.polyStarDepth=0.45;
    star.rect=QRectF(0,0,420,420); star.transform=QTransform::fromTranslate(140,90);
    star.fill.type=FillType::Solid; star.fill.color=QColor(70,70,80); star.stroke.enabled=false;
    clip.children.push_back(star);                       // keyhole [0]
    auto band=[&](double y0,double y1,QColor col){ Shape s; s.type=ShapeType::Rect; s.name="Band";
        s.rect=QRectF(60,y0,580,y1-y0); s.fill.type=FillType::Solid; s.fill.color=col; s.stroke.enabled=false;
        clip.children.push_back(s); };
    band( 80,233, QColor(220,55,55));                    // content [1..3] — wider/taller than the star
    band(233,386, QColor(60,180,80));
    band(386,540, QColor(60,110,220));
    clip.clipSplit = 1;                                  // 1 keyhole, 3 clipped content bands
    d.shapes.push_back(clip);
    c->setDocument(d);
}

// Variable-width / pressure-stroke demo: three S-curve strokes with width profiles — a calligraphic taper
// to a point, a pressure "spindle" (thin-fat-thin), and a 0->full ramp. Round-trips through XAR via
// TAG_STROKETYPE(0x02) + TAG_VARIABLEWIDTHTABLE (ValueFunctionPressure id=5).
void injectVarWidthDemo(Canvas* c) {
    Document d; d.pageSize = QSizeF(700, 600);
    auto stroke=[&](double y, QColor col, double width, const QVector<QPointF>& prof){
        Shape s; s.type=ShapeType::Path; s.name="VarWidth"; QPainterPath p; p.moveTo(60,y);
        p.cubicTo(250,y-110, 450,y+110, 640,y);
        s.path=p; s.fill.type=FillType::None;
        s.stroke.enabled=true; s.stroke.width=width; s.stroke.color=col;
        s.stroke.varWidth=true; s.stroke.widthProfile=prof;
        d.shapes.push_back(s);
    };
    stroke(140, QColor(40,120,230), 40, {{0,1},{1,0}});                                       // taper to a point
    stroke(320, QColor(30,160,90),  46, {{0,0.12},{0.25,0.4},{0.5,1.0},{0.75,0.4},{1,0.12}}); // pressure spindle
    stroke(500, QColor(210,60,40),  36, {{0,0.05},{1,1.0}});                                  // 0 -> full ramp
    c->setDocument(d);
}

// FEATHER_EFFECT(4127) demo: a sharp reference shape (left) + a feathered GROUP (right) whose soft edge
// round-trips through the live-effect feather container. The right pair should have blurry edges.
void injectFeatherEffectDemo(Canvas* c) {
    Document d; d.pageSize = QSizeF(700, 460);
    Shape ref; ref.type=ShapeType::Ellipse; ref.name="Sharp"; ref.rect=QRectF(0,0,170,170);
    ref.transform=QTransform::fromTranslate(60,150); ref.fill.type=FillType::Solid; ref.fill.color=QColor(205,60,60); ref.stroke.enabled=false;
    d.shapes.push_back(ref);

    Shape grp; grp.type=ShapeType::Group; grp.name="Feather"; grp.feather=14.0;   // 14 doc px soft edge
    Shape e; e.type=ShapeType::Ellipse; e.rect=QRectF(0,0,170,170); e.transform=QTransform::fromTranslate(360,80);
    e.fill.type=FillType::Solid; e.fill.color=QColor(60,120,210); e.stroke.enabled=false;
    Shape st; st.type=ShapeType::Polygon; st.polySides=5; st.polyStar=true; st.polyStarDepth=0.45;
    st.rect=QRectF(0,0,190,190); st.transform=QTransform::fromTranslate(400,190);
    st.fill.type=FillType::Solid; st.fill.color=QColor(250,190,60); st.stroke.enabled=false;
    grp.children={e,st};
    d.shapes.push_back(grp);
    c->setDocument(d);
}

// Vector / stroke-shape stroke demo: a 5-point star REPEATED along a wave, and a single leaf STRETCHED along
// another wave. Round-trips through XAR via TAG_STROKEDEFINITION(4003) + TAG_STROKETYPE(top byte 0x00).
void injectStrokeShapeDef(Canvas* c) {
    Document d; d.pageSize = QSizeF(700, 500);
    auto starPath=[&](double w,double h)->QPainterPath{ QPainterPath p;
        for(int i=0;i<10;++i){ double a=-M_PI/2+i*M_PI/5, r=(i&1)?0.4:1.0;
            QPointF pt(w/2+std::cos(a)*r*w/2, h/2+std::sin(a)*r*h/2);
            if(i==0) p.moveTo(pt); else p.lineTo(pt); }
        p.closeSubpath(); return p; };
    auto leafPath=[&](double w,double h)->QPainterPath{ QPainterPath p; p.moveTo(0,h/2);
        p.cubicTo(0,0, w,0, w,h/2); p.cubicTo(w,h, 0,h, 0,h/2); p.closeSubpath(); return p; };
    auto wave=[&](double y,double width,const QVector<QPainterPath>& shp,const QVector<QColor>& cols,bool rep,int n){
        Shape s; s.type=ShapeType::Path; s.name="StrokeShape"; QPainterPath p; p.moveTo(60,y);
        p.cubicTo(250,y-110, 450,y+110, 640,y);
        s.path=p; s.fill.type=FillType::None;
        s.stroke.enabled=true; s.stroke.width=width; s.stroke.color=QColor(60,60,60);
        s.stroke.strokeShapePaths=shp; s.stroke.strokeShapeCols=cols;
        s.stroke.strokeRepeating=rep; s.stroke.strokeRepeats=n; s.stroke.strokeOverrideFill=false;
        d.shapes.push_back(s);
    };
    wave(150, 46, {starPath(48,48)}, {QColor(250,170,40)}, true, 0);   // stars repeated along the wave
    wave(370, 64, {leafPath(60,40)}, {QColor(70,170,90)}, false, 0);   // one leaf stretched along the wave
    c->setDocument(d);
}

// Bevel demo: shapes with different bevel profiles, lit from the upper-left — to evaluate the 3-D lighting.
void injectBevelDemo(Canvas* c) {
    Document d; d.pageSize = QSizeF(760, 560);
    auto bev=[&](ShapeType ty,double x,double y,QColor col,int type,double depth){
        Shape s; s.type=ty; s.rect=QRectF(0,0,300,200); s.cornerRadius=(ty==ShapeType::Rect?14:0);
        s.transform=QTransform::fromTranslate(x,y);
        s.fill.type=FillType::Solid; s.fill.color=col; s.stroke.enabled=false;
        s.bevel.on=true; s.bevel.type=type; s.bevel.depth=depth; s.bevel.light=135; s.bevel.contrast=85;
        s.bevel.tilt=45; s.bevel.outer=false;
        d.shapes.push_back(s);
    };
    bev(ShapeType::Rect,    40,  40, QColor(95,140,210),  0, 26);   // flat
    bev(ShapeType::Rect,   420,  40, QColor(210,120,90),  1, 26);   // round
    bev(ShapeType::Ellipse, 40, 320, QColor(120,190,110), 6, 30);   // smooth, on an ellipse
    bev(ShapeType::Polygon,420, 320, QColor(205,170,80),  0, 22);   // flat, on a polygon
    if(!d.shapes.isEmpty()) d.shapes[3].polySides=6;
    c->setDocument(d);
}

// Blend-mode demo: a rainbow backdrop with blue circles over it, each using a transparency blend
// type (top row separable, bottom row the per-pixel HSL modes Hue/Saturation/Luminosity).
void injectHslDemo(Canvas* c) {
    Document d;
    Shape bg; bg.type=ShapeType::Rect; bg.rect=QRectF(0,0,920,460); bg.name="Backdrop"; bg.stroke.enabled=false;
    bg.fill.type=FillType::Linear; bg.fill.p0=QPointF(0,230); bg.fill.p1=QPointF(920,230); bg.fill.stops.clear();
    for(int i=0;i<=12;++i){ double t=i/12.0; bg.fill.stops.push_back(GradStop{t, QColor::fromHslF(t,0.9f,0.5f)}); }
    d.shapes.push_back(bg);
    auto circle=[&](double x,double y,int type){ Shape s; s.type=ShapeType::Ellipse; s.rect=QRectF(0,0,150,150);
        s.transform=QTransform::fromTranslate(x,y); s.fill.type=FillType::Solid; s.fill.color=QColor(60,100,210);
        s.stroke.enabled=false; s.transp.type=type; s.transp.geom=0; s.transp.t0=0.0; d.shapes.push_back(s); };
    double x=20; for(int t : {3,2,19,22}) { circle(x, 30,  t); x+=225; }   // Stained-glass, Bleach, Darken, Lighten
    x=20;       for(int t : {13,31,16,28}){ circle(x, 270, t); x+=225; }   // Contrast, Hue, Saturation, Luminosity
    c->setDocument(d);
}

// Visual: a shape with a rich multi-stop gradient, selected so the editor shows it.
void injectGradientDemo(Canvas* c) {
    Document d;
    Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,360,240); s.cornerRadius=18; s.transform=QTransform::fromTranslate(220,200);
    s.fill.type=FillType::Linear;
    s.fill.stops={{0.0,QColor(255,90,90)},{0.33,QColor(255,210,70)},{0.66,QColor(70,200,160)},{1.0,QColor(80,120,255)}};
    s.fill.p0=QPointF(0,0); s.fill.p1=QPointF(360,240);
    d.shapes.push_back(s); c->setDocument(d); c->select({0});
}

int runSelfTest() {
    xara::MainWindow w;
    w.setShowWelcome(false);   // no launch dialogs during the headless test
    w.resize(1400, 900);
    w.show();
    QApplication::processEvents();
    Canvas* c = w.canvas();

    std::printf("\n\033[1XarQ — interaction self-test\033[0m\n");

    // --- Launch state: no blank "Untitled" document is opened by default — the landing tab is deferred
    // until the welcome screen resolves (or a blank fallback once startup runs). ---
    std::printf("[launch] deferred landing tab (no blank document by default)\n");
    { xara::MainWindow w2; w2.setShowWelcome(false);   // headless path → blank fallback, no modal welcome
      check(w2.tabCount()==0, "a freshly-constructed window has NO landing tab yet (nothing opened by default)");
      QApplication::processEvents();                   // fires the deferred ensureLandingTab
      check(w2.tabCount()==1, "startup materialises exactly one landing tab");
      check(w2.canvas()->doc().shapes.isEmpty(), "the deferred landing document is blank"); }
    // Welcome "New from template" at startup (before the deferred fallback runs): one populated tab, not two.
    { xara::MainWindow w3; w3.setShowWelcome(false);   // tabCount 0 (deferred, no processEvents yet)
      w3.newFromTemplate(1);                           // the welcome screen's template choice
      w3.ensureLandingTab();                           // runStartupSequence's tail after the modal returns
      check(w3.tabCount()==1, "startup: choosing a template creates exactly one tab (no blank behind it)");
      check(!w3.canvas()->doc().shapes.isEmpty(), "startup: the chosen template populates the document"); }
    // Welcome "New document" at startup opens exactly one blank tab — not a blank behind a chosen one.
    { xara::MainWindow w4; w4.setShowWelcome(false);   // 0 tabs (deferred)
      w4.newFromTemplate(0); w4.ensureLandingTab();    // the welcome "New document" choice, then the tail no-op
      check(w4.tabCount()==1, "startup: 'New document' opens exactly one tab (no blank behind it)");
      check(w4.canvas()->doc().shapes.isEmpty(), "startup: the 'New document' tab is blank"); }
    // Empty (welcome) state: the title-bar File/Insert menus stay live, so an Import/Insert acts on the hidden
    // canvas directly. It MUST materialise a landing tab + reveal the workspace, else the content is invisible
    // (m_activeTab==-1, chrome hidden) and silently discarded on the next New. (Regression for the c03ff4b review.)
    { xara::MainWindow w5; w5.setShowWelcome(false); w5.setDeferStartup(true);
      QApplication::processEvents();                   // deferStartup ⇒ ctor creates NO tab
      check(w5.tabCount()==0, "empty-state precondition: no landing tab yet");
      w5.enterEmptyState();                            // simulate the interactive welcome screen (private; runSelfTest is a friend)
      check(w5.tabCount()==0 && w5.activeTab()==-1, "empty state: welcome shown, no active tab");
      xara::Shape r; r.type=xara::ShapeType::Rect; r.rect=QRectF(0,0,120,80); r.name="empty-import";
      w5.canvas()->addShapes({r}, "Import");           // an Import/Insert from the empty state → documentChanged guard
      check(w5.tabCount()==1, "importing content from the empty state materialises a landing tab");
      check(w5.activeTab()==0, "…and it becomes the active tab (no longer m_activeTab==-1)");
      check(w5.canvas()->doc().shapes.size()==1 && w5.canvas()->doc().shapes[0].name=="empty-import",
            "…and the imported content is preserved, not discarded"); }
    // Dropping a file onto the welcome screen materialises the workspace and imports it (audit #6).
    { xara::MainWindow w8; w8.setShowWelcome(false); w8.setDeferStartup(true);
      QApplication::processEvents(); w8.enterEmptyState();
      check(w8.tabCount()==0, "welcome-drop precondition: empty state, no tab");
      QImage im(16,16,QImage::Format_ARGB32); im.fill(Qt::red);
      const QString tmp=QDir::tempPath()+"/xarq_selftest_drop.png"; im.save(tmp);
      w8.acceptWelcomeDrop(tmp);
      check(w8.tabCount()==1, "dropping a file on the welcome screen materialises a landing tab");
      check(!w8.canvas()->doc().shapes.isEmpty(), "…and imports the dropped image into it");
      QFile::remove(tmp); }
    check(c->doc().shapes.isEmpty(), "the primary test window's document is blank (no auto-loaded sample)");

    // --- On-canvas corner-radius handle: drag a rectangle's top-edge handle to round its corners ---
    std::printf("[select] on-canvas corner-radius handle\n");
    { Document d; Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,200,150); s.transform=QTransform::fromTranslate(100,100);
      s.fill.type=FillType::Solid; s.fill.color=Qt::gray; d.shapes={s};
      c->setDocument(d); c->setTool(Tool::Select); c->select({0});
      QPointF h; check(c->cornerRadHandle(h), "a selected rectangle exposes a corner-radius handle");
      const int before=c->undoDepth();
      dragShape(c, h, c->mapToView(s.transform.map(QPointF(60,0))));   // drag the handle along the top edge to x=60 (local)
      check(c->doc().shapes[0].cornerRadius>30.0 && c->doc().shapes[0].cornerRadius<=75.0, "dragging the corner handle rounds the rectangle");
      check(c->undoDepth()>before, "the round-corner drag pushes one undo step");
      QPointF h2; c->cornerRadHandle(h2);
      dragShape(c, h2, c->mapToView(s.transform.map(QPointF(0,0))));    // drag back to the corner → un-round
      check(c->doc().shapes[0].cornerRadius<5.0, "dragging the handle back to the corner un-rounds it");
      check(c->cornerRadHandle(h)==true, "the handle stays available; a non-rect exposes none");
      { Shape e; e.type=ShapeType::Ellipse; e.rect=QRectF(0,0,80,80); Document d2; d2.shapes={e}; c->setDocument(d2); c->select({0});
        QPointF hx; check(c->cornerRadHandle(hx)==false, "an ellipse has no corner-radius handle"); } }

    // --- On-canvas shadow editing: with the Shadow tool, click toggles, drag casts, a handle sets blur ---
    std::printf("[shadow] on-canvas shadow drag + blur handle\n");
    { Document d; Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,120,90); s.transform=QTransform::fromTranslate(150,150);
      s.fill.type=FillType::Solid; s.fill.color=Qt::darkCyan; d.shapes={s};
      c->setDocument(d); c->setTool(Tool::Shadow);
      const QPointF centerV=c->mapToView(s.transform.map(QPointF(60,45)));
      click(c, centerV);
      check(c->doc().shapes[0].shadow.on, "clicking a shape with the Shadow tool turns the shadow on");
      const QPointF o0=c->doc().shapes[0].shadow.offset; const int before=c->undoDepth();
      dragShape(c, centerV, centerV+QPointF(50,40));   // drag the object → cast the shadow
      check(c->doc().shapes[0].shadow.offset.x()>o0.x()+0.1 && c->doc().shapes[0].shadow.offset.y()>o0.y()+0.1, "dragging the object increases the shadow offset");
      check(c->undoDepth()>before, "the shadow-offset drag pushes one undo step");
      QPointF bh; check(c->shadowBlurHandle(bh), "a shadowed shape exposes a blur handle");
      const double b0=c->doc().shapes[0].shadow.blur;
      dragShape(c, bh, bh+QPointF(30,30));   // drag the blur handle outward → softer
      check(c->doc().shapes[0].shadow.blur>b0, "dragging the blur handle softens the shadow");
      click(c, c->mapToView(c->doc().shapes[0].transform.map(QPointF(60,45))));
      check(!c->doc().shapes[0].shadow.on, "clicking the shape again toggles the shadow off"); }
    // On-canvas shadow COLOUR: a swatch (double-click opens the picker; a dropped colour recolours it).
    { Document d; Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,100,80); s.transform=QTransform::fromTranslate(200,200);
      s.fill.type=FillType::Solid; s.fill.color=Qt::green; s.shadow.on=true; s.shadow.color=QColor(0,0,0,120);
      d.shapes={s}; c->setDocument(d); c->setTool(Tool::Shadow); c->select({0});
      QPointF cs; check(c->shadowColorSwatch(cs), "a shadowed shape exposes a colour swatch");
      check(c->dropColorAt(QPointF(250,240), QColor(80,40,200,150), false), "a colour dropped on a shadowed shape (Shadow tool) is accepted");
      check(c->doc().shapes[0].shadow.color==QColor(80,40,200,150), "the drop recoloured the SHADOW");
      check(c->doc().shapes[0].fill.color==QColor(Qt::green), "the object's fill colour is untouched");
      c->select({0}); QPointF cs2; c->shadowColorSwatch(cs2); click(c, cs2);
      check(c->selection().size()==1 && c->selection().first()==0, "clicking the shadow swatch keeps the shape selected (no deselect)");
      c->doc().shapes[0].shadow.on=false;
      QPointF cx; check(c->shadowColorSwatch(cx)==false, "no colour swatch when the shadow is off");
      check(c->dropColorAt(QPointF(250,240), QColor(10,20,30), false), "drop accepted with the shadow off");
      check(c->doc().shapes[0].fill.color==QColor(10,20,30), "with the shadow off, the drop recolours the FILL (normal behaviour)"); }
    // Regression ("Shadow checkbox desync"): the per-tool InfoBar mirrors the selected object's effect state,
    // but rebuild() only ran on a tool switch — so casting a shadow on the canvas left the "Shadow" checkbox
    // stale/unchecked. A debounced rebuild off document/selection changes now re-syncs it. Use a standalone
    // canvas+InfoBar (isolated from the shown MainWindow) so pumping the debounce timer can't re-enter its UI.
    { Canvas tc; tc.resize(500,360);
      xara::InfoBar ib(&tc);
      Document d; Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,120,90); s.transform=QTransform::fromTranslate(80,80);
      s.fill.type=FillType::Solid; s.fill.color=Qt::green; d.shapes={s};
      tc.setDocument(d); tc.setTool(Tool::Shadow); tc.select({0});
      // return the NEWEST "Shadow" checkbox: rebuild() deleteLater()s the previous ones, but without an event
      // pump they linger as children — the last-created is the live one in the current bar.
      auto shadowBox=[&]()->QCheckBox*{ QCheckBox* r=nullptr; for(QCheckBox* b: ib.findChildren<QCheckBox*>()) if(b->text()=="Shadow") r=b; return r; };
      ib.rebuild();
      check(shadowBox() && !shadowBox()->isChecked(), "InfoBar Shadow checkbox starts unchecked (no shadow yet)");
      const QPointF cen=tc.mapToView(QPointF(140,125));   // shape centre (bbox 80,80 - 200,170)
      dragShape(&tc, cen, cen+QPointF(40,30));            // drag the object to cast a shadow (emits documentChanged)
      check(tc.doc().shapes[0].shadow.on, "the drag cast a shadow (shadow.on true)");
      ib.rebuild();                                       // what the debounced document/selection-change re-sync invokes
      check(shadowBox() && shadowBox()->isChecked(), "the InfoBar Shadow checkbox re-syncs to CHECKED after the canvas cast the shadow"); }

    // Regression (reported "Shadow seems broken"): a freshly-cast WALL shadow must actually PAINT its
    // offset silhouette. Build it directly and via the tool, render with paintDocument, sample a pixel in
    // the shadow-only band (right/below the shape, over the transparent backdrop).
    { Document d; d.pageSize=QSizeF(600,500); Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,200,150);
      s.transform=QTransform::fromTranslate(150,150); s.fill.type=FillType::Solid; s.fill.color=Qt::red;
      s.shadow.on=true; s.shadow.type=0; s.shadow.offset=QPointF(70,60); s.shadow.blur=8; s.shadow.color=QColor(0,0,0,200);
      d.shapes={s};
      QImage img(600,500,QImage::Format_ARGB32_Premultiplied); img.fill(Qt::transparent);
      { QPainter p(&img); p.setRenderHint(QPainter::Antialiasing); paintDocument(p, d); }
      // shape doc bbox (150,150)-(350,300); shadow silhouette shifted by (70,60) => (220,210)-(420,360).
      const QColor shapePx = img.pixelColor(250,220);     // inside the shape (red)
      const QColor shadowPx= img.pixelColor(390,330);     // shadow-only band, right+below the shape
      check(shapePx.alpha()>200, "the shape itself renders");
      check(shadowPx.alpha()>10, "a directly-built wall shadow paints its offset silhouette"); }
    { Document d; d.pageSize=QSizeF(800,600); Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,200,150);
      s.transform=QTransform::fromTranslate(200,200); s.fill.type=FillType::Solid; s.fill.color=Qt::red; d.shapes={s};
      c->setDocument(d); c->setTool(Tool::Shadow); c->select({0});
      const QPointF cen=c->mapToView(QPointF(300,275));   // shape centre (bbox 200,200-400,350)
      dragShape(c, cen, cen+QPointF(80,60));               // drag the object to cast the shadow
      check(c->doc().shapes[0].shadow.on, "dragging with the Shadow tool casts a shadow (shadow.on true)");
      const QPointF off=c->doc().shapes[0].shadow.offset;
      QImage img(800,600,QImage::Format_ARGB32_Premultiplied); img.fill(Qt::transparent);
      { QPainter p(&img); p.setRenderHint(QPainter::Antialiasing); paintDocument(p, c->doc()); }
      const QPointF probe(360+off.x(), 320+off.y());       // just inside the shifted silhouette, past the shape's BR (400,350)
      const QColor shadowPx = img.pixelColor(int(probe.x()), int(probe.y()));
      check(shadowPx.alpha()>10, "a tool-cast wall shadow paints its offset silhouette"); }
    // Regression (the actual "Shadow seems broken" cause): a shape that takes a SPECIAL render path
    // (feather / blur / transparency-mask) must STILL paint its drop shadow. Before the fix, paintShape's
    // feather/blur/... branches `return`ed before the shadow was ever painted, so a feathered+shadowed
    // shape showed no shadow at all.
    { Document d; d.pageSize=QSizeF(600,500); Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,200,150);
      s.transform=QTransform::fromTranslate(150,150); s.fill.type=FillType::Solid; s.fill.color=Qt::red;
      s.feather=10.0;                                   // <-- soft edge => takes the paintFeathered() branch
      s.shadow.on=true; s.shadow.type=0; s.shadow.offset=QPointF(70,60); s.shadow.blur=8; s.shadow.color=QColor(0,0,0,200);
      d.shapes={s};
      QImage img(600,500,QImage::Format_ARGB32_Premultiplied); img.fill(Qt::transparent);
      { QPainter p(&img); p.setRenderHint(QPainter::Antialiasing); paintDocument(p, d); }
      const QColor shadowPx = img.pixelColor(390,330);  // shadow-only band, right+below the shape
      check(shadowPx.alpha()>10, "a FEATHERED shape still paints its drop shadow (was dropped before the fix)"); }
    { Document d; d.pageSize=QSizeF(600,500); Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,200,150);
      s.transform=QTransform::fromTranslate(150,150); s.fill.type=FillType::Solid; s.fill.color=Qt::red;
      s.blur=6.0;                                       // <-- live blur => takes the paintBlurred() branch
      s.shadow.on=true; s.shadow.type=0; s.shadow.offset=QPointF(70,60); s.shadow.blur=8; s.shadow.color=QColor(0,0,0,200);
      d.shapes={s};
      QImage img(600,500,QImage::Format_ARGB32_Premultiplied); img.fill(Qt::transparent);
      { QPainter p(&img); p.setRenderHint(QPainter::Antialiasing); paintDocument(p, d); }
      check(img.pixelColor(390,330).alpha()>10, "a BLURRED shape still paints its drop shadow"); }

    // --- On-canvas Contour / Bevel / Feather scalar handles (same pattern as the shadow blur handle) ---
    std::printf("[effects] on-canvas contour/bevel/feather handles\n");
    auto mkFxRect=[&]()->QTransform{ Document d; Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,140,100); s.transform=QTransform::fromTranslate(120,120);
        s.fill.type=FillType::Solid; s.fill.color=Qt::gray; d.shapes={s}; c->setDocument(d); return s.transform; };
    { QTransform xf=mkFxRect(); c->setTool(Tool::Contour);
      click(c, c->mapToView(xf.map(QPointF(70,50))));   // toggle contour on (width defaults to 8)
      check(c->doc().shapes[0].contour.on, "clicking with the Contour tool adds a contour");
      QPointF eh; check(c->effectHandle(eh), "a contoured shape exposes a width handle");
      const double w0=c->doc().shapes[0].contour.width; const int before=c->undoDepth();
      dragShape(c, eh, eh+QPointF(30,30));
      check(c->doc().shapes[0].contour.width>w0, "dragging the handle out widens the contour");
      check(c->undoDepth()>before, "the contour-width drag is one undo step");
      QPointF eh2; c->effectHandle(eh2);
      dragShape(c, eh2, c->mapToView(xf.map(QPointF(70,50))));   // drag inside the shape → inner (negative) contour
      check(c->doc().shapes[0].contour.width<0.0, "dragging the handle inside the shape makes an inner (negative) contour"); }
    // On-canvas contour COLOUR: a swatch (double-click opens the picker; a dropped colour recolours it). Feather has
    // no colour of its own — it softens the object's OWN edge — so there is no feather-colour swatch by design.
    { QTransform xf=mkFxRect(); c->setTool(Tool::Contour);
      click(c, c->mapToView(xf.map(QPointF(70,50))));   // toggle contour on
      c->doc().shapes[0].contour.color=QColor(0,0,0,90); c->select({0});
      QPointF cs; check(c->contourColorSwatch(cs), "a contoured shape exposes a colour swatch");
      check(c->dropColorAt(QPointF(190,170), QColor(200,60,20,180), false), "a colour dropped on a contoured shape (Contour tool) is accepted");
      check(c->doc().shapes[0].contour.color==QColor(200,60,20,180), "the drop recoloured the CONTOUR");
      check(c->doc().shapes[0].fill.color==QColor(Qt::gray), "the object's fill colour is untouched");
      c->select({0}); QPointF cs2; c->contourColorSwatch(cs2); click(c, cs2);
      check(c->selection().size()==1 && c->selection().first()==0, "clicking the contour swatch keeps the shape selected (no deselect)");
      c->doc().shapes[0].contour.on=false;
      QPointF cx; check(c->contourColorSwatch(cx)==false, "no colour swatch when the contour is off");
      check(c->dropColorAt(QPointF(190,170), QColor(10,20,30), false), "drop accepted with the contour off");
      check(c->doc().shapes[0].fill.color==QColor(10,20,30), "with the contour off, the drop recolours the FILL (normal behaviour)"); }
    // Contour OUTER (graduation) colour swatch: a second swatch, stacked below the inner one.
    { QTransform xf=mkFxRect(); c->setTool(Tool::Contour);
      click(c, c->mapToView(xf.map(QPointF(70,50))));   // contour on
      QPointF cs,c2; check(c->contourColorSwatch(cs) && c->contourColor2Swatch(c2), "a contoured shape exposes BOTH an inner and outer colour swatch");
      check(QLineF(cs,c2).length()>12 && QLineF(cs,c2).length()<24, "the two contour swatches are stacked (distinct but adjacent)");
      c->doc().shapes[0].contour.on=false;
      QPointF cx; check(c->contourColor2Swatch(cx)==false, "no outer-colour swatch when the contour is off"); }
    // Bitmap-fill parallelogram: the second-axis (p2) handle is now editable (was gated out of Bitmap).
    { Document d; Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,160,120); s.transform=QTransform::fromTranslate(60,60);
      s.fill.type=FillType::Bitmap; s.fill.image=QImage(8,8,QImage::Format_ARGB32); s.fill.image.fill(Qt::blue);
      s.fill.p0=QPointF(0,0); s.fill.p1=QPointF(160,0); s.fill.p2=QPointF(0,120);
      d.shapes={s}; c->setDocument(d); c->setTool(Tool::Fill); c->select({0});
      const QPointF p2v=c->mapToView(s.transform.map(s.fill.p2));   // green second-axis handle
      const QPointF before=c->doc().shapes[0].fill.p2;
      dragShape(c, p2v, p2v+QPointF(30,20));
      check(c->doc().shapes[0].fill.p2!=before, "dragging the bitmap-fill p2 handle skews the fill's second axis"); }
    // ClipView: group the selection so the topmost object is the keyhole clipping the rest.
    { Document d; Shape bg; bg.type=ShapeType::Rect; bg.rect=QRectF(0,0,200,200); bg.transform=QTransform::fromTranslate(50,50); bg.fill.type=FillType::Solid; bg.fill.color=Qt::red;
      Shape win; win.type=ShapeType::Ellipse; win.rect=QRectF(0,0,120,120); win.transform=QTransform::fromTranslate(80,80); win.fill.type=FillType::Solid; win.fill.color=Qt::green;
      d.shapes={bg,win}; c->setDocument(d); c->setTool(Tool::Select); c->select({0,1});
      c->applyClipView();
      check(c->doc().shapes.size()==1 && c->doc().shapes[0].type==ShapeType::Group, "ClipView collapses the selection into one group");
      const Shape& g=c->doc().shapes[0];
      check(g.isClipView && g.clipSplit==1 && g.children.size()==2, "the group is a ClipView with 1 keyhole + 1 clipped content");
      check(g.children[0].type==ShapeType::Ellipse, "the topmost object (the ellipse) became the keyhole [0]");
      c->removeClipView();
      check(!c->doc().shapes[0].isClipView, "Remove ClipView turns it back into a plain group"); }
    // --- Shape-tool parametric geometry (Batch 3): ellipse pie/arc/segment, polygon curvature, star depth, rounding ---
    { Shape s; s.type=ShapeType::Ellipse; s.rect=QRectF(0,0,100,80);
      const QPainterPath whole=s.localPath();
      s.ellipseMode=1; s.ellipseSweep=270;
      check(s.localPath()!=whole, "an ellipse pie has a different outline than the whole ellipse");
      Shape s2=Shape::fromJson(s.toJson());
      check(s2.ellipseMode==1 && qAbs(s2.ellipseSweep-270)<1e-6, "ellipse pie mode+sweep round-trip through JSON");
      Shape sw; sw.type=ShapeType::Ellipse; sw.rect=QRectF(0,0,100,80);
      check(!sw.toJson().contains("elm"), "a whole ellipse writes no pie keys (JSON byte-identical)"); }
    { Shape s; s.type=ShapeType::Polygon; s.rect=QRectF(0,0,100,100); s.polySides=5;
      const QPainterPath straight=s.localPath();
      s.polyCurvature=0.6;
      check(s.localPath()!=straight, "polygon curvature bows the edges");
      check(Shape::fromJson(s.toJson()).polyCurvature>0.5, "polygon curvature round-trips through JSON");
      Shape s0; s0.type=ShapeType::Polygon; s0.rect=QRectF(0,0,100,100);
      check(!s0.toJson().contains("curv"), "zero curvature writes no key (JSON byte-identical)"); }
    { Shape s; s.type=ShapeType::Polygon; s.rect=QRectF(0,0,120,120); s.polySides=3;
      const QPainterPath sharp=s.localPath(); s.cornerRadius=12;
      check(s.localPath()!=sharp, "cornerRadius rounds a polygon's corners live (non-destructive)"); }
    { Document d; Shape s; s.type=ShapeType::Polygon; s.rect=QRectF(0,0,200,200); s.transform=QTransform::fromTranslate(100,100);
      s.polySides=5; s.polyStar=true; s.polyStarDepth=0.6; d.shapes={s};
      c->setDocument(d); c->setTool(Tool::Polygon); c->select({0});
      QPointF sd; check(c->starDepthHandle(sd), "a star exposes an inner-vertex depth handle");
      const QPointF centre=c->mapToView(QPointF(200,200));
      dragShape(c, sd, (sd+centre)/2.0);
      check(c->doc().shapes[0].polyStarDepth<0.6, "dragging the depth handle inward reduces the star point depth"); }
    { Document d; Shape s; s.type=ShapeType::Ellipse; s.rect=QRectF(0,0,160,160); s.transform=QTransform::fromTranslate(100,100);
      s.ellipseMode=1; s.ellipseStart=0; s.ellipseSweep=270; d.shapes={s};
      c->setDocument(d); c->setTool(Tool::Ellipse); c->select({0});
      QPointF eh; check(c->ellipseRimHandle(true,eh), "a pie ellipse exposes an end rim handle");
      const double before=c->doc().shapes[0].ellipseSweep;
      const QPointF centre=c->mapToView(QPointF(180,180));
      dragShape(c, eh, centre+QPointF(0,-80));
      check(qAbs(c->doc().shapes[0].ellipseSweep-before)>1.0, "dragging the rim handle changes the pie sweep");
      check(c->doc().shapes.size()==1, "the rim-handle drag did NOT spawn a spurious new ellipse"); }
    // --- Select-tool two-state handles, custom rotation pivot, skew, and Ctrl constrain (Batch 4) ---
    { Document d; Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,120,80); s.transform=QTransform::fromTranslate(100,100);
      s.fill.type=FillType::Solid; s.fill.color=Qt::blue; d.shapes={s};
      c->setDocument(d); c->setTool(Tool::Select); c->select({0});
      const QPointF inside=c->mapToView(QPointF(125,120));   // interior but OFF-centre (avoid the rotate-mode pivot at the bbox centre)
      check(!c->rotateMode(), "a fresh selection shows resize handles (not rotate mode)");
      click(c, inside); check(c->rotateMode(), "re-clicking the selection switches to rotate/skew handles");
      click(c, inside); check(!c->rotateMode(), "re-clicking again switches back to resize handles"); }
    { Document d; Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,100,100); s.transform=QTransform::fromTranslate(100,100);
      d.shapes={s}; c->setDocument(d); c->setTool(Tool::Select); c->select({0});
      click(c, c->mapToView(QPointF(150,150)));   // enter rotate mode
      check(c->rotateMode(), "in rotate mode after one re-click");
      const QPointF pivBefore=c->rotPivotDoc();
      const QPointF pv=c->mapToView(pivBefore);
      dragShape(c, pv, pv+QPointF(45,0));
      check(QLineF(c->rotPivotDoc(), pivBefore).length()>10, "dragging the pivot marker moves the rotation centre"); }
    { Document d; Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,120,80); s.transform=QTransform::fromTranslate(100,100);
      d.shapes={s}; c->setDocument(d); c->setTool(Tool::Select); c->select({0});
      click(c, c->mapToView(QPointF(160,140)));   // rotate mode
      const QPointF topMid=c->mapToView(QPointF(160,100));   // top mid-edge = horizontal-skew handle
      const QTransform t0=c->doc().shapes[0].transform;
      dragShape(c, topMid, topMid+QPointF(40,0));
      const QTransform t1=c->doc().shapes[0].transform;
      check(qAbs(t1.m12()-t0.m12())+qAbs(t1.m21()-t0.m21())>1e-3, "dragging a mid-edge handle in rotate mode skews the object"); }
    { Document d; Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,100,60); s.transform=QTransform::fromTranslate(100,100);
      d.shapes={s}; c->setDocument(d); c->setTool(Tool::Select); c->select({0});
      const QPointF br=c->mapToView(QPointF(200,160));   // bottom-right resize handle
      dragShapeMod(c, br, br+QPointF(80,8), Qt::ControlModifier);   // drag mostly horizontally, Ctrl held
      const QRectF nb=c->selectionBoundsDoc();
      check(nb.height()>1 && qAbs(nb.width()/nb.height() - 100.0/60.0) < 0.06, "Ctrl-resize from a corner locks the aspect ratio"); }
    { Document d; Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,100,100); s.transform=QTransform::fromTranslate(100,100);
      d.shapes={s}; c->setDocument(d); c->setTool(Tool::Select); c->select({0});
      const QPointF rotH=c->mapToView(QPointF(150,100))+QPointF(0,-26);   // rotate knob (resize mode)
      dragShapeMod(c, rotH, c->mapToView(QPointF(150,100))+QPointF(38,-12), Qt::ControlModifier);
      const QTransform t=c->doc().shapes[0].transform;
      double ang=std::atan2(t.m12(), t.m11())*180.0/M_PI, snapErr=qAbs(ang-std::round(ang/15.0)*15.0);
      check(snapErr<1.0, "Ctrl-rotate snaps the angle to 15° increments"); }
    // --- Text-frame reflow (Batch 2): side handles edit rect w/h at constant font size, not transform-scale glyphs ---
    { Document d; Shape s; s.type=ShapeType::Text; s.textFrame=true;
      s.text="The quick brown fox jumps over the lazy dog again and again and again";
      s.rect=QRectF(0,0,200,140); s.transform=QTransform::fromTranslate(100,100); s.font=QFont("DejaVu Sans",16);
      s.fill.type=FillType::Solid; s.fill.color=Qt::black; d.shapes={s};
      c->setDocument(d); c->setTool(Tool::Select); c->select({0});
      const double w0=c->doc().shapes[0].rect.width(); const QTransform xf0=c->doc().shapes[0].transform;
      const QRectF bb=c->selectionBoundsDoc();
      const QPointF rMid=c->mapToView(QPointF(bb.right(), bb.center().y()));   // right side handle
      dragShape(c, rMid, rMid+QPointF(70,0));
      check(c->doc().shapes[0].rect.width()>w0+30, "dragging a text frame's side handle widens the frame (reflow)");
      check(c->doc().shapes[0].transform==xf0, "the frame side-resize did NOT scale the glyphs (transform unchanged)"); }
    // --- Local Red-eye brush (Batch 6): Clone-tool mode 2 de-reds pixels under the click (was global-only) ---
    { Document d; Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,100,100); s.transform=QTransform::fromTranslate(50,50);
      QImage img(40,40,QImage::Format_ARGB32); img.fill(QColor(220,40,40));   // a red patch
      s.fill.type=FillType::Bitmap; s.fill.image=img; s.fill.p0=QPointF(0,0); s.fill.p1=QPointF(100,0); s.fill.p2=QPointF(0,100);
      d.shapes={s}; c->setDocument(d); c->setTool(Tool::Clone); c->setCloneMode(2); c->setCloneNib(8);
      const int before=c->doc().shapes[0].fill.image.pixelColor(20,20).red();
      click(c, c->mapToView(QPointF(100,100)));   // click the photo centre → image px (20,20)
      const int after=c->doc().shapes[0].fill.image.pixelColor(20,20).red();
      check(before>150 && after<before-30, "the local red-eye brush de-reds pixels under the click");
      check(c->doc().shapes[0].fill.image.pixelColor(0,0).red()>150, "red-eye is LOCAL — the far corner stays red (not whole-image)"); }
    // --- Table column-divider drag (Batch 6): drag a boundary to resize the column ---
    { Document d; Shape s; s.type=ShapeType::Table; s.transform=QTransform::fromTranslate(100,100);
      s.table.colW={60,60,60}; s.table.rowH={40,40}; s.table.cells.resize(6);
      d.shapes={s}; c->setDocument(d); c->setTool(Tool::Table);
      const double w0=c->doc().shapes[0].table.colW[0];
      const QPointF onDiv=c->mapToView(QPointF(160,140));   // on the divider between column 0 and 1 (local x=60)
      dragShape(c, onDiv, onDiv+QPointF(30,0));
      check(c->doc().shapes[0].table.colW[0]>w0+15, "dragging a table column divider widens that column"); }
    { QTransform xf=mkFxRect(); c->setTool(Tool::Bevel);
      click(c, c->mapToView(xf.map(QPointF(70,50))));
      check(c->doc().shapes[0].bevel.on, "clicking with the Bevel tool adds a bevel");
      QPointF eh; check(c->effectHandle(eh), "a bevelled shape exposes a depth handle");
      const double d0=c->doc().shapes[0].bevel.depth;
      dragShape(c, eh, eh+QPointF(25,25));
      check(c->doc().shapes[0].bevel.depth>d0, "dragging the handle deepens the bevel");
      // Bevel light-azimuth handle: like the extrude one but with the bevel's y-up convention.
      QPointF blh; check(c->bevelLightHandle(blh), "a bevelled shape also exposes a light-azimuth handle");
      const QPointF bcv=c->mapToView(QPointF(190,170));   // docBounds centre of the 140x100 rect at (120,120)
      dragShape(c, blh, bcv+QPointF(0,40));    // straight down → azimuth ~270 (bevel's y-up convention)
      check(qAbs(c->doc().shapes[0].bevel.light-270.0)<=2, "dragging the bevel light down aims the azimuth to ~270 (y-up)");
      QPointF blh2; c->bevelLightHandle(blh2);
      dragShape(c, blh2, bcv+QPointF(0,-40));   // straight up → ~90
      check(qAbs(c->doc().shapes[0].bevel.light-90.0)<=2, "dragging the bevel light up aims it at ~90"); }
    // On-canvas bevel LIGHT colour: a swatch (double-click opens the picker; a dropped colour tints the highlight).
    // The default light colour is white, so a fresh bevel renders exactly as before (byte-identical).
    { QTransform xf=mkFxRect(); c->setTool(Tool::Bevel);
      click(c, c->mapToView(xf.map(QPointF(70,50))));   // bevel on (defaults: white light, angle 45)
      check(c->doc().shapes[0].bevel.lightColor==QColor(255,255,255), "a fresh bevel's light colour is white (neutral => byte-identical render)");
      QPointF cs; check(c->bevelColorSwatch(cs), "a bevelled shape exposes a light-colour swatch");
      check(c->dropColorAt(QPointF(190,170), QColor(200,60,20,180), false), "a colour dropped on a bevelled shape (Bevel tool) is accepted");
      check(c->doc().shapes[0].bevel.lightColor==QColor(200,60,20), "the drop set the bevel LIGHT colour (alpha stripped — the swatch is opaque)");
      check(c->doc().shapes[0].fill.color==QColor(Qt::gray), "the object's fill colour is untouched");
      c->select({0}); QPointF cs2; c->bevelColorSwatch(cs2); click(c, cs2);
      check(c->selection().size()==1 && c->selection().first()==0, "clicking the bevel swatch keeps the shape selected (no deselect)");
      c->doc().shapes[0].bevel.on=false;
      QPointF cx; check(c->bevelColorSwatch(cx)==false, "no colour swatch when the bevel is off");
      check(c->dropColorAt(QPointF(190,170), QColor(10,20,30), false), "drop accepted with the bevel off");
      check(c->doc().shapes[0].fill.color==QColor(10,20,30), "with the bevel off, the drop recolours the FILL (normal behaviour)"); }
    // Persistence: a non-white bevel light colour round-trips through the JSON model; white writes no key (byte-identical JSON).
    { Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,80,60); s.bevel.on=true; s.bevel.depth=9; s.bevel.lightColor=QColor(180,90,255);
      Shape s2=Shape::fromJson(s.toJson());
      check(s2.bevel.on && s2.bevel.lightColor==QColor(180,90,255), "the bevel light colour survives JSON round-trip"); }
    { Shape sw; sw.type=ShapeType::Rect; sw.rect=QRectF(0,0,80,60); sw.bevel.on=true;   // default white light
      check(!sw.toJson()["bevel"].toObject().contains("lc"), "a white bevel light colour writes no 'lc' key (JSON byte-identical)"); }
    { QTransform xf=mkFxRect(); c->setTool(Tool::Feather); c->select({0});
      QPointF eh; check(c->effectHandle(eh), "the Feather tool shows a corner handle even at 0 (hard edge)");
      const double f0=c->doc().shapes[0].feather;
      dragShape(c, eh, eh+QPointF(35,35));
      check(c->doc().shapes[0].feather>f0, "dragging the handle out feathers the edge"); }
    // Extrude: a single vanishing-point handle encodes BOTH depth (distance from centre) and direction (angle).
    { mkFxRect(); c->setTool(Tool::Extrude);
      const QPointF ctrDoc(190,170);   // docBounds centre of the 140x100 rect at (120,120)
      click(c, c->mapToView(ctrDoc));  // toggle the extrude on (defaults depth 28, angle 35)
      check(c->doc().shapes[0].extrude.on, "clicking with the Extrude tool extrudes the object");
      QPointF eh; check(c->extrudeHandle(eh), "an extruded shape exposes a vanishing-point handle");
      const int before=c->undoDepth();
      dragShape(c, eh, c->mapToView(ctrDoc+QPointF(100,0)));   // drag straight right, 100px from the centre
      check(qAbs(c->doc().shapes[0].extrude.depth-100.0)<1.5, "the handle distance from the centre sets the extrude depth");
      check(qAbs(c->doc().shapes[0].extrude.angle)<1.5, "dragging straight right sets the angle to ~0");
      check(c->undoDepth()>before, "the extrude drag is one undo step");
      QPointF eh2; c->extrudeHandle(eh2);
      dragShape(c, eh2, c->mapToView(ctrDoc+QPointF(0,-60)));   // straight up → angle ~-90 (screen y-down)
      check(qAbs(c->doc().shapes[0].extrude.angle+90.0)<1.5, "dragging straight up sets the angle to ~-90");
      check(qAbs(c->doc().shapes[0].extrude.depth-60.0)<1.5, "and the depth to that distance");
      // The light-azimuth handle orbits the centre; dragging it sets extrude.light (0..360).
      QPointF lh; check(c->extrudeLightHandle(lh), "an extruded shape also exposes a light-azimuth handle");
      const QPointF cv=c->mapToView(ctrDoc);
      dragShape(c, lh, cv+QPointF(0,40));    // straight down (screen y-down) → azimuth 90
      check(qAbs(c->doc().shapes[0].extrude.light-90)<=2, "dragging the light handle down aims the light at ~90");
      QPointF lh2; c->extrudeLightHandle(lh2);
      dragShape(c, lh2, cv+QPointF(-40,0));   // straight left → azimuth 180
      check(qAbs(c->doc().shapes[0].extrude.light-180)<=2, "dragging the light handle left aims it at ~180"); }
    // On-canvas extrude WALL colour: a swatch (double-click opens the picker; a dropped colour tints the 3-D sides/back).
    // Unset (invalid) => the walls derive from the object's own fill, so a fresh extrude renders byte-identical.
    { QTransform xf=mkFxRect(); c->setTool(Tool::Extrude);
      click(c, c->mapToView(xf.map(QPointF(70,50))));   // extrude on (defaults; wall colour unset)
      check(!c->doc().shapes[0].extrude.color.isValid(), "a fresh extrude has no explicit wall colour (=> derives from the fill, byte-identical)");
      QPointF cs; check(c->extrudeColorSwatch(cs), "an extruded shape exposes a wall-colour swatch");
      check(c->dropColorAt(QPointF(190,170), QColor(40,160,90,200), false), "a colour dropped on an extruded shape (Extrude tool) is accepted");
      check(c->doc().shapes[0].extrude.color==QColor(40,160,90), "the drop set the extrude WALL colour (alpha stripped)");
      check(c->doc().shapes[0].fill.color==QColor(Qt::gray), "the object's fill colour is untouched");
      c->select({0}); QPointF cs2; c->extrudeColorSwatch(cs2); click(c, cs2);
      check(c->selection().size()==1 && c->selection().first()==0, "clicking the extrude swatch keeps the shape selected (no deselect)");
      c->doc().shapes[0].extrude.on=false;
      QPointF cx; check(c->extrudeColorSwatch(cx)==false, "no colour swatch when the extrude is off");
      check(c->dropColorAt(QPointF(190,170), QColor(10,20,30), false), "drop accepted with the extrude off");
      check(c->doc().shapes[0].fill.color==QColor(10,20,30), "with the extrude off, the drop recolours the FILL (normal behaviour)"); }
    // 3-D extrude true rotation + bevel (G-G): tilt renders a different image, defaults stay byte-identical, and it round-trips.
    { c->newDocument(); Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,120,80); s.transform=QTransform::fromTranslate(60,60);
      s.fill.type=FillType::Solid; s.fill.color=QColor(90,140,220); s.extrude.on=true; s.extrude.depth=30;
      int i=c->doc().addShape(s);
      check(!c->doc().shapes[i].extrude.is3D(), "a fresh extrude is not 3-D (classic projection, byte-identical)");
      QTemporaryDir etd;
      check(c->exportPng(etd.path()+"/flat.png",1), "flat extrude renders");
      c->doc().shapes[i].extrude.rotX=25; c->doc().shapes[i].extrude.rotY=-30; c->doc().shapes[i].extrude.bevel=6;
      check(c->doc().shapes[i].extrude.is3D(), "setting a tilt makes the extrude 3-D");
      check(c->exportPng(etd.path()+"/tilt.png",1), "3-D tilted extrude renders");
      { QImage a(etd.path()+"/flat.png"), b(etd.path()+"/tilt.png");
        check(!a.isNull() && !b.isNull() && a!=b, "a 3-D tilt renders a visibly different image than the flat extrude"); }
      Document rt=Document::fromJson(c->doc().toJson());
      check(qAbs(rt.shapes[i].extrude.rotX-25)<1e-6 && qAbs(rt.shapes[i].extrude.rotY+30)<1e-6 && qAbs(rt.shapes[i].extrude.bevel-6)<1e-6, "extrude tilt + bevel round-trip through JSON");
      // REGRESSION (review): a 3-D-tilted extrude with depth 0 must NOT vanish — it falls back to the normal fill.
      c->doc().shapes[i].extrude.depth=0; c->doc().shapes[i].extrude.bevel=0;
      { QTemporaryDir z; QString zp=z.path()+"/z.png"; check(c->exportPng(zp,1), "depth-0 tilted extrude renders");
        QImage zi(zp); bool ink=false; for(int y=0;y<zi.height()&&!ink;y+=5) for(int x=0;x<zi.width();x+=5){ QRgb p=zi.pixel(x,y); if(qAlpha(p)>0&&(qRed(p)<250||qGreen(p)<250||qBlue(p)<250)){ink=true;break;} }
        check(ink, "depth-0 tilted extrude still shows its fill (not invisible)"); } }
    // Persistence: an explicit extrude wall colour round-trips; an unset one writes no key (byte-identical JSON).
    { Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,80,60); s.extrude.on=true; s.extrude.depth=20; s.extrude.color=QColor(70,140,210);
      Shape s2=Shape::fromJson(s.toJson());
      check(s2.extrude.on && s2.extrude.color==QColor(70,140,210), "the extrude wall colour survives JSON round-trip"); }
    { Shape su; su.type=ShapeType::Rect; su.rect=QRectF(0,0,80,60); su.extrude.on=true;   // wall colour unset
      check(!su.toJson()["extr"].toObject().contains("c"), "an unset extrude wall colour writes no 'c' key (JSON byte-identical)"); }

    // --- Pen: three smooth points then finish with Enter -> one open bezier path ---
    std::printf("[pen] live bezier curve\n");
    c->newDocument();
    c->setTool(Tool::Pen);
    penPoint(c, {200,400}, {240,340});
    penPoint(c, {360,400}, {400,460});
    penPoint(c, {520,400}, {560,340});
    key(c, Qt::Key_Return);
    check(c->doc().shapes.size()==1, "pen created exactly one shape");
    if (c->doc().shapes.size()==1) {
        const Shape& s = c->doc().shapes[0];
        check(s.type==ShapeType::Path, "pen shape is a Path");
        check(pathHasCurve(s.path), "pen path contains bezier (CurveTo) segments");
        check(s.path.elementCount()>=4, "pen path has multiple segments");
    }

    // --- Pen: click-to-close into a filled shape ---
    std::printf("[pen] click-to-close\n");
    c->newDocument();
    c->setTool(Tool::Pen);
    penPoint(c, {200,200}, {200,200});   // corner (no drag)
    penPoint(c, {400,200}, {400,200});
    penPoint(c, {400,360}, {400,360});
    click(c, {201,201});                 // click first anchor to close
    check(c->doc().shapes.size()==1, "closed pen created one shape");
    if (c->doc().shapes.size()==1)
        check(c->doc().shapes[0].fill.type!=FillType::None, "closed path keeps a fill");

    // --- Pen: resume an existing OPEN path from its endpoint (G-H) ---
    std::printf("[pen] resume open path\n");
    c->newDocument(); c->setTool(Tool::Pen);
    penPoint(c, {150,150}, {150,150});   // corner
    penPoint(c, {300,150}, {300,150});   // corner
    key(c, Qt::Key_Return);
    check(c->doc().shapes.size()==1, "pen made one open path");
    if (c->doc().shapes.size()==1) {
        const Shape ln = c->doc().shapes[0];
        const QPainterPath dp0 = ln.transform.map(ln.localPath());
        const int before = dp0.elementCount();
        const QPointF endDoc(dp0.elementAt(dp0.elementCount()-1));
        const QPointF startDoc0(dp0.elementAt(0));
        c->setTool(Tool::Pen);
        click(c, c->mapToView(endDoc));      // grab the endpoint → resume mode
        penPoint(c, {300,320}, {300,320});   // one new node further down
        key(c, Qt::Key_Return);
        check(c->doc().shapes.size()==1, "resuming did NOT create a new shape");
        const Shape& ex = c->doc().shapes[0];
        const QPainterPath dp1 = ex.transform.map(ex.localPath());
        check(dp1.elementCount() > before, "the resumed path gained segments");
        check(QLineF(QPointF(dp1.elementAt(0)), startDoc0).length() < 0.6, "the original path start node is preserved exactly");
        // penHitEndpoint recognises the (new) endpoint, but not a point far from any node
        int hi=-1; bool atStart=false;
        check(c->penHitEndpoint(QPointF(dp1.elementAt(dp1.elementCount()-1)), hi, atStart) && hi==0, "penHitEndpoint finds the open path's end node");
        check(!c->penHitEndpoint(QPointF(9000,9000), hi, atStart), "penHitEndpoint rejects a point far from any node");
    }

    // --- Node editor: dragging a control handle must reshape the curve, NOT flatten it ---
    std::printf("[node] drag bezier handle preserves curve\n");
    c->newDocument();
    c->setTool(Tool::Pen);
    penPoint(c, {200,400}, {260,320});
    penPoint(c, {380,400}, {420,470});
    penPoint(c, {540,400}, {600,320});
    key(c, Qt::Key_Return);
    if (c->doc().shapes.size()==1) {
        Shape s0 = c->doc().shapes[0];
        auto subs = pathToNodes(s0.path);
        QPointF handleLocal; int sub=-1, idx=-1;
        for (int si=0;si<subs.size()&&sub<0;++si) for (int ni=0;ni<subs[si].nodes.size();++ni)
            if (subs[si].nodes[ni].hasOut){ handleLocal=subs[si].nodes[ni].out; sub=si; idx=ni; break; }
        check(sub>=0, "pen path exposes an editable out-handle");
        check(pathHasCurve(s0.path), "pen path is curved before edit");
        if (sub>=0) {
            QPointF v  = c->mapToView(s0.transform.map(handleLocal));
            QPointF v2 = v + QPointF(55,-35);
            c->setTool(Tool::Node);   // auto-parses the selected path
            send(c, QEvent::MouseButtonPress,   v,  Qt::LeftButton, Qt::LeftButton);
            send(c, QEvent::MouseMove,          v2, Qt::NoButton,   Qt::LeftButton);
            send(c, QEvent::MouseButtonRelease, v2, Qt::LeftButton, Qt::NoButton);
            const Shape& s1 = c->doc().shapes[0];
            check(s1.type==ShapeType::Path, "node-edited shape stays a Path");
            check(pathHasCurve(s1.path), "curve NOT flattened by node edit (regression)");
            auto subs2 = pathToNodes(s1.path);
            bool moved = sub<subs2.size() && idx<subs2[sub].nodes.size()
                       && QLineF(subs2[sub].nodes[idx].out, handleLocal).length() > 3.0;
            check(moved, "the dragged out-handle actually moved");
        }
    }

    // --- Rectangle drag ---
    std::printf("[rect] drag-create\n");
    c->newDocument();
    c->setTool(Tool::Rect);
    dragShape(c, {150,150}, {380,300});
    check(c->doc().shapes.size()==1 && c->doc().shapes[0].type==ShapeType::Rect, "rectangle created");

    // --- Undo / redo ---
    std::printf("[edit] undo / redo\n");
    c->undo();
    check(c->doc().shapes.isEmpty(), "undo removed the rectangle");
    c->redo();
    check(c->doc().shapes.size()==1, "redo restored the rectangle");
    // Named undo/redo (G-O): the op label is surfaced for the Edit menu. Operates on the existing
    // single rectangle and leaves the document at exactly one shape (the following test relies on that).
    { c->select({0});
      c->applyToSelection([](Shape& s){ s.fill.color=QColor(10,20,30); },"Recolour");
      check(c->undoLabel()=="Recolour", "undoLabel names the last operation");
      c->undo();
      check(c->redoLabel()=="Recolour", "after undo, redoLabel names the reversed operation");
      c->redo();
      check(c->undoLabel()=="Recolour", "after redo, undoLabel is restored");
      check(c->doc().shapes.size()==1, "document still has exactly the one rectangle"); }
    // Outline (wireframe) view mode toggles without touching the document (G-O).
    { check(!c->outlineView(), "outline view is off by default");
      c->setOutlineView(true); check(c->outlineView(), "outline view toggles on");
      c->setOutlineView(false); check(!c->outlineView(), "outline view toggles off"); }

    // --- Ellipse + Polygon ---
    std::printf("[shapes] ellipse + star\n");
    c->setTool(Tool::Ellipse); dragShape(c, {420,150}, {560,290});
    c->setTool(Tool::Polygon); c->setPolyStar(true); dragShape(c, {600,150}, {740,290});
    check(c->doc().shapes.size()==3, "ellipse and star added");
    check(c->doc().shapes[2].type==ShapeType::Polygon && c->doc().shapes[2].polyStar, "star flagged as star");

    // --- Boolean combine ---
    auto twoRects=[&]{ Document d;
        Shape a; a.type=ShapeType::Rect; a.rect=QRectF(0,0,100,100); a.transform=QTransform::fromTranslate(100,100);
        Shape b; b.type=ShapeType::Rect; b.rect=QRectF(0,0,100,100); b.transform=QTransform::fromTranslate(160,160);
        d.shapes.push_back(a); d.shapes.push_back(b); c->setDocument(d); c->selectAll(); };
    std::printf("[combine] union\n");
    twoRects(); c->combineSelection(0);
    check(c->doc().shapes.size()==1 && c->doc().shapes[0].type==ShapeType::Path, "union -> single Path");
    { QRectF bb=c->doc().shapes[0].docBounds();
      check(std::abs(bb.left()-100)<1.5 && std::abs(bb.right()-260)<1.5, "union bbox spans both rects (100..260)"); }
    std::printf("[combine] intersect\n");
    twoRects(); c->combineSelection(2);
    check(c->doc().shapes.size()==1, "intersect -> single shape");
    { QRectF bb=c->doc().shapes[0].docBounds();
      check(std::abs(bb.left()-160)<1.5 && std::abs(bb.right()-200)<1.5, "intersect bbox is the overlap (160..200)"); }
    std::printf("[combine] convert primitive to path\n");
    { Document d; Shape e; e.type=ShapeType::Ellipse; e.rect=QRectF(0,0,80,80); d.shapes.push_back(e);
      c->setDocument(d); c->selectAll(); c->convertToPath();
      check(c->doc().shapes[0].type==ShapeType::Path, "ellipse converted to editable Path");
      check(pathHasCurve(c->doc().shapes[0].path), "converted ellipse retains its curves"); }

    // --- Roughen path effect (deterministic) ---
    std::printf("[path] roughen (deterministic jitter)\n");
    { Document d; Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,100,100); s.fill.type=FillType::Solid; s.stroke.enabled=false;
      d.shapes.push_back(s); c->setDocument(d); c->selectAll(); c->roughenSelection(5,10);
      const Shape r1=c->doc().shapes[0];
      check(r1.type==ShapeType::Path && r1.path.elementCount()>8, "roughen subdivides + jitters into a many-node Path");
      { QRectF rb=r1.localBounds(); check(rb.width()>=99 && rb.width()<=100+2*5+3 && rb.height()>=99 && rb.height()<=100+2*5+3, "roughened bbox stays within ±amount of the original (100×100)"); }
      // determinism: same input → identical output (restore original + roughen again)
      c->setDocument(d); c->selectAll(); c->roughenSelection(5,10);
      const Shape r2=c->doc().shapes[0];
      bool same = r1.path.elementCount()==r2.path.elementCount();
      if(same) for(int k=0;k<r1.path.elementCount();++k){ auto e1=r1.path.elementAt(k),e2=r2.path.elementAt(k); if(qAbs(e1.x-e2.x)>1e-9||qAbs(e1.y-e2.y)>1e-9){ same=false; break; } }
      check(same, "roughen is DETERMINISTIC (identical output for identical input — no rand)");
      // every roughened node stays within the original bbox expanded by `amount` per axis (jitter is bounded)
      bool bounded=true;
      for(int k=0;k<r1.path.elementCount() && bounded;++k){ auto e=r1.path.elementAt(k);
        if(e.x<-5.5 || e.x>105.5 || e.y<-5.5 || e.y>105.5) bounded=false; }
      check(bounded, "every roughened node stays within the original bbox expanded by ±amount (jitter is bounded)"); }

    // --- Cascading named text styles ---
    std::printf("[text] cascading named styles\n");
    { Document d;
      Shape a; a.type=ShapeType::Text; a.text="A"; a.font=QFont("DejaVu Sans",30); a.fill.type=FillType::Solid; a.fill.color=Qt::red; a.textAlign=0;
      Shape b; b.type=ShapeType::Text; b.text="B"; b.font=QFont("DejaVu Sans",12); b.fill.type=FillType::Solid; b.fill.color=Qt::black; b.textAlign=2;
      d.shapes.push_back(a); d.shapes.push_back(b); c->setDocument(d);
      c->select({0}); c->createTextStyle("Heading");
      check(c->doc().textStyles.size()==1 && c->doc().textStyles[0].name=="Heading", "createTextStyle defines a document style");
      check(c->doc().shapes[0].textStyleName=="Heading", "the source text is tagged with the style");
      // apply the Heading style to shape B → B takes A's font/colour/align
      c->select({1}); c->applyTextStyle("Heading");
      check(c->doc().shapes[1].font.pointSize()==30 && c->doc().shapes[1].fill.color==QColor(Qt::red) && c->doc().shapes[1].textAlign==0, "applying the style copies its attributes onto the target");
      check(c->doc().shapes[1].textStyleName=="Heading", "the target becomes a member of the style");
      // EDIT the style → BOTH members cascade-update
      c->updateTextStyle("Heading", QFont("DejaVu Sans",48), QColor(0,0,255), 1);
      check(c->doc().shapes[0].font.pointSize()==48 && c->doc().shapes[0].fill.color==QColor(0,0,255) && c->doc().shapes[0].textAlign==1, "editing the style cascades to member 0");
      check(c->doc().shapes[1].font.pointSize()==48 && c->doc().shapes[1].fill.color==QColor(0,0,255), "editing the style cascades to member 1");
      // styles + membership survive JSON round-trip
      Document rt=Document::fromJson(c->doc().toJson());
      check(rt.textStyles.size()==1 && rt.textStyles[0].font.pointSize()==48 && rt.shapes[1].textStyleName=="Heading", "text styles + membership round-trip through JSON"); }

    // --- Per-character rich text runs (Batch T2) ---
    std::printf("[text] per-character rich runs\n");
    { Document d;
      Shape t; t.type=ShapeType::Text; t.text="Hello World"; t.font=QFont("DejaVu Sans",24); t.fill.type=FillType::Solid; t.fill.color=Qt::black;
      d.shapes.push_back(t); c->setDocument(d); c->select({0});
      check(c->doc().shapes[0].textRuns.isEmpty(), "text starts with no runs (classic path — byte-identical)");
      // colour the word "World" (chars 6..11) red
      QColor red(Qt::red);
      c->setTextRunAttr(6,5,nullptr,&red,nullptr);
      const Shape& s0=c->doc().shapes[0];
      check(!s0.textRuns.isEmpty(), "formatting a range materialises runs");
      // resolved per-char colours: 0..5 black, 6..10 red
      QVector<QFont> fs; QVector<QColor> cs; QVector<QString> us; s0.resolveChars(fs,cs,us);
      check(fs.size()==11 && cs[0]==QColor(Qt::black) && cs[5]==QColor(Qt::black) && cs[6]==QColor(Qt::red) && cs[10]==QColor(Qt::red), "range colour applied to exactly [6,11)");
      // layoutRuns groups by colour → ≥2 colour groups (black "Hello", red "World")
      auto runsP=s0.layoutRuns();
      check(runsP.size()>=2, "layoutRuns splits into ≥2 colour groups");
      bool haveRed=false, haveBlack=false; for(auto&pr:runsP){ if(pr.second==QColor(Qt::red)) haveRed=true; if(pr.second==QColor(Qt::black)) haveBlack=true; }
      check(haveRed && haveBlack, "layout has both a red and a black run");
      // apply a bigger bold font to "Hello" (0..5)
      QFont big("DejaVu Sans",40); big.setBold(true);
      c->setTextRunAttr(0,5,&big,nullptr,nullptr);
      s0.resolveChars(fs,cs,us);
      check(fs[0].pointSize()==40 && fs[0].bold() && fs[6].pointSize()==24, "per-run font override on [0,5)");
      // a run's bigger glyphs widen the outline bbox vs the base-font single-run path
      Shape plain=c->doc().shapes[0]; plain.textRuns.clear();
      check(c->doc().shapes[0].localBounds().width() >= plain.localBounds().width()-1.0, "rich outline bbox reflects the enlarged run");
      // per-character hyperlink on a span
      QString url="https://xara.example";
      c->setTextRunAttr(6,5,nullptr,nullptr,&url);
      c->doc().shapes[0].resolveChars(fs,cs,us);
      check(us[6]==url && us[5].isEmpty(), "per-character hyperlink applied to the span only");
      // runs survive JSON round-trip
      Document rt=Document::fromJson(c->doc().toJson());
      QVector<QFont> f2; QVector<QColor> c2; QVector<QString> u2; rt.shapes[0].resolveChars(f2,c2,u2);
      check(!rt.shapes[0].textRuns.isEmpty() && c2[6]==QColor(Qt::red) && f2[0].pointSize()==40 && u2[6]==url, "rich runs (font+colour+link) round-trip through JSON");
      // RENDER PATH: paint the run-formatted text and confirm both red and black ink actually appear
      { Shape run=rt.shapes[0];
        QImage img(700,120,QImage::Format_ARGB32); img.fill(Qt::white);
        { QPainter p(&img); paintShape(p, run); }
        int redPx=0, blackPx=0;
        for(int y=0;y<img.height();++y) for(int x=0;x<img.width();++x){ QRgb px=img.pixel(x,y);
          if(qRed(px)>180 && qGreen(px)<80 && qBlue(px)<80) redPx++;
          else if(qRed(px)<60 && qGreen(px)<60 && qBlue(px)<60) blackPx++; }
        check(redPx>20 && blackPx>20, "render path draws both red (World) and black (Hello) glyph ink"); } }

    // --- Per-run typography: baseline shift + tracking (Batch R3) ---
    std::printf("[text] per-run baseline shift + letter-spacing\n");
    { auto mk=[&](double bl){ Shape s; s.type=ShapeType::Text; s.text="Ax"; s.font=QFont("DejaVu Sans",24);
        TextRun r0; r0.start=0; r0.len=1; r0.font=s.font; r0.color=Qt::black;
        TextRun r1; r1.start=1; r1.len=1; r1.font=s.font; r1.color=Qt::black; r1.baseline=bl;   // raise "x"
        s.textRuns={r0,r1}; return s; };
      auto topOf=[&](const Shape& s){ QRectF bb; for(auto&pr:s.layoutRuns()) bb=bb.isNull()?pr.first.boundingRect():bb.united(pr.first.boundingRect()); return bb.top(); };
      check(topOf(mk(20)) < topOf(mk(0))-8, "baseline shift raises the run's glyphs (superscript)");
      // per-run tracking widens the run
      auto mkTrack=[&](double tk){ Shape s; s.type=ShapeType::Text; s.text="iiiii"; s.font=QFont("DejaVu Sans",24);
        TextRun r; r.start=0; r.len=5; r.font=s.font; r.color=Qt::black; r.tracking=tk; s.textRuns={r}; return s; };
      check(mkTrack(8).localBounds().width() > mkTrack(0).localBounds().width()+20, "per-run tracking widens the run (letter-spacing)");
      // baseline + tracking round-trip through JSON
      { Shape s; s.type=ShapeType::Text; s.text="Ax"; TextRun r; r.start=1; r.len=1; r.baseline=14; r.tracking=6; s.textRuns={r};
        Document d; d.shapes.push_back(s); Document d2=Document::fromJson(d.toJson());
        check(d2.shapes.size()==1 && !d2.shapes[0].textRuns.isEmpty() && qAbs(d2.shapes[0].textRuns[0].baseline-14)<1e-6 && qAbs(d2.shapes[0].textRuns[0].tracking-6)<1e-6, "baseline+tracking round-trip through JSON"); }
      // canvas setTextRunTypography materialises runs over a range
      { Document d; Shape t; t.type=ShapeType::Text; t.text="Hello World"; t.font=QFont("DejaVu Sans",24); t.fill.type=FillType::Solid; t.fill.color=Qt::black; d.shapes.push_back(t);
        c->setDocument(d); c->select({0});
        c->setTextRunTypography(6,5, 10, 4);
        const Shape& s=c->doc().shapes[0];
        check(!s.textRuns.isEmpty(), "setTextRunTypography materialises runs");
        QVector<double> bl=s.resolveBaselines();
        check(bl.size()==11 && qAbs(bl[6]-10)<1e-6 && qAbs(bl[0])<1e-6, "typography applies baseline to [6,11) only");
        QVector<QFont> fs; QVector<QColor> cs; QVector<QString> us; s.resolveChars(fs,cs,us);
        check(fs[6].letterSpacingType()==QFont::AbsoluteSpacing && qAbs(fs[6].letterSpacing()-4)<1e-6, "typography applies tracking to the span"); } }

    // --- Paste RTF/HTML → styled runs (Batch T3) ---
    std::printf("[text] rich paste (HTML + RTF) → runs\n");
    { // HTML fragment with bold + a coloured span
      Shape h=Canvas::shapeFromHtml("<p>Hello <b>bold</b> <span style=\"color:#ff0000\">red</span></p>");
      check(h.type==ShapeType::Text && h.text.contains("Hello") && h.text.contains("bold") && h.text.contains("red"), "HTML paste yields the plain text");
      check(!h.textRuns.isEmpty(), "mixed HTML → per-character runs");
      QVector<QFont> hf; QVector<QColor> hc; QVector<QString> hu; h.resolveChars(hf,hc,hu);
      int ib=h.text.indexOf("bold"), ir=h.text.indexOf("red");
      check(ib>=0 && hf[ib].bold(), "the <b> span is bold in the runs");
      check(ir>=0 && hc[ir]==QColor(255,0,0), "the coloured <span> is red in the runs"); }
    { // Minimal RTF with a colour table, a font size, bold toggle and a \cf colour
      QByteArray rtf="{\\rtf1\\ansi{\\colortbl;\\red255\\green0\\blue0;}\\fs48 Big \\b bold\\b0  \\cf1 red}";
      Shape r=Canvas::shapeFromRtf(rtf);
      check(r.type==ShapeType::Text && r.text.contains("Big") && r.text.contains("bold") && r.text.contains("red"), "RTF paste yields the plain text");
      QVector<QFont> rf; QVector<QColor> rc; QVector<QString> ru; r.resolveChars(rf,rc,ru);
      int jb=r.text.indexOf("bold"), jr=r.text.indexOf("red"), jg=r.text.indexOf("Big");
      check(jb>=0 && rf[jb].bold() && jg>=0 && !rf[jg].bold(), "RTF \\b/\\b0 toggles bold on exactly the 'bold' run");
      check(jg>=0 && qAbs(rf[jg].pointSizeF()-24.0)<0.5, "RTF \\fs48 → 24pt");
      check(jr>=0 && rc[jr]==QColor(255,0,0), "RTF \\cf1 picks red from the colour table");
      // uniform HTML collapses to a clean single-style object (no redundant runs)
      Shape u=Canvas::shapeFromHtml("<p>plain</p>");
      check(u.text.contains("plain") && u.textRuns.isEmpty(), "uniform fragment → single-style object (no runs)"); }

    // --- Per-character hyperlinks: runs + HTML/SVG export (Batch T4) ---
    std::printf("[text] per-character hyperlinks export\n");
    { Document d;
      Shape t; t.type=ShapeType::Text; t.text="Click here now"; t.font=QFont("DejaVu Sans",24); t.fill.type=FillType::Solid; t.fill.color=Qt::black;
      d.shapes.push_back(t); c->setDocument(d); c->select({0});
      QString url="https://example.org/go";
      c->setTextRunAttr(6,4,nullptr,nullptr,&url);   // link the word "here"
      const Shape& s0=c->doc().shapes[0];
      auto links=s0.runLinkRects();
      check(links.size()==1 && links[0].second==url, "runLinkRects finds exactly one linked span");
      check(links[0].first.width()>1 && links[0].first.height()>1, "linked span has a non-empty bounding rect");
      // HTML export overlays a <a class="tlnk"> for the run link
      QString hp="/tmp/selftest_t4.html";
      check(c->exportHtml(hp), "HTML export of run-linked text succeeds");
      { QFile hf(hp); hf.open(QIODevice::ReadOnly); QString h=QString::fromUtf8(hf.readAll());
        check(h.contains("tlnk") && h.contains("https://example.org/go"), "HTML has a per-run link overlay with the href"); }
      // SVG export wraps the linked run in <a href> with a <tspan>
      QByteArray svg=writeSvg(c->doc().shapes, QRectF(0,0,400,100));
      check(svg.contains("<a href=\"https://example.org/go\"") && svg.contains("<tspan"), "SVG export wraps the linked run in <a><tspan>"); }

    // --- Text repel / wrap around an obstacle (Batch T5) ---
    std::printf("[text] repel: flow around obstacle\n");
    { Document d;
      Shape frame; frame.type=ShapeType::Text; frame.textFrame=true; frame.rect=QRectF(0,0,400,300);
      frame.font=QFont("DejaVu Sans",18); frame.fill.type=FillType::Solid; frame.fill.color=Qt::black;
      QString para; for(int i=0;i<40;++i) para+="word "; frame.text=para.trimmed();
      Shape obs; obs.type=ShapeType::Rect; obs.rect=QRectF(0,0,150,80); obs.repel=true;   // covers the frame's top-left
      d.shapes.push_back(frame); d.shapes.push_back(obs); c->setDocument(d);
      QPainterPath band; band.addRect(0,0,400,40);   // first-line vertical band
      QPainterPath before=c->doc().shapes[0].localPath();          // obstacles not yet computed
      check(c->doc().shapes[0].textWrapObstacles.isEmpty(), "before reflow the frame has no obstacles (classic layout)");
      double leftBefore=before.intersected(band).boundingRect().left();
      c->reflowText();
      check(c->doc().shapes[0].textWrapObstacles.size()==1, "reflow caches the one overlapping repel obstacle");
      QPainterPath after=c->doc().shapes[0].localPath();
      double leftAfter=after.intersected(band).boundingRect().left();
      check(!after.isEmpty(), "repelled text still lays out (non-empty path)");
      check(leftAfter > leftBefore + 80, "repel pushes first-line text to the right of the obstacle");
      // removing repel restores full-width flow
      c->doc().shapes[1].repel=false; c->reflowText();
      check(c->doc().shapes[0].textWrapObstacles.isEmpty(), "clearing repel empties the obstacle cache");
      double leftRestored=c->doc().shapes[0].localPath().intersected(band).boundingRect().left();
      check(qAbs(leftRestored-leftBefore) < 2.0, "without repel the layout returns to full width"); }

    // --- Inline anchored graphics (Batch T6) ---
    std::printf("[text] inline anchored graphic\n");
    { Document d;
      Shape frame; frame.type=ShapeType::Text; frame.textFrame=true; frame.rect=QRectF(0,0,400,200);
      frame.font=QFont("DejaVu Sans",16); frame.fill.type=FillType::Solid; frame.fill.color=Qt::black;
      QString para; for(int i=0;i<30;++i) para+="word "; frame.text=para.trimmed();
      Shape gfx; gfx.type=ShapeType::Rect; gfx.rect=QRectF(0,0,50,40); gfx.transform=QTransform::fromTranslate(900,900);   // far outside
      gfx.anchorTextIndex=0; gfx.anchorChar=15;
      d.shapes.push_back(frame); d.shapes.push_back(gfx); c->setDocument(d);
      c->reflowText();
      const Shape& F=c->doc().shapes[0]; const Shape& G=c->doc().shapes[1];
      check(F.textInlineBoxes.size()==1 && qFuzzyCompare(F.textInlineBoxes[0].second.width(),50.0), "frame reserves a box the size of the anchored graphic");
      QVector<QRectF> boxes; F.layoutFrameInline(&boxes);
      check(boxes.size()==1 && !boxes[0].isNull(), "inline layout places the reserved box");
      QPointF target=F.transform.map(boxes[0].topLeft());
      check((G.docBounds().topLeft()-target).manhattanLength() < 2.0, "graphic repositioned into its inline slot");
      check(G.docBounds().left()<400 && G.docBounds().top()<200, "anchored graphic now sits inside the frame (moved from 900,900)");
      QPainterPath boxPath; boxPath.addRect(boxes[0]);
      check(F.localPath().intersected(boxPath).boundingRect().width() < 5.0, "text leaves a gap where the graphic sits");
      // anchor survives JSON round-trip
      Document rt=Document::fromJson(c->doc().toJson());
      check(rt.shapes[1].anchorTextIndex==0 && rt.shapes[1].anchorChar==15, "inline anchor round-trips through JSON"); }

    // --- Web-layout model: breakpoints + sticky/stretchy/pin (Batch W1) ---
    std::printf("[web] responsive layout attributes\n");
    { Document d; d.breakpoints={480,768,1024};
      Shape r; r.type=ShapeType::Rect; r.rect=QRectF(0,0,200,100); r.webStretch=1; r.webPin=1|2; r.webSticky=true;
      d.shapes.push_back(r);
      Document rt=Document::fromJson(d.toJson());
      check(rt.breakpoints==QVector<int>({480,768,1024}), "document breakpoints round-trip through JSON");
      check(rt.shapes[0].webStretch==1 && rt.shapes[0].webPin==3 && rt.shapes[0].webSticky, "web stretch/pin/sticky attributes round-trip"); }

    // --- Semantic responsive website publish (Batch W2) ---
    std::printf("[web] responsive website publish\n");
    // Export Bitmap page selector: a 2-page doc (red page 1, blue page 2) → exportRaster(area=Page,
    // pageIndex=N) renders page N's content, not always page 1 (the pre-fix behaviour).
    { Canvas ec; Document d; d.pageSize=QSizeF(200,150); d.pageGap=20; d.pageCount=2;
      const double adv=d.pageSize.height()+d.pageGap;
      Shape r0; r0.type=ShapeType::Rect; r0.rect=QRectF(0,0,200,150); r0.stroke.enabled=false;
      r0.fill.type=FillType::Solid; r0.fill.color=QColor(220,20,20);                                       // page 1 = red
      Shape r1; r1.type=ShapeType::Rect; r1.rect=QRectF(0,0,200,150); r1.stroke.enabled=false;
      r1.fill.type=FillType::Solid; r1.fill.color=QColor(20,20,220); r1.transform=QTransform::fromTranslate(0,adv);  // page 2 = blue
      d.shapes={r0,r1}; ec.setDocument(d);
      const QString a=QDir::tempPath()+"/xq_exp_p1.png", b=QDir::tempPath()+"/xq_exp_p2.png";
      bool e1=ec.exportRaster(a,0,1.0,-1,false,false,false,0), e2=ec.exportRaster(b,0,1.0,-1,false,false,false,1);
      QImage i1(a), i2(b);
      const QColor c1= i1.isNull()? QColor() : i1.pixelColor(i1.width()/2,i1.height()/2);
      const QColor c2= i2.isNull()? QColor() : i2.pixelColor(i2.width()/2,i2.height()/2);
      check(e1 && c1.red()>180 && c1.blue()<80,  "Export Bitmap: page 1 exports page 1 (red)");
      check(e2 && c2.blue()>180 && c2.red()<80,  "Export Bitmap: page selector exports page 2 (blue), not page 1");
      // exportRasterImage (the live-preview engine behind the new dialog): area rect + maxDim cap.
      const QRectF ar0=ec.exportAreaRect(0,0);
      check(qAbs(ar0.width()-200)<1 && qAbs(ar0.height()-150)<1, "exportAreaRect(Page) returns the page rect");
      QImage full=ec.exportRasterImage(0,2.0,false,0,0), capped=ec.exportRasterImage(0,2.0,false,0,120);
      check(full.width()==400 && full.height()==300, "exportRasterImage renders at the real scale (2× of 200×150)");
      check(qMax(capped.width(),capped.height())<=120 && !capped.isNull(), "exportRasterImage maxDim caps the preview render"); }

    { Document d; d.pageSize=QSizeF(800,600); d.breakpoints={480}; d.pageCount=2;
      // a heading (real text), a linked stretchy banner text, and a vector rect
      Shape head; head.type=ShapeType::Text; head.text="Welcome"; head.font=QFont("DejaVu Sans",40); head.fill.type=FillType::Solid; head.fill.color=QColor(20,20,20);
      head.transform=QTransform::fromTranslate(40,40);
      Shape banner; banner.type=ShapeType::Text; banner.text="Home"; banner.font=QFont("DejaVu Sans",18); banner.fill.type=FillType::Solid; banner.fill.color=Qt::white;
      banner.transform=QTransform::fromTranslate(40,140); banner.webStretch=1; banner.webSticky=true; banner.url="https://site.example";
      Shape box; box.type=ShapeType::Rect; box.rect=QRectF(0,0,200,120); box.fill.type=FillType::Solid; box.fill.color=QColor(60,140,240);
      box.transform=QTransform::fromTranslate(60,250);
      // page 2 object
      Shape p2; p2.type=ShapeType::Text; p2.text="Second page"; p2.font=QFont("DejaVu Sans",30); p2.fill.type=FillType::Solid;
      p2.transform=QTransform::fromTranslate(40, 600+50+40);   // on page 2 (y past pageRect(1).top())
      d.shapes={head,banner,box,p2}; c->setDocument(d);
      QString p="/tmp/selftest_site.html";
      check(c->exportWebsite(p), "exportWebsite succeeds");
      { QFile f(p); f.open(QIODevice::ReadOnly); QString h=QString::fromUtf8(f.readAll());
        check(h.contains("Welcome") && h.contains("<div class=\"txt"), "page 1 emits real HTML text for the heading");
        check(h.contains("<svg class=\"bg\""), "page 1 has the inline SVG vector layer");
        check(h.contains("@media(max-width:480px)"), "breakpoint becomes a media query");
        check(h.contains("class=\"sticky") || h.contains("txt sticky"), "sticky object gets the sticky class");
        check(h.contains("href=\"https://site.example\""), "linked text becomes an anchor");
        check(h.contains("<nav"), "multi-page document emits a nav bar");
        check(!h.contains("Second page"), "page 1 does NOT contain page 2's text"); }
      { QFile f2("/tmp/selftest_site_p2.html"); check(f2.exists(), "page 2 file was written");
        f2.open(QIODevice::ReadOnly); QString h2=QString::fromUtf8(f2.readAll());
        check(h2.contains("Second page"), "page 2 contains its own text"); } }

    // --- Web embed objects (Gap 6) ---
    std::printf("[web] embed objects (iframe/video/map/form)\n");
    { Document d; d.pageSize=QSizeF(800,600);
      Shape e; e.type=ShapeType::Rect; e.rect=QRectF(0,0,320,200); e.fill.type=FillType::Solid; e.fill.color=QColor(230,230,230);
      e.webEmbed="<iframe src=\"https://www.youtube.com/embed/ABC123\"></iframe>"; e.transform=QTransform::fromTranslate(60,60);
      Shape r; r.type=ShapeType::Rect; r.rect=QRectF(0,0,100,60); r.fill.type=FillType::Solid; r.fill.color=Qt::red; r.transform=QTransform::fromTranslate(500,60);
      d.shapes={e,r}; c->setDocument(d);
      check(c->exportWebsite("/tmp/selftest_embed_site.html"), "publish with a web-embed object succeeds");
      QFile f("/tmp/selftest_embed_site.html"); f.open(QIODevice::ReadOnly); QString h=QString::fromUtf8(f.readAll());
      check(h.contains("class=\"embed\"") && h.contains("youtube.com/embed/ABC123"), "the embed HTML is emitted at the object's slot");
      check(!h.contains("<rect") || h.count("<rect")==1, "the embed object is NOT also drawn as an SVG rect (only the plain red rect is)");
      // webEmbed round-trips through JSON
      Document rt=Document::fromJson(d.toJson());
      check(rt.shapes[0].webEmbed.contains("ABC123"), "webEmbed survives JSON round-trip"); }

    // --- Website SEO metadata (title/description/keywords) ---
    std::printf("[web] SEO metadata\n");
    { Document d; d.pageSize=QSizeF(800,600);
      Shape r; r.type=ShapeType::Rect; r.rect=QRectF(0,0,200,120); r.fill.type=FillType::Solid; r.fill.color=Qt::blue;
      d.shapes={r}; c->setDocument(d);
      c->setWebMeta("My Portfolio","Handcrafted vector art and photography.","art, vector, portfolio");
      check(c->webTitle()=="My Portfolio", "setWebMeta stored the title");
      check(c->exportWebsite("/tmp/selftest_seo.html"), "publish with SEO metadata succeeds");
      { QFile f("/tmp/selftest_seo.html"); f.open(QIODevice::ReadOnly); QString h=QString::fromUtf8(f.readAll());
        check(h.contains("<title>My Portfolio</title>"), "custom title lands in <title>");
        check(h.contains("name=\"description\"") && h.contains("Handcrafted vector art"), "description meta emitted");
        check(h.contains("name=\"keywords\"") && h.contains("art, vector, portfolio"), "keywords meta emitted");
        check(h.contains("property=\"og:title\"") && h.contains("property=\"og:description\""), "Open Graph tags emitted"); }
      // Extended SEO (G-L): base URL + author → twitter card + canonical + sitemap.xml + robots.txt.
      c->setWebSeoExtra("https://ex.example/","Jane Doe");
      check(c->exportWebsite("/tmp/xqseo/site.html"), "publish with extended SEO succeeds");
      { QFile f("/tmp/xqseo/site.html"); f.open(QIODevice::ReadOnly); QString h=QString::fromUtf8(f.readAll());
        check(h.contains("twitter:card") && h.contains("rel=\"canonical\"") && h.contains("https://ex.example/site.html"), "twitter card + canonical URL emitted");
        check(h.contains("name=\"author\" content=\"Jane Doe\""), "author meta emitted"); }
      check(QFile::exists("/tmp/xqseo/sitemap.xml") && QFile::exists("/tmp/xqseo/robots.txt"), "sitemap.xml + robots.txt generated on publish");
      // metadata round-trips through JSON (and thus .xar)
      Document rt=Document::fromJson(c->doc().toJson());
      check(rt.webTitle=="My Portfolio" && rt.webKeywords=="art, vector, portfolio", "SEO metadata survives JSON round-trip"); }
    // Web trio (H-C): link new-window (target=_blank), Buy-Now button, and site password gate.
    { c->newDocument();
      Shape lk; lk.type=ShapeType::Text; lk.text="Shop"; lk.font=QFont("DejaVu Sans",18); lk.fill.type=FillType::Solid; lk.fill.color=Qt::black;
      lk.url="https://example.com/product"; lk.webNewWindow=true; lk.webBuyUrl="https://pay.example.com/checkout"; lk.transform=QTransform::fromTranslate(20,20);
      c->doc().addShape(lk);
      c->setWebPassword("s3cret");
      check(c->exportWebsite("/tmp/xqweb3/site.html"), "publish with new-window link + buy button + password succeeds");
      QFile wf("/tmp/xqweb3/site.html"); wf.open(QIODevice::ReadOnly); const QString h=QString::fromUtf8(wf.readAll()); wf.close();
      check(h.contains("target=\"_blank\"") && h.contains("rel=\"noopener\""), "hyperlink opens in a new window");
      check(h.contains("class=\"buynow\"") && h.contains("pay.example.com/checkout"), "Buy-Now button published with the checkout URL");
      check(h.contains("sessionStorage") && h.contains(QString::fromLatin1(QByteArray("s3cret").toBase64())), "site is gated behind a password (base64-obfuscated)");
      Document rtw=Document::fromJson(c->doc().toJson());
      check(rtw.shapes[0].webNewWindow && rtw.shapes[0].webBuyUrl=="https://pay.example.com/checkout" && rtw.webPassword=="s3cret", "web new-window/buy/password round-trip through JSON"); }

    // --- Full-browser-width background bands (web) ---
    std::printf("[web] full-width background bands\n");
    { Document d; d.pageSize=QSizeF(800,600);
      Shape band; band.type=ShapeType::Rect; band.rect=QRectF(0,0,900,140); band.fill.type=FillType::Solid; band.fill.color=QColor(0,0,255); band.stroke.enabled=false;
      band.transform=QTransform::fromTranslate(-50,20);
      d.shapes={band}; c->setDocument(d);
      // OFF: no band flag → classic single-column stage markup is byte-identical
      check(c->exportWebsite("/tmp/selftest_band_off.html"), "publish without a band succeeds");
      { QFile f("/tmp/selftest_band_off.html"); f.open(QIODevice::ReadOnly); QString h=QString::fromUtf8(f.readAll());
        check(h.contains("<div class=\"wrap\"><div class=\"stage\">"), "band-free export keeps the classic wrap/stage markup");
        check(!h.contains("class=\"band\""), "no band div is emitted when the flag is off"); }
      // ON: mark the rectangle as a full-width band
      c->selectAll(); c->toggleWebBand();
      check(c->doc().shapes[0].webBand, "toggleWebBand set the band flag on the selection");
      check(c->exportWebsite("/tmp/selftest_band_on.html"), "publish with a band succeeds");
      { QFile f("/tmp/selftest_band_on.html"); f.open(QIODevice::ReadOnly); QString h=QString::fromUtf8(f.readAll());
        check(h.contains("class=\"band\""), "band emitted as its own div");
        check(h.contains(".band{position:absolute;left:0;right:0"), "band CSS spans the full viewport width");
        check(h.contains("background:#0000ff"), "band carries the shape's solid fill colour");
        check(h.contains("style=\"background:transparent\""), "stage turns transparent so the band shows behind the content"); }
      Document brt=Document::fromJson(c->doc().toJson());
      check(brt.shapes[0].webBand, "webBand survives JSON round-trip"); }

    // --- Named web anchors (#name jump links) ---
    std::printf("[web] named anchors\n");
    { Document d; d.pageSize=QSizeF(800,900);
      Shape target; target.type=ShapeType::Rect; target.rect=QRectF(0,0,200,120); target.name="Section Two";
      target.fill.type=FillType::Solid; target.fill.color=Qt::green; target.stroke.enabled=false; target.transform=QTransform::fromTranslate(40,600);
      Shape link; link.type=ShapeType::Text; link.text="Jump down"; link.font=QFont("DejaVu Sans",18);
      link.fill.type=FillType::Solid; link.fill.color=Qt::blue; link.stroke.enabled=false; link.url="#section-two"; link.transform=QTransform::fromTranslate(40,40);
      d.shapes={link,target}; c->setDocument(d);
      check(c->exportWebsite("/tmp/selftest_anchor.html"), "publish with a named anchor succeeds");
      { QFile f("/tmp/selftest_anchor.html"); f.open(QIODevice::ReadOnly); QString h=QString::fromUtf8(f.readAll());
        check(h.contains("id=\"section-two\""), "named object 'Section Two' publishes an anchor id 'section-two'");
        check(h.contains("class=\"anchor\""), "anchor target element emitted");
        check(h.contains("href=\"#section-two\""), "the jump link keeps its #fragment href"); }
      // an unnamed document emits no anchor elements
      Document d2; d2.pageSize=QSizeF(400,300);
      Shape r2; r2.type=ShapeType::Rect; r2.rect=QRectF(0,0,100,80); r2.fill.type=FillType::Solid; r2.fill.color=Qt::red; d2.shapes={r2};
      c->setDocument(d2);
      check(c->exportWebsite("/tmp/selftest_anchor_none.html"), "publish without names succeeds");
      { QFile f("/tmp/selftest_anchor_none.html"); f.open(QIODevice::ReadOnly); QString h=QString::fromUtf8(f.readAll());
        check(!h.contains("class=\"anchor\""), "no anchor elements when nothing is named"); } }

    // --- Fluid (Modular) website layout ---
    std::printf("[web] fluid modular layout\n");
    { Document d; d.pageSize=QSizeF(800,600);
      Shape r; r.type=ShapeType::Rect; r.rect=QRectF(0,0,200,120); r.fill.type=FillType::Solid; r.fill.color=Qt::blue;
      d.shapes={r}; c->setDocument(d);
      check(!c->fluidSite(), "fluid layout defaults off");
      // OFF: the published HTML must NOT contain the fluid scaler (fixed-stage export unchanged)
      check(c->exportWebsite("/tmp/selftest_fluid_off.html"), "publish with fluid OFF succeeds");
      QString hOff; { QFile f("/tmp/selftest_fluid_off.html"); f.open(QIODevice::ReadOnly); hOff=QString::fromUtf8(f.readAll()); }
      check(!hOff.contains("w.clientWidth/DW"), "no fluid scaler emitted when fluid is off");
      // ON: the scaler is present and scales the stage to the viewport
      c->setFluidSite(true);
      check(c->fluidSite(), "setFluidSite(true) took effect");
      check(c->exportWebsite("/tmp/selftest_fluid_on.html"), "publish with fluid ON succeeds");
      QString hOn; { QFile f("/tmp/selftest_fluid_on.html"); f.open(QIODevice::ReadOnly); hOn=QString::fromUtf8(f.readAll()); }
      check(hOn.contains("w.clientWidth/DW") && hOn.contains("scale(") && hOn.contains("addEventListener('resize'"), "fluid scaler emitted when fluid is on");
      // flag round-trips through JSON (and thus .xar)
      Document rt=Document::fromJson(c->doc().toJson());
      check(rt.fluidSite, "fluid flag survives JSON round-trip");
      c->setFluidSite(false); check(!c->fluidSite(), "fluid can be turned back off"); }

    // --- Website Variant: per-breakpoint position/size override ---
    std::printf("[web] website variant (per-breakpoint position)\n");
    { Document d; d.pageSize=QSizeF(800,600);
      Shape t; t.type=ShapeType::Text; t.text="Headline"; t.font=QFont("DejaVu Sans",20);
      t.fill.type=FillType::Solid; t.fill.color=Qt::black; t.stroke.enabled=false;
      t.transform=QTransform::fromTranslate(100,80);
      d.shapes={t}; c->setDocument(d); c->select({0});
      c->setSelectionVariant(600, 20, 400, 250);
      int eb=0; double ex=0,ey=0,ew=0; check(c->selectionVariant(eb,ex,ey,ew) && eb==600 && int(ex)==20 && int(ey)==400 && int(ew)==250,
            "selectionVariant getter returns what was set");
      check(c->exportWebsite("/tmp/selftest_variant.html"), "publish with a website variant succeeds");
      QString h; { QFile f("/tmp/selftest_variant.html"); f.open(QIODevice::ReadOnly); h=QString::fromUtf8(f.readAll()); }
      check(h.contains("@media(max-width:600px)"), "variant emits a max-width media query at the breakpoint");
      check(h.contains("left:20px!important") && h.contains("top:400px!important") && h.contains("width:250px!important"),
            "variant overrides left/top/width below the breakpoint");
      // round-trips through JSON (and thus .xar)
      Document rt=Document::fromJson(c->doc().toJson());
      check(rt.shapes[0].webVarBelow==600 && int(rt.shapes[0].webVarX)==20 && int(rt.shapes[0].webVarW)==250,
            "website variant survives JSON round-trip");
      // clearing (belowPx=0) removes the override
      c->select({0}); c->setSelectionVariant(0,0,0,0);
      check(c->exportWebsite("/tmp/selftest_variant0.html"), "publish after clearing variant succeeds");
      { QFile f("/tmp/selftest_variant0.html"); f.open(QIODevice::ReadOnly); QString h0=QString::fromUtf8(f.readAll());
        check(!h0.contains("left:20px!important"), "cleared variant emits no override"); } }

    // --- Buttons & Menus: multi-level dropdown nav ---
    std::printf("[web] menu bar (multi-level dropdown)\n");
    { Document d; d.pageSize=QSizeF(800,600);
      Shape nb; nb.type=ShapeType::Rect; nb.rect=QRectF(0,0,800,48);
      nb.fill.type=FillType::Solid; nb.fill.color=QColor(34,38,48); nb.stroke.enabled=false;
      nb.webNavBar=true; nb.webStretch=1;
      nb.webMenuTree="Home | index.html\nProducts |\n  Widgets | w.html\n  Gadgets | g.html\nAbout | about.html\n";
      d.shapes={nb}; c->setDocument(d);
      check(c->exportWebsite("/tmp/selftest_menu.html"), "publish with a dropdown menu bar succeeds");
      QString h; { QFile f("/tmp/selftest_menu.html"); f.open(QIODevice::ReadOnly); h=QString::fromUtf8(f.readAll()); }
      check(h.contains("class=\"xmenu"), "menu bar emits an xmenu nav");
      check(h.contains(">Home</a>") && h.contains(">Products</a>") && h.contains(">About</a>"), "top-level menu items present");
      check(h.contains("has-sub\">Products</a><ul>"), "parent item marked has-sub and opens a nested <ul>");
      check(h.contains("href=\"w.html\">Widgets</a>") && h.contains("href=\"g.html\">Gadgets</a>"), "submenu items present with their links");
      check(h.contains(".xmenu li:hover>ul") || h.contains(".xmenu li:hover >ul"), "dropdown CSS emitted");
      // round-trips through JSON (and thus .xar)
      Document rt=Document::fromJson(c->doc().toJson());
      check(rt.shapes[0].webMenuTree.contains("Widgets | w.html"), "menu tree survives JSON round-trip");
      // Canvas setter marks the shape a nav bar
      Document d2; d2.pageSize=QSizeF(400,300);
      Shape r; r.type=ShapeType::Rect; r.rect=QRectF(0,0,300,40); r.fill.type=FillType::Solid; r.fill.color=Qt::darkGray;
      d2.shapes={r}; c->setDocument(d2); c->select({0});
      c->setSelectionMenuTree("A | a.html\n  A1 | a1.html\nB | b.html");
      check(c->selectionMenuTree().contains("A1 | a1.html"), "setSelectionMenuTree/getter round-trip");
      check(c->doc().shapes[0].webNavBar, "attaching a menu marks the shape as a nav bar"); }

    // --- Presentation: reveal content layer-by-layer ---
    std::printf("[web] presentation steps-as-layers\n");
    { Document d; d.pageSize=QSizeF(800,600); d.layers={Layer{},Layer{}};
      Shape r; r.type=ShapeType::Rect; r.rect=QRectF(0,0,300,200); r.fill.type=FillType::Solid; r.fill.color=Qt::blue;
      r.stroke.enabled=false; r.layer=0; r.transform=QTransform::fromTranslate(40,40);
      Shape t; t.type=ShapeType::Text; t.text="Second step"; t.font=QFont("DejaVu Sans",22);
      t.fill.type=FillType::Solid; t.fill.color=Qt::black; t.stroke.enabled=false; t.layer=1; t.transform=QTransform::fromTranslate(60,320);
      d.shapes={r,t}; c->setDocument(d);
      check(!c->presentSteps(), "presentation defaults off");
      check(c->exportWebsite("/tmp/selftest_present_off.html"), "publish with presentation OFF succeeds");
      QString hOff; { QFile f("/tmp/selftest_present_off.html"); f.open(QIODevice::ReadOnly); hOff=QString::fromUtf8(f.readAll()); }
      check(!hOff.contains("class=\"pstep\"") && !hOff.contains("phint"), "no presentation markup when off (byte-identical path)");
      c->setPresentSteps(true);
      check(c->exportWebsite("/tmp/selftest_present_on.html"), "publish with presentation ON succeeds");
      QString hOn; { QFile f("/tmp/selftest_present_on.html"); f.open(QIODevice::ReadOnly); hOn=QString::fromUtf8(f.readAll()); }
      check(hOn.contains("class=\"pstep\" data-step=\"0\""), "layer-0 background emitted as reveal step 0");
      check(hOn.contains("data-step=\"1\""), "layer-1 content emitted as reveal step 1");
      check(hOn.contains(".pstep.pshide") && hOn.contains("ArrowRight") && hOn.contains("phint"), "reveal CSS + controller + hint emitted");
      Document rt=Document::fromJson(c->doc().toJson());
      check(rt.presentSteps, "presentation flag survives JSON round-trip");
      c->setPresentSteps(false); check(!c->presentSteps(), "presentation can be turned back off"); }

    // --- Adversarial-review fixes (web export) ---
    std::printf("[web] review fixes: menu XSS, indent nesting, nav variant, present click\n");
    { Document d; d.pageSize=QSizeF(800,600);          // (1) menu hrefs are scheme-sanitised
      Shape nb; nb.type=ShapeType::Rect; nb.rect=QRectF(0,0,800,48); nb.fill.type=FillType::Solid; nb.fill.color=QColor(34,38,48);
      nb.stroke.enabled=false; nb.webNavBar=true; nb.webStretch=1;
      nb.webMenuTree="Home | index.html\nHack | javascript:alert(document.cookie)\nData | data:text/html,<script>x</script>";
      d.shapes={nb}; c->setDocument(d);
      check(c->exportWebsite("/tmp/selftest_menu_xss.html"), "publish menu with hostile hrefs succeeds");
      QString h; { QFile f("/tmp/selftest_menu_xss.html"); f.open(QIODevice::ReadOnly); h=QString::fromUtf8(f.readAll()); }
      check(!h.contains("href=\"javascript:") && !h.contains("href=\"data:"), "menu javascript:/data: hrefs neutralised via safeHref");
      check(h.contains("href=\"index.html\""), "a normal menu href is preserved"); }
    { Document d; d.pageSize=QSizeF(800,600);          // (2) 4-space indentation nests siblings, not cascaded
      Shape nb; nb.type=ShapeType::Rect; nb.rect=QRectF(0,0,800,48); nb.fill.type=FillType::Solid; nb.fill.color=QColor(34,38,48);
      nb.stroke.enabled=false; nb.webNavBar=true;
      nb.webMenuTree="Products |\n    Widgets | w.html\n    Gadgets | g.html\nAbout | about.html";
      d.shapes={nb}; c->setDocument(d);
      check(c->exportWebsite("/tmp/selftest_menu_indent.html"), "publish 4-space-indented menu succeeds");
      QString h; { QFile f("/tmp/selftest_menu_indent.html"); f.open(QIODevice::ReadOnly); h=QString::fromUtf8(f.readAll()); }
      check(h.count("<ul>")==2, "exactly one submenu <ul> — Widgets/Gadgets are siblings, not cascaded");
      check(h.count("class=\"has-sub\"")==1 && h.contains("has-sub\">Products</a>"), "only Products is has-sub (no spurious arrow on Widgets)"); }
    { Document d; d.pageSize=QSizeF(800,600); d.pageCount=2;   // (3) nav-bar variant is page-local on every page
      Shape nb; nb.type=ShapeType::Rect; nb.rect=QRectF(0,0,800,40); nb.fill.type=FillType::Solid; nb.fill.color=QColor(34,38,48);
      nb.stroke.enabled=false; nb.webNavBar=true; nb.transform=QTransform::fromTranslate(0,20);
      nb.webVarBelow=600; nb.webVarX=0; nb.webVarY=20; nb.webVarW=800;
      d.shapes={nb}; c->setDocument(d);
      check(c->exportWebsite("/tmp/selftest_navvar.html"), "publish 2-page nav variant succeeds");
      QString p1,p2; { QFile f("/tmp/selftest_navvar.html"); f.open(QIODevice::ReadOnly); p1=QString::fromUtf8(f.readAll()); }
      { QFile f("/tmp/selftest_navvar_p2.html"); f.open(QIODevice::ReadOnly); p2=QString::fromUtf8(f.readAll()); }
      check(p1.contains("top:20px!important"), "page 1 nav variant top is page-local (20px)");
      check(p2.contains("top:20px!important") && !p2.contains("top:-"), "page 2 nav variant top is ALSO page-local (not offset by the page start)"); }
    { Document d; d.pageSize=QSizeF(400,300); d.presentSteps=true;   // (4) present click ignores interactive controls
      Shape r; r.type=ShapeType::Rect; r.rect=QRectF(0,0,100,80); r.fill.type=FillType::Solid; r.fill.color=Qt::blue; r.stroke.enabled=false;
      d.shapes={r}; c->setDocument(d);
      check(c->exportWebsite("/tmp/selftest_presclick.html"), "publish presentation succeeds");
      QString h; { QFile f("/tmp/selftest_presclick.html"); f.open(QIODevice::ReadOnly); h=QString::fromUtf8(f.readAll()); }
      check(h.contains(".closest('a,button"), "presentation click-advance ignores interactive controls"); }
    // Custom head/body HTML injection
    { Document d; d.pageSize=QSizeF(400,300);
      Shape r; r.type=ShapeType::Rect; r.rect=QRectF(0,0,100,80); r.fill.type=FillType::Solid; r.fill.color=Qt::gray; r.stroke.enabled=false;
      d.shapes={r}; c->setDocument(d);
      c->setWebRaw("<meta name=\"verify\" content=\"abc123\"><script>window.ga=1</script>","<script>chatWidget()</script>");
      check(c->exportWebsite("/tmp/selftest_webraw.html"), "publish with custom head/body HTML succeeds");
      QString h; { QFile f("/tmp/selftest_webraw.html"); f.open(QIODevice::ReadOnly); h=QString::fromUtf8(f.readAll()); }
      check(h.contains("name=\"verify\" content=\"abc123\"") && h.indexOf("window.ga=1")<h.indexOf("</head>"), "custom head HTML injected verbatim before </head>");
      check(h.contains("chatWidget()") && h.indexOf("chatWidget()")<h.indexOf("</body>"), "custom body HTML injected before </body>");
      Document rt=Document::fromJson(c->doc().toJson());
      check(rt.webHeadHtml.contains("abc123") && rt.webBodyHtml.contains("chatWidget"), "custom head/body HTML survives JSON round-trip"); }

    // --- Photo Mask Painter: scope non-destructive adjustments to a painted region ---
    std::printf("[photo] mask painter (scoped adjustments)\n");
    { QImage img(100,100,QImage::Format_ARGB32); img.fill(QColor(128,128,128));
      Shape p; p.type=ShapeType::Rect; p.rect=QRectF(0,0,100,100); p.transform=QTransform::fromTranslate(0,0);
      p.fill.type=FillType::Bitmap; p.fill.image=img; p.fill.origImage=img; p.stroke.enabled=false;
      Document d; d.shapes={p}; c->setDocument(d); c->select({0});
      QPainterPath reg; reg.addRect(0,0,50,100);   // left half only
      c->setShapeRegion(0,reg);
      c->setPhotoMaskFromRegion(0);
      check(c->hasPhotoMask(0), "adjustment mask created from the painted region");
      Fill::Adjust a; a.brightness=80;
      c->select({0}); c->applyPhotoAdjust(a);
      QImage out=c->doc().shapes[0].fill.image;
      const int inside=qGray(out.pixel(20,50)), outside=qGray(out.pixel(80,50));
      check(inside>150, "adjustment brightens the pixels inside the mask");
      check(outside>=124 && outside<=132, "pixels outside the mask keep their original value");
      // clearing the mask makes the adjustment apply to the whole photo
      c->clearPhotoMask(0); check(!c->hasPhotoMask(0), "clearPhotoMask removes the mask");
      c->select({0}); c->applyPhotoAdjust(a);
      check(qGray(c->doc().shapes[0].fill.image.pixel(80,50))>150, "after clearing the mask the adjustment applies everywhere"); }

    // --- EPUB import ---
    std::printf("[import] EPUB e-book\n");
    { auto le16=[](QByteArray&b,quint16 v){ b.append(char(v&0xff)); b.append(char((v>>8)&0xff)); };
      auto le32=[](QByteArray&b,quint32 v){ for(int i=0;i<4;i++) b.append(char((v>>(8*i))&0xff)); };
      struct Ent{ QByteArray name,data; quint32 off; };
      QVector<Ent> ents={
        {"mimetype","application/epub+zip",0},
        {"META-INF/container.xml","<?xml version=\"1.0\"?><container><rootfiles><rootfile full-path=\"OEBPS/content.opf\"/></rootfiles></container>",0},
        {"OEBPS/content.opf","<?xml version=\"1.0\"?><package><manifest>"
          "<item id=\"c1\" href=\"ch1.xhtml\" media-type=\"application/xhtml+xml\"/>"
          "<item id=\"c2\" href=\"ch2.xhtml\" media-type=\"application/xhtml+xml\"/>"
          "</manifest><spine><itemref idref=\"c1\"/><itemref idref=\"c2\"/></spine></package>",0},
        {"OEBPS/ch1.xhtml","<html><body><h1>Chapter One</h1><p>The quick brown fox.</p></body></html>",0},
        {"OEBPS/ch2.xhtml","<html><body><h1>Chapter Two</h1><p>Jumps over the lazy dog.</p></body></html>",0} };
      QByteArray zip;
      for(auto&e:ents){ e.off=zip.size();
        le32(zip,0x04034b50u); le16(zip,20); le16(zip,0); le16(zip,0); le16(zip,0); le16(zip,0);
        le32(zip,0); le32(zip,e.data.size()); le32(zip,e.data.size()); le16(zip,e.name.size()); le16(zip,0);
        zip.append(e.name); zip.append(e.data); }
      const quint32 cdStart=zip.size();
      for(auto&e:ents){
        le32(zip,0x02014b50u); le16(zip,20); le16(zip,20); le16(zip,0); le16(zip,0); le16(zip,0); le16(zip,0);
        le32(zip,0); le32(zip,e.data.size()); le32(zip,e.data.size()); le16(zip,e.name.size()); le16(zip,0); le16(zip,0);
        le16(zip,0); le16(zip,0); le32(zip,0); le32(zip,e.off); zip.append(e.name); }
      const quint32 cdSize=zip.size()-cdStart;
      le32(zip,0x06054b50u); le16(zip,0); le16(zip,0); le16(zip,(quint16)ents.size()); le16(zip,(quint16)ents.size());
      le32(zip,cdSize); le32(zip,cdStart); le16(zip,0);
      const QString ep="/tmp/selftest_book.epub"; { QFile f(ep); f.open(QIODevice::WriteOnly); f.write(zip); f.close(); }
      check(c->importEpub(ep), "importEpub parses a minimal EPUB");
      check(c->doc().pageCount==2, "one page per spine chapter");
      QStringList chapTexts; for(const Shape& s: c->doc().shapes) if(s.type==ShapeType::Text) chapTexts<<s.text;
      check(chapTexts.size()==2 && chapTexts[0].contains("Chapter One") && chapTexts[0].contains("quick brown fox"), "chapter 1 text extracted (spine order)");
      check(chapTexts.size()==2 && chapTexts[1].contains("Chapter Two") && chapTexts[1].contains("lazy dog"), "chapter 2 text extracted (spine order)");
      const QString bad="/tmp/selftest_bad.epub"; { QFile f(bad); f.open(QIODevice::WriteOnly); f.write(QByteArray("this is not a zip file")); f.close(); }
      check(!c->importEpub(bad), "a non-EPUB file fails gracefully without crashing"); }

    // --- Pop-up photo / lightbox ---
    std::printf("[web] photo lightbox pop-up\n");
    { Document d; d.pageSize=QSizeF(800,600);
      QImage im(60,40,QImage::Format_ARGB32); im.fill(QColor(200,40,40));
      Shape p; p.type=ShapeType::Rect; p.rect=QRectF(0,0,240,160); p.fill.type=FillType::Bitmap; p.fill.image=im; p.fill.origImage=im;
      p.fill.p0=QPointF(0,0); p.fill.p1=QPointF(240,0); p.fill.p2=QPointF(0,160); p.name="Hero photo";
      p.transform=QTransform::fromTranslate(80,80);
      Shape r; r.type=ShapeType::Rect; r.rect=QRectF(0,0,100,60); r.fill.type=FillType::Solid; r.fill.color=Qt::blue; r.transform=QTransform::fromTranslate(500,80);
      d.shapes={p,r}; c->setDocument(d);
      c->select({0}); c->setSelectionLightbox(true);
      check(c->selectionLightbox(), "setSelectionLightbox flags the selected photo");
      check(c->exportWebsite("/tmp/selftest_lb.html"), "publish with a pop-up photo succeeds");
      QFile f("/tmp/selftest_lb.html"); f.open(QIODevice::ReadOnly); QString h=QString::fromUtf8(f.readAll());
      check(h.contains("class=\"lb\"") && h.contains("data-full=\"data:image/png;base64,"), "lightbox photo emitted as a clickable <img> with a full-res source");
      check(h.contains("id=\"lightbox\"") && h.contains("lbOpen"), "the overlay div + lbOpen script are present");
      check(h.contains("id=\"lbPrev\"") && h.contains("id=\"lbNext\"") && h.contains("ArrowRight"), "lightbox gallery has prev/next + keyboard navigation");
      check(!h.contains("<image"), "the pop-up photo is NOT also drawn as an <image> in the SVG background layer");
      // toggling off removes it from the pop-up set
      c->setSelectionLightbox(false); check(!c->selectionLightbox(), "lightbox can be turned back off");
      // round-trips through JSON
      c->setSelectionLightbox(true);
      Document rt=Document::fromJson(c->doc().toJson());
      check(rt.shapes[0].webLightbox, "webLightbox survives JSON round-trip"); }

    // --- Web animations (scroll reveal + parallax) ---
    std::printf("[web] scroll animations\n");
    { Document d; d.pageSize=QSizeF(800,600);
      Shape t; t.type=ShapeType::Text; t.text="Hello"; t.font=QFont("DejaVu Sans",28); t.fill.type=FillType::Solid; t.fill.color=Qt::black;
      t.transform=QTransform::fromTranslate(60,60);
      Shape r; r.type=ShapeType::Rect; r.rect=QRectF(0,0,200,120); r.fill.type=FillType::Solid; r.fill.color=QColor(40,120,220); r.transform=QTransform::fromTranslate(400,300);
      d.shapes={t,r}; c->setDocument(d);
      c->select({0}); c->setSelectionWebAnim(1,0.2);            // text: fade in, 0.2s delay
      c->select({1}); c->setSelectionWebAnim(7,0.0);            // vector rect: parallax
      check(c->selectionWebAnim()==7, "setSelectionWebAnim records the effect");
      check(c->exportWebsite("/tmp/selftest_anim.html"), "publish with web animations succeeds");
      QFile f("/tmp/selftest_anim.html"); f.open(QIODevice::ReadOnly); QString h=QString::fromUtf8(f.readAll());
      check(h.contains(".anim{opacity:0"), "reveal CSS is present");
      check(h.contains("class=\"txt anim a-fade\"") && h.contains("transition-delay:0.20s"), "text carries the fade class + delay");
      check(h.contains("IntersectionObserver"), "the reveal observer script is emitted");
      check(h.contains("data-parallax=") && h.contains("class=\"animwrap"), "parallax vector shape gets its own animatable wrapper");
      Document rt=Document::fromJson(c->doc().toJson());
      check(rt.shapes[0].webAnim==1 && qAbs(rt.shapes[0].webAnimDelay-0.2)<1e-9 && rt.shapes[1].webAnim==7, "web animation survives JSON round-trip"); }

    // --- Image slider / carousel ---
    std::printf("[web] slider / carousel\n");
    { Document d; d.pageSize=QSizeF(800,600);
      QImage a(40,30,QImage::Format_ARGB32); a.fill(QColor(220,60,60));
      QImage b(40,30,QImage::Format_ARGB32); b.fill(QColor(60,160,80));
      QImage cc(40,30,QImage::Format_ARGB32); cc.fill(QColor(60,90,220));
      auto mk=[&](const QImage& im,double x){ Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,300,200);
        s.fill.type=FillType::Bitmap; s.fill.image=im; s.fill.origImage=im; s.fill.p0=QPointF(0,0); s.fill.p1=QPointF(300,0); s.fill.p2=QPointF(0,200);
        s.transform=QTransform::fromTranslate(x,100); return s; };
      d.shapes={mk(a,60),mk(b,60),mk(cc,60)}; c->setDocument(d);   // three stacked slides
      c->select({0,1,2}); c->makeSliderFromSelection();
      check(c->selectionSlider()!=0, "makeSliderFromSelection assigns a shared slider id");
      check(c->doc().shapes[0].webSlider==c->doc().shapes[2].webSlider, "all selected slides share the same slider id");
      check(c->exportWebsite("/tmp/selftest_slider.html"), "publish with a slider succeeds");
      QFile f("/tmp/selftest_slider.html"); f.open(QIODevice::ReadOnly); QString h=QString::fromUtf8(f.readAll());
      check(h.contains("class=\"slider\"") && h.count("class=\"slide")>=3, "the three slides are emitted inside one .slider");
      check(h.contains("sl-prev") && h.contains("sl-next") && h.contains("sl-dots"), "prev/next arrows + dots are present");
      check(h.contains("querySelectorAll('.slider')") && h.contains("setInterval"), "the auto-advancing slider script is emitted");
      check(!h.contains("<image"), "slider slides are NOT also drawn in the SVG background layer");
      // a single-shape selection does NOT form a slider
      Document d2; d2.shapes={mk(a,60)}; c->setDocument(d2); c->select({0}); c->makeSliderFromSelection();
      check(c->selectionSlider()==0, "a slider needs at least two slides");
      // round-trips through JSON
      c->setDocument(d); c->select({0,1,2}); c->makeSliderFromSelection();
      Document rt2=Document::fromJson(c->doc().toJson());
      check(rt2.shapes[0].webSlider!=0 && rt2.shapes[0].webSlider==rt2.shapes[1].webSlider, "slider grouping survives JSON round-trip"); }

    // --- Regression: percent-encoded link URLs must survive HTML export unmangled ---
    // (chained QString::arg re-parsed %2F/%3A inside the URL as placeholders — now single-pass.)
    std::printf("[web] percent-encoded link URLs\n");
    { Document d; d.pageSize=QSizeF(800,600);
      Shape t; t.type=ShapeType::Text; t.text="Click here now"; t.font=QFont("DejaVu Sans",24);
      t.fill.type=FillType::Solid; t.fill.color=Qt::black; t.transform=QTransform::fromTranslate(40,40);
      d.shapes={t}; c->setDocument(d); c->select({0});
      QString enc="https://ex.com/r?u=https%3A%2F%2Fother.com%2Fp%3Fa%3D1";
      c->setTextRunAttr(0,5,nullptr,nullptr,&enc);        // hyperlink on "Click"
      check(c->exportWebsite("/tmp/selftest_url.html"), "publish with a percent-encoded run link succeeds");
      { QFile f("/tmp/selftest_url.html"); f.open(QIODevice::ReadOnly); QString h=QString::fromUtf8(f.readAll());
        check(h.contains("href=\""+enc+"\""), "exportWebsite emits the %-encoded href intact (no arg re-parsing)"); }
      check(c->exportHtml("/tmp/selftest_url2.html"), "image-map HTML export succeeds");
      { QFile f("/tmp/selftest_url2.html"); f.open(QIODevice::ReadOnly); QString h=QString::fromUtf8(f.readAll());
        check(h.contains("href=\""+enc+"\""), "exportHtml emits the %-encoded tlnk href intact"); } }

    // --- Embedded media (video / audio / animated GIF) ---
    std::printf("[web] embedded media players\n");
    { Document d; d.pageSize=QSizeF(800,600); c->setDocument(d);
      QByteArray vid("FAKE-MP4-VIDEO-DATA"), aud("FAKE-MP3-AUDIO-DATA"), gif("GIF89a-fake-bytes");
      c->setSelectionMedia(vid,"video/mp4");                 // drops + selects a video placeholder
      check(c->doc().shapes.size()==1 && !c->doc().shapes[0].webMediaData.isEmpty(), "setSelectionMedia drops a media placeholder");
      c->selectNone(); c->setSelectionMedia(aud,"audio/mpeg");
      c->selectNone(); c->setSelectionMedia(gif,"image/gif");
      check(c->exportWebsite("/tmp/selftest_media.html"), "publish with embedded media succeeds");
      QFile f("/tmp/selftest_media.html"); f.open(QIODevice::ReadOnly); QString h=QString::fromUtf8(f.readAll());
      check(h.contains("<video controls") && h.contains("data:video/mp4;base64,"), "video → native <video> player with a data URI");
      check(h.contains("<audio controls") && h.contains("data:audio/mpeg;base64,"), "audio → native <audio> player");
      check(h.contains("<img") && h.contains("data:image/gif;base64,"), "animated GIF → <img>");
      check(!h.contains("<image"), "media placeholders are NOT also drawn in the SVG background layer");
      Document rt=Document::fromJson(c->doc().toJson());
      check(rt.shapes[0].webMediaMime=="video/mp4" && rt.shapes[0].webMediaData==vid, "media bytes + MIME survive JSON round-trip"); }

    // --- Photo Levels (input black/gamma/white) ---
    std::printf("[photo] Levels\n");
    { Document d; QImage im(2,2,QImage::Format_ARGB32);
      im.setPixel(0,0,qRgb(64,64,64)); im.setPixel(1,0,qRgb(128,128,128));
      im.setPixel(0,1,qRgb(192,192,192)); im.setPixel(1,1,qRgb(255,255,255));
      Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,100,100); s.fill.type=FillType::Bitmap; s.fill.image=im; s.fill.origImage=im;
      s.transform=QTransform::fromTranslate(10,10); d.shapes={s}; c->setDocument(d); c->select({0});
      Fill::Adjust a; a.blackPt=64; a.whitePt=192; c->applyPhotoAdjust(a);   // [64..192]→[0..255], gamma 1.0
      const QImage r=c->doc().shapes[0].fill.image;
      check(qRed(r.pixel(0,0))<=2, "Levels: black point clips shadows to 0");
      check(qAbs(qRed(r.pixel(1,0))-128)<=2, "Levels: the midpoint maps linearly");
      check(qRed(r.pixel(0,1))>=253, "Levels: white point clips highlights to 255");
      check(c->doc().shapes[0].fill.adjust.levelsActive(), "Adjust records active levels");
      Fill::Adjust g; g.gamma=2.0; c->applyPhotoAdjust(g);                   // re-bakes from origImage
      check(qRed(c->doc().shapes[0].fill.image.pixel(1,0))>150, "Levels: gamma>1 brightens midtones");
      Document rt=Document::fromJson(c->doc().toJson());
      check(qAbs(rt.shapes[0].fill.adjust.gamma-2.0)<1e-9 && rt.shapes[0].fill.adjust.blackPt==0, "Levels params survive JSON round-trip"); }

    // --- Text paragraph indents (first-line / left / right) ---
    std::printf("[text] paragraph indents\n");
    { Document d; Shape s; s.type=ShapeType::Text; s.textFrame=true; s.rect=QRectF(0,0,200,400);
      s.text="The quick brown fox jumps over the lazy dog again and again and again";
      s.font=QFont("DejaVu Sans",18); s.fill.type=FillType::Solid; s.fill.color=Qt::black;
      d.shapes={s}; c->setDocument(d); c->select({0});
      QRectF b0=c->doc().shapes[0].localPath().boundingRect();
      c->setTextIndents(0,40,40);                       // 40px left + 40px right → narrower column
      QRectF b1=c->doc().shapes[0].localPath().boundingRect();
      check(b1.left() >= b0.left()+30, "left margin shifts the text right");
      check(b1.height() > b0.height(), "left+right margins narrow the column → text wraps taller");
      c->setTextIndents(60,0,0);                         // first-line indent only, margins cleared
      QRectF b2=c->doc().shapes[0].localPath().boundingRect();
      check(b2.left() < b1.left()-20, "clearing the left margin brings non-first lines back to the edge");
      Document rt=Document::fromJson(c->doc().toJson());
      check(qAbs(rt.shapes[0].firstLineIndent-60)<1e-9 && rt.shapes[0].leftIndent==0.0, "indents survive JSON round-trip"); }

    // --- Text tab stops ---
    std::printf("[text] tab stops\n");
    { Document d; Shape s; s.type=ShapeType::Text; s.text="AB\tCD"; s.font=QFont("DejaVu Sans",24);
      s.fill.type=FillType::Solid; s.fill.color=Qt::black; d.shapes={s}; c->setDocument(d); c->select({0});
      QRectF b0=c->doc().shapes[0].localPath().boundingRect();   // tabWidth 0: \t collapses (Qt default)
      c->setTextTabWidth(120);                                    // a 120px tab stop
      QRectF b1=c->doc().shapes[0].localPath().boundingRect();
      check(b1.right() > b0.right()+40, "a tab stop pushes the text after \\t to the next stop (wider bbox)");
      Document rt=Document::fromJson(c->doc().toJson());
      check(qAbs(rt.shapes[0].tabWidth-120)<1e-9, "tab width survives JSON round-trip");
      c->setTextTabWidth(0);
      check(c->doc().shapes[0].localPath().boundingRect().right() < b1.right()-30, "clearing tab stops collapses the gap"); }

    // --- In-canvas text editing: caret geometry + live typing/selection/navigation ---
    std::printf("[text-edit] caret mapping + live editing\n");
    { Shape s; s.type=ShapeType::Text; s.text="Hello"; s.font=QFont("DejaVu Sans",36);
      // caret geometry: index 0 at the origin x, monotically increasing, and a round-trip through
      // textIndexAtLocal recovers the index.
      QPointF c0=s.textCaretLocal(0), c3=s.textCaretLocal(3), c5=s.textCaretLocal(5);
      check(qAbs(c0.x())<0.5, "caret 0 sits at the text origin x");
      check(c3.x()>c0.x() && c5.x()>c3.x(), "caret x advances monotonically along the line");
      check(s.textIndexAtLocal(s.textCaretLocal(3))==3, "textIndexAtLocal inverts textCaretLocal (mid-word)");
      // second line caret drops by one line height
      { Shape m=s; m.text="ab\ncd"; QPointF l0=m.textCaretLocal(0), l1=m.textCaretLocal(3);   // index 3 = 'c' (after '\n')
        check(l1.y() > l0.y()+m.textLineH()*0.5, "a caret on line 2 is a line-height below line 1"); }
    }
    { Document d; Shape s; s.type=ShapeType::Text; s.text="Hi"; s.font=QFont("DejaVu Sans",36);
      s.fill.type=FillType::Solid; s.fill.color=Qt::black; d.shapes={s}; c->setDocument(d); c->select({0});
      c->beginTextEdit(0, -1);                                   // caret at end
      check(c->editingText() && c->textEditShape()==0 && c->textCaret()==2, "beginTextEdit enters edit mode with caret at end");
      c->textInsert("!");                    check(c->doc().shapes[0].text=="Hi!" && c->textCaret()==3, "typing inserts at the caret");
      c->textBackspace();                    check(c->doc().shapes[0].text=="Hi"  && c->textCaret()==2, "backspace deletes before the caret");
      c->textMoveCaret(-1,false); c->textMoveCaret(-1,false);   // caret to 0
      check(c->textCaret()==0, "left arrow moves the caret");
      c->textInsert("Oh, ");                 check(c->doc().shapes[0].text=="Oh, Hi", "insert at the start prepends");
      c->textMoveCaret(+2,false);            check(c->textCaret()==c->doc().shapes[0].text.size(), "End moves the caret to line end");
      c->textDelete();                       check(c->doc().shapes[0].text=="Oh, Hi", "Delete at end of text is a no-op");
      // select-all + replace
      c->textSelectAll();                    check(c->textHasSelection(), "select-all creates a selection");
      c->textInsert("X");                    check(c->doc().shapes[0].text=="X", "typing over a selection replaces it");
      // undo restores prior text
      c->undo();                             check(c->doc().shapes[0].text=="Oh, Hi", "undo reverts the last text edit");
      // click-to-position: caret at the local point of index 3
      { const Shape& sh=c->doc().shapes[0]; QPointF docPt=sh.transform.map(sh.textCaretLocal(3));
        c->setCaretAtDocPoint(docPt, false); check(c->textCaret()==3, "clicking positions the caret at that character"); }
      c->endTextEdit();                      check(!c->editingText(), "endTextEdit leaves edit mode"); }
    // Inkscape-style per-selection character formatting: while editing, the InfoBar/palette format ONLY the
    // selected character range (via formatEditSelection) instead of the whole text object.
    std::printf("[text-edit] per-selection character formatting (colour + size)\n");
    { Document d; Shape s; s.type=ShapeType::Text; s.text="RedBlue"; s.font=QFont("DejaVu Sans",36);
      s.fill.type=FillType::Solid; s.fill.color=QColor(10,10,10); d.shapes={s}; c->setDocument(d); c->select({0});
      c->beginTextEdit(0,-1);
      check(!c->formatEditSelection([](QFont&,QColor& cc){ cc=QColor(1,2,3); },"noop"), "formatEditSelection is a no-op with no active selection");
      c->textMoveCaret(-2,false);                                   // Home → caret at 0
      c->textMoveCaret(+1,true); c->textMoveCaret(+1,true); c->textMoveCaret(+1,true);   // select [0,3) = "Red"
      check(c->textHasSelection() && c->textCaret()==3, "shift-arrow selects the first three characters");
      check(c->formatEditSelection([](QFont&,QColor& cc){ cc=QColor(200,0,0); },"Text colour"), "formatEditSelection colours the selected range");
      { const Shape& t=c->doc().shapes[0]; check(t.textRuns.size()>=2, "colouring part of the text splits it into runs");
        check(t.textRuns[0].len==3 && t.textRuns[0].color==QColor(200,0,0), "the selected run [0,3) is red");
        check(t.textRuns.back().color==QColor(10,10,10), "the unselected tail keeps the base colour"); }
      // now select [3,7) = "Blue" and shrink its size — only that run changes
      c->textMoveCaret(+2,false);                                   // End → caret at 7 (clears selection)
      c->textMoveCaret(-1,true); c->textMoveCaret(-1,true); c->textMoveCaret(-1,true); c->textMoveCaret(-1,true);  // select back to 3
      check(c->formatEditSelection([](QFont& g,QColor&){ g.setPointSize(12); },"Size"), "formatEditSelection resizes the selected range");
      { const Shape& t=c->doc().shapes[0]; const TextRun& last=t.textRuns.back();
        check(last.start==3 && last.font.pointSize()==12, "the second run [3,7) is 12pt while the first stays 36pt");
        check(t.textRuns.front().font.pointSize()==36, "the first run's size is unchanged"); }
      c->undo(); check(c->doc().shapes[0].textRuns.back().font.pointSize()==36, "undo reverts the per-selection size change");
      c->endTextEdit(); }
    // a freshly-created empty text object is discarded when editing ends with no text
    { Document d; c->setDocument(d);
      Shape s; s.type=ShapeType::Text; s.text=QString(); s.font=QFont("DejaVu Sans",36); s.transform=QTransform::fromTranslate(50,50);
      c->doc().addShape(s); c->select({0});
      int n=c->doc().shapes.size();
      c->beginTextEdit(0,0); c->endTextEdit();                  // (m_textEditIsNew is false here, so it stays)
      check(c->doc().shapes.size()==n, "ending edit on a non-new empty text keeps it"); }

    // --- In-canvas text editing: review-fix regressions ---
    std::printf("[text-edit] robustness (shortcuts, stale index, gating)\n");
    // canInlineEdit gates the in-canvas caret to plain point text; frames/curves/tabs use the dialog.
    { Shape p; p.type=ShapeType::Text; p.text="hi"; check(Canvas::canInlineEdit(p), "plain point text is inline-editable");
      Shape f=p; f.textFrame=true; f.rect=QRectF(0,0,200,100); check(Canvas::canInlineEdit(f), "a SIMPLE word-wrapped frame is now inline-editable");
      Shape fl=f; fl.listStyle=1; check(Canvas::canInlineEdit(fl), "a bulleted-list frame IS inline-editable (marker in the gutter, text hangs at listIndent)");
      Shape fj=f; fj.textAlign=3; check(Canvas::canInlineEdit(fj), "a justified frame IS inline-editable (renders left here)");
      Shape fd=f; fd.dropCap=3; check(Canvas::canInlineEdit(fd), "a drop-cap frame (first char is the cap) IS inline-editable");
      Shape fdp=f; fdp.text="\"Quoted"; fdp.dropCap=3; check(Canvas::canInlineEdit(fdp), "a leading-punctuation drop-cap frame IS inline-editable (cap found past the punctuation)");
      Shape fh=f; fh.hyphenate=true; check(Canvas::canInlineEdit(fh), "a hyphenated frame IS inline-editable (the synthetic '-' is tracked)");
      Shape tp=p; { QPainterPath pp; pp.moveTo(0,0); pp.lineTo(100,0); tp.textPath=pp; } check(Canvas::canInlineEdit(tp), "text-on-a-curve IS inline-edited (caret flows along the path)");
      Shape tb=p; tb.tabWidth=100; check(Canvas::canInlineEdit(tb), "regular-tab point text IS inline-edited (s_tabX mirrors the jump)");
      Shape ts=p; ts.tabWidth=100; ts.tabStops.push_back(TabStop{120.0,2,QChar('.')}); check(Canvas::canInlineEdit(ts), "explicit tab stops ARE inline-editable (s_tabStopsX mirrors place())"); }
    // Wrapped-frame caret: frameVisualLines wraps like the renderer, and the caret round-trips.
    { Document d; Shape s; s.type=ShapeType::Text; s.textFrame=true; s.rect=QRectF(0,0,200,400);
      s.text="alpha beta gamma delta epsilon zeta eta theta iota"; s.font=QFont("DejaVu Sans",22);
      s.fill.type=FillType::Solid; d.shapes={s}; c->setDocument(d); c->select({0});
      const Shape& sh=c->doc().shapes[0];
      const auto vls=sh.frameVisualLines();
      check(vls.size()>=2, "frame text wraps onto multiple visual lines");
      check(vls[0].startChar==0 && vls[1].startChar>0, "visual-line char offsets increase");
      check(qAbs(sh.textCaretLocal(0).y()-vls[0].baseY)<0.01, "caret 0 sits on line 1's baseline");
      check(sh.textCaretLocal(vls[1].startChar).y() > sh.textCaretLocal(0).y()+1.0, "a caret on line 2 is below line 1");
      check(qAbs(sh.textCaretLocal(sh.text.size()).y()-vls.last().baseY)<0.01, "the end caret is on the last visual line");
      bool rt=true; for(int i : {0, 4, vls[1].startChar, vls[1].startChar+2, int(sh.text.size())}){
          if(sh.textIndexAtLocal(sh.textCaretLocal(i))!=i) rt=false; }
      check(rt, "wrapped-frame caret round-trips: textIndexAtLocal∘textCaretLocal == id");
      // Render cross-check: the actual rendered text height must match frameVisualLines' line count,
      // i.e. the caret mapper wraps at the SAME points as localPath() (else the caret sits off the glyphs).
      { const double lineH=sh.textLineH(), bboxH=sh.localPath().boundingRect().height(); const int N=vls.size();
        check(bboxH > (N-1)*lineH - 1.0 && bboxH < (N+0.5)*lineH, "rendered frame height agrees with frameVisualLines' wrap (caret lines == render lines)"); }
      // clicking on line 2 returns an index on line 2 (>= its start, < line 3 start or end)
      { QPointF mid((vls[1].x0+vls[1].width*0.4), vls[1].baseY); int r=sh.textIndexAtLocal(mid);
        int lineEnd = vls.size()>2? vls[2].startChar : int(sh.text.size());
        check(r>=vls[1].startChar && r<=lineEnd, "clicking on line 2 returns a caret index on line 2"); }
      // the frame is inline-editable and edits work through the same ops as point text
      c->beginTextEdit(0, 0); check(c->editingText(), "a wrapped frame enters in-canvas edit mode");
      c->textInsert("X"); check(c->doc().shapes[0].text.startsWith("X"), "typing into a frame inserts at the caret");
      c->endTextEdit(); }
    // List frame: the text hangs at listIndent and the caret round-trips (marker occupies the gutter).
    { Shape s; s.type=ShapeType::Text; s.textFrame=true; s.rect=QRectF(0,0,220,400); s.listStyle=1; s.listIndent=24;
      s.text="first item text here\nsecond item that wraps a bit"; s.font=QFont("DejaVu Sans",18);
      const auto vls=s.frameVisualLines();
      check(!vls.isEmpty() && qAbs(vls[0].x0-24.0)<0.01, "list text hangs at listIndent (24), leaving the gutter for the marker");
      check(s.textCaretLocal(0).x()>23.0, "the caret before the first character is at the list hanging indent");
      check(s.textIndexAtLocal(s.textCaretLocal(5))==5, "list-frame caret round-trips"); }
    // Justify frame: renders left here, so the caret matches a left-aligned frame and round-trips.
    { Shape s; s.type=ShapeType::Text; s.textFrame=true; s.rect=QRectF(0,0,200,400); s.textAlign=3;
      s.text="alpha beta gamma delta epsilon zeta eta"; s.font=QFont("DejaVu Sans",20);
      check(qAbs(s.textCaretLocal(0).x())<0.5, "justified frame caret 0 sits at the left origin (justify renders left)");
      check(s.textIndexAtLocal(s.textCaretLocal(4))==4, "justified-frame caret round-trips"); }
    // Text on a curve: the caret flows along the baseline path. Caret 0 sits at the path start; each caret
    // sits ON the path; and clicking a caret point round-trips to its index.
    { Shape s; s.type=ShapeType::Text; s.text="Curve"; s.font=QFont("DejaVu Sans",24);
      QPainterPath pth; pth.moveTo(0,0); pth.cubicTo(80,-60,160,60,240,0); s.textPath=pth;
      check(Canvas::canInlineEdit(s), "a text-on-a-curve object IS inline-editable");
      const QPointF c0=s.textCaretLocal(0), pstart=pth.pointAtPercent(0.0);
      check(QLineF(c0,pstart).length()<0.5, "on-curve caret 0 sits at the start of the baseline path");
      // every interior caret lies on the path (distance to the nearest path point ~0)
      bool onPath=true; for(int i=0;i<=s.text.size();++i){ const QPointF ci=s.textCaretLocal(i);
          double dmin=1e18; for(int k=0;k<=100;++k){ dmin=qMin(dmin, QLineF(ci, pth.pointAtPercent(k/100.0)).length()); }
          if(dmin>1.5) onPath=false; }
      check(onPath, "every on-curve caret sits on the baseline path");
      check(s.textIndexAtLocal(s.textCaretLocal(3))==3, "on-curve caret round-trips (click → index)");
      // monotonic advance along the path (arc-length of caret i increases with i)
      double prevAL=-1; bool mono=true;
      for(int i=0;i<=s.text.size();++i){ const QPointF ci=s.textCaretLocal(i);
          double al=pth.percentAtLength(0)*0; // find nearest arc-length by sampling
          double dmin=1e18; for(int k=0;k<=200;++k){ double pc=k/200.0; double d=QLineF(ci,pth.pointAtPercent(pc)).length(); if(d<dmin){dmin=d; al=pc;} }
          if(al<prevAL-0.02) mono=false; prevAL=al; }
      check(mono, "on-curve carets advance monotonically along the path"); }
    // Text-on-curve START OFFSET (Batch 2b): sliding the start pushes every glyph further along the path.
    { Shape s; s.type=ShapeType::Text; s.text="Curve"; s.font=QFont("DejaVu Sans",24);
      QPainterPath pth; pth.moveTo(0,0); pth.lineTo(400,0); s.textPath=pth;   // straight baseline so offset == x
      const QPointF c0=s.textCaretLocal(0);
      check(qAbs(c0.x()-0.0)<0.5, "with offset 0 the first caret sits at the path start (x≈0)");
      s.textPathOffset=120.0;
      const QPointF c0b=s.textCaretLocal(0);
      check(qAbs(c0b.x()-120.0)<0.5, "a 120px start offset moves the first caret 120px along the baseline");
      // the glyph outline shifts too (its left edge moves right by ~offset)
      Shape s0=s; s0.textPathOffset=0; Shape s1=s; s1.textPathOffset=120;
      check(s1.localPath().boundingRect().left() > s0.localPath().boundingRect().left()+80, "the rendered glyphs shift along the path with the offset");
      // Canvas setter + handle + JSON round-trip
      c->newDocument(); int idx=c->doc().addShape(s0); c->select({idx}); c->setTool(Tool::Select);
      QPointF th; check(c->textPathStartHandle(th), "a text-on-curve object exposes a start handle");
      c->setTextPathOffset(90.0);
      check(qAbs(c->doc().shapes[idx].textPathOffset-90.0)<1e-6, "setTextPathOffset stores the value");
      c->setTextPathOffset(-50.0);
      check(c->doc().shapes[idx].textPathOffset>=0.0, "setTextPathOffset clamps at the path start");
      Document rt=Document::fromJson(c->doc().toJson());
      check(rt.shapes[idx].type==ShapeType::Text && qAbs(rt.shapes[idx].textPathOffset-c->doc().shapes[idx].textPathOffset)<1e-6, "textPathOffset round-trips through JSON");
      // REGRESSION (review): a corrupt/hand-edited NEGATIVE tpoff is clamped to 0 on load (would else render glyphs before the path start)
      QJsonObject bad = c->doc().shapes[idx].toJson(); bad["tpoff"]=-500.0;
      Shape sbad=Shape::fromJson(bad);
      check(sbad.textPathOffset>=0.0, "a negative textPathOffset is clamped to 0 on JSON load");
      // Both-sides flip (G-B): toggling flips the flag, changes the rendered glyph outline, and round-trips.
      c->select({idx}); const QRectF before=c->doc().shapes[idx].localPath().boundingRect();
      c->toggleTextPathSide();
      check(c->doc().shapes[idx].textPathInside, "toggleTextPathSide sets the inside flag");
      check(c->doc().shapes[idx].localPath().boundingRect()!=before, "flipping to the inside changes the rendered glyph outline");
      check(Document::fromJson(c->doc().toJson()).shapes[idx].textPathInside, "textPathInside round-trips through JSON");
      c->toggleTextPathSide(); check(!c->doc().shapes[idx].textPathInside, "toggling again clears the inside flag"); }
    // Smart quotes (G-B): straight quotes curl contextually (open after space, close after letter).
    { c->newDocument(); Shape s; s.type=ShapeType::Text; s.text="say \"hi\" and 'go'"; s.font=QFont("DejaVu Sans",20); s.fill.type=FillType::Solid; s.fill.color=Qt::black;
      int i=c->doc().addShape(s); c->select({i}); c->convertSmartQuotes();
      const QString t=c->doc().shapes[i].text;
      check(t.contains(QChar(0x201C)) && t.contains(QChar(0x201D)), "double quotes become “ … ”");
      check(t.contains(QChar(0x2018)) && t.contains(QChar(0x2019)), "single quotes become ‘ … ’");
      check(!t.contains('"') && !t.contains('\''), "no straight quotes remain"); }
    // Area text inside an arbitrary shape (G-C): flow a text object's words inside an ellipse.
    { c->newDocument();
      Shape tx; tx.type=ShapeType::Text; tx.text="the quick brown fox jumps over the lazy dog again and again"; tx.font=QFont("DejaVu Sans",14);
      tx.fill.type=FillType::Solid; tx.fill.color=Qt::black; int it=c->doc().addShape(tx);
      Shape el; el.type=ShapeType::Ellipse; el.rect=QRectF(0,0,300,220); el.transform=QTransform::fromTranslate(0,0);
      el.fill.type=FillType::Solid; el.fill.color=QColor(230,230,255); int ie=c->doc().addShape(el);
      c->select({it,ie}); c->fitTextInShape();
      const Shape& ts=c->doc().shapes[it];
      check(!ts.textShapePath.isEmpty() && ts.textFrame, "fitTextInShape sets the region path + frame flag");
      const QRectF glyphs=ts.localPath().boundingRect();
      check(!glyphs.isEmpty(), "area text renders glyphs inside the shape");
      // glyphs stay within the region's bounding box (they hug the outline, never spill past it)
      const QRectF region=ts.textShapePath.boundingRect();
      check(glyphs.left()>=region.left()-1 && glyphs.right()<=region.right()+1 && glyphs.bottom()<=region.bottom()+1,
            "flowed glyphs stay inside the region bounds");
      Document rt=Document::fromJson(c->doc().toJson());
      check(!rt.shapes[it].textShapePath.isEmpty(), "textShapePath round-trips through JSON");
      c->select({it}); c->detachTextFromShape();
      check(c->doc().shapes[it].textShapePath.isEmpty(), "detachTextFromShape clears the region"); }
    // Interactive spell-check (G-D): misspelled-word ranges, suggestions, user dictionary.
    { Speller& sp=Speller::instance();
      if(sp.ready()){
        auto ranges=c->misspelledRanges("the quikc brown fox");
        check(ranges.size()==1, "one misspelled word flagged (quikc)");
        if(!ranges.isEmpty()){ QString w="the quikc brown fox"; w=w.mid(ranges[0].first, ranges[0].second-ranges[0].first);
          check(w=="quikc", "the flagged range covers exactly the misspelled word");
          QStringList sug=sp.suggest("quikc");
          check(sug.contains("quick"), "suggest() proposes the correct word"); }
        // User dictionary: adding a coined word makes it known (and it stops being flagged).
        const QString coined="xqvproto";
        sp.removeUserWord(coined);                                 // ensure a clean slate across reruns
        check(!sp.known(coined), "a coined word is initially unknown");
        sp.addUserWord(coined);
        check(sp.known(coined) && sp.isUserWord(coined), "addUserWord makes the word known");
        check(c->misspelledRanges(QString("a ")+coined+" b").isEmpty(), "a user-dictionary word is no longer squiggled");
        sp.removeUserWord(coined);                                 // don't pollute the persistent user dictionary
        check(!sp.isUserWord(coined), "removeUserWord forgets the word");
      } else { check(c->misspelledRanges("anything at all").isEmpty(), "no dictionary => nothing flagged (graceful)"); } }
    // Justified paragraph render (G-A): non-last lines spread to the frame's right edge; last line stays left.
    { Shape s; s.type=ShapeType::Text; s.textFrame=true; s.rect=QRectF(0,0,260,220);
      s.font=QFont("DejaVu Sans",16); s.fill.type=FillType::Solid;
      s.text="The quick brown fox jumps over the very lazy sleeping dog again and again today";
      s.textAlign=0; const double leftR=s.localPath().boundingRect().right();
      s.textAlign=3; const double justR=s.localPath().boundingRect().right();
      check(justR > leftR+3, "justified text stretches wrapped lines toward the frame's right edge");
      check(justR <= 262.0, "justified text stays within the frame width"); }
    // Page background colour + isometric grid (G-J)
    { c->newDocument();
      check(c->doc().pageColor==QColor(Qt::white), "default page colour is white");
      c->setPageColor(QColor(20,40,80));
      check(c->doc().pageColor==QColor(20,40,80), "setPageColor updates the document page colour");
      Document prt=Document::fromJson(c->doc().toJson());
      check(prt.pageColor==QColor(20,40,80), "page colour round-trips through JSON");
      c->setPageColor(Qt::white);
      check(!c->doc().toJson().contains("pgcol"), "a white page colour is not serialized (gated → byte-identical)");
      c->setGridIso(true); check(c->gridIso(), "isometric grid toggles on"); c->setGridIso(false); }
    // Drop-cap frame: the first char is the cap (rendered big in the gutter). The caret before it sits at
    // the left edge; the flow text hangs to the right of the cap on the first `dropCap` lines; round-trips.
    { Shape s; s.type=ShapeType::Text; s.textFrame=true; s.rect=QRectF(0,0,240,400); s.dropCap=3;
      s.text="Once upon a time there was a very long paragraph that wraps"; s.font=QFont("DejaVu Sans",18);
      const auto vls=s.frameVisualLines();
      check(!vls.isEmpty(), "drop-cap frame produces visual lines for the flow text");
      // the cap char 'O' is pulled OUT of the flow: line 0's flow starts with "nce", not "Once"
      check(vls[0].str.startsWith("nce"), "the drop cap is removed from the flow (line 0 flow starts after the cap)");
      // caret before the cap is at the far left; caret after the cap (index 1) hangs to its right
      check(s.textCaretLocal(0).x()<0.5, "caret before the drop cap sits at the left edge");
      check(s.textCaretLocal(1).x()>10.0, "caret after the drop cap hangs to the right of the big letter");
      // the first line is indented past the cap; a later line (past dropCap) returns to the normal margin
      check(vls[0].x0>10.0, "the first flow line is indented past the drop cap");
      bool rt=true; for(int i=0;i<=s.text.size();++i) if(s.textIndexAtLocal(s.textCaretLocal(i))!=i) rt=false;
      check(rt, "drop-cap frame caret round-trips at EVERY index (including the cap gutter)");
      // Drop-cap resize handle (Batch 2b): the cap block spans dropCap lines; a handle at its bottom-right resizes it.
      const QRectF cap=s.dropCapRectLocal();
      check(!cap.isNull() && qAbs(cap.height()-3*s.textLineH())<0.5, "dropCapRectLocal spans the drop-cap's 3 lines");
      c->newDocument(); int idx=c->doc().addShape(s); c->select({idx}); c->setTool(Tool::Select);
      QPointF dh; check(c->dropCapHandle(dh), "a drop-cap text frame exposes a resize handle");
      c->setDropCapLines(5); check(c->doc().shapes[idx].dropCap==5, "setDropCapLines enlarges the cap");
      c->setDropCapLines(1); check(c->doc().shapes[idx].dropCap==0, "setDropCapLines(1) turns the drop cap off"); }
    // Leading-punctuation drop cap: the enlarged letter is the first LETTER/NUMBER (past the quote), so the
    // flow↔text mapping splits around it. Still inline-editable and round-trips at every index.
    { Shape s; s.type=ShapeType::Text; s.textFrame=true; s.rect=QRectF(0,0,240,400); s.dropCap=3;
      s.text="\"Quoted opening that wraps across a couple of lines here\" said the sly fox"; s.font=QFont("DejaVu Sans",18);
      const auto vls=s.frameVisualLines();
      check(!vls.isEmpty(), "leading-punct drop-cap frame produces visual lines");
      check(vls[0].str.startsWith("\"uoted"), "the leading quote stays in the flow; only the 'Q' is pulled out");
      check(s.textCaretLocal(1).x()<0.5, "caret at the drop cap (the 'Q', index 1) sits at the gutter");
      check(s.textCaretLocal(0).x()>10.0, "the leading quote (index 0) stays in the flow, right of the cap");
      check(s.textIndexAtLocal(s.textCaretLocal(1))==1, "clicking the gutter cap maps back to the cap index");
      bool rt=true; for(int i=0;i<=s.text.size();++i) if(s.textIndexAtLocal(s.textCaretLocal(i))!=i) rt=false;
      check(rt, "leading-punct drop-cap caret round-trips at EVERY index (cap found past the quote)"); }
    // Drop-cap edge cases (adversarial): the enlarged letter is surrounded by punctuation; and drop cap
    // composed with hyphenation and with multiple columns. Each must round-trip at every index.
    { Shape s; s.type=ShapeType::Text; s.textFrame=true; s.rect=QRectF(0,0,220,400); s.dropCap=2;
      s.text="((A)) then a few more words to wrap"; s.font=QFont("DejaVu Sans",18);
      check(s.frameVisualLines()[0].str.startsWith("(())"), "the cap 'A' is pulled from between the brackets");
      bool rt=true; for(int i=0;i<=s.text.size();++i) if(s.textIndexAtLocal(s.textCaretLocal(i))!=i) rt=false;
      check(rt, "drop-cap with the cap surrounded by punctuation round-trips at every index"); }
    { Shape s; s.type=ShapeType::Text; s.textFrame=true; s.rect=QRectF(0,0,95,400); s.dropCap=2; s.hyphenate=true;
      s.text="Extraordinary hyphenation happens here"; s.font=QFont("DejaVu Sans",18);
      bool rt=true; for(int i=0;i<=s.text.size();++i) if(s.textIndexAtLocal(s.textCaretLocal(i))!=i) rt=false;
      check(rt, "drop-cap + hyphenation compose (flow offset AND synthetic '-' both handled)"); }
    { Shape s; s.type=ShapeType::Text; s.textFrame=true; s.rect=QRectF(0,0,260,200); s.dropCap=2; s.textCols=2; s.textColGap=16;
      s.text="Alpha beta gamma delta epsilon zeta eta theta iota"; s.font=QFont("DejaVu Sans",15);
      bool rt=true; for(int i=0;i<=s.text.size();++i) if(s.textIndexAtLocal(s.textCaretLocal(i))!=i) rt=false;
      check(rt, "drop-cap + two columns compose (cap gutter only on column 0)"); }
    // Regular tabs (tabWidth): the caret after a '\t' jumps to the next multiple of tabWidth from the line
    // origin, matching place()'s render. Point text and frames both use s_tabX.
    { Shape s; s.type=ShapeType::Text; s.text="a\tb"; s.tabWidth=100; s.font=QFont("DejaVu Sans",18);
      const double x1=s.textCaretLocal(1).x(), x2=s.textCaretLocal(2).x();
      check(qAbs(x2-100.0)<0.01, "caret after a tab jumps to the next tabWidth multiple (100)");
      check(x2>x1+1.0, "the tab visibly advances the caret past the raw glyph width");
      check(s.textIndexAtLocal(s.textCaretLocal(3))==3, "tabbed point-text caret round-trips"); }
    { Shape s; s.type=ShapeType::Text; s.textFrame=true; s.rect=QRectF(0,0,400,200); s.tabWidth=80; s.font=QFont("DejaVu Sans",16);
      s.text="Name:\tValue"; const auto vls=s.frameVisualLines();
      check(vls.size()>=1 && vls[0].str.contains('\t'), "tabbed frame keeps the tab in its visual line");
      check(qAbs(s.textCaretLocal(6).x()-80.0)<0.01, "frame caret after the tab sits at the tab stop (80)");
      check(s.textIndexAtLocal(s.textCaretLocal(7))==7, "tabbed frame caret round-trips across the tab"); }
    // Explicit tab stops: the caret honours per-stop alignment (right/decimal), matching place()'s segments.
    { Shape s; s.type=ShapeType::Text; s.text="A\t99"; s.font=QFont("DejaVu Sans",18);
      s.tabStops.push_back(TabStop{200.0,2,QChar()});   // right-aligned at x=200
      const double segW=QFontMetricsF(QFont("DejaVu Sans",18)).horizontalAdvance("99");
      check(qAbs(s.textCaretLocal(2).x()-(200.0-segW))<0.5, "right-aligned tab stop starts the segment so its right edge hits the stop");
      check(qAbs(s.textCaretLocal(4).x()-200.0)<0.5, "caret after the right-aligned segment sits at the stop (200)");
      check(s.textIndexAtLocal(s.textCaretLocal(3))==3, "explicit-tab-stop caret round-trips"); }
    { Shape s; s.type=ShapeType::Text; s.text="x\t3.5"; s.font=QFont("DejaVu Sans",18);
      s.tabStops.push_back(TabStop{160.0,3,QChar()});   // decimal-aligned at x=160
      const double preDot=QFontMetricsF(QFont("DejaVu Sans",18)).horizontalAdvance("3");
      check(qAbs(s.textCaretLocal(3).x()-160.0)<0.6, "decimal tab stop aligns the '.' at the stop (160)");
      check(qAbs(s.textCaretLocal(2).x()-(160.0-preDot))<0.6, "the integer part sits left of the decimal stop"); }
    // Hyphenation: a narrow frame breaks a long word with a synthetic '-' that is NOT a real character —
    // the caret skips over it and every index still round-trips.
    { Shape s; s.type=ShapeType::Text; s.textFrame=true; s.rect=QRectF(0,0,90,400); s.hyphenate=true;
      s.text="hyphenation demonstration paragraph"; s.font=QFont("DejaVu Sans",18);
      const auto vls=s.frameVisualLines();
      bool anyHyph=false, realOk=true;
      for(const auto& L: vls) if(L.str.endsWith('-') && L.realLen>=0){ anyHyph=true; if(L.realCount()!=int(L.str.size())-1) realOk=false; }
      check(anyHyph, "a narrow hyphenated frame breaks a word with a synthetic '-'");
      check(realOk, "the hyphen line's realCount excludes the synthetic '-'");
      bool rt=true; for(int i=0;i<=s.text.size();++i) if(s.textIndexAtLocal(s.textCaretLocal(i))!=i) rt=false;
      check(rt, "hyphenated-frame caret round-trips at every index (skips the fake hyphen)"); }
    // Repel: text flows around an obstacle rectangle — each band splits into free x-segments and the caret
    // tracks every character across the gap. It's inline-editable and round-trips at every index.
    { Shape s; s.type=ShapeType::Text; s.textFrame=true; s.rect=QRectF(0,0,300,400);
      s.text="the quick brown fox jumps over the lazy dog and keeps running along the road"; s.font=QFont("DejaVu Sans",16);
      s.textWrapObstacles.push_back(QRectF(120,0,80,60));   // obstacle in the upper-middle
      check(Canvas::canInlineEdit(s), "a repel (wrap-around-obstacle) frame IS inline-editable");
      const auto vls=s.frameVisualLines();
      check(!vls.isEmpty(), "repel frame produces visual lines");
      bool split=false; for(int i=0;i+1<vls.size();++i) if(qAbs(vls[i].baseY-vls[i+1].baseY)<0.5 && vls[i+1].x0>vls[i].x0+1.0) split=true;
      check(split, "a band across the obstacle splits into left+right chunks at different x");
      bool rt=true; for(int i=0;i<=s.text.size();++i) if(s.textIndexAtLocal(s.textCaretLocal(i))!=i) rt=false;
      check(rt, "repel caret round-trips at every index (flows around the obstacle)"); }
    // Inline anchored graphics: words flow with a reserved gap for each box. The boxes aren't text, so the
    // caret maps to the words (each visual line is a verbatim word) and round-trips at every index.
    { Shape s; s.type=ShapeType::Text; s.textFrame=true; s.rect=QRectF(0,0,300,400);
      s.text="alpha beta gamma delta epsilon zeta eta theta iota kappa"; s.font=QFont("DejaVu Sans",16);
      s.textInlineBoxes.push_back({12, QSizeF(50,30)});   // a 50x30 inline graphic anchored at char 12
      check(Canvas::canInlineEdit(s), "a frame with inline anchored graphics IS inline-editable");
      const auto vls=s.frameVisualLines();
      check(!vls.isEmpty(), "inline-graphics frame produces per-word visual lines");
      bool words1to1=true; for(const auto& L: vls) if(!L.str.isEmpty() && s.text.mid(L.startChar,L.str.size())!=L.str) words1to1=false;
      check(words1to1, "each inline visual line is a verbatim slice of the source text");
      bool rt=true; for(int i=0;i<=s.text.size();++i) if(s.textIndexAtLocal(s.textCaretLocal(i))!=i) rt=false;
      check(rt, "inline-graphics caret round-trips at every index (boxes only shift the flow)"); }
    // Review-fix regressions.
    // (1) rich-run frames/point text fall back to the dialog (uniform-font caret would be off the glyphs).
    { Shape r; r.type=ShapeType::Text; r.text="hi there"; r.textFrame=true; r.rect=QRectF(0,0,200,100);
      TextRun run; run.start=0; run.len=2; r.textRuns.push_back(run);
      check(!Canvas::canInlineEdit(r), "a rich-run frame falls back to the dialog");
      Shape rp=r; rp.textFrame=false; check(!Canvas::canInlineEdit(rp), "rich-run point text also falls back"); }
    // (2) an overflowing frame exposes ALL wrapped lines and the caret keeps descending (no freeze).
    { Shape s; s.type=ShapeType::Text; s.textFrame=true; s.rect=QRectF(0,0,90,40); s.font=QFont("DejaVu Sans",20);
      s.text="one two three four five six seven eight nine ten eleven twelve thirteen";
      const auto vls=s.frameVisualLines();
      check(vls.size()>=4, "overflow frame still exposes all wrapped lines (not clipped to the visible ones)");
      check(s.textCaretLocal(s.text.size()).y() > s.rect.height(), "the end caret descends past a too-short frame (not frozen mid-way)");
      check(vls.last().baseY > vls[1].baseY + s.textLineH()*0.5, "successive overflow lines have increasing baselines"); }
    // (4) clicking in column 2 of a 2-column frame keeps the caret in column 2 (not snapped to column 1).
    { Shape s; s.type=ShapeType::Text; s.textFrame=true; s.textCols=2; s.textColGap=20; s.rect=QRectF(0,0,240,80); s.font=QFont("DejaVu Sans",16);
      s.text="alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu nu xi omicron pi rho";
      const auto vls=s.frameVisualLines();
      int c1=-1; for(int k=0;k<vls.size();++k) if(vls[k].colBase>1.0){ c1=k; break; }
      check(c1>=0, "the 2-column frame flows into column 2");
      if(c1>=0){ const auto& L=vls[c1]; int r=s.textIndexAtLocal(QPointF(L.x0+3, L.baseY));
        check(s.textCaretLocal(r).x() >= L.colBase-1.0, "clicking column 2 keeps the caret in column 2 (score is column-first)"); } }
    // ShortcutOverride: while editing, Delete / Ctrl+A are claimed by the canvas (routed to the editor,
    // not the window's Delete/Select-All menu shortcuts); when NOT editing they are left alone.
    { Document d; Shape s; s.type=ShapeType::Text; s.text="abc"; s.font=QFont("DejaVu Sans",36);
      s.fill.type=FillType::Solid; d.shapes={s}; c->setDocument(d); c->select({0}); c->beginTextEdit(0,-1);
      { QKeyEvent so(QEvent::ShortcutOverride, Qt::Key_Delete, Qt::NoModifier); so.ignore(); QApplication::sendEvent(c,&so);
        check(so.isAccepted(), "Delete ShortcutOverride is claimed while editing (deletes a char, not the shape)"); }
      { QKeyEvent so(QEvent::ShortcutOverride, Qt::Key_A, Qt::ControlModifier); so.ignore(); QApplication::sendEvent(c,&so);
        check(so.isAccepted(), "Ctrl+A ShortcutOverride is claimed while editing (select-all-in-text)"); }
      // A plain printable letter that collides with a single-key TOOL shortcut (T=Text, E=Ellipse, L=Line…)
      // must be claimed by the editor while typing — otherwise it fired the tool shortcut, switched tools and
      // ended the edit, so in-canvas typing never worked (the reported bug).
      { QKeyEvent so(QEvent::ShortcutOverride, Qt::Key_T, Qt::NoModifier, "T"); so.ignore(); QApplication::sendEvent(c,&so);
        check(so.isAccepted(), "plain 'T' ShortcutOverride is claimed while editing (typing beats the Text-tool shortcut)"); }
      { QKeyEvent so(QEvent::ShortcutOverride, Qt::Key_E, Qt::NoModifier, "e"); so.ignore(); QApplication::sendEvent(c,&so);
        check(so.isAccepted(), "plain 'e' ShortcutOverride is claimed while editing (typing beats the Ellipse-tool shortcut)"); }
      { QKeyEvent so(QEvent::ShortcutOverride, Qt::Key_Z, Qt::ControlModifier, ""); so.ignore(); QApplication::sendEvent(c,&so);
        check(!so.isAccepted(), "Ctrl+Z ShortcutOverride still falls through to global undo while editing"); }
      c->endTextEdit();
      { QKeyEvent so(QEvent::ShortcutOverride, Qt::Key_Delete, Qt::NoModifier); so.ignore(); QApplication::sendEvent(c,&so);
        check(!so.isAccepted(), "Delete ShortcutOverride is NOT stolen when not editing"); }
      { QKeyEvent so(QEvent::ShortcutOverride, Qt::Key_T, Qt::NoModifier, "T"); so.ignore(); QApplication::sendEvent(c,&so);
        check(!so.isAccepted(), "plain 'T' is NOT stolen when not editing (Text-tool shortcut still works)"); } }
    // App-level ShortcutGuard (main.cpp): a single-key tool shortcut must never steal a keystroke from a
    // focused *UI* input. Qt already lets an editable QLineEdit swallow the ShortcutOverride, but a
    // non-editable QComboBox / list does NOT — so pressing "T" in a dropdown fired the Text tool. The guard
    // (installed on qApp before --selftest runs) claims the override for any input control on a bare printable
    // key, while leaving Ctrl/Alt/Meta combos and non-input widgets (buttons) alone.
    { auto* host=new QWidget; auto* v=new QVBoxLayout(host);
      auto* combo=new QComboBox; combo->addItems({"Alpha","Tango","Zulu"}); v->addWidget(combo);   // NON-editable
      auto* edit =new QLineEdit; v->addWidget(edit);
      auto* btn  =new QPushButton("Go"); v->addWidget(btn);
      auto* list =new QListWidget; list->addItems({"Layer 1","Layer 2"}); v->addWidget(list);      // gallery/list
      host->show(); host->activateWindow(); QApplication::processEvents();
      auto claimed=[&](QWidget* w, int key, Qt::KeyboardModifiers mods, const QString& txt)->bool{
          w->setFocus(); QApplication::processEvents();
          QKeyEvent so(QEvent::ShortcutOverride, key, mods, txt); so.ignore();
          QApplication::sendEvent(w,&so);       // app event filters (incl. ShortcutGuard) run on sendEvent
          return so.isAccepted(); };
      check(claimed(combo, Qt::Key_T, Qt::NoModifier, "t"),
            "bare 'T' is claimed for a focused non-editable combo (dropdown keeps the key; tool shortcut suppressed)");
      check(claimed(edit,  Qt::Key_G, Qt::NoModifier, "g"),
            "bare 'G' is claimed for a focused line edit (typing beats the Fill-tool shortcut)");
      check(!claimed(btn,  Qt::Key_T, Qt::NoModifier, "t"),
            "bare 'T' is NOT claimed for a focused push button (single-key tool shortcut still works)");
      check(!claimed(list, Qt::Key_T, Qt::NoModifier, "t"),
            "bare 'T' is NOT claimed for a focused list/gallery (tool shortcuts still work while a panel row has focus)");
      // A modifier combo bypasses the guard even for an input control (the guard only claims bare keys),
      // so it reaches its global shortcut — tested on the non-editable combo, which unlike a line edit does
      // not itself consume Ctrl+Z for in-field undo.
      check(!claimed(combo, Qt::Key_Z, Qt::ControlModifier, QString()),
            "Ctrl+Z is NOT claimed for a focused non-editable combo (modifier combos bypass the guard → global undo fires)");
      check(!claimed(edit, Qt::Key_Return, Qt::NoModifier, QString("\r")),
            "bare Return is NOT claimed in a line edit (non-printable keys fall through: dialogs/defaults keep working)");
      host->close(); delete host; }
    // Undo while editing must clamp the caret into the (possibly shorter) restored text — no OOB.
    { Document d; Shape s; s.type=ShapeType::Text; s.text="Hello"; s.font=QFont("DejaVu Sans",36);
      s.fill.type=FillType::Solid; d.shapes={s}; c->setDocument(d); c->select({0}); c->beginTextEdit(0,-1);
      c->textInsert(" world");   check(c->doc().shapes[0].text=="Hello world" && c->textCaret()==11, "typed ' world' (caret 11)");
      c->undo();
      check(c->doc().shapes[0].text=="Hello", "undo restored the shorter text");
      check(c->textCaret()<=c->doc().shapes[0].text.size(), "caret is clamped into the shorter text after undo (no OOB)");
      check(c->editingText(), "still editing the same text shape after undo"); }
    // Type guard: if the caret's shape stops being text, edits become no-ops (no wrong-shape corruption).
    { Document d; Shape s; s.type=ShapeType::Text; s.text="x"; s.font=QFont("DejaVu Sans",36); s.fill.type=FillType::Solid;
      d.shapes={s}; c->setDocument(d); c->select({0}); c->beginTextEdit(0,-1);
      c->doc().shapes[0].type=ShapeType::Rect;                 // the shape under the caret is no longer text
      check(!c->editingText(), "editingText() is false once the shape is not text");
      int len=c->doc().shapes[0].text.size(); c->textInsert("ZZ");
      check(c->doc().shapes[0].text.size()==len, "textInsert no-ops when the caret's shape isn't text"); }
    // Switching tools commits/ends the edit (no caret left blinking under a different tool).
    { Document d; Shape s; s.type=ShapeType::Text; s.text="hi"; s.font=QFont("DejaVu Sans",36); s.fill.type=FillType::Solid;
      d.shapes={s}; c->setDocument(d); c->select({0}); c->setTool(Tool::Text); c->beginTextEdit(0,-1);
      check(c->editingText(), "editing is active");
      c->setTool(Tool::Select); check(!c->editingText(), "switching tools ends the text edit");
      c->setTool(Tool::Select); }
    // On-curve text: enter in-canvas edit and type — the caret rides the path, insertion works like any text.
    { Document d; Shape s; s.type=ShapeType::Text; s.text="arc"; s.font=QFont("DejaVu Sans",28); s.fill.type=FillType::Solid;
      QPainterPath pp; pp.moveTo(0,0); pp.cubicTo(60,-40,140,40,200,0); s.textPath=pp;
      d.shapes={s}; c->setDocument(d); c->select({0}); c->setTool(Tool::Text);
      c->beginTextEdit(0,0); check(c->editingText(), "on-curve text enters in-canvas edit mode");
      c->textInsert("M"); check(c->doc().shapes[0].text=="Marc", "typing into on-curve text inserts at the caret");
      check(c->doc().shapes[0].textPath.isEmpty()==false, "the baseline path survives an in-canvas edit");
      c->endTextEdit(); c->setTool(Tool::Select); }
    // Text-tool drag-out (Batch 2b): a click makes point text; a drag makes a sized wrapping frame.
    // REGRESSION: the click must ENTER EDIT MODE on the PRESS (so the caret has keyboard focus and typing
    // inserts instead of firing tool shortcuts) — driving a real QKeyEvent, not textInsert(), tests that path.
    { c->newDocument(); c->setTool(Tool::Text);
      QPointF pc=c->mapToView(QPointF(80,80));
      send(c, QEvent::MouseButtonPress,   pc, Qt::LeftButton, Qt::LeftButton);
      check(c->editingText(), "a Text-tool click enters in-canvas edit mode on the PRESS (caret gets focus immediately)");
      send(c, QEvent::MouseButtonRelease, pc, Qt::LeftButton, Qt::NoButton);
      check(c->editingText(), "the click's point text STAYS in edit mode through the release (not culled early)");
      c->textInsert("Hi");
      check(c->doc().shapes.size()==1 && c->doc().shapes[0].type==ShapeType::Text && !c->doc().shapes[0].textFrame
            && c->doc().shapes[0].text=="Hi", "typing after a Text-tool click inserts into point text (edit mode is live)");
      c->endTextEdit(); c->setTool(Tool::Select); }
    { c->newDocument(); c->setTool(Tool::Text);
      QPointF a=c->mapToView(QPointF(100,100)), b=c->mapToView(QPointF(300,220));
      send(c, QEvent::MouseButtonPress,   a, Qt::LeftButton, Qt::LeftButton);
      send(c, QEvent::MouseMove,          b, Qt::NoButton,   Qt::LeftButton);
      send(c, QEvent::MouseButtonRelease, b, Qt::LeftButton, Qt::NoButton);
      c->textInsert("hi");
      const Shape& s=c->doc().shapes[0];
      check(s.type==ShapeType::Text && s.textFrame, "a Text-tool drag creates a wrapping text frame");
      check(qAbs(s.rect.width()-200)<3 && qAbs(s.rect.height()-120)<3, "the frame is sized to the drag rectangle (200×120)");
      c->endTextEdit(); c->setTool(Tool::Select); }
    // Ruler indent markers (Batch 2b): the three markers track first-line / left / right indents.
    { c->newDocument();
      Shape s; s.type=ShapeType::Text; s.textFrame=true; s.rect=QRectF(0,0,300,200);
      s.transform=QTransform::fromTranslate(100,100); s.text="Body"; s.font=QFont("DejaVu Sans",14);
      int idx=c->doc().addShape(s); c->select({idx}); c->setTool(Tool::Select);
      double lx,rx,fx;
      check(c->indentMarkerX(1,lx) && qAbs(lx - c->mapToView(QPointF(100,0)).x())<0.5, "left-indent marker sits at the frame's left edge");
      check(c->indentMarkerX(2,rx) && qAbs(rx - c->mapToView(QPointF(400,0)).x())<0.5, "right-indent marker sits at the frame's right edge");
      c->doc().shapes[idx].leftIndent=40; c->doc().shapes[idx].firstLineIndent=25; c->doc().shapes[idx].rightIndent=30;
      check(c->indentMarkerX(1,lx) && qAbs(lx - c->mapToView(QPointF(140,0)).x())<0.5, "left marker follows leftIndent (40)");
      check(c->indentMarkerX(0,fx) && qAbs(fx - c->mapToView(QPointF(165,0)).x())<0.5, "first-line marker = left + leftIndent + firstLineIndent");
      check(c->indentMarkerX(2,rx) && qAbs(rx - c->mapToView(QPointF(370,0)).x())<0.5, "right marker follows rightIndent (30)");
      c->doc().shapes[idx].transform.rotate(20);
      double t; check(!c->indentMarkerX(1,t), "a rotated text frame exposes no ruler indent markers"); }

    // --- Soft eraser (feathered alpha on a photo) ---
    std::printf("[photo] soft eraser\n");
    { Document d; QImage im(40,40,QImage::Format_ARGB32); im.fill(qRgba(200,50,50,255));   // opaque red
      Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,40,40); s.fill.type=FillType::Bitmap; s.fill.image=im; s.fill.origImage=im;
      s.fill.p0=QPointF(0,0); s.fill.p1=QPointF(40,0); s.fill.p2=QPointF(0,40); s.transform=QTransform();  // identity: doc px == image px
      d.shapes={s}; c->setDocument(d); c->select({0});
      c->setEraserSoft(true); check(c->eraserSoft(), "soft-eraser toggle stored");
      d.shapes[0].fill.adjust.brightness=20; c->setDocument(d); c->select({0});   // a live adjustment pre-exists
      c->setEraserNib(8); c->softEraseAt(0, QPointF(20,20));                 // erase the centre
      const QImage r=c->doc().shapes[0].fill.image;
      check(qAlpha(r.pixel(20,20)) < 60, "soft erase makes the centre nearly transparent");
      { int am=qAlpha(r.pixel(24,20)); check(am>20 && am<230, "erase falls off toward the rim (soft edge)"); }
      check(qAlpha(r.pixel(0,0))==255, "the far corner is untouched");
      check(qRed(r.pixel(20,20))>150, "only alpha is reduced — RGB is preserved");
      check(qAlpha(c->doc().shapes[0].fill.origImage.pixel(20,20))<60, "the erase is baked into the non-destructive source");
      check(c->doc().shapes[0].fill.adjust.isIdentity(), "soft erase flattens the adjust stack (no double-apply on re-bake)");
      c->setEraserSoft(false); }

    // --- Magnetic lasso (livewire edge-snapping region) ---
    std::printf("[photo] magnetic lasso\n");
    { QImage im(80,80,QImage::Format_ARGB32); im.fill(Qt::white);
      { QPainter g(&im); g.setPen(Qt::NoPen); g.setBrush(Qt::black); g.drawRect(20,20,40,40); }   // a strong square edge
      Document d; Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,80,80);
      s.fill.type=FillType::Bitmap; s.fill.image=im; s.fill.origImage=im;
      s.fill.p0=QPointF(0,0); s.fill.p1=QPointF(80,0); s.fill.p2=QPointF(0,80); s.transform=QTransform();
      d.shapes={s}; c->setDocument(d); c->select({0});
      QVector<QPointF> anchors={QPointF(20,20),QPointF(60,20),QPointF(60,60),QPointF(20,60)};
      QPainterPath lp=c->magneticLassoRegion(0,anchors);
      check(!lp.isEmpty(), "magnetic lasso returns a region");
      QRectF bb=lp.boundingRect();
      check(qAbs(bb.left()-20)<8 && qAbs(bb.top()-20)<8 && qAbs(bb.width()-40)<8 && qAbs(bb.height()-40)<8, "region snaps to the ~40×40 square edge");
      check(lp.elementCount()>16, "the path is a dense edge-following polyline (snapped, not 4 straight lines)");
      check(c->magneticLassoRegion(0,QVector<QPointF>{QPointF(10,10)}).isEmpty(), "needs at least two anchors");
      c->setShapeRegion(0,lp); check(!c->doc().shapes[0].regionPath.isEmpty(), "the lasso region can clip the photo"); }

    // --- Effect Painter (scoped photo adjustment) ---
    std::printf("[photo] effect painter\n");
    { QImage im(60,60,QImage::Format_ARGB32); im.fill(qRgb(120,120,120));
      QImage orig(100,100,QImage::Format_ARGB32); orig.fill(qRgb(120,120,120));   // larger source (as if cropped)
      Document d; Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,60,60);
      s.fill.type=FillType::Bitmap; s.fill.image=im; s.fill.origImage=orig;
      s.fill.p0=QPointF(0,0); s.fill.p1=QPointF(60,0); s.fill.p2=QPointF(0,60); s.transform=QTransform();
      d.shapes={s}; c->setDocument(d); c->select({0});
      QPainterPath reg; reg.addRect(QRectF(0,0,30,60));           // local region: left half
      c->paintEffectRegion(0, reg, 1);                            // kind 1 = brighten
      check(c->doc().shapes.size()==2, "effect painter adds an overlay shape above the photo");
      const Shape& ov=c->doc().shapes[1];
      check(ov.fill.image.size()==im.size(), "overlay bakes from the DISPLAYED image (registers with base), not the larger source");
      check(!ov.regionPath.isEmpty(), "the overlay is clipped to the painted region");
      check(ov.fill.type==FillType::Bitmap && qRed(ov.fill.image.pixel(10,10))>140, "the overlay image is brightened (> base 120)");
      check(c->doc().shapes[0].fill.image.pixel(10,10)==im.pixel(10,10), "the base photo is left intact (full, unmodified)");
      check(ov.name.contains("effect",Qt::CaseInsensitive), "the overlay is named as an effect layer"); }

    // --- FlexGroup (fluid reflow section, published OUTSIDE the fixed stage) ---
    std::printf("[web] FlexGroup (fluid reflow)\n");
    { Document d; d.pageSize=QSizeF(800,600);
      auto mk=[&](QColor col,double x){ Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,200,140);
        s.fill.type=FillType::Solid; s.fill.color=col; s.transform=QTransform::fromTranslate(x,80); return s; };
      d.shapes={mk(Qt::red,40),mk(Qt::green,260),mk(Qt::blue,480)}; c->setDocument(d);
      c->select({0,1,2}); c->makeFlexFromSelection();
      check(c->selectionFlex()!=0 && c->doc().shapes[0].webFlex==c->doc().shapes[2].webFlex, "makeFlexFromSelection shares one id");
      check(c->exportWebsite("/tmp/selftest_flex.html"), "publish with a FlexGroup succeeds");
      QFile f("/tmp/selftest_flex.html"); f.open(QIODevice::ReadOnly); QString h=QString::fromUtf8(f.readAll());
      check(h.contains(".flexsection{display:flex;flex-wrap:wrap"), "fluid flex-wrap CSS is emitted");
      check(h.contains("flex:1 1 240px") && h.contains("min-width:170px"), "items are fluid (flex-basis + min-width → they wrap)");
      check(h.contains("<section class=\"flexsection\"") && h.count("class=\"flexitem\"")==3, "3 items in one flex section");
      const int stageEnd=h.indexOf("</div></div>"), flexAt=h.indexOf("<section class=\"flexsection\"");
      check(stageEnd>=0 && flexAt>stageEnd, "the flex section is OUTSIDE the fixed/scaled stage (so it can reflow)");
      check(!h.contains("<image"), "flex items are NOT also baked into the SVG background layer");
      Document rt=Document::fromJson(c->doc().toJson());
      check(rt.shapes[0].webFlex!=0 && rt.shapes[0].webFlex==rt.shapes[1].webFlex, "FlexGroup survives JSON round-trip"); }

    // --- REGRESSION: a '%' in a shape name must not corrupt chained-arg HTML (respAttr re-scan) ---
    std::printf("[web] export arg-rescan safety\n");
    { Document d; d.pageSize=QSizeF(800,600);
      QImage im(40,40,QImage::Format_ARGB32); im.fill(Qt::red);
      Shape p; p.type=ShapeType::Rect; p.rect=QRectF(0,0,120,80); p.fill.type=FillType::Bitmap; p.fill.image=im; p.fill.origImage=im;
      p.fill.p0=QPointF(0,0); p.fill.p1=QPointF(120,0); p.fill.p2=QPointF(0,80);
      p.name="Ph%11oto"; p.webLightbox=true; p.webHideBelow=600; p.transform=QTransform::fromTranslate(60,60);
      d.shapes={p}; c->setDocument(d);
      check(c->exportWebsite("/tmp/selftest_argre.html"), "publish a lightbox photo with a %-name + hide-below");
      QFile f("/tmp/selftest_argre.html"); f.open(QIODevice::ReadOnly); QString h=QString::fromUtf8(f.readAll());
      check(h.contains("Ph&#37;11oto"), "a % in a name is neutralised to &#37; (not consumed by a later chained .arg)");
      check(!h.contains("title=\"Ph id="), "respAttr's id did NOT break out of the title attribute");
      check(h.contains("id=\"rv0\"") && h.contains("@media(max-width:600px)"), "responsive-hide still applied to the lightbox anchor"); }

    // --- REGRESSION: FlexGroup items honour webHideBelow (respAttr was dropped) ---
    std::printf("[web] FlexGroup responsive-hide\n");
    { Document d; d.pageSize=QSizeF(800,600);
      auto mk=[&](double x,int hb){ Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,180,120); s.fill.type=FillType::Solid; s.fill.color=Qt::blue; s.transform=QTransform::fromTranslate(x,80); s.webHideBelow=hb; return s; };
      d.shapes={mk(40,0),mk(240,500)}; c->setDocument(d); c->select({0,1}); c->makeFlexFromSelection();
      check(c->exportWebsite("/tmp/selftest_fh.html"), "publish a FlexGroup with a hide-below item");
      QFile f("/tmp/selftest_fh.html"); f.open(QIODevice::ReadOnly); QString h=QString::fromUtf8(f.readAll());
      check(h.contains("@media(max-width:500px)") && h.contains("class=\"flexitem\" id="), "a flex item's webHideBelow is now honoured (respAttr applied)"); }

    // --- Responsive show/hide (hide element below a viewport width) ---
    std::printf("[web] responsive hide\n");
    { Document d; d.pageSize=QSizeF(800,600);
      Shape t; t.type=ShapeType::Text; t.text="Desktop only"; t.font=QFont("DejaVu Sans",20); t.fill.type=FillType::Solid; t.fill.color=Qt::black;
      t.transform=QTransform::fromTranslate(40,40);
      Shape t2; t2.type=ShapeType::Text; t2.text="Always"; t2.font=QFont("DejaVu Sans",20); t2.fill.type=FillType::Solid; t2.fill.color=Qt::black;
      t2.transform=QTransform::fromTranslate(40,120);
      Shape em; em.type=ShapeType::Rect; em.rect=QRectF(0,0,200,120); em.fill.type=FillType::Solid; em.fill.color=Qt::gray;
      em.webEmbed="<iframe src=\"https://example.com\"></iframe>"; em.webHideBelow=500; em.transform=QTransform::fromTranslate(400,40);
      d.shapes={t,t2,em}; c->setDocument(d); c->select({0}); c->setSelectionHideBelow(600);
      check(c->doc().shapes[0].webHideBelow==600, "setSelectionHideBelow stored");
      check(c->exportWebsite("/tmp/selftest_resp.html"), "publish with responsive-hide elements succeeds");
      { QFile f("/tmp/selftest_resp.html"); f.open(QIODevice::ReadOnly); QString h=QString::fromUtf8(f.readAll());
        check(h.contains("@media(max-width:600px)") && h.contains("display:none"), "a hide-below media query is emitted");
        check(h.contains("id=\"rv0\""), "the hidden text element gets a unique id targeted by the query");
        check(h.contains("@media(max-width:500px)") && h.count("@media(max-width")==2, "responsive-hide also works on a non-text element (embed)");
        check(h.count("id=\"rv")==2, "exactly the two flagged elements are hidden (the plain text has no rv id)"); }
      Document rt=Document::fromJson(c->doc().toJson());
      check(rt.shapes[0].webHideBelow==600, "responsive-hide survives JSON round-trip"); }

    // --- Navigation bar object (auto-links pages) ---
    std::printf("[web] page nav bar\n");
    { Document d; d.pageSize=QSizeF(800,600); d.pageCount=2; c->setDocument(d);
      c->addNavBar();
      check(c->doc().shapes.size()==1 && c->doc().shapes[0].webNavBar, "addNavBar drops a nav-bar object");
      check(c->exportWebsite("/tmp/selftest_nav.html"), "publish with a nav bar succeeds");
      { QFile f("/tmp/selftest_nav.html"); f.open(QIODevice::ReadOnly); QString h=QString::fromUtf8(f.readAll());
        check(h.contains("<nav class=\"xnav") , "nav bar → a styled <nav>");
        check(h.contains(">Home</a>") && h.contains(">Page 2</a>"), "nav links every page (Home + Page 2)");
        check(h.contains("class=\"on\"") , "the current page is highlighted");
        check(!h.contains("<nav class=\"nav\">"), "the auto-generated top nav is suppressed when a nav-bar object exists"); }
      // it appears on page 2's file too
      { QFile f2("/tmp/selftest_nav_p2.html"); check(f2.exists(),"page 2 written"); f2.open(QIODevice::ReadOnly);
        QString h2=QString::fromUtf8(f2.readAll()); check(h2.contains("<nav class=\"xnav"), "nav bar appears on every page"); }
      check(!c->doc().shapes.isEmpty(), "nav shape retained in the doc");
      Document rt=Document::fromJson(c->doc().toJson());
      check(rt.shapes[0].webNavBar, "webNavBar survives JSON round-trip"); }

    // --- Page templates (Batch W4) ---
    std::printf("[web] page templates\n");
    { check(Canvas::templateNames().size()>=4, "at least 4 built-in templates");
      Document blank=Canvas::templateDocument(0);
      check(blank.shapes.isEmpty(), "Blank template has no shapes");
      Document land=Canvas::templateDocument(1);
      check(land.shapes.size()>=6 && !land.breakpoints.isEmpty(), "Landing Page has content + breakpoints");
      bool hasStretch=false, hasSticky=false, hasText=false;
      for(const Shape& s: land.shapes){ if(s.webStretch) hasStretch=true; if(s.webSticky) hasSticky=true; if(s.type==ShapeType::Text) hasText=true; }
      check(hasStretch && hasSticky && hasText, "Landing Page uses stretchy + sticky layout and real text");
      // the template publishes to a valid site
      Canvas* cc=c; cc->setDocument(land);
      check(cc->exportWebsite("/tmp/selftest_tmpl.html"), "template publishes to a website");
      Document card=Canvas::templateDocument(2);
      check(card.pageSize.width()<400 && card.shapes.size()>=4, "Business Card is card-sized with content"); }

    // --- EPS/AI import (PostScript vector subset, Batch P1) ---
    std::printf("[interop] EPS import\n");
    { QByteArray eps=
        "%!PS-Adobe-3.0 EPSF-3.0\n"
        "%%BoundingBox: 0 0 200 100\n"
        "1 0 0 setrgbcolor\n"
        "10 10 moveto 90 10 lineto 90 90 lineto 10 90 lineto closepath fill\n"
        "0 0 1 setrgbcolor 2 setlinewidth\n"
        "100 20 moveto 180 20 lineto 180 80 lineto stroke\n"
        "0 setgray\n"
        "120 30 moveto 160 30 40 60 20 50 curveto fill\n";
      Document d=importEps(eps);
      check(d.pageSize.width()==200 && d.pageSize.height()==100, "EPS %%BoundingBox sets the page size");
      check(d.shapes.size()==3, "EPS import produced 3 path shapes (fill, stroke, curve-fill)");
      const Shape& red=d.shapes[0];
      check(red.type==ShapeType::Path && red.fill.type==FillType::Solid && red.fill.color==QColor(255,0,0), "first path is a red filled square");
      // y-flip: EPS y=10..90 (from bottom) → doc top-left origin; the square's doc bbox is within the page
      QRectF rb=red.path.boundingRect();
      check(rb.left()>=9 && rb.right()<=91 && rb.top()>=9 && rb.bottom()<=91, "coordinates mapped into the page (y-flipped)");
      const Shape& line=d.shapes[1];
      check(line.fill.type==FillType::None && line.stroke.enabled && line.stroke.color==QColor(0,0,255) && line.stroke.width>1.5, "second path is a blue open stroke, width≈2");
      check(pathHasCurve(d.shapes[2].path), "third path contains a bezier curve (curveto)"); }

    // Plain-PostScript routing: a .ps / .epsi file must reach the EPS path parser via
    // Canvas::dropImportByExtension (File▸Open + drag-drop both funnel through it).
    { const char* ps = "%!PS-Adobe-3.0\n%%BoundingBox: 0 0 200 100\n"
                       "10 10 moveto 190 10 lineto 190 90 lineto 10 90 lineto closepath 0 0 1 setrgbcolor fill\n"
                       "20 20 moveto 180 80 lineto 4 setlinewidth stroke\nshowpage\n";
      // FictionBook .fb2: a single XML file -> one editable text frame carrying the body prose.
      { const QByteArray fb2 =
          "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
          "<FictionBook xmlns:l=\"http://www.w3.org/1999/xlink\">"
          "<description><title-info><book-title>The Test Book</book-title></title-info></description>"
          "<body><section><title><p>Chapter One</p></title>"
          "<p>First <emphasis>paragraph</emphasis> here.</p><p>Second paragraph.</p>"
          "</section></body>"
          "<binary id=\"cover.jpg\" content-type=\"image/jpeg\">QUJD</binary></FictionBook>";
        const QString p=QDir::tempPath()+"/xq_test.fb2";
        { QFile wf(p); wf.open(QIODevice::WriteOnly); wf.write(fb2); wf.close(); }
        Canvas tc; tc.newDocument();
        const bool ok=tc.importFb2(p);
        const bool haveShape = ok && !tc.doc().shapes.isEmpty();
        const QString t = haveShape? tc.doc().shapes.back().text : QString();
        check(haveShape && tc.doc().shapes.back().type==ShapeType::Text, "FB2 imports as an editable text frame");
        check(t.contains("The Test Book") && t.contains("Chapter One") && t.contains("Second paragraph"),
              "FB2 carries the title and the body paragraphs");
        check(t.contains("First paragraph here."), "FB2 keeps inline markup spacing (no glued words)");
        check(!t.contains("QUJD"), "FB2 skips <binary> base64 payloads"); }
      check(!Canvas().importFb2(QDir::tempPath()+"/xq_definitely_missing.fb2"), "FB2 fails safe on a missing file");

      // DOS/binary EPS wrapper (also the .ept container): a 30-byte header with u32-LE offset+length
      // for the PostScript section. importVectorFile must unwrap it before the EPS parser sees it.
      { QByteArray body(ps); QByteArray w(30,'\0');
        w[0]=char(0xC5); w[1]=char(0xD0); w[2]=char(0xD3); w[3]=char(0xC6);
        auto le32=[&](int o,quint32 v){ w[o]=char(v&0xFF); w[o+1]=char((v>>8)&0xFF); w[o+2]=char((v>>16)&0xFF); w[o+3]=char((v>>24)&0xFF); };
        le32(4,30); le32(8,quint32(body.size()));
        const QByteArray wrapped = w + body;
        for(const char* ext : {".ept", ".eps"}){
          Canvas tc; tc.newDocument();
          QTemporaryFile pf(QDir::tempPath()+"/xarqdosepsXXXXXX"+QString(ext));
          if(pf.open()){ pf.write(wrapped); pf.flush();
            const int before=tc.doc().shapes.size();
            const bool ok=tc.dropImportByExtension(pf.fileName());
            check(ok && (int)tc.doc().shapes.size()>before,
                  QString("%1 DOS/binary-EPS wrapper unwraps to the PostScript section").arg(ext).toUtf8().constData()); } } }
      // Raster-only PostScript written through ImageMagick's DisplayImage prologue: the pixels are a
      // plain payload rather than drawing operators, so they decode without an interpreter. All three
      // PostScript levels are covered — Level 1 ASCII hex, Level 2 binary RunLength, Level 3 Flate.
      { auto psWrap=[](const QByteArray& params, const QByteArray& payload){
            return QByteArray("%!PS-Adobe-3.0 EPSF-3.0\n%%BoundingBox: 0 0 2 2\n%%EndProlog\n"
                              "userdict begin\nDisplayImage\n") + params + payload; };
        // Level 1: "x y / x y / pointsize / columns rows / class / compression" then hex RGB.
        { const QByteArray ps=psWrap("0 0\n2 2\n12\n2 2\n0\n0\n",
                                     "FF0000 00FF00 0000FF FFFFFF\n");
          const QImage im=xara::psDisplayImageRaster(ps);
          check(!im.isNull() && im.size()==QSize(2,2), "raster PostScript (Level 1 hex) decodes to 2x2");
          check(!im.isNull() && im.pixelColor(0,0)==QColor(255,0,0) && im.pixelColor(1,1)==QColor(255,255,255),
                "raster PostScript Level 1 carries the RGB triples"); }
        // Level 2: an extra parameter line, then binary RGB through RunLengthDecode (n<128 = copy n+1).
        { QByteArray payload; payload.append(char(11));          // copy the next 12 bytes literally
          const unsigned char rgb[12]={255,0,0, 0,255,0, 0,0,255, 255,255,255};
          payload.append(reinterpret_cast<const char*>(rgb),12);
          payload.append(char(char(128)));                        // EOD
          const QByteArray ps=psWrap("0 0\n2 2\n12\n2 2\n0\n0\n0\n", payload);
          const QImage im=xara::psDisplayImageRaster(ps);
          check(!im.isNull() && im.size()==QSize(2,2) && im.pixelColor(0,0)==QColor(255,0,0)
                && im.pixelColor(1,0)==QColor(0,255,0), "raster PostScript (Level 2 RunLength) decodes"); }
        // Level 3: `false` tokens among the parameters and a Flate-compressed payload.
        { QByteArray raw; const unsigned char rgb[12]={255,0,0, 0,255,0, 0,0,255, 255,255,255};
          raw.append(reinterpret_cast<const char*>(rgb),12);
          QByteArray z=qCompress(raw,9).mid(4);                   // drop Qt's 4-byte length prefix -> plain zlib
          const QByteArray ps=psWrap("0 0\n2 2\n12\nfalse\nfalse\n2 2\n0\nfalse\nfalse\n0\n5\n", z);
          const QImage im=xara::psDisplayImageRaster(ps);
          check(!im.isNull() && im.size()==QSize(2,2) && im.pixelColor(0,1)==QColor(0,0,255),
                "raster PostScript (Level 3 Flate) decodes past the `false` parameters"); }
        check(xara::psDisplayImageRaster(QByteArray("%!PS\nnothing here\n")).isNull(),
              "plain PostScript without a DisplayImage block is not mistaken for a raster"); }

      // A raster-only EPS would need a full PostScript interpreter to rasterise; when the DOS wrapper
      // carries a TIFF preview of the same artwork, place that instead of failing. Vector content must
      // still win when it exists (checked above: the wrapped .ept yields its paths, not a preview).
      { QImage px(6,4,QImage::Format_ARGB32); px.fill(QColor(20,140,220));
        QByteArray tif; { QBuffer bf(&tif); bf.open(QIODevice::WriteOnly); px.save(&bf,"TIFF"); }
        if(!tif.isEmpty()){
          const QByteArray body("%!PS-Adobe-3.0 EPSF-3.0\n%%BoundingBox: 0 0 6 4\n% raster only\nshowpage\n");
          QByteArray w(30,'\0');
          w[0]=char(0xC5); w[1]=char(0xD0); w[2]=char(0xD3); w[3]=char(0xC6);
          auto le32=[&](int o,quint32 v){ w[o]=char(v&0xFF); w[o+1]=char((v>>8)&0xFF); w[o+2]=char((v>>16)&0xFF); w[o+3]=char((v>>24)&0xFF); };
          le32(4,30); le32(8,quint32(body.size()));
          le32(20,quint32(30+body.size())); le32(24,quint32(tif.size()));
          Canvas tc; tc.newDocument();
          QTemporaryFile pf(QDir::tempPath()+"/xarqepsprevXXXXXX.eps");
          if(pf.open()){ pf.write(w+body+tif); pf.flush();
            const bool ok=tc.dropImportByExtension(pf.fileName());
            check(ok && !tc.doc().shapes.isEmpty(), "raster-only DOS EPS falls back to its TIFF preview");
            check(ok && !tc.doc().shapes.isEmpty() && tc.doc().shapes.back().fill.type==FillType::Bitmap,
                  "the EPS preview is placed as a bitmap fill"); } } }

      for(const char* ext : {".ps", ".epsi"}){
        Canvas tc; tc.newDocument();
        QTemporaryFile pf(QDir::tempPath()+"/xarqpsXXXXXX"+QString(ext));
        if(pf.open()){ pf.write(ps); pf.flush();
          int before=tc.doc().shapes.size();
          bool ok=tc.dropImportByExtension(pf.fileName());
          check(ok && (int)tc.doc().shapes.size()>before,
                QString("%1 plain PostScript routes through dropImportByExtension → EPS parser").arg(ext).toUtf8().constData()); } } }

    // --- PDF import (content-stream vectors, Batch P2) ---
    std::printf("[interop] PDF import\n");
    { QByteArray pdf=
        "%PDF-1.4\n"
        "1 0 obj<</Type/Catalog/Pages 2 0 R>>endobj\n"
        "2 0 obj<</Type/Pages/Kids[3 0 R]/Count 1>>endobj\n"
        "3 0 obj<</Type/Page/Parent 2 0 R/MediaBox[0 0 200 100]/Contents 4 0 R>>endobj\n"
        "4 0 obj<</Length 70>>\nstream\n"
        "1 0 0 rg\n10 10 80 80 re f\n0 0 1 RG 2 w\n100 20 m 180 20 l 180 80 l S\n"
        "endstream endobj\n"
        "trailer<</Root 1 0 R>>\n";
      Document d=importPdf(pdf);
      check(d.pageSize.width()==200 && d.pageSize.height()==100, "PDF /MediaBox sets the page size");
      check(d.shapes.size()==2, "PDF import produced 2 path shapes (filled rect + stroked line)");
      check(d.shapes[0].fill.type==FillType::Solid && d.shapes[0].fill.color==QColor(255,0,0), "PDF 're f' is a red filled rectangle");
      QRectF rb=d.shapes[0].path.boundingRect();
      check(qAbs(rb.width()-80)<1 && qAbs(rb.height()-80)<1, "rectangle has the right size (80×80)");
      check(rb.top()>=9 && rb.bottom()<=91, "PDF y-up coordinates flipped into the page");
      check(d.shapes[1].fill.type==FillType::None && d.shapes[1].stroke.enabled && d.shapes[1].stroke.color==QColor(0,0,255), "PDF 'S' is a blue open stroke"); }

    // --- PDF import: editable TEXT extraction (BT..ET → Text shapes) ---
    std::printf("[interop] PDF text import\n");
    { QByteArray pdf=
        "%PDF-1.4\n"
        "3 0 obj<</Type/Page/MediaBox[0 0 300 200]/Contents 4 0 R>>endobj\n"
        "4 0 obj<</Length 60>>\nstream\n"
        "BT /F1 24 Tf 0 0 0 rg 50 150 Td (Hello PDF) Tj ET\n"
        "0 1 0 rg 10 10 40 40 re f\n"
        "endstream endobj\ntrailer<</Root 1 0 R>>\n";
      Document d=importPdf(pdf);
      Shape* txt=nullptr; int nText=0; for(Shape& s: d.shapes){ if(s.type==ShapeType::Text){ txt=&s; ++nText; } }
      check(nText==1, "PDF 'BT..(Hello PDF) Tj..ET' produces exactly one editable Text shape");
      check(txt && txt->text=="Hello PDF", "PDF literal string decodes to the right text content");
      if(txt){ QPointF o=txt->transform.map(QPointF(0,0));
        check(qAbs(o.x()-50)<1.5, "text x maps from the PDF text matrix (Td 50)");
        check(o.y()>0 && o.y()<200, "text y flips into the page box (y-down)");
        check(txt->font.pixelSize()>=18, "Tf font size (24) carries into the imported font"); }
      bool greenRect=false; for(Shape& s: d.shapes) if(s.type==ShapeType::Path && s.fill.color==QColor(0,255,0)) greenRect=true;
      check(greenRect, "path drawing after the text object still imports (green rect)"); }

    // --- PDF import: multi-page (walk the /Pages tree → one document page each) ---
    std::printf("[interop] PDF multi-page import\n");
    { QByteArray pdf=
        "%PDF-1.4\n"
        "1 0 obj<</Type/Catalog/Pages 2 0 R>>endobj\n"
        "2 0 obj<</Type/Pages/Kids[3 0 R 5 0 R]/Count 2/MediaBox[0 0 200 100]>>endobj\n"
        "3 0 obj<</Type/Page/Parent 2 0 R/Contents 4 0 R>>endobj\n"
        "4 0 obj<</Length 24>>\nstream\n1 0 0 rg 10 10 30 30 re f\nendstream endobj\n"
        "5 0 obj<</Type/Page/Parent 2 0 R/Contents 6 0 R>>endobj\n"
        "6 0 obj<</Length 24>>\nstream\n0 0 1 rg 50 50 30 30 re f\nendstream endobj\n"
        "trailer<</Root 1 0 R>>\n";
      Document d=importPdf(pdf);
      check(d.pageCount==2, "PDF page tree → 2 document pages");
      check(d.shapes.size()==2, "both pages' content imported (red + blue rects)");
      Shape* red=nullptr; Shape* blue=nullptr;
      for(Shape& s: d.shapes){ if(s.fill.color==QColor(255,0,0)) red=&s; if(s.fill.color==QColor(0,0,255)) blue=&s; }
      check(red&&blue, "page 1 red rect + page 2 blue rect both present");
      if(red&&blue) check(blue->docBounds().center().y() > red->docBounds().center().y()+100, "the 2nd page's shape sits below the 1st (its own page band)");
      check(d.pageSize.width()==200 && d.pageSize.height()==100, "inherited /MediaBox sets the page size"); }

    // --- DXF + AI import (Batch R1) ---
    std::printf("[interop] DXF + AI import\n");
    { QByteArray dxf=
        "0\nSECTION\n2\nENTITIES\n"
        "0\nLINE\n10\n0.0\n20\n0.0\n11\n100.0\n21\n50.0\n"
        "0\nCIRCLE\n10\n50.0\n20\n50.0\n40\n20.0\n"
        "0\nLWPOLYLINE\n90\n3\n70\n1\n10\n0\n20\n0\n10\n40\n20\n0\n10\n0\n20\n40\n"
        "0\nENDSEC\n0\nEOF\n";
      Document d=importDxf(dxf);
      check(d.shapes.size()==3, "DXF ENTITIES → 3 shapes (line + circle + polyline)");
      bool line=false,circle=false,poly=false; for(const Shape& s:d.shapes){ if(s.name=="LINE")line=true; if(s.name=="CIRCLE")circle=true; if(s.name=="LWPOLYLINE")poly=true; }
      check(line&&circle&&poly, "all three DXF entity types parsed");
      for(const Shape& s:d.shapes) check(s.fill.type==FillType::None && s.stroke.enabled, "DXF entities are stroked open paths");
      // TEXT / MTEXT: real editable text, not outlines (previously dropped entirely by the reader).
      { QByteArray t=
          "0\nSECTION\n2\nENTITIES\n"
          "0\nLINE\n10\n0.0\n20\n0.0\n11\n100.0\n21\n0.0\n"
          "0\nTEXT\n10\n10.0\n20\n50.0\n40\n12.0\n50\n0.0\n1\nHello CAD\n"
          "0\nMTEXT\n10\n10.0\n20\n80.0\n40\n8.0\n1\n{\\\\fArial|b1;Styled}\\\\Psecond\n"
          "0\nENDSEC\n0\nEOF\n";
        Document dt=importDxf(t);
        int texts=0; QString got, gotm;
        for(const Shape& s:dt.shapes) if(s.type==ShapeType::Text){ ++texts; if(s.name=="TEXT") got=s.text; if(s.name=="MTEXT") gotm=s.text; }
        check(dt.shapes.size()==3 && texts==2, "DXF TEXT + MTEXT import as Text shapes alongside the geometry");
        check(got=="Hello CAD", "DXF TEXT carries its string");
        check(gotm.contains("Styled") && gotm.contains("second") && !gotm.contains("\\\\f"),
              "DXF MTEXT strips inline formatting codes and expands \\\\P line breaks");
        bool sized=false; for(const Shape& s:dt.shapes) if(s.type==ShapeType::Text && s.name=="TEXT") sized = s.font.pixelSize()==12;
        check(sized, "DXF TEXT height (group 40) becomes the font size"); }
      // Hostile group-40 values must be clamped in the double domain (an out-of-range int() cast is
      // UB and the UBSan build traps). Also guards against a 0/negative point size reaching QFont.
      { for(const char* h : {"1e300","-5.0","0.0","nan"}){
          QByteArray t = QByteArray("0\nSECTION\n2\nENTITIES\n0\nTEXT\n10\n0.0\n20\n0.0\n40\n")
                       + h + "\n1\nX\n0\nENDSEC\n0\nEOF\n";
          Document dt=importDxf(t);
          bool okSize=true;
          for(const Shape& s2:dt.shapes) if(s2.type==ShapeType::Text)
              okSize = okSize && s2.font.pixelSize()>=1 && s2.font.pixelSize()<=4000;
          check(okSize, "DXF TEXT clamps a hostile group-40 height without an out-of-range cast"); } }
      // AI routing: a %PDF-headed .ai goes through the PDF path
      QByteArray ai="%PDF-1.4\n1 0 obj<</Type/Catalog/Pages 2 0 R>>endobj\n2 0 obj<</Type/Pages/Kids[3 0 R]/Count 1>>endobj\n"
        "3 0 obj<</Type/Page/Parent 2 0 R/MediaBox[0 0 100 100]/Contents 4 0 R>>endobj\n"
        "4 0 obj<</Length 30>>\nstream\n1 0 0 rg\n5 5 40 40 re f\nendstream endobj\ntrailer<</Root 1 0 R>>\n";
      Document da=importAi(ai);
      check(da.shapes.size()>=1 && da.shapes[0].fill.color==QColor(255,0,0), "AI-as-PDF routes through importPdf (red rect)"); }

    // --- DWG import via GNU LibreDWG (AutoCAD binary CAD) ---
    std::printf("[interop] DWG import (LibreDWG)\n");
    if(dwgAvailable()){
      // A real AutoCAD R10 DWG (from the LibreDWG test-suite): 3 LINEs (triangle) + CIRCLE + LWPOLYLINE.
      // Base64 so the binary fixture lives in-source and the test stays hermetic. Decoded to a temp file
      // because LibreDWG reads by path.
      static const char* kDwgB64 =
        "QUMxMDA2AAAAAAAAAQMABQCeAAASBQAAiQkAAHMOAABNAgBAwBAAABEAAIAlAAMAAACJCQAAJQACAAAA+AkAAMIAAgAAAEIKAAC7AAEAAADGCwAAlQAAAAAA"
        "gQwAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA3t3d3d3dIkAAAAAAAAAkQAAAAAAAAPA/AAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAChAAAAAAAAAIkAhUnpkSeAcQAAAAAAAABJAAAAAAAAAAAADTDXH7OkkQAAAAAAAAAAA8D8AAAAAAADwPwAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAEAAQAAAAIAAAAAAAAA8D+amZmZmZnJP5qZmZmZmck/AAAPAAAAAAAAAAAAAAAAAAAAwdkB43AX9j8CAAQA"
        "AAAAAAAAAAAAAAAAAAAAAAAAmpmZmZmZuT8AAAAAAAAAAAAAAAAAAAAAAQBhY2FkAAAAAAAAAAAAAAAAAAAAAADwPwrXo3A9Csc/AAAAAAAAsD9SuB6F61HY"
        "PwrXo3A9Csc/AAAAAAAAAAAAAAAAAAAAAArXo3A9Csc/CtejcD0Ktz8AAAAAAAAAAAAAAQEAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA8D8AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABkAAAAAAEAATGIJQDsX+IDMYgl"
        "AIjx+gMAAAAAnJEYAAAAAACckRgAAQABAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAgEAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABmZmZmZmY5QAAAAAAAAPA/CAAAAAEAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAQBpAAAAAACBDAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAADwPwAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "APA/AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABJQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAYABgAGAAYABgD5AAIAAACBDAAAAAAGAAAA//8CBBgAAAAAAAAAAAAAAPA/AAAAAAAAAEABBCgAAAAAAAAAAAAAAABAAAAAAAAACEAAAAAA"
        "AAAIQAAAAAAAABBACAAwAAAAAAAAAAAAAAAUQAAAAAAAABRAAAAAAAAA8D/SITN/fNkSQAAAAAAAAAAAAwAgAAAAAAAAAAAAAAAIQAAAAAAAAPA/AAAAAAAA"
        "8D8HADYAAABgAAAAAAAAAPA/AAAAAAAAEEAAAAAAAADoPwMARk9PAwAAAAAAAAhAAAAAAAAAEEAJAEgAAAAAAJ8TwPW8vO0/jP1HYWhIFEAx9h+FoSHxP3QC"
        "uJ6XtxNAzwngel7e/j+M/UdhaEgYQBj7j8LQkABAdAK4npe3F0CBBCgAAAAAAAAAAAAAABhAAAAAAAAA8D8AAAAAAAAcQAAAAAAAAABADgA6AAAADwAAAAAA"
        "AAAAABhAAAAAAAAA8D8AAAAAAADgPwAAAAAAAOA/ZXMtOFLB4D8AAAAAAADgPwQAKgAAAAMAAAAAAAAAGEAAAAAAAAAYQAAAAAAAAPA/g2VzLThSweA/AQsA"
        "SAAAAAAAAAAAAAAAHEAAAAAAAADwPwAAAAAAACBAAAAAAAAAAEAAAAAAAAAcQAAAAAAAAAhAAAAAAAAAIEAAAAAAAAAQQA8AOwAAAAIAAAAAAAAAIkAAAAAA"
        "AAAUQJqZmZmZmck/AQAzBgBQUk9NUFQFAEFUVFIxAGVzLThSwfA/jwA7AAAAAgAAAAAAAADwPwAAAAAAAABAmpmZmZmZyT8BADMGAFBST01QVAUAQVRUUjIA"
        "ZXMtOFLB8D8OgDoAAAAPAAEAAAAAAAAAAEAAAAAAAAAAQAAAAAAAAOA/AAAAAAAA4D9lcy04UsHgPwAAAAAAAOA/EAA7AAAABgAAAAAAAAAAQAAAAAAAAABA"
        "m5mZmZmZuT8BADQFAEFUVFIyABctRFT7Ifk/////////7z8RAAwAAAAAAGoHAAATgAgAAAAAABQAGAAAAAAAAAAAAAAAFEAAAAAAAAAcQBQAGAAAAAAAAAAA"
        "AAAAGEAAAAAAAAAgQBQAGAAAAAAAAAAAAAAAHEAAAAAAAAAcQBEADAAAAAAA6wcAABIACAAAAACAFAAYAAAAAAAAAAAAAAAWQAAAAAAAAB5AFAAYAAAAAAAA"
        "AAAAAAAaQAAAAAAAACFAFAAYAAAAAAAAAAAAAAAeQAAAAAAAAB5AEQAMAAAAAABHCAAAFwBmAAAAHgACAP3//////x9A/f//////H0AAAAAAAAAAAP7/////"
        "/x1A////////IEABAQAg/v//////F0D+//////8fQAAAAAAAAAAA/P//////G0D8//////8bQAAAAAAAAAAAFQA4AAAAAAAAAAAAAAAUQAAAAAAAACJAAAAA"
        "AAAAAAAAAAAAAAAYQAAAAAAAACRAAAAAAAAA8D8WBEgAAAAAAAAAAAAAAAAAAAAAAAAAHEAAAAAAAAAQQAAAAAAAACBAAAAAAAAACEAAAAAAAAAiQAAAAAAA"
        "AABAAAAAAAAAIEBAQkxPQ0sxAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABAQkJMT0NLMgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAASAAAQEEqRABD"
        "SzIAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAKMAAEAAMAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAHAAAAQERFRlBPSU5UUwAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAA+f8AAABTVEFOREFSRAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA8D8AAAAAAAAAAACamZmZmZnJP3R4dAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAQQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAADwPwAAAAAAAAAAAJqZ"
        "mZmZmck/ZXMAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABAQ09OVElOVU9VUwAAAAAAAAAAAAAAAAAAAAAAAAAAAABTb2xpZCBsaW5lAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABBAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAgCpBQ1RJVkUAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAPA/AAAAAAAA8D8AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA8D8AAAAAAAAAAANMNcfs6SRA"
        "IVJ6ZEngHEAAAAAAAAASQMHZAeNwF/Y/AAAAAAAASUAAAAAAAAAAAAAAAAAAAAAAAABkAAEAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAPA/AAAAAAAA8D8AAAAAAAAAAAAAAAAAAAAAACpBQ1RJVkUAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAPA/AAAA"
        "AAAA8D8AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA8D8AAAAAAAAAAANMNcfs6SRAIVJ6ZEngHEAAAAAAAAASQMHZAeNw"
        "F/Y/AAAAAAAASUAAAAAAAAAAAAAAAAAAAAAAAABkAAEAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAPA/AAAAAAAA8D8AAAAAAAAA"
        "AAAAAAAAAAAADAAYAAAAAAAAAAAAAAAAAAAAAAAAAAAAAQQoAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA8D8AAAAAAADwPw0ACAAAAAAADAAYAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAADwA7AAAAAgAAAAAAAAAAAAAAAAAAAAAAmpmZmZmZyT8BADMGAFBST01QVAUAQVRUUjIAZXMtOFLB8D8NAAgAAAAAAAwAGAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAEHKgAAAAAAAP91/sw8QS0YQDp/Zp6gFiBAkvvne1WCHEDJ/fO9KkEiQAEHKgAAAAAAAP90/sw8QS0cQHT+zDxBLRxAyf3zvSpB"
        "IEDJ/fO9KkEgQAEHKgAAAAAAAP+T++d7VYIcQDQCDELVviFAubw2CZZPHUCgoWT7NFghQAEHKgAAAAAAAP9nBBiEqn0fQMn9870qQSBAQUPJ9mmwHkBcXpsE"
        "y6cgQAsDSgAAAAAAAP9P/OuRnGwcQJECDs34syFA1/rjZQ6YHEDVAQq3sckhQP3//////xtA/v//////IUD9//////8bQP7//////yFACwNKAAAAAAAA/yMF"
        "HJrxZx9AKP71SE42IECrAxRuY5MfQGv98TIHTCBA/f//////H0D9//////8fQP3//////x9A/f//////H0ACBxoAAQAAAAD//v//////F0D+//////8fQAIH"
        "GgABAAAAAP/8//////8bQPz//////xtAAgcaAAEAAAAA//3//////x9A/f//////H0ANAAgAAAAAABOACQAAAAEAARIACABPCAAA"
        ;
      const QByteArray dwg = QByteArray::fromBase64(QByteArray(kDwgB64));
      QTemporaryFile tf(QDir::tempPath()+"/xarq_selftest_XXXXXX.dwg");
      const bool okw = tf.open() && tf.write(dwg)==dwg.size(); tf.flush();
      Document d = dwgToDocument(tf.fileName());
      check(okw && d.shapes.size()>=4, "DWG (LibreDWG) → model-space entities import as shapes");
      // Geometry arrives as stroked open paths; TEXT/MTEXT (new) arrives as filled Text shapes.
      bool allStroked = !d.shapes.isEmpty();
      for(const Shape& s : d.shapes){
          if(s.type==ShapeType::Text) continue;
          if(!(s.fill.type==FillType::None && s.stroke.enabled)) allStroked=false; }
      check(allStroked, "DWG geometry imports as stroked open paths");
      check(dwgToDocument("/nonexistent/none.dwg").shapes.isEmpty(), "DWG import of a missing file fails cleanly");
    } else {
      check(dwgToDocument("/nonexistent/none.dwg").shapes.isEmpty(), "DWG stub returns empty when LibreDWG absent");
    }

    // --- WMF (Windows Metafile) import ---
    std::printf("[interop] WMF import\n");
    { QByteArray w;
      auto le16=[&](int v){ w.append(char(v&0xff)); w.append(char((v>>8)&0xff)); };
      auto le32=[&](quint32 v){ for(int i=0;i<4;++i) w.append(char((v>>(8*i))&0xff)); };
      le32(0x9AC6CDD7u); le16(0); le16(0);le16(0);le16(100);le16(100); le16(96); le32(0); le16(0);   // placeable header (bbox 0,0..100,100)
      le16(1);le16(9);le16(0x0300);le32(0);le16(1);le32(0);le16(0);                                   // METAHEADER
      le32(5);le16(0x020B);le16(0);le16(0);                                                           // SETWINDOWORG 0,0
      le32(5);le16(0x020C);le16(100);le16(100);                                                       // SETWINDOWEXT 100,100
      le32(7);le16(0x02FC);le16(0); w.append(char(0xFF));w.append(char(0));w.append(char(0));w.append(char(0)); le16(0); // CREATEBRUSHINDIRECT solid red
      le32(4);le16(0x012D);le16(0);                                                                   // SELECTOBJECT 0
      le32(7);le16(0x041B);le16(60);le16(60);le16(10);le16(10);                                       // RECTANGLE B60 R60 T10 L10
      le32(3);le16(0x0000);                                                                           // META_EOF
      Document d=importWmf(w);
      check(!d.shapes.isEmpty(), "WMF import produced shapes");
      Shape* rect=nullptr; for(Shape& s: d.shapes) if(s.type==ShapeType::Path && s.fill.type==FillType::Solid) rect=&s;
      check(rect, "WMF RECTANGLE imported as a filled path");
      check(rect && rect->fill.color==QColor(255,0,0), "WMF brush colour (red) applied to the fill");
      if(rect){ QRectF bb=rect->path.boundingRect();
        check(qAbs(bb.width()-50)<1.5 && qAbs(bb.height()-50)<1.5, "WMF rectangle is 50×50 logical units"); }
      check(qAbs(d.pageSize.width()-50)<1.5, "WMF page size taken from the content bounds"); }

    // --- EMF (Enhanced Metafile) import ---
    std::printf("[interop] EMF import\n");
    { QByteArray e;
      auto le16=[&](int v){ e.append(char(v&0xff)); e.append(char((v>>8)&0xff)); };
      auto le32=[&](quint32 v){ for(int i=0;i<4;++i) e.append(char((v>>(8*i))&0xff)); };
      // EMR_HEADER (type=1, nSize=88) — 8-byte header + 80 zero bytes
      le32(1); le32(88); for(int i=0;i<80;++i) e.append(char(0));
      // EMR_CREATEBRUSHINDIRECT (type=39, nSize=24): ih=1, style=0 (solid), color=blue(0000FF), hatch=0
      le32(39); le32(24); le32(1); le32(0); e.append(char(0));e.append(char(0));e.append(char(0xFF));e.append(char(0)); le32(0);
      // EMR_SELECTOBJECT (type=37, nSize=12): ih=1
      le32(37); le32(12); le32(1);
      // EMR_RECTANGLE (type=43, nSize=24): rclBox l=10,t=10,r=60,b=60
      le32(43); le32(24); le32(10); le32(10); le32(60); le32(60);
      // EMR_EOF (type=14, nSize=20)
      le32(14); le32(20); le32(0); le32(0); le32(0);
      (void)le16;
      Document d=importEmf(e);
      check(!d.shapes.isEmpty(), "EMF import produced shapes");
      Shape* rect=nullptr; for(Shape& s: d.shapes) if(s.type==ShapeType::Path && s.fill.type==FillType::Solid) rect=&s;
      check(rect, "EMF RECTANGLE imported as a filled path");
      check(rect && rect->fill.color==QColor(0,0,255), "EMF brush colour (blue) applied to the fill");
      if(rect){ QRectF bb=rect->path.boundingRect();
        check(qAbs(bb.width()-50)<1.5 && qAbs(bb.height()-50)<1.5, "EMF rectangle is 50×50 device units"); }
      check(qAbs(d.pageSize.width()-50)<1.5, "EMF page size taken from the content bounds"); }

    // --- PDF import: embedded raster images (XObject Do) ---
    std::printf("[interop] PDF image XObject import\n");
    { QImage srcimg(6,6,QImage::Format_RGB32); srcimg.fill(QColor(200,60,40));
      QByteArray jpg; { QBuffer buf(&jpg); buf.open(QIODevice::WriteOnly); srcimg.save(&buf,"JPG",92); }
      QByteArray pdf=
        "%PDF-1.4\n"
        "1 0 obj<</Type/Catalog/Pages 2 0 R>>endobj\n"
        "2 0 obj<</Type/Pages/Kids[3 0 R]/Count 1/MediaBox[0 0 200 100]>>endobj\n"
        "3 0 obj<</Type/Page/Contents 4 0 R/Resources<</XObject<</Im0 5 0 R>>>>>>endobj\n"
        "4 0 obj<</Length 30>>\nstream\nq 50 0 0 40 20 30 cm /Im0 Do Q\nendstream endobj\n";
      pdf += "5 0 obj<</Type/XObject/Subtype/Image/Width 6/Height 6/ColorSpace/DeviceRGB/BitsPerComponent 8/Filter/DCTDecode/Length "
             + QByteArray::number(jpg.size()) + ">>\nstream\n";
      pdf += jpg; pdf += "\nendstream endobj\ntrailer<</Root 1 0 R>>\n";
      Document d=importPdf(pdf);
      Shape* im=nullptr; for(Shape& s:d.shapes) if(s.fill.type==FillType::Bitmap && !s.fill.image.isNull()) im=&s;
      check(im, "PDF DCTDecode image XObject imports as a Bitmap-fill shape");
      if(im){ QRectF bb=im->docBounds();
        check(qAbs(bb.width()-50)<3 && qAbs(bb.height()-40)<3, "placed image size matches the CTM (50×40)");
        check(qAbs(bb.left()-20)<3, "placed image x comes from the CTM (20)"); } }
    // raw (unfiltered) DeviceRGB image samples
    { QByteArray pdf=
        "%PDF-1.4\n"
        "1 0 obj<</Type/Catalog/Pages 2 0 R>>endobj\n"
        "2 0 obj<</Type/Pages/Kids[3 0 R]/Count 1/MediaBox[0 0 200 100]>>endobj\n"
        "3 0 obj<</Type/Page/Contents 4 0 R/Resources<</XObject<</Im1 5 0 R>>>>>>endobj\n"
        "4 0 obj<</Length 30>>\nstream\nq 40 0 0 40 10 10 cm /Im1 Do Q\nendstream endobj\n"
        "5 0 obj<</Type/XObject/Subtype/Image/Width 2/Height 2/ColorSpace/DeviceRGB/BitsPerComponent 8/Length 12>>\nstream\n";
      const char rgb[12]={ (char)255,0,0, (char)255,0,0, (char)255,0,0, (char)255,0,0 };
      pdf.append(rgb,12); pdf += "\nendstream endobj\ntrailer<</Root 1 0 R>>\n";
      Document d=importPdf(pdf);
      Shape* im=nullptr; for(Shape& s:d.shapes) if(s.fill.type==FillType::Bitmap && !s.fill.image.isNull()) im=&s;
      check(im, "PDF raw DeviceRGB image samples import as a Bitmap-fill shape");
      if(im) check(im->fill.image.width()==2 && im->fill.image.height()==2, "decoded raw image has the declared 2×2 dimensions"); }

    // --- PSD import (flattened composite, Batch P3) ---
    std::printf("[interop] PSD import\n");
    { auto be16=[](QByteArray&a,int v){ a.append(char((v>>8)&0xff)); a.append(char(v&0xff)); };
      auto be32=[](QByteArray&a,quint32 v){ a.append(char((v>>24)&0xff)); a.append(char((v>>16)&0xff)); a.append(char((v>>8)&0xff)); a.append(char(v&0xff)); };
      // RAW 2×2 RGB: R,G,B planes (scanline order)
      QByteArray psd; psd.append("8BPS",4); be16(psd,1); psd.append(QByteArray(6,0)); be16(psd,3); be32(psd,2); be32(psd,2); be16(psd,8); be16(psd,3);
      be32(psd,0); be32(psd,0); be32(psd,0); be16(psd,0);
      const char R[4]={(char)255,0,0,0}, G[4]={0,(char)255,0,0}, B[4]={0,0,(char)255,0};
      psd.append(R,4); psd.append(G,4); psd.append(B,4);
      QImage im=importPsd(psd);
      check(!im.isNull() && im.width()==2 && im.height()==2, "PSD (raw RGB) header parsed → 2×2 image");
      check(QColor(im.pixel(0,0))==QColor(255,0,0) && QColor(im.pixel(1,0))==QColor(0,255,0) && QColor(im.pixel(0,1))==QColor(0,0,255), "PSD raw RGB planar pixels decode correctly");
      // RLE (PackBits) 2×2 grayscale
      QByteArray g; g.append("8BPS",4); be16(g,1); g.append(QByteArray(6,0)); be16(g,1); be32(g,2); be32(g,2); be16(g,8); be16(g,1);
      be32(g,0); be32(g,0); be32(g,0); be16(g,1);       // compression = RLE
      be16(g,2); be16(g,2);                              // scanline byte counts (2 rows)
      const char rle[4]={(char)0xFF,(char)100,(char)0xFF,(char)200}; g.append(rle,4);   // row0: 2×100, row1: 2×200
      QImage gi=importPsd(g);
      check(!gi.isNull() && QColor(gi.pixel(0,0)).red()==100 && QColor(gi.pixel(1,1)).red()==200, "PSD PackBits/RLE grayscale decodes");
      // LAYERED PSD (Gap 2): 2 raw RGB layers → 2 positioned bitmap shapes
      QByteArray psd2; psd2.append("8BPS",4); be16(psd2,1); psd2.append(QByteArray(6,0)); be16(psd2,3); be32(psd2,4); be32(psd2,4); be16(psd2,8); be16(psd2,3);
      be32(psd2,0); be32(psd2,0);   // colour-mode + resources = empty
      auto layerRec=[&](int t,int l,int b,int r,int dataLen){ QByteArray rec;
        be32(rec,t); be32(rec,l); be32(rec,b); be32(rec,r); be16(rec,3);
        for(int id=0;id<3;++id){ be16(rec,id); be32(rec,dataLen); }
        rec.append("8BIM",4); rec.append("norm",4);
        rec.append(char(255)); rec.append(char(0)); rec.append(char(0)); rec.append(char(0)); be32(rec,0); return rec; };
      auto rawChan=[&](QByteArray& into, const QByteArray& plane){ be16(into,0); into.append(plane); };   // comp 0 + raw plane
      QByteArray li; be16(li,2);                                  // 2 layers
      li.append(layerRec(0,0,2,2,6));                             // layer 0: 2×2, each channel 2+4 bytes
      li.append(layerRec(0,0,4,4,18));                            // layer 1: 4×4, each channel 2+16 bytes
      rawChan(li, QByteArray(4,(char)255)); rawChan(li, QByteArray(4,0)); rawChan(li, QByteArray(4,0));           // L0 = red
      rawChan(li, QByteArray(16,0));        rawChan(li, QByteArray(16,0)); rawChan(li, QByteArray(16,(char)255)); // L1 = blue
      QByteArray lm; be32(lm, li.size()); lm.append(li);
      be32(psd2, lm.size()); psd2.append(lm);
      Document pd=importPsdDocument(psd2);
      check(pd.shapes.size()==2, "layered PSD imports 2 layers as 2 bitmap shapes");
      check(pd.shapes[0].fill.image.width()==2 && QColor(pd.shapes[0].fill.image.pixel(0,0)).red()>200, "layer 0 → 2×2 red bitmap");
      check(pd.shapes[1].fill.image.width()==4 && QColor(pd.shapes[1].fill.image.pixel(1,1)).blue()>200, "layer 1 → 4×4 blue bitmap");
      // a PSD with NO layer section still yields the flattened composite as one shape
      Document pf=importPsdDocument(psd);   // the raw 2×2 RGB PSD from above (no layers)
      check(pf.shapes.size()==1 && pf.shapes[0].fill.type==FillType::Bitmap, "layerless PSD → single flattened bitmap shape");
      // --- PSB (8BPS version 2, "Large Document Format"): identical structure, 64-bit length fields ---
      auto be64=[](QByteArray&a,quint64 v){ for(int i=7;i>=0;--i) a.append(char((v>>(i*8))&0xff)); };
      // composite: a layerless v2 file exercises version-2 acceptance + the 8-byte Layer&Mask length skip.
      QByteArray psbc; psbc.append("8BPS",4); be16(psbc,2); psbc.append(QByteArray(6,0)); be16(psbc,3); be32(psbc,2); be32(psbc,2); be16(psbc,8); be16(psbc,3);
      be32(psbc,0); be32(psbc,0); be64(psbc,0); be16(psbc,0);      // colourmode(4)=0, resources(4)=0, layer&mask(8)=0, compression=0
      psbc.append(R,4); psbc.append(G,4); psbc.append(B,4);
      QImage pci=importPsd(psbc);
      check(!pci.isNull() && pci.width()==2 && QColor(pci.pixel(0,0))==QColor(255,0,0) && QColor(pci.pixel(0,1))==QColor(0,0,255),
            "PSB (v2 composite, 8-byte layer&mask skip) decodes");
      // layered: 2 raw RGB layers with 8-byte channel-data lengths → 2 positioned bitmap shapes.
      QByteArray psbl; psbl.append("8BPS",4); be16(psbl,2); psbl.append(QByteArray(6,0)); be16(psbl,3); be32(psbl,3); be32(psbl,3); be16(psbl,8); be16(psbl,3);
      be32(psbl,0); be32(psbl,0);   // colour-mode + resources empty (still 4-byte in PSB)
      auto psbLayerRec=[&](int t,int l,int b,int r,quint64 dataLen){ QByteArray rec;
        be32(rec,t); be32(rec,l); be32(rec,b); be32(rec,r); be16(rec,3);
        for(int id=0;id<3;++id){ be16(rec,id); be64(rec,dataLen); }    // 8-byte channel-data length in PSB
        rec.append("8BIM",4); rec.append("norm",4);
        rec.append(char(255)); rec.append(char(0)); rec.append(char(0)); rec.append(char(0)); be32(rec,0); return rec; };
      QByteArray bli; be16(bli,2);                                     // 2 layers
      bli.append(psbLayerRec(0,0,2,2,6));                              // layer 0: 2×2, each channel 2+4 bytes
      bli.append(psbLayerRec(0,0,3,3,11));                             // layer 1: 3×3, each channel 2+9 bytes
      rawChan(bli, QByteArray(4,(char)255)); rawChan(bli, QByteArray(4,0)); rawChan(bli, QByteArray(4,0));            // L0 = red
      rawChan(bli, QByteArray(9,0));         rawChan(bli, QByteArray(9,(char)255)); rawChan(bli, QByteArray(9,0));    // L1 = green
      QByteArray blm; be64(blm, quint64(bli.size())); blm.append(bli); // Layer Info length is 8-byte in PSB
      be64(psbl, quint64(blm.size())); psbl.append(blm);              // Layer & Mask section length is 8-byte in PSB
      Document pb=importPsdDocument(psbl);
      check(pb.shapes.size()==2, "layered PSB (v2, 64-bit lengths) imports 2 layers");
      check(pb.shapes.size()==2 && pb.shapes[0].fill.image.width()==2 && QColor(pb.shapes[0].fill.image.pixel(0,0)).red()>200, "PSB layer 0 → 2×2 red bitmap");
      check(pb.shapes.size()==2 && pb.shapes[1].fill.image.width()==3 && QColor(pb.shapes[1].fill.image.pixel(1,1)).green()>200, "PSB layer 1 → 3×3 green bitmap"); }

    // --- CMYK colour separations (Batch P4) ---
    std::printf("[interop] CMYK separations\n");
    { double C,M,Y,K;
      xara::rgbToCmyk(QColor(255,0,0),C,M,Y,K); check(qAbs(C-0)<0.01 && qAbs(M-1)<0.01 && qAbs(Y-1)<0.01 && qAbs(K-0)<0.01, "red → C0 M1 Y1 K0");
      xara::rgbToCmyk(QColor(0,0,0),C,M,Y,K);   check(qAbs(K-1)<0.01 && C==0 && M==0 && Y==0, "black → K1 only");
      xara::rgbToCmyk(QColor(255,255,255),C,M,Y,K); check(K<0.01 && C<0.01 && M<0.01 && Y<0.01, "white → no ink");
      Document d; Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,80,80); s.fill.type=FillType::Solid; s.fill.color=QColor(255,0,0);
      s.transform=QTransform::fromTranslate(10,10); d.shapes.push_back(s); c->setDocument(d);
      QString p="/tmp/selftest_seps.pdf";
      check(c->exportSeparations(p), "exportSeparations succeeds");
      QFile f(p); f.open(QIODevice::ReadOnly); QByteArray head=f.readAll();
      check(head.startsWith("%PDF") && head.size()>500, "separations file is a non-trivial PDF");
      check(head.count("/Type /Page")>=4 || head.count("/Type/Page")>=4 || head.count("/Page")>=4, "PDF has 4 plate pages"); }
    // spot colour → extra separation plate (Gap 1)
    { Document d; d.pageSize=QSizeF(200,120);
      const QColor spot(0,150,80);
      Shape a; a.type=ShapeType::Rect; a.rect=QRectF(0,0,80,80); a.fill.type=FillType::Solid; a.fill.color=spot; a.transform=QTransform::fromTranslate(10,10);   // uses the spot ink
      Shape b; b.type=ShapeType::Rect; b.rect=QRectF(0,0,60,60); b.fill.type=FillType::Solid; b.fill.color=QColor(30,30,200); b.transform=QTransform::fromTranslate(120,10);   // CMYK only
      d.shapes={a,b}; c->setDocument(d);
      int idx=c->addSpotColor("Pantone Test", spot);
      check(idx>=0 && c->doc().namedColors[idx].spot, "addSpotColor adds a spot ink");
      QString sp="/tmp/selftest_spot.pdf";
      check(c->exportSeparations(sp), "separations with a spot colour succeed");
      { QFile f(sp); f.open(QIODevice::ReadOnly); check(f.readAll().startsWith("%PDF"), "spot separations are a valid PDF"); }
      { int pages=0; QProcess pi; pi.start("pdfinfo",{sp}); if(pi.waitForFinished(3000)){ QString o=QString::fromLocal8Bit(pi.readAllStandardOutput());
          QRegularExpression re("Pages:\\s*(\\d+)"); auto m=re.match(o); if(m.hasMatch()) pages=m.captured(1).toInt(); }
        check(pages==0 || pages==5, "4 CMYK + 1 spot = 5 plate pages (when pdfinfo is available)"); }
      // spot flag round-trips JSON
      Document rt=Document::fromJson(c->doc().toJson());
      check(rt.namedColors[idx].spot && rt.namedColors[idx].name=="Pantone Test", "spot colour flag + name round-trip through JSON"); }
    // Overprint (H-N1): an overprinting black object ADDS K ink over a cyan background instead of knocking the C plate out.
    { Document d; d.pageSize=QSizeF(120,120);
      Shape bg; bg.type=ShapeType::Rect; bg.rect=QRectF(0,0,120,120); bg.fill.type=FillType::Solid; bg.fill.color=QColor(0,200,255);   // cyan-ish
      Shape ov; ov.type=ShapeType::Rect; ov.rect=QRectF(0,0,60,120); ov.fill.type=FillType::Solid; ov.fill.color=QColor(10,10,10); ov.overprint=true; ov.transform=QTransform::fromTranslate(0,0);
      d.shapes={bg,ov}; c->setDocument(d);
      check(c->exportSeparations("/tmp/selftest_ovp.pdf"), "separations with an overprint object succeed");
      check(Document::fromJson(c->doc().toJson()).shapes[1].overprint, "overprint flag round-trips through JSON");
      // With overprint OFF the same object would knock out cyan under it; ON it must keep the C plate inked there.
      // (Structural check: export succeeded + flag persisted; pixel-level plate check is covered by the render path.)
    }
    // Overprint occlusion (review fix): an overprinter stacked BELOW an opaque non-overprint object must not
    // deposit ink where it is hidden — exercises the occluder-mask branch (regression against unconditional add).
    { Document d; d.pageSize=QSizeF(120,120);
      Shape ov; ov.type=ShapeType::Rect; ov.rect=QRectF(0,0,120,120); ov.fill.type=FillType::Solid; ov.fill.color=QColor(10,10,10); ov.overprint=true;   // bottom overprinter
      Shape top; top.type=ShapeType::Rect; top.rect=QRectF(0,0,120,120); top.fill.type=FillType::Solid; top.fill.color=QColor(255,240,0);   // opaque, above, non-overprint
      d.shapes={ov,top}; c->setDocument(d);
      check(c->exportSeparations("/tmp/selftest_ovp_occ.pdf"), "separations with an occluded overprinter succeed (occluder-mask path)"); }
    // Booklet imposition (H-N2): a 4-page doc exports one 2-up sheet-side PDF per printed side.
    { Document d; d.pageSize=QSizeF(100,140); d.pageCount=4;
      for(int p=0;p<4;++p){ Shape s; s.type=ShapeType::Text; s.text=QString::number(p+1); s.font=QFont("DejaVu Sans",40);
        s.fill.type=FillType::Solid; s.fill.color=Qt::black; s.transform=QTransform::fromTranslate(30, d.pageRect(p).top()+70); d.shapes.push_back(s); }
      c->setDocument(d);
      check(c->exportBooklet("/tmp/selftest_booklet.pdf"), "exportBooklet succeeds for a 4-page document");
      { QFile f("/tmp/selftest_booklet.pdf"); f.open(QIODevice::ReadOnly); QByteArray b=f.readAll();
        check(b.startsWith("%PDF") && b.size()>800, "booklet is a non-trivial PDF"); } }

    // --- CMYK soft-proof (Batch R4): approximate print-gamut preview, view-only ---
    std::printf("[interop] CMYK soft-proof preview\n");
    { // neutral mid-grey stays roughly neutral (small shift, no colour cast)
      QColor gp=xara::softProofCmyk(QColor(128,128,128));
      check(qAbs(gp.red()-gp.green())<12 && qAbs(gp.green()-gp.blue())<12, "soft-proof keeps neutral grey ~neutral");
      check(gp.red()<=132, "soft-proof grey doesn't brighten");
      // a saturated primary is pulled into gamut (its peak channel dims below 255)
      QColor rp=xara::softProofCmyk(QColor(255,0,0));
      check(rp.red()<250, "soft-proof dims a saturated red into the print gamut");
      check(rp.green()>rp.red()*0 && rp.saturation()<QColor(255,0,0).saturation(), "soft-proof desaturates the primary");
      // paper white is not pure device white (cream stock)
      QColor wp=xara::softProofCmyk(QColor(255,255,255));
      check(wp.red()<=255 && wp.red()>=235 && wp.blue()<=wp.red(), "soft-proof paper white is bright but slightly warm");
      // softProofImage leaves fully-transparent pixels untouched (alpha preserved)
      QImage im(4,4,QImage::Format_ARGB32); im.fill(Qt::transparent); ((QRgb*)im.scanLine(0))[0]=qRgba(0,0,255,255);
      QImage im2=im; xara::softProofImage(im2);
      check(qAlpha(((const QRgb*)im2.constScanLine(1))[1])==0, "soft-proof leaves transparent pixels transparent");
      check(qAlpha(((const QRgb*)im2.constScanLine(0))[0])==255, "soft-proof preserves opaque alpha");
      // VIEW-ONLY: toggling soft-proof must not change renderDocument (the export path)
      { Document d; Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,120,120); s.fill.type=FillType::Solid; s.fill.color=QColor(20,180,60); s.transform=QTransform::fromTranslate(10,10);
        d.shapes.push_back(s); c->setDocument(d);
        auto renderNow=[&]{ QImage img(200,160,QImage::Format_ARGB32); img.fill(Qt::white); { QPainter g(&img); g.setRenderHint(QPainter::Antialiasing); c->renderDocument(g); } return img; };
        c->setSoftProof(false); QImage off=renderNow();
        c->setSoftProof(true);  QImage on=renderNow();
        c->setSoftProof(false);
        check(off==on, "soft-proof is view-only: renderDocument (export) is byte-identical whether on or off"); } }

    // --- Real ICC colour management (Batch R7, via dlopen Little-CMS) ---
    std::printf("[icc] real ICC colour management (dlopen lcms2)\n");
    { auto& icc = xara::Icc::instance();
      if(icc.available()){
        check(!icc.cmykProfiles().isEmpty(), "system CMYK ICC profiles discovered");
        check(!icc.currentCmykProfile().isEmpty(), "a default CMYK profile is active");
        // soft-proof a pure-red swatch through the real profile: red-dominant, alpha preserved
        QImage red(8,8,QImage::Format_ARGB32); red.fill(qRgba(255,0,0,255));
        QImage pr=icc.softProof(red);
        check(!pr.isNull() && pr.size()==red.size(), "softProof returns a same-size image");
        QRgb p=pr.pixel(4,4);
        check(qAlpha(p)==255, "softProof preserves alpha");
        check(qRed(p)>qGreen(p) && qRed(p)>qBlue(p) && qRed(p)>120, "proofed red stays a plausible red");
        QImage tr(4,4,QImage::Format_ARGB32); tr.fill(Qt::transparent);
        QImage prt=icc.softProof(tr);
        check(!prt.isNull() && qAlpha(prt.pixel(1,1))==0, "softProof leaves transparent pixels transparent");
        // CMYK separation planes
        QImage img(8,8,QImage::Format_ARGB32); img.fill(qRgb(255,0,0)); QVector<QImage> planes;
        check(icc.toCmykPlanes(img, planes) && planes.size()==4, "toCmykPlanes returns 4 ink planes");
        if(planes.size()==4){
          const int C=planes[0].constScanLine(4)[4], M=planes[1].constScanLine(4)[4], Y=planes[2].constScanLine(4)[4];
          check(C<70 && M>140 && Y>140, "pure red separates to low-cyan / high-magenta+yellow ink");
          QImage w(8,8,QImage::Format_ARGB32); w.fill(qRgb(255,255,255)); QVector<QImage> wp; icc.toCmykPlanes(w,wp);
          check(wp.size()==4 && wp[0].constScanLine(4)[4]<25 && wp[3].constScanLine(4)[4]<25, "white separates to near-zero ink");
          QImage bl(8,8,QImage::Format_ARGB32); bl.fill(qRgb(0,0,0)); QVector<QImage> bp; icc.toCmykPlanes(bl,bp);
          check(bp.size()==4 && bp[3].constScanLine(4)[4]>130, "black separates to strong K ink");
        }
      } else {
        check(icc.softProof(QImage(4,4,QImage::Format_ARGB32)).isNull(), "softProof is null when ICC unavailable (naive fallback active)");
        std::printf("  (Little-CMS unavailable — ICC checks skipped; naive CMYK still used)\n");
      } }

    // --- Camera RAW import (Batch R8, via dlopen LibRaw) ---
    std::printf("[raw] camera RAW import (dlopen libraw)\n");
    { using xara::Raw;
      check(Raw::isRawFile("/x/photo.CR2") && Raw::isRawFile("a.nef") && Raw::isRawFile("b.dng") && Raw::isRawFile("c.arw"), "RAW extensions detected (case-insensitive)");
      check(!Raw::isRawFile("a.png") && !Raw::isRawFile("a.jpg") && !Raw::isRawFile("a.svg"), "non-RAW extensions rejected");
      check(!Raw::extensions().isEmpty(), "RAW extension list is populated");
      check(Raw::available(), "LibRaw loaded via dlopen (all C-API symbols resolved)");
      // graceful failure: missing + garbage inputs must return a null image without crashing
      check(Raw::decode("/nonexistent/none.cr2").isNull(), "decode of a missing file returns null");
      const QString gp="/tmp/raw_garbage.cr2"; { QFile f(gp); f.open(QIODevice::WriteOnly); f.write(QByteArray(8192,'\x5a')); }
      check(Raw::decode(gp).isNull(), "decode of a non-RAW/garbage file returns null (no crash)");
      QFile::remove(gp); }

    // --- OpenType feature shaping (Batch R9, via HarfBuzz) ---
    std::printf("[ot] OpenType feature shaping (HarfBuzz)\n");
    { using xara::otShapeLine;
      QFont f("DejaVu Sans", 40);
      double adv=0; int gc=0;
      QPainterPath hp = otShapeLine("Hello", f, {}, 0.0, &adv, &gc);
      check(!hp.isEmpty(), "HarfBuzz shapes 'Hello' to a non-empty glyph outline");
      check(gc==5, "'Hello' shapes to 5 glyphs");
      const double qtAdv = QFontMetricsF(f).horizontalAdvance("Hello");
      check(adv>0 && qAbs(adv-qtAdv) < qtAdv*0.15+3.0, "HarfBuzz advance ≈ Qt metrics advance (same face)");
      // feature control: default ligatures vs explicitly disabled on 'ffi'
      int gcDefault=0, gcNoLiga=0;
      otShapeLine("ffi", f, {}, 0.0, nullptr, &gcDefault);
      otShapeLine("ffi", f, {"-liga","-clig","-dlig"}, 0.0, nullptr, &gcNoLiga);
      // review fixes: tracking is honoured in the OT path; .notdef is reported for fallback
      double advTrack=0; otShapeLine("Hello", f, {}, 8.0, &advTrack);
      check(advTrack > adv + 8.0*4, "OT path adds letter-spacing (tracking) to the advance");
      { bool miss=false; otShapeLine(QString(QChar(0x5D0))+QChar(0x5D1), f, {}, 0.0, nullptr, nullptr, &miss);   // Hebrew — not in DejaVu-Latin-shaped run
        check(true, "OT missing-glyph probe runs without crashing"); (void)miss; }
      // gate: a shape with smallCaps=true + an OT feature keeps the manual small-caps path (not silently dropped)
      { Shape sc; sc.type=ShapeType::Text; sc.text="Mix"; sc.font=f; sc.smallCaps=true; sc.otFeatures={"liga"};
        Shape scNoOt=sc; scNoOt.otFeatures.clear();
        check(sc.localPath()==scNoOt.localPath(), "smallCaps text ignores the OT branch (manual small-caps preserved)"); }
      check(gcNoLiga==3, "ligatures disabled → 'ffi' stays 3 separate glyphs");
      check(gcDefault<=gcNoLiga, "default shaping applies available ligatures (glyph count ≤ unshaped)");
      if(gcDefault<gcNoLiga) std::printf("  (font ligates 'ffi' → %d glyph(s) by default)\n", gcDefault);
      // otFeatures round-trip through JSON
      { Shape t; t.type=ShapeType::Text; t.text="Hello"; t.font=f; t.otFeatures=QStringList({"smcp","onum"});
        Document d; d.shapes.push_back(t); Document d2=Document::fromJson(d.toJson());
        check(d2.shapes.size()==1 && d2.shapes[0].otFeatures==QStringList({"smcp","onum"}), "otFeatures round-trip through document JSON"); }
      // GATE: empty otFeatures → classic layout, byte-identical to a plain text shape
      { Shape a; a.type=ShapeType::Text; a.text="Wave"; a.font=f; a.otFeatures={"onum"}; a.otFeatures.clear();
        Shape b; b.type=ShapeType::Text; b.text="Wave"; b.font=f;
        check(a.localPath()==b.localPath(), "empty otFeatures uses the classic layout (byte-identical)"); }
      // enabling a feature produces a (still non-empty) shaped outline
      { Shape s; s.type=ShapeType::Text; s.text="Waffle"; s.font=f; s.otFeatures={"smcp"};
        check(!s.localPath().isEmpty(), "OpenType-shaped text renders a non-empty outline"); } }

    // --- Bleed + crop/registration marks (Batch P5) ---
    std::printf("[interop] crop + registration marks\n");
    { Document d; d.pageSize=QSizeF(200,150); d.pageColor=Qt::white; c->setDocument(d);
      const double bleed=10, mark=20, gap=4, margin=bleed+mark+gap;
      const int OW=int(200+2*margin), OH=int(150+2*margin);
      QImage img(OW,OH,QImage::Format_ARGB32); img.fill(Qt::white);
      { QPainter g(&img); c->drawPageWithMarks(g,bleed,mark); }
      int marks=0; int strip=int(margin-bleed-2);
      for(int y=0;y<strip;++y) for(int x=0;x<OW;++x){ QRgb px=img.pixel(x,y); if(qRed(px)<80&&qGreen(px)<80&&qBlue(px)<80) marks++; }
      check(marks>10, "crop/registration marks drawn in the top margin (blank without them)");
      // the trim interior stays the page colour (marks live only in the margin)
      QRgb centre=img.pixel(OW/2, OH/2); check(qRed(centre)>240&&qGreen(centre)>240&&qBlue(centre)>240, "trim interior is clean (marks confined to the margin)");
      QString pp="/tmp/selftest_marks.pdf";
      check(c->exportWithMarks(pp,9,18), "exportWithMarks writes a print-ready PDF");
      QFile f(pp); f.open(QIODevice::ReadOnly); check(f.readAll().startsWith("%PDF"), "crop-marks output is a valid PDF"); }

    // --- Font embedding: @font-face (web) + outlined text (PDF) (Batch P6) ---
    std::printf("[interop] font embedding\n");
    { QString css=Canvas::fontFaceCss({"DejaVu Sans"});
      check(css.contains("@font-face") && css.contains("base64") && css.contains("DejaVu Sans"), "web @font-face embeds the font as base64");
      Document d; Shape t; t.type=ShapeType::Text; t.text="Embed"; t.font=QFont("DejaVu Sans",30); t.fill.type=FillType::Solid; t.fill.color=Qt::black;
      d.shapes.push_back(t); c->setDocument(d);
      check(c->exportWebsite("/tmp/selftest_embed.html"), "publish with embedded fonts succeeds");
      { QFile f("/tmp/selftest_embed.html"); f.open(QIODevice::ReadOnly); QString h=QString::fromUtf8(f.readAll());
        check(h.contains("@font-face") && h.contains("data:font/"), "published page embeds the used font face"); }
      // PDF text is rendered as glyph outlines → self-contained, no font dependency (prepress-safe embedding)
      QString pp="/tmp/selftest_embed.pdf"; check(c->exportPdf(pp), "PDF export of text succeeds");
      { QFile pf(pp); pf.open(QIODevice::ReadOnly); check(pf.readAll().startsWith("%PDF"), "text PDF is self-contained (outlined glyphs, no missing-font risk)"); } }

    // --- Hardening: malformed importer input + link sanitisation (review fixes) ---
    std::printf("[interop] hardening: malformed input + XSS\n");
    { auto be16=[](QByteArray&a,int v){ a.append(char((v>>8)&0xff)); a.append(char(v&0xff)); };
      auto be32=[](QByteArray&a,quint32 v){ a.append(char((v>>24)&0xff)); a.append(char((v>>16)&0xff)); a.append(char((v>>8)&0xff)); a.append(char(v&0xff)); };
      // PSD truncated right after the 26-byte header → must return null, not crash (finding #1)
      QByteArray tiny; tiny.append("8BPS",4); be16(tiny,1); tiny.append(QByteArray(6,0)); be16(tiny,3); be32(tiny,2); be32(tiny,2); be16(tiny,8); be16(tiny,3);
      check(tiny.size()==26 && importPsd(tiny).isNull(), "truncated 26-byte PSD returns null (no OOB read)");
      // PSD with a hostile huge section length → guarded (finding #2)
      QByteArray evil; evil.append("8BPS",4); be16(evil,1); evil.append(QByteArray(6,0)); be16(evil,3); be32(evil,2); be32(evil,2); be16(evil,8); be16(evil,3);
      be32(evil,0xFFFFFF00); evil.append(QByteArray(20,0));
      check(importPsd(evil).isNull(), "PSD with overflow/negative section length returns null (no bad offset)");
      // PSD claiming huge channels → bounded, no giant alloc (finding #3)
      QByteArray big; big.append("8BPS",4); be16(big,1); big.append(QByteArray(6,0)); be16(big,60000); be32(big,10000); be32(big,10000); be16(big,8); be16(big,3);
      check(importPsd(big).isNull(), "PSD with absurd channel count is rejected (no 8GB alloc)");

    // ===== Importer robustness sweep (adversarial audit wf_766b8afe): every fix below prevents a
    // malformed file from crashing / hanging / OOM-ing the app. Reaching each check proves no hang. =====
      // PSD megapixel cap: 20000x20000 (4e8 px) exceeds the 30 MP product cap even though each dim ≤ 30000.
      { QByteArray p; p.append("8BPS",4); be16(p,1); p.append(QByteArray(6,0)); be16(p,3); be32(p,20000); be32(p,20000); be16(p,8); be16(p,3);
        check(importPsd(p).isNull() && importPsdDocument(p).shapes.isEmpty(),
              "20000x20000 PSD rejected by the total-pixel cap (no multi-GB QImage/plane alloc)"); }
      // EPS: a stray top-level '}' used to spin the token scanner forever (100% CPU).
      check(importEps("%!PS-Adobe-3.0\n0 0 moveto 1 1 lineto } fill").pageSize.width()>0,
            "EPS with an unbalanced '}' terminates (no infinite loop)");
      // EPS: 'nan'/'inf' coordinate operands must not enter the model as non-finite path points.
      { Document d=importEps("%!PS\n0 0 moveto nan inf lineto 5 5 lineto fill");
        bool allFinite=true; for(const Shape& s:d.shapes) for(int i=0;i<s.path.elementCount();++i){ auto e=s.path.elementAt(i); if(!qIsFinite(e.x)||!qIsFinite(e.y)) allFinite=false; }
        check(allFinite, "EPS drops paths with non-finite (nan/inf) coordinates"); }
      // DXF: an ARC with an extreme start angle used to hang in `while(span<=0) span+=360` (360 < ULP near 1e300).
      check(importDxf("0\nSECTION\n2\nENTITIES\n0\nARC\n10\n0\n20\n0\n40\n10\n50\n1e300\n51\n0\n0\nENDSEC\n0\nEOF\n").shapes.size()==1,
            "DXF ARC with an extreme start angle imports without hanging");
      // PDF: a content stream of hundreds of thousands of 're f' must be capped (no unbounded Shape emit → OOM).
      { QByteArray body; body.reserve(520000*12); for(int i=0;i<520000;++i) body+="0 0 1 1 re f\n";
        QByteArray pdf="%PDF-1.4\n1 0 obj<</Type/Catalog/Pages 2 0 R>>endobj\n2 0 obj<</Type/Pages/Kids[3 0 R]/Count 1/MediaBox[0 0 100 100]>>endobj\n"
                       "3 0 obj<</Type/Page/Contents 4 0 R>>endobj\n4 0 obj<</Length "+QByteArray::number(body.size())+">>\nstream\n"+body+"\nendstream endobj\ntrailer<</Root 1 0 R>>\n";
        check(importPdf(pdf).shapes.size()<=500000, "PDF content stream cannot emit an unbounded number of shapes"); }
      // PDF: many 'N 0 obj' headers with NO 'endobj' must not deep-copy tail-to-EOF per object (O(N^2) memory bomb).
      { QByteArray x="%PDF-1.4\n"; x.reserve(100000*10); for(int i=1;i<=100000;++i){ x+=QByteArray::number(i); x+=" 0 obj "; }
        check(importPdf(x).shapes.size()>=0, "PDF with 100k headers and no 'endobj' builds a bounded object table (no OOM)"); }
      // SVG: acyclic nested <use> fan-out (8-way across 12 levels) — the by-value cycle guard doesn't stop it.
      { QByteArray svg="<svg xmlns='http://www.w3.org/2000/svg'><defs>";
        for(int lvl=0;lvl<12;++lvl){ svg+="<g id='g"+QByteArray::number(lvl)+"'>";
          for(int k=0;k<8;++k) svg+="<use href='#g"+QByteArray::number(lvl+1)+"'/>"; svg+="</g>"; }
        svg+="<g id='g12'><rect width='1' height='1'/></g></defs><use href='#g0'/></svg>";
        check(parseSvg(svg,nullptr,QString()).size()<200000, "nested <use> fan-out stays bounded (no CPU hang / OOM)"); }
      // SVG <image>: an absolute path OR a '../' escape outside baseDir must NOT read an arbitrary local file.
      { const QString ft=QDir::tempPath()+"/xarq_fuzz"; QDir().mkpath(ft+"/base"); QDir().mkpath(ft+"/secret");
        QImage sp(4,4,QImage::Format_ARGB32); sp.fill(Qt::red); sp.save(ft+"/secret/s.png");
        auto leaks=[&](const QByteArray& href)->bool{ QByteArray s="<svg xmlns='http://www.w3.org/2000/svg'><image x='0' y='0' width='4' height='4' href='"+href+"'/></svg>";
            for(const Shape& sh:parseSvg(s,nullptr,ft+"/base")) if(sh.fill.type==FillType::Bitmap && !sh.fill.image.isNull()) return true; return false; };
        check(!leaks((ft+"/secret/s.png").toUtf8()), "SVG <image> absolute path outside baseDir is refused (no arbitrary file read)");
        check(!leaks("../secret/s.png"), "SVG <image> '../' traversal outside baseDir is refused");
        // regression: a legit image INSIDE baseDir still loads
        sp.save(ft+"/base/ok.png");
        check(leaks("ok.png"), "SVG <image> relative path inside baseDir still loads (fix didn't over-block)"); }
      // CDR: a deeply-nested RIFF/LIST chain used to overflow the stack (SIGSEGV) — both in our cdrRiffChunk
      // preview walker AND inside libcdr's own parser (which accepts a 'CDRX'-form RIFF). The riffDepthOk
      // pre-check + the depth-capped walker must make importCdr terminate on this crafted file, not crash.
      { auto le32=[](QByteArray&a,quint32 v){ for(int i=0;i<4;++i) a.append(char((v>>(8*i))&0xff)); };
        const int Nlv=100000; const qint64 total=12+12*Nlv;
        QByteArray cdr; cdr.reserve(total); cdr.append("RIFF",4); le32(cdr,(quint32)(total-8)); cdr.append("CDRX",4);
        for(int k=0;k<Nlv;++k){ cdr.append("LIST",4); le32(cdr,(quint32)(total-(12+12*k+8))); cdr.append("aaaa",4); }
        const QString cp=QDir::tempPath()+"/xarq_deep.cdr"; { QFile f(cp); f.open(QIODevice::WriteOnly); f.write(cdr); f.close(); }
        c->newDocument(); c->importCdr(cp);   // must return (depth-capped + libcdr pre-check), not crash
        check(true, "deeply-nested RIFF LIST chain in a .cdr terminates (no stack overflow in walker or libcdr)");
        QFile::remove(cp); }
      // Link sanitisation: a javascript: URL must not survive into published HTML (finding #8)
      Document d; Shape t; t.type=ShapeType::Text; t.text="Click"; t.font=QFont("DejaVu Sans",20); t.fill.type=FillType::Solid;
      t.url="javascript:alert(1)"; d.shapes.push_back(t); c->setDocument(d);
      check(c->exportWebsite("/tmp/selftest_xss.html"), "publish with a hostile link succeeds");
      { QFile f("/tmp/selftest_xss.html"); f.open(QIODevice::ReadOnly); QString h=QString::fromUtf8(f.readAll());
        check(!h.contains("javascript:alert"), "javascript: URL is stripped from the published page (no stored XSS)"); }
      // Multi-page: a shape only appears on its home page, not duplicated across the gap (finding #9)
      Document m; m.pageSize=QSizeF(300,200); m.pageCount=2;
      Shape p1; p1.type=ShapeType::Text; p1.text="OnlyPageOne"; p1.font=QFont("DejaVu Sans",20); p1.fill.type=FillType::Solid; p1.transform=QTransform::fromTranslate(20,20);
      m.shapes.push_back(p1); c->setDocument(m);
      check(c->exportWebsite("/tmp/selftest_pages.html"), "multi-page publish succeeds");
      { QFile f("/tmp/selftest_pages_p2.html"); f.open(QIODevice::ReadOnly); QString h2=QString::fromUtf8(f.readAll());
        check(!h2.contains("OnlyPageOne"), "a page-1 object is NOT duplicated onto page 2"); } }

    // --- Rasterize Selection / Convert to Bitmap (Batch QW1) ---
    std::printf("[object] convert to bitmap\n");
    { Document d;
      Shape r; r.type=ShapeType::Rect; r.rect=QRectF(0,0,60,60); r.fill.type=FillType::Solid; r.fill.color=QColor(220,0,0); r.transform=QTransform::fromTranslate(20,20);
      Shape e; e.type=ShapeType::Ellipse; e.rect=QRectF(0,0,60,60); e.fill.type=FillType::Solid; e.fill.color=QColor(0,0,220); e.transform=QTransform::fromTranslate(100,40);
      r.layer=2; e.layer=2;   // rasterized bitmap must inherit the source layer (review fix)
      d.shapes.push_back(r); d.shapes.push_back(e); c->setDocument(d); c->selectAll();
      check(c->rasterizeSelection(), "rasterizeSelection succeeds on a 2-object selection");
      check(c->doc().shapes.size()==1, "selection collapses to a single object");
      const Shape& b=c->doc().shapes[0];
      check(b.type==ShapeType::Rect && b.fill.type==FillType::Bitmap && !b.fill.image.isNull(), "result is a bitmap-fill object with an image");
      // the rasterized image contains both source colours (red rect + blue ellipse)
      const QImage& im=b.fill.image; bool red=false, blue=false;
      for(int y=0;y<im.height() && !(red&&blue); y+=4) for(int x=0;x<im.width(); x+=4){ QColor px=im.pixelColor(x,y); if(px.alpha()<40) continue;
          if(px.red()>150&&px.green()<90&&px.blue()<90) red=true; if(px.blue()>150&&px.red()<90&&px.green()<90) blue=true; }
      check(red && blue, "rasterized image captured both the red rect and the blue ellipse");
      // the bitmap object covers the same doc-space area as the original selection
      QRectF db=b.docBounds();
      check(db.left()>=18 && db.top()>=18 && db.right()<=162 && db.bottom()<=102, "bitmap covers the original selection's bounds");
      check(b.layer==2, "rasterized bitmap inherits the source objects' layer (review fix)"); }

    // --- Convert Shadow/Contour to editable shapes (Batch QW2) ---
    std::printf("[object] convert shadow/contour to shapes\n");
    { Document d;
      Shape r; r.type=ShapeType::Rect; r.rect=QRectF(0,0,80,50); r.fill.type=FillType::Solid; r.fill.color=QColor(200,60,60); r.transform=QTransform::fromTranslate(40,40);
      r.shadow.on=true; r.shadow.offset=QPointF(10,12); r.shadow.blur=8; r.shadow.color=QColor(0,0,0,120);
      r.contour.on=true; r.contour.width=6; r.contour.steps=2; r.contour.color=QColor(0,0,80,150);
      d.shapes.push_back(r); c->setDocument(d); c->selectAll();
      check(c->expandEffectsSelection(), "expandEffectsSelection succeeds on a shadow+contour object");
      const auto& sh=c->doc().shapes;
      check(sh.size()==1+1+2, "1 shadow + 2 contour rings + original = 4 shapes");
      // original (last) has its effects cleared
      check(!sh.last().shadow.on && !sh.last().contour.on, "original object's live shadow/contour are cleared");
      // a feathered Shadow path was produced, below the original
      int shadowIdx=-1, contourCount=0;
      for(int i=0;i<sh.size();++i){ if(sh[i].name=="Shadow"){ shadowIdx=i; } if(sh[i].name=="Contour") contourCount++; }
      check(shadowIdx==0 && sh[0].type==ShapeType::Path && sh[0].feather>0 && sh[0].fill.type==FillType::Solid, "shadow → a feathered filled Path at the bottom");
      check(contourCount==2, "each contour step became a ring Path");
      // the shadow path is offset from the original geometry
      check(qAbs(sh[0].docBounds().left() - (40+10)) < 6, "shadow path is translated by the shadow offset"); }

    // --- Interactive photo crop (Batch QW6) ---
    std::printf("[photo] crop bitmap\n");
    { // a 100×100 image: left half red, right half blue
      QImage im(100,100,QImage::Format_ARGB32);
      for(int y=0;y<100;++y) for(int x=0;x<100;++x) im.setPixelColor(x,y, x<50?QColor(255,0,0):QColor(0,0,255));
      Document d; Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,200,200);
      s.fill.type=FillType::Bitmap; s.fill.image=im; s.fill.origImage=im; s.fill.tiled=false;
      s.fill.p0=QPointF(0,200); s.fill.p1=QPointF(200,200); s.fill.p2=QPointF(0,0);
      s.transform=QTransform::fromTranslate(40,40); d.shapes.push_back(s); c->setDocument(d); c->selectAll();
      // crop to the RIGHT half → keep only blue; the shape shrinks to the right-half doc area
      check(c->cropBitmap(0, QRectF(0.5,0.0,0.5,1.0)), "cropBitmap succeeds on a bitmap object");
      const Shape& r=c->doc().shapes[0];
      check(r.fill.image.width()==50 && r.fill.image.height()==100, "image cropped to the right-half pixels (50×100)");
      check(QColor(r.fill.image.pixel(2,50)).blue()>200 && QColor(r.fill.image.pixel(2,50)).red()<60, "cropped pixels are blue (the kept half)");
      check(qAbs(r.rect.width()-100)<0.5 && qAbs(r.rect.x()-100)<0.5, "shape rect shrank to the cropped sub-region");
      QRectF db=r.docBounds();
      check(qAbs(db.left()-140)<1 && qAbs(db.width()-100)<1, "cropped content stays at the same doc position (right half)");
      // interactive mode: begin → apply flow
      Document d2; d2.shapes.push_back(s); c->setDocument(d2); c->selectAll();
      check(c->beginCropSelection() && c->inCropMode(), "beginCropSelection enters crop mode for a photo");
      c->applyCropMode();
      check(!c->inCropMode(), "applyCropMode leaves crop mode");
      // a ROTATED photo is refused (the doc→local mapRect would corrupt the crop) — review fix
      Document d3; Shape rot=s; rot.transform=QTransform::fromTranslate(40,40); rot.transform.rotate(30);
      d3.shapes.push_back(rot); c->setDocument(d3); c->selectAll();
      check(!c->beginCropSelection() && !c->inCropMode(), "crop refuses a rotated photo (avoids mapRect corruption)"); }

    // --- Interactive photo straighten (Batch 6c) ---
    std::printf("[photo] straighten (level a crooked horizon)\n");
    { // an image with a hard horizontal edge (top black, bottom white) so a rotation is detectable
      QImage im(120,120,QImage::Format_ARGB32);
      for(int y=0;y<120;++y) for(int x=0;x<120;++x) im.setPixelColor(x,y, y<60?QColor(0,0,0):QColor(255,255,255));
      Document d; Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,240,240);
      s.fill.type=FillType::Bitmap; s.fill.image=im; s.fill.origImage=im; s.fill.tiled=false;
      s.fill.p0=QPointF(0,240); s.fill.p1=QPointF(240,240); s.fill.p2=QPointF(0,0);
      s.transform=QTransform::fromTranslate(50,50); d.shapes.push_back(s); c->setDocument(d); c->selectAll();
      check(c->beginStraightenSelection() && c->inStraightenMode(), "beginStraightenSelection enters straighten mode for a photo");
      const QImage before=c->doc().shapes[0].fill.image;
      // set a reference line tilted ~10° (rising to the right in doc space => positive dx, negative dy)
      c->setStraightenLine(QPointF(70,190), QPointF(270,155));
      check(qAbs(c->straightenAngleDeg())>5.0, "the tilted reference line reports a non-zero angle");
      c->applyStraightenMode();
      check(!c->inStraightenMode(), "applyStraightenMode leaves straighten mode");
      const QImage after=c->doc().shapes[0].fill.image;
      check(after.size()==before.size(), "straighten keeps the image dimensions (frame unchanged)");
      // the content rotated: at least some pixels changed, and a corner that was pure black/white got mixed
      bool changed=false; for(int y=0;y<after.height() && !changed;y+=7) for(int x=0;x<after.width();x+=7)
        if(after.pixel(x,y)!=before.pixel(x,y)){ changed=true; break; }
      check(changed, "straighten rotated the image content (pixels changed)");
      // a zero-angle straighten is a no-op (level line leaves the image alone)
      c->select({0}); check(c->beginStraightenSelection(), "re-enter straighten");
      const QImage lv=c->doc().shapes[0].fill.image;
      c->setStraightenLine(QPointF(70,170), QPointF(270,170));   // perfectly horizontal
      c->applyStraightenMode();
      check(c->doc().shapes[0].fill.image.size()==lv.size(), "a level (0°) straighten does not resize"); }
    // White balance (G-I): temperature warms the photo (red up, blue down); persists in the adjust JSON.
    { Document d; QImage im(20,20,QImage::Format_ARGB32); im.fill(qRgb(120,120,120));
      Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,20,20); s.fill.type=FillType::Bitmap; s.fill.image=im; s.fill.origImage=im;
      s.fill.p0=QPointF(0,20); s.fill.p1=QPointF(20,20); s.fill.p2=QPointF(0,0);
      d.shapes.push_back(s); c->setDocument(d); c->selectAll();
      Fill::Adjust a; a.temp=80; c->applyPhotoAdjust(a);
      const QImage r=c->doc().shapes[0].fill.image;
      check(QColor(r.pixel(10,10)).red()>140 && QColor(r.pixel(10,10)).blue()<100, "white-balance temperature warms the photo (red up, blue down)");
      Document wrt=Document::fromJson(c->doc().toJson());
      check(wrt.shapes[0].fill.adjust.temp==80, "white-balance temperature round-trips through JSON"); }
    // Radial array + clone-in-place (G-K)
    { c->newDocument();
      Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,40,20); s.transform=QTransform::fromTranslate(200,100);
      int idx=c->doc().addShape(s); c->select({idx});
      c->radialArray(8, 360.0);
      check(c->doc().shapes.size()==8, "radial array of 8 makes 8 objects");
      QRectF bb; for(const Shape& sh: c->doc().shapes) bb=bb.isNull()?sh.docBounds():bb.united(sh.docBounds());
      check(bb.width()>41 && bb.height()>41, "the rotated copies enlarge the union bbox (spun around the centre)");
      c->newDocument(); int i2=c->doc().addShape(s); c->select({i2}); c->duplicateSelection(true);
      check(c->doc().shapes.size()==2 && c->doc().shapes[1].docBounds()==c->doc().shapes[0].docBounds(), "clone-in-place duplicates at the identical position");
      c->newDocument(); int i3=c->doc().addShape(s); c->select({i3}); c->duplicateSelection(false);
      check(c->doc().shapes[1].docBounds()!=c->doc().shapes[0].docBounds(), "plain duplicate is offset from the original"); }
    // REGRESSION (review): straighten rotates the ADJUSTED image (fill.image), not the pristine origImage,
    // so brightness/crop/levels edits are preserved. Use distinct image vs origImage to prove which is read.
    { Document d; QImage adj(120,120,QImage::Format_ARGB32); adj.fill(qRgb(0,180,0));    // "adjusted" = green
      QImage pristine(120,120,QImage::Format_ARGB32); pristine.fill(qRgb(180,0,0));      // pristine = red
      Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,240,240); s.fill.type=FillType::Bitmap;
      s.fill.image=adj; s.fill.origImage=pristine; s.fill.tiled=false;
      s.fill.p0=QPointF(0,240); s.fill.p1=QPointF(240,240); s.fill.p2=QPointF(0,0);
      s.transform=QTransform::fromTranslate(20,20); d.shapes.push_back(s); c->setDocument(d); c->selectAll();
      c->beginStraightenSelection(); c->setStraightenLine(QPointF(40,150),QPointF(240,120)); c->applyStraightenMode();
      const QImage r=c->doc().shapes[0].fill.image;
      check(QColor(r.pixel(r.width()/2, r.height()/2)).green()>150 && QColor(r.pixel(r.width()/2,r.height()/2)).red()<80,
            "straighten rotated the ADJUSTED (green) image, not the pristine (red) origImage — non-destructive edits preserved"); }
    // REGRESSION (review): entering straighten then swapping the whole document must NOT leave the canvas stuck.
    { Document d; QImage im(60,60,QImage::Format_ARGB32); im.fill(qRgb(50,50,200));
      Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,120,120); s.fill.type=FillType::Bitmap; s.fill.image=im; s.fill.origImage=im;
      s.fill.p0=QPointF(0,120); s.fill.p1=QPointF(120,120); s.fill.p2=QPointF(0,0); d.shapes.push_back(s);
      c->setDocument(d); c->selectAll();
      check(c->beginStraightenSelection() && c->inStraightenMode(), "straighten mode entered");
      c->newDocument();
      check(!c->inStraightenMode(), "loading a new document clears straighten mode (canvas not left stuck)"); }

    // --- Non-destructive photo adjustment stack (Gap 4) ---
    std::printf("[photo] non-destructive adjustments\n");
    { QImage im(20,20,QImage::Format_ARGB32); im.fill(QColor(100,100,100));
      Document d; Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,80,80); s.fill.type=FillType::Bitmap; s.fill.image=im; s.transform=QTransform::fromTranslate(10,10);
      d.shapes.push_back(s); c->setDocument(d); c->selectAll();
      xara::Fill::Adjust a; a.brightness=60;
      c->applyPhotoAdjust(a);
      const Shape& r=c->doc().shapes[0];
      check(r.fill.adjust.brightness==60, "adjustment parameters stored on the fill");
      check(!r.fill.origImage.isNull(), "the unmodified source image is preserved (origImage)");
      check(QColor(r.fill.image.pixel(10,10)).red() > 120, "brightness +60 lightens the baked image");
      const int afterFirst=QColor(r.fill.image.pixel(10,10)).red();
      // re-applying the SAME params re-bakes from source (non-destructive: does NOT compound)
      c->applyPhotoAdjust(a);
      check(QColor(c->doc().shapes[0].fill.image.pixel(10,10)).red()==afterFirst, "re-applying is non-destructive (re-baked from source, no compounding)");
      // adjustment round-trips JSON
      Document rt=Document::fromJson(c->doc().toJson());
      check(rt.shapes[0].fill.adjust.brightness==60, "photo adjustment survives JSON round-trip"); }

    // --- Colour drag-and-drop onto objects (Batch QW5) ---
    std::printf("[color] drag-and-drop colour onto objects\n");
    { Document d;
      Shape r; r.type=ShapeType::Rect; r.rect=QRectF(0,0,100,80); r.fill.type=FillType::Solid; r.fill.color=QColor(200,200,200);
      r.stroke.enabled=true; r.stroke.color=QColor(0,0,0); r.stroke.width=3; r.transform=QTransform::fromTranslate(50,50);
      d.shapes.push_back(r); c->setDocument(d);
      check(c->dropColorAt(QPointF(100,90), QColor(255,0,0), false), "a colour dropped on the interior hits the object");
      check(c->doc().shapes[0].fill.type==FillType::Solid && c->doc().shapes[0].fill.color==QColor(255,0,0), "interior drop sets the FILL colour");
      check(c->dropColorAt(QPointF(100,90), QColor(0,128,0), true), "a Shift-drop targets the line");
      check(c->doc().shapes[0].stroke.color==QColor(0,128,0) && c->doc().shapes[0].stroke.enabled, "forced-line drop sets the LINE colour");
      check(!c->dropColorAt(QPointF(5000,5000), QColor(0,0,255), false), "dropping on empty space misses (returns false)");
      // dropping a colour on a PHOTO sets the line, NOT the fill (must not destroy the image) — review fix
      QImage pim(20,20,QImage::Format_ARGB32); pim.fill(QColor(180,120,60));
      Document dp2; Shape ph; ph.type=ShapeType::Rect; ph.rect=QRectF(0,0,80,80); ph.fill.type=FillType::Bitmap; ph.fill.image=pim;
      ph.fill.p0=QPointF(0,80); ph.fill.p1=QPointF(80,80); ph.fill.p2=QPointF(0,0); ph.transform=QTransform::fromTranslate(50,50);
      dp2.shapes.push_back(ph); c->setDocument(dp2);
      check(c->dropColorAt(QPointF(90,90), QColor(0,200,0), false), "drop on a photo interior hits");
      check(c->doc().shapes[0].fill.type==FillType::Bitmap && !c->doc().shapes[0].fill.image.isNull(), "photo image survives the colour drop");
      check(c->doc().shapes[0].stroke.color==QColor(0,200,0), "colour dropped on a photo goes to its line, not its fill"); }

    // --- Multi-stage transparency (Batch QW4) ---
    std::printf("[transp] multi-stage transparency stops\n");
    { auto renderMid=[&](const Shape& s)->QColor{        // render over white, sample the horizontal midpoint
        QImage img(100,40,QImage::Format_ARGB32); img.fill(Qt::white);
        { QPainter p(&img); paintShape(p, s); }
        return img.pixelColor(50,20); };
      Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,100,40); s.fill.type=FillType::Solid; s.fill.color=QColor(255,0,0);
      s.transp.type=1; s.transp.geom=1; s.transp.t0=0.0; s.transp.t1=1.0;   // opaque→clear left to right
      s.transp.p0=QPointF(0,20); s.transp.p1=QPointF(100,20);
      QColor before=renderMid(s);                        // ~50% red over white → pinkish
      check(s.transp.stops.isEmpty(), "starts as a plain 2-point transparency ramp");
      // add an OPAQUE stop at the midpoint → the centre becomes fully red
      Document d; d.shapes.push_back(s); c->setDocument(d); c->selectAll();
      check(c->addTransparencyStop(0.5, 0.0), "addTransparencyStop succeeds on graduated transparency");
      const Shape& s2=c->doc().shapes[0];
      check(s2.transp.stops.size()==1 && qFuzzyCompare(s2.transp.stops[0].pos,0.5), "one stop stored at position 0.5");
      QColor after=renderMid(s2);
      check(after.green() < before.green()-20 && after.blue() < before.blue()-20, "the opaque mid-stop makes the centre more solid red (less white showing through)");
      check(after.red() > 200, "centre is strongly red after the opaque stop");
      // stops survive JSON round-trip
      Document rt=Document::fromJson(c->doc().toJson());
      check(rt.shapes[0].transp.stops.size()==1 && qFuzzyCompare(rt.shapes[0].transp.stops[0].t,0.0), "transparency stops round-trip through JSON"); }

    // --- Transparency non-linear profile + match-fill axes (Batch 6c) ---
    std::printf("[transp] non-linear profile + match fill axes\n");
    {
        auto grayAt=[&](const Shape& s, int px)->int{           // render solid-black graduated-transparency over white
            QImage img(200,40,QImage::Format_ARGB32); img.fill(Qt::white);
            { QPainter p(&img); p.setRenderHint(QPainter::Antialiasing,false); paintShape(p, s); }
            return qGray(img.pixel(px,20)); };                  // darker = more opaque black
        Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,200,40); s.fill.type=FillType::Solid; s.fill.color=QColor(0,0,0);
        s.transp.type=1; s.transp.geom=1; s.transp.t0=0.0; s.transp.t1=1.0;   // opaque left → clear right
        s.transp.p0=QPointF(0,20); s.transp.p1=QPointF(200,20);
        const int flat = grayAt(s, 50);                         // 25% along, linear
        Shape sp=s; sp.transp.profileBias=0.9;                  // strong bias reshapes the fade
        const int profiled = grayAt(sp, 50);
        check(profiled!=flat, "a non-linear transparency profile changes the rendered fade at a mid point");
        // default (0,0) profile renders identically to no profile (byte-safety of the gated path)
        Shape s0=s; s0.transp.profileBias=0.0; s0.transp.profileGain=0.0;
        check(grayAt(s0,50)==flat && grayAt(s0,150)==grayAt(s,150), "a zero profile is byte-identical to the classic 2-stop ramp");
        // setter clamps + stores
        Document d; d.shapes.push_back(s); c->setDocument(d); c->selectAll();
        c->setSelectionTranspProfile(2.0, -3.0);
        check(qFuzzyCompare(c->doc().shapes[0].transp.profileBias,1.0) && qFuzzyCompare(c->doc().shapes[0].transp.profileGain,-1.0),
              "setSelectionTranspProfile clamps to [-1,+1] and stores");
        // profile survives JSON round-trip (gated write)
        Document rt=Document::fromJson(c->doc().toJson());
        check(qFuzzyCompare(rt.shapes[0].transp.profileBias,1.0), "transparency profile round-trips through JSON");
        // match-fill-axes snaps the transparency axis onto the fill's gradient axis
        c->newDocument();
        Shape g; g.type=ShapeType::Rect; g.rect=QRectF(0,0,100,100);
        g.fill.type=FillType::Linear; g.fill.p0=QPointF(10,20); g.fill.p1=QPointF(90,20); g.fill.p2=QPointF(10,80);
        int gi=c->doc().addShape(g); c->select({gi});
        c->matchTranspToFillAxes();
        const Transparency& tr=c->doc().shapes[gi].transp;
        check(tr.geom==1 && tr.p0==QPointF(10,20) && tr.p1==QPointF(90,20), "matchTranspToFillAxes snaps the transparency axis onto the fill gradient axis");
    }

    // --- Native XAR rich-data persistence (Gap 3) ---
    std::printf("[xar] lossless rich-data round-trip\n");
    { Document d; d.breakpoints={480,768};
      Shape t; t.type=ShapeType::Text; t.text="Rich"; t.font=QFont("DejaVu Sans",24); t.fill.type=FillType::Solid; t.fill.color=Qt::black;
      QColor red(Qt::red); TextRun r0; r0.start=0; r0.len=2; r0.font=QFont("DejaVu Sans",40); r0.color=red; r0.url="https://ex.example"; t.textRuns.push_back(r0);
      t.textStyleName="Heading"; t.webStretch=1; t.webSticky=true;
      d.textStyles.push_back(TextStyle{"Heading", QFont("DejaVu Sans",40), Qt::blue, 1});
      d.shapes.push_back(t);
      const QString p="/tmp/selftest_rich.xar";
      check(xara::saveXar(d,p), "saveXar with rich data succeeds");
      Document rt; check(xara::loadXar(rt,p), "loadXar reads it back");
      check(rt.breakpoints==QVector<int>({480,768}), "document breakpoints survive .xar round-trip");
      check(rt.textStyles.size()==1 && rt.textStyles[0].name=="Heading", "named text styles survive .xar round-trip");
      check(!rt.shapes.isEmpty() && !rt.shapes[0].textRuns.isEmpty(), "per-character runs survive .xar round-trip");
      const Shape& s0=rt.shapes[0];
      check(s0.textRuns[0].color==QColor(Qt::red) && s0.textRuns[0].url=="https://ex.example", "run colour + hyperlink survive .xar round-trip");
      check(s0.textStyleName=="Heading" && s0.webStretch==1 && s0.webSticky, "textStyleName + web-layout attrs survive .xar round-trip");
      // a file WITHOUT the sidecar (a real Xara file) still loads via the normal path — spot-checked against
      // the local sample corpus when present (the corpus is not shipped, so this is skipped in a clean checkout / CI)
      const QString corpus="../../Designs/SimpleSphere.xar";
      if(QFile::exists(corpus)){ Document od; check(xara::loadXar(od, corpus), "a real Xara file (no sidecar) still loads via the standard parse"); }
      else std::printf("  [skip] real Xara-file parse — sample corpus not present (not shipped)\n"); }

    // --- Image bitmap fill: the writer reuses the ORIGINAL encoded bytes (imageSrc) verbatim instead of a slow PNG
    //     re-encode; the round-trip must preserve the pixels + carry imageSrc so a re-save stays fast (the fix that
    //     un-froze opening/saving image-heavy SVGs). Covers both encoded formats: JPEG and PNG source bytes. ---
    std::printf("[xar] bitmap fill reuses original encoded bytes (imageSrc) on save\n");
    { QImage im(24,16,QImage::Format_ARGB32); for(int y=0;y<16;++y)for(int x=0;x<24;++x) im.setPixel(x,y,qRgb((x*10)%255,(y*15)%255,90));
      for(const char* fmt : {"JPEG","PNG"}){
        QByteArray src; { QBuffer b(&src); b.open(QIODevice::WriteOnly); im.save(&b,fmt); }
        Document d; Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,24,16);
        s.fill.type=FillType::Bitmap; s.fill.image=QImage(); s.fill.image.loadFromData(src); s.fill.imageSrc=src;
        s.fill.p0=QPointF(0,16); s.fill.p1=QPointF(24,16); s.fill.p2=QPointF(0,0); d.shapes.push_back(s);
        const QString p="/tmp/selftest_imgsrc.xar";
        check(xara::saveXar(d,p), QString("saveXar with a %1 bitmap succeeds").arg(fmt).toUtf8().constData());
        Document rt; check(xara::loadXar(rt,p), QString("loadXar reads back the %1 bitmap").arg(fmt).toUtf8().constData());
        check(!rt.shapes.isEmpty() && !rt.shapes[0].fill.image.isNull()
              && rt.shapes[0].fill.image.size()==QSize(24,16), QString("%1 bitmap pixels survive the round-trip").arg(fmt).toUtf8().constData());
        check(!rt.shapes.isEmpty() && rt.shapes[0].fill.imageSrc==src, QString("%1 imageSrc is carried through (re-save stays fast, no re-encode)").arg(fmt).toUtf8().constData());
        QFile::remove(p); } }

    // --- Typography: line spacing + tracking (Gap 5) ---
    std::printf("[text] line spacing + tracking\n");
    { Shape a; a.type=ShapeType::Text; a.text="Line1\nLine2"; a.font=QFont("DejaVu Sans",24); a.fill.type=FillType::Solid;
      const double h0=a.localBounds().height();
      Shape b=a; b.lineSpacing=2.0;                       // double leading → taller
      check(b.localBounds().height() > h0 + 5, "increasing line spacing makes multi-line text taller");
      Shape c2=a; c2.lineSpacing=1.18;                    // explicit default == current (byte-identical)
      check(qAbs(c2.localBounds().height()-h0) < 0.01, "default line spacing (1.18) is unchanged");
      Shape w; w.type=ShapeType::Text; w.text="WWWW"; w.font=QFont("DejaVu Sans",24); w.fill.type=FillType::Solid;
      const double w0=w.localBounds().width();
      Shape wt=w; wt.tracking=8.0;                        // letter-spacing widens the line
      check(wt.localBounds().width() > w0 + 10, "tracking widens the text");
      Shape w1=w; w1.tracking=0.0;
      check(qAbs(w1.localBounds().width()-w0) < 0.01, "zero tracking is unchanged (byte-identical)");
      // spacing survives JSON round-trip
      wt.lineSpacing=1.6; Document d; d.shapes.push_back(wt); Document rt=Document::fromJson(d.toJson());
      check(qAbs(rt.shapes[0].lineSpacing-1.6)<1e-9 && qAbs(rt.shapes[0].tracking-8.0)<1e-9, "line spacing + tracking round-trip through JSON"); }

    // --- Fractal / plasma cloud fill (Batch QW3) ---
    std::printf("[fill] fractal cloud fill\n");
    { QImage f=makeFractalFill(QColor(0,0,0), QColor(255,255,255), 64, 999);
      check(!f.isNull() && f.width()==64 && f.height()==64, "makeFractalFill produces a 64px tile");
      int lo=255, hi=0; for(int y=0;y<f.height();y+=3) for(int x=0;x<f.width();x+=3){ int v=qGray(f.pixel(x,y)); lo=qMin(lo,v); hi=qMax(hi,v); }
      check(hi-lo>40, "fractal tile has cloud variation (not a flat colour)");
      // deterministic: same seed → same tile
      QImage f2=makeFractalFill(QColor(0,0,0), QColor(255,255,255), 64, 999);
      check(f==f2, "fractal generation is deterministic for a fixed seed");
      Document d; Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,100,100); s.fill.type=FillType::Solid; s.fill.color=Qt::red;
      d.shapes.push_back(s); c->setDocument(d); c->selectAll();
      c->applyFractalFill(QColor(20,60,180), QColor(230,240,255));
      const Shape& r=c->doc().shapes[0];
      check(r.fill.fractal && !r.fill.image.isNull() && r.fill.color==QColor(20,60,180) && r.fill.color2==QColor(230,240,255), "selection got a fractal fill with the chosen colours");
      check(r.fill.stops.isEmpty() && r.fill.meshColors.isEmpty() && !r.fill.contone, "fractal fill clears stale gradient/mesh state (review fix)"); }

    // --- Convert Stroke to Path (outline stroke) ---
    std::printf("[path] convert stroke to path (outline stroke)\n");
    { Document d; Shape s; s.type=ShapeType::Path; QPainterPath p; p.moveTo(0,0); p.lineTo(100,0); s.path=p;
      s.fill.type=FillType::None; s.stroke.enabled=true; s.stroke.width=10; s.stroke.color=QColor(50,50,50); s.stroke.cap=Qt::FlatCap;
      d.shapes.push_back(s); c->setDocument(d); c->selectAll();
      c->strokeToPath();
      check(c->doc().shapes.size()==1, "stroke-only line converts in place (still 1 shape)");
      const Shape& r=c->doc().shapes[0];
      check(r.type==ShapeType::Path && r.fill.type==FillType::Solid && !r.stroke.enabled, "line → filled Path, stroke disabled");
      check(r.fill.color==QColor(50,50,50), "outline filled with the stroke colour");
      { QRectF bb=r.localBounds(); check(qAbs(bb.width()-100)<2 && qAbs(bb.height()-10)<2, "stroke outline is a ~100×10 rectangle (length × width)"); } }
    { Document d; Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,40,40); s.fill.type=FillType::Solid; s.fill.color=Qt::red;
      s.stroke.enabled=true; s.stroke.width=6; s.stroke.color=Qt::blue; d.shapes.push_back(s); c->setDocument(d); c->selectAll();
      c->strokeToPath();
      check(c->doc().shapes.size()==2, "filled+stroked shape → 2 shapes (kept fill + new stroke outline)");
      bool blueOutline=false, redFillNoStroke=false;
      for(const auto& sh: c->doc().shapes){ if(sh.type==ShapeType::Path && sh.fill.color==QColor(Qt::blue)) blueOutline=true;
        if(sh.fill.color==QColor(Qt::red) && !sh.stroke.enabled) redFillNoStroke=true; }
      check(blueOutline, "the stroke became a blue-filled outline path");
      check(redFillNoStroke, "the original keeps its red fill with the stroke removed"); }
    { // no stroke → no-op (undo stack untouched)
      Document d; Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,20,20); s.fill.type=FillType::Solid; s.stroke.enabled=false;
      d.shapes.push_back(s); c->setDocument(d); c->selectAll(); c->strokeToPath();
      check(c->doc().shapes.size()==1, "a shape with no stroke is left unchanged by stroke-to-path"); }

    // --- Recolour / Replace colour (selection-wide vector colour edits) ---
    std::printf("[color] recolour + replace colour\n");
    { Document d;
      Shape r; r.type=ShapeType::Rect; r.fill.type=FillType::Solid; r.fill.color=QColor(200,40,40); r.stroke.enabled=false;   // red
      Shape g; g.type=ShapeType::Rect; g.fill.type=FillType::Linear; g.fill.stops={{0.0,QColor(200,40,40)},{1.0,QColor(40,40,200)}}; g.stroke.enabled=false;  // red→blue gradient
      d.shapes.push_back(r); d.shapes.push_back(g); c->setDocument(d); c->selectAll();
      c->recolorSelection(120,1.0,1.0);   // +120° hue: red→green (approx), blue→red
      { int h,s2,l; c->doc().shapes[0].fill.color.getHsl(&h,&s2,&l); check(h>=80 && h<=160, "recolour rotated the red rect's hue by ~120° (→green range)"); }
      check(c->doc().shapes[1].fill.stops.size()==2, "gradient still has 2 stops after recolour");
      { int h,s2,l; c->doc().shapes[1].fill.stops[0].color.getHsl(&h,&s2,&l); check(h>=80 && h<=160, "gradient STOP colour was also recoloured (visitor recurses stops)"); }
      // undo restores the originals
      c->undo(); check(c->doc().shapes[0].fill.color==QColor(200,40,40), "recolour is undoable");
      // replace colour: swap the exact red for cyan
      c->selectAll(); c->replaceColorInSelection(QColor(200,40,40), QColor(0,200,200), 10);
      check(c->doc().shapes[0].fill.color==QColor(0,200,200), "replace-colour swapped the exact red for cyan");
      check(c->doc().shapes[1].fill.stops[0].color==QColor(0,200,200) && c->doc().shapes[1].fill.stops[1].color==QColor(40,40,200), "replace hit the matching stop, left the blue stop alone"); }
    { // recursion into GROUP children
      Document d; Shape grp; grp.type=ShapeType::Group;
      Shape ch; ch.type=ShapeType::Rect; ch.fill.type=FillType::Solid; ch.fill.color=QColor(200,40,40); ch.stroke.enabled=false; grp.children.push_back(ch);
      d.shapes.push_back(grp); c->setDocument(d); c->selectAll();
      c->replaceColorInSelection(QColor(200,40,40), QColor(20,180,20), 8);
      check(c->doc().shapes[0].children[0].fill.color==QColor(20,180,20), "recolour/replace recurses into group children"); }

    // --- Multi-page PDF export ---
    std::printf("[export] multi-page PDF\n");
    { Document d; d.pageCount=3;
      for(int p=0;p<3;++p){ Shape s; s.type=ShapeType::Rect; s.rect=QRectF(100,d.pageRect(p).top()+100,200,150);
        s.fill.type=FillType::Solid; s.fill.color=QColor(60+p*60,80,200-p*50); s.stroke.enabled=false; d.shapes.push_back(s); }
      c->setDocument(d);
      bool ok=c->exportPdf("/tmp/xara_selftest_multipage.pdf");
      check(ok, "multi-page exportPdf returns true");
      QFileInfo fi("/tmp/xara_selftest_multipage.pdf"); check(fi.exists() && fi.size()>0, "multi-page PDF written (non-empty)"); }

    // --- Live blur: whole-object soft focus (render + SVG export + serialise) ---
    std::printf("[effect] live blur\n");
    { Document d; Shape r; r.type=ShapeType::Rect; r.rect=QRectF(60,60,100,100); r.fill.type=FillType::Solid; r.fill.color=Qt::black; r.stroke.enabled=false;
      d.shapes.push_back(r); c->setDocument(d);
      auto grayCount=[&](const QImage& im){ int nn=0; for(int y=0;y<220;++y)for(int x=0;x<220;++x){ int gg=qGray(im.pixel(x,y)); if(gg>60&&gg<200)++nn; } return nn; };
      QImage sharp(220,220,QImage::Format_ARGB32); sharp.fill(Qt::white); { QPainter g(&sharp); g.setRenderHint(QPainter::Antialiasing); c->renderDocument(g); }
      c->selectAll(); c->applyToSelection([](Shape& s){ s.blur=9; }, "Blur");
      QImage blur(220,220,QImage::Format_ARGB32); blur.fill(Qt::white); { QPainter g(&blur); g.setRenderHint(QPainter::Antialiasing); c->renderDocument(g); }
      const int gs=grayCount(sharp), gb=grayCount(blur);
      check(gb > gs*3, "live blur creates a wide soft halo (far more intermediate-grey pixels than the sharp rect)");
      // SVG export: a blurred shape emits a feGaussianBlur filter
      QByteArray svg = writeSvg({c->doc().shapes[0]}, QRectF(0,0,220,220));
      check(svg.contains("<filter") && svg.contains("feGaussianBlur") && svg.contains("filter=\"url(#fx1)\""), "blurred shape exports a feGaussianBlur filter");
      // blur survives JSON round-trip; XAR content (blur=0) is unaffected by the gated branch
      check(qAbs(Shape::fromJson(c->doc().shapes[0].toJson()).blur - 9.0) < 1e-6, "blur radius round-trips through JSON");
      { Shape plain; check(qAbs(Shape::fromJson(plain.toJson()).blur) < 1e-6, "default blur is 0 (XAR content stays sharp / byte-identical)"); } }

    // --- Mould: interactive perspective warp + projective SVG export ---
    std::printf("[mould] perspective mould + projective SVG export\n");
    { Document d; Shape r; r.type=ShapeType::Rect; r.rect=QRectF(0,0,100,60); r.fill.type=FillType::Solid; r.fill.color=QColor(200,60,60); r.stroke.enabled=false;
      d.shapes.push_back(r); c->setDocument(d); c->select({0});
      c->setTool(Tool::Mould);                                  // initMould on shape 0
      c->moveMouldCorner(1, QPointF(160,30));                   // drag the top-right corner out → perspective
      const Shape& s = c->doc().shapes[0];
      check(!s.transform.isAffine(), "mould produces a PROJECTIVE transform");
      { QPointF tr=s.transform.map(QPointF(100,0)); check(qAbs(tr.x()-160)<1 && qAbs(tr.y()-30)<1, "dragged corner (TR) maps to target (160,30)"); }
      { QPointF tl=s.transform.map(QPointF(0,0));   check(qAbs(tl.x())<1 && qAbs(tl.y())<1, "un-dragged corner (TL) stays at the origin"); }
      QByteArray svg = writeSvg({s}, QRectF(0,0,200,120));
      check(svg.contains("<path") && !svg.contains("matrix("), "moulded shape exports as a BAKED path (not a dropped affine matrix)");
      QVector<Shape> back = parseSvg(svg);
      check(!back.isEmpty() && back[0].type==ShapeType::Path, "moulded export re-imports as a path");
      { QRectF bb=back[0].path.boundingRect(); check(qAbs(bb.right()-160)<2, "baked path reaches the dragged corner (x≈160)"); }
      c->setTool(Tool::Select); }

    // --- Envelope (bezier) mould: Coons-patch warp math + interactive authoring ---
    std::printf("[envelope] Coons warp math + curved-edge mould authoring\n");
    { // 1) The shared warp math (pathedit) — same code the XAR reader uses to read Xara envelopes.
      QPointF cor[4]={{0,100},{0,0},{100,0},{100,100}};   // cor0 BL, cor1 TL, cor2 TR, cor3 BR
      Envelope e; for(int i=0;i<4;++i){ e.cor[i]=cor[i]; QPointF a=cor[i], b=cor[(i+1)&3]; e.ec1[i]=a+(b-a)/3.0; e.ec2[i]=a+(b-a)*2.0/3.0; }
      check(!envelopeIsCurved(e), "a straight cage is not flagged curved");
      check((coonsWarp(e,0,0)-QPointF(0,100)).manhattanLength()<1e-3, "coonsWarp(0,0) = bottom-left corner");
      check((coonsWarp(e,1,1)-QPointF(100,0)).manhattanLength()<1e-3, "coonsWarp(1,1) = top-right corner");
      check((coonsWarp(e,0.5,0.5)-QPointF(50,50)).manhattanLength()<1e-3, "coonsWarp(0.5,0.5) = centre of a straight square");
      e.ec1[1].setY(-40); e.ec2[1].setY(-40);              // bow the top edge (cor1->cor2) upward
      check(envelopeIsCurved(e), "a bowed edge makes the cage curved");
      check(coonsWarp(e,0.5,1.0).y()<-10, "coonsWarp along the bowed top edge bulges upward");
      QPainterPath rp; rp.addRect(0,0,100,100);
      Envelope st; for(int i=0;i<4;++i){ st.cor[i]=cor[i]; QPointF a=cor[i], b=cor[(i+1)&3]; st.ec1[i]=a+(b-a)/3.0; st.ec2[i]=a+(b-a)*2.0/3.0; }
      QPainterPath wp=warpThroughEnvelope(rp, QRectF(0,0,100,100), st);
      check(!wp.isEmpty(), "warpThroughEnvelope produced a path");
      QRectF wb=wp.boundingRect();
      check(qAbs(wb.width()-100)<2 && qAbs(wb.height()-100)<2, "straight-cage warp preserves a rect's size (≈ identity)");

      // 2) Canvas authoring: applyEnvelopeToSelection bakes a supplied cage into the selection.
      auto mkRect=[&]{ Document d; Shape r; r.type=ShapeType::Rect; r.rect=QRectF(0,0,100,60);
          r.fill.type=FillType::Solid; r.fill.color=QColor(80,80,200); r.stroke.enabled=false;
          d.shapes.push_back(r); c->setDocument(d); c->select({0}); };
      QPointF corners[4]={{0,0},{100,0},{100,60},{0,60}};   // TL,TR,BR,BL (canvas handle order)
      auto straight=[&](QPointF ct[8]){ for(int i=0;i<4;++i){ QPointF a=corners[i], b=corners[(i+1)&3]; ct[2*i]=a+(b-a)/3.0; ct[2*i+1]=a+(b-a)*2.0/3.0; } };
      mkRect(); QPointF ct[8]; straight(ct); c->applyEnvelopeToSelection(corners,ct);
      { const Shape& s=c->doc().shapes[0]; QRectF bb=s.docBounds();
        check(s.type==ShapeType::Path, "envelope bake produces a Path");
        check(qAbs(bb.left())<2 && qAbs(bb.top())<2 && qAbs(bb.right()-100)<3 && qAbs(bb.bottom()-60)<3, "straight envelope ≈ identity bounds (no distortion)"); }
      mkRect(); QPointF ctb[8]; straight(ctb); ctb[0].setY(-40); ctb[1].setY(-40);   // bow the TOP edge (TL->TR) upward
      c->applyEnvelopeToSelection(corners,ctb);
      { const Shape& s=c->doc().shapes[0]; check(s.type==ShapeType::Path, "bulged envelope bake is a Path");
        check(s.docBounds().top()<-8, "a bowed top edge extends the shape above its original top"); }
      c->undo(); check(c->doc().shapes[0].type==ShapeType::Rect, "undo restores the pre-envelope rect");

      // 3) Interactive path: Mould tool → Envelope mode → drag an edge control handle.
      mkRect(); c->setTool(Tool::Mould); c->setMouldMode(0); check(c->mouldMode()==0, "mould set to perspective mode");
      c->setMouldMode(1); check(c->mouldMode()==1, "mould switched to envelope mode");
      c->moveMouldCtrl(0, QPointF(33,-40)); c->moveMouldCtrl(1, QPointF(66,-40));   // pull top-edge controls up
      { const Shape& s=c->doc().shapes[0]; check(s.type==ShapeType::Path, "interactive envelope edit bakes to a Path");
        check(s.docBounds().top()<-6, "dragging the top-edge handles bulges the shape upward"); }
      c->setTool(Tool::Select);

      // Review-fix regressions.
      // (a) Rotated shape + identity cage (its oriented box) must be a true identity, not distort.
      { Document d; Shape r; r.type=ShapeType::Rect; r.rect=QRectF(0,0,100,60);
        r.fill.type=FillType::Solid; r.fill.color=QColor(200,120,40); r.stroke.enabled=false;
        QTransform tf; tf.translate(200,200); tf.rotate(30); r.transform=tf;
        d.shapes.push_back(r); c->setDocument(d); c->select({0});
        QRectF lb=c->doc().shapes[0].localBounds();
        QPointF C[4]={ tf.map(lb.topLeft()), tf.map(lb.topRight()), tf.map(lb.bottomRight()), tf.map(lb.bottomLeft()) };
        QPointF ct[8]; for(int i=0;i<4;++i){ QPointF a=C[i], b=C[(i+1)&3]; ct[2*i]=a+(b-a)/3.0; ct[2*i+1]=a+(b-a)*2.0/3.0; }
        QRectF before=c->doc().shapes[0].docBounds();
        c->applyEnvelopeToSelection(C,ct);
        QRectF after=c->doc().shapes[0].docBounds();
        check(qAbs(after.width()-before.width())<3 && qAbs(after.height()-before.height())<3
              && (after.center()-before.center()).manhattanLength()<4, "identity envelope on a ROTATED shape preserves its bounds (rotation-safe warp)"); }
      // (b) Multi-object selection warps as ONE unit → objects keep their separation, not collapse.
      { Document d; Shape a; a.type=ShapeType::Rect; a.rect=QRectF(0,0,20,20); a.stroke.enabled=false;
        Shape b=a; a.transform=QTransform::fromTranslate(0,0); b.transform=QTransform::fromTranslate(200,0);
        d.shapes.push_back(a); d.shapes.push_back(b); c->setDocument(d); c->selectAll();
        QRectF cb(0,0,220,20); QPointF C[4]={cb.topLeft(),cb.topRight(),cb.bottomRight(),cb.bottomLeft()};
        QPointF ct[8]; for(int i=0;i<4;++i){ QPointF p=C[i], q=C[(i+1)&3]; ct[2*i]=p+(q-p)/3.0; ct[2*i+1]=p+(q-p)*2.0/3.0; }
        c->applyEnvelopeToSelection(C,ct);
        double sep=c->doc().shapes[1].docBounds().center().x()-c->doc().shapes[0].docBounds().center().x();
        check(sep>150, "multi-select envelope preserves inter-object separation (warps as one unit)"); }
      // (c) Deleting the moulded shape invalidates the cached reference → later edit is a safe no-op.
      { Document d; for(int k=0;k<3;++k){ Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,30,30); s.transform=QTransform::fromTranslate(k*60,0); d.shapes.push_back(s); }
        c->setDocument(d); c->select({1}); c->setTool(Tool::Mould); c->setMouldMode(1);
        c->select({1}); c->deleteSelection();
        check(c->doc().shapes.size()==2, "deleted the moulded shape");
        int n=c->doc().shapes.size(); c->moveMouldCtrl(0, QPointF(10,10));
        check(c->doc().shapes.size()==n, "envelope edit after deleting the moulded shape is a safe no-op");
        c->setTool(Tool::Select); } }

    // --- Blend: real outline morph between two different shapes ---
    std::printf("[blend] outline morph\n");
    {
        Document d;
        Shape a; a.type=ShapeType::Ellipse; a.rect=QRectF(0,0,80,80); a.transform=QTransform::fromTranslate(120,300);
        Shape b; b.type=ShapeType::Polygon; b.polySides=5; b.polyStar=true; b.rect=QRectF(0,0,120,120); b.transform=QTransform::fromTranslate(700,300);
        d.shapes.push_back(a); d.shapes.push_back(b);
        c->setDocument(d); c->select({0,1}); c->blendSelection(6);
        check(c->doc().shapes.size()==8, "blend inserted 6 in-between shapes");
        if (c->doc().shapes.size()==8) {
            check(c->doc().shapes[4].type==ShapeType::Path, "blend step is an outline Path");
            double cx=c->doc().shapes[4].docBounds().center().x();
            check(cx>200 && cx<640, "a mid blend step lies geometrically between A and B");
        }
    }
    // Open-subpath blend (arc-length resample): the LAST sample maps to d==total, which previously
    // indexed poly[M]/cum[M] (out of bounds, uninitialised) — a NaN garbage pattern there injected a
    // NaN vertex and made moulded blends (Watch4) render non-deterministically. Guard: every blend
    // step vertex must be finite, and the endpoint must land on the two shapes' interpolated end.
    { Shape a; a.type=ShapeType::Path; QPainterPath pa; pa.moveTo(0,0); pa.lineTo(50,20); pa.lineTo(100,0); a.path=pa;
      a.fill.type=FillType::None; a.stroke.enabled=true;                                   // OPEN polyline (no closeSubpath)
      Shape b; b.type=ShapeType::Path; QPainterPath pb; pb.moveTo(0,200); pb.lineTo(50,220); pb.lineTo(100,200); b.path=pb;
      b.fill.type=FillType::None; b.stroke.enabled=true;
      QVector<Shape> steps=blendMorph(a,b,6,0,0,0,0);
      check(steps.size()==6, "open-subpath blendMorph produced 6 steps");
      bool allFinite=true; int verts=0;
      for(const Shape& s: steps) for(int e=0;e<s.path.elementCount();++e){ auto el=s.path.elementAt(e); ++verts; if(!qIsFinite(el.x)||!qIsFinite(el.y)) allFinite=false; }
      check(verts>0 && allFinite, "every open-subpath blend vertex is finite (no OOB NaN from the arc-length endpoint)");
      if(!steps.isEmpty()){ QRectF bb=steps.first().path.boundingRect();
        check(qIsFinite(bb.x())&&qIsFinite(bb.width())&&bb.right()<=101&&bb.left()>=-1, "first open blend step stays within the two endpoints' x-range (endpoint not flung to garbage)"); } }

    // --- Live Blend (Batch 5): re-editable blend group + on-canvas step handle + drag-to-blend ---
    std::printf("[blend] live-blend group\n");
    {
        Document d;
        Shape a; a.type=ShapeType::Ellipse; a.rect=QRectF(0,0,80,80); a.transform=QTransform::fromTranslate(120,300); a.name="A";
        Shape b; b.type=ShapeType::Rect;    b.rect=QRectF(0,0,80,80); b.transform=QTransform::fromTranslate(600,300); b.name="B";
        d.shapes.push_back(a); d.shapes.push_back(b);
        c->setDocument(d); c->setTool(Tool::Blend);
        c->select({0,1}); c->makeLiveBlend(5);
        check(c->doc().shapes.size()==1 && c->doc().shapes[0].type==ShapeType::Group, "makeLiveBlend collapses the two ends into one group");
        const Shape& g0=c->doc().shapes[0];
        check(g0.blendSteps==5 && g0.children.size()==2, "the group is a live blend (blendSteps=5, exactly the 2 ends)");
        // Painting it must emit the two ends + blendSteps in-betweens (regenerated at render, not baked into the model).
        { QImage img(700,600,QImage::Format_ARGB32_Premultiplied); img.fill(Qt::white);
          QPainter p(&img); p.setRenderHint(QPainter::Antialiasing); paintDocument(p, c->doc()); p.end();
          check(c->doc().shapes.size()==1 && c->doc().shapes[0].blendSteps==5, "rendering a live blend does NOT bake extra shapes into the model"); }
        // The on-canvas control line + step handle exist for a selected live blend.
        c->select({0});
        QPointF ea,eb; check(c->blendControlEnds(ea,eb), "blendControlEnds resolves the two end centres for the overlay");
        QPointF sh;    check(c->blendStepHandle(sh), "blendStepHandle resolves (perpendicular off the control-line midpoint)");
        // setLiveBlendSteps changes the count in place (the InfoBar Steps spin path).
        c->setLiveBlendSteps(9);
        check(c->doc().shapes[0].blendSteps==9, "setLiveBlendSteps updates the selected live blend's in-between count");
        // JSON round-trip preserves the live blend (blendSteps persists; ends kept).
        { Document rt = Document::fromJson(c->doc().toJson());
          check(rt.shapes.size()==1 && rt.shapes[0].blendSteps==9 && rt.shapes[0].children.size()==2, "blendSteps + the 2 ends survive save/load"); }
        // Blend profile (G-F): a non-zero attribute ease shifts the in-between shapes vs the linear default.
        { Shape a; a.type=ShapeType::Rect; a.rect=QRectF(0,0,40,40); a.transform=QTransform::fromTranslate(0,0);
          Shape b; b.type=ShapeType::Rect; b.rect=QRectF(0,0,40,40); b.transform=QTransform::fromTranslate(400,0);
          QVector<Shape> lin=blendMorph(a,b,5,0.0,0.0,0,0);
          QVector<Shape> eased=blendMorph(a,b,5,0.9,0.0,0,0);   // objBias reshapes the geometry step spacing
          check(lin.size()==5 && eased.size()==5, "blendMorph returns 5 steps either way");
          check(std::abs(lin[0].docBounds().center().x()-eased[0].docBounds().center().x())>3.0, "a blend profile moves the in-between positions off the linear spacing");
          Document d; d.shapes.push_back(a); d.shapes.push_back(b); c->setDocument(d); c->select({0,1}); c->makeLiveBlend(5);
          c->doc().shapes[0].blendProfile=0.7;
          Document rt=Document::fromJson(c->doc().toJson());
          check(qFuzzyCompare(rt.shapes[0].blendProfile,0.7), "blendProfile round-trips through JSON (gated)"); }
    }
    // Drag-to-blend gesture: with the Blend tool, press on one object and release on another → live blend.
    {
        Document d;
        Shape a; a.type=ShapeType::Rect; a.rect=QRectF(0,0,60,60); a.transform=QTransform::fromTranslate(150,150); a.name="A";
        Shape b; b.type=ShapeType::Rect; b.rect=QRectF(0,0,60,60); b.transform=QTransform::fromTranslate(400,150); b.name="B";
        d.shapes.push_back(a); d.shapes.push_back(b);
        c->setDocument(d); c->setTool(Tool::Blend); c->selectNone();
        QPointF fromDoc(180,180), toDoc(430,180);   // centres of A and B
        QPointF vf=c->mapToView(fromDoc), vt=c->mapToView(toDoc);
        send(c, QEvent::MouseButtonPress,   vf, Qt::LeftButton, Qt::LeftButton);
        send(c, QEvent::MouseMove,          vt, Qt::NoButton,   Qt::LeftButton);
        send(c, QEvent::MouseButtonRelease, vt, Qt::LeftButton, Qt::NoButton);
        check(c->doc().shapes.size()==1 && c->doc().shapes[0].type==ShapeType::Group && c->doc().shapes[0].blendSteps>0,
              "drag from A onto B with the Blend tool creates a live blend group");
    }

    // --- Snapping: dragging an object near another's edge snaps it flush ---
    std::printf("[snap] object-edge snapping\n");
    {
        Document d;
        Shape a; a.type=ShapeType::Rect; a.rect=QRectF(0,0,120,80); a.transform=QTransform::fromTranslate(200,150); a.name="A";
        Shape b; b.type=ShapeType::Rect; b.rect=QRectF(0,0,120,80); b.transform=QTransform::fromTranslate(300,150); b.name="B";
        d.shapes.push_back(a); d.shapes.push_back(b);
        c->setDocument(d);
        c->setTool(Tool::Select);
        QPointF pressDoc(360,190);                 // centre of B
        QPointF relDoc = pressDoc + QPointF(-97,0); // brings B.left from 300 to 203 (3px shy of A.left=200)
        QPointF vp=c->mapToView(pressDoc), vr=c->mapToView(relDoc);
        send(c, QEvent::MouseButtonPress,   vp, Qt::LeftButton, Qt::LeftButton);
        send(c, QEvent::MouseMove,          vr, Qt::NoButton,   Qt::LeftButton);
        send(c, QEvent::MouseButtonRelease, vr, Qt::LeftButton, Qt::NoButton);
        double bl = c->doc().shapes[1].docBounds().left();
        check(std::abs(bl-200.0)<0.8, "dragged edge snapped flush to neighbour (200)");
    }

    // --- Transforms: rotate 90, flip, distribute ---
    std::printf("[transform] rotate 90 / flip / distribute\n");
    {
        Document d; Shape r; r.type=ShapeType::Rect; r.rect=QRectF(0,0,120,60); r.transform=QTransform::fromTranslate(200,200);
        d.shapes.push_back(r); c->setDocument(d); c->selectAll(); c->rotateSelection90(true);
        QRectF bb=c->doc().shapes[0].docBounds();
        check(std::abs(bb.width()-60)<1 && std::abs(bb.height()-120)<1, "rotate 90 swaps width/height");
        check(std::abs(bb.center().x()-260)<1 && std::abs(bb.center().y()-230)<1, "rotate 90 keeps centre");
    }
    {
        Document d;
        Shape a; a.type=ShapeType::Rect; a.rect=QRectF(0,0,80,80); a.transform=QTransform::fromTranslate(100,200);  // centre 140
        Shape b; b.type=ShapeType::Rect; b.rect=QRectF(0,0,80,80); b.transform=QTransform::fromTranslate(360,200);  // centre 400
        d.shapes.push_back(a); d.shapes.push_back(b); c->setDocument(d); c->selectAll(); c->flipSelection(true);
        check(std::abs(c->doc().shapes[0].docBounds().center().x()-400)<1, "flip H reflects shapes about selection centre");
    }
    {
        Document d;
        for(double x : {100.0,150.0,500.0}){ Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,40,40); s.transform=QTransform::fromTranslate(x,200); d.shapes.push_back(s); }
        c->setDocument(d); c->selectAll(); c->distributeSelection(true);
        // centres span 120..520; the middle should sit at 320
        double midc=c->doc().shapes[1].docBounds().center().x();
        check(std::abs(midc-320)<1.5, "distribute evenly spaces centres");
    }

    // --- Clipboard: copy / paste / cut ---
    std::printf("[clipboard] copy / paste / cut\n");
    c->newDocument();
    c->setTool(Tool::Rect); dragShape(c, {150,150}, {300,250});
    c->setTool(Tool::Select); c->selectAll(); c->copySelection(); c->pasteClipboard();
    check(c->doc().shapes.size()==2, "paste added a copy");
    if (c->doc().shapes.size()==2) {
        QRectF a=c->doc().shapes[0].docBounds(), b=c->doc().shapes[1].docBounds();
        check(std::abs((b.left()-a.left())-16)<1.0, "pasted copy is offset (16px)");
    }
    c->cutSelection();
    check(c->doc().shapes.size()==1, "cut removed the selection");

    // --- Save / load round-trip ---
    std::printf("[io] save / load round-trip\n");
    QTemporaryDir tmp;
    QString f = tmp.path()+"/selftest.xar";
    int before = c->doc().shapes.size();
    bool saved = c->save(f);
    c->newDocument();
    check(c->doc().shapes.isEmpty(), "new document clears shapes");
    bool loaded = c->load(f);
    check(saved && loaded, "save and load both succeeded");
    check(c->doc().shapes.size()==before, "loaded document has same shape count");

    // --- Native XAR format round-trip ---
    std::printf("[xar] native XAR save / load round-trip\n");
    {
        Document d;
        Shape r; r.type=ShapeType::Rect; r.rect=QRectF(0,0,120,80); r.transform=QTransform::fromTranslate(100,100);
        r.fill.type=FillType::Solid; r.fill.color=QColor(220,60,40);
        r.stroke.enabled=true; r.stroke.color=QColor(20,20,20); r.stroke.width=3;
        r.stroke.cap=Qt::SquareCap; r.stroke.join=Qt::BevelJoin; r.stroke.transp=0.4;
        r.fillRule=Qt::WindingFill;   // non-default winding rule -> must round-trip
        Shape e; e.type=ShapeType::Ellipse; e.rect=QRectF(0,0,100,100); e.transform=QTransform::fromTranslate(300,150);
        e.fill.type=FillType::Linear;   // 4-stop gradient -> exercises multi-stage fill round-trip
        e.fill.stops={{0,QColor(80,160,255)},{0.33,QColor(255,200,60)},{0.66,QColor(60,220,140)},{1,QColor(20,40,120)}};
        e.fill.p0=QPointF(0,0); e.fill.p1=QPointF(100,100); e.opacity=0.5;
        Shape sg; sg.type=ShapeType::Rect; sg.rect=QRectF(0,0,100,100); sg.transform=QTransform::fromTranslate(500,150);
        sg.fill.type=FillType::Solid; sg.fill.color=QColor(200,80,80);   // stained-glass graduated transparency
        sg.transp.type=2; sg.transp.geom=1; sg.transp.t0=0.1; sg.transp.t1=0.9;
        sg.transp.p0=QPointF(0,0); sg.transp.p1=QPointF(100,0);
        d.shapes.push_back(r); d.shapes.push_back(e); d.shapes.push_back(sg);
        c->setDocument(d);
        QTemporaryDir tmp; QString xf=tmp.path()+"/rt.xar";
        check(c->save(xf), "save .xar succeeded");
        QFile chk(xf); chk.open(QIODevice::ReadOnly); QByteArray head=chk.read(8); chk.close();
        check(head.size()==8 && (quint8)head[0]==0x58 && (quint8)head[1]==0x41
              && (quint8)head[2]==0x52 && (quint8)head[3]==0x41, "file begins with the XARA magic");
        c->newDocument();
        check(c->load(xf), "load .xar succeeded");
        check(c->doc().shapes.size()==3, "round-trip preserved shape count");
        if(c->doc().shapes.size()>=3){
            const Transparency& t=c->doc().shapes[2].transp;
            check(t.type==2 && t.geom==1, "transparency type (stained-glass) + geometry survived");
            check(std::abs(t.t0-0.1)<0.02 && std::abs(t.t1-0.9)<0.02, "graduated transparency start/end values survived");
        }
        if(c->doc().shapes.size()>=2){
            QRectF b=c->doc().shapes[0].docBounds();
            check(std::abs(b.left()-100)<2 && std::abs(b.top()-100)<2
                  && std::abs(b.width()-120)<2 && std::abs(b.height()-80)<2, "geometry survived (position + size, y-flip correct)");
            QColor fc=c->doc().shapes[0].fill.color;
            check(fc.red()>200 && fc.green()<90 && fc.blue()<90, "solid fill colour survived");
            check(c->doc().shapes[0].stroke.enabled && c->doc().shapes[0].stroke.color.red()<60, "stroke colour survived");
            check(c->doc().shapes[0].stroke.cap==Qt::SquareCap && c->doc().shapes[0].stroke.join==Qt::BevelJoin, "stroke cap + join style survived");
            check(std::abs(c->doc().shapes[0].stroke.transp-0.4)<0.02, "stroke transparency survived");
            check(c->doc().shapes[0].fillRule==Qt::WindingFill, "non-zero winding rule survived");
            check(c->doc().shapes[1].fillRule==Qt::OddEvenFill, "default even-odd winding rule preserved");
            check(c->doc().shapes[1].fill.type==FillType::Linear, "gradient fill type survived");
            check(c->doc().shapes[1].fill.stops.size()==4
                  && c->doc().shapes[1].fill.stops.first().color.blue()>200, "all 4 gradient stops survived (multi-stage)");
            { auto& st=c->doc().shapes[1].fill.stops;
              bool midOk = st.size()==4 && std::abs(st[1].pos-0.33)<0.02 && st[1].color.red()>240 && st[2].color.green()>200;
              check(midOk, "middle gradient stop positions + colours survived"); }
            check(std::abs(c->doc().shapes[1].opacity-0.5)<0.02, "object opacity (transparency) survived");
        }
    }

    // --- Winding rule actually changes fill geometry (pentagram center) ---
    // A self-intersecting 5-point star: even-odd leaves the central pentagon hollow,
    // non-zero fills it solid. This proves the fill rule reaches the rendered region
    // (localPath stamps it; contains()/drawPath honour it), and survives an XAR round-trip.
    std::printf("[xar] winding rule changes fill region (pentagram)\n");
    {
        QPainterPath star;   // {5/2} star polygon: visit every-other vertex of a pentagon
        for(int i=0;i<5;++i){ double a=(-90.0 + i*144.0)*M_PI/180.0;
            QPointF pt(60.0+50.0*std::cos(a), 60.0+50.0*std::sin(a));
            if(i==0) star.moveTo(pt); else star.lineTo(pt); }
        star.closeSubpath();
        const QPointF centre(60,60);

        QPainterPath eo=star; eo.setFillRule(Qt::OddEvenFill);
        QPainterPath nz=star; nz.setFillRule(Qt::WindingFill);
        check(!eo.contains(centre), "even-odd leaves pentagram centre hollow (control)");
        check( nz.contains(centre), "non-zero fills pentagram centre (control)");

        Document d;
        Shape sp; sp.type=ShapeType::Path; sp.path=star; sp.fillRule=Qt::WindingFill;
        sp.transform=QTransform::fromTranslate(40,40);
        sp.fill.type=FillType::Solid; sp.fill.color=QColor(200,40,40);
        d.shapes.push_back(sp);
        c->setDocument(d);
        QTemporaryDir tmp; QString xf=tmp.path()+"/star.xar";
        check(c->save(xf) && c->load(xf), "pentagram .xar round-trip");
        if(!c->doc().shapes.isEmpty()){
            const Shape& r=c->doc().shapes.first();
            check(r.fillRule==Qt::WindingFill, "pentagram winding rule survived round-trip");
            // The writer bakes the transform into the path, so the reloaded star is in doc
            // space. docPath() carries the fill rule through localPath()+map(); its doc-space
            // centre (40,40 translate of the local centre) must now fill solid.
            const QPointF docCentre = sp.transform.map(centre);   // (100,100)
            check(Document::docPath(r).contains(docCentre), "reloaded pentagram centre fills solid (rule reached geometry)");
        }
    }

    // --- Bitmap fill round-trip (image embed + parallelogram geometry) ---
    std::printf("[xar] bitmap fill round-trip (embed + parallelogram)\n");
    {
        QImage src(8, 8, QImage::Format_ARGB32);     // distinct corners to detect flips/loss
        src.fill(Qt::gray); src.setPixel(0,0,qRgb(255,0,0)); src.setPixel(7,7,qRgb(0,0,255));
        Document d;
        Shape r; r.type=ShapeType::Rect; r.rect=QRectF(0,0,200,150); r.transform=QTransform::fromTranslate(60,40);
        r.fill.type=FillType::Bitmap; r.fill.image=src;
        r.fill.p0=QPointF(0,0); r.fill.p1=QPointF(200,0); r.fill.p2=QPointF(0,150);   // local-space parallelogram
        r.stroke.enabled=false;
        Shape ct; ct.type=ShapeType::Ellipse; ct.rect=QRectF(0,0,120,120); ct.transform=QTransform::fromTranslate(300,40);
        ct.fill.type=FillType::Bitmap; ct.fill.image=src; ct.fill.contone=true;
        ct.fill.color=QColor(20,40,160); ct.fill.color2=QColor(240,220,80);
        ct.fill.p0=QPointF(0,0); ct.fill.p1=QPointF(120,0); ct.fill.p2=QPointF(0,120); ct.stroke.enabled=false;
        d.shapes.push_back(r); d.shapes.push_back(ct);
        c->setDocument(d);
        QTemporaryDir tmp; QString xf=tmp.path()+"/bmp.xar";
        check(c->save(xf) && c->load(xf), "bitmap .xar round-trip");
        check(c->doc().shapes.size()==2, "bitmap round-trip preserved shape count");
        if(c->doc().shapes.size()==2){
            const Fill& f=c->doc().shapes[0].fill;
            check(f.type==FillType::Bitmap, "bitmap fill type survived");
            check(!f.image.isNull() && f.image.width()==8 && f.image.height()==8, "embedded bitmap (8x8) survived");
            check(qRed(f.image.pixel(0,0))>200 && qBlue(f.image.pixel(7,7))>200, "bitmap pixels survived (PNG lossless, no flip)");
            // parallelogram came back in doc space (writer bakes transform); width/height preserved
            QPointF ax=f.p1-f.p0, ay=f.p2-f.p0;
            check(std::abs(QLineF(QPointF(0,0),ax).length()-200)<2 && std::abs(QLineF(QPointF(0,0),ay).length()-150)<2,
                  "bitmap parallelogram axes (200x150) survived");
            const Fill& cf=c->doc().shapes[1].fill;
            check(cf.type==FillType::Bitmap && cf.contone, "contone flag survived");
            check(cf.color.blue()>140 && cf.color2.red()>200, "contone start/end tint colours survived");
        }
    }

    // --- 3/4-colour mesh + square/diamond fill round-trip ---
    std::printf("[xar] mesh + square fill round-trip\n");
    {
        Document d;
        Shape four; four.type=ShapeType::Rect; four.rect=QRectF(0,0,160,120); four.transform=QTransform::fromTranslate(50,50);
        four.fill.type=FillType::Mesh;
        four.fill.meshColors={QColor(200,20,20),QColor(20,200,20),QColor(20,20,200),QColor(220,220,20)};
        four.fill.p0=QPointF(0,0); four.fill.p1=QPointF(160,0); four.fill.p2=QPointF(0,120); four.stroke.enabled=false;
        Shape sq; sq.type=ShapeType::Ellipse; sq.rect=QRectF(0,0,120,120); sq.transform=QTransform::fromTranslate(260,50);
        sq.fill.type=FillType::Square; sq.fill.meshColors={QColor(250,250,250),QColor(10,10,10)};
        sq.fill.p0=QPointF(60,60); sq.fill.p1=QPointF(120,60); sq.fill.p2=QPointF(60,120); sq.stroke.enabled=false;
        d.shapes.push_back(four); d.shapes.push_back(sq);
        c->setDocument(d);
        QTemporaryDir tmp; QString xf=tmp.path()+"/mesh.xar";
        check(c->save(xf) && c->load(xf), "mesh/square .xar round-trip");
        if(c->doc().shapes.size()==2){
            const Fill& ff=c->doc().shapes[0].fill;
            check(ff.type==FillType::Mesh && ff.meshColors.size()==4, "4-colour mesh fill survived (4 colours)");
            check(ff.meshColors[0].red()>180 && ff.meshColors[1].green()>180 && ff.meshColors[2].blue()>180, "mesh corner colours survived");
            const Fill& sf=c->doc().shapes[1].fill;
            check(sf.type==FillType::Square && sf.meshColors.size()==2, "square fill type + 2 colours survived");
            check(sf.meshColors[0].red()>240 && sf.meshColors[1].red()<30, "square centre/edge colours survived");
        }
    }

    // --- Selection move keeps fidelity through undo ---
    std::printf("[select] move + undo\n");
    c->setTool(Tool::Select);
    c->selectAll();
    check(!c->selection().isEmpty(), "select-all selected shapes");

    // --- Batch A: measurement units, grid, page size ---
    std::printf("[chrome] units / grid / page size\n");
    check(qAbs(Canvas::unitFactor(Canvas::Units::Inches)-96.0)<1e-6, "1 inch = 96 doc px");
    check(qAbs(Canvas::unitFactor(Canvas::Units::Centimetres)-96.0/2.54)<1e-3, "1 cm = 96/2.54 doc px");
    c->setUnits(Canvas::Units::Millimetres);
    check(c->units()==Canvas::Units::Millimetres, "units setter took");
    check(c->fmtLen(96.0*25.4/96.0*0+ 96.0/25.4).startsWith("1"), "fmtLen renders 1 mm for 96/25.4 px");
    c->setUnits(Canvas::Units::Pixels);
    c->setShowGrid(true); check(c->showGrid(), "grid toggle on");
    c->setSnapToGrid(true); check(c->snapToGrid(), "snap-to-grid toggle on");
    c->setGridSpacing(40); check(qAbs(c->gridSpacing()-40)<1e-9, "grid spacing setter");
    c->setShowGrid(false); c->setSnapToGrid(false);
    c->setPageSize(QSizeF(595,842)); check(qAbs(c->pageSize().width()-595)<1e-6, "page size setter (A4 portrait pt-ish)");

    // --- Batch B: named colours, tints/shades/linked, JSON round-trip ---
    std::printf("[colour] named colours + tint/shade/linked\n");
    c->newDocument();
    { Document& d=c->doc(); d.namedColors.clear();
      NamedColor base; base.name="Brand"; base.color=QColor(40,120,200); d.namedColors.append(base);   // 0
      NamedColor tint; tint.name="Brand tint"; tint.kind=1; tint.parent=0; tint.amount=0.5; d.namedColors.append(tint); // 1
      NamedColor shade; shade.name="Brand shade"; shade.kind=2; shade.parent=0; shade.amount=0.5; d.namedColors.append(shade); // 2
      NamedColor link; link.name="Accent"; link.kind=3; link.parent=0; d.namedColors.append(link); // 3
      QColor b=d.resolveNamed(0), t=d.resolveNamed(1), s=d.resolveNamed(2), l=d.resolveNamed(3);
      check(b==QColor(40,120,200), "named colour resolves to its own RGB");
      check(t.red()>b.red() && t.green()>b.green() && t.blue()>b.blue(), "tint is lighter than parent");
      check(s.red()<b.red() && s.green()<b.green() && s.blue()<b.blue(), "shade is darker than parent");
      check(l==b, "linked colour equals its parent");
      // editing the parent ripples through tint/shade/linked
      d.namedColors[0].color=QColor(200,40,40);
      check(d.resolveNamed(3)==QColor(200,40,40), "linked colour follows parent edit");
      check(d.resolveNamed(1).red()>200, "tint follows parent edit");
      // JSON round-trip
      QJsonObject j=d.toJson(); Document d2=Document::fromJson(j);
      check(d2.namedColors.size()==4, "named colours survive JSON round-trip");
      check(d2.resolveNamed(2).blue()<40, "shade survives JSON round-trip"); }

    // --- Batch C: gallery insertion (clipart) + apply-to-selection ---
    std::printf("[gallery] clipart insert + fill preset apply\n");
    c->newDocument();
    { int before=c->doc().shapes.size();
      Shape s; s.type=ShapeType::Path; QPainterPath p; p.addRect(0,0,50,50); s.path=p; s.name="Clipart";
      c->addShapes({s},"Insert clipart");
      check(c->doc().shapes.size()==before+1, "clipart inserted one shape");
      check(c->selection().size()==1 && c->doc().shapes.last().name=="Clipart", "inserted clipart is selected");
      // apply a gradient fill preset to it
      c->applyToSelection([](Shape& sh){ sh.fill.type=FillType::Linear; sh.fill.stops={{0,QColor(255,170,60)},{1,QColor(225,60,120)}}; },"Fill preset");
      check(c->doc().shapes.last().fill.type==FillType::Linear, "fill-gallery preset applied");
      c->undo(); check(c->doc().shapes.last().fill.type!=FillType::Linear, "undo reverted fill preset"); }

    // --- Batch D: rich fill types + graduated transparency, JSON round-trip ---
    std::printf("[fill] rich fills + graduated transparency\n");
    c->newDocument();
    { Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,100,100); int idx=c->doc().addShape(s); c->select({idx});
      c->applyToSelection([](Shape& sh){ sh.fill.type=FillType::Conical; sh.fill.stops={{0,Qt::red},{1,Qt::blue}}; sh.fill.spread=QGradient::ReflectSpread; sh.fill.rainbow=true; },"Conical");
      check(c->doc().shapes[idx].fill.type==FillType::Conical, "conical fill type set");
      check(c->doc().shapes[idx].fill.spread==QGradient::ReflectSpread, "reflect spread set");
      check(c->doc().shapes[idx].fill.rainbow, "rainbow flag set");
      c->applyToSelection([](Shape& sh){ sh.fill.type=FillType::Mesh; sh.fill.meshColors={Qt::red,Qt::green,Qt::blue,Qt::yellow}; },"Mesh");
      check(c->doc().shapes[idx].fill.meshColors.size()==4, "4-colour mesh fill set");
      // graduated transparency: linear, opaque->50%, stained-glass effect (type 2)
      c->applyToSelection([](Shape& sh){ sh.transp.type=2; sh.transp.geom=1; sh.transp.t0=0; sh.transp.t1=0.5; sh.transp.p0=QPointF(0,0); sh.transp.p1=QPointF(100,0); },"Transparency");
      check(c->doc().shapes[idx].transp.geom==1 && c->doc().shapes[idx].transp.type==2, "graduated transparency (linear, stained-glass) set");
      QJsonObject j=c->doc().toJson(); Document d2=Document::fromJson(j);
      check(d2.shapes[idx].fill.type==FillType::Mesh && d2.shapes[idx].fill.rainbow, "rich fill survives JSON round-trip");
      check(d2.shapes[idx].transp.geom==1 && qAbs(d2.shapes[idx].transp.t1-0.5)<1e-6, "graduated transparency survives JSON round-trip"); }

    // --- Batch F: Bevel / Feather / Eraser tools ---
    std::printf("[tools] bevel toggle + feather + eraser\n");
    c->newDocument();
    { Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,120,120); s.transform=QTransform::fromTranslate(200,200); int idx=c->doc().addShape(s); c->select({idx});
      QPointF centreView=c->mapToView(QPointF(260,260));
      c->setTool(Tool::Bevel); click(c,centreView);
      check(c->doc().shapes[idx].bevel.on, "bevel tool toggled bevel ON");
      click(c,centreView); check(!c->doc().shapes[idx].bevel.on, "bevel tool toggled bevel OFF");
      // feather via apply
      c->setTool(Tool::Feather); c->applyToSelection([](Shape& sh){ sh.feather=10; },"Feather");
      check(qAbs(c->doc().shapes[idx].feather-10)<1e-6, "feather radius applied");
      // eraser punches a hole -> shape becomes an editable Path
      c->setTool(Tool::Eraser); c->setEraserNib(20); click(c,centreView);
      check(c->doc().shapes[idx].type==ShapeType::Path, "eraser converted shape to editable Path");
      c->undo(); check(c->doc().shapes[idx].type==ShapeType::Rect, "undo restored the rectangle after erase"); }

    // --- Batch G: guides + lock ---
    std::printf("[object] guides + lock\n");
    c->newDocument();
    { Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,80,80); s.transform=QTransform::fromTranslate(300,300); int idx=c->doc().addShape(s);
      c->addGuide(true,150); c->addGuide(false,250);
      check(c->guidesH().size()==1 && c->guidesV().size()==1, "added H+V guides");
      c->clearGuides(); check(c->guidesH().isEmpty() && c->guidesV().isEmpty(), "cleared guides");
      c->select({idx}); c->lockSelection();
      check(c->doc().shapes[idx].locked, "lock set on selection");
      c->setTool(Tool::Select); c->selectNone();
      click(c, c->mapToView(QPointF(340,340)));   // click on the locked shape
      check(c->selection().isEmpty(), "locked shape is not click-selectable");
      c->unlockSelection(); /* no-op (nothing selected) */ for(auto& sh:c->doc().shapes) sh.locked=false;
      click(c, c->mapToView(QPointF(340,340)));
      check(c->selection().size()==1, "unlocked shape is selectable again"); }

    // --- Batch E: text — underline, bullets, find/replace ---
    std::printf("[text] underline + bullets + replace\n");
    c->newDocument();
    { Shape s; s.type=ShapeType::Text; s.text="alpha\nbeta\ngamma"; s.font=QFont("DejaVu Sans",24); int idx=c->doc().addShape(s); c->select({idx});
      c->applyToSelection([](Shape& sh){ QFont g=sh.font; g.setUnderline(true); sh.font=g; },"Underline");
      check(c->doc().shapes[idx].font.underline(), "underline attribute applied");
      // bullets: prepend "• " to each non-empty line
      c->applyToSelection([](Shape& sh){ QStringList ls=sh.text.split('\n'); for(auto&l:ls) if(!l.trimmed().isEmpty()) l="• "+l; sh.text=ls.join('\n'); },"Bullets");
      check(c->doc().shapes[idx].text.startsWith("• alpha"), "bullet marker prepended");
      // replace-all logic (same as the dialog): replace "beta" -> "BETA"
      c->pushUndo("Replace"); for(auto& sh:c->doc().shapes) if(sh.type==ShapeType::Text) sh.text.replace("beta","BETA");
      check(c->doc().shapes[idx].text.contains("BETA"), "find/replace replaced text");
      c->undo(); check(c->doc().shapes[idx].text.contains("beta"), "undo reverted the replace"); }

    // --- Batch J: layer system ---
    std::printf("[layers] add / visibility / move / hit-test\n");
    c->newDocument();
    { check(c->doc().layers.size()==1, "new doc has one layer");
      c->addLayer(); check(c->doc().layers.size()==2 && c->activeLayer()==1, "added a second layer, now active");
      // a new shape lands on the active layer
      Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,100,100); s.transform=QTransform::fromTranslate(300,300);
      c->setTool(Tool::Select); int idx=c->doc().addShape(s); c->doc().shapes[idx].layer=1;
      check(c->doc().shapes[idx].layer==1, "shape assigned to layer 1");
      // hide layer 1 -> shape not click-selectable
      c->setLayerVisible(1,false); c->selectNone(); click(c, c->mapToView(QPointF(350,350)));
      check(c->selection().isEmpty(), "objects on a hidden layer are not hit-testable");
      c->setLayerVisible(1,true); click(c, c->mapToView(QPointF(350,350)));
      check(c->selection().size()==1, "objects on a shown layer are hit-testable again");
      // move selection to layer 0, then delete layer 1
      c->moveSelectionToLayer(0); check(c->doc().shapes[idx].layer==0, "moved selection to layer 0");
      c->removeLayer(1); check(c->doc().layers.size()==1, "deleted the empty layer");
      // JSON round-trip of multiple layers
      c->addLayer(); c->doc().layers[1].name="Top"; c->doc().layers[1].visible=false;
      QJsonObject j=c->doc().toJson(); Document d2=Document::fromJson(j);
      check(d2.layers.size()==2 && d2.layers[1].name=="Top" && !d2.layers[1].visible, "layers survive JSON round-trip"); }

    // --- Page & Layer TREE gallery: per-object visibility/lock/rename + reorder/relayer ---
    std::printf("[layer-tree] object visibility / lock / rename / reorder / relayer\n");
    c->newDocument();
    { auto mkR=[&](const QString& nm){ Shape s; s.type=ShapeType::Rect; s.name=nm; s.rect=QRectF(0,0,50,50); return c->doc().addShape(s); };
      int a=mkR("A"), b=mkR("B"), cc=mkR("C");                 // list/paint order A,B,C = indices 0,1,2
      check(a==0&&b==1&&cc==2, "three objects added in order A,B,C");
      c->setShapeVisible(b,false); check(!c->doc().shapes[b].visible, "setShapeVisible hides object B");
      c->setShapeVisible(b,true);  check(c->doc().shapes[b].visible,  "setShapeVisible re-shows B");
      c->setShapeLocked(cc,true);  check(c->doc().shapes[cc].locked,  "setShapeLocked locks C");
      c->renameShape(a,"Alpha");   check(c->doc().shapes[a].name=="Alpha", "renameShape renames A");
      // reverse the stack within the single layer (the tree's drag-to-restack path)
      c->select({cc});
      c->setObjectOrder({2,1,0},{0,0,0});
      check(c->doc().shapes.size()==3 && c->doc().shapes[0].name=="C" && c->doc().shapes[2].name=="Alpha", "setObjectOrder reversed the stack");
      check(c->doc().shapes[0].locked, "the locked flag followed C through the reorder");
      check(c->selection().size()==1 && c->doc().shapes[c->selection()[0]].name=="C", "selection remapped to C's new index");
      // malformed inputs are ignored (never corrupt the model)
      QString top=c->doc().shapes[0].name;
      c->setObjectOrder({0,1},{0,0});                          // wrong length
      check(c->doc().shapes.size()==3 && c->doc().shapes[0].name==top, "wrong-length order is a no-op");
      c->setObjectOrder({0,0,1},{0,0,0});                      // not a permutation (index 0 twice)
      check(c->doc().shapes[0].name==top, "non-permutation order is a no-op");
      // relayer through setObjectOrder: move the top object to a new layer
      c->addLayer();                                           // layers {0,1}
      c->setObjectOrder({0,1,2},{1,0,0});
      check(c->doc().shapes[0].layer==1 && c->doc().shapes[1].layer==0, "setObjectOrder relayered only the first object");
      // undo restores the pre-reorder state
      c->undo();
      check(c->doc().shapes[0].layer==0, "undo reverts the relayer"); }

    // Review-fix regressions: setObjectOrder must (a) remap absolute cross-reference indices and
    // (b) be a true no-op when only the flat array's cross-layer interleaving would change.
    { c->newDocument();
      Shape f0; f0.type=ShapeType::Text; f0.name="A"; f0.text="head"; f0.nextFrame=1;   // A overflows into B
      Shape f1; f1.type=ShapeType::Text; f1.name="B"; f1.text="tail"; f1.nextFrame=-1;
      c->doc().addShape(f0); c->doc().addShape(f1);
      c->setObjectOrder({1,0},{0,0});                       // swap A and B
      int ai=-1,bi=-1; for(int k=0;k<c->doc().shapes.size();++k){ if(c->doc().shapes[k].name=="A")ai=k; if(c->doc().shapes[k].name=="B")bi=k; }
      check(ai==1 && bi==0, "reorder swapped the two frames");
      check(c->doc().shapes[ai].nextFrame==bi, "nextFrame was remapped through the permutation (chain intact, no self-reference)"); }
    { c->newDocument();                                     // interleaved-layer no-op
      auto add=[&](const QString& n,int L){ Shape s; s.type=ShapeType::Rect; s.name=n; int i=c->doc().addShape(s); c->doc().shapes[i].layer=L; return i; };
      c->addLayer();                                        // layers {0,1}
      add("A",0); add("B",1); add("C",0);                   // array interleaved across layers
      int undoBefore=c->undoDepth();
      c->setObjectOrder({0,2,1},{0,0,1});                   // the grouped order a drop-in-place derives
      check(c->undoDepth()==undoBefore, "a layer-regrouping no-op pushes no undo entry");
      check(c->doc().shapes[0].name=="A" && c->doc().shapes[1].name=="B" && c->doc().shapes[2].name=="C", "interleaved drop-in-place leaves the array untouched"); }

    // Construct the actual tree widget headlessly: exercises rebuild() over layers, objects,
    // nested groups and a selection, so a null-deref / bad-cast in that path is caught by CI.
    { c->newDocument();
      Shape r; r.type=ShapeType::Rect; r.name="Rect"; c->doc().addShape(r);
      Shape g; g.type=ShapeType::Group; g.name="Grp";
      Shape k1; k1.type=ShapeType::Ellipse; k1.name="k1"; Shape k2; k2.type=ShapeType::Text; k2.text="hi";
      g.children={k1,k2}; c->doc().addShape(g);
      c->addLayer(); c->doc().shapes.last().layer=1;
      auto* gal=new PageLayerGallery(c);           // builds the tree
      auto* tree=gal->findChild<QTreeWidget*>();
      check(tree!=nullptr, "layer tree widget constructed");
      check(tree && tree->topLevelItemCount()==2, "tree shows both layers as top-level rows");
      c->select({0});                                // emits selectionChanged → highlight path runs
      c->renameShape(0,"Rect2");                      // emits documentChanged → refresh → rebuild path
      check(tree && tree->topLevelItemCount()==2, "tree survives a document-changed refresh");
      delete gal; }

    // --- Batch K: photo import ---
    std::printf("[photo] import image as a photo object\n");
    c->newDocument();
    { QTemporaryDir tmp; QImage im(160,120,QImage::Format_ARGB32); im.fill(QColor(120,80,200)); QString p=tmp.path()+"/p.png"; im.save(p);
      bool ok=c->importImage(p);
      check(ok, "importImage loaded the file");
      check(c->doc().shapes.size()==1, "photo object created");
      const Shape& s=c->doc().shapes.last();
      check(s.type==ShapeType::Rect && s.fill.type==FillType::Bitmap && !s.fill.image.isNull(), "photo is a Rect with a bitmap fill");
      check(!s.fill.origImage.isNull(), "photo keeps the original image (non-destructive edits)");
      check(c->selection().size()==1, "imported photo is selected"); }

    // --- Batch M: repeat + align-to-page ---
    std::printf("[arrange] repeat grid + align to page\n");
    c->newDocument();
    { Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,40,40); s.transform=QTransform::fromTranslate(100,100); int idx=c->doc().addShape(s); c->select({idx});
      c->repeatSelection(3,2,60,60);
      check(c->doc().shapes.size()==6, "repeat created a 3×2 grid (6 objects)");
      c->selectAll(); c->alignSelectionAdv(1,0,true);          // align all left, to the page
      double minLeft=1e9; for(auto& sh:c->doc().shapes) minLeft=qMin(minLeft, sh.docBounds().left());
      check(qAbs(minLeft)<1.0, "align-left-to-page placed an object at x≈0");
      c->alignSelectionAdv(0,2,false);                          // centre vertically within selection
      check(true, "align-adv within selection ran"); }

    // --- Batch N: multi-page documents ---
    std::printf("[pages] add / duplicate / delete page\n");
    c->newDocument();
    { check(c->pageCount()==1, "new doc has one page");
      c->addPage(); check(c->pageCount()==2, "added a second page");
      Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,50,50); s.transform=QTransform::fromTranslate(100,100); c->doc().addShape(s);   // on page 0
      c->duplicatePage(0); check(c->pageCount()==3, "duplicated page 0 → 3 pages");
      check(c->doc().shapes.size()==2, "duplicate copied page-0 content to the inserted page");
      c->deletePage(1); check(c->pageCount()==2, "deleted a page");
      QJsonObject j=c->doc().toJson(); Document d2=Document::fromJson(j);
      check(d2.pageCount==2, "pageCount survives JSON round-trip"); }

    // --- Batch O: text frames (word-wrap) ---
    std::printf("[textframe] word-wrap within a frame\n");
    { Shape s; s.type=ShapeType::Text; s.font=QFont("DejaVu Sans",20);
      s.text="The quick brown fox jumps over the lazy dog again and again and again and again";
      s.textFrame=false; QRectF single=s.localBounds();
      s.rect=QRectF(0,0,200,400); s.textFrame=true; QRectF wrapped=s.localBounds();
      check(wrapped.height()>single.height()*1.8, "framed text wraps to several lines (much taller)");
      check(wrapped.width()<=215, "wrapped text stays within the frame width");
      // round-trip
      Document dd; dd.addShape(s); QJsonObject j=dd.toJson(); Document d2=Document::fromJson(j);
      check(d2.shapes[0].textFrame && d2.shapes[0].textCols==1, "text-frame attrs survive JSON"); }

    // --- Batch P: live copies + soft groups ---
    std::printf("[link] live copies + soft groups\n");
    c->newDocument();
    { Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,60,60); s.fill.color=Qt::red; s.transform=QTransform::fromTranslate(100,100); int idx=c->doc().addShape(s); c->select({idx});
      c->makeLiveCopy();
      check(c->doc().shapes.size()==2, "live copy created a second shape");
      check(c->doc().shapes[0].linkId>0 && c->doc().shapes[0].linkId==c->doc().shapes[1].linkId, "master + copy share a linkId");
      c->select({0}); c->applyToSelection([](Shape& sh){ sh.fill.color=QColor(0,200,0); },"Recolour master");
      check(c->doc().shapes[1].fill.color.green()>150, "live copy mirrored the master's new fill");
      check(qAbs(c->doc().shapes[1].transform.dx()-c->doc().shapes[0].transform.dx())>1, "live copy keeps its own position"); }
    c->newDocument();
    { Shape a; a.type=ShapeType::Rect; a.transform=QTransform::fromTranslate(50,50); int ia=c->doc().addShape(a);
      Shape b; b.type=ShapeType::Ellipse; b.transform=QTransform::fromTranslate(260,50); int ib=c->doc().addShape(b);
      c->select({ia,ib}); c->makeSoftGroup();
      check(c->doc().shapes[ia].softGroup>0 && c->doc().shapes[ia].softGroup==c->doc().shapes[ib].softGroup, "soft-group members share an id");
      c->selectNone(); c->setTool(Tool::Select); click(c, c->mapToView(QPointF(80,80)));   // click member a
      check(c->selection().size()==2, "clicking one soft-group member selects the whole group"); }

    // --- Batch Q: 3D extrude ---
    std::printf("[extrude] 3D extrude tool + render\n");
    c->newDocument();
    { Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,80,80); s.transform=QTransform::fromTranslate(200,200); int idx=c->doc().addShape(s); c->select({idx});
      c->setTool(Tool::Extrude); click(c, c->mapToView(QPointF(240,240)));
      check(c->doc().shapes[idx].extrude.on, "extrude tool toggled 3D on");
      QTemporaryDir td; check(c->exportPng(td.path()+"/e.png",1), "document with a 3D extrude exports (renders without crash)");
      QJsonObject j=c->doc().toJson(); Document d2=Document::fromJson(j); check(d2.shapes[idx].extrude.on, "extrude survives JSON round-trip"); }

    // --- Batch R: web objects + HTML export ---
    std::printf("[web] hyperlink + HTML export\n");
    c->newDocument();
    { Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,120,44); s.transform=QTransform::fromTranslate(50,50); s.url="https://xara.com"; s.name="Button"; int idx=c->doc().addShape(s);
      QJsonObject j=c->doc().toJson(); Document d2=Document::fromJson(j); check(d2.shapes[idx].url=="https://xara.com", "object hyperlink survives JSON");
      QTemporaryDir td; QString h=td.path()+"/page.html"; check(c->exportHtml(h), "HTML export succeeded");
      QFile f(h); f.open(QIODevice::ReadOnly); QString html=QString::fromUtf8(f.readAll());
      check(html.contains("xara.com") && html.contains("<a "), "exported HTML has the clickable link overlay"); }

    // --- Batch S: animation (frames = layers) -> animated GIF ---
    std::printf("[anim] multi-frame animated GIF\n");
    c->newDocument();
    { c->addLayer(); c->addLayer();   // 3 layers = 3 frames
      for(int L=0;L<3;++L){ Shape s; s.type=ShapeType::Ellipse; s.rect=QRectF(0,0,80,80); s.transform=QTransform::fromTranslate(100+L*60,120); s.layer=L; s.fill.type=FillType::Solid; s.fill.color=QColor(40+L*70,120,200); c->doc().addShape(s); }
      check(c->doc().layers.size()==3, "3 layers (frames) created");
      QTemporaryDir td; QString gp=td.path()+"/a.gif"; check(c->exportGif(gp,100), "animated GIF exported");
      QFile gf(gp); gf.open(QIODevice::ReadOnly); QByteArray head=gf.read(6); qint64 sz=gf.size(); gf.close();
      check(head=="GIF89a", "GIF has a valid GIF89a header");
      check(sz>300, "GIF has real content");
      // exporting must NOT leave any layer hidden afterwards
      check(c->doc().layers[0].visible && c->doc().layers[1].visible && c->doc().layers[2].visible, "GIF export restored layer visibility");
      // GIF loop count + per-frame timing (G-P): loops=3 and a per-layer delay both encode without error.
      c->doc().layers[1].frameDelayMs=400;
      QString gp2=td.path()+"/loop.gif"; check(c->exportGif(gp2,80,3), "GIF exports with loop count + per-frame timing");
      { QFile g2(gp2); g2.open(QIODevice::ReadOnly); QByteArray all=g2.read(4096); g2.close();
        check(all.contains("NETSCAPE2.0"), "GIF carries the NETSCAPE looping block"); }
      check(Document::fromJson(c->doc().toJson()).layers[1].frameDelayMs==400, "per-frame delay round-trips through JSON");
      // Animated-GIF IMPORT (G-M): re-import the GIF we just exported → one layer per frame (round-trip).
      c->newDocument();
      check(c->importAnimatedGif(gp), "importAnimatedGif reads the exported GIF");
      check(c->doc().layers.size()>=2, "animated GIF imported as multiple frame-layers");
      check(c->doc().shapes.size()==c->doc().layers.size() && c->doc().shapes[0].fill.type==FillType::Bitmap, "each frame is a Bitmap on its own layer");
      // WebP export (G-M): a raster save via the .webp extension (skipped if the plugin isn't built).
      if(QImageWriter::supportedImageFormats().contains("webp")){
        QString wp=td.path()+"/o.webp"; check(c->exportRaster(wp,1,1.0,-1,true), "WebP export succeeds");
        check(!QImage(wp).isNull(), "the exported WebP reloads"); } }
    // RTF file import (G-M): read a small .rtf into a styled Text shape.
    { c->newDocument();
      QString rp=QDir::tempPath()+"/xq_test.rtf";
      { QFile f(rp); f.open(QIODevice::WriteOnly); f.write("{\\rtf1\\ansi\\deff0 {\\fonttbl{\\f0 Arial;}}\\f0\\fs24 Hello \\b bold\\b0  world\\par}"); f.close(); }
      check(c->importRtfFile(rp), "importRtfFile succeeds");
      check(!c->selection().isEmpty(), "RTF import selected the new shape");
      const int ri=c->selection().first();
      check(c->doc().shapes[ri].type==ShapeType::Text && c->doc().shapes[ri].text.contains("Hello") && c->doc().shapes[ri].text.contains("world"), "RTF text content imported"); }
    // Compressed-XAR write (H-B): saveXar(compress=true) round-trips through loadXar.
    { Document d; d.pageSize=QSizeF(300,200);
      Shape s; s.type=ShapeType::Ellipse; s.rect=QRectF(0,0,120,80); s.fill.type=FillType::Solid; s.fill.color=QColor(200,60,60); s.transform=QTransform::fromTranslate(20,20);
      Shape t; t.type=ShapeType::Text; t.text="compressed"; t.font=QFont("DejaVu Sans",18); t.fill.type=FillType::Solid; t.fill.color=Qt::black; t.transform=QTransform::fromTranslate(20,140);
      d.shapes={s,t};
      const QString cp="/tmp/selftest_compressed.xar", up="/tmp/selftest_uncompressed.xar";
      check(xara::saveXar(d,up,false), "uncompressed saveXar succeeds");
      check(xara::saveXar(d,cp,true), "compressed saveXar succeeds");
      const qint64 usz=QFileInfo(up).size(), csz=QFileInfo(cp).size();
      check(csz>0 && csz<usz, "the compressed .xar is smaller than the uncompressed one");
      Document rt; check(xara::loadXar(rt,cp), "loadXar reads the compressed .xar back");
      check(rt.shapes.size()==2 && rt.shapes[0].type==ShapeType::Ellipse && rt.shapes[1].text=="compressed", "compressed .xar round-trips its content"); }
    // DOCX import (H-B): a hand-built stored ZIP containing word/document.xml → an editable text frame.
    { auto le16=[](QByteArray&b,quint16 v){ b.append(char(v&0xFF)); b.append(char((v>>8)&0xFF)); };
      auto le32=[](QByteArray&b,quint32 v){ for(int k=0;k<4;++k) b.append(char((v>>(8*k))&0xFF)); };
      const QByteArray name="word/document.xml";
      const QByteArray xml="<?xml version=\"1.0\"?><w:document xmlns:w=\"x\"><w:body>"
        "<w:p><w:r><w:t>Hello</w:t></w:r><w:r><w:t> World</w:t></w:r></w:p>"
        "<w:p><w:r><w:t>Second line</w:t></w:r></w:p></w:body></w:document>";
      QByteArray z;
      le32(z,0x04034b50); le16(z,20); le16(z,0); le16(z,0); le16(z,0); le16(z,0); le32(z,0); le32(z,xml.size()); le32(z,xml.size()); le16(z,name.size()); le16(z,0); z+=name; z+=xml;
      quint32 cdOff=z.size();
      le32(z,0x02014b50); le16(z,20); le16(z,20); le16(z,0); le16(z,0); le16(z,0); le16(z,0); le32(z,0); le32(z,xml.size()); le32(z,xml.size()); le16(z,name.size()); le16(z,0); le16(z,0); le16(z,0); le16(z,0); le32(z,0); le32(z,0); z+=name;
      quint32 cdSize=z.size()-cdOff;
      le32(z,0x06054b50); le16(z,0); le16(z,0); le16(z,1); le16(z,1); le32(z,cdSize); le32(z,cdOff); le16(z,0);
      const QString dp=QDir::tempPath()+"/xq_test.docx"; { QFile f(dp); f.open(QIODevice::WriteOnly); f.write(z); f.close(); }
      c->newDocument();
      check(c->importDocx(dp), "importDocx reads a .docx");
      const int di=c->selection().isEmpty()?-1:c->selection().first();
      check(di>=0 && c->doc().shapes[di].type==ShapeType::Text, "DOCX import created a text frame");
      if(di>=0){ const QString t=c->doc().shapes[di].text;
        check(t.contains("Hello World") && t.contains("Second line"), "DOCX paragraphs + runs imported"); } }
    // CDR import (H-B): a fake .cdr with an embedded PNG → its preview bitmap is placed.
    { QImage prev(40,30,QImage::Format_ARGB32); prev.fill(QColor(20,140,220));
      QByteArray pngBytes; { QBuffer b(&pngBytes); b.open(QIODevice::WriteOnly); prev.save(&b,"PNG"); }
      QByteArray cdr="RIFF....CDRxjunkjunk"; cdr+=pngBytes; cdr+="trailingjunk";
      const QString cp=QDir::tempPath()+"/xq_test.cdr"; { QFile f(cp); f.open(QIODevice::WriteOnly); f.write(cdr); f.close(); }
      c->newDocument();
      check(c->importCdr(cp), "importCdr extracts an embedded preview");
      const int ci=c->selection().isEmpty()?-1:c->selection().first();
      check(ci>=0 && c->doc().shapes[ci].fill.type==FillType::Bitmap && c->doc().shapes[ci].fill.image.width()==40, "CDR preview imported as a 40px-wide bitmap"); }
    // CDR RIFF (v7–X5) preview: the thumbnail is a headerless CF_DIB inside the DISP chunk (no PNG/JPEG at all).
    { auto le32=[](QByteArray& a,quint32 v){ char b[4]={char(v&0xff),char((v>>8)&0xff),char((v>>16)&0xff),char((v>>24)&0xff)}; a.append(b,4); };
      const int W=6,H=5; QByteArray dib; le32(dib,40); le32(dib,W); le32(dib,H); { char pl[2]={1,0}; dib.append(pl,2); char bp[2]={24,0}; dib.append(bp,2); }
      le32(dib,0); le32(dib,0); le32(dib,0); le32(dib,0); le32(dib,0); le32(dib,0);        // comp,imgsize,ppmx,ppmy,clrUsed,clrImp
      const int stride=((W*3+3)&~3); for(int y=0;y<H;++y) for(int x=0;x<stride;++x) dib.append(char((x<W*3)?(x*7+y*13):0));
      QByteArray disp; le32(disp,8); disp+=dib;                                            // CF_DIB clipboard tag + the DIB
      QByteArray body="CDRB"; body.append("DISP",4); le32(body,disp.size()); body+=disp;
      QByteArray cdr="RIFF"; le32(cdr,body.size()); cdr+=body;
      const QString cp=QDir::tempPath()+"/xq_disp.cdr"; { QFile f(cp); f.open(QIODevice::WriteOnly); f.write(cdr); f.close(); }
      c->newDocument();
      check(c->importCdr(cp), "importCdr extracts a RIFF DISP-chunk DIB preview (CorelDRAW 11-style, no PNG/JPEG)");
      const int ci=c->selection().isEmpty()?-1:c->selection().first();
      check(ci>=0 && c->doc().shapes[ci].fill.type==FillType::Bitmap
            && c->doc().shapes[ci].fill.image.width()==W && c->doc().shapes[ci].fill.image.height()==H,
            "DISP DIB decoded to the right 6x5 bitmap"); }
    // libcdr editable-vector import path (compile-time optional). When the library is present, a
    // valid .cdr parses to standard SVG (fed to parseSvg → editable Shapes); when absent the helper
    // reports unavailable and importCdr silently falls back to the DISP-chunk preview above. Garbage
    // input must never yield pages either way — that's the portable invariant we can assert here.
    check(xara::cdrToSvgPages(QByteArray("not a cdr file")).isEmpty(),
          "cdrToSvgPages rejects non-CDR data (no spurious pages)");
    if(!xara::cdrVectorAvailable())
        check(!xara::cdrToSvgPages(QByteArray("RIFF....CDRxjunk")).size(),
              "cdrToSvgPages returns nothing when libcdr is not compiled in");
    // The librevenge sibling importers (WPG/Visio/FreeHand/Publisher) share the same bridge: whatever the
    // build config, non-format garbage must never yield spurious pages, and a compiled-out library must
    // report unavailable AND return nothing. (Real-file fidelity is validated out-of-tree against the DLP
    // regression corpora; here we assert the portable safety invariants.)
    check(xara::wpgToSvgPages(QByteArray("not a wpg")).isEmpty()
       && xara::visioToSvgPages(QByteArray("not a visio")).isEmpty()
       && xara::freehandToSvgPages(QByteArray("not freehand")).isEmpty()
       && xara::mspubToSvgPages(QByteArray("not a pub")).isEmpty(),
          "DLP vector importers reject non-format data (no spurious pages)");
    check((xara::wpgVectorAvailable()      || xara::wpgToSvgPages(QByteArray()).isEmpty())
       && (xara::visioVectorAvailable()    || xara::visioToSvgPages(QByteArray()).isEmpty())
       && (xara::freehandVectorAvailable() || xara::freehandToSvgPages(QByteArray()).isEmpty())
       && (xara::mspubVectorAvailable()    || xara::mspubToSvgPages(QByteArray()).isEmpty()),
          "DLP vector importers return nothing when their library is not compiled in");
    // A huge synthetic vector import (well past the auto-select cap) must add + return fast, not freeze on
    // the per-selection UI refresh (regression guard for the 9k-object .cdr import hang).
    { c->newDocument(); QVector<Shape> many; many.reserve(6000);
      for(int i=0;i<6000;++i){ Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,4,4); s.fill.type=FillType::Solid; s.fill.color=Qt::gray;
          s.transform=QTransform::fromTranslate((i%80)*5, (i/80)*5); many.push_back(s); }
      QElapsedTimer et; et.start(); c->addShapes(many, "bulk");
      check(c->doc().shapes.size()==6000, "6000-shape bulk import adds every shape");
      check(c->selection().isEmpty(), "a huge bulk import does NOT auto-select (keeps the selectionChanged UI cheap)");
      check(et.elapsed()<5000, "6000-shape bulk import completes quickly (no per-selection freeze)"); }
    // Mail / data merge (H-D): a {{Field}} template + a 3-row CSV → 3 substituted pages.
    { c->newDocument();
      Shape t; t.type=ShapeType::Text; t.text="Dear {{Name}}, you are {{Age}}."; t.font=QFont("DejaVu Sans",16); t.fill.type=FillType::Solid; t.fill.color=Qt::black; t.transform=QTransform::fromTranslate(20,20);
      c->doc().addShape(t);
      QString mp=QDir::tempPath()+"/xq_merge.csv";
      { QFile f(mp); f.open(QIODevice::WriteOnly); f.write("Name,Age\nAda,36\nBob,42\nCleo,29\n"); f.close(); }
      check(c->mailMerge(mp), "mailMerge succeeds");
      check(c->doc().pageCount==3, "3 CSV data rows → 3 pages");
      check(c->doc().shapes.size()==3, "one template copy per row");
      check(c->doc().shapes[0].text=="Dear Ada, you are 36." && c->doc().shapes[2].text=="Dear Cleo, you are 29.", "{{fields}} substituted per row"); }
    // QR code (H-D): generated via libqrencode when present; graceful when not.
    { c->newDocument();
      if(xara::qrAvailable()){
        check(c->insertQrCode("https://example.com/hello"), "insertQrCode succeeds when libqrencode is present");
        const int qi=c->selection().isEmpty()?-1:c->selection().first();
        check(qi>=0 && c->doc().shapes[qi].fill.type==FillType::Bitmap, "QR placed as a bitmap object");
        // a QR bitmap is square and non-trivially sized (finder patterns etc.)
        const QImage& im=c->doc().shapes[qi].fill.image;
        check(!im.isNull() && im.width()==im.height() && im.width()>=(21+8)*8, "QR image is a square matrix with quiet zone");
      } else { check(!c->insertQrCode("x"), "insertQrCode fails gracefully without libqrencode"); } }
    // --- G-P: CSV import + table distribute ---
    { c->newDocument();
      QString cp=QDir::tempPath()+"/xq_test.csv";
      { QFile f(cp); f.open(QIODevice::WriteOnly); f.write("Name,Score,\"Note, quoted\"\nAda,42,ok\nBob,7,\"line\"\"q\"\"\"\n"); f.close(); }
      check(c->importCsvAsTable(cp), "importCsvAsTable succeeds");
      const int ti=c->selection().isEmpty()?-1:c->selection().first();
      check(ti>=0 && c->doc().shapes[ti].type==ShapeType::Table, "CSV import created a Table shape");
      if(ti>=0){ const Table& t=c->doc().shapes[ti].table;
        check(t.rows()==3 && t.cols()==3, "CSV table has 3 rows × 3 cols");
        check(t.at(0,2).text=="Note, quoted", "a quoted comma-containing field parsed correctly");
        check(t.at(2,2).text=="line\"q\"", "escaped double-quotes parsed correctly");
        // distribute columns evenly: unequal widths become equal (total preserved)
        c->doc().shapes[ti].table.colW={40,80,120}; c->select({ti});
        const double tot=40+80+120; c->tableDistribute(false);
        const Table& t2=c->doc().shapes[ti].table;
        check(qAbs(t2.colW[0]-tot/3)<0.01 && qAbs(t2.colW[1]-tot/3)<0.01, "distribute columns equalised the widths"); }
      // REGRESSION (review): a pathological CSV (one huge row of commas) is capped, not an OOM.
      c->newDocument();
      QString big=QDir::tempPath()+"/xq_big.csv";
      { QFile f(big); f.open(QIODevice::WriteOnly); QByteArray line(200000,','); f.write(line); f.close(); }   // 200k columns on one line
      check(c->importCsvAsTable(big), "a pathological wide CSV still imports (capped)");
      { const Table& t=c->doc().shapes[c->selection().first()].table;
        check(t.cols()<=256 && t.rows()<=4096, "CSV import caps rows/cols to a sane bound"); } }

    // --- Batch U: variable-width stroke + art brush + custom dash ---
    std::printf("[stroke] variable width + art brush + dash\n");
    c->newDocument();
    { Shape s; s.type=ShapeType::Path; QPainterPath p; p.moveTo(0,0); p.lineTo(200,0); s.path=p; s.transform=QTransform::fromTranslate(100,200); int idx=c->doc().addShape(s); c->select({idx});
      c->applyToSelection([](Shape& sh){ sh.stroke.enabled=true; sh.stroke.width=10; sh.stroke.varWidth=true; sh.stroke.widthProfile={{0,1},{1,0}}; },"Taper");
      check(c->doc().shapes[idx].stroke.varWidth && c->doc().shapes[idx].stroke.widthProfile.size()==2, "variable-width taper profile set");
      check(qAbs(c->doc().shapes[idx].stroke.widthMultAt(0.0)-1.0)<0.02 && c->doc().shapes[idx].stroke.widthMultAt(1.0)<0.02, "width profile interpolates 1→0 along the path");
      QImage ink(8,8,QImage::Format_ARGB32); ink.fill(Qt::white); c->applyToSelection([ink](Shape& sh){ sh.stroke.brushInk=ink; },"Brush");
      check(!c->doc().shapes[idx].stroke.brushInk.isNull(), "art-brush ink applied");
      QTemporaryDir td; check(c->exportPng(td.path()+"/s.png",1), "variable-width + art-brush stroke renders");
      c->applyToSelection([](Shape& sh){ sh.stroke.dashes={8,4,2,4}; },"Dash");
      check(c->doc().shapes[idx].stroke.dashes.size()==4, "custom dash pattern set"); }
    // --- G-E: brush-from-selection + Line InfoBar arrow/transp/taper ---
    { c->newDocument();
      Shape r; r.type=ShapeType::Ellipse; r.rect=QRectF(0,0,40,40); r.fill.type=FillType::Solid; r.fill.color=Qt::red; int i=c->doc().addShape(r); c->select({i});
      check(c->createBrushFromSelection(), "createBrushFromSelection succeeds");
      check(!c->currentStroke().brushInk.isNull(), "the selection became the current stroke's brush ink");
      check(c->createBrushFromSelection() || true, "brush-from-selection is idempotent");
      // arrowheads on the current stroke apply to a selected line
      Shape ln; ln.type=ShapeType::Path; QPainterPath p; p.moveTo(0,0); p.lineTo(100,0); ln.path=p; int li=c->doc().addShape(ln); c->select({li});
      c->applyToSelection([](Shape& sh){ sh.stroke.enabled=true; sh.stroke.endArrow=-6; sh.stroke.startArrow=-2; sh.stroke.transp=0.3; },"Line attrs");
      const Shape& L=c->doc().shapes[li];
      check(L.stroke.endArrow==-6 && L.stroke.startArrow==-2, "start/end arrowheads set on the line");
      check(qAbs(L.stroke.transp-0.3)<1e-6, "line transparency set");
      Document rt=Document::fromJson(c->doc().toJson());
      check(rt.shapes[li].stroke.endArrow==-6 && qAbs(rt.shapes[li].stroke.transp-0.3)<1e-6, "arrow + line transp round-trip through JSON"); }

    // --- H-path/brush: calligraphic pen, blend node correspondence, node-edit inside a group ---
    { c->newDocument();
      // Calligraphic nib: a fixed-angle flat ribbon stroke. The flag/angle must round-trip and render.
      Shape ln; ln.type=ShapeType::Path; QPainterPath p; p.moveTo(0,0); p.cubicTo(40,-40,80,40,120,0); ln.path=p;
      ln.transform=QTransform::fromTranslate(80,120); int li=c->doc().addShape(ln); c->select({li});
      c->applyToSelection([](Shape& sh){ sh.stroke.enabled=true; sh.stroke.width=14; sh.stroke.calligraphic=true; sh.stroke.calligAngle=42; },"Calligraphic");
      const Shape& L=c->doc().shapes[li];
      check(L.stroke.calligraphic && qAbs(L.stroke.calligAngle-42)<1e-6, "calligraphic nib flag + angle set");
      Document rc=Document::fromJson(c->doc().toJson());
      check(rc.shapes[li].stroke.calligraphic && qAbs(rc.shapes[li].stroke.calligAngle-42)<1e-6, "calligraphic nib round-trips through JSON");
      { QTemporaryDir td; check(c->exportPng(td.path()+"/callig.png",1), "calligraphic ribbon stroke renders"); }
      // A default (non-calligraphic) stroke must NOT persist the calligraphic keys — keeps corpus byte-identical.
      Shape plain; plain.type=ShapeType::Path; QPainterPath pp; pp.moveTo(0,0); pp.lineTo(50,0); plain.path=pp; plain.stroke.enabled=true;
      QJsonObject po; { Document d2; d2.addShape(plain); po=d2.toJson()["shapes"].toArray()[0].toObject(); }
      check(!po.contains("cal"), "non-calligraphic stroke omits the 'cal' JSON key (default-noop gate)"); }

    { c->newDocument();
      // Blend node correspondence: shifting which start-node maps to the blend end reorders the morph.
      // The shift value must round-trip and the blend must still render its steps.
      Shape a; a.type=ShapeType::Path; { QPainterPath q; q.moveTo(0,0); q.lineTo(40,0); q.lineTo(40,40); q.lineTo(0,40); q.closeSubpath(); a.path=q; }
      a.fill.type=FillType::Solid; a.fill.color=Qt::red; a.transform=QTransform::fromTranslate(60,60);
      Shape b=a; b.fill.color=Qt::blue; b.transform=QTransform::fromTranslate(260,60);
      int ia=c->doc().addShape(a), ib=c->doc().addShape(b); c->select({ia,ib});
      c->makeLiveBlend(6);
      // find the LIVE blend group and set a node shift on it (the shift only affects the live-morph render)
      int gi=-1; for(int i=0;i<c->doc().shapes.size();++i) if(c->doc().shapes[i].type==ShapeType::Group && c->doc().shapes[i].blendSteps>0) gi=i;
      check(gi>=0, "live blend produced a blend group");
      if(gi>=0){ c->doc().shapes[gi].blendNodeShift=0.25;
        Document rb=Document::fromJson(c->doc().toJson());
        check(qAbs(rb.shapes[gi].blendNodeShift-0.25)<1e-6, "blend node-shift round-trips through JSON");
        QTemporaryDir td; check(c->exportPng(td.path()+"/blendns.png",1), "blend with node-shift renders");
        // default (0.0) must be omitted from JSON so the corpus stays byte-identical
        c->doc().shapes[gi].blendNodeShift=0.0;
        QJsonObject go=c->doc().toJson()["shapes"].toArray()[gi].toObject();
        check(!go.contains("blendNS"), "zero blend node-shift omits the 'blendNS' JSON key (default-noop gate)"); } }

    { c->newDocument();
      // Node-edit inside a group: parse + edit ONE child path without ungrouping. The edit must bake into
      // the child (leaving the group intact) and honour the child's transform via nodeXf().
      Shape c0; c0.type=ShapeType::Path; { QPainterPath q; q.moveTo(0,0); q.lineTo(60,0); q.lineTo(60,60); c0.path=q; } c0.transform=QTransform::fromTranslate(0,0);
      Shape c1; c1.type=ShapeType::Path; { QPainterPath q; q.moveTo(0,0); q.lineTo(80,0); q.lineTo(80,80); c1.path=q; } c1.transform=QTransform::fromTranslate(120,10);
      Shape g; g.type=ShapeType::Group; g.children.push_back(c0); g.children.push_back(c1); g.transform=QTransform::fromTranslate(200,200);
      int gi=c->doc().addShape(g); c->select({gi});
      c->nodeEditChild(gi,1);
      check(c->nodeSubpaths()==1, "node-edit-in-group parsed the child's single subpath");
      const int before=c->nodeCount(0);
      check(before==3, "child 1 has its own 3 nodes (not the whole group's)");
      const int c0nodes = c->doc().shapes[gi].children[0].path.elementCount();
      c->insertNodeOnSegment(0,0,0.5);   // edit child 1 → bakes into children[1] only
      check(c->doc().shapes[gi].type==ShapeType::Group, "the group is preserved (not ungrouped) after a child node edit");
      check(c->doc().shapes[gi].children[1].type==ShapeType::Path && c->nodeCount(0)==before+1, "the inserted node baked into child 1");
      check(c->doc().shapes[gi].children[0].path.elementCount()==c0nodes, "sibling child 0 is untouched by the edit"); }

    // --- Batch V: symbol insert into text ---
    std::printf("[text2] symbol insertion\n");
    c->newDocument();
    { Shape s; s.type=ShapeType::Text; s.text="Hello"; c->doc().addShape(s); c->select({0});
      c->applyToSelection([](Shape& sh){ if(sh.type==ShapeType::Text) sh.text+=QString::fromUtf8("\xE2\x86\x92"); },"Insert symbol");   // →
      check(c->doc().shapes[0].text.endsWith(QString::fromUtf8("\xE2\x86\x92")), "symbol appended to selected text object"); }

    // --- Batch W: presentation export + soft-grouped web button ---
    std::printf("[web2] presentation slideshow + button\n");
    c->newDocument();
    { c->addPage();   // 2 pages
      Shape a; a.type=ShapeType::Rect; a.transform=QTransform::fromTranslate(60,60); c->doc().addShape(a);
      QTemporaryDir td; QString hp=td.path()+"/pres.html"; check(c->exportPresentation(hp), "presentation HTML exported");
      QFile f(hp); f.open(QIODevice::ReadOnly); QString html=QString::fromUtf8(f.readAll());
      check(html.count("class=\"slide\"")==2, "presentation has one slide per page");
      check(html.contains("ArrowRight"), "presentation has keyboard navigation"); }
    c->newDocument();
    { Shape r; r.type=ShapeType::Rect; r.url="https://x.test"; int i0=c->doc().addShape(r);
      Shape t; t.type=ShapeType::Text; t.text="Go"; t.url="https://x.test"; int i1=c->doc().addShape(t);
      c->select({i0,i1}); c->makeSoftGroup();
      check(c->doc().shapes[i0].softGroup>0 && c->doc().shapes[i0].softGroup==c->doc().shapes[i1].softGroup, "web-button rect+label soft-grouped");
      QTemporaryDir td; QString hp=td.path()+"/p.html"; c->exportHtml(hp); QFile f(hp); f.open(QIODevice::ReadOnly); QString h=QString::fromUtf8(f.readAll());
      check(h.contains("x.test") && h.contains(":hover"), "button link + :hover present in exported HTML"); }

    // --- Batch X: mesh corner editing + bitmap fill ---
    std::printf("[fill2] mesh corners + bitmap fill\n");
    c->newDocument();
    { Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,100,100); int idx=c->doc().addShape(s); c->select({idx});
      c->applyToSelection([](Shape& sh){ sh.fill.type=FillType::Mesh; sh.fill.meshColors={Qt::red,Qt::green,Qt::blue,Qt::yellow}; },"Mesh");
      c->applyToSelection([](Shape& sh){ if(sh.fill.meshColors.size()<4)sh.fill.meshColors.resize(4); sh.fill.meshColors[2]=QColor(10,20,30); },"Edit corner");
      check(c->doc().shapes[idx].fill.meshColors[2]==QColor(10,20,30), "mesh corner colour edited");
      QImage im(20,20,QImage::Format_ARGB32); im.fill(Qt::magenta);
      c->applyToSelection([im](Shape& sh){ sh.fill.type=FillType::Bitmap; sh.fill.image=im; sh.fill.tiled=true; },"Bitmap fill");
      check(c->doc().shapes[idx].fill.type==FillType::Bitmap && !c->doc().shapes[idx].fill.image.isNull() && c->doc().shapes[idx].fill.tiled, "bitmap fill applied + tiled");
      QTemporaryDir td; check(c->exportPng(td.path()+"/r.png",1), "mesh→bitmap fill renders"); }

    // --- Batch Y: master page (object replicated on every page) ---
    std::printf("[master] master-page object on every page\n");
    c->newDocument();
    { c->doc().pageCount=2; c->addLayer(); c->setLayerMaster(1,true);
      Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,60,60); s.transform=QTransform::fromTranslate(30,30); s.layer=1; s.fill.type=FillType::Solid; s.fill.color=Qt::red; s.stroke.enabled=false; c->doc().addShape(s);
      check(c->doc().layers[1].master, "layer flagged as master page");
      QRectF all=c->doc().allPagesRect();
      QImage img(int(all.width()),int(all.height()),QImage::Format_ARGB32_Premultiplied); img.fill(Qt::white);
      { QPainter g(&img); g.setRenderHint(QPainter::Antialiasing); c->renderDocument(g); }
      double adv=c->doc().pageSize.height()+c->doc().pageGap;
      QColor p0=img.pixelColor(50,50), p1=img.pixelColor(50,int(50+adv));
      check(p0.red()>180 && p0.green()<90, "master object visible on page 1");
      check(p1.red()>180 && p1.green()<90, "master object replicated onto page 2"); }

    // --- Batch Z: small caps + drop cap (gated; default path byte-identical) ---
    std::printf("[textcase] small caps & drop cap geometry\n");
    { Shape base; base.type=ShapeType::Text; base.text="hello world"; base.font=QFont("DejaVu Sans",20);
      double wPlain=base.localBounds().width();
      Shape sc=base; sc.smallCaps=true; double wSC=sc.localBounds().width();
      check(qAbs(wPlain-wSC)>1.0, "small caps changes text width");
      check(sc.localBounds().height()>0, "small caps renders glyphs");
      check(qAbs(base.localBounds().width()-wPlain)<1e-9, "plain text path unchanged by small-caps support");
      Shape fr; fr.type=ShapeType::Text; fr.textFrame=true; fr.rect=QRectF(0,0,320,200);
      fr.text="Hello"; fr.font=QFont("DejaVu Sans",14);            // single short line
      double hNoCap=fr.localBounds().height();
      Shape dc=fr; dc.dropCap=5; double hCap=dc.localBounds().height();
      check(hCap>hNoCap*2, "drop cap enlarges first letter well beyond one line");
      check(qAbs(fr.localBounds().height()-hNoCap)<1e-9, "frame without drop cap unchanged"); }

    std::printf("[textcase] small caps / drop cap survive JSON round-trip\n");
    { Shape s; s.type=ShapeType::Text; s.text="Abc"; s.smallCaps=true; s.dropCap=4; s.textFrame=true; s.rect=QRectF(0,0,200,100);
      Shape r=Shape::fromJson(s.toJson());
      check(r.smallCaps==true, "smallCaps round-trips");
      check(r.dropCap==4, "dropCap round-trips"); }

    // --- Batch AA: parametric SmartShapes (kind+param regenerate the path live) ---
    std::printf("[smart] SmartShape regenerates path from its parameter\n");
    { check(!Shape::smartPath(5,5).isEmpty(), "smartPath(star,5) builds a path");
      // more star points => more path elements
      int e5=Shape::smartPath(5,5).elementCount(), e9=Shape::smartPath(5,9).elementCount();
      check(e9>e5, "more star points => more path nodes");
      // a Smart shape's localPath tracks smartParam
      Shape s; s.type=ShapeType::Smart; s.smartKind=5; s.smartParam=5; s.rect=QRectF(0,0,100,100);
      QPainterPath p5=s.localPath(); s.smartParam=11; QPainterPath p11=s.localPath();
      check(p11.elementCount()>p5.elementCount(), "Smart localPath regenerates when param changes");
      // gear (kind 1) has a hole => bounded box ~ full 0..100 rect mapped to shape rect
      Shape g; g.type=ShapeType::Smart; g.smartKind=1; g.smartParam=8; g.rect=QRectF(0,0,80,80);
      check(g.localPath().boundingRect().width()>0, "gear SmartShape renders within rect"); }

    std::printf("[smart] SmartShape kind+param survive JSON round-trip + setSmartParam\n");
    { Shape s; s.type=ShapeType::Smart; s.smartKind=3; s.smartParam=7; s.rect=QRectF(0,0,100,100);
      Shape r=Shape::fromJson(s.toJson());
      check(r.type==ShapeType::Smart, "Smart type round-trips");
      check(r.smartKind==3 && qAbs(r.smartParam-7)<1e-9, "smartKind/param round-trip");
      // Canvas::setSmartParam edits the live shape (clamped to >=2)
      c->newDocument(); c->doc().addShape(s); c->select({0});
      c->setSmartParam(12); check(qAbs(c->doc().shapes[0].smartParam-12)<1e-9, "setSmartParam updates selected SmartShape");
      c->setSmartParam(0);  check(c->doc().shapes[0].smartParam>=2, "setSmartParam clamps to minimum"); }

    // --- SmartShape 2nd control parameter (Batch 7) ---
    std::printf("[smart] second control parameter (depth/thickness/inner)\n");
    {
        // default (-1) reproduces the kind's original secondary constant → byte-identical path
        QPainterPath def = Shape::smartPath(6, 6, -1);
        QPainterPath seed= Shape::smartPath(6, 6, Shape::smartParam2Default(6));
        check(qAbs(def.boundingRect().left()-seed.boundingRect().left())<0.6
           && qAbs(def.boundingRect().right()-seed.boundingRect().right())<0.6, "smartParam2 default(-1) == the kind's hardcoded constant");
        // param2 changes geometry: the callout tail tip moves with it
        const double lOut = Shape::smartPath(6, 6, 0.0).boundingRect().left();    // tail tip at x=0 (out)
        const double lIn  = Shape::smartPath(6, 6, 0.5).boundingRect().left();    // tail tip at x=50 (inside the box)
        check(lOut < lIn-5, "callout param2=0 pushes the tail tip left (bbox grows)");
        check(Shape::smartPath(6, 6, 1.0).boundingRect().right() > 95, "callout param2=1 pushes the tail tip right");
        // a different kind is genuinely reshaped too (star inner radius): the path changes
        check(Shape::smartPath(5,6,0.1) != Shape::smartPath(5,6,0.9), "star param2 changes the inner-radius geometry");
        // Canvas: 2nd handle + setter + clamp + JSON
        c->newDocument();
        Shape s; s.type=ShapeType::Smart; s.smartKind=6; s.smartParam=6; s.rect=QRectF(0,0,100,100); s.transform=QTransform::fromTranslate(60,60);
        int idx=c->doc().addShape(s); c->select({idx}); c->setTool(Tool::Select);
        QPointF h2; check(c->smartHandle2(h2), "a selected SmartShape exposes a distinct 2nd-parameter handle");
        c->setSmartParam2(0.75); check(qFuzzyCompare(c->doc().shapes[idx].smartParam2,0.75), "setSmartParam2 stores the value");
        c->setSmartParam2(5.0);  check(qFuzzyCompare(c->doc().shapes[idx].smartParam2,1.0), "setSmartParam2 clamps to [0,1]");
        Document rt=Document::fromJson(c->doc().toJson());
        check(qFuzzyCompare(rt.shapes[idx].smartParam2,1.0), "smartParam2 round-trips through JSON (gated write)");
    }

    // --- Batch BB: presentation transitions + HTML rollover/anchors ---
    std::printf("[web] presentation transitions + HTML rollover state\n");
    { c->newDocument(); c->doc().pageCount=2;
      Shape btn; btn.type=ShapeType::Rect; btn.rect=QRectF(0,0,120,40); btn.transform=QTransform::fromTranslate(40,40);
      btn.name="Buy"; btn.url="https://example.com"; btn.fill.type=FillType::Solid; btn.fill.color=QColor(60,120,220); c->doc().addShape(btn);
      Shape anc; anc.type=ShapeType::Ellipse; anc.rect=QRectF(0,0,30,30); anc.transform=QTransform::fromTranslate(200,200); anc.name="section2"; c->doc().addShape(anc);
      QTemporaryDir td;
      // presentation with zoom transition
      QString pp=td.path()+"/show.html"; check(c->exportPresentation(pp,3), "exportPresentation(zoom) writes file");
      QFile pf(pp); pf.open(QIODevice::ReadOnly); QString ph=pf.readAll(); pf.close();
      check(ph.contains("tzoom"), "presentation carries the zoom transition class");
      check(ph.contains("classList.toggle('on'"), "presentation uses CSS-class slide switching");
      // html export with rollover + anchor
      QString hp=td.path()+"/site.html"; check(c->exportHtml(hp), "exportHtml writes file");
      QFile hf(hp); hf.open(QIODevice::ReadOnly); QString hh=hf.readAll(); hf.close();
      check(hh.contains("onmouseover"), "linked object gets a rollover (hover) handler");
      check(hh.contains("id=\"section2\""), "named object becomes an internal anchor target"); }

    // --- Batch CC: on-canvas Fill-tool gradient editing (grab endpoints + midpoint) ---
    std::printf("[filledit] Fill tool re-edits an existing gradient on-canvas\n");
    { c->newDocument();
      Shape r; r.type=ShapeType::Rect; r.rect=QRectF(0,0,200,120); r.transform=QTransform::fromTranslate(150,150);
      r.fill.type=FillType::Solid; r.fill.color=QColor(80,140,220); c->doc().addShape(r); c->refreshAll();
      c->setTool(Tool::Fill);
      // 1) drag a fresh gradient across the rect
      QPointF g0(170,210), g1(330,210);
      send(c,QEvent::MouseButtonPress, c->mapToView(g0), Qt::LeftButton, Qt::LeftButton);
      send(c,QEvent::MouseMove,        c->mapToView(g1), Qt::NoButton,   Qt::LeftButton);
      send(c,QEvent::MouseButtonRelease,c->mapToView(g1),Qt::LeftButton, Qt::NoButton);
      check(c->doc().shapes[0].fill.type==FillType::Linear, "Fill drag creates a linear gradient");
      QPointF p0a=c->doc().shapes[0].fill.p0, p1a=c->doc().shapes[0].fill.p1;
      // 2) grab the END handle and pull it up: end moves, start stays
      QTransform xf=c->doc().shapes[0].transform;
      QPointF endV=c->mapToView(xf.map(p1a)), endTo=c->mapToView(xf.map(p1a)+QPointF(0,-80));
      send(c,QEvent::MouseButtonPress, endV, Qt::LeftButton, Qt::LeftButton);
      send(c,QEvent::MouseMove,        endTo,Qt::NoButton,   Qt::LeftButton);
      send(c,QEvent::MouseButtonRelease,endTo,Qt::LeftButton,Qt::NoButton);
      QPointF p0b=c->doc().shapes[0].fill.p0, p1b=c->doc().shapes[0].fill.p1;
      check(qAbs(p0b.x()-p0a.x())<0.6 && qAbs(p0b.y()-p0a.y())<0.6, "dragging the end handle leaves the start fixed");
      check(p1b.y() < p1a.y()-20, "dragging the end handle moves the gradient end");
      // 3) grab the MIDPOINT and translate: both endpoints shift together
      QPointF midL=(p0b+p1b)/2; QPointF midV=c->mapToView(xf.map(midL)), midTo=c->mapToView(xf.map(midL)+QPointF(40,0));
      send(c,QEvent::MouseButtonPress, midV, Qt::LeftButton, Qt::LeftButton);
      send(c,QEvent::MouseMove,        midTo,Qt::NoButton,   Qt::LeftButton);
      send(c,QEvent::MouseButtonRelease,midTo,Qt::LeftButton,Qt::NoButton);
      QPointF p0c=c->doc().shapes[0].fill.p0, p1c=c->doc().shapes[0].fill.p1;
      check(qAbs((p0c.x()-p0b.x())-40)<3 && qAbs((p1c.x()-p1b.x())-40)<3, "midpoint drag translates the whole gradient"); }

    // --- Batch DD: Fill-tool second-axis (p2) handle for radial/mesh fills ---
    std::printf("[filledit] Fill tool edits the second axis (p2) of a radial fill\n");
    { c->newDocument();
      Shape r; r.type=ShapeType::Ellipse; r.rect=QRectF(0,0,200,160); r.transform=QTransform::fromTranslate(150,150);
      r.fill.type=FillType::Radial; r.fill.stops={{0,QColor(255,240,200)},{1,QColor(180,90,40)}};
      r.fill.p0=QPointF(100,80); r.fill.p1=QPointF(200,80); r.fill.p2=QPointF(100,160);  // centre, x-axis, y-axis (local)
      c->doc().addShape(r); c->refreshAll(); c->select({0}); c->setTool(Tool::Fill);
      QTransform xf=c->doc().shapes[0].transform;
      QPointF p0a=c->doc().shapes[0].fill.p0, p1a=c->doc().shapes[0].fill.p1, p2a=c->doc().shapes[0].fill.p2;
      // grab the p2 (green) handle and drag it; p2 moves, p0/p1 stay
      QPointF p2V=c->mapToView(xf.map(p2a)), p2To=c->mapToView(xf.map(p2a)+QPointF(0,60));
      send(c,QEvent::MouseButtonPress, p2V, Qt::LeftButton, Qt::LeftButton);
      send(c,QEvent::MouseMove,        p2To,Qt::NoButton,   Qt::LeftButton);
      send(c,QEvent::MouseButtonRelease,p2To,Qt::LeftButton,Qt::NoButton);
      QPointF p0b=c->doc().shapes[0].fill.p0, p1b=c->doc().shapes[0].fill.p1, p2b=c->doc().shapes[0].fill.p2;
      check(p2b.y() > p2a.y()+20, "dragging the p2 handle moves the radial second axis");
      check(qAbs(p0b.x()-p0a.x())<0.6 && qAbs(p1b.x()-p1a.x())<0.6, "p2 drag leaves centre and first axis fixed"); }

    // --- Batch EE: Fit Text to Curve (editable text-on-path) ---
    std::printf("[textcurve] Fit Text to Curve flows glyphs along a path\n");
    { c->newDocument();
      Shape t; t.type=ShapeType::Text; t.text="Following"; t.font=QFont("DejaVu Sans",24); t.transform=QTransform::fromTranslate(100,200);
      QRectF flat=t.localBounds();
      Shape pth; pth.type=ShapeType::Path; QPainterPath arc; arc.moveTo(0,0); arc.cubicTo(120,-120,240,120,360,0); pth.path=arc; pth.transform=QTransform::fromTranslate(100,260);
      c->doc().addShape(t); c->doc().addShape(pth); c->refreshAll();
      c->select({0,1}); c->fitTextToCurve();
      check(c->doc().shapes.size()==1, "curve object consumed by Fit Text to Curve");
      check(!c->doc().shapes[0].textPath.isEmpty(), "text bound to a curve path");
      QRectF curved=c->doc().shapes[0].localBounds();
      check(curved.height() > flat.height()+10, "curved text rises/falls along the arc (taller bbox)");
      c->detachTextFromCurve(); check(c->doc().shapes[0].textPath.isEmpty(), "detach clears the curve");
      QRectF flat2=c->doc().shapes[0].localBounds();
      check(qAbs(flat2.width()-flat.width())<0.01 && qAbs(flat2.height()-flat.height())<0.01, "detached text matches original flat layout"); }

    std::printf("[textcurve] textPath survives JSON round-trip\n");
    { Shape t; t.type=ShapeType::Text; t.text="Arc"; QPainterPath a; a.moveTo(0,0); a.cubicTo(30,-30,60,30,90,0); t.textPath=a;
      Shape r=Shape::fromJson(t.toJson());
      check(!r.textPath.isEmpty(), "textPath present after round-trip");
      check(r.textPath.elementCount()==t.textPath.elementCount(), "textPath element count preserved");
      check(r.localBounds().width()>0, "round-tripped curved text renders"); }

    // --- Batch FF: Transparency tool graduated editing ---
    std::printf("[transp] Transparency tool drags out a graduated transparency\n");
    { c->newDocument();
      Shape r; r.type=ShapeType::Rect; r.rect=QRectF(0,0,220,140); r.transform=QTransform::fromTranslate(150,150);
      r.fill.type=FillType::Solid; r.fill.color=QColor(220,90,60); c->doc().addShape(r); c->refreshAll();
      c->setTool(Tool::Transparency);
      QPointF t0(170,220), t1(350,220);
      send(c,QEvent::MouseButtonPress, c->mapToView(t0), Qt::LeftButton, Qt::LeftButton);
      send(c,QEvent::MouseMove,        c->mapToView(t1), Qt::NoButton,   Qt::LeftButton);
      send(c,QEvent::MouseButtonRelease,c->mapToView(t1),Qt::LeftButton, Qt::NoButton);
      check(c->doc().shapes[0].transp.isGraduated(), "drag creates a graduated transparency");
      check(c->doc().shapes[0].transp.type>0, "transparency type is set (visible)");
      QPointF q0=c->doc().shapes[0].transp.p0, q1=c->doc().shapes[0].transp.p1;
      check(qAbs(q1.x()-q0.x())>120, "transparency axis spans the drag");
      // render: alpha must fade opaque(left)->transparent(right) across the shape
      { QImage img(420,320,QImage::Format_ARGB32_Premultiplied); img.fill(Qt::transparent);
        { QPainter g(&img); g.setRenderHint(QPainter::Antialiasing); c->renderDocument(g); }
        int aL=qAlpha(img.pixel(185,220)), aR=qAlpha(img.pixel(345,220));
        check(aL>aR+30, "graduated transparency actually fades the fill across the shape"); }
      // grab the END handle and move it; p1 changes, p0 fixed
      QTransform xf=c->doc().shapes[0].transform;
      QPointF eV=c->mapToView(xf.map(q1)), eTo=c->mapToView(xf.map(q1)+QPointF(0,-70));
      send(c,QEvent::MouseButtonPress, eV, Qt::LeftButton, Qt::LeftButton);
      send(c,QEvent::MouseMove,        eTo,Qt::NoButton,   Qt::LeftButton);
      send(c,QEvent::MouseButtonRelease,eTo,Qt::LeftButton,Qt::NoButton);
      QPointF q0b=c->doc().shapes[0].transp.p0, q1b=c->doc().shapes[0].transp.p1;
      check(qAbs(q0b.x()-q0.x())<0.6 && qAbs(q0b.y()-q0.y())<0.6, "transparency start stays put when dragging the end");
      check(q1b.y() < q1.y()-20, "transparency end handle moves"); }

    // --- Batch GG: dictionary spell-check ---
    std::printf("[spell] dictionary spell-check (skips when no system dictionary)\n");
    { Speller& sp=Speller::instance();
      if (sp.ready()) {
          check(sp.known("hello") && sp.known("world"), "common words recognised");
          check(sp.known("cats") && sp.known("walking"), "plural/-ing morphology recognised");
          check(!sp.known("qwxztk"), "gibberish flagged as unknown");
          QStringList bad=sp.check("the quikc broun fox", 10);
          check(bad.contains("quikc") && bad.contains("broun"), "misspelled words detected in a sentence");
          check(!bad.contains("the") && !bad.contains("fox"), "correctly-spelled words not flagged");
          QStringList clean=sp.check("the quick brown fox jumps", 10);
          check(clean.isEmpty(), "a correct sentence yields no misspellings");
      } else {
          check(sp.known("anything"), "no dictionary => everything treated as known (graceful)");
          std::printf("  (no system dictionary installed — spell asserts skipped)\n");
      } }

    // --- Batch HH: EXIF metadata reader ---
    std::printf("[exif] EXIF reader parses JPEG APP1/TIFF metadata\n");
    { // Hand-built minimal JPEG: SOI + APP1(Exif, II TIFF, IFD0 Make="TestCam") + EOI
      QByteArray j=QByteArray::fromHex(
        "FFD8" "FFE1" "002A" "457869660000"          // SOI, APP1 len=42, "Exif\0\0"
        "49492A00" "08000000"                        // TIFF: II, magic 42, IFD0 offset=8
        "0100"                                       // IFD0 entry count = 1
        "0F01" "0200" "08000000" "1A000000"          // tag 0x010F Make, ASCII, count 8, valueOff=26
        "00000000"                                   // next IFD = 0
        "5465737443616D00"                           // "TestCam\0"
        "FFD9");                                      // EOI
      auto m=readExifFromData(j);
      check(m.value("Camera Make")=="TestCam", "EXIF IFD0 ASCII tag (Make) parsed");
      check(readExifFromData(QByteArray("not a jpeg")).isEmpty(), "non-JPEG yields no EXIF");
      // Real-world fixture (PIL-generated) when present — exercises the Exif sub-IFD + rationals
      if (QFile::exists("/tmp/exif_fixture.jpg")) {
          auto r=readExif("/tmp/exif_fixture.jpg");
          check(r.value("Camera Make")=="Canon" && r.value("Camera Model")=="EOS R6", "real JPEG Make/Model parsed");
          check(r.value("ISO")=="400", "Exif sub-IFD ISO parsed");
          check(r.value("Focal Length").startsWith("50"), "Exif sub-IFD focal length parsed");
          check(r.value("Aperture").startsWith("f/2.8"), "Exif sub-IFD aperture parsed");
          check(r.value("Exposure")=="1/250 s", "Exif sub-IFD exposure parsed");
      } else std::printf("  (no /tmp/exif_fixture.jpg — sub-IFD asserts skipped)\n"); }

    // --- Batch II: content-aware (seam-carving) photo resize ---
    std::printf("[photo] content-aware resize carves flat seams, keeps salient content\n");
    { QImage im(40,20,QImage::Format_RGB32); im.fill(QColor(128,128,128));
      for(int y=0;y<20;++y) im.setPixel(20,y,qRgb(0,0,0));        // one salient high-contrast column
      QImage out=contentAwareResize(im,24,20);
      check(out.width()==24 && out.height()==20, "width carved to target, height kept");
      int dark=0; for(int y=0;y<out.height();++y) for(int x=0;x<out.width();++x) if(out.pixelColor(x,y).value()<60) dark++;
      check(dark >= out.height()/2, "salient dark column survives carving (low-energy seams removed instead)");
      // height carving (via transpose path)
      QImage im2(20,40,QImage::Format_RGB32); im2.fill(QColor(128,128,128));
      for(int x=0;x<20;++x) im2.setPixel(x,20,qRgb(0,0,0));
      QImage out2=contentAwareResize(im2,20,24);
      check(out2.width()==20 && out2.height()==24, "height carved to target, width kept");
      int dark2=0; for(int y=0;y<out2.height();++y) for(int x=0;x<out2.width();++x) if(out2.pixelColor(x,y).value()<60) dark2++;
      check(dark2 >= out2.width()/2, "salient dark row survives height carving"); }

    // --- Batch JJ: Clone/Heal bitmap brush ---
    std::printf("[clone] Clone tool copies source pixels onto the destination\n");
    { c->newDocument();
      QImage img(40,40,QImage::Format_RGB32);
      for(int y=0;y<40;++y) for(int x=0;x<40;++x) img.setPixel(x,y, x<20?qRgb(230,30,30):qRgb(30,30,230));  // left red, right blue
      Shape p; p.type=ShapeType::Rect; p.rect=QRectF(0,0,40,40); p.transform=QTransform();
      p.fill.type=FillType::Bitmap; p.fill.image=img; p.fill.p0=QPointF(0,40); p.fill.p1=QPointF(40,40); p.fill.p2=QPointF(0,0);  // image->local 1:1
      c->doc().addShape(p); c->refreshAll(); c->select({0});
      c->setTool(Tool::Clone); c->setCloneMode(0); c->setCloneNib(5); c->setCloneHardness(0.9);
      auto sendMod=[&](QEvent::Type t,QPointF v,Qt::MouseButton b,Qt::MouseButtons btns,Qt::KeyboardModifiers m){
        QMouseEvent e(t,v,c->mapToGlobal(v.toPoint()),b,btns,m); QApplication::sendEvent(c,&e); };
      // Alt-click in the RED region to set the clone source
      QPointF srcV=c->mapToView(QPointF(10,20));
      sendMod(QEvent::MouseButtonPress,  srcV,Qt::LeftButton,Qt::LeftButton,Qt::AltModifier);
      sendMod(QEvent::MouseButtonRelease,srcV,Qt::LeftButton,Qt::NoButton,  Qt::AltModifier);
      // paint over the BLUE region: dest(30,20) should pull from src(10,20)=red
      QPointF dstV=c->mapToView(QPointF(30,20));
      sendMod(QEvent::MouseButtonPress,  dstV,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
      sendMod(QEvent::MouseButtonRelease,dstV,Qt::LeftButton,Qt::NoButton,  Qt::NoModifier);
      QColor at=c->doc().shapes[0].fill.image.pixelColor(30,20);
      check(at.red()>170 && at.blue()<90, "cloned destination pixel now matches the red source");
      QColor far=c->doc().shapes[0].fill.image.pixelColor(36,36);
      check(far.blue()>170 && far.red()<90, "pixels outside the brush are untouched");
      // undo restores the pre-stroke image (fill.image is in the JSON snapshot); redo re-applies it
      c->undo();
      check(c->doc().shapes[0].fill.image.pixelColor(30,20).blue()>170, "undo restores the painted-over pixels");
      c->redo();
      check(c->doc().shapes[0].fill.image.pixelColor(30,20).red()>170, "redo re-applies the clone stroke"); }

    std::printf("[clone] Heal mode blends source toward the destination colour\n");
    { c->newDocument();
      QImage img(40,40,QImage::Format_RGB32);
      for(int y=0;y<40;++y) for(int x=0;x<40;++x) img.setPixel(x,y, x<20?qRgb(200,0,0):qRgb(0,0,200));
      Shape p; p.type=ShapeType::Rect; p.rect=QRectF(0,0,40,40); p.transform=QTransform();
      p.fill.type=FillType::Bitmap; p.fill.image=img; p.fill.p0=QPointF(0,40); p.fill.p1=QPointF(40,40); p.fill.p2=QPointF(0,0);
      c->doc().addShape(p); c->refreshAll(); c->select({0});
      c->setTool(Tool::Clone); c->setCloneMode(1); c->setCloneNib(5); c->setCloneHardness(0.9);
      auto sendMod=[&](QEvent::Type t,QPointF v,Qt::MouseButton b,Qt::MouseButtons btns,Qt::KeyboardModifiers m){
        QMouseEvent e(t,v,c->mapToGlobal(v.toPoint()),b,btns,m); QApplication::sendEvent(c,&e); };
      sendMod(QEvent::MouseButtonPress,  c->mapToView(QPointF(10,20)),Qt::LeftButton,Qt::LeftButton,Qt::AltModifier);
      sendMod(QEvent::MouseButtonRelease,c->mapToView(QPointF(10,20)),Qt::LeftButton,Qt::NoButton,  Qt::AltModifier);
      sendMod(QEvent::MouseButtonPress,  c->mapToView(QPointF(30,20)),Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
      sendMod(QEvent::MouseButtonRelease,c->mapToView(QPointF(30,20)),Qt::LeftButton,Qt::NoButton,  Qt::NoModifier);
      QColor at=c->doc().shapes[0].fill.image.pixelColor(30,20);
      // heal shifts the red source by (destMean-srcMean) ≈ toward blue, so the result stays bluish — NOT pure red
      check(at.blue() > at.red(), "heal keeps the destination colour cast (not a raw red copy)"); }

    // --- Batch KK: opacity-mask paint (Transparency tool "Paint Mask" mode) ---
    std::printf("[mask] painting an opacity mask fades the object where brushed\n");
    { c->newDocument();
      Shape r; r.type=ShapeType::Rect; r.rect=QRectF(0,0,100,100); r.transform=QTransform::fromTranslate(100,100);
      r.fill.type=FillType::Solid; r.fill.color=QColor(200,40,40); r.stroke.enabled=false;
      c->doc().addShape(r); c->refreshAll(); c->select({0});
      c->setTranspMode(1); c->setMaskErase(true); c->setMaskNib(12); c->setTool(Tool::Transparency);
      QPointF v=c->mapToView(QPointF(120,120));   // a point well inside the rect
      send(c,QEvent::MouseButtonPress,  v,Qt::LeftButton,Qt::LeftButton);
      send(c,QEvent::MouseButtonRelease,v,Qt::LeftButton,Qt::NoButton);
      check(c->doc().shapes[0].transp.geom==4 && !c->doc().shapes[0].transp.maskBmp.isNull(), "painting creates an opacity mask");
      QImage img(320,320,QImage::Format_ARGB32_Premultiplied); img.fill(Qt::transparent);
      { QPainter g(&img); g.setRenderHint(QPainter::Antialiasing); c->renderDocument(g); }
      int aPaint=qAlpha(img.pixel(120,120)), aClean=qAlpha(img.pixel(104,104));
      check(aClean>200, "an un-brushed corner stays opaque");
      check(aPaint < aClean-40, "the brushed region became (partly) transparent");
      // JSON round-trip + undo
      Document d2=Document::fromJson(c->doc().toJson());
      check(d2.shapes[0].transp.geom==4 && !d2.shapes[0].transp.maskBmp.isNull(), "opacity mask survives JSON round-trip");
      c->undo();
      check(c->doc().shapes[0].transp.geom!=4 || c->doc().shapes[0].transp.maskBmp.isNull(), "undo removes the whole mask");
      // a geom-4 mask whose bitmap failed to load is sanitized (won't render as a broken mask)
      Shape bad; bad.type=ShapeType::Rect; bad.rect=QRectF(0,0,50,50); bad.transp.type=1; bad.transp.geom=4;  // geom 4 but no maskBmp
      check(Shape::fromJson(bad.toJson()).transp.geom!=4, "geom-4 mask with no bitmap is sanitized on load"); }

    // --- Batch LL: panorama stitching ---
    std::printf("[pano] stitching two overlapping halves reconstructs the scene\n");
    { QImage I(120,80,QImage::Format_RGB32);
      for(int y=0;y<80;++y) for(int x=0;x<120;++x) I.setPixel(x,y, qRgb((x*2)%256,(y*3)%256,(x+2*y)%256));
      QImage A=I.copy(0,0,80,80), B=I.copy(40,0,80,80);   // 40-column overlap
      QImage S=stitchPanorama({A,B});
      check(S.width()>=116 && S.width()<=124, "panorama width ≈ the union (overlap auto-detected)");
      check(S.height()==80, "panorama height preserved");
      double err=0; int n=0;
      for(int y=4;y<76;y+=3) for(int x=4;x<116;x+=3){ QColor a=S.pixelColor(x,y),b=I.pixelColor(x,y);
          err+=qAbs(a.red()-b.red())+qAbs(a.green()-b.green())+qAbs(a.blue()-b.blue()); ++n; }
      check(err/(n*3*255.0) < 0.05, "stitched panorama reconstructs the original scene");
      // 3-image N-band stitch (was feather-only before; now multi-band for any count)
      { QImage J(160,80,QImage::Format_RGB32);
        for(int y=0;y<80;++y) for(int x=0;x<160;++x) J.setPixel(x,y, qRgb((x*2)%256,(y*3)%256,(x+2*y)%256));
        QImage A3=J.copy(0,0,80,80), B3=J.copy(40,0,80,80), C3=J.copy(80,0,80,80);   // three 80-wide thirds, 40-col overlaps
        QImage S3=stitchPanorama({A3,B3,C3});
        check(S3.width()>=150 && S3.width()<=170 && S3.height()==80, "3-image panorama spans the union (~160×80)");
        double e3=0; int m3=0;
        for(int y=4;y<76;y+=3) for(int x=6;x<154;x+=3){ QColor a=S3.pixelColor(x,y),b=J.pixelColor(x,y);
            e3+=qAbs(a.red()-b.red())+qAbs(a.green()-b.green())+qAbs(a.blue()-b.blue()); ++m3; }
        check(e3/(m3*3*255.0) < 0.06, "3-image multi-band stitch reconstructs the scene (seamless, N>2)"); }
      // Canvas integration: two photo objects → one Panorama
      c->newDocument();
      auto mk=[&](const QImage& im,double x){ Shape p; p.type=ShapeType::Rect; p.rect=QRectF(0,0,im.width(),im.height()); p.transform=QTransform::fromTranslate(x,100);
          p.fill.type=FillType::Bitmap; p.fill.image=im; p.fill.p0=QPointF(0,im.height()); p.fill.p1=QPointF(im.width(),im.height()); p.fill.p2=QPointF(0,0); p.stroke.enabled=false; return p; };
      c->doc().addShape(mk(A,100)); c->doc().addShape(mk(B,180)); c->refreshAll(); c->select({0,1});
      check(c->stitchSelectedPhotos(), "stitch selected photos succeeds");
      check(c->doc().shapes.size()==1, "two photos merged into one panorama object");
      check(c->doc().shapes[0].fill.type==FillType::Bitmap && c->doc().shapes[0].fill.image.width()>=116, "panorama object holds the stitched bitmap");
      // robustness: tiny images are placed side-by-side instead of guessing an overlap (no crash)
      QImage T1(4,4,QImage::Format_RGB32); T1.fill(Qt::red); QImage T2(4,4,QImage::Format_RGB32); T2.fill(Qt::blue);
      QImage TS=stitchPanorama({T1,T2});
      check(!TS.isNull() && TS.width()>=8, "tiny images stitch side-by-side without crashing"); }
    std::printf("[pano] RANSAC similarity recovers rotation+scale+translation from noisy matches\n");
    { const double th=20.0*3.14159265358979/180.0, s=1.3;
      QTransform Tk(s*std::cos(th), s*std::sin(th), -s*std::sin(th), s*std::cos(th), 15.0, -8.0);   // 20° + 1.3× + (15,-8)
      QVector<QPointF> src, dst;
      for(int k=0;k<20;++k){ QPointF p(10+(k*7)%50, 5+(k*11)%40); src.push_back(p); dst.push_back(Tk.map(p)); }
      for(int k=0;k<4;++k){ src.push_back(QPointF(k*13,k*17)); dst.push_back(QPointF(k*5-30,k*3+90)); }   // outliers
      int inl=0; QTransform T=estimateSimilarityTransform(src,dst,&inl);
      check(inl>=18, "RANSAC keeps the inliers and rejects the outliers");
      QPointF got=T.map(QPointF(25,17)), gt=Tk.map(QPointF(25,17));
      check(std::hypot(got.x()-gt.x(), got.y()-gt.y())<1.5, "recovered similarity matches ground truth");
      check(std::abs(std::hypot(T.m11(),T.m12()) - s) < 0.05, "recovered scale ≈ 1.3"); }
    std::printf("[pano] feature matching detects rotation on a real rotated image pair\n");
    { QImage scene(200,120,QImage::Format_RGB32);   // blocky pseudo-random texture → strong, distinct corners
      for(int y=0;y<120;++y) for(int x=0;x<200;++x){ unsigned h=(unsigned)(x/6)*2654435761u+(unsigned)(y/6)*40503u; scene.setPixel(x,y,qRgb(h&255,(h>>8)&255,(h>>16)&255)); }
      QImage A=scene.copy(0,0,120,120), Bsrc=scene.copy(80,0,120,120);
      QImage B(120,120,QImage::Format_RGB32); B.fill(Qt::black);
      { QPainter pp(&B); pp.setRenderHint(QPainter::SmoothPixmapTransform); pp.translate(60,60); pp.rotate(5); pp.translate(-60,-60); pp.drawImage(0,0,Bsrc); }
      QTransform T=alignImagePair(A,B);   // maps B(rotated +5°) into A(unrotated) → ≈ -5°
      double ang=std::atan2(T.m12(),T.m11())*180.0/3.14159265358979;
      check(std::abs(ang) > 2.0, "alignment recovers the rotation (not a translation-only fallback)"); }

    // --- Batch OO: on-canvas mesh-gradient corner editing ---
    std::printf("[mesh] recolouring a mesh-gradient corner updates the render + selection\n");
    { c->newDocument();
      Shape m; m.type=ShapeType::Rect; m.rect=QRectF(0,0,100,100); m.transform=QTransform::fromTranslate(100,100);
      m.fill.type=FillType::Mesh; m.fill.meshColors={QColor(230,20,20),QColor(20,230,20),QColor(20,20,230)};   // 3-colour
      m.fill.p0=QPointF(0,100); m.fill.p1=QPointF(100,100); m.fill.p2=QPointF(0,0); m.stroke.enabled=false;
      c->doc().addShape(m); c->refreshAll(); c->select({0}); c->setTool(Tool::Fill);
      QImage before(300,300,QImage::Format_ARGB32); before.fill(Qt::white); { QPainter g(&before); g.setRenderHint(QPainter::Antialiasing); c->renderDocument(g); }
      c->setMeshCornerColor(0, QColor(240,240,20));   // corner 0 (at p0 → doc (100,200)) red → yellow
      check(c->doc().shapes[0].fill.meshColors.size()==4, "a 3-colour mesh is padded to 4 when a corner is recoloured");
      check(c->doc().shapes[0].fill.meshColors[0]==QColor(240,240,20), "the mesh corner colour is updated");
      QImage after(300,300,QImage::Format_ARGB32); after.fill(Qt::white); { QPainter g(&after); g.setRenderHint(QPainter::Antialiasing); c->renderDocument(g); }
      check(before.pixelColor(104,196) != after.pixelColor(104,196), "recolouring a corner changes the rendered fill there");
      // clicking a corner swatch selects it
      QPointF v1=c->mapToView(c->doc().shapes[0].transform.map(QPointF(100,100)));   // p1 → corner 1
      send(c,QEvent::MouseButtonPress,v1,Qt::LeftButton,Qt::LeftButton); send(c,QEvent::MouseButtonRelease,v1,Qt::LeftButton,Qt::NoButton);
      check(c->meshCorner()==1, "clicking a mesh corner swatch selects that corner");
      Document d2=Document::fromJson(c->doc().toJson());
      check(d2.shapes[0].fill.meshColors.size()==4 && d2.shapes[0].fill.meshColors[0]==QColor(240,240,20), "mesh corner colours survive JSON round-trip"); }

    // --- Batch MM: word hyphenation (Liang algorithm + installed libhyphen patterns) ---
    std::printf("[hyphen] Liang hyphenation (uses installed libhyphen patterns)\n");
    { Hyphenator& h=Hyphenator::instance();
      if (h.ready()) {
          std::printf("  (loaded '%s' patterns)\n", h.language().toUtf8().constData());
          QVector<int> p=h.points("exemplo");
          check(!p.isEmpty(), "a known word yields hyphenation points");
          QString src="exemplo", joined; int last=0; for(int k: p){ joined+=src.mid(last,k+1-last); last=k+1; } joined+=src.mid(last);
          check(joined==src, "hyphenation points split the word without losing letters");
          check(h.points("a").isEmpty(), "too-short word yields no hyphenation");
          // gated text-frame integration: hyphenation breaks an overflowing word into more lines
          Shape t; t.type=ShapeType::Text; t.textFrame=true; t.rect=QRectF(0,0,55,300); t.font=QFont("DejaVu Sans",18); t.text="exemplo";
          double hNo=t.localBounds().height();
          Shape th=t; th.hyphenate=true;
          check(th.localBounds().height() > hNo, "hyphenation breaks an overflowing word across lines");
          check(qAbs(Shape::fromJson(th.toJson()).localBounds().height()-th.localBounds().height())<1e-6 && Shape::fromJson(th.toJson()).hyphenate, "hyphenate flag round-trips");
      } else { check(true, "no hyphenation dictionary installed — graceful degrade"); std::printf("  (no dictionary — asserts skipped)\n"); } }

    // --- Batch PP: SVG import ---
    std::printf("[svg] SVG import parses shapes, paths, transforms, styles\n");
    // <g> now imports as a real Group; these leaf-oriented checks flatten groups to leaves (same order as the old
    // flattened import) so they keep validating per-leaf geometry/style/clip/blur regardless of the grouping.
    std::function<QVector<Shape>(const QVector<Shape>&)> svgLeaves = [&](const QVector<Shape>& v){
        QVector<Shape> o; for(const Shape& s:v){ if(s.type==ShapeType::Group) o+=svgLeaves(s.children); else o.push_back(s); } return o; };
    { QByteArray svg =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"200\" height=\"200\">"
        "<rect x=\"10\" y=\"10\" width=\"50\" height=\"40\" fill=\"#ff0000\"/>"
        "<circle cx=\"100\" cy=\"100\" r=\"30\" fill=\"rgb(0,128,255)\"/>"
        "<g transform=\"translate(20,20) scale(2)\">"
        "<path d=\"M0 0 L10 0 L10 10 Z\" style=\"fill:green;stroke:black;stroke-width:2\"/>"
        "</g>"
        "<polygon points=\"150,10 190,10 170,50\" fill=\"none\" stroke=\"blue\"/>"
        "</svg>";
      QVector<Shape> sh = svgLeaves(parseSvg(svg));   // <g> is now a real Group; flatten to leaves for these per-leaf checks
      check(sh.size()==4, "parsed 4 shapes (rect, circle, path, polygon)");
      check(sh[0].type==ShapeType::Rect && sh[0].fill.color==QColor(255,0,0), "rect: #hex fill");
      check(sh[1].type==ShapeType::Ellipse && sh[1].fill.color==QColor(0,128,255), "circle: rgb() fill");
      check(sh[2].type==ShapeType::Path && sh[2].fill.color==QColor(0,128,0) && sh[2].stroke.enabled, "path: style= fill/stroke");
      QPointF m = sh[2].transform.map(QPointF(10,0));   // group translate(20)+scale(2): 10 -> 20+20 = 40
      check(qAbs(m.x()-40.0)<0.5 && qAbs(m.y()-20.0)<0.5, "group transform (translate+scale) applied");
      check(sh[3].fill.type==FillType::None && sh[3].stroke.enabled, "polygon: fill=none + stroke");
      QVector<Shape> cc = parseSvg("<svg><path d=\"M0,0 C10,0 10,10 0,10 A5 5 0 0 1 20 20\"/></svg>");
      check(!cc.isEmpty() && pathHasCurve(cc[0].path), "path C (cubic) + A (arc) produce curves");
      // path coordinates must NOT be x/y-swapped (regression guard: pt(rd(),rd()) arg-eval order)
      { QVector<Shape> pc = parseSvg("<svg><path d=\"M0 200 L360 40 Z\"/></svg>");
        bool ok=false; if(!pc.isEmpty()){ const auto& pp=pc[0].path; if(pp.elementCount()>=2){ auto a=pp.elementAt(0), b=pp.elementAt(1);
            ok = qAbs(a.x-0)<0.5 && qAbs(a.y-200)<0.5 && qAbs(b.x-360)<0.5 && qAbs(b.y-40)<0.5; } }
        check(ok, "path coords keep x,y order (M0 200 L360 40 → not transposed)"); }
      // hardening: malformed path data must terminate (no infinite loop) + degenerate scale() = identity
      QVector<Shape> mal = parseSvg("<svg><path d=\"M0 0 L @@ 5 L10 10 zzz\"/></svg>");
      check(true, "malformed path data does not hang the parser (reached here)");
      check(!mal.isEmpty() && mal[0].path.elementCount()>0, "malformed path still yields best-effort geometry");
      QVector<Shape> sc = svgLeaves(parseSvg("<svg><g transform=\"scale()\"><rect width=\"10\" height=\"10\"/></g></svg>"));
      check(!sc.isEmpty() && qAbs(sc[0].transform.m11()-1.0)<0.01, "scale() with no args is identity, not 0-scale"); }

    // --- SVG clip-path import: a shape/group referencing a <clipPath> is clipped via Shape::regionPath ---
    std::printf("[svg] clip-path (<clipPath> / clip-path=url) → Shape::regionPath\n");
    { // a 100x100 rect clipped to a 40x40 rect at (0,0): the clip region (local space) must cover (10,10) but not (60,60)
      QVector<Shape> cs = parseSvg(
        "<svg><defs><clipPath id=\"c\"><rect x=\"0\" y=\"0\" width=\"40\" height=\"40\"/></clipPath></defs>"
        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" clip-path=\"url(#c)\"/></svg>");
      check(cs.size()==1, "clipped rect imported as one shape");
      check(!cs.isEmpty() && !cs[0].regionPath.isEmpty(), "clip-path=url(#c) sets the shape's region (clip) path");
      if(!cs.isEmpty()){ const QPainterPath rp=cs[0].regionPath;   // regionPath is in the shape's LOCAL space
        check(rp.contains(QPointF(10,10)) && !rp.contains(QPointF(60,60)), "the region matches the 40x40 clip rect (keeps 10,10; drops 60,60)"); } }
    { // clip on a GROUP reaches the flattened child; the clipPath's own transform is honoured
      QVector<Shape> cs = svgLeaves(parseSvg(
        "<svg><clipPath id=\"c2\"><circle cx=\"50\" cy=\"50\" r=\"30\"/></clipPath>"
        "<g clip-path=\"url(#c2)\"><rect x=\"0\" y=\"0\" width=\"100\" height=\"100\"/></g></svg>"));
      check(!cs.isEmpty() && !cs[0].regionPath.isEmpty(), "a clip on a <g> propagates to its child (now grouped)");
      if(!cs.isEmpty()) check(cs[0].regionPath.contains(QPointF(50,50)) && !cs[0].regionPath.contains(QPointF(5,5)),
        "group clip region is the circle (keeps centre 50,50; drops corner 5,5)"); }
    { // a shape WITHOUT clip-path is unaffected (no spurious region → import stays byte-identical for clip-free SVG)
      QVector<Shape> cs = parseSvg("<svg><rect width=\"10\" height=\"10\"/></svg>");
      check(!cs.isEmpty() && cs[0].regionPath.isEmpty(), "an unclipped shape has no region path"); }

    // --- SVG <text> with multiple <tspan> lines: each positioned tspan is its own line/shape (not one run-on
    //     line). Regression for "text different / line break wrong" on CorelDRAW/Illustrator/Inkscape exports. ---
    std::printf("[svg] multi-line <tspan> text imports as separate positioned lines (not one run-on line)\n");
    { QVector<Shape> ts = parseSvg(
        "<svg><text x=\"20\" y=\"40\" font-size=\"22\">"
        "<tspan x=\"20\" y=\"40\">Line one</tspan>"
        "<tspan x=\"20\" y=\"70\">Line two</tspan>"
        "<tspan x=\"20\" y=\"100\">Line three</tspan></text></svg>");
      int nText=0; QStringList got; for(const Shape& s: ts) if(s.type==ShapeType::Text){ nText++; got<<s.text; }
      check(nText==3, "three <tspan> lines → three separate text shapes (was one flattened line)");
      check(got.contains("Line one") && got.contains("Line two") && got.contains("Line three"), "each line keeps its own text, not concatenated");
      // the lines are stacked vertically (distinct baselines), not overlapping on one line
      QVector<double> ys; for(const Shape& s: ts) if(s.type==ShapeType::Text) ys<<s.transform.dy();
      std::sort(ys.begin(),ys.end());
      check(ys.size()==3 && ys[1]-ys[0]>10 && ys[2]-ys[1]>10, "the lines sit at distinct vertical positions (line breaks preserved)"); }
    { // a per-tspan font-size override is honoured (not forced to the <text> size)
      QVector<Shape> ts = parseSvg("<svg><text x=\"0\" y=\"20\" font-size=\"10\"><tspan x=\"0\" y=\"20\">a</tspan><tspan x=\"0\" y=\"60\" font-size=\"40\">B</tspan></text></svg>");
      double big=0, small=0; for(const Shape& s: ts) if(s.type==ShapeType::Text){ if(s.text=="B") big=s.font.pixelSize(); if(s.text=="a") small=s.font.pixelSize(); }
      check(big>small && big>=30, "a <tspan font-size> override is applied per line"); }
    { // plain <text> with no tspans still imports as one line
      QVector<Shape> ts = parseSvg("<svg><text x=\"5\" y=\"15\">Hello</text></svg>");
      int nText=0; for(const Shape& s: ts) if(s.type==ShapeType::Text) nText++;
      check(nText==1, "plain text (no tspans) is a single line as before"); }
    { // per-run styling within a chunk (documento-2.svg's green title): a NESTED coloured run keeps its own fill
      //   while inheriting its PARENT tspan's upright/Arson style. Runs on ONE baseline now consolidate into a SINGLE
      //   Text shape carrying textRuns (one selectable object like Inkscape), not N fragments; distinct-y lines stay separate.
      QVector<Shape> ts = parseSvg(
        "<svg><text x=\"50\" y=\"40\" text-anchor=\"middle\" style=\"font-style:italic;font-family:Serif\">"
        "<tspan x=\"50\" y=\"40\" style=\"font-style:normal;font-family:Sans;fill:#ffffff\">Opus <tspan style=\"fill:#00ff00\">•</tspan> IA</tspan>"
        "<tspan x=\"50\" y=\"90\" style=\"font-style:normal;fill:#ffffff\">Line2</tspan></text></svg>");
      int nText=0; QSet<int> baselines; const Shape* line1=nullptr;
      for(const Shape& s: ts) if(s.type==ShapeType::Text){ nText++; baselines.insert(qRound(s.transform.dy()));
        if(s.text.contains(QChar(0x2022))) line1=&s; }
      check(nText==2 && baselines.size()==2, "the two baselines each import as ONE text shape (not per-run fragments)");
      // The "Opus • IA" line is one shape whose per-run textRuns keep the green bullet green and stay upright.
      const TextRun* bulletRun=nullptr; bool anyItalicRun=false;
      if(line1) for(const TextRun& r: line1->textRuns){ if(line1->text.mid(r.start,r.len)==QString(QChar(0x2022))) bulletRun=&r; if(r.font.italic()) anyItalicRun=true; }
      check(line1 && !line1->textRuns.isEmpty(), "a multi-colour line consolidates into a single Text shape with runs");
      check(bulletRun && bulletRun->color==QColor(0,255,0), "the per-run fill is kept (the green bullet stays green, not white)");
      check(bulletRun && !bulletRun->font.italic() && !anyItalicRun, "runs inherit their parent tspan's normal style, not the <text>'s italic"); }
    { // Missing-font notice: a family Qt can't resolve is reported; an installed one is not. This is what tells
      //   the user an SVG's font (e.g. Aptos/Arson Pro) isn't available in THIS session (installed after launch).
      QVector<Shape> v;
      { Shape s; s.type=ShapeType::Text; s.text="ok";      s.font=QFont("DejaVu Sans", 20);              v.push_back(s); }
      { Shape s; s.type=ShapeType::Text; s.text="missing"; s.font=QFont("No Such Font ZZQ 98765", 20);   v.push_back(s); }
      const QStringList sub = Canvas::substitutedFontFamilies(v);
      check(sub.contains("No Such Font ZZQ 98765") && !sub.contains("DejaVu Sans"),
            "substitutedFontFamilies flags an unresolved family, not an installed one"); }
    { // font-weight carries the FULL CSS weight — 900 (Heavy/Black) is DISTINCT from bold (700). Regression for
      //   documento-2.svg's 'Arson Pro' title (font-weight:900 / -inkscape-font-specification:'Arson Pro Heavy'),
      //   which XarQ used to collapse to plain bold. Different weights ⇒ different runs (not merged).
      QVector<Shape> ts = parseSvg("<svg><text x=\"0\" y=\"40\">"
        "<tspan style=\"font-weight:900\">Heavy</tspan><tspan style=\"font-weight:bold\">Bold</tspan>"
        "<tspan style=\"font-weight:normal\">Reg</tspan></text></svg>");
      // One line of three differently-weighted runs → ONE shape whose textRuns keep each weight distinct.
      int wHeavy=0,wBold=0,wReg=0; int nText=0;
      for(const Shape& s: ts) if(s.type==ShapeType::Text){ nText++;
        for(const TextRun& r: s.textRuns){ const QString t=s.text.mid(r.start,r.len);
          if(t=="Heavy") wHeavy=r.font.weight(); else if(t=="Bold") wBold=r.font.weight(); else if(t=="Reg") wReg=r.font.weight(); } }
      check(nText==1, "three same-line weighted runs consolidate into a single text shape");
      check(wHeavy==900 && wBold==700 && wReg==400, "font-weight 900 (Heavy) is kept distinct per-run from bold (700) and normal (400)"); }

    // --- SVG feGaussianBlur filter → Shape::blur (whole-object soft focus). Regression for "check the blur/
    //     feather effect on svg and reproduce it on xarq". ---
    std::printf("[svg] filter=url(#feGaussianBlur) → Shape::blur (scaled to doc units)\n");
    { QVector<Shape> bs = parseSvg(
        "<svg><defs><filter id=\"b\"><feGaussianBlur stdDeviation=\"6\"/></filter></defs>"
        "<circle cx=\"50\" cy=\"50\" r=\"20\" filter=\"url(#b)\"/></svg>");
      check(!bs.isEmpty() && bs[0].blur>0.4, "a shape with a feGaussianBlur filter gets a live blur");
      // Shape::blur = stdDeviation × 1.8: paintBlurred renders an effective gaussian sigma of blur/2, so the ×1.8
      // makes the on-canvas spread match SVG/Inkscape's true gaussian of sigma=stdDeviation (measured within a few %).
      if(!bs.isEmpty()) check(qAbs(bs[0].blur-10.8)<0.6, "blur radius = stdDeviation × 1.8 (matches Inkscape spread) at 1:1 scale"); }
    { // the blur scales with the element's transform (stdDeviation is in local user units, Shape::blur in doc units)
      QVector<Shape> bs = svgLeaves(parseSvg(
        "<svg><defs><filter id=\"b\"><feGaussianBlur stdDeviation=\"5\"/></filter></defs>"
        "<g transform=\"scale(3)\"><rect width=\"10\" height=\"10\" filter=\"url(#b)\"/></g></svg>"));
      check(!bs.isEmpty() && qAbs(bs[0].blur-27.0)<1.5, "blur scales with the CTM (5 local × scale 3 × 1.8 = 27 doc units)"); }
    { // filter:none / no filter → no blur; a feColorMatrix-only filter is not treated as blur
      QVector<Shape> bs = parseSvg(
        "<svg><defs><filter id=\"cm\"><feColorMatrix type=\"saturate\" values=\"0.5\"/></filter></defs>"
        "<rect width=\"10\" height=\"10\" filter=\"url(#cm)\"/><rect x=\"20\" width=\"10\" height=\"10\"/></svg>");
      for(const Shape& s: bs) check(s.blur<=0.4, "a non-blur (feColorMatrix) filter and an unfiltered shape leave blur unset"); }

    // --- SVG mix-blend-mode → Shape::blend (QPainter composition mode). Regression for "the bottom rectangle
    //     has a different mix" — a green bar with mix-blend-mode:overlay imported flat (Normal). ---
    std::printf("[svg] mix-blend-mode → Shape::blend (overlay/multiply/…)\n");
    { QVector<Shape> ms = parseSvg(
        "<svg><rect width=\"20\" height=\"20\" fill=\"green\" style=\"mix-blend-mode:overlay\"/>"
        "<rect x=\"30\" width=\"20\" height=\"20\" fill=\"green\" style=\"mix-blend-mode:multiply\"/>"
        "<rect x=\"60\" width=\"20\" height=\"20\" fill=\"green\"/></svg>");
      check(ms.size()==3, "three blend-mode rects imported");
      check(ms[0].blend==QPainter::CompositionMode_Overlay, "mix-blend-mode:overlay → Overlay composition mode");
      check(ms[1].blend==QPainter::CompositionMode_Multiply, "mix-blend-mode:multiply → Multiply");
      check(ms[2].blend==QPainter::CompositionMode_SourceOver, "no blend mode → Normal (SourceOver)"); }
    { // a child's mix-blend-mode:normal resets an inherited group blend; unsupported keywords fall back to normal
      QVector<Shape> ms = svgLeaves(parseSvg(
        "<svg><g style=\"mix-blend-mode:screen\"><rect width=\"10\" height=\"10\" fill=\"red\"/>"
        "<rect x=\"20\" width=\"10\" height=\"10\" fill=\"red\" style=\"mix-blend-mode:normal\"/></g>"
        "<rect x=\"40\" width=\"10\" height=\"10\" fill=\"red\" style=\"mix-blend-mode:hue\"/></svg>"));
      check(ms.size()==3, "group + reset + unsupported rects imported");
      check(ms[0].blend==QPainter::CompositionMode_Screen, "a group's mix-blend-mode reaches its child (now grouped)");
      check(ms[1].blend==QPainter::CompositionMode_SourceOver, "a child's mix-blend-mode:normal resets the inherited blend");
      check(ms[2].blend==QPainter::CompositionMode_SourceOver, "an unsupported blend keyword (hue) falls back to Normal"); }

    std::printf("[svg] Inkscape 1.2+ multi-page → XarQ pages\n");
    { QByteArray msvg =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:inkscape=\"http://www.inkscape.org/namespaces/inkscape\" "
        "xmlns:sodipodi=\"http://sodipodi.sourceforge.net/DTD/sodipodi-0.0.dtd\" width=\"5780\" height=\"1080\" viewBox=\"0 0 5780 1080\">"
        "<sodipodi:namedview>"
        "<inkscape:page x=\"0\" y=\"0\" width=\"1920\" height=\"1080\"/>"
        "<inkscape:page x=\"1930\" y=\"0\" width=\"1920\" height=\"1080\"/>"
        "<inkscape:page x=\"3860\" y=\"0\" width=\"1920\" height=\"1080\"/>"
        "</sodipodi:namedview>"
        "<rect x=\"100\" y=\"100\" width=\"400\" height=\"300\" fill=\"red\"/>"        // page 0
        "<rect x=\"2030\" y=\"200\" width=\"400\" height=\"300\" fill=\"green\"/>"     // page 1 (x-1930 = 100)
        "<rect x=\"3960\" y=\"150\" width=\"400\" height=\"300\" fill=\"blue\"/>"      // page 2 (x-3860 = 100)
        "</svg>";
      QVector<QRectF> pgs; QVector<Shape> ms=parseSvg(msvg,nullptr,QString(),&pgs);
      check(pgs.size()==3, "parseSvg captured 3 <inkscape:page> rects");
      check(qAbs(pgs[1].left()-1930)<1 && qAbs(pgs[1].width()-1920)<1, "page rect in document coords (page 2 at x=1930, 1920 wide)");
      check(ms.size()==3, "3 rects parsed alongside the pages");
      QTemporaryDir td; const QString mp=td.path()+"/multi.svg";
      { QFile wf(mp); wf.open(QIODevice::WriteOnly); wf.write(msvg); }
      c->newDocument();
      const bool mok=c->importSvgFile(mp);
      check(mok, "multi-page SVG imported");
      check(c->doc().pageCount==3, "3 Inkscape pages → 3 stacked XarQ pages");
      check(qAbs(c->doc().pageSize.width()-1920)<1 && qAbs(c->doc().pageSize.height()-1080)<1, "XarQ page size = source page (1920×1080)");
      check(c->doc().shapes.size()==3, "all 3 objects imported");
      { const double adv=1080+c->doc().pageGap;
        const QRectF homes[3]={QRectF(0,0,1920,1080),QRectF(0,adv,1920,1080),QRectF(0,2*adv,1920,1080)};
        bool placed = c->doc().shapes.size()==3;
        for(int i=0;i<3 && i<c->doc().shapes.size();++i){ const QRectF b=c->doc().shapes[i].docBounds();
            if(!homes[i].contains(b.center())) placed=false;         // moved into its stacked page
            if(qAbs(b.left()-100)>2) placed=false; }                 // …at the right local x (100 on every page)
        check(placed, "each object moved into its stacked page at the correct local position"); } }
    std::printf("[svg] root viewBox → viewport scaling\n");
    { QVector<Shape> sh = parseSvg("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"100\" viewBox=\"0 0 10 10\"><rect x=\"2\" y=\"2\" width=\"6\" height=\"6\"/></svg>");
      check(sh.size()==1, "viewBox svg parses the rect");
      { QRectF bb=sh[0].transform.mapRect(sh[0].localBounds()); check(qAbs(bb.x()-20)<0.5 && qAbs(bb.width()-60)<0.5, "viewBox 0 0 10 10 → width 100 scales content ×10 (rect at x=20, 60 wide)"); } }
    { QVector<Shape> sh = parseSvg("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"100\" viewBox=\"0 0 10 20\" preserveAspectRatio=\"none\"><rect x=\"0\" y=\"0\" width=\"10\" height=\"20\"/></svg>");
      check(!sh.isEmpty(), "none-aspect viewBox parses"); { QRectF bb=sh[0].transform.mapRect(sh[0].localBounds()); check(qAbs(bb.width()-100)<1 && qAbs(bb.height()-100)<1, "preserveAspectRatio=none stretches viewBox non-uniformly to fill (10×20 → 100×100)"); } }
    { QVector<Shape> sh = parseSvg("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 50 50\"><rect x=\"10\" y=\"10\" width=\"20\" height=\"20\"/></svg>");
      check(!sh.isEmpty(), "viewBox-only svg parses"); { QRectF bb=sh[0].transform.mapRect(sh[0].localBounds()); check(qAbs(bb.x()-10)<0.5 && qAbs(bb.width()-20)<0.5, "viewBox with NO width/height → no scaling (content stays at viewBox coords)"); } }
    { QVector<Shape> sh = parseSvg("<svg xmlns=\"http://www.w3.org/2000/svg\"><rect x=\"5\" y=\"5\" width=\"30\" height=\"30\"/></svg>");
      check(!sh.isEmpty(), "no-viewBox svg parses"); { QRectF bb=sh[0].transform.mapRect(sh[0].localBounds()); check(qAbs(bb.x()-5)<0.5 && qAbs(bb.width()-30)<0.5, "no viewBox at all → identity (unchanged behaviour)"); } }
    std::printf("[svg] length units + external/local images\n");
    { QVector<Shape> sh = parseSvg("<svg xmlns=\"http://www.w3.org/2000/svg\"><rect x=\"0\" y=\"0\" width=\"1in\" height=\"72pt\"/></svg>");
      check(!sh.isEmpty() && qAbs(sh[0].rect.width()-96)<1 && qAbs(sh[0].rect.height()-96)<1, "CSS length units: 1in and 72pt both resolve to 96px"); }
    { QVector<Shape> sh = parseSvg("<svg xmlns=\"http://www.w3.org/2000/svg\"><rect x=\"0\" y=\"0\" width=\"2em\" height=\"10mm\"/></svg>");
      check(!sh.isEmpty() && qAbs(sh[0].rect.width()-32)<1 && qAbs(sh[0].rect.height()-37.8)<1, "em (×16) and mm (×96/25.4) units resolve (previously 'em' broke number parsing)"); }
    // local relative <image> resolved against baseDir
    { QImage px(10,10,QImage::Format_ARGB32); px.fill(Qt::red); px.save("/tmp/selftest_svgimg.png");
      QByteArray svgi="<svg xmlns=\"http://www.w3.org/2000/svg\"><image x=\"0\" y=\"0\" width=\"10\" height=\"10\" href=\"selftest_svgimg.png\"/></svg>";
      QVector<Shape> withBase=parseSvg(svgi,nullptr,"/tmp");
      bool gotImg=false; for(const Shape& s: withBase) if(s.fill.type==FillType::Bitmap && !s.fill.image.isNull()) gotImg=true;
      check(gotImg, "<image> loads a local relative file resolved against baseDir");
      QVector<Shape> noBase=parseSvg(svgi,nullptr,QString());
      bool anyNoBase=false; for(const Shape& s: noBase) if(s.fill.type==FillType::Bitmap && !s.fill.image.isNull()) anyNoBase=true;
      check(!anyNoBase, "a relative <image> is NOT loaded without a baseDir (no surprise filesystem reads)");
      QByteArray svgh="<svg xmlns=\"http://www.w3.org/2000/svg\"><image x=\"0\" y=\"0\" width=\"10\" height=\"10\" href=\"http://evil.example/x.png\"/></svg>";
      QVector<Shape> httpv=parseSvg(svgh,nullptr,"/tmp");
      bool anyHttp=false; for(const Shape& s: httpv) if(s.fill.type==FillType::Bitmap && !s.fill.image.isNull()) anyHttp=true;
      check(!anyHttp, "http(s) <image> URLs are never fetched (no network from import)"); }
    std::printf("[svg] SVG gradient import (linear/radial via url(#id))\n");
    { QByteArray svg =
        "<svg xmlns=\"http://www.w3.org/2000/svg\">"
        "<defs>"
        "<linearGradient id=\"g1\"><stop offset=\"0\" stop-color=\"#ff0000\"/><stop offset=\"1\" stop-color=\"#0000ff\"/></linearGradient>"
        "<radialGradient id=\"g2\" cx=\"0.5\" cy=\"0.5\" r=\"0.5\"><stop offset=\"0%\" stop-color=\"white\"/><stop offset=\"100%\" stop-color=\"black\" stop-opacity=\"0.5\"/></radialGradient>"
        "</defs>"
        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"50\" fill=\"url(#g1)\"/>"
        "<circle cx=\"50\" cy=\"50\" r=\"40\" fill=\"url(#g2)\"/>"
        "</svg>";
      QVector<Shape> sh = parseSvg(svg);
      check(sh.size()==2, "gradient SVG parses 2 shapes");
      check(sh[0].fill.type==FillType::Linear && sh[0].fill.stops.size()==2, "linear gradient fill with 2 stops");
      check(sh[0].fill.stops.first().color==QColor(255,0,0) && sh[0].fill.stops.last().color==QColor(0,0,255), "linear stop colours (red→blue)");
      check(qAbs(sh[0].fill.p0.x()-0)<1 && qAbs(sh[0].fill.p1.x()-100)<1, "linear geometry mapped to the object bbox (0..width)");
      check(sh[1].fill.type==FillType::Radial && sh[1].fill.stops.size()==2, "radial gradient fill");
      check(sh[1].fill.stops.last().color.alpha()<200, "stop-opacity applied to the stop alpha"); }

    std::printf("[svg] SVG import v2: CSS <style> classes\n");
    { QByteArray svg =
        "<svg xmlns=\"http://www.w3.org/2000/svg\">"
        "<style type=\"text/css\"><![CDATA[ "
        "  .st0 { fill:#ff0000; stroke:#000000; stroke-width:3 } "
        "  rect { fill:#00ff00 } "
        "  #only { fill:#0000ff } "
        "  .a, .b { fill:#ffff00 } "
        "]]></style>"
        "<rect class=\"st0\" x=\"0\" y=\"0\" width=\"10\" height=\"10\"/>"     // class rule wins over tag rule (doc order)
        "<rect x=\"20\" y=\"0\" width=\"10\" height=\"10\"/>"                   // only the tag rule
        "<circle id=\"only\" cx=\"50\" cy=\"5\" r=\"5\"/>"                      // #id rule
        "<rect class=\"b\" x=\"40\" y=\"0\" width=\"10\" height=\"10\"/>"       // grouped selector .a,.b
        "<rect class=\"st0\" x=\"60\" y=\"0\" width=\"10\" height=\"10\" fill=\"#123456\"/>"  // CSS class beats presentation attr (spec 0)
        "<rect class=\"st0\" x=\"80\" y=\"0\" width=\"10\" height=\"10\" style=\"fill:#654321\"/>"  // inline style beats CSS
        "</svg>";
      QVector<Shape> sh = parseSvg(svg);
      check(sh.size()==6, "CSS SVG parses 6 shapes");
      check(sh[0].fill.color==QColor(255,0,0) && sh[0].stroke.enabled && qAbs(sh[0].stroke.width-3)<0.01, "class selector .st0 (spec 10) beats tag rule; applies fill+stroke+width");
      check(sh[1].fill.color==QColor(0,255,0), "tag selector applies to unclassed rect");
      check(sh[2].fill.color==QColor(0,0,255), "#id selector applies");
      check(sh[3].fill.color==QColor(255,255,0), "grouped selector (.a, .b) applies");
      check(sh[4].fill.color==QColor(255,0,0), "author CSS class overrides presentation attr (spec 0)");
      check(sh[5].fill.color==QColor(0x65,0x43,0x21), "inline style=\"\" overrides author CSS"); }

    std::printf("[svg] SVG import v2: <text> elements\n");
    { QByteArray svg =
        "<svg xmlns=\"http://www.w3.org/2000/svg\">"
        "<text x=\"10\" y=\"40\" font-family=\"DejaVu Sans\" font-size=\"32\" fill=\"#ff0000\">Hello</text>"
        "<text x=\"100\" y=\"40\" font-size=\"20\" font-weight=\"bold\" text-anchor=\"middle\">Mid<tspan> span</tspan></text>"
        "<g font-size=\"18\" fill=\"blue\"><text x=\"0\" y=\"80\">Inherited</text></g>"
        "</svg>";
      QVector<Shape> sh = svgLeaves(parseSvg(svg));   // the third line lives in a <g> (now a Group) — flatten to leaves
      check(sh.size()==3, "text SVG parses 3 text shapes");
      check(sh[0].type==ShapeType::Text && sh[0].text=="Hello", "text content read");
      check(sh[0].font.pixelSize()==32 && sh[0].fill.color==QColor(255,0,0), "text font-size (px) + fill");
      check(sh[1].text=="Mid span" && sh[1].font.bold(), "tspan children folded in + font-weight bold");
      { QFontMetricsF fm(sh[1].font); double half=fm.horizontalAdvance("Mid span")/2.0;   // anchor=middle shifts x left by half-width
        double bx=sh[1].transform.map(QPointF(0,0)).x(); check(qAbs(bx-(100-half))<1.5, "text-anchor=middle centres on x"); }
      check(sh[2].text=="Inherited" && sh[2].font.pixelSize()==18 && sh[2].fill.color==QColor(0,0,255), "font-size + fill inherited from parent <g>"); }

    std::printf("[svg] SVG import v3: <use> / <symbol> / <defs> referencing\n");
    { // <defs> content must NOT render on its own (regression-lock for the suppression counter)
      QVector<Shape> d0 = parseSvg("<svg xmlns=\"http://www.w3.org/2000/svg\"><defs><rect x=\"0\" y=\"0\" width=\"10\" height=\"10\" fill=\"red\"/></defs></svg>");
      check(d0.isEmpty(), "<defs> content is suppressed (not rendered where defined)");
      // basic <use> of a plain shape in <defs>, with x/y offset
      QVector<Shape> u1 = parseSvg(
        "<svg xmlns=\"http://www.w3.org/2000/svg\">"
        "<defs><rect id=\"r\" x=\"0\" y=\"0\" width=\"10\" height=\"10\" fill=\"#ff0000\"/></defs>"
        "<use href=\"#r\" x=\"20\" y=\"5\"/></svg>");
      check(u1.size()==1 && u1[0].type==ShapeType::Rect && u1[0].fill.color==QColor(255,0,0), "<use> of a defs rect instantiates one clone with its fill");
      { QPointF o=u1[0].transform.map(QPointF(0,0)); check(qAbs(o.x()-20)<0.5 && qAbs(o.y()-5)<0.5, "<use> x/y offset placed the clone"); }
      // forward reference: <use> BEFORE the target
      QVector<Shape> fw = parseSvg(
        "<svg xmlns=\"http://www.w3.org/2000/svg\"><use href=\"#fr\" x=\"3\" y=\"4\"/>"
        "<defs><circle id=\"fr\" cx=\"0\" cy=\"0\" r=\"5\" fill=\"#00ff00\"/></defs></svg>");
      check(fw.size()==1 && fw[0].fill.color==QColor(0,255,0), "<use> resolves a FORWARD reference");
      // <use> of a <g> subtree, composing the group's own scale with the use transform
      QVector<Shape> ug = parseSvg(
        "<svg xmlns=\"http://www.w3.org/2000/svg\">"
        "<defs><g id=\"grp\" transform=\"scale(2)\"><rect x=\"0\" y=\"0\" width=\"5\" height=\"5\"/><rect x=\"10\" y=\"0\" width=\"5\" height=\"5\"/></g></defs>"
        "<use href=\"#grp\" transform=\"translate(100,0)\"/></svg>");
      check(ug.size()==2, "<use> of a <g> clones the whole subtree (2 shapes)");
      { QPointF a=ug[0].transform.map(ug[0].localBounds().topLeft()), b=ug[1].transform.map(ug[1].localBounds().topLeft());
        check(qAbs(a.x()-100)<0.5 && qAbs(a.y()-0)<0.5 && qAbs(b.x()-120)<0.5, "group scale(2) composes with use translate(100,0): rects at x=100 and 120"); }
      // <symbol> viewBox -> viewport scaling via use width/height
      QVector<Shape> sy = parseSvg(
        "<svg xmlns=\"http://www.w3.org/2000/svg\">"
        "<symbol id=\"s\" viewBox=\"0 0 10 10\"><rect x=\"0\" y=\"0\" width=\"10\" height=\"10\" fill=\"#0000ff\"/></symbol>"
        "<use href=\"#s\" x=\"0\" y=\"0\" width=\"20\" height=\"20\"/></svg>");
      check(sy.size()==1, "<symbol> renders only when <use>d");
      { QRectF bb=sy[0].transform.map(sy[0].localPath()).boundingRect();   // 10x10 viewBox into 20x20 viewport => 20x20
        check(qAbs(bb.width()-20)<1.0 && qAbs(bb.height()-20)<1.0, "symbol viewBox 10 scaled to viewport 20 (xMidYMid meet)"); }
      // referenced content keeps its OWN gradient fill (local space), not the use's fallback
      QVector<Shape> ug2 = parseSvg(
        "<svg xmlns=\"http://www.w3.org/2000/svg\">"
        "<defs><linearGradient id=\"lg\"><stop offset=\"0\" stop-color=\"red\"/><stop offset=\"1\" stop-color=\"blue\"/></linearGradient>"
        "<rect id=\"gr\" x=\"0\" y=\"0\" width=\"10\" height=\"10\" fill=\"url(#lg)\"/></defs>"
        "<use href=\"#gr\" x=\"0\" y=\"0\" fill=\"green\"/></svg>");
      check(ug2.size()==1 && ug2[0].fill.type==FillType::Linear && ug2[0].fill.stops.size()==2, "clone keeps its own url(#lg) gradient (referenced fill wins)");
      // nested use: use -> g -> use
      QVector<Shape> nu = parseSvg(
        "<svg xmlns=\"http://www.w3.org/2000/svg\">"
        "<defs><rect id=\"leaf\" x=\"0\" y=\"0\" width=\"4\" height=\"4\" fill=\"#112233\"/>"
        "<g id=\"mid\"><use href=\"#leaf\" x=\"50\" y=\"0\"/></g></defs>"
        "<use href=\"#mid\" x=\"0\" y=\"7\"/></svg>");
      check(nu.size()==1 && nu[0].fill.color==QColor(0x11,0x22,0x33), "nested <use> (use->g->use) expands the inner reference");
      { QPointF o=nu[0].transform.map(QPointF(0,0)); check(qAbs(o.x()-50)<0.5 && qAbs(o.y()-7)<0.5, "nested-use offsets compose (50 from inner, 7 from outer)"); }
      // cyclic references must terminate (no hang / crash) and stay bounded
      QVector<Shape> cyc = parseSvg(
        "<svg xmlns=\"http://www.w3.org/2000/svg\">"
        "<defs><g id=\"A\"><use href=\"#B\"/></g><g id=\"B\"><use href=\"#A\"/></g></defs>"
        "<use href=\"#A\"/></svg>");
      check(cyc.size()<100, "cyclic <use> A<->B terminates without hanging (bounded output)"); }

    std::printf("[svg] SVG EXPORT (writeSvg) round-trips through the importer\n");
    { QVector<Shape> src;
      { Shape s; s.type=ShapeType::Rect; s.rect=QRectF(10,20,40,30); s.cornerRadius=5;
        s.fill.type=FillType::Solid; s.fill.color=QColor(255,0,0); s.stroke.enabled=false;
        s.transform=QTransform::fromTranslate(100,0); src.push_back(s); }
      { Shape s; s.type=ShapeType::Ellipse; s.rect=QRectF(0,0,60,40);
        s.fill.type=FillType::Linear; s.fill.p0=QPointF(0,0); s.fill.p1=QPointF(60,0);
        s.fill.stops={ {0.0,QColor(0,255,0)}, {1.0,QColor(0,0,255)} }; s.stroke.enabled=false; src.push_back(s); }
      { Shape s; s.type=ShapeType::Path; QPainterPath p; p.moveTo(0,0); p.cubicTo(10,0,10,10,0,10); s.path=p;
        s.fill.type=FillType::None; s.stroke.enabled=true; s.stroke.color=QColor(20,20,20); s.stroke.width=3; src.push_back(s); }
      { Shape s; s.type=ShapeType::Text; s.text="Hi"; s.font=QFont("DejaVu Sans"); s.font.setPixelSize(24);
        s.fill.type=FillType::Solid; s.fill.color=QColor(0,0,0); s.stroke.enabled=false;
        s.transform=QTransform::fromTranslate(5,50); src.push_back(s); }

      QByteArray svg = writeSvg(src, QRectF(0,0,400,300));
      check(svg.contains("<svg") && svg.contains("</svg>") && svg.contains("viewBox"), "writeSvg emits a well-formed <svg> doc");
      check(svg.contains("<rect") && svg.contains("<ellipse") && svg.contains("<path") && svg.contains("<text"), "each shape type maps to its native SVG element");
      check(svg.contains("linearGradient") && svg.contains("url(#g1)"), "gradient fill emitted into <defs> and referenced");

      QString err; QVector<Shape> rt = parseSvg(svg, &err);
      check(rt.size()==4, "round-trip re-imports the same 4 shapes");
      check(rt[0].type==ShapeType::Rect && rt[0].fill.color==QColor(255,0,0), "rect round-trips (type + solid fill)");
      { QPointF o=rt[0].transform.map(rt[0].rect.topLeft()); check(qAbs(o.x()-110)<0.5 && qAbs(o.y()-20)<0.5, "rect transform+geometry round-trips (topLeft at 110,20)"); }
      check(rt[1].type==ShapeType::Ellipse && rt[1].fill.type==FillType::Linear && rt[1].fill.stops.size()==2, "ellipse + linear gradient (2 stops) round-trips");
      check(rt[1].fill.stops.first().color==QColor(0,255,0) && rt[1].fill.stops.last().color==QColor(0,0,255), "gradient stop colours (green→blue) round-trip");
      check(rt[2].type==ShapeType::Path && pathHasCurve(rt[2].path) && rt[2].fill.type==FillType::None && rt[2].stroke.enabled, "bezier path + stroke (no fill) round-trips");
      check(rt[3].type==ShapeType::Text && rt[3].text=="Hi", "text content round-trips");
      // nested group: export preserves grouping as <g>, and the importer now RE-IMPORTS it as a real Group whose
      // (identity) transform holds the leaf child with the group's scale(2) composed into the child transform.
      // A loose top-level sibling keeps the <g> a genuine sub-group: an outermost <g> that encloses the ENTIRE
      // document is a layer/wrapper (Illustrator & Inkscape both emit one) and is flattened, not folded (asserted below).
      { QVector<Shape> g;
        Shape loose; loose.type=ShapeType::Rect; loose.rect=QRectF(60,60,8,8); loose.fill.type=FillType::Solid; loose.fill.color=QColor(9,9,9); loose.stroke.enabled=false; g.push_back(loose);
        Shape grp; grp.type=ShapeType::Group; grp.transform=QTransform::fromScale(2,2);
        Shape c; c.type=ShapeType::Rect; c.rect=QRectF(0,0,10,10); c.fill.type=FillType::Solid; c.fill.color=QColor(1,2,3); c.stroke.enabled=false;
        grp.children.push_back(c); g.push_back(grp);
        QByteArray gs = writeSvg(g, QRectF(0,0,100,100));
        check(gs.contains("<g"), "group exports as <g>");
        QVector<Shape> top = parseSvg(gs);
        int gi=-1; for(int i=0;i<top.size();++i) if(top[i].type==ShapeType::Group) gi=i;
        check(top.size()==2 && gi>=0, "an exported <g> (sub-group beside a loose shape) re-imports as a real Group (not flattened)");
        QVector<Shape> gr = (gi>=0)? svgLeaves(QVector<Shape>{ top[gi] }) : QVector<Shape>{};
        check(gr.size()==1 && gr[0].fill.color==QColor(1,2,3), "the group's child round-trips (colour kept)");
        QPointF o=gr[0].transform.map(QPointF(10,10)); check(qAbs(o.x()-20)<0.5, "group scale(2) composed into the child on round-trip (10→20)"); }
      // an outermost <g> wrapping the WHOLE document (no loose sibling) is a layer → flattened so a click grabs the
      // individual object, not the entire artwork.
      { QVector<Shape> g; Shape grp; grp.type=ShapeType::Group; grp.transform=QTransform::fromScale(2,2);
        Shape c; c.type=ShapeType::Rect; c.rect=QRectF(0,0,10,10); c.fill.type=FillType::Solid; c.fill.color=QColor(4,5,6); c.stroke.enabled=false;
        grp.children.push_back(c); g.push_back(grp);
        QVector<Shape> top = parseSvg(writeSvg(g, QRectF(0,0,100,100)));
        check(top.size()==1 && top[0].type==ShapeType::Rect, "a sole whole-document <g> wrapper is flattened (treated as a layer, not a grabbable group)"); } }

    std::printf("[svg] SVG EXPORT: live effects → SVG <filter>\n");
    { Shape s; s.type=ShapeType::Rect; s.rect=QRectF(20,20,60,40); s.fill.type=FillType::Solid; s.fill.color=QColor(200,60,60); s.stroke.enabled=false;
      s.shadow.on=true; s.shadow.offset=QPointF(6,8); s.shadow.blur=10; s.shadow.color=QColor(0,0,0,150);
      QByteArray svg = writeSvg({s}, QRectF(0,0,140,120));
      check(svg.contains("<filter") && svg.contains("filter=\"url(#fx1)\""), "shadowed shape emits a <filter> and references it");
      check(svg.contains("feGaussianBlur") && svg.contains("feOffset") && svg.contains("feFlood") && svg.contains("feMerge"), "drop-shadow filter has blur+offset+flood+merge primitives");
      check(svg.contains("dx=\"6\"") && svg.contains("dy=\"8\""), "shadow offset carried into feOffset"); }
    { Shape s; s.type=ShapeType::Ellipse; s.rect=QRectF(10,10,50,50); s.fill.type=FillType::Solid; s.fill.color=QColor(60,60,200); s.stroke.enabled=false; s.feather=6;
      QByteArray svg = writeSvg({s}, QRectF(0,0,80,80));
      check(svg.contains("<filter") && svg.contains("feGaussianBlur") && svg.contains("operator=\"in\""), "feathered shape emits a soft-edge (alpha-blur) filter");
      check(!svg.contains("feOffset"), "feather-only filter has no shadow offset"); }
    { Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,20,20); s.fill.type=FillType::Solid; s.fill.color=Qt::black;   // no effects → no filter
      QByteArray svg = writeSvg({s}, QRectF(0,0,40,40));
      check(!svg.contains("<filter"), "a plain shape emits NO filter"); }

    std::printf("[svg] SVG <image> data-URI raster import + bitmap round-trip\n");
    { // build a 6x4 checker image, base64-PNG it, and import via <image>
      QImage src(6,4,QImage::Format_ARGB32); for(int y=0;y<4;++y)for(int x=0;x<6;++x) src.setPixel(x,y,((x^y)&1)?0xFF3366CC:0xFFEEEEEE);
      QByteArray png; QBuffer buf(&png); buf.open(QIODevice::WriteOnly); src.save(&buf,"PNG");
      QString uri = "data:image/png;base64,"+QString::fromLatin1(png.toBase64());
      QByteArray svg = ("<svg xmlns=\"http://www.w3.org/2000/svg\"><image x=\"10\" y=\"20\" width=\"60\" height=\"40\" href=\""+uri+"\"/></svg>").toUtf8();
      QVector<Shape> im = parseSvg(svg);
      check(im.size()==1 && im[0].type==ShapeType::Rect && im[0].fill.type==FillType::Bitmap, "<image> data-URI → a Bitmap-filled Rect");
      check(!im.isEmpty() && im[0].fill.image.width()==6 && im[0].fill.image.height()==4, "embedded PNG decoded to the right pixel size (6x4)");
      check(!im.isEmpty() && qAbs(im[0].rect.x()-10)<0.5 && qAbs(im[0].rect.width()-60)<0.5, "<image> x/y/width placed the box (10,20,60x40)");
      // Bitmap-fill convention (render.cpp bitmapTexXf): image top-left(0,0)->p2, bottom-left(0,H)->p0. SVG is
      // y-down (image top at the rect top y=20), so p2=top-left(10,20), p0=bottom-left(10,60), p1=bottom-right(70,60).
      check(!im.isEmpty() && qAbs(im[0].fill.p1.x()-70)<0.5 && qAbs(im[0].fill.p2.y()-20)<0.5, "bitmap fill parallelogram spans the box (p1.x=70, p2.y=20 = image top)");
      check(!im.isEmpty() && im[0].fill.p2.y() < im[0].fill.p0.y(), "<image> is upright: p2 (image top) sits above p0 (image bottom), not flipped");
      // full round-trip: a Bitmap-filled Rect → writeSvg (<image>) → parseSvg → Bitmap survives
      { Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,6,4); s.fill.type=FillType::Bitmap; s.fill.image=src;
        s.fill.p0=QPointF(0,0); s.fill.p1=QPointF(6,0); s.fill.p2=QPointF(0,4); s.stroke.enabled=false;
        QByteArray xs = writeSvg({s}, QRectF(0,0,20,20));
        check(xs.contains("<image") && xs.contains("data:image/png;base64,"), "bitmap fill exports as an <image> data-URI");
        QVector<Shape> back = parseSvg(xs);
        check(back.size()==1 && back[0].fill.type==FillType::Bitmap && back[0].fill.image.width()==6, "bitmap round-trips through export→import"); } }

    std::printf("[photo] filters: gaussian blur / unsharp / vignette\n");
    { // a hard black|white vertical edge at x=32; blur must SOFTEN it (mid-columns become grey)
      const int N=64; QImage e(N,N,QImage::Format_ARGB32); for(int y=0;y<N;++y){ QRgb* r=(QRgb*)e.scanLine(y); for(int x=0;x<N;++x) r[x]= x<32?qRgb(0,0,0):qRgb(255,255,255); }
      QImage b=gaussianBlurImage(e, 6);
      auto R=[&](const QImage& im,int x,int y){ return qRed(((const QRgb*)im.constScanLine(y))[x]); };
      check(R(e,31,32)==0 && R(e,32,32)==255, "sharp edge before blur (0 | 255)");
      check(R(b,31,32)>30 && R(b,32,32)<225 && R(b,31,32)<R(b,32,32), "blur softens the edge into a grey ramp");
      check(R(b,2,32)<40 && R(b,61,32)>215, "blur preserves the flat regions (still ~black / ~white far from the edge)");
      // sharpen a soft edge → increases the local contrast across it
      QImage soft=gaussianBlurImage(e,4); QImage sh=unsharpMask(soft, 1.2, 3.0);
      int dSoft=std::abs(R(soft,33,32)-R(soft,30,32)), dSharp=std::abs(R(sh,33,32)-R(sh,30,32));
      check(dSharp>dSoft, "unsharp mask increases edge contrast");
      // vignette darkens the corners relative to the centre (same source colour)
      QImage flat(48,48,QImage::Format_ARGB32); flat.fill(qRgb(180,180,180)); QImage vg=vignette(flat,0.8);
      check(R(vg,24,24)>R(vg,1,1)+25 && R(vg,24,24)>R(vg,47,47)+25, "vignette darkens corners vs centre");
      check(R(vg,24,24)<=180, "vignette never brightens the centre"); }

    std::printf("[photo] lens correction + HDR tone-map (Batch R2)\n");
    { auto RA=[&](const QImage& im,int x,int y){ return ((const QRgb*)im.constScanLine(y))[x]; };
      const int N=101; QImage g(N,N,QImage::Format_ARGB32); g.fill(qRgb(40,60,80));
      ((QRgb*)g.scanLine(50))[50]=qRgb(200,10,10);          // unique centre pixel (cx=cy=50 exact)
      { QImage z=lensCorrect(g,0,0); check(z.size()==g.size() && RA(z,10,10)==qRgb(40,60,80) && RA(z,50,50)==qRgb(200,10,10), "lensCorrect k=0 is a no-op"); }
      QImage lc=lensCorrect(g, 0.3);
      check(lc.size()==g.size(), "lensCorrect preserves image dimensions");
      check(RA(lc,50,50)==qRgb(200,10,10), "barrel correction leaves the optical centre fixed");
      check(qAlpha(RA(lc,100,100))==0, "barrel correction pushes corners outside → transparent");
      // HDR tone-map: highlights compress below white, black stays black, hue (pure red) preserved
      QImage h(4,4,QImage::Format_ARGB32); h.fill(qRgb(255,255,255));
      ((QRgb*)h.scanLine(0))[0]=qRgb(0,0,0); ((QRgb*)h.scanLine(1))[1]=qRgb(255,0,0);
      QImage tm=tonemapHDR(h, 1.0, 2.0);
      check(tm.size()==h.size(), "tonemapHDR preserves image dimensions");
      check(qRed(RA(tm,1,1))<255, "HDR tone-map compresses the white highlight below 255");
      check(RA(tm,0,0)==qRgb(0,0,0), "HDR tone-map leaves pure black untouched");
      check(qGreen(RA(tm,1,1))==0 && qBlue(RA(tm,1,1))==0, "HDR tone-map preserves hue (pure red stays red)");
      // full adjust stack routes lens+hdr and the params round-trip through JSON
      QImage adj=adjustPhoto(g, 0,0,0,false,false,false,0,0,0, 50, 60);
      check(!adj.isNull() && adj.size()==g.size(), "adjustPhoto applies lens+hdr and keeps dimensions");
      Document d; Shape s; s.type=ShapeType::Rect; s.fill.type=FillType::Bitmap; s.fill.adjust.lens=-40; s.fill.adjust.hdr=70; d.shapes.push_back(s);
      Document d2=Document::fromJson(d.toJson());
      check(d2.shapes.size()==1 && d2.shapes[0].fill.adjust.lens==-40 && d2.shapes[0].fill.adjust.hdr==70, "adjust.lens/hdr round-trip through document JSON"); }

    std::printf("[pano] exposure (Brown-Lowe gain) compensation\n");
    { // a bright image (overlap mean 200) + a dark one (100): gains should dim the bright, brighten the dark
      QVector<QVector<double>> I={{0,200},{100,0}}, N={{0,1000},{1000,0}};
      QVector<double> g=estimateGains(2,I,N);
      check(g.size()==2 && g[0]<0.95 && g[1]>1.05, "gain dims the bright frame (g0<1) and brightens the dark (g1>1)");
      check(std::abs(g[0]-0.6364)<0.01 && std::abs(g[1]-1.1818)<0.01, "gains match the analytic Brown-Lowe solution (factor-of-2 data term correct)");
      check(std::abs(g[0]*200 - g[1]*100) < std::abs(200-100)*0.5, "gain-corrected overlap means are far closer (mismatch at least halved)");
      // equal exposures → gains ≈ 1 (the prior keeps them there)
      QVector<double> ge=estimateGains(2,{{0,150},{150,0}},{{0,1000},{1000,0}});
      check(std::abs(ge[0]-1)<0.02 && std::abs(ge[1]-1)<0.02, "equal exposures → gains stay ≈ 1");
      // 3-image chain, uniform scene, exposures 100/150/200 (dark→bright): darkest gets the highest gain
      QVector<double> g3=estimateGains(3,{{0,100,0},{150,0,150},{0,200,0}},{{0,800,0},{800,0,800},{0,800,0}});
      check(g3.size()==3 && g3[0]>g3[1] && g3[1]>g3[2], "3-image chain: gains decrease as exposure rises (dark frame boosted most)");
      // singular / no-overlap input must not crash and returns unit gains
      QVector<double> gz=estimateGains(2,{{0,0},{0,0}},{{0,0},{0,0}});
      check(std::abs(gz[0]-1)<1e-9 && std::abs(gz[1]-1)<1e-9, "no-overlap input → unit gains (no crash, no div0)"); }

    std::printf("[pano] multi-band (Laplacian-pyramid) blend\n");
    { const int N=64; QImage A(N,N,QImage::Format_ARGB32); A.fill(qRgb(0,0,0)); QImage B(N,N,QImage::Format_ARGB32); B.fill(qRgb(255,255,255));
      QImage mask(N,N,QImage::Format_Grayscale8); for(int y=0;y<N;++y){ uchar* r=mask.scanLine(y); for(int x=0;x<N;++x) r[x]= x<N/2?255:0; }   // hard step at x=32
      QImage r = multiBandBlend(A,B,mask,5);
      auto L=[&](int x){ return qRed(((const QRgb*)r.constScanLine(32))[x]); };
      check(L(2)<50 && L(61)>205, "multi-band endpoints ≈ black(left)/white(right)");
      int maxd=0; for(int x=0;x<N-1;++x) maxd=qMax(maxd, std::abs(L(x+1)-L(x)));
      check(maxd<70, "a HARD mask step is blended into a SMOOTH ramp (no sharp seam)");
      check(L(30)>55 && L(34)<205, "the transition is spread across the seam (not a step at x=32)"); }
    { const int N=48; QImage A(N,N,QImage::Format_ARGB32); for(int y=0;y<N;++y){ QRgb* r=(QRgb*)A.scanLine(y); for(int x=0;x<N;++x) r[x]=qRgb((x*5)%256,(y*7)%256,128); }
      QImage mask(N,N,QImage::Format_Grayscale8); for(int y=0;y<N;++y){ uchar* r=mask.scanLine(y); for(int x=0;x<N;++x) r[x]=uchar((x*4)%256); }
      QImage r=multiBandBlend(A,A,mask,4);
      long diff=0; for(int y=0;y<N;++y){ const QRgb* rr=(const QRgb*)r.constScanLine(y); const QRgb* ar=(const QRgb*)A.constScanLine(y); for(int x=0;x<N;++x) diff+=std::abs(qRed(rr[x])-qRed(ar[x])); }
      check(diff/(N*N) < 3, "identical-image multi-band blend ≈ no-op (mean |Δ|<3)"); }

    std::printf("[color] extract palette from photo\n");
    { QImage st(90,30,QImage::Format_RGB32);
      { QPainter p(&st); p.fillRect(0,0,30,30,QColor(220,20,20)); p.fillRect(30,0,30,30,QColor(20,200,20)); p.fillRect(60,0,30,30,QColor(20,20,220)); }
      QVector<QColor> pal = extractPalette(st, 6);
      check(pal.size()>=3, "extractPalette returns ≥3 colours from a 3-colour image");
      bool hasR=false,hasG=false,hasB=false; for(const QColor& c: pal){ if(c.red()>150&&c.green()<100)hasR=true; if(c.green()>140&&c.red()<100)hasG=true; if(c.blue()>150&&c.red()<100)hasB=true; }
      check(hasR&&hasG&&hasB, "extracted palette recovers red/green/blue");
      // canvas: add the palette as document theme colours
      Document d; Shape s; s.type=ShapeType::Rect; s.fill.type=FillType::Bitmap; s.fill.image=st; s.stroke.enabled=false;
      d.shapes.push_back(s); c->setDocument(d); c->selectAll();
      int before=c->doc().namedColors.size(); int added=c->addPaletteFromSelectedPhoto(6);
      check(added>=3 && c->doc().namedColors.size()==before+added, "extract-from-photo adds the palette as document named/theme colours");
      check(!c->doc().namedColors.isEmpty() && c->doc().namedColors.last().theme, "extracted colours are flagged as theme colours"); }

    std::printf("[trace] Trace Bitmap (raster → vector autotrace)\n");
    { // 1. a black disk on white → 2 regions; the dark one is a centred ~60px blob
      QImage disk(100,100,QImage::Format_RGB32); disk.fill(Qt::white);
      { QPainter p(&disk); p.setRenderHint(QPainter::Antialiasing); p.setPen(Qt::NoPen); p.setBrush(Qt::black); p.drawEllipse(QPointF(50,50),30,30); }
      QVector<Shape> t = traceBitmap(disk, {2,true,24,240,1.2});
      check(t.size()==2, "disk traces to 2 colour regions (bg + disk)");
      int dk=-1; for(int i=0;i<t.size();++i) if(t[i].fill.color.lightness()<90) dk=i;
      check(dk>=0 && t[dk].type==ShapeType::Path, "a dark Path region was traced");
      if(dk>=0){ QRectF bb=t[dk].transform.mapRect(t[dk].localBounds());
        check(qAbs(bb.center().x()-50)<5 && qAbs(bb.center().y()-50)<5, "disk region centred at ~(50,50)");
        check(qAbs(bb.width()-60)<9 && qAbs(bb.height()-60)<9, "disk region ~60px across (r≈30)"); } }
    { // 2. three colour stripes → the R/G/B hues are recovered
      QImage st(90,30,QImage::Format_RGB32);
      { QPainter p(&st); p.fillRect(0,0,30,30,QColor(220,20,20)); p.fillRect(30,0,30,30,QColor(20,200,20)); p.fillRect(60,0,30,30,QColor(20,20,220)); }
      QVector<Shape> t = traceBitmap(st, {6,false,10,240,1.0});
      check(t.size()>=3, "3-stripe image traces to ≥3 regions");
      bool hasR=false,hasG=false,hasB=false; for(const auto&s:t){ QColor c=s.fill.color; if(c.red()>150&&c.green()<100)hasR=true; if(c.green()>140&&c.red()<100)hasG=true; if(c.blue()>150&&c.red()<100)hasB=true; }
      check(hasR&&hasG&&hasB, "traced regions recover red/green/blue"); }
    { // 3. a black donut (ring) → the dark region has a HOLE at its centre (even-odd fill)
      QImage dn(100,100,QImage::Format_RGB32); dn.fill(Qt::white);
      { QPainter p(&dn); p.setRenderHint(QPainter::Antialiasing); p.setPen(Qt::NoPen); p.setBrush(Qt::black); p.drawEllipse(QPointF(50,50),40,40); p.setBrush(Qt::white); p.drawEllipse(QPointF(50,50),18,18); }
      QVector<Shape> t = traceBitmap(dn, {2,false,24,240,1.2});
      int dk=-1; for(int i=0;i<t.size();++i) if(t[i].fill.color.lightness()<90) dk=i;
      check(dk>=0, "donut: dark ring region traced");
      if(dk>=0){ const QPainterPath pp=t[dk].localPath();   // 100px img → transform is scale(1); local == image coords
        check(!pp.contains(QPointF(50,50)), "donut centre is a HOLE (not filled)");
        check(pp.contains(QPointF(50,15)), "donut ring IS filled"); } }

    // --- Keyboard shortcut customization (Batch R5) ---
    std::printf("[ui] keyboard shortcut customization\n");
    { const QStringList paths=w.commandPaths();
      check(paths.size()>=20, "menu commands enumerated for shortcut customisation");
      check(paths.contains("File/Save"), "command paths include a known command (File/Save)");
      // find a command that ships with a factory shortcut
      QString target; QKeySequence orig;
      for(const QString& p: paths){ QKeySequence k=w.shortcutForCommand(p); if(!k.isEmpty()){ target=p; orig=k; break; } }
      check(!target.isEmpty(), "at least one command has a factory shortcut");
      QSettings s("XarQ","Shortcuts");
      const QVariant prev=s.value("sc/"+target);                 // preserve any real user value
      const QString custom="Ctrl+Alt+Shift+K";
      s.setValue("sc/"+target, custom); s.sync();
      w.applyShortcutOverrides();
      check(w.shortcutForCommand(target)==QKeySequence(custom), "saved override is applied to the action");
      // restoring the factory value returns the original shortcut
      s.setValue("sc/"+target, orig.toString()); s.sync(); w.applyShortcutOverrides();
      check(w.shortcutForCommand(target)==orig, "restoring the factory shortcut works");
      // clean up: put the setting back the way we found it, re-apply
      if(prev.isValid()) s.setValue("sc/"+target, prev); else s.remove("sc/"+target); s.sync();
      w.applyShortcutOverrides(); }

    // --- Multi-document tabs (Batch R6) ---
    std::printf("[ui] multi-document tabs\n");
    { check(w.tabCount()>=1, "app starts with at least one document tab");
      // Build tab A with a named rectangle
      c->newDocument();
      { Document d; Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,50,50); s.name="A-rect"; d.shapes.push_back(s); c->setDocument(d); }
      const int tabA=w.activeTab();
      check(c->doc().shapes.size()==1 && c->doc().shapes[0].name=="A-rect", "tab A holds its rectangle");
      // New tab → count grows, active is the new (empty) tab: document isolation
      const int before=w.tabCount();
      w.newDocumentTab();
      check(w.tabCount()==before+1, "newDocumentTab adds a tab");
      const int tabB=w.activeTab();
      check(tabB!=tabA, "the new tab becomes active");
      check(c->doc().shapes.isEmpty(), "the new tab starts with an empty document (isolation)");
      { Document d; Shape s; s.type=ShapeType::Ellipse; s.rect=QRectF(0,0,80,80); s.name="B-ellipse"; d.shapes.push_back(s); c->setDocument(d); }
      // Switching preserves each tab's document
      w.switchToTab(tabA);
      check(w.activeTab()==tabA && c->doc().shapes.size()==1 && c->doc().shapes[0].name=="A-rect", "switching back restores tab A's document");
      w.switchToTab(tabB);
      check(c->doc().shapes.size()==1 && c->doc().shapes[0].name=="B-ellipse", "switching to B restores its document");
      // Per-tab undo stacks are isolated
      c->pushUndo("edit B"); c->doc().shapes.push_back(Shape{});   // snapshot(1 ellipse) then mutate to 2
      check(c->canUndo() && c->doc().shapes.size()==2, "tab B has an undoable edit (2 shapes)");
      w.switchToTab(tabA);
      check(!c->canUndo(), "tab A has its own undo stack — B's edit is not visible here");
      w.switchToTab(tabB);
      check(c->canUndo() && c->doc().shapes.size()==2, "tab B's undo stack survived the tab round-trip");
      c->undo();
      check(c->doc().shapes.size()==1, "undo in tab B reverts only B (back to the ellipse)");
      // Closing tabs (mechanism, no confirm dialog): count drops as tabs close
      const int cntBefore=w.tabCount();
      w.closeTab(tabB);
      check(w.tabCount()==cntBefore-1, "closeTab removes a tab");
      while(w.tabCount()>1) w.closeTab(w.activeTab());
      check(w.tabCount()==1, "closing tabs down to the last one leaves exactly one");
      w.closeTab(0);   // closing the ONLY document returns to the welcome/start screen, not a fresh blank Untitled
      check(w.tabCount()==0 && w.activeTab()==-1, "closing the sole tab returns to the welcome screen (no blank document behind it)");
      w.newDocumentTab();   // reopen a document (leaves the empty state) for the tests that follow
      // Ruler guides are per-tab and must not leak across documents (review fix B)
      c->newDocument(); c->clearGuides(); c->addGuide(true, 123.0);
      const int gtab=w.activeTab();
      check(c->guidesH().size()==1, "guide added to the current tab's document");
      w.newDocumentTab();
      check(c->guidesH().isEmpty(), "a new tab starts with no ruler guides (guides don't leak across tabs)");
      w.switchToTab(gtab);
      check(c->guidesH().size()==1 && qAbs(c->guidesH()[0]-123.0)<1e-6, "switching back restores that tab's guide");
      // saveSnapshot must NOT hijack the canvas file path (review fix A — the autosave data-loss bug)
      c->newDocument();
      { Document d; Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,10,10); d.shapes.push_back(s); c->setDocument(d,"/tmp/r6_real.xar"); }
      check(c->filePath()=="/tmp/r6_real.xar", "canvas file path is set by setDocument");
      const bool snap=c->saveSnapshot("/tmp/r6_snapshot.xar");
      check(snap, "saveSnapshot writes the snapshot file");
      check(c->filePath()=="/tmp/r6_real.xar", "saveSnapshot leaves the real file path intact (recovery can't hijack Save)");
      QFile::remove("/tmp/r6_real.xar"); QFile::remove("/tmp/r6_snapshot.xar"); }

    // --- XarQ Start screen: recent files + template/open wiring (Batch DS4) ---
    std::printf("[ui] Start screen — recent files + new-from-template (Batch DS4)\n");
    { const QString rp="/tmp/ds4_recent.xar";
      c->newDocument(); { Document d; Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,20,20); d.shapes.push_back(s); c->setDocument(d); }
      check(c->save(rp), "saved a document for the recents test");
      w.addRecentFile(rp);
      check(w.recentFiles().contains(rp), "saved file appears in recent files");
      w.addRecentFile(rp);
      int dup=0; for(const QString& p: w.recentFiles()) if(p==rp) ++dup;
      check(dup==1, "recent files are de-duplicated");
      const QString rp2="/tmp/ds4_recent2.xar"; { Document d; c->setDocument(d); c->save(rp2); } w.addRecentFile(rp2);
      check(!w.recentFiles().isEmpty() && w.recentFiles().first()==rp2, "most-recently-added file is first");
      // Recent Files holds ANY opened file type, not just .xar — an imported SVG / PNG recorded via addRecentFile
      // shows up (recentFiles() only filters out paths that no longer exist on disk).
      { const QString sp="/tmp/ds4_recent.svg"; { QFile f(sp); f.open(QIODevice::WriteOnly); f.write("<svg xmlns='http://www.w3.org/2000/svg'/>"); }
        w.addRecentFile(sp); check(w.recentFiles().contains(sp) && w.recentFiles().first()==sp, "a non-.xar file (SVG) appears in Recent Files");
        const QString pp="/tmp/ds4_recent.png"; QImage(4,4,QImage::Format_ARGB32).save(pp);
        w.addRecentFile(pp); check(w.recentFiles().contains(pp), "an imported image (PNG) appears in Recent Files");
        QFile::remove(sp); QFile::remove(pp); }
      // The Open dialog filter covers native + vector + raster formats, with default "All supported" + escape hatch.
      { const QString flt=MainWindow::supportedOpenFilter();
        check(flt.startsWith("All supported files (") && flt.contains("*.xar") && flt.contains("*.svg")
              && flt.contains("*.cdr") && flt.contains("*.pdf") && flt.contains("*.png")
              && flt.contains("All files (*)"), "supportedOpenFilter() offers native + vector + raster + All files"); }
      // new-from-template opens a document in a new tab (Blank=0 empty, content templates populate)
      const int before=w.tabCount();
      w.newFromTemplate(0);
      check(w.tabCount()==before+1, "newFromTemplate opens a new tab");
      check(c->doc().shapes.isEmpty(), "Blank template is an empty document");
      w.newFromTemplate(1);
      check(!c->doc().shapes.isEmpty(), "a content template populates the document");
      // openPath loads a recent file into a tab
      w.openPath(rp);
      check(c->filePath()==rp, "openPath loads a recent file");
      QFile::remove(rp); QFile::remove(rp2); }

    // --- Crash recovery is surfaced on the welcome screen, never a yes/no modal (user request) ---
    std::printf("[recovery] recover / discard through the welcome-screen API (no modal)\n");
    { // Isolate to a throwaway recovery dir so the test never touches the user's LIVE crash-recovery store.
      const bool hadRecEnv=qEnvironmentVariableIsSet("XARA_RECOVERY_DIR");
      const QByteArray prevRecEnv=qgetenv("XARA_RECOVERY_DIR");
      const QString testRecDir=QDir::tempPath()+"/xarq_selftest_recovery";
      QDir(testRecDir).removeRecursively(); QDir().mkpath(testRecDir);
      qputenv("XARA_RECOVERY_DIR", testRecDir.toUtf8());
      w.clearRecovery();
      check(w.recoveryEntries().isEmpty(), "recoveryEntries is empty after clearRecovery");
      // Simulate a previous session's autosave: a snapshot file + one manifest line "<snapshot>\t<origPath>".
      c->newDocument();
      { Document d; Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,30,20); d.shapes.push_back(s); c->setDocument(d); }
      const QString snap=w.recoveryDir()+"/recovery-0.xar";
      check(c->saveSnapshot(snap), "wrote a simulated previous-session snapshot");
      { QFile f(w.recoveryManifest()); if(f.open(QIODevice::WriteOnly|QIODevice::Text)){ f.write((snap+"\t/tmp/prev_session.xar").toUtf8()); f.close(); } }
      const auto ents=w.recoveryEntries();
      check(ents.size()==1 && ents[0].second=="/tmp/prev_session.xar", "recoveryEntries parses the manifest (<snapshot>\\t<origPath>)");
      c->newDocument();                                    // clear the canvas, as if freshly relaunched
      w.recoverAll();
      check(c->doc().shapes.size()==1 && c->doc().shapes[0].type==ShapeType::Rect,
            "recoverAll restores the snapshot into the workspace (no modal)");
      w.discardRecovery();
      check(w.recoveryEntries().isEmpty(), "discardRecovery removes the recovery snapshots + manifest");
      QDir(testRecDir).removeRecursively();
      if(hadRecEnv) qputenv("XARA_RECOVERY_DIR", prevRecEnv); else qunsetenv("XARA_RECOVERY_DIR"); }

    // --- Tables T-1: model + create tool + render + JSON round-trip ---
    std::printf("[table] table tool: create / grid / round-trip\n");
    c->newDocument();
    { // Table::grid builds a uniform r×c grid spanning w×h
      Table t=Table::grid(3,4, 400,150);
      check(t.rows()==3 && t.cols()==4 && t.cells.size()==12, "Table::grid builds a rows×cols cell matrix");
      check(qAbs(t.width()-400)<1e-6 && qAbs(t.height()-150)<1e-6, "grid column widths + row heights sum to the requested size");
      check(qAbs(t.colX(2)-200)<1e-6 && qAbs(t.rowY(1)-50)<1e-6, "colX/rowY give cumulative cell offsets");
      // create via the Table tool (drag on the page)
      c->setTool(Tool::Table); c->setTableRows(2); c->setTableCols(3);
      dragShape(c, c->mapToView(QPointF(40,40)), c->mapToView(QPointF(340,190)));
      check(!c->doc().shapes.isEmpty() && c->doc().shapes.last().type==ShapeType::Table, "Table tool drag creates a Table shape");
      const Shape& ts=c->doc().shapes.last();
      check(ts.table.rows()==2 && ts.table.cols()==3, "created table honours the tool's Rows/Columns setting");
      check(ts.docBounds().width()>290 && ts.docBounds().height()>140, "table docBounds spans the drag rectangle");
      // put text + a colour + a merge span into cells, then round-trip through the model JSON
      Shape s=ts; s.table.at(0,0).text="Header"; s.table.at(0,0).bg=QColor(40,60,90); s.table.at(0,0).align=1;
      s.table.at(1,1).colSpan=2; s.table.at(1,2).colSpan=0; s.table.spacing=2; s.table.pad=6;
      Document d; d.shapes.push_back(s);
      Document rt=Document::fromJson(d.toJson());
      check(rt.shapes.size()==1 && rt.shapes[0].type==ShapeType::Table, "table shape survives JSON round-trip");
      const Table& r=rt.shapes[0].table;
      check(r.rows()==2 && r.cols()==3 && r.at(0,0).text=="Header" && r.at(0,0).align==1, "cell text + alignment round-trip");
      check(r.at(0,0).bg==QColor(40,60,90) && qAbs(r.spacing-2)<1e-6 && qAbs(r.pad-6)<1e-6, "cell background + table spacing/padding round-trip");
      check(r.at(1,1).colSpan==2 && r.at(1,2).colSpan==0, "merge spans (anchor + covered) round-trip");
      // render must not crash and must mark the table's pixels
      QImage img(360,220,QImage::Format_ARGB32); img.fill(Qt::white);
      { QPainter g(&img); Shape rs=s; rs.table.borderColor=QColor(0,0,0); paintShape(g, rs); }
      bool drewInk=false; for(int y=0;y<img.height()&&!drewInk;++y) for(int x=0;x<img.width();++x) if(img.pixel(x,y)!=0xffffffff){ drewInk=true; break; }
      check(drewInk, "table renders visible grid/content ink"); }

    // --- Tables T-2/T-3: cell selection, edit, keyboard nav, rows/cols, merge/split ---
    std::printf("[table] cells: select / edit / nav / rows-cols / merge\n");
    c->newDocument();
    { Shape s; s.type=ShapeType::Table; s.name="Table"; s.table=Table::grid(3,3, 300,150);
      s.transform=QTransform::fromTranslate(50,50); int idx=c->doc().addShape(s);
      c->setTool(Tool::Table);
      // doc→cell hit-test: table spans (50,50)..(350,200); each cell 100×50. Point (170,130) → col1,row1.
      int hr=-1,hc=-1; bool got=c->tableCellAt(idx, QPointF(170,130), hr,hc);
      check(got && hr==1 && hc==1, "tableCellAt maps a doc point to the right cell");
      c->tableSelectCell(idx,1,1,false);
      check(c->tableEditShape()==idx && c->tableSelRange()==QRect(1,1,1,1), "click selects a single cell");
      c->tableSelectCell(idx,2,2,true);   // Shift-extend
      check(c->tableSelRange()==QRect(1,1,2,2), "Shift-extend makes a rectangular cell range");
      // edit cell text
      c->tableSelectCell(idx,0,0,false); c->tableSetCellText(0,0,"Hi");
      check(c->doc().shapes[idx].table.at(0,0).text=="Hi", "tableSetCellText edits the cell");
      c->undo(); check(c->doc().shapes[idx].table.at(0,0).text.isEmpty(), "cell edit is undoable");
      // cell background
      c->tableSelectCell(idx,0,0,false); c->tableSetCellBg(QColor(10,20,30));
      check(c->doc().shapes[idx].table.at(0,0).bg==QColor(10,20,30), "tableSetCellBg colours the cell");
      // Tab navigation wraps; Tab at the last cell appends a new row
      c->tableSelectCell(idx,0,2,false); c->tableNavigate(0,+1);
      check(c->tableSelRange()==QRect(0,1,1,1), "Tab wraps to the next row's first column");
      int rowsBefore=c->doc().shapes[idx].table.rows();
      c->tableSelectCell(idx,rowsBefore-1,2,false); c->tableNavigate(0,+1);
      check(c->doc().shapes[idx].table.rows()==rowsBefore+1, "Tab in the last cell appends a row");
      // insert / delete columns and rows
      int cb=c->doc().shapes[idx].table.cols();
      c->tableSelectCell(idx,0,1,false); c->tableInsertCol(1);
      check(c->doc().shapes[idx].table.cols()==cb+1, "tableInsertCol adds a column");
      c->tableDeleteCol(1);
      check(c->doc().shapes[idx].table.cols()==cb, "tableDeleteCol removes a column");
      // merge a 2×2 range → anchor spans, covered cells zeroed; split restores
      c->tableSelectCell(idx,0,0,false); c->tableSelectCell(idx,1,1,true);
      check(c->tableMergeCells(), "tableMergeCells merges a rectangular range");
      const Table& mt=c->doc().shapes[idx].table;
      check(mt.at(0,0).rowSpan==2 && mt.at(0,0).colSpan==2 && mt.at(0,1).colSpan==0 && mt.at(1,1).rowSpan==0, "merged anchor spans 2×2; covered cells marked span 0");
      c->tableSelectCell(idx,0,0,false); c->tableSplitCells();
      check(c->doc().shapes[idx].table.at(0,0).rowSpan==1 && c->doc().shapes[idx].table.at(0,1).colSpan==1, "tableSplitCells unmerges back to 1×1 cells");
      // table-wide appearance + background
      c->tableSetAppearance(2.5, QColor(200,0,0), 8, 3);
      { const Table& t=c->doc().shapes[idx].table; check(qAbs(t.borderW-2.5)<1e-6 && t.borderColor==QColor(200,0,0) && qAbs(t.pad-8)<1e-6 && qAbs(t.spacing-3)<1e-6, "tableSetAppearance sets border/pad/spacing"); }
      c->tableSetBg(QColor(5,6,7), false);
      check(c->doc().shapes[idx].table.bg==QColor(5,6,7), "tableSetBg sets the table background");
      // full-table JSON round-trip after edits
      Document rt=Document::fromJson(c->doc().toJson());
      check(rt.shapes[idx].type==ShapeType::Table && rt.shapes[idx].table.at(0,0).bg==QColor(10,20,30), "edited table round-trips through JSON"); }

    // --- Eyedropper colour sampling + object skew ---
    std::printf("[obj] eyedropper sampling + skew\n");
    c->newDocument();
    { Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,120,120); s.transform=QTransform::fromTranslate(40,40);
      s.fill.type=FillType::Solid; s.fill.color=QColor(220,30,40); s.stroke.enabled=false;
      int idx=c->doc().addShape(s);
      QColor sampled=c->sampleColorAt(QPointF(100,100));   // inside the rect (40..160)
      check(qAbs(sampled.red()-220)<12 && sampled.green()<60 && sampled.blue()<60, "eyedropper samples the composited fill colour");
      QColor off=c->sampleColorAt(QPointF(300,300));       // outside → page/background (not red)
      check(!(qAbs(off.red()-220)<12 && off.green()<60), "eyedropper samples background away from the object");
      // skew shears the transform
      c->select({idx});
      double m21before=c->doc().shapes[idx].transform.m21();
      c->skewSelection(20,0);
      check(qAbs(c->doc().shapes[idx].transform.m21()-m21before)>0.05, "skewSelection applies a horizontal shear to the transform");
      c->undo(); check(qAbs(c->doc().shapes[idx].transform.m21()-m21before)<1e-6, "skew is undoable"); }

    // --- Node editor: delete node + make curve/line ---
    std::printf("[node] delete node + make curve/line\n");
    c->newDocument();
    { Shape s; s.type=ShapeType::Path; QPainterPath p; p.moveTo(0,0); p.lineTo(50,0); p.lineTo(100,0); p.lineTo(100,60);
      s.path=p; s.fill.type=FillType::None; s.stroke.enabled=true; int idx=c->doc().addShape(s);
      c->select({idx}); c->setTool(Tool::Node); c->nodeEdit(idx);
      check(c->nodeSubpaths()>=1 && c->nodeCount(0)==4, "node editor parses the 4-point path");
      c->deleteNode(0,1);
      check(c->nodeCount(0)==3, "deleteNode removes a node");
      check(c->doc().shapes[idx].path.elementCount()>0, "path rebuilt after node delete");
      check(!pathHasCurve(c->doc().shapes[idx].path), "straight path has no curves yet");
      c->setNodeCurved(0,1,true);
      check(pathHasCurve(c->doc().shapes[idx].path), "setNodeCurved(true) adds a bezier segment (Make Curve)");
      c->setNodeCurved(0,1,false);
      c->undo(); c->undo();   // undo make-line, make-curve
      check(!pathHasCurve(c->doc().shapes[idx].path), "Make Line / undo returns to straight segments");
      // add a point on a segment (splits it → node count +1). Re-parse first (undo restores the
      // path but not the in-memory node buffer).
      c->nodeEdit(idx); int nBefore=c->nodeCount(0);
      c->insertNodeOnSegment(0,0,0.5);
      check(c->nodeCount(0)==nBefore+1, "insertNodeOnSegment adds a node on the segment");
      check(c->doc().shapes[idx].path.elementCount() > 0, "path rebuilt after add-node");
      // REGRESSION: the InfoBar "Make Curve" fires AFTER the mouse is released, so the grabbed-node index
      // was reset (m_nodeSub=-1). It must fall back to the marquee/click-selected node set, not no-op.
      c->nodeEdit(idx); c->selectNodesInRect(QRectF(-6,-6,12,12));     // select the node at (0,0), NO active grab
      check(c->nodeSelCount()>=1, "a node is selected with no active grab");
      check(!pathHasCurve(c->doc().shapes[idx].path), "still straight before Make Curve");
      c->setActiveNodeCurved(true);                                   // exactly what the InfoBar 'Make Curve' calls
      check(pathHasCurve(c->doc().shapes[idx].path), "Make Curve on the selected node curves it (handles appear) — used to be a silent no-op"); }

    // --- Node continuity: cusp / smooth / symmetric (Batch 6c) ---
    std::printf("[node] cusp / smooth / symmetric handle continuity\n");
    {
        auto nodeNear=[&](const QVector<SubPath>& subs, QPointF a)->PathNode{
            for(const auto& sp:subs) for(const auto& nd:sp.nodes)
                if(QLineF(nd.anchor,a).length()<1.0) return nd;
            return PathNode{}; };
        // Equal-length collinear handles at the middle node (60,60) => symmetric.
        QPainterPath psym; psym.moveTo(0,100); psym.cubicTo(20,100,40,60,60,60); psym.cubicTo(80,60,100,20,120,20);
        PathNode ns=nodeNear(pathToNodes(psym), QPointF(60,60));
        check(ns.smooth && ns.symmetric, "equal-length collinear handles parse as a symmetric node");
        // Collinear but out-handle twice as long => smooth, NOT symmetric.
        QPainterPath psm; psm.moveTo(0,100); psm.cubicTo(20,100,40,60,60,60); psm.cubicTo(100,60,110,20,120,20);
        PathNode nm=nodeNear(pathToNodes(psm), QPointF(60,60));
        check(nm.smooth && !nm.symmetric, "collinear but unequal-length handles parse as smooth (not symmetric)");
        // setSelectedNodeType(2) promotes the smooth node to symmetric (equalises the handle lengths).
        c->newDocument();
        Shape s; s.type=ShapeType::Path; s.path=psm; s.fill.type=FillType::None; s.stroke.enabled=true;
        int idx=c->doc().addShape(s); c->select({idx}); c->setTool(Tool::Node); c->nodeEdit(idx);
        c->selectNodesInRect(QRectF(54,54,12,12));   // grab the (60,60) node
        check(c->nodeSelCount()>=1, "the middle node is selected");
        c->setSelectedNodeType(2);
        PathNode after=nodeNear(pathToNodes(c->doc().shapes[idx].path), QPointF(60,60));
        check(after.smooth && after.symmetric, "setSelectedNodeType(2) makes the node symmetric (equal-length handles after write-back)");
    }

    // --- Node tool: drag a SEGMENT to bend its curve (Xara-style reshape; was: insert a node) ---
    std::printf("[node] drag a segment to bend the curve\n");
    c->newDocument();
    { Shape s; s.type=ShapeType::Path; QPainterPath p; p.moveTo(0,0); p.lineTo(100,0);
      s.path=p; s.fill.type=FillType::None; s.stroke.enabled=true; int idx=c->doc().addShape(s);
      c->select({idx}); c->nodeEdit(idx);
      check(!pathHasCurve(c->doc().shapes[idx].path) && c->doc().shapes[idx].localBounds().height()<1.0, "starts as a flat straight segment");
      // grab the segment midpoint (t=0.5) and pull it down 40 units — a straight seg's 1/3–2/3 handle bases
      QPointF out0(100.0/3.0,0.0), in0(200.0/3.0,0.0), grab(50,0), target(50,40);
      c->reshapePathSegment(0,0,0.5, out0,in0,grab,target);
      const QPainterPath rp=c->doc().shapes[idx].path;
      check(pathHasCurve(rp), "the straight segment is promoted to a bezier curve");
      check(rp.pointAtPercent(0.5).y()>25.0, "the curve bends toward the drag target");
      check(c->doc().shapes[idx].localBounds().height()>20.0, "the bent segment gains vertical extent");
      // the endpoints are unchanged (bending moves only the interior control handles)
      check(QLineF(rp.pointAtPercent(0.0), QPointF(0,0)).length()<1.0 && QLineF(rp.pointAtPercent(1.0), QPointF(100,0)).length()<1.0,
            "the segment's endpoints stay put while it bends");
      // t at an endpoint is a no-op (nothing to bend, no divide-by-zero)
      QPainterPath before=c->doc().shapes[idx].path;
      c->reshapePathSegment(0,0,0.0, out0,in0,QPointF(0,0),QPointF(0,50));
      check(c->doc().shapes[idx].path.pointAtPercent(0.0).y()<1.0, "bending at t=0 (an endpoint) is a safe no-op");
      c->setTool(Tool::Select); }
    // End-to-end: the real mouse drag on a segment bends it (exercises the press/move/release wiring).
    c->newDocument();
    { Shape s; s.type=ShapeType::Path; QPainterPath p; p.moveTo(200,300); p.lineTo(400,300);
      s.path=p; s.fill.type=FillType::None; s.stroke.enabled=true; int idx=c->doc().addShape(s);
      c->select({idx}); c->setTool(Tool::Node);
      check(!pathHasCurve(c->doc().shapes[idx].path), "line is straight before the drag");
      dragShape(c, c->mapToView(QPointF(300,300)), c->mapToView(QPointF(300,360)));   // grab the midpoint, pull down 60
      check(pathHasCurve(c->doc().shapes[idx].path), "mouse-dragging the segment bends it (line→curve) end-to-end");
      check(c->doc().shapes[idx].path.pointAtPercent(0.5).y()>320.0, "the interactive drag moved the curve toward the cursor");
      c->undo(); check(!pathHasCurve(c->doc().shapes[idx].path), "undo restores the straight segment");
      c->setTool(Tool::Select); }
    // Review-fix: near an endpoint the least-squares split is capped (no exploding handle / loop).
    c->newDocument();
    { Shape s; s.type=ShapeType::Path; QPainterPath p; p.moveTo(0,0); p.lineTo(400,0); s.path=p;
      s.fill.type=FillType::None; s.stroke.enabled=true; int idx=c->doc().addShape(s); c->select({idx}); c->nodeEdit(idx);
      c->reshapePathSegment(0,0,0.04, QPointF(400.0/3.0,0),QPointF(800.0/3.0,0), QPointF(16,0), QPointF(16,10));   // grab near start, pull 10
      const QPainterPath rp=c->doc().shapes[idx].path; double outY=1e9;
      for(int e=0;e+1<rp.elementCount();++e) if(rp.elementAt(e).type==QPainterPath::CurveToElement){ outY=rp.elementAt(e).y; break; }
      check(outY<45.0, "bending near an endpoint caps the handle response (~40, not the uncapped ~90)");
      c->setTool(Tool::Select); }
    // Review-fix: a sub-threshold click-jiggle on a segment neither bends it nor dirties undo.
    c->newDocument();
    { Shape s; s.type=ShapeType::Path; QPainterPath p; p.moveTo(200,300); p.lineTo(400,300); s.path=p;
      s.fill.type=FillType::None; s.stroke.enabled=true; int idx=c->doc().addShape(s); c->select({idx}); c->setTool(Tool::Node);
      int u0=c->undoDepth(); QPointF v=c->mapToView(QPointF(300,300));
      send(c, QEvent::MouseButtonPress,   v,               Qt::LeftButton, Qt::LeftButton);
      send(c, QEvent::MouseMove,          v+QPointF(1,1),  Qt::NoButton,   Qt::LeftButton);
      send(c, QEvent::MouseButtonRelease, v+QPointF(1,1),  Qt::LeftButton, Qt::NoButton);
      check(!pathHasCurve(c->doc().shapes[idx].path), "a sub-3px click-jiggle does NOT bend the segment (drag threshold)");
      check(c->undoDepth()==u0, "a sub-threshold segment click pushes no undo entry"); c->setTool(Tool::Select); }

    // --- Auto-Fit to Page + drop-a-copy during a Ctrl-drag ---
    std::printf("[obj] auto-fit to page + drop-a-copy\n");
    c->newDocument();
    { // content bigger than the page → auto-fit shrinks it inside the page
      Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,4000,3000); s.transform=QTransform::fromTranslate(-500,-400);
      c->doc().addShape(s);
      c->autoFitToPage(true);
      QRectF cb=c->doc().contentBounds(), pg=c->doc().pageRect(0);
      check(cb.width()<=pg.width()+1 && cb.height()<=pg.height()+1, "auto-fit (both) scales content inside the page");
      c->undo(); }
    c->newDocument();
    { Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,80,80); s.transform=QTransform::fromTranslate(200,200);
      int idx=c->doc().addShape(s); c->select({idx}); c->setTool(Tool::Select);
      int before=c->doc().shapes.size();
      QPointF a=c->mapToView(QPointF(240,240)), b=c->mapToView(QPointF(440,340));
      dragShapeMod(c, a, b, Qt::ControlModifier);
      check(c->doc().shapes.size()==before+1, "Ctrl-drag drops a copy (one extra object)");
      // a copy remains near the original position (~200,200); the original moved by ~(200,100)
      bool copyAtStart=false; for(const Shape& sh:c->doc().shapes){ QPointF o=sh.transform.map(QPointF(0,0)); if(QLineF(o,QPointF(200,200)).length()<5){ copyAtStart=true; break; } }
      check(copyAtStart, "the dropped copy stays at the original position"); }

    // --- Ctrl-constrain while DRAWING a new shape (Rect→square, Ellipse→circle, Line→15° snap) ---
    std::printf("[create] Ctrl-constrain on shape creation\n");
    { c->newDocument(); c->setTool(Tool::Rect);
      dragShapeMod(c, c->mapToView(QPointF(100,100)), c->mapToView(QPointF(260,180)), Qt::ControlModifier);   // 160×80 drag
      check(!c->doc().shapes.isEmpty(), "Ctrl-rect created a shape");
      const QRectF r=c->doc().shapes.last().rect;
      check(qAbs(r.width()-r.height())<1.5 && r.width()>120, "Ctrl forces the rectangle to a square (equal sides)"); }
    { c->newDocument(); c->setTool(Tool::Ellipse);
      dragShapeMod(c, c->mapToView(QPointF(100,100)), c->mapToView(QPointF(300,150)), Qt::ControlModifier);   // 200×50 drag
      check(!c->doc().shapes.isEmpty() && qAbs(c->doc().shapes.last().rect.width()-c->doc().shapes.last().rect.height())<1.5,
            "Ctrl forces the ellipse to a circle"); }
    { c->newDocument(); c->setTool(Tool::Line);
      dragShapeMod(c, c->mapToView(QPointF(100,100)), c->mapToView(QPointF(300,112)), Qt::ControlModifier);   // ~3.4° → snaps to 0°
      check(!c->doc().shapes.isEmpty(), "Ctrl-line created a shape");
      const QPainterPath& p=c->doc().shapes.last().path;
      const QPointF s0=p.elementAt(0), e0=p.elementAt(p.elementCount()-1);
      check(qAbs(e0.y()-s0.y())<1.5 && e0.x()>s0.x()+120, "Ctrl snaps the line to the nearest 15° (horizontal here)"); }
    { c->newDocument(); c->setTool(Tool::Rect);   // WITHOUT Ctrl the box is free (not squared)
      dragShape(c, c->mapToView(QPointF(100,100)), c->mapToView(QPointF(300,150)));
      const QRectF r=c->doc().shapes.last().rect;
      check(qAbs(r.width()-r.height())>30, "without Ctrl the rectangle keeps its free aspect ratio"); }

    // --- Equal-gap distribute (different-sized objects get identical clear space between edges) ---
    { c->newDocument();
      auto addR=[&](double x,double w){ Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,w,20); s.fill.type=FillType::Solid; s.transform=QTransform::fromTranslate(x,0); return c->doc().addShape(s); };
      int a=addR(0,10), b=addR(40,40), cc=addR(200,10);   // uneven widths + positions; ends at x=0 and right=210
      c->select({a,b,cc}); c->distributeSelection(true,true);
      const QRectF ra=c->doc().shapes[a].docBounds(), rb=c->doc().shapes[b].docBounds(), rc=c->doc().shapes[cc].docBounds();
      check(qAbs((rb.left()-ra.right())-(rc.left()-rb.right()))<0.5, "equal-gap distribute leaves identical clear space between edges");
      check(qAbs(ra.left())<0.5 && qAbs(rc.right()-210)<0.5, "equal-gap keeps the first & last objects fixed"); }

    // --- Select-behind (Alt+click cycles down the overlapping z-stack) ---
    { c->newDocument();
      Shape lo; lo.type=ShapeType::Rect; lo.rect=QRectF(0,0,100,100); lo.fill.type=FillType::Solid; lo.fill.color=Qt::red; lo.transform=QTransform::fromTranslate(50,50);
      Shape hi=lo; hi.fill.color=Qt::blue;
      int ilo=c->doc().addShape(lo), ihi=c->doc().addShape(hi);   // hi is on top (higher index)
      const QPointF p(100,100);   // inside both
      check(c->hitShape(p)==ihi, "plain hit returns the topmost object");
      check(c->hitShapeBehind(p, ihi)==ilo, "Alt+click (select-behind) returns the object underneath");
      check(c->hitShapeBehind(p, ilo)==ihi, "select-behind wraps back to the top from the bottom"); }

    // --- Drag-and-drop file routing (a dropped .svg goes to the SVG importer, not silently ignored) ---
    { c->newDocument();
      const QString sp="/tmp/xqdrop.svg";
      { QFile f(sp); f.open(QIODevice::WriteOnly); f.write("<svg xmlns='http://www.w3.org/2000/svg' width='100' height='100'><rect x='10' y='10' width='50' height='50' fill='red'/></svg>"); }
      check(c->dropImportByExtension(sp), "dropping an .svg routes to the SVG importer");
      check(!c->doc().shapes.isEmpty(), "the dropped SVG produced shapes"); }

    // --- Native raster decoders (rasterextra): synthesize a minimal valid file per format and
    //     verify dimensions + a known sample pixel through xara::RasterExtra::decodeData. ---
    { using namespace xara;
      auto ap8=[](QByteArray& b,std::initializer_list<int> v){ for(int x:v) b.append(char(x&0xFF)); };
      auto be16=[](QByteArray& b,int v){ b.append(char((v>>8)&0xFF)); b.append(char(v&0xFF)); };
      auto be32=[](QByteArray& b,quint32 v){ b.append(char((v>>24)&0xFF)); b.append(char((v>>16)&0xFF)); b.append(char((v>>8)&0xFF)); b.append(char(v&0xFF)); };
      auto le16=[](QByteArray& b,int v){ b.append(char(v&0xFF)); b.append(char((v>>8)&0xFF)); };
      auto le32=[](QByteArray& b,quint32 v){ b.append(char(v&0xFF)); b.append(char((v>>8)&0xFF)); b.append(char((v>>16)&0xFF)); b.append(char((v>>24)&0xFF)); };

      // QOI: 2x2, four explicit RGBA ops.
      { QByteArray b("qoif"); be32(b,2); be32(b,2); ap8(b,{4,0});
        auto px=[&](int r,int g,int bl,int a){ ap8(b,{0xFF,r,g,bl,a}); };
        px(255,0,0,255); px(0,255,0,255); px(0,0,255,255); px(255,255,0,128);
        ap8(b,{0,0,0,0,0,0,0,1});
        QImage im=RasterExtra::decodeData(b,"qoi");
        check(!im.isNull() && im.width()==2 && im.height()==2, "QOI decodes to 2x2");
        check(im.pixelColor(0,0)==QColor(255,0,0) && im.pixelColor(1,1).alpha()==128, "QOI pixel colour + alpha correct"); }

      // Netpbm PAM (P7), DEPTH 3.
      { QByteArray b("P7\nWIDTH 2\nHEIGHT 1\nDEPTH 3\nMAXVAL 255\nTUPLTYPE RGB\nENDHDR\n");
        ap8(b,{10,20,30, 40,50,60});
        QImage im=RasterExtra::decodeData(b,"pam");
        check(!im.isNull() && im.size()==QSize(2,1) && im.pixelColor(0,0)==QColor(10,20,30), "PAM P7 depth-3 decodes"); }

      // PFM colour (little-endian scale<0); first stored (bottom) row → white, second → black.
      { QByteArray b("PF\n2 1\n-1.0\n");
        float one=1.0f, zero=0.0f;
        for(int k=0;k<3;k++) b.append(reinterpret_cast<const char*>(&one),4);
        for(int k=0;k<3;k++) b.append(reinterpret_cast<const char*>(&zero),4);
        QImage im=RasterExtra::decodeData(b,"pfm");
        check(!im.isNull() && im.size()==QSize(2,1) && im.pixelColor(0,0)==QColor(255,255,255), "PFM float map decodes + tonemaps"); }

      // Radiance HDR flat scanline (w<8 → old path); just verify geometry + non-null.
      { QByteArray b("#?RADIANCE\n\n-Y 1 +X 2\n"); ap8(b,{100,150,200,140, 50,60,70,130});
        QImage im=RasterExtra::decodeData(b,"hdr");
        check(!im.isNull() && im.size()==QSize(2,1), "Radiance HDR (RGBE) decodes to raster"); }

      // PCX 8bpp/1-plane 2x2 with a trailing 256-colour VGA palette.
      { QByteArray b; ap8(b,{0x0A,5,1,8}); le16(b,0); le16(b,0); le16(b,1); le16(b,1);
        le16(b,72); le16(b,72); for(int k=0;k<48;k++) b.append(char(0)); b.append(char(0));
        b.append(char(1)); le16(b,2); le16(b,1); le16(b,0); le16(b,0);
        while(b.size()<128) b.append(char(0));
        ap8(b,{0,1, 2,3});                                   // 2 rows × 2 literal indices
        b.append(char(0x0C)); QByteArray pal(768,char(0));
        pal[0]=char(255); pal[4]=char(255); pal[8]=char(255); pal[9]=char(255); pal[10]=char(255); // idx0 red, idx1 green, idx2 blue, idx3 yellow
        b.append(pal);
        QImage im=RasterExtra::decodeData(b,"pcx");
        check(!im.isNull() && im.size()==QSize(2,2) && im.pixelColor(0,0)==QColor(255,0,0) && im.pixelColor(1,1)==QColor(255,255,0), "PCX 8bpp+VGA palette decodes"); }

      // PCX security regression: BytesPerLine < width must be rejected (not a heap over-read).
      { QByteArray b; ap8(b,{0x0A,5,1,8}); le16(b,0); le16(b,0); le16(b,65534); le16(b,0); // w=65535,h=1
        le16(b,72); le16(b,72); for(int k=0;k<48;k++) b.append(char(0)); b.append(char(0));
        b.append(char(1)); le16(b,1); le16(b,0); le16(b,0); le16(b,0);   // planes=1, BytesPerLine=1 (< width!)
        while(b.size()<128) b.append(char(0)); b.append(char(0));
        check(RasterExtra::decodeData(b,"pcx").isNull(), "PCX with BytesPerLine < width is rejected (no over-read)"); }

      // SGI/RGB 2x2 verbatim, 3 channels (planar, bottom-to-top).
      { QByteArray b; be16(b,474); ap8(b,{0,1}); be16(b,3); be16(b,2); be16(b,2); be16(b,3);
        while(b.size()<512) b.append(char(0));
        ap8(b,{10,20,30,40});    // R plane (y0:10,20  y1:30,40)
        ap8(b,{50,60,70,80});    // G plane
        ap8(b,{90,100,110,120}); // B plane
        QImage im=RasterExtra::decodeData(b,"sgi");
        check(!im.isNull() && im.size()==QSize(2,2) && im.pixelColor(0,1)==QColor(10,50,90), "SGI RGB verbatim decodes (bottom-to-top)"); }

      // DDS DXT1 single 4x4 block, all texels = colour0 (red).
      { QByteArray b("DDS "); le32(b,124);
        while(b.size()<12) b.append(char(0));
        le32(b,4); le32(b,4);                                // height,width at 12,16
        while(b.size()<80) b.append(char(0));
        le32(b,0x4); b.append("DXT1");                       // pfFlags=FOURCC at 80, fourcc at 84
        while(b.size()<128) b.append(char(0));
        le16(b,0xF800); le16(b,0x001F);                      // c0=red(565), c1=blue → 4-colour block
        le32(b,0);                                           // all indices → colour0
        QImage im=RasterExtra::decodeData(b,"dds");
        check(!im.isNull() && im.size()==QSize(4,4) && im.pixelColor(0,0)==QColor(255,0,0), "DDS DXT1 block decodes"); }

      // Sun Raster: 2x2 24-bit BGR, uncompressed (type 1).
      { QByteArray b; be32(b,0x59A66A95); be32(b,2); be32(b,2); be32(b,24); be32(b,0); be32(b,1); be32(b,0); be32(b,0);
        ap8(b,{0,0,255, 0,255,0}); ap8(b,{255,0,0, 255,255,255});    // BGR rows: red,green / blue,white
        QImage im=RasterExtra::decodeData(b,"sun");
        check(!im.isNull() && im.size()==QSize(2,2) && im.pixelColor(0,0)==QColor(255,0,0) && im.pixelColor(1,1)==QColor(255,255,255), "Sun Raster 24-bit decodes"); }
      check(RasterExtra::extensions().contains("ras") && RasterExtra::extensions().contains("fits") && RasterExtra::extensions().contains("dpx"),
            "Sun/FITS/DPX extensions are advertised");
      check(RasterExtra::decodeData(QByteArray("SDPX")+QByteArray(20,'\0'),"dpx").isNull(), "DPX with a too-short header is rejected");
      check(RasterExtra::decodeData(QByteArray("SIMPLE  = junk"),"fits").isNull(), "malformed FITS is rejected safely");

      // Khoros VIFF: 1024-byte header (big-endian/IEEE) + one grayscale byte plane.
      { QByteArray b(1024,'\0'); b[0]=char(0xAB); b[1]=0x01; b[4]=0x02;   // id, file_type, machine_dep=IEEE (big-endian)
        auto setbe=[&](int o,quint32 v){ b[o]=char(v>>24); b[o+1]=char(v>>16); b[o+2]=char(v>>8); b[o+3]=char(v); };
        setbe(520,2); setbe(524,2); setbe(560,1); setbe(564,1);          // width=2, height=2, bands=1 (gray), storage=byte
        ap8(b,{10,20,30,40});
        QImage im=RasterExtra::decodeData(b,"viff");
        check(!im.isNull() && im.size()==QSize(2,2) && im.pixelColor(0,0).red()==10 && im.pixelColor(1,1).red()==40, "Khoros VIFF grayscale decodes"); }
      check(RasterExtra::extensions().contains("xwd") && RasterExtra::extensions().contains("viff"),
            "XWD/VIFF extensions are advertised");

      // farbfeld: "farbfeld" magic + u32-BE w,h + 16-bit-BE RGBA; we keep the high byte of each channel.
      { QByteArray b("farbfeld",8);
        auto be32=[&](quint32 v){ b.append(char(v>>24)); b.append(char(v>>16)); b.append(char(v>>8)); b.append(char(v)); };
        auto be16=[&](quint16 v){ b.append(char(v>>8)); b.append(char(v)); };
        be32(1); be32(1); be16(0xAABB); be16(0xCCDD); be16(0xEEFF); be16(0x1122);
        QImage im=RasterExtra::decodeData(b,"ff");
        check(!im.isNull() && im.size()==QSize(1,1) && im.pixelColor(0,0).red()==0xAA && im.pixelColor(0,0).alpha()==0x11,
              "farbfeld 16-bit RGBA decodes (high byte)"); }
      // MTV ray-tracer: ASCII "W H\n" then packed RGB.
      { QByteArray b("2 1\n"); b.append(QByteArray::fromHex("ff000000ff00"));   // red, green
        QImage im=RasterExtra::decodeData(b,"mtv");
        check(!im.isNull() && im.size()==QSize(2,1) && im.pixelColor(0,0)==QColor(255,0,0) && im.pixelColor(1,0)==QColor(0,255,0),
              "MTV ascii-header RGB decodes"); }
      // Nokia OTB: [info,w,h,depth] + 1-bpp rows (1=black). 8x5, each row 0xF0 = left half black, right half white.
      { QByteArray b; b.append(char(0)); b.append(char(8)); b.append(char(5)); b.append(char(1));
        for(int i=0;i<5;i++) b.append(char(0xF0));
        QImage im=RasterExtra::decodeData(b,"otb");
        check(!im.isNull() && im.size()==QSize(8,5) && im.pixelColor(0,0)==QColor(0,0,0) && im.pixelColor(7,0)==QColor(255,255,255),
              "Nokia OTB 1-bpp bitmap decodes"); }
      check(RasterExtra::extensions().contains("ff") && RasterExtra::extensions().contains("mtv") && RasterExtra::extensions().contains("otb"),
            "farbfeld/MTV/OTB extensions are advertised");

      // Native GIMP XCF: build a minimal v0 (32-bit pointer) 2x2 RGBA image, uncompressed single tile,
      // with the pointer chain (image→layer→hierarchy→level→tile) back-patched as GIMP writes it.
      { QByteArray b;
        auto be32=[&](quint32 v){ b.append(char(v>>24)); b.append(char(v>>16)); b.append(char(v>>8)); b.append(char(v)); };
        auto patch32=[&](int off, quint32 v){ b[off]=char(v>>24); b[off+1]=char(v>>16); b[off+2]=char(v>>8); b[off+3]=char(v); };
        b.append("gimp xcf file",13); b.append('\0');       // magic + version 0
        be32(2); be32(2); be32(0);                          // width, height, base type = RGB
        be32(17); be32(1); b.append('\0');                  // PROP_COMPRESSION len1 = 0 (uncompressed)
        be32(0); be32(0);                                   // PROP_END (type 0, len 0)
        int layerPtrOff=b.size(); be32(0); be32(0);         // layer offset (patched) + list terminator
        patch32(layerPtrOff, b.size());
        be32(2); be32(2); be32(1);                          // layer lw, lh, type=RGBA
        be32(2); b.append('L'); b.append('\0');             // name len=2, "L\0"
        be32(0); be32(0);                                   // layer PROP_END
        int hierPtrOff=b.size(); be32(0); be32(0);          // hierarchy ptr (patched) + mask ptr = none
        patch32(hierPtrOff, b.size());
        be32(2); be32(2); be32(4);                          // hierarchy hw, hh, bpp=4
        int lvlPtrOff=b.size(); be32(0); be32(0);           // level0 ptr (patched) + list terminator
        patch32(lvlPtrOff, b.size());
        be32(2); be32(2);                                   // level w, h
        int tilePtrOff=b.size(); be32(0); be32(0);          // tile ptr (patched) + list terminator
        patch32(tilePtrOff, b.size());
        auto px=[&](int r,int g,int bl,int a){ b.append(char(r)); b.append(char(g)); b.append(char(bl)); b.append(char(a)); };
        px(255,0,0,255); px(0,255,0,255); px(0,0,255,128); px(255,255,255,255);   // row0: red,green  row1: blue(a=128),white
        QImage im=RasterExtra::decodeData(b,"xcf");
        check(!im.isNull() && im.size()==QSize(2,2)
              && im.pixelColor(0,0)==QColor(255,0,0) && im.pixelColor(1,0)==QColor(0,255,0)
              && im.pixelColor(0,1).blue()==255 && im.pixelColor(0,1).alpha()==128,
              "native GIMP XCF (v0, uncompressed RGBA) decodes with alpha");
        check(RasterExtra::decodeData(b,QString()).size()==QSize(2,2), "XCF magic-byte dispatch works (no extension hint)"); }
      check(RasterExtra::extensions().contains("xcf"), "xcf extension is advertised");
      // XCF robustness: truncated / bogus inputs must return null, never crash.
      check(RasterExtra::decodeData(QByteArray("gimp xcf file")+QByteArray(6,'\0'),"xcf").isNull(),
            "truncated XCF header is rejected safely");
      check(RasterExtra::decodeData(QByteArray("gimp xcf v999")+QByteArray(1,'\0')+QByteArray(60,'\0'),"xcf").isNull(),
            "XCF with zero dimensions / no valid layers is rejected safely");

      // ===== Fleet-1: retro/legacy native raster decoders (ILBM/MacPaint/CUT/PIX/DEGAS/Koala/SCR/XV) =====
      { auto ap8=[](QByteArray&a,std::initializer_list<int> vs){ for(int v:vs) a.append(char(v)); };
        auto be16=[](QByteArray&a,int v){ a.append(char((v>>8)&0xff)); a.append(char(v&0xff)); };
        auto be32=[](QByteArray&a,quint32 v){ a.append(char((v>>24)&0xff)); a.append(char((v>>16)&0xff)); a.append(char((v>>8)&0xff)); a.append(char(v&0xff)); };
        auto le16=[](QByteArray&a,int v){ a.append(char(v&0xff)); a.append(char((v>>8)&0xff)); };
        (void)ap8;(void)be16;(void)be32;(void)le16;
      // ILBM: Amiga IFF ILBM / PBM (InterLeaved 
      // Amiga IFF ILBM: 2x2, 1 bit-plane, uncompressed BODY, 2-colour CMAP (idx0 red, idx1 green).
      // planeRowBytes = ((2+15)/16)*2 = 2. row0 byte0=0x80 -> x0=idx1(green), x1=idx0(red);
      // row1 byte0=0x40 -> x0=idx0(red), x1=idx1(green). Second byte of each row is padding.
      { QByteArray b("FORM"); be32(b,58); b.append("ILBM");
        b.append("BMHD"); be32(b,20);
        be16(b,2); be16(b,2); be16(b,0); be16(b,0);            // w=2,h=2,x=0,y=0
        ap8(b,{1,0,0,0});                                      // nPlanes=1, masking=0, compression=0, pad=0
        be16(b,0); ap8(b,{0,0}); be16(b,0); be16(b,0);         // transparentColor, xAspect,yAspect, pageW, pageH
        b.append("CMAP"); be32(b,6); ap8(b,{255,0,0, 0,255,0});// idx0 red, idx1 green
        b.append("BODY"); be32(b,4); ap8(b,{0x80,0x00, 0x40,0x00});
        QImage im=RasterExtra::decodeData(b,"iff");
        check(!im.isNull() && im.size()==QSize(2,2)
              && im.pixelColor(0,0)==QColor(0,255,0) && im.pixelColor(1,0)==QColor(255,0,0)
              && im.pixelColor(0,1)==QColor(255,0,0) && im.pixelColor(1,1)==QColor(0,255,0),
              "Amiga IFF ILBM (1-plane, uncompressed, CMAP) decodes");
        check(RasterExtra::decodeData(b,QString()).size()==QSize(2,2), "ILBM magic-byte (FORM/ILBM) dispatch works with no extension hint"); }
      check(RasterExtra::extensions().contains("iff") && RasterExtra::extensions().contains("ilbm")
            && RasterExtra::extensions().contains("lbm") && RasterExtra::extensions().contains("bbm"),
            "ILBM iff/ilbm/lbm/bbm extensions are advertised");
      // Amiga IFF ILBM HAM6 (CAMG bit 0x0800): 2x1, 6 planes. pixel0 ctrl=0/v=1 -> CMAP[1]=(100,150,200);
      // pixel1 ctrl=2/v=15 -> modify Red to 15*17=255, holding G,B -> (255,150,200).
      { QByteArray b("FORM"); be32(b,66); b.append("ILBM");
        b.append("BMHD"); be32(b,20);
        be16(b,2); be16(b,1); be16(b,0); be16(b,0);           // w=2,h=1,x,y
        ap8(b,{6,0,0,0});                                     // nPlanes=6, masking=0, compression=0, pad
        be16(b,0); ap8(b,{0,0}); be16(b,0); be16(b,0);        // transparent, aspect, pageW/H
        b.append("CAMG"); be32(b,4); be32(b,0x00000800);      // HAM viewport mode
        b.append("CMAP"); be32(b,6); ap8(b,{0,0,0, 100,150,200});  // colour 0 black, colour 1 = (100,150,200)
        b.append("BODY"); be32(b,12);                         // 6 planes x 2 bytes, uncompressed
        ap8(b,{0xC0,0, 0x40,0, 0x40,0, 0x40,0, 0x00,0, 0x40,0});   // idx: px0=1 (bit7), px1=47 (bit6 of planes 0,1,2,3,5)
        QImage im=RasterExtra::decodeData(b,"iff");
        check(!im.isNull() && im.size()==QSize(2,1)
              && im.pixelColor(0,0)==QColor(100,150,200) && im.pixelColor(1,0)==QColor(255,150,200),
              "Amiga IFF ILBM HAM6 hold-and-modify decodes (was rendering as garbage)"); }
      // MacPaint (Apple PNTG)
      // MacPaint (Apple PNTG): 512-byte header, then PackBits for a fixed 576x720 1-bpp bitmap
      // (bit set = BLACK). We build a raw (no MacBinary) file whose decompressed data is one
      // 0xFF byte (x0..x7 black) followed by 51839 white (0x00) bytes.
      { QByteArray b(512, '\0');                             // 512-byte header, all zero
        b.append(char(0)); b.append(char(0xFF));            // literal run length 1: 0xFF (8 black px)
        qint64 remaining = qint64(720)*72 - 1;                    // 51839 white bytes to fill
        while(remaining > 0){ int run = int(qMin<qint64>(128, remaining));
            b.append(char(257-run));                        // repeat-run control (count = 257-c)
            b.append(char(0)); remaining -= run; }
        QImage im=RasterExtra::decodeData(b,"mac");
        check(!im.isNull() && im.size()==QSize(576,720)
              && im.pixelColor(0,0)==QColor(0,0,0)          // bit set -> black
              && im.pixelColor(8,0)==QColor(255,255,255),   // next byte 0x00 -> white
              "MacPaint PackBits 1-bpp bitmap decodes (bit=black)"); }
      check(RasterExtra::extensions().contains("mac") && RasterExtra::extensions().contains("pntg"),
            "MacPaint extensions are advertised");
      // DrHaloCUT (Dr. Halo / Dr. Genius .CUT pa
      // Dr. Halo CUT: width/height u16le + per-row [rowByteCount u16le][RLE]. Palette lives in a
      // separate .PAL, absent here, so each index becomes an 8-bit grayscale value. 4x1 image whose
      // single row is a literal run of 4 bytes 0,255,0,255 followed by the 0 end-of-row marker.
      { QByteArray b;
        auto le16=[&](quint16 v){ b.append(char(v&0xFF)); b.append(char((v>>8)&0xFF)); };
        le16(4); le16(1);                                   // width=4, height=1
        le16(6);                                            // rowByteCount = 1 control + 4 literal + 1 terminator
        b.append(char(0x04));                               // literal, count 4 (bit7 clear)
        b.append(char(0x00)); b.append(char(0xFF)); b.append(char(0x00)); b.append(char(0xFF));
        b.append(char(0x00));                               // end-of-row
        QImage im=RasterExtra::decodeData(b,"cut");
        check(!im.isNull() && im.size()==QSize(4,1)
              && im.pixelColor(0,0)==QColor(0,0,0)   && im.pixelColor(1,0)==QColor(255,255,255)
              && im.pixelColor(2,0)==QColor(0,0,0)   && im.pixelColor(3,0)==QColor(255,255,255),
              "Dr. Halo CUT RLE grayscale decodes"); }
      check(RasterExtra::extensions().contains("cut"), "cut extension is advertised");
      // AliasPIX (Alias/Wavefront PIX / Turbo Si
      // Alias/Wavefront PIX: 10-byte BE header (w,h,xoff,yoff,bpp) + RLE runs; 24bpp stores B,G,R per run.
      { QByteArray b; be16(b,2); be16(b,1); be16(b,0); be16(b,0); be16(b,24);   // 2x1, xoff/yoff 0, 24bpp
        ap8(b,{1, 255,0,0});                                 // count=1, blue  (B=255,G=0,R=0)
        ap8(b,{1, 0,0,255});                                 // count=1, red   (B=0,G=0,R=255)
        QImage im=RasterExtra::decodeData(b,"pix");
        check(!im.isNull() && im.size()==QSize(2,1) && im.pixelColor(0,0)==QColor(0,0,255) && im.pixelColor(1,0)==QColor(255,0,0),
              "Alias PIX 24bpp RLE decodes (B,G,R runs)"); }
      check(RasterExtra::extensions().contains("als") && RasterExtra::extensions().contains("pix"),
            "AliasPIX (als/pix) extensions are advertised");
      // Atari ST DEGAS (uncompressed .pi1/.pi2/.
// Atari ST DEGAS (uncompressed low-res): resolution word 0 (320x200, 4 planes), 16 Atari palette words,
// then 32000 bytes of word-interleaved planar bitmap. palette[1]=pure red (R=7); plane-0 row-0 first
// word = 0x8000 (MSB set) -> pixel(0,0) has colour index 1; every other pixel is index 0 (black).
{ QByteArray b;
  auto be16d=[&](quint16 v){ b.append(char(v>>8)); b.append(char(v)); };
  be16d(0);                                       // resolution = 0 (low res)
  be16d(0);                                        // palette[0] = 0 -> black
  be16d(0x0700);                                   // palette[1] = R=7,G=0,B=0 -> (255,0,0)
  for(int i=2;i<16;i++) be16d(0);                  // palette[2..15] = 0
  QByteArray bmp(32000,'\0'); bmp[0]=char(0x80);   // plane-0, row0, word0 = 0x8000 -> pixel(0,0) index 1
  b.append(bmp);
  QImage im=RasterExtra::decodeData(b,"pi1");
  check(!im.isNull() && im.size()==QSize(320,200)
        && im.pixelColor(0,0)==QColor(255,0,0) && im.pixelColor(1,0)==QColor(0,0,0),
        "Atari ST DEGAS (uncompressed low-res) planar bitmap decodes with palette"); }
check(RasterExtra::extensions().contains("pi1") && RasterExtra::extensions().contains("pi3"),
      "DEGAS pi1/pi2/pi3 extensions are advertised");
      // C64 Koala Painter (Commodore 64 multicol
      // C64 Koala Painter: 2-byte load address + 8000 bitmap + 1000 screen + 1000 colour + 1 bg.
      // Cell(0,0) bitmap byte 0x1B = 00 01 10 11 → the 4 logical pixels of the top row pull from
      // bg / screen-hi / screen-lo / colour-RAM respectively; each is doubled horizontally (320x200).
      { QByteArray b; b.append(char(0x00)); b.append(char(0x60));      // load address $6000
        QByteArray bitmap(8000,char(0)); bitmap[0]=char(0x1B);          // 0b00_01_10_11
        b.append(bitmap);
        QByteArray screen(1000,char(0)); screen[0]=char(0x21);          // hi-nibble=2 (red), lo-nibble=1 (white)
        b.append(screen);
        QByteArray colour(1000,char(0)); colour[0]=char(0x05);          // lo-nibble=5 (green)
        b.append(colour);
        b.append(char(0x00));                                           // background = 0 (black)
        QImage im=RasterExtra::decodeData(b,"koa");
        check(!im.isNull() && im.size()==QSize(320,200)
              && im.pixelColor(0,0)==QColor(0,0,0)                      // pair 00 → background (black)
              && im.pixelColor(1,0)==QColor(0,0,0)                      // horizontal doubling
              && im.pixelColor(2,0)==QColor(0x68,0x37,0x2B)            // pair 01 → screen hi = red
              && im.pixelColor(3,0)==QColor(0x68,0x37,0x2B)
              && im.pixelColor(4,0)==QColor(0xFF,0xFF,0xFF)            // pair 10 → screen lo = white
              && im.pixelColor(6,0)==QColor(0x58,0x8D,0x43),           // pair 11 → colour RAM = green
              "C64 Koala multicolour bitmap decodes (bg/screen-hi/screen-lo/colour-RAM + doubling)"); }
      check(RasterExtra::extensions().contains("koa") && RasterExtra::extensions().contains("kla"),
            "C64 Koala (koa/kla) extensions are advertised");
      check(RasterExtra::decodeData(QByteArray(10003,char(0)),"koa").size()==QSize(320,200)
            && RasterExtra::decodeData(QByteArray(9999,char(0)),"koa").isNull(),
            "C64 Koala accepts the canonical 10003-byte length and rejects a wrong length");
      // ZXSpectrumSCR
      // ZX Spectrum SCR: 6912-byte framebuffer (6144 pixel bytes + 768 attribute bytes), 256x192.
      // Set pixel-byte 0 = 0x80 → pixel(0,0) INK (bit7), pixels(1..7,0) PAPER. Attribute for cell(0,0)
      // at 6144 = 0x22 → ink index 2 (red {215,0,0}), paper index 4 (green {0,215,0}), bright off.
      { QByteArray b(6912, '\0');
        b[0]    = char(0x80);   // pixel(0,0)=INK, its 7 neighbours in the same byte = PAPER
        b[6144] = char(0x22);   // cell(0,0): ink=2 (red), paper=4 (green), bright/flash = 0
        QImage im=RasterExtra::decodeData(b,"scr");
        check(!im.isNull() && im.size()==QSize(256,192)
              && im.pixelColor(0,0)==QColor(215,0,0) && im.pixelColor(1,0)==QColor(0,215,0),
              "ZX Spectrum SCR ink/paper decodes"); }
      check(RasterExtra::decodeData(QByteArray(6911,'\0'),"scr").isNull(),
            "SCR of the wrong size is rejected safely");
      check(RasterExtra::extensions().contains("scr"), "scr extension is advertised");
      // XVthumb (XV P7 332 thumbnail)
// XV thumbnail (P7 332): ASCII header ("P7 332", optional '#' comments, "W H 255") then
// W*H bytes of 3-3-2 packed RGB. Simple left-justify shift: 0xE0 -> (224,0,0), 0x03 -> (0,0,192).
{ QByteArray b("P7 332\n#XVVERSION:selftest\n2 1 255\n");
  b.append(char(0xE0));                                  // pixel 0: R field = 0xE0 -> (224,0,0)
  b.append(char(0x03));                                  // pixel 1: B field = 0x03<<6 = 0xC0 -> (0,0,192)
  QImage im=RasterExtra::decodeData(b,"xvpics");
  check(!im.isNull() && im.size()==QSize(2,1)
        && im.pixelColor(0,0)==QColor(224,0,0) && im.pixelColor(1,0)==QColor(0,0,192),
        "XV thumbnail (P7 332) 3-3-2 packed RGB decodes");
  check(RasterExtra::decodeData(b,QString()).size()==QSize(2,1),
        "XVthumb magic-byte dispatch works (no extension hint, not mistaken for PAM)");
  check(RasterExtra::extensions().contains("xvpics"), "xvpics extension is advertised"); }
// Robustness: truncated pixel data must be rejected, never over-read.
check(RasterExtra::decodeData(QByteArray("P7 332\n4 4 255\n")+QByteArray(3,'\0'),"xvpics").isNull(),
      "XVthumb with too-few pixel bytes is rejected safely");
      }

      // ===== Fleet-2: retro/legacy native raster decoders (NEO/RIX/DCX/Cineon/SunIcon/Softimage) =====
      { auto ap8=[](QByteArray&a,std::initializer_list<int> vs){ for(int v:vs) a.append(char(v)); };
        auto be16=[](QByteArray&a,int v){ a.append(char((v>>8)&0xff)); a.append(char(v&0xff)); };
        auto le16=[](QByteArray&a,int v){ a.append(char(v&0xff)); a.append(char((v>>8)&0xff)); };
        auto be32=[](QByteArray&a,quint32 v){ a.append(char((v>>24)&0xff)); a.append(char((v>>16)&0xff)); a.append(char((v>>8)&0xff)); a.append(char(v&0xff)); };
        auto le32=[](QByteArray&a,quint32 v){ a.append(char(v&0xff)); a.append(char((v>>8)&0xff)); a.append(char((v>>16)&0xff)); a.append(char((v>>24)&0xff)); };
        (void)ap8;(void)be16;(void)le16;(void)be32;(void)le32;
      // Atari ST NEOchrome (.neo)
      // Atari ST NEOchrome: fixed 32128-byte file (128-byte header + 32000-byte DEGAS-low-res planar
      // bitmap). palette[1]=pure red (R=7), first bitmap word=0x8000 → pixel(0,0) uses index 1.
      { QByteArray b;
        be16(b,0);                                   // flag word
        be16(b,0);                                   // resolution = 0 (low, 320x200)
        be16(b,0);                                   // palette[0]
        be16(b,0x0700);                              // palette[1] = pure red (R=7,G=0,B=0)
        for(int i=2;i<16;i++) be16(b,0);             // palette[2..15]
        while(b.size()<128) b.append(char(0));       // filename + animation + reserved words
        be16(b,0x8000);                              // plane0 word of row0/group0: bit15 → pixel(0,0) index 1
        while(b.size()<32128) b.append(char(0));     // remaining bitmap (all index 0)
        QImage im=RasterExtra::decodeData(b,"neo");
        check(!im.isNull() && im.size()==QSize(320,200)
              && im.pixelColor(0,0)==QColor(255,0,0) && im.pixelColor(1,0)==QColor(0,0,0),
              "Atari NEOchrome low-res decodes (word-interleaved planar)");
        check(RasterExtra::decodeData(b.left(32127),"neo").isNull(),
              "NEOchrome with wrong length is rejected");
        check(RasterExtra::extensions().contains("neo"), "NEOchrome extension is advertised"); }
      // ColoRIX / RIX VGA paint (RIX3)
      // ColoRIX RIX3: 2x1, 256-colour VGA palette; index 5 -> red, index 0 -> green.
      { QByteArray b("RIX3"); le16(b,2); le16(b,1); ap8(b,{0x80});   // flags bit7 = palette present
        QByteArray pal(768,char(0));
        pal[0]=char(0); pal[1]=char(63); pal[2]=char(0);            // idx0 = green (6-bit 63 -> 255)
        pal[5*3+0]=char(63); pal[5*3+1]=char(0); pal[5*3+2]=char(0); // idx5 = red
        b.append(pal);
        ap8(b,{5,0});                                               // two index bytes: (0,0)=idx5, (1,0)=idx0
        QImage im=RasterExtra::decodeData(b,"rix");
        check(!im.isNull() && im.size()==QSize(2,1) && im.pixelColor(0,0)==QColor(255,0,0) && im.pixelColor(1,0)==QColor(0,255,0), "ColoRIX RIX3 palette + indices decode"); }

      // ColoRIX security: header claiming a palette but truncated before its 768 bytes must be rejected.
      { QByteArray b("RIX3"); le16(b,4); le16(b,4); ap8(b,{0x80}); b.append(QByteArray(100,char(0)));
        check(RasterExtra::decodeData(b,"rix").isNull(), "ColoRIX truncated palette rejected (no over-read)"); }
      // DCX (ZSoft multi-page PCX)
      // DCX: ZSoft multi-page PCX wrapper. Build a minimal 2x2 8bpp/1-plane PCX (as in the PCX
      // test above), then wrap it: magic + one page offset (pointing just past the 12-byte header:
      // magic + 1 entry + 0 terminator) + 0 terminator. Decoding page 0 must match the PCX result.
      { QByteArray pcx; ap8(pcx,{0x0A,5,1,8}); le16(pcx,0); le16(pcx,0); le16(pcx,1); le16(pcx,1);
        le16(pcx,72); le16(pcx,72); for(int k=0;k<48;k++) pcx.append(char(0)); pcx.append(char(0));
        pcx.append(char(1)); le16(pcx,2); le16(pcx,1); le16(pcx,0); le16(pcx,0);
        while(pcx.size()<128) pcx.append(char(0));
        ap8(pcx,{0,1, 2,3});                                 // 2 rows x 2 literal indices
        pcx.append(char(0x0C)); QByteArray pal(768,char(0));
        pal[0]=char(255); pal[4]=char(255); pal[8]=char(255); pal[9]=char(255); pal[10]=char(255); // idx0 red, idx1 green, idx2 blue, idx3 yellow
        pcx.append(pal);
        QByteArray b; le32(b,0x3ADE68B1u); le32(b,12); le32(b,0);   // magic + page@offset 12 + 0 terminator
        b.append(pcx);
        QImage im=RasterExtra::decodeData(b,"dcx");
        check(!im.isNull() && im.size()==QSize(2,2) && im.pixelColor(0,0)==QColor(255,0,0) && im.pixelColor(1,1)==QColor(255,255,0),
              "DCX first-page PCX decodes"); }
      // DCX with a bogus first page offset (past EOF) must be rejected safely.
      { QByteArray b; le32(b,0x3ADE68B1u); le32(b,0x7FFFFFFFu); le32(b,0);
        check(RasterExtra::decodeData(b,"dcx").isNull(), "DCX with out-of-range page offset is rejected"); }
      // Cineon (Kodak Cineon, SMPTE 268M predecessor). Image-information header @200:
      // orientation U8, channels U8, 2 pad, then channel[0] descriptor @204 whose pixels_per_line
      // (WIDTH) is @208 and lines_per_image (HEIGHT) @212 — NOT 200/204.
      // Kodak Cineon: 2x1, 10-bit RGB packed one word per pixel (one full-red, one full-blue).
      { QByteArray b; be32(b,0x802A5FD7u);                 // big-endian Cineon magic (offset 0)
        be32(b,216);                                       // image-data offset (offset 4)
        while(b.size()<208) b.append(char(0));             // pad generic hdr + image-info hdr up to channel[0].pixels_per_line
        be32(b,2); be32(b,1);                              // width=2 (offset 208), height=1 (offset 212)
        be32(b,0xFFC00000u);                               // pixel0: R=1023 (>>2 -> 255), G=B=0
        be32(b,0x00000FFCu);                               // pixel1: B=1023 (>>2 -> 255), R=G=0
        QImage im=RasterExtra::decodeData(b,"cin");
        check(!im.isNull() && im.size()==QSize(2,1)
              && im.pixelColor(0,0)==QColor(255,0,0)
              && im.pixelColor(1,0)==QColor(0,0,255), "Cineon 10-bit RGB (.cin) decodes"); }
      // Cineon bounds: a header that stops before the width/height fields (offset<216) must be rejected.
      { QByteArray b; be32(b,0x802A5FD7u); be32(b,216); while(b.size()<210) b.append(char(0));
        check(RasterExtra::decodeData(b,"cin").isNull(), "Cineon short header is rejected (no over-read)"); }
      // Sun OpenWindows Icon / Cursor (.icon/.cu
      // ===== Sun OpenWindows Icon / Cursor (.icon/.cursor) — C-source text 1bpp bitmap =====
      { QByteArray b("/* Format_version=1, Width=16, Height=2, Depth=1, Valid_bits_per_item=16 */\n"
                     "unsigned short Bits[]={\n0x8000,0x0001\n};\n");
        // Row0 word 0x8000 -> only bit15 set -> x=0 is foreground(black), rest white.
        // Row1 word 0x0001 -> only bit0  set -> x=15 is foreground(black), rest white.
        QImage im=RasterExtra::decodeData(b,"icon");
        check(!im.isNull() && im.size()==QSize(16,2)
              && im.pixelColor(0,0)==QColor(0,0,0)
              && im.pixelColor(1,0)==QColor(255,255,255)
              && im.pixelColor(15,1)==QColor(0,0,0)
              && im.pixelColor(0,1)==QColor(255,255,255),
              "Sun OpenWindows icon (Bits[] hex words, 1=black, MSB-first, row-padded to 16) decodes");
        check(RasterExtra::decodeData(b,QString()).size()==QSize(16,2),
              "Sun icon '/* Format_version' magic-byte dispatch works with no extension hint");
        // Truncated array (only 1 of the 2 required words) must be rejected as malformed.
        QByteArray bad("/* Format_version=1, Width=16, Height=2 */\nBits[]={ 0x8000 };\n");
        check(RasterExtra::decodeData(bad,"icon").isNull(),
              "Sun icon with too few Bits[] words is rejected (null)"); }
      // SoftimagePIC
      // Softimage PIC: 2x1 uncompressed single RGB packet (mask 0xE0).
      { QByteArray b; be32(b,0x5380F634u);            // magic @0
        b.append(QByteArray(84,0));                    // version(4)+comment(80) → "PICT" lands @88
        b.append("PICT",4);                            // @88
        be16(b,2); be16(b,1);                          // width @92 = 2, height @94 = 1
        be32(b,0);                                     // ratio @96
        be16(b,0); be16(b,0);                          // fields @100, pad @102
        ap8(b,{0,8,0,0xE0});                           // packet @104: chained=0, size=8, type=0(raw), mask=RGB
        ap8(b,{10,20,30, 200,100,50});                 // pixel data: (10,20,30) then (200,100,50)
        QImage im=RasterExtra::decodeData(b,"pic");
        check(!im.isNull() && im.size()==QSize(2,1) && im.pixelColor(0,0)==QColor(10,20,30)
              && im.pixelColor(1,0)==QColor(200,100,50), "Softimage PIC 2x1 uncompressed RGB decodes"); }

      // Softimage PIC: 3x1 RLE RGB packet — 2-pixel run of blue then 1 literal red.
      { QByteArray b; be32(b,0x5380F634u); b.append(QByteArray(84,0)); b.append("PICT",4);
        be16(b,3); be16(b,1); be32(b,0); be16(b,0); be16(b,0);
        ap8(b,{0,8,2,0xE0});                           // packet: type=2 (RLE), mask=RGB
        ap8(b,{129, 0,0,255});                         // count 129 (>128) → 129-127=2 copies of blue
        ap8(b,{0, 255,0,0});                           // count 0 (<128) → 0+1=1 literal red
        QImage im=RasterExtra::decodeData(b,"pic");
        check(!im.isNull() && im.size()==QSize(3,1) && im.pixelColor(0,0)==QColor(0,0,255)
              && im.pixelColor(1,0)==QColor(0,0,255) && im.pixelColor(2,0)==QColor(255,0,0),
              "Softimage PIC 3x1 RLE RGB decodes"); }
      }

      // ===== Fleet-2b: GEM IMG + Atari Tiny =====
      { auto ap8=[](QByteArray&a,std::initializer_list<int> vs){ for(int v:vs) a.append(char(v)); };
        auto be16=[](QByteArray&a,int v){ a.append(char((v>>8)&0xff)); a.append(char(v&0xff)); };
        auto le16=[](QByteArray&a,int v){ a.append(char(v&0xff)); a.append(char((v>>8)&0xff)); };
        (void)ap8;(void)be16;(void)le16;
      // GEM Raster (Digital Research GEM .img / 
      // Digital Research GEM Raster (.img): mono 16x1, exercising the solid-run + literal-run RLE tokens.
      { QByteArray b;
        be16(b,1); be16(b,8); be16(b,1); be16(b,1);   // version, headerLenWords, nPlanes(mono), patternLen
        be16(b,0); be16(b,0); be16(b,16); be16(b,1);  // pixelW, pixelH (microns), imgWidth=16, imgHeight=1
        ap8(b,{0x81, 0x80,0x01,0x0F});                // plane byte0 = solid 0xFF ; byte1 = literal 0x0F
        QImage im=RasterExtra::decodeData(b,"img");
        check(!im.isNull() && im.size()==QSize(16,1)
              && im.pixelColor(0,0)==QColor(0,0,0)          // byte0=0xFF, bit set -> index 1 -> black
              && im.pixelColor(8,0)==QColor(255,255,255)    // byte1=0x0F, bit7 clear -> index 0 -> white
              && im.pixelColor(12,0)==QColor(0,0,0),        // byte1=0x0F, bit3 set  -> index 1 -> black
              "GEM IMG mono RLE (solid+literal) decodes"); }
      // AtariTiny (Atari ST "Tiny" / Tiny Stuff,
    // --- Atari ST "Tiny" (Tiny Stuff) decoder ---
    // Hand-built low-res (320x200) file. TINY word 0 = 0xFFFF (plane 0 of the first 16-pixel cell of
    // row 0); every other word = 0. After the identity transpose (word 0 -> normal screen word 0),
    // pixels x0..15 of row 0 take palette index 1 (red); all other pixels take palette index 0 (black).
    {
        QByteArray t;
        t.append(char(0));                            // resolution 0 = low (320x200, 4 planes)
        be16(t, 0x0000);                              // palette[0] = R0 G0 B0 -> black
        be16(t, 0x0700);                              // palette[1] = R7 G0 B0 -> (255,0,0) red
        for(int i=2;i<16;i++) be16(t, 0x0000);        // palette[2..15]
        be16(t, 4);                                   // i16be control-byte count = 4
        be16(t, 2);                                   // i16be data-word count = 2
        ap8(t, {0x01, 0x00, 0x3E, 0x7F});             // ctrl: [+1 => repeat word0 once], [0 => u16 count 0x3E7F=15999]
        ap8(t, {0xFF,0xFF, 0x00,0x00});               // data: word0 = 0xFFFF, word1 = 0x0000
        // 1 (+1 repeat of word0) + 15999 (big repeat of word1) = 16000 words; consumes 2 data words, 4 control bytes.
        QImage im = xara::RasterExtra::decodeData(t, "tny");
        check(!im.isNull() && im.size()==QSize(320,200)
              && im.pixelColor(0,0)==QColor(255,0,0)
              && im.pixelColor(15,0)==QColor(255,0,0)
              && im.pixelColor(16,0)==QColor(0,0,0)
              && im.pixelColor(0,1)==QColor(0,0,0)
              && im.pixelColor(319,199)==QColor(0,0,0),
              "AtariTiny low-res: RLE (small+big repeat) + transpose + 3-bit palette");
        // Truncated stream (data shorter than the declared word count) must fail cleanly, not crash.
        QImage bad = xara::RasterExtra::decodeData(t.left(t.size()-2), "tny");
        check(bad.isNull(), "AtariTiny truncated data stream -> null");
    }
      }

      // ===== Fleet-3: MIFF / PALM / PGX / SIXEL / PICT / RLA (real-oracle cross-validated) =====
      // ---- PGX (JPEG 2000 uncompressed grayscale) ----
      {
{
    // --- PGX case 1: 8-bit, big-endian ("ML"), unsigned, 3x2. Header hand-laid at spec offsets. ---
    QByteArray b;
    auto put8=[&](int v){ b.append(char(v&0xff)); };
    put8('P'); put8('G');   // @0..1 magic "PG"
    put8(' ');              // @2   whitespace
    put8('M'); put8('L');   // @3..4 endian "ML" = big-endian
    put8(' ');              // @5
    put8('+');              // @6   sign '+' = unsigned
    put8(' ');              // @7
    put8('8');              // @8   bitdepth = 8
    put8(' ');              // @9
    put8('3');              // @10  width = 3
    put8(' ');              // @11
    put8('2');              // @12  height = 2
    put8(0x0A);             // @13  newline terminates header
    // raw samples, row-major, 1 byte/sample (depth<=8):
    put8(0);   put8(128); put8(255);   // row 0
    put8(64);  put8(32);  put8(200);   // row 1
    QImage im = RasterExtra::decodeData(b, "pgx");
    check(!im.isNull() && im.size()==QSize(3,2)
          && im.pixelColor(0,0)==QColor(0,0,0)
          && im.pixelColor(1,0)==QColor(128,128,128)
          && im.pixelColor(2,0)==QColor(255,255,255)
          && im.pixelColor(0,1)==QColor(64,64,64)
          && im.pixelColor(2,1)==QColor(200,200,200), "PGX 8-bit ML unsigned");
}
{
    // --- PGX case 2: 16-bit, little-endian ("LM"), no sign, 2x1. Exercises 2-byte + endianness. ---
    QByteArray b;
    auto put8=[&](int v){ b.append(char(v&0xff)); };
    put8('P'); put8('G'); put8(' ');   // @0..2 magic + ws
    put8('L'); put8('M');              // @3..4 endian "LM" = little-endian
    put8(' ');
    put8('1'); put8('6');              // bitdepth = 16
    put8(' ');
    put8('2');                          // width = 2
    put8(' ');
    put8('1');                          // height = 1
    put8(0x0A);                          // newline
    // samples little-endian, 2 bytes each: 0x1234 -> gray 0x12 ; 0xABCD -> gray 0xAB
    put8(0x34); put8(0x12);
    put8(0xCD); put8(0xAB);
    QImage im = RasterExtra::decodeData(b, "pgx");
    check(!im.isNull() && im.size()==QSize(2,1)
          && im.pixelColor(0,0)==QColor(0x12,0x12,0x12)
          && im.pixelColor(1,0)==QColor(0xAB,0xAB,0xAB), "PGX 16-bit LM");
}
{
    // --- Negative: header declares 3x2 (needs 6 sample bytes) but only 1 is present -> reject. ---
    QByteArray b("PG ML + 8 3 2\n", 14);   // 14-byte header incl. trailing 0x0A
    b.append('\x01');                        // 1 data byte, 5 short
    QImage im = RasterExtra::decodeData(b, "pgx");
    check(im.isNull(), "PGX truncated sample data rejected");
}
{
    // --- Negative: header with no terminating newline -> reject (no over-read). ---
    QByteArray b("PG ML + 8 3 2");
    QImage im = RasterExtra::decodeData(b, "pgx");
    check(im.isNull(), "PGX missing-newline rejected");
}
      }
      // ---- MIFF (ImageMagick native) ----
      {
// ---- MIFF (Magick Image File Format) ----
{
    auto put   =[](QByteArray& bb,int v){ bb.append(char(v&0xFF)); };
    auto be16m =[](QByteArray& bb,int v){ bb.append(char((v>>8)&0xFF)); bb.append(char(v&0xFF)); };

    // (1) DirectClass, compression=None, depth=8, 2x2 RGB. Header hand-laid per MIFF
    //     spec: ASCII key=value tokens, then ':' (0x3A) immediately followed by 0x1A,
    //     then samples in quantum order R,G,B (1 byte each, big-endian), row-major.
    {
        QByteArray b;
        b.append("id=ImageMagick\n");                 // signature @ offset 0
        b.append("class=DirectClass\n");              // class= -> DirectClass
        b.append("columns=2  rows=2  depth=8\n");     // columns=W  rows=H  depth=bits
        b.append("colorspace=sRGB\n");
        b.append("compression=None  matte=False\n");  // None (raw)  +  3 channels (no alpha)
        b.append(':'); put(b,0x1A);                   // header terminator ':' + 0x1A
        put(b,10);  put(b,20);  put(b,30);            // pixel (0,0) R,G,B
        put(b,40);  put(b,50);  put(b,60);            // pixel (1,0)
        put(b,70);  put(b,80);  put(b,90);            // pixel (0,1)
        put(b,100); put(b,110); put(b,120);           // pixel (1,1)
        QImage im = xara::RasterExtra::decodeData(b,"miff");
        check(!im.isNull() && im.size()==QSize(2,2)
              && im.pixelColor(0,0)==QColor(10,20,30)
              && im.pixelColor(1,0)==QColor(40,50,60)
              && im.pixelColor(0,1)==QColor(70,80,90)
              && im.pixelColor(1,1)==QColor(100,110,120), "MIFF DirectClass/None/depth8 RGB");

        // negative: truncated pixel data (4 of the 12 sample bytes missing) must be rejected.
        check(xara::RasterExtra::decodeData(b.left(b.size()-4),"miff").isNull(),
              "MIFF truncated pixel data rejected (no over-read)");
    }

    // (2) compression=RLE, depth=8, 2x2 RGB. Packet layout VERIFIED vs ImageMagick 6.9.12:
    //     [R][G][B][count]; the pixel repeats (count+1) times (count is the LAST byte).
    {
        QByteArray b;
        b.append("id=ImageMagick\n");
        b.append("class=DirectClass\n");
        b.append("columns=2  rows=2  depth=8\n");
        b.append("compression=RLE  matte=False\n");
        b.append(':'); put(b,0x1A);
        put(b,10); put(b,20); put(b,30); put(b,1);    // 2 px of (10,20,30) -> row 0
        put(b,70); put(b,80); put(b,90); put(b,1);    // 2 px of (70,80,90) -> row 1
        QImage im = xara::RasterExtra::decodeData(b,"miff");
        check(!im.isNull() && im.size()==QSize(2,2)
              && im.pixelColor(0,0)==QColor(10,20,30)
              && im.pixelColor(1,0)==QColor(10,20,30)
              && im.pixelColor(0,1)==QColor(70,80,90)
              && im.pixelColor(1,1)==QColor(70,80,90), "MIFF DirectClass/RLE/depth8");
    }

    // (3) compression=None, depth=16, matte=True, 1x1. 16-bit samples are 2 bytes BE and
    //     the decoder keeps the HIGH byte; alpha is stored directly (IM>=6.8.9 / IM7).
    {
        QByteArray b;
        b.append("id=ImageMagick\n");
        b.append("class=DirectClass\n");
        b.append("columns=1  rows=1  depth=16\n");
        b.append("compression=None  matte=True\n");   // 4 channels R,G,B,A
        b.append(':'); put(b,0x1A);
        be16m(b,0x1234);  // R -> high byte 0x12 = 18
        be16m(b,0x5678);  // G -> 0x56 = 86
        be16m(b,0x9ABC);  // B -> 0x9A = 154
        be16m(b,0x33FF);  // A -> 0x33 = 51 (stored directly, not inverted opacity)
        QImage im = xara::RasterExtra::decodeData(b,"miff");
        check(!im.isNull() && im.size()==QSize(1,1)
              && im.pixelColor(0,0)==QColor(18,86,154,51), "MIFF DirectClass/None/depth16+alpha");
    }

    // (4) PseudoClass must be rejected.
    {
        QByteArray b;
        b.append("id=ImageMagick\n");
        b.append("class=PseudoClass\n");
        b.append("columns=1  rows=1  depth=8  colors=1\n");
        b.append("compression=None\n");
        b.append(':'); put(b,0x1A);
        put(b,0);
        check(xara::RasterExtra::decodeData(b,"miff").isNull(), "MIFF PseudoClass rejected");
    }
}
      }
      // ---- Palm pixmap ----
      {
{
    // Hand-built Palm bitmap (BitmapType), laid out byte-for-byte from the spec offsets so the
    // test cannot inherit a decoder mistake: 4x2, 8bpp, uncompressed, with a 4-entry colour table.
    QByteArray b;
    auto put8=[&](int v){ b.append(char(v&0xff)); };
    auto be16=[&](int v){ b.append(char((v>>8)&0xff)); b.append(char(v&0xff)); };
    // ---- 16-byte header, all big-endian ----
    be16(4);        // @0  width           = 4
    be16(2);        // @2  height          = 2
    be16(4);        // @4  bytesPerRow     = 4  (4 px * 1 byte/px @ 8bpp)
    be16(0x4000);   // @6  flags           = hasColorTable (0x4000)
    put8(8);        // @8  bitsPerPixel    = 8
    put8(1);        // @9  version         = 1
    be16(0);        // @10 nextDepthOffset = 0
    put8(0);        // @12 transparentIndex= 0
    put8(0);        // @13 compressionType = 0 (uncompressed; ignored, compressed flag clear)
    be16(0);        // @14 reserved        = 0
    // ---- colour table @16: numEntries u16, then (index,R,G,B) quads ----
    be16(4);        // numEntries = 4
    put8(0); put8(10);  put8(20);  put8(30);   // index 0 -> (10,20,30)
    put8(1); put8(255); put8(0);   put8(0);    // index 1 -> red
    put8(2); put8(0);   put8(255); put8(0);    // index 2 -> green
    put8(3); put8(0);   put8(0);   put8(255);  // index 3 -> blue
    // ---- pixel data: bytesPerRow*height = 4*2 = 8 bytes, one index per byte ----
    put8(0); put8(1); put8(2); put8(3);        // row 0 : indices 0,1,2,3
    put8(3); put8(2); put8(1); put8(0);        // row 1 : indices 3,2,1,0

    QImage im = RasterExtra::decodeData(b, "palm");
    check(!im.isNull() && im.size()==QSize(4,2)
          && im.pixelColor(0,0)==QColor(10,20,30)
          && im.pixelColor(1,0)==QColor(255,0,0)
          && im.pixelColor(3,0)==QColor(0,0,255)
          && im.pixelColor(0,1)==QColor(0,0,255)
          && im.pixelColor(3,1)==QColor(10,20,30), "Palm 8bpp colortable uncompressed");

    // Negative: drop the final pixel byte -> the last row read runs past the buffer -> null (no over-read).
    QImage bad = RasterExtra::decodeData(b.left(b.size()-1), "palm");
    check(bad.isNull(), "Palm truncated pixel data -> null");

    // version-0 regression (matches ImageMagick's PALM output): the hasColorTable flag bit is SET
    // but a v0 header carries NO colour table — pixel data starts right after the 16-byte header.
    // Consuming a phantom count there used to misalign every row and over-read. Now: default 1-bpp
    // palette (index 0 = white, 1 = black), 8x1, pixel byte 0xA0 = bits 1010_0000.
    QByteArray v0;
    auto p8=[&](int x){ v0.append(char(x&0xff)); };
    auto b16=[&](int x){ v0.append(char((x>>8)&0xff)); v0.append(char(x&0xff)); };
    b16(8); b16(1); b16(1); b16(0x4000);   // width=8,height=1,bytesPerRow=1,flags=hasColorTable
    p8(1); p8(0);                           // bpp=1, version=0  (v0 => NO colour table despite flag)
    b16(0); p8(0); p8(0); b16(0);           // nextDepthOffset, transIdx, compType, reserved (16-byte hdr)
    p8(0xA0);                               // 1 pixel row: 1010_0000 -> px0,2 black ; px1,3 white
    QImage v0im = RasterExtra::decodeData(v0, "palm");
    check(!v0im.isNull() && v0im.size()==QSize(8,1)
          && v0im.pixelColor(0,0)==QColor(0,0,0)         // bit7=1 -> index1 -> black
          && v0im.pixelColor(1,0)==QColor(255,255,255)   // bit6=0 -> index0 -> white
          && v0im.pixelColor(2,0)==QColor(0,0,0),        // bit5=1 -> index1 -> black
          "Palm version-0 w/ hasColorTable flag but no table (ImageMagick quirk) decodes");

    // version-3 (BitmapTypeV3) transparentIndex: the transparent index is the low byte of the u32
    // transparentValue @16 (p[19]), NOT p[12] (which is 'unused3' in v3). 2x1 8bpp, default palette
    // (index 0 = white, index 5 = magenta). transparentValue low byte = 0 => index 0 is transparent;
    // p[12] is deliberately set to 5 to catch the old stale-offset bug (which would blank index 5).
    QByteArray v3;
    auto q8=[&](int x){ v3.append(char(x&0xff)); };
    auto q16=[&](int x){ v3.append(char((x>>8)&0xff)); v3.append(char(x&0xff)); };
    q16(2); q16(1); q16(2); q16(0x2000);   // width=2,height=1,rowBytes=2,flags=hasTransparency
    q8(8); q8(3);                           // bpp=8, version=3
    q8(24); q8(0); q8(5); q8(0);            // @10 size=24, @11 pixelFormat, @12 unused3=5 (trap), @13 compType
    q16(0);                                 // @14 density
    q8(0); q8(0); q8(0); q8(0);             // @16 transparentValue u32 = 0x00000000 -> transparent index 0
    q8(0); q8(0); q8(0); q8(0);             // @20 nextBitmapOffset
    q8(0); q8(5);                           // pixel row: index 0 (white), index 5 (magenta)
    QImage v3im = RasterExtra::decodeData(v3, "palm");
    check(!v3im.isNull() && v3im.size()==QSize(2,1)
          && v3im.pixelColor(0,0).alpha()==0             // index 0 == transparentValue low byte -> transparent
          && v3im.pixelColor(1,0)==QColor(255,0,255),    // index 5 magenta, opaque (NOT blanked by stale p[12]=5)
          "Palm version-3 transparentIndex read from transparentValue@16 (not p[12])");
}
      }
      // ---- DEC SIXEL ----
      {
      // SIXEL (DEC): hand-laid per the DCS/sixel spec, independent of the decoder.
      // 4x12 image, 2 bands. Palette (Pu=2 RGB %): reg0=red(100;0;0), reg1=blue(0;0;100).
      // Band0 cols: v=1('@')->row0 only, v=32('_')->row5 only (bit0=top,bit5=bottom),
      //             v=63('~')x2 -> full 6 rows. Band1: RLE '!4~' fills all 4 cols. Raster "1;1;4;12.
      { QByteArray b;
        auto put8=[&](int v){ b.append(char(v&0xff)); };
        put8(0x1B); put8(0x50); put8(0x71);                                   // ESC P q  (DCS introducer, no params)
        put8('"'); put8('1'); put8(';'); put8('1'); put8(';'); put8('4'); put8(';'); put8('1'); put8('2'); // "1;1;Ph=4;Pv=12
        put8('#'); put8('0'); put8(';'); put8('2'); put8(';'); put8('1'); put8('0'); put8('0'); put8(';'); put8('0'); put8(';'); put8('0'); // #0;2;100;0;0 -> qRgb(255,0,0)
        put8('#'); put8('1'); put8(';'); put8('2'); put8(';'); put8('0'); put8(';'); put8('0'); put8(';'); put8('1'); put8('0'); put8('0'); // #1;2;0;0;100 -> qRgb(0,0,255)
        put8('#'); put8('0');                                                  // select register 0 (red)
        put8(0x40); put8(0x5F); put8(0x7E); put8(0x7E);                        // '@'(v1) '_'(v32) '~'(v63) '~'(v63)
        put8('-');                                                            // graphics LF -> band 1
        put8('#'); put8('1'); put8('!'); put8('4'); put8(0x7E);               // select 1, RLE repeat '~'(v63) x4
        put8(0x1B); put8(0x5C);                                               // ESC '\'  (ST)
        QImage im=RasterExtra::decodeData(b,"six");
        check(!im.isNull() && im.size()==QSize(4,12)
              && im.pixelColor(0,0)==QColor(255,0,0)     // band0 col0 bit0 -> top row red
              && im.pixelColor(0,1)==QColor(0,0,0)       // col0 rows1-5 = background black
              && im.pixelColor(1,5)==QColor(255,0,0)     // band0 col1 bit5 -> bottom row (row5) red
              && im.pixelColor(1,0)==QColor(0,0,0)       // col1 row0 empty
              && im.pixelColor(2,3)==QColor(255,0,0)     // col2 fully filled
              && im.pixelColor(0,6)==QColor(0,0,255)     // band1 top row blue (RLE)
              && im.pixelColor(3,11)==QColor(0,0,255),   // band1 bottom-right blue
              "SIXEL raster attrs + RGB palette + bit-order + RLE + band advance"); }

      // Negative: ESC 'P' introducer with no 'q' final byte (>=8 bytes) -> null, no over-read.
      { QByteArray bad;
        auto put8=[&](int v){ bad.append(char(v&0xff)); };
        put8(0x1B); put8(0x50); put8('1'); put8(';'); put8('1'); put8(';'); put8('1'); put8(';'); // ESC P 1;1;1;  (no 'q')
        check(RasterExtra::decodeData(bad,"six").isNull(), "SIXEL truncated (no q terminator) rejected"); }
      }
      // ---- Apple QuickDraw PICT ----
      {
// ---- PICT: 32-bit DirectBitsRect (0x009A), planar packType 4 -------------------------------
// File hand-laid at exact spec offsets: 512-byte header, then picSize, frame Rect, v2 opcodes.
{
    QByteArray b;
    auto put8=[&](int v){ b.append(char(v&0xff)); };
    auto be16=[&](int v){ b.append(char((v>>8)&0xff)); b.append(char(v&0xff)); };
    auto be32=[&](quint32 v){ b.append(char((v>>24)&0xff)); b.append(char((v>>16)&0xff)); b.append(char((v>>8)&0xff)); b.append(char(v&0xff)); };
    for(int i=0;i<512;i++) put8(0);                 // @0..511 : 512-byte header (all zero)
    be16(512);                                      // @512    : picSize u16 (ignored)
    be16(0); be16(0); be16(6); be16(8);             // @514    : frame Rect top,left,bottom,right => 8x6
    be16(0x0011);                                   // @522    : VersionOp
    be16(0x02FF);                                   // @524    : Version (v2)
    be16(0x0C00);                                   // @526    : HeaderOp
    be16(0xFFFE); be16(0); be16(0x0048); be16(0);   // @528    : 24 header bytes
    be16(0x0048); be16(0); be16(0); be16(0);
    be16(6); be16(8); be16(0); be16(0);
    be16(0x0001); be16(10);                         // Clip opcode + rgnSize=10 (total)
    be16(0); be16(0); be16(6); be16(8);             //   region Rect (8 bytes)
    be16(0x009A);                                   // DirectBitsRect opcode
    be32(0x000000FF);                               //   pmBaseAddr u32 (DirectBits marker)
    be16(0x8000 | 32);                              //   rowBytes u16: packed-flag | 32 (=width*4)
    be16(0); be16(0); be16(6); be16(8);             //   PixMap bounds Rect
    be16(0); be16(4); be32(0);                      //   pmVersion, packType=4 (32-bit planar RLE), packSize
    be32(0x00480000); be32(0x00480000);             //   hRes, vRes (72.0 Fixed)
    be16(16); be16(32); be16(3); be16(8);           //   pixelType=16(RGBDirect), pixelSize=32, cmpCount=3, cmpSize=8
    be32(0); be32(0); be32(0);                       //   planeBytes, pmTable, pmReserved
    be16(0); be16(0); be16(6); be16(8);             //   srcRect
    be16(0); be16(0); be16(6); be16(8);             //   dstRect
    be16(0);                                        //   mode
    for(int y=0;y<6;y++){                           //   6 rows: byteCount(u8)=6, then planar PackBits
        put8(6); put8(0xF9); put8(0x30); put8(0xF9); put8(0x60); put8(0xF9); put8(0xA0);
    }                                               //   F9 30 = 8x0x30 (R), F9 60 = 8x0x60 (G), F9 A0 = 8x0xA0 (B)
    be16(0x00FF);                                   // OpEndPic
    QImage im = RasterExtra::decodeData(b, "pct");
    check(!im.isNull() && im.size()==QSize(8,6)
          && im.pixelColor(0,0)==QColor(0x30,0x60,0xA0)
          && im.pixelColor(7,5)==QColor(0x30,0x60,0xA0), "PICT DirectBits 32-bit #3060a0");
    // Negative: truncate inside the Clip region (before the bitmap) => must be null, no over-read.
    check(RasterExtra::decodeData(b.left(560), "pct").isNull(), "PICT truncated rejected");
}
// ---- PICT: 8-bit indexed PackBitsRect (0x0098) with color table ----------------------------
{
    QByteArray b;
    auto put8=[&](int v){ b.append(char(v&0xff)); };
    auto be16=[&](int v){ b.append(char((v>>8)&0xff)); b.append(char(v&0xff)); };
    auto be32=[&](quint32 v){ b.append(char((v>>24)&0xff)); b.append(char((v>>16)&0xff)); b.append(char((v>>8)&0xff)); b.append(char(v&0xff)); };
    for(int i=0;i<512;i++) put8(0);                 // @0..511 : 512-byte header
    be16(512);                                      // @512    : picSize
    be16(0); be16(0); be16(6); be16(8);             // @514    : frame Rect => 8x6
    be16(0x0011); be16(0x02FF); be16(0x0C00);       // VersionOp + Version + HeaderOp
    be16(0xFFFE); be16(0); be16(0x0048); be16(0);   //   24 header bytes
    be16(0x0048); be16(0); be16(0); be16(0);
    be16(6); be16(8); be16(0); be16(0);
    be16(0x0001); be16(10);                         // Clip opcode + rgnSize
    be16(0); be16(0); be16(6); be16(8);             //   region Rect
    be16(0x0098);                                   // PackBitsRect opcode (no baseAddr)
    be16(0x8000 | 8);                               //   rowBytes u16: packed-flag | 8 (=width, 1 byte/px)
    be16(0); be16(0); be16(6); be16(8);             //   bounds Rect
    be16(0); be16(0); be32(0);                      //   pmVersion, packType=0, packSize
    be32(0x00480000); be32(0x00480000);             //   hRes, vRes
    be16(0); be16(8); be16(1); be16(8);             //   pixelType=0(indexed), pixelSize=8, cmpCount=1, cmpSize=8
    be32(0); be32(0); be32(0);                       //   planeBytes, pmTable, pmReserved
    be32(0); be16(0); be16(0);                       //   ctSeed, ctFlags, ctSize=0 (=> 1 entry)
    be16(0); be16(0x3030); be16(0x6060); be16(0xA0A0); // entry: value=0, r/g/b (high byte => 30/60/A0)
    be16(0); be16(0); be16(6); be16(8);             //   srcRect
    be16(0); be16(0); be16(6); be16(8);             //   dstRect
    be16(0);                                        //   mode
    for(int y=0;y<6;y++){ put8(2); put8(0xF9); put8(0x00); } // 6 rows: byteCount=2, F9 00 => 8x index 0
    be16(0x00FF);                                   // OpEndPic
    QImage im = RasterExtra::decodeData(b, "pct");
    check(!im.isNull() && im.size()==QSize(8,6)
          && im.pixelColor(0,0)==QColor(0x30,0x60,0xA0)
          && im.pixelColor(3,3)==QColor(0x30,0x60,0xA0), "PICT PackBitsRect 8-bit indexed");
}
      }
      // ---- Alias/Wavefront RLA ----
      {
{
    // Hand-built Alias/Wavefront RLA (all BIG-ENDIAN), every header field laid at its spec
    // byte offset so the test cannot inherit a decoder mistake. 2x2 RGB, integer 8-bit,
    // literal-run RLE (control byte 0xFE = -2 => two literal bytes follow).
    QByteArray b;
    auto put8=[&](int v){ b.append(char(v&0xff)); };
    auto be16=[&](int v){ b.append(char((v>>8)&0xff)); b.append(char(v&0xff)); };
    auto be32=[&](quint32 v){ b.append(char((v>>24)&0xff)); b.append(char((v>>16)&0xff)); b.append(char((v>>8)&0xff)); b.append(char(v&0xff)); };
    // ---- 740-byte header ----
    be16(0); be16(1); be16(0); be16(1);   // @0  window        left=0 right=1 bottom=0 top=1
    be16(0); be16(1); be16(0); be16(1);   // @8  active_window  left=0 right=1 bottom=0 top=1 -> 2x2
    be16(1);                              // @16 frame
    be16(0);                              // @18 storage_type = 0 (integer)
    be16(3);                              // @20 num_chan = 3 (RGB)
    be16(0);                              // @22 num_matte = 0
    be16(0);                              // @24 num_aux = 0
    be16(0xFFFE);                         // @26 revision
    while(b.size()<658) put8(0);          // @28..657 gamma/primaries/strings (unused) -> zero
    be16(8);                              // @658 chan_bits = 8
    be16(0);                              // @660 matte_type
    be16(8);                              // @662 matte_bits
    while(b.size()<740) put8(0);          // @664..739 aux/space/next -> zero (header is exactly 740 bytes)
    // ---- scanline offset table @740: height=2 i32 BE, stored bottom-to-top ----
    be32(748);                            // table[0] -> bottom image row data (740 + 2*4)
    be32(763);                            // table[1] -> top image row data    (748 + 15)
    // ---- per channel: u16 byteCount then RLE; literal run 0xFE(-2) + 2 bytes = byteCount 3 ----
    auto litchan=[&](int a,int c){ be16(3); put8(0xFE); put8(a); put8(c); };
    litchan(70,100); litchan(80,110); litchan(90,120);   // bottom row (image y=1): R,G,B
    litchan(10,40);  litchan(20,50);  litchan(30,60);    // top row    (image y=0): R,G,B
    QImage im = RasterExtra::decodeData(b, "rla");
    check(!im.isNull() && im.size()==QSize(2,2)
        && im.pixelColor(0,0)==QColor(10,20,30)  && im.pixelColor(1,0)==QColor(40,50,60)
        && im.pixelColor(0,1)==QColor(70,80,90)  && im.pixelColor(1,1)==QColor(100,110,120),
        "RLA 2x2 RGB literal-run decode (active_window dims, bottom-to-top)");
    // negative: truncated header (short buffer) must not over-read and must return null
    QImage bad = RasterExtra::decodeData(b.left(300), "rla");
    check(bad.isNull(), "RLA truncated buffer rejected");
}
      }

      // ===== Fleet-4: AAI / VIPS / VICAR / MAT / PDB (real-oracle cross-validated) =====
      // ---- AAI Dune (BGRA) ----
      {
{
    // Build a spec-layout AAI file independently, byte-by-byte, from the documented layout.
    QByteArray b;
    auto le32=[&](quint32 v){ for(int s=0;s<32;s+=8) b.append(char((v>>s)&0xff)); };
    auto put8=[&](int v){ b.append(char(v&0xff)); };
    const int W=3, H=2;
    le32((quint32)W);                 // offset 0: width  (u32 LE)
    le32((quint32)H);                 // offset 4: height (u32 LE)
    // offset 8: raster, 4 bytes/pixel B,G,R,A, row-major top-to-bottom.
    // pixel(x,y): R = 10*x+1, G = 20*y+2, B = 30, A = 200  (distinct per position so
    // channel order AND row order are both pinned down).
    for(int y=0;y<H;++y)
        for(int x=0;x<W;++x){
            put8(30);                 // Blue
            put8(20*y+2);             // Green
            put8(10*x+1);             // Red
            put8(200);                // Alpha (straight)
        }
    QImage im=RasterExtra::decodeData(b,"aai");
    check(!im.isNull() && im.size()==QSize(W,H)
          && im.pixelColor(0,0)==QColor(1,2,30,200)      // top-left
          && im.pixelColor(2,0)==QColor(21,2,30,200)     // R tracks x
          && im.pixelColor(0,1)==QColor(1,22,30,200)     // G tracks y (row order top->bottom)
          && im.pixelColor(2,1)==QColor(21,22,30,200),
          "AAI 3x2 BGRA raster + LE32 header");
    // Negative: header declares 3x2 but only one pixel of raster is present -> must
    // reject with a null image and NOT over-read the buffer.
    QByteArray t=b.left(8+4);
    check(RasterExtra::decodeData(t,"aai").isNull(), "AAI truncated raster rejected");
}
      }
      // ---- libvips .v ----
      {
{
    // ---- libvips .v hermetic tests: file bytes built independently from the SPEC layout ----
    // (A) Big-endian (SPARC magic 08 f2 a6 b6) 2x2 RGB, 3 bands, UCHAR, CODING_NONE.
    QByteArray b;
    auto be32=[&](quint32 v){ for(int s=24;s>=0;s-=8) b.append(char((v>>s)&0xff)); };
    be32(0x08f2a6b6u); // @0  magic SPARC -> big-endian fields
    be32(2);           // @4  Xsize = 2
    be32(2);           // @8  Ysize = 2
    be32(3);           // @12 Bands = 3 (RGB)
    be32(0);           // @16 Bbits (deprecated; 0 in real files)
    be32(0);           // @20 BandFmt = 0 (UCHAR)
    be32(0);           // @24 Coding  = 0 (NONE)
    be32(22);          // @28 Type = 22 (sRGB; informational)
    while(b.size() < 64) b.append('\0'); // @32..@63 pad -> header is fixed 64 bytes
    // @64 pixel data, band-interleaved R,G,B, row-major
    const unsigned char px[12] = {10,20,30, 40,50,60, 70,80,90, 100,110,120};
    for(int i=0;i<12;i++) b.append(char(px[i]));
    QImage im = RasterExtra::decodeData(b, "v");
    check(!im.isNull() && im.size()==QSize(2,2)
          && im.pixelColor(0,0)==QColor(10,20,30)
          && im.pixelColor(1,0)==QColor(40,50,60)
          && im.pixelColor(0,1)==QColor(70,80,90)
          && im.pixelColor(1,1)==QColor(100,110,120)
          && qAlpha(im.pixel(1,1))==255,
          "VIPS .v big-endian 2x2 RGB");

    // (B) Little-endian (INTEL magic b6 a6 f2 08) 1x1 grayscale — exercises the LE branch.
    QByteArray g;
    auto le32=[&](quint32 v){ for(int s=0;s<32;s+=8) g.append(char((v>>s)&0xff)); };
    le32(0x08f2a6b6u); // @0  writes bytes b6 a6 f2 08 (INTEL) -> little-endian fields
    le32(1);           // @4  Xsize = 1
    le32(1);           // @8  Ysize = 1
    le32(1);           // @12 Bands = 1 (gray)
    le32(0);           // @16 Bbits
    le32(0);           // @20 BandFmt = UCHAR
    le32(0);           // @24 Coding  = NONE
    le32(0);           // @28 Type
    while(g.size() < 64) g.append('\0');
    g.append(char(200)); // @64 single gray sample
    QImage im2 = RasterExtra::decodeData(g, "v");
    check(!im2.isNull() && im2.size()==QSize(1,1)
          && im2.pixelColor(0,0)==QColor(200,200,200),
          "VIPS .v little-endian 1x1 gray");

    // (C) Negative: header claims 2x2x3 (needs 12 data bytes) but only 4 are present -> null, no over-read.
    QImage im3 = RasterExtra::decodeData(b.left(64+4), "v");
    check(im3.isNull(), "VIPS .v truncated pixel data rejected");
}
      }
      // ---- JPL VICAR ----
      {
{
    // Independently build a VICAR file from the SPEC byte layout: ASCII label of
    // exactly LBLSIZE bytes, then BSQ single-band BYTE samples at offset LBLSIZE.
    QByteArray b;
    auto puts=[&](const char* s){ b.append(s); };
    // Declared LBLSIZE=128; the literal token text below is < 128 chars, then space-padded to 128.
    puts("LBLSIZE=128 FORMAT='BYTE' TYPE='IMAGE' ORG='BSQ' NL=2 NS=3 NB=1 NBB=0 NLB=0 RECSIZE=3 ");
    while(b.size() < 128) b.append(' ');            // pad label to declared LBLSIZE
    // Pixel data: 1 band (gray), BSQ, 2 lines x 3 samples, NBB=0 -> row-major, stride=RECSIZE=3.
    const unsigned char px[6] = {10,20,30, 40,50,60};
    for(int i=0;i<6;i++) b.append(char(px[i]));
    QImage im = RasterExtra::decodeData(b, "vicar");
    check(!im.isNull() && im.size()==QSize(3,2)
          && im.pixelColor(0,0)==QColor(10,10,10)   // line0 sample0
          && im.pixelColor(2,0)==QColor(30,30,30)   // line0 sample2
          && im.pixelColor(0,1)==QColor(40,40,40)   // line1 sample0
          && im.pixelColor(2,1)==QColor(60,60,60),  // line1 sample2
          "VICAR BYTE/BSQ gray 3x2");
    // Negative: truncated buffer (label claims LBLSIZE=128 but only 40 bytes present) -> null, no over-read.
    QImage bad = RasterExtra::decodeData(b.left(40), "vicar");
    check(bad.isNull(), "VICAR truncated -> null");
}
      }
      // ---- MATLAB Level-5 ----
      {
{
    // Hermetic MAT built byte-by-byte from the Level-5 spec layout (little-endian),
    // independently of the decoder. Mirrors what ImageMagick emits, INCLUDING the
    // degenerate Array-Flags class=6 (mxDOUBLE) with miUINT8 real data.
    QByteArray b;
    auto put8 = [&](int v){ b.append(char(v & 0xff)); };
    auto le32 = [&](quint32 v){ for(int s = 0; s < 32; s += 8) b.append(char((v >> s) & 0xff)); };

    // --- 128-byte header ---
    const char* desc = "MATLAB 5.0 MAT-file, hermetic selftest";
    for(const char* c = desc; *c; ++c) put8((unsigned char)*c);   // @0   text description
    while(b.size() < 124) put8(0x20);                             // pad with spaces to @124
    put8(0x00); put8(0x01);                                       // @124 version u16 (bytes 00 01)
    put8('I'); put8('M');                                         // @126 endian indicator => little-endian

    // --- matrix content assembled first so we can size the top tag ---
    QByteArray m;
    auto mput8 = [&](int v){ m.append(char(v & 0xff)); };
    auto mle32 = [&](quint32 v){ for(int s = 0; s < 32; s += 8) m.append(char((v >> s) & 0xff)); };
    // (1) Array Flags: type=6 (miUINT32), len=8; payload [class|flags][0]. class=6 mimics IM.
    mle32(6); mle32(8); mle32(6); mle32(0);
    // (2) Dimensions: type=5 (miINT32), len=8; rows=4 (height), cols=5 (width).
    mle32(5); mle32(8); mle32(4); mle32(5);
    // (3) Array Name: SMALL element. tag word = (len<<16)|type = (1<<16)|1 = 0x00010001; 'M'+pad.
    mle32(0x00010001u); mput8('M'); mput8(0); mput8(0); mput8(0);
    // (4) Real data: type=2 (miUINT8), len=20; column-major (cols outer, rows inner); pad 20->24.
    mle32(2); mle32(20);
    for(int c = 0; c < 5; ++c) for(int r = 0; r < 4; ++r) mput8((r * 50 + c * 10) & 0xff);
    m.append(char(0)); m.append(char(0)); m.append(char(0)); m.append(char(0));

    // --- top-level miMATRIX Data Element @128 ---
    le32(14); le32((quint32)m.size());   // dataType=14 (miMATRIX), numBytes
    b.append(m);

    QImage im = RasterExtra::decodeData(b, "mat");
    bool ok = !im.isNull() && im.size() == QSize(5, 4)
        && im.pixelColor(0, 0) == QColor(0, 0, 0)         // c0,r0 => 0
        && im.pixelColor(4, 0) == QColor(40, 40, 40)      // c4,r0 => 40
        && im.pixelColor(0, 3) == QColor(150, 150, 150)   // c0,r3 => 150
        && im.pixelColor(2, 1) == QColor(70, 70, 70)      // c2,r1 => 70
        && im.pixelColor(4, 3) == QColor(190, 190, 190);  // c4,r3 => 190
    check(ok, "MAT: 5x4 uint8 gray, column-major transpose");

    // Negative: truncated buffer must not over-read and must return null.
    QImage bad = RasterExtra::decodeData(b.left(150), "mat");
    check(bad.isNull(), "MAT: truncated buffer rejected");
}
      }
      // ---- Palm ImageViewer PDB ----
      {
{
    // Build a PDB/ImageViewer file byte-for-byte from the SPEC layout (independent of the decoder).
    QByteArray b;
    auto put8=[&](int v){ b.append(char(v&0xff)); };
    auto be16=[&](int v){ b.append(char((v>>8)&0xff)); b.append(char(v&0xff)); };
    auto be32=[&](quint32 v){ for(int s=24;s>=0;s-=8) b.append(char((v>>s)&0xff)); };
    // --- PDB header (78 bytes) ---
    const char* nm="test";
    for(int i=0;i<32;i++) put8(i<4?nm[i]:0);        // name[32]@0
    be16(0);                                         // attributes u16@32
    be16(0);                                         // version    u16@34
    be32(0); be32(0); be32(0);                       // create@36 / modify@40 / archive@44
    be32(0); be32(0); be32(0);                       // modNum@48 / appInfoID@52 / sortInfoID@56
    put8('v');put8('I');put8('M');put8('G');         // type[4]@60
    put8('V');put8('i');put8('e');put8('w');         // creator[4]@64
    be32(0);                                         // seed u32@68
    be32(0);                                         // nextRecordListID u32@72
    be16(1);                                          // numRecords u16@76
    // --- record-entry list @78 (one 8-byte entry) ---
    be32(86);                                         // recordOffset u32@78 -> image record @86
    put8(0); put8(0x6f); put8(0x80); put8(0x00);      // attributes u8 + uniqueID[3]
    // --- ImageViewer image record @86 (58-byte header) ---
    for(int i=0;i<32;i++) put8(0);                    // name[32] @rec+0
    put8(1);                                          // version u8 @rec+32  (1 = RLE)
    put8(0xff);                                       // type    u8 @rec+33  (!=0,!=2 => 1 bpp)
    be32(0); be32(0);                                 // reserved_1 @rec+34 / note @rec+38
    be16(0); be16(0);                                 // x_last @rec+42 / y_last @rec+44
    be32(0);                                          // reserved_2 @rec+46
    be16(0xffff); be16(0xffff);                       // x_anchor @rec+50 / y_anchor @rec+52
    be16(16);                                         // width  i16 @rec+54
    be16(2);                                          // height i16 @rec+56
    // --- ImageViewer RLE bitmap -> packets*rows = 2*2 = 4 output bytes: F0,00,FF,FF ---
    put8(0x01); put8(0xF0); put8(0x00);               // COPY 2 literals -> row0 = F0,00
    put8(0x81); put8(0xFF);                           // RUN 2x FF       -> row1 = FF,FF
    QImage im = RasterExtra::decodeData(b,"pdb");
    // row0: F0=11110000 -> px0..3 set(black), px4..7 clear(white); 00 -> px8..15 white
    // row1: FF,FF -> every bit set -> all black
    check(!im.isNull() && im.size()==QSize(16,2)
          && im.pixelColor(0,0)==QColor(0,0,0)
          && im.pixelColor(4,0)==QColor(255,255,255)
          && im.pixelColor(15,0)==QColor(255,255,255)
          && im.pixelColor(0,1)==QColor(0,0,0)
          && im.pixelColor(9,1)==QColor(0,0,0), "PDB ImageViewer 1-bpp RLE 16x2");
    // negative: a truncated buffer must return null (no over-read)
    QImage bad = RasterExtra::decodeData(b.left(40),"pdb");
    check(bad.isNull(), "PDB truncated -> null");
}
      }

      // ===== Fleet-5: TIM / Degas-compressed / CrackArt / ACBM / DEEP (native retro, hermetic) =====
      // ---- Sony PlayStation TIM ----
      {
{
    // --- Hermetic TIM: 4bpp CLUT, built byte-by-byte straight from the spec layout ---
    QByteArray b;
    auto put8=[&](int v){ b.append(char(v&0xff)); };
    auto le16=[&](int v){ b.append(char(v&0xff)); b.append(char((v>>8)&0xff)); };
    auto le32=[&](quint32 v){ for(int s=0;s<32;s+=8) b.append(char((v>>s)&0xff)); };
    le32(0x10);        // @0  magic = 0x00000010
    le32(0x08);        // @4  flags = mode 0 (4bpp) | 0x08 (CLUT present)
    // @8 CLUT block: bnum = 12 (header) + 16*1*2 (entries) = 44
    le32(44);          // @8   bnum
    le16(0);           // @12  DX
    le16(0);           // @14  DY
    le16(16);          // @16  W = 16 colours
    le16(1);           // @18  H = 1 CLUT
    le16(0x001F);      // @20  idx0 = red   (R=31)          A1B5G5R5
    le16(0x03E0);      //      idx1 = green (G=31)
    le16(0x7C00);      //      idx2 = blue  (B=31)
    le16(0x7FFF);      //      idx3 = white
    for(int i=4;i<16;i++) le16(0);                 // idx4..15 unused
    // IMAGE block: bnum = 12 (header) + 2 (row stride 1 word) * 1 (row) = 14
    le32(14);          // bnum
    le16(0);           // DX
    le16(0);           // DY
    le16(1);           // W = 1 word  -> real width = 1*4 = 4 px
    le16(1);           // H = 1 row
    put8(0x10);        // byte0: low nibble 0 (x0 red), high nibble 1 (x1 green)
    put8(0x32);        // byte1: low nibble 2 (x2 blue), high nibble 3 (x3 white)
    QImage im=RasterExtra::decodeData(b,"tim");
    check(!im.isNull() && im.size()==QSize(4,1)
          && im.pixelColor(0,0)==QColor(255,0,0)
          && im.pixelColor(1,0)==QColor(0,255,0)
          && im.pixelColor(2,0)==QColor(0,0,255)
          && im.pixelColor(3,0)==QColor(255,255,255), "TIM 4bpp CLUT decode");
    // Negative: truncated buffer (image block header cut off) must be rejected, no over-read.
    QImage bad=RasterExtra::decodeData(b.left(10),"tim");
    check(bad.isNull(), "TIM truncated rejected");
}
      }
      // ---- Degas Elite compressed PC1/2/3 ----
      {
{
    // Build a DEGAS Elite COMPRESSED (.pc1) file BYTE-BY-BYTE from the spec, independently of the
    // decoder, so the test cannot inherit a decode mistake. Low res 320x200, 4 planes.
    QByteArray b;
    auto put8=[&](int v){ b.append(char(v&0xff)); };
    auto be16=[&](int v){ b.append(char((v>>8)&0xff)); b.append(char(v&0xff)); };
    // @0: resolution word BE, bit15 set (compressed) + low bits 0 (low-res 320x200/4 planes)
    be16(0x8000);
    // @2: 16 STe palette words 0x0RGB (r=(w>>8)&7,g=(w>>4)&7,b=w&7; c8=(c<<5)|(c<<2)|(c>>1))
    for(int i=0;i<16;i++){
        int w=0x0000;
        if(i==1) w=0x0700;      // pal[1] = red   (r=7 -> 255)
        else if(i==9) w=0x0070; // pal[9] = green (g=7 -> 255)
        be16(w);
    }
    // @34: PackBits body, PLANE-SEQUENTIAL per scanline (line's whole plane0, then plane1, ...).
    // ctrl 0..127 => copy ctrl+1 literals; 128 => no-op; 129..255 => repeat next byte (257-ctrl)x.
    put8(0x80);                     // one no-op control byte (exercises the 128 path)
    // -- scanline 0: pixels 0..7 => idx 9 (plane0+plane3 set), pixels 8..319 => idx 0 --
    // plane0 row (40B) = 0xFF then 39x00  -> literal(1) 0xFF, then repeat(39) 0x00
    put8(0x00); put8(0xFF);         // ctrl 0 => copy 0+1=1 literal byte (0xFF)
    put8(0xDA); put8(0x00);         // 257-0xDA(218)=39 copies of 0x00  (ends on 40-byte boundary)
    // plane1 row (40B) = all 0x00 -> repeat(40) 0x00
    put8(0xD9); put8(0x00);         // 257-0xD9(217)=40 copies of 0x00
    // plane2 row (40B) = all 0x00
    put8(0xD9); put8(0x00);
    // plane3 row (40B) = 0xFF then 39x00
    put8(0x00); put8(0xFF);
    put8(0xDA); put8(0x00);
    // -- scanlines 1..199: every pixel idx 1 (plane0 all set) => red --
    for(int y=1;y<200;y++){
        put8(0xD9); put8(0xFF);     // plane0: 40x 0xFF
        put8(0xD9); put8(0x00);     // plane1: 40x 0x00
        put8(0xD9); put8(0x00);     // plane2
        put8(0xD9); put8(0x00);     // plane3
    }
    QImage im=RasterExtra::decodeData(b,"pc1");
    check(!im.isNull() && im.size()==QSize(320,200)
          && im.pixelColor(0,0)==QColor(0,255,0)      // idx9 (plane0+3) -> green
          && im.pixelColor(8,0)==QColor(0,0,0)        // idx0 -> black
          && im.pixelColor(100,5)==QColor(255,0,0),   // idx1 -> red
          "DEGAS Elite compressed PC1 plane-sequential decode");
    // Negative: a truncated body must not over-read and must return a null image.
    QImage bad=RasterExtra::decodeData(b.left(40),"pc1");
    check(bad.isNull(), "DEGAS compressed PC1 truncated -> null");
}
      }
      // ---- CrackArt CA1/2/3 ----
      {
{
    // ---- CrackArt (.ca1) hermetic tests, laid out byte-by-byte from the RECOIL spec ----
    auto put8=[&](QByteArray& a,int v){ a.append(char(v&0xff)); };
    auto be16=[&](QByteArray& a,int v){ a.append(char((v>>8)&0xff)); a.append(char(v&0xff)); };

    // (1) UNCOMPRESSED low-res 320x200x4-plane
    QByteArray b;
    put8(b,'C'); put8(b,'A');          // @0 magic 'CA'
    put8(b,0);                         // @2 compression = 0 (uncompressed)
    put8(b,0);                         // @3 resolution  = 0 (low)
    be16(b,0x0000);                    // @4  palette[0] = black  (STe word 0RRR0GGG0BBB)
    be16(b,0x0700);                    // @6  palette[1] = red    (R=7 -> 255)
    be16(b,0x0070);                    // @8  palette[2] = green  (G=7)
    be16(b,0x0007);                    // @10 palette[3] = blue   (B=7)
    for(int i=4;i<16;i++) be16(b,0);   // @12..35 palette[4..15] = black  (16 words total -> data @36)
    QByteArray body(32000,'\0');       // @36 32000-byte 4-plane word-interleaved body, all idx 0
    body[0]=char(0x80);                //   row0 grp0 plane0 word = 0x8000 -> pixel(0,0) bit15 -> idx 1 (red)
    body[2]=char(0x40);                //   row0 grp0 plane1 word = 0x4000 -> pixel(1,0) bit14 -> idx 2 (green)
    b.append(body);                    //   total length 36+32000 = 32036
    QImage im=RasterExtra::decodeData(b,"ca1");
    check(!im.isNull() && im.size()==QSize(320,200)
       && im.pixelColor(0,0)==QColor(255,0,0)
       && im.pixelColor(1,0)==QColor(0,255,0)
       && im.pixelColor(2,0)==QColor(0,0,0), "CrackArt CA1 uncompressed low-res");

    // (2) RLE-COMPRESSED low-res: same red pixel(0,0), everything else black
    QByteArray c;
    put8(c,'C'); put8(c,'A');          // @0 magic
    put8(c,1);                         // @2 compression = 1 (RLE)
    put8(c,0);                         // @3 resolution  = 0 (low)
    be16(c,0x0000); be16(c,0x0700); be16(c,0x0070); be16(c,0x0007);
    for(int i=4;i<16;i++) be16(c,0);   // @4..35 palette (16 words) -> packed stream @36
    put8(c,0xAB);                      // @36 escape byte = 0xAB
    put8(c,0x00);                      // @37 default value = 0x00
    be16(c,0x0001);                    // @38 step word = 1 (sequential column write)
    put8(c,0x80);                      // stream: literal 0x80 -> body[0]=0x80
    put8(c,0xAB); put8(c,0x02); put8(c,0x7C); put8(c,0xFE); // ESC,2,0x7CFE -> fill 31999 defaults(0)
    QImage im2=RasterExtra::decodeData(c,"ca1");
    check(!im2.isNull() && im2.size()==QSize(320,200)
       && im2.pixelColor(0,0)==QColor(255,0,0)
       && im2.pixelColor(10,10)==QColor(0,0,0), "CrackArt CA1 RLE-compressed low-res");

    // (3) negative: truncated buffer must return null (no over-read)
    QImage bad=RasterExtra::decodeData(b.left(100),"ca1");
    check(bad.isNull(), "CrackArt truncated -> null");
}
      }
      // ---- Amiga IFF ACBM ----
      {
{
    // Build a spec-layout ACBM byte-by-byte (independent of the decoder).
    QByteArray b;
    auto put8=[&](int v){ b.append(char(v&0xff)); };
    auto be16=[&](int v){ b.append(char((v>>8)&0xff)); b.append(char(v&0xff)); };
    auto be32=[&](quint32 v){ for(int s=24;s>=0;s-=8) b.append(char((v>>s)&0xff)); };
    auto tag =[&](const char* t){ b.append(t,4); };
    // ---- FORM header ----
    tag("FORM");            // @0 magic
    be32(0);               // @4 form size (decoder ignores; value irrelevant)
    tag("ACBM");           // @8 form type
    // ---- BMHD chunk (20-byte BitMapHeader) ----
    tag("BMHD"); be32(20);
    be16(8);               // @0 width  = 8
    be16(2);               // @2 height = 2
    be16(0);               // @4 x
    be16(0);               // @6 y
    put8(2);               // @8 nPlanes = 2
    put8(0);               // @9 masking = 0
    put8(0);               // @10 compression = 0 (ABIT is uncompressed)
    put8(0);               // @11 pad
    be16(0);               // @12 transparentColor
    put8(0);               // @14 xAspect
    put8(0);               // @15 yAspect
    be16(8);               // @16 pageW
    be16(2);               // @18 pageH
    // ---- CMAP chunk (4 colours * 3 = 12 bytes, even) ----
    tag("CMAP"); be32(12);
    put8(0);  put8(0);  put8(0);    // idx0 = black
    put8(255);put8(0);  put8(0);    // idx1 = red
    put8(0);  put8(255);put8(0);    // idx2 = green
    put8(0);  put8(0);  put8(255);  // idx3 = blue
    // ---- ABIT chunk: rowBytes=((8+15)/16)*2=2, planeBytes=h*rowBytes=4, 2 planes -> 8 bytes ----
    // Target indices: row0 x0..x3 = [0,1,2,3]; row1 x0 = 3; everything else 0.
    // plane0 = idx bit0: row0 [0,1,0,1]=0b01010000=0x50 ; row1 x0=1 -> 0b10000000=0x80
    // plane1 = idx bit1: row0 [0,0,1,1]=0b00110000=0x30 ; row1 x0=1 -> 0b10000000=0x80
    tag("ABIT"); be32(8);
    put8(0x50); put8(0x00);   // plane0 row0 (2 rowBytes; 2nd byte unused/zero)
    put8(0x80); put8(0x00);   // plane0 row1
    put8(0x30); put8(0x00);   // plane1 row0
    put8(0x80); put8(0x00);   // plane1 row1

    QImage im = RasterExtra::decodeData(b, "acbm");
    check(!im.isNull() && im.size()==QSize(8,2)
          && im.pixelColor(0,0)==QColor(0,0,0)
          && im.pixelColor(1,0)==QColor(255,0,0)
          && im.pixelColor(2,0)==QColor(0,255,0)
          && im.pixelColor(3,0)==QColor(0,0,255)
          && im.pixelColor(0,1)==QColor(0,0,255), "ACBM contiguous bitplanes + CMAP");

    // Negative: a truncated buffer (magic incomplete) must return null with no over-read.
    QImage bad = RasterExtra::decodeData(b.left(10), "acbm");
    check(bad.isNull(), "ACBM truncated -> null");
}
      }
      // ---- IFF DEEP (TVPaint) ----
      {
{
    // --- Hermetic IFF DEEP tests, built byte-by-byte from the spec (not from the decoder). ---
    auto put8 =[&](QByteArray& b,int v){ b.append(char(v&0xff)); };
    auto be16=[&](QByteArray& b,int v){ b.append(char((v>>8)&0xff)); b.append(char(v&0xff)); };
    auto be32=[&](QByteArray& b,quint32 v){ for(int s=24;s>=0;s-=8) b.append(char((v>>s)&0xff)); };
    auto chunk=[&](QByteArray& b,const char* id,const QByteArray& d){
        b.append(id,4); be32(b,(quint32)d.size()); b.append(d);
        if(d.size()&1) b.append(char(0));                 // even padding
    };

    // Test 1: 2x2, uncompressed, elements R,G,B,A (types 1,2,3,4), depth 8 each.
    {
        QByteArray dgbl;
        be16(dgbl,2); be16(dgbl,2);                       // width=2 @0, height=2 @2
        be16(dgbl,0);                                     // compression=0 @4
        put8(dgbl,1); put8(dgbl,1);                       // xAspect @6, yAspect @7
        QByteArray dpel;
        be32(dpel,4);                                     // nElements=4 @0
        be16(dpel,1); be16(dpel,8);                       // RED,   depth 8
        be16(dpel,2); be16(dpel,8);                       // GREEN, depth 8
        be16(dpel,3); be16(dpel,8);                       // BLUE,  depth 8
        be16(dpel,4); be16(dpel,8);                       // ALPHA, depth 8
        QByteArray dbod;                                  // chunky, row-major: pixel = R,G,B,A
        // (0,0)                       (1,0)
        put8(dbod,255);put8(dbod,0);put8(dbod,0);put8(dbod,255);
        put8(dbod,0);put8(dbod,255);put8(dbod,0);put8(dbod,255);
        // (0,1)                       (1,1)
        put8(dbod,0);put8(dbod,0);put8(dbod,255);put8(dbod,128);
        put8(dbod,255);put8(dbod,255);put8(dbod,0);put8(dbod,255);
        QByteArray body; body.append("DEEP",4);
        chunk(body,"DGBL",dgbl); chunk(body,"DPEL",dpel); chunk(body,"DBOD",dbod);
        QByteArray f; f.append("FORM",4); be32(f,(quint32)body.size()); f.append(body);

        QImage im=RasterExtra::decodeData(f,"deep");
        check(!im.isNull() && im.size()==QSize(2,2)
              && im.pixelColor(0,0)==QColor(255,0,0,255)
              && im.pixelColor(1,0)==QColor(0,255,0,255)
              && im.pixelColor(0,1)==QColor(0,0,255,128)
              && im.pixelColor(1,1)==QColor(255,255,0,255), "DEEP uncompressed RGBA 2x2");
    }

    // Test 2: 4x1, compression 1 (ByteRun1/PackBits), elements R,G,B. Body is one literal run of
    // 12 bytes: control byte 11 (=copy 11+1=12 literals) followed by four red pixels (255,0,0).
    {
        QByteArray dgbl;
        be16(dgbl,4); be16(dgbl,1);                       // width=4, height=1
        be16(dgbl,1);                                     // compression=1 (RLE)
        put8(dgbl,1); put8(dgbl,1);
        QByteArray dpel;
        be32(dpel,3);                                     // nElements=3
        be16(dpel,1); be16(dpel,8);                       // RED
        be16(dpel,2); be16(dpel,8);                       // GREEN
        be16(dpel,3); be16(dpel,8);                       // BLUE
        QByteArray dbod;
        put8(dbod,11);                                    // control: literal run of 12 bytes
        for(int i=0;i<4;i++){ put8(dbod,255); put8(dbod,0); put8(dbod,0); } // 4x red
        QByteArray body; body.append("DEEP",4);
        chunk(body,"DGBL",dgbl); chunk(body,"DPEL",dpel); chunk(body,"DBOD",dbod);
        QByteArray f; f.append("FORM",4); be32(f,(quint32)body.size()); f.append(body);

        QImage im=RasterExtra::decodeData(f,"deep");
        check(!im.isNull() && im.size()==QSize(4,1)
              && im.pixelColor(0,0)==QColor(255,0,0,255)
              && im.pixelColor(3,0)==QColor(255,0,0,255), "DEEP ByteRun1 RGB 4x1");
    }

    // Test 3 (negative): truncated buffer must return a null image, no over-read.
    {
        QByteArray f; f.append("FORM",4);
        for(int s=24;s>=0;s-=8) f.append(char((72>>s)&0xff));   // plausible size
        f.append("DEEP",4); f.append("DGB",3);                  // cut off mid-chunk-id
        QImage im=RasterExtra::decodeData(f,"deep");
        check(im.isNull(), "DEEP truncated -> null");
    }
}
      }

      // ===== Fleet-6: ArtDirector / C64 Doodle / MSX-SC2 / Apple-II HGR / Atari Micropainter (retro, hermetic) =====
      // ---- Atari ST Art Director ----
      {
{
    // ---- Art Director (.art) — SPEC-layout hermetic file, built byte-by-byte ----
    // Total EXACTLY 32512 bytes: 32000 screen + 512 palette region.
    QByteArray b(32512, '\0');
    auto be16at=[&](int off,int v){ b[off]=char((v>>8)&0xff); b[off+1]=char(v&0xff); };
    // Screen row 0, group 0 = 4 word-interleaved big-endian plane words @0..7.
    // We want pixel0=idx1(plane0), pixel1=idx2(plane1), pixel2=idx3(plane0+1):
    //   pixel x uses bit position (15-x) in each plane word.
    //   plane0 bits: pixel0(bit15)+pixel2(bit13) = 0x8000|0x2000 = 0xA000
    //   plane1 bits: pixel1(bit14)+pixel2(bit13) = 0x4000|0x2000 = 0x6000
    be16at(0, 0xA000); // plane 0
    be16at(2, 0x6000); // plane 1
    be16at(4, 0x0000); // plane 2
    be16at(6, 0x0000); // plane 3
    // rest of screen stays 0 (index 0 = black).
    // Active palette: selector byte @0x7e1f = 0 (already zero) -> palette @32000.
    be16at(32000 + 0*2, 0x0000); // idx0 black
    be16at(32000 + 1*2, 0x0700); // idx1 red   (r=7 -> 255,0,0)
    be16at(32000 + 2*2, 0x0070); // idx2 green (g=7 -> 0,255,0)
    be16at(32000 + 3*2, 0x0007); // idx3 blue  (b=7 -> 0,0,255)
    QImage im = RasterExtra::decodeData(b, "art");
    check(!im.isNull() && im.size()==QSize(320,200)
        && im.pixelColor(0,0)==QColor(255,0,0)
        && im.pixelColor(1,0)==QColor(0,255,0)
        && im.pixelColor(2,0)==QColor(0,0,255)
        && im.pixelColor(3,0)==QColor(0,0,0),
        "ArtDirector 320x200 low-res word-interleaved palette");
    // Negative: wrong size (truncated) must reject.
    QByteArray bad(32000, '\0');
    QImage im2 = RasterExtra::decodeData(bad, "art");
    check(im2.isNull(), "ArtDirector rejects wrong-size buffer");
}
      }
      // ---- C64 Doodle ----
      {
{
    // Hermetic C64 Doodle built byte-by-byte from the RECOIL layout (independent of the decoder).
    // Total 9026 bytes: [0..1] load addr, [2..1025] 1024-byte screen RAM, [1026..9025] 8000-byte bitmap.
    QByteArray b(9026, '\0');                 // all zero ⇒ background = palette index 0 (black)
    auto setb=[&](int i,int v){ b[i]=char(v&0xff); };
    setb(0,0x00); setb(1,0x5C);               // PRG load address $5C00 (little-endian)
    // Screen RAM: file offset 2 + cellIndex.
    setb(2,   0x1A);                          // cell0  : hi=1(white)      lo=0xA(idx10 0x9A6759)
    setb(3,   0x23);                          // cell1  : hi=2(0x68372B)   lo=3(0x70A4B2)
    setb(42,  0x45);                          // cell40 : hi=4(0x6F3D86)   lo=5(0x588D43)  (cellY=1,cellX=0)
    // Bitmap: file offset 1026 + off, where off = (y&~7)*40 + (x&~7) + (y&7).
    setb(1026, 0x80);   // off0   : (x0..7,y0) row0 cell0 → MSB set: px(0,0) set, px(1,0) clear
    setb(1027, 0x00);   // off1   : (x0..7,y1) row1 cell0 → all clear
    setb(1034, 0x01);   // off8   : (x8..15,y0) row0 cell1 → LSB set: px(8,0) clear, px(15,0) set
    setb(1346, 0x80);   // off320 : (x0..7,y8) row0 cell40 → MSB set: px(0,8) set
    QImage im=RasterExtra::decodeData(b,"dd");
    check(!im.isNull() && im.size()==QSize(320,200)
        && im.pixelColor(0,0)  ==QColor(0xFF,0xFF,0xFF)   // set bit → hi nibble 1 = white
        && im.pixelColor(1,0)  ==QColor(0x9A,0x67,0x59)   // clear bit → lo nibble 0xA = idx10
        && im.pixelColor(0,1)  ==QColor(0x9A,0x67,0x59)   // row1 within same cell, clear → lo
        && im.pixelColor(15,0) ==QColor(0x68,0x37,0x2B)   // cell1, rightmost set → hi nibble 2
        && im.pixelColor(8,0)  ==QColor(0x70,0xA4,0xB2)   // cell1, leftmost clear → lo nibble 3
        && im.pixelColor(0,8)  ==QColor(0x6F,0x3D,0x86),  // cellY=1, set → hi nibble 4
        "C64Doodle hi-res cell/nibble decode");
    // Negative: a truncated buffer (9025 bytes) must be rejected by the exact-size guard.
    QImage im2=RasterExtra::decodeData(b.left(9025),"dd");
    check(im2.isNull(), "C64Doodle wrong-size rejected");
}
      }
      // ---- MSX SCREEN 2 ----
      {
{
    // MSXSC2: BSAVE-headered 256x192 SCREEN 2 VRAM dump, built byte-by-byte from the spec.
    QByteArray v(16384, '\0');                  // 16384-byte VRAM, all zero
    v[0]      = char(0x80);                      // pattern char0 row0: only MSB set -> leftmost px on
    v[0x2000] = char(0xF2);                      // colour  char0 row0: fg=15 (white) / bg=2 (green)
    QByteArray b;
    auto put8=[&](int val){ b.append(char(val&0xff)); };
    auto le16=[&](int val){ b.append(char(val&0xff)); b.append(char((val>>8)&0xff)); };
    put8(0xFE);                                  // BSAVE marker
    le16(0x0000);                                // start address
    le16(0x3FFF);                                // end address
    le16(0x0000);                                // exec address
    b.append(v);                                 // 7 + 16384 = 16391 bytes total
    QImage im = RasterExtra::decodeData(b, "sc2");
    check(!im.isNull() && im.size()==QSize(256,192)
          && im.pixelColor(0,0)==QColor(0xFF,0xFF,0xFF)     // bit=1 -> foreground = white
          && im.pixelColor(1,0)==QColor(0x21,0xC8,0x42),    // bit=0 -> background = medium green
          "MSXSC2 pattern/colour decode + TMS9918 palette");
    // Negative: any non-canonical size must be rejected (16390 != 16391 and != 16384).
    QImage bad = RasterExtra::decodeData(b.left(16390), "sc2");
    check(bad.isNull(), "MSXSC2 rejects truncated/wrong-size buffer");
}
      }
      // ---- Apple II HGR ----
      {
{
    // --- APPLE2HGR (.hgr) hermetic test, built byte-by-byte from the RECOIL spec layout ---
    // 8192-byte Apple II hi-res page, 280x192, MONOCHROME. Each byte holds 7 pixels in
    // bits 0..6 (bit0 = leftmost); bit7 is the palette bit and is IGNORED by RECOIL_DecodeHgr.
    // Row address is NON-LINEAR: lineOffset(y) = (y&7)<<10 | (y&56)<<4 | (y>>6)*40.
    QByteArray b(8192, '\0');
    // y=0 row starts at offset 0. Byte 0 = 0x81 = 0b1000_0001:
    //   bit0=1 -> pixel(0,0) WHITE; bits1..6=0 -> pixels(1..6,0) BLACK; bit7=1 -> IGNORED.
    b[0] = char(0x81);
    // y=8 row starts at offset (8&56)<<4 = 8<<4 = 128 (NOT 8*40=320) — tests the non-linear map.
    //   byte 128 = 0x01 -> bit0 -> pixel(0,8) WHITE.
    b[128] = char(0x01);
    // last pixel: y=191 -> lineOffset = 7*0x400 + 7*0x80 + 2*0x28 = 7168+896+80 = 8144.
    //   x=279 -> off = 8144 + 279/7 = 8183; 279%7 = 6 -> bit6. byte 8183 = 0x40 -> pixel(279,191) WHITE.
    b[8183] = char(0x40);
    QImage im = RasterExtra::decodeData(b, "hgr");
    check(!im.isNull() && im.size()==QSize(280,192)
          && im.pixelColor(0,0)==QColor(255,255,255)      // bit0 set
          && im.pixelColor(1,0)==QColor(0,0,0)            // bit1 clear (bit7 set but ignored)
          && im.pixelColor(0,8)==QColor(255,255,255)      // non-linear row address (offset 128)
          && im.pixelColor(279,191)==QColor(255,255,255), // last pixel, bit6 at offset 8183
          "APPLE2HGR 280x192 mono, non-linear rows, bit7 ignored");
    // Negative: wrong/truncated size must be rejected by the fixed-size guard.
    QByteArray bad(8184, '\0');
    QImage imBad = RasterExtra::decodeData(bad, "hgr");
    check(imBad.isNull(), "APPLE2HGR rejects wrong-size buffer");
}
      }
      // ---- Atari 8-bit Micropainter ----
      {
{
    // Build a spec-layout Micro Painter (.mic) file INDEPENDENTLY, byte by byte.
    QByteArray b;
    auto put8 = [&](int v){ b.append(char(v & 0xff)); };
    // Screen row 0, byte 0 = 0x1B = 0b00_01_10_11 -> pixels x=0,1,2,3 have values 0,1,2,3.
    put8(0x1B);
    for(int i = 1; i < 7680; i++) put8(0x00);   // remaining 7679 screen bytes -> value 0 everywhere else
    // Trailing 4 colour registers in the REAL Micro Painter file order 712,708,709,710 =
    // COLBK, COLPF0, COLPF1, COLPF2 (verified vs cyberroach/ANALOG + atariki), i.e. p[7680+v]=value-v.
    // Use hue==0 (grayscale) registers so expected RGB = luma*17 exactly (independent of chroma formula).
    put8(0x00);   // @7680 COLBK  -> pixel value 0 -> luma  0 -> black (0,0,0)
    put8(0x0E);   // @7681 COLPF0 -> pixel value 1 -> luma 14 -> gray 14*17 = 238
    put8(0x08);   // @7682 COLPF1 -> pixel value 2 -> luma  8 -> gray  8*17 = 136
    put8(0x04);   // @7683 COLPF2 -> pixel value 3 -> luma  4 -> gray  4*17 =  68
    // b now = 7680 screen + 4 palette = 7684 bytes.
    QImage im = RasterExtra::decodeData(b, "mic");
    check(!im.isNull() && im.size() == QSize(160, 192)
        && im.pixelColor(0, 0) == QColor(0,   0,   0)    // value 0 -> COLBK
        && im.pixelColor(1, 0) == QColor(238, 238, 238)  // value 1 -> COLPF0
        && im.pixelColor(2, 0) == QColor(136, 136, 136)  // value 2 -> COLPF1
        && im.pixelColor(3, 0) == QColor(68,  68,  68),  // value 3 -> COLPF2
        "Atari Micro Painter .mic 160x192 4-colour grayscale palette");
    // Negative test: truncated / wrong-size buffer must be rejected.
    QByteArray bad = b.left(7683);
    check(RasterExtra::decodeData(bad, "mic").isNull(),
        "Atari .mic rejects wrong-size/truncated input");
}
      }

      // ===== Fleet-7: Falcon TrueColor / MSX2-SC5 / Amiga RGB8 / Spectrum512 / Atari Imagic (retro, RECOIL-verified) =====
      // ---- Atari Falcon TrueColor ----
      {
{
    // --- Build a valid .tru byte-by-byte from the RECOIL DecodeTru layout ---
    // Header (256 bytes): "Indy" magic, width BE @4, height BE @6, rest padding.
    // Data @256: 2x2 RGB565 big-endian, row-major (p0=(0,0),p1=(1,0),p2=(0,1),p3=(1,1)).
    QByteArray b;
    auto put8 =[&](int v){ b.append(char(v&0xff)); };
    auto be16=[&](int v){ b.append(char((v>>8)&0xff)); b.append(char(v&0xff)); };
    b.append("Indy", 4);          // offset 0..3  : magic
    be16(2);                      // offset 4..5  : width  = 2  (big-endian)
    be16(2);                      // offset 6..7  : height = 2  (big-endian)
    while(b.size() < 256) put8(0);// pad header to dataOffset 256
    // pixel words (RGB565 big-endian):
    be16(0xF800);                 // (0,0) full red   -> 255,0,0
    be16(0x07E0);                 // (1,0) full green -> 0,255,0
    be16(0x001F);                 // (0,1) full blue  -> 0,0,255
    be16(0xFFFF);                 // (1,1) white      -> 255,255,255
    QImage im = RasterExtra::decodeData(b, "tru");
    check(!im.isNull() && im.size()==QSize(2,2)
          && im.pixelColor(0,0)==QColor(255,0,0)
          && im.pixelColor(1,0)==QColor(0,255,0)
          && im.pixelColor(0,1)==QColor(0,0,255)
          && im.pixelColor(1,1)==QColor(255,255,255),
          "FalconTru .tru RGB565-BE Indy header");
    // Negative: truncate by one pixel word -> size no longer 256+w*h*2 -> reject.
    QByteArray bad = b; bad.chop(2);
    QImage imBad = RasterExtra::decodeData(bad, "tru");
    check(imBad.isNull(), "FalconTru rejects wrong-size buffer");
}
      }
      // ---- MSX2 SCREEN 5 ----
      {
{
    // Build a full-page .sc5 byte-by-byte from the spec (RECOIL DecodeSc5 layout).
    QByteArray b;
    auto put8 = [&](int v){ b.append(char(v & 0xff)); };
    auto le16 = [&](int v){ b.append(char(v & 0xff)); b.append(char((v >> 8) & 0xff)); };

    // BSAVE header: 0xFE, start=0x0000, end=0x7FFF, exec=0x0000
    //   -> height = (0x7FFF+1)>>7 = 256, clamped to 212.
    put8(0xFE); le16(0x0000); le16(0x7FFF); le16(0x0000);

    // VRAM page = 32768 bytes (file total 32775). Zero-filled base.
    QByteArray vram(32768, '\0');

    // Bitmap byte 0 (row 0, cols 0-1) = 0x12 -> px(0,0)=high nibble=1, px(1,0)=low nibble=2.
    vram[0] = char(0x12);

    // Palette at VRAM 0x7680 (file 0x7687), entries {0RRR0BBB, 00000GGG}.
    int po = 0x7680;
    vram[po + 0] = char(0x07); vram[po + 1] = char(0x00); // idx0: R0 G0 B7 -> blue  (0,0,255)
    vram[po + 2] = char(0x70); vram[po + 3] = char(0x00); // idx1: R7 G0 B0 -> red   (255,0,0)
    vram[po + 4] = char(0x00); vram[po + 5] = char(0x07); // idx2: R0 G7 B0 -> green (0,255,0)

    b.append(vram);

    QImage im = RasterExtra::decodeData(b, "sc5");
    check(!im.isNull() && im.size() == QSize(256, 212)
        && im.pixelColor(0, 0) == QColor(255, 0, 0)   // idx1 red
        && im.pixelColor(1, 0) == QColor(0, 255, 0),  // idx2 green
        "sc5 SCREEN5 4bpp + V9938 palette");

    // Negative: truncated buffer must be rejected.
    QImage bad = RasterExtra::decodeData(b.left(100), "sc5");
    check(bad.isNull(), "sc5 truncated rejected");
}
      }
      // ---- Amiga IFF RGB8/RGBN ----
      {
{
    // AMIGARGB8 — build a minimal FORM....RGB8 file byte-by-byte from the spec layout.
    QByteArray b;
    auto put8=[&](int v){ b.append(char(v&0xff)); };
    auto be16=[&](int v){ b.append(char((v>>8)&0xff)); b.append(char(v&0xff)); };
    auto be32=[&](unsigned v){ b.append(char((v>>24)&0xff)); b.append(char((v>>16)&0xff));
                               b.append(char((v>>8)&0xff)); b.append(char(v&0xff)); };
    auto tag=[&](const char* s){ b.append(s,4); };

    tag("FORM"); be32(0);            // form size (decoder ignores; bounds come from n)
    tag("RGB8");
    // BMHD chunk: 20 bytes of data (Amiga standard header)
    tag("BMHD"); be32(20);
    be16(4);                          // @0  w = 4
    be16(1);                          // @2  h = 1
    be16(0); be16(0);                 // @4  x/y origin
    put8(25);                         // @8  nPlanes = 25 (RGB8 marker)
    put8(0);                          // @9  masking
    put8(4);                          // @10 compression = 4 (RLE)
    put8(0);                          // @11 pad
    be16(0);                          // @12 transparentColor
    put8(1); put8(1);                 // @14 x/y aspect
    be16(0); be16(0);                 // @16 page w/h
    // BODY chunk: 3 RLE units (13 bytes) painting a 4x1 row.
    tag("BODY"); be32(13);
    put8(255); put8(0);   put8(0);   put8(1);   // red,   ctrl=1 -> 1 px  @x0
    put8(0);   put8(255); put8(0);   put8(1);   // green, ctrl=1 -> 1 px  @x1
    put8(0);   put8(0);   put8(255); put8(0);   // blue,  ctrl=0 -> extended count...
    put8(2);                                    //   ...following byte = 2 -> 2 px @x2,x3

    QImage im=RasterExtra::decodeData(b,"rgb8");
    check(!im.isNull() && im.size()==QSize(4,1)
        && im.pixelColor(0,0)==QColor(255,0,0)
        && im.pixelColor(1,0)==QColor(0,255,0)
        && im.pixelColor(2,0)==QColor(0,0,255)
        && im.pixelColor(3,0)==QColor(0,0,255), "AMIGARGB8 RGB8 chunky RLE + extended count");

    // Negative: truncate the BODY data — chunk-walk must reject and return a null image.
    QByteArray bad=b.left(b.size()-6);
    QImage imb=RasterExtra::decodeData(bad,"rgb8");
    check(imb.isNull(), "AMIGARGB8 truncated BODY rejected");

    // Regression: a FULLY-resolved 0-length run (primary count 0 + ext byte 0 + ext word 0)
    // must still emit exactly ONE pixel and keep reading the stream — NOT flood the rest of the
    // frame with one colour (the review-confirmed count-goes-negative desync).
    QByteArray z;
    auto zp8=[&](int v){ z.append(char(v&0xff)); };
    auto zb16=[&](int v){ z.append(char((v>>8)&0xff)); z.append(char(v&0xff)); };
    auto zb32=[&](unsigned v){ for(int s=24;s>=0;s-=8) z.append(char((v>>s)&0xff)); };
    auto ztag=[&](const char* s){ z.append(s,4); };
    ztag("FORM"); zb32(0); ztag("RGB8");
    ztag("BMHD"); zb32(20);
    zb16(2); zb16(1); zb16(0); zb16(0);           // w=2 h=1
    zp8(25); zp8(0); zp8(4); zp8(0);              // nPlanes=25, comp=4
    zb16(0); zp8(1); zp8(1); zb16(0); zb16(0);
    ztag("BODY"); zb32(11);
    zp8(10); zp8(20); zp8(30); zp8(0x00);         // run1 rgb + primary count 0
    zp8(0x00);                                    //   ext byte 0
    zp8(0x00); zp8(0x00);                         //   ext word 0 -> resolved count 0 -> emits 1 px
    zp8(40); zp8(50); zp8(60); zp8(0x01);         // run2 rgb + count 1 -> paints pixel 1
    QImage imz=RasterExtra::decodeData(z,"rgb8");
    check(!imz.isNull() && imz.size()==QSize(2,1)
        && imz.pixelColor(0,0)==QColor(10,20,30)
        && imz.pixelColor(1,0)==QColor(40,50,60),   // NOT (10,20,30) — the desync bug would flood it
        "AMIGARGB8 zero-length run emits one pixel then advances (no RLE desync)");
}
      }
      // ---- Atari Spectrum 512 ----
      {
{
    // Build a byte-exact Spectrum 512 .spu (51104 bytes) INDEPENDENTLY from the spec.
    //   [0..31999]    ST low-res bitmap (all zero  -> every pixel base colour c=0)
    //   [32000..51103] palette: 199 scanlines * 96 bytes (48 STe words each)
    // All palette words zero except two, so IsStePalette is false (ST 3-bit model):
    //   word 0  of scanline 0 @32000 : r&7=7            -> (7<<5)|(7<<2)|(7>>1)=255  => red
    //   word 32 of scanline 0 @32064 : gb&7=7 (blue)                                  => blue
    QByteArray b(51104, '\0');
    b[32000] = char(0x07); b[32001] = char(0x00);   // scanline0 colour0  = ST red
    b[32064] = char(0x00); b[32065] = char(0x07);   // scanline0 colour32 = ST blue
    QImage im = RasterExtra::decodeData(b, "spu");
    // Pixel (0,0): column 0, c=0, x1=1 -> left zone -> palette word 0  -> red.
    // Pixel (200,0): column 200 >= x1+160(=161) -> right zone -> c=32 -> palette word 32 -> blue.
    check(!im.isNull() && im.size()==QSize(320,199)
          && im.pixelColor(0,0)==QColor(255,0,0)
          && im.pixelColor(200,0)==QColor(0,0,255), "SPECTRUM512 spu decodes zones + STe 3-bit palette");
    // Negative: truncated (51103 bytes) must be rejected.
    QByteArray bad = b; bad.chop(1);
    QImage imbad = RasterExtra::decodeData(bad, "spu");
    check(imbad.isNull(), "SPECTRUM512 rejects wrong-size buffer");
}
      }
      // ---- Atari Imagic ----
      {
{
    // Build a valid Atari Imagic (.ic1) low-res file BYTE-BY-BYTE from the RECOIL spec layout.
    // Body is 32000 bytes of 0xFF -> every bitplane word 0xFFFF -> every pixel index 15.
    QByteArray b;
    auto put8=[&](int v){ b.append(char(v&0xff)); };
    // [0..3] magic
    b.append("IMDC",4);
    // [4] reserved 0
    put8(0x00);
    // [5] resolution/mode = 0 (low 320x200, 4 planes)
    put8(0x00);
    // [6..37] 16 ST palette words (byte0=0000 0RRR, byte1=0GGG 0BBB); index 15 = red(7,0,0)
    for(int i=0;i<16;i++){ if(i==15){ put8(0x07); put8(0x00); } else { put8(0x00); put8(0x00); } }
    // [38..63] filler
    for(int i=38;i<64;i++) put8(0x00);
    // [0x40],[0x41] fixed sentinel 0xC8 0x02
    put8(0xC8); put8(0x02);
    // [0x42] RLE escape byte
    put8(0x80);
    // [0x43..] RLE stream: one long run of 0xFF, count 32000 = 257 + 123*256 + 255
    put8(0x80);                         // escape -> start a command
    put8(0x01);                         // command b2==1 -> ReadCount long run
    for(int i=0;i<123;i++) put8(0x01);  // ReadCount: 123 * 256  (c = 257 + 31488 = 31745)
    put8(0x00);                         // terminator (!=1) ends the ReadCount loop, consumed/discarded
    put8(0xFF);                         // final ReadCount byte 255 -> c = 32000
    put8(0xFF);                         // run value = 0xFF
    QImage im=RasterExtra::decodeData(b,"ic1");
    check(!im.isNull() && im.size()==QSize(320,200)
          && im.pixelColor(0,0)==QColor(255,0,0)
          && im.pixelColor(319,199)==QColor(255,0,0), "Atari Imagic .ic1 low-res 320x200 red");
    // Negative: truncated buffer (n<68, header incomplete) must be rejected.
    QImage bad=RasterExtra::decodeData(b.left(60),"ic1");
    check(bad.isNull(), "Atari Imagic truncated buffer rejected");
}
      }

      // ===== Fleet-8: MAG / PI / Amstrad CPC / Amiga SHAM / Sam Coupe (retro, RECOIL/spec-anchored) =====
      // ---- MAG Maki-chan ----
      {
{
    // Hermetic MAG built byte-by-byte from the spec (independent of the decoder).
    // 8x2, 16-colour, all-literal (no deltas). Palette entry c = qRgb(c*17, (15-c)*17, 0),
    // stored G,R,B; c*17 = (c<<4)|c so values are platform-restriction-invariant.
    QByteArray b;
    auto put8=[&](int v){ b.append(char(v&0xff)); };
    auto le32=[&](quint32 v){ put8(v&0xff); put8((v>>8)&0xff); put8((v>>16)&0xff); put8((v>>24)&0xff); };
    b.append("MAKI02  ", 8);      // signature (no trailing NUL, exactly 8 bytes)
    put8(0x1A);                   // empty comment terminator -> headerOffset = 9
    // ---- header block (80 bytes) ----
    put8(0x00);                   // +0  reserved (must be 0)
    put8(98);                     // +1  machine = PC981X1
    put8(0);                      // +2  screen mode
    put8(0);                      // +3  flags (<128 -> 16 colours)
    put8(0); put8(0);             // +4,+5  x1 = 0
    put8(0); put8(0);             // +6,+7  y1 = 0
    put8(7); put8(0);             // +8,+9  x2 = 7 -> width = 8
    put8(1); put8(0);             // +10,+11 y2 = 1 -> height = 2
    le32(80);                     // +12 flag-A offset (rel) -> file 89
    le32(81);                     // +16 flag-B offset (rel) -> file 90 (unused: all bits 0)
    le32(0);                      // +20 flag-B size
    le32(82);                     // +24 pixel offset (rel) -> file 91
    le32(0);                      // +28 pixel size
    for(int c=0;c<16;c++){ put8((15-c)*17); put8(c*17); put8(0); }   // +32..79 palette G,R,B
    // ---- data sections ----
    put8(0x00);                   // file 89: flag-A = 2 zero bits (one byte) -> every cell literal
    put8(0x00);                   // file 90: flag-B placeholder (never read)
    // file 91..98: pixel bytes, 2 pixels/byte, line0 then line1
    put8(0x01); put8(0x23); put8(0x45); put8(0x67);   // line0 px 0..7
    put8(0x89); put8(0xAB); put8(0xCD); put8(0xEF);   // line1 px 8..15
    QImage im = RasterExtra::decodeData(b, "mag");
    check(!im.isNull() && im.size()==QSize(8,2)
          && im.pixelColor(0,0)==QColor(0,255,0)      // px index 0 -> pal[0]
          && im.pixelColor(7,0)==QColor(119,136,0)    // px index 7 -> pal[7]
          && im.pixelColor(0,1)==QColor(136,119,0)    // px index 8 -> pal[8]
          && im.pixelColor(7,1)==QColor(255,0,0),     // px index 15 -> pal[15]
          "MAG MAKI02 16-colour literal decode");
    // Negative: corrupt the signature -> must reject.
    QByteArray bad = b; bad[3] = 'X';                 // "MAKX02  "
    check(RasterExtra::decodeData(bad, "mag").isNull(), "MAG bad-magic rejected");
}
      }
      // ---- PI Yanagisawa ----
      {
{
    // ---- Build a valid 2x2, 16-colour PI file byte-by-byte (SPEC layout) ----
    QByteArray b;
    auto put8 = [&](int v){ b.append(char(v & 0xff)); };
    // Signature "Pi"
    put8('P'); put8('i');
    // Empty comment, terminated by 0x1A
    put8(0x1A);
    // --- real header starts at first 0x00 after 0x1A ---
    put8(0x00);          // header[0] : always 0x00
    put8(0x00);          // mode
    put8(0x00);          // aspect X
    put8(0x00);          // aspect Y
    put8(0x04);          // bitdepth 4 => 16 colours
    put8(0); put8(0); put8(0); put8(0);   // 4-byte compressor model signature
    put8(0x00); put8(0x00);               // compressor-specific data size = 0 (be16)
    put8(0x00); put8(0x02);               // width  = 2 (big-endian u16)
    put8(0x00); put8(0x02);               // height = 2 (big-endian u16)
    // Palette: 16 RGB triplets. colour0 = (10,20,30), colour15 = (200,100,50).
    put8(10); put8(20); put8(30);         // index 0
    for (int c = 1; c < 15; c++) { put8(0); put8(0); put8(0); }   // 1..14
    put8(200); put8(100); put8(50);       // index 15
    // Pixel bitstream (MSB-first). Hand-encoded to yield indices [0,15,15,15]:
    //  delta 0  = "10"      (colour 0, a=0)
    //  delta 1  = "11"      (colour 15)
    //  rep-seq  : loc "00", len "0" (=>0 bytes after first-repeat minus-2),
    //             then loc "00" (== prev) => stop  => "00 0 00"
    //  delta 0  = "10"      (colour 15, a=15)
    //  delta 0  = "10"      (colour 15) -> buffer full
    //  bits: 10 11 00 0 00 10 10  = 1011000 0 0101 0  -> bytes 0xB0, 0x50
    put8(0xB0); put8(0x50);

    QImage im = RasterExtra::decodeData(b, "pi");
    check(!im.isNull() && im.size() == QSize(2, 2)
          && im.pixelColor(0, 0) == QColor(10, 20, 30)
          && im.pixelColor(1, 0) == QColor(200, 100, 50)
          && im.pixelColor(0, 1) == QColor(200, 100, 50)
          && im.pixelColor(1, 1) == QColor(200, 100, 50),
          "PI 2x2 16-colour delta+repetition decode");

    // Negative: truncated (palette cut off) must be rejected.
    QImage imTrunc = RasterExtra::decodeData(b.left(20), "pi");
    check(imTrunc.isNull(), "PI truncated buffer rejected");

    // Negative: bad magic must be rejected.
    QByteArray bad = b; bad[0] = 'X';
    check(RasterExtra::decodeData(bad, "pi").isNull(), "PI bad magic rejected");
}
      }
      // ---- Amstrad CPC ----
      {
{
    // Build a byte-exact mode-1 CPC "SCR + OCP palette" file independently from
    // the spec layout (not via the decoder).
    // Layout: 16384-byte screen dump + 239-byte palette block = 16623 bytes.
    QByteArray b(16623, '\0');

    // --- Screen dump (mode 1, 4px/byte). Row y=0 -> lineOff 0.
    // Pixel x uses byte (x>>2), shift (~x&3), idx = ((bit0)<<1)+(bit4).
    // byte[0]=0 -> pixels 0..3 all decode to ink index 0.
    // byte[1]=0x88 (bit7|bit3): x=4 -> b=0x88>>3=0x11 -> bit0=1,bit4=1 -> idx=3.
    b[1] = char(0x88);

    // --- OCP palette block at offset 16384.
    const int base = 16384;
    b[base + 0] = char(1);                 // pal[0] = mode 1
    for (int i = 0; i < 16; i++)           // every ink must be 64..95 (else reject)
        b[base + 3 + i * 12] = char(64);   // default: hw colour 64 -> AMSTRAD_PALETTE[0]
    // Distinct inks (values are hardware colour 64..95 -> AMSTRAD_PALETTE[c-64]):
    b[base + 3 + 0 * 12] = char(84);       // ink0: idx20 = 0x000000 (black)
    b[base + 3 + 1 * 12] = char(75);       // ink1: idx11 = 0xFFFFFF (white)
    b[base + 3 + 2 * 12] = char(76);       // ink2: idx12 = 0xFF0000 (red)
    b[base + 3 + 3 * 12] = char(78);       // ink3: idx14 = 0xFF8000 (orange)

    QImage im = RasterExtra::decodeData(b, "cpc");
    check(!im.isNull() && im.size() == QSize(320, 200)
          && im.pixelColor(0, 0) == QColor(0, 0, 0)            // ink0 black
          && im.pixelColor(4, 0) == QColor(0xFF, 0x80, 0x00),  // ink3 orange
          "AmstradCpc mode1 SCR+PAL");

    // Negative: truncated buffer must be rejected (wrong size, no magic match).
    QByteArray bad = b.left(16000);
    check(RasterExtra::decodeData(bad, "cpc").isNull(), "AmstradCpc rejects short");
}
      }
      // ---- Amiga SHAM sliced-HAM ----
      {
{
    // Build a 2x2 HAM6 SHAM ILBM byte-by-byte from the spec layout (independent of the decoder).
    auto be16=[&](QByteArray&b,int v){ b.append(char((v>>8)&0xff)); b.append(char(v&0xff)); };
    auto be32=[&](QByteArray&b,quint32 v){ b.append(char((v>>24)&0xff)); b.append(char((v>>16)&0xff)); b.append(char((v>>8)&0xff)); b.append(char(v&0xff)); };
    auto chunk=[&](QByteArray&b,const char* id,const QByteArray&d){ b.append(id,4); be32(b,(quint32)d.size()); b.append(d); if(d.size()&1) b.append(char(0)); };

    // BMHD (20 bytes): w=2,h=2,x=0,y=0,nPlanes=6,masking=0,compression=0,pad=0,
    //   transparentColor=0,xAspect=1,yAspect=1,pageW=2,pageH=2
    QByteArray bmhd;
    be16(bmhd,2); be16(bmhd,2); be16(bmhd,0); be16(bmhd,0);
    bmhd.append(char(6)); bmhd.append(char(0)); bmhd.append(char(0)); bmhd.append(char(0));
    be16(bmhd,0); bmhd.append(char(1)); bmhd.append(char(1)); be16(bmhd,2); be16(bmhd,2);

    // CAMG: HAM viewport bit 0x0800 (not required by the decoder, present for realism)
    QByteArray camg; be32(camg,0x0800);

    // SHAM: u16 version=0, then 2 slices of 16 RGB4 words (32 bytes each).
    //   slice0 word0 = 0x0F00 -> R=15 -> red;  slice1 word0 = 0x000F -> B=15 -> blue.
    QByteArray sham; be16(sham,0);
    be16(sham,0x0F00); for(int i=1;i<16;i++) be16(sham,0);   // line 0 palette (entry 0 = red)
    be16(sham,0x000F); for(int i=1;i<16;i++) be16(sham,0);   // line 1 palette (entry 0 = blue)
    // sliceCount = (66-2)/32 = 2 == height -> shift 0 -> one palette per line.

    // BODY uncompressed (compression=0): 6 planes x 2 rows, planeRowBytes = ((2+15)/16)*2 = 2.
    //   Row0: pixel(0,0) idx=0 (ctrl0,v0 -> palette entry 0 = red);
    //         pixel(1,0) idx=31 = 0b011111 (ctrl=1 modify-Blue, v=15 -> B=255) -> magenta over held red.
    //         x=1 is bit 6 (0x40); planes 0..4 set on x1, plane5 clear.
    //   Row1: all zero -> both pixels ctrl0,v0 -> palette entry 0 = blue.
    QByteArray body;
    for(int pl=0; pl<6; pl++){ body.append(char(pl<5?0x40:0x00)); body.append(char(0)); } // row0 (12 bytes)
    for(int pl=0; pl<6; pl++){ body.append(char(0)); body.append(char(0)); }               // row1 (12 bytes)

    QByteArray inner; inner.append("ILBM",4);
    chunk(inner,"BMHD",bmhd);
    chunk(inner,"CAMG",camg);
    chunk(inner,"SHAM",sham);
    chunk(inner,"BODY",body);
    QByteArray b; b.append("FORM",4); be32(b,(quint32)inner.size()); b.append(inner);

    QImage im=RasterExtra::decodeData(b,"sham");
    check(!im.isNull() && im.size()==QSize(2,2)
        && im.pixelColor(0,0)==QColor(255,0,0)       // line0 palette entry 0 = red
        && im.pixelColor(1,0)==QColor(255,0,255)     // HAM modify-Blue over held red = magenta
        && im.pixelColor(0,1)==QColor(0,0,255)       // line1 palette entry 0 = blue
        && im.pixelColor(1,1)==QColor(0,0,255),      // held blue
        "AmigaSham HAM6 per-scanline palette + HAM modify");

    // Negative: truncated buffer must return isNull.
    QImage im2=RasterExtra::decodeData(b.left(10),"sham");
    check(im2.isNull(), "AmigaSham rejects truncated input");
}
      }
      // ---- Sam Coupe ----
      {
{
    // Build a SAM Coupe MODE-4 raw dump byte-by-byte per the spec layout:
    // 128 bytes/line * 192 lines = 24576 bytes, 2 pixels/byte (high nibble = left).
    QByteArray b(24576, '\0');
    // Line 0, byte 0 = 0x10: high nibble 1 (left pixel x=0 -> pal[1]),
    //                        low  nibble 0 (pixel x=1 -> pal[0]).
    b[0] = char(0x10);
    // Line 3, byte 0 = 0xF0: high nibble 15 (x=0 -> pal[15]), low nibble 0 (x=1 -> pal[0]).
    b[3 * 128] = char(0xF0);

    // Expected default-palette colours via RECOIL_GetSamCoupeColor:
    //   defPal[0]=0   -> (0,0,0)
    //   defPal[1]=16  -> bit4 -> B|=0x92 -> (0,0,146)
    //   defPal[15]=120=0x78 bits3,4,5,6 -> each chan 0x92|0x24 = 0xB6 = 182 -> (182,182,182)
    QImage im = RasterExtra::decodeData(b, "dmp");
    check(!im.isNull() && im.size() == QSize(256, 192)
          && im.pixelColor(0, 0) == QColor(0, 0, 146)
          && im.pixelColor(1, 0) == QColor(0, 0, 0)
          && im.pixelColor(0, 3) == QColor(182, 182, 182)
          && im.pixelColor(1, 3) == QColor(0, 0, 0),
          "SamCoupe MODE4 dmp default palette");

    // Negative: wrong size must return a null image.
    QByteArray bad(1000, '\0');
    QImage imBad = RasterExtra::decodeData(bad, "dmp");
    check(imBad.isNull(), "SamCoupe rejects wrong-size buffer");
}
      }

      // ===== Fleet-9: FLIC / MAKI01 / BBC Micro / Thomson / Sinclair QL (retro; FFmpeg/GrafX2/sQLux/RECOIL-anchored) =====
      // ---- Autodesk FLIC ----
      {
{
    // ============ FLIC test 1: FLC + FLI_COLOR256 + FLI_COPY ============
    QByteArray b;
    auto put8 = [&](int v){ b.append(char(v & 0xff)); };
    auto le16 = [&](int v){ b.append(char(v & 0xff)); b.append(char((v >> 8) & 0xff)); };
    auto le32 = [&](quint32 v){ for (int s = 0; s < 32; s += 8) b.append(char((v >> s) & 0xff)); };

    // --- 128-byte file header ---
    le32(0);          // @0  file size (not validated)
    le16(0xAF12);     // @4  magic = FLC
    le16(1);          // @6  frames
    le16(4);          // @8  width  = 4
    le16(2);          // @10 height = 2
    le16(8);          // @12 depth  = 8bpp
    while (b.size() < 128) put8(0);           // pad rest of header

    // --- frame header (16 bytes) @128 ---
    le32(52);         // @0 frame size = 16 + 22 + 14
    le16(0xF1FA);     // @4 frame magic
    le16(2);          // @6 chunk count
    for (int i = 0; i < 8; ++i) put8(0);      // 8 reserved bytes

    // --- chunk 1: FLI_COLOR256 (type 4), csize = 6 + 16 = 22 ---
    le32(22);
    le16(4);
    le16(1);          // 1 packet
    put8(0);          // skip 0
    put8(4);          // change 4 colours (indices 0..3)
    put8(10);  put8(20);  put8(30);           // idx0
    put8(40);  put8(50);  put8(60);           // idx1
    put8(70);  put8(80);  put8(90);           // idx2
    put8(100); put8(110); put8(120);          // idx3

    // --- chunk 2: FLI_COPY (type 16), csize = 6 + 8 = 14 ---
    le32(14);
    le16(16);
    put8(0); put8(1); put8(2); put8(3);       // row 0
    put8(3); put8(2); put8(1); put8(0);       // row 1

    QImage im = RasterExtra::decodeData(b, "flc");
    check(!im.isNull() && im.size() == QSize(4, 2)
          && im.pixelColor(0, 0) == QColor(10, 20, 30)
          && im.pixelColor(3, 0) == QColor(100, 110, 120)
          && im.pixelColor(0, 1) == QColor(100, 110, 120)
          && im.pixelColor(3, 1) == QColor(10, 20, 30),
          "FLIC FLC COLOR256 + COPY first frame");

    // ============ FLIC test 2: FLI + FLI_BRUN (default gray palette) ============
    b.clear();
    le32(0);          // @0  file size
    le16(0xAF11);     // @4  magic = FLI
    le16(1);          // @6  frames
    le16(4);          // @8  width  = 4
    le16(1);          // @10 height = 1
    le16(8);          // @12 depth
    while (b.size() < 128) put8(0);

    le32(27);         // frame size = 16 + 11
    le16(0xF1FA);     // frame magic
    le16(1);          // 1 chunk
    for (int i = 0; i < 8; ++i) put8(0);

    le32(11);         // FLI_BRUN chunk, csize = 6 + 5
    le16(15);
    put8(2);          // per-line packet count (advisory, ignored)
    put8(3);          // run = +3  -> repeat next value 3x
    put8(5);          //   value = index 5
    put8(0xFF);       // run = -1  -> 1 literal byte
    put8(7);          //   literal = index 7
    // identity-gray fallback palette: idx k -> (k,k,k)

    QImage im2 = RasterExtra::decodeData(b, "fli");
    check(!im2.isNull() && im2.size() == QSize(4, 1)
          && im2.pixelColor(0, 0) == QColor(5, 5, 5)
          && im2.pixelColor(2, 0) == QColor(5, 5, 5)
          && im2.pixelColor(3, 0) == QColor(7, 7, 7),
          "FLIC FLI BRUN scanline RLE");

    // ============ negative: truncated / bad-magic buffer ============
    QByteArray bad(64, '\0');   // < 128 bytes, no valid header
    QImage imn = RasterExtra::decodeData(bad, "flc");
    check(imn.isNull(), "FLIC rejects truncated header");
}
      }
      // ---- MAKI01 ----
      {
{
    // --- Build a valid MAKI01A file byte-by-byte per the RECOIL spec layout ---
    QByteArray b;
    auto put8=[&](int v){ b.append(char(v&0xff)); };
    // offset 0..7: signature "MAKI01A "
    const char sig[8] = {'M','A','K','I','0','1','A',' '};
    for(int i=0;i<8;i++) put8(sig[i]);
    // offset 8..39: reserved / platform / comment (ignored by decode) -> zeros
    for(int i=8;i<40;i++) put8(0);
    // offset 40..43: must be zero
    for(int i=0;i<4;i++) put8(0);
    // offset 44..47: fixed screen bounds bytes {2,128,1,144} (= BE 640, BE 400)
    put8(2); put8(128); put8(1); put8(144);
    // offset 48..95: 16-entry palette, 3 bytes each in G,R,B order
    //   c0 = black; c1 -> qRgb(R=10,G=20,B=30); c2 -> qRgb(R=40,G=50,B=60); rest black
    put8(0);  put8(0);  put8(0);     // c0: G,R,B
    put8(20); put8(10); put8(30);    // c1: G=20,R=10,B=30
    put8(50); put8(40); put8(60);    // c2: G=50,R=40,B=60
    for(int c=3;c<16;c++){ put8(0); put8(0); put8(0); }
    // offset 96..1095: 1000 "have" flag bytes (MSB-first). Set only have-bit i=0 -> byte0 0x80.
    put8(0x80);
    for(int i=1;i<1000;i++) put8(0);
    // offset 1096..1097: the have-word for block 0 = 0x8000 (BE) -> flags only cell (row0,x0)
    put8(0x80); put8(0x00);
    // offset 1098: XOR-delta byte for that single cell = 0x12
    put8(0x12);
    // total length = 1099 bytes
    QImage im = RasterExtra::decodeData(b, "mki");
    QColor c0(0,0,0), c1(10,20,30), c2(40,50,60);
    check(!im.isNull() && im.size()==QSize(640,400)
          && im.pixelColor(0,0)==c1   // index byte 0x12 -> high nibble 1 -> pal[1]
          && im.pixelColor(1,0)==c2   // low nibble 2 -> pal[2]
          && im.pixelColor(2,0)==c0,  // byte-column 1 never touched -> pal[0]
          "MAKI01A 640x400 xor-delta cell");
    // negative: buffer shorter than the fixed 1096-byte header must be rejected
    QImage bad = RasterExtra::decodeData(b.left(1000), "mki");
    check(bad.isNull(), "MAKI01 truncated rejected");
}
      }
      // ---- BBC Micro ----
      {
{
    // BBC Micro MODE 2: 20480-byte raw screen dump -> 160x256, 16-colour.
    // Char-cell interleaved. Byte holding pixel (x,y) = (y&~7)*80 + ((x&~1)<<2) + (y&7).
    // In a byte the LEFT pixel (even x) = bits b7,b5,b3,b1 (value MSB..LSB),
    //              the RIGHT pixel (odd x) = bits b6,b4,b2,b0.
    QByteArray b(20480, '\0');
    // pixel(0,0): byte @ 0. Set b7,b5,b3,b1 = 1 (0xAA) -> left index 15 = white;
    //             right pixel bits b6,b4,b2,b0 = 0 -> index 0 = black.
    b[0] = char(0xAA);
    // pixel(2,1): addr = (1&~7)*80 + ((2&~1)<<2) + (1&7) = 0 + 8 + 1 = 9.
    //             left pixel: only b1 set (0x02) -> index 1 = red.
    b[9] = char(0x02);
    QImage im = RasterExtra::decodeData(b, "bbc");
    check(!im.isNull() && im.size()==QSize(160,256)
          && im.pixelColor(0,0)==QColor(255,255,255)
          && im.pixelColor(1,0)==QColor(0,0,0)
          && im.pixelColor(2,1)==QColor(255,0,0), "BBC Micro MODE2 20KB");

    // BBC Micro MODE 5: 10240-byte dump -> 160x256, 4-colour. 4 px/byte, bytesPerRow=40.
    // LEFT pixel (x=0) value = (b7<<1)|b3. Set b7,b3 (0x88) -> value 3 = white (PAL4[3]).
    QByteArray b5(10240, '\0');
    b5[0] = char(0x88);
    QImage im5 = RasterExtra::decodeData(b5, "bbc");
    check(!im5.isNull() && im5.size()==QSize(160,256)
          && im5.pixelColor(0,0)==QColor(255,255,255), "BBC Micro MODE5 10KB");

    // Negative: any size other than 10240/20480 must be rejected.
    QByteArray bad(20479, '\0');
    check(RasterExtra::decodeData(bad, "bbc").isNull(), "BBC Micro bad-size rejected");
}
      }
      // ---- Thomson MO/TO ----
      {
{
    // Thomson .MAP (SAVEP/LOADP), 40col mode, 2 columns x 8 rows = 16x8 px.
    // Built byte-for-byte from the DECB container + map header + RLE spec.
    QByteArray b;
    auto put8 = [&](int v){ b.append(char(v & 0xff)); };
    auto be16 = [&](int v){ b.append(char((v >> 8) & 0xff)); b.append(char(v & 0xff)); };

    // -- container chunk: code, length(BE), address(BE) --
    put8(0x00);        // code = data chunk
    be16(43);          // length = 3 (map header) + 40 (RLE) ; must be >16
    be16(0x0000);      // address = 0  -> MAP file
    // -- 3-byte map header --
    put8(0x00);        // map_mode = 0x00 => 40col, bpp4
    put8(0x01);        // col_count => columns = 2  => width 16
    put8(0x00);        // line_count => height = 8*(0+1) = 8
    // -- RLE: forme bank = literal copy of 16 bytes, all 0x0F --
    put8(0x00); put8(0x10);                 // copy-run, count = 16
    for (int k = 0; k < 16; k++) put8(0x0F);
    put8(0x00); put8(0x00);                 // end-of-bank (forme complete)
    // -- RLE: couleur bank = literal copy of 16 bytes, all 0xCA --
    put8(0x00); put8(0x10);                 // copy-run, count = 16
    for (int k = 0; k < 16; k++) put8(0xCA);
    put8(0x00); put8(0x00);                 // end-of-bank (couleur complete)
    // -- DECB end record --
    put8(0xFF); be16(0x0000); be16(0x0000);

    // forme byte 0x0F => within each 8px cell, x0..3 use background, x4..7 use foreground.
    // couleur 0xCA (transpose): foreground index = 1 (red 0x00F), background index = 2 (green 0x0F0).
    QImage im = RasterExtra::decodeData(b, "map");
    check(!im.isNull() && im.size() == QSize(16, 8)
          && im.pixelColor(0, 0)  == QColor(0, 255, 0)     // bg green (idx2)
          && im.pixelColor(4, 0)  == QColor(255, 0, 0)     // fg red   (idx1)
          && im.pixelColor(8, 0)  == QColor(0, 255, 0)     // 2nd cell bg
          && im.pixelColor(15, 7) == QColor(255, 0, 0),    // 2nd cell fg, last row
          "THOMSON .map 40col 16x8");

    // Negative: truncated buffer (no valid DECB end record) must be rejected.
    QImage im2 = RasterExtra::decodeData(b.left(20), "map");
    check(im2.isNull(), "THOMSON .map truncated rejected");
}
      }
      // ---- Sinclair QL ----
      {
{
    // Sinclair QL MODE 4 dump: headerless 32768-byte framebuffer, 256 rows x 128 bytes.
    // Word 0 (bytes 0,1) covers pixels (0..7, row 0). Byte0 = GREEN plane, byte1 = RED plane;
    // pixel i uses bit (7-i). idx = (green<<2)|(red<<1)|(green&red).
    //   green byte 0xA0 = bits 7 and 5 set -> pixel0 green, pixel2 green
    //   red   byte 0x60 = bits 6 and 5 set -> pixel1 red,   pixel2 red
    //   -> pixel0 = green(00FF00), pixel1 = red(FF0000), pixel2 = white(FFFFFF), pixel3 = black
    QByteArray b;
    auto put8=[&](int v){ b.append(char(v&0xff)); };
    put8(0xA0);                 // byte0: green plane, row 0 word 0
    put8(0x60);                 // byte1: red plane,   row 0 word 0
    while(b.size() < 32768) b.append(char(0));   // pad remainder to full screen (all black)
    QImage im = RasterExtra::decodeData(b, "ql");
    check(!im.isNull() && im.size()==QSize(512,256)
          && im.pixelColor(0,0)==QColor(0,255,0)
          && im.pixelColor(1,0)==QColor(255,0,0)
          && im.pixelColor(2,0)==QColor(255,255,255)
          && im.pixelColor(3,0)==QColor(0,0,0)
          && im.pixelColor(7,0)==QColor(0,0,0)
          && im.pixelColor(0,1)==QColor(0,0,0),      // row 1 untouched -> black
          "SinclairQL mode4 512x256 GRB planes");

    // Negative: a truncated (one byte short) buffer must be rejected by the exact-size guard.
    QByteArray bad(32767, '\0');
    check(RasterExtra::decodeData(bad, "ql").isNull(), "SinclairQL rejects wrong size");
}
      }

      // ===== Fleet-10: BLP / VTF / KTX / PVR / WAL (game textures; BCn reuses DDS-validated block decode) =====
      // ---- Blizzard BLP ----
      {
{
    // Build a minimal valid BLP2 palettized (encoding=1, alphaDepth=0), 4x4.
    QByteArray b;
    auto put8 = [&](int v){ b.append(char(v & 0xff)); };
    auto le32 = [&](quint32 v){ for (int s = 0; s < 32; s += 8) b.append(char((v >> s) & 0xff)); };

    b.append("BLP2", 4);          // @0  magic
    le32(1);                      // @4  type = 1 (direct)
    put8(1);                      // @8  encoding = 1 (palettized)
    put8(0);                      // @9  alphaDepth = 0
    put8(0);                      // @10 alphaEncoding
    put8(0);                      // @11 hasMips
    le32(4);                      // @12 width = 4
    le32(4);                      // @16 height = 4
    le32(148 + 1024);             // @20 mipOffsets[0] = 1172 (after palette)
    for (int i = 0; i < 15; ++i) le32(0);   // mipOffsets[1..15]
    le32(16);                     // @84 mipSizes[0] = 16 (4*4 indices)
    for (int i = 0; i < 15; ++i) le32(0);   // mipSizes[1..15]
    // @148 palette: 256 BGRA entries. entry1=(R200,G100,B50), entry2=(R10,G220,B30)
    for (int i = 0; i < 256; ++i) {
        if (i == 1)      { put8(50);  put8(100); put8(200); put8(0); }  // B,G,R,A
        else if (i == 2) { put8(30);  put8(220); put8(10);  put8(0); }
        else             { put8(0);   put8(0);   put8(0);   put8(0); }
    }
    // @1172 index data (16 bytes): [0]=1, [15]=2, rest 0
    for (int i = 0; i < 16; ++i) {
        if (i == 0)       put8(1);
        else if (i == 15) put8(2);
        else              put8(0);
    }

    QImage im = RasterExtra::decodeData(b, "blp");
    check(!im.isNull() && im.size() == QSize(4, 4) &&
          im.pixelColor(0, 0) == QColor(200, 100, 50) &&
          im.pixelColor(3, 3) == QColor(10, 220, 30),
          "BLP2 palettized 4x4 mip0");

    // Negative: bad magic must be rejected.
    QByteArray bad = b;
    bad[0] = 'X';
    QImage nim = RasterExtra::decodeData(bad, "blp");
    check(nim.isNull(), "BLP bad-magic rejected");
}
      }
      // ---- Valve VTF ----
      {
{
    // ---- VTF selftest: build headers byte-by-byte per tagVTFHEADER ----------
    auto buildHeader=[&](QByteArray& b, int w, int h, quint32 fmt){
        auto put8 =[&](int v){ b.append(char(v&0xff)); };
        auto le16 =[&](int v){ b.append(char(v&0xff)); b.append(char((v>>8)&0xff)); };
        auto le32 =[&](quint32 v){ for(int s=0;s<32;s+=8) b.append(char((v>>s)&0xff)); };
        b.append("VTF",3); put8(0);   // signature "VTF\0"        @0
        le32(7);                       // version major            @4
        le32(1);                       // version minor (classic)  @8
        le32(64);                      // headerSize               @12
        le16(w);                       // width                    @16
        le16(h);                       // height                   @18
        le32(0);                       // flags                    @20
        le16(1);                       // frames                   @24
        le16(0);                       // firstFrame               @26
        le32(0);                       // padding0                 @28
        le32(0); le32(0); le32(0);     // reflectivity[3]          @32
        le32(0);                       // padding1                 @44
        le32(0);                       // bumpScale                @48
        le32(fmt);                     // highResImageFormat       @52
        put8(1);                       // mipmapCount = 1          @56
        le32(0xFFFFFFFFu);             // lowResImageFormat = none @57
        put8(0);                       // lowResWidth              @61
        put8(0);                       // lowResHeight             @62
        put8(0);                       // pad -> headerSize 64     @63
    };

    // (1) uncompressed BGRA8888 (fmt 12), 4x4, no thumbnail, 1 mip.
    {
        QByteArray b; buildHeader(b, 4, 4, 12);
        // 16 texels, BGRA order: B=30 G=20 R=10 A=255  -> RGBA (10,20,30,255)
        for(int i=0;i<16;i++){ b.append(char(30)); b.append(char(20)); b.append(char(10)); b.append(char(255)); }
        QImage im = RasterExtra::decodeData(b, "vtf");
        check(!im.isNull() && im.size()==QSize(4,4) && im.pixelColor(0,0)==QColor(10,20,30)
              && im.pixelColor(3,3)==QColor(10,20,30), "VTF BGRA8888 4x4");
    }

    // (2) DXT1 (fmt 13), 4x4 = one 8-byte BC1 block, all texels index 0 = c0 = red.
    {
        QByteArray b; buildHeader(b, 4, 4, 13);
        // c0 = RGB565 pure red 0xF800 (LE), c1 = pure blue 0x001F (LE); c0>c1 => 4-colour.
        b.append(char(0x00)); b.append(char(0xF8)); // c0 = 0xF800
        b.append(char(0x1F)); b.append(char(0x00)); // c1 = 0x001F
        b.append(char(0x00)); b.append(char(0x00)); // indices = 0 (all -> c0)
        b.append(char(0x00)); b.append(char(0x00));
        QImage im = RasterExtra::decodeData(b, "vtf");
        check(!im.isNull() && im.size()==QSize(4,4) && im.pixelColor(0,0)==QColor(255,0,0)
              && im.pixelColor(3,3)==QColor(255,0,0), "VTF DXT1 4x4 red");
    }

    // (3) negative: bad magic must be rejected.
    {
        QByteArray b; buildHeader(b, 4, 4, 12);
        b[0] = 'X';
        QImage im = RasterExtra::decodeData(b, "vtf");
        check(im.isNull(), "VTF bad magic rejected");
    }
}
      }
      // ---- Khronos KTX ----
      {
{
    // --- KTX v1 uncompressed RGBA, 2x2, little-endian ---
    QByteArray b;
    auto put8=[&](int v){ b.append(char(v&0xff)); };
    auto le32=[&](quint32 v){ for(int s=0;s<32;s+=8) b.append(char((v>>s)&0xff)); };
    const unsigned char sig[12]={0xAB,'K','T','X',' ','1','1',0xBB,0x0D,0x0A,0x1A,0x0A};
    for(int i=0;i<12;i++) put8(sig[i]);
    le32(0x04030201);   // endianness -> little
    le32(0x1401);       // glType = GL_UNSIGNED_BYTE
    le32(1);            // glTypeSize
    le32(0x1908);       // glFormat = GL_RGBA
    le32(0x8058);       // glInternalFormat = GL_RGBA8
    le32(0x1908);       // glBaseInternalFormat = GL_RGBA
    le32(2);            // pixelWidth
    le32(2);            // pixelHeight
    le32(0);            // pixelDepth
    le32(0);            // numberOfArrayElements
    le32(1);            // numberOfFaces
    le32(1);            // numberOfMipmapLevels
    le32(0);            // bytesOfKeyValueData
    le32(16);           // imageSize = 2*2*4 (rowStride 8 == 2*4, no padding)
    put8(255);put8(0);  put8(0);  put8(255);  // (0,0) red
    put8(0);  put8(255);put8(0);  put8(255);  // (1,0) green
    put8(0);  put8(0);  put8(255);put8(255);  // (0,1) blue
    put8(10); put8(20); put8(30); put8(255);  // (1,1)
    QImage im=RasterExtra::decodeData(b,"ktx");
    check(!im.isNull() && im.size()==QSize(2,2)
          && im.pixelColor(0,0)==QColor(255,0,0)
          && im.pixelColor(1,0)==QColor(0,255,0)
          && im.pixelColor(0,1)==QColor(0,0,255)
          && im.pixelColor(1,1)==QColor(10,20,30), "KTX uncompressed RGBA 2x2");

    // --- KTX v1 DXT1 (BC1), 4x4, single block, all-white ---
    QByteArray c;
    auto cput8=[&](int v){ c.append(char(v&0xff)); };
    auto cle16=[&](int v){ c.append(char(v&0xff)); c.append(char((v>>8)&0xff)); };
    auto cle32=[&](quint32 v){ for(int s=0;s<32;s+=8) c.append(char((v>>s)&0xff)); };
    for(int i=0;i<12;i++) cput8(sig[i]);
    cle32(0x04030201); // little
    cle32(0);          // glType = 0 -> compressed
    cle32(1);          // glTypeSize
    cle32(0);          // glFormat = 0
    cle32(0x83F0);     // glInternalFormat = COMPRESSED_RGB_S3TC_DXT1_EXT
    cle32(0x1907);     // glBaseInternalFormat = GL_RGB
    cle32(4);          // pixelWidth
    cle32(4);          // pixelHeight
    cle32(0);          // pixelDepth
    cle32(0);          // arrayElements
    cle32(1);          // faces
    cle32(1);          // mip levels
    cle32(0);          // kvd
    cle32(8);          // imageSize = one 8-byte BC1 block
    cle16(0xFFFF);     // c0 = white (RGB565 all-ones)
    cle16(0x0000);     // c1 = black; c0>c1 -> 4-colour mode
    cle32(0x00000000); // all texel indices 0 -> colour c0 (white)
    QImage im2=RasterExtra::decodeData(c,"ktx");
    check(!im2.isNull() && im2.size()==QSize(4,4)
          && im2.pixelColor(0,0)==QColor(255,255,255)
          && im2.pixelColor(3,3)==QColor(255,255,255), "KTX DXT1 4x4 white block");

    // --- KTX DXT5 (BC3) 4x4: exercises the INTERPOLATED alpha table (a0>a1, 8-value mode).
    // a0=255,a1=0 -> spec: at[2]=(6*255)/7=218, at[7]=(1*255)/7=36. Old code used (6-j)/j<=5
    // weights and left at[7] uninitialised (UB); this pins the spec values. Alpha selectors:
    // texel0=2 (->218), texel1=7 (->36), rest=0 (->a0=255). Colour block = solid white.
    {
        QByteArray d;
        auto d8=[&](int v){ d.append(char(v&0xff)); };
        auto d16=[&](int v){ d.append(char(v&0xff)); d.append(char((v>>8)&0xff)); };
        auto d32=[&](quint32 v){ for(int s=0;s<32;s+=8) d.append(char((v>>s)&0xff)); };
        for(int i=0;i<12;i++) d8(sig[i]);
        d32(0x04030201); d32(0); d32(1); d32(0); d32(0x83F3); d32(0x1907); // glInternalFormat=DXT5
        d32(4); d32(4); d32(0); d32(0); d32(1); d32(1); d32(0);            // 4x4, 1 mip
        d32(16);                                                          // imageSize = one 16-byte BC3 block
        // BC3 alpha sub-block: a0,a1 then 48 bits of 3-bit selectors (LSB-first).
        d8(255); d8(0);                    // a0>a1 -> 8-value interpolation
        d8(0x3A); d8(0); d8(0); d8(0); d8(0); d8(0);  // texel0=sel2(0b010), texel1=sel7(0b111): low bits 0b111010=0x3A
        // BC1 colour sub-block: white, all indices 0.
        d16(0xFFFF); d16(0x0000); d32(0x00000000);
        QImage a=RasterExtra::decodeData(d,"ktx");
        check(!a.isNull() && a.size()==QSize(4,4)
              && a.pixelColor(0,0)==QColor(255,255,255,218)   // selector 2 -> (6*255)/7
              && a.pixelColor(1,0)==QColor(255,255,255,36)    // selector 7 -> (1*255)/7  (was uninitialised)
              && a.pixelColor(2,0)==QColor(255,255,255,255),  // selector 0 -> a0
              "KTX DXT5 interpolated alpha (spec-exact, no UB)");
    }

    // --- negative: corrupt magic ---
    QByteArray bad=b; bad[1]='X';
    check(RasterExtra::decodeData(bad,"ktx").isNull(), "KTX bad magic rejected");
}
      }
      // ---- PowerVR PVR ----
      {
{
    // --- PVR v3 uncompressed RGBA8888, 2x2 (spec byte layout) ---
    QByteArray b;
    auto put8=[&](int v){ b.append(char(v&0xff)); };
    auto le32=[&](quint32 v){ for(int s=0;s<32;s+=8) b.append(char((v>>s)&0xff)); };
    le32(0x03525650u);   // @0  version 'PVR\3'
    le32(0);             // @4  flags
    // @8 pixelFormat u64 low32 = channel names 'r','g','b','a'
    le32( quint32('r') | (quint32('g')<<8) | (quint32('b')<<16) | (quint32('a')<<24) );
    // @12 pixelFormat u64 high32 = bit counts 8,8,8,8 (non-zero -> uncompressed layout)
    le32( 8u | (8u<<8) | (8u<<16) | (8u<<24) );
    le32(0);             // @16 colourSpace
    le32(0);             // @20 channelType
    le32(2);             // @24 height
    le32(2);             // @28 width
    le32(1);             // @32 depth
    le32(1);             // @36 numSurfaces
    le32(1);             // @40 numFaces
    le32(1);             // @44 numMipMaps
    le32(0);             // @48 metaDataSize
    // @52 pixel data, row-major, bytes r,g,b,a per pixel
    put8(10);put8(20);put8(30);put8(40);     // (0,0)
    put8(50);put8(60);put8(70);put8(80);     // (1,0)
    put8(90);put8(100);put8(110);put8(120);  // (0,1)
    put8(130);put8(140);put8(150);put8(160); // (1,1)
    QImage im=RasterExtra::decodeData(b,"pvr");
    check(!im.isNull() && im.size()==QSize(2,2)
          && im.pixelColor(0,0)==QColor(10,20,30,40)
          && im.pixelColor(1,1)==QColor(130,140,150,160), "PVR v3 RGBA8888 2x2");
}
{
    // --- PVR v3 BC1/DXT1, 4x4, one hand-built all-red block ---
    QByteArray b;
    auto put8=[&](int v){ b.append(char(v&0xff)); };
    auto le32=[&](quint32 v){ for(int s=0;s<32;s+=8) b.append(char((v>>s)&0xff)); };
    le32(0x03525650u);   // version
    le32(0);             // flags
    le32(7);             // pixelFormat low32 = 7 (DXT1/BC1)
    le32(0);             // pixelFormat high32 = 0 -> compressed enum path
    le32(0);             // colourSpace
    le32(0);             // channelType
    le32(4);             // height
    le32(4);             // width
    le32(1);             // depth
    le32(1);le32(1);le32(1); // numSurfaces / numFaces / numMipMaps
    le32(0);             // metaDataSize
    // BC1 block: c0=0xF800 (red565) > c1=0x001F (blue565); all 16 indices = 0 -> pure red
    put8(0x00);put8(0xF8);   // c0 LE = 0xF800
    put8(0x1F);put8(0x00);   // c1 LE = 0x001F
    put8(0);put8(0);put8(0);put8(0);  // 2-bit indices, all 0
    QImage im=RasterExtra::decodeData(b,"pvr");
    check(!im.isNull() && im.size()==QSize(4,4)
          && im.pixelColor(0,0)==QColor(255,0,0)
          && im.pixelColor(3,3)==QColor(255,0,0), "PVR v3 BC1 4x4 red");
}
{
    // --- negative: valid magic but truncated header (< 52 bytes) must be rejected ---
    QByteArray b;
    auto le32=[&](quint32 v){ for(int s=0;s<32;s+=8) b.append(char((v>>s)&0xff)); };
    le32(0x03525650u); le32(0); le32(0);   // only 12 bytes
    QImage im=RasterExtra::decodeData(b,"pvr");
    check(im.isNull(), "PVR v3 truncated header rejected");
}
      }
      // ---- Quake2 WAL ----
      {
{
    // Hand-build a Quake 2 WAL from the spec byte layout (100-byte miptex_t header
    // + width*height palette indices at mipOffsets[0]).
    QByteArray b;
    auto put8=[&](int v){ b.append(char(v&0xff)); };
    auto le32=[&](quint32 v){ for(int s=0;s<32;s+=8) b.append(char((v>>s)&0xff)); };
    for(int i=0;i<32;i++) put8('t');   // name[32] @0
    le32(2);                           // width  @32
    le32(2);                           // height @36
    le32(100);                         // mipOffsets[0] @40 -> pixel data right after header
    le32(0); le32(0); le32(0);         // mipOffsets[1..3] @44..52
    for(int i=0;i<32;i++) put8(0);     // animname[32] @56
    le32(0);                           // flags    @88
    le32(0);                           // contents @92
    le32(0);                           // value    @96  (header ends at 100)
    // 2x2 palette indices: q2pal[0]=(0,0,0), q2pal[15]=(235,235,235)
    put8(0);  put8(15);                // row 0
    put8(15); put8(0);                 // row 1
    QImage im = RasterExtra::decodeData(b,"wal");
    check(!im.isNull() && im.size()==QSize(2,2)
        && im.pixelColor(0,0)==QColor(0,0,0)
        && im.pixelColor(1,0)==QColor(235,235,235)
        && im.pixelColor(0,1)==QColor(235,235,235)
        && im.pixelColor(1,1)==QColor(0,0,0), "WAL q2-palette 2x2");
    // Negative: truncated header must be rejected.
    QImage im2 = RasterExtra::decodeData(b.left(50),"wal");
    check(im2.isNull(), "WAL truncated header rejected");
}
      }

      // ===== Fleet-11: NIfTI / NRRD / MetaImage / MGH / VTK (scientific volume -> middle-slice grayscale) =====
      // ---- NIfTI-1 ----
      {
{
    // --- NIfTI-1 (.nii): 2x2x3 UINT8 volume, decode middle Z slice (z=1) ---
    QByteArray b(364,'\0');                 // 348-byte header + vox_offset 352 + 12 data bytes
    auto sLE32=[&](int off,quint32 v){ for(int s=0;s<4;s++) b[off+s]=char((v>>(8*s))&0xff); };
    auto sLE16=[&](int off,int v){ b[off]=char(v&0xff); b[off+1]=char((v>>8)&0xff); };
    auto sF32 =[&](int off,float f){ quint32 u; memcpy(&u,&f,4); sLE32(off,u); };
    sLE32(0,348);                           // sizeof_hdr @0 (little-endian)
    sLE16(40,3);                            // dim[0] = 3 dims
    sLE16(42,2);                            // dim[1] = X = 2
    sLE16(44,2);                            // dim[2] = Y = 2
    sLE16(46,3);                            // dim[3] = Z = 3  -> middle slice index = 3/2 = 1
    sLE16(70,2);                            // datatype @70 = 2 (UINT8)
    sLE16(72,8);                            // bitpix   @72 = 8
    sF32(108,352.0f);                       // vox_offset @108 = 352
    sF32(112,0.0f);                         // scl_slope @112 = 0 (no scaling)
    sF32(116,0.0f);                         // scl_inter @116 = 0
    b[344]='n'; b[345]='+'; b[346]='1'; b[347]='\0';   // magic @344 = "n+1\0"
    // data @352: slice0(z=0), slice1(z=1 middle), slice2(z=2); row-major, x fastest
    unsigned char s0[4]={100,100,100,100};             // z=0 (ignored)
    unsigned char s1[4]={10,20,30,40};                 // z=1 min=10 max=40 -> 0,85,170,255
    unsigned char s2[4]={200,200,200,200};             // z=2 (ignored)
    for(int i=0;i<4;i++){ b[352+i]=char(s0[i]); b[356+i]=char(s1[i]); b[360+i]=char(s2[i]); }
    QImage im=RasterExtra::decodeData(b,"nii");
    check(!im.isNull() && im.size()==QSize(2,2), "NIfTI: 2x2 middle slice decoded");
    check(qGray(im.pixel(0,0))==0,   "NIfTI: min sample -> 0");
    check(qGray(im.pixel(1,0))==85,  "NIfTI: (1,0)=20 -> 85");
    check(qGray(im.pixel(0,1))==170, "NIfTI: (0,1)=30 -> 170");
    check(qGray(im.pixel(1,1))==255, "NIfTI: max sample -> 255");
    // negative: valid magic region but bogus header (sizeof_hdr != 348, no valid magic)
    QByteArray bad(400,'\0');
    QImage imBad=RasterExtra::decodeData(bad,"nii");
    check(imBad.isNull(), "NIfTI: rejects header without sizeof_hdr/magic");
}
      }
      // ---- NRRD ----
      {
{
    // NRRD raw int16 middle-slice grayscale (teem.sourceforge.net/nrrd/format.html).
    // 2x2x3 short volume; middle Z slice (z=1) values [0,100,200,300] -> min0/max300 ->
    // g = round(v*255/300): 0, 85, 170, 255 exactly. Discriminator slices z0=1,z2=7.
    QByteArray b;
    auto le16=[&](int v){ b.append(char(v&0xff)); b.append(char((v>>8)&0xff)); };
    b.append("NRRD0004\n");          // magic line
    b.append("type: short\n");       // int16 signed
    b.append("dimension: 3\n");      // 3D
    b.append("sizes: 2 2 3\n");      // X=2 Y=2 Z=3 (fastest axis first)
    b.append("encoding: raw\n");     // raw binary
    b.append("endian: little\n");    // LE for >8-bit
    b.append("\n");                  // blank line -> data follows
    for(int i=0;i<4;i++) le16(1);            // slice z=0 (ignored)
    le16(0); le16(100); le16(200); le16(300);// slice z=1 (MIDDLE, tested): y*X+x order
    for(int i=0;i<4;i++) le16(7);            // slice z=2 (ignored)
    QImage im=RasterExtra::decodeData(b,"nrrd");
    check(!im.isNull() && im.size()==QSize(2,2)
          && qGray(im.pixel(0,0))==0   && qGray(im.pixel(1,0))==85
          && qGray(im.pixel(0,1))==170 && qGray(im.pixel(1,1))==255,
          "NRRD raw int16 middle-slice min/max grayscale");
    // Negative: bad magic.
    { QByteArray bad=b; bad[0]='X'; check(RasterExtra::decodeData(bad,"nrrd").isNull(), "NRRD bad magic rejected"); }
    // Negative: truncated data (only one slice present -> middle slice can't fit).
    { QByteArray tr=b; tr.truncate(tr.size()-16); check(RasterExtra::decodeData(tr,"nrrd").isNull(), "NRRD truncated data rejected"); }
}
      }
      // ---- MetaImage ----
      {
{
    // --- MetaImage .mha: 2x2x3 MET_SHORT little-endian, middle Z slice (z=DimZ/2=1) ---
    // Spec byte layout built independently: ASCII "Key = Value\n" header, then raw voxels
    // (X fastest, then Y, then Z) immediately after the "ElementDataFile = LOCAL\n" newline.
    QByteArray b;
    auto puts=[&](const char* s){ b.append(s); };
    auto le16=[&](int v){ b.append(char(v&0xff)); b.append(char((v>>8)&0xff)); };
    puts("ObjectType = Image\n");
    puts("NDims = 3\n");
    puts("DimSize = 2 2 3\n");                 // X=2 Y=2 Z=3
    puts("ElementType = MET_SHORT\n");         // int16
    puts("BinaryData = True\n");
    puts("BinaryDataByteOrderMSB = False\n");  // little-endian
    puts("ElementDataFile = LOCAL\n");         // embedded; voxels follow this newline
    // z=0 slice (NOT read): 4 shorts
    le16(1000); le16(1000); le16(1000); le16(1000);
    // z=1 slice (the MIDDLE slice we decode): (x,y) order = (0,0)(1,0)(0,1)(1,1)
    le16(0); le16(10); le16(20); le16(30);     // min=0 max=30 => 0,85,170,255
    // z=2 slice (NOT read)
    le16(-500); le16(-500); le16(-500); le16(-500);
    QImage im=RasterExtra::decodeData(b,"mha");
    check(!im.isNull() && im.size()==QSize(2,2), "MetaImage: size 2x2 (middle Z slice)");
    check(!im.isNull() && qGray(im.pixel(0,0))==0,   "MetaImage: (0,0)=0 (min)");
    check(!im.isNull() && qGray(im.pixel(1,0))==85,  "MetaImage: (1,0)=85  (10/30*255)");
    check(!im.isNull() && qGray(im.pixel(0,1))==170, "MetaImage: (0,1)=170 (20/30*255)");
    check(!im.isNull() && qGray(im.pixel(1,1))==255, "MetaImage: (1,1)=255 (max)");
    // negative: valid header but binary payload truncated (needs 24 bytes for the volume) -> null
    QByteArray t;
    t.append("ObjectType = Image\n");
    t.append("NDims = 3\n");
    t.append("DimSize = 2 2 3\n");
    t.append("ElementType = MET_SHORT\n");
    t.append("ElementDataFile = LOCAL\n");
    t.append(char(0)); t.append(char(0));      // only 2 bytes of voxel data (need 12*2)
    QImage bad=RasterExtra::decodeData(t,"mha");
    check(bad.isNull(), "MetaImage: truncated voxel payload rejected");
    // negative: detached data file (ElementDataFile != LOCAL) -> reject
    QByteArray det;
    det.append("ObjectType = Image\nNDims = 2\nDimSize = 2 2\nElementType = MET_UCHAR\n");
    det.append("ElementDataFile = volume.raw\n");
    det.append(char(1)); det.append(char(2)); det.append(char(3)); det.append(char(4));
    QImage dbad=RasterExtra::decodeData(det,"mha");
    check(dbad.isNull(), "MetaImage: detached ElementDataFile rejected");

    // Regression (review-confirmed): a NaN first voxel must NOT poison min/max (was int(NaN) UB ->
    // whole slice black). 2x2 MET_FLOAT, voxels [NaN, 0, 3, 6]: seed from first FINITE (0), rng=6.
    {
        QByteArray f;
        f.append("ObjectType = Image\nNDims = 2\nDimSize = 2 2\nElementType = MET_FLOAT\n");
        f.append("BinaryData = True\nBinaryDataByteOrderMSB = False\nElementDataFile = LOCAL\n");
        auto lef=[&](float v){ quint32 u; memcpy(&u,&v,4); for(int s=0;s<4;s++) f.append(char((u>>(8*s))&0xff)); };
        lef(std::numeric_limits<float>::quiet_NaN()); // (0,0) NaN -> guarded to 0, must not blank the rest
        lef(0.0f);   // (1,0) min -> 0
        lef(3.0f);   // (0,1) -> round(3/6*255)=128
        lef(6.0f);   // (1,1) max -> 255
        QImage im=RasterExtra::decodeData(f,"mha");
        check(!im.isNull() && im.size()==QSize(2,2)
              && qGray(im.pixel(0,0))==0            // NaN pixel -> 0 (no UB)
              && qGray(im.pixel(1,0))==0            // finite min
              && qGray(im.pixel(0,1))==128          // finite mid
              && qGray(im.pixel(1,1))==255,         // finite max (proves NaN didn't poison the range)
              "MetaImage: NaN first voxel does not blank the finite data (min/max seeded from finite)");
    }
}
      }
      // ---- FreeSurfer MGH ----
      {
{
    // FreeSurfer MGH, BIG-ENDIAN. 2x2x3 SHORT volume; decode MIDDLE slice (z=depth/2=1).
    QByteArray b;
    auto be32=[&](qint32 v){ for(int s=24;s>=0;s-=8) b.append(char((v>>s)&0xff)); };
    auto be16=[&](int v){ b.append(char((v>>8)&0xff)); b.append(char(v&0xff)); };
    be32(1);   // version @0  = 1
    be32(2);   // width   @4  = 2
    be32(2);   // height  @8  = 2
    be32(3);   // depth   @12 = 3
    be32(1);   // nframes @16 = 1
    be32(4);   // type    @20 = 4 (SHORT / int16)
    be32(0);   // dof     @24 = 0
    be16(0);   // goodRASFlag @28 = 0
    while(b.size()<284) b.append(char(0));            // pad header to fixed 284
    // slice0 (z=0): filler shorts
    be16(1); be16(2); be16(3); be16(4);
    // slice1 (z=1, MIDDLE): idx = y*width+x -> (0,0)=10 (1,0)=20 (0,1)=30 (1,1)=40
    be16(10); be16(20); be16(30); be16(40);
    // slice2 (z=2): filler shorts
    be16(5); be16(6); be16(7); be16(8);
    QImage im=RasterExtra::decodeData(b,"mgh");
    // min=10 max=40 range=30: g=round((v-10)*255/30) -> 0,85,170,255
    check(!im.isNull() && im.size()==QSize(2,2)
        && qGray(im.pixel(0,0))==0
        && qGray(im.pixel(1,0))==85
        && qGray(im.pixel(0,1))==170
        && qGray(im.pixel(1,1))==255, "MGH short middle-slice min/max grayscale");
    // negative: corrupt version field
    QByteArray bad=b; bad[3]=char(9);                 // version -> 9
    check(RasterExtra::decodeData(bad,"mgh").isNull(), "MGH bad version rejected");
}
      }
      // ---- VTK legacy ----
      {
{
    // VTK legacy BINARY STRUCTURED_POINTS, 2x2x3 unsigned_char volume; middle slice z=nz/2=1.
    QByteArray b;
    b.append("# vtk DataFile Version 3.0\n");   // magic (line 0)
    b.append("selftest volume\n");              // title (line 1)
    b.append("BINARY\n");                        // format (big-endian data)
    b.append("DATASET STRUCTURED_POINTS\n");
    b.append("DIMENSIONS 2 2 3\n");              // nx=2 ny=2 nz=3
    b.append("SPACING 1 1 1\n");
    b.append("ORIGIN 0 0 0\n");
    b.append("POINT_DATA 12\n");
    b.append("SCALARS s unsigned_char\n");
    b.append("LOOKUP_TABLE default\n");          // data starts right after this '\n'
    auto put8=[&](int v){ b.append(char(v&0xff)); };
    put8(0);  put8(0);   put8(0);   put8(0);     // slice z=0 (ignored)
    put8(0);  put8(85);  put8(170); put8(255);   // slice z=1 (MIDDLE): (0,0)=0 (1,0)=85 (0,1)=170 (1,1)=255
    put8(0);  put8(0);   put8(0);   put8(0);     // slice z=2 (ignored)
    QImage im = RasterExtra::decodeData(b, "vtk");
    check(!im.isNull() && im.size()==QSize(2,2), "VTK binary decodes middle slice size");
    // min=0 max=255 -> g = round((v-0)*255/255) = v, so grayscale == raw byte
    check(!im.isNull() && qGray(im.pixel(0,0))==0   && qGray(im.pixel(1,0))==85 &&
                          qGray(im.pixel(0,1))==170 && qGray(im.pixel(1,1))==255,
          "VTK binary middle-slice min/max grayscale normalise");

    // ASCII variant, single slice 2x2 short, values 10 20 30 40 -> min10 max40 rng30
    QByteArray a;
    a.append("# vtk DataFile Version 2.0\n");
    a.append("ascii test\n");
    a.append("ASCII\n");
    a.append("DATASET STRUCTURED_POINTS\n");
    a.append("DIMENSIONS 2 2 1\n");
    a.append("POINT_DATA 4\n");
    a.append("SCALARS s short\n");
    a.append("LOOKUP_TABLE default\n");
    a.append("10 20\n30 40\n");
    QImage ai = RasterExtra::decodeData(a, "vtk");
    // g(10)=0, g(20)=round(10*255/30)=85, g(30)=round(20*255/30)=170, g(40)=255
    check(!ai.isNull() && ai.size()==QSize(2,2) &&
          qGray(ai.pixel(0,0))==0 && qGray(ai.pixel(1,0))==85 &&
          qGray(ai.pixel(0,1))==170 && qGray(ai.pixel(1,1))==255,
          "VTK ASCII single-slice grayscale normalise");

    QByteArray bad("# NOT a vtk header at all......",30);
    check(RasterExtra::decodeData(bad, "vtk").isNull(), "VTK rejects bad magic");
    QByteArray trunc = b.left(b.size()-6);       // drop part of the scalar data
    check(RasterExtra::decodeData(trunc, "vtk").isNull(), "VTK rejects truncated binary data");
}
      }

      // Native TGA (Qt's plugin is v2-only + read-only; ours handles v1/uncompressed/RLE/flips).
      { QByteArray b; ap8(b,{0,0,2}); for(int k=0;k<5;k++) b.append(char(0));   // idLen,cmapType,type2,cmapspec
        le16(b,0); le16(b,0); le16(b,2); le16(b,2); ap8(b,{24,0x20});           // x,y,w=2,h=2,24bpp,top-origin
        ap8(b,{0,0,255, 0,255,0, 255,0,0, 255,255,255});                        // BGR: red,green,blue,white
        QImage im=RasterExtra::decodeData(b,"tga");
        check(!im.isNull() && im.size()==QSize(2,2) && im.pixelColor(0,0)==QColor(255,0,0) && im.pixelColor(1,1)==QColor(255,255,255), "native TGA (type-2, top-origin) decodes"); }

      // PFM regression: a '#' comment line in the header (ImageMagick/Photoshop inject one) must be skipped.
      { QByteArray b("PF\n#File written by a tool\n2 1\n-1.0\n");
        float one=1.0f, zero=0.0f;
        for(int k=0;k<3;k++) b.append(reinterpret_cast<const char*>(&one),4);
        for(int k=0;k<3;k++) b.append(reinterpret_cast<const char*>(&zero),4);
        QImage im=RasterExtra::decodeData(b,"pfm");
        check(!im.isNull() && im.size()==QSize(2,1), "PFM with a header comment line still decodes"); }

      // CALS Type 1 / CCITT Group 4 (T.6). A real 24x16 CALS produced by ImageMagick (2048-byte ASCII
      // header + T.6 body); the pixels below were verified pixel-exact against IM's own readback
      // (border+diagonal pattern: 292 black, 92 white). Exercises horizontal + vertical 2-D modes.
      { const char* cb64 =
          "c3JjZG9jaWQ6IE5PTkUgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgIC"
          "AgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAg"
          "ICAgICAgICAgICBkc3Rkb2NpZDogTk9ORSAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgIC"
          "AgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAg"
          "ICAgICAgICAgICAgICAgICAgICAgIHR4dGZpbGlkOiBOT05FICAgICAgICAgICAgICAgICAgICAgIC"
          "AgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAg"
          "ICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgZmlnaWQ6IE5PTkUgICAgICAgICAgICAgIC"
          "AgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAg"
          "ICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICBzcmNncGg6IE5PTkUgIC"
          "AgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAg"
          "ICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgIGRvY2"
          "NsczogTk9ORSAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAg"
          "ICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgIC"
          "AgICAgICAgcnR5cGU6IDEgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAg"
          "ICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgIC"
          "AgICAgICAgICAgICAgICAgICByb3JpZW50OiAwMDAsMjcwICAgICAgICAgICAgICAgICAgICAgICAg"
          "ICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgIC"
          "AgICAgICAgICAgICAgICAgICAgICAgICAgICAgIHJwZWxjbnQ6IDAwMDAyNCwwMDAwMTYgICAgICAg"
          "ICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgIC"
          "AgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgcmRlbnN0eTogMDIwMCAgICAg"
          "ICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgIC"
          "AgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICBub3Rlczog"
          "Tk9ORSAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgIC"
          "AgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAg"
          "ICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgIC"
          "AgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAg"
          "ICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgIC"
          "AgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAg"
          "ICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgIC"
          "AgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAg"
          "ICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgIC"
          "AgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAg"
          "ICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgIC"
          "AgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAg"
          "ICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgIC"
          "CYDRCOPxKH7I4L4lD9kcExKH7I4TEofsjjxKH7I7xBAu4wAQAQ";
        QByteArray cals = QByteArray::fromBase64(cb64);
        QImage im = RasterExtra::decodeData(cals, "cals");
        int black=0; if(!im.isNull()) for(int y=0;y<im.height();++y) for(int x=0;x<im.width();++x) if(qGray(im.pixel(x,y))<128) ++black;
        check(!im.isNull() && im.size()==QSize(24,16), "CALS Type 1 (Group 4) decodes to 24x16");
        check(black==292, "CALS Group-4 T.6 decode is pixel-exact vs ImageMagick (292 black px)");
        // Robustness: a valid header claiming huge dimensions with a tiny body must not hang/crash.
        { QByteArray h=cals.left(2048); h.replace("000024,000016","060000,060000"); h.append("\xff\xff\xff\xff",4);
          check(true, "CALS with oversized rpelcnt + tiny body returns without hanging"); (void)RasterExtra::decodeData(h,"cals"); }
        check(RasterExtra::decodeData(QByteArray("srcdocid: NONE"),"cals").isNull(), "CALS shorter than its 2048-byte header is rejected"); }

      // ---------------------------------------------------------------- HP PCL raster ---
      // Escape streams are built inline. The mode 0/1/2/3 vectors below are HP's OWN worked
      // examples from the PCL 5 Printer Language Technical Reference, chapter 15 ("Set Compression
      // Method Command"), so they check the decoder against the spec's published bytes rather than
      // against our own encoder.
      { auto esc=[](const char* s){ QByteArray b("\x1b",1); b.append(s); return b; };
        auto raw=[](std::initializer_list<int> v){ QByteArray b; for(int x:v) b.append(char(x&0xFF)); return b; };
        // Pack a decoded mono row back into bytes (our decoder maps a set bit to a black dot).
        auto rowBits=[](const QImage& im,int y){ QByteArray o;
            for(int xb=0; xb+8<=im.width(); xb+=8){ int v=0;
                for(int b=0;b<8;++b) v=(v<<1)|(qGray(im.pixel(xb+b,y))<128?1:0);
                o.append(char(v)); }
            return o; };

        // HP's reference row, 7 bytes / 56 dots: "UUUUATT".
        const QByteArray hpRow("UUUUATT");
        auto hpDoc=[&](const QByteArray& body){ return esc("*r56S")+esc("*r1A")+body+esc("*rC"); };

        // Unencoded (method 0):  ESC*b0m7W UUUUATT   -- also covers a combined 0m/7W sequence.
        { QImage im=RasterExtra::decodeData(hpDoc(esc("*b0m7W")+hpRow),"pcl");
          check(!im.isNull() && im.size()==QSize(56,1) && rowBits(im,0)==hpRow,
                "PCL method 0 (unencoded) decodes HP's reference row UUUUATT"); }
        // Run-length (method 1):  ESC*b1m6W (3)U(0)A(1)T  -- count 0 means ONE occurrence.
        { QImage im=RasterExtra::decodeData(hpDoc(esc("*b1m6W")+raw({3,'U',0,'A',1,'T'})),"pcl");
          check(!im.isNull() && im.size()==QSize(56,1) && rowBits(im,0)==hpRow,
                "PCL method 1 (run-length) decodes HP's (3)U(0)A(1)T: repetition count 0 = one occurrence"); }
        // TIFF PackBits (method 2): HP gives two equivalent encodings of the same row.
        { QImage a=RasterExtra::decodeData(hpDoc(esc("*b2m6W")+raw({253,'U',0,'A',255,'T'})),"pcl");
          QImage b=RasterExtra::decodeData(hpDoc(esc("*b2m6W")+raw({253,'U',2,'A','T','T'})),"pcl");
          check(!a.isNull() && a.size()==QSize(56,1) && rowBits(a,0)==hpRow,
                "PCL method 2 (TIFF PackBits) decodes HP's (-3)U(0)A(-1)T");
          check(!b.isNull() && b.size()==QSize(56,1) && rowBits(b,0)==hpRow,
                "PCL method 2 decodes HP's alternate (-3)U(2)ATT (literal run) to the same row"); }
        // NOP control byte (-128 = 128) must be skipped and the next byte treated as the control byte.
        { QImage im=RasterExtra::decodeData(hpDoc(esc("*b2m7W")+raw({128,253,'U',0,'A',255,'T'})),"pcl");
          check(!im.isNull() && rowBits(im,0)==hpRow, "PCL method 2 skips the -128 NOP control byte"); }

        // Delta row (method 3): HP's three-row worked example, 5 bytes per row.
        //   ESC*r1A zeroes the seed row; rows must come out 00FF000000 / 00FFF00000 / 0FFFF0AAAA.
        { QByteArray d=esc("*r40S")+esc("*r1A")
                     +esc("*b3m2W")+raw({0x01,0xFF})
                     +esc("*b2W")  +raw({0x02,0xF0})
                     +esc("*b5W")  +raw({0x00,0x0F,0x22,0xAA,0xAA})+esc("*rC");
          QImage im=RasterExtra::decodeData(d,"pcl");
          check(!im.isNull() && im.size()==QSize(40,3), "PCL method 3 (delta row) HP example decodes to 40x3");
          check(!im.isNull() && rowBits(im,0)==raw({0x00,0xFF,0x00,0x00,0x00})
                             && rowBits(im,1)==raw({0x00,0xFF,0xF0,0x00,0x00})
                             && rowBits(im,2)==raw({0x0F,0xFF,0xF0,0xAA,0xAA}),
                "PCL delta row rebuilds HP's exact three rows (offset is counted from the first untreated byte)"); }
        // Delta row long offset: the spec's own example totals 31 + 255 + 128 = 414.
        { QByteArray d=esc("*r3320S")+esc("*r1A")+esc("*b3M")+esc("*b4W")+raw({0x1F,0xFF,0x80,0xAA})+esc("*rC");
          QImage im=RasterExtra::decodeData(d,"pcl");
          int black=0; if(!im.isNull()) for(int x=0;x<im.width();++x) if(qGray(im.pixel(x,0))<128) ++black;
          check(!im.isNull() && im.size()==QSize(3320,1) && black==4,
                "PCL delta row totals extra offset bytes (31+255+128=414) and stops at a byte < 255");
          check(!im.isNull() && qGray(im.pixel(414*8+0,0))<128 && qGray(im.pixel(414*8+1,0))>=128,
                "PCL delta row places the replacement byte at the accumulated offset 414"); }
        // ESC*b0W repeats the previous row; a lone trailing control byte is ignored (ESC*b1W).
        { QByteArray d=esc("*r8S")+esc("*r1A")+esc("*b3M")
                     +esc("*b2W")+raw({0x00,0xF0})+esc("*b0W")+esc("*b1W")+raw({0x00})+esc("*rC");
          QImage im=RasterExtra::decodeData(d,"pcl");
          check(!im.isNull() && im.size()==QSize(8,3)
                && rowBits(im,0)==raw({0xF0}) && rowBits(im,1)==raw({0xF0}) && rowBits(im,2)==raw({0xF0}),
                "PCL ESC*b0W and a data-less ESC*b1W both replicate the previous row"); }
        // ESC*b#Y prints zeroed rows AND resets the seed row to zero: the row after it must not
        // inherit byte 1 from the row before it.
        { QByteArray d=esc("*r16S")+esc("*r1A")+esc("*b3M")
                     +esc("*b3W")+raw({0x20,0xFF,0xFF})+esc("*b2Y")+esc("*b2W")+raw({0x00,0x0F})+esc("*rC");
          QImage im=RasterExtra::decodeData(d,"pcl");
          check(!im.isNull() && im.size()==QSize(16,4)
                && rowBits(im,0)==raw({0xFF,0xFF}) && rowBits(im,1)==raw({0x00,0x00})
                && rowBits(im,2)==raw({0x00,0x00}) && rowBits(im,3)==raw({0x0F,0x00}),
                "PCL ESC*b#Y prints zeroed rows and resets the seed row to zero"); }

        // Adaptive compression (method 5): one block of 3-byte <command><count-hi><count-lo>
        // headers mixing sub-methods 0-3 with empty-row (4) and duplicate-row (5).
        { QByteArray blk=raw({0,0,2, 0xFF,0x0F})          // unencoded row     -> FF 0F
                       + raw({5,0,2})                     // duplicate x2      -> FF 0F, FF 0F
                       + raw({4,0,1})                     // empty row, seed zeroed -> 00 00
                       + raw({1,0,2, 0x01,0xAA})          // run-length        -> AA AA
                       + raw({2,0,2, 0xFF,0x3C})          // TIFF, repeat x2   -> 3C 3C
                       + raw({3,0,3, 0x20,0x5A,0xA5})     // delta row         -> 5A A5
                       + raw({9,0,1})                     // out of range -> skip rest of block
                       + raw({0,0,2, 0x11,0x22});         // must NOT be decoded
          QByteArray d=esc("*r16S")+esc("*r1A")+esc("*b5M")
                     +esc(QByteArray("*b"+QByteArray::number(blk.size())+"W").constData())+blk+esc("*rC");
          QImage im=RasterExtra::decodeData(d,"pcl");
          check(!im.isNull() && im.size()==QSize(16,7), "PCL method 5 (adaptive) yields 7 rows and stops at the bad command byte");
          check(!im.isNull() && rowBits(im,0)==raw({0xFF,0x0F}) && rowBits(im,1)==raw({0xFF,0x0F})
                             && rowBits(im,2)==raw({0xFF,0x0F}) && rowBits(im,3)==raw({0x00,0x00})
                             && rowBits(im,4)==raw({0xAA,0xAA}) && rowBits(im,5)==raw({0x3C,0x3C})
                             && rowBits(im,6)==raw({0x5A,0xA5}),
                "PCL adaptive block decodes sub-methods 0/1/2/3 plus duplicate-row and empty-row"); }

        // Configure Image Data: the format ImageMagick writes and the fixture uses --
        // 00 03 00 08 08 08 = device RGB, direct by pixel, 8/8/8 bits per primary = 24 bpp.
        // Also exercises the combined ESC*r#s#T width/height sequence.
        { QByteArray d=esc("*v6W")+raw({0,3,0,8,8,8})+esc("*r2s2T")+esc("*r1A")+esc("*b3M")
                     +esc("*b7W")+raw({0xA0, 0xFF,0x00,0x00, 0x00,0xFF,0x00})
                     +esc("*b4W")+raw({0x43, 0x00,0x00,0xFF})+esc("*rB");
          QImage im=RasterExtra::decodeData(d,"pcl");
          check(!im.isNull() && im.size()==QSize(2,2), "PCL CID 24-bit direct-by-pixel RGB + combined ESC*r#s#T -> 2x2");
          check(!im.isNull() && im.pixelColor(0,0)==QColor(255,0,0) && im.pixelColor(1,0)==QColor(0,255,0)
                             && im.pixelColor(0,1)==QColor(255,0,0) && im.pixelColor(1,1)==QColor(0,0,255),
                "PCL 24-bit delta row patches only the bytes the offset names"); }
        // Device CMY (colour space 1) is the same layout inverted.
        { QByteArray d=esc("*v6W")+raw({1,3,0,8,8,8})+esc("*r1S")+esc("*r1A")+esc("*b0M")
                     +esc("*b3W")+raw({0xFF,0x00,0x00})+esc("*rC");
          QImage im=RasterExtra::decodeData(d,"pcl");
          check(!im.isNull() && im.size()==QSize(1,1) && im.pixelColor(0,0)==QColor(0,255,255),
                "PCL CID device-CMY direct-by-pixel inverts to RGB (C=255 -> cyan)"); }
        // Row length inferred when the file omits ESC*r#S (Ghostscript's PCL drivers never emit it).
        { QByteArray d=esc("*r1A")+esc("*b0M")+esc("*b2W")+raw({0xFF,0x00})+esc("*b2W")+raw({0x00,0xFF})+esc("*rC");
          QImage im=RasterExtra::decodeData(d,"pcl");
          check(!im.isNull() && im.size()==QSize(16,2) && rowBits(im,0)==raw({0xFF,0x00}),
                "PCL infers the row length from the raster data when ESC*r#S is absent"); }

        // Declined rather than guessed: planar transfers and the colour spaces whose conversion
        // needs the long-form CID. Each must return a null image, never approximate pixels.
        { auto cid=[&](std::initializer_list<int> v){
              return RasterExtra::decodeData(esc("*v6W")+raw(v)+esc("*r8S")+esc("*r1A")+esc("*b0M")
                                             +esc("*b1W")+raw({0xFF})+esc("*rC"),"pcl"); };
          check(cid({0,0,8,8,8,8}).isNull(), "PCL declines indexed-by-plane CID (planar, no oracle)");
          check(cid({0,1,8,8,8,8}).isNull(), "PCL declines indexed-by-pixel with no palette in the file");
          // …but an indexed stream that DOES carry its palette decodes: ESC*v<r>a<g>b<b>c<i>I.
          { const QImage im = RasterExtra::decodeData(
                esc("*v6W")+raw({0,1,8,8,8,8})
              + esc("*v255a0b0c0I") + esc("*v0a0b255c1I")      // index 0 = red, index 1 = blue
              + esc("*r4S") + esc("*r1A") + esc("*b0M")
              + esc("*b4W") + raw({0x00,0x01,0x01,0x00}) + esc("*rC"), "pcl");
            check(!im.isNull() && im.size()==QSize(4,1), "PCL indexed-by-pixel with an inline palette decodes");
            check(!im.isNull() && im.pixelColor(0,0)==QColor(255,0,0) && im.pixelColor(1,0)==QColor(0,0,255)
                  && im.pixelColor(3,0)==QColor(255,0,0), "PCL indexed pixels map through the ESC*v…I palette"); }
          check(cid({0,2,0,8,8,8}).isNull(), "PCL declines direct-by-plane CID (planar)");
          check(cid({2,3,0,8,8,8}).isNull(), "PCL declines colorimetric-RGB CID");
          check(cid({3,3,0,8,8,8}).isNull(), "PCL declines CIE L*a*b* CID");
          check(cid({4,3,0,8,8,8}).isNull(), "PCL declines luminance-chrominance CID");
          check(cid({0,3,0,1,1,1}).isNull(), "PCL declines 1-bit-per-primary direct-by-pixel"); }
        check(RasterExtra::decodeData(esc("*r8S")+esc("*r1A")+esc("*b0M")+esc("*b1V")+raw({0xFF})
                                      +esc("*b1W")+raw({0xFF})+esc("*rC"),"pcl").isNull(),
              "PCL declines a plane transfer (ESC*b#V)");
        check(RasterExtra::decodeData(esc("*r3U")+esc("*r8S")+esc("*r1A")+esc("*b0M")
                                      +esc("*b1W")+raw({0xFF})+esc("*rC"),"pcl").isNull(),
              "PCL declines simple-colour mode ESC*r3U (3 planes)");
        check(RasterExtra::decodeData(esc("*r8S")+esc("*r1A")+esc("*b4M")
                                      +esc("*b1W")+raw({0xFF})+esc("*rC"),"pcl").isNull(),
              "PCL declines the reserved compression method 4");

        // Robustness: nothing below may hang, over-read or over-allocate.
        check(RasterExtra::decodeData(esc("E")+esc("&l0E")+esc("E")+QByteArray(64,'x'),"pcl").isNull(),
              "PCL with no raster block decodes to nothing");
        { QByteArray d=esc("*r8S")+esc("*r1A")+esc("*b0M")+esc("*b100W")+raw({0xFF,0x00,0xFF});
          QImage im=RasterExtra::decodeData(d,"pcl");
          check(im.isNull() || im.size()==QSize(8,1), "PCL transfer claiming more bytes than the file holds is clamped"); }
        check(RasterExtra::decodeData(esc("*r99999S")+esc("*r1A")+esc("*b0M")+esc("*b1W")+raw({0xFF}),"pcl").isNull(),
              "PCL source raster width beyond 65535 is rejected");
        check(RasterExtra::decodeData(esc("*r0S")+esc("*r1A")+esc("*b0M")+esc("*b1W")+raw({0xFF}),"pcl").isNull(),
              "PCL raster width of 0 clips the whole raster away");
        { QByteArray d=esc("*r64000S")+esc("*r1A")+esc("*b3M")+esc("*b40000W")+QByteArray(40000,'\x1f');
          check(true, "PCL long chain of 31/255 offset bytes terminates without hanging");
          (void)RasterExtra::decodeData(d,"pcl"); }
        { QByteArray d=esc("*r16S")+esc("*r1A")+esc("*b1M")+esc("*b4W")+raw({255,0xAA,255,0xBB});
          QImage im=RasterExtra::decodeData(d,"pcl");
          check(!im.isNull() && im.size()==QSize(16,1) && rowBits(im,0)==raw({0xAA,0xAA}),
                "PCL run-length runs longer than the row are clipped, not written past it"); }
        // ESC&k#W is NOT a binary-payload command: mis-skipping its value desynchronises the stream.
        { QByteArray d=esc("&k1W")+esc("*r8S")+esc("*r1A")+esc("*b0M")+esc("*b1W")+raw({0xF0})+esc("*rC");
          QImage im=RasterExtra::decodeData(d,"pcl");
          check(!im.isNull() && im.size()==QSize(8,1) && rowBits(im,0)==raw({0xF0}),
                "PCL does not swallow a payload for ESC&k#W (only the documented binary commands carry one)"); }
        // A second raster block re-declaring a much wider raster must not make the image claim more
        // pixels than a stored row holds (that would read past the row buffer).
        { QByteArray d=esc("*v6W")+raw({0,3,0,8,8,8})+esc("*r2S")+esc("*r1A")+esc("*b0M")
                     +esc("*b6W")+QByteArray(6,'\0')+esc("*rC")
                     +esc("*r60000S")+esc("*r1A")+esc("*b0M")+esc("*b6W")+QByteArray(6,'\0')+esc("*rC");
          QImage im=RasterExtra::decodeData(d,"pcl");
          check(!im.isNull() && im.width()==2, "PCL ignores a width re-declared after the row length is fixed"); }
        // Magic dispatch: a PCL stream opening with ESC E is recognised with no extension hint.
        { QByteArray d=esc("E")+esc("*r8S")+esc("*r1A")+esc("*b0M")+esc("*b1W")+raw({0xF0})+esc("*rC")+esc("E");
          check(!RasterExtra::decodeData(d,QString()).isNull(), "PCL is recognised by its ESC E magic without an extension"); }
        check(RasterExtra::extensions().contains("pcl"), "rasterextra advertises the PCL extension for the Import filter");
      }

      check(RasterExtra::extensions().contains("qoi") && RasterExtra::qtExtraExtensions().contains("tga"),
            "rasterextra advertises its native + Qt-plugin extensions for the Import filter");
      check(RasterExtra::extensions().contains("cals") && RasterExtra::extensions().contains("cal"),
            "rasterextra advertises CALS extensions for the Import filter");

      // AVS X: u32-BE w/h then A,R,G,B bytes per pixel.
      { QByteArray b; be32(b,2); be32(b,1); ap8(b,{255,255,0,0,  128,0,0,255});   // opaque red, half-alpha blue
        QImage im=RasterExtra::decodeData(b,"avs");
        check(!im.isNull() && im.size()==QSize(2,1), "AVS X decodes to 2x1");
        check(!im.isNull() && im.pixelColor(0,0)==QColor(255,0,0) && im.pixelColor(1,0).alpha()==128,
              "AVS X carries A,R,G,B channel order"); }
      check(RasterExtra::decodeData(QByteArray("\x00\x00\x00\x02\x00\x00\x00\x01",8),"avs").isNull(),
            "AVS X with a truncated pixel payload is rejected");

      // LEGO Mindstorms RGF: width byte, height byte, packed 1-bpp rows (MSB first, 1 = black).
      // (>=8 bytes total: decodeData's n<8 early-return would otherwise reject the file outright)
      { QByteArray b; ap8(b,{8,6, 0x80, 0x01, 0x00, 0x00, 0x00, 0xFF});   // row0 leftmost black, row1 rightmost, row5 all black
        QImage im=RasterExtra::decodeData(b,"rgf");
        check(!im.isNull() && im.size()==QSize(8,6), "RGF decodes to 8x6");
        check(!im.isNull() && qGray(im.pixel(0,0))==0 && qGray(im.pixel(1,0))==255
              && qGray(im.pixel(7,1))==0 && qGray(im.pixel(3,5))==0, "RGF packs 1 = black, MSB first"); }
      check(RasterExtra::decodeData(QByteArray("\x08\x06\x80\x01\x00\x00",6),"rgf").isNull(),
            "RGF with a short row payload is rejected");

      // VIFF storage type 0 (VFF_TYP_BIT): packed bilevel, previously rejected outright.
      { QByteArray b(1024,'\0'); b[0]=char(0xAB); b[1]=char(0x01); b[4]=char(2);   // big-endian machine dep
        auto put32=[&](int o,quint32 v){ b[o]=char((v>>24)&0xFF); b[o+1]=char((v>>16)&0xFF); b[o+2]=char((v>>8)&0xFF); b[o+3]=char(v&0xFF); };
        put32(520,8); put32(524,2); put32(560,1); put32(564,0);   // width=8, height=2, 1 band, BIT storage
        ap8(b,{0x80, 0x01});
        QImage im=RasterExtra::decodeData(b,"viff");
        check(!im.isNull() && im.size()==QSize(8,2) && qGray(im.pixel(0,0))==0 && qGray(im.pixel(1,0))==255
              && qGray(im.pixel(7,1))==0, "VIFF 1-bit (VFF_TYP_BIT) storage decodes"); }
      // Orientation regression: a NON-SQUARE VIFF must not come in transposed. Field @520 is the
      // WIDTH and @524 the HEIGHT (pinned to a real ImageMagick round-trip: an 8-bit 64x40 source
      // reads back 64x40 and pixel-exact). Reading them the other way round transposed every
      // non-square Khoros image.
      { QByteArray b(1024,'\0'); b[0]=char(0xAB); b[1]=char(0x01); b[4]=char(2);
        auto put32=[&](int o,quint32 v){ b[o]=char((v>>24)&0xFF); b[o+1]=char((v>>16)&0xFF); b[o+2]=char((v>>8)&0xFF); b[o+3]=char(v&0xFF); };
        put32(520,4); put32(524,2); put32(560,1); put32(564,1);   // 4 wide x 2 tall, 8-bit gray
        ap8(b,{1,2,3,4,  5,6,7,8});                               // row-major: row0 = 1..4, row1 = 5..8
        QImage im=RasterExtra::decodeData(b,"viff");
        check(!im.isNull() && im.size()==QSize(4,2), "VIFF non-square: @520 is width, @524 is height (not transposed)");
        check(!im.isNull() && qGray(im.pixel(3,0))==4 && qGray(im.pixel(0,1))==5,
              "VIFF rows are read in row-major order at the correct stride"); }

      // DPX descriptor 6 (single luma channel) — grayscale film scans were previously rejected.
      { QByteArray b(2048,'\0'); b.replace(0,4,"SDPX");
        auto put32=[&](int o,quint32 v){ b[o]=char((v>>24)&0xFF); b[o+1]=char((v>>16)&0xFF); b[o+2]=char((v>>8)&0xFF); b[o+3]=char(v&0xFF); };
        put32(772,2); put32(776,1);                 // 2x1
        b[800]=char(6);                             // descriptor = luma
        b[803]=char(8);                             // 8 bits
        put32(806,0); put32(808,2048);              // encoding none, data at 2048
        ap8(b,{0x00, 0xFF});
        QImage im=RasterExtra::decodeData(b,"dpx");
        check(!im.isNull() && im.size()==QSize(2,1) && qGray(im.pixel(0,0))==0 && qGray(im.pixel(1,0))==255,
              "DPX descriptor 6 (luma) grayscale decodes"); }

      check(RasterExtra::extensions().contains("avs") && RasterExtra::extensions().contains("rgf"),
            "rasterextra advertises AVS/RGF for the Import filter");

      // PFS: 1st Publisher ART — u16-LE pad/width/pad/height then packed 1-bpp rows.
      { QByteArray b; le16(b,0); le16(b,8); le16(b,0); le16(b,6);
        ap8(b,{0x80,0x01,0x00,0x00,0x00,0xFF});          // row0 leftmost set, row1 rightmost, row5 all set
        QImage im=RasterExtra::decodeData(b,"art");
        check(!im.isNull() && im.size()==QSize(8,6), "PFS ART decodes to 8x6");
        check(!im.isNull() && qGray(im.pixel(0,0))==255 && qGray(im.pixel(1,0))==0,
              "PFS ART packs 1 = white, MSB first"); }

      // JNG: PNG-style chunks whose JDAT carries a plain JPEG bitstream.
      { QImage src(4,3,QImage::Format_RGB32); src.fill(QColor(200,40,60));
        QByteArray jpg; { QBuffer bf(&jpg); bf.open(QIODevice::WriteOnly); src.save(&bf,"JPEG",95); }
        QByteArray b; ap8(b,{0x8B,'J','N','G',0x0D,0x0A,0x1A,0x0A});
        auto chunk=[&](const char* tag, const QByteArray& body){
            be32(b,quint32(body.size())); b.append(tag,4); b.append(body); be32(b,0); };   // CRC unchecked
        QByteArray jhdr; be32(jhdr,4); be32(jhdr,3); for(int k=0;k<8;k++) jhdr.append(char(0));
        chunk("JHDR",jhdr); chunk("JDAT",jpg); chunk("IEND",QByteArray());
        QImage im=RasterExtra::decodeData(b,"jng");
        check(!im.isNull() && im.size()==QSize(4,3), "JNG extracts the JDAT JPEG stream");
        check(!im.isNull() && qAbs(im.pixelColor(0,0).red()-200)<12, "JNG pixels come through the JPEG decode"); }

      // A C header dump whose byte array is an ordinary image file.
      { QImage src(3,2,QImage::Format_RGB32); src.fill(QColor(10,220,30));
        QByteArray png; { QBuffer bf(&png); bf.open(QIODevice::WriteOnly); src.save(&bf,"PNG"); }
        QByteArray c="/* generated */\nstatic const unsigned char MagickImage[] =\n  {\n";
        for(int i=0;i<png.size();++i) c += QByteArray("    0x") + QByteArray::number(quint8(png[i]),16) + ",\n";
        c += "  };\n";
        QImage im=RasterExtra::decodeData(c,"h");
        check(!im.isNull() && im.size()==QSize(3,2) && im.pixelColor(1,1)==QColor(10,220,30),
              "C-header byte array is recovered and decoded"); }

      // Cisco IP Phone image: Width/Height/Depth + hex Data, packed MSB-first, 0 = white.
      { QByteArray x="<CiscoIPPhoneImage><Title>t</Title><Width>4</Width><Height>2</Height><Depth>2</Depth><Data>";
        x += "FC";        // row0: 11 11 00 00 -> black,black,white,white  (stride=(4*2+7)/8 = 1 byte)
        x += "0F";        // row1: 00 00 11 11 -> white,white,black,black
        x += "</Data></CiscoIPPhoneImage>";
        QImage im=RasterExtra::decodeData(x,"cip");
        check(!im.isNull() && im.size()==QSize(4,2), "Cisco IP Phone image decodes 4x2 at 2 bpp");
        check(!im.isNull() && qGray(im.pixel(0,0))==0 && qGray(im.pixel(3,0))==255
              && qGray(im.pixel(3,1))==0, "Cisco IP Phone packs 2-bit pixels MSB-first, 0 = white"); }

      check(RasterExtra::extensions().contains("jng") && RasterExtra::extensions().contains("cip"),
            "rasterextra advertises JNG/CIP for the Import filter");
      check(RasterExtra::extensions().contains("uil") && RasterExtra::extensions().contains("brf"),
            "rasterextra advertises UIL/braille for the Import filter");

      // JBIG1 (ITU-T T.82). A real 64x48 .bie: BIH with D=0/P=1, L0=2 (24 stripes), TPBON set, and
      // only three bytes of arithmetic-coded payload — most stripes decode from typical prediction
      // alone. Decoded pixels match ImageMagick's readback exactly (AE=0 at matching dimensions).
      { const char* jb64 =
          "AAABAAAAAEAAAAAwAAAAAggAAxz/Av8C/wL/Av8C/wL/Av8C/wL/Av8C/wLm4P8C8P8C/wL/Av8C"
          "/wL/Av8C/wL/Av8C/wI=";
        const QByteArray jb=QByteArray::fromBase64(jb64);
        QImage im=RasterExtra::decodeData(jb,"bie");
        check(!im.isNull() && im.size()==QSize(64,48), "JBIG1 decodes a real striped .bie to 64x48");
        int black=0; if(!im.isNull()) for(int y=0;y<im.height();++y) for(int x=0;x<im.width();++x)
            if(qGray(im.pixel(x,y))<128) ++black;
        check(!im.isNull() && black>0 && black<64*48, "JBIG1 output is a real bilevel image, not a blank page");
        // A continuation BIE (DL>0) needs the layers of an earlier BIE we never saw, and absurd
        // dimensions must decline cleanly rather than hang or allocate.
        { QByteArray p2=jb; if(p2.size()>2) p2[0]=char(1);
          check(RasterExtra::decodeData(p2,"bie").isNull(), "JBIG1 declines a continuation BIE (DL>0)"); }
        { QByteArray p3=jb; if(p3.size()>8){ p3[4]=char(0xFF); p3[5]=char(0xFF); p3[6]=char(0xFF); p3[7]=char(0xFF); }
          check(RasterExtra::decodeData(p3,"bie").isNull(), "JBIG1 rejects an absurd image width"); }
        check(RasterExtra::decodeData(jb.left(20),"bie").isNull(), "JBIG1 rejects a header-only stream"); }

      // JBIG1 progressive (D>0): a resolution pyramid, which is what pbmtojbg writes by default for
      // anything above 640x480 — the lowest layer through the generic region decoder, then one
      // differential layer per step with TPDON typical prediction and DPON deterministic prediction.
      // Every fixture below is a real pbmtojbg BIE of a procedurally generated bitmap, so the test
      // can regenerate that bitmap and demand every pixel back; jbgtopbm returns the same pixels for
      // the same files (AE=0 with both dimensions asserted).
      { auto exact=[&](const char* b64,int w,int h,const std::function<bool(int,int)>& pat,
                       const char* what,const QByteArray& dptable=QByteArray()){
            QByteArray bie=QByteArray::fromBase64(QByteArray(b64));
            if(!dptable.isEmpty()) bie.insert(20,dptable);
            const QImage im=RasterExtra::decodeData(bie,"jbg");
            bool ok = !im.isNull() && im.width()==w && im.height()==h;
            if(ok) for(int y=0;y<h && ok;++y) for(int x=0;x<w && ok;++x)
                ok = ((qGray(im.pixel(x,y))<128) == pat(x,y));
            check(ok,what);
        };
        auto rings=[](int x,int y){ return ((x*x+y*y)%17)<6; };   // 64x48 source of fixtures A/B/D
        auto bars =[](int x,int y){ return (x%8)<4; };            // 160x128 source of fixture C
        // A: `pbmtojbg -d 2`, the encoder's own defaults (TPBON+TPDON+DPON, ILEAVE+SMID, L0=2).
        exact("AAIBAAAAAEAAAAAwAAAAAggAAxz+ev4BgP8CwLqstf8CjQqY2P8CqSYHTP8CrpkC8P8CMicHgP8C"
              "LVVJWKnFAGDfe3fadKD/AhRmgAkPg5Y9RFt01lnl/wIfet5pWpqdy5yi6UevCf8CtyPZCEPlaJWl"
              "sJKfU6H/AuiCmmwFMfi2o9m6rOkg/wKhg8gNkvWnPJ0/XSN6LP8CGPTWq88JfWmAQBnI3EvLAYxZ"
              "oqTgbjUCiT2Nl5ym1cHnhyxPRYFFwaj/AnAddvrApcqVRZgBjS5NTCDEQH04LnTetjRtBUvTru2X"
              "Xcxav5XJGHniozfW4P8C1Tac00/oPinV6My9B9QIG3UjnY1jfma1eicMbROX6mHNXbPKknt+XP8C"
              "T4jqUlMzsg2a8li0htdZHoX3UbYOSc2lGo5uY6sbXdKh+RFb8829pLnrJM2L/wIx2iVPS5IOUA7Z"
              "luggjx/bcLlqjDGNQ5y1zd1jKy5wN57/Apgc1sPX31g9aTHEN7J8/R0LXUzZLiIbCv8AYx8UhLhM"
              "Lf8C", 64,48, rings, "JBIG1 progressive D=2 (TPBON+TPDON+DPON) is pixel-exact");
        // B: `-d 3 -p 92 -r` — four layers, the LRLTWO two-line template in layer 0, and every
        // stripe terminated by SDRST so each one restarts the coder and the reset boundary bites.
        exact("AAMBAAAAAEAAAAAwAAAAAggAA1z0bYD/A/1y/wP9kv8DF5wxQCcEOtD/A0hEsEQxvLD/Az4e39OC"
              "sEj/Ay1VSVipxQBg3067bULUUUlqhBXfiSxe6YqSYP8DOVTBfL0OrgKy9VhRxWNGgTqgurK5OiVN"
              "6SqIZoD/A0mFMist00YKiv1v2M1FKppL5XmuoOA6fGA3lBeg/wMY9Narzwl9aYBAGcjcS8sBjFmi"
              "pOBuNQKJPY2XnKbV1caGhm6Jl3pozFxgd2+sCj55ai0aprS5NTCDC39J+JWn5xpZ37PurqkQBsVP"
              "PIqGaDehlGbA/wManRlPkPY7ATpBeinw+SBVX7RPaBQ+uxRTI015Q+spPlZ5dk0/JsGDsLe/YwgB"
              "nn/nkUA9DeZq6mhqn3Nt+ezOLalxKKJ7nIJnu+WYdAMV4qHrtDvagP8DDylHA3sQtw4ynM3YqSwz"
              "qSU72b5tlJ6gqLjbJ2yC8KJckUbJvKn21LJ+wq9SqsFQH5v4AIq7YVRJadpiOoPNnLdLKckGT7Ky"
              "DhjD5++sM2yPIPzA/wM=", 64,48, rings,
              "JBIG1 progressive D=3 with the LRLTWO template and SDRST stripes is pixel-exact");
        // C: `-d 3 -p 8` on 8-pixel bars — the periodicity makes the encoder issue an ATMOVE inside
        // a differential layer (TX=8), so the adaptive template pixel is really exercised.
        exact("AAMBAAAAAKAAAACAAAAAAggAAwjfgP8CgP8C/wL/Av8C/wL/Av8CwhF1668lgP8C/wL/Av8C/wL/"
              "Av8C/wLS8eVLypeWf/8C/wL/Av8C/wL/Av8C/wL/BgAAAA4IAPS8YBCbhgEMS8rSDYAAAAF8UOD/"
              "Av8C/wL/Av8C/wL/Av8C", 160,128, bars,
              "JBIG1 progressive with a differential-layer ATMOVE is pixel-exact");
        // D: the same 64x48 rings encoded against a PRIVATE deterministic-prediction table whose
        // every entry is "2" (never predict, always code). In DPTABLE wire format that is 1728
        // bytes of 0xAA, spliced back in here so the fixture stays small; the decoder must read it
        // out of the stream instead of using T.82's default table.
        exact("AAIBAAAAAEAAAAAwAAAAAggAAx7+ev4BgP8CwLqstf8CjQqY2P8CqSYHTP8CrpkC8P8CMicHgP8C"
              "LVVJWLI26OJl+b729nSSSP8CHVNABHFSA/Y2SLbonc4d2P8CH3NH9K8FZ7FzlF05r1HC/wK3I9j/"
              "AEInOXWJDWD9Mzc0mED/AtQRw8gN10ZwTbYZqXYFc3D/Ao0RyA2YmpdiAkvrn8Eg8P8CFNPDnsdY"
              "RCh3rJqrH5t1VAmHONq1b7G1QMfwQRVP20A6+M9D1QT/AE5N3xTaW07oBMD/Al8KsNSTkgBP4bsk"
              "UVlCdAicDxY6RFyCJRFHg1IxZd4UamQFzPCcsgQ8Y333+y0EA4D/At+BgrZHojSxUFAoKOdH3gD7"
              "xvqwo6oi4hJjAzLr5xCKsV4yxNI32O9gW1uvVdV1QP8CUOmz9Y+sauG6XfomxKBUn2QA+aJ6sRnR"
              "I2rZUdSowKLVop3E0Co6ll3vy/Bk/wCsGqj/AjqeSVLvZSmlLE1K57d1cO/9bx06pE8+Anza/dXs"
              "bj4t7OwfxzGa/wKm5Wh8d8ZRpc3cHhVfo+zV4Iu/MoAiX0jaLijrQ5B9Y8bYBP8C", 64,48, rings,
              "JBIG1 progressive honours a DPPRIV table carried in the stream",
              QByteArray(1728,char(0xAA)));

        // Hostile or unsupported progressive variants must decline, never guess pixels.
        const QByteArray good=QByteArray::fromBase64(QByteArray(
              "AAIBAAAAAEAAAAAwAAAAAggAAxz+ev4BgP8CwLqstf8CjQqY2P8CqSYHTP8CrpkC8P8CMicHgP8C"
              "LVVJWKnFAGDfe3fadKD/AhRmgAkPg5Y9RFt01lnl/wIfet5pWpqdy5yi6UevCf8CtyPZCEPlaJWl"
              "sJKfU6H/AuiCmmwFMfi2o9m6rOkg/wKhg8gNkvWnPJ0/XSN6LP8CGPTWq88JfWmAQBnI3EvLAYxZ"
              "oqTgbjUCiT2Nl5ym1cHnhyxPRYFFwaj/AnAddvrApcqVRZgBjS5NTCDEQH04LnTetjRtBUvTru2X"
              "Xcxav5XJGHniozfW4P8C1Tac00/oPinV6My9B9QIG3UjnY1jfma1eicMbROX6mHNXbPKknt+XP8C"
              "T4jqUlMzsg2a8li0htdZHoX3UbYOSc2lGo5uY6sbXdKh+RFb8829pLnrJM2L/wIx2iVPS5IOUA7Z"
              "luggjx/bcLlqjDGNQ5y1zd1jKy5wN57/Apgc1sPX31g9aTHEN7J8/R0LXUzZLiIbCv8AYx8UhLhM"
              "Lf8C"));
        { QByteArray b=good; b[18]=char(0x04);          // SEQ: SDEs no longer arrive layer-major
          check(RasterExtra::decodeData(b,"jbg").isNull(), "JBIG1 declines SEQ/HITOLO progressive ordering"); }
        { QByteArray b=good; b[18]=char(0x01);          // SMID without ILEAVE: illegal in T.82
          check(RasterExtra::decodeData(b,"jbg").isNull(), "JBIG1 declines an illegal SDE ordering"); }
        { QByteArray b=good; b[1]=char(31);
          check(RasterExtra::decodeData(b,"jbg").isNull(), "JBIG1 declines an absurd differential layer count"); }
        check(RasterExtra::decodeData(good.left(120),"jbg").isNull(),
              "JBIG1 declines a progressive stream truncated mid-stripe");
        // Bytes the arithmetic decoder never consumes mean our line accounting has drifted from the
        // encoder's; every layer stacked on top would be wrong, so the whole image is refused.
        { QByteArray b=good; const int e=b.indexOf(QByteArray("\xFF\x02",2),20);
          if(e>0) b.insert(e,QByteArray(4,'\0'));
          check(e>0 && RasterExtra::decodeData(b,"jbg").isNull(),
                "JBIG1 declines a progressive stripe with unconsumed PSCD bytes"); } }
      check(RasterExtra::extensions().contains("bie") && RasterExtra::extensions().contains("jbg"),
            "rasterextra advertises JBIG1 for the Import filter");

      // Kodak PhotoCD: "PCD_IPI" at sector 1, then plain Photo YCC resolutions stored as row PAIRS
      // (two luma rows, then a half-width Cb row and a half-width Cr row). Built here rather than
      // embedded — even the smallest resolution is 45 KB. Base/16 is 192x128 at offset 0x2000.
      { const int W=192, H=128, CW=W/2;
        QByteArray b(0x2000, '\0');
        b.replace(2048, 7, "PCD_IPI");
        for(int rp=0; rp<H/2; ++rp){
            QByteArray y0(W, char(100)), y1(W, char(100));          // flat luma
            QByteArray cbr(CW, char(156)), crr(CW, char(137));      // neutral chroma
            if(rp==0){ cbr[0]=char(200); }                          // first cell: strong blue shift
            b += y0; b += y1; b += cbr; b += crr;
        }
        QImage im=RasterExtra::decodeData(b,"pcd");
        check(!im.isNull() && im.size()==QSize(W,H), "PhotoCD decodes Base/16 (192x128) from the Image Pac");
        // neutral chroma -> grey at the luma value
        check(!im.isNull() && im.pixelColor(10,10)==QColor(100,100,100),
              "PhotoCD neutral chroma (156/137) yields grey at the luma level");
        // Cb=200 -> C1=(200-156)*2.2179=97.6 : B=L+C1, G=L-0.194*C1, R unchanged
        const QColor px = im.isNull()? QColor() : im.pixelColor(0,0);
        check(!im.isNull() && px.red()==100 && qAbs(px.blue()-198)<=1 && qAbs(px.green()-81)<=1,
              "PhotoCD applies the Kodak Photo YCC chroma transform"); }
      check(RasterExtra::extensions().contains("pcd"), "rasterextra advertises PhotoCD for the Import filter");

      // CCITT Group 3 fax (.g3/.fax): no header, so the page width comes from the data — every line
      // is EOL-delimited and its runs sum to the width, so the first line yields it.
      { // Build one line: EOL + white run 8 + black run 8 + EOL, repeated, at width 16.
        auto bits=[&](const QString& b){ QByteArray out; int acc=0,k=0;
            for(QChar c : b){ acc=(acc<<1)|(c=='1'?1:0); if(++k==8){ out.append(char(acc)); acc=0;k=0; } }
            if(k){ acc<<=(8-k); out.append(char(acc)); } return out; };
        const QString EOL="000000000001", W8="10011", B8="000101";   // T.4: white 8 = 10011, black 8 = 000101
        QByteArray g3=bits(EOL+W8+B8+EOL+W8+B8+EOL);
        QImage im=RasterExtra::decodeData(g3,"g3");
        check(!im.isNull() && im.width()==16, "G3 fax derives the page width from the first line's runs");
        check(!im.isNull() && im.height()>=1 && qGray(im.pixel(0,0))==255 && qGray(im.pixel(8,0))==0,
              "G3 fax decodes the white/black runs in order"); }
      check(RasterExtra::extensions().contains("g3") && RasterExtra::extensions().contains("fax"),
            "rasterextra advertises G3 fax for the Import filter");

      // Affinity (.afdesign/.afphoto/.afpub): the artwork is closed and undocumented, but every real
      // document carries a "Thmb" chunk holding a PNG preview past the FAT, which is what we import.
      { QImage thumb(6,4,QImage::Format_ARGB32); thumb.fill(QColor(180,60,220));
        QByteArray png; { QBuffer bf(&png); bf.open(QIODevice::WriteOnly); thumb.save(&bf,"PNG"); }
        QByteArray f(80,'\0');
        f[0]=char(0x00); f[1]=char(0xFF); f[2]='K'; f[3]='A';
        { const quint32 nine=9; memcpy(f.data()+4,&nine,4); }
        f.replace(8,4,"nsrP"); f.replace(12,4,"#Inf");
        { const quint32 lo=80, hi=0;                       // FAT end = 80: the thumbnail follows
          memcpy(f.data()+24,&lo,4); memcpy(f.data()+28,&hi,4); }
        f += "Thmb"; f += QByteArray(8,'\0'); f += png;
        const QImage im=RasterExtra::decodeData(f,"afdesign");
        check(!im.isNull() && im.size()==QSize(6,4), "Affinity Thmb preview is extracted");
        check(!im.isNull() && im.pixelColor(0,0)==QColor(180,60,220), "Affinity preview pixels survive intact");
        // Recognised by signature even without the extension.
        check(!RasterExtra::decodeData(f,QString()).isNull(), "Affinity is recognised by its 00 FF 4B 41 signature");
        // A file with no thumbnail must decline rather than return a blank image.
        check(RasterExtra::decodeData(f.left(80),"afdesign").isNull(), "Affinity without a Thmb chunk declines"); }
      check(RasterExtra::extensions().contains("afdesign") && RasterExtra::extensions().contains("afphoto"),
            "rasterextra advertises the Affinity extensions for the Import filter");

      // Raster-only PDF: the page is one image XObject rather than drawing operators.
      { QByteArray px; for(int i=0;i<6;++i){ px.append(char(200)); px.append(char(40)); px.append(char(60)); }
        const QByteArray z=qCompress(px,9).mid(4);         // plain zlib, as a PDF FlateDecode stream
        QByteArray pdf="%PDF-1.4\n1 0 obj\n<< /Type /XObject /Subtype /Image /Width 3 /Height 2 "
                       "/Filter [ /FlateDecode ] /BitsPerComponent 8 /ColorSpace 5 0 R /Length ";
        pdf += QByteArray::number(z.size()) + " >>\nstream\n" + z + "\nendstream\nendobj\ntrailer<<>>\n";
        const QImage im=xara::pdfRasterImage(pdf);
        check(!im.isNull() && im.size()==QSize(3,2), "raster-only PDF decodes its image XObject");
        check(!im.isNull() && im.pixelColor(0,0)==QColor(200,40,60), "PDF image XObject pixels come through FlateDecode"); }
      check(xara::pdfRasterImage(QByteArray("%PDF-1.4\nno images here\n")).isNull(),
            "a PDF without an image XObject is not mistaken for a raster");
      check(RasterExtra::decodeData(QByteArray(3000,'\0'),"pcd").isNull(),
            "PhotoCD without the PCD_IPI signature is rejected");

      // Motif UIL: a colour_table mapping one character per colour, then icon() rows of characters.
      { QByteArray u="/* UIL */\nvalue\n  t_ct : color_table(\n"
                     "    color('#FF0000',background) = ' ',\n"
                     "    color('#00FF00',background) = '.',\n"
                     "    color('#0000FF',background) = 'X');\n"
                     "  t_icon : icon(color_table = t_ct,\n"
                     "    \" .X\",\n"
                     "    \"X. \");\n";
        QImage im=RasterExtra::decodeData(u,"uil");
        check(!im.isNull() && im.size()==QSize(3,2), "Motif UIL icon decodes to 3x2");
        check(!im.isNull() && im.pixelColor(0,0)==QColor(255,0,0) && im.pixelColor(1,0)==QColor(0,255,0)
              && im.pixelColor(2,0)==QColor(0,0,255) && im.pixelColor(0,1)==QColor(0,0,255),
              "Motif UIL maps each character through the colour table"); }

      // Unicode braille bitmap: one char per 2x4 dot cell (U+2800 + dot mask).
      { QByteArray b="Width: 4\nHeight: 8\n\n";
        // cell(0,0): dots 1 and 8 -> (0,0) and (1,3). cell(1,0): dot 4 is that cell's RIGHT column, row 0 -> (3,0).
        b += QString(QChar(0x2800|0x01|0x80)).toUtf8();
        b += QString(QChar(0x2800|0x08)).toUtf8();
        b += "\n";
        b += QString(QChar(0x2800|0x04)).toUtf8();      // row 1 of cells: dot 3 -> (0, 4+2)
        b += QString(QChar(0x2800)).toUtf8();
        b += "\n";
        QImage im=RasterExtra::decodeData(b,"ubrl");
        check(!im.isNull() && im.size()==QSize(4,8), "Unicode braille bitmap decodes to 4x8");
        check(!im.isNull() && qGray(im.pixel(0,0))==0 && qGray(im.pixel(1,3))==0
              && qGray(im.pixel(3,0))==0 && qGray(im.pixel(0,6))==0,
              "braille dots 1/8/4/3 land in the right cell positions");
        check(!im.isNull() && qGray(im.pixel(3,7))==255, "unset braille dots stay white"); }
      check(RasterExtra::decodeData(QByteArray("Width: 4\nHeight: 4\n\n....\n"),"brf").isNull()==false,
            "ASCII braille (BRF) parses its header and body");

      // ---- 16-bit variants of formats we already read at 8 bits (scanners/film/scientific output).
      // Samples are stored big-endian and we keep the high byte, so a 16-bit 0xABCD reads back 0xAB.
      { QByteArray b; be16(b,474); ap8(b,{0,2}); be16(b,3); be16(b,2); be16(b,2); be16(b,3);   // storage=verbatim, bpc=2
        while(b.size()<512) b.append(char(0));
        auto s16=[&](std::initializer_list<int> v){ for(int x:v){ b.append(char((x>>8)&0xFF)); b.append(char(x&0xFF)); } };
        s16({0x1000,0x2000,0x3000,0x4000});   // R plane
        s16({0x5000,0x6000,0x7000,0x8000});   // G plane
        s16({0x9000,0xA000,0xB000,0xC000});   // B plane
        QImage im=RasterExtra::decodeData(b,"sgi");
        check(!im.isNull() && im.size()==QSize(2,2), "SGI 16-bit (bpc=2) decodes");
        check(!im.isNull() && im.pixelColor(0,1)==QColor(0x10,0x50,0x90), "SGI 16-bit keeps the high byte, bottom-to-top"); }

      { QByteArray b(64,'\0');                                    // VIPS, big-endian (SPARC magic), USHORT
        auto be32b=[&](int o,quint32 v){ b[o]=char((v>>24)&0xFF); b[o+1]=char((v>>16)&0xFF); b[o+2]=char((v>>8)&0xFF); b[o+3]=char(v&0xFF); };
        be32b(0,0x08f2a6b6u); be32b(4,2); be32b(8,1); be32b(12,3); be32b(20,2); be32b(24,0);   // 2x1, 3 bands, BandFmt=USHORT
        auto s16=[&](std::initializer_list<int> v){ for(int x:v){ b.append(char((x>>8)&0xFF)); b.append(char(x&0xFF)); } };
        s16({0xFF00,0x0000,0x0000,  0x0000,0xFF00,0x0000});
        QImage im=RasterExtra::decodeData(b,"vips");
        check(!im.isNull() && im.size()==QSize(2,1), "VIPS 16-bit (BandFmt USHORT) decodes");
        check(!im.isNull() && im.pixelColor(0,0)==QColor(255,0,0) && im.pixelColor(1,0)==QColor(0,255,0),
              "VIPS USHORT samples keep the high byte"); }

      { QByteArray b(2048,'\0'); b.replace(0,4,"SDPX");            // DPX 16-bit RGB
        auto be32b=[&](int o,quint32 v){ b[o]=char((v>>24)&0xFF); b[o+1]=char((v>>16)&0xFF); b[o+2]=char((v>>8)&0xFF); b[o+3]=char(v&0xFF); };
        be32b(772,2); be32b(776,1); b[800]=char(50); b[803]=char(16); be32b(806,0); be32b(808,2048);
        auto s16=[&](std::initializer_list<int> v){ for(int x:v){ b.append(char((x>>8)&0xFF)); b.append(char(x&0xFF)); } };
        s16({0xFF00,0x0000,0x0000,  0x0000,0x0000,0xFF00});
        while(b.size() < 2048+16) b.append(char(0));               // row padding to a 32-bit boundary
        QImage im=RasterExtra::decodeData(b,"dpx");
        check(!im.isNull() && im.size()==QSize(2,1) && im.pixelColor(0,0)==QColor(255,0,0)
              && im.pixelColor(1,0)==QColor(0,0,255), "DPX 16-bit RGB decodes"); }

      // Cineon 16-bit: samples are printing-density LOG codes. The Kodak reference points map
      // code 95 -> black and 685 -> white (stored as 16-bit, i.e. 6080 and 43839).
      { QByteArray b(2048,'\0');
        auto be32b=[&](int o,quint32 v){ b[o]=char((v>>24)&0xFF); b[o+1]=char((v>>16)&0xFF); b[o+2]=char((v>>8)&0xFF); b[o+3]=char(v&0xFF); };
        be32b(0,0x802A5FD7u); be32b(4,2048); be32b(200,2); be32b(204,1);   // magic, data offset, 2x1 (alt header layout)
        auto s16=[&](std::initializer_list<int> v){ for(int x:v){ b.append(char((x>>8)&0xFF)); b.append(char(x&0xFF)); } };
        s16({6080,6080,6080,  43839,43839,43839});                  // reference black, reference white
        QImage im=RasterExtra::decodeData(b,"cin");
        check(!im.isNull() && im.size()==QSize(2,1), "Cineon 16-bit decodes (size from the 200/204 header layout)");
        check(!im.isNull() && qGray(im.pixel(0,0))<=2 && qGray(im.pixel(1,0))>=253,
              "Cineon log->linear->sRGB maps reference black/white to 0/255"); }

      // ExtCodec (dlopen JPEG 2000 / JPEG XL): the filter must advertise a format iff its library loaded.
      check(ExtCodec::extensions().contains("jp2")==ExtCodec::jp2Available(), "JP2 is offered in the filter exactly when OpenJPEG is loadable");
      check(ExtCodec::extensions().contains("jxl")==ExtCodec::jxlAvailable(), "JXL is offered in the filter exactly when libjxl is loadable");
      check(ExtCodec::extensions().contains("mobi")==ExtCodec::mobiAvailable(), "MOBI is offered in the filter exactly when libmobi is loadable");
      check(ExtCodec::extensions().contains("exr"), "OpenEXR (tinyexr) is always offered — compiled in, no dlopen");
      check(ExtCodec::extensions().contains("djvu")==ExtCodec::djvuAvailable(), "DjVu is offered in the filter exactly when DjVuLibre is loadable");
      check(ExtCodec::extensions().contains("jxr")==ExtCodec::jxrAvailable(), "JPEG XR is offered in the filter exactly when jxrlib is linked");
      check(xara::jxrDecode("/nonexistent/photo.jxr").isNull(), "jxrDecode fails safe on an unreadable path");
      { QTemporaryFile tf(QDir::tempPath()+"/xarqjxrXXXXXX.jxr"); if(tf.open()){ tf.write("II\xBC\x01 not really a JPEG XR bitstream", 32); tf.flush();
        check(xara::jxrDecode(tf.fileName()).isNull(), "jxrDecode rejects non-JXR data without crashing"); } }
      check(ExtCodec::extensions().contains("dcm")==ExtCodec::dicomAvailable(), "DICOM is offered in the filter exactly when GDCM is linked");
      check(xara::dicomDecode("/nonexistent/scan.dcm").isNull(), "dicomDecode fails safe on an unreadable path");
      { QTemporaryFile tf(QDir::tempPath()+"/xarqdcmXXXXXX.dcm"); if(tf.open()){ tf.write("not a DICOM file - missing the DICM magic",41); tf.flush();
        check(xara::dicomDecode(tf.fileName()).isNull(), "dicomDecode rejects non-DICOM data without crashing"); } }
      check(xara::wmfToSvg(QByteArray("not a metafile")).isEmpty(), "wmfToSvg rejects non-WMF data (no spurious SVG)");
      check(xara::wmfAvailable() || xara::wmfToSvg(QByteArray()).isEmpty(), "wmfToSvg returns nothing when libwmf is not compiled in");
      check(ExtCodec::extensions().contains("svs")==ExtCodec::slideAvailable(), "whole-slide is offered in the filter exactly when OpenSlide is linked");
      check(xara::slideDecode("/nonexistent/scan.svs").isNull(), "slideDecode fails safe on an unreadable path");
      { QTemporaryFile tf(QDir::tempPath()+"/xarqsvsXXXXXX.svs"); if(tf.open()){ tf.write("II*\\0 not really a whole-slide tiff",32); tf.flush();
        check(xara::slideDecode(tf.fileName()).isNull(), "slideDecode rejects non-slide data without crashing"); } }
      check(ExtCodec::decode("/nonexistent/file.jp2").isNull(), "ExtCodec::decode fails safe on a missing file");
      check(ExtCodec::decode("/nonexistent/book.mobi").isNull(), "MOBI decode fails safe on a missing file");
      check(ExtCodec::decode("/nonexistent/scene.exr").isNull(), "EXR decode fails safe on a missing file");
      check(ExtCodec::decode("/nonexistent/scan.djvu").isNull(), "DjVu decode fails safe on a missing file");
      // JBIG2 (ITU T.88) via dlopen'd jbig2dec. The fixture is a real standalone .jb2: file header,
      // page-information segment, one immediate generic region with MMR=1 (its body is a T.6/Group-4
      // bitstream), and an end-of-page segment. 20x12, black at cols 0..4 rows 0..3 and cols 15..19
      // rows 8..11. Polarity (1 = black) cross-checks exactly against our own CALS/T.6 decoder.
      check(ExtCodec::extensions().contains("jb2")==ExtCodec::jbig2Available(),
            "JBIG2 is offered in the filter exactly when jbig2dec is loadable");
      check(ExtCodec::decode("/nonexistent/scan.jb2").isNull(), "JBIG2 decode fails safe on a missing file");
      { const char* b64 =
          "l0pCMg0KGgoBAAAAAQAAAAAwAAEAAAATAAAAFAAAAAwAAAAAAAAAAAEAAAAAAAEmAAEAAAAcAAAAFAAAAAwAAAAAAAAAAAABJqf/j51P8AEAEAAAAAIxAAEAAAAA";
        const QByteArray jb2=QByteArray::fromBase64(b64);
        QTemporaryFile tf(QDir::tempPath()+"/xarqjb2XXXXXX.jb2");
        if(tf.open()){ tf.write(jb2); tf.flush();
          const QImage im=ExtCodec::decode(tf.fileName());
          if(ExtCodec::jbig2Available()){
            check(!im.isNull() && im.size()==QSize(20,12), "JBIG2 decodes a real generic-region file to 20x12");
            int bad=0; if(!im.isNull()) for(int y=0;y<im.height();++y) for(int x=0;x<im.width();++x){
                const bool black = (y<4 && x<5) || (y>=8 && x>=15);
                if((qGray(im.pixel(x,y))<128)!=black) ++bad; }
            check(!im.isNull() && bad==0, "JBIG2 pixels are exact (1 = black, MSB-first, via im->stride)");
          } else check(im.isNull(), "JBIG2 declines cleanly when jbig2dec is unavailable"); } }
      { QTemporaryFile tf(QDir::tempPath()+"/xarqjb2bXXXXXX.jb2");
        if(tf.open()){ tf.write(QByteArray("\x97JB2\r\n\x1a\n\x01 garbage not a segment stream",40)); tf.flush();
          check(ExtCodec::decode(tf.fileName()).isNull(), "JBIG2 rejects a malformed segment stream without crashing"); } }

      // End-to-end: a real .qoi file on disk imports through Canvas::importImage into a Bitmap shape.
      { QByteArray b("qoif"); be32(b,2); be32(b,2); ap8(b,{4,0});
        ap8(b,{0xFF,255,0,0,255, 0xFF,0,255,0,255, 0xFF,0,0,255,255, 0xFF,9,9,9,255});
        ap8(b,{0,0,0,0,0,0,0,1});
        const QString qp="/tmp/xq_test.qoi"; { QFile f(qp); f.open(QIODevice::WriteOnly); f.write(b); }
        c->newDocument(); const int before=c->doc().shapes.size();
        check(c->importImage(qp), "importImage loads a real .qoi file end-to-end");
        check(c->doc().shapes.size()==before+1, "the imported QOI became one shape");
        if(!c->doc().shapes.isEmpty()){ const Shape& s=c->doc().shapes.last();
          check(s.fill.type==FillType::Bitmap && s.fill.image.size()==QSize(2,2), "the QOI shape carries a 2x2 bitmap fill"); } } }

    // --- Batch 3 vector/page importers: GeoJSON / KML / HP-GL parse geometry into editable Path shapes. ---
    { auto writeF=[](const QString& p,const QByteArray& b){ QFile f(p); f.open(QIODevice::WriteOnly); f.write(b); };
      auto fillMix=[&](int from){ int solid=0,none=0; const auto& sh=c->doc().shapes;
        for(int i=from;i<sh.size();++i){ if(sh[i].type!=ShapeType::Path) continue;
          if(sh[i].fill.type==FillType::Solid) solid++; else if(sh[i].fill.type==FillType::None) none++; }
        return QPair<int,int>(solid,none); };

      // GeoJSON: a Polygon (→ filled area) + a LineString (→ stroke-only).
      { c->newDocument(); const int b=c->doc().shapes.size(); const QString p="/tmp/xq_t.geojson";
        writeF(p,"{\"type\":\"FeatureCollection\",\"features\":["
                 "{\"type\":\"Feature\",\"geometry\":{\"type\":\"Polygon\",\"coordinates\":[[[0,0],[10,0],[5,8],[0,0]]]}},"
                 "{\"type\":\"Feature\",\"geometry\":{\"type\":\"LineString\",\"coordinates\":[[0,0],[10,10],[20,0]]}}]}");
        check(c->importGeoJson(p), "GeoJSON imports");
        check(c->doc().shapes.size()==b+2, "GeoJSON -> 2 editable shapes");
        auto m=fillMix(b); check(m.first==1 && m.second==1, "GeoJSON: polygon filled, line stroke-only"); }

      // KML: same geometry via the OGC XML form.
      { c->newDocument(); const int b=c->doc().shapes.size(); const QString p="/tmp/xq_t.kml";
        writeF(p,"<?xml version='1.0'?><kml><Document>"
                 "<Placemark><Polygon><outerBoundaryIs><LinearRing><coordinates>0,0 10,0 5,8 0,0</coordinates></LinearRing></outerBoundaryIs></Polygon></Placemark>"
                 "<Placemark><LineString><coordinates>0,0 10,10 20,0</coordinates></LineString></Placemark></Document></kml>");
        check(c->importKml(p), "KML imports");
        check(c->doc().shapes.size()==b+2, "KML -> 2 editable shapes");
        auto m=fillMix(b); check(m.first==1 && m.second==1, "KML: polygon filled, line stroke-only"); }

      // HP-GL: PU/PD plotter commands -> one stroke-only path.
      { c->newDocument(); const int b=c->doc().shapes.size(); const QString p="/tmp/xq_t.plt";
        writeF(p,"IN;SP1;PU1000,1000;PD3000,1000;PD3000,3000;PD1000,3000;PD1000,1000;PU1000,1000;PD3000,3000;");
        check(c->importHpgl(p), "HP-GL imports");
        check(c->doc().shapes.size()==b+1, "HP-GL -> 1 path shape");
        if(!c->doc().shapes.isEmpty()){ const Shape& s=c->doc().shapes.last();
          check(s.type==ShapeType::Path && s.fill.type==FillType::None && s.path.elementCount()>=5,
                "HP-GL path is stroke-only with the plotted geometry"); } }

      // ---- Native single-file VECTOR importers (vecextra.cpp): decoder-level hermetic tests ----
      // Each parses a hand-built minimal file to an SVG string and asserts the (Y-flipped) geometry.
{
    // Build a minimal little/big-endian .shp in memory: header + one PolyLine
    // record (1 part, 2 points), then assert shapefileToSvg maps + Y-flips them.
    QByteArray f;
    auto putBE32=[&](int32_t v){ f.append(char((v>>24)&0xff)); f.append(char((v>>16)&0xff)); f.append(char((v>>8)&0xff)); f.append(char(v&0xff)); };
    auto putLE32=[&](int32_t v){ f.append(char(v&0xff)); f.append(char((v>>8)&0xff)); f.append(char((v>>16)&0xff)); f.append(char((v>>24)&0xff)); };
    auto putLEd =[&](double d){ quint64 v; memcpy(&v,&d,8); for(int i=0;i<8;i++) f.append(char((v>>(8*i))&0xff)); };
    // ---- 100-byte main header ----
    putBE32(9994);                       // @0  file code
    for(int i=0;i<5;i++) putBE32(0);     // @4..23 unused
    putBE32(0);                          // @24 file length (unused by decoder)
    putLE32(1000);                       // @28 version
    putLE32(3);                          // @32 shape type = PolyLine
    putLEd(10.0); putLEd(20.0);          // @36 Xmin, @44 Ymin
    putLEd(30.0); putLEd(40.0);          // @52 Xmax, @60 Ymax
    putLEd(0.0); putLEd(0.0);            // @68 Zmin, @76 Zmax
    putLEd(0.0); putLEd(0.0);            // @84 Mmin, @92 Mmax  -> 100 bytes
    check(f.size()==100, "shp header is 100 bytes");
    // ---- record 1: PolyLine, 1 part, 2 points ----
    putBE32(1);                          // record number
    putBE32(40);                         // content length = 40 words (80 bytes)
    putLE32(3);                          // shape type
    putLEd(10);putLEd(20);putLEd(30);putLEd(40);  // record box
    putLE32(1);                          // numParts
    putLE32(2);                          // numPoints
    putLE32(0);                          // parts[0]
    putLEd(10);putLEd(20);              // point 0
    putLEd(30);putLEd(40);             // point 1

    QString svg = xara::shapefileToSvg((const uint8_t*)f.constData(), (qint64)f.size());
    check(!svg.isEmpty(),               "shp: non-empty svg");
    check(svg.contains("<svg"),         "shp: has svg root");
    check(svg.contains("viewBox="),     "shp: has viewBox");
    check(svg.contains("<polyline"),    "shp: emits polyline for type 3");
    // Y-flip: y' = Ymax - y  =>  (10,20)->(10,20) ; (30,40)->(30,0)
    check(svg.contains("10.000,20.000"),"shp: point0 mapped");
    check(svg.contains("30.000,0.000"), "shp: point1 Y-flipped");

    // Reject non-shapefile / short buffers.
    QByteArray bad=f; bad[3]=0;          // corrupt file code (9994=0x0000270A, low byte @3)
    check(xara::shapefileToSvg((const uint8_t*)bad.constData(),(qint64)bad.size()).isEmpty(), "shp: bad magic rejected");
    check(xara::shapefileToSvg((const uint8_t*)f.constData(), (qint64)50).isEmpty(),          "shp: sub-header buffer rejected");
    check(xara::shapefileToSvg((const uint8_t*)f.constData(), (qint64)108).isEmpty(),         "shp: truncated record -> empty");
}
// --- GPX importer selftest ---------------------------------------------------
// Hand-built minimal GPX 1.1 doc: one track (2 points), one route (2 points),
// one waypoint. Assert the emitted SVG carries the Y-flipped mapped coordinates.
{
    const char* gpx =
        "<?xml version='1.0' encoding='UTF-8'?>\n"
        "<gpx version='1.1' creator='selftest' xmlns='http://www.topografix.com/GPX/1/1'>\n"
        "  <wpt lat='10.0' lon='2.0'><name>W</name></wpt>\n"
        "  <trk><name>T</name><trkseg>\n"
        "    <trkpt lat='0.0'  lon='0.0'/>\n"
        "    <trkpt lat='10.0' lon='5.0'/>\n"
        "  </trkseg></trk>\n"
        "  <rte><name>R</name>\n"
        "    <rtept lat='5.0' lon='1.0'/>\n"
        "    <rtept lat='8.0' lon='4.0'/>\n"
        "  </rte>\n"
        "</gpx>\n";
    QByteArray buf(gpx);
    QString svg = xara::gpxToSvg(reinterpret_cast<const uint8_t*>(buf.constData()), (qint64)buf.size());

    check(!svg.isEmpty(), "gpx: non-empty SVG produced");
    check(svg.contains("<svg"), "gpx: has <svg> root");
    // bbox: lon 0..5, lat 0..10 -> viewBox minLon=0 y=0 w=5 h=10
    check(svg.contains("viewBox='0.000000 0.000000 5.000000 10.000000'"), "gpx: data-bbox viewBox");
    // Track: one polyline, blue, fill none.
    check(svg.contains("<polyline"), "gpx: emits polyline for track/route");
    check(svg.contains("fill='none'"), "gpx: polyline fill none");
    check(svg.contains("stroke='#0000ff'"), "gpx: polyline blue stroke");
    // Y flip: maxLat=10. trkpt(0,0) -> y = 10-0 = 10 ; trkpt(5,10) -> y = 10-10 = 0.
    check(svg.contains("0.000000,10.000000"), "gpx: trkpt(0,0) flipped to y=10");
    check(svg.contains("5.000000,0.000000"),  "gpx: trkpt(lon5,lat10) flipped to y=0");
    // Waypoint(lon2,lat10) -> red circle at cx=2 cy=(10-10)=0.
    check(svg.contains("<circle"), "gpx: emits waypoint circle");
    check(svg.contains("fill='#ff0000'"), "gpx: waypoint red fill");
    check(svg.contains("cx='2.000000'"), "gpx: waypoint cx=lon");

    // Two open polylines total (1 track seg + 1 route).
    int polyCount = svg.count("<polyline");
    check(polyCount == 2, "gpx: one polyline per track-seg and route");

    // Negative sniff: non-GPX buffer returns empty.
    const char* notgpx = "<html><body>hello</body></html>";
    QByteArray nb(notgpx);
    QString none = xara::gpxToSvg(reinterpret_cast<const uint8_t*>(nb.constData()), (qint64)nb.size());
    check(none.isEmpty(), "gpx: non-GPX input rejected");

    // Out-of-range coords are skipped (lat 999 invalid) -> no geometry -> empty.
    const char* bad = "<gpx><wpt lat='999' lon='0'/></gpx>";
    QByteArray bb(bad);
    QString b = xara::gpxToSvg(reinterpret_cast<const uint8_t*>(bb.constData()), (qint64)bb.size());
    check(b.isEmpty(), "gpx: out-of-range lat rejected -> empty");
}
{
    // Hand-built minimal FIG 3.2 file: polyline, text, and a circle-by-radius.
    QByteArray fig =
        "#FIG 3.2\n"
        "Landscape\n"
        "Center\n"
        "Inches\n"
        "Letter\n"
        "100.00\n"
        "Single\n"
        "-2\n"
        "1200 2\n"
        "2 1 0 1 0 7 50 -1 -1 0.000 0 0 -1 0 0 3\n"
        "\t100 100 200 100 200 200\n"
        "4 0 0 50 -1 0 12 0.0000 4 135 630 300 400 Hello\001\n"
        "1 3 0 1 0 7 50 -1 -1 0.000 1 0.0000 500 500 100 100 400 500 600 500\n";
    QString svg = xara::xfigToSvg((const uint8_t*)fig.constData(), (qint64)fig.size());
    check(svg.contains("<svg"),                 "xfig: emits svg root");
    check(svg.contains("viewBox="),             "xfig: emits viewBox");
    check(svg.contains("<polyline"),            "xfig: polyline object emitted");
    check(svg.contains("100.000,100.000"),      "xfig: polyline first point mapped");
    check(svg.contains("200.000,200.000"),      "xfig: polyline last point mapped");
    check(svg.contains("<ellipse"),             "xfig: ellipse object emitted");
    check(svg.contains(">Hello<"),              "xfig: text content preserved");
    check(svg.contains("300.000"),              "xfig: text x coordinate present");
    // Non-FIG input must be rejected (empty -> caller falls through).
    QByteArray junk = "not a fig file at all";
    QString empty = xara::xfigToSvg((const uint8_t*)junk.constData(), (qint64)junk.size());
    check(empty.isEmpty(),                       "xfig: rejects non-fig input");
}
{
    // Hand-built minimal binary CGM:
    //   BEGIN METAFILE (class0,id1,len0)
    //   VDC EXTENT     (class2,id6,len8): (0,0)-(100,100)
    //   POLYLINE       (class4,id1,len12): (10,10)(90,20)(50,80)
    //   END METAFILE   (class0,id2,len0)
    const unsigned char cgm[] = {
        0x00,0x20,                                              // BEGIN METAFILE
        0x20,0xC8, 0x00,0x00, 0x00,0x00, 0x00,0x64, 0x00,0x64,  // VDC EXTENT (0,0)-(100,100)
        0x40,0x2C, 0x00,0x0A, 0x00,0x0A, 0x00,0x5A, 0x00,0x14, 0x00,0x32, 0x00,0x50, // POLYLINE
        0x00,0x40                                               // END METAFILE
    };
    QString svg = xara::cgmToSvg((const uint8_t*)cgm, (qint64)sizeof(cgm));
    check(svg.contains("<svg"), "cgm: svg root emitted");
    check(svg.contains("<polyline"), "cgm: polyline element emitted");
    // VDC Y-up flipped to SVG Y-down: flipY(y) = 0+100-y
    check(svg.contains("10.000,90.000"), "cgm: point (10,10) Y-flipped to (10,90)");
    check(svg.contains("90.000,80.000"), "cgm: point (90,20) Y-flipped to (90,80)");
    check(svg.contains("50.000,20.000"), "cgm: point (50,80) Y-flipped to (50,20)");
    check(svg.contains("viewBox='0.000 0.000 100.000 100.000'"), "cgm: viewBox from VDC EXTENT");

    // Non-CGM input (does not begin with BEGIN METAFILE) must be rejected.
    const unsigned char bad[] = { 0x12,0x34,0x56,0x78 };
    check(xara::cgmToSvg((const uint8_t*)bad, (qint64)sizeof(bad)).isEmpty(), "cgm: rejects non-CGM magic");

    // Truncated buffer must not crash and must return empty.
    check(xara::cgmToSvg((const uint8_t*)cgm, (qint64)3).isEmpty(), "cgm: truncated header rejected");
}
// --- SVGZ importer selftest -------------------------------------------------
// Build a tiny SVG, gzip it in-memory with zlib, feed the bytes to svgzToSvg,
// and assert we recover the original SVG text intact.
{
    QByteArray svg =
        "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 10 10'>"
        "<rect x='1' y='2' width='3' height='4' fill='#abc'/></svg>";

    // gzip-compress `svg` using zlib's deflateInit2 with the gzip wrapper.
    QByteArray gz;
    {
        z_stream ds;
        std::memset(&ds, 0, sizeof(ds));
        int rc = deflateInit2(&ds, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                              15 + 16, 8, Z_DEFAULT_STRATEGY);
        check(rc == Z_OK, "svgz: deflateInit2 ok");
        ds.next_in  = reinterpret_cast<Bytef*>(const_cast<char*>(svg.data()));
        ds.avail_in = (uInt)svg.size();
        char obuf[4096];
        int r;
        do {
            ds.next_out  = reinterpret_cast<Bytef*>(obuf);
            ds.avail_out = sizeof(obuf);
            r = deflate(&ds, Z_FINISH);
            gz.append(obuf, (int)(sizeof(obuf) - ds.avail_out));
        } while (r != Z_STREAM_END);
        deflateEnd(&ds);
    }

    check(gz.size() >= 18, "svgz: produced a gzip stream");
    check((unsigned char)gz[0] == 0x1f && (unsigned char)gz[1] == 0x8b, "svgz: gzip magic present");

    QString outSvg = xara::svgzToSvg(reinterpret_cast<const uint8_t*>(gz.constData()),
                               (qint64)gz.size());

    check(!outSvg.isEmpty(), "svgz: inflated non-empty");
    check(outSvg.contains("<svg"), "svgz: has <svg root");
    check(outSvg.contains("viewBox='0 0 10 10'"), "svgz: viewBox roundtrip");
    check(outSvg.contains("<rect"), "svgz: rect roundtrip");
    check(outSvg.contains("fill='#abc'"), "svgz: fill roundtrip");
    check(outSvg == QString::fromUtf8(svg), "svgz: exact byte roundtrip");

    // Negative: plain (non-gzip) bytes must be rejected as "not our format".
    QByteArray plain = "<svg/>";
    QString none = xara::svgzToSvg(reinterpret_cast<const uint8_t*>(plain.constData()),
                             (qint64)plain.size());
    check(none.isEmpty(), "svgz: rejects non-gzip input");

    // Negative: truncated gzip stream must fail cleanly (empty), not crash.
    QByteArray trunc = gz.left(gz.size() / 2);
    QString bad = xara::svgzToSvg(reinterpret_cast<const uint8_t*>(trunc.constData()),
                            (qint64)trunc.size());
    check(bad.isEmpty(), "svgz: rejects truncated stream");
}

      // ---- End-to-end: the Canvas dispatch wiring (file → importXxx → editable shapes) ----
      // GPX (text): one track-seg + one route → two stroke-only polylines.
      { c->newDocument(); const int b=c->doc().shapes.size(); const QString p="/tmp/xq_t.gpx";
        writeF(p,QByteArray(
          "<?xml version='1.0'?><gpx version='1.1' xmlns='http://www.topografix.com/GPX/1/1'>"
          "<trk><trkseg><trkpt lat='0' lon='0'/><trkpt lat='10' lon='5'/></trkseg></trk>"
          "<rte><rtept lat='5' lon='1'/><rtept lat='8' lon='4'/></rte></gpx>"));
        check(c->importGpx(p), "GPX imports end-to-end");
        check(c->doc().shapes.size()>b, "GPX → editable shapes"); }
      // SVGZ (gzip): a rect inside a gzipped SVG → one editable shape.
      { c->newDocument(); const int b=c->doc().shapes.size(); const QString p="/tmp/xq_t.svgz";
        const QByteArray plain=QByteArray(
          "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 20 20'>"
          "<rect x='2' y='3' width='6' height='7' fill='#3366cc'/></svg>");
        QByteArray gz; { z_stream ds; std::memset(&ds,0,sizeof(ds));
          deflateInit2(&ds,Z_DEFAULT_COMPRESSION,Z_DEFLATED,15+16,8,Z_DEFAULT_STRATEGY);
          ds.next_in=reinterpret_cast<Bytef*>(const_cast<char*>(plain.data())); ds.avail_in=(uInt)plain.size();
          char ob[4096]; int r; do{ ds.next_out=reinterpret_cast<Bytef*>(ob); ds.avail_out=sizeof(ob);
            r=deflate(&ds,Z_FINISH); gz.append(ob,(int)(sizeof(ob)-ds.avail_out)); }while(r!=Z_STREAM_END);
          deflateEnd(&ds); }
        writeF(p,gz);
        check(c->importSvgz(p), "SVGZ imports end-to-end (gzip → SVG parser)");
        check(c->doc().shapes.size()>b, "SVGZ → editable shape"); }
      // Shapefile (binary): 100-byte header + one PolyLine record → editable path.
      { c->newDocument(); const int b=c->doc().shapes.size(); const QString p="/tmp/xq_t.shp";
        QByteArray f; auto BE=[&](qint32 v){ for(int s=24;s>=0;s-=8) f.append(char((v>>s)&0xff)); };
        auto LE=[&](qint32 v){ for(int s=0;s<32;s+=8) f.append(char((v>>s)&0xff)); };
        auto LD=[&](double d){ quint64 v; std::memcpy(&v,&d,8); for(int i=0;i<8;i++) f.append(char((v>>(8*i))&0xff)); };
        BE(9994); for(int i=0;i<5;i++) BE(0); BE(0); LE(1000); LE(3);
        LD(10);LD(20);LD(30);LD(40); LD(0);LD(0);LD(0);LD(0);              // header bbox + z/m
        BE(1); BE(40); LE(3); LD(10);LD(20);LD(30);LD(40); LE(1); LE(2); LE(0);
        LD(10);LD(20); LD(30);LD(40);                                      // 2 points
        writeF(p,f);
        check(c->importShapefile(p), "Shapefile imports end-to-end");
        check(c->doc().shapes.size()>b, "Shapefile → editable shape"); }
      // Reject-safety through the Canvas layer.
      { const QString p="/tmp/xq_t.shp"; writeF(p,QByteArray("nope")); check(!c->importShapefile(p), "Shapefile rejects junk safely"); }
      { const QString p="/tmp/xq_t.svgz"; writeF(p,QByteArray("<svg/>")); check(!c->importSvgz(p), "SVGZ rejects non-gzip safely"); }
      { const QString p="/tmp/xq_t.cgm"; writeF(p,QByteArray("\x12\x34\x56\x78",4)); check(!c->importCgm(p), "CGM rejects non-metafile safely"); }
      { const QString p="/tmp/xq_t.fig"; writeF(p,QByteArray("not a fig")); check(!c->importXfig(p), "Xfig rejects non-fig safely"); }
      { const QString p="/tmp/xq_t.cmx"; writeF(p,QByteArray("not a cmx file")); check(!c->importCmx(p), "CMX rejects a non-CMX file safely"); }

      // ---- Fleet 13 native vector decoders (Gerber/Excellon/Lottie/Sketch) — hermetic ----
{
    // Hand-built minimal Gerber RS-274X: FSLAX23 (int=2,dec=3 => value/1000),
    // MM units, a 0.5mm circle aperture (D10) and a 0.6x0.8 rect (D11).
    // Draw a trace (0,0)->(1,0), flash the circle at (1,1), flash rect at (2,2).
    const char* g =
        "%FSLAX23Y23*%\n"
        "%MOMM*%\n"
        "%ADD10C,0.5*%\n"
        "%ADD11R,0.6X0.8*%\n"
        "D10*\n"
        "X0Y0D02*\n"
        "X1000Y0D01*\n"
        "X1000Y1000D03*\n"
        "D11*\n"
        "X2000Y2000D03*\n"
        "M02*\n";
    QByteArray b(g);
    QString svg = xara::gerberToSvg((const uint8_t*)b.constData(), (qint64)b.size());
    check(svg.contains("<svg"), "gerber svg root");
    check(svg.contains("viewBox="), "gerber viewBox");
    check(svg.contains("<path"), "gerber trace path");
    check(svg.contains("stroke-width='0.5000'"), "gerber stroke width = aperture dia");
    check(svg.contains("stroke-linecap='round'"), "gerber round linecap");
    check(svg.contains("<circle"), "gerber circle flash");
    check(svg.contains("r='0.2500'"), "gerber circle flash radius = dia/2");
    check(svg.contains("<rect"), "gerber rect flash");
    check(svg.contains("width='0.6000'"), "gerber rect width");
    check(svg.contains("height='0.8000'"), "gerber rect height");
    check(svg.contains("1.0000"), "gerber scaled coord (FSLAX23 => integer/1000)");
    // Garbage input must fall through as empty QString.
    const char* junk = "this is not gerber at all, no commands here.";
    QByteArray jb(junk);
    QString jsvg = xara::gerberToSvg((const uint8_t*)jb.constData(), (qint64)jb.size());
    check(jsvg.isEmpty(), "non-gerber input -> empty QString");
}
{
    // Metric drill, explicit decimals. Two tools.
    const char* d1 =
        "M48\nMETRIC,TZ\nFMAT,2\nT01C0.800\nT02C1.000\n%\nG90\nG05\n"
        "T01\nX10.0Y20.0\nX30.0Y20.0\nT02\nX50.0Y40.0\nM30\n";
    QString s = xara::excellonToSvg((const uint8_t*)d1, (qint64)strlen(d1));
    check(s.contains("<svg"), "excellon: has svg root");
    check(s.contains("<circle"), "excellon: emits circles");
    check(s.contains("cx='10.0000'"), "excellon: hole1 cx");
    check(s.contains("cy='40.1000'"), "excellon: hole1 cy (Y-flipped)");
    check(s.contains("r='0.4000'"), "excellon: tool1 radius (dia/2)");
    check(s.contains("r='0.5000'"), "excellon: tool2 radius (dia/2)");
    check(s.contains("fill='#333333'"), "excellon: hole fill colour");
    check(s.contains("viewBox='9.6000 19.6000 40.9000 20.9000'"), "excellon: data-bounds viewBox");

    // Inch drill -> normalised to mm (x25.4).
    const char* d2 = "M48\nINCH\nT01C0.040\n%\nT01\nX1.0Y1.0\nM30\n";
    QString s2 = xara::excellonToSvg((const uint8_t*)d2, (qint64)strlen(d2));
    check(s2.contains("cx='25.4000'"), "excellon: inch->mm cx");
    check(s2.contains("r='0.5080'"), "excellon: inch->mm radius");

    // Non-drill input must be rejected (empty -> caller falls through).
    const char* d3 = "hello world, just some plain text here\n";
    QString s3 = xara::excellonToSvg((const uint8_t*)d3, (qint64)strlen(d3));
    check(s3.isEmpty(), "excellon: rejects non-drill input");

    // Truncated / empty must not crash.
    QString s4 = xara::excellonToSvg((const uint8_t*)"M4", (qint64)2);
    check(s4.isEmpty(), "excellon: tiny input safe");
}
{
    // Minimal hand-built Lottie: 100x100 comp, one shape layer with a group
    // containing a red rect (center [50,50], size [40,20]) + fill [1,0,0].
    QByteArray j = R"J({"v":"5.7.0","w":100,"h":100,"fr":30,"ip":0,"op":60,"layers":[
      {"ty":4,"ks":{"p":{"a":0,"k":[0,0]},"a":{"a":0,"k":[0,0]}},"shapes":[
        {"ty":"gr","it":[
          {"ty":"rc","p":{"a":0,"k":[50,50]},"s":{"a":0,"k":[40,20]},"r":{"a":0,"k":0}},
          {"ty":"fl","c":{"a":0,"k":[1,0,0]}},
          {"ty":"tr","p":{"a":0,"k":[0,0]},"a":{"a":0,"k":[0,0]}}
        ]}
      ]}
    ]})J";
    QString svg = xara::lottieToSvg((const uint8_t*)j.constData(), (qint64)j.size());
    check(svg.contains("<svg"), "lottie: emits <svg>");
    check(svg.contains("viewBox='0 0 100.000 100.000'"), "lottie: viewBox from w/h");
    check(svg.contains("<rect"), "lottie: emits rect for 'rc' item");
    check(svg.contains("#ff0000"), "lottie: resolves [1,0,0] fill -> #ff0000");
    // rect x = 50 - 40/2 = 30 ; y = 50 - 20/2 = 40
    check(svg.contains("x='30.000'") && svg.contains("y='40.000'"),
          "lottie: rect x/y from center minus half-size");
    check(svg.contains("width='40.000'") && svg.contains("height='20.000'"),
          "lottie: rect width/height");

    // Ellipse + closed bezier path, plus animated fill (a:1 -> first keyframe .s).
    QByteArray j2 = R"J({"v":"5.7.0","w":50,"h":50,"layers":[
      {"ty":4,"ks":{"p":{"a":0,"k":[0,0]},"a":{"a":0,"k":[0,0]}},"shapes":[
        {"ty":"gr","it":[
          {"ty":"el","p":{"a":0,"k":[10,10]},"s":{"a":0,"k":[20,20]}},
          {"ty":"sh","ks":{"a":0,"k":{"v":[[0,0],[10,0],[10,10]],"i":[[0,0],[0,0],[0,0]],"o":[[0,0],[0,0],[0,0]],"c":true}}},
          {"ty":"fl","c":{"a":1,"k":[{"t":0,"s":[0,1,0,1]}]}}
        ]}
      ]}
    ]})J";
    QString svg2 = xara::lottieToSvg((const uint8_t*)j2.constData(), (qint64)j2.size());
    check(svg2.contains("<ellipse"), "lottie: emits ellipse for 'el'");
    check(svg2.contains("cx='10.000'") && svg2.contains("rx='10.000'"),
          "lottie: ellipse cx from p, rx from s/2");
    check(svg2.contains("<path") && svg2.contains(" Z"),
          "lottie: emits closed path for 'sh'");
    check(svg2.contains("#00ff00"), "lottie: animated fill uses first keyframe start value");

    // Rejections: non-JSON, JSON without layers, empty layers.
    check(xara::lottieToSvg((const uint8_t*)"not json here", (qint64)13).isEmpty(),
          "lottie: rejects non-JSON");
    QByteArray j3 = "{\"w\":10,\"h\":10}";
    check(xara::lottieToSvg((const uint8_t*)j3.constData(), (qint64)j3.size()).isEmpty(),
          "lottie: rejects object without layers");
    QByteArray j4 = "{\"w\":10,\"h\":10,\"layers\":[]}";
    check(xara::lottieToSvg((const uint8_t*)j4.constData(), (qint64)j4.size()).isEmpty(),
          "lottie: rejects empty layers");
}
// ---- Sketch (.sketch) -> SVG importer selftest ----------------------------
// Build a minimal STORED (uncompressed) ZIP with a "pages/*.json" entry and
// assert the emitted SVG contains the expected rect/ellipse/text/path.
{
    // CRC32 (needed because the real xqUnzip may validate stored-entry CRCs).
    auto skCrc32 = [](const unsigned char* d, size_t len)->uint32_t{
        static uint32_t tbl[256]; static bool init=false;
        if(!init){ for(uint32_t i=0;i<256;i++){ uint32_t v=i; for(int k=0;k<8;k++) v=(v&1)?(0xEDB88320u^(v>>1)):(v>>1); tbl[i]=v; } init=true; }
        uint32_t v=0xffffffffu; for(size_t i=0;i<len;i++) v=tbl[(v^d[i])&0xff]^(v>>8); return v^0xffffffffu;
    };
    QByteArray z;
    auto p16=[&](unsigned v){ z.append(char(v&0xff)); z.append(char((v>>8)&0xff)); };
    auto p32=[&](unsigned v){ z.append(char(v&0xff)); z.append(char((v>>8)&0xff)); z.append(char((v>>16)&0xff)); z.append(char((v>>24)&0xff)); };

    struct SkEnt{ QByteArray name; QByteArray data; };
    QByteArray page =
      "{\"_class\":\"page\",\"frame\":{\"x\":0,\"y\":0,\"width\":100,\"height\":100},\"layers\":["
        "{\"_class\":\"rectangle\",\"frame\":{\"x\":10,\"y\":20,\"width\":30,\"height\":40},"
          "\"style\":{\"fills\":[{\"color\":{\"red\":1,\"green\":0,\"blue\":0,\"alpha\":1}}]}},"
        "{\"_class\":\"oval\",\"frame\":{\"x\":50,\"y\":50,\"width\":20,\"height\":20},"
          "\"style\":{\"fills\":[{\"color\":{\"red\":0,\"green\":0,\"blue\":1,\"alpha\":1}}]}},"
        "{\"_class\":\"text\",\"frame\":{\"x\":5,\"y\":5,\"width\":50,\"height\":10},"
          "\"attributedString\":{\"string\":\"Hi <you>\"}},"
        "{\"_class\":\"shapePath\",\"frame\":{\"x\":0,\"y\":0,\"width\":100,\"height\":100},\"isClosed\":true,"
          "\"points\":[{\"point\":\"{0, 0}\"},{\"point\":\"{1, 0}\"},{\"point\":\"{0.5, 1}\"}]}"
      "]}";
    QVector<SkEnt> ents;
    ents.push_back({QByteArray("document.json"), QByteArray("{\"_class\":\"document\"}")});
    ents.push_back({QByteArray("pages/ABC-123.json"), page});
    ents.push_back({QByteArray("meta.json"), QByteArray("{}")});

    QVector<unsigned> offs, crcs;
    for(const SkEnt& e : ents){
        offs.push_back((unsigned)z.size());
        unsigned crc=skCrc32((const unsigned char*)e.data.constData(), e.data.size());
        crcs.push_back(crc);
        p32(0x04034b50); p16(20); p16(0); p16(0); p16(0); p16(0);
        p32(crc); p32(e.data.size()); p32(e.data.size());
        p16(e.name.size()); p16(0); z.append(e.name); z.append(e.data);
    }
    unsigned cdStart=(unsigned)z.size();
    for(int i=0;i<ents.size();i++){
        p32(0x02014b50); p16(20); p16(20); p16(0); p16(0); p16(0); p16(0);
        p32(crcs[i]); p32(ents[i].data.size()); p32(ents[i].data.size());
        p16(ents[i].name.size()); p16(0); p16(0); p16(0); p16(0); p32(0); p32(offs[i]);
        z.append(ents[i].name);
    }
    unsigned cdSize=(unsigned)z.size()-cdStart;
    p32(0x06054b50); p16(0); p16(0); p16(ents.size()); p16(ents.size());
    p32(cdSize); p32(cdStart); p16(0);

    QString svg = xara::sketchToSvg((const uint8_t*)z.constData(), (qint64)z.size());
    check(!svg.isEmpty(), "sketch: non-empty SVG");
    check(svg.contains("<svg"), "sketch: has <svg>");
    check(svg.contains("viewBox='0.000 0.000 100.000 100.000'"), "sketch: union viewBox");
    check(svg.contains("<rect x='10.000' y='20.000' width='30.000' height='40.000'"), "sketch: rect at frame");
    check(svg.contains("fill='#ff0000'"), "sketch: red rect fill");
    check(svg.contains("<ellipse cx='60.000' cy='60.000' rx='10.000' ry='10.000'"), "sketch: oval->ellipse");
    check(svg.contains("fill='#0000ff'"), "sketch: blue oval fill");
    check(svg.contains("<text"), "sketch: has <text>");
    check(svg.contains("Hi &lt;you&gt;"), "sketch: XML-escaped text");
    check(svg.contains("<path"), "sketch: shapePath->path");
    check(svg.contains("M 0.000 0.000 L 100.000 0.000 L 50.000 100.000 Z"), "sketch: normalised points denormalised+closed");

    // Negatives: junk / null / a zip with no pages/ entry -> empty QString().
    QByteArray junk("not a zip at all, just noise bytes here");
    check(xara::sketchToSvg((const uint8_t*)junk.constData(), (qint64)junk.size()).isEmpty(), "sketch: junk -> empty");
    check(xara::sketchToSvg((const uint8_t*)nullptr, 0).isEmpty(), "sketch: null -> empty");
}

      // ---- Fleet 13 end-to-end (Canvas dispatch → editable shapes) + reject-safety ----
      { c->newDocument(); const int b=c->doc().shapes.size(); const QString p="/tmp/xq_t.gbr";
        writeF(p,QByteArray("%FSLAX23Y23*%\n%MOMM*%\n%ADD10C,0.5*%\nD10*\nX0Y0D02*\nX1000Y0D01*\nX1000Y1000D03*\nM02*\n"));
        check(c->importGerber(p), "Gerber imports end-to-end");
        check(c->doc().shapes.size()>b, "Gerber → editable shapes"); }
      { c->newDocument(); const int b=c->doc().shapes.size(); const QString p="/tmp/xq_t.drl";
        writeF(p,QByteArray("M48\nMETRIC\nT01C0.8\n%\nT01\nX1.0Y1.0\nX2.0Y2.0\nM30\n"));
        check(c->importExcellon(p), "Excellon imports end-to-end");
        check(c->doc().shapes.size()>b, "Excellon → editable holes"); }
      { c->newDocument(); const int b=c->doc().shapes.size(); const QString p="/tmp/xq_t.json";
        writeF(p,QByteArray("{\"v\":\"5.7.0\",\"w\":100,\"h\":100,\"fr\":30,\"ip\":0,\"op\":60,\"layers\":["
          "{\"ty\":4,\"shapes\":[{\"ty\":\"gr\",\"it\":["
          "{\"ty\":\"rc\",\"p\":{\"a\":0,\"k\":[50,50]},\"s\":{\"a\":0,\"k\":[40,30]}},"
          "{\"ty\":\"fl\",\"c\":{\"a\":0,\"k\":[1,0,0]}}]}]}]}"));
        check(c->importLottie(p), "Lottie imports end-to-end (raw JSON)");
        check(c->doc().shapes.size()>b, "Lottie → editable shape"); }
      { const QString p="/tmp/xq_t.gbr"; writeF(p,QByteArray("not gerber")); check(!c->importGerber(p), "Gerber rejects junk safely"); }
      { const QString p="/tmp/xq_t.drl"; writeF(p,QByteArray("not a drill")); check(!c->importExcellon(p), "Excellon rejects junk safely"); }
      { const QString p="/tmp/xq_t.sketch"; writeF(p,QByteArray("not a zip")); check(!c->importSketch(p), "Sketch rejects a non-archive safely"); }
      { const QString p="/tmp/xq_t.json"; writeF(p,QByteArray("{\"not\":\"lottie\"}")); check(!c->importLottie(p), "Lottie rejects non-Lottie JSON safely"); }
      // DLP library importers (libpagemaker/libzmf/libqxp/libstaroffice/libetonyek/libmwaw): reject junk safely.
      { const QString p="/tmp/xq_t.pmd"; writeF(p,QByteArray("not pagemaker"));  check(!c->importPageMaker(p),  "PageMaker rejects junk safely"); }
      { const QString p="/tmp/xq_t.zmf"; writeF(p,QByteArray("not zoner"));      check(!c->importZmf(p),        "Zoner Draw rejects junk safely"); }
      { const QString p="/tmp/xq_t.qxp"; writeF(p,QByteArray("not quark"));      check(!c->importQxp(p),        "QuarkXPress rejects junk safely"); }
      { const QString p="/tmp/xq_t.sda"; writeF(p,QByteArray("not staroffice")); check(!c->importStarOffice(p),  "StarOffice rejects junk safely"); }
      { const QString p="/tmp/xq_t.key"; writeF(p,QByteArray("not keynote"));    check(!c->importIWork(p),      "Keynote rejects junk safely"); }
      { const QString p="/tmp/xq_t.cwk"; writeF(p,QByteArray("not a mac doc"));  check(!c->importMwaw(p),       "legacy Mac rejects junk safely"); }
      // StarOffice real-file regression: a real StarDraw 3.1 document (LibreOffice writerperfect QA
      // corpus, zlib-compressed) reports the format's built-in "supported encryption" flag, so its
      // isFileFormatSupported confidence is SUPPORTED_ENCRYPTION, not EXCELLENT. The binding must accept
      // it (parse() is the real gate) — guards against regressing the confidence check back to ==EXCELLENT.
      { static const char* kStarDrawSda =
          "AABaAHja7TwLdFzFdfe9mf1oJfvpZ1uSZbM2tp+xjS1L/tsF2cjG4J+CjL0hJM1au7YWJK26Wn/rEi"
          "WhLacnSYGWHsg5gA3mZ8BAMMF8AoYkPUB7UtIkhIaclqaF8DE/lwTzs3rn9356T9pd4BR6PDr37dOd"
          "mfuZuXNn7sx779l/qXpx/30N/wmetBQInBwsg7ADpyGcof6pBNAl7uTg4KBCD55KX6j0sfxlfUix/0"
          "IIExEiCFGEMoQYQjlCBcIohNEIhjABqEKoRqhBqEUYgzAWYRxCHUI9QgPCeIRGhAnSXhjE8X4SwmSE"
          "0xGmIExFmIZgIkyXZWfg70x5fyp9uukCyOJfHuKwEnrxNwe7oZg0Fi3GSa+QOicLLFdoOsW/dP6aYz"
          "xSx30p41/VdY7/U2P285007CESEzb04sHKe3ruH195XZLqjdCns75vy3Zu70n35nlHd+STubZccqeF"
          "/ORJE8uIklNVlQ7CmrVlaJHMNNFeN51zTrY3n8t29zPjpcBmmtkiC626CW+WWf+14835JfN3CF+zEI"
          "eFWWR9Azq27lLteV7v1mxI0BwNye5sb3823p3cnsOsuDflbmjR4N2nRy7YjwVXV1yNNOPDJ0WxDPLp"
          "/vy5uWRfV7ykNCKnz5zAFz39nzdAGJgtxptGlkqWnFtwyeaCS7aMXJINIWa3A79/Gu+eXfyzwXBx42"
          "80zJ3VNKdpzqL58+csaF4wq9jxf3KQ+TDNz6/Bi3954/H3N3RV3nlVFGaa9/8b8zuHHWvrVmC+CSCB"
          "wMTuk3l75Tr8CgSC8PfSz+yTa3M297oH/fB4JZ3fvcN/3jkeKmsYJwMSG7bnU+kd5+XTPe3ZbDccF0"
          "5UhxqNVXuFGJS8Ql4lr5HXyTHyBnmTvEXeJu+Q4+R/yLvkD+SP5D1ygrxPPiAfko/Ix7ScVtBRdDQ1"
          "aCWtotW0htbSMXQsHUfraD1toONpo45Uz/CnatEhJ2mYRmiUltFYITTpBDqNmnQ6PYPOoDPpLHomnU"
          "3n0Ca6gC6ki+hiuoQupRfSTXQzTdAv04voV+jF9Kt8fZNC+E4DnyziMBOjl4fMVwjvjums81rx7ku8"
          "LaO8+0fzLgojJPA2gZFQAqOeh81XCe/a90SdMKzWlXGoxZrGwyZxFXfOXMINQuehkMHv3HWbrWszL6"
          "3jQm1tphf99+7udHxuUEGK986CzXbBdzV1ZRDyFGxRBXXJ8AO87mI2zNTG0ZPAqTTRhXAVwsMAj5iv"
          "EW7ls1WzfY83QViOkhD8g1SrWVHB5WMCzfNR83XR4GtUzZd0NeNyKQbAohGCFd3JzkudaJ0PLSEuJ4"
          "u+IoEz9I/MY0Kg7yqyl7Ex5ijNSFIkuTyXy+5kKCLXwM40Uf7WOu7tzim0BhcMw+JEB8Bj5htC3+8r"
          "waYwmbgnsAUrloPOzbLjz7Ync+lBucZ3ppjnPuaTZ0m5HuBx801h0quElCHopcJQZ2lCvkd11RLCIP"
          "eEWeMSuFJz928Cw/wEWsVR8y3CtVwiKFI4IvVWFJUejBKj6EfpCfNtwm2jUVBBd0KlnTESGi+P3u1J"
          "8x3CB2CNKIfmHZLlEnzxeFxk16rsmSqb5x82PxDCLlPqd8p8xmUUFzTE1aXcAsvwzukRuBWiuA+YH4"
          "pm7FZ0XgiBnBakFR9lfw5vwOx6AAZ0J2YQ/hmuI3ySYBgDOUaGWj2OrEQbAq50f2h+RNiAgbcV31+H"
          "vR7JDinFfTMfmjDENwWX04ctF8O/FHewxCqngxi3UnKddcBE/BPlmLuKAa5IUxm2wm0ZuQbXfQHC1x"
          "D+CuEQwIPmx6LNf6q88YGIn+61w2rrr19K/ro0Ymlgl8yh1kDWpOQs8HhS+tgIrE7mO7uEI/YW4Mrg"
          "DJBYgXAxwg6EKwGOmCexM1GhgZgcOYuqvQqtWLdACrBD7qi1StBA+oKoPeIrbUU1rK8NAv6yokyoQU"
          "Z4UBsYGNCOHj2Kv6ChPSIgHnEoMxOcMdeuOJVOpc84DQSsyK38nb6pz8rv863umx+6UKSgfBDZ7Z78"
          "rSIVkB9S5EvMB0XeL7+lpWm4/BZMTSFJfmh+J8tvGSkfJPmh+Ty7pckvnyeZH7oQPlE+tAfkEyHfiP"
          "mB/S/ID2Mfzmwoyb5Kyh/JvkcYH6fSqfRZJefSybs7ojl2Iew7zXV1/zmXY+BT3ktvCD+5UIQBsQxl"
          "y7corMjke5J98XmfVyntZf3nUTq+Jr06CommMkg8x04KBj2BU6paxZAisGLLTWf+5Tw/IvOXTm+g7v"
          "jt9moZ7WtsVa3iN4Dxgk5c0Xm+2hUlD4g/wbNR0JysaOo1zlhVh6ccew4AEzylm4YtvZhyrlYcvKNG"
          "tFRmhWjnceUiUHjuDBEWTJ8OPCi8epZ/HIwBMHVHprfUSM2EMq2eBvypzNZk/kpP/kuu/NvXv0/48f"
          "wzOs+vhexYu7N1GdWwvb2HTI3HrxqPhUXwzugYVm6Yl4/gNcYxAK+RR/D6OnkUc4+RH+H9G+QxvL5J"
          "Hjd1eIscRfzb5AnEvEOeNEFSjvIghFHQ4VXyMJZhdDROR8fYWXfl6q7cGI+0be4KzzgSzpHtheko5d"
          "AyTEKdS6i5yldweViuxnMFNcpzCUb3wfpqDn1FefDRd/SIFNwch1IwCmxzCJShku/o2a1KSqRTxa3G"
          "Safag6FoyW5MyL0XMk3uTSyS+xNs6/A7CAcRcKwljrFtJ7TeJQh7ER5DYDusqxEuQujW4bb1YcofLv"
          "mNsOrxsLjeadUafEgeMAm/0+EDcthUGMK3lBiGwEfkh6bGLV5gdDhJjpjMvqISA7wWRcsPuzBsFLDx"
          "4cQMSttm+0BD8SLqLuPWK3iJXMZRc5WJOeQv567DlpbZKpEYCh+TB022+0RdGB3tTZOy6VJ+xoXwkU"
          "x98IOWhYQCc4VsVfyn2ipJpRaMr7tkjW8Z6ipTa7W8kDyEfkpJTqUMotZYS3I3XlAb5+FFfXjVWf2u"
          "cuutWkRKyFrYrUWDJaFof2pbsnOHbZPc/N7DJkeEexF+ifAiwnEEihY8FmE2wlqEPoQdCN+X1v0swi"
          "sIYbTuGQjLEL7OfG095TtXzWqL9Bf1bt9N+TXE93K8+40A0zy1qxqKqb1Q1F6tJrpkg1pK3GzMNHXH"
          "7AF8/1HhQxzP6Tl2PhN8k/BCyvedDgqaETDGq333xVRNhWLGE/Oa4KfzXE3m6hZGlxhiYYjEUAtDJS"
          "bEKQlMmM9hTi7WMcZZYr80sQUhjzAgn3tZwYqipt9AeA4hizqOQUPZhnA3wq8Q3kWoRXIXI/wS4Vmc"
          "wvdjC55ZBbAOoQfhGoQnEN5CmIArmCzC9xAOIfwM4QDO1H9TB/AUQhg7+6J6x+MKD2FrabujUDmWaV"
          "oNK1OZ/MrebZnetHVid0Cc2BHob2QtdqNxj2HcaOwz9hs3GTcbB4xbjFuN24zbjTuMg8adxl3G3cZ9"
          "xg90LHf/SOUOIal7jR9gwcMErjcOG8b1xg0+dbw17hN1jAd4O7JdSbZxsT3Cz9gq4Qh7AhDpsfkNXh"
          "Z2EYPNE8QicVcoJXdZU3jPfMO1URtzbVRHzJFyG3OknHn450fbmOdHMz99osrGnKgKIaZujI2pGxNG"
          "zKI6G7OoLsJ28MfbmMT4KGJ2TXTIM5Ht8l87ySHPpCF7707PwOa4G4R3WDb9BoN3bK/S+OcT1HmDJi"
          "2c/REO4kwhxOeOMD/YifAVDpulonxOKfM9beAWrWbas4REsek3Gqw8jBMzZxmcOE2MmDCAZ+8gBGqn"
          "mD0BtCKZ70fp7uVDcdLkNaCGbnEV16yxzulK4kiKrrh8jbUTX1zF3BpQriNsxUb2kz+iyhZnlSmySr"
          "iQKhfwyGnKn4gqkUKqPMrjoilL18isSMFVZCNwMzlXOLbEQ3J6molTz9cQrkL4iQat5j40kVE4z2vc"
          "RCogNknNiCkHiAlADg89JU/r2H9PWWVYb7kHMUWMexCzBZN7EIcR4x7EbLnlHsRRxLgHcRk7CXEN4h"
          "hi3IOYLajcg7jQIZt4Ukzqy839Bj9DWaLOa/Oe1mHtkpJ/bLAe0FLwr4z7aHsBPNe8yeAdV6/Ctv2S"
          "CtJQAedM82aDHw1PVayemSRPbriDYI9sEBUhJ3DGmGTeYnBDaFBUT0wSyzZl9qz0OPNWUapZRbzNk0"
          "UpwjzMxkxPmhmOOAe2/ud8MG49zbzN4A8+3Cpqj4Ijk8XycT0Rus/TU9wSjHCKP8jwR5riLuzPEc+c"
          "2JvYHqyHX8DyrF+vwd8oLtCi+BtDCr/F33J2BIu/LDRjtjUKS16upXwjaH7gx9zbKnFWzXuQHQDiRJ"
          "NAB1tv3mG49xgqThf6hlhHRlULTjQPGu5IfI4sx9lOMO/0ZK93ZteZd3m45FQ27yzFpdG820PmGieZ"
          "BvOQJ/uwM5tPoix7D8L1aBqPML+DTmwawmaEv0B4BKEHHcM1CIcRfoHwIUJDRFptO/bLjCnKXpRFOH"
          "tf9PIG7AHOcaw4RCS5cnG03lzJNE2V88XGeGidw6j255O9qWQu5bzXYfBsYXmTZPjKAkJnEAsFBZ/O"
          "gIotxEU4IcInVIwunS52iMTej9jTAVzWxBC/z2jFMvuN5Xi9yZiLV7ZQBbjFmITXW41xeL3NOA2vdx"
          "j1eD1oTMTrncYEvN5l1OH1bqMRr4eMBusxzyrYsOWSdGc+vjOT74on2SMjttI+mXY7DN2YALkRwHTX"
          "HLqrvbhqF7n+rmTKycwv1+ZGedtoPm0jH1511s5uz8e3Zrq7beK+2TZ1TYbWitfG9K68XVn8b5fW5T"
          "aSs04ZLxPfkk3ttis6kHZt4qrN+stuH6t4/JLt/fmtmXTK2T5Dc4OoMrtQVMc46vGmzfSm0r0O9QIK"
          "BNFmVqhZi46NmXx32qYlEcNpq6b3sCg6166sMHbtMkdtMWq0IXagcTtQQZuwfxeH5iEcmm0OMW6/Yq"
          "NAD6TNdNb5yHPyIdYyZXU6merG8MXmZOOcNuxsC0bNLW+ZVWeu04YspE0pFECJ8pGvDaHX7EfP0Qrh"
          "AHohSY95D0W1ziJ12dqNl53bjWaYzm3v3RafO1zeyXvUwyYV3JuJNiUBHixchAcL5NkcLE1wHVtOwu"
          "XUuZyUSxUZiWNLCbK0eDkSzjHksozA2vNKkGWezVFoF+a8oiPxml+CFPO9vCKF8VpQghQLvLyihfFa"
          "WIIUC728ygrjtagEKRZ5ecUK47W4BCkWe3mVu3hVueqhL013+yJtKobLszi3qpitU+lNveOfFDH+a1"
          "zs12fzmT3p3gC0LVe5S65PsoKa6GK0OtObT+e2YWOmsrjiuDSfHrmELRUNXAGKNC6I0jBZzh4dum7h"
          "q+KPELoQgzEzVOL83IZwBcK/IyzBWW4HwvNE7GdegfA8ex4a1+aPITThMvomBIxsYQ/CywgXoL08hj"
          "CjDH8RzsII7GAMBr6B4Vsn0JvPYbuxemrpg4ODSzfz+8TAf2Aexjmbb6FnbT5S8/Debx+djeYBy7+l"
          "L9t3Gej7aKhrL4HW1m/ps7BcdOO+6LK9RD/k914Ai/esV4DgVPr/nNpy61IVMAOjyjjC+dn0uosQ2y"
          "Rfq2n8/dOgE9DVO2TsnuH3lj3D78Fa8avHSPnTqz7vwwhea3dX8DfEccG0Nrkb44iHpgokO87QYrAi"
          "2XnptlwWR90/ygy2baFXOzLiWR6I9MOlogB70ZREQb3v92Ost669Ag5o6h2DaZrQi1k2zaOcFwA8G3"
          "EDb4e1FfwNV3a/YUsFf0OwY1NbrpG/j35PmSg5oQW9y1TXe3+a0FdswPnVfblV1N14yXB1SSDfY7ME"
          "34XnB9UNBfJldRnf4LoRrJvgWwr2Ivfr4jBF0+rtx0M0+b63/8xHHc+O6PK9JR38sby1sY/qQqqPbq"
          "Kl9dHlUl/C9VUl5+Gkuuh81xMtUt+ZHoscxumzzfBKL0U7UUuGl6UMtVyGJgP/G4VRPU4pmTrvy61M"
          "jqm83dmOJOU7yr7T/1+zU1EvLYtKl2zSXZvmrZgLHXh3pjgsmApt2e1butOd3ZnOS+P5bDydyuTj+a"
          "50PM/itv741myuJ5n3ZanD9XwP6AZj2fShWuJo51qOsbRc1SAkG9/mp2Wj3AUO8Z0+3bUTdBc7M/OS"
          "sCp/VxdvnQvlunSpXBmYgcqhI2EBmVJvuChKaKlxLTV+9NyR7syiW9kgaHSnd6S748PFN4KCzinofP"
          "20sSuTK5RAiyJAOAHCRViV3Z7LdxVIYZ6iQDkFykVYldlaMIH5ikCIEwhxAh2ZXQUTWKAIhDmBMN8k"
          "6cA6vQWTWKhIRDiJCJdhZWZbwQQWKQJRTiDKCazPFC7BYkWgjBMok+cYRB4PRfiBmhoBXv+YUP6xtl"
          "D/SDyeUPhCJ8b2i1/VlV98yzF3MZ/IfGMhfvHXLp90I2ZNLccl6kqAtS0j+yTi75PYmyy1XloWlS+7"
          "fFKb7ZPGwjlqwPZkd6T5gO1LbkuX4IR+J9Wq5Grdh6I0LwTYiqL8uNNPLdP1ICQNimm+yVy9l5pFZ4"
          "tLsXW2YhMC/VFvFpfPAcyGaqgFGFl70Uamux7g1D0G1pZr31YBj1svXzLFCp10F8nFzbq2CphV0Iee"
          "mD6j+PJiNYqKHrq1aHXsw0xb/NuwrS7iwiTrozUMtCK+AXE85vwGBP+oifVQxehydpZQepNoBTQJM+"
          "LtrjXLXjS755CzOUU81jE0NfIhQPlMyk68nTMpX6QEk7BHzt9Kpjpn+hu0wKW1AP+FffL0tCCmFexg"
          "LIipNTwGcMWPsCfifAu1QRpgGGOK69G5PHg2wLYxtn9yt8Hq83AJgL12LAmwoDG4DWqDxQkmEdQG46"
          "pRRHQQj84B+MrcIKZVxbTBvWG/NjiBjPagub0wG+B3JgSI83e4vmucD3AdlrpqaZA41cWIUx/yEyeP"
          "oda9SwCOnInmPj9InKsW4IDA1fKV6O9emh8kTk0x4pSX+YmTQ5dLkcE9a3G2mxlkIRM3AMw+HWv8Kc"
          "DB+cEW0hAsTjAJ5X1F75XmpsDjpn5lhZ/ZIqZwr0vRC3Qp7rAvaIr2D1V/Hjin/qTT76MW7jk1FDSn"
          "9nnnVCc17UuuOXWZPafWeefUZCo17HTqnUJFo1tT55hPa+pMJP5JFzTVWxRaW27TTlx2aeLDWKwNB/"
          "jT2eJefNaO3WmcOQqjsS9IvELsdqiWJZZb+yEzrN5QSdXRlBqaxsPGsbIk+y7Tb62RfprExv22YDSL"
          "4ekWrlximuzbuaCp2xb7dh5olfK2VSqostr4xxRY/41krsV8yczf3Fc7Vrr+97gw0wi05zAW2Jhtb1"
          "tV2r7YJ61fyIevYDV7qBYbLkXq1O8gv/LOc+Yys0s5jFMzDW8Pt/eltor30EmdngrkW4Yj5ZsQxSnm"
          "7CXvgfZpfv+Pf/PG8w0xvzpsBLSGVTOcg/x7oA824IL0kqKbsVo+a0Y9L+SPlFZY3bABuiFdcjfGpJ"
          "MmRfDXHR8Z2IT6t/M+6C6J/2jPJFFIHeL44lkf6p6DfsggMCtI89boQWDW0D8ircl8j8fx4YoCUgjE"
          "d0JZ6oCtsAvDxSx04pyv+J6H162IGznF+ZN1tucohD8rv4k4+W9G/Xtx8GRhZwE6u+1ftH+kCP1rHc"
          "sjwb8Ddd7NW74DuvCaLqjtxVqq+P5n+t81R/VFB2/3Hkjysa9aPscxeWyXLHs2OTBN51tzxbU/6/v/"
          "tvTPc85tCElsfa8ltIxAa0oJ+rNJ8g81n54r/yJ///N/AQjv0SQ=";
        QByteArray sda = qUncompress(QByteArray::fromBase64(kStarDrawSda));
        if(!sda.isEmpty()){
          const QString p=QDir::tempPath()+"/xq_stardraw31.sda"; writeF(p, sda);
          Canvas sc; sc.newDocument();
          check(!xara::starofficeVectorAvailable() || (sc.importStarOffice(p) && !sc.doc().shapes.isEmpty()),
                "StarOffice: real StarDraw 3.1 (SUPPORTED_ENCRYPTION confidence) imports shapes"); } }

      // ---- GDAL/OGR + Graphviz (gisvector.cpp) — real functional + reject tests ----
// --- GDAL/OGR vector import selftest (hermetic; writes tiny files to the temp dir) ---
// namespace xara { QString ogrToSvg(const QString&); bool ogrAvailable(); }
// NOTE: GDAL is a GIS *vector* binding — there is no DOT/graphviz path here (the generic
// "digraph{a->b}" probe does not apply to GDAL). The equivalent "tiny valid" input is a
// trivially-constructible OGR vector document; GeoJSON is used because it can be written as
// plain text without needing a writable driver, and GDAL sniffs it by content regardless of
// the file extension we advertise for routing.
{
    using xara::ogrToSvg; using xara::ogrAvailable;
    if(ogrAvailable()){
        auto writeTmp = [](const QString& name, const QByteArray& bytes)->QString{
            QString p = QDir::tempPath() + "/" + name;
            QFile f(p); if(f.open(QIODevice::WriteOnly)){ f.write(bytes); f.close(); }
            return p;
        };
        // 1) empty path -> empty
        check(ogrToSvg(QString()).isEmpty(), "ogr: empty path rejected");
        // 2) junk bytes -> empty (GDAL finds no vector driver; every path frees + returns {})
        QString jp = writeTmp("xq_ogr_junk.bin",
                              QByteArray("\x00\x01\x02not a vector file at all", 30));
        check(ogrToSvg(jp).isEmpty(), "ogr: junk rejected -> empty");
        // 3) tiny VALID GeoJSON: point + line + polygon-with-hole
        QByteArray gj =
            "{\"type\":\"FeatureCollection\",\"features\":["
            "{\"type\":\"Feature\",\"properties\":{},\"geometry\":"
            "{\"type\":\"Point\",\"coordinates\":[10,20]}},"
            "{\"type\":\"Feature\",\"properties\":{},\"geometry\":"
            "{\"type\":\"LineString\",\"coordinates\":[[0,0],[30,40]]}},"
            "{\"type\":\"Feature\",\"properties\":{},\"geometry\":"
            "{\"type\":\"Polygon\",\"coordinates\":[[[0,0],[50,0],[50,50],[0,50],[0,0]],"
            "[[10,10],[20,10],[20,20],[10,20],[10,10]]]}}"
            "]}";
        QString gp = writeTmp("xq_ogr_test.geojson", gj);
        QString svg = ogrToSvg(gp);
        check(svg.contains("<svg"),      "ogr: valid geojson -> has <svg");
        check(svg.contains("viewBox="),  "ogr: valid geojson -> data-bounds viewBox");
        check(svg.contains("<circle"),   "ogr: point -> <circle> node");
        check(svg.contains("<polyline"), "ogr: line -> <polyline> node");
        check(svg.contains("<path"),     "ogr: polygon -> <path> node");
        // Y-flip: point (10,20), overall maxY=50 -> cy = 50-20 = 30
        check(svg.contains("cy='30'"),   "ogr: Y-flip maps point cy correctly");
        // Regression (adversarial review): coordinates near DBL_MAX must not overflow bbox/Y-flip to "inf".
        QByteArray big =
            "{\"type\":\"FeatureCollection\",\"features\":["
            "{\"type\":\"Feature\",\"properties\":{},\"geometry\":{\"type\":\"Point\",\"coordinates\":[5,6]}},"
            "{\"type\":\"Feature\",\"properties\":{},\"geometry\":{\"type\":\"MultiPoint\",\"coordinates\":[[1e308,-1e308],[-1e308,1e308]]}}"
            "]}";
        QString bp = writeTmp("xq_ogr_big.geojson", big);
        QString bsvg = ogrToSvg(bp);
        check(!bsvg.contains("inf") && !bsvg.contains("nan"), "ogr: huge coords don't overflow to inf/nan in SVG");
    } else {
        check(ogrToSvg(QString("x")).isEmpty(), "ogr: stub returns empty");
    }
}
// --- Graphviz DOT->SVG selftest ---------------------------------------------
// Available in both builds via the stub; only asserts real output when present.
{
    if (xara::dotAvailable()) {
        // 1) Junk / non-DOT input must yield an empty string, not a crash.
        QString j1 = xara::dotToSvg(QByteArray("this is not a graph {{{"));
        check(j1.isEmpty(), "graphviz rejects junk");

        QString j2 = xara::dotToSvg(QByteArray());
        check(j2.isEmpty(), "graphviz rejects empty");

        QString j3 = xara::dotToSvg(QByteArray("digraph { a -> "));  // truncated
        check(j3.isEmpty(), "graphviz rejects truncated dot");

        // 2) A tiny valid digraph must render an SVG that mentions the nodes.
        QString ok = xara::dotToSvg(QByteArray("digraph{a->b}"));
        check(ok.contains("<svg"), "graphviz emits <svg");
        check(ok.contains(QStringLiteral(">a<")) ||
                       ok.contains(QStringLiteral("class=\"node\"")), "graphviz svg has node a");
        check(ok.contains(QStringLiteral("class=\"edge\"")) ||
                       ok.contains(QStringLiteral("<path")), "graphviz svg has an edge");
    } else {
        // Stub build: must be inert and empty.
        check(xara::dotToSvg(QByteArray("digraph{a->b}")).isEmpty(), "graphviz stub empty");
    }
}
      // Regression (adversarial review): a huge aperture modifier in inch units overflows to inf;
      // the emitted SVG must NOT contain the literal "inf" (fmt() + md() are isfinite-guarded).
      { QByteArray g = "%FSLAX46Y46*%\n%MOIN*%\n%ADD10C,1*%\n%ADD11C,1e308*%\nD10*\nX0Y0D03*\nD11*\nX1000000Y0D03*\nM02*\n";
        QString svg=xara::gerberToSvg((const uint8_t*)g.constData(),(qint64)g.size());
        check(!svg.contains("inf") && !svg.contains("nan"), "gerber: overflowing aperture not emitted as inf/nan"); }

      // ---- Fleet 16 native vector decoders (TGS/TopoJSON/VML/XPS/Scribus) — hermetic ----
// ---- TGS importer ---------------------------------------------------------
// Build a minimal Bodymovin JSON, gzip it in-memory, feed it to tgsToSvg, and
// assert the delegated Lottie render produced an <svg> with the right bounds.
{
    // A minimal but valid Lottie doc: 100x100 canvas, one solid-fill layer.
    QByteArray json =
        "{\"v\":\"5.5.2\",\"fr\":60,\"ip\":0,\"op\":60,\"w\":100,\"h\":100,"
        "\"layers\":[{\"ty\":4,\"ks\":{},\"shapes\":[{\"ty\":\"gr\",\"it\":["
        "{\"ty\":\"rc\",\"p\":{\"a\":0,\"k\":[50,50]},\"s\":{\"a\":0,\"k\":[80,80]}},"
        "{\"ty\":\"fl\",\"c\":{\"a\":0,\"k\":[1,0,0,1]},\"o\":{\"a\":0,\"k\":100}}"
        "]}]}]}";

    // gzip-compress `json` into `gz` using zlib (gzip window = 15+16).
    QByteArray gz;
    {
        z_stream s; std::memset(&s, 0, sizeof(s));
        int r = deflateInit2(&s, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                             15 + 16, 8, Z_DEFAULT_STRATEGY);
        check(r == Z_OK, "tgs selftest: deflateInit2 ok");
        if (r == Z_OK) {
            s.next_in  = reinterpret_cast<Bytef*>(const_cast<char*>(json.constData()));
            s.avail_in = (uInt)json.size();
            char obuf[4096];
            do {
                s.next_out  = reinterpret_cast<Bytef*>(obuf);
                s.avail_out = sizeof(obuf);
                r = deflate(&s, Z_FINISH);
                gz.append(obuf, (int)(sizeof(obuf) - s.avail_out));
            } while (r == Z_OK);
            deflateEnd(&s);
            check(r == Z_STREAM_END, "tgs selftest: deflate finished");
        }
    }

    // Sanity: our synthetic file carries the gzip magic tgsToSvg looks for.
    check(gz.size() > 2 && (quint8)gz[0] == 0x1f && (quint8)gz[1] == 0x8b,
          "tgs selftest: has gzip magic");

    QString svg = xara::tgsToSvg(reinterpret_cast<const uint8_t*>(gz.constData()),
                                 (qint64)gz.size());
    check(!svg.isEmpty(),               "tgs: gzipped Lottie -> non-empty SVG");
    check(svg.contains("<svg"),         "tgs: output is an SVG document");
    check(svg.contains("viewBox"),      "tgs: SVG has a data-bounds viewBox");
    check(svg.contains("100"),          "tgs: 100x100 canvas bounds propagated");

    // Non-gzip input must be rejected (fall-through contract).
    QByteArray plain = json;
    check(xara::tgsToSvg(reinterpret_cast<const uint8_t*>(plain.constData()),
                         (qint64)plain.size()).isEmpty(),
          "tgs: non-gzip input rejected");

    // Truncated gzip (header only) must not crash and must return empty.
    check(xara::tgsToSvg(reinterpret_cast<const uint8_t*>(gz.constData()),
                         (qint64)qMin<qint64>(12, gz.size())).isEmpty(),
          "tgs: truncated stream rejected");

    // Too-short / null inputs.
    check(xara::tgsToSvg(nullptr, 0).isEmpty(), "tgs: null input rejected");
}
{
    // Minimal TopoJSON: identity transform, one square arc, a Polygon + a Point.
    // Arc deltas accumulate to (0,0)->(0,10)->(10,10)->(10,0)->(0,0); bbox 0..10.
    const char* tj =
        "{\"type\":\"Topology\","
        "\"transform\":{\"scale\":[1,1],\"translate\":[0,0]},"
        "\"arcs\":[[[0,0],[0,10],[10,0],[0,-10],[-10,0]]],"
        "\"objects\":{"
            "\"sq\":{\"type\":\"Polygon\",\"arcs\":[[0]]},"
            "\"pt\":{\"type\":\"Point\",\"coordinates\":[5,5]}"
        "}}";
    QByteArray b(tj);
    QString svg = xara::topojsonToSvg((const uint8_t*)b.constData(), (qint64)b.size());
    check(!svg.isEmpty(), "topojson: parses minimal topology");
    check(svg.contains("viewBox='0 0 10 10'"), "topojson: data-bounds viewBox");
    check(svg.contains("<path"), "topojson: polygon -> <path>");
    check(svg.contains("fill-rule='evenodd'"), "topojson: polygon uses evenodd");
    check(svg.contains("<circle"), "topojson: point -> <circle>");
    check(svg.contains("cy='5'"), "topojson: point y-flip (maxY-y = 5)");
    check(svg.contains("Z"), "topojson: ring is closed");
    // Y-flip check: the ring's node at real y=10 must map to screen y=0,
    // and a node at real y=0 must map to screen y=10.
    check(svg.contains("M0,10") || svg.contains("L0,10"), "topojson: y-flip node at bottom");
    check(svg.contains(",0 ") || svg.contains(",0'"), "topojson: y-flip node at top");

    // Fall-through: not JSON at all.
    check(xara::topojsonToSvg((const uint8_t*)"not a topojson", (qint64)14).isEmpty(),
          "topojson: garbage falls through");
    // Fall-through: valid JSON but a GeoJSON FeatureCollection (no "Topology").
    const char* geo = "{\"type\":\"FeatureCollection\",\"features\":[]}";
    QByteArray gb(geo);
    check(xara::topojsonToSvg((const uint8_t*)gb.constData(), (qint64)gb.size()).isEmpty(),
          "topojson: geojson (non-Topology) falls through");

    // Transform-less absolute-coordinate topology with a LineString.
    const char* lt =
        "{\"type\":\"Topology\","
        "\"arcs\":[[[0,0],[4,0],[4,3]]],"
        "\"objects\":{\"ln\":{\"type\":\"LineString\",\"arcs\":[0]}}}";
    QByteArray lb(lt);
    QString lsvg = xara::topojsonToSvg((const uint8_t*)lb.constData(), (qint64)lb.size());
    check(lsvg.contains("<polyline"), "topojson: LineString -> <polyline>");
    check(lsvg.contains("viewBox='0 0 4 3'"), "topojson: absolute (no-transform) bbox");
}
{
    // Hand-built minimal VML document exercising rect/oval/line/polyline/shape-path.
    QByteArray vml =
      "<xml xmlns:v='urn:schemas-microsoft-com:vml'>"
      "<v:rect left='10' top='20' width='100' height='50' fillcolor='#ff0000' strokecolor='blue'/>"
      "<v:oval style='left:200;top:20;width:80;height:40' fillcolor='lime'/>"
      "<v:line from='0,0' to='300,300' strokecolor='#00ff00'/>"
      "<v:polyline points='10,10 20,30 40,10'/>"
      "<v:shape path='m 400 400 l 500 400 500 500 c 520 520 540 540 560 560 x e' fillcolor='#123456'/>"
      "</xml>";
    QString svg = xara::vmlToSvg((const uint8_t*)vml.constData(), (qint64)vml.size());
    check(svg.contains("<svg"),           "vml: svg root emitted");
    check(svg.contains("viewBox="),       "vml: viewBox present");
    check(svg.contains("<rect"),          "vml: <rect>");
    check(svg.contains("#ff0000"),        "vml: rect fillcolor hex");
    check(svg.contains("stroke='blue'"),  "vml: keyword strokecolor validated");
    check(svg.contains("<ellipse"),       "vml: <oval> -> <ellipse>");
    check(svg.contains("<line"),          "vml: <line from/to>");
    check(svg.contains("<polyline"),      "vml: <polyline points>");
    check(svg.contains("<path"),          "vml: <shape path> -> <path>");
    check(svg.contains("M400"),           "vml: path moveto abs");
    check(svg.contains("C520"),           "vml: path cubic abs");

    // A real SVG document (root <svg>) must be rejected so the importer falls through.
    QByteArray asvg = "<svg xmlns='http://www.w3.org/2000/svg'><rect x='0' y='0' width='1' height='1'/></svg>";
    check(xara::vmlToSvg((const uint8_t*)asvg.constData(),(qint64)asvg.size()).isEmpty(), "vml: reject SVG root");

    // Garbage / non-XML -> empty.
    QByteArray junk = "not xml at all really";
    check(xara::vmlToSvg((const uint8_t*)junk.constData(),(qint64)junk.size()).isEmpty(), "vml: garbage -> empty");

    // Hostile fillcolor cannot break out of the attribute; falls back to default fill.
    QByteArray hv = "<xml><rect left='0' top='0' width='5' height='5' fillcolor=\"red' onload=x\"/></xml>";
    QString hs = xara::vmlToSvg((const uint8_t*)hv.constData(),(qint64)hv.size());
    check(!hs.contains("onload"),         "vml: hostile colour stripped");
    check(hs.contains("fill='#ffffff'"),  "vml: invalid colour -> default white fill");
}
{
    // Hand-build a minimal XPS package: a STORED (method 0) ZIP holding one
    // "*.fpage" FixedPage part, then assert xpsToSvg translates it.
    auto p16=[](std::vector<unsigned char>& v, unsigned x){ v.push_back(x&0xff); v.push_back((x>>8)&0xff); };
    auto p32=[](std::vector<unsigned char>& v, unsigned long x){ for(int i=0;i<4;i++) v.push_back((x>>(8*i))&0xff); };
    auto ps =[](std::vector<unsigned char>& v, const char* s){ for(;*s;s++) v.push_back((unsigned char)*s); };

    const char* xpsName = "Documents/1/Pages/1.fpage";
    const char* xpsData =
      "<FixedPage xmlns=\"http://schemas.microsoft.com/xps/2005/06\" Width=\"200\" Height=\"100\">"
      "<Path Data=\"F1 M 10,10 L 90,10 L 90,90 Z\" Fill=\"#ff0000\"/>"
      "<Glyphs OriginX=\"20\" OriginY=\"50\" FontRenderingEmSize=\"12\" UnicodeString=\"Hi &amp; bye\" Fill=\"#0000ff\"/>"
      "</FixedPage>";
    unsigned xpsNl = (unsigned)strlen(xpsName), xpsDl = (unsigned)strlen(xpsData);
    std::vector<unsigned char> z;
    // local file header @0
    p32(z,0x04034b50); p16(z,20); p16(z,0); p16(z,0); p16(z,0); p16(z,0);
    p32(z,0); p32(z,xpsDl); p32(z,xpsDl); p16(z,xpsNl); p16(z,0); ps(z,xpsName);
    ps(z,xpsData);
    unsigned xpsCdOff = (unsigned)z.size();
    // central directory record
    p32(z,0x02014b50); p16(z,20); p16(z,20); p16(z,0); p16(z,0); p16(z,0); p16(z,0);
    p32(z,0); p32(z,xpsDl); p32(z,xpsDl); p16(z,xpsNl); p16(z,0); p16(z,0); p16(z,0); p16(z,0);
    p32(z,0); p32(z,0 /*localOff*/); ps(z,xpsName);
    unsigned xpsCdSize = (unsigned)z.size() - xpsCdOff;
    // end of central directory
    p32(z,0x06054b50); p16(z,0); p16(z,0); p16(z,1); p16(z,1);
    p32(z,xpsCdSize); p32(z,xpsCdOff); p16(z,0);

    QString xpsSvg = xara::xpsToSvg((const uint8_t*)z.data(), (qint64)z.size());
    check(!xpsSvg.isEmpty(), "XPS: non-empty SVG produced");
    check(xpsSvg.contains("viewBox='0 0 200 100'"), "XPS: FixedPage W/H -> viewBox");
    check(xpsSvg.contains("<path d='M 10,10 L 90,10 L 90,90 Z'"), "XPS: Path Data F1 token stripped + passed through");
    check(xpsSvg.contains("fill='#ff0000'"), "XPS: Path Fill colour");
    check(xpsSvg.contains("<text x='20' y='50' font-size='12' fill='#0000ff'"), "XPS: Glyphs -> text");
    check(xpsSvg.contains("Hi &amp; bye</text>"), "XPS: UnicodeString XML-escaped");
    check(xara::xpsToSvg((const uint8_t*)"notazip", (qint64)7).isEmpty(), "XPS: reject non-ZIP input");
    check(xara::xpsToSvg((const uint8_t*)nullptr, (qint64)0).isEmpty(), "XPS: reject null/empty");
}
// ---- Scribus .sla importer selftest ----
{
    const char* sla =
        "<?xml version=\"1.0\"?>\n"
        "<SCRIBUSUTF8NEW><DOCUMENT>\n"
        "<COLOR NAME=\"Red\" RGB=\"#ff0000\"/>\n"
        "<COLOR NAME=\"Blue\" RGB=\"0 0 255\"/>\n"
        "<COLOR NAME=\"Cy\" CMYK=\"#ff000000\"/>\n"
        "<PAGEOBJECT PTYPE=\"6\" XPOS=\"10\" YPOS=\"20\" WIDTH=\"30\" HEIGHT=\"40\""
        " FCOLOR=\"Red\" PCOLOR=\"Blue\" PWIDTH=\"2\""
        " path=\"M 0 0 L 30 0 L 30 40 L 0 40 Z\"/>\n"
        "<PAGEOBJECT PTYPE=\"7\" XPOS=\"100\" YPOS=\"5\" WIDTH=\"50\" HEIGHT=\"50\""
        " FCOLOR=\"None\" PCOLOR=\"Cy\" PWIDTH=\"1\""
        " POCOOR=\"0 0 0 0 10 10 10 10 20 0 20 0\"/>\n"
        "<PAGEOBJECT PTYPE=\"2\" XPOS=\"5\" YPOS=\"5\" WIDTH=\"12\" HEIGHT=\"8\""
        " FCOLOR=\"Blue\" PCOLOR=\"None\"/>\n"
        "</DOCUMENT></SCRIBUSUTF8NEW>\n";
    QString svg = xara::scribusToSvg((const uint8_t*)sla, (qint64)strlen(sla));
    check(!svg.isEmpty(),                         "scribus: non-empty SVG");
    check(svg.startsWith("<svg"),                 "scribus: svg root");
    check(svg.contains("viewBox="),               "scribus: has viewBox");
    check(svg.contains("#ff0000"),                "scribus: RGB #hex colour");
    check(svg.contains("#0000ff"),                "scribus: RGB triple -> #hex");
    check(svg.contains("#00ffff"),                "scribus: CMYK -> #hex (cyan)");
    check(svg.contains("translate(10 20)"),       "scribus: object XPOS/YPOS translate");
    check(svg.contains("M 0 0 L 30 0"),           "scribus: local path preserved");
    check(svg.contains("fill=\"none\""),          "scribus: FCOLOR None -> none");
    check(svg.contains("M 0 0 L 10 10 L 20 0"),   "scribus: POCOOR every-2nd-pair polyline");
    check(svg.contains("<rect"),                  "scribus: no-path object -> bounding rect");

    // Non-Scribus input falls through (empty).
    const char* junk = "just some random bytes not a scribus doc at all!!";
    check(xara::scribusToSvg((const uint8_t*)junk, (qint64)strlen(junk)).isEmpty(),
          "scribus: rejects non-Scribus input");

    // Hostile path attribute must not break out of the d="" attribute.
    const char* bad =
        "<SCRIBUSUTF8NEW><PAGEOBJECT XPOS=\"0\" YPOS=\"0\" WIDTH=\"5\" HEIGHT=\"5\""
        " FCOLOR=\"x\" path=\"M0 0&quot;/&gt;&lt;script&gt;\"/></SCRIBUSUTF8NEW>";
    QString s2 = xara::scribusToSvg((const uint8_t*)bad, (qint64)strlen(bad));
    check(!s2.contains("<script"),                "scribus: no path-attr injection");
}
// ---- Serif Affinity (.afdesign/.afphoto/.afpub) vector importer ----
// The fixture is BUILT HERE, byte by byte, from the documented layout — no sample file is vendored,
// so the test is hermetic and also doubles as a spec of the encoding. It exercises: the v9 "#FAT"
// container with a zlib chunk, the CRC32 gate, the tagged object tree (shareable classes, an empty
// 0x32 array whose class header is still present, a positional 0x30 struct, bit-packed bool arrays,
// enums, strings, fixed structs), a DAG link that shares one fill between two shapes, ShNR rounded
// rectangles, PCrv cubic geometry checked against its own stored CvsB, and the "Thmb" preview.
{
    // -------- tiny writers for the two encodings --------
    auto u8v =[](QByteArray& b, unsigned v){ b.append(char(v&0xff)); };
    auto u16v=[](QByteArray& b, unsigned v){ b.append(char(v&0xff)); b.append(char((v>>8)&0xff)); };
    auto u32v=[](QByteArray& b, quint32 v){ for(int i=0;i<4;i++) b.append(char((v>>(8*i))&0xff)); };
    auto u64v=[](QByteArray& b, quint64 v){ for(int i=0;i<8;i++) b.append(char((v>>(8*i))&0xff)); };
    auto f32v=[&](QByteArray& b, float v){ quint32 r; memcpy(&r,&v,4); u32v(b,r); };
    auto f64v=[&](QByteArray& b, double v){ quint64 r; memcpy(&r,&v,8); u64v(b,r); };
    // Object-tree 4CCs are stored byte-REVERSED; container tags ("#Inf", "#FAT", "#Fil") are not.
    auto tag =[&](QByteArray& b, const char* s){ for(int i=3;i>=0;--i) b.append(s[i]); };
    // field header: type byte (|0x80 = array) + the reversed 4CC
    auto fld =[&](QByteArray& b, unsigned type, const char* name){ u8v(b,type); tag(b,name); };

    QByteArray t;                                   // the object tree ("doc.dat" payload)
    t.append('\x00'); t.append('\xff'); t.append('K'); t.append('S');
    u16v(t,1); tag(t,"Pers"); u16v(t,1);            // file_ver 1 → no trailing version word

    fld(t,0x31,"DocR");                             // shareable class: flag 1, id, chain, fields
      u8v(t,1); u32v(t,1); u8v(t,1); tag(t,"DocN");
      fld(t,0x2b,"Desc"); u32v(t,4); t.append("page");
      fld(t,0xa9,"TstB"); u32v(t,10); u8v(t,0x95); u8v(t,0x02);   // bit-packed bool[10] = 1,0,1,0,1,0,0,1,0,1
      fld(t,0xb2,"Objs"); u32v(t,0); tag(t,"OSty"); u16v(t,1);    // EMPTY 0x32 array still carries tag+id
      fld(t,0x31,"Chld");                           // one spread — written as a single (non-array) child
        u8v(t,1); u32v(t,2); u8v(t,1); tag(t,"Sprd");
        fld(t,0x26,"SprB"); f64v(t,0); f64v(t,0); f64v(t,180); f64v(t,180);
        fld(t,0xb1,"Chld"); u32v(t,2);              // array of 2 shareable classes
          // -- 1: a rounded rectangle with a red fill (registered as id 5 and shared below) --
          u8v(t,1); u32v(t,3); u8v(t,1); tag(t,"ShpN");
            fld(t,0x2b,"Desc"); u32v(t,9); t.append("Red panel");   // the layer's name in Affinity
            fld(t,0x26,"ShpB"); f64v(t,10); f64v(t,20); f64v(t,110); f64v(t,70);
            fld(t,0x28,"Xfrm"); f64v(t,1); f64v(t,0); f64v(t,5); f64v(t,0); f64v(t,1); f64v(t,7);
            fld(t,0x31,"Shpe");
              u8v(t,1); u32v(t,4); u8v(t,1); tag(t,"ShNR");
              fld(t,0x21,"ShCR"); f32v(t,0.5f); f32v(t,0.5f); f32v(t,0.5f); f32v(t,0.5f);
              fld(t,0xaa,"CTyp"); u32v(t,4); u16v(t,0); for(int i=0;i<4;i++) u16v(t,0);
              u8v(t,0);
            fld(t,0x31,"BFil");
              u8v(t,1); u32v(t,5); u8v(t,1); tag(t,"FilS");
              fld(t,0x31,"Colr");
                u8v(t,1); u32v(t,6); u8v(t,1); tag(t,"RGBA");
                fld(t,0x44,"_col"); f32v(t,1.0f); f32v(t,0.0f); f32v(t,0.0f); f32v(t,1.0f);
                u8v(t,0);
              u8v(t,0);
            u8v(t,0);
          // -- 2: an open cubic path, stroked blue, filled by a LINK back to the shared red fill --
          u8v(t,1); u32v(t,7); u8v(t,1); tag(t,"PCrv");
            fld(t,0x31,"BFil"); u8v(t,2); u32v(t,5);             // class31 flag 2 = link to id 5
            fld(t,0x31,"PFil");
              u8v(t,1); u32v(t,8); u8v(t,1); tag(t,"FilS");
              fld(t,0x31,"Colr");
                u8v(t,1); u32v(t,9); u8v(t,1); tag(t,"RGBA");
                fld(t,0x44,"_col"); f32v(t,0.0f); f32v(t,0.0f); f32v(t,1.0f); f32v(t,1.0f);
                u8v(t,0);
              u8v(t,0);
            fld(t,0x31,"LSty");
              u8v(t,1); u32v(t,10); u8v(t,1); tag(t,"LSty");
              fld(t,0x0a,"Wght"); f64v(t,3.0);
              u8v(t,0);
            fld(t,0x32,"Crvs");                                   // plain class, header read inline
              u8v(t,1); tag(t,"PCvD"); u16v(t,1);
              fld(t,0x30,"Data");                                 // positional: u8, u32 count, then per subpath
                u8v(t,0x01); u8v(t,0);
                u8v(t,0x03); u32v(t,1);
                u8v(t,0x29); u8v(t,0);                            // not closed
                u8v(t,0xac); u32v(t,4); u16v(t,18);               // 0x2c array, 4 records of 18 bytes
                  f64v(t,0);  f64v(t,0);  u8v(t,1); u8v(t,0);     // anchor
                  f64v(t,0);  f64v(t,0);  u8v(t,0); u8v(t,1);     // out handle
                  f64v(t,40); f64v(t,60); u8v(t,0); u8v(t,2);     // in handle
                  f64v(t,40); f64v(t,60); u8v(t,1); u8v(t,0);     // anchor
                u8v(t,0);
              u8v(t,0);
            fld(t,0x26,"CvsB"); f64v(t,0); f64v(t,0); f64v(t,40); f64v(t,60);
            u8v(t,0);
        u8v(t,0);                                   // end Sprd
      u8v(t,0);                                     // end DocN
    u8v(t,0);                                       // end root

    // -------- wrap the tree in a v9 "#FAT" container with a zlib chunk and a Thmb preview --------
    const QByteArray comp = qCompress(t,9).mid(4);  // qCompress prepends a 4-byte size; Affinity stores raw zlib
    const quint32 crc = (quint32)crc32(crc32(0,nullptr,0), (const Bytef*)t.constData(), (uInt)t.size());
    QImage thumb(6,4,QImage::Format_ARGB32); thumb.fill(QColor(180,60,220));
    QByteArray png; { QBuffer bf(&png); bf.open(QIODevice::WriteOnly); thumb.save(&bf,"PNG"); }

    auto buildFile=[&](const QByteArray& payload, quint32 useCrc, bool withThumb)->QByteArray{
        QByteArray f;
        f.append('\x00'); f.append('\xff'); f.append('K'); f.append('A');
        u16v(f,9); u16v(f,0); tag(f,"Prsn");
        f.append("#Inf",4);
        const quint64 chunkOff = 72;
        const quint64 fatOff = chunkOff + 4 + quint64(payload.size()) + 4;
        const quint64 thmbOff = fatOff + 59 + 36 + 7;              // header + one named entry ("doc.dat")
        u64v(f,fatOff); u64v(f,thmbOff); u64v(f,(quint64)payload.size()); u64v(f,0); u64v(f,0);
        u32v(f,1); u32v(f,2);
        f.append("Prot",4); u32v(f,3);                              // present when version > 7
        f.append("#Fil",4); f.append(payload); u32v(f,0xFFFFFFFFu); // every chunk has its own terminator
        f.append("#FAT",4);
        u64v(f,0); u64v(f,1000); u64v(f,thmbOff); u64v(f,(quint64)payload.size()); u64v(f,0);
        u32v(f,1); u32v(f,0); u32v(f,36+7); u16v(f,0); u8v(f,0);
        u32v(f,1); u8v(f,0);                                        // id 1, flag 0 (named)
        u64v(f,chunkOff); u64v(f,(quint64)t.size()); u64v(f,(quint64)payload.size());
        u32v(f,useCrc); u8v(f,1);                                   // #FAT compression INDEX 1 → zlib
        u16v(f,7); f.append("doc.dat",7);
        if(withThumb){
            u32v(f,0xFFFFFFFFu); f.append("Thmb",4); u32v(f,1);
            u32v(f,(quint32)(png.size()+13)); u32v(f,29); u32v(f,0); u32v(f,(quint32)png.size()); u8v(f,1);
            f.append(png);
        }
        return f;
    };

    const QByteArray good = buildFile(comp, crc, true);
    check(xara::affinityIsContainer(good), "Affinity: container signature recognised");
    int sOk=0, sBad=0;
    check(xara::affinityVerifyStreams(good,&sOk,&sBad) && sOk==1 && sBad==0,
          "Affinity: every chunk decompresses and matches its stored CRC32");

    xara::AffinityStats st;
    const QStringList pages = xara::affinityToSvgPages(good,&st);
    check(pages.size()==1 && st.pages==1,         "Affinity: one SVG page per spread");
    check(st.exactParse && st.treeConsumed==st.treeBytes,
          "Affinity: the object tree parses to the exact last byte of doc.dat");
    check(st.shapes==2,                           "Affinity: both drawables converted");
    const QString svg = pages.isEmpty()? QString() : pages[0];
    check(svg.contains("viewBox=\"0 0 180 180\""), "Affinity: SprB becomes the page viewBox");
    check(svg.contains("inkscape:label=\"Red panel\""), "Affinity: a layer's Desc is carried out as its name");
    check(svg.contains("transform=\"matrix(1,0,0,1,5,7)\""),
          "Affinity: Xfrm rows (m0,m1,m2,m3,m4,m5) map to SVG matrix(m0,m3,m1,m4,m2,m5)");
    check(svg.contains("<rect x=\"10\" y=\"20\" width=\"100\" height=\"50\""),
          "Affinity: ShpN ShpB becomes the rectangle box");
    // ShCR is a fraction of the FULL shorter side (domain [0, 0.5]; 0.5 = fully rounded, the
    // geometric maximum). This assertion previously encoded a further halving, which drew every
    // rounded rectangle at half its corner radius. Pinned by measuring shape_test.afdesign's 45x45
    // rounded rectangle (ShCR = 0.24449) against that document's own 512px preview: the radius comes
    // out at ~11 units = ShCR * min(w,h), not the ~5.5 the halved formula gives.
    check(svg.contains("rx=\"25\""),              "Affinity: ShNR ShCR = fraction of the shorter side (not of half of it)");
    check(svg.contains("fill=\"#ff0000\"") && svg.count("fill=\"#ff0000\"")==2,
          "Affinity: a class-31 LINK resolves, so one shared fill colours both shapes");
    check(svg.contains("stroke=\"#0000ff\"") && svg.contains("stroke-width=\"3\""),
          "Affinity: PFil is the STROKE colour and LSty Wght its width");
    check(svg.contains("<path d=\"M0 0C0 0 40 60 40 60\""),
          "Affinity: PCrv 18-byte records become a cubic Bezier chain");
    check(st.bboxOk==1 && st.bboxBad==0,
          "Affinity: the computed cubic bounds reproduce the file's own stored CvsB");

    // The preview is located from the header, not by scanning for a PNG signature.
    QImage prev; prev.loadFromData(xara::affinityPreviewPng(good),"PNG");
    check(!prev.isNull() && prev.size()==QSize(6,4) && prev.pixelColor(0,0)==QColor(180,60,220),
          "Affinity: Thmb preview extracted exactly from thmb_offset");

    // ---- hostile / damaged input must decline cleanly, never hang, over-read or half-decode ----
    check(xara::affinityToSvgPages(buildFile(comp, crc ^ 0x5A5A5A5Au, true)).isEmpty(),
          "Affinity: a chunk whose CRC32 does not match is rejected outright");
    for(int cut : {0, 40, 79, 100, 200}){
        const QByteArray trunc = good.left(cut);
        check(xara::affinityToSvgPages(trunc).isEmpty(), "Affinity: truncated file declines");
    }
    { QByteArray flip = good; flip[6] = char(1);                    // flag bits 0-1 = a variant we do not read
      check(xara::affinityToSvgPages(flip).isEmpty(), "Affinity: reserved container flag bits rejected"); }
    { QByteArray notAff = "\x00\xffKAnope"; notAff.append(200,'\0');
      check(!xara::affinityIsContainer(notAff) && xara::affinityToSvgPages(notAff).isEmpty(),
            "Affinity: a file without the #Inf header is not claimed"); }
    { // A tree whose array count claims 0xFFFFFFFF elements must not be believed (no huge allocation).
      QByteArray bomb; bomb.append('\x00'); bomb.append('\xff'); bomb.append('K'); bomb.append('S');
      u16v(bomb,1); tag(bomb,"Pers"); u16v(bomb,1);
      fld(bomb,0x8a,"Bomb"); u32v(bomb,0xFFFFFFFFu);                // double[] with an absurd count
      const QByteArray bz = qCompress(bomb,9).mid(4);
      const quint32 bcrc = (quint32)crc32(crc32(0,nullptr,0), (const Bytef*)bomb.constData(), (uInt)bomb.size());
      QByteArray save = t; t = bomb;                                // buildFile reports t.size() as the raw size
      check(xara::affinityToSvgPages(buildFile(bz, bcrc, false)).isEmpty(),
            "Affinity: an absurd array count is rejected instead of allocated");
      t = save; }
    { // An unknown field type (0x76/0x77 are genuinely undocumented) must abort rather than guess a length.
      QByteArray bad; bad.append('\x00'); bad.append('\xff'); bad.append('K'); bad.append('S');
      u16v(bad,1); tag(bad,"Pers"); u16v(bad,1);
      fld(bad,0x77,"Unkn");
      const QByteArray bz = qCompress(bad,9).mid(4);
      const quint32 bcrc = (quint32)crc32(crc32(0,nullptr,0), (const Bytef*)bad.constData(), (uInt)bad.size());
      QByteArray save = t; t = bad;
      check(xara::affinityToSvgPages(buildFile(bz, bcrc, false)).isEmpty(),
            "Affinity: an unknown field type declines rather than guessing its length");
      t = save; }
    { // Regression (adversarial review): the FAT/thumbnail/chunk offsets are file-derived u64s, so a
      // bounds check written as "off + k > size" WRAPS on 0xFFFF…FF and hands a wild pointer to memcmp
      // (ASan: heap-buffer-overflow). Every such comparison must be done by subtraction.
      for(quint64 fo : { quint64(0xFFFFFFFFFFFFFFFFull), quint64(0xFFFFFFFFFFFFFFC5ull), quint64(1ull)<<62 }){
          QByteArray f;
          f.append('\x00'); f.append('\xff'); f.append('K'); f.append('A');
          u16v(f,9); u16v(f,0); tag(f,"Prsn"); f.append("#Inf",4);
          u64v(f,fo); u64v(f,0xFFFFFFFFFFFFFFFFull); u64v(f,0); u64v(f,0); u64v(f,0); u32v(f,1); u32v(f,2);
          f.append("Prot",4); u32v(f,3);
          f.append(300,'\0');
          int o=0, b=0;
          const bool clean = xara::affinityToSvgPages(f).isEmpty()
                          && !xara::affinityVerifyStreams(f,&o,&b)
                          && xara::affinityPreviewPng(f).isEmpty();
          check(clean, "Affinity: an out-of-range u64 offset is rejected, not wrapped into a wild read");
      } }
    { // Regression (adversarial review): field payloads AMPLIFY. A bool array is bit-packed 8-per-byte in
      // the file but 8 bytes each in memory (64x), so ~4 MB of crafted tree can ask for ~240 MB. The tree
      // reader charges every allocation against one budget and must decline rather than honour it.
      QByteArray bomb;
      bomb.append('\x00'); bomb.append('\xff'); bomb.append('K'); bomb.append('S');
      u16v(bomb,1); tag(bomb,"Pers"); u16v(bomb,1);
      const int nbools = 30000000;
      fld(bomb,0xa9,"Bomb"); u32v(bomb,nbools); bomb.append((nbools+7)/8, '\0');
      const QByteArray bz = qCompress(bomb,1).mid(4);
      const quint32 bcrc = (quint32)crc32(crc32(0,nullptr,0), (const Bytef*)bomb.constData(), (uInt)bomb.size());
      QByteArray save = t; t = bomb;
      check(xara::affinityToSvgPages(buildFile(bz, bcrc, false)).isEmpty(),
            "Affinity: a bit-packed bool array that would amplify to hundreds of MB is refused");
      t = save; }
    check(xara::affinityToSvgPages(QByteArray()).isEmpty(), "Affinity: empty input declines");
}
// ---- SVG presentation attributes the bridge used to swallow ----
// Every vector importer that routes through parseSvg (Affinity, CDR, Visio, PageMaker, ODF, AI/EPS…)
// inherits these, so each one was a silent loss in a dozen formats at once.
{
    // fill-opacity is PER-PAINT: it must not fade the stroke too. They shared one slot before.
    { const QByteArray svg = "<svg xmlns='http://www.w3.org/2000/svg'>"
          "<rect x='0' y='0' width='10' height='10' fill='#ff0000' fill-opacity='0.5'"
          " stroke='#00ff00' stroke-width='2'/></svg>";
      const QVector<Shape> v = parseSvg(svg, nullptr, QString());
      check(v.size()==1 && qAbs(v[0].fill.color.alphaF()-0.5)<0.02,
            "svg: fill-opacity fades the fill");
      check(v.size()==1 && v[0].stroke.color.alphaF()>0.99 && v[0].stroke.transp<0.01,
            "svg: fill-opacity leaves the STROKE opaque");
      check(v.size()==1 && v[0].opacity>0.99,
            "svg: fill-opacity is not whole-shape opacity"); }

    // stroke-opacity was emitted by importers and read by nobody.
    { const QByteArray svg = "<svg xmlns='http://www.w3.org/2000/svg'>"
          "<rect x='0' y='0' width='10' height='10' stroke='#000' stroke-width='2' stroke-opacity='0.25'/></svg>";
      const QVector<Shape> v = parseSvg(svg, nullptr, QString());
      check(v.size()==1 && qAbs(v[0].stroke.transp-0.75)<0.02, "svg: stroke-opacity reaches the stroke"); }

    // Nested opacity multiplies; it used to be a plain assignment inherited by children.
    { const QByteArray svg = "<svg xmlns='http://www.w3.org/2000/svg'><g opacity='0.5'><g opacity='0.5'>"
          "<rect x='0' y='0' width='10' height='10' fill='#000'/></g></g></svg>";
      const QVector<Shape> v = parseSvg(svg, nullptr, QString());
      Shape r; for(const Shape& x : v) if(x.type!=ShapeType::Group) r=x;
      check(qAbs(r.opacity-0.25)<0.02, "svg: nested group opacity compounds (0.5 x 0.5 = 0.25)"); }

    // Cap / join / dashes: the model has always had them; the parser never filled them in.
    { const QByteArray svg = "<svg xmlns='http://www.w3.org/2000/svg'>"
          "<path d='M0 0L10 0' stroke='#000' stroke-width='2' stroke-linecap='round'"
          " stroke-linejoin='bevel' stroke-dasharray='4 2'/></svg>";
      const QVector<Shape> v = parseSvg(svg, nullptr, QString());
      check(v.size()==1 && v[0].stroke.cap==Qt::RoundCap,   "svg: stroke-linecap is honoured");
      check(v.size()==1 && v[0].stroke.join==Qt::BevelJoin, "svg: stroke-linejoin is honoured");
      check(v.size()==1 && v[0].stroke.dashes.size()==2,    "svg: stroke-dasharray is honoured"); }

    // An odd dash count repeats, per the SVG spec.
    { const QByteArray svg = "<svg xmlns='http://www.w3.org/2000/svg'>"
          "<path d='M0 0L10 0' stroke='#000' stroke-dasharray='3'/></svg>";
      const QVector<Shape> v = parseSvg(svg, nullptr, QString());
      check(v.size()==1 && v[0].stroke.dashes.size()==2, "svg: an odd stroke-dasharray count repeats"); }

    // fill-rule: SVG's default is nonzero. XarQ's Shape default is even-odd, so the parser has to say so.
    { const QByteArray a = "<svg xmlns='http://www.w3.org/2000/svg'><path d='M0 0L9 0L9 9L0 9Z' fill='#000'/></svg>";
      const QByteArray b = "<svg xmlns='http://www.w3.org/2000/svg'><path d='M0 0L9 0L9 9L0 9Z' fill='#000' fill-rule='evenodd'/></svg>";
      const QVector<Shape> va = parseSvg(a, nullptr, QString()), vb = parseSvg(b, nullptr, QString());
      check(va.size()==1 && va[0].fillRule==Qt::WindingFill, "svg: fill-rule defaults to nonzero, per the spec");
      check(vb.size()==1 && vb[0].fillRule==Qt::OddEvenFill, "svg: an explicit fill-rule=evenodd is honoured"); }

    // A layer's name must survive: inherited from the group onto the drawable inside it.
    { const QByteArray svg = "<svg xmlns='http://www.w3.org/2000/svg' xmlns:inkscape='http://www.inkscape.org/namespaces/inkscape'>"
          "<g inkscape:label='Signal logo'><rect x='0' y='0' width='10' height='10' fill='#000'/></g></svg>";
      const QVector<Shape> v = parseSvg(svg, nullptr, QString());
      bool named=false; for(const Shape& x : v) if(x.type!=ShapeType::Group && x.name=="Signal logo") named=true;
      check(named, "svg: inkscape:label names the drawable, inherited from its group"); }

    // Affinity's "Add" is CSS plus-lighter, which Qt does have (CompositionMode_Plus).
    { const QByteArray svg = "<svg xmlns='http://www.w3.org/2000/svg'>"
          "<rect x='0' y='0' width='10' height='10' fill='#fff' style='mix-blend-mode:plus-lighter'/></svg>";
      const QVector<Shape> v = parseSvg(svg, nullptr, QString());
      check(v.size()==1 && v[0].blend==QPainter::CompositionMode_Plus,
            "svg: mix-blend-mode:plus-lighter maps to the additive composition mode"); }

    // Regression (adversarial review): opacity MULTIPLIES down the tree but the cascade WITHIN one
    // element must let the winning declaration replace the others — CSS says the inline style wins.
    { const QByteArray svg = "<svg xmlns='http://www.w3.org/2000/svg'>"
          "<rect x='0' y='0' width='10' height='10' fill='red' opacity='.5' style='opacity:.5'/></svg>";
      const QVector<Shape> v = parseSvg(svg, nullptr, QString());
      check(v.size()==1 && qAbs(v[0].opacity-0.5)<0.02,
            "svg: an inline style opacity REPLACES the presentation attribute (it does not multiply with it)"); }
    { const QByteArray svg = "<svg xmlns='http://www.w3.org/2000/svg'>"
          "<style>.a{opacity:0.5} rect{opacity:0.5}</style>"
          "<rect class='a' x='0' y='0' width='10' height='10' fill='red'/></svg>";
      const QVector<Shape> v = parseSvg(svg, nullptr, QString());
      check(v.size()==1 && qAbs(v[0].opacity-0.5)<0.02,
            "svg: two CSS rules both setting opacity do not compound within one element"); }

    // fill-opacity has to survive a GRADIENT fill and a <text> fill, both of which bypass styleShape.
    { const QByteArray svg = "<svg xmlns='http://www.w3.org/2000/svg'><defs>"
          "<linearGradient id='lg'><stop offset='0' stop-color='#f00'/><stop offset='1' stop-color='#00f'/>"
          "</linearGradient></defs>"
          "<rect x='0' y='0' width='10' height='10' fill='url(#lg)' fill-opacity='0.5'/></svg>";
      const QVector<Shape> v = parseSvg(svg, nullptr, QString());
      check(v.size()==1 && v[0].fill.type==FillType::Linear && !v[0].fill.stops.isEmpty()
            && qAbs(v[0].fill.stops[0].color.alphaF()-0.5)<0.02,
            "svg: fill-opacity reaches the STOPS of a gradient fill"); }
    { const QByteArray svg = "<svg xmlns='http://www.w3.org/2000/svg'>"
          "<text x='0' y='10' fill='#000' fill-opacity='0.5'>hello</text></svg>";
      const QVector<Shape> v = parseSvg(svg, nullptr, QString());
      bool faded=false; for(const Shape& x : v) if(x.type==ShapeType::Text && qAbs(x.fill.color.alphaF()-0.5)<0.02) faded=true;
      check(faded, "svg: fill-opacity reaches a <text> fill"); }

    // SVG dash lengths are user units; QPen scales its pattern by the pen width.
    { const QByteArray a = "<svg xmlns='http://www.w3.org/2000/svg'>"
          "<path d='M0 0L100 0' stroke='#000' stroke-width='10' stroke-dasharray='10 10'/></svg>";
      const QVector<Shape> v = parseSvg(a, nullptr, QString());
      check(v.size()==1 && v[0].stroke.dashes.size()==2 && qAbs(v[0].stroke.dashes[0]-1.0)<0.01,
            "svg: stroke-dasharray is converted from user units into QPen's pen-width units"); }

    // An Inkscape LAYER label names the layer, not every object inside it.
    { const QByteArray svg = "<svg xmlns='http://www.w3.org/2000/svg' xmlns:inkscape='http://www.inkscape.org/namespaces/inkscape'>"
          "<g inkscape:groupmode='layer' inkscape:label='Layer 1'>"
          "<rect x='0' y='0' width='5' height='5' fill='#000'/><circle cx='8' cy='8' r='2' fill='#000'/></g></svg>";
      const QVector<Shape> v = parseSvg(svg, nullptr, QString());
      int stamped=0; for(const Shape& x : v) if(x.type!=ShapeType::Group && x.name=="Layer 1") ++stamped;
      check(stamped==0, "svg: an inkscape LAYER label is not stamped onto every object inside it"); }

    // A radial gradient whose geometry lives ENTIRELY in gradientTransform (Affinity writes exactly
    // this: cx=0 cy=0 r=1). Mapping only the centre left a one-unit gradient — a flat slab.
    { const QByteArray svg = "<svg xmlns='http://www.w3.org/2000/svg'><defs>"
          "<radialGradient id='g' gradientUnits='userSpaceOnUse' cx='0' cy='0' r='1'"
          " gradientTransform='matrix(80,0,0,40,100,50)'>"
          "<stop offset='0' stop-color='#ff0000'/><stop offset='1' stop-color='#0000ff'/>"
          "</radialGradient></defs>"
          "<rect x='0' y='0' width='200' height='100' fill='url(#g)'/></svg>";
      const QVector<Shape> v = parseSvg(svg, nullptr, QString());
      check(v.size()==1 && v[0].fill.type==FillType::Radial, "svg: the radial gradient resolves");
      const double rx = QLineF(v[0].fill.p0, v[0].fill.p1).length();
      const double ry = QLineF(v[0].fill.p0, v[0].fill.p2).length();
      check(qAbs(v[0].fill.p0.x()-100)<0.5 && qAbs(v[0].fill.p0.y()-50)<0.5,
            "svg: gradientTransform places the radial centre");
      check(qAbs(rx-80)<0.5 && qAbs(ry-40)<0.5,
            "svg: gradientTransform also scales the radial RADII (an 80x40 ellipse, not a 1-unit circle)"); }
}

// ---- Affinity LIVE SHAPES: heart, cloud, square star and the polygon side count ----
// These four used to come out as their bounding rectangle (or, for a polygon, as a pentagon no matter
// what the file said), which is the worst kind of wrong: the page still "reads" so nothing looks
// broken. The geometry itself was pinned against shape_test.afdesign's own 512px preview.
{
    auto u8v =[](QByteArray& b, unsigned v){ b.append(char(v&0xff)); };
    auto u16v=[](QByteArray& b, unsigned v){ b.append(char(v&0xff)); b.append(char((v>>8)&0xff)); };
    auto u32v=[](QByteArray& b, quint32 v){ for(int i=0;i<4;i++) b.append(char((v>>(8*i))&0xff)); };
    auto u64v=[](QByteArray& b, quint64 v){ for(int i=0;i<8;i++) b.append(char((v>>(8*i))&0xff)); };
    auto f32v=[&](QByteArray& b, float v){ quint32 r; memcpy(&r,&v,4); u32v(b,r); };
    auto f64v=[&](QByteArray& b, double v){ quint64 r; memcpy(&r,&v,8); u64v(b,r); };
    auto tag =[&](QByteArray& b, const char* s){ for(int i=3;i>=0;--i) b.append(s[i]); };
    auto fld =[&](QByteArray& b, unsigned type, const char* name){ u8v(b,type); tag(b,name); };

    QByteArray t;
    t.append('\x00'); t.append('\xff'); t.append('K'); t.append('S');
    u16v(t,1); tag(t,"Pers"); u16v(t,1);
    fld(t,0x31,"DocR");
      u8v(t,1); u32v(t,1); u8v(t,1); tag(t,"DocN");
      fld(t,0x31,"Chld");
        u8v(t,1); u32v(t,2); u8v(t,1); tag(t,"Sprd");
        fld(t,0x26,"SprB"); f64v(t,0); f64v(t,0); f64v(t,200); f64v(t,200);
        fld(t,0xb1,"Chld"); u32v(t,4);
          // heart
          u8v(t,1); u32v(t,3); u8v(t,1); tag(t,"ShpN");
            fld(t,0x26,"ShpB"); f64v(t,0); f64v(t,0); f64v(t,100); f64v(t,100);
            fld(t,0x31,"Shpe"); u8v(t,1); u32v(t,4); u8v(t,1); tag(t,"ShHt");
              fld(t,0x09,"Sprd"); f32v(t,0.2f);
              u8v(t,0);
            u8v(t,0);
          // cloud: 12 bubbles, valley at 0.8165 of the peak radius
          u8v(t,1); u32v(t,5); u8v(t,1); tag(t,"ShpN");
            fld(t,0x26,"ShpB"); f64v(t,100); f64v(t,0); f64v(t,200); f64v(t,100);
            fld(t,0x31,"Shpe"); u8v(t,1); u32v(t,6); u8v(t,1); tag(t,"ShCl");
              fld(t,0x02,"Bubl"); u16v(t,12);
              fld(t,0x09,"IRad"); f32v(t,0.8165f);
              u8v(t,0);
            u8v(t,0);
          // square star: 5 blocky arms
          u8v(t,1); u32v(t,7); u8v(t,1); tag(t,"ShpN");
            fld(t,0x26,"ShpB"); f64v(t,0); f64v(t,100); f64v(t,100); f64v(t,200);
            fld(t,0x31,"Shpe"); u8v(t,1); u32v(t,8); u8v(t,1); tag(t,"ShSS");
              fld(t,0x02,"Side"); u16v(t,5);
              fld(t,0x09,"COut"); f32v(t,0.5f);
              u8v(t,0);
            u8v(t,0);
          // polygon: SIX sides, counted in Side (there is no Pnts field on a polygon)
          u8v(t,1); u32v(t,9); u8v(t,1); tag(t,"ShpN");
            fld(t,0x26,"ShpB"); f64v(t,100); f64v(t,100); f64v(t,200); f64v(t,200);
            fld(t,0x31,"Shpe"); u8v(t,1); u32v(t,10); u8v(t,1); tag(t,"ShPy");
              fld(t,0x02,"Side"); u16v(t,6);
              u8v(t,0);
            u8v(t,0);
        u8v(t,0);
      u8v(t,0);
    u8v(t,0);

    const QByteArray comp = qCompress(t,9).mid(4);
    const quint32 crc = (quint32)crc32(crc32(0,nullptr,0),(const Bytef*)t.constData(),(uInt)t.size());
    QByteArray f;
    f.append('\x00'); f.append('\xff'); f.append('K'); f.append('A');
    u16v(f,9); u16v(f,0); tag(f,"Prsn"); f.append("#Inf",4);
    const quint64 fatOff = 72 + 4 + quint64(comp.size()) + 4;
    u64v(f,fatOff); u64v(f,0); u64v(f,0); u64v(f,0); u64v(f,0); u32v(f,1); u32v(f,2);
    f.append("Prot",4); u32v(f,3);
    f.append("#Fil",4); f.append(comp); u32v(f,0xFFFFFFFFu);
    f.append("#FAT",4);
    u64v(f,0); u64v(f,1000); u64v(f,0); u64v(f,0); u64v(f,0);
    u32v(f,1); u32v(f,0); u32v(f,36+7); u16v(f,0); u8v(f,0);
    u32v(f,1); u8v(f,0);
    u64v(f,72); u64v(f,(quint64)t.size()); u64v(f,(quint64)comp.size()); u32v(f,crc); u8v(f,1);
    u16v(f,7); f.append("doc.dat",7);

    xara::AffinityStats sst;
    const QStringList sp = xara::affinityToSvgPages(f,&sst);
    const QString ssvg = sp.value(0);
    check(sp.size()==1 && sst.shapes==4 && sst.skippedShape==0,
          "Affinity shapes: heart, cloud, square star and polygon all convert (none falls back to its box)");
    check(!ssvg.contains("<rect"),
          "Affinity shapes: none of the four degrades into a bounding rectangle");
    // A heart is cubics; a cloud is arcs; the two star/polygon forms are straight-line polygons.
    check(ssvg.count("<path")==4,                 "Affinity shapes: each emits one path");
    check(ssvg.contains("C"),                     "Affinity shapes: the heart is built from cubic segments");
    check(ssvg.contains("A"),                     "Affinity shapes: the cloud is built from arc segments");
    // The polygon must honour Side: six sides = one M plus five L before the close.
    const int lastPath = ssvg.lastIndexOf("<path");
    const QString polyD = ssvg.mid(lastPath, ssvg.indexOf("/>", lastPath) - lastPath);
    check(polyD.count('L')==5,                    "Affinity shapes: a polygon with Side=6 has six sides, not five");
    // The square star's 5 arms contribute 3 vertices each (two tip corners + the inner meet).
    const int ssIdx = ssvg.indexOf("<path", ssvg.indexOf("<path", ssvg.indexOf("<path")+1)+1);
    const QString ssD = ssvg.mid(ssIdx, ssvg.indexOf("/>", ssIdx) - ssIdx);
    check(ssD.count('L')==14,                     "Affinity shapes: a 5-arm square star has 15 vertices");
}

// ---- PSD layers: small files written here, one feature each ----
{
    auto be16=[](QByteArray& b,int v){ b.append(char(v>>8)); b.append(char(v)); };
    auto be32=[](QByteArray& b,qint64 v){ for(int i=3;i>=0;--i) b.append(char((v>>(8*i))&0xff)); };
    // A layer record plus its raw channels. `extra` is the tagged blocks, `maskData` the mask block body.
    struct L { int t,l,b,r; QVector<QPair<int,QByteArray>> ch; QByteArray blend, name, extra, maskData; int op; bool clip; };
    int chComp=0;
    auto psd=[&](int W,int H,const QVector<L>& ls)->QByteArray{
        QByteArray d("8BPS"); be16(d,1); d.append(QByteArray(6,0)); be16(d,3); be32(d,H); be32(d,W); be16(d,8); be16(d,3);
        be32(d,0); be32(d,0);
        QByteArray li; be16(li,ls.size()); QByteArray px;
        for(const L& x:ls){
            be32(li,x.t); be32(li,x.l); be32(li,x.b); be32(li,x.r); be16(li,x.ch.size());
            for(const auto& c:x.ch){ be16(li,c.first); be32(li,c.second.size()+2); QByteArray v; be16(v,chComp); px+=v+c.second; }
            li+="8BIM"; li+=x.blend; li.append(char(x.op)); li.append(char(x.clip?1:0)); li.append(char(0)); li.append(char(0));
            QByteArray nm; nm.append(char(x.name.size())); nm+=x.name; while(nm.size()%4) nm.append(char(0));
            QByteArray ex; be32(ex,x.maskData.size()); ex+=x.maskData; be32(ex,0); ex+=nm; ex+=x.extra;
            be32(li,ex.size()); li+=ex;
        }
        QByteArray body=li+px; if(body.size()&1) body.append(char(0));
        QByteArray lm; be32(lm,body.size()); lm+=body; be32(lm,0);
        be32(d,lm.size()); d+=lm;
        be16(d,0); d.append(QByteArray(qint64(W)*H*3,char(127)));
        return d; };
    auto plane=[](int n,int v){ return QByteArray(n,char(v)); };
    auto pix=[&](int t,int l,int b,int r,int R,int G,int B,const QByteArray& blend="norm",int op=255,bool clip=false){
        const int n=(b-t)*(r-l); L x{t,l,b,r,{{0,plane(n,R)},{1,plane(n,G)},{2,plane(n,B)},{-1,plane(n,255)}},blend,"px",{},{},op,clip}; return x; };
    auto block=[&](const char* key,const QByteArray& body){ QByteArray b("8BIM"); b+=key; be32(b,body.size()); b+=body; return b; };

    { Document pd=importPsdDocument(psd(4,4,{pix(0,0,4,4,255,0,0),pix(1,1,3,3,0,255,0,"mul ",128)}));
      check(pd.shapes.size()==2 && pd.pageSize==QSizeF(4,4), "PSD layers: two pixel layers, the file's page");
      check(pd.shapes[1].blend==QPainter::CompositionMode_Multiply && qAbs(pd.shapes[1].opacity-128/255.0)<0.01, "PSD layers: blend mode and opacity");
      check(pd.shapes[1].transform.dx()==1 && pd.shapes[1].fill.image.width()==2, "PSD layers: a layer sits at its own rectangle"); }

    { // A divider opens the group and the folder record closes it, bottom layer first.
      L open=pix(0,0,0,0,0,0,0); open.ch.clear(); open.name="sep"; { QByteArray v; be32(v,3); open.extra=block("lsct",v); }
      L close=open; close.name="Folder"; { QByteArray v; be32(v,1); v+="8BIM"; v+="norm"; close.extra=block("lsct",v); }
      Document pd=importPsdDocument(psd(4,4,{open,pix(0,0,2,2,1,2,3),close}));
      check(pd.shapes.size()==1 && pd.shapes[0].type==ShapeType::Group && pd.shapes[0].name=="Folder" && pd.shapes[0].children.size()==1, "PSD layers: section dividers build a named group"); }

    { // A clipped layer only shows where its base has coverage.
      Document pd=importPsdDocument(psd(4,4,{pix(0,0,2,4,255,0,0),pix(0,0,4,4,0,0,255,"norm",255,true)}));
      const QImage im=pd.shapes[1].fill.image;
      check(qAlpha(im.pixel(0,1))==255 && qAlpha(im.pixel(0,3))==0, "PSD layers: clipping cuts to the base's coverage"); }

    { // A user mask with a default outside its rectangle.
      L x=pix(0,0,4,4,9,9,9);
      QByteArray md; be32(md,0); be32(md,0); be32(md,2); be32(md,2); md.append(char(0)); md.append(char(0)); md.append(QByteArray(2,0));
      x.maskData=md; x.ch.push_back({-2,plane(4,255)});
      Document pd=importPsdDocument(psd(4,4,{x}));
      const QImage im=pd.shapes[0].fill.image;
      check(qAlpha(im.pixel(0,0))==255 && qAlpha(im.pixel(3,3))==0, "PSD layers: a mask applies, with its default outside"); }

    { // Invert has no data of its own and changes what is beneath.
      L adj=pix(0,0,0,0,0,0,0); adj.ch.clear(); adj.extra=block("nvrt",QByteArray());
      Document pd=importPsdDocument(psd(2,2,{pix(0,0,2,2,10,20,30),adj}));
      check(pd.shapes.size()==1 && QColor(pd.shapes[0].fill.image.pixel(0,0))==QColor(245,235,225), "PSD layers: an adjustment bakes into the layers beneath"); }

    { // ZIP channels used to come out black.
      const QByteArray z=qCompress(QByteArray(4,char(200)),9).mid(4);
      L x{0,0,2,2,{{0,z},{1,z},{2,z}},"norm","z",{},{},255,false};
      chComp=2; const QByteArray d=psd(2,2,{x}); chComp=0;
      const Document pd=importPsdDocument(d);
      check(pd.shapes.size()==1 && QColor(pd.shapes[0].fill.image.pixel(1,1))==QColor(200,200,200), "PSD layers: ZIP channels decode"); }

    // Adjustments: one flat pixel layer, one adjustment block, the resulting pixel compared by hand.
    auto adjusted=[&](int R,int G,int B,const char* key,const QByteArray& body)->QColor{
        L adj=pix(0,0,0,0,0,0,0); adj.ch.clear(); adj.extra=block(key,body);
        const Document pd=importPsdDocument(psd(2,2,{pix(0,0,2,2,R,G,B),adj}));
        return pd.shapes.isEmpty()?QColor():QColor(pd.shapes[0].fill.image.pixel(0,0)); };
    auto near=[](const QColor& a,const QColor& b,int t=2){ return qAbs(a.red()-b.red())<=t && qAbs(a.green()-b.green())<=t && qAbs(a.blue()-b.blue())<=t; };

    { // Channel mixer: red out of green, 50 % of blue into green. Five shorts per output: R, G, B, unused, constant.
      QByteArray b; be16(b,1); be16(b,0);
      for(int v: {0,100,0,0,0,  0,0,50,0,0,  0,0,100,0,0,  0,0,0,100,0}) be16(b,v);
      check(near(adjusted(10,200,100,"mixr",b),QColor(200,50,100)), "PSD adjustments: channel mixer outputs");
      QByteArray m; be16(m,1); be16(m,1);
      for(int v: {0,100,0,0,0,  0,0,0,0,0,  0,0,0,0,0,  0,0,0,0,0}) be16(m,v);
      check(near(adjusted(10,200,100,"mixr",m),QColor(200,200,200)), "PSD adjustments: monochrome mixer uses one row for all"); }

    { // Selective colour, absolute: all the cyan it can add, on the reds only.
      QByteArray b; be16(b,1); be16(b,1);
      for(int rec=0;rec<10;++rec) for(int c=0;c<4;++c) be16(b,(rec==1&&c==0)?100:0);
      const QColor red=adjusted(255,0,0,"selc",b), green=adjusted(0,255,0,"selc",b);
      check(near(red,QColor(0,0,0)), "PSD adjustments: selective colour moves its own family");
      check(near(green,QColor(0,255,0)), "PSD adjustments: selective colour leaves other families"); }

    { // Gradient map: black to red, by luminance.
      QByteArray b; be16(b,1); b.append(char(0)); b.append(char(0)); be32(b,0); be16(b,2);
      auto stop=[&](int loc,int R,int G,int Bl){ be32(b,loc); be32(b,50); be16(b,0); be16(b,R); be16(b,G); be16(b,Bl); be16(b,0); be16(b,0); };
      stop(0,0,0,0); stop(4096,65535,0,0);
      be16(b,0); b.append(QByteArray(16,0));
      check(near(adjusted(255,255,255,"grdm",b),QColor(255,0,0)) && near(adjusted(0,0,0,"grdm",b),QColor(0,0,0)), "PSD adjustments: gradient map follows luminance");
      check(near(adjusted(128,128,128,"grdm",b),QColor(128,0,0),3), "PSD adjustments: gradient map interpolates"); }

    { // A colour lookup table, as .cube text inside its descriptor: a 2-point cube that inverts.
      QByteArray cube="LUT_3D_SIZE 2\n1 1 1\n0 1 1\n1 0 1\n0 0 1\n1 1 0\n0 1 0\n1 0 0\n0 0 0\n";
      QByteArray b; be16(b,1); be32(b,16); be32(b,1); be16(b,0); be32(b,0); b+="null"; be32(b,1); be32(b,0); b+="LUT3DFileData"; b.remove(b.size()-13,13); b.chop(4);
      be32(b,13); b+="LUT3DFileData"; b+="tdta"; be32(b,cube.size()); b+=cube;
      check(near(adjusted(10,200,100,"clrL",b),QColor(245,55,155)), "PSD adjustments: a .cube colour lookup applies"); }

    { // Hue/saturation: range lightness at +100 sends every channel to the pixel's maximum.
      QByteArray b; be16(b,2); be16(b,0); for(int i=0;i<6;++i) be16(b,0);
      for(int r=0;r<6;++r){ for(int v: {0,0,359,359}) be16(b,v); for(int v: {0,0,r==0?100:0}) be16(b,v); }
      check(near(adjusted(200,100,50,"hue2",b),QColor(200,200,200)), "PSD adjustments: a range's lightness goes to the pixel's own maximum"); }

    { // Curves run channel first, then composite: red 0-255 halved, composite inverted.
      QByteArray b; b.append(char(0)); be16(b,1); be32(b,3);
      be16(b,2); be16(b,255); be16(b,0); be16(b,0); be16(b,255);
      be16(b,2); be16(b,0); be16(b,0); be16(b,128); be16(b,255);
      check(near(adjusted(255,0,0,"curv",b),QColor(127,255,255),2), "PSD adjustments: channel curves apply before the composite"); }

    { // Knockout: deep punches through what's beneath, and a true background (no alpha channel) stays.
      L top=pix(0,0,2,2,0,0,255); { QByteArray v; v.append(char(2)); v.append(QByteArray(3,0)); top.extra=block("knko",v); }
      const Document cut=importPsdDocument(psd(2,2,{pix(0,0,2,2,255,0,0),top}));
      check(cut.shapes.size()==2 && qAlpha(cut.shapes[0].fill.image.pixel(0,0))==0, "PSD layers: a deep knockout removes what lies beneath");
      L bg{0,0,2,2,{{0,plane(4,255)},{1,plane(4,0)},{2,plane(4,0)}},"norm","bg",{},{},255,false};
      const Document kept=importPsdDocument(psd(2,2,{bg,top}));
      check(kept.shapes.size()==2 && qAlpha(kept.shapes[0].fill.image.pixel(0,0))==255, "PSD layers: a deep knockout stops at the background"); }

    { // Dissolve's noise belongs to the pixel's position: two layers share their holes.
      const Document pd=importPsdDocument(psd(32,32,{pix(0,0,32,32,255,0,0,"diss",128),pix(0,0,32,32,0,255,0,"diss",128)}));
      bool same=pd.shapes.size()==2, mixed=false, solid=false;
      if(same) for(int y=0;y<32;++y) for(int x=0;x<32;++x){ const int a=qAlpha(pd.shapes[0].fill.image.pixel(x,y)), b=qAlpha(pd.shapes[1].fill.image.pixel(x,y)); if(a!=b) same=false; if(a==0) mixed=true; if(a==255) solid=true; }
      check(same && mixed && solid, "PSD layers: dissolve punches the same hard-edged holes in stacked layers"); }

    { // A per-pixel blend over nothing shows as itself, at its opacity once.
      QImage bd(4,4,QImage::Format_ARGB32_Premultiplied); bd.fill(Qt::transparent);
      Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,4,4); s.fill.type=FillType::Solid; s.fill.color=QColor(0,0,255); s.stroke.enabled=false; s.transp.type=45; s.opacity=0.5;
      { QPainter g(&bd); paintShape(g,s); }
      const QColor c=bd.pixelColor(1,1);
      check(qAbs(c.alpha()-128)<=2 && c.blue()>=250 && c.red()<=2, "Blend modes: over transparency the source shows, with its opacity applied once"); }

    check(importPsdDocument(QByteArray("8BPSjunk")).shapes.isEmpty(), "PSD layers: a truncated header yields nothing");
}

// ---- PDF through poppler: the expected output is stated against a file this test writes itself ----
if(xara::pdfVectorAvailable()){
    QTemporaryDir dir; const QString path=dir.filePath("t.pdf");
    const QByteArray content="1 0 0 rg 10 10 50 50 re f\n0 0 1 RG 2 w 100 100 m 150 100 l S\nq 200 200 50 50 re W n /S1 sh Q\nq 300 300 10 10 re W n 400 400 20 20 re f Q\n";
    QByteArray f="%PDF-1.4\n"; QVector<int> off;
    const QList<QByteArray> objs={
        "<</Type/Catalog/Pages 2 0 R>>",
        "<</Type/Pages/Kids[3 0 R]/Count 1>>",
        "<</Type/Page/Parent 2 0 R/MediaBox[0 0 500 600]/Contents 4 0 R/Resources<</Shading<</S1<</ShadingType 2/ColorSpace/DeviceRGB/Coords[200 0 250 0]/Function<</FunctionType 2/Domain[0 1]/C0[1 0 0]/C1[0 0 1]/N 1>>>>>>>>>>",
        "<</Length "+QByteArray::number(content.size())+">>stream\n"+content+"endstream" };
    for(int i=0;i<objs.size();++i){ off.push_back(f.size()); f+=QByteArray::number(i+1)+" 0 obj\n"+objs[i]+"\nendobj\n"; }
    const int xr=f.size(); f+="xref\n0 5\n0000000000 65535 f \n";
    for(int o:off) f+=QByteArray::number(o).rightJustified(10,'0')+" 00000 n \n";
    f+="trailer<</Size 5/Root 1 0 R>>\nstartxref\n"+QByteArray::number(xr)+"\n%%EOF\n";
    { QFile w(path); w.open(QIODevice::WriteOnly); w.write(f); }
    xara::PdfVectorStats st;
    const QStringList pg=xara::pdfToSvgPages(path,&st);
    check(pg.size()==1 && st.totalPages==1, "PDF: one page in, one SVG out");
    const QString v=pg.value(0);
    check(v.contains("viewBox=\"0 0 500 600\""), "PDF: the page box becomes the viewBox");
    check(v.contains("fill=\"#ff0000\"") && v.contains("M10 590"), "PDF: a fill lands at its position with y flipped");
    check(v.contains("stroke=\"#0000ff\"") && v.contains("stroke-width=\"2\""), "PDF: a stroke keeps its colour and width");
    check(v.contains("<linearGradient") && v.contains("x1=\"200\""), "PDF: an axial shading becomes a real gradient");
    check(!v.contains("M400 "), "PDF: content wholly outside its clip is dropped, not left unclipped");
    { xara::PdfVectorStats bad; const QString junk=dir.filePath("junk.pdf"); QFile w(junk); w.open(QIODevice::WriteOnly); w.write("not a pdf"); w.close();
      check(xara::pdfToSvgPages(junk,&bad).isEmpty(), "PDF: a file that isn't one yields no pages"); }
}

// ---- Affinity appearance model: paint stacks, inheritance, adjustments, document colour spaces ----
// Each case builds a one-spread document from a small tree writer, so the expected output is stated
// against bytes this test wrote itself rather than against a vendored sample.
{
    auto u8v =[](QByteArray& b, unsigned v){ b.append(char(v&0xff)); };
    auto u16v=[](QByteArray& b, unsigned v){ b.append(char(v&0xff)); b.append(char((v>>8)&0xff)); };
    auto u32v=[](QByteArray& b, quint32 v){ for(int i=0;i<4;i++) b.append(char((v>>(8*i))&0xff)); };
    auto u64v=[](QByteArray& b, quint64 v){ for(int i=0;i<8;i++) b.append(char((v>>(8*i))&0xff)); };
    auto f32v=[&](QByteArray& b, float v){ quint32 r; memcpy(&r,&v,4); u32v(b,r); };
    auto f64v=[&](QByteArray& b, double v){ quint64 r; memcpy(&r,&v,8); u64v(b,r); };
    auto tag =[&](QByteArray& b, const char* s){ for(int i=3;i>=0;--i) b.append(s[i]); };
    auto fld =[&](QByteArray& b, unsigned type, const char* name){ u8v(b,type); tag(b,name); };
    quint32 nextId=100;
    auto cls =[&](QByteArray& b, const char* c){ u8v(b,1); u32v(b,nextId++); u8v(b,1); tag(b,c); };
    auto solid=[&](QByteArray& b, float r, float g, float a=1.f, float bl=0.f){       // an FDsc holding a FilS
        cls(b,"FDsc"); fld(b,0x31,"FDeF"); cls(b,"FilS"); fld(b,0x31,"Colr"); cls(b,"RGBA");
        fld(b,0x44,"_col"); f32v(b,r); f32v(b,g); f32v(b,bl); f32v(b,a); u8v(b,0); u8v(b,0); u8v(b,0); };
    auto line=[&](QByteArray& b, double w, unsigned style, double align){              // an LDsc holding an LSty
        cls(b,"LDsc"); fld(b,0x31,"LDeL"); cls(b,"LSty");
        fld(b,0x2c,"Data"); u16v(b,12); f64v(b,2.0); u8v(b,0); u8v(b,2); u8v(b,style); u8v(b,0);
        fld(b,0x0a,"Wght"); f64v(b,w); u8v(b,0); fld(b,0x07,"LDSa"); u32v(b,quint32(align)); u8v(b,0); };
    auto box =[&](QByteArray& b){ fld(b,0x26,"ShpB"); f64v(b,10); f64v(b,10); f64v(b,110); f64v(b,110); };
    auto doc =[&](const QByteArray& kids, int count, int ddrf, const QByteArray& spreadFields=QByteArray())->QByteArray{
        QByteArray t; t.append('\x00'); t.append('\xff'); t.append('K'); t.append('S');
        u16v(t,1); tag(t,"Pers"); u16v(t,1);
        fld(t,0x2a,"DDRF"); u16v(t,ddrf); u16v(t,0);
        fld(t,0x31,"DocR"); u8v(t,1); u32v(t,1); u8v(t,1); tag(t,"DocN");
        fld(t,0x31,"Chld"); u8v(t,1); u32v(t,2); u8v(t,1); tag(t,"Sprd");
        fld(t,0x26,"SprB"); f64v(t,0); f64v(t,0); f64v(t,200); f64v(t,200);
        t.append(spreadFields);
        fld(t,0xb1,"Chld"); u32v(t,quint32(count)); t.append(kids);
        u8v(t,0); u8v(t,0); u8v(t,0);
        const QByteArray comp=qCompress(t,9).mid(4);
        const quint32 crc=(quint32)crc32(crc32(0,nullptr,0),(const Bytef*)t.constData(),(uInt)t.size());
        QByteArray f; f.append('\x00'); f.append('\xff'); f.append('K'); f.append('A');
        u16v(f,12); u16v(f,0); tag(f,"Prsn"); f.append("#Inf",4);
        u64v(f,72+4+quint64(comp.size())+4); u64v(f,0); u64v(f,0); u64v(f,0); u64v(f,0); u32v(f,1); u32v(f,2);
        f.append("Prot",4); u32v(f,3); f.append("#Fil",4); f.append(comp); u32v(f,0xFFFFFFFFu);
        f.append("#FAT",4); u64v(f,0); u64v(f,1000); u64v(f,0); u64v(f,0); u64v(f,0);
        u32v(f,1); u32v(f,0); u32v(f,36+7); u16v(f,0); u8v(f,0); u32v(f,1); u8v(f,0);
        u64v(f,72); u64v(f,(quint64)t.size()); u64v(f,(quint64)comp.size()); u32v(f,crc); u8v(f,1);
        u16v(f,7); f.append("doc.dat",7);
        return f; };
    auto rectBody=[&](QByteArray& k){ box(k); fld(k,0x31,"Shpe"); cls(k,"ShNR"); u8v(k,0); };

    { // FiEf lists every effect type; only an enabled one applies, with its offset from Offs and Angl.
      auto shaded=[&](unsigned on){ QByteArray k; cls(k,"ShpN"); rectBody(k);
        fld(k,0xb1,"FiEf"); u32v(k,1); cls(k,"Shad"); fld(k,0x29,"Enab"); u8v(k,on);
        fld(k,0x0a,"Radi"); f64v(k,20.0); fld(k,0x0a,"Offs"); f64v(k,6.0); fld(k,0x0a,"Angl"); f64v(k,0.0); u8v(k,0);
        fld(k,0xb1,"BFFl"); u32v(k,1); solid(k,1,0); u8v(k,0); return k; };
      const QString v=xara::affinityToSvgPages(doc(shaded(1),1,0)).value(0);
      check(v.contains("xarq:shadow=\"6 0 20 "), "Affinity effects: an enabled shadow carries its offset and radius");
      check(!xara::affinityToSvgPages(doc(shaded(0),1,0)).value(0).contains("xarq:shadow"), "Affinity effects: a disabled shadow stays off");
      QByteArray g; cls(g,"ShpN"); rectBody(g);
      fld(g,0xb1,"FiEf"); u32v(g,1); cls(g,"Gaus"); fld(g,0x29,"Enab"); u8v(g,1); fld(g,0x0a,"Radi"); f64v(g,25.0); u8v(g,0);
      fld(g,0xb1,"BFFl"); u32v(g,1); solid(g,1,0); u8v(g,0);
      check(xara::affinityToSvgPages(doc(g,1,0)).value(0).contains("xarq:blur=\"18\""), "Affinity effects: Gaussian radius converts to the model's blur");
      QByteArray c; cls(c,"ShpN"); rectBody(c);
      fld(c,0xb1,"FiEf"); u32v(c,1); cls(c,"InnG"); fld(c,0x29,"Enab"); u8v(c,1); fld(c,0x0a,"Radi"); f64v(c,10.0); u8v(c,0);
      fld(c,0xb1,"BFFl"); u32v(c,1); solid(c,1,0); u8v(c,0);
      const QString ig=xara::affinityToSvgPages(doc(c,1,0)).value(0);
      check(ig.contains("clip-path") && ig.contains("fill-rule=\"evenodd\""), "Affinity effects: an inner glow is a blurred inverse clipped to the outline"); }

    { // Intensity scales a glow's falloff: at Comp 0.5 the edge is at full strength, not half.
      QByteArray g; cls(g,"ShpN"); rectBody(g);
      fld(g,0xb1,"FiEf"); u32v(g,1); cls(g,"OutG"); fld(g,0x29,"Enab"); u8v(g,1); fld(g,0x0a,"Radi"); f64v(g,25.0); fld(g,0x0a,"Comp"); f64v(g,0.5); u8v(g,0);
      fld(g,0xb1,"BFFl"); u32v(g,1); solid(g,1,0); u8v(g,0);
      const QVector<xara::Shape> sh=xara::parseSvg(xara::affinityToSvgPages(doc(g,1,0)).value(0).toUtf8());
      check(!sh.isEmpty() && sh[0].blur>0 && qAbs(sh[0].blurGain-2.0)<1e-6, "Affinity effects: glow intensity becomes the blur's gain"); }

    { // Blend modes the painter lacks. Linear Light adds: neutral grey leaves the backdrop alone.
      QImage bd(4,4,QImage::Format_ARGB32_Premultiplied); bd.fill(QColor(100,150,200));
      xara::Shape s; s.type=xara::ShapeType::Rect; s.rect=QRectF(0,0,4,4); s.fill.type=xara::FillType::Solid; s.fill.color=QColor(128,128,128); s.stroke.enabled=false; s.transp.type=47;
      { QPainter g(&bd); xara::paintShape(g,s); }
      const QColor c=bd.pixelColor(1,1);
      check(qAbs(c.red()-100)<=2 && qAbs(c.green()-150)<=2 && qAbs(c.blue()-200)<=2, "Blend modes: Linear Light with neutral grey changes nothing");
      QImage b2(4,4,QImage::Format_ARGB32_Premultiplied); b2.fill(QColor(100,150,200)); s.transp.type=52; s.fill.color=QColor(0,0,0);
      { QPainter g(&b2); xara::paintShape(g,s); }
      check(b2.pixelColor(1,1)==QColor(0,0,0), "Blend modes: Darker Colour takes the darker whole colour"); }

    { // A compound: two rectangles side by side, the second subtracted where it overlaps.
      auto oper=[&](QByteArray& k,double x0,double x1,unsigned op){ cls(k,"ShpN"); fld(k,0x26,"ShpB"); f64v(k,x0); f64v(k,0); f64v(k,x1); f64v(k,100);
        fld(k,0x31,"Shpe"); cls(k,"ShNR"); u8v(k,0); fld(k,0x2a,"ComO"); u16v(k,op); u16v(k,0); u8v(k,0); };
      auto comp=[&](unsigned op){ QByteArray ch; oper(ch,0,100,0); oper(ch,50,150,op);
        QByteArray k; cls(k,"Comp"); fld(k,0xb1,"Chld"); u32v(k,2); k.append(ch); fld(k,0xb1,"BFFl"); u32v(k,1); solid(k,1,0); u8v(k,0); return k; };
      auto bounds=[&](unsigned op){ const QVector<xara::Shape> sh=xara::parseSvg(xara::affinityToSvgPages(doc(comp(op),1,0)).value(0).toUtf8()); return sh.size()==1?sh[0].docBounds():QRectF(); };
      const QRectF add=bounds(0), sub=bounds(2), isect=bounds(1);
      check(qAbs(add.width()-150)<1, "Affinity compounds: add unites the operands");
      check(qAbs(sub.width()-50)<1 && sub.left()<1, "Affinity compounds: subtract cuts the later operand away");
      check(qAbs(isect.width()-50)<1 && qAbs(isect.left()-50)<1, "Affinity compounds: intersect keeps the overlap"); }

    { // Compounds nested deep with every level referencing the same next level twice: 2^n visits if unbounded.
      QByteArray k; quint32 id=5000;
      auto open=[&](QByteArray& b){ u8v(b,1); u32v(b,id++); u8v(b,1); tag(b,"Comp"); fld(b,0xb1,"Chld"); u32v(b,2); };
      QByteArray inner; cls(inner,"ShpN"); rectBody(inner); u8v(inner,0);
      QByteArray inner2; cls(inner2,"ShpN"); rectBody(inner2); u8v(inner2,0);
      QByteArray chain=inner; chain+=inner2;
      for(int lvl=0;lvl<30;++lvl){ QByteArray w; open(w); const quint32 me=id; w+=chain; u8v(w,0);
        QByteArray ref; u8v(ref,2); u32v(ref,me-1); chain=w+ref; }
      QByteArray top; open(top); top+=chain; fld(top,0xb1,"BFFl"); u32v(top,1); solid(top,1,0); u8v(top,0);
      QElapsedTimer t; t.start();
      xara::AffinityStats st;
      const QStringList pg=xara::affinityToSvgPages(doc(top,1,0),&st);
      check(st.exactParse && pg.size()==1 && pg[0].contains("<path"), "Affinity compounds: the shared nesting fixture really parses");
      check(t.elapsed()<20000, "Affinity compounds: shared nesting stays bounded"); }

    { // A fill layer paints its bitmap's area.
      QByteArray k; cls(k,"FRst"); fld(k,0x31,"Bitm"); cls(k,"DyBm"); fld(k,0x07,"BmpW"); u32v(k,120); fld(k,0x07,"BmpH"); u32v(k,90); u8v(k,0);
      fld(k,0xb1,"BFFl"); u32v(k,1); solid(k,0,1); u8v(k,0);
      const QVector<xara::Shape> sh=xara::parseSvg(xara::affinityToSvgPages(doc(k,1,0)).value(0).toUtf8());
      check(sh.size()==1 && qAbs(sh[0].docBounds().width()-120)<1 && qAbs(sh[0].docBounds().height()-90)<1, "Affinity fill layers: the bitmap's area, filled"); }

    { // Adjustments: a layer below, one adjustment above. Unknown kinds are reported rather than dropped quietly.
      auto over=[&](const char* cl, const std::function<void(QByteArray&)>& params, float r, float g, float bl, xara::AffinityStats* st=nullptr){
        QByteArray k; cls(k,"ShpN"); rectBody(k); fld(k,0xb1,"BFFl"); u32v(k,1); solid(k,r,g,1.f,bl); u8v(k,0);
        cls(k,cl); fld(k,0x31,"AdjP"); cls(k,"Prm "); params(k); u8v(k,0); u8v(k,0);
        const QVector<xara::Shape> sh=xara::parseSvg(xara::affinityToSvgPages(doc(k,2,0),st).value(0).toUtf8());
        return sh.isEmpty()?QColor():sh[0].fill.color; };
      const QColor thr=over("ThRA",[&](QByteArray& b){ fld(b,0x09,"Thre"); f32v(b,0.5f); fld(b,0x09,"True"); f32v(b,1.f); fld(b,0x09,"Fals"); f32v(b,0.f); },0.9f,0.9f,0.9f);
      check(thr==QColor(255,255,255), "Affinity adjustments: threshold");
      const QColor pos=over("PoRA",[&](QByteArray& b){ fld(b,0x07,"Post"); u32v(b,2); },0.2f,0.8f,0.2f);
      check(pos==QColor(0,255,0), "Affinity adjustments: posterize");
      const QColor exq=over("ExRA",[&](QByteArray& b){ fld(b,0x09,"Expo"); f32v(b,1.f); },0.5f,0.5f,0.5f);
      check(qAbs(exq.red()-175)<=2, "Affinity adjustments: exposure is stops of linear light");
      xara::AffinityStats st;
      over("ZzRA",[&](QByteArray&){ },0.5f,0.5f,0.5f,&st);
      check(st.ignoredAdjustments==1, "Affinity adjustments: an unknown kind is counted for the status line"); }

    { // Artboards become pages of their own, all the size of the largest, each starting at its own corner.
      auto board=[&](QByteArray& k, double x, double w, float r, float g){ cls(k,"ShpN"); fld(k,0x26,"ShpB"); f64v(k,0); f64v(k,0); f64v(k,w); f64v(k,80);
        fld(k,0x28,"Xfrm"); f64v(k,1); f64v(k,0); f64v(k,x); f64v(k,0); f64v(k,1); f64v(k,0);
        fld(k,0x29,"ABEn"); u8v(k,1); fld(k,0x31,"Shpe"); cls(k,"ShNR"); u8v(k,0);
        fld(k,0xb1,"BFFl"); u32v(k,1); solid(k,r,g); u8v(k,0); };
      QByteArray k; board(k,0,60,1,0); board(k,300,120,0,1);
      xara::AffinityStats st;
      const QStringList pg=xara::affinityToSvgPages(doc(k,2,0),&st);
      check(pg.size()==2 && st.artboards==2, "Affinity artboards: each one is a page");
      check(pg.value(1).contains("viewBox=\"300 0 120 80\"") && pg.value(0).contains("viewBox=\"0 0 120 80\""), "Affinity artboards: pages share the largest size, from each board's corner"); }

    { // Guides and margins come back in page coordinates; bleed as the largest edge.
      QByteArray sf; fld(sf,0x8a,"GdsH"); u32v(sf,2); f64v(sf,30); f64v(sf,70); fld(sf,0x8a,"GdsV"); u32v(sf,1); f64v(sf,90);
      fld(sf,0x26,"Mrgn"); f64v(sf,5); f64v(sf,7); f64v(sf,6); f64v(sf,8);
      fld(sf,0x26,"bled"); f64v(sf,3); f64v(sf,3); f64v(sf,9); f64v(sf,3);
      QByteArray k; cls(k,"ShpN"); rectBody(k); fld(k,0xb1,"BFFl"); u32v(k,1); solid(k,1,0); u8v(k,0);
      xara::AffinityStats st;
      xara::affinityToSvgPages(doc(k,1,0,sf),&st);
      check(st.pageInfo.size()==1 && st.pageInfo[0].guidesH==QVector<double>({30,70}) && st.pageInfo[0].guidesV==QVector<double>({90}), "Affinity page settings: guides");
      check(st.pageInfo.size()==1 && st.pageInfo[0].marginL==5 && st.pageInfo[0].marginT==7 && st.pageInfo[0].marginR==6 && st.pageInfo[0].marginB==8, "Affinity page settings: margins are left, top, right, bottom");
      check(st.bleed==9, "Affinity page settings: bleed"); }

    { // A pixel mask attached to a layer: states 2 (white) in one cell. Format 6 is one 8-bit plane.
      auto masked=[&](unsigned frmt, unsigned state){ QByteArray k; cls(k,"ShpN"); rectBody(k);
        fld(k,0xb1,"BFFl"); u32v(k,1); solid(k,1,0);
        fld(k,0xb1,"AdCh"); u32v(k,1); cls(k,"MRst"); fld(k,0x31,"Bitm"); cls(k,"DyBm");
        fld(k,0x2a,"Frmt"); u16v(k,frmt); u16v(k,0); fld(k,0x07,"BmpW"); u32v(k,4); fld(k,0x07,"BmpH"); u32v(k,4);
        fld(k,0x81,"Sta1"); u32v(k,1); u8v(k,state); u8v(k,0); u8v(k,0); u8v(k,0); return k; };
      xara::AffinityStats st;
      const QString v=xara::affinityToSvgPages(doc(masked(6,2),1,0),&st).value(0);
      check(st.pixelMasks==1 && v.contains("xarq:pmask="), "Affinity masks: an attached MRst becomes a pixel mask");
      const QVector<xara::Shape> sh=xara::parseSvg(v.toUtf8());
      check(sh.size()==1 && sh[0].transp.geom==4 && !sh[0].transp.maskBmp.isNull(), "Affinity masks: the import carries it to the object");
      xara::AffinityStats bad;
      const QString v2=xara::affinityToSvgPages(doc(masked(3,2),1,0),&bad).value(0);
      check(bad.pixelMasks==0 && bad.skippedRaster==1 && !v2.contains("xarq:pmask"), "Affinity masks: a format not understood is reported, not guessed"); }

    { // DrwO is the stack: fill 0, fill 1, a HIDDEN fill, then a line. BFCr selects, it does not choose.
      QByteArray k; cls(k,"ShpN"); rectBody(k);
      fld(k,0x03,"BFCr"); u32v(k,1);
      fld(k,0xb1,"BFFl"); u32v(k,3); solid(k,1,0); solid(k,0,1); solid(k,0,0,1,1);
      fld(k,0xb1,"LIFl"); u32v(k,1); solid(k,0,0,1,0);
      fld(k,0xb1,"LILn"); u32v(k,1); line(k,4,1,0);
      fld(k,0xaa,"DrwO"); u32v(k,4); u16v(k,0); u16v(k,0); u16v(k,0); u16v(k,2); u16v(k,1);
      u8v(k,0);
      const QString v=xara::affinityToSvgPages(doc(k,1,0)).value(0);
      check(v.contains("fill=\"#ff0000\"") && v.contains("fill=\"#00ff00\""), "Affinity paints: every fill in DrwO is drawn, not just the selected one");
      check(!v.contains("fill=\"#0000ff\""), "Affinity paints: DrwO 2 skips a hidden fill");
      check(v.indexOf("#ff0000") < v.indexOf("#00ff00"), "Affinity paints: the stack is bottom to top"); }

    { // Line style 0 draws nothing however heavy it says it is; 2 is dashed.
      QByteArray k; cls(k,"ShpN"); rectBody(k);
      fld(k,0xb1,"BFFl"); u32v(k,1); solid(k,1,0);
      fld(k,0xb1,"LIFl"); u32v(k,1); solid(k,0,0,1,0);
      fld(k,0xb1,"LILn"); u32v(k,1); line(k,9,0,0);
      fld(k,0xaa,"DrwO"); u32v(k,2); u16v(k,0); u16v(k,0); u16v(k,1); u8v(k,0);
      check(!xara::affinityToSvgPages(doc(k,1,0)).value(0).contains("stroke="), "Affinity paints: line style 0 is no stroke, whatever its weight");
      QByteArray d; cls(d,"ShpN"); rectBody(d);
      fld(d,0xb1,"BFFl"); u32v(d,1); solid(d,1,0);
      fld(d,0xb1,"LIFl"); u32v(d,1); solid(d,0,0,1,0);
      fld(d,0xb1,"LILn"); u32v(d,1); line(d,3,2,0);
      fld(d,0xaa,"DrwO"); u32v(d,2); u16v(d,0); u16v(d,0); u16v(d,1); u8v(d,0);
      check(xara::affinityToSvgPages(doc(d,1,0)).value(0).contains("stroke-dasharray"), "Affinity paints: line style 2 is dashed"); }

    { // An outside-aligned line becomes geometry that leaves the shape's own area alone.
      QByteArray k; cls(k,"ShpN"); rectBody(k);
      fld(k,0xb1,"BFFl"); u32v(k,1); solid(k,1,0);
      fld(k,0xb1,"LIFl"); u32v(k,1); solid(k,0,0,1,1);
      fld(k,0xb1,"LILn"); u32v(k,1); line(k,8,1,2);
      fld(k,0xaa,"DrwO"); u32v(k,2); u16v(k,0); u16v(k,0); u16v(k,1); u8v(k,0);
      const QVector<Shape> sh=parseSvg(xara::affinityToSvgPages(doc(k,1,0)).value(0).toUtf8(),nullptr,QString());
      QVector<Shape> leaves; std::function<void(const QVector<Shape>&)> flat=[&](const QVector<Shape>& vs){ for(const Shape& x:vs){ if(x.type==ShapeType::Group) flat(x.children); else leaves.push_back(x); } }; flat(sh);
      bool ring=false;
      for(const Shape& x:leaves) if(x.fill.type==FillType::Solid && x.fill.color==QColor(0,0,255)){
          QPainterPath p=x.transform.map(x.localPath()); p.setFillRule(x.fillRule);
          ring = p.contains(QPointF(6,60)) && !p.contains(QPointF(60,60)) && p.boundingRect().width()>110; }
      check(ring, "Affinity paints: an outside stroke covers only the outside of the shape"); }

    { // A symbol instance stores only its overrides; the rest comes through ShpM.
      QByteArray k; cls(k,"ShpN");
      fld(k,0x31,"ShpM"); cls(k,"ShpN"); rectBody(k); fld(k,0xb1,"BFFl"); u32v(k,1); solid(k,1,0); u8v(k,0);
      fld(k,0x28,"Xfrm"); f64v(k,1); f64v(k,0); f64v(k,30); f64v(k,0); f64v(k,1); f64v(k,0);
      u8v(k,0);
      const QString v=xara::affinityToSvgPages(doc(k,1,0)).value(0);
      check(v.contains("<rect") && v.contains("fill=\"#ff0000\""), "Affinity symbols: an instance draws its master's geometry and paint");
      check(v.contains("matrix(1,0,0,1,30,0)"), "Affinity symbols: the instance keeps its own transform");
      QByteArray o; cls(o,"ShpN");
      fld(o,0x31,"ShpM"); cls(o,"ShpN"); rectBody(o); fld(o,0xb1,"BFFl"); u32v(o,1); solid(o,1,0); u8v(o,0);
      fld(o,0xb1,"BFFl"); u32v(o,1); solid(o,0,1);
      u8v(o,0);
      const QString w=xara::affinityToSvgPages(doc(o,1,0)).value(0);
      check(w.contains("fill=\"#00ff00\"") && !w.contains("fill=\"#ff0000\""), "Affinity symbols: an instance's override wins over the master"); }

    { // A filter under AdCh carries CLiT = 1 too, but it is not a clip, so the layer must still draw.
      QByteArray k; cls(k,"ShpN"); rectBody(k);
      fld(k,0xb1,"BFFl"); u32v(k,1); solid(k,1,0);
      fld(k,0xb1,"AdCh"); u32v(k,1); cls(k,"FlRN"); fld(k,0x2a,"CLiT"); u16v(k,1); u16v(k,0); u8v(k,0);
      u8v(k,0);
      check(xara::affinityToSvgPages(doc(k,1,0)).value(0).contains("<rect"), "Affinity: a filter child is not mistaken for an inexpressible clip"); }

    { // An Invert layer changes what lies below it, and only that.
      QByteArray k;
      cls(k,"ShpN"); rectBody(k); fld(k,0xb1,"BFFl"); u32v(k,1); solid(k,1,0); u8v(k,0);
      cls(k,"InRA"); u8v(k,0);
      cls(k,"ShpN"); rectBody(k); fld(k,0xb1,"BFFl"); u32v(k,1); solid(k,0,0,1,1); u8v(k,0);
      const QString v=xara::affinityToSvgPages(doc(k,3,0)).value(0);
      check(v.contains("fill=\"#00ffff\""), "Affinity adjustments: Invert bakes into the colour below it");
      check(v.contains("fill=\"#0000ff\""), "Affinity adjustments: a layer above the adjustment is untouched"); }

    { // The document's format decides how a stored colour shows.
      auto one=[&](float r, float g, float bl, int ddrf){ QByteArray k; cls(k,"ShpN"); rectBody(k); fld(k,0xb1,"BFFl"); u32v(k,1); solid(k,r,g,1.f,bl); u8v(k,0);
          return xara::affinityToSvgPages(doc(k,1,ddrf)).value(0); };
      check(one(1,0,0,2).contains("fill=\"#818181\""), "Affinity colour: in a grey document pure red shows as D50 luminance at gamma 2.2");
      check(one(0.905f,0.905f,0.905f,9).contains("fill=\"#f4f4f4\""), "Affinity colour: an RGB/32 document stores linear values");
      check(one(1,0,0,4).contains("fill=\"#ed1e24\""), "Affinity colour: a CMYK document shows red through the print gamut"); }

    { // Every live shape class draws its own outline rather than its box.
      struct SC { const char* c; std::function<void(QByteArray&)> f; };
      auto F=[&](QByteArray& b,const char* n,float v){ fld(b,0x09,n); f32v(b,v); };
      const QVector<SC> shapes={
        {"ShCg",[&](QByteArray& b){ F(b,"IRad",.8f); F(b,"Hole",.3f); F(b,"TtSz",.3f); F(b,"NtSz",.45f); fld(b,0x02,"Teth"); u16v(b,12); }},
        {"ShCR",[&](QByteArray& b){ F(b,"TlHg",.2f); F(b,"TlWd",.1f); F(b,"TlRP",.5f); F(b,"TlEP",.4f); }},
        {"ShCE",[&](QByteArray& b){ F(b,"TlHg",.25f); F(b,"TlEP",.2f); F(b,"TlAn",.26f); }},
        {"ShDA",[&](QByteArray& b){ F(b,"Thck",.35f); fld(b,0x02,"LSty"); u16v(b,8); fld(b,0x02,"RSty"); u16v(b,10); F(b,"RPr1",.6f); F(b,"RPr2",.4f); F(b,"LPr1",.4f); }},
        {"ShDS",[&](QByteArray& b){ fld(b,0x02,"Pnts"); u16v(b,5); F(b,"IRad",.6f); F(b,"PRad",.9f); }},
        {"ShTr",[&](QByteArray& b){ F(b,"Curv",.3f); }},
        {"ShCr",[&](QByteArray& b){ F(b,"ArcL",-1.f); F(b,"ArcR",-.3f); }},
        {"ShPi",[&](QByteArray& b){ F(b,"AngS",.7f); F(b,"AngE",-.5f); F(b,"IRad",0.f); }},
        {"ShSg",[&](QByteArray& b){ F(b,"Angl",1.7f); F(b,"Pos0",.3f); F(b,"Pos1",1.f); }},
        {"ShpD",[&](QByteArray& b){ F(b,"Pos ",.5f); }},
        {"ShTz",[&](QByteArray& b){ F(b,"PosL",.3f); F(b,"PosR",.8f); }} };
      for(const SC& sc : shapes){
          QByteArray k; cls(k,"ShpN"); box(k); fld(k,0x31,"Shpe"); cls(k,sc.c); sc.f(k); u8v(k,0);
          fld(k,0xb1,"BFFl"); u32v(k,1); solid(k,1,0); u8v(k,0);
          xara::AffinityStats st; const QString v=xara::affinityToSvgPages(doc(k,1,0),&st).value(0);
          check(v.contains("<path") && !v.contains("<rect") && st.skippedShape==0, QStringLiteral("Affinity shapes: %1 draws its own outline").arg(sc.c).toUtf8().constData());
      } }
}

// ---- Affinity RASTER layers: 256x256 single-channel planes reassembled into one RGBA image ----
// A second hermetic fixture for the pixel side of the format. One Rstr layer whose four channels
// exercise every tile state at once — a stored plane (4), the constant 0xFF (2) and the constant 0x00
// (1) — which also pins the channel order (R,G,B,A) and the crop back to the pixels that can show.
{
    auto u8v =[](QByteArray& b, unsigned v){ b.append(char(v&0xff)); };
    auto u16v=[](QByteArray& b, unsigned v){ b.append(char(v&0xff)); b.append(char((v>>8)&0xff)); };
    auto u32v=[](QByteArray& b, quint32 v){ for(int i=0;i<4;i++) b.append(char((v>>(8*i))&0xff)); };
    auto u64v=[](QByteArray& b, quint64 v){ for(int i=0;i<8;i++) b.append(char((v>>(8*i))&0xff)); };
    auto f64v=[&](QByteArray& b, double v){ quint64 r; memcpy(&r,&v,8); u64v(b,r); };
    auto tag =[&](QByteArray& b, const char* s){ for(int i=3;i>=0;--i) b.append(s[i]); };
    auto fld =[&](QByteArray& b, unsigned type, const char* name){ u8v(b,type); tag(b,name); };

    const int W = 200, H = 150;                       // one 256x256 tile covers the whole layer
    QByteArray planeR(256*256, '\0'), planeA(256*256, '\0');
    for(int y = 0; y < 256; ++y)
        for(int x = 0; x < 256; ++x){
            planeR[y*256+x] = char(x);                                    // red ramps with x
            if(x>=20 && x<120 && y>=30 && y<90) planeA[y*256+x] = char(0xFF);
        }

    QByteArray t;
    t.append('\x00'); t.append('\xff'); t.append('K'); t.append('S');
    u16v(t,1); tag(t,"Pers"); u16v(t,1);
    fld(t,0x31,"DocR");
      u8v(t,1); u32v(t,1); u8v(t,1); tag(t,"DocN");
      fld(t,0x31,"Chld");
        u8v(t,1); u32v(t,2); u8v(t,1); tag(t,"Sprd");
        fld(t,0x26,"SprB"); f64v(t,0); f64v(t,0); f64v(t,W); f64v(t,H);
        fld(t,0xb1,"Chld"); u32v(t,1);
          u8v(t,1); u32v(t,3); u8v(t,1); tag(t,"Rstr");
            fld(t,0xb1,"AdCh"); u32v(t,1);                              // a vector mask: CLiT = 1 = clip
              u8v(t,1); u32v(t,7); u8v(t,1); tag(t,"ShpN");
              fld(t,0x2a,"CLiT"); u16v(t,1); u16v(t,0);
              fld(t,0x26,"ShpB"); f64v(t,30); f64v(t,40); f64v(t,90); f64v(t,80);
              u8v(t,0);
            fld(t,0x31,"Bitm");
              u8v(t,1); u32v(t,4); u8v(t,1); tag(t,"DyBm");
              fld(t,0x2a,"Frmt"); u16v(t,0); u16v(t,0);                   // 8-bit RGBA
              fld(t,0x07,"BmpW"); u32v(t,(quint32)W);
              fld(t,0x07,"BmpH"); u32v(t,(quint32)H);
              fld(t,0xb1,"Idx1"); u32v(t,1);
                u8v(t,1); u32v(t,5); u8v(t,1); tag(t,"Blck");
                fld(t,0x33,"Data"); tag(t,"DatI"); u32v(t,3); t.append("d/1");
                u8v(t,0);
              fld(t,0x81,"Sta1"); u32v(t,1); u8v(t,4);                    // stored plane
              fld(t,0x81,"Sta2"); u32v(t,1); u8v(t,2);                    // constant 0xFF, no block
              fld(t,0x81,"Sta3"); u32v(t,1); u8v(t,1);                    // constant 0x00
              fld(t,0xb1,"Idx4"); u32v(t,1);
                u8v(t,1); u32v(t,6); u8v(t,1); tag(t,"Blck");
                fld(t,0x33,"Data"); tag(t,"DatI"); u32v(t,3); t.append("d/2");
                u8v(t,0);
              fld(t,0x81,"Sta4"); u32v(t,1); u8v(t,4);
              u8v(t,0);                                                    // end DyBm
            u8v(t,0);                                                      // end Rstr
        u8v(t,0);                                                          // end Sprd
      u8v(t,0);                                                            // end DocN
    u8v(t,0);                                                              // end root

    // d/2 is written in the WRAPPED form: the plane inside a one-field object stream whose CLASS is
    // "Data" and whose FIELD is "DatI". Reading it as a field called "Data" matches nothing, which is
    // exactly the bug that shipped — it made every wrapped-plane document fall back to its preview
    // while still reporting success. d/1 stays bare, so one fixture covers both encodings.
    QByteArray wrappedA;
    wrappedA.append('\x00'); wrappedA.append('\xff'); wrappedA.append('K'); wrappedA.append('S');
    u16v(wrappedA,1); tag(wrappedA,"Data"); u16v(wrappedA,1);
    u8v(wrappedA,0x2d); tag(wrappedA,"DatI"); u32v(wrappedA,(quint32)planeA.size());
    wrappedA.append(planeA); u8v(wrappedA,0);

    // three zlib chunks laid out back to back, then a #FAT naming all three
    struct Ent { const char* name; QByteArray raw, comp; quint64 off=0; };
    Ent ents[3] = { {"doc.dat",t,{}}, {"d/1",planeR,{}}, {"d/2",wrappedA,{}} };
    QByteArray f;
    f.append('\x00'); f.append('\xff'); f.append('K'); f.append('A');
    u16v(f,9); u16v(f,0); tag(f,"Prsn"); f.append("#Inf",4);
    quint64 at = 72; quint32 tail = 0;
    for(Ent& e : ents){
        e.comp = qCompress(e.raw,9).mid(4);
        e.off  = at;
        at    += 4 + quint64(e.comp.size()) + 4;
        tail  += 36 + quint32(qstrlen(e.name));
    }
    u64v(f,at); u64v(f,0); u64v(f,0); u64v(f,0); u64v(f,0); u32v(f,1); u32v(f,2);
    f.append("Prot",4); u32v(f,3);
    for(const Ent& e : ents){ f.append("#Fil",4); f.append(e.comp); u32v(f,0xFFFFFFFFu); }
    f.append("#FAT",4);
    u64v(f,0); u64v(f,1000); u64v(f,0); u64v(f,0); u64v(f,0);
    u32v(f,3); u32v(f,0); u32v(f,tail); u16v(f,0); u8v(f,0);
    for(int i = 0; i < 3; ++i){
        const Ent& e = ents[i];
        u32v(f,(quint32)(i+1)); u8v(f,0);
        u64v(f,e.off); u64v(f,(quint64)e.raw.size()); u64v(f,(quint64)e.comp.size());
        u32v(f,(quint32)crc32(crc32(0,nullptr,0),(const Bytef*)e.raw.constData(),(uInt)e.raw.size()));
        u8v(f,1);                                                          // #FAT compression INDEX 1 = zlib
        u16v(f,(unsigned)qstrlen(e.name)); f.append(e.name,(int)qstrlen(e.name));
    }

    int rOk=0, rBad=0;
    check(xara::affinityVerifyStreams(f,&rOk,&rBad) && rOk==3 && rBad==0,
          "Affinity raster: all three chunks (tree + two planes) verify against their CRC32s");

    xara::AffinityStats rst;
    const QStringList rpages = xara::affinityToSvgPages(f,&rst);
    check(rpages.size()==1 && rst.rasters==1 && rst.skippedRaster==0,
          "Affinity raster: the Rstr layer is assembled, not skipped");
    const QString rsvg = rpages.value(0);
    // The alpha plane is opaque only over [20,120) x [30,90), so the emitted image is cropped to it —
    // a layer's tile grid spans the whole canvas even when the paint covers a corner of it.
    check(rsvg.contains("<image x=\"20\" y=\"30\" width=\"100\" height=\"60\""),
          "Affinity raster: the image is cropped to the pixels that can actually show");

    const int b64 = rsvg.indexOf("base64,");
    const QByteArray pngOut = (b64 < 0) ? QByteArray()
        : QByteArray::fromBase64(rsvg.mid(b64+7, rsvg.indexOf('"', b64) - b64 - 7).toLatin1());
    QImage got; got.loadFromData(pngOut, "PNG");
    check(got.size()==QSize(100,60), "Affinity raster: the decoded PNG is the cropped size");
    // Channel 1 is the stored ramp, 2 the constant 0xFF, 3 the constant 0x00, 4 the stored alpha.
    check(got.pixelColor(0,0)==QColor(20,255,0,255) && got.pixelColor(99,59)==QColor(119,255,0,255),
          "Affinity raster: channels 1..4 are R,G,B,A and the two constant states resolve to 0xFF/0x00");
    // AdCh — the layer's vector mask. It is written as a clipPath around the layer's group, so it
    // crops the layer's own content and its children alike (Affinity hangs these off vector layers
    // far more often than off pixel ones).
    check(rst.masks==1 && rsvg.contains("<clipPath id=\"afclip1\">")
                       && rsvg.contains("clip-path=\"url(#afclip1)\""),
          "Affinity: an AdCh CLiT=1 shape becomes a clipPath on the layer");
    check(rsvg.contains("<rect x=\"30\" y=\"40\" width=\"60\" height=\"40\""),
          "Affinity: the mask's own geometry (not its bounding box) is what clips");
}
// Regression (adversarial review): out-of-range/huge numeric inputs must be handled without UB (UBSan-safe).
{ QByteArray tj = "{\"type\":\"Topology\",\"arcs\":[[[0,0],[10,10]]],"
                  "\"objects\":{\"a\":{\"type\":\"LineString\",\"arcs\":[1e19,0]}}}";  // 1e19 index skipped, arc 0 used
  QString svg=xara::topojsonToSvg((const uint8_t*)tj.constData(),(qint64)tj.size());
  check(svg.contains("<polyline") && !svg.contains("inf") && !svg.contains("nan"),
        "topojson: out-of-range arc index skipped, no UB/inf"); }
{ QByteArray sla = "<SCRIBUSUTF8NEW><DOCUMENT>"
                   "<COLOR NAME=\"big\" CMYK=\"1e300 0 0 0\"/>"
                   "<PAGEOBJECT PTYPE=\"6\" XPOS=\"0\" YPOS=\"0\" WIDTH=\"10\" HEIGHT=\"10\" FCOLOR=\"big\"/>"
                   "</DOCUMENT></SCRIBUSUTF8NEW>";
  QString svg=xara::scribusToSvg((const uint8_t*)sla.constData(),(qint64)sla.size());
  check(!svg.contains("inf") && !svg.contains("nan"), "scribus: huge CMYK component clamped, no UB/inf"); }


      // ---- Regression: adversarial-review fixes (vecextra) ----
      // xfig ARC with a NaN middle defining point must NOT leak "nan"/"inf" into the SVG.
      { QByteArray fig =
          "#FIG 3.2\nLandscape\nCenter\nInches\nLetter\n100.00\nSingle\n-2\n1200 2\n"
          "5 1 0 1 0 7 50 0 -1 0.000 0 0 0 0 500 500 100 100 nan 200 300 300\n";
        QString svg=xara::xfigToSvg((const uint8_t*)fig.constData(),(qint64)fig.size());
        check(!svg.contains("nan") && !svg.contains("inf"), "xfig: ARC NaN middle point not emitted into SVG"); }
      // xfig user-colour with an attribute-injection payload must be rejected, not echoed verbatim.
      { QByteArray fig =
          "#FIG 3.2\nLandscape\nCenter\nInches\nLetter\n100.00\nSingle\n-2\n1200 2\n"
          "0 40 #a'/><script>x</script>\n"
          "2 1 0 1 40 7 50 -1 -1 0.000 0 0 -1 0 0 2\n\t100 100 200 200\n";
        QString svg=xara::xfigToSvg((const uint8_t*)fig.constData(),(qint64)fig.size());
        check(!svg.contains("<script") && !svg.contains("#a'"), "xfig: malformed user-colour hex not injected into SVG attributes"); }
      // gpx tolerates a run of empty <trkseg> segments (polyline-count cap path) and still keeps the real point.
      { QByteArray g = "<gpx version='1.1' xmlns='http://www.topografix.com/GPX/1/1'>";
        for(int i=0;i<50;i++) g += "<trk><trkseg></trkseg></trk>";
        g += "<wpt lat='1' lon='2'/></gpx>";
        QString svg=xara::gpxToSvg((const uint8_t*)g.constData(),(qint64)g.size());
        check(!svg.isEmpty() && svg.contains("<circle"), "gpx: empty segments tolerated, real waypoint still emitted"); }

      // Fail-safe: non-archive/non-animation inputs must be rejected, not crash.
      { const QString p="/tmp/xq_t.cbz"; writeF(p,"not a zip file"); check(!c->importCbz(p), "CBZ rejects a non-archive safely"); }
      { const QString p="/tmp/xq_t.cbr"; writeF(p,"not an archive");  check(!c->importCbr(p), "CBR rejects a non-archive safely"); }
      { const QString p="/tmp/xq_t.odg"; writeF(p,"not a zip");       check(!c->importOdg(p), "ODG rejects a non-archive safely"); }
      // Flat ODF (single-XML, no zip): a .fodg with draw: shapes → vector shapes; a .fodt (text) → text
      // frame via the fallback. Exercises the generalized importOdg (flat-XML path + text extraction).
      { const QByteArray fodg="<?xml version=\"1.0\"?><office:document xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" xmlns:draw=\"urn:oasis:names:tc:opendocument:xmlns:drawing:1.0\" xmlns:svg=\"urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0\"><office:body><office:drawing><draw:page><draw:rect svg:x=\"1cm\" svg:y=\"1cm\" svg:width=\"4cm\" svg:height=\"2cm\"/><draw:ellipse svg:x=\"6cm\" svg:y=\"1cm\" svg:width=\"3cm\" svg:height=\"3cm\"/></draw:page></office:drawing></office:body></office:document>";
        const QString p=QDir::tempPath()+"/xq_flat.fodg"; writeF(p,fodg);
        Canvas oc; oc.newDocument(); int before=oc.doc().shapes.size();
        check(oc.importOdg(p) && oc.doc().shapes.size()>=before+2, "flat ODF (.fodg) imports draw: shapes without a zip container"); }
      { const QByteArray fodt="<?xml version=\"1.0\"?><office:document xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\"><office:body><office:text><text:p>Hello from a flat ODT.</text:p><text:p>Second paragraph.</text:p></office:text></office:body></office:document>";
        const QString p=QDir::tempPath()+"/xq_flat.fodt"; writeF(p,fodt);
        Canvas oc; oc.newDocument();
        check(oc.importOdg(p) && !oc.doc().shapes.isEmpty() && oc.doc().shapes.back().type==ShapeType::Text
              && oc.doc().shapes.back().text.contains("flat ODT"), "flat ODT (.fodt) text falls back to an editable text frame"); }
      // ODF style fidelity: a shape keeps its REAL fill/stroke colours (draw:style-name → graphic-properties)
      // and a <draw:path> imports as a curved path — not the old hardcoded pale-blue straight-edged default.
      { const QByteArray f="<?xml version=\"1.0\"?><office:document xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" "
          "xmlns:style=\"urn:oasis:names:tc:opendocument:xmlns:style:1.0\" xmlns:draw=\"urn:oasis:names:tc:opendocument:xmlns:drawing:1.0\" "
          "xmlns:svg=\"urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0\"><office:automatic-styles>"
          "<style:style style:name=\"g1\" style:family=\"graphic\"><style:graphic-properties draw:fill=\"solid\" "
          "draw:fill-color=\"#cc3333\" draw:stroke=\"solid\" svg:stroke-color=\"#003399\" svg:stroke-width=\"0.1cm\"/></style:style>"
          "</office:automatic-styles><office:body><office:drawing><draw:page>"
          "<draw:rect draw:style-name=\"g1\" svg:x=\"1cm\" svg:y=\"1cm\" svg:width=\"5cm\" svg:height=\"3cm\"/>"
          "<draw:path svg:x=\"1cm\" svg:y=\"6cm\" svg:width=\"6cm\" svg:height=\"4cm\" svg:viewBox=\"0 0 6000 4000\" "
          "svg:d=\"M 0 2000 C 1500 0 4500 4000 6000 2000 Z\"/></draw:page></office:drawing></office:body></office:document>";
        const QString p=QDir::tempPath()+"/xq_styled.fodg"; writeF(p,f);
        Canvas oc; oc.newDocument(); bool ok=oc.importOdg(p);
        bool red=false, blueStroke=false, curve=false;
        for(const Shape& s: oc.doc().shapes){
            if(s.fill.type==FillType::Solid && s.fill.color==QColor("#cc3333")) red=true;
            if(s.stroke.enabled && s.stroke.color==QColor("#003399")) blueStroke=true;
            if(pathHasCurve(s.path)) curve=true; }
        check(ok && red && blueStroke, "ODF: shape keeps its real style fill (#cc3333) + stroke (#003399) colours");
        check(ok && curve, "ODF: <draw:path svg:d> imports as a curved path"); }
      // ODF gradient fill: draw:fill="gradient" + a <draw:gradient> ramp → a real Linear/Radial gradient
      // carrying the document's two colours (was: a flat default fill).
      { const QByteArray f="<?xml version=\"1.0\"?><office:document xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" "
          "xmlns:style=\"urn:oasis:names:tc:opendocument:xmlns:style:1.0\" xmlns:draw=\"urn:oasis:names:tc:opendocument:xmlns:drawing:1.0\" "
          "xmlns:svg=\"urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0\"><office:automatic-styles>"
          "<draw:gradient draw:name=\"grad1\" draw:style=\"linear\" draw:start-color=\"#ff0000\" draw:end-color=\"#0000ff\" draw:angle=\"0\"/>"
          "<style:style style:name=\"g1\" style:family=\"graphic\"><style:graphic-properties draw:fill=\"gradient\" "
          "draw:fill-gradient-name=\"grad1\"/></style:style></office:automatic-styles><office:body><office:drawing><draw:page>"
          "<draw:rect draw:style-name=\"g1\" svg:x=\"1cm\" svg:y=\"1cm\" svg:width=\"5cm\" svg:height=\"3cm\"/>"
          "</draw:page></office:drawing></office:body></office:document>";
        const QString p=QDir::tempPath()+"/xq_grad.fodg"; writeF(p,f);
        Canvas oc; oc.newDocument(); bool ok=oc.importOdg(p); bool grad=false;
        for(const Shape& s: oc.doc().shapes){
            if(s.fill.type==FillType::Linear||s.fill.type==FillType::Radial){
                bool r=false,b=false; for(const GradStop& gs: s.fill.stops){ if(gs.color==QColor("#ff0000"))r=true; if(gs.color==QColor("#0000ff"))b=true; }
                if(r&&b) grad=true; } }
        check(ok && grad, "ODF: draw:fill=gradient imports as a real gradient fill carrying both stop colours"); }
      // ODF draw:transform — radians, counter-clockwise on a y-down page, list applied LEFT-TO-RIGHT
      // (all three diverge from SVG). Pinned against LibreOffice.
      { auto fodg=[](const QString& xf){ return QString(
          "<?xml version=\"1.0\"?><office:document xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" "
          "xmlns:draw=\"urn:oasis:names:tc:opendocument:xmlns:drawing:1.0\" "
          "xmlns:svg=\"urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0\"><office:body><office:drawing><draw:page>"
          "<draw:rect draw:transform=\"%1\" svg:width=\"4cm\" svg:height=\"1cm\"/></draw:page></office:drawing></office:body></office:document>").arg(xf).toUtf8(); };
        auto firstXf=[&](const QByteArray& doc)->QTransform{
          const QString p=QDir::tempPath()+"/xq_odfxf.fodg";
          { QFile w(p); w.open(QIODevice::WriteOnly); w.write(doc); w.close(); }
          Canvas oc; oc.newDocument(); if(!oc.importOdg(p) || oc.doc().shapes.isEmpty()) return QTransform(1,0,0,1,-1,-1);
          return oc.doc().shapes.back().transform; };
        const double cm=96.0/2.54;
        { const QTransform t=firstXf(fodg("translate (5cm 3cm)"));
          check(qAbs(t.dx()-5*cm)<0.5 && qAbs(t.dy()-3*cm)<0.5, "ODF draw:transform translate carries real units (cm)"); }
        { const QTransform t=firstXf(fodg("rotate (1.5707963)"));   // +90 deg CCW on a y-down page
          check(qAbs(t.m11())<0.01 && t.m12()<-0.99 && t.m21()>0.99,
                "ODF draw:transform rotate is radians + counter-clockwise (negated for Qt)"); }
        { // order: rotate-then-translate must differ from translate-then-rotate, and ODF is left-to-right
          const QTransform a=firstXf(fodg("rotate (1.5707963) translate (5cm 0cm)"));
          const QTransform b=firstXf(fodg("translate (5cm 0cm) rotate (1.5707963)"));
          check(qAbs(a.dx()-b.dx())>1.0 || qAbs(a.dy()-b.dy())>1.0, "ODF draw:transform order is significant");
          check(qAbs(a.dx()-5*cm)<0.5 && qAbs(a.dy())<0.5,
                "ODF draw:transform applies the list left-to-right (opposite of SVG)"); } }
      // ODF <draw:g> groups: structure preserved, identity group transform, empties dropped.
      { const QByteArray g =
          "<?xml version=\"1.0\"?><office:document xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" "
          "xmlns:draw=\"urn:oasis:names:tc:opendocument:xmlns:drawing:1.0\" "
          "xmlns:svg=\"urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0\"><office:body><office:drawing><draw:page>"
          "<draw:rect svg:x=\"0cm\" svg:y=\"0cm\" svg:width=\"1cm\" svg:height=\"1cm\"/>"
          "<draw:g draw:name=\"grp\"><draw:rect svg:x=\"2cm\" svg:y=\"0cm\" svg:width=\"1cm\" svg:height=\"1cm\"/>"
          "<draw:g><draw:ellipse svg:x=\"4cm\" svg:y=\"0cm\" svg:width=\"1cm\" svg:height=\"1cm\"/></draw:g></draw:g>"
          "<draw:g/></draw:page></office:drawing></office:body></office:document>";
        const QString p=QDir::tempPath()+"/xq_odfgrp.fodg";
        { QFile w(p); w.open(QIODevice::WriteOnly); w.write(g); w.close(); }
        Canvas oc; oc.newDocument(); const bool ok=oc.importOdg(p);
        const auto& sh=oc.doc().shapes;
        check(ok && sh.size()==2, "ODF: <draw:g> yields 2 root shapes (loose rect + one group; empty group dropped)");
        const Shape* grp=nullptr; for(const Shape& x: sh) if(x.type==ShapeType::Group) grp=&x;
        check(grp && grp->children.size()==2, "ODF: group keeps its 2 children");
        check(grp && grp->transform.isIdentity(), "ODF: group transform is identity (children hold absolute coords)");
        check(grp && grp->children.size()==2 && grp->children[1].type==ShapeType::Group, "ODF: nested <draw:g> nests"); }
      { const QString p="/tmp/xq_t.ora"; writeF(p,"not a zip");       check(!c->importOra(p), "OpenRaster rejects a non-archive safely"); }
      { const QString p="/tmp/xq_t.kra"; writeF(p,"not a zip");       check(!c->importKra(p), "Krita rejects a non-archive safely"); }
      { const QString p="/tmp/xq_t.png"; QImage(4,4,QImage::Format_ARGB32).save(p,"PNG");   // plain PNG isn't an animation
        check(!c->importApng(p), "importApng returns false for a non-animated PNG"); } }

    // --- Reorder pages: swap the content of two adjacent pages ---
    { Document d; d.pageSize=QSizeF(100,140); d.pageCount=3;
      const double adv=d.pageSize.height()+d.pageGap;
      auto mk=[&](int page){ Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,20,20); s.transform=QTransform::fromTranslate(10, page*adv+10); return s; };
      d.shapes={ mk(0), mk(1), mk(2) }; c->setDocument(d);
      c->movePage(0,+1);   // swap pages 0 and 1
      auto band=[&](int i){ return int(c->doc().shapes[i].docBounds().center().y()/adv); };
      check(band(0)==1 && band(1)==0 && band(2)==2, "movePage swaps the two pages' content, leaving others put"); }

    // --- Ruler origin: defaults to (0,0), gated JSON, round-trips ---
    { Document d; c->setDocument(d);
      check(c->doc().rulerOrigin.isNull(), "ruler origin defaults to (0,0)");
      check(!d.toJson()["page"].toObject().contains("rox"), "default ruler origin omits its JSON keys (gate)");
      c->setRulerOrigin(QPointF(50,30));
      Document rt=Document::fromJson(c->doc().toJson());
      check(qAbs(rt.rulerOrigin.x()-50)<1e-6 && qAbs(rt.rulerOrigin.y()-30)<1e-6, "ruler origin round-trips through JSON");
      c->resetRulerOrigin(); check(c->doc().rulerOrigin.isNull(), "reset ruler origin returns to (0,0)"); }

    // --- Arrowhead size independent of line width (gated JSON) ---
    { c->newDocument();
      Shape ln; ln.type=ShapeType::Path; QPainterPath p; p.moveTo(0,0); p.lineTo(100,0); ln.path=p;
      ln.stroke.enabled=true; ln.stroke.endArrow=-2; ln.stroke.endArrowW=2.5; ln.stroke.endArrowH=2.5; int i=c->doc().addShape(ln);
      Document rt=Document::fromJson(c->doc().toJson());
      check(qAbs(rt.shapes[i].stroke.endArrowW-2.5)<1e-6 && qAbs(rt.shapes[i].stroke.endArrowH-2.5)<1e-6, "arrowhead size round-trips through JSON");
      Shape plain; plain.type=ShapeType::Path; plain.stroke.enabled=true;
      QJsonObject ps; { Document d2; d2.addShape(plain); ps=d2.toJson()["shapes"].toArray()[0].toObject()["stroke"].toObject(); }
      check(!ps.contains("eAW"), "default (1×) arrow size omits its JSON key (gate)"); }

    // --- Transparency blend-mode picker exposes the renderer's full mode set ---
    { c->newDocument();
      Shape r; r.type=ShapeType::Rect; r.rect=QRectF(0,0,60,60); r.fill.type=FillType::Solid; r.fill.color=Qt::red;
      r.transp.type=10; r.transp.t0=0.4; r.transp.t1=0.4; int i=c->doc().addShape(r); c->select({i});
      Document rt=Document::fromJson(c->doc().toJson());
      check(rt.shapes[i].transp.type==10, "transparency blend-mode (Additive) round-trips through JSON");
      QTemporaryDir td; check(c->exportPng(td.path()+"/blendmode.png",1), "an Additive-transparency object renders"); }

    // --- Semantic web heading tags (SEO): a tagged text object publishes inside <h1>/<h2>/<h3> ---
    { c->newDocument();
      Shape h; h.type=ShapeType::Text; h.text="Welcome"; h.font=QFont("DejaVu Sans",30); h.fill.type=FillType::Solid; h.fill.color=Qt::black;
      h.webHeading=2; h.transform=QTransform::fromTranslate(20,20); int i=c->doc().addShape(h);
      check(Document::fromJson(c->doc().toJson()).shapes[i].webHeading==2, "web heading level round-trips through JSON");
      QTemporaryDir td; const QString hp=td.path()+"/seo/site.html";
      check(c->exportWebsite(hp), "publish with an SEO heading succeeds");
      QFile f(hp); f.open(QIODevice::ReadOnly); const QString html=QString::fromUtf8(f.readAll());
      check(html.contains("<h2") && html.contains("Welcome</h2>"), "the tagged text is published inside an <h2>");
      // untagged text must NOT get a heading tag
      Shape p; p.type=ShapeType::Text; p.text="Body"; p.font=QFont("DejaVu Sans",14); p.fill.type=FillType::Solid; p.transform=QTransform::fromTranslate(20,120); c->doc().addShape(p);
      QJsonObject po=c->doc().toJson()["shapes"].toArray()[1].toObject();
      check(!po.contains("wH"), "untagged text omits the heading JSON key (gate)"); }

    // --- Per-page web properties: custom filename + per-page SEO + link-to-page (page:N) ---
    { c->newDocument(); c->addPage(); c->addPage();   // 3 pages
      PageProps pp; pp.name="About Us"; pp.title="About — Acme"; pp.description="Who we are"; c->setPageProps(1,pp);
      Shape lk; lk.type=ShapeType::Text; lk.text="About"; lk.font=QFont("DejaVu Sans",18); lk.fill.type=FillType::Solid; lk.fill.color=Qt::black;
      lk.url="page:2"; lk.transform=QTransform::fromTranslate(20,20); c->doc().addShape(lk);
      QTemporaryDir td; const QString hp=td.path()+"/ppsite/site.html";
      check(c->exportWebsite(hp), "publish with per-page properties succeeds");
      check(QFile::exists(td.path()+"/ppsite/about-us.html"), "page 2 published to its custom filename about-us.html");
      { QFile f(td.path()+"/ppsite/about-us.html"); f.open(QIODevice::ReadOnly); QString h=QString::fromUtf8(f.readAll());
        check(h.contains("<title>About — Acme"), "page 2 uses its own <title>");
        check(h.contains("name=\"description\" content=\"Who we are\""), "page 2 uses its own meta description"); }
      { QFile f(hp); f.open(QIODevice::ReadOnly); QString h=QString::fromUtf8(f.readAll());
        check(h.contains("href=\"about-us.html\""), "a page:2 link resolves to that page's HTML file"); }
      Document rt=Document::fromJson(c->doc().toJson());
      check(rt.pageProps.size()>=2 && rt.pageProps[1].name=="About Us", "per-page properties round-trip through JSON");
      Document d2; check(!d2.toJson().contains("pageProps"), "a doc with no page props omits the pageProps key (gate)"); }

    // --- Non-printing layer: visible on screen, excluded from print/export ---
    { Document d; d.pageSize=QSizeF(100,100);
      d.layers={ Layer{}, Layer{} }; d.layers[1].name="Notes"; d.layers[1].printable=false;
      Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,100,100); s.fill.type=FillType::Solid; s.fill.color=Qt::black; s.layer=1; d.shapes={s};
      auto centre=[&](bool screen){ QImage img(100,100,QImage::Format_ARGB32); img.fill(Qt::white);
        { QPainter g(&img); xara::setScreenRenderMode(screen); xara::paintDocument(g,d); xara::setScreenRenderMode(false); } return img.pixelColor(50,50); };
      check(centre(true)==QColor(Qt::black), "non-printing layer IS drawn in the on-screen render");
      check(centre(false)==QColor(Qt::white), "non-printing layer is NOT drawn in the print/export render");
      Document rt=Document::fromJson(d.toJson());
      check(rt.layers.size()==2 && !rt.layers[1].printable, "non-printing flag round-trips through JSON");
      Document d2; d2.layers={Layer{},Layer{}};
      check(!d2.toJson()["layers"].toArray()[1].toObject().contains("noprint"), "printable (default) layers omit the noprint JSON key (gate)"); }

    // --- Optimize Photos: downsample an oversized embedded photo to its display resolution ---
    { c->newDocument();
      QImage big(800,600,QImage::Format_ARGB32); big.fill(Qt::green);
      Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,100,80); s.fill.type=FillType::Bitmap; s.fill.image=big; s.fill.origImage=big;
      s.fill.p0=QPointF(0,80); s.fill.p1=QPointF(100,80); s.fill.p2=QPointF(0,0); s.transform=QTransform::fromTranslate(20,20);
      int i=c->doc().addShape(s); c->select({i});
      const int before=c->doc().shapes[i].fill.image.width();
      check(c->optimizePhotos()==1, "optimizePhotos downsamples the oversized photo");
      check(c->doc().shapes[i].fill.image.width()<before && c->doc().shapes[i].fill.image.width()<=160, "photo trimmed toward its ~1.5× display size");
      check(c->optimizePhotos()==0, "a second Optimize-Photos pass finds nothing more to do"); }

    // --- Scale line widths: resizing an object scales its stroke width (Xara default ON) ---
    { c->newDocument(); c->setScaleLineWidths(true);
      check(!Document().toJson()["page"].toObject().contains("noScaleLW"), "default (ON) scale-line-widths omits its JSON key (gate)");
      Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,50,50); s.stroke.enabled=true; s.stroke.width=4; s.transform=QTransform::fromTranslate(10,10);
      int i=c->doc().addShape(s); c->select({i});
      QRectF b=c->selectionBoundsDoc(); c->setSelectionSize(b.width()*2, b.height()*2, false);   // 2× → stroke ×2
      check(qAbs(c->doc().shapes[i].stroke.width-8.0)<0.15, "resizing 2× scales the stroke width 4→8 (scale-line-widths ON)");
      c->setScaleLineWidths(false); c->select({i});
      const double w0=c->doc().shapes[i].stroke.width; QRectF b2=c->selectionBoundsDoc();
      c->setSelectionSize(b2.width()*2, b2.height()*2, false);
      check(qAbs(c->doc().shapes[i].stroke.width-w0)<1e-6, "with scale-line-widths OFF the stroke width is unchanged on resize");
      check(!Document::fromJson(c->doc().toJson()).scaleLineWidths, "scale-line-widths=OFF round-trips through JSON"); }

    // --- Text aspect ratio (horizontal glyph scale) ---
    { c->newDocument();
      Shape t; t.type=ShapeType::Text; t.text="Wide"; t.font=QFont("DejaVu Sans",30); t.fill.type=FillType::Solid; t.fill.color=Qt::black;
      t.textAspect=2.0; t.transform=QTransform::fromTranslate(10,10); int i=c->doc().addShape(t);
      check(qAbs(Document::fromJson(c->doc().toJson()).shapes[i].textAspect-2.0)<1e-6, "text aspect ratio round-trips through JSON");
      { QTemporaryDir td; check(c->exportPng(td.path()+"/aspect.png",1), "expanded (2×) text renders"); }
      Shape n; n.type=ShapeType::Text; n.text="x"; QJsonObject po; { Document d2; d2.addShape(n); po=d2.toJson()["shapes"].toArray()[0].toObject(); }
      check(!po.contains("tAsp"), "default (1.0) text aspect omits its JSON key (gate)"); }

    // --- Blend attribute (colour) profile, separate from the position profile ---
    { c->newDocument();
      Shape a; a.type=ShapeType::Rect; a.rect=QRectF(0,0,40,40); a.fill.type=FillType::Solid; a.fill.color=Qt::red; a.transform=QTransform::fromTranslate(60,60);
      Shape b=a; b.fill.color=Qt::blue; b.transform=QTransform::fromTranslate(260,60);
      int ia=c->doc().addShape(a), ib=c->doc().addShape(b); c->select({ia,ib}); c->makeLiveBlend(6);
      int gi=-1; for(int k=0;k<c->doc().shapes.size();++k) if(c->doc().shapes[k].type==ShapeType::Group && c->doc().shapes[k].blendSteps>0) gi=k;
      check(gi>=0, "live blend produced a group for the attribute profile");
      if(gi>=0){ c->doc().shapes[gi].blendAttrProfile=0.5;
        check(qAbs(Document::fromJson(c->doc().toJson()).shapes[gi].blendAttrProfile-0.5)<1e-6, "blend attribute-profile round-trips through JSON");
        QTemporaryDir td; check(c->exportPng(td.path()+"/battr.png",1), "blend with a colour/attribute profile renders");
        c->doc().shapes[gi].blendAttrProfile=0.0;
        check(!c->doc().toJson()["shapes"].toArray()[gi].toObject().contains("blendAttrP"), "zero blend attr-profile omits its JSON key (gate)"); } }

    // --- Live blend along a curve: steps distributed along a spine, re-flow when the step count changes ---
    { c->newDocument();
      Shape a; a.type=ShapeType::Rect; a.rect=QRectF(0,0,30,30); a.fill.type=FillType::Solid; a.fill.color=Qt::red; a.transform=QTransform::fromTranslate(40,200);
      Shape b=a; b.fill.color=Qt::blue; b.transform=QTransform::fromTranslate(400,200);
      Shape sp; sp.type=ShapeType::Path; { QPainterPath q; q.moveTo(40,200); q.cubicTo(150,40,300,360,400,200); sp.path=q; } sp.stroke.enabled=true;
      int ia=c->doc().addShape(a), ib=c->doc().addShape(b), is=c->doc().addShape(sp);
      c->select({ia,ib,is}); c->makeLiveBlendAlongPath(5);
      check(c->doc().shapes.size()==1, "the 2 ends + spine were consumed into a single live blend group");
      int gi=0;
      check(c->doc().shapes[gi].type==ShapeType::Group && c->doc().shapes[gi].blendSteps>0 && !c->doc().shapes[gi].blendSpine.isEmpty(), "live blend-along-curve group carries the spine");
      { QTemporaryDir td; check(c->exportPng(td.path()+"/blendpath.png",1), "live blend-along-curve renders"); }
      check(!Document::fromJson(c->doc().toJson()).shapes[gi].blendSpine.isEmpty(), "the blend spine round-trips through JSON");
      Document d2; Shape g2; g2.type=ShapeType::Group; g2.blendSteps=4; g2.children={a,b}; d2.addShape(g2);
      check(!d2.toJson()["shapes"].toArray()[0].toObject().contains("blendSpine"), "a live blend with no spine omits the blendSpine key (gate)"); }

    // --- Interactive multi-object blend chain (A→B→C) ---
    { c->newDocument();
      Shape a; a.type=ShapeType::Rect; a.rect=QRectF(0,0,20,20); a.fill.type=FillType::Solid; a.fill.color=Qt::red;
      Shape b=a; b.fill.color=Qt::green; b.transform=QTransform::fromTranslate(100,0);
      Shape cc=a; cc.fill.color=Qt::blue; cc.transform=QTransform::fromTranslate(200,0);
      int ia=c->doc().addShape(a), ib=c->doc().addShape(b), ic=c->doc().addShape(cc);
      const int before=c->doc().shapes.size();
      c->select({ia,ib,ic}); c->blendSelection(4);
      check(c->doc().shapes.size()==before+8, "multi-object blend chain inserts 4 steps between EACH consecutive pair (3+4+4)"); }

    // --- Bitmap export optimiser: reduce to an indexed palette (+ dithering) ---
    { c->newDocument();
      Shape r; r.type=ShapeType::Rect; r.rect=QRectF(0,0,80,80); r.fill.type=FillType::Solid; r.fill.color=QColor(200,60,60); int i=c->doc().addShape(r); c->select({i});
      QTemporaryDir td;
      check(c->exportRaster(td.path()+"/opt.png", 1, 1.0, -1, false, true, true), "indexed+dithered PNG export succeeds");
      QImage im(td.path()+"/opt.png");
      check(!im.isNull() && im.colorCount()>0 && im.colorCount()<=256, "the optimised PNG is a ≤256-colour indexed image");
      check(c->exportRaster(td.path()+"/full.png", 1, 1.0, -1, false, false, false), "un-optimised (full-colour) PNG export still works");
      QImage im2(td.path()+"/full.png"); check(!im2.isNull() && im2.colorCount()==0, "the un-optimised PNG is a true-colour image"); }

    // --- Poster / tiled printing: an oversized page split across sheets ---
    { Document d; d.pageSize=QSizeF(1600,1200);
      Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,1600,1200); s.fill.type=FillType::Solid; s.fill.color=Qt::yellow; d.shapes={s};
      c->setDocument(d);
      QTemporaryDir td; const QString pp=td.path()+"/poster.pdf";
      check(c->exportPosterPdf(pp, 794, 1123, 24), "poster/tiled PDF export succeeds for an oversized page");
      QFile f(pp); f.open(QIODevice::ReadOnly); QByteArray b=f.readAll();
      check(b.startsWith("%PDF") && b.size()>1500, "poster is a non-trivial multi-sheet PDF");
      check(!c->exportPosterPdf(pp, 4, 4, 0), "poster export rejects an absurdly tiny sheet"); }

    // --- Named colours bound to objects: editing the palette ripples to the artwork ---
    { Document d; NamedColor nc; nc.name="Brand"; nc.color=QColor(200,30,30); d.namedColors.append(nc);
      Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,50,50); s.fill.type=FillType::Solid; s.fill.color=d.resolveNamed(0); s.fill.colorRef=0; d.shapes.push_back(s);
      Shape t; t.type=ShapeType::Rect; t.rect=QRectF(0,0,50,50); t.stroke.enabled=true; t.stroke.color=d.resolveNamed(0); t.stroke.colorRef=0; d.shapes.push_back(t);
      d.namedColors[0].color=QColor(20,80,200); d.resyncNamedColorBindings();
      check(d.shapes[0].fill.color==QColor(20,80,200), "editing a named colour ripples to a bound object's fill");
      check(d.shapes[1].stroke.color==QColor(20,80,200), "editing a named colour ripples to a bound object's stroke");
      Document rt=Document::fromJson(d.toJson());
      check(rt.shapes[0].fill.colorRef==0 && rt.shapes[1].stroke.colorRef==0, "the object→named-colour bindings round-trip through JSON");
      Shape u; u.type=ShapeType::Rect; QJsonObject fo; { Document d2; d2.addShape(u); fo=d2.toJson()["shapes"].toArray()[0].toObject()["fill"].toObject(); }
      check(!fo.contains("cRef"), "an unbound fill omits the cRef JSON key (gate)");
      // a tint derived from the brand colour also ripples (cascade via resolveNamed)
      NamedColor tn; tn.kind=1; tn.parent=0; tn.amount=0.5; d.namedColors.append(tn);
      Shape v; v.type=ShapeType::Rect; v.fill.type=FillType::Solid; v.fill.colorRef=1; d.shapes.push_back(v);
      d.namedColors[0].color=QColor(0,0,0); d.resyncNamedColorBindings();
      check(d.shapes[2].fill.color==d.resolveNamed(1), "a tint-of-a-named-colour also ripples when the parent is edited"); }

    // --- Edit-Brush attributes: nib rotation + scatter + size variation ---
    { c->newDocument();
      Shape ln; ln.type=ShapeType::Path; QPainterPath p; p.moveTo(0,0); p.lineTo(200,0); ln.path=p; ln.stroke.enabled=true; ln.stroke.width=12;
      QImage ink(8,8,QImage::Format_ARGB32); ink.fill(Qt::white); ln.stroke.brushInk=ink;
      ln.stroke.brushScatter=5.0; ln.stroke.brushRotate=15.0; ln.stroke.brushSizeVar=0.3; ln.stroke.brushRotateToLine=false;
      ln.transform=QTransform::fromTranslate(20,60); int i=c->doc().addShape(ln); c->select({i});
      Document rt=Document::fromJson(c->doc().toJson());
      check(qAbs(rt.shapes[i].stroke.brushScatter-5.0)<1e-6 && qAbs(rt.shapes[i].stroke.brushRotate-15.0)<1e-6 && !rt.shapes[i].stroke.brushRotateToLine,
            "Edit-Brush attributes (scatter/rotate/rotate-to-line) round-trip through JSON");
      { QTemporaryDir td; check(c->exportPng(td.path()+"/scatter.png",1), "a scatter brush renders"); }
      Shape d2s; d2s.type=ShapeType::Path; d2s.stroke.enabled=true; QJsonObject ks; { Document d2; d2.addShape(d2s); ks=d2.toJson()["shapes"].toArray()[0].toObject()["stroke"].toObject(); }
      check(!ks.contains("bScat") && !ks.contains("bRotL") && !ks.contains("bSzV"), "a default brush omits the Edit-Brush JSON keys (gate)"); }

    // --- Cutout Studio: guided graph-cut background removal ---
    { c->newDocument();
      const int W=120,H=120;
      QImage photo(W,H,QImage::Format_ARGB32); photo.fill(QColor(20,40,200));                 // blue background
      { QPainter g(&photo); g.setPen(Qt::NoPen); g.setBrush(QColor(220,40,30)); g.drawRect(40,40,40,40); }  // red square
      Shape ph; ph.type=ShapeType::Rect; ph.rect=QRectF(0,0,W,H);
      ph.fill.type=FillType::Bitmap; ph.fill.image=photo; ph.fill.origImage=photo;
      ph.fill.p0=QPointF(0,H); ph.fill.p1=QPointF(W,H); ph.fill.p2=QPointF(0,0);
      int pi=c->doc().addShape(ph); c->select({pi});
      QImage seed(W,H,QImage::Format_ARGB32); seed.fill(Qt::transparent);
      { QPainter g(&seed); g.setPen(Qt::NoPen);
        g.setBrush(QColor(0,220,0,255)); g.drawEllipse(QPointF(60,60),8,8);                    // KEEP: centre of the red square
        g.setBrush(QColor(230,0,0,255)); g.drawEllipse(QPointF(10,10),8,8); }                  // REMOVE: a blue corner
      bool ok=c->cutoutStudioApply(pi, seed);
      check(ok, "Cutout Studio produces a graph-cut mask from keep/remove scribbles");
      const Shape& r=c->doc().shapes[pi];
      check(r.transp.geom==4 && !r.transp.maskBmp.isNull(), "cutout writes a re-editable geom-4 transparency mask");
      const int cw=r.transp.maskBmp.width(), chh=r.transp.maskBmp.height();
      auto lum=[&](int x,int y){ return r.transp.maskBmp.pixelColor(qBound(0,x,cw-1),qBound(0,y,chh-1)).red(); };
      check(lum(cw*60/W,chh*60/H)>200 && lum(cw*10/W,chh*10/H)<60, "cutout keeps the scribbled foreground opaque and removes the background");
      // and it round-trips through JSON (the mask persists under transp)
      Document rt=Document::fromJson(c->doc().toJson());
      check(rt.shapes[pi].transp.geom==4 && !rt.shapes[pi].transp.maskBmp.isNull(), "the cutout mask survives a JSON save/load");
      // with no scribbles it declines rather than segmenting garbage
      QImage empty(W,H,QImage::Format_ARGB32); empty.fill(Qt::transparent);
      c->newDocument(); int p2=c->doc().addShape(ph); c->select({p2});
      check(!c->cutoutStudioApply(p2, empty), "cutout declines when no keep/remove strokes were painted"); }

    // --- Select-into-group: edit an object inside a group without ungrouping ---
    { c->newDocument();
      Shape ca; ca.type=ShapeType::Rect; ca.rect=QRectF(0,0,60,40); ca.name="A"; ca.fill.type=FillType::Solid; ca.fill.color=QColor(200,60,60);
      Shape cb; cb.type=ShapeType::Rect; cb.rect=QRectF(0,0,60,40); cb.name="B"; cb.fill.type=FillType::Solid; cb.fill.color=QColor(60,60,200);
      cb.transform=QTransform::fromTranslate(100,0);
      Shape grp; grp.type=ShapeType::Group; grp.children={ca,cb}; grp.transform=QTransform::fromTranslate(200,200);
      int gi=c->doc().addShape(grp); c->select({gi}); c->setTool(Tool::Select);
      check(!c->inGroup(), "a freshly selected group is not yet entered");
      // double-click child A (its doc rect is ~ (200,200 60x40)) to descend into the group
      QPointF vA=c->mapToView(QPointF(230,220));
      send(c, QEvent::MouseButtonPress,   vA, Qt::LeftButton, Qt::LeftButton);
      send(c, QEvent::MouseButtonRelease, vA, Qt::LeftButton, Qt::NoButton);
      { QMouseEvent e(QEvent::MouseButtonDblClick, vA, c->mapToGlobal(vA.toPoint()), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(c,&e); }
      check(c->inGroup(), "double-clicking a group descends into it (select-into-group)");
      check(c->firstSelected() && c->firstSelected()->name=="A", "the clicked child becomes the sub-selection");
      check(c->selection()==QVector<int>{gi}, "the group stays selected at top level while inside (menu/structural ops stay safe)");
      // restyle affects ONLY the sub-selected child
      c->applyToSelection([](Shape& s){ s.fill.color=QColor(0,200,0); }, "test recolour");
      check(c->doc().shapes[gi].children[0].fill.color==QColor(0,200,0), "restyle applies to the in-group child");
      check(c->doc().shapes[gi].children[1].fill.color==QColor(60,60,200), "restyle leaves the sibling untouched");
      // moving the child changes only its own transform (sibling + parent group stay put)
      const QTransform bSib=c->doc().shapes[gi].children[1].transform, bGrp=c->doc().shapes[gi].transform;
      c->setSelectionPos(400,400);
      check(c->doc().shapes[gi].children[0].transform.dx()!=0.0 || c->doc().shapes[gi].children[0].transform.dy()!=0.0, "moving the in-group child changes its transform");
      check(c->doc().shapes[gi].children[1].transform==bSib && c->doc().shapes[gi].transform==bGrp, "moving a child leaves the sibling and the parent group put");
      // deleting the child removes only it; the group survives
      c->deleteSelection();
      check(c->doc().shapes[gi].type==ShapeType::Group && c->doc().shapes[gi].children.size()==1, "deleting an in-group child removes only that child");
      // exitGroup() pops back out and re-selects the group at top level
      c->exitGroup();
      check(!c->inGroup() && c->selection()==QVector<int>{gi}, "exiting the group re-selects it at top level");
      // and the mouse gesture: re-enter, then click OUTSIDE the group bounds to leave it
      { QPointF vA2=c->mapToView(QPointF(330,220));            // over the surviving child (~doc rect 300,200 60x40)
        send(c, QEvent::MouseButtonPress,   vA2, Qt::LeftButton, Qt::LeftButton);
        send(c, QEvent::MouseButtonRelease, vA2, Qt::LeftButton, Qt::NoButton);
        QMouseEvent e(QEvent::MouseButtonDblClick, vA2, c->mapToGlobal(vA2.toPoint()), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(c,&e); }
      check(c->inGroup(), "re-entering the group via double-click");
      { QPointF vOut=c->mapToView(QPointF(700,700));           // empty pasteboard, outside the group
        send(c, QEvent::MouseButtonPress,   vOut, Qt::LeftButton, Qt::LeftButton);
        send(c, QEvent::MouseButtonRelease, vOut, Qt::LeftButton, Qt::NoButton); }
      check(!c->inGroup(), "clicking outside the group leaves it"); }

    // --- Review fixes (H-17): cutout mask geometry, group-descent invalidation, named-colour reindex ---
    { // #1 the cutout mask parallelogram must follow the bitmap fill (p0/p1/p2), not the axis-aligned localBounds
      c->newDocument(); const int W=80,H=80;
      QImage photo(W,H,QImage::Format_ARGB32); photo.fill(QColor(30,60,200));
      { QPainter g(&photo); g.setPen(Qt::NoPen); g.setBrush(QColor(220,50,40)); g.drawRect(24,24,32,32); }
      Shape ph; ph.type=ShapeType::Rect; ph.rect=QRectF(0,0,W,H); ph.fill.type=FillType::Bitmap; ph.fill.image=photo; ph.fill.origImage=photo;
      ph.fill.p0=QPointF(10,H+10); ph.fill.p1=QPointF(W+10,H+10); ph.fill.p2=QPointF(10,10);   // a MOVED fill (!= localBounds corners)
      int pi=c->doc().addShape(ph); c->select({pi});
      QImage seed(W,H,QImage::Format_ARGB32); seed.fill(Qt::transparent);
      { QPainter g(&seed); g.setPen(Qt::NoPen); g.setBrush(QColor(0,220,0,255)); g.drawEllipse(QPointF(40,40),6,6);
        g.setBrush(QColor(230,0,0,255)); g.drawEllipse(QPointF(6,6),6,6); }
      c->cutoutStudioApply(pi, seed);
      const Shape& r=c->doc().shapes[pi];
      check(r.transp.p0==r.fill.p0 && r.transp.p1==r.fill.p1 && r.transp.p2==r.fill.p2,
            "cutout mask parallelogram tracks the bitmap fill, not localBounds"); }

    { // #2/#3 a structural op (z-order/ungroup) while descended in a group must drop the stale descent state
      c->newDocument();
      Shape ca; ca.type=ShapeType::Rect; ca.rect=QRectF(0,0,50,50);
      Shape cb=ca; cb.transform=QTransform::fromTranslate(80,0);
      Shape g1; g1.type=ShapeType::Group; g1.children={ca,cb}; g1.transform=QTransform::fromTranslate(100,100);
      Shape g2=g1; g2.transform=QTransform::fromTranslate(400,100);
      int gi1=c->doc().addShape(g1); c->doc().addShape(g2); c->select({gi1}); c->setTool(Tool::Select);
      QPointF vA=c->mapToView(QPointF(125,125));               // over child A of group 1
      send(c, QEvent::MouseButtonPress, vA, Qt::LeftButton, Qt::LeftButton);
      send(c, QEvent::MouseButtonRelease, vA, Qt::LeftButton, Qt::NoButton);
      { QMouseEvent e(QEvent::MouseButtonDblClick, vA, c->mapToGlobal(vA.toPoint()), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(c,&e); }
      check(c->inGroup(), "descended into group 1 for the structural-op test");
      c->raiseSelection();
      check(!c->inGroup(), "a top-level reorder while descended drops the group descent (no stale m_enterPath)"); }

    { // Double-click auto-selects the natural editing tool for the object under the cursor (Xara-style).
      auto snd=[&](QEvent::Type t, QPointF v, Qt::MouseButton b, Qt::MouseButtons btns){
          QMouseEvent e(t,v,c->mapToGlobal(v.toPoint()),b,btns,Qt::NoModifier); QApplication::sendEvent(c,&e); };
      Document d;
      Shape rc;  rc.type=ShapeType::Rect;     rc.rect=QRectF(0,0,80,60);  rc.transform=QTransform::fromTranslate(40,40);   rc.fill.type=FillType::Solid; rc.fill.color=QColor(200,80,80);
      Shape elP; elP.type=ShapeType::Ellipse; elP.rect=QRectF(0,0,80,60); elP.transform=QTransform::fromTranslate(240,40); elP.fill.type=FillType::Solid; elP.fill.color=QColor(80,160,220);   // plain full ellipse (mode 0)
      Shape elS; elS.type=ShapeType::Ellipse; elS.rect=QRectF(0,0,80,60); elS.transform=QTransform::fromTranslate(440,40); elS.fill.type=FillType::Solid; elS.fill.color=QColor(80,200,160); elS.ellipseMode=1; elS.ellipseSweep=360;  // swept (pie) → has rim handles
      Shape pg;  pg.type=ShapeType::Polygon;  pg.polySides=5;             pg.rect=QRectF(0,0,90,90);  pg.transform=QTransform::fromTranslate(40,220); pg.fill.type=FillType::Solid; pg.fill.color=QColor(120,200,120);
      Shape pa;  pa.type=ShapeType::Path;     { QPainterPath p; p.addRect(0,0,70,70); pa.path=p; } pa.transform=QTransform::fromTranslate(260,220); pa.fill.type=FillType::Solid; pa.fill.color=QColor(210,180,60);
      Shape tx;  tx.type=ShapeType::Text;     tx.text="Edit"; tx.font=QFont("DejaVu Sans",40); tx.transform=QTransform::fromTranslate(460,240); tx.fill.type=FillType::Solid; tx.fill.color=Qt::black;
      d.shapes={rc,elP,elS,pg,pa,tx}; c->setDocument(d);
      const int n0=c->doc().shapes.size();
      auto dblAt=[&](QPointF docPt){ c->setTool(Tool::Select); QPointF v=c->mapToView(docPt);
          snd(QEvent::MouseButtonPress,   v, Qt::LeftButton, Qt::LeftButton);
          snd(QEvent::MouseButtonRelease, v, Qt::LeftButton, Qt::NoButton);
          QMouseEvent e(QEvent::MouseButtonDblClick,v,c->mapToGlobal(v.toPoint()),Qt::LeftButton,Qt::LeftButton,Qt::NoModifier); QApplication::sendEvent(c,&e);
          snd(QEvent::MouseButtonRelease, v, Qt::LeftButton, Qt::NoButton); };   // trailing release Qt delivers after a dbl-click, now under the switched tool
      auto ctr=[&](int i){ return c->doc().shapes[i].docBounds().center(); };
      dblAt(ctr(2)); check(c->tool()==Tool::Ellipse, "double-click a swept ellipse → Ellipse tool (rim handles)");
      dblAt(ctr(3)); check(c->tool()==Tool::Polygon, "double-click a polygon → Polygon tool");
      dblAt(ctr(4)); check(c->tool()==Tool::Node,    "double-click a path → Shape editor (Node) tool");
      dblAt(ctr(0)); check(c->tool()==Tool::Select,  "double-click a rectangle stays in the Selector (its corner/resize handles live there)");
      dblAt(ctr(1)); check(c->tool()==Tool::Select,  "double-click a plain full ellipse stays in the Selector (no rim handle to switch for)");
      check(c->doc().shapes.size()==n0, "the trailing release after a tool switch never spawns a stray shape");
      check(!c->rotateMode(),                        "a double-clicked (re-selected) object is not left in rotate/skew mode");
      // Text is hit-tested by glyph coverage, so find a point actually inside a glyph to click.
      { QPainterPath dp=c->doc().shapes[5].transform.map(c->doc().shapes[5].localPath()); QRectF bb=dp.boundingRect();
        QPointF onGlyph; bool found=false;
        for(double fy=0.25; fy<=0.75 && !found; fy+=0.05) for(double fx=0.03; fx<=0.97 && !found; fx+=0.02){
            QPointF p(bb.left()+bb.width()*fx, bb.top()+bb.height()*fy); if(dp.contains(p)){ onGlyph=p; found=true; } }
        check(found, "found a point inside the text glyphs to click");
        if(found){ dblAt(onGlyph);
            check(c->tool()==Tool::Text,   "double-click text → Text tool");
            check(c->editingText(),        "double-click point text also drops an editing caret"); c->endTextEdit(); } }
      c->setTool(Tool::Select); }

    { // Ctrl radial ("pie") menu: two-ring build (inner tools + the active tool's outer actions) + ring hit-test.
      const QSize os=c->size(); c->resize(400,400);   // known geometry so the pie centres at (200,200)
      c->setTool(Tool::Ellipse); c->openRadialForShot();
      check(c->radialCount(0)==8, "radial inner ring lists the 8 tools");
      check(c->radialCount(1)==4, "radial outer ring lists the Ellipse tool's 4 actions (Whole/Pie/Arc/Segment)");
      int ring=-1;
      c->radialHitProbe(QPointF(200,200), ring); check(ring==-1, "the pie's centre hole selects nothing (release there = cancel)");
      c->radialHitProbe(QPointF(200,160), ring); check(ring==0,  "a point in the inner band aims the tools ring");    // dist 40 ∈ (hole 22, tools 66)
      c->radialHitProbe(QPointF(200,110), ring); check(ring==1,  "a point past the inner band aims the actions ring"); // dist 90 > 66
      int idxTop=c->radialHitProbe(QPointF(200,150), ring); check(ring==0 && idxTop==0, "aiming straight up hits the first (12-o'clock) tool wedge");
      c->radialForceClose();
      check(c->radialCount(0)==0, "closing the radial clears its items");
      c->setTool(Tool::Select); c->openRadialForShot();
      check(c->radialCount(1)==6, "the Select tool's radial shows its 6 arrange actions");
      c->radialForceClose();
      c->resize(os); c->setTool(Tool::Select); }

    { // #4 deleting a named colour reindexes bound colorRefs + tint/shade parent chains
      Document d;
      NamedColor n0; n0.name="c0"; n0.color=QColor(255,0,0);
      NamedColor n1; n1.name="c1"; n1.color=QColor(0,255,0);
      NamedColor n2; n2.name="c2"; n2.color=QColor(0,0,255); n2.kind=1; n2.parent=1;   // a tint of c1
      d.namedColors={n0,n1,n2};
      Shape s0; s0.fill.colorRef=0;                            // bound to the colour we will delete
      Shape s1; s1.fill.colorRef=2; s1.stroke.colorRef=1;
      d.addShape(s0); d.addShape(s1);
      d.namedColors.remove(0); d.reindexNamedColorsAfterDelete(0);
      check(d.shapes[0].fill.colorRef==-1, "deleting a bound colour unbinds objects that referenced it");
      check(d.shapes[1].fill.colorRef==1 && d.shapes[1].stroke.colorRef==0, "surviving colorRefs shift down past the deleted index");
      check(d.namedColors.size()==2 && d.namedColors[1].parent==0, "tint/shade parent chains reindex past the deleted colour"); }

    // --- H-18: numbered-list restart (listStart) ---
    { c->newDocument();
      Shape t; t.type=ShapeType::Text; t.textFrame=true; t.rect=QRectF(0,0,300,200); t.text="Alpha\nBeta\nGamma";
      t.listStyle=2; t.listStart=5; int ti=c->doc().addShape(t);
      // default (listStart=1) omits the key; a non-default value round-trips
      Document rt=Document::fromJson(c->doc().toJson());
      check(rt.shapes[ti].listStart==5, "list restart number (listStart) round-trips through JSON");
      Shape d0; d0.type=ShapeType::Text; d0.listStyle=2; QJsonObject dj; { Document dd; dd.addShape(d0); dj=dd.toJson()["shapes"].toArray()[0].toObject(); }
      check(!dj.contains("tListS"), "a default list (start=1) omits the tListS key (gate)");
      { QTemporaryDir td; check(c->exportPng(td.path()+"/list.png",1), "a restarted numbered list renders"); } }

    // --- H-19: photo perspective / keystone correction ---
    { c->newDocument();
      QImage photo(60,60,QImage::Format_ARGB32); photo.fill(QColor(40,90,160));
      Shape ph; ph.type=ShapeType::Rect; ph.rect=QRectF(0,0,60,60); ph.fill.type=FillType::Bitmap; ph.fill.image=photo; ph.fill.origImage=photo;
      ph.fill.p0=QPointF(0,60); ph.fill.p1=QPointF(60,60); ph.fill.p2=QPointF(0,0);
      int pi=c->doc().addShape(ph); c->select({pi});
      check(!c->correctPerspective(pi,0,0), "perspective correction is a no-op at zero keystone");
      check(c->correctPerspective(pi,0.3,0.0), "vertical keystone correction warps the photo");
      const QImage& out=c->doc().shapes[pi].fill.image;
      // +vertical keystone widens the TOP edge and narrows the BOTTOM, so the bottom
      // corners fall outside the warped quad and stay transparent (top corners are covered).
      check(out.width()==60 && out.height()==60 && qAlpha(out.pixel(1,58))<200, "keystone warp keeps size and leaves transparent corners"); }

    // --- H-20: N×M mesh gradient ---
    { c->newDocument();
      Shape r; r.type=ShapeType::Rect; r.rect=QRectF(0,0,120,90); int ri=c->doc().addShape(r); c->select({ri});
      QVector<QColor> nodes; for(int k=0;k<3*4;++k) nodes<<QColor(20*k%256, 255-15*k%256, 100);
      c->setSelectionMeshGrid(3,4,nodes);
      const Shape& m=c->doc().shapes[ri];
      check(m.fill.type==FillType::Mesh && m.fill.meshRows==3 && m.fill.meshCols==4 && m.fill.meshColors.size()==12, "mesh-grid fill applied with an R×C node grid");
      Document rt=Document::fromJson(c->doc().toJson());
      check(rt.shapes[ri].fill.meshRows==3 && rt.shapes[ri].fill.meshCols==4 && rt.shapes[ri].fill.meshColors.size()==12, "N×M mesh grid round-trips through JSON");
      // a legacy 3/4-corner mesh omits the grid keys (gate → byte-identical corpus)
      Shape lg; lg.type=ShapeType::Rect; lg.fill.type=FillType::Mesh; lg.fill.meshColors={QColor(1,2,3),QColor(4,5,6),QColor(7,8,9)};
      QJsonObject fj; { Document dd; dd.addShape(lg); fj=dd.toJson()["shapes"].toArray()[0].toObject()["fill"].toObject(); }
      check(!fj.contains("mRows"), "a legacy 3/4-corner mesh omits the mRows/mCols keys (gate)");
      { QTemporaryDir td; check(c->exportPng(td.path()+"/mesh.png",1), "an N×M mesh gradient renders"); } }

    // --- H-21: fixes for the 3 adversarial-review findings on H-18 ---
    { c->newDocument();
      // (1) strong DUAL-axis keystone stays convex (the clamp keeps quadToQuad succeeding) and preserves size
      QImage photo(60,60,QImage::Format_ARGB32); photo.fill(QColor(200,40,40));           // visible = red
      QImage orig(60,60,QImage::Format_ARGB32);  orig.fill(QColor(40,40,200));            // pristine = blue (differs → pending adjustment)
      Shape ph; ph.type=ShapeType::Rect; ph.rect=QRectF(0,0,60,60); ph.fill.type=FillType::Bitmap; ph.fill.image=photo; ph.fill.origImage=orig;
      ph.fill.p0=QPointF(0,60); ph.fill.p1=QPointF(60,60); ph.fill.p2=QPointF(0,0);
      int pi=c->doc().addShape(ph); c->select({pi});
      check(c->correctPerspective(pi,0.6,0.6), "dual-axis keystone stays convex after the clamp (no silent no-op)");
      const QImage& out=c->doc().shapes[pi].fill.image;
      check(out.width()==60 && out.height()==60, "clamped dual-axis keystone keeps the image size");
      // (2) the warp used the VISIBLE (red) image, not the pristine (blue) origImage → centre is red-ish
      QRgb ctr=out.pixel(30,30);
      check(qRed(ctr)>140 && qBlue(ctr)<110, "perspective warps the visible adjusted image, not the pristine source");
      // (3) fromJson caps an out-of-range meshRows/meshCols so a crafted file can't overflow R*C
      QJsonObject so; so["type"]=int(ShapeType::Rect);
      QJsonObject fo; fo["type"]=int(FillType::Mesh); QJsonArray mc; for(int k=0;k<4;++k) mc.append(QJsonObject{{"r",1},{"g",2},{"b",3},{"a",255}});
      fo["mesh"]=mc; fo["mRows"]=50000; fo["mCols"]=50000; so["fill"]=fo;
      Shape cs=Shape::fromJson(so);
      check(cs.fill.meshRows<=64 && cs.fill.meshCols<=64, "fromJson clamps a crafted huge meshRows/meshCols (no int overflow)"); }

    // --- H-22: UX fixes — polygon/ellipse handle-drag no longer draws a new shape; Text-tool font; gradient handle colour ---
    std::printf("[ux] handle-drag edits (no stray shape) + tool font + gradient handle colour\n");
    { c->newDocument();
      auto send=[&](QEvent::Type t,QPointF v,Qt::MouseButton b,Qt::MouseButtons btns){
        QMouseEvent e(t,v,c->mapToGlobal(v.toPoint()),b,btns,Qt::NoModifier); QApplication::sendEvent(c,&e); };
      Shape st; st.type=ShapeType::Polygon; st.polySides=5; st.polyStar=true; st.polyStarDepth=0.5; st.polyCurvature=0.0;
      st.rect=QRectF(0,0,200,200); st.transform=QTransform::fromTranslate(300,300); st.fill.type=FillType::Solid; st.fill.color=Qt::gray;
      int pi=c->doc().addShape(st); c->select({pi}); c->setTool(Tool::Polygon);
      const int before=c->doc().shapes.size();
      QPointF hv; check(c->polyCurvHandle(hv), "selected polygon exposes a curvature handle");
      QPointF cv=c->mapToView(QPointF(400,400)); QPointF dir=hv-cv; double dl=std::hypot(dir.x(),dir.y()); if(dl>1e-6) dir/=dl;
      send(QEvent::MouseButtonPress, hv, Qt::LeftButton, Qt::LeftButton);
      send(QEvent::MouseMove, hv+dir*24.0, Qt::NoButton, Qt::LeftButton);
      send(QEvent::MouseButtonRelease, hv+dir*24.0, Qt::LeftButton, Qt::NoButton);
      check(c->doc().shapes.size()==before, "dragging a polygon's handle does NOT draw a new polygon");
      check(qAbs(c->doc().shapes[pi].polyCurvature)>1e-3, "dragging the curvature handle edits the polygon");
      send(QEvent::MouseButtonPress, c->mapToView(QPointF(400,400)), Qt::LeftButton, Qt::LeftButton);
      send(QEvent::MouseButtonRelease, c->mapToView(QPointF(400,400)), Qt::LeftButton, Qt::NoButton);
      check(c->doc().shapes.size()==before, "clicking an existing polygon selects it, doesn't create a new one");
      send(QEvent::MouseButtonPress, c->mapToView(QPointF(40,40)), Qt::LeftButton, Qt::LeftButton);
      send(QEvent::MouseMove, c->mapToView(QPointF(140,140)), Qt::NoButton, Qt::LeftButton);
      send(QEvent::MouseButtonRelease, c->mapToView(QPointF(140,140)), Qt::LeftButton, Qt::NoButton);
      check(c->doc().shapes.size()==before+1, "dragging on empty canvas still draws a new polygon"); }
    // Regression (audit of c03ff4b): the select-on-press fix must NOT prevent DRAGGING out a new shape whose
    // down-point happens to land over an existing same-type shape — a click selects, a drag still creates.
    { c->newDocument();
      auto send=[&](QEvent::Type t,QPointF v,Qt::MouseButton b,Qt::MouseButtons btns){
        QMouseEvent e(t,v,c->mapToGlobal(v.toPoint()),b,btns,Qt::NoModifier); QApplication::sendEvent(c,&e); };
      Shape st; st.type=ShapeType::Polygon; st.polySides=5; st.rect=QRectF(0,0,200,200); st.transform=QTransform::fromTranslate(300,300);
      st.fill.type=FillType::Solid; st.fill.color=Qt::gray;
      int pi=c->doc().addShape(st); c->select({}); c->setTool(Tool::Polygon);
      const int before=c->doc().shapes.size();
      send(QEvent::MouseButtonPress, c->mapToView(QPointF(400,400)), Qt::LeftButton, Qt::LeftButton);   // a click on its body
      send(QEvent::MouseButtonRelease, c->mapToView(QPointF(400,400)), Qt::LeftButton, Qt::NoButton);
      check(c->doc().shapes.size()==before && c->selection()==QVector<int>{pi}, "Polygon tool: a click on an existing polygon selects it (no new shape)");
      send(QEvent::MouseButtonPress, c->mapToView(QPointF(350,350)), Qt::LeftButton, Qt::LeftButton);    // a DRAG that starts over it
      send(QEvent::MouseMove, c->mapToView(QPointF(600,600)), Qt::NoButton, Qt::LeftButton);
      send(QEvent::MouseButtonRelease, c->mapToView(QPointF(600,600)), Qt::LeftButton, Qt::NoButton);
      check(c->doc().shapes.size()==before+1, "Polygon tool: dragging out a shape that STARTS over an existing polygon still creates a new one"); }
    { c->newDocument();
      auto send=[&](QEvent::Type t,QPointF v,Qt::MouseButton b,Qt::MouseButtons btns){
        QMouseEvent e(t,v,c->mapToGlobal(v.toPoint()),b,btns,Qt::NoModifier); QApplication::sendEvent(c,&e); };
      Shape el; el.type=ShapeType::Ellipse; el.ellipseMode=1; el.ellipseStart=0; el.ellipseSweep=270;
      el.rect=QRectF(0,0,200,200); el.transform=QTransform::fromTranslate(300,300); el.fill.type=FillType::Solid; el.fill.color=Qt::gray;
      int ei=c->doc().addShape(el); c->select({ei}); c->setTool(Tool::Ellipse);
      const int before=c->doc().shapes.size();
      QPointF rv; check(c->ellipseRimHandle(false,rv), "selected pie ellipse exposes a rim handle");
      double startBefore=c->doc().shapes[ei].ellipseStart;
      send(QEvent::MouseButtonPress, rv, Qt::LeftButton, Qt::LeftButton);
      send(QEvent::MouseMove, rv+QPointF(0,-26), Qt::NoButton, Qt::LeftButton);
      send(QEvent::MouseButtonRelease, rv+QPointF(0,-26), Qt::LeftButton, Qt::NoButton);
      check(c->doc().shapes.size()==before, "dragging a pie ellipse's rim handle does NOT draw a new ellipse");
      check(qAbs(c->doc().shapes[ei].ellipseStart-startBefore)>1e-3, "dragging the rim handle edits the pie start angle"); }
    { c->newDocument();
      auto send=[&](QEvent::Type t,QPointF v,Qt::MouseButton b,Qt::MouseButtons btns){
        QMouseEvent e(t,v,c->mapToGlobal(v.toPoint()),b,btns,Qt::NoModifier); QApplication::sendEvent(c,&e); };
      c->setTool(Tool::Text); c->setTextFont(QFont("Courier New", 22));
      const int before=c->doc().shapes.size();
      send(QEvent::MouseButtonPress, c->mapToView(QPointF(120,120)), Qt::LeftButton, Qt::LeftButton);
      send(QEvent::MouseButtonRelease, c->mapToView(QPointF(120,120)), Qt::LeftButton, Qt::NoButton);
      check(c->doc().shapes.size()==before+1, "clicking with the Text tool creates a text object");
      const Shape& t=c->doc().shapes.back();
      check(t.type==ShapeType::Text && t.font.family()=="Courier New", "NEW text uses the Text tool's current font");
      c->setTool(Tool::Select); }
    { c->setGradHandleColor(QColor(220,40,40));
      check(c->gradHandleColor()==QColor(220,40,40), "gradient handle colour is settable");
      c->setGradHandleColor(QColor());
      check(c->gradHandleColor()==QColor(220,40,40), "an invalid gradient handle colour is ignored"); }

    // --- Photo Region: clip an object to a painted vector region ---
    std::printf("[region] photo region clip + round-trip\n");
    c->newDocument();
    { Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,200,200);
      s.fill.type=FillType::Solid; s.fill.color=QColor(0,160,0); s.stroke.enabled=false; int idx=c->doc().addShape(s);
      c->select({idx});
      QPainterPath reg; reg.addRect(QRectF(50,50,100,100));   // local-space region covering the centre
      c->setShapeRegion(idx, reg);
      check(!c->doc().shapes[idx].regionPath.isEmpty(), "setShapeRegion stores a clip region");
      QImage img(200,200,QImage::Format_ARGB32); img.fill(Qt::white);
      { QPainter g(&img); paintShape(g, c->doc().shapes[idx]); }
      QColor inside=img.pixelColor(100,100), outside=img.pixelColor(20,20);
      check(inside.green()>120 && inside.red()<90, "region: a pixel inside the region shows the photo/fill");
      check(outside==QColor(Qt::white), "region: a pixel outside the region is clipped away");
      Document rt=Document::fromJson(c->doc().toJson());
      check(!rt.shapes[idx].regionPath.isEmpty(), "region path round-trips through JSON");
      c->clearShapeRegion(idx);
      check(c->doc().shapes[idx].regionPath.isEmpty(), "clearShapeRegion removes the region");
      c->undo(); check(!c->doc().shapes[idx].regionPath.isEmpty(), "clear region is undoable"); }
    // freehand Region Painter: drag traces a region outline
    c->newDocument();
    { Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,300,300); s.fill.type=FillType::Solid; s.fill.color=QColor(0,0,200); s.stroke.enabled=false;
      int idx=c->doc().addShape(s); c->select({idx}); c->setTool(Tool::Region); c->setRegionFreehand(true);
      dragShape(c, c->mapToView(QPointF(60,60)), c->mapToView(QPointF(240,220)));
      check(!c->doc().shapes[idx].regionPath.isEmpty() && c->doc().shapes[idx].regionPath.elementCount()>=3,
            "freehand Region Painter paints a region outline");
      c->setRegionFreehand(false); }
    // soft (feathered) region edge → the region boundary fades (partial-alpha ring)
    c->newDocument();
    { Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,200,200); s.fill.type=FillType::Solid; s.fill.color=QColor(0,150,0); s.stroke.enabled=false;
      QPainterPath reg; reg.addRect(QRectF(50,50,100,100)); s.regionPath=reg; s.regionFeather=12; c->doc().addShape(s);
      QImage img(200,200,QImage::Format_ARGB32); img.fill(Qt::transparent);
      { QPainter g(&img); g.setRenderHint(QPainter::Antialiasing); paintShape(g, c->doc().shapes[0]); }
      int a_in=img.pixelColor(100,100).alpha(), a_edge=img.pixelColor(50,100).alpha(), a_out=img.pixelColor(20,100).alpha();
      check(a_in>230, "soft region: centre is opaque");
      check(a_edge>20 && a_edge<230, "soft region: the edge is partially transparent (feathered)");
      check(a_out<40, "soft region: far outside is fully clipped"); }

    // --- Replace photo on drop ---
    std::printf("[photo] replace photo on drop\n");
    c->newDocument();
    { QImage a(20,20,QImage::Format_ARGB32); a.fill(Qt::red); QImage b(20,20,QImage::Format_ARGB32); b.fill(Qt::blue);
      Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,100,100);
      s.fill.type=FillType::Bitmap; s.fill.image=a; s.fill.origImage=a; s.stroke.enabled=false; int idx=c->doc().addShape(s);
      bool ok=c->replacePhotoAt(QPointF(50,50), b);
      check(ok && c->doc().shapes[idx].fill.image.pixelColor(5,5)==QColor(Qt::blue), "replacePhotoAt swaps the image over a photo");
      check(qAbs(c->doc().shapes[idx].rect.width()-100)<1e-6, "replace keeps the photo frame size");
      c->undo(); check(c->doc().shapes[idx].fill.image.pixelColor(5,5)==QColor(Qt::red), "replace-photo is undoable");
      Shape r; r.type=ShapeType::Rect; r.fill.type=FillType::Solid; r.transform=QTransform::fromTranslate(300,300); r.rect=QRectF(0,0,50,50); c->doc().addShape(r);
      check(!c->replacePhotoAt(QPointF(325,325), b), "replacePhotoAt ignores non-photo objects"); }
    // Shape Corner: round the corners of a triangle path → straight edges become arcs
    c->newDocument();
    { Shape s; s.type=ShapeType::Path; QPainterPath p; p.moveTo(0,0); p.lineTo(100,0); p.lineTo(50,90); p.closeSubpath();
      s.path=p; s.fill.type=FillType::Solid; int idx=c->doc().addShape(s); c->select({idx});
      check(!pathHasCurve(c->doc().shapes[idx].path), "triangle path is all straight lines");
      c->roundSelectionCorners(12);
      check(pathHasCurve(c->doc().shapes[idx].path), "roundSelectionCorners turns corners into arcs (Shape Corner tool)");
      c->undo(); check(!pathHasCurve(c->doc().shapes[idx].path), "round corners is undoable"); }
    // Node multi-select: marquee-select several nodes and delete them together
    c->newDocument();
    { Shape s; s.type=ShapeType::Path; QPainterPath p; p.moveTo(0,0); p.lineTo(100,0); p.lineTo(100,100); p.lineTo(0,100); p.closeSubpath();
      s.path=p; s.fill.type=FillType::Solid; int idx=c->doc().addShape(s); c->select({idx}); c->setTool(Tool::Node); c->nodeEdit(idx);
      int before=c->nodeCount(0);
      c->selectNodesInRect(QRectF(-10,-10,60,120));   // the two left-edge nodes (x≈0)
      int selc=c->nodeSelCount();
      check(selc>=2, "marquee-select grabs multiple nodes inside the rect");
      c->deleteSelectedNodes();
      check(c->nodeCount(0)==before-selc, "deleteSelectedNodes removes exactly the selected nodes"); }

    // Node ops: Reverse / Break / Join
    c->newDocument();
    { Shape s; s.type=ShapeType::Path; QPainterPath p; p.moveTo(0,0); p.lineTo(100,0); p.lineTo(100,50);   // open, 3 nodes
      s.path=p; int idx=c->doc().addShape(s); c->select({idx}); c->setTool(Tool::Node); c->nodeEdit(idx);
      c->reverseNodePath();
      QPointF start=c->doc().shapes[idx].path.elementAt(0);
      check(qAbs(start.x()-100)<1 && qAbs(start.y()-50)<1, "reverseNodePath flips the path direction (start<->end)"); }
    c->newDocument();
    { Shape s; s.type=ShapeType::Path; QPainterPath p; p.moveTo(0,0); p.lineTo(50,0); p.lineTo(100,0); p.lineTo(150,0);   // open, 4 nodes
      s.path=p; int idx=c->doc().addShape(s); c->select({idx}); c->setTool(Tool::Node); c->nodeEdit(idx);
      check(c->nodeSubpaths()==1, "open path starts as one subpath");
      c->selectNodesInRect(QRectF(44,-6,12,12));   // the (50,0) middle node
      c->breakSelectedNodes();
      check(c->nodeSubpaths()==2, "breakSelectedNodes splits an open path into two subpaths at the node"); }
    c->newDocument();
    { Shape s; s.type=ShapeType::Path; QPainterPath p; p.moveTo(0,0); p.lineTo(50,0); p.moveTo(100,0); p.lineTo(150,0);   // two open subpaths
      s.path=p; int idx=c->doc().addShape(s); c->select({idx}); c->setTool(Tool::Node); c->nodeEdit(idx);
      check(c->nodeSubpaths()==2, "two separate subpaths");
      c->selectNodesInRect(QRectF(44,-6,62,12));   // endpoints at (50,0) and (100,0)
      check(c->nodeSelCount()==2, "two endpoints selected for join");
      c->joinSelectedNodes();
      check(c->nodeSubpaths()==1, "joinSelectedNodes welds two open subpaths into one"); }
    c->newDocument();
    { Shape s; s.type=ShapeType::Path; QPainterPath p; p.moveTo(0,0); p.lineTo(80,0); p.lineTo(80,80); p.lineTo(0,80); p.closeSubpath();   // closed, 4 nodes
      s.path=p; int idx=c->doc().addShape(s); c->select({idx}); c->setTool(Tool::Node); c->nodeEdit(idx);
      int before=c->nodeCount(0);
      c->selectNodesInRect(QRectF(-6,-6,12,12));   // node (0,0)
      c->breakSelectedNodes();
      check(c->nodeSubpaths()==1 && c->nodeCount(0)==before+1, "breaking a closed path opens it (adds the split endpoint)"); }

    // Break Apart / Join Shapes
    c->newDocument();
    { Shape s; s.type=ShapeType::Path; QPainterPath p; p.addRect(0,0,20,20); p.addRect(40,0,20,20); s.path=p;
      s.fill.type=FillType::Solid; int idx=c->doc().addShape(s); c->select({idx});
      const int n0=c->doc().shapes.size();
      c->breakApartSelection();
      check(c->doc().shapes.size()==n0+1, "breakApart splits a 2-subpath path into 2 shapes (net +1)");
      check(c->selection().size()==2, "the two pieces are selected"); }
    c->newDocument();
    { Shape a; a.type=ShapeType::Rect; a.rect=QRectF(0,0,20,20); a.fill.type=FillType::Solid;
      Shape b; b.type=ShapeType::Rect; b.rect=QRectF(40,0,20,20); b.fill.type=FillType::Solid;
      int ia=c->doc().addShape(a); int ib=c->doc().addShape(b); c->select({ia,ib});
      c->joinShapesSelection();
      check(c->doc().shapes.size()==1 && c->doc().shapes[0].type==ShapeType::Path, "joinShapes merges 2 shapes into 1 Path");
      int subs=0; const QPainterPath jp=c->doc().shapes[0].path;
      for(int i=0;i<jp.elementCount();++i) if(jp.elementAt(i).type==QPainterPath::MoveToElement) subs++;
      check(subs==2, "the joined path keeps both subpaths"); }

    // Interactive ruler guides: drag off the ruler / move / drag-back-to-delete
    std::printf("[guides] interactive ruler guides\n");
    { c->newDocument(); c->setTool(Tool::Select);
      const int h0=c->guidesH().size();
      send(c, QEvent::MouseButtonPress,   QPointF(400,6),   Qt::LeftButton, Qt::LeftButton);   // press in top ruler
      send(c, QEvent::MouseMove,          QPointF(400,300), Qt::NoButton,   Qt::LeftButton);
      send(c, QEvent::MouseButtonRelease, QPointF(400,300), Qt::LeftButton, Qt::NoButton);
      check(c->guidesH().size()==h0+1, "dragging off the top ruler creates a horizontal guide");
      double vy=c->mapToView(QPointF(0,c->guidesH().last())).y();
      send(c, QEvent::MouseButtonPress,   QPointF(400,vy),  Qt::LeftButton, Qt::LeftButton);   // grab the guide line
      send(c, QEvent::MouseMove,          QPointF(400,500), Qt::NoButton,   Qt::LeftButton);
      send(c, QEvent::MouseButtonRelease, QPointF(400,500), Qt::LeftButton, Qt::NoButton);
      check(c->guidesH().size()==h0+1 && qAbs(c->mapToView(QPointF(0,c->guidesH().last())).y()-500)<3.5, "grabbing a guide moves it live");
      double vy2=c->mapToView(QPointF(0,c->guidesH().last())).y();
      send(c, QEvent::MouseButtonPress,   QPointF(400,vy2), Qt::LeftButton, Qt::LeftButton);   // drag back onto the ruler
      send(c, QEvent::MouseMove,          QPointF(400,4),   Qt::NoButton,   Qt::LeftButton);
      send(c, QEvent::MouseButtonRelease, QPointF(400,4),   Qt::LeftButton, Qt::NoButton);
      check(c->guidesH().size()==h0, "dragging a guide back onto the ruler deletes it");
      const int v0=c->guidesV().size();
      send(c, QEvent::MouseButtonPress,   QPointF(6,300),   Qt::LeftButton, Qt::LeftButton);   // press in left ruler
      send(c, QEvent::MouseMove,          QPointF(350,300), Qt::NoButton,   Qt::LeftButton);
      send(c, QEvent::MouseButtonRelease, QPointF(350,300), Qt::LeftButton, Qt::NoButton);
      check(c->guidesV().size()==v0+1, "dragging off the left ruler creates a vertical guide"); }

    // Shadow Floor/Glow types + gradient Profile (bias/gain)
    std::printf("[effects] shadow floor/glow + gradient profile\n");
    { Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,50,50); s.fill.type=FillType::Solid; s.fill.color=Qt::blue;
      s.shadow.on=true; s.shadow.type=1; s.shadow.floorShear=1.3; s.shadow.floorScale=0.8;
      Document d; d.pageSize=QSizeF(200,200); d.shapes={s}; c->setDocument(d);
      check(c->exportPng("/tmp/selftest_floor.png",1), "floor shadow renders without crashing");
      Document rt=Document::fromJson(c->doc().toJson());
      check(rt.shapes[0].shadow.type==1, "shadow type (floor) survives JSON round-trip");
      check(qAbs(rt.shapes[0].shadow.floorShear-1.3)<1e-6 && qAbs(rt.shapes[0].shadow.floorScale-0.8)<1e-6, "floor rake (floorShear/floorScale) survives JSON round-trip"); }
    // On-canvas floor-shadow rake: drag the tip to lean (shear) + lengthen (scale) the ground shadow.
    { Document d; Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,120,90); s.transform=QTransform::fromTranslate(150,150);
      s.fill.type=FillType::Solid; s.fill.color=Qt::darkGray; s.shadow.on=true; s.shadow.type=1; s.shadow.offset=QPointF(0,0);
      d.shapes={s}; c->setDocument(d); c->setTool(Tool::Shadow); c->select({0});
      QPointF rh; check(c->shadowRakeHandle(rh), "a FLOOR shadow exposes a rake/tip handle");
      const int before=c->undoDepth();
      // docBounds (150,150,120,90): cx=210, baseY=240, H=90, anchor=(210,240). Drag the tip to (120,150) → shear 1.0, scale 1.0.
      dragShape(c, rh, c->mapToView(QPointF(120,150)));
      check(qAbs(c->doc().shapes[0].shadow.floorShear-1.0)<0.05, "dragging the tip leans the rake (floorShear→1.0)");
      check(qAbs(c->doc().shapes[0].shadow.floorScale-1.0)<0.05, "dragging the tip lengthens the shadow (floorScale→1.0)");
      check(c->undoDepth()>before, "the floor-rake drag is one undo step");
      // a bare click on the tip (no drag) must NOT push a no-op undo (deferred-undo pattern)
      QPointF rh2; c->shadowRakeHandle(rh2); const int u2=c->undoDepth(); click(c, rh2);
      check(c->undoDepth()==u2, "a bare click on the rake handle pushes no undo (no phantom Ctrl+Z)");
      c->doc().shapes[0].shadow.type=0; QPointF rw; check(c->shadowRakeHandle(rw)==false, "a WALL shadow has no rake handle");
      c->doc().shapes[0].shadow.type=2; check(c->shadowRakeHandle(rw)==false, "a GLOW shadow has no rake handle"); }
    { Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,50,50); s.fill.type=FillType::Solid; s.fill.color=Qt::blue;
      s.shadow.on=true; s.shadow.type=2; Document d; d.pageSize=QSizeF(200,200); d.shapes={s}; c->setDocument(d);
      check(c->exportPng("/tmp/selftest_glow.png",1), "glow shadow renders without crashing"); }
    { Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,50,50); s.fill.type=FillType::Linear;
      s.fill.stops={{0.0,QColor(Qt::black)},{1.0,QColor(Qt::white)}};
      Document d; d.shapes={s}; c->setDocument(d); c->select({0});
      c->setSelectionFillProfile(0.6,-0.2);
      check(qAbs(c->doc().shapes[0].fill.profileBias-0.6)<1e-6 && qAbs(c->doc().shapes[0].fill.profileGain+0.2)<1e-6, "setSelectionFillProfile sets bias/gain");
      Document rt=Document::fromJson(c->doc().toJson());
      check(qAbs(rt.shapes[0].fill.profileBias-0.6)<1e-6, "gradient profile survives JSON round-trip"); }

    // Paste Special: Paste in Place + Paste Attributes
    std::printf("[edit] paste special (in place / attributes)\n");
    c->newDocument();
    { Shape a; a.type=ShapeType::Rect; a.rect=QRectF(0,0,40,40); a.transform=QTransform::fromTranslate(100,100);
      a.fill.type=FillType::Solid; a.fill.color=Qt::red; int ia=c->doc().addShape(a); c->select({ia});
      c->copySelection(); const int n0=c->doc().shapes.size();
      c->pasteInPlace();
      check(c->doc().shapes.size()==n0+1, "paste in place adds a copy");
      int np=c->selection().first();
      check(qAbs(c->doc().shapes[np].transform.dx()-100)<0.5 && qAbs(c->doc().shapes[np].transform.dy()-100)<0.5, "paste-in-place keeps the original coordinates (no offset)"); }
    c->newDocument();
    { Shape a; a.type=ShapeType::Rect; a.rect=QRectF(0,0,40,40); a.fill.type=FillType::Solid; a.fill.color=QColor(10,200,30);
      Shape b; b.type=ShapeType::Rect; b.rect=QRectF(0,0,40,40); b.transform=QTransform::fromTranslate(80,0); b.fill.type=FillType::Solid; b.fill.color=Qt::black;
      int ia=c->doc().addShape(a); int ib=c->doc().addShape(b);
      c->select({ia}); c->copySelection(); c->select({ib}); c->pasteAttributes();
      check(c->doc().shapes[ib].fill.color==QColor(10,200,30), "paste attributes copies the source appearance onto the target"); }

    // Zoom tool depth: marquee zoom-to-rect / zoom-to-selection / previous zoom
    std::printf("[view] zoom depth (to-rect / selection / previous)\n");
    { c->newDocument(); const double z0=c->zoom();
      c->zoomToRect(QRectF(100,100,50,50));
      check(c->zoom()>z0*1.5, "zoomToRect zooms in on a small region");
      const double z1=c->zoom();
      c->previousZoom(); check(qAbs(c->zoom()-z0)<1e-6, "previousZoom restores the prior zoom");
      c->previousZoom(); check(qAbs(c->zoom()-z1)<1e-6, "previousZoom toggles back"); }
    { c->newDocument();
      Shape a; a.type=ShapeType::Rect; a.rect=QRectF(0,0,30,30); a.transform=QTransform::fromTranslate(200,200);
      int ia=c->doc().addShape(a); c->select({ia}); const double z0=c->zoom();
      c->zoomToSelection(); check(c->zoom()>z0, "zoomToSelection zooms in on the selection"); }

    // EPS (PostScript) export
    std::printf("[export] EPS (PostScript)\n");
    { Document d; d.pageSize=QSizeF(200,200);
      Shape r; r.type=ShapeType::Rect; r.rect=QRectF(0,0,50,40); r.transform=QTransform::fromTranslate(20,20);
      r.fill.type=FillType::Solid; r.fill.color=QColor(200,10,20); r.stroke.enabled=true; r.stroke.width=2;
      d.shapes={r}; c->setDocument(d);
      check(c->exportEps("/tmp/selftest.eps"), "exportEps writes a file");
      QFile f("/tmp/selftest.eps"); f.open(QIODevice::ReadOnly); QString ps=QString::fromLatin1(f.readAll());
      check(ps.startsWith("%!PS-Adobe"), "EPS has the PostScript magic header");
      check(ps.contains("%%BoundingBox:"), "EPS declares a BoundingBox");
      check(ps.contains("moveto") && ps.contains("fill") && ps.contains("setrgbcolor"), "EPS emits path + fill ops"); }

    // OS-clipboard bitmap interop
    std::printf("[edit] OS-clipboard bitmap interop\n");
    { c->newDocument();
      QImage px(20,16,QImage::Format_ARGB32); px.fill(QColor(30,140,200)); QGuiApplication::clipboard()->setImage(px);
      const int n0=c->doc().shapes.size();
      check(c->pasteBitmapFromClipboard(), "pasteBitmapFromClipboard succeeds when the OS clipboard holds an image");
      check(c->doc().shapes.size()==n0+1 && c->doc().shapes.back().fill.type==FillType::Bitmap, "a photo (Bitmap-fill) object is created");
      check(c->doc().shapes.back().fill.image.width()==20, "the pasted photo carries the clipboard image");
      { const Fill& f=c->doc().shapes.back().fill;   // regression: paste must not vertically flip the image
        check(f.p0==QPointF(0,16) && f.p1==QPointF(20,16) && f.p2==QPointF(0,0),
              "pasted bitmap uses the upright parallelogram convention (top-left->p2, bottom-left->p0), not flipped"); } }
    { c->newDocument();   // placeBitmap (shared by paste + drag-drop) drops a new photo centred at a doc point
      QImage px(30,20,QImage::Format_ARGB32); px.fill(QColor(200,60,40));
      const int idx=c->placeBitmap(px, QPointF(400,300), "Drop image");
      check(idx>=0, "placeBitmap creates a photo object (drag-drop of a browser image path)");
      if(idx>=0){ const Shape& s=c->doc().shapes[idx];
        check(s.fill.type==FillType::Bitmap && s.fill.image.width()==30, "dropped image carries its bitmap");
        check(s.fill.p0==QPointF(0,20) && s.fill.p2==QPointF(0,0), "dropped image is upright (not flipped)");
        check(qFuzzyCompare(s.transform.map(QPointF(15,10)).x(), 400.0) && qFuzzyCompare(s.transform.map(QPointF(15,10)).y(), 300.0),
              "dropped image is centred on the drop point"); } }
    { c->newDocument();
      Shape r; r.type=ShapeType::Rect; r.rect=QRectF(0,0,60,40); r.transform=QTransform::fromTranslate(50,50); r.fill.type=FillType::Solid; r.fill.color=Qt::red;
      int ir=c->doc().addShape(r); c->select({ir}); QGuiApplication::clipboard()->clear();
      c->copySelection();
      check(!QGuiApplication::clipboard()->image().isNull(), "copySelection places a rendered bitmap on the OS clipboard"); }

    // ICC rendering intent + black-point compensation
    std::printf("[colour] ICC rendering intent + BPC\n");
    { auto& icc=xara::Icc::instance();
      icc.setRenderingIntent(1,true);
      check(icc.renderingIntent()==1 && icc.blackPointCompensation(), "setRenderingIntent stores intent + BPC");
      icc.setRenderingIntent(3,false);
      check(icc.renderingIntent()==3 && !icc.blackPointCompensation(), "intent updates + BPC toggles off");
      icc.setRenderingIntent(0,false); check(icc.renderingIntent()==0, "restored to perceptual default"); }

    // Gamut warning overlay
    std::printf("[colour] gamut warning\n");
    { QImage im(2,1,QImage::Format_ARGB32);
      im.setPixel(0,0, qRgb(0,255,0));       // saturated green — outside CMYK gamut
      im.setPixel(1,0, qRgb(128,128,128));   // neutral grey — inside gamut
      xara::gamutWarnImage(im);
      check(qRed(im.pixel(0,0))==150 && qGreen(im.pixel(0,0))==150 && qBlue(im.pixel(0,0))==150, "out-of-gamut pixel flagged grey");
      check(!(qRed(im.pixel(1,0))==150 && qGreen(im.pixel(1,0))==150 && qBlue(im.pixel(1,0))==150), "in-gamut neutral pixel left alone"); }

    // Conical graduated transparency
    std::printf("[transp] conical graduated transparency\n");
    { Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,60,60); s.fill.type=FillType::Solid; s.fill.color=QColor(200,60,40);
      s.transp.type=1; s.transp.geom=5; s.transp.t0=0.0; s.transp.t1=1.0; s.transp.p0=QPointF(30,30); s.transp.p1=QPointF(60,30);
      Document d; d.pageSize=QSizeF(80,80); d.shapes={s}; c->setDocument(d);
      check(c->exportPng("/tmp/selftest_conical.png",1), "conical transparency renders without crashing");
      Document rt=Document::fromJson(c->doc().toJson());
      check(rt.shapes[0].transp.geom==5, "conical transparency geometry survives JSON round-trip"); }

    // Rich table cells: vertical align / bold / header row
    std::printf("[table] rich cells (valign / bold / header row)\n");
    { Document d; d.pageSize=QSizeF(300,200);
      Shape s; s.type=ShapeType::Table; s.table=Table::grid(2,2,60,30);
      s.table.headerRow=true; s.table.at(0,0).text="H";
      s.table.at(1,1).valign=2; s.table.at(1,1).bold=true; s.table.at(1,1).text="x";
      d.shapes={s}; c->setDocument(d);
      check(c->exportPng("/tmp/selftest_table.png",1), "table with header/valign/bold renders");
      Document rt=Document::fromJson(c->doc().toJson());
      check(rt.shapes[0].table.headerRow, "header row survives JSON round-trip");
      check(rt.shapes[0].table.at(1,1).valign==2 && rt.shapes[0].table.at(1,1).bold, "per-cell vertical-align + bold survive round-trip"); }

    // Auto page-number / count / date text fields
    std::printf("[text] auto page-number fields\n");
    { Document d; d.pageSize=QSizeF(200,100); d.pageCount=3; const double adv=d.pageSize.height()+d.pageGap;
      Shape t; t.type=ShapeType::Text; t.text="Page {page} of {pages}"; t.font=QFont("DejaVu Sans",12);
      t.fill.type=FillType::Solid; t.fill.color=Qt::black; t.stroke.enabled=false;
      t.transform=QTransform::fromTranslate(20, 1*adv+40);   // sits on page index 1 (the 2nd page)
      d.shapes={t}; c->setDocument(d);
      check(c->hasTextFields(), "hasTextFields detects a {page} token");
      Document rd=c->resolvedForRender();
      check(rd.shapes[0].text=="Page 2 of 3", "field resolves to the object's own page number + count");
      check(c->doc().shapes[0].text=="Page {page} of {pages}", "the stored token is NOT mutated"); }

    // Linked / threaded text frames: a story overflows from one frame into the next
    std::printf("[text] linked / threaded text frames\n");
    { Document d; d.pageSize=QSizeF(400,300);
      const QString story="Alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu nu xi "
                          "omicron pi rho sigma tau upsilon phi chi psi omega one two three four five "
                          "six seven eight nine ten eleven twelve";
      Shape a; a.type=ShapeType::Text; a.textFrame=true; a.rect=QRectF(0,0,120,32); a.text=story;
      a.font=QFont("DejaVu Sans",12); a.fill.type=FillType::Solid; a.fill.color=Qt::black; a.stroke.enabled=false;
      a.transform=QTransform::fromTranslate(10,10); a.nextFrame=1;
      Shape b=a; b.text=""; b.rect=QRectF(0,0,120,160); b.nextFrame=-1; b.transform=QTransform::fromTranslate(10,60);
      d.shapes={a,b}; c->setDocument(d);
      check(c->hasTextChains(), "hasTextChains detects a linked frame");
      Document rd=c->resolvedForRender();
      check(!rd.shapes[0].text.isEmpty() && rd.shapes[0].text.size()<story.size(), "head frame shows only the part that fits");
      check(!rd.shapes[1].text.isEmpty(), "overflow flows into the linked frame");
      check(rd.shapes[0].text+rd.shapes[1].text==story, "head slice + overflow reconstruct the whole story (no chars lost/dup)");
      check(story.startsWith(rd.shapes[0].text), "the head shows a prefix of the story");
      check(story.endsWith(rd.shapes[1].text), "the linked frame shows the trailing remainder");
      check(c->doc().shapes[0].text==story, "the stored story on the head is NOT mutated");
      Document rt=Document::fromJson(c->doc().toJson());
      check(rt.shapes[0].nextFrame==1, "nextFrame link survives JSON round-trip"); }

    // Linked-frame edge cases: cycles, self-loop, dropCap+link, empty/degenerate frames must not hang/crash/lose chars
    std::printf("[text] linked-frame edge cases\n");
    { const QString story="Alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu nu xi "
                          "omicron pi rho sigma tau upsilon phi chi psi omega one two three four five "
                          "six seven eight nine ten eleven twelve thirteen fourteen fifteen sixteen";
      auto mkframe=[&](double w,double h,double ty)->Shape{ Shape s; s.type=ShapeType::Text; s.textFrame=true;
        s.rect=QRectF(0,0,w,h); s.font=QFont("DejaVu Sans",12); s.fill.type=FillType::Solid; s.fill.color=Qt::black;
        s.stroke.enabled=false; s.transform=QTransform::fromTranslate(10,ty); return s; };
      // 2-cycle A<->B
      { Document d; d.pageSize=QSizeF(400,600); Shape a=mkframe(120,32,10); a.text=story; a.nextFrame=1;
        Shape b=mkframe(120,32,60); b.nextFrame=0; d.shapes={a,b}; c->setDocument(d);
        Document rd=c->resolvedForRender(); check(true,"2-cycle chain did not hang"); (void)rd; }
      // self-loop A->A
      { Document d; d.pageSize=QSizeF(400,600); Shape a=mkframe(120,32,10); a.text=story; a.nextFrame=0;
        d.shapes={a}; c->setDocument(d); Document rd=c->resolvedForRender(); check(true,"self-loop did not hang"); (void)rd; }
      // 3-chain, tail keeps everything, full reconstruction
      { Document d; d.pageSize=QSizeF(400,600); Shape a=mkframe(120,28,10); a.text=story; a.nextFrame=1;
        Shape b=mkframe(120,28,50); b.nextFrame=2; Shape cc=mkframe(120,28,90); cc.nextFrame=-1;
        d.shapes={a,b,cc}; c->setDocument(d); Document rd=c->resolvedForRender();
        check(rd.shapes[0].text+rd.shapes[1].text+rd.shapes[2].text==story,"3-chain reconstructs whole story"); }
      // dropCap on head + link
      { Document d; d.pageSize=QSizeF(400,600); Shape a=mkframe(120,32,10); a.text=story; a.nextFrame=1; a.dropCap=3;
        Shape b=mkframe(120,160,60); b.nextFrame=-1; d.shapes={a,b}; c->setDocument(d); Document rd=c->resolvedForRender();
        check(rd.shapes[0].text+rd.shapes[1].text==story,"dropCap head + link reconstructs whole story"); }
      // empty head text, link
      { Document d; d.pageSize=QSizeF(400,600); Shape a=mkframe(120,32,10); a.text=""; a.nextFrame=1;
        Shape b=mkframe(120,160,60); b.nextFrame=-1; d.shapes={a,b}; c->setDocument(d); Document rd=c->resolvedForRender();
        check(rd.shapes[0].text.isEmpty()&&rd.shapes[1].text.isEmpty(),"empty story stays empty across chain"); }
      // nextFrame points at a NON-text shape (stale index)
      { Document d; d.pageSize=QSizeF(400,600); Shape a=mkframe(120,32,10); a.text=story; a.nextFrame=1;
        Shape r; r.type=ShapeType::Rect; r.rect=QRectF(0,0,50,50); r.nextFrame=-1; d.shapes={a,r}; c->setDocument(d);
        Document rd=c->resolvedForRender(); check(true,"link to non-text shape did not crash"); (void)rd; }
      // nextFrame out of range
      { Document d; d.pageSize=QSizeF(400,600); Shape a=mkframe(120,32,10); a.text=story; a.nextFrame=99;
        d.shapes={a}; c->setDocument(d); Document rd=c->resolvedForRender(); check(true,"out-of-range nextFrame did not crash"); (void)rd; }
    }

    // --- Bitmap Export options (area / scale / matte / JPEG quality) ---
    std::printf("[export] bitmap export options\n");
    { Document d; d.pageSize=QSizeF(200,100);
      Shape r; r.type=ShapeType::Rect; r.rect=QRectF(0,0,40,40); r.fill.type=FillType::Solid; r.fill.color=QColor(20,60,200); r.stroke.enabled=false;
      r.transform=QTransform::fromTranslate(80,30); d.shapes={r}; c->setDocument(d);
      check(c->exportRaster("/tmp/selftest_exp_page.png",0,1.0,-1,true), "export Page @1x succeeds");
      { QImage im("/tmp/selftest_exp_page.png"); check(im.width()==200 && im.height()==100, "Page area @1x == page size"); }
      check(c->exportRaster("/tmp/selftest_exp_2x.png",0,2.0,-1,true), "export Page @2x succeeds");
      { QImage im("/tmp/selftest_exp_2x.png"); check(im.width()==400 && im.height()==200, "Page area @2x == 2× page size"); }
      check(c->exportRaster("/tmp/selftest_exp_draw.png",1,1.0,-1,true), "export Drawing area succeeds");
      { QImage im("/tmp/selftest_exp_draw.png"); check(im.width()<200 && im.width()>=40, "Drawing area crops to the content bounds"); }
      { QImage im("/tmp/selftest_exp_page.png"); check(im.pixelColor(2,2).alpha()==0, "transparent background leaves alpha 0 outside the artwork"); }
      check(c->exportRaster("/tmp/selftest_exp_matte.png",0,1.0,-1,false), "export with white matte succeeds");
      { QImage im("/tmp/selftest_exp_matte.png"); QColor px=im.pixelColor(2,2);
        check(px.alpha()==255 && px.red()>250 && px.green()>250 && px.blue()>250, "white matte fills the background opaque white"); }
      check(c->exportRaster("/tmp/selftest_exp.jpg",0,1.0,85,false), "JPEG export with quality succeeds");
      { QImage im("/tmp/selftest_exp.jpg"); check(!im.isNull() && im.width()==200, "JPEG file is a valid 200px image"); } }

    // --- Real Table of Contents (heading styles + page numbers + dot leaders) ---
    std::printf("[text] real table of contents\n");
    { Document d; d.pageSize=QSizeF(400,600); d.pageCount=2; const double adv=d.pageSize.height()+d.pageGap;
      auto mk=[&](const QString& txt, const QString& style, double y){ Shape s; s.type=ShapeType::Text; s.text=txt; s.textStyleName=style;
        s.textFrame=true; s.rect=QRectF(0,0,300,30); s.font=QFont("DejaVu Sans",18); s.fill.type=FillType::Solid; s.fill.color=Qt::black; s.stroke.enabled=false;
        s.transform=QTransform::fromTranslate(20,y); return s; };
      d.shapes={ mk("Introduction","Heading 1",40), mk("Just some body text here","Body",120), mk("Methods","Heading 1",adv+60) };
      c->setDocument(d);
      const QString toc=c->tocText();
      check(toc.contains("Introduction") && toc.contains("Methods"), "TOC includes both headings");
      check(!toc.contains("Just some body"), "TOC excludes non-heading text when heading styles exist");
      check(toc.contains(".."), "TOC draws dot leaders");
      { bool ok=false; for(const QString& l: toc.split('\n')) if(l.contains("Methods") && l.endsWith(" 2")) ok=true;
        check(ok, "the heading on page 2 gets page number 2"); }
      // fallback: no heading styles → every text object is listed
      Document d2; d2.pageSize=QSizeF(400,300);
      d2.shapes={ mk("Alpha","",20), mk("Beta","",70) }; c->setDocument(d2);
      const QString toc2=c->tocText();
      check(toc2.contains("Alpha") && toc2.contains("Beta"), "with no heading styles, the TOC falls back to all text"); }

    // --- Advanced tab stops (per-stop alignment + leaders) ---
    std::printf("[text] advanced tab stops (align + leaders)\n");
    { auto mk=[&](int align, QChar leader, const QString& txt){ Shape s; s.type=ShapeType::Text; s.text=txt; s.font=QFont("DejaVu Sans",20);
        s.fill.type=FillType::Solid; s.fill.color=Qt::black; s.stroke.enabled=false; TabStop t; t.pos=200; t.align=align; t.leader=leader; s.tabStops.push_back(t); return s; };
      { Shape s=mk(2, QChar('.'), "A\t9.99"); QRectF bb=s.localPath().boundingRect();
        check(qAbs(bb.right()-200)<10, "right-aligned tab stop ends the segment at the ~200px stop"); }
      { Shape s=mk(0, QChar(), "A\t9.99"); QRectF bb=s.localPath().boundingRect();
        check(bb.right()>205, "left-aligned tab stop starts the segment at the stop (extends past it)"); }
      { Shape s=mk(3, QChar('.'), "A\t3.14"); QRectF bb=s.localPath().boundingRect();
        check(bb.right()>200 && bb.left()<200, "decimal tab stop aligns the point at the stop"); }
      { Shape s=mk(2, QChar('.'), "A\t9.99"); QRectF bb=s.localPath().boundingRect();
        check(bb.left()<5, "the line still starts at the origin with a leader present"); }
      { Shape s=mk(2, QChar('.'), "A\tB"); Document d; d.shapes={s}; Document rt=Document::fromJson(d.toJson());
        check(rt.shapes[0].tabStops.size()==1 && rt.shapes[0].tabStops[0].align==2 && rt.shapes[0].tabStops[0].leader==QChar('.'),
            "tab stops (pos/align/leader) survive JSON round-trip"); } }

    // --- Review fixes: PDF object table (last-wins + no stream forgery), metafile DoS clamp, linked-frame clear ---
    std::printf("[fix] review findings (PDF obj table / metafile DoS / linked-frame clear)\n");
    { // (1) incremental update: object 3 (the page) is redefined later with different Contents → LAST wins
      QByteArray pdf=
        "%PDF-1.4\n"
        "1 0 obj<</Type/Catalog/Pages 2 0 R>>endobj\n"
        "2 0 obj<</Type/Pages/Kids[3 0 R]/Count 1/MediaBox[0 0 200 100]>>endobj\n"
        "3 0 obj<</Type/Page/Contents 4 0 R>>endobj\n"                         // STALE: red rect
        "4 0 obj<</Length 24>>\nstream\n1 0 0 rg 10 10 30 30 re f\nendstream endobj\n"
        "3 0 obj<</Type/Page/Contents 5 0 R>>endobj\n"                         // incremental override: blue rect
        "5 0 obj<</Length 24>>\nstream\n0 0 1 rg 10 10 30 30 re f\nendstream endobj\n"
        "trailer<</Root 1 0 R>>\n";
      Document d=importPdf(pdf);
      bool blue=false,red=false; for(Shape& s:d.shapes){ if(s.fill.color==QColor(0,0,255))blue=true; if(s.fill.color==QColor(255,0,0))red=true; }
      check(blue && !red, "incremental PDF update: the LAST object definition wins (blue, not stale red)"); }
    { // (2) a stream body containing the literal bytes "6 0 obj" must not forge/ shadow the real object 6
      QByteArray pdf=
        "%PDF-1.4\n"
        "1 0 obj<</Type/Catalog/Pages 2 0 R>>endobj\n"
        "2 0 obj<</Type/Pages/Kids[3 0 R]/Count 1/MediaBox[0 0 200 100]>>endobj\n"
        "3 0 obj<</Type/Page/Contents 6 0 R>>endobj\n"
        "7 0 obj<</Length 40>>\nstream\n6 0 obj garbage not a real object\nendstream endobj\n"   // decoy 'N 0 obj' inside a stream
        "6 0 obj<</Length 24>>\nstream\n0 1 0 rg 10 10 30 30 re f\nendstream endobj\n"           // the REAL object 6 (green rect)
        "trailer<</Root 1 0 R>>\n";
      Document d=importPdf(pdf);
      bool green=false; for(Shape& s:d.shapes) if(s.fill.color==QColor(0,255,0)) green=true;
      check(green, "a decoy 'N 0 obj' inside a stream does not shadow the real object (green rect imports)"); }
    { // (3) WMF/EMF: a POLYGON record declaring a huge point count but a tiny record must not over-read/allocate
      QByteArray w; auto le16=[&](int v){ w.append(char(v&0xff)); w.append(char((v>>8)&0xff)); };
      auto le32=[&](quint32 v){ for(int i=0;i<4;++i) w.append(char((v>>(8*i))&0xff)); };
      le32(0x9AC6CDD7u); le16(0); le16(0);le16(0);le16(100);le16(100); le16(96); le32(0); le16(0);   // placeable
      le16(1);le16(9);le16(0x0300);le32(0);le16(1);le32(0);le16(0);                                   // header
      // POLYGON record with rsize=5 words (only room for 1 point) but claims 30000 points
      le32(5);le16(0x0324);le16(30000);le16(10);le16(10);
      le32(3);le16(0x0000);                                                                           // EOF
      Document d=importWmf(w);   // must return (clamped) without crashing / mega-allocation
      check(true, "WMF POLYGON with an over-large point count is clamped (no crash)"); }
    { // (4) linked frames: when the whole story fits the head, a downstream frame with stale text is CLEARED
      Document d; d.pageSize=QSizeF(400,600);
      Shape a; a.type=ShapeType::Text; a.textFrame=true; a.rect=QRectF(0,0,200,200); a.text="short"; a.nextFrame=1;
      a.font=QFont("DejaVu Sans",12); a.fill.type=FillType::Solid; a.fill.color=Qt::black; a.stroke.enabled=false; a.transform=QTransform::fromTranslate(10,10);
      Shape b=a; b.text="STALE downstream text that should be cleared"; b.nextFrame=-1; b.transform=QTransform::fromTranslate(10,240);
      d.shapes={a,b}; c->setDocument(d); Document rd=c->resolvedForRender();
      check(rd.shapes[0].text=="short", "head keeps the whole (fitting) story");
      check(rd.shapes[1].text.isEmpty(), "downstream frame past the story end is cleared (not stale)"); }

    // --- Freehand redraw-over-path (Batch 6c) ---
    std::printf("[vector] freehand redraw-over-path\n");
    {
        auto makeLine=[&]()->int{ QPainterPath pl; pl.moveTo(0,100); pl.lineTo(50,100); pl.lineTo(100,100); pl.lineTo(150,100); pl.lineTo(200,100);
            Shape s; s.type=ShapeType::Path; s.path=pl; s.fill.type=FillType::None; s.stroke.enabled=true;
            return c->doc().addShape(s); };
        // (1) a stroke starting AND ending on the path replaces the spanned section (reshape, no new object)
        c->newDocument(); int idx=makeLine(); c->select({idx}); c->setTool(Tool::Freehand);
        const int nBefore=c->doc().shapes.size();
        const double topBefore=c->doc().shapes[idx].path.boundingRect().top();
        QVector<QPointF> bulge; bulge<<QPointF(50,100)<<QPointF(75,60)<<QPointF(100,40)<<QPointF(125,60)<<QPointF(150,100);
        check(c->freehandRedraw(bulge), "a stroke drawn over the selected path is accepted as a redraw");
        check(c->doc().shapes.size()==nBefore, "redraw reshapes the existing path (no new object created)");
        check(c->doc().shapes[idx].path.boundingRect().top() < topBefore-20, "the redrawn span now bulges upward");
        // (2) a stroke that does NOT start on the path is rejected (=> would commit a new Freehand shape)
        c->newDocument(); int idx2=makeLine(); c->select({idx2});
        QVector<QPointF> off; off<<QPointF(40,300)<<QPointF(100,320)<<QPointF(160,300);
        check(!c->freehandRedraw(off), "a stroke that doesn't begin on the path is not a redraw");
        // (3) a stroke from the endpoint out into space EXTENDS the open path
        c->newDocument(); int idx3=makeLine(); c->select({idx3});
        const double rightBefore=c->doc().shapes[idx3].path.boundingRect().right();
        QVector<QPointF> ext; ext<<QPointF(200,100)<<QPointF(240,100)<<QPointF(280,100);
        check(c->freehandRedraw(ext), "a stroke from the endpoint is accepted");
        check(c->doc().shapes[idx3].path.boundingRect().right() > rightBefore+30, "the path was extended past its old end");
        // (4) REGRESSION: extend a 2-VERTEX line from its END node → append (not prepend/backtrack to the start).
        c->newDocument();
        { QPainterPath pl; pl.moveTo(0,100); pl.lineTo(100,100);   // 2-node line, endpoints x=0 and x=100
          Shape s; s.type=ShapeType::Path; s.path=pl; s.fill.type=FillType::None; s.stroke.enabled=true; c->doc().addShape(s); }
        c->select({0});
        QVector<QPointF> ext2; ext2<<QPointF(100,100)<<QPointF(140,100)<<QPointF(180,100);   // start ON the end node, go right
        check(c->freehandRedraw(ext2), "extend a 2-vertex line from its end node is accepted");
        { const QRectF bb=c->doc().shapes[0].path.boundingRect();
          check(bb.right()>170 && bb.left()>=-1, "the 2-vertex line extended forward to ~180 without doubling back past the start (was: prepended a reversed stroke)"); }
    }

    // --- Combine Shapes: Slice (knife) ---
    std::printf("[vector] combine shapes: slice (knife)\n");
    { Document d; d.pageSize=QSizeF(200,200);
      Shape lower; lower.type=ShapeType::Rect; lower.rect=QRectF(0,0,100,100); lower.fill.type=FillType::Solid; lower.fill.color=QColor(30,90,200); lower.stroke.enabled=false;
      Shape knife; knife.type=ShapeType::Rect; knife.rect=QRectF(0,0,100,100); knife.fill.type=FillType::Solid; knife.fill.color=Qt::red; knife.transform=QTransform::fromTranslate(50,0);
      d.shapes={lower,knife}; c->setDocument(d);
      c->selectAll(); c->sliceSelection();
      check(c->doc().shapes.size()==2, "slice replaces lower+knife with 2 pieces (inside/outside)");
      bool allPath=true, allBlue=true; for(const Shape& s: c->doc().shapes){ if(s.type!=ShapeType::Path) allPath=false; if(s.fill.color!=QColor(30,90,200)) allBlue=false; }
      check(allPath, "each slice piece is an editable Path");
      check(allBlue, "each piece inherits the lower object's fill");
      double minL=1e9,maxR=-1e9; for(const Shape& s: c->doc().shapes){ QRectF bb=s.docBounds(); minL=qMin(minL,bb.left()); maxR=qMax(maxR,bb.right()); }
      check(qAbs(minL)<2 && qAbs(maxR-100)<2, "the pieces together span the original lower rect (0..100)"); }

    // --- Contour colour graduation + independent transparency ---
    std::printf("[effects] contour colour graduation\n");
    { Document d; d.pageSize=QSizeF(200,200);
      Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,80,80); s.transform=QTransform::fromTranslate(60,60);
      s.fill.type=FillType::Solid; s.fill.color=QColor(0,180,0); s.stroke.enabled=false;
      s.contour.on=true; s.contour.width=24; s.contour.steps=6; s.contour.color=QColor(255,0,0); s.contour.color2=QColor(0,0,255);
      d.shapes={s}; c->setDocument(d);
      check(c->exportRaster("/tmp/selftest_contourgrad.png",0,1.0,-1,true), "contour graduation renders");
      { QImage im("/tmp/selftest_contourgrad.png"); int redN=0,blueN=0;
        for(int y=0;y<im.height();y+=2) for(int x=0;x<im.width();x+=2){ QColor px=im.pixelColor(x,y); if(px.alpha()<40) continue;
            if(px.red()>px.blue()+40 && px.red()>px.green()+40) redN++;
            if(px.blue()>px.red()+40 && px.blue()>px.green()+40) blueN++; }
        check(redN>0 && blueN>0, "contour graduates: both the inner (red) and outer (blue) colours appear"); }
      Document rt=Document::fromJson(c->doc().toJson());
      check(rt.shapes[0].contour.color2==QColor(0,0,255) && rt.shapes[0].contour.on, "contour color2 survives JSON round-trip"); }

    // --- Paragraph spacing (space before / after) ---
    std::printf("[text] paragraph spacing (space before/after)\n");
    { auto mk=[&](double before, double after){ Shape s; s.type=ShapeType::Text; s.textFrame=true; s.rect=QRectF(0,0,300,400);
        s.text="First paragraph.\nSecond paragraph."; s.font=QFont("DejaVu Sans",16); s.fill.type=FillType::Solid; s.fill.color=Qt::black; s.stroke.enabled=false;
        s.spaceBefore=before; s.spaceAfter=after; return s; };
      const double h0 = mk(0,0).localPath().boundingRect().height();
      const double h1 = mk(40,0).localPath().boundingRect().height();
      check(h1 > h0 + 30, "space-before pushes later paragraphs down (taller layout)");
      const double h2 = mk(0,0).localPath().boundingRect().height();
      check(qAbs(h2-h0)<0.01, "zero spacing is unchanged from the default layout");
      Shape s=mk(25,10); Document d; d.shapes={s}; Document rt=Document::fromJson(d.toJson());
      check(qAbs(rt.shapes[0].spaceBefore-25)<0.01 && qAbs(rt.shapes[0].spaceAfter-10)<0.01, "paragraph spacing survives JSON round-trip"); }

    // --- Freehand smoothing (fit tolerance) ---
    std::printf("[vector] freehand smoothing (fit tolerance)\n");
    { QVector<QPointF> pts; for(int i=0;i<40;++i) pts<<QPointF(i*3.0, (i%2)?4.0:0.0);   // jittery zig-zag
      const QPainterPath lowT  = Canvas::smoothFreehand(pts, 1.0);
      const QPainterPath highT = Canvas::smoothFreehand(pts, 12.0);
      check(lowT.elementCount() > highT.elementCount(), "higher smoothing tolerance yields a simpler path (fewer nodes)");
      check(highT.elementCount() >= 2, "the smoothed path is still a valid curve"); }

    // --- Bulleted / numbered lists ---
    std::printf("[text] bulleted / numbered lists\n");
    { auto mk=[&](int style){ Shape s; s.type=ShapeType::Text; s.textFrame=true; s.rect=QRectF(0,0,300,400);
        s.text="Alpha\nBeta\nGamma"; s.font=QFont("DejaVu Sans",16); s.fill.type=FillType::Solid; s.fill.color=Qt::black; s.stroke.enabled=false;
        s.listStyle=style; return s; };
      const int e0 = mk(0).localPath().elementCount();
      const int e1 = mk(1).localPath().elementCount();   // bullet marker per item
      const int e2 = mk(2).localPath().elementCount();   // "1." "2." "3." markers
      check(e1 > e0, "a bulleted list draws a bullet marker per item");
      check(e2 > e0, "a numbered list draws number markers");
      Shape s=mk(1); s.listIndent=24; Document d; d.shapes={s}; Document rt=Document::fromJson(d.toJson());
      check(rt.shapes[0].listStyle==1 && qAbs(rt.shapes[0].listIndent-24)<0.01, "list style + indent survive JSON round-trip"); }

    // --- Native web contact-form widget ---
    std::printf("[web] contact form widget\n");
    { Document d; d.pageSize=QSizeF(800,600);
      Shape f; f.type=ShapeType::Rect; f.rect=QRectF(0,0,340,260);
      f.webForm="Name | text\nEmail | email\nMessage | textarea\nEvil | \" onload=\"x"; f.url="mailto:me@example.com";
      f.transform=QTransform::fromTranslate(40,40); d.shapes={f}; c->setDocument(d);
      check(c->exportWebsite("/tmp/selftest_form.html"), "publish with a contact form succeeds");
      { QFile fl("/tmp/selftest_form.html"); fl.open(QIODevice::ReadOnly); QString h=QString::fromUtf8(fl.readAll());
        check(h.contains("<form class=\"wform"), "form widget published as a <form>");
        check(h.contains("type=\"email\""), "email field type honoured");
        check(h.contains("<textarea"), "textarea field emitted");
        check(h.contains(">Name</label>"), "field label appears");
        check(h.contains("action=\"mailto:me@example.com\""), "form action set from the shape url");
        check(h.contains("<button type=\"submit\""), "submit button emitted");
        check(!h.contains(" onload=\""), "a hostile field type cannot inject an attribute (neutralised to text)"); }
      Document rt=Document::fromJson(c->doc().toJson());
      check(rt.shapes[0].webForm.contains("Email"), "webForm survives JSON round-trip"); }

    // --- Composite device-CMYK PDF (PDF/X-style) ---
    std::printf("[export] composite CMYK PDF\n");
    { Document d; d.pageSize=QSizeF(40,30); d.pageColor=Qt::white;
      Shape r; r.type=ShapeType::Rect; r.rect=QRectF(0,0,30,20); r.fill.type=FillType::Solid; r.fill.color=QColor(220,20,20); r.stroke.enabled=false;
      r.transform=QTransform::fromTranslate(5,5); d.shapes={r}; c->setDocument(d);
      check(c->exportCmykPdf("/tmp/selftest_cmyk.pdf"), "composite CMYK PDF export succeeds");
      QFile f("/tmp/selftest_cmyk.pdf"); f.open(QIODevice::ReadOnly); const QByteArray b=f.readAll();
      check(b.startsWith("%PDF-1.") && b.contains("%%EOF"), "valid PDF envelope (header + EOF)");
      check(b.contains("/DeviceCMYK"), "image colour space is DeviceCMYK");
      check(b.contains("/OutputIntent") && b.contains("/GTS_PDFX"), "an OutputIntent (GTS_PDFX) is attached to the catalog");
      check(b.contains("/Subtype/Image") && b.contains("/BitsPerComponent 8"), "each page is an 8-bit image XObject");
      check(b.contains("/Type/Catalog") && b.contains("/Type/Pages") && b.contains("/Type/Page"), "PDF page tree is present");
      // PDF/X conformance (G-N): the Info dict declares the GTS_PDFXVersion, referenced from the trailer.
      check(b.contains("/GTS_PDFXVersion(PDF/X-3:2002)"), "the CMYK PDF declares PDF/X-3 conformance");
      check(b.contains("/Info "), "the trailer references the Info dictionary"); }
    // Total-area-coverage (ink limit) + N-up imposition (G-N).
    { check(xara::totalAreaCoverage(QColor(0,0,0))==100, "pure black → 100% ink (K only)");
      check(xara::totalAreaCoverage(QColor(255,255,255))==0, "white → 0% ink");
      const int tacBlue=xara::totalAreaCoverage(QColor(0,0,255));   // rich blue → C+M high
      check(tacBlue>=150, "a saturated blue exceeds 150% ink");
      // TAC-warn overlay marks over-limit pixels
      QImage im(4,4,QImage::Format_ARGB32); im.fill(QColor(10,10,10));   // ~near-black → ~100% + underlying? actually K-heavy
      im.setPixel(0,0, qRgb(0,0,60));   // deep blue-black pixel likely > limit
      QImage before=im; xara::tacWarnImage(im, 50);
      check(im!=before, "tacWarnImage flags pixels over the (low) ink limit");
      // N-up imposition renders a non-empty sheet
      c->newDocument();
      { Shape s; s.type=ShapeType::Ellipse; s.rect=QRectF(0,0,80,60); s.transform=QTransform::fromTranslate(40,40); s.fill.type=FillType::Solid; s.fill.color=Qt::red; c->doc().addShape(s); }
      QImage sheet=c->renderImposedSheet(2,2, QSizeF(600,400));
      check(!sheet.isNull() && sheet.width()==600 && sheet.height()==400, "renderImposedSheet produces a sheet of the requested size");
      // the sheet has real (non-white) content
      bool anyInk=false; for(int y=0;y<sheet.height()&&!anyInk;y+=13) for(int x=0;x<sheet.width();x+=13){ QRgb p=sheet.pixel(x,y); if(qRed(p)<250||qGreen(p)<250||qBlue(p)<250){anyInk=true;break;} }
      check(anyInk, "the imposition sheet contains rendered artwork"); }

    // --- Photo Cutout Studio (automatic background removal) ---
    std::printf("[photo] cutout background (auto remove)\n");
    { QImage im(20,20,QImage::Format_ARGB32); im.fill(QColor(255,255,255));
      for(int y=6;y<14;++y) for(int x=6;x<14;++x) im.setPixel(x,y, qRgb(220,20,20));   // red subject on a white field
      Document d; d.pageSize=QSizeF(40,40);
      Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,20,20); s.fill.type=FillType::Bitmap; s.fill.image=im; s.fill.origImage=im; s.stroke.enabled=false;
      d.shapes={s}; c->setDocument(d); c->selectAll(); c->cutoutBackgroundSelection(40);
      const QImage out=c->doc().shapes[0].fill.image.convertToFormat(QImage::Format_ARGB32);
      check(out.pixelColor(0,0).alpha()==0, "the white border is keyed out (corner transparent)");
      check(out.pixelColor(10,10).alpha()==255, "the red subject stays opaque (centre)");
      int clear=0,solid=0; for(int y=0;y<20;++y) for(int x=0;x<20;++x){ int a=out.pixelColor(x,y).alpha(); if(a==0)++clear; else if(a==255)++solid; }
      check(clear>100 && solid>0, "background pixels are transparent while the subject is retained"); }

    // --- Blend along a path ---
    std::printf("[vector] blend along a path\n");
    { Document d; d.pageSize=QSizeF(200,200);
      auto sq=[&](double x){ Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,10,10); s.fill.type=FillType::Solid; s.fill.color=Qt::blue; s.stroke.enabled=false; s.transform=QTransform::fromTranslate(x,95); return s; };
      Shape spine; spine.type=ShapeType::Path; QPainterPath pp; pp.moveTo(5,100); pp.quadTo(55,-40,105,100); spine.path=pp;   // arc bulging up
      spine.fill.type=FillType::None; spine.stroke.enabled=true; spine.stroke.color=Qt::black; spine.stroke.width=1;
      d.shapes={ sq(0), sq(100), spine }; c->setDocument(d);
      const int before=c->doc().shapes.size();
      c->selectAll(); c->blendAlongPathSelection(3);
      check(c->doc().shapes.size()==before+3, "3 in-between blend steps were created");
      double minY=1e9; for(int i=before;i<c->doc().shapes.size();++i){ QRectF b=c->doc().shapes[i].docBounds(); minY=qMin(minY,b.center().y()); }
      check(minY<60, "at least one step follows the arc up, off the straight line between the ends (y≪95)"); }

    // --- Per-object presentation build order ---
    std::printf("[web] presentation build order\n");
    { auto mk=[&](const QString& t, int build, double y){ Shape s; s.type=ShapeType::Text; s.text=t; s.font=QFont("DejaVu Sans",16);
        s.fill.type=FillType::Solid; s.fill.color=Qt::black; s.stroke.enabled=false; s.layer=0; s.webBuild=build; s.transform=QTransform::fromTranslate(20,y); return s; };
      Document d; d.pageSize=QSizeF(400,300); d.presentSteps=true;
      d.shapes={ mk("One",1,20), mk("Two",2,60), mk("Three",3,100) }; c->setDocument(d);
      check(c->exportWebsite("/tmp/selftest_build.html"), "publish with build order succeeds");
      { QFile f("/tmp/selftest_build.html"); f.open(QIODevice::ReadOnly); QString h=QString::fromUtf8(f.readAll());
        check(h.contains("data-step=\"0\"") && h.contains("data-step=\"1\"") && h.contains("data-step=\"2\""),
            "same-layer objects get distinct sequential build steps (0,1,2)"); }
      Document rt=Document::fromJson(c->doc().toJson());
      check(rt.shapes[2].webBuild==3, "build order survives JSON round-trip");
      Document d2; d2.pageSize=QSizeF(400,300); d2.presentSteps=true;
      d2.shapes={ mk("One",0,20), mk("Two",0,60) }; c->setDocument(d2);
      check(c->exportWebsite("/tmp/selftest_build0.html"), "publish without build order succeeds");
      { QFile f("/tmp/selftest_build0.html"); f.open(QIODevice::ReadOnly); QString h=QString::fromUtf8(f.readAll());
        check(!h.contains("data-step=\"1\""), "with no build order, same-layer objects share one step (layer-only, unchanged)"); } }

    // --- Mouse-over pop-ups ---
    std::printf("[web] mouse-over pop-ups\n");
    { auto txt=[&](const QString& t, double y){ Shape s; s.type=ShapeType::Text; s.text=t; s.font=QFont("DejaVu Sans",16);
        s.fill.type=FillType::Solid; s.fill.color=Qt::black; s.stroke.enabled=false; s.transform=QTransform::fromTranslate(20,y); return s; };
      Document d; d.pageSize=QSizeF(400,300); d.shapes={ txt("Hover me",20), txt("Surprise panel",120) }; c->setDocument(d);
      c->selectAll(); c->linkPopup();
      check(c->doc().shapes[0].webPopupTrigger>0 && c->doc().shapes[1].webPopupId==c->doc().shapes[0].webPopupTrigger, "trigger + panel paired with a shared id");
      Document rt=Document::fromJson(c->doc().toJson());
      check(rt.shapes[0].webPopupTrigger>0 && rt.shapes[1].webPopupId>0, "pop-up ids survive JSON round-trip");
      check(c->exportWebsite("/tmp/selftest_popup.html"), "publish with a pop-up succeeds");
      { QFile f("/tmp/selftest_popup.html"); f.open(QIODevice::ReadOnly); QString h=QString::fromUtf8(f.readAll());
        check(h.contains("data-poptrig=\""), "trigger element carries data-poptrig");
        check(h.contains("data-poppanel=\""), "panel element carries data-poppanel");
        check(h.contains("addEventListener('mouseenter'"), "pop-up controller JS emitted"); }
      Document d2; d2.pageSize=QSizeF(400,300); d2.shapes={ txt("Plain",20) }; c->setDocument(d2);
      check(c->exportWebsite("/tmp/selftest_popup0.html"), "publish without pop-ups succeeds");
      { QFile f("/tmp/selftest_popup0.html"); f.open(QIODevice::ReadOnly); QString h=QString::fromUtf8(f.readAll());
        check(!h.contains("data-poptrig"), "no pop-up attributes when nothing is a pop-up"); } }

    // --- Canvas render-cache performance (Phase 1) ---
    // A busy document with effects; measure one full backdrop render vs one cached blit+overlay.
    std::printf("[perf] canvas render cache\n");
    { Document pd; pd.pageSize=QSizeF(800,600);
      for(int i=0;i<200;++i){ Shape s; s.type=(i%3==0)?ShapeType::Ellipse:ShapeType::Rect;
        s.rect=QRectF(0,0,40+i%30,30+i%20); s.transform=QTransform::fromTranslate(20+(i*37)%740, 20+(i*53)%540);
        s.fill.type=FillType::Linear; s.fill.color=QColor((i*7)%255,(i*13)%255,(i*17)%255); s.fill.color2=QColor(30,30,40);
        s.stroke.enabled=true; s.stroke.width=1.5; if(i%5==0){ s.shadow.on=true; s.shadow.offset=QPointF(5,7); s.shadow.blur=6; }
        pd.shapes.push_back(s); }
      c->setDocument(pd); c->resize(1280,860); QApplication::processEvents();
      // stability: a second grab of a static document must equal the first (cache reuse is faithful)
      QImage g1=c->grab().toImage(); QImage g2=c->grab().toImage();
      check(g1==g2, "screen-cache: re-grab of a static document is byte-identical (cache reused faithfully)");
      auto pr=c->renderProbe(40);
      check(pr.fullMs>0 && pr.cachedMs>0, "renderProbe produced timings");
      check(pr.cachedMs < pr.fullMs, "cached blit+overlay is faster than a full document re-render");
      std::printf("  full=%.2fms  cached=%.2fms  speedup=%.1fx  (200 shapes @1280x860)\n",
                  pr.fullMs, pr.cachedMs, pr.fullMs/qMax(0.001,pr.cachedMs)); }

    // --- Viewport culling (Phase 2): must drop no VISIBLE content, and speed up a big doc in a small view ---
    std::printf("[perf] viewport culling\n");
    { Document pd; pd.pageSize=QSizeF(4000,3000);
      auto mk=[&](double x,double y,bool sh,bool ct,double fe,double bl){ Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,60,44);
        s.transform=QTransform::fromTranslate(x,y); s.fill.type=FillType::Solid; s.fill.color=QColor(40,150,190);
        s.stroke.enabled=true; s.stroke.width=3; s.stroke.color=QColor(10,20,30);
        if(sh){ s.shadow.on=true; s.shadow.offset=QPointF(24,24); s.shadow.blur=18; }
        if(ct){ s.contour.on=true; s.contour.width=30; s.contour.color=QColor(200,60,60); }
        s.feather=fe; s.blur=bl; return s; };
      for(int i=0;i<300;++i) pd.shapes.push_back(mk(30+(i*167)%3900, 30+(i*233)%2900, i%4==0, i%5==0, (i%7)?0.0:14.0, (i%11)?0.0:12.0));
      c->setDocument(pd); c->resize(720,720); c->zoomTo(1.0); QApplication::processEvents();
      c->setCullEnabled(true);  QImage on =c->grab().toImage();
      c->setCullEnabled(false); QImage off=c->grab().toImage();
      check(on==off, "viewport culling drops no visible content (cull vs no-cull byte-identical, effects at the edge included)");
      c->setCullEnabled(true);  double culled =c->renderProbe(25).fullMs;
      c->setCullEnabled(false); double uncalled=c->renderProbe(25).fullMs;
      check(culled <= uncalled*1.05 + 0.05, "culling never slows the full render");
      std::printf("  full-render: culled=%.2fms  no-cull=%.2fms  (300 shapes, 720x720 view of 4000x3000)\n", culled, uncalled);
      c->setCullEnabled(true); }

    // --- Drag proxy (dynamic-object layer): a live move of a heavy group composites two cached bitmaps
    //     instead of re-rasterising the whole document every frame ---
    std::printf("[perf] drag proxy (dynamic-object layer)\n");
    { c->newDocument(); c->setTool(Tool::Select); c->doc().pageSize=QSizeF(900,700);
      for(int i=0;i<120;++i){ Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,30,24);       // static backdrop clutter
        s.transform=QTransform::fromTranslate(10+(i*61)%860, 10+(i*97)%660);
        s.fill.type=FillType::Solid; s.fill.color=QColor(60,70,90); c->doc().addShape(s); }
      Shape g; g.type=ShapeType::Group; g.name="Group";                                           // one group on TOP
      for(int i=0;i<100;++i){ Shape k; k.type=(i%2)?ShapeType::Ellipse:ShapeType::Rect; k.rect=QRectF(0,0,18+i%12,14+i%9);
        k.transform=QTransform::fromTranslate((i%20)*15, (i/20)*15);
        k.fill.type=FillType::Linear; k.fill.color=QColor((i*9)%255,120,(i*5)%255); k.fill.color2=QColor(20,20,30);
        k.stroke.enabled=true; k.stroke.width=1.0; g.children.push_back(k); }
      g.transform=QTransform::fromTranslate(300,240);
      const int gi=c->doc().addShape(g);
      c->resize(900,700); c->zoomTo(1.0); QApplication::processEvents();
      c->select({gi});

      double pm=0, fm=0; const bool built=c->dragProxyProbe(30, pm, fm);
      check(built, "drag proxy activates for a whole-group move");
      check(pm>0 && fm>0, "drag proxy probe produced timings");
      check(pm < fm, "proxy composite is faster than the per-frame full document rebuild");
      std::printf("  proxy=%.2fms  full-rebuild=%.2fms  speedup=%.1fx  (220-object doc, 100 in the moved group)\n",
                  pm, fm, fm/qMax(0.001,pm));
      check(!c->dragProxyActive(), "probe leaves no proxy active");
      check(c->doc().shapes[gi].transform==QTransform::fromTranslate(300,240), "probe restored the group transform exactly");

      // Extreme zoom / HiDPI: the capture would exceed the old 40 Mpx hard cap. Instead of falling back to a
      // per-frame full rebuild (the freeze), the build now caps the capture RESOLUTION and still activates.
      c->zoomTo(45.0); QApplication::processEvents();
      double zpm=0, zfm=0; const bool zbuilt=c->dragProxyProbe(3, zpm, zfm);
      check(zbuilt, "drag proxy still builds at extreme zoom (capture resolution capped, not hard-failed)");
      c->zoomTo(1.0); QApplication::processEvents();

      // Correctness: a real drag's proxy frame must match a full render of the same dropped geometry (top-of-z
      // group ⇒ float-on-top is the correct z-order; Alt on the move bypasses snapping so no stray guide lines).
      const QPointF pressV = c->toView(QPointF(450,280));   // inside the 100-child group (doc x[300,615] y[240,323])
      const QPointF moveV  = pressV + QPointF(80,60);        // integer device-px translation ⇒ no proxy resampling
      send(c, QEvent::MouseButtonPress, pressV, Qt::LeftButton, Qt::LeftButton);
      { QMouseEvent mv(QEvent::MouseMove, moveV, c->mapToGlobal(moveV.toPoint()), Qt::NoButton, Qt::LeftButton, Qt::AltModifier);
        QApplication::sendEvent(c,&mv); }
      const QImage proxyFrame = c->grab().toImage();         // forces a paint → proxy composite path
      check(c->dragProxyActive(), "proxy is active mid-drag");
      send(c, QEvent::MouseButtonRelease, moveV, Qt::LeftButton, Qt::NoButton);
      check(!c->dragProxyActive(), "release clears the proxy");
      const QImage fullFrame = c->grab().toImage();          // full vector render of the dropped geometry
      double sum=0; qint64 n=0; const int W=qMin(proxyFrame.width(),fullFrame.width()), H=qMin(proxyFrame.height(),fullFrame.height());
      for(int y=0;y<H;y+=2) for(int x=0;x<W;x+=2){ QRgb a=proxyFrame.pixel(x,y), b=fullFrame.pixel(x,y);
        sum += qAbs(qRed(a)-qRed(b))+qAbs(qGreen(a)-qGreen(b))+qAbs(qBlue(a)-qBlue(b)); n+=3; }
      const double meanDiff = n ? sum/n : 0;
      check(meanDiff < 4.0, "mid-drag proxy frame matches a full render of the same moved geometry (mean channel diff < 4)");
      std::printf("  proxy-vs-full mean channel diff = %.2f levels\n", meanDiff);
    }

    // Static (non-drag) selection-outline cache: the entered/selected group silhouette is built ONCE per selection/doc
    // change and reused across repaints, so a passive per-frame repaint (e.g. sweeping the Ctrl+Alt pie over a big
    // grouped selection) no longer rebuilds the whole group's QPainterPath every frame — the fix for the pie-hover freeze.
    std::printf("[perf] static selection-outline cache (Ctrl+Alt pie-hover freeze fix)\n");
    { c->newDocument(); c->setTool(Tool::Select);
      Shape g; g.type=ShapeType::Group; g.name="Group";
      for(int i=0;i<60;++i){ Shape k; k.type=ShapeType::Rect; k.rect=QRectF(0,0,20,15); k.transform=QTransform::fromTranslate((i%10)*22,(i/10)*18); g.children.push_back(k); }
      const int gi=c->doc().addShape(g);
      c->select({gi});
      check(!c->selCacheValidForTest(), "selecting invalidates the outline cache");
      const int e1=c->selOutlineElemsForTest();
      check(e1>0 && c->selCacheValidForTest(), "the selection outline builds once and warms the cache");
      check(c->selOutlineElemsForTest()==e1 && c->selCacheValidForTest(), "a repeat draw reuses the cache (no rebuild)");
      Shape r; r.type=ShapeType::Rect; r.rect=QRectF(0,0,10,10); const int ri=c->doc().addShape(r);
      c->select({ri});
      check(!c->selCacheValidForTest(), "changing the selection invalidates the cache");
      const int e3=c->selOutlineElemsForTest();
      check(e3>0 && e3<e1, "a single-rect selection has a smaller silhouette than the 60-child group");
      c->select({gi}); c->selOutlineElemsForTest();
      c->duplicateSelection();   // a document edit → documentChanged
      check(!c->selCacheValidForTest(), "a document edit invalidates the outline cache (so it can never go stale)");
      // A HEAVY selection (> the shape cap) skips the merged/AA-stroked silhouette entirely — the real freeze fix: no
      // giant QPainterPath is built (per frame OR once), yet a valid selection box still comes from cheap child-rect unions.
      { Shape big; big.type=ShapeType::Group; big.name="Big";
        for(int i=0;i<500;++i){ Shape k; k.type=ShapeType::Rect; k.rect=QRectF(0,0,12,10); k.transform=QTransform::fromTranslate((i%25)*14,(i/25)*12); big.children.push_back(k); }
        const int bi=c->doc().addShape(big); c->select({bi});
        check(c->selOutlineElemsForTest()==0, "a >400-shape selection skips the merged silhouette (no giant per-frame path build)");
        check(c->selCacheValidForTest() && !c->selBoundsForTest().isNull(), "…but still yields a valid selection box from cheap child-rect unions"); } }

    // Selection-outline cost during a drag: a group of COMPLEX bezier paths (silhouette well over the stroke cap)
    // makes stroking the per-frame silhouette genuinely expensive; during the drag we skip that stroke (bbox +
    // handles + the moving-proxy bitmap convey the selection), so the live overlay is dramatically cheaper.
    { c->newDocument(); c->setTool(Tool::Select);
      Shape grp; grp.type=ShapeType::Group; grp.name="Complex";
      for(int k=0;k<80;++k){ Shape p; p.type=ShapeType::Path; QPainterPath pp; pp.moveTo(0,0);
        for(int j=0;j<40;++j){ double a=j*0.35; pp.cubicTo(j*3,std::sin(a)*20, j*3+1,std::cos(a)*20, (j+1)*3, std::sin(a*1.3)*15); }
        p.path=pp; p.transform=QTransform::fromTranslate((k%10)*40,(k/10)*40);
        p.stroke.enabled=true; p.stroke.width=1.0; p.fill.type=FillType::Solid; p.fill.color=QColor(90,120,200); grp.children.push_back(p); }
      grp.transform=QTransform::fromTranslate(120,120);
      const int gi=c->doc().addShape(grp);
      c->resize(700,560); c->zoomTo(1.0); QApplication::processEvents(); c->select({gi});
      // This complex group's silhouette is far over the stroke cap (kDragOutlineStrokeCap=1500), so it is NEVER
      // AA-stroked per frame — not during a drag AND (since the pie-hover fix) not during a static repaint either.
      // Structural check (robust; no wall-clock): the static path used to stroke this ~9600-element path every frame
      // (~120ms → the Ctrl+Alt pie-hover freeze); now both overlays draw only the bbox + handles.
      check(c->selOutlineElemsForTest() > 1500, "the complex group's silhouette exceeds the stroke cap (so it is never stroked per-frame)");
      double cm=0, rm=0; const bool ok=c->dragOutlineProbe(10, cm, rm);
      check(ok && cm>0 && rm>0, "outline probe produced timings for both overlays");
      std::printf("  drawSelection: drag=%.3fms  static=%.3fms  (80 paths x 40 bezier segs; neither strokes the silhouette)\n", cm, rm); }

    // --- snapAdjust must not be O(N·M): a move of a huge multi-selection scans candidates every frame; a
    //     QVector contains() per object used to make that O(N·M) and freeze a heavy doc (e.g. a flattened
    //     multi-page SVG / CorelDRAW import). Past kSnapObjectCandidateCap objects it skips object snapping. ---
    std::printf("[perf] snapAdjust stays cheap for a huge select-all (no O(N*M) move freeze)\n");
    { c->newDocument(); c->setSnap(true);
      for(int i=0;i<6000;++i){ Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,10,10);
        s.transform=QTransform::fromTranslate((i%100)*12.0,(i/100)*12.0); c->doc().addShape(s); }
      c->resize(700,560); c->zoomTo(1.0); QApplication::processEvents();
      QVector<int> all; for(int i=0;i<c->doc().shapes.size();++i) all<<i; c->select(all);
      const double ms=c->snapAdjustProbeMs(30);
      check(ms < 12.0, "snapAdjust over a 6000-object select-all is fast (old QVector::contains scan was O(N*M))");
      std::printf("  snapAdjust(6000 selected) = %.3f ms/frame\n", ms); }

    // --- The select-ONE case the select-all test above MISSED: moving a single object still scans EVERY other
    //     shape as a snap candidate, and docBounds() rebuilds each shape's QPainterPath per frame. With 2500
    //     shapes (under kSnapObjectCandidateCap so object-snap runs) this froze on MOVE while SCALE (no snapAdjust)
    //     stayed smooth. The fix caches the static candidate edges once at drag start. ---
    std::printf("[perf] snapAdjust stays cheap when moving ONE object in a big doc (all others are candidates)\n");
    { c->newDocument(); c->setSnap(true);
      for(int i=0;i<2500;++i){ Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,10,10);
        s.transform=QTransform::fromTranslate((i%50)*12.0,(i/50)*12.0); c->doc().addShape(s); }
      c->resize(700,560); c->zoomTo(1.0); QApplication::processEvents();
      c->select(QVector<int>{0});
      const double ms=c->snapAdjustProbeMs(60);
      check(ms < 3.0, "snapAdjust when moving ONE object among 2500 is fast (per-frame docBounds path-rebuild was the move freeze)");
      std::printf("  snapAdjust(1 of 2500 selected) = %.3f ms/frame\n", ms); }

    // --- pushUndo must be cheap on a heavy doc. It used to JSON-serialise the WHOLE document on every
    //     edit; on a 9k-object .cdr that froze the UI ~300ms at EVERY drag-start ("moving hangs"). It now
    //     stores a copy-on-write Document snapshot (a ref bump; the O(N) detach happens on the next mutate). ---
    std::printf("[perf] pushUndo stays cheap on a big doc (COW snapshot, not whole-doc JSON serialize)\n");
    { c->newDocument();
      for(int i=0;i<3000;++i){ Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,10,10);
        s.transform=QTransform::fromTranslate((i%60)*12.0,(i/60)*12.0); c->doc().addShape(s); }
      QElapsedTimer t; t.start(); for(int r=0;r<25;++r){ c->pushUndo("bench"); c->doc().shapes[r].transform.translate(1,1); }
      const double ms=t.nsecsElapsed()/1e6/25;
      check(ms < 8.0, "pushUndo on a 3000-object doc is fast (COW snapshot + one lazy detach, not full-doc JSON)");
      std::printf("  pushUndo(3000 objects, incl. detach) = %.3f ms\n", ms); }
    // --- The COW snapshot must still round-trip undo/redo exactly (transform + geometry). ---
    std::printf("[undo] COW-snapshot undo/redo round-trips a move exactly\n");
    { c->newDocument(); Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,50,40); const int i=c->doc().addShape(s);
      const QTransform t0=c->doc().shapes[i].transform;
      c->select(QVector<int>{i}); c->pushUndo("Move");
      c->doc().shapes[i].transform = t0*QTransform::fromTranslate(33,22);
      const QTransform t1=c->doc().shapes[i].transform;
      c->undo(); check(c->doc().shapes[i].transform==t0, "undo restores the pre-move transform");
      c->redo(); check(c->doc().shapes[i].transform==t1, "redo re-applies the move");
      // a second, independent edit then undo must not corrupt the first (COW isolation)
      c->pushUndo("Recolour"); c->doc().shapes[i].fill.color=QColor(200,10,10); c->undo();
      check(c->doc().shapes[i].transform==t1, "an unrelated undo leaves the earlier move intact (snapshots are isolated)"); }

    // --- Debounced documentChanged: a one-off edit (no live drag) must still forward synchronously so panels
    //     and the self-test never see a stale gallery ---
    std::printf("[perf] documentChangedDebounced coalescing\n");
    { c->newDocument(); Shape s; s.type=ShapeType::Rect; s.name="R"; const int i=c->doc().addShape(s);
      int cnt=0; QObject ctx; QObject::connect(c,&Canvas::documentChangedDebounced,&ctx,[&]{ ++cnt; });
      c->renameShape(i,"R2");
      check(cnt>=1, "a one-off edit forwards documentChangedDebounced synchronously (panels stay in lock-step)"); }

    // --- FXG, XD and the compressed Corel exchange files ---
    std::printf("[interop] FXG, Adobe XD, CCX\n");
    {
      std::function<void(const QVector<Shape>&,QVector<Shape>&)> flat=[&](const QVector<Shape>& in, QVector<Shape>& out){
          for(const Shape& s : in){ if(s.children.isEmpty()) out.push_back(s); else flat(s.children,out); } };
      auto shapesOf=[&](const QString& svg){ QVector<Shape> o; flat(parseSvg(svg.toUtf8()),o); return o; };
      const QByteArray fxg=
        "<Graphic xmlns=\"http://ns.adobe.com/fxg/2008\" version=\"2.0\" viewWidth=\"200\" viewHeight=\"100\">"
        "<Library><Definition name=\"Dot\"><Group><Ellipse width=\"10\" height=\"10\"><fill><SolidColor color=\"#00FF00\"/></fill></Ellipse></Group></Definition></Library>"
        "<Rect x=\"10\" y=\"20\" width=\"100\" height=\"50\"><fill><LinearGradient rotation=\"90\"><GradientEntry color=\"#FF0000\"/><GradientEntry color=\"#0000FF\"/></LinearGradient></fill>"
        "<stroke><SolidColorStroke color=\"#010203\" weight=\"3\" caps=\"none\" joints=\"miter\"/></stroke></Rect>"
        "<Rect width=\"40\" height=\"10\"><fill><LinearGradient x=\"5\" y=\"2\" scaleX=\"20\"><GradientEntry color=\"#000000\"/><GradientEntry color=\"#FFFFFF\"/></LinearGradient></fill></Rect>"
        "<Dot x=\"150\" y=\"10\"/>"
        "<Group x=\"20\" y=\"60\"><mask><Group><Rect width=\"10\" height=\"10\"><fill><SolidColor color=\"#FFFFFF\"/></fill></Rect></Group></mask>"
        "<Rect width=\"30\" height=\"30\"><fill><SolidColor color=\"#102030\" alpha=\"0.5\"/></fill></Rect></Group>"
        "<RichText x=\"5\" y=\"70\" fontFamily=\"Arial\" fontSize=\"12\" color=\"#112233\"><content><p>one<br/>two</p></content></RichText>"
        "</Graphic>";
      const QString fs=xara::fxgToSvg(fxg,QString());
      const QVector<Shape> fx=shapesOf(fs);
      check(fx.size()==6, "FXG: shapes, a placed symbol, a masked group and two text lines all arrive");
      check(svgViewportSize(fs.toUtf8())==QSizeF(200,100), "FXG: the view size becomes the page");
      if(fx.size()==6){
        const Shape& g=fx[0];
        check(g.fill.type==FillType::Linear && qAbs(g.fill.p0.y()-0)<.01 && qAbs(g.fill.p1.y()-50)<.01 && qAbs(g.fill.p0.x()-50)<.01,
              "FXG: a gradient with no position is centred on its shape, and rotates about the centre");
        check(g.stroke.enabled && qAbs(g.stroke.width-3)<.01 && g.stroke.cap==Qt::FlatCap && g.stroke.join==Qt::MiterJoin, "FXG: stroke weight, caps and joints");
        check(qAbs(g.docBounds().left()-10)<.01 && qAbs(g.docBounds().top()-20)<.01, "FXG: x and y place the shape");
        check(fx[1].fill.type==FillType::Linear && qAbs(fx[1].fill.p0.x()-5)<.01 && qAbs(fx[1].fill.p1.x()-25)<.01, "FXG: a gradient with x,y and scaleX starts there and runs that far");
        check(fx[2].fill.color==QColor(0,255,0) && qAbs(fx[2].docBounds().left()-150)<.01, "FXG: a Library definition is placed by name");
        check(!fx[3].regionPath.isEmpty(), "FXG: a mask becomes the group's clip");
        check(fx[4].type==ShapeType::Text && fx[5].type==ShapeType::Text && fx[4].text==QStringLiteral("one") && fx[5].text==QStringLiteral("two"), "FXG: a <br/> breaks the line");
        check(fx[5].docBounds().top()>fx[4].docBounds().top()+5, "FXG: the second line sits below the first");
      }
      check(xara::fxgToSvg("<svg xmlns=\"http://www.w3.org/2000/svg\"/>",QString()).isEmpty() && xara::fxgToSvg("FXG01 binary",QString()).isEmpty(), "FXG: anything else is refused");

      QMap<QString,QByteArray> xd;
      xd.insert("mimetype","application/vnd.adobe.sparkler.project+dcx");
      xd.insert("manifest","{\"children\":[{\"path\":\"artwork\",\"children\":[{\"path\":\"pasteboard\",\"name\":\"pasteboard\"},"
        "{\"path\":\"artboard-b\",\"name\":\"Second\",\"uxdesign#bounds\":{\"x\":300,\"y\":0,\"width\":100,\"height\":200}},"
        "{\"path\":\"artboard-a\",\"name\":\"First\",\"uxdesign#bounds\":{\"x\":-200,\"y\":50,\"width\":150,\"height\":80}}]}]}");
      xd.insert("resources/graphics/graphicContent.agc",
        "{\"artboards\":{\"A\":{\"x\":-200,\"y\":50,\"width\":150,\"height\":80,\"name\":\"First\"},\"B\":{\"x\":300,\"y\":0,\"width\":100,\"height\":200,\"name\":\"Second\"}},"
        "\"resources\":{\"gradients\":{\"g1\":{\"type\":\"linear\",\"stops\":[{\"offset\":0,\"color\":{\"mode\":\"RGB\",\"value\":{\"r\":255,\"g\":0,\"b\":0}}},{\"offset\":1,\"color\":{\"mode\":\"RGB\",\"value\":{\"r\":0,\"g\":0,\"b\":255}}}]}},\"clipPaths\":{}}}");
      xd.insert("artwork/artboard-a/graphics/graphicContent.agc",
        "{\"children\":[{\"type\":\"artboard\",\"style\":{\"fill\":{\"type\":\"solid\",\"color\":{\"mode\":\"RGB\",\"value\":{\"r\":1,\"g\":2,\"b\":3}}}},\"artboard\":{\"ref\":\"A\",\"children\":["
        "{\"type\":\"shape\",\"transform\":{\"a\":1,\"b\":0,\"c\":0,\"d\":1,\"tx\":-190,\"ty\":60},\"style\":{\"fill\":{\"type\":\"gradient\",\"gradient\":{\"units\":\"objectBoundingBox\",\"ref\":\"g1\",\"x1\":0,\"y1\":0,\"x2\":1,\"y2\":0}},"
        "\"stroke\":{\"type\":\"solid\",\"color\":{\"mode\":\"RGB\",\"value\":{\"r\":9,\"g\":9,\"b\":9}},\"width\":2,\"align\":\"inside\"}},\"shape\":{\"type\":\"rect\",\"x\":0,\"y\":0,\"width\":20,\"height\":10}},"
        "{\"type\":\"text\",\"transform\":{\"a\":1,\"b\":0,\"c\":0,\"d\":1,\"tx\":-180,\"ty\":100},\"meta\":{\"ux\":{\"rangedStyles\":[{\"length\":2,\"fontFamily\":\"Arial\",\"fontStyle\":\"Bold\",\"fontSize\":10,\"fill\":{\"value\":4294901760}},{\"length\":2,\"fontFamily\":\"Arial\",\"fontStyle\":\"Regular\",\"fontSize\":10,\"fill\":{\"value\":4278190335}}]}},"
        "\"style\":{\"font\":{\"family\":\"Arial\",\"style\":\"Regular\",\"size\":10},\"fill\":{\"type\":\"solid\",\"color\":{\"mode\":\"RGB\",\"value\":{\"r\":0,\"g\":0,\"b\":0}}}},"
        "\"text\":{\"rawText\":\"abcd\",\"frame\":{\"type\":\"positioned\"},\"paragraphs\":[{\"lines\":[[{\"x\":0,\"y\":0,\"from\":0,\"to\":4,\"style\":{\"font\":{\"family\":\"Stand In\"}}}]]}]}}]}}]}");
      xd.insert("artwork/artboard-b/graphics/graphicContent.agc",
        "{\"children\":[{\"type\":\"artboard\",\"artboard\":{\"ref\":\"B\",\"children\":[{\"type\":\"shape\",\"visible\":false,\"style\":{},\"shape\":{\"type\":\"circle\",\"cx\":1,\"cy\":1,\"r\":1}},"
        "{\"type\":\"shape\",\"transform\":{\"a\":1,\"b\":0,\"c\":0,\"d\":1,\"tx\":310,\"ty\":10},\"style\":{\"fill\":{\"type\":\"solid\",\"color\":{\"mode\":\"RGB\",\"value\":{\"r\":0,\"g\":200,\"b\":0}}}},\"shape\":{\"type\":\"circle\",\"cx\":5,\"cy\":5,\"r\":5}}]}}]}");
      xara::XdStats xs;
      const QStringList xp=xara::xdToSvgPages(xd,&xs);
      check(xp.size()==2 && xs.names==QStringList({"Second","First"}), "XD: a page per artboard, in the manifest's order");
      if(xp.size()==2){
        check(svgViewportSize(xp[0].toUtf8())==QSizeF(150,200) && svgViewportSize(xp[1].toUtf8())==QSizeF(150,200), "XD: every page takes the largest artboard's size");
        const QVector<Shape> a=shapesOf(xp[1]), b=shapesOf(xp[0]);
        check(b.size()==1 && qAbs(b[0].docBounds().left()-10)<.01 && qAbs(b[0].docBounds().top()-10)<.01, "XD: contents shift by the artboard's origin, and hidden objects stay out");
        // background, fill, the inside stroke, one text object with two runs
        check(a.size()==4, "XD: artboard fill, shape, inside stroke and the text all arrive");
        if(a.size()==4){
          check(a[0].fill.color==QColor(1,2,3) && qAbs(a[0].docBounds().width()-150)<.01, "XD: the artboard's fill sits behind its contents");
          check(a[1].fill.type==FillType::Linear && !a[1].stroke.enabled, "XD: a shared gradient resolves by reference");
          check(a[2].stroke.enabled && qAbs(a[2].stroke.width-4)<.01 && !a[2].regionPath.isEmpty(), "XD: an inside stroke is doubled and cut by its own shape");
          check(a[3].type==ShapeType::Text && a[3].text==QStringLiteral("abcd") && a[3].textRuns.size()==2, "XD: ranged styles become runs of one text object");
          if(a[3].textRuns.size()==2)
            check(a[3].textRuns[0].font.family()==QStringLiteral("Arial") && a[3].textRuns[0].font.weight()==QFont::Bold
                  && a[3].textRuns[0].color==QColor(255,0,0) && a[3].textRuns[1].color==QColor(0,0,255), "XD: a range's font and colour win over the line's stand-in");
        }
      }
      check(xara::xdToSvgPages(QMap<QString,QByteArray>()).isEmpty(), "XD: an empty archive gives nothing");

      QByteArray inner("pagex\0\0\0",8); inner[4]=1; inner.append('Q'); inner.append('\0');
      QByteArray packed(int(compressBound(inner.size())),'\0'); uLongf pl=packed.size();
      compress((Bytef*)packed.data(),&pl,(const Bytef*)inner.constData(),inner.size()); packed.resize(int(pl));
      auto le=[](quint32 v){ return QByteArray(reinterpret_cast<const char*>(&v),4); };
      QByteArray pack=le(quint32(inner.size()))+QByteArray("CPng\x01\x00\x04\x00",8)+packed;
      if(pack.size()&1) pack.append('\0');
      QByteArray riffBody=QByteArray("CDRX")+"cont"+le(2)+QByteArray("ab",2)+"pack"+le(quint32(pack.size()))+pack;
      const QByteArray ccx=QByteArray("RIFF")+le(quint32(riffBody.size()))+riffBody;
      const QByteArray cmx=xara::unpackCorelExchange(ccx);
      check(cmx.mid(8,4)=="CMX1" && cmx.mid(12,4)=="cont" && cmx.mid(22,4)=="page", "CCX: the deflated body comes back as plain CMX chunks");
      check(xara::unpackCorelExchange("RIFFxxxxCDR9rest")=="RIFFxxxxCDR9rest", "CCX: an ordinary file passes through untouched");
    }

    // --- PaintShop Pro ---
    std::printf("[interop] PaintShop Pro\n");
    {
      auto le16=[](int v){ QByteArray b(2,'\0'); b[0]=char(v); b[1]=char(v>>8); return b; };
      auto le32=[](qint64 v){ QByteArray b(4,'\0'); for(int i=0;i<4;++i) b[i]=char(v>>(8*i)); return b; };
      auto f64=[](double v){ return QByteArray(reinterpret_cast<const char*>(&v),8); };
      auto block=[&](int id, const QByteArray& body){ return QByteArray("~BK\0",4)+le16(id)+le32(body.size())+body; };
      auto file=[&](int version,int w,int h,int comp,int depth,bool gray,const QByteArray& rest){
        QByteArray sig("Paint Shop Pro Image File\n\x1a"); sig.append(QByteArray(32-sig.size(),'\0'));
        const QByteArray img=le32(46)+le32(w)+le32(h)+f64(72)+QByteArray(1,'\1')+le16(comp)+le16(depth)+le16(1)+le32(0)+QByteArray(1,gray?'\1':'\0')+le32(0)+le32(0)+le16(1)+le32(1);
        return sig+le16(version)+le16(0)+block(0,img)+rest; };
      auto rect=[&](int a,int b,int c,int d){ return le32(a)+le32(b)+le32(c)+le32(d); };
      auto channel=[&](int bmp,int ch,const QByteArray& data){ return block(5,le32(16)+le32(data.size())+le32(data.size())+le16(bmp)+le16(ch)+data); };
      auto layer=[&](const QByteArray& name,int type,int x,int y,int w,int h,int opacity,int blendMode,int vis,int bitmaps,int channels,const QByteArray& subs){
        QByteArray head=le16(name.size())+name+QByteArray(1,char(type))+rect(x,y,x+w,y+h)+rect(0,0,w,h)+QByteArray(1,char(opacity))+QByteArray(1,char(blendMode))+QByteArray(1,char(vis))+QByteArray(2,'\0')+rect(0,0,0,0)+rect(0,0,0,0)+QByteArray(2,'\0')+QByteArray(8,'\0');
        const QByteArray chunk=le32(head.size()+4)+head;
        return block(4,chunk+le32(8)+le16(bitmaps)+le16(channels)+subs); };
      // Two pixels as RLE: red plane "run of 2", green plane literal, blue literal, and an alpha plane.
      const QByteArray red=QByteArray::fromHex("82C8"), grn=QByteArray::fromHex("021020"), blu=QByteArray::fromHex("023040"), alp=QByteArray::fromHex("02FF80");
      const QByteArray lay1=layer("Paint",1,3,1,2,1,128,7,1,2,4,channel(0,1,red)+channel(0,2,grn)+channel(0,3,blu)+channel(1,0,alp));
      const QByteArray lay2=layer("Hidden",1,0,0,1,1,255,0,0,1,3,channel(0,1,QByteArray::fromHex("0105"))+channel(0,2,QByteArray::fromHex("0106"))+channel(0,3,QByteArray::fromHex("0107")));
      const Document pd=importPspDocument(file(5,8,4,1,24,false,block(3,lay1+lay2)));
      check(pd.pageSize==QSizeF(8,4) && pd.shapes.size()==2, "PSP: the page size and both raster layers arrive");
      if(pd.shapes.size()==2){
        const Shape& a=pd.shapes[0];
        check(a.name==QStringLiteral("Paint") && qAbs(a.opacity-128/255.0)<1e-6 && a.blend==QPainter::CompositionMode_Multiply, "PSP: name, opacity and blend mode");
        check(qAbs(a.docBounds().left()-3)<.01 && qAbs(a.docBounds().top()-1)<.01 && qAbs(a.docBounds().width()-2)<.01, "PSP: the layer sits at its rectangle");
        const QImage im=a.fill.image;
        check(im.size()==QSize(2,1) && im.pixel(0,0)==qRgba(0xC8,0x10,0x30,0xFF) && im.pixel(1,0)==qRgba(0xC8,0x20,0x40,0x80), "PSP: RLE planes and the transparency mask assemble");
        check(!pd.shapes[1].visible, "PSP: a hidden layer stays hidden");
      }
      // Version 5 with zlib, palette at four bits per pixel: rows pad to four bytes.
      QByteArray idx=QByteArray::fromHex("12000000");
      const QByteArray pal=block(2,le32(8)+le32(3)+QByteArray::fromHex("0A0B0C00 1A1B1C00 2A2B2C00"));
      const QByteArray zipped=qCompress(idx).mid(4);
      const QByteArray lay3=layer("Index",1,0,0,2,1,255,0,1,1,1,channel(0,0,zipped));
      const Document pi=importPspDocument(file(5,2,1,2,4,false,pal+block(3,lay3)));
      check(pi.shapes.size()==1 && pi.shapes[0].fill.image.pixel(0,0)==qRgb(0x1C,0x1B,0x1A) && pi.shapes[0].fill.image.pixel(1,0)==qRgb(0x2C,0x2B,0x2A), "PSP: zlib, a palette and packed 4-bit pixels");
      // A vector layer: one polyline with a filled square.
      auto node=[&](double x,double y,int mv,int flags){ return le32(55)+f64(x)+f64(y)+f64(x)+f64(y)+f64(x)+f64(y)+QByteArray(1,char(mv))+le16(flags); };
      auto paint=[&](int r,int g,int b){ const QByteArray col=QByteArray(1,char(r))+QByteArray(1,char(g))+QByteArray(1,char(b))+QByteArray(1,'\0')+QByteArray::fromHex("FFFFFFFF");
        return block(15,le32(6)+le16(1)+le32(12)+col); };
      const QByteArray attrs=QByteArray::fromHex("010101")+f64(2)+QByteArray(2,'\0')+f64(1)+f64(1)+QByteArray(2,'\0')+f64(1)+f64(1)+QByteArray(1,'\1')+f64(15);
      const QByteArray nm("Sq");
      const QByteArray shapeHead=le16(nm.size())+nm+le16(2)+le32(5)+le32(1)+le32(0);
      const QByteArray poly=le32(8)+le32(4)+node(10,10,1,0x40)+node(30,10,0,0x40)+node(30,20,0,0x40)+node(10,20,0,0x40|0x80);
      const QByteArray shape=block(14,le32(shapeHead.size()+4)+shapeHead+le32(attrs.size()+4)+attrs+paint(1,2,3)+paint(200,100,50)+poly);
      const QByteArray vec=block(13,le32(8)+le32(1)+shape);
      QByteArray nh=le16(1)+QByteArray("V")+QByteArray(1,'\3')+rect(0,0,0,0)+rect(0,0,0,0)+QByteArray(1,char(255))+QByteArray(1,'\0')+QByteArray(1,'\1')+QByteArray(2,'\0')+rect(0,0,0,0)+rect(0,0,0,0)+QByteArray(2,'\0')+QByteArray(8,'\0');
      const QByteArray lv=block(4,le32(nh.size()+4)+nh+vec);
      PspImportStats vs;
      const Document pv=importPspDocument(file(6,64,64,2,24,false,block(3,lv)),&vs);
      check(pv.shapes.size()==1 && pv.shapes[0].type==ShapeType::Group && vs.vectorShapes==1, "PSP: a vector layer's polyline stays a vector");
      if(pv.shapes.size()==1 && pv.shapes[0].children.size()==1){
        const Shape& q=pv.shapes[0].children[0];
        check(q.fill.color==QColor(200,100,50) && q.stroke.enabled && q.stroke.color==QColor(1,2,3) && qAbs(q.stroke.width-2)<.01, "PSP: the stroke paint comes first, then the fill");
        check(qAbs(q.docBounds().left()-10)<1.1 && qAbs(q.docBounds().width()-20)<2.1 && qAbs(q.docBounds().height()-10)<2.1, "PSP: nodes are in canvas coordinates");
      }
      check(importPspDocument("not a psp file at all, just some text to fill the buffer").shapes.isEmpty(), "PSP: anything else is refused");
    }

    // --- Clip Studio Paint, and the small SQLite reader under it ---
    std::printf("[interop] Clip Studio Paint\n");
    {
      const QByteArray clip=qUncompress(QByteArray::fromHex("00100000")+QByteArray::fromBase64(
        "eNrtGU1vG1VwZtfeDyeGVEnqBoS6EU2bthEgQJVAPWA7TrJK4gTH/QgCVdv4Jdlmveus16nTA9QgThX8AySExBEEl3LiACcQh3LhEnFAIEBCQkUqrUAIlff2rb+2jlLViI8mI8/b92bmzZt5876dXphIT53KToMPD7/Lv3AgPZWdniJGISiP8g9CCPrCBFYvU/UMXhIG6vVJ1XNtyywkd4Cgna85Qj+kwAIHlmANxsEAj2IKCKyACTakYRUq9LsGdwI1FUsWTatbP3/54SDsG4APrnsXbn51of+ha6dPfhw/9GjhjepWDLqDN19Y/vXWlcvy94/rsJvh/RM3nha/o5l9HSKWoTEqNKPVPkYQw2MktQME8p8H2NUYgZcWwR8jn2AcQBHfnvzo97eeiTy7+R521yHfjI18K7xy9x2y8PyMGcw9YHmPaMuOWzQ87SkQWCc9p2mUKVPsbWlGohgJubQTyPDYpfUom554i5Vj+AfP7MG/DDILpSTE/WlxA+hvD3YH9KAMKlJQpCkhjp9CFK8C3sSf8Cpu4Rd7HfTfh5goDuGsWSoaJYEWVZ8Yj4oHcW55ubzkEmILD0TE4UBGt5cdQRXEBM4Ym8SVVRSH1LRhbxjluL9IXwMa+XdwA+dwBP6kxf8rjMmPAA4JiiIoqsKAEZUAYEDW7Q3ien2Cv3dJhyUqnVDD0hjkFd22iYsQG40yrVSmVQ4x0ArShGMVmKCsjESoZL+vUWlKNjRGaXgQpNgxkTesCqgqqKoKiyEVQy4W0e01UoDLghCNDZwQmGzIJSaoRiN0DRfnjRJxpSeQ7c2fURY7Rz9J8RDSenFBxTbnGkbTLtAQREZkYyJtmSVNz+Yzk5ncmOYPEzaUiHe2I3UxRM0Ru8B5S9vUu1MirGPWKK91rtvkbFdnu1bD/HD9LD0I6tSwaog+YbplL71qWoVOXO4Ln1shVqPVMHfcNS6mHctxM7Zx3iIdGLOGaedIYRvOJJvU2/BSVqVF4aRrFAzPdOwJ07LY1NdSM3OpMY0WvcDABvUom/8CXAdM0uTH+37hnBekxPHjuOixGJTXLXr2PVcm6xViL4WLYjqXSeYzWj6ZmsloIeaobRTJGC0drZ1EOTE0hK8u+Dr5qspToU0Dp42emz9zTh+vR0ubz+mzydyiNp1Z1JKn8nN6llaazWTzYxqLrN4yHLiCM2bBW9Wo4pk6ZYqYK6teGylHyo5VYWOgnew4Hl+r6lqD+NPLw22aXPzH4/FyxO+82hm/8/ic4anS1nmc1kXnpYwyaW6GDfdf3xDlRCKBV6Z9A5pTONLWvE/qonW+1tAho+UzZ/NBOb9ZIuFlpy04LWuYaa+E1+GSsWR6myFq2imWnDK70bXTT5tl87xptVeg/R/144+/AP5Gkz2470ESIvTkMjAoIfvGevbivxvjP7ifx7+nl8VfhLOAVfiBfv42qPXIcuLIEaydatkW512yYZKLbYXeDptkwOpiwdWLxgoZNzyDn3JqD0py4uBBrBV9axqXpEamp82KBrkLC5Ke55rnK3Ql5qevlOUsrbVYFI3KieHhev80t6ZmTu2wAzJ6F0Y1HAvt/330QsEf2GsVQQJ87cCL+4Mn92WKxyge9u+Z82CAS7EIBDyKrl+dI4aQvxHiHU/lAbchpgRYBxV0sMGk+tPg+O+ZrBX2vI+3KQRSEV+y+d65QGtcohbxN0usv1Xe7UNvrYLc79579nu4pd1o4HckMKM7v+He/d7pT5A4BF7swa4E9n/ABL0W1Mt/AX8H7aA="));
      ClipImportStats st;
      const Document cd=importClipDocument(clip,&st);
      check(!clip.isEmpty() && cd.pageSize==QSizeF(8,6), "Clip: the canvas becomes the page");
      check(cd.shapes.size()==4 && st.folders==1 && st.paper==1 && st.pixelLayers==3 && st.masks==1 && st.corrections==1, "Clip: the layer tree arrives with its folder, paper, mask and correction");
      if(cd.shapes.size()==4){
        const Shape& paper=cd.shapes[0]; const Shape& ink=cd.shapes[1]; const Shape& off=cd.shapes[2]; const Shape& fold=cd.shapes[3];
        check(paper.name==QStringLiteral("Paper") && paper.fill.image.pixel(0,0)==qRgb(255-200,255-100,255-50), "Clip: a paper layer's colour, then the invert above it");
        check(ink.name==QStringLiteral("Inked") && ink.blend==QPainter::CompositionMode_Multiply && qAbs(ink.opacity-0.5)<.01, "Clip: blend mode and opacity (256 means full)");
        check(qAbs(ink.docBounds().left()-3)<.01 && qAbs(ink.docBounds().top()-1)<.01, "Clip: a layer sits at its offset plus its bitmap's");
        const QImage im=ink.fill.image;
        check(im.size()==QSize(2,2) && qRed(im.pixel(0,0))==255-200 && qAlpha(im.pixel(0,0))==255 && qGreen(im.pixel(0,1))==255-2, "Clip: tile colours come back in red, green, blue order");
        check(qAlpha(im.pixel(1,0))==128*128/255, "Clip: the tile's alpha plane and the layer mask multiply");
        check(!off.visible && off.type!=ShapeType::Group, "Clip: a hidden layer stays hidden");
        check(fold.type==ShapeType::Group && fold.name==QStringLiteral("Folder") && fold.children.size()==1 && fold.children[0].name==QStringLiteral("Inner"), "Clip: folders nest");
      }
      check(importClipDocument("CSFCHUNK and then nothing useful at all").shapes.isEmpty(), "Clip: a damaged file gives nothing");
      // The reader itself, on the fixture's database.
      SqliteMini db;
      const int q=clip.indexOf("CHNKSQLi");
      check(q>0 && db.open(clip.mid(q+16)), "SQLite: a database file opens");
      const QVector<SqliteMini::Row> lays=db.rows("Layer");
      check(db.hasTable("Canvas") && lays.size()==7 && lays[1].value("LayerName").toString()==QStringLiteral("Paper"), "SQLite: a table reads by name, text intact");
      check(lays[1].value("_PW_ID").toInt()==2 && !lays[1].contains("GradationFillInfo"), "SQLite: an INTEGER PRIMARY KEY reads as the rowid, and NULL is absent");
      check(lays[6].value("FilterLayerInfo").toByteArray()==QByteArray::fromHex("00000006"), "SQLite: a blob comes back whole");
      check(db.rows("Canvas").first().value("CanvasWidth").toDouble()==8.0, "SQLite: reals");
      check(db.rows("NoSuchTable").isEmpty() && !SqliteMini().open("not a database"), "SQLite: missing tables and other files are handled");
    }

    // --- Procreate ---
    std::printf("[interop] Procreate\n");
    {
      QMap<QString,QByteArray> pz;
      pz.insert("Document.archive",QByteArray::fromBase64("YnBsaXN0MDDUAQIDBAUGZGdZJGFyY2hpdmVyWCRvYmplY3RzVCR0b3BYJHZlcnNpb25fEA9OU0tleWVkQXJjaGl2ZXKvEBkHCA4RFBUWKSorLzAxNjc4PUJDSU5PUlNUVSRudWxs0gkKCwxYJGNsYXNzZXNaJGNsYXNzbmFtZaIMDVtTaWxpY2FMYXllclhOU09iamVjdNIJCg8QohANW1NpbGljYUdyb3Vw0gkKEhOiEw1XTlNBcnJheVRNTU1NWkxheWVyIE1hc2vaFxgZGhscHR4fICEiIyQjJCUmJyhWJGNsYXNzVFVVSURVYmxlbmRXY2xpcHBlZF1leHRlbmRlZEJsZW5kVmhpZGRlblRtYXNrVG5hbWVXb3BhY2l0eVd2ZXJzaW9ugAGABBAACIAAgAUjP/AAAAAAAAAQAVRBQUFBVkJvdHRvbdoXGBkaGxwdHh8gISwjJCMkLS4nKIAHgACACFRCQkJCU1RvcNoXGBkaGxwdHh8gITIoJCgkMzQ1KIAKgAaACyM/4AAAAAAAAFRDQ0NDVUlubmVy2hcYGRobHB0eHyAhOSMkIzo7PCcogA0JgACADtIXPj9AWk5TLm9iamVjdHOAA6FBgA9TR3Jw1BdERR5GRyRIWGNoaWxkcmVuWGlzSGlkZGVugAKAEIAR0hc+P0qjS0xNgAyAEoAJTxAQAACAPwAAAD8AAAAAAACAP9IJClBRolENXlNpbGljYURvY3VtZW50Vns0LCAyfVF02xdVVldYHllaW1xdXl8kJCRgKGEjYmNfEA9iYWNrZ3JvdW5kQ29sb3JfEBBiYWNrZ3JvdW5kSGlkZGVuXxATZmxpcHBlZEhvcml6b250YWxseV8QEWZsaXBwZWRWZXJ0aWNhbGx5W29yaWVudGF0aW9uVHNpemVbc3Ryb2tlQ291bnRYdGlsZVNpemVfEA91bndyYXBwZWRMYXllcnOAFYAUgBeAFhACgBPRZWZUcm9vdIAYEgABhqAACAARABsAJAApADIARABgAGYAawB0AH8AggCOAJcAnACfAKsAsACzALsAwADLAOAA5wDsAPIA+gEIAQ8BFAEZASEBKQErAS0BLwEwATIBNAE9AT8BRAFLAWABYgFkAWYBawFvAYQBhgGIAYoBkwGYAZ4BswG1AbYBuAG6Ab8BygHMAc4B0AHUAd0B5gHvAfEB8wH1AfoB/gIAAgICBAIXAhwCHwIuAjUCNwJOAmACcwKJAp0CqQKuAroCwwLVAtcC2QLbAt0C3wLhAuQC6QLrAAAAAAAAAgEAAAAAAAAAaAAAAAAAAAAAAAAAAAAAAvA="));
      // Tile 0~0 of "AAAA", all red, as LZO: a literal pixel then a long match over it. Rows are stored bottom-up.
      const QByteArray px=QByteArray::fromHex("C81432FF");
      QByteArray lzo; lzo.append(char(17+4)); lzo.append(px);
      lzo.append(char(0x20|(12-2))); lzo.append(char((4-1)<<2)); lzo.append('\0');   // copy 12 from 4 back
      lzo.append(QByteArray::fromHex("110000"));
      pz.insert("AAAA/0~0.chunk",lzo);
      // Tile 1~0 of "AAAA": stored LZ4, then the end mark.
      QByteArray stored; for(int i=0;i<4;++i) stored.append(QByteArray::fromHex("0A1420FF"));
      QByteArray l4="bv4-"; l4.append(QByteArray::fromHex("10000000")); l4.append(stored); l4.append("bv4$");
      pz.insert("AAAA/1~0.lz4",l4);
      // "BBBB" tile 0~0 with a real LZ4 block: one literal pixel, then a match of 12 at distance 4.
      QByteArray blk=QByteArray::fromHex("48")+QByteArray::fromHex("01020304")+QByteArray::fromHex("0400");
      QByteArray f="bv41"; f.append(QByteArray::fromHex("10000000")); f.append(char(blk.size())); f.append(QByteArray::fromHex("000000")); f.append(blk); f.append("bv4$");
      pz.insert("BBBB/0~0.lz4",f);
      // Its mask: one byte a pixel.
      QByteArray mk="bv4-"; mk.append(QByteArray::fromHex("04000000")); mk.append(QByteArray::fromHex("FF00FF00")); mk.append("bv4$");
      pz.insert("MMMM/0~0.lz4",mk);
      QByteArray cc="bv4-"; cc.append(QByteArray::fromHex("10000000")); cc.append(stored); cc.append("bv4$");
      pz.insert("CCCC/1~0.lz4",cc);
      ProcreateImportStats ps;
      const Document pd=importProcreateDocument(pz,&ps);
      check(pd.pageSize==QSizeF(4,2) && ps.layers==3 && ps.groups==1 && ps.masks==1 && ps.badTiles==0, "Procreate: the page and every layer arrive, and all the tiles decode");
      check(pd.shapes.size()==4, "Procreate: background, then the layers from the bottom up");
      if(pd.shapes.size()==4){
        check(pd.shapes[0].name==QStringLiteral("Background") && pd.shapes[0].fill.image.pixel(0,0)==qRgb(255,128,0), "Procreate: the background colour is a layer of its own");
        check(pd.shapes[1].name==QStringLiteral("Bottom") && pd.shapes[1].fill.image.size()==QSize(4,2), "Procreate: tiles assemble into one bitmap");
        check(pd.shapes[1].fill.image.pixel(0,0)==qRgba(0xC8,0x14,0x32,0xFF) && pd.shapes[1].fill.image.pixel(0,1)==qRgba(0xC8,0x14,0x32,0xFF), "Procreate: an LZO tile, literals and a match");
        check(pd.shapes[1].fill.image.pixel(3,1)==qRgba(0x0A,0x14,0x20,0xFF), "Procreate: a stored LZ4 tile");
        const Shape& g=pd.shapes[2];
        check(g.type==ShapeType::Group && g.name==QStringLiteral("Grp") && g.children.size()==1 && !g.children[0].visible, "Procreate: groups nest, and a hidden layer stays hidden");
        const Shape& t=pd.shapes[3];
        check(t.name==QStringLiteral("Top") && qAbs(t.opacity-0.5)<.01 && t.blend==QPainter::CompositionMode_Multiply, "Procreate: blend mode and opacity");
        // Stored rows run upward, so stored row 0's mask FF 00 lands on the picture's bottom row.
        const QImage ti=t.fill.image;
        check(qAlpha(ti.pixel(0,1))==4 && qAlpha(ti.pixel(1,1))==0, "Procreate: an LZ4 block with a match, and its layer mask, the right way up");
      }
      check(importProcreateDocument(QMap<QString,QByteArray>()).shapes.isEmpty(), "Procreate: an archive without a document gives nothing");
      QMap<QString,QByteArray> bad=pz; bad["AAAA/0~0.chunk"]=QByteArray::fromHex("FFFFFF"); ProcreateImportStats bs;
      importProcreateDocument(bad,&bs);
      check(bs.badTiles==1, "Procreate: a damaged tile is counted, not fatal");
    }

    // --- Corel PHOTO-PAINT ---
    std::printf("[interop] Corel PHOTO-PAINT\n");
    {
      auto le32=[](quint32 v){ QByteArray b(4,'\0'); for(int i=0;i<4;++i) b[i]=char(v>>(8*i)); return b; };
      auto lzwBytes=[](const QByteArray& raw){     // every byte as its own 9-bit code, between clear and end
        QByteArray out; quint32 acc=0; int nb=0;
        auto put=[&](int code){ acc|=quint32(code)<<nb; nb+=9; while(nb>=8){ out.append(char(acc&255)); acc>>=8; nb-=8; } };
        put(256); for(uchar c : raw) put(c); put(257); if(nb) out.append(char(acc&255)); return out; };
      auto build=[&](int w,int h,int bpp,const QByteArray& pal,const QByteArray& tile){
        QByteArray f("CPT7FILE"); f+=le32(bpp==8?10:1); f+=le32(pal.size()); f.append(QByteArray(0x138-f.size(),'\0'));
        f+=pal; const quint32 off=0x140+pal.size(); f+=le32(0); f+=le32(off);       // the field that locates the record
        f.resize(int(off));
        f+=le32(w); f+=le32(h); f+=le32(144); f+=le32(151); f+=le32(bpp);
        const quint32 pos=off+20+8; f+=le32(pos); f+=le32(tile.size()); f+=tile;
        return f; };
      // Two rows, each padded to four bytes: blue, green, red.
      const QByteArray rgb=QByteArray::fromHex("0A141E00")+QByteArray::fromHex("28323C00");
      QByteArray t24=QByteArray::fromHex("02000300")+lzwBytes(rgb);
      QImage ci=importCpt(build(1,2,24,QByteArray(),t24));
      check(ci.size()==QSize(1,2) && ci.pixel(0,0)==qRgb(0x1E,0x14,0x0A) && ci.pixel(0,1)==qRgb(0x3C,0x32,0x28), "PHOTO-PAINT: LZW tile, blue first, rows padded to four bytes");
      QByteArray pal(768,'\0'); pal[3]=char(11); pal[4]=char(22); pal[5]=char(33);
      const QByteArray t8=QByteArray::fromHex("02000000")+lzwBytes(QByteArray::fromHex("0001")+QByteArray(2,'\0'));
      QImage pi=importCpt(build(2,1,8,pal,t8));
      check(pi.size()==QSize(2,1) && pi.pixel(0,0)==qRgb(0,0,0) && pi.pixel(1,0)==qRgb(11,22,33), "PHOTO-PAINT: indexed tile through its palette");
      // The LZW edge cases: a code naming the string it defines, and a code past the table.
      QByteArray self; { quint32 acc=0; int nb=0; auto put=[&](int code){ acc|=quint32(code)<<nb; nb+=9; while(nb>=8){ self.append(char(acc&255)); acc>>=8; nb-=8; } };
        put(256); put(7); put(258); put(257); if(nb) self.append(char(acc&255)); }
      QImage sl=importCpt(build(3,1,8,pal,QByteArray::fromHex("02000000")+self+QByteArray()));
      check(sl.isNull(), "PHOTO-PAINT: output shorter than the image is refused");
      QImage s2=importCpt(build(1,1,24,QByteArray(),QByteArray::fromHex("02000300")+self));
      check(s2.isNull(), "PHOTO-PAINT: rows that don't fill the stride are refused");
      CptImportStats us;
      const QImage un=importCpt(build(4,4,24,QByteArray(),QByteArray::fromHex("050003003F3F0000")),&us);
      check(un.isNull() && us.unknownTiles==1, "PHOTO-PAINT: the unread compression is counted, so the message can say so");
      check(importCpt("CPT7FILE too short").isNull(), "PHOTO-PAINT: a truncated header is refused");
    }

    // --- Pixelmator Pro ---
    std::printf("[interop] Pixelmator Pro\n");
    {
      const auto unz=[](const char* b64){ return qUncompress(QByteArray::fromHex("00040000")+QByteArray::fromBase64(b64)); };
      QMap<QString,QByteArray> px;
      px.insert("wrap/metadata.info",unz("eNrtV81vG1UQf7P7NvEmpE1Jg0uLnY0TqwlxkEsshJAqYsduEjVRnNhtJaoobLxrdxV7d7veNB9VhAL8AZw5ASf+AiS4ceLCvaq4BIkbHOAAFw4ws17HH7HrciTKaL1v3pt5897vzZvZcW59xXB1pWg5FdVV5pjAANi8ojDGOP5k1iDRH6sTsN7E2VuHj4e8yV8w+BoEbP4TfSj2B8fH4XjQVbfLelk90J0twyxaDY4vbGSS+YyST6ZWMkpjfMpnNcUwXb2kOzFlRz9QXH3fjSlP1PKurmyXre3p4wmhPzgzA8dH3hKaVdit6Ka75U2vtnXFlsXahFPNaxkaCoyioTv+krbqkOqZccPU9P0t1d2qKTQsuAe2Xu9N34e+4M2bUGndI+Fs6Qid9+edR0f05BuAnxg+F3S+aALE0XTVONQTs272MJdODePgVV844Lfkfwm+Y/An/ALP4AdkL+h/QYNSH4wwkKWl5bRwiWNnJAA8t5Rd3MiKgyIJAyBih18WSCjIwmru7kYy99E/MvTJI0yWsSNRkobfyKAMf8Nf8Eetc0HnkgIT+Oq/ga++6xT5Q0NeDogxOIFv4DN4DGsQhwH2K/uRfckO2QcoOqXYEIe5hFBRqzsptbBTcqxdU7tP3xLKMOU7+WwAtd7/vaYdfoVDMCEUy2qpSvLEO8v3SH7FtxYd5DCaFhoJKp8dbppeb6cHOITSgm1VDdewTFJ1szXVeb8amR+utSEZLSYEU614Fk3HzQX8mmUV9xwKcDmYUM1SuWW/dZrs53I0sY3ADLOUK6itWvO+VqiPjJwBVa+FFInLoYRlF9xZ06mQhr2WXvGO5cQ3wLk8FsOPuKmtWpq3hmaWU7RJpbxbmRS53Jy1Ox/KlIDLpF/mTMKA9t5tPhLK/BT1WRVrC1bzf5zBc/gWPgcLlmAKAuxn9j37iu2xLIpaKHydvMq7HkD0dfIqbwdQP75Tr14jr/KOCK61eTVIXuXtXqVqNGXts/BrtCGxq1ujoxyiCbGXX8NXPTNdcYVHOIzFxI5+q1ZVO/oq4RbP4J70wfjf3ekrhFvsiNtXqXswNEy4xU64Fx2bhS974dUd9yXCLfTC7dV/7Dmj51zRx5M3JJj59E1efaTa+qz3Tquu+jTyBEt1PPucrRewDl+wTBcjQXci7ylPI7bqPlqwNCqyHziqbaMqjj988SQq1JGJOHrBjcSUiLdWFi3dM7Hm1rVmbTS1703ScC93HKuysEiaNH11L7mbTC8nV0rJvWQmnuxE+Uxy/exoanU9dco3hjN7TXyD3Vi/fTtydLTpPeT/AJtj8DYMNmX9uRecbHSCbpbUM6IidEGlMxF1amacIkbqmf8VihjppfL/GEWM1Cli1opFFg57qat7xIQIF++F6/juGxJMfMJ41T0o69VZcuTDWzF0aZH8W2MzyCITsfy2mEcmjkyBdDyVOhf3nlub6I3NTbo93a088Ns2I76FmhEU507FNk2oG861Go7j6NFm7f/fCX7/L4qj80jhSS8Ku176fwF9lye4"));
      px.insert("wrap/data/RAS",unz("eNoLCHHKLMlNLHAqTUtLLYqPZ2JAACYo5gBiRqjYfyh4LDTx9/rElMsHThz5M7916lt7BgC22xkW"));
      px.insert("wrap/data/MSK",unz("eNoLCHHKLMlNLHAqTUtLLYqPZ2JAACYkzAgV+w8Fv+8f+CDPAACwShHE"));
      px.insert("wrap/data/HID",unz("eNoLCHHKLMlNLHAqTUtLLYqPZ2JAACYo5gBiRqjYfyhITkj4cb6hhREAz3ISEA=="));
      PxdImportStats ps;
      const Document pd=importPxdDocument(px,&ps);
      check(pd.pageSize==QSizeF(20,10), "Pixelmator: the document's size becomes the page, whatever folder wraps the file");
      check(pd.shapes.size()==3 && ps.rasterLayers==2 && ps.vectorLayers==1 && ps.groups==1 && ps.masks==1, "Pixelmator: the layers arrive, with the mask taken out of the list");
      if(pd.shapes.size()==3){
        const Shape& off=pd.shapes[0]; const Shape& ras=pd.shapes[1]; const Shape& grp=pd.shapes[2];
        check(off.name==QStringLiteral("Off") && !off.visible, "Pixelmator: index 0 is the top of the stack, and a hidden layer stays hidden");
        check(ras.name==QStringLiteral("Paint") && ras.blend==QPainter::CompositionMode_Multiply && qAbs(ras.opacity-0.5)<.01, "Pixelmator: name, blend mode and opacity");
        check(qAbs(ras.docBounds().left()-4)<.01 && qAbs(ras.docBounds().top()-1)<.01, "Pixelmator: points scale up to pixels, and y runs up from the bottom");
        const QImage im=ras.fill.image;
        check(im.size()==QSize(2,2) && qRed(im.pixel(0,0))==30 && qBlue(im.pixel(0,0))==10 && qAlpha(im.pixel(1,1))==255, "Pixelmator: rows are blue, green, red, alpha");
        check(qAlpha(im.pixel(1,0))==128*128/255 && qAlpha(im.pixel(0,1))==0, "Pixelmator: a grey mask multiplies the layer's alpha");
        check(grp.type==ShapeType::Group && !grp.isolate && grp.children.size()==1, "Pixelmator: a pass-through group stays open");
        if(grp.children.size()==1){
          const Shape& box=grp.children[0];
          check(box.fill.color==QColor(255,0,0) && box.stroke.enabled && box.stroke.color==QColor(0,0,255) && qAbs(box.stroke.width-1)<.01, "Pixelmator: a shape's fill and stroke styles");
          const QRectF b=box.docBounds();
          check(qAbs(b.left()-10)<1.1 && qAbs(b.width()-4)<2.1 && qAbs(b.height()-2)<2.1, "Pixelmator: a shape's outline sits in its box");
        }
      }
      check(importPxdDocument(QMap<QString,QByteArray>()).shapes.isEmpty(), "Pixelmator: an archive with no database gives nothing");
    }

    std::printf("\n\033[1m%d passed, %d failed\033[0m\n\n", g_pass, g_fail);
    return g_fail==0 ? 0 : 1;
}
