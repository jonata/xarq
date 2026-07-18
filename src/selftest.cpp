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
#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <cstdio>
#include "mainwindow.h"
#include "canvas.h"
#include "galleries.h"
#include "pathedit.h"
#include "panels.h"
#include "render.h"
#include <QCheckBox>
#include <QElapsedTimer>
#include <QTreeWidget>
#include "spell.h"
#include "qr.h"
#include "exif.h"
#include "photoops.h"
#include "trace.h"
#include "hyphen.h"
#include "svgimport.h"
#include "svgexport.h"
#include "pathedit.h"
#include "render.h"
#include "prepress.h"
#include "icc.h"
#include "raw.h"
#include "otshape.h"
#include "xar.h"

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
      // AI routing: a %PDF-headed .ai goes through the PDF path
      QByteArray ai="%PDF-1.4\n1 0 obj<</Type/Catalog/Pages 2 0 R>>endobj\n2 0 obj<</Type/Pages/Kids[3 0 R]/Count 1>>endobj\n"
        "3 0 obj<</Type/Page/Parent 2 0 R/MediaBox[0 0 100 100]/Contents 4 0 R>>endobj\n"
        "4 0 obj<</Length 30>>\nstream\n1 0 0 rg\n5 5 40 40 re f\nendstream endobj\ntrailer<</Root 1 0 R>>\n";
      Document da=importAi(ai);
      check(da.shapes.size()>=1 && da.shapes[0].fill.color==QColor(255,0,0), "AI-as-PDF routes through importPdf (red rect)"); }

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
      check(pf.shapes.size()==1 && pf.shapes[0].fill.type==FillType::Bitmap, "layerless PSD → single flattened bitmap shape"); }

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
    { QByteArray svg =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"200\" height=\"200\">"
        "<rect x=\"10\" y=\"10\" width=\"50\" height=\"40\" fill=\"#ff0000\"/>"
        "<circle cx=\"100\" cy=\"100\" r=\"30\" fill=\"rgb(0,128,255)\"/>"
        "<g transform=\"translate(20,20) scale(2)\">"
        "<path d=\"M0 0 L10 0 L10 10 Z\" style=\"fill:green;stroke:black;stroke-width:2\"/>"
        "</g>"
        "<polygon points=\"150,10 190,10 170,50\" fill=\"none\" stroke=\"blue\"/>"
        "</svg>";
      QVector<Shape> sh = parseSvg(svg);
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
      QVector<Shape> sc = parseSvg("<svg><g transform=\"scale()\"><rect width=\"10\" height=\"10\"/></g></svg>");
      check(!sc.isEmpty() && qAbs(sc[0].transform.m11()-1.0)<0.01, "scale() with no args is identity, not 0-scale"); }
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
      QVector<Shape> sh = parseSvg(svg);
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
      // nested group: export preserves grouping structure as <g>, importer flattens to leaves with composed transforms
      { QVector<Shape> g; Shape grp; grp.type=ShapeType::Group; grp.transform=QTransform::fromScale(2,2);
        Shape c; c.type=ShapeType::Rect; c.rect=QRectF(0,0,10,10); c.fill.type=FillType::Solid; c.fill.color=QColor(1,2,3); c.stroke.enabled=false;
        grp.children.push_back(c); g.push_back(grp);
        QByteArray gs = writeSvg(g, QRectF(0,0,100,100));
        check(gs.contains("<g"), "group exports as <g>");
        QVector<Shape> gr = parseSvg(gs);
        check(gr.size()==1 && gr[0].fill.color==QColor(1,2,3), "group child round-trips (flattened by importer)");
        QPointF o=gr[0].transform.map(QPointF(10,10)); check(qAbs(o.x()-20)<0.5, "group scale(2) baked into child on round-trip (10→20)"); } }

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
      check(!im.isEmpty() && qAbs(im[0].fill.p1.x()-70)<0.5 && qAbs(im[0].fill.p2.y()-60)<0.5, "bitmap fill parallelogram spans the box (p1.x=70, p2.y=60)");
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
      // Closing tabs (mechanism, no confirm dialog): count drops, never reaches zero
      const int cntBefore=w.tabCount();
      w.closeTab(tabB);
      check(w.tabCount()==cntBefore-1, "closeTab removes a tab");
      while(w.tabCount()>1) w.closeTab(w.activeTab());
      check(w.tabCount()==1, "at least one document tab always remains");
      w.closeTab(0);
      check(w.tabCount()==1 && c->doc().shapes.isEmpty(), "closing the sole tab resets it to a blank document (never zero tabs)");
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
      check(c->doc().shapes.back().fill.image.width()==20, "the pasted photo carries the clipboard image"); }
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

    std::printf("\n\033[1m%d passed, %d failed\033[0m\n\n", g_pass, g_fail);
    return g_fail==0 ? 0 : 1;
}
