// main.cpp — XarQ: a modern Qt6 vector + photo + layout studio (the wx→Qt6 port of Xara Xtreme).
// Applies the XarQ design system (dark-first brushed-graphite theme, Montserrat + Ubuntu Mono,
// signal-blue accent) and the application shell. Supports a headless --shot mode for UI testing.
#include <QApplication>
#include <QPalette>
#include <QStyleFactory>
#include <QFile>
#include <QTimer>
#include <QFontDatabase>
#include <QFont>
#include <QIcon>
#include <QDialog>
#include <QProcessEnvironment>
#include <QMenu>
#include <QEvent>
#include "mainwindow.h"
#include "splash.h"
#include "canvas.h"
#include "photoops.h"
#include "xar.h"
#include <functional>
#include <cstdio>

static void applyTheme(QApplication& app) {
    app.setStyle(QStyleFactory::create("Fusion"));
    // XarQ design system — bundled Montserrat (UI/display) + Ubuntu Mono (numerics).
    for(const char* f : {"Montserrat-Regular","Montserrat-Medium","Montserrat-SemiBold","Montserrat-Bold","Montserrat-ExtraBold"})
        QFontDatabase::addApplicationFont(QString(":/xarq/assets/fonts/%1.ttf").arg(f));
    QFontDatabase::addApplicationFont(":/xarq/assets/fonts/UbuntuMono.ttf");
    { QFont uiFont("Montserrat"); uiFont.setPixelSize(12); uiFont.setStyleStrategy(QFont::PreferAntialias); app.setFont(uiFont); }

    // XarQ palette — cool graphite ramp + signal-blue accent (#2D6FF7).
    QPalette p;
    const QColor chrome(0x11,0x13,0x18), panel(0x17,0x1A,0x20), raised(0x1D,0x20,0x27),
                 well(0x23,0x27,0x2F), textStrong(0xEE,0xF1,0xF5), textBody(0xD2,0xD8,0xE0),
                 textSec(0x8B,0x94,0xA3), textDis(0x54,0x5C,0x6B), accent(0x2D,0x6F,0xF7);
    p.setColor(QPalette::Window, chrome);
    p.setColor(QPalette::WindowText, textBody);
    p.setColor(QPalette::Base, well);
    p.setColor(QPalette::AlternateBase, panel);
    p.setColor(QPalette::Text, textBody);
    p.setColor(QPalette::BrightText, textStrong);
    p.setColor(QPalette::Button, raised);
    p.setColor(QPalette::ButtonText, textBody);
    p.setColor(QPalette::ToolTipBase, QColor(0x0C,0x0D,0x10));
    p.setColor(QPalette::ToolTipText, textBody);
    p.setColor(QPalette::Highlight, accent);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Link, QColor(0x54,0x88,0xFF));
    p.setColor(QPalette::PlaceholderText, textSec);
    p.setColor(QPalette::Disabled, QPalette::Text, textDis);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, textDis);
    p.setColor(QPalette::Disabled, QPalette::WindowText, textDis);
    app.setPalette(p);
    app.setWindowIcon(QIcon(":/xarq/assets/logo/xarq-appicon.svg"));

    app.setStyleSheet(R"QSS(
        /* ===== XarQ design system — dark-first, brushed graphite, signal blue ===== */
        * { font-family: "Montserrat","DejaVu Sans","Segoe UI",sans-serif; font-size: 12px; outline: none; }
        QMainWindow { background: #111318; }
        QWidget { background: #171A20; color: #D2D8E0; }
        /* Content widgets are transparent so they show their PARENT surface (web-CSS behaviour), not the
           blanket QWidget colour — otherwise labels/checkboxes band against bars, dialogs, panels, etc. */
        QLabel, QCheckBox, QRadioButton, QGroupBox::indicator, QAbstractButton#link, QSlider { background: transparent; }
        QToolTip { background: #0C0D10; color: #D2D8E0; border: 1px solid rgba(255,255,255,0.10); padding: 5px 8px; border-radius: 5px; }

        /* Top action bar — grad-chrome brushed sheen */
        QToolBar#topbar { background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #1B1E24, stop:1 #14161A); border: none; border-bottom: 1px solid rgba(255,255,255,0.06); padding: 5px 8px; spacing: 3px; }
        QToolBar#topbar QToolButton { padding: 6px; border-radius: 3px; color:#D2D8E0; }
        QToolBar#topbar QToolButton:hover { background: #2A2F38; }
        QToolBar#topbar QToolButton:pressed { background: rgba(45,111,247,0.24); }
        QToolBar QToolButton { color:#D2D8E0; padding:4px 8px; border-radius:3px; }
        QLabel#appTitle { font-weight: 800; font-size:17px; color: #EEF1F5; letter-spacing: -0.3px; background:transparent; }

        /* Tool rail — DS ToolRail: a floating glass panel over the canvas; active tool = rounded accent tint */
        QWidget#railGlass { background: rgba(23,26,32,0.72); border:1px solid rgba(255,255,255,0.08); border-radius:5px; }
        QScrollArea#railScroll { background:transparent; border:none; }
        QFrame#toolrail { background: transparent; border:none; }
        QFrame#railsep { max-height:1px; margin:5px 10px; background:rgba(255,255,255,0.10); border:none; }
        #toolrail QToolButton { border-radius: 4px; background: transparent; color:#8B94A3; padding:6px; }
        #toolrail QToolButton:hover { background: rgba(255,255,255,0.07); color:#D2D8E0; }
        #toolrail QToolButton:checked { background: rgba(45,111,247,0.24); border:1px solid rgba(45,111,247,0.5); color:#5488FF; }

        /* Docks / panels — solid graphite, uppercase tracked title */
        QDockWidget { color:#8B94A3; titlebar-close-icon:none; }
        QDockWidget::title { background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #1B1E24, stop:1 #15171C); padding: 8px 12px; border-bottom: 1px solid rgba(255,255,255,0.06); font-weight:700; font-size:11px; letter-spacing:1.4px; color:#8B94A3; }
        QDockWidget > QWidget { background: #171A20; }
        QLabel#panelTitle { font-size: 11px; font-weight: 700; color: #8B94A3; letter-spacing:1.4px; text-transform:uppercase; padding-bottom: 2px; }

        /* DS RightDock — floats over the canvas; translucent glass backdrop so the artwork shows through
           (the section cards stay solid). Left hairline separates it from the canvas. */
        QWidget#rightPanel { background: rgba(15,17,21,0.80); border-left:1px solid rgba(255,255,255,0.08); }
        QWidget#rpPage { background:transparent; }
        QScrollArea#rpScroll { background:transparent; border:none; }
        QTabWidget#rpTabs::pane { border:none; background:transparent; }
        #rpTabs QTabBar { qproperty-drawBase:0; background:transparent; }
        #rpTabs QTabBar::tab { background:transparent; color:#8B94A3; padding:8px 15px; border:none; font-size:13px; font-weight:600; margin-right:2px; }
        #rpTabs QTabBar::tab:selected { color:#EEF1F5; font-weight:700; border-bottom:2px solid #2D6FF7; }
        #rpTabs QTabBar::tab:hover:!selected { color:#D2D8E0; }
        /* Collapsible section card (DS Panel) — draggable to reorder / detach to a floating panel */
        QWidget#dsSection { background:#171A20; border:1px solid rgba(255,255,255,0.06); border-radius:5px; }
        QWidget#dsSectionHdr { background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #1B1E24, stop:1 #14161A); border-top-left-radius:5px; border-top-right-radius:5px; border-bottom:1px solid rgba(255,255,255,0.06); }
        QLabel#dsGrip { color:#3D4450; font-size:10px; background:transparent; }
        QLabel#dsChevron { color:#6E7787; font-size:11px; font-weight:700; background:transparent; }
        QLabel#dsSectionTitle { color:#8B94A3; font-size:10px; font-weight:700; letter-spacing:0.6px; background:transparent; }
        QToolButton#dsAct { background:transparent; border:none; border-radius:3px; padding:2px; }
        QToolButton#dsAct:hover { background:rgba(255,255,255,0.08); }
        QWidget#floatPanel { background:transparent; }
        QWidget#panelGrip { background:transparent; }
        QWidget#panelGrip:hover { background:rgba(45,111,247,0.45); }

        QGroupBox { border: 1px solid rgba(255,255,255,0.08); border-radius: 6px; margin-top: 11px; padding: 7px 8px 7px 8px; background:#1D2027; }
        QGroupBox::title { subcontrol-origin: margin; left: 9px; padding: 0 4px; color:#8B94A3; font-weight:700; font-size:11px; letter-spacing:1.2px; }
        /* Right-dock inspectors (Properties / Layers / Colour) — compact: smaller type, tighter group
           padding + input wells (DS RightDock density) */
        QWidget#propsHost *, QWidget#layersHost *, QWidget#colourHost * { font-size: 11px; }
        QWidget#propsHost QGroupBox { margin-top: 9px; padding: 4px 6px 5px 6px; }
        QWidget#propsHost QComboBox, QWidget#propsHost QSpinBox, QWidget#propsHost QDoubleSpinBox, QWidget#propsHost QLineEdit { padding:2px 6px; min-height:15px; }
        QWidget#layersHost QListWidget::item { padding:2px 4px; }

        /* Inputs — recessed wells */
        QComboBox, QSpinBox, QDoubleSpinBox, QLineEdit { background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #1B1E24, stop:1 #23272F); border:1px solid #3D4450; border-radius:3px; padding:3px 6px; color:#EEF1F5; min-height:17px; selection-background-color:#2D6FF7; }
        QSpinBox, QDoubleSpinBox { font-family:"Ubuntu Mono","DejaVu Sans Mono",monospace; }
        QComboBox:hover, QSpinBox:hover, QDoubleSpinBox:hover, QLineEdit:hover { border-color:#545C6B; }
        QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus, QLineEdit:focus { border:1px solid #2D6FF7; }
        QComboBox::drop-down { border:none; width:16px; }
        /* Dropdown popup — rounded; the popup window is made translucent (RoundPopups filter) so the rounded
           corners show through instead of a square window background. */
        QComboBox QAbstractItemView { background:#1D2027; border:1px solid rgba(255,255,255,0.12); border-radius:6px; padding:4px; selection-background-color:rgba(45,111,247,0.24); outline:none; }
        QComboBox QAbstractItemView::item { padding:4px 8px; border-radius:3px; min-height:18px; }
        QSpinBox::up-button, QDoubleSpinBox::up-button, QSpinBox::down-button, QDoubleSpinBox::down-button { width:15px; border:none; background:transparent; }

        /* Buttons — raised, top highlight; default (primary) = accent gradient */
        QPushButton { background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #2E333C, stop:1 #262A31); border:1px solid rgba(255,255,255,0.10); border-radius:3px; padding:5px 11px; color:#EEF1F5; min-height:15px; }
        QPushButton:hover { background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #363C46, stop:1 #2E333C); border-color:rgba(255,255,255,0.16); }
        QPushButton:pressed { background:#23272F; }
        QPushButton:default { background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #4680FF, stop:1 #2D6FF7); border:1px solid #235FE0; color:#FFFFFF; font-weight:700; }
        QPushButton:default:hover { background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #5488FF, stop:1 #3B7AFF); }
        QPushButton:default:pressed { background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #2D6FF7, stop:1 #235FE0); }
        QPushButton:disabled { color:#545C6B; background:#1D2027; border-color:rgba(255,255,255,0.06); }
        /* Primary accent CTA (DS command-bar "Export") */
        QPushButton#ctaExport { background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #4680FF, stop:1 #2D6FF7); border:1px solid #235FE0; border-radius:3px; color:#FFFFFF; font-weight:700; padding:5px 14px; min-height:18px; }
        QPushButton#ctaExport:hover { background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #5488FF, stop:1 #3B7AFF); }
        QPushButton#ctaExport:pressed { background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #2D6FF7, stop:1 #235FE0); }

        QToolButton { border-radius:3px; color:#D2D8E0; }
        QToolButton:hover { background:#2A2F38; }
        QToolButton:checked { background:rgba(45,111,247,0.24); color:#5488FF; }

        QSlider::groove:horizontal { height:4px; background:#2A2F38; border-radius:2px; }
        QSlider::handle:horizontal { width:14px; height:14px; margin:-6px 0; border-radius:7px; background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #4680FF, stop:1 #2D6FF7); border:1px solid #235FE0; }
        QSlider::handle:horizontal:hover { background:#5488FF; }
        QSlider::sub-page:horizontal { background:#2D6FF7; border-radius:2px; }

        QCheckBox { spacing:7px; }
        QCheckBox::indicator, QRadioButton::indicator { width:16px; height:16px; border-radius:2px; border:1px solid #545C6B; background:#171A20; }
        QRadioButton::indicator { border-radius:8px; }
        QCheckBox::indicator:hover, QRadioButton::indicator:hover { border-color:#2D6FF7; }
        QCheckBox::indicator:checked, QRadioButton::indicator:checked { background:#2D6FF7; border-color:#2D6FF7; }

        QListWidget, QTreeWidget, QTableWidget { background:#14161A; border:1px solid rgba(255,255,255,0.08); border-radius:5px; padding:2px; alternate-background-color:#171A20; }
        QListWidget::item, QTreeWidget::item { padding:3px 7px; border-radius:3px; }
        QListWidget::item:selected, QTreeWidget::item:selected { background:rgba(45,111,247,0.24); color:#EEF1F5; }
        QListWidget::item:hover, QTreeWidget::item:hover { background:#1D2027; }
        QHeaderView::section { background:#1B1E24; color:#8B94A3; border:none; border-bottom:1px solid rgba(255,255,255,0.08); padding:5px 8px; font-weight:700; font-size:11px; }

        QScrollArea { border:none; background:#171A20; }
        QScrollBar:vertical { background:transparent; width:9px; margin:2px; }
        QScrollBar::handle:vertical { background:#333944; border-radius:4px; min-height:28px; }
        QScrollBar::handle:vertical:hover { background:#3D4450; }
        QScrollBar::add-line, QScrollBar::sub-line { height:0; width:0; }
        QScrollBar::add-page, QScrollBar::sub-page { background:transparent; }
        QScrollBar:horizontal { background:transparent; height:9px; margin:2px; }
        QScrollBar::handle:horizontal { background:#333944; border-radius:4px; min-width:28px; }
        QScrollBar::handle:horizontal:hover { background:#3D4450; }
        /* Slim, unobtrusive scrollbar inside the floating glass tool rail */
        QScrollArea#railScroll QScrollBar:vertical { width:5px; margin:1px; }
        QScrollArea#railScroll QScrollBar::handle:vertical { background:rgba(255,255,255,0.14); border-radius:2px; min-height:24px; }
        QScrollArea#railScroll QScrollBar::handle:vertical:hover { background:rgba(255,255,255,0.24); }

        /* DS TitleBar — custom widget (setMenuWidget): wordmark + menus + centered doc + Share/cloud */
        QWidget#titleBar { background:#111318; border-bottom:1px solid rgba(255,255,255,0.06); }
        QToolButton#tlClose { background:#FF5F57; border:none; border-radius:6px; }
        QToolButton#tlMin   { background:#FEBC2E; border:none; border-radius:6px; }
        QToolButton#tlMax   { background:#28C840; border:none; border-radius:6px; }
        QToolButton#tlClose:hover { background:#FF7B74; } QToolButton#tlMin:hover { background:#FFCB55; } QToolButton#tlMax:hover { background:#45D65F; }
        /* DS Canvas Toolbar — floating glass pan/fit/guides/grid pill */
        QWidget#zoomPill { background: rgba(23,26,32,0.80); border:1px solid rgba(255,255,255,0.08); border-radius:5px; }
        QToolButton#pillBtn { background:transparent; border:none; border-radius:4px; padding:4px; }
        QToolButton#pillBtn:hover { background: rgba(255,255,255,0.08); }
        QToolButton#pillBtn:checked { background: rgba(45,111,247,0.28); }
        QFrame#pillSep { max-width:1px; background: rgba(255,255,255,0.12); margin:8px 3px; border:none; }
        QMenuBar, QMenuBar#embeddedMenu { background:transparent; border:none; padding:0; }
        QMenuBar::item { padding:4px 8px; background:transparent; color:#D2D8E0; border-radius:3px; }
        QToolButton#titleGhost { background:transparent; border:none; color:#D2D8E0; padding:5px 11px; border-radius:4px; font-size:13px; font-weight:600; }
        QToolButton#titleGhost:hover { background: rgba(255,255,255,0.08); color:#EEF1F5; }
        QLabel#docName { color:#D2D8E0; font-size:13px; font-weight:600; background:transparent; }
        /* radius must live in the SAME rule as the background (from the [tone] selector) or Qt won't clip it */
        QLabel#docBadge { font-size:10px; font-weight:700; padding:2px 9px; letter-spacing:0.3px; }
        QLabel#docBadge[tone="warning"] { color:#F5B23D; background:rgba(245,178,61,0.16); border-radius:9px; }
        QLabel#docBadge[tone="ok"] { color:#8B94A3; background:rgba(255,255,255,0.06); border-radius:9px; }
        QMenuBar::item:selected { background:#2A2F38; border-radius:3px; }
        QMenuBar::item:pressed { background:rgba(45,111,247,0.24); border-radius:3px; }
        QMenu { background:#1D2027; border:1px solid rgba(255,255,255,0.10); border-radius:5px; padding:4px; }
        QMenu::item { padding:4px 24px 4px 14px; border-radius:3px; color:#D2D8E0; }
        QMenu::item:selected { background:rgba(45,111,247,0.24); color:#EEF1F5; }
        QMenu::item:disabled { color:#545C6B; }
        QMenu::separator { height:1px; background:rgba(255,255,255,0.08); margin:5px 10px; }

        QTabBar#docTabBar { background:#111318; }
        QTabBar#docTabBar::tab { background:#1B1E24; color:#8B94A3; padding:5px 13px; margin:5px 1px 0 1px; border:1px solid rgba(255,255,255,0.05); border-bottom:none; border-top-left-radius:6px; border-top-right-radius:6px; min-width:66px; }
        QTabBar#docTabBar::tab:selected { background:#1D2027; color:#EEF1F5; border-top:2px solid #2D6FF7; }
        QTabBar#docTabBar::tab:hover:!selected { background:#23272F; color:#D2D8E0; }
        /* subtle grey close X (replaces the platform default red one) */
        QTabBar#docTabBar::close-button { image:url(:/xarq/assets/logo/tabclose.svg); width:12px; height:12px; border-radius:3px; margin-left:4px; }
        QTabBar#docTabBar::close-button:hover { background:rgba(255,255,255,0.14); }
        QTabWidget::pane { border:1px solid rgba(255,255,255,0.08); border-radius:6px; }
        QTabBar::tab { background:#1B1E24; color:#8B94A3; padding:6px 12px; border:1px solid rgba(255,255,255,0.06); border-bottom:none; border-top-left-radius:3px; border-top-right-radius:3px; }
        QTabBar::tab:selected { background:#1D2027; color:#EEF1F5; }

        /* DS StatusBar — 11px, tertiary base; accent tool label, mono numerics, green Saved */
        QStatusBar { background:#111318; border-top:1px solid rgba(255,255,255,0.06); color:#6E7787; }
        QStatusBar QLabel { color:#6E7787; font-size:11px; padding:0 8px; background:transparent; }
        QStatusBar QLabel#sTool { color:#7FA5FF; font-weight:600; }
        QStatusBar QLabel#sNum  { font-family:"Ubuntu Mono","DejaVu Sans Mono",monospace; color:#8B94A3; }
        QStatusBar QLabel#sHint { color:#545C6B; }
        QStatusBar QLabel#sMeta { color:#6E7787; }
        QStatusBar QWidget#sSpacer { background:transparent; }
        QStatusBar QLabel#sSaved[tone="ok"]      { color:#2FBF71; font-weight:600; }
        QStatusBar QLabel#sSaved[tone="warning"] { color:#F5B23D; font-weight:600; }
        QStatusBar::item { border:none; }

        QDialog { background:#171A20; }
        QDialog QLabel { color:#D2D8E0; }
        QLabel#printPreview { background:#0C0D10; border:1px solid rgba(255,255,255,0.08); border-radius:6px; }
        QDialog#printDialog QGroupBox { min-width:300px; }

        /* InfoBar — the per-tool options bar */
        QToolBar#infobarTb { background:#1D2027; border:none; border-bottom:1px solid rgba(255,255,255,0.06); padding:0; spacing:0; }
        QWidget#infobar { background:#1D2027; }
        QLabel#infoTool { font-weight:700; color:#5488FF; padding:3px 10px; background:rgba(45,111,247,0.24); border-radius:5px; letter-spacing:0.3px; }
        QLabel#infoLbl  { color:#8B94A3; padding-left:3px; }
        QLabel#infoHint { color:#6E7787; font-style:italic; }
        QFrame#infoSep  { background:rgba(255,255,255,0.10); max-width:1px; margin:5px 5px; border:none; }
        QWidget#infobar QComboBox, QWidget#infobar QSpinBox, QWidget#infobar QDoubleSpinBox { padding:2px 6px; min-height:18px; }
        QWidget#infobar QToolButton { padding:3px 10px; border:1px solid rgba(255,255,255,0.10); background:#262A31; border-radius:3px; font-weight:600; }
        QWidget#infobar QToolButton:hover { background:#2E333C; border-color:rgba(255,255,255,0.16); }
        QWidget#infobar QToolButton:checked { background:rgba(45,111,247,0.24); border-color:#2D6FF7; color:#5488FF; }

        /* Colour Line — bottom palette strip */
        QToolBar#colourTb { background:#0C0D10; border:none; border-top:1px solid rgba(255,255,255,0.06); padding:0; spacing:0; }
        QWidget#colourline { background:#0C0D10; }
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

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setApplicationName("XarQ");
    app.setApplicationDisplayName("XarQ");
    app.setOrganizationName("XarQ");
    applyTheme(app);
    app.installEventFilter(new RoundPopups(&app));   // translucent (rounded) menu + dropdown popups

    QString shotPath, exportPath, openPath, savePath, shotNodePath;
    for (int i=1;i<argc;++i){ QString a(argv[i]);
        if(a=="--shot"&&i+1<argc) shotPath=argv[++i];
        else if(a=="--export"&&i+1<argc) exportPath=argv[++i];
        else if(a=="--open"&&i+1<argc) openPath=argv[++i];
        else if(a=="--save"&&i+1<argc) savePath=argv[++i];
        else if(a=="--shotnode"&&i+1<argc) shotNodePath=argv[++i];
        else if(a=="--selftest"){ extern int runSelfTest(); return runSelfTest(); }
        else if(a=="--xarinfo"&&i+1<argc){ // diagnostic: load a .xar and report what was read
            QString p=argv[++i]; xara::Document d;
            bool ok=xara::loadXar(d,p);
            std::function<int(const QVector<xara::Shape>&)> cnt=[&](const QVector<xara::Shape>& v){ int n=0; for(auto&s:v){ n++; if(s.type==xara::ShapeType::Group) n+=cnt(s.children); } return n; };
            fprintf(stderr,"%s: ok=%d shapes=%d (deep=%d) page=%.0fx%.0f\n",
                qPrintable(p), ok, (int)d.shapes.size(), ok?cnt(d.shapes):0, d.pageSize.width(), d.pageSize.height());
            return ok?0:1; } }

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

    xara::MainWindow w;   // heavy construction — the splash above stays up covering it
    // Non-interactive ⇒ suppress the launch dialogs (recovery + the *modal* welcome screen, which would
    // otherwise block the event loop and hang every scripted grab).
    if(!interactive) w.setShowWelcome(false);
    w.setDeferStartup(interactive);   // the splash drives recovery+welcome once it fades
    if(!openPath.isEmpty()){ if(openPath.endsWith(".svg",Qt::CaseInsensitive)){ w.canvas()->newDocument(); w.canvas()->importSvgFile(openPath); } else w.canvas()->load(openPath); }

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
        // The main window stays hidden behind the splash and is revealed only when the splash fades — so it
        // never appears "together with" the workspace and can't linger over it.
        QObject::connect(splash,&xara::LaunchSplash::finished,&w,[&w]{
            w.show(); w.raise(); w.activateWindow(); w.runStartupSequence(); });
    } else {
        w.show();
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
