// splash.cpp — see splash.h. Faithful Qt rendering of the DS `ui_kits/splash` kit.
#include "splash.h"
#include <QtWidgets>
#include <QPropertyAnimation>

namespace xara {

// ---- DS tokens used here (from tokens/colors.css + effects.css) ----
static const QColor kSurfacePanel (0x1A,0x23,0x2B);   // --surface-panel  (neutral-850)
static const QColor kSurfaceRaised(0x21,0x2D,0x37);   // --surface-raised (neutral-800)
static const QColor kSurfaceWell  (0x30,0x42,0x51);   // --surface-well   (neutral-750)
static const QColor kSurfaceChrome(0x0D,0x12,0x16);   // --surface-chrome (neutral-900)
static const QColor kTextStrong   (0xF0,0xF6,0xFB);   // --text-strong
static const QColor kTextSecondary(0x8E,0xA6,0xB9);   // --text-secondary
static const QColor kTextTertiary (0x7A,0x93,0xA7);   // --text-tertiary
static const QColor kAccent400    (0xC8,0xDB,0xE9);   // --accent-400
static const QColor kAccent500    (0x2D,0x6F,0xF7);   // --accent-500
static const QColor kAccentTop    (0xC8,0xDB,0xE9);   // grad-accent top
static const QColor kViolet       (0x7C,0x5C,0xFF);   // mark violet blade
static const QColor kSuccess      (0x5D,0xE8,0x45);   // --success-500

static const char* kSteps[] = {
    "Starting Qt runtime…",
    "Loading GPU canvas…",
    "Initializing colour engine…",
    "Restoring workspace…",
    "Loading fonts & swatches…",
    "Ready",
};
static const int kStepCount = 6;

// widget = panel (720×460) + margin for the drop shadow
static constexpr int kMargin = 22;
static constexpr int kPanelW = 720, kPanelH = 460;
static constexpr int kFooterH = 96;

LaunchSplash::LaunchSplash(QWidget* parent)
    : QWidget(parent, Qt::SplashScreen | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setFixedSize(kPanelW + 2*kMargin, kPanelH + 2*kMargin);
    setCursor(Qt::PointingHandCursor);
    m_timer = new QTimer(this);
    m_timer->setInterval(130);   // brief boot flash, not a long fake load
    connect(m_timer, &QTimer::timeout, this, &LaunchSplash::tick);
}

void LaunchSplash::start() {
    // Centre on the primary screen.
    if (QScreen* s = QGuiApplication::primaryScreen()) {
        const QRect g = s->availableGeometry();
        move(g.center() - QPoint(width()/2, height()/2));
    }
    show(); raise(); activateWindow();
    m_timer->start();
}

void LaunchSplash::tick() {
    if (m_done) return;
    m_pct = qMin(100.0, m_pct + 24.0);
    m_step = qMin(kStepCount-1, int(m_pct/100.0 * (kStepCount-1) + 1e-6));
    update();
    if (m_pct >= 100.0) {
        m_timer->stop();
        // brief hold on the green "Ready" beat, then fade out
        QTimer::singleShot(250, this, &LaunchSplash::finishNow);
    }
}

void LaunchSplash::finishNow() {
    if (m_done) return;
    m_done = true;
    m_timer->stop();
    m_fade = new QPropertyAnimation(this, "windowOpacity", this);
    m_fade->setDuration(200);
    m_fade->setStartValue(windowOpacity());
    m_fade->setEndValue(0.0);
    // CRITICAL: hide the (always-on-top) splash BEFORE emitting finished() — the finished handler opens the
    // MODAL welcome screen via a blocking exec(), so if the splash were still shown it would sit on top and
    // swallow every click on the welcome/open dialogs. Then destroy it.
    connect(m_fade, &QPropertyAnimation::finished, this, [this]{ hide(); emit finished(); deleteLater(); });
    m_fade->start(QAbstractAnimation::DeleteWhenStopped);
}

void LaunchSplash::mousePressEvent(QMouseEvent*) { finishNow(); }
void LaunchSplash::keyPressEvent(QKeyEvent*)     { finishNow(); }

// Draw the XarQ logomark (the bundled xarq-mark.svg, in its native 100×100 box)
// centred inside a `size`-px square whose top-left is `topLeft`.
static void drawMark(QPainter& p, const QPointF& topLeft, double boxSize, double markSize) {
    p.save();
    const double inset = (boxSize - markSize) / 2.0;
    p.translate(topLeft.x() + inset, topLeft.y() + inset);
    p.scale(markSize/100.0, markSize/100.0);
    p.setBrush(Qt::NoBrush);
    { QPen pen(kViolet, 15); pen.setCapStyle(Qt::RoundCap); p.setPen(pen); p.drawLine(QPointF(72,28), QPointF(28,72)); }
    { QPen pen(kAccent500, 15); pen.setCapStyle(Qt::RoundCap); p.setPen(pen); p.drawLine(QPointF(28,28), QPointF(84,84)); }
    { QPen pen(kViolet, 3.4); p.setPen(pen); p.setBrush(QColor(0x0C,0x0D,0x10));
      p.drawRoundedRect(QRectF(66.7,22.7,10.6,10.6), 2.2, 2.2); }
    p.restore();
}

void LaunchSplash::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    const QRectF panel(kMargin, kMargin - 4, kPanelW, kPanelH);   // nudged up a touch for a heavier bottom shadow
    const qreal R = 10.0;   // --radius-2xl

    // ---- drop shadow (layered rounded rects → approximates a soft blur) ----
    p.setPen(Qt::NoPen);
    for (int i = kMargin; i >= 1; --i) {
        QRectF r = panel.adjusted(-i, -i + 4, i, i + 7);
        p.setBrush(QColor(0, 0, 0, 7));
        p.drawRoundedRect(r, R + i, R + i);
    }

    // ---- panel body ----
    QPainterPath panelPath; panelPath.addRoundedRect(panel, R, R);
    p.fillPath(panelPath, kSurfacePanel);

    // hero / footer split
    const QRectF hero(panel.left(), panel.top(), panel.width(), panel.height() - kFooterH);
    const QRectF footer(panel.left(), panel.bottom() - kFooterH, panel.width(), kFooterH);
    const double cx = panel.center().x();

    // ---- hero: glow + grid, clipped to the panel ----
    p.save();
    p.setClipPath(panelPath);
    QRectF heroClip = hero;
    p.setClipRect(heroClip, Qt::IntersectClip);

    // brand glow (radial approximation of grad-brand, blurred)
    {
        QRadialGradient g(QPointF(cx, hero.top() + 24), hero.width()*0.55);
        g.setColorAt(0.0, QColor(0xB8,0xCE,0xE0, 60));
        g.setColorAt(0.45, QColor(0xB8,0xCE,0xE0, 30));
        g.setColorAt(0.75, QColor(0x7C,0x5C,0xFF, 30));
        g.setColorAt(1.0, QColor(0x7C,0x5C,0xFF, 0));
        p.fillRect(heroClip, g);
    }
    // faint 32px grid
    {
        QPen gp(QColor(255,255,255,14)); gp.setWidthF(1.0); p.setPen(gp);
        for (double x = hero.left(); x <= hero.right(); x += 32) p.drawLine(QPointF(x, hero.top()), QPointF(x, hero.bottom()));
        for (double y = hero.top();  y <= hero.bottom(); y += 32) p.drawLine(QPointF(hero.left(), y), QPointF(hero.right(), y));
    }
    // radial vignette: fade glow+grid toward the panel colour at the edges (the DS mask)
    {
        QRadialGradient m(QPointF(cx, hero.top() + hero.height()*0.42), hero.width()*0.62);
        m.setColorAt(0.0, QColor(0,0,0,0));
        m.setColorAt(0.55, QColor(0,0,0,0));
        m.setColorAt(1.0, kSurfacePanel);
        p.fillRect(heroClip, m);
    }
    p.restore();

    // ---- logo tile ----
    const double tileSize = 132;
    const double tileTop  = hero.top() + 62;
    const QRectF tile(cx - tileSize/2, tileTop, tileSize, tileSize);
    p.save();
    p.setPen(Qt::NoPen);
    // soft shadow under the tile
    for (int i = 10; i >= 1; --i) { p.setBrush(QColor(0,0,0,10)); p.drawRoundedRect(tile.adjusted(-i, -i+3, i, i+5), 7+i, 7+i); }
    QPainterPath tilePath; tilePath.addRoundedRect(tile, 7, 7);   // --radius-xl
    p.fillPath(tilePath, kSurfaceRaised);
    p.setPen(QPen(QColor(255,255,255,41), 1));    // --border-strong (~0.16)
    p.setBrush(Qt::NoBrush); p.drawRoundedRect(tile.adjusted(0.5,0.5,-0.5,-0.5), 7, 7);
    // top highlight (--hl-top)
    p.setPen(QPen(QColor(255,255,255,20), 1)); p.drawLine(tile.left()+7, tile.top()+1, tile.right()-7, tile.top()+1);
    p.restore();
    drawMark(p, tile.topLeft(), tileSize, 84);

    // ---- wordmark: accent "X" + "arQ" ----
    QFont wf("Montserrat"); wf.setPixelSize(44); wf.setWeight(QFont::ExtraBold);
    wf.setLetterSpacing(QFont::AbsoluteSpacing, -1.3);
    QFontMetricsF wfm(wf);
    const double wordW = wfm.horizontalAdvance("XarQ");
    const double wordX = cx - wordW/2.0;
    const double wordBaseline = tile.bottom() + 22 + wfm.ascent();
    p.setFont(wf);
    p.setPen(kTextStrong); p.drawText(QPointF(wordX, wordBaseline), "XarQ");
    p.setPen(kAccent400);  p.drawText(QPointF(wordX, wordBaseline), "X");   // overprint the X in accent

    // ---- tagline ----
    QFont tf("Montserrat"); tf.setPixelSize(12); tf.setWeight(QFont::DemiBold);
    tf.setLetterSpacing(QFont::AbsoluteSpacing, 2.2);
    QFontMetricsF tfm(tf);
    const QString tag = "VECTOR · PHOTO · LAYOUT";
    const double tagBaseline = wordBaseline + wfm.descent() + 10 + tfm.ascent();
    p.setFont(tf); p.setPen(kTextTertiary);
    p.drawText(QPointF(cx - tfm.horizontalAdvance(tag)/2.0, tagBaseline), tag);

    // ---- footer ----
    p.save();
    p.setClipPath(panelPath);
    p.fillRect(footer, kSurfaceChrome);
    p.setPen(QPen(QColor(255,255,255,16), 1));   // --border-subtle top divider
    p.drawLine(footer.left(), footer.top(), footer.right(), footer.top());
    p.restore();

    const double padX = 24;
    const bool ready = m_pct >= 100.0;

    // status row
    QFont sf("Montserrat"); sf.setPixelSize(12); sf.setWeight(QFont::Medium); QFontMetricsF sfm(sf);
    const double statusBaseline = footer.top() + 18 + sfm.ascent();
    // status dot / check
    const QPointF dotC(footer.left()+padX+6, statusBaseline - sfm.ascent()/2.0 + 1);
    p.setPen(Qt::NoPen); p.setBrush(ready ? kSuccess : kAccent400);
    p.drawEllipse(dotC, 5, 5);
    if (ready) { QPen cp(QColor(0x0C,0x0D,0x10), 1.6); cp.setCapStyle(Qt::RoundCap); p.setPen(cp);
        p.drawLine(dotC + QPointF(-2.2,0.2), dotC + QPointF(-0.6,1.9));
        p.drawLine(dotC + QPointF(-0.6,1.9), dotC + QPointF(2.4,-1.9)); }
    p.setFont(sf); p.setPen(ready ? kSuccess : kTextSecondary);
    p.drawText(QPointF(footer.left()+padX+18, statusBaseline), kSteps[m_step]);
    // percent (mono, right)
    QFont mf("Ubuntu Mono"); mf.setPixelSize(12); mf.setWeight(QFont::Medium); QFontMetricsF mfm(mf);
    const QString pctStr = QString::number(int(m_pct + 0.5)) + "%";
    p.setFont(mf); p.setPen(kTextTertiary);
    p.drawText(QPointF(footer.right()-padX - mfm.horizontalAdvance(pctStr), statusBaseline), pctStr);

    // progress track
    const double trackY = footer.top() + 18 + sfm.height() + 6;
    const QRectF track(footer.left()+padX, trackY, footer.width()-2*padX, 4);
    p.setPen(Qt::NoPen);
    p.fillRect(track, QColor(0,0,0,0));   // nothing; use rounded path below
    { QPainterPath tp; tp.addRoundedRect(track, 2, 2); p.fillPath(tp, kSurfaceWell);
      p.setPen(QPen(QColor(0,0,0,90),1)); p.setBrush(Qt::NoBrush);   // inset-well hint
      p.drawRoundedRect(track.adjusted(0.5,0.5,-0.5,-0.5), 2, 2); }
    const double fillW = qMax(4.0, track.width() * m_pct/100.0);
    QRectF fill(track.left(), track.top(), fillW, track.height());
    QLinearGradient fg(fill.topLeft(), fill.bottomLeft());
    fg.setColorAt(0.0, kAccentTop); fg.setColorAt(1.0, QColor(0xB8,0xCE,0xE0));
    QPainterPath fp; fp.addRoundedRect(fill, 2, 2); p.fillPath(fp, fg);

    // meta line
    QFont metaUi("Montserrat"); metaUi.setPixelSize(11); metaUi.setWeight(QFont::Medium); QFontMetricsF muf(metaUi);
    QFont metaMono("Ubuntu Mono"); metaMono.setPixelSize(11); metaMono.setWeight(QFont::Medium);
    const double metaBaseline = trackY + track.height() + 16 + muf.ascent();
    p.setPen(kTextTertiary);
    p.setFont(metaMono);
    p.drawText(QPointF(footer.left()+padX, metaBaseline), QString("Version 2.0.1 · Qt %1").arg(qVersion()));
    p.setFont(metaUi);
    const QString credit = "Forked from Xara Xtreme · © 2026 XarQ";
    p.drawText(QPointF(footer.right()-padX - muf.horizontalAdvance(credit), metaBaseline), credit);
}

} // namespace xara
