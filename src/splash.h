// splash.h — XarQ application launch / splash screen.
//
// A frameless, translucent, always-on-top window shown briefly while the app
// "boots", matching the DS `ui_kits/splash` kit: a 720×460 rounded graphite
// panel with a brand glow + faint grid, the XarQ mark in a raised tile, the
// accent-X wordmark + tagline, and an animated load-progress footer that steps
// through boot statuses and ends on a green "Ready".
//
// Purely cosmetic and shown ONLY on a genuine interactive launch — every
// headless/scripted/screenshot path (see main.cpp gating + MainWindow::m_showSplash)
// skips it, so --selftest and offscreen render/export are untouched.
#pragma once
#include <QWidget>
class QTimer;
class QPropertyAnimation;

namespace xara {

class LaunchSplash : public QWidget {
    Q_OBJECT
public:
    explicit LaunchSplash(QWidget* parent=nullptr);
    void start();                 // show centred + begin the load animation

signals:
    void finished();              // emitted once the animation (or an early dismiss) completes

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;   // click to skip
    void keyPressEvent(QKeyEvent*) override;        // any key to skip

private:
    void tick();                  // advance progress one step
    void finishNow();             // stop, fade out, then emit finished()

    double m_pct{6.0};
    int    m_step{0};
    bool   m_done{false};
    QTimer* m_timer{nullptr};
    QPropertyAnimation* m_fade{nullptr};
};

} // namespace xara
