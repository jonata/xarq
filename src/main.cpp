// main.cpp — XarQ: a modern Qt6 vector + photo + layout studio (the wx→Qt6 port of Xara Xtreme).
// Applies the XarQ design system (dark-first brushed-graphite theme, Montserrat + Ubuntu Mono,
// signal-blue accent) and the application shell. Supports a headless --shot mode for UI testing.
#include <QApplication>
#include <QMouseEvent>
#include <QElapsedTimer>
#include <QPalette>
#include <QStyleFactory>
#include <QFile>
#include <QTimer>
#include <QFontDatabase>
#include <QFont>
#include <QFontInfo>
#include <QFileInfo>
#include <QMap>
#include <QSet>
#include <QDir>
#include <QDirIterator>
#include <QStandardPaths>
#include <QIcon>
#include <QDialog>
#include <QPainter>
#include <QAction>
#include <memory>
#include <QScreen>
#include "dialogchrome.h"
#include <QProcessEnvironment>
#include <QMenu>
#include <QEvent>
#include <QKeyEvent>
#include <QVariant>
#include <QFileOpenEvent>
#include <QScreen>
#include <functional>
#include "mainwindow.h"
#include "splash.h"
#include "canvas.h"
#include "rasterextra.h"
#include "extcodecs.h"
#include "photoops.h"
#include "xar.h"
#include "svgimport.h"
#include "prepress.h"
#include <functional>
#include <cstdio>

static void applyTheme(QApplication& app) {
    app.setStyle(QStyleFactory::create("Fusion"));
    // XarQ design system — bundled Montserrat (UI/display) + Ubuntu Mono (numerics).
    for(const char* f : {"Montserrat-Regular","Montserrat-Medium","Montserrat-SemiBold","Montserrat-Bold","Montserrat-ExtraBold"})
        QFontDatabase::addApplicationFont(QString(":/xarq/assets/fonts/%1.ttf").arg(f));
    QFontDatabase::addApplicationFont(":/xarq/assets/fonts/UbuntuMono.ttf");
    { QFont uiFont("Montserrat"); uiFont.setPixelSize(12); uiFont.setStyleStrategy(QFont::PreferAntialias); app.setFont(uiFont); }

    // XarQ palette — Subtitld's slate ink scale + sky accent (#B8CEE0).
    QPalette p;
    const QColor chrome(0x0D,0x12,0x16), panel(0x1A,0x23,0x2B), raised(0x21,0x2D,0x37),
                 well(0x30,0x42,0x51), textStrong(0xF0,0xF6,0xFB), textBody(0xB8,0xCE,0xE0),
                 textSec(0x8E,0xA6,0xB9), textDis(0x56,0x6F,0x84), accent(0xB8,0xCE,0xE0);
    p.setColor(QPalette::Window, chrome);
    p.setColor(QPalette::WindowText, textBody);
    p.setColor(QPalette::Base, well);
    p.setColor(QPalette::AlternateBase, panel);
    p.setColor(QPalette::Text, textBody);
    p.setColor(QPalette::BrightText, textStrong);
    p.setColor(QPalette::Button, raised);
    p.setColor(QPalette::ButtonText, textBody);
    p.setColor(QPalette::ToolTipBase, QColor(0x1A,0x23,0x2B));
    p.setColor(QPalette::ToolTipText, Qt::white);
    p.setColor(QPalette::Highlight, accent);
    p.setColor(QPalette::HighlightedText, QColor(0x30,0x42,0x51));
    p.setColor(QPalette::Link, QColor(0xC8,0xDB,0xE9));
    p.setColor(QPalette::PlaceholderText, textSec);
    p.setColor(QPalette::Disabled, QPalette::Text, textDis);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, textDis);
    p.setColor(QPalette::Disabled, QPalette::WindowText, textDis);
    app.setPalette(p);
    app.setWindowIcon(QIcon(":/xarq/assets/logo/xarq-appicon.svg"));

    app.setStyleSheet(R"QSS(
        /* ===== XarQ, in Subtitld's language — near-black window gradient, slate ink scale, sky accent, 2px corners, no shadows ===== */
        * { font-family: "Montserrat","DejaVu Sans","Segoe UI",sans-serif; font-size: 12px; outline: none; }
        QMainWindow { background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #07090B, stop:1 #1A232B); }
        QWidget { background: #1A232B; color: #B8CEE0; }
        /* Content widgets are transparent so they show their PARENT surface (web-CSS behaviour), not the
           blanket QWidget colour — otherwise labels/checkboxes band against bars, dialogs, panels, etc. */
        QLabel, QCheckBox, QRadioButton, QGroupBox::indicator, QAbstractButton#link, QSlider { background: transparent; }
        QToolTip { background: #E51A232B; color: white; border: none; padding: 3px 6px; font-size: 10px; }

        /* Top action bar — the slate band Subtitld's player controls sit on */
        QToolBar#topbar { background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #2E3E4C, stop:1 #1E2A34); border: none; padding: 5px 8px; spacing: 3px; }
        QToolBar#topbar QToolButton { padding: 6px; border-radius: 2px; color:#B8CEE0; }
        QToolBar#topbar QToolButton:hover { background: #3E5363; }
        QToolBar#topbar QToolButton:pressed { background: #2E3E4C; }
        QToolBar QToolButton { color:#B8CEE0; padding:4px 8px; border-radius:2px; }
        QLabel#appTitle { font-weight: 800; font-size:17px; color: #F0F6FB; letter-spacing: -0.3px; background:transparent; }

        /* Tool rail — Subtitld's navigation rail: 50% black, and the active item carries a sky bar on its left edge */
        QWidget#railGlass { background: rgba(7,9,11,0.86); border:none; border-radius:2px; }
        QScrollArea#railScroll { background:transparent; border:none; }
        QFrame#toolrail { background: transparent; border:none; }
        QFrame#railsep { max-height:1px; margin:5px 10px; background:rgba(255,255,255,0.06); border:none; }
        #toolrail QToolButton { border-radius:0; background: transparent; color:#8EA6B9; padding:6px; border:none; border-left:3px solid transparent; }
        #toolrail QToolButton:hover { background: rgba(184,206,224,0.02); border-left:3px solid rgba(184,206,224,0.25); color:#B8CEE0; }
        #toolrail QToolButton:checked { background: rgba(255,255,255,0.05); border-left:3px solid #C8DBE9; color:#C8DBE9; }

        /* Docks / panels — solid graphite, uppercase tracked title */
        QDockWidget { color:#8EA6B9; titlebar-close-icon:none; }
        QDockWidget::title { background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #1E2A34, stop:1 #151D24); padding: 8px 12px; border-bottom: 1px solid rgba(255,255,255,0.06); font-weight:700; font-size:11px; letter-spacing:1.4px; color:#8EA6B9; }
        QDockWidget > QWidget { background: #1A232B; }
        QLabel#panelTitle { font-size: 11px; font-weight: 700; color: #8EA6B9; letter-spacing:1.4px; text-transform:uppercase; padding-bottom: 2px; }

        /* DS RightDock — floats over the canvas; translucent glass backdrop so the artwork shows through
           (the section cards stay solid). Left hairline separates it from the canvas. */
        QWidget#rightPanel { background: rgba(26,35,43,0.94); border-left:1px solid rgba(255,255,255,0.04); }
        QWidget#rpPage { background:transparent; }
        QScrollArea#rpScroll { background:transparent; border:none; }
        QTabWidget#rpTabs::pane { border:none; background:transparent; }
        #rpTabs QTabBar { qproperty-drawBase:0; background:transparent; }
        #rpTabs QTabBar::tab { background:transparent; color:#80B8CEE0; padding:7px 10px 6px 10px; border:none; border-bottom:1px solid transparent; font-size:9px; font-weight:700; text-transform:uppercase; }
        #rpTabs QTabBar::tab:selected { background: qlineargradient(x1:0,y1:1,x2:0,y2:0, stop:0 #1AC0D4E5, stop:1 #00C0D4E5); color:#C8DBE9; border-bottom:1px solid #B8CEE0; }
        #rpTabs QTabBar::tab:hover:!selected { color:#B8CEE0; }
        /* Collapsible section card (DS Panel) — draggable to reorder / detach to a floating panel */
        QWidget#dsSection { background:#212D37; border:none; border-radius:2px; }
        QWidget#dsSectionHdr { background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #3E5363, stop:1 #304251); border-top-left-radius:2px; border-top-right-radius:2px; border-bottom:none; }
        QLabel#dsGrip { color:#566F84; font-size:10px; background:transparent; }
        QLabel#dsChevron { color:#8EA6B9; font-size:11px; font-weight:700; background:transparent; }
        QLabel#dsSectionTitle { color:#B8CEE0; font-size:9px; font-weight:700; letter-spacing:0.4px; text-transform:uppercase; background:transparent; }
        QToolButton#dsAct { background:transparent; border:none; border-radius:2px; padding:2px; }
        QToolButton#dsAct:hover { background:rgba(255,255,255,0.08); }
        QWidget#floatPanel { background:transparent; }
        QWidget#panelGrip { background:transparent; }
        QWidget#panelGrip:hover { background:rgba(184,206,224,0.45); }

        QGroupBox { border-image:url(:/xarq/assets/chrome/groupbox.svg) 3 3 3 3 stretch stretch; border-width:3px; margin-top:11px; padding:5px 6px 5px 6px; background:transparent; font-size:9px; font-weight:700; text-transform:uppercase; color:#B8CEE0; }
        QGroupBox::title { subcontrol-origin: margin; left: 4px; padding: 0 2px; color:#B8CEE0; font-weight:700; font-size:9px; text-transform:uppercase; }
        /* Right-dock inspectors (Properties / Layers / Colour) — compact: smaller type, tighter group
           padding + input wells (DS RightDock density) */
        QWidget#propsHost *, QWidget#layersHost *, QWidget#colourHost * { font-size: 11px; }
        QWidget#propsHost QGroupBox { margin-top: 9px; padding: 4px 6px 5px 6px; }
        QWidget#propsHost QComboBox, QWidget#propsHost QSpinBox, QWidget#propsHost QDoubleSpinBox, QWidget#propsHost QLineEdit { padding:2px 6px; min-height:15px; }
        QWidget#layersHost QListWidget::item { padding:2px 4px; }

        /* Inputs — flat slate fields, sky text, no outline */
        QComboBox, QSpinBox, QDoubleSpinBox, QLineEdit { background:#304251; border:1px solid transparent; border-radius:2px; padding:3px 6px; color:#B8CEE0; min-height:17px; selection-background-color:#B8CEE0; selection-color:#304251; }
        QSpinBox, QDoubleSpinBox { font-family:"Ubuntu Mono","DejaVu Sans Mono",monospace; }
        QComboBox:hover, QSpinBox:hover, QDoubleSpinBox:hover, QLineEdit:hover { background:#3E5363; }
        QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus, QLineEdit:focus { border:1px solid #8DB0C9; }
        QComboBox:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled, QLineEdit:disabled { background:#803E5363; color:#566F84; }
        QComboBox::drop-down { background:#3E5363; border:none; border-top-right-radius:2px; border-bottom-right-radius:2px; subcontrol-origin:padding; subcontrol-position:top right; width:18px; }
        QComboBox::down-arrow { image:url(:/xarq/assets/chrome/combo-arrow.svg); width:16px; height:16px; }
        /* Dropdown popup — the popup window is made translucent (RoundPopups filter) so the rounded
           corners show through instead of a square window background. */
        QComboBox QAbstractItemView { background:#304251; border:none; border-radius:4px; padding:3px; color:#B8CEE0; selection-background-color:#566F84; selection-color:#FFFFFF; outline:none; }
        QComboBox QAbstractItemView::item { padding:4px 8px; border-radius:2px; min-height:18px; }
        QSpinBox::up-button, QDoubleSpinBox::up-button, QSpinBox::down-button, QDoubleSpinBox::down-button { width:15px; border:none; background:transparent; }
        QSpinBox::up-arrow, QDoubleSpinBox::up-arrow { image:url(:/xarq/assets/chrome/spin-up.svg); width:8px; height:4px; }
        QSpinBox::down-arrow, QDoubleSpinBox::down-arrow { image:url(:/xarq/assets/chrome/spin-down.svg); width:8px; height:4px; }

        /* Buttons — Subtitld's families: slate by default; sky for the affirmative action. 2px corners, bold caps. */
        QPushButton { background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #4A5F70, stop:1 #3E5363); border:none; border-radius:2px; padding:6px 12px; color:#B8CEE0; min-height:14px; font-size:11px; font-weight:700; text-transform:uppercase; }
        QPushButton:hover { background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #566F84, stop:1 #4A5F70); }
        QPushButton:pressed { background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #2E3E4C, stop:1 #3A4D5D); }
        QPushButton:default { background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #C8DBE9, stop:1 #B8CEE0); color:#304251; }
        QPushButton:default:hover { background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #D7E4EF, stop:1 #C8DBE9); }
        QPushButton:default:pressed { background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #8DB0C9, stop:1 #5F7B8D); }
        QPushButton:disabled { color:#66B8CEE0; background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #8040566A, stop:1 #803E5363); }
        /* The optional Standard toolbar's Export */
        QPushButton#ctaExport { background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #C8DBE9, stop:1 #B8CEE0); border:none; border-radius:2px; color:#304251; padding:5px 14px; min-height:18px; }
        QPushButton#ctaExport:hover { background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #D7E4EF, stop:1 #C8DBE9); }
        QPushButton#ctaExport:pressed { background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #8DB0C9, stop:1 #5F7B8D); }

        QToolButton { border-radius:2px; color:#B8CEE0; }
        QToolButton:hover { background:#3E5363; }
        QToolButton:checked { background:rgba(184,206,224,0.18); color:#C8DBE9; }

        QSlider::groove:horizontal { height:6px; background:#803E5363; border-radius:2px; }
        QSlider::handle:horizontal { width:10px; margin:-4px 0; border-radius:2px; background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #C8DBE9, stop:1 #B8CEE0); }
        QSlider::handle:horizontal:hover { background:#D7E4EF; }
        QSlider::sub-page:horizontal { background:#566F84; border-radius:2px; }

        QCheckBox { spacing:7px; }
        QCheckBox::indicator, QGroupBox::indicator { width:16px; height:16px; }
        QCheckBox::indicator:checked, QGroupBox::indicator:checked { image:url(:/xarq/assets/chrome/check-on.svg); }
        QCheckBox::indicator:checked:hover, QGroupBox::indicator:checked:hover { image:url(:/xarq/assets/chrome/check-on-hover.svg); }
        QCheckBox::indicator:unchecked, QGroupBox::indicator:unchecked { image:url(:/xarq/assets/chrome/check-off.svg); }
        QCheckBox::indicator:unchecked:hover, QGroupBox::indicator:unchecked:hover { image:url(:/xarq/assets/chrome/check-off-hover.svg); }
        QCheckBox::indicator:disabled, QGroupBox::indicator:disabled { image:url(:/xarq/assets/chrome/check-disabled.svg); }
        QRadioButton::indicator { width:14px; height:14px; border-radius:7px; border:1px solid #566F84; background:#304251; }
        QRadioButton::indicator:hover { background:#3E5363; }
        QRadioButton::indicator:checked { background:#B8CEE0; border-color:#B8CEE0; }

        QListWidget, QTreeWidget, QTableWidget { background:#141C23; border:none; border-radius:2px; padding:2px; alternate-background-color:#1A232B; }
        QListWidget::item, QTreeWidget::item { padding:3px 7px; border-radius:2px; }
        QListWidget::item:selected, QTreeWidget::item:selected { background:#566F84; color:#FFFFFF; }
        QListWidget::item:hover, QTreeWidget::item:hover { background:#3E5363; }
        QHeaderView::section { background:#304251; color:#B8CEE0; border:none; padding:5px 8px; font-weight:700; font-size:9px; text-transform:uppercase; }

        QScrollArea { border:none; background:#1A232B; }
        QScrollBar:vertical { background:transparent; width:8px; margin:2px 0; border-radius:4px; }
        QScrollBar::handle:vertical { background:rgba(136,136,136,0.5); border-radius:4px; min-height:20px; }
        QScrollBar::handle:vertical:hover { background:rgba(170,170,170,0.7); }
        QScrollBar::add-line, QScrollBar::sub-line { height:0; width:0; }
        QScrollBar::add-page, QScrollBar::sub-page { background:transparent; }
        QScrollBar:horizontal { background:transparent; height:8px; margin:0 2px; border-radius:4px; }
        QScrollBar::handle:horizontal { background:rgba(136,136,136,0.5); border-radius:4px; min-width:20px; }
        QScrollBar::handle:horizontal:hover { background:rgba(170,170,170,0.7); }
        /* Slim, unobtrusive scrollbar inside the floating glass tool rail */
        QScrollArea#railScroll QScrollBar:vertical { width:5px; margin:1px; }
        QScrollArea#railScroll QScrollBar::handle:vertical { background:rgba(255,255,255,0.14); border-radius:2px; min-height:24px; }
        QScrollArea#railScroll QScrollBar::handle:vertical:hover { background:rgba(255,255,255,0.24); }

        /* Frameless title bar, after Subtitld's: Save + the curved Export flush in the corner | title⇄menu hover-swap |
           wordmark on a faint curved tab + window controls. The curves are 9-sliced sprites. */
        QWidget#titleBar { background:#07090B; border:none; }
        QWidget#titleCenter, QWidget#titleBar QWidget { background:transparent; }
        /* ...but a QMenu popped from the embedded menu bar is a QObject-descendant of #titleBar, so the
           blanket rule above (an ID selector, higher specificity than a plain `QMenu`) would win and make
           the whole translucent popup see-through. Re-assert the menu body at equal specificity, after it. */
        QWidget#titleBar QMenu { background:#304251; }
        QToolButton#titleSave { background:transparent; border-radius:0; border-width:0 1px 2px 0; padding:0 0 0 10px; border-image:url(:/xarq/assets/chrome/save-normal.svg) 1 1 2 1 stretch stretch; }
        QToolButton#titleSave:hover { background:transparent; border-image:url(:/xarq/assets/chrome/save-hover.svg) 1 1 2 1 stretch stretch; }
        QToolButton#titleSave:pressed { border-image:url(:/xarq/assets/chrome/save-pressed.svg) 1 1 2 1 stretch stretch; }
        QToolButton#titleSave[dirty="true"] { border-image:url(:/xarq/assets/chrome/save-unsaved-normal.svg) 1 1 2 1 stretch stretch; }
        QToolButton#titleSave[dirty="true"]:hover { border-image:url(:/xarq/assets/chrome/save-unsaved-hover.svg) 1 1 2 1 stretch stretch; }
        QToolButton#titleSave[dirty="true"]:pressed { border-image:url(:/xarq/assets/chrome/save-unsaved-pressed.svg) 1 1 2 1 stretch stretch; }
        QToolButton#titleExport { background:transparent; border-radius:0; border-width:0 34px 1px 0; padding:0; border-image:url(:/xarq/assets/chrome/export-normal.svg) 0 34 1 0 stretch stretch; }
        QToolButton#titleExport:hover { background:transparent; border-image:url(:/xarq/assets/chrome/export-hover.svg) 0 34 1 0 stretch stretch; }
        QToolButton#titleExport:pressed { border-image:url(:/xarq/assets/chrome/export-pressed.svg) 0 34 1 0 stretch stretch; }
        QToolButton#winBtn { background:transparent; border:none; border-radius:0; }
        QToolButton#winBtn:hover { background:rgba(255,255,255,0.024); }
        QToolButton#winBtn:pressed { background:rgba(255,255,255,0.047); }
        QLabel#titlePath  { color:rgba(255,255,255,0.45); font-size:9px; background:transparent; }
        QLabel#titleName  { color:rgba(255,255,255,0.85); font-size:11px; font-weight:700; background:transparent; }
        QLabel#brandTab   { color:rgba(255,255,255,0.32); font-size:12px; padding:0 10px 0 0; border-width:0 1px 1px 36px; border-image:url(:/xarq/assets/chrome/brand-tab.svg) 0 1 1 36 stretch stretch; }
        /* Floating canvas toolbar — shaped like Subtitld's timecode well: both sides curved, flaring to the bottom */
        QWidget#zoomPill { background:transparent; border-width:0 20px 0 20px; border-image:url(:/xarq/assets/chrome/pill.svg) 0 20 0 20 stretch stretch; }
        QToolButton#pillBtn { background:transparent; border:none; border-radius:2px; padding:4px; }
        QToolButton#pillBtn:hover { background: rgba(255,255,255,0.08); }
        QToolButton#pillBtn:checked { background: rgba(184,206,224,0.22); }
        QFrame#pillSep { max-width:1px; background: rgba(255,255,255,0.10); margin:8px 3px; border:none; }
        QMenuBar, QMenuBar#embeddedMenu { background:transparent; border:none; padding:0; }
        QMenuBar::item { padding:4px 10px; background:transparent; color:#B8CEE0; border-radius:2px; font-size:11px; font-weight:700; }
        QMenuBar::item:selected { background:#3E5363; color:#FFFFFF; border-radius:2px; }
        QMenuBar::item:pressed { background:#304251; color:#FFFFFF; border-radius:2px; }
        QMenu { background:#304251; border:none; border-radius:4px; padding:3px; }
        QMenu::item { padding:4px 24px 4px 14px; border-radius:2px; color:#B8CEE0; font-size:11px; }
        QMenu::item:selected { background:#566F84; color:#FFFFFF; }
        QMenu::item:disabled { color:#66B8CEE0; }
        QMenu::separator { height:1px; background:rgba(255,255,255,0.08); margin:4px 8px; }

        /* Document tabs — the Export button's S-curve on both sides, standing on the canvas */
        QTabBar#docTabBar { background:#0D1216; qproperty-drawBase:0; }
        QTabBar#docTabBar::tab { background:transparent; color:#8EA6B9; font-size:11px; font-weight:600; text-transform:none; padding:0 2px; margin:4px 0 0 0; min-width:84px; height:28px; border-width:0 18px 0 18px; border-image:url(:/xarq/assets/chrome/tab-normal.svg) 0 18 0 18 stretch stretch; }
        QTabBar#docTabBar::tab:selected { color:#FFFFFF; border-image:url(:/xarq/assets/chrome/tab-selected.svg) 0 18 0 18 stretch stretch; }
        QTabBar#docTabBar::tab:hover:!selected { color:#B8CEE0; border-image:url(:/xarq/assets/chrome/tab-hover.svg) 0 18 0 18 stretch stretch; }
        QWidget#docTabCloseBox { background:transparent; }
        QToolButton#docTabClose { background:transparent; border:none; border-radius:2px; padding:0; }
        QToolButton#docTabClose:hover { background:rgba(255,255,255,0.14); }
        QTabWidget::pane { background:transparent; border:none; border-top:1px solid #1AC0D4E5; margin-top:-1px; }
        QTabBar { background:transparent; border:none; }
        QTabBar::tab { background:transparent; color:#80B8CEE0; font-size:9px; font-weight:700; text-transform:uppercase; padding:7px 10px 6px 10px; border:none; border-bottom:1px solid transparent; }
        QTabBar::tab:hover { color:#B8CEE0; }
        QTabBar::tab:selected { background: qlineargradient(x1:0,y1:1,x2:0,y2:0, stop:0 #1AC0D4E5, stop:1 #00C0D4E5); color:#C8DBE9; border-bottom:1px solid #B8CEE0; }

        /* DS StatusBar — 11px, tertiary base; accent tool label, mono numerics, green Saved */
        QStatusBar { background:#07090B; border:none; color:#7A93A7; }
        QStatusBar QLabel { color:#7A93A7; font-size:11px; padding:0 8px; background:transparent; }
        QStatusBar QLabel#sTool { color:#C8DBE9; font-weight:600; }
        QStatusBar QLabel#sNum  { font-family:"Ubuntu Mono","DejaVu Sans Mono",monospace; color:#8EA6B9; }
        QStatusBar QLabel#sHint { color:#566F84; }
        QStatusBar QLabel#sMeta { color:#7A93A7; }
        QStatusBar QWidget#sSpacer { background:transparent; }
        QStatusBar QLabel#sSaved[tone="ok"]      { color:#8EA6B9; font-weight:600; }
        QStatusBar QLabel#sSaved[tone="warning"] { color:#5DE845; font-weight:600; }
        QStatusBar::item { border:none; }

        QDialog { background:#1A232B; }
        QDialog QLabel { color:#B8CEE0; }

        /* Dialog chrome, after Subtitld's SimpleDialog (DialogChrome, dialogchrome.cpp): one gradient top to bottom
           with 4px corners, the title on it, the close button in a slanted notch, Cancel as text on the left of the
           footer and the main button on a sky tab at its right. */
        QDialog[xqChromed="true"] { background:transparent; }
        QWidget#dialogFrame { background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #1E2833, stop:1 #F23A4D5D); border:1px solid #1A232B; border-radius:4px; }
        QWidget#dialogTitle, QWidget#dialogContent, QWidget#dialogBottom { background:transparent; }
        QWidget#dialogFrame .QWidget, QWidget#dialogFrame .QFrame, QWidget#dialogFrame QStackedWidget { background:transparent; }   /* plain containers let the gradient through */
        QDialog[xqChromed="true"] QSizeGrip { image:none; background:transparent; width:12px; height:12px; }
        QLabel#dialogTitleLabel { background:transparent; padding-left:15px; qproperty-indent:0; font-size:11px; font-weight:700; text-transform:uppercase; color:#F0F6FB; }
        QWidget#dialogTitleNotch { border-image:url(:/xarq/assets/chrome/dialog-notch.svg) 0 4 1 36 stretch stretch; border-width:0 4px 1px 36px; background:transparent; }
        QWidget#dialogTitleNotch[hover="true"] { border-image:url(:/xarq/assets/chrome/dialog-notch-hover.svg) 0 4 1 36 stretch stretch; }
        QToolButton#dialogClose { background:transparent; border:none; padding:0; }
        QWidget#dialogBottomLeft { border-image:url(:/xarq/assets/chrome/dialog-bottom.svg) 0 32 1 5 stretch stretch; border-width:0 0 1px 5px; background:transparent; }
        QWidget#dialogBottomRight { border-image:url(:/xarq/assets/chrome/dialog-bottom-right.svg) 5 5 5 24 stretch stretch; border-width:5px 5px 5px 24px; background:transparent; }
        QWidget#dialogBottom QPushButton { padding:8px 12px; font-size:11px; font-weight:700; text-transform:uppercase; border:none; border-radius:0; background:transparent; min-height:15px; }
        QWidget#dialogBottom QPushButton[footer="left"] { color:#B8CEE0; padding-left:15px; border-top-right-radius:4px; border-bottom-left-radius:4px; }
        QWidget#dialogBottom QPushButton[footer="left"]:hover { background:#26B8CEE0; }
        QWidget#dialogBottom QPushButton[footer="left"]:disabled { color:#566F84; }
        QWidget#dialogBottom QPushButton[footer="tab"] { color:#1E2B36; }
        QWidget#dialogBottom QPushButton[footer="tab"]:hover { color:#0D1319; background:transparent; }
        QWidget#dialogBottom QPushButton[footer="tab"]:disabled { color:#7A93A7; }
        QLabel#printPreview { background:#07090B; border:none; border-radius:2px; }
        QDialog#printDialog QGroupBox { min-width:300px; }

        /* InfoBar — the slate band; the tool name is a curved tag flush with the window edge, echoing Export above it */
        QToolBar#infobarTb { background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #2E3E4C, stop:1 #1E2A34); border:none; padding:0; spacing:0; }
        QWidget#infobar { background:transparent; }
        QLabel#infoTool { font-weight:700; font-size:11px; text-transform:uppercase; color:#F0F6FB; padding:0 4px 0 12px; border-width:0 30px 0 0; border-image:url(:/xarq/assets/chrome/tooltag.svg) 0 30 0 0 stretch stretch; }
        QLabel#infoLbl  { color:#8EA6B9; padding-left:3px; }
        QLabel#infoHint { color:#7A93A7; font-style:italic; }
        QFrame#infoSep  { background:rgba(255,255,255,0.08); max-width:1px; margin:5px 5px; border:none; }
        QWidget#infobar QComboBox, QWidget#infobar QSpinBox, QWidget#infobar QDoubleSpinBox, QWidget#infobar QLineEdit { background:#3E5363; padding:2px 6px; min-height:18px; }
        QWidget#infobar QComboBox:hover, QWidget#infobar QSpinBox:hover, QWidget#infobar QDoubleSpinBox:hover, QWidget#infobar QLineEdit:hover { background:#4A5F70; }
        QWidget#infobar QToolButton { padding:3px 10px; border:none; background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #4A5F70, stop:1 #3E5363); border-radius:2px; font-weight:700; font-size:11px; }
        QWidget#infobar QToolButton:hover { background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #566F84, stop:1 #4A5F70); }
        QWidget#infobar QToolButton:checked { background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #C8DBE9, stop:1 #B8CEE0); color:#304251; }

        /* Colour Line — bottom palette strip */
        QToolBar#colourTb { background:#07090B; border:none; padding:0; spacing:0; }
        QWidget#colourline { background:#07090B; }
    )QSS");
}

// Makes popup windows (menu-bar menus, context menus, combobox dropdowns) translucent so their QSS
// border-radius renders with transparent corners instead of a square window backdrop. WA_TranslucentBackground
// must be set before the native window is created — QEvent::Polish fires before first show, so we catch it there.
class RoundPopups : public QObject {
public:
    using QObject::QObject;
protected:
    bool eventFilter(QObject* o, QEvent* e) override {
        if(e->type()==QEvent::Polish){
            if(auto* w=qobject_cast<QWidget*>(o)){
                if(qobject_cast<QMenu*>(w) || w->inherits("QComboBoxPrivateContainer")){
                    if(!w->testAttribute(Qt::WA_TranslucentBackground)){
                        w->setAttribute(Qt::WA_TranslucentBackground);
                        w->setWindowFlags(w->windowFlags() | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
                    }
                }
            }
        }
        return QObject::eventFilter(o,e);
    }
};

// XarQ's single-key tool shortcuts (V/N/M/E/P/B/L/F/T/G/Y/K…) are Qt::WindowShortcut QActions. Qt's
// ShortcutOverride mechanism lets a *focused* editable QLineEdit swallow a plain letter so typing wins —
// but a NON-editable QComboBox does NOT accept the override, so pressing e.g. "T" while a dropdown had
// focus fired the Text tool instead of doing a type-ahead / accepting the keystroke. Claim the
// ShortcutOverride ourselves for any *text-entry* control on a *bare* (no Ctrl/Alt/Meta) printable key, so
// a single-key shortcut can never steal a keystroke meant for a widget. Deliberately NOT item views
// (galleries/lists/trees): clicking a layer row and then pressing "T"/"V"/"E" should still switch tools the
// way vector-editor users expect, so those keep their single-key shortcuts. Modifier combos (Ctrl+Z/S/C…)
// and non-printable keys (Enter/Escape/arrows/F-keys) fall through so global shortcuts, dialogs and
// navigation keep working. The on-canvas text caret is handled separately by Canvas::event().
class ShortcutGuard : public QObject {
public:
    using QObject::QObject;
    static bool isInputControl(QWidget* w){
        if(!w) return false;
        // A read-only text field won't consume the key, so claiming it would only turn the tool shortcut
        // into a dead key — let those fall through (matches native QLineEdit/QTextEdit, which don't accept
        // the override when read-only). property() covers QLineEdit/QTextEdit/QPlainTextEdit/QAbstractSpinBox;
        // widgets without a readOnly property (QComboBox/QKeySequenceEdit) return an invalid variant.
        { const QVariant ro=w->property("readOnly"); if(ro.isValid() && ro.toBool()) return false; }
        // String-based inherits() so no widget headers are pulled into main.cpp; each base covers its
        // subclasses (QComboBox⊃QFontComboBox, QAbstractSpinBox⊃Q(Double)SpinBox/QDateTimeEdit).
        static const char* kinds[]={"QLineEdit","QComboBox","QAbstractSpinBox","QTextEdit",
                                    "QPlainTextEdit","QKeySequenceEdit"};
        for(const char* k : kinds) if(w->inherits(k)) return true;
        return false;
    }
protected:
    bool eventFilter(QObject* o, QEvent* e) override {
        if(e->type()==QEvent::ShortcutOverride){
            auto* ke=static_cast<QKeyEvent*>(e);
            if(!(ke->modifiers() & (Qt::ControlModifier|Qt::AltModifier|Qt::MetaModifier))){
                const QString t=ke->text();
                if(!t.isEmpty() && t.at(0).isPrint() && isInputControl(QApplication::focusWidget())){
                    ke->accept(); return true;   // suppress the single-key shortcut; the KeyPress still reaches the input
                }
            }
        }
        return QObject::eventFilter(o,e);
    }
};

// Load user-installed fonts (~/.local/share/fonts, ~/.fonts) directly via addApplicationFont, so a font
// installed AFTER the system fontconfig cache was last built is still available THIS session — no relaunch
// or `fc-cache` needed. Qt reads the fontconfig database once at launch, so without this a freshly-dropped
// font would be invisible until the next run. Only a file whose family is NOT already in the database is
// kept (one fontconfig already knows is loaded, checked, then dropped), so a font present the normal way is
// never double-registered and rendering is unchanged. GUI-only: never called on the headless export/save/
// shot/selftest paths, so the byte-identical corpus renders are untouched. Returns the families it added.
static QStringList loadUserApplicationFonts(){
    QSet<QString> known; for(const QString& f : QFontDatabase::families()) known.insert(f.toLower());
    QStringList added;
    QStringList dirs; dirs << QStandardPaths::writableLocation(QStandardPaths::FontsLocation)
                          << QDir::homePath()+QStringLiteral("/.fonts");
    QSet<QString> seenDir;
    for(const QString& d : dirs){
        if(d.isEmpty() || seenDir.contains(d) || !QDir(d).exists()) continue; seenDir.insert(d);
        QDirIterator it(d, {QStringLiteral("*.ttf"),QStringLiteral("*.otf"),QStringLiteral("*.ttc"),
                            QStringLiteral("*.otc"),QStringLiteral("*.pfb")}, QDir::Files, QDirIterator::Subdirectories);
        while(it.hasNext()){
            const QString path=it.next();
            const int id=QFontDatabase::addApplicationFont(path);
            if(id<0) continue;
            const QStringList fams=QFontDatabase::applicationFontFamilies(id);
            bool novel=false; for(const QString& fam: fams) if(!known.contains(fam.toLower())){ novel=true; break; }
            if(!novel){ QFontDatabase::removeApplicationFont(id); continue; }   // fontconfig already had it
            for(const QString& fam: fams){ if(!known.contains(fam.toLower())) added<<fam; known.insert(fam.toLower()); }
        }
    }
    return added;
}

// A request from the desktop to open a file: Tracker on Haiku, Finder on macOS.
class FileOpenRouter : public QObject {
public:
    explicit FileOpenRouter(std::function<void(const QString&)> open, QObject* parent): QObject(parent), m_open(std::move(open)) {}
protected:
    bool eventFilter(QObject* o, QEvent* e) override {
        if(e->type()!=QEvent::FileOpen) return QObject::eventFilter(o,e);
        m_open(static_cast<QFileOpenEvent*>(e)->file());
        return true;
    }
    std::function<void(const QString&)> m_open;
};

int main(int argc, char** argv) {
#ifdef Q_OS_HAIKU
    // Qt's raster fills hand spans to a thread pool and wait on a QSemaphore held on the stack. Without futexes
    // that semaphore is a mutex, and Haiku's pthread_mutex_unlock can still touch it after the waiter has woken
    // and freed it, which corrupts the heap at random. Filling on one thread is slower but safe.
    qputenv("QT_NO_GUI_THREADPOOL", "1");
#endif
    QApplication app(argc, argv);
    app.setApplicationName("XarQ");
#ifndef Q_OS_HAIKU   // Qt would append it to every title, and Haiku shows titles in the window tab
    app.setApplicationDisplayName("XarQ");
#endif
    app.setOrganizationName("XarQ");
    app.setApplicationVersion(XARQ_VERSION);
    applyTheme(app);
    app.installEventFilter(new xara::DialogChrome(&app));   // Subtitld's dialog chrome on every dialog
    app.installEventFilter(new RoundPopups(&app));   // translucent (rounded) menu + dropdown popups
    app.installEventFilter(new ShortcutGuard(&app)); // single-key tool shortcuts never steal a keystroke from a focused input

    // Files named on the command line or sent by the desktop wait here until the window is up.
    QStringList pendingFiles;
    xara::MainWindow* readyWindow=nullptr;
    app.installEventFilter(new FileOpenRouter([&](const QString& p){
        if(readyWindow) readyWindow->openPath(p); else pendingFiles<<p; }, &app));

    QString shotPath, exportPath, openPath, savePath, shotNodePath;
    for (int i=1;i<argc;++i){ QString a(argv[i]);
        if(a=="--shot"&&i+1<argc) shotPath=argv[++i];
        else if(a=="--export"&&i+1<argc) exportPath=argv[++i];
        else if(a=="--open"&&i+1<argc) openPath=argv[++i];
        else if(a=="--save"&&i+1<argc) savePath=argv[++i];
        else if(a=="--shotnode"&&i+1<argc) shotNodePath=argv[++i];
        else if(a=="--selftest"){ extern int runSelfTest(); return runSelfTest(); }
        else if(a=="--svgfontdump"&&i+1<argc){ // diagnostic: parse an SVG and report each Text shape's requested vs Qt-resolved font
            QString p=argv[++i]; QFile f(p);
            if(!f.open(QIODevice::ReadOnly)){ fprintf(stderr,"svgfontdump: cannot open %s\n", qPrintable(p)); return 1; }
            QString err; QVector<xara::Shape> shapes = xara::parseSvg(f.readAll(), &err, QFileInfo(p).absolutePath());
            if(!err.isEmpty()) fprintf(stderr,"svgfontdump: parse warning: %s\n", qPrintable(err));
            int nText=0; QMap<QString,int> reqTally, resTally;
            std::function<void(const QVector<xara::Shape>&)> walk=[&](const QVector<xara::Shape>& v){
                for(const auto& s: v){
                    if(s.type==xara::ShapeType::Group){ walk(s.children); continue; }
                    if(s.type!=xara::ShapeType::Text) continue;
                    ++nText;
                    const QString req=s.font.family();
                    QFontInfo fi(s.font);
                    const QString res=fi.family();
                    reqTally[req]++; resTally[res]++;
                    if(nText<=40)
                        fprintf(stderr,"  #%d req=\"%s\" (w=%d i=%d px=%d) -> resolved=\"%s\" style=\"%s\" exact=%d  text=\"%.24s\"\n",
                            nText, qPrintable(req), s.font.weight(), s.font.italic(), s.font.pixelSize(),
                            qPrintable(res), qPrintable(QFontInfo(s.font).styleName()), s.font.exactMatch(), qPrintable(QString(s.text).replace('\n',' ')));
                }
            };
            walk(shapes);
            fprintf(stderr,"svgfontdump %s: %d text shapes\n", qPrintable(p), nText);
            fprintf(stderr,"  REQUESTED families: "); for(auto it=reqTally.begin();it!=reqTally.end();++it) fprintf(stderr,"[%s×%d] ", qPrintable(it.key()), it.value()); fprintf(stderr,"\n");
            fprintf(stderr,"  RESOLVED families: "); for(auto it=resTally.begin();it!=resTally.end();++it) fprintf(stderr,"[%s×%d] ", qPrintable(it.key()), it.value()); fprintf(stderr,"\n");
            return 0; }
        else if(a=="--xarinfo"&&i+1<argc){ // diagnostic: load a .xar and report what was read
            QString p=argv[++i]; xara::Document d;
            bool ok=xara::loadXar(d,p);
            std::function<int(const QVector<xara::Shape>&)> cnt=[&](const QVector<xara::Shape>& v){ int n=0; for(auto&s:v){ n++; if(s.type==xara::ShapeType::Group) n+=cnt(s.children); } return n; };
            fprintf(stderr,"%s: ok=%d shapes=%d (deep=%d) page=%.0fx%.0f\n",
                qPrintable(p), ok, (int)d.shapes.size(), ok?cnt(d.shapes):0, d.pageSize.width(), d.pageSize.height());
            return ok?0:1; }
        else if(a=="--import-check"&&i+2<argc){ // dev: decode an image the way importImage does, write a PNG, print WxH (for real-file testing)
            QString in=argv[++i], out=argv[++i];
            QImage img; QString via="qt";
            // Whole-slide formats prefer OpenSlide (mirrors importImage) so a real gigapixel base isn't read by Qt.
            { static const char* const sx[]={".svs",".ndpi",".scn",".mrxs",".vms",".vmu",".svslide",".bif"};
              for(const char* e : sx) if(in.endsWith(QString::fromLatin1(e),Qt::CaseInsensitive)){ img=xara::ExtCodec::decode(in); via="extcodec"; break; } }
            if(img.isNull()){ img=QImage(in); via="qt"; }     // Qt plugins first (mirrors importImage)
            if(img.isNull()){ img=xara::RasterExtra::decode(in); via="rasterextra"; }
            if(img.isNull()){ img=xara::ExtCodec::decode(in);    via="extcodec"; }
            if(img.isNull() && (in.endsWith(".psd",Qt::CaseInsensitive)||in.endsWith(".psb",Qt::CaseInsensitive))){
                QFile pf(in); if(pf.open(QIODevice::ReadOnly)){ img=xara::importPsd(pf.readAll()); via="psd"; } }
            if(img.isNull()){ fprintf(stderr,"import-check FAIL: %s could not be decoded\n", qPrintable(in)); return 1; }
            bool ok=img.convertToFormat(QImage::Format_ARGB32).save(out,"PNG");
            fprintf(stdout,"%dx%d via=%s saved=%d %s\n", img.width(), img.height(), qPrintable(via), ok?1:0, qPrintable(out));
            return ok?0:1; }
        else if(a=="--import-vec"&&i+1<argc){ // dev: import a vector/page file via dropImportByExtension, report shape/page counts
            QString in=argv[++i];
            QString outPng; if(i+1<argc && argv[i+1][0]!='-') outPng=argv[++i];   // optional: render the imported page to a PNG
            xara::MainWindow w; w.setShowWelcome(false); w.resize(1200,800); w.show(); QApplication::processEvents();
            xara::Canvas* c=w.canvas(); c->newDocument();
            const bool ok=c->dropImportByExtension(in);
            fprintf(stdout,"import-vec %s: ok=%d shapes=%d pages=%d pagesize=%.1fx%.1f\n", qPrintable(in), ok?1:0,
                    (int)c->doc().shapes.size(), c->doc().pageCount,
                    c->doc().pageSize.width(), c->doc().pageSize.height());
            const auto& sh=c->doc().shapes;                                // dump first/last bitmap fills for content checks
            if(!sh.isEmpty() && sh.first().fill.type==xara::FillType::Bitmap && !sh.first().fill.image.isNull())
                sh.first().fill.image.save(in+".f0.png");
            if(sh.size()>=2 && sh.last().fill.type==xara::FillType::Bitmap && !sh.last().fill.image.isNull())
                sh.last().fill.image.save(in+".fN.png");
            if(ok && !outPng.isEmpty()){ QApplication::processEvents(); c->exportPng(outPng,2); }
            return ok?0:1; }
        else if(a=="--snapbench"&&i+1<argc){ // diagnostic: time one move-frame's snapAdjust on a real file (move ONE object)
            QString p=argv[++i];
            xara::MainWindow w; w.setShowWelcome(false); w.resize(1400,900); w.show(); QApplication::processEvents();
            xara::Canvas* c=w.canvas(); c->newDocument();
            if(p.endsWith(".cdr",Qt::CaseInsensitive)) c->importCdr(p);
            else if(p.endsWith(".svg",Qt::CaseInsensitive)) c->importSvgFile(p);
            else c->load(p);
            QApplication::processEvents(); c->setSnap(true); c->zoomTo(1.0); QApplication::processEvents();
            const int n=c->doc().shapes.size();
            if(n<1){ fprintf(stderr,"snapbench: no shapes in %s\n", qPrintable(p)); return 1; }
            c->select(QVector<int>{ n/2 });   // move ONE object ⇒ the other n-1 are snap candidates (the real move-drag cost)
            const double ms=c->snapAdjustProbeMs(120);
            double proxyMs=0, fullMs=0; bool ok=c->dragProxyProbe(20, proxyMs, fullMs);
            fprintf(stderr,"snapbench %s: shapes=%d  snapAdjust=%.3f  proxyPaint=%.3f  fullPaint=%.3f ms/frame (proxyOK=%d)\n",
                    qPrintable(p), n, ms, proxyMs, fullMs, ok?1:0);
            // also time the whole-selection case (select all, move together) — the classic heavy drag
            { QVector<int> allv; for(int i=0;i<n;++i) allv<<i; c->select(allv);
              double pm=0, fm=0; bool ok2=c->dragProxyProbe(20, pm, fm);
              fprintf(stderr,"  select-all move: proxyPaint=%.3f  fullPaint=%.3f ms/frame (proxyOK=%d)\n", pm, fm, ok2?1:0); }
            return 0; }
        else if(a=="--zoombench"&&i+1<argc){ // diagnostic: time one full canvas repaint at increasing zoom on a real file
            QString p=argv[++i];
            xara::MainWindow w; w.setShowWelcome(false); w.resize(1400,900); w.show(); QApplication::processEvents();
            xara::Canvas* c=w.canvas(); c->newDocument();
            if(p.endsWith(".cdr",Qt::CaseInsensitive)) c->importCdr(p);
            else if(p.endsWith(".svg",Qt::CaseInsensitive)) c->importSvgFile(p);
            else c->load(p);
            QApplication::processEvents();
            const int n=c->doc().shapes.size();
            if(n<1){ fprintf(stderr,"zoombench: no shapes in %s\n", qPrintable(p)); return 1; }
            c->zoomFit(); QApplication::processEvents();   // frame the doc so its (effect-bearing) content stays centred as we zoom in
            auto timeAt=[&](double z)->double{
                double best=1e18;
                for(int k=0;k<4;++k){
                    c->zoomTo(z + k*2e-4);            // tiny jitter ⇒ cache key changes ⇒ a fresh FULL repaint each sample (keeps the view centre)
                    c->benchForceCrisp();             // measure the crisp document render, not the interactive preview blit
                    QElapsedTimer t; t.start(); c->repaint(); const double ms=t.nsecsElapsed()/1e6;
                    QApplication::processEvents(); if(ms<best) best=ms;
                }
                return best;
            };
            fprintf(stderr,"zoombench %s (shapes=%d): full (crisp) canvas repaint at increasing zoom —\n", qPrintable(p), n);
            for(double z : {1.0, 4.0, 10.0, 20.0, 40.0})
                fprintf(stderr,"    zoom=%5.1fx   fullRepaint=%8.2f ms\n", z, timeAt(z));
            return 0; }
        else if(a=="--previewshot"&&i+2<argc){ // diagnostic: capture an interactive zoom-PREVIEW frame (scaled-bitmap, mid-gesture)
            QString p=argv[++i]; QString out=argv[++i];
            xara::MainWindow w; w.setShowWelcome(false); w.resize(1400,900); w.show(); QApplication::processEvents();
            xara::Canvas* c=w.canvas(); c->newDocument();
            if(p.endsWith(".cdr",Qt::CaseInsensitive)) c->importCdr(p);
            else if(p.endsWith(".svg",Qt::CaseInsensitive)) c->importSvgFile(p);
            else if(p.endsWith(".dwg",Qt::CaseInsensitive)) c->importVectorFile(p);
            else c->load(p);
            c->zoomFit(); QApplication::processEvents();   // builds the crisp cache (sets m_lastBackdropMs, so the preview arms)
            c->repaint();                                  // ensure a crisp cache exists before the gesture
            c->zoomIn(); c->zoomIn();                      // two wheel-equivalent steps → arms the bitmap-preview path
            const QImage frame=c->grab().toImage();        // synchronous repaint → takes the preview branch
            frame.save(out);
            fprintf(stderr,"previewshot %s: preview=%d saved %s (%dx%d)\n", qPrintable(p), c->previewActiveForTest()?1:0, qPrintable(out), frame.width(), frame.height());
            return 0; }
        else if(a=="--groupshot"&&i+2<argc){ // diagnostic: capture the group-isolation ("enter group") fade overlay
            QString p=argv[++i]; QString out=argv[++i];
            xara::MainWindow w; w.setShowWelcome(false); w.resize(1400,900); w.show(); QApplication::processEvents();
            xara::Canvas* c=w.canvas(); c->newDocument();
            if(p.endsWith(".svg",Qt::CaseInsensitive)) c->importSvgFile(p);
            else if(p.endsWith(".cdr",Qt::CaseInsensitive)) c->importCdr(p);
            else c->load(p);
            c->zoomFit(); QApplication::processEvents();
            const int gi=c->firstTopLevelGroup();
            if(gi<0){ fprintf(stderr,"groupshot: no top-level group in %s\n", qPrintable(p)); return 1; }
            c->enterGroupForTest(gi); QApplication::processEvents();
            c->grab().toImage().save(out);
            fprintf(stderr,"groupshot %s: entered group #%d → %s\n", qPrintable(p), gi, qPrintable(out));
            return 0; }
        else if(a=="--dragbench"&&i+1<argc){ // faithfully simulate a real MOVE and a real SCALE drag (mouse events), timing each frame's repaint
            QString p=argv[++i];
            xara::MainWindow w; w.setShowWelcome(false); w.resize(1400,900); w.show(); QApplication::processEvents();
            xara::Canvas* c=w.canvas(); c->newDocument();
            if(p.endsWith(".cdr",Qt::CaseInsensitive)) c->importCdr(p);
            else if(p.endsWith(".svg",Qt::CaseInsensitive)) c->importSvgFile(p);
            else c->load(p);
            QApplication::processEvents(); c->setSnap(true); c->zoomTo(1.0); QApplication::processEvents();
            const int n=c->doc().shapes.size();
            if(n<1){ fprintf(stderr,"dragbench: no shapes in %s\n", qPrintable(p)); return 1; }
            const int si=n/2; const QRectF db=c->doc().shapes[si].docBounds();
            { QElapsedTimer t; t.start(); c->pushUndo("bench"); const double us=t.nsecsElapsed()/1e6;
              fprintf(stderr,"  pushUndo (undo snapshot, runs once at drag-start) = %.2f ms\n", us); }
            auto send=[&](QEvent::Type ty, QPointF lp, Qt::MouseButton b, Qt::MouseButtons bs){
                QMouseEvent ev(ty, lp, c->mapToGlobal(lp.toPoint()), b, bs, Qt::NoModifier); QApplication::sendEvent(c, &ev); };
            auto runDrag=[&](const char* name, QPointF startView, QPointF stepView){
                c->select(QVector<int>{ si }); QApplication::processEvents();
                send(QEvent::MouseButtonPress, startView, Qt::LeftButton, Qt::LeftButton);
                double sum=0, mx=0, mvSum=0; const int frames=12;
                for(int k=1;k<=frames;++k){
                    QElapsedTimer tm; tm.start();
                    send(QEvent::MouseMove, startView+stepView*k, Qt::NoButton, Qt::LeftButton);  // fires documentChanged slots synchronously
                    const double mvMs=tm.nsecsElapsed()/1e6;
                    QElapsedTimer t; t.start(); c->repaint(); const double pMs=t.nsecsElapsed()/1e6;
                    QApplication::processEvents();                                                // let debounced gallery/panel rebuilds fire
                    const double ms=mvMs+pMs; if(k>1){ sum+=ms; mvSum+=mvMs; if(ms>mx)mx=ms; } }
                send(QEvent::MouseButtonRelease, startView+stepView*frames, Qt::LeftButton, Qt::NoButton);
                fprintf(stderr,"  %-6s per-frame (move-event + repaint): mean=%.2f  max=%.2f ms/frame  [move-event slots alone: mean=%.2f]\n",
                        name, sum/(frames-1), mx, mvSum/(frames-1)); };
            fprintf(stderr,"dragbench %s: shapes=%d (dragging shape #%d)\n", qPrintable(p), n, si);
            runDrag("MOVE",  c->toView(db.center()),   QPointF(6,4));   // press on the shape body → move
            runDrag("SCALE", c->toView(db.topLeft()),  QPointF(6,4));   // press on the TL corner handle → scale
            return 0; }
        else if(!a.startsWith('-')) pendingFiles<<a; }

    // Is this a genuine interactive launch, or any headless / scripted / screenshot mode?
    // (A capture CLI flag, any XARA_* test/screenshot env var, or the offscreen platform ⇒ not interactive.)
    // Computed BEFORE building the (heavy) main window so the splash can cover the whole construction.
    bool anyXaraEnv=false;
    for(const QString& k : QProcessEnvironment::systemEnvironment().keys())
        if(k.startsWith("XARA_")){ anyXaraEnv=true; break; }
    const QString qpa = QApplication::platformName();
    const bool headlessQpa = (qpa=="offscreen" || qpa=="minimal" || qpa=="minimalegl" || qpa=="vnc");   // no visible window ⇒ never splash / modal
    const bool interactive =
        shotPath.isEmpty() && exportPath.isEmpty() && openPath.isEmpty() && savePath.isEmpty() && shotNodePath.isEmpty()
        && !anyXaraEnv && !headlessQpa;
    // Branded boot splash FIRST — shown + painted before the heavy MainWindow construction, so the user sees it
    // immediately (never a blank screen), and it's hidden before the modal welcome so it can't block clicks.
    xara::LaunchSplash* splash=nullptr;
    if(interactive){ splash=new xara::LaunchSplash; splash->show(); splash->raise(); app.processEvents(); splash->start(); }

    // Pick up user-installed fonts the system font cache may not carry yet — BEFORE the UI (font pickers,
    // galleries) is built and BEFORE any document opens, so a freshly-installed font resolves this session
    // with no relaunch/fc-cache. GUI only: skipped on every headless output path (export/save/shot), which
    // keeps the byte-identical corpus renders untouched (--selftest already returned during arg parsing).
    const bool headlessOutput = !exportPath.isEmpty() || !savePath.isEmpty() || !shotPath.isEmpty() || !shotNodePath.isEmpty();
    if(!headlessOutput){ const QStringList af=loadUserApplicationFonts();
        if(!af.isEmpty()) qInfo("XarQ: loaded %d user font family(ies) not in the system font cache: %s", (int)af.size(), qUtf8Printable(af.join(", "))); }

    xara::MainWindow w;   // heavy construction — the splash above stays up covering it
    // Non-interactive ⇒ suppress the launch dialogs (recovery + the *modal* welcome screen, which would
    // otherwise block the event loop and hang every scripted grab).
    if(!interactive) w.setShowWelcome(false);
    w.setDeferStartup(interactive);   // the splash drives recovery+welcome once it fades
    if(!openPath.isEmpty()){ if(openPath.endsWith(".svg",Qt::CaseInsensitive)){ w.canvas()->newDocument(); w.canvas()->importSvgFile(openPath); } else if(openPath.endsWith(".cdr",Qt::CaseInsensitive)){ w.canvas()->newDocument(); w.canvas()->importCdr(openPath); } else if(openPath.endsWith(".dwg",Qt::CaseInsensitive)){ w.canvas()->newDocument(); w.canvas()->importVectorFile(openPath); } else w.canvas()->load(openPath); }

    if (!savePath.isEmpty()) return w.canvas()->save(savePath)?0:2;   // headless save self-test
    if (!exportPath.isEmpty()) {   // headless export self-test
        bool ok = exportPath.endsWith(".svg") ? w.canvas()->exportSvg(exportPath)
                : exportPath.endsWith(".pdf") ? w.canvas()->exportPdf(exportPath)
                : (exportPath.endsWith(".html")||exportPath.endsWith(".htm")) ? w.canvas()->exportHtml(exportPath)
                : exportPath.endsWith(".gif") ? w.canvas()->exportGif(exportPath)
                : w.canvas()->exportPng(exportPath,2);
        return ok?0:2;
    }
    w.resize(1500,950);
    if (interactive) {
        if(QScreen* screen=QApplication::primaryScreen()){
            QRect room=screen->availableGeometry();
#ifdef Q_OS_HAIKU
            room.adjust(5,29,-5,-5);   // Qt places the content, so leave the yellow tab and window border on screen
#endif
            w.resize(w.size().boundedTo(room.size()));
#ifdef Q_OS_HAIKU
            w.move(room.topLeft());
#endif
        }
        // The main window stays hidden behind the splash and is revealed only when the splash fades — so it
        // never appears "together with" the workspace and can't linger over it.
        QObject::connect(splash,&xara::LaunchSplash::finished,&w,[&]{
            w.show(); w.raise(); w.activateWindow(); w.runStartupSequence();
            readyWindow=&w;
            for(const QString& p : std::as_const(pendingFiles)) w.openPath(p);
            pendingFiles.clear(); });
    } else {
        w.show();
        readyWindow=&w;
    }

    if (!qEnvironmentVariableIsEmpty("XARA_WININFO")) {
        QTimer::singleShot(200,[&]{
            auto ms=w.minimumSizeHint(), mn=w.minimumSize(), mx=w.maximumSize(), sh=w.sizeHint();
            fprintf(stderr,"WININFO minHint=%dx%d minSize=%dx%d maxSize=%dx%d sizeHint=%dx%d flags=0x%llx state=0x%x\n",
                ms.width(),ms.height(), mn.width(),mn.height(), mx.width(),mx.height(), sh.width(),sh.height(),
                (unsigned long long)w.windowFlags(), (unsigned)w.windowState());
            app.quit(); });
    }

    if (!shotPath.isEmpty()) {
        QTimer::singleShot(350,[&]{ w.grab().save(shotPath); app.quit(); });
    }
    if (!qEnvironmentVariableIsEmpty("XARA_SHOTDIALOGS")) {   // screenshot dialogs: XARA_SHOTACTIONS="Menu item|Menu item…"
        // Each named menu action is triggered in turn; once its dialog is up, the dialog alone, the whole screen
        // and the window with the dialog over it are saved to XARA_SHOTDIALOGS as NN-dialog.png, NN-screen.png
        // and NN-window.png, and the dialog is cancelled. XARA_SHOTDELAY (ms) leaves time to set a scene up first.
        const QString dir=qEnvironmentVariable("XARA_SHOTDIALOGS");
        const QStringList names=qEnvironmentVariable("XARA_SHOTACTIONS").split('|',Qt::SkipEmptyParts);
        auto norm=[](QString t){ return t.remove('&').remove(QChar(0x2026)).remove("...").trimmed().toLower(); };
        auto step=std::make_shared<std::function<void(int)>>();
        *step=[&,dir,names,norm,step](int i){
            if(i>=names.size()){ app.quit(); return; }
            QAction* hit=nullptr;
            for(QAction* a : w.findChildren<QAction*>()) if(norm(a->text())==norm(names[i])){ hit=a; break; }
            if(!hit){ fprintf(stderr,"SHOTDIALOGS: no action '%s'\n",qPrintable(names[i])); (*step)(i+1); return; }
            QTimer::singleShot(900,[&,dir,i,step]{
                QWidget* d=QApplication::activeModalWidget();
                if(!d) for(QWidget* t : QApplication::topLevelWidgets()) if(qobject_cast<QDialog*>(t) && t->isVisible()) d=t;
                const QString n=QString("%1/%2-").arg(dir).arg(i,2,10,QChar('0'));
                if(d){ d->grab().save(n+"dialog.png"); if(QScreen* sc=d->screen()) sc->grabWindow(0).save(n+"screen.png");
                       // NN-window.png: the main window with the dialog painted over it, alpha and all, as a
                       // compositor would show it (Xvfb has none, so the screen grab has black corners).
                       { QPixmap comp=w.grab(); QPainter g(&comp); g.drawPixmap(d->mapToGlobal(QPoint(0,0))-w.mapToGlobal(QPoint(0,0)),d->grab()); g.end(); comp.save(n+"window.png"); }
                       if(auto* dd=qobject_cast<QDialog*>(d)) dd->reject(); else d->close(); }
                else fprintf(stderr,"SHOTDIALOGS: no dialog for #%d\n",i);
                QTimer::singleShot(300,[step,i]{ (*step)(i+1); }); });
            hit->trigger(); };
        QTimer::singleShot(qEnvironmentVariableIsSet("XARA_SHOTDELAY")? qEnvironmentVariableIntValue("XARA_SHOTDELAY") : 1500,[step]{ (*step)(0); });   // time to set a scene up first
    }
    if (!qEnvironmentVariableIsEmpty("XARA_SHOTSTART")) {   // screenshot the welcome/start screen
        const QString sp=qEnvironmentVariable("XARA_SHOTSTART"); w.setShowWelcome(false);
        QTimer::singleShot(500,[&,sp]{ for(QDialog* d : w.findChildren<QDialog*>()) if(d->objectName()=="startDialog"){ d->grab().save(sp); d->reject(); } app.quit(); });
        QTimer::singleShot(90,[&]{ w.showStartScreen(); });
    }
    if (!qEnvironmentVariableIsEmpty("XARA_SHOTSPLASH")) {  // screenshot the branded launch splash mid-boot
        const QString sp=qEnvironmentVariable("XARA_SHOTSPLASH");
        auto* splash=new xara::LaunchSplash(&w); splash->start();
        // grab a mid-progress frame (~54%) unless XARA_SHOTSPLASH_READY asks for the final "Ready" beat
        const int at = qEnvironmentVariableIsEmpty("XARA_SHOTSPLASH_READY") ? 950 : 2300;
        QTimer::singleShot(at,[&,sp,splash]{ splash->grab().save(sp); app.quit(); });
    }
    if (!shotNodePath.isEmpty()) {   // deterministic node-editor screenshot
        extern void injectNodeDemo(xara::Canvas*);
        QTimer::singleShot(350,[&]{ injectNodeDemo(w.canvas()); w.grab().save(shotNodePath); app.quit(); });
    }
    if (!qEnvironmentVariableIsEmpty("XARA_SHOTEXPORT")) {   // screenshot the Export bitmap dialog
        const QString sp=qEnvironmentVariable("XARA_SHOTEXPORT");
        extern void injectGradientDemo(xara::Canvas*);
        QTimer::singleShot(250,[&,sp]{
            injectGradientDemo(w.canvas());
            QTimer::singleShot(600,[&,sp]{ for(QDialog* d : w.findChildren<QDialog*>()) if(d->objectName()=="exportDlg"){ d->grab().save(sp); d->reject(); } app.quit(); });
            w.bitmapExportDialog("/tmp/xarq_shotexport.png");   // blocking exec(); the inner timer fires inside it
        });
    }
    if (!qEnvironmentVariableIsEmpty("XARA_SHOTSNAP")) {   // smart-guide screenshot
        extern void injectSnapDemo(xara::Canvas*);
        QString p=qEnvironmentVariable("XARA_SHOTSNAP");
        QTimer::singleShot(350,[&,p]{ injectSnapDemo(w.canvas()); w.grab().save(p); app.quit(); });
    }
    if (!qEnvironmentVariableIsEmpty("XARA_SHOTCOMBINE")) {   // boolean-op screenshot
        extern void injectCombineDemo(xara::Canvas*);
        QString p=qEnvironmentVariable("XARA_SHOTCOMBINE");
        QTimer::singleShot(350,[&,p]{ injectCombineDemo(w.canvas()); w.grab().save(p); app.quit(); });
    }
    if (!qEnvironmentVariableIsEmpty("XARA_SHOTBLEND")) {   // outline-morph blend screenshot
        extern void injectBlendDemo(xara::Canvas*);
        QString p=qEnvironmentVariable("XARA_SHOTBLEND");
        QTimer::singleShot(350,[&,p]{ injectBlendDemo(w.canvas()); w.grab().save(p); app.quit(); });
    }
    if (!qEnvironmentVariableIsEmpty("XARA_SHOTGRAD")) {   // gradient-editor screenshot
        extern void injectGradientDemo(xara::Canvas*);
        QString p=qEnvironmentVariable("XARA_SHOTGRAD");
        QTimer::singleShot(350,[&,p]{ injectGradientDemo(w.canvas()); w.grab().save(p); app.quit(); });
    }
    if (!qEnvironmentVariableIsEmpty("XARA_SHOTBRUSH")) {   // art-brush demo: inject, save a .xar, screenshot
        extern void injectBrushDemo(xara::Canvas*);
        QString p=qEnvironmentVariable("XARA_SHOTBRUSH");
        QTimer::singleShot(350,[&,p]{ injectBrushDemo(w.canvas()); w.canvas()->save("/tmp/brushtest.xar"); w.grab().save(p); app.quit(); });
    }
    if (!qEnvironmentVariableIsEmpty("XARA_SHOTHSL")) {   // blend-mode demo: inject, save a .xar, screenshot
        extern void injectHslDemo(xara::Canvas*);
        QString p=qEnvironmentVariable("XARA_SHOTHSL");
        QTimer::singleShot(350,[&,p]{ injectHslDemo(w.canvas()); w.canvas()->save("/tmp/hsltest.xar"); w.grab().save(p); app.quit(); });
    }
    if (!qEnvironmentVariableIsEmpty("XARA_SHOTCLIP")) {   // clip-view demo: inject, save a .xar, screenshot
        extern void injectClipViewDemo(xara::Canvas*);
        QString p=qEnvironmentVariable("XARA_SHOTCLIP");
        QTimer::singleShot(350,[&,p]{ injectClipViewDemo(w.canvas()); w.canvas()->save("/tmp/cliptest.xar"); w.grab().save(p); app.quit(); });
    }
    if (!qEnvironmentVariableIsEmpty("XARA_SHOTVW")) {   // variable-width stroke demo: inject, save a .xar, screenshot
        extern void injectVarWidthDemo(xara::Canvas*);
        QString p=qEnvironmentVariable("XARA_SHOTVW");
        QTimer::singleShot(350,[&,p]{ injectVarWidthDemo(w.canvas()); w.canvas()->save("/tmp/vwtest.xar"); w.grab().save(p); app.quit(); });
    }
    if (!qEnvironmentVariableIsEmpty("XARA_SHOTFEAT")) {   // live-effect feather demo: inject, save a .xar, screenshot
        extern void injectFeatherEffectDemo(xara::Canvas*);
        QString p=qEnvironmentVariable("XARA_SHOTFEAT");
        QTimer::singleShot(350,[&,p]{ injectFeatherEffectDemo(w.canvas()); w.canvas()->save("/tmp/feattest.xar"); w.grab().save(p); app.quit(); });
    }
    if (!qEnvironmentVariableIsEmpty("XARA_SHOTSS")) {   // vector stroke-shape demo: inject, save a .xar, screenshot
        extern void injectStrokeShapeDef(xara::Canvas*);
        QString p=qEnvironmentVariable("XARA_SHOTSS");
        QTimer::singleShot(350,[&,p]{ injectStrokeShapeDef(w.canvas()); w.canvas()->save("/tmp/sstest.xar"); w.grab().save(p); app.quit(); });
    }
    if (!qEnvironmentVariableIsEmpty("XARA_SHOTBEVEL")) {   // 3D bevel demo: inject + screenshot
        extern void injectBevelDemo(xara::Canvas*);
        QString p=qEnvironmentVariable("XARA_SHOTBEVEL");
        QTimer::singleShot(350,[&,p]{ injectBevelDemo(w.canvas()); w.grab().save(p); app.quit(); });
    }
    return app.exec();
}
