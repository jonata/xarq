#include "mainwindow.h"
#include "panels.h"
#include "galleries.h"
#include "dialogs.h"
#include "spell.h"
#include "exif.h"
#include "photoops.h"
#include "icons.h"
#include "render.h"
#include "splash.h"
#include <QtWidgets>
#include <QStackedWidget>
#include <QDesktopServices>
#include <QUrl>
#include <QMimeData>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QStandardPaths>
#include <QTimer>
#include <QDir>
#include <QPrinter>
#include <QPrintDialog>
#include <QPrinterInfo>
#include <QPageSetupDialog>
#include <QRegularExpression>
#include <QSettings>
#include <functional>
#include "xar.h"
#include "icc.h"
#include "raw.h"
#include "startscreen.h"

namespace xara {

MainWindow::MainWindow() {
    m_canvas = new Canvas(this);
    { QWidget* central=nullptr; setupTabBar(central); setCentralWidget(central); }

    buildTopBar();

    // InfoBar — Xara's signature per-tool options bar, on its own row below the action bar.
    m_infobar=new InfoBar(m_canvas);
    auto* ibTb=new QToolBar("Tool Options"); ibTb->setObjectName("infobarTb"); ibTb->setMovable(false);
    ibTb->addWidget(m_infobar); addToolBarBreak(Qt::TopToolBarArea); addToolBar(Qt::TopToolBarArea,ibTb);

    buildToolRail();
    buildDocks();
    buildMenus();
    buildTitleBar();   // DS TitleBar: wordmark + menus + centered doc name/badge + Share/cloud (setMenuWidget)
    buildZoomPill();   // DS Canvas Toolbar: floating glass pan/fit/guides/grid pill, bottom-centre of the canvas

    // Colour Line — bottom palette strip (click = fill, right-click = line colour).
    m_colourLine=new ColourLine(m_canvas);
    auto* clTb=new QToolBar("Colour Line"); clTb->setObjectName("colourTb"); clTb->setMovable(false);
    clTb->addWidget(m_colourLine); addToolBar(Qt::BottomToolBarArea,clTb);

    // status bar — DS StatusBar wording: "<tool> tool" · X/Y · Sel W×H (left) | centred panel hint |
    // sRGB · snap · pages · units · zoom · Saved (right).
    m_sTool=new QLabel("Select tool"); m_sTool->setObjectName("sTool");
    m_sCoord=new QLabel; m_sCoord->setObjectName("sNum");
    m_sSel=new QLabel; m_sSel->setObjectName("sNum");
    m_sSnap=new QLabel;
    statusBar()->addWidget(m_sTool); statusBar()->addWidget(m_sCoord); statusBar()->addWidget(m_sSel);
    { auto* lsp=new QWidget; lsp->setObjectName("sSpacer"); statusBar()->addWidget(lsp,1); }
    { auto* hint=new QLabel("⊞  Drag panel headers to rearrange · detach to float"); hint->setObjectName("sHint"); statusBar()->addWidget(hint); }
    { auto* rsp=new QWidget; rsp->setObjectName("sSpacer"); statusBar()->addWidget(rsp,1); }
    { auto* profile=new QLabel("sRGB IEC61966"); profile->setObjectName("sMeta"); statusBar()->addPermanentWidget(profile); }
    statusBar()->addPermanentWidget(m_sSnap);

    // page navigation (multi-page documents)
    auto* pagePrev=new QToolButton; pagePrev->setText("◀"); pagePrev->setAutoRaise(true); pagePrev->setToolTip("Previous page");
    auto* pageLbl=new QLabel; pageLbl->setObjectName("pageLbl"); pageLbl->setMinimumWidth(70); pageLbl->setAlignment(Qt::AlignCenter);
    auto* pageNext=new QToolButton; pageNext->setText("▶"); pageNext->setAutoRaise(true); pageNext->setToolTip("Next page");
    auto updPage=[this,pageLbl]{ pageLbl->setText(QString("Page %1 / %2").arg(m_canvas->currentPage()+1).arg(m_canvas->pageCount())); };
    connect(pagePrev,&QToolButton::clicked,this,[this,updPage]{ m_canvas->gotoPage(m_canvas->currentPage()-1); updPage(); });
    connect(pageNext,&QToolButton::clicked,this,[this,updPage]{ m_canvas->gotoPage(m_canvas->currentPage()+1); updPage(); });
    connect(m_canvas,&Canvas::documentChanged,this,[this,updPage]{ updPage();
        if(!m_suppressDirty){
            // Content added from a still-visible title-bar menu (Import…, Insert…) while the welcome
            // screen is up: materialise the landing tab and reveal the canvas, else the edit lands on the
            // hidden canvas with no tab (invisible, and discarded by the next New). See enterEmptyState().
            if(m_welcomePanel && m_centerStack && m_centerStack->currentWidget()==m_welcomePanel) ensureLandingTab();
            m_recoveryDirty=true; if(!m_dirty){ m_dirty=true; refreshTabText(m_activeTab); refreshDocStatus(); } } });
    connect(m_canvas,&Canvas::mouseDoc,this,[updPage](QPointF){ updPage(); });
    statusBar()->addPermanentWidget(pagePrev); statusBar()->addPermanentWidget(pageLbl); statusBar()->addPermanentWidget(pageNext);
    updPage();

    // measurement-units selector
    auto* unitCombo=new QComboBox; unitCombo->setObjectName("unitCombo");
    unitCombo->addItems({"Pixels","Millimetres","Centimetres","Inches","Points"});
    unitCombo->setToolTip("Measurement units");
    connect(unitCombo,QOverload<int>::of(&QComboBox::activated),this,[this](int i){ m_canvas->setUnits(Canvas::Units(i)); });
    statusBar()->addPermanentWidget(unitCombo);

    // editable zoom combo with presets
    auto* zoomCombo=new QComboBox; zoomCombo->setObjectName("zoomCombo"); zoomCombo->setEditable(true); zoomCombo->setFixedWidth(86);
    zoomCombo->addItems({"25%","50%","75%","100%","150%","200%","400%","800%"});
    zoomCombo->setToolTip("Zoom level");
    auto applyZoom=[this,zoomCombo]{ QString s=zoomCombo->currentText(); s.remove('%'); bool ok; double v=s.trimmed().toDouble(&ok); if(ok&&v>1) m_canvas->zoomTo(v/100.0); };
    connect(zoomCombo,QOverload<int>::of(&QComboBox::activated),this,[applyZoom]{ applyZoom(); });
    connect(zoomCombo->lineEdit(),&QLineEdit::returnPressed,this,[applyZoom]{ applyZoom(); });
    statusBar()->addPermanentWidget(zoomCombo);
    // (Save state is shown by the title-bar badge; no duplicate indicator in the status bar.)

    // DS wording: "Sel <W> × <H> px" for a single object, else a count; coords "X <x>   Y <y>".
    auto updSel=[this]{ int n=m_canvas->selection().size();
        if(n==0) m_sSel->setText("");
        else if(n==1){ QRectF b=m_canvas->selectionBoundsDoc(); m_sSel->setText("Sel "+m_canvas->fmtLen(b.width(),false)+" × "+m_canvas->fmtLen(b.height(),true)); }
        else m_sSel->setText(QString("Sel %1 objects").arg(n)); };
    auto updSnap=[this]{ m_sSnap->setText(m_canvas->snapEnabled()?"Snap ●":"Snap ○"); };
    connect(m_canvas,&Canvas::mouseDoc,this,[this](QPointF p){ m_sCoord->setText("X "+m_canvas->fmtLen(p.x(),false)+"   Y "+m_canvas->fmtLen(p.y(),false)); });
    connect(m_canvas,&Canvas::zoomChanged,this,[zoomCombo](double z){ zoomCombo->setCurrentText(QString::number(int(z*100+0.5))+"%"); });
    connect(m_canvas,&Canvas::selectionChanged,this,[updSel]{ updSel(); });
    connect(m_canvas,&Canvas::statusMessage,this,[this](QString s){ statusBar()->showMessage(s,3000); });
    connect(m_canvas,&Canvas::toolChanged,this,[this](Tool t){ if(auto* b=m_tools->button(int(t))){ b->setChecked(true); m_sTool->setText(b->toolTip().section(' ',0,0)+" tool"); } });   // sync rail + status even on programmatic setTool
    updSnap();

    setMinimumSize(640, 460);   // keep the window resizable/maximisable on any screen (panels scroll if cramped)
    resize(1480, 920);
    setWindowTitle("XarQ — Modern");
    m_canvas->newDocument();   // empty canvas backdrop; the landing tab/document is created on demand below
    // Autosave: snapshot changed work to a recovery file every 90s; offer to restore it on launch.
    m_autosaveTimer=new QTimer(this); m_autosaveTimer->setInterval(90000);
    connect(m_autosaveTimer,&QTimer::timeout,this,[this]{ autosave(); });
    m_autosaveTimer->start();
    m_dirty=false; m_recoveryDirty=false;   // a fresh blank canvas is not "unsaved work"
    m_activeTab=-1;                          // no landing tab yet — created lazily (see ensureLandingTab)
    refreshDocStatus();                      // seed the title badge + status "Saved" now that all widgets exist
    // Don't open a blank "Untitled" document by default: the landing tab is created only once the user picks
    // something in the welcome screen (or as a blank fallback if they skip it / it's disabled). When no
    // splash drives startup (headless/tests/--export), materialise the blank landing tab now so it's usable.
    QTimer::singleShot(0,[this]{
        if(m_startupDialogs && !m_deferStartup) runStartupSequence();
        else if(!m_deferStartup) ensureLandingTab();
    });
    QTimer::singleShot(0,[this]{ m_canvas->zoomFit();
        if(qEnvironmentVariableIsSet("XARA_UITOOL")){ int t=qEnvironmentVariable("XARA_UITOOL").toInt(); m_canvas->select({2}); m_canvas->setTool(Tool(t)); }
        if(qEnvironmentVariableIsSet("XARA_SHOWDOCK")){ if(auto* d=findChild<QDockWidget*>(qEnvironmentVariable("XARA_SHOWDOCK"))){ d->show(); d->raise(); } } });   // screenshot/test helper
}

void MainWindow::buildTopBar() {
    auto* tb=new QToolBar("Main"); tb->setMovable(false); tb->setIconSize(QSize(20,20));
    tb->setObjectName("topbar"); addToolBar(Qt::TopToolBarArea, tb);
    // Toolbar buttons carry the shortcut in their tooltip only — the accelerators live on the MENU actions.
    // (Binding the same QKeySequence on both the toolbar action and the menu action makes Qt treat it as an
    // ambiguous shortcut and fire NEITHER — that broke Ctrl+Z, Ctrl+N, etc.)
    auto add=[&](const QString& ic,const QString& txt,const QString& sc,std::function<void()> fn){
        auto* a=tb->addAction(actionIcon(ic),txt); a->setToolTip(sc.isEmpty()?txt:txt+"  ("+sc+")");
        connect(a,&QAction::triggered,this,[fn]{fn();}); return a; };
    add("new","New","Ctrl+N",[this]{newFile();});
    add("open","Open","Ctrl+O",[this]{openFile();});
    add("save","Save","Ctrl+S",[this]{saveFile();});
    tb->addSeparator();
    add("undo","Undo","Ctrl+Z",[this]{m_canvas->undo();});
    add("redo","Redo","Ctrl+Shift+Z",[this]{m_canvas->redo();});
    tb->addSeparator();
    add("dup","Duplicate","Ctrl+D",[this]{m_canvas->duplicateSelection();});
    add("group","Group","Ctrl+G",[this]{m_canvas->groupSelection();});
    add("delete","Delete","",[this]{m_canvas->deleteSelection();});

    auto* spacer=new QWidget; spacer->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Preferred); tb->addWidget(spacer);

    // Zoom cluster (DS: zoom-out · percent · zoom-in)
    auto zoomBtn=[&](const QString& t,std::function<void()> fn){ auto* b=new QToolButton; b->setText(t); b->setAutoRaise(true); connect(b,&QToolButton::clicked,this,[fn]{fn();}); tb->addWidget(b); return b; };
    zoomBtn("−",[this]{m_canvas->zoomOut();});
    zoomBtn("Fit",[this]{m_canvas->zoomFit();});
    zoomBtn("100%",[this]{m_canvas->zoomReset();});
    zoomBtn("+",[this]{m_canvas->zoomIn();});

    // Primary accent Export button (DS command-bar CTA), far right.
    auto* gap=new QWidget; gap->setFixedWidth(6); tb->addWidget(gap);
    auto* cta=new QPushButton(actionIcon("export"),"  Export"); cta->setObjectName("ctaExport");
    cta->setCursor(Qt::PointingHandCursor); cta->setToolTip("Export…  (Ctrl+E)");
    connect(cta,&QPushButton::clicked,this,[this]{ exportImage(); });
    tb->addWidget(cta);
    auto* gap2=new QWidget; gap2->setFixedWidth(4); tb->addWidget(gap2);
}

QToolButton* MainWindow::railButton(Tool t, const QString& tip, const QString& sc) {
    auto* b=new QToolButton; b->setCheckable(true); b->setAutoRaise(true);
    b->setIcon(toolIcon(t)); b->setIconSize(QSize(19,19)); b->setFixedSize(32,32);
    b->setToolTip(sc.isEmpty()? tip : tip+"  ("+sc+")"); b->setProperty("tool",int(t));
    if(!sc.isEmpty()){ auto* a=new QAction(this); a->setShortcut(QKeySequence(sc)); connect(a,&QAction::triggered,b,[b]{ b->click(); }); addAction(a); }
    m_tools->addButton(b,int(t));
    connect(b,&QToolButton::clicked,this,[this,t]{ m_canvas->setTool(t); });
    return b;
}

void MainWindow::buildToolRail() {
    auto* rail=new QFrame; rail->setObjectName("toolrail");
    auto* V=new QVBoxLayout(rail); V->setContentsMargins(6,8,6,8); V->setSpacing(2); V->setAlignment(Qt::AlignTop);
    m_tools=new QButtonGroup(this); m_tools->setExclusive(true);
    struct E{Tool t;const char*tip;const char*sc;};
    const E groups[][5]={
        {{Tool::Select,"Selector","V"},{Tool::Node,"Shape editor","N"},{}},
        {{Tool::Rect,"Rectangle","M"},{Tool::Ellipse,"Ellipse","E"},{Tool::Polygon,"Polygon / Star","P"},{}},
        {{Tool::Pen,"Pen (bezier)","B"},{Tool::Line,"Line","L"},{Tool::Freehand,"Freehand","F"},{Tool::Text,"Text","T"},{}},
        {{Tool::Fill,"Fill / Gradient","G"},{Tool::Transparency,"Transparency","Y"},{Tool::Eraser,"Eraser","Shift+E"},{Tool::Clone,"Clone / Heal","K"},{}},
        {{Tool::Shadow,"Shadow","S"},{Tool::Contour,"Contour","C"},{Tool::Bevel,"Bevel","J"},{Tool::Feather,"Feather","Shift+F"},{Tool::Extrude,"3D Extrude","X"}},
        {{Tool::Blend,"Blend","W"},{Tool::Mould,"Mould / Envelope","D"},{Tool::Table,"Table","Shift+T"},{Tool::Region,"Photo Region","Shift+R"}},
        {{Tool::Zoom,"Zoom","Z"},{Tool::Pan,"Pan / Push","H"},{}},
    };
    bool first=true;
    for (auto& grp : groups) {
        if(!first){ auto* sep=new QFrame; sep->setFrameShape(QFrame::HLine); sep->setObjectName("railsep"); V->addWidget(sep); }
        first=false;
        for (auto& e : grp){ if(e.tip==nullptr) break; V->addWidget(railButton(e.t,e.tip,e.sc), 0, Qt::AlignHCenter); }
    }
    if(auto* b=m_tools->button(int(Tool::Select))) b->setChecked(true);
    m_railInner=rail;

    // DS ToolRail: a floating glass panel overlaying the canvas (not a docked bar). The rail scrolls
    // internally when its natural height (~22 tools × 40px) exceeds the canvas, so the window's minimum
    // height stays small on any screen.
    auto* railScroll=new QScrollArea; railScroll->setObjectName("railScroll"); railScroll->setWidget(rail);
    railScroll->setWidgetResizable(true); railScroll->setFrameShape(QFrame::NoFrame);
    railScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); railScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    railScroll->viewport()->setStyleSheet("background:transparent;");

    m_railOverlay=new QWidget(m_canvas); m_railOverlay->setObjectName("railGlass");
    m_railOverlay->setAttribute(Qt::WA_StyledBackground, true);
    auto* gl=new QVBoxLayout(m_railOverlay); gl->setContentsMargins(0,6,0,6); gl->setSpacing(0); gl->addWidget(railScroll);
    auto* sh=new QGraphicsDropShadowEffect(m_railOverlay); sh->setBlurRadius(24); sh->setOffset(0,8); sh->setColor(QColor(0,0,0,150));
    m_railOverlay->setGraphicsEffect(sh);
    m_canvas->installEventFilter(this);
    positionRailOverlay();
    m_railOverlay->show(); m_railOverlay->raise();
}

// Keep the floating glass rail pinned to the top-left of the canvas viewport, sized to its content but
// never taller than the viewport (it scrolls internally past that).
void MainWindow::positionRailOverlay(){
    if(!m_railOverlay || !m_canvas || !m_railInner) return;
    const int margin=12, w=44;
    const int natural = m_railInner->sizeHint().height() + 14;   // + overlay's top/bottom padding
    const int maxH = m_canvas->height() - 2*margin;
    const int h = qBound(80, natural, qMax(80, maxH));
    m_railOverlay->setGeometry(margin, margin, w, h);
    m_railOverlay->raise();
}

bool MainWindow::eventFilter(QObject* o, QEvent* e){
    if(o==m_welcomePanel && m_welcomePanel){                 // drag a file onto the welcome screen → open/import it
        if(e->type()==QEvent::DragEnter){ auto* de=static_cast<QDragEnterEvent*>(e);
            if(de->mimeData()->hasUrls()){ de->acceptProposedAction(); return true; } }
        else if(e->type()==QEvent::Drop){ auto* dr=static_cast<QDropEvent*>(e);
            QString path; if(dr->mimeData()->hasUrls()) for(const QUrl& u : dr->mimeData()->urls()) if(u.isLocalFile()){ path=u.toLocalFile(); break; }
            if(!path.isEmpty()){ dr->acceptProposedAction(); acceptWelcomeDrop(path); return true; } }
    }
    if(o==m_canvas && (e->type()==QEvent::Resize || e->type()==QEvent::Show)){ positionRailOverlay(); positionZoomPill(); positionRightPanel(); }
    else if(o==m_panelGrip){        // drag the left-edge handle to resize the floating panel
        // grabMouse() so the drag's MouseMove events keep coming to the grip (consuming the press in the
        // filter would otherwise skip the implicit grab and the moves would go to whatever's under the cursor).
        if(e->type()==QEvent::MouseButtonPress){ m_gripDrag=true; m_panelGrip->grabMouse(); return true; }
        if(e->type()==QEvent::MouseMove && m_gripDrag){ auto* me=static_cast<QMouseEvent*>(e);
            const int gx=m_canvas->mapFromGlobal(me->globalPosition().toPoint()).x();
            m_rightPanelW=qBound(240, m_canvas->width()-gx, qMax(240, m_canvas->width()-160)); positionRightPanel(); return true; }
        if(e->type()==QEvent::MouseButtonRelease){ m_gripDrag=false; m_panelGrip->releaseMouse(); return true; }
    }
    return QMainWindow::eventFilter(o,e);
}

// DS Canvas Toolbar — a floating glass pill at the bottom-centre of the canvas: pan · fit | guides · grid.
void MainWindow::buildZoomPill(){
    // simple painted icons (reliable + crisp; actionIcon has no hand/fit/guides/grid glyphs)
    auto pillIcon=[](const QString& kind)->QIcon{
        QPixmap pm(18,18); pm.fill(Qt::transparent);
        QPainter g(&pm); g.setRenderHint(QPainter::Antialiasing);
        QPen pen(QColor(0xC7,0xCE,0xD8),1.6); pen.setCapStyle(Qt::RoundCap); pen.setJoinStyle(Qt::RoundJoin); g.setPen(pen);
        if(kind=="pan"){ g.drawLine(9,3,9,15); g.drawLine(3,9,15,9);
            g.drawLine(9,3,7,5); g.drawLine(9,3,11,5); g.drawLine(9,15,7,13); g.drawLine(9,15,11,13);
            g.drawLine(3,9,5,7); g.drawLine(3,9,5,11); g.drawLine(15,9,13,7); g.drawLine(15,9,13,11); }
        else if(kind=="fit"){ g.drawLine(3,6,3,3); g.drawLine(3,3,6,3); g.drawLine(15,6,15,3); g.drawLine(15,3,12,3);
            g.drawLine(3,12,3,15); g.drawLine(3,15,6,15); g.drawLine(15,12,15,15); g.drawLine(15,15,12,15); }
        else if(kind=="guides"){ g.drawLine(9,2,9,16); g.drawLine(2,9,16,9); }
        else if(kind=="grid"){ g.drawLine(3,3,15,3); g.drawLine(3,9,15,9); g.drawLine(3,15,15,15);
            g.drawLine(3,3,3,15); g.drawLine(9,3,9,15); g.drawLine(15,3,15,15); }
        return QIcon(pm);
    };
    m_zoomPill=new QWidget(m_canvas); m_zoomPill->setObjectName("zoomPill");
    m_zoomPill->setAttribute(Qt::WA_StyledBackground,true); m_zoomPill->setFixedHeight(36);
    auto* h=new QHBoxLayout(m_zoomPill); h->setContentsMargins(8,0,8,0); h->setSpacing(4);
    auto mkBtn=[&](const QString& kind,const QString& tip,bool checkable){
        auto* b=new QToolButton; b->setObjectName("pillBtn"); b->setIcon(pillIcon(kind)); b->setIconSize(QSize(18,18));
        b->setToolTip(tip); b->setCheckable(checkable); b->setCursor(Qt::PointingHandCursor); b->setAutoRaise(true); return b; };
    auto* pan=mkBtn("pan","Pan",true);
    connect(pan,&QToolButton::clicked,this,[this]{ m_canvas->setTool(Tool::Pan); });
    connect(m_canvas,&Canvas::toolChanged,this,[pan](Tool t){ pan->setChecked(t==Tool::Pan); });
    auto* fit=mkBtn("fit","Fit to window",false);
    connect(fit,&QToolButton::clicked,this,[this]{ m_canvas->zoomFit(); });
    auto* sep=new QFrame; sep->setObjectName("pillSep"); sep->setFrameShape(QFrame::VLine);
    auto* guides=mkBtn("guides","Smart guides (snap to objects)",true); guides->setChecked(m_canvas->snapEnabled());
    connect(guides,&QToolButton::toggled,this,[this](bool on){ m_canvas->setSnap(on); if(m_sSnap) m_sSnap->setText(on?"Snap ●":"Snap ○"); });
    auto* grid=mkBtn("grid","Show grid",true); grid->setChecked(m_canvas->showGrid());
    connect(grid,&QToolButton::toggled,this,[this](bool on){ m_canvas->setShowGrid(on); });
    h->addWidget(pan); h->addWidget(fit); h->addWidget(sep); h->addWidget(guides); h->addWidget(grid);
    auto* sh=new QGraphicsDropShadowEffect(m_zoomPill); sh->setBlurRadius(24); sh->setOffset(0,6); sh->setColor(QColor(0,0,0,150));
    m_zoomPill->setGraphicsEffect(sh);
    positionZoomPill(); m_zoomPill->show(); m_zoomPill->raise();
}

void MainWindow::positionZoomPill(){
    if(!m_zoomPill || !m_canvas) return;
    const int w=m_zoomPill->sizeHint().width(), hh=36;
    m_zoomPill->setGeometry((m_canvas->width()-w)/2, m_canvas->height()-hh-14, w, hh);
    m_zoomPill->raise();
}

// DS Panel: a collapsible section card (surface-panel + border-subtle + radius-lg + shadow-sm) with a
// grad-chrome header — grip + UPPERCASE title + a chevron that toggles the body. Wraps any content widget.
class PanelStack;   // manages a column of sections: drag-to-reorder + detach/dock

// small painted "pop-out" icon for the detach/dock button
static QIcon detachIcon(){
    QPixmap pm(14,14); pm.fill(Qt::transparent);
    QPainter g(&pm); g.setRenderHint(QPainter::Antialiasing);
    QPen pen(QColor(0x8B,0x94,0xA3),1.3); g.setPen(pen); g.setBrush(Qt::NoBrush);
    g.drawRect(2,4,7,7); g.drawLine(9,3,12,3); g.drawLine(12,3,12,6); g.drawLine(12,3,8,7);
    return QIcon(pm);
}

// DS Panel — a collapsible section card. Header: grip + UPPERCASE title + collapse chevron + detach button.
// Dragging the header reorders the card within its PanelStack (or moves the window when detached).
class CollapsibleSection : public QWidget {
public:
    CollapsibleSection(const QString& title, QWidget* content, PanelStack* host);
    void setOpen(bool o){ m_open=o; m_body->setVisible(o); m_chev->setText(o?QStringLiteral("▾"):QStringLiteral("▸")); }
    bool isOpen() const { return m_open; }
    bool isFloating() const { return m_float!=nullptr; }
    QWidget* floatWin() const { return m_float; }
    void setFloating(QWidget* w){ m_float=w; if(m_detach) m_detach->setToolTip(w?"Dock panel":"Detach panel"); }
protected:
    bool eventFilter(QObject* o, QEvent* e) override;
private:
    PanelStack* m_host{}; QWidget* m_hdr{}; QLabel* m_chev{}; QWidget* m_body{}; QToolButton* m_detach{};
    QWidget* m_float{}; bool m_open=true;
    bool m_pressed=false, m_dragging=false; QPoint m_pressGlobal, m_winOrigin;
};

class PanelStack : public QWidget {
public:
    explicit PanelStack(QWidget* parent=nullptr) : QWidget(parent){
        setObjectName("rpPage");
        m_col=new QVBoxLayout(this); m_col->setContentsMargins(8,8,8,8); m_col->setSpacing(8); m_col->addStretch(1);
    }
    void addSection(CollapsibleSection* s){ m_sections.append(s); m_col->insertWidget(m_col->count()-1, s); }
    // live reorder while dragging a docked section: move it past siblings whose centre the cursor crossed
    void dragReorder(CollapsibleSection* s, int globalY){
        int cur=m_sections.indexOf(s); if(cur<0) return; int target=cur;
        for(int i=0;i<m_sections.size();++i){ if(i==cur) continue; CollapsibleSection* o=m_sections[i];
            int oc=o->mapToGlobal(QPoint(0,o->height()/2)).y();
            if(i<cur && globalY<oc){ target=i; break; }
            if(i>cur && globalY>oc){ target=i; } }
        if(target!=cur){ m_sections.move(cur,target); m_col->removeWidget(s); m_col->insertWidget(target,s); }
    }
    void detach(CollapsibleSection* s){
        if(s->isFloating()) return;
        m_sections.removeAll(s); m_col->removeWidget(s);
        auto* win=new QWidget(window(), Qt::Tool|Qt::FramelessWindowHint); win->setObjectName("floatPanel");
        win->setAttribute(Qt::WA_TranslucentBackground,true);   // only the rounded section card shows; corners transparent
        auto* wl=new QVBoxLayout(win); wl->setContentsMargins(0,0,0,0); wl->addWidget(s);
        s->setFloating(win); s->show();
        win->resize(258, qMax(120, s->sizeHint().height()+2));
        win->move(QCursor::pos()-QPoint(120,12)); win->show(); win->raise();
    }
    void dock(CollapsibleSection* s){
        QWidget* win=s->floatWin(); if(!win) return;
        win->layout()->removeWidget(s); s->setParent(nullptr); s->setFloating(nullptr);
        m_sections.append(s); m_col->insertWidget(m_col->count()-1, s); s->show();
        win->deleteLater();
    }
private:
    QVBoxLayout* m_col{}; QList<CollapsibleSection*> m_sections;
};

CollapsibleSection::CollapsibleSection(const QString& title, QWidget* content, PanelStack* host)
    : m_host(host), m_body(content){
    setObjectName("dsSection"); setAttribute(Qt::WA_StyledBackground,true);
    auto* v=new QVBoxLayout(this); v->setContentsMargins(0,0,0,0); v->setSpacing(0);
    m_hdr=new QWidget; m_hdr->setObjectName("dsSectionHdr"); m_hdr->setFixedHeight(30); m_hdr->setCursor(Qt::OpenHandCursor);
    auto* h=new QHBoxLayout(m_hdr); h->setContentsMargins(8,0,5,0); h->setSpacing(7);
    auto* grip=new QLabel(QStringLiteral("⋮⋮")); grip->setObjectName("dsGrip"); grip->setAttribute(Qt::WA_TransparentForMouseEvents,true);
    auto* lbl=new QLabel(title.toUpper()); lbl->setObjectName("dsSectionTitle"); lbl->setAttribute(Qt::WA_TransparentForMouseEvents,true);
    m_chev=new QLabel; m_chev->setObjectName("dsChevron"); m_chev->setAttribute(Qt::WA_TransparentForMouseEvents,true);
    m_detach=new QToolButton; m_detach->setObjectName("dsAct"); m_detach->setIcon(detachIcon()); m_detach->setIconSize(QSize(14,14));
    m_detach->setToolTip("Detach panel"); m_detach->setCursor(Qt::PointingHandCursor); m_detach->setAutoRaise(true);
    connect(m_detach,&QToolButton::clicked,this,[this]{ if(!m_host) return; if(isFloating()) m_host->dock(this); else m_host->detach(this); });
    h->addWidget(grip); h->addWidget(lbl,1); h->addWidget(m_chev); h->addWidget(m_detach);
    v->addWidget(m_hdr); v->addWidget(m_body);
    m_hdr->installEventFilter(this);
    setOpen(true);
}

bool CollapsibleSection::eventFilter(QObject* o, QEvent* e){
    if(o!=m_hdr) return QWidget::eventFilter(o,e);
    if(e->type()==QEvent::MouseButtonPress){ auto* me=static_cast<QMouseEvent*>(e);
        if(me->button()==Qt::LeftButton){ m_pressed=true; m_dragging=false; m_pressGlobal=me->globalPosition().toPoint();
            if(m_float) m_winOrigin=m_float->pos(); m_hdr->setCursor(Qt::ClosedHandCursor); } return false; }
    if(e->type()==QEvent::MouseMove && m_pressed){ auto* me=static_cast<QMouseEvent*>(e);
        const QPoint g=me->globalPosition().toPoint(), d=g-m_pressGlobal;
        if(!m_dragging && d.manhattanLength()>6) m_dragging=true;
        if(m_dragging){ if(m_float) m_float->move(m_winOrigin+d); else if(m_host) m_host->dragReorder(this, g.y()); }
        return false; }
    if(e->type()==QEvent::MouseButtonRelease){ auto* me=static_cast<QMouseEvent*>(e);
        if(me->button()==Qt::LeftButton){ const bool wasDrag=m_dragging; m_pressed=false; m_dragging=false;
            m_hdr->setCursor(Qt::OpenHandCursor); if(!wasDrag) setOpen(!m_open); } return true; }
    return QWidget::eventFilter(o,e);
}

// Prototype tab content — real web/interaction attributes bound to the selection (Shape.url / webSticky /
// webStretch, consumed by HTML export). Refreshes on selection/document change like the other panels.
class InteractionPanel : public QWidget {
public:
    InteractionPanel(Canvas* c, QWidget* parent=nullptr) : QWidget(parent), m_c(c){
        auto* v=new QVBoxLayout(this); v->setContentsMargins(12,12,12,12); v->setSpacing(12);
        m_hint=new QLabel("Select an object to add a link, sticky behaviour, or responsive sizing.");
        m_hint->setObjectName("infoHint"); m_hint->setWordWrap(true); v->addWidget(m_hint);
        m_form=new QWidget; auto* fl=new QFormLayout(m_form); fl->setContentsMargins(0,0,0,0); fl->setSpacing(9);
        fl->setLabelAlignment(Qt::AlignLeft); fl->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        m_link=new QLineEdit; m_link->setPlaceholderText("https://…");
        m_sticky=new QCheckBox("Stays on screen while scrolling");
        m_stretch=new QComboBox; m_stretch->addItems({"Fixed","Stretch width","Stretch width + height"});
        m_hide=new QSpinBox; m_hide->setRange(0,2000); m_hide->setSuffix(" px"); m_hide->setSpecialValueText("Always show"); m_hide->setToolTip("Hide this object when the browser is narrower than this width (0 = always show)");
        fl->addRow("Link URL", m_link); fl->addRow("Behaviour", m_sticky); fl->addRow("Responsive", m_stretch); fl->addRow("Hide below", m_hide);
        v->addWidget(m_form); v->addStretch();
        connect(m_link,&QLineEdit::editingFinished,this,[this]{ if(m_block)return; const QString u=m_link->text(); m_c->applyToSelection([u](Shape&s){ s.url=u; },"Link"); });
        connect(m_sticky,&QCheckBox::toggled,this,[this](bool on){ if(m_block)return; m_c->applyToSelection([on](Shape&s){ s.webSticky=on; },"Sticky"); });
        connect(m_stretch,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this](int i){ if(m_block)return; m_c->applyToSelection([i](Shape&s){ s.webStretch=i; },"Sizing"); });
        connect(m_hide,QOverload<int>::of(&QSpinBox::valueChanged),this,[this](int v){ if(m_block)return; m_c->setSelectionHideBelow(v); });
        connect(m_c,&Canvas::selectionChanged,this,[this]{ refresh(); });
        connect(m_c,&Canvas::documentChanged,this,[this]{ refresh(); });
        refresh();
    }
    void refresh(){
        Shape* s=m_c->firstSelected(); m_block=true;
        m_form->setEnabled(s!=nullptr); m_hint->setVisible(s==nullptr);
        if(s){ m_link->setText(s->url); m_sticky->setChecked(s->webSticky); m_stretch->setCurrentIndex(qBound(0,s->webStretch,2)); m_hide->setValue(qBound(0,s->webHideBelow,2000)); }
        else { m_link->clear(); m_sticky->setChecked(false); m_stretch->setCurrentIndex(0); m_hide->setValue(0); }
        m_block=false;
    }
private:
    Canvas* m_c{}; QWidget* m_form{}; QLabel* m_hint{}; QLineEdit* m_link{}; QCheckBox* m_sticky{}; QComboBox* m_stretch{}; QSpinBox* m_hide{}; bool m_block=false;
};

void MainWindow::buildDocks() {
    m_props=new PropertiesPanel(m_canvas);
    m_layers=new LayersPanel(m_canvas);
    m_colors=new ColorPanel(m_canvas);

    auto* colorGal=new ColorGallery(m_canvas);
    auto* colGalScroll=new QScrollArea; colGalScroll->setWidgetResizable(true); colGalScroll->setWidget(colorGal); colGalScroll->setFrameShape(QFrame::NoFrame);

    // DS RightDock — one dock, "Design | Prototype" tabs over collapsible section cards that can be
    // dragged to reorder and detached into floating panels (PanelStack manages both).
    auto* design=new PanelStack;
    m_secProps =new CollapsibleSection("Properties", m_props,  design);
    m_secLayers=new CollapsibleSection("Layers",     m_layers, design);
    m_secColour=new CollapsibleSection("Colour",     m_colors, design);
    design->addSection(m_secProps); design->addSection(m_secLayers); design->addSection(m_secColour);
    m_designScroll=new QScrollArea; m_designScroll->setObjectName("rpScroll"); m_designScroll->setWidgetResizable(true);
    m_designScroll->setWidget(design); m_designScroll->setFrameShape(QFrame::NoFrame);
    auto* proto=new PanelStack;
    proto->addSection(new CollapsibleSection("Interaction", new InteractionPanel(m_canvas), proto));
    auto* protoScroll=new QScrollArea; protoScroll->setObjectName("rpScroll"); protoScroll->setWidgetResizable(true);
    protoScroll->setWidget(proto); protoScroll->setFrameShape(QFrame::NoFrame);
    m_rpTabs=new QTabWidget; m_rpTabs->setObjectName("rpTabs"); m_rpTabs->setDocumentMode(true);
    m_rpTabs->addTab(m_designScroll,"Design"); m_rpTabs->addTab(protoScroll,"Prototype");
    auto* rightPanel=new QWidget; rightPanel->setObjectName("rightPanel");
    { auto* rpv=new QVBoxLayout(rightPanel); rpv->setContentsMargins(0,0,0,0); rpv->setSpacing(0); rpv->addWidget(m_rpTabs); }

    // The right panel FLOATS as a translucent glass overlay over the canvas (DS RightDock) so the artwork
    // shows through — a child of the canvas, not an opaque dock column.
    m_rightPanel=rightPanel; rightPanel->setParent(m_canvas);
    rightPanel->setAttribute(Qt::WA_StyledBackground,true);
    auto* d4=new QDockWidget("Colour Gallery",this); d4->setObjectName("colorGalleryDock"); d4->setWidget(colGalScroll);
    // Batch C galleries
    auto wrapScroll=[](QWidget* w){ auto* s=new QScrollArea; s->setWidgetResizable(true); s->setWidget(w); s->setFrameShape(QFrame::NoFrame); return s; };
    auto* d5=new QDockWidget("Fill Gallery",this); d5->setObjectName("fillGalleryDock"); d5->setWidget(wrapScroll(new FillGallery(m_canvas)));
    auto* d6=new QDockWidget("Line Gallery",this); d6->setObjectName("lineGalleryDock"); d6->setWidget(wrapScroll(new LineGallery(m_canvas)));
    auto* d7=new QDockWidget("Fonts",this); d7->setObjectName("fontsGalleryDock"); d7->setWidget(new FontsGallery(m_canvas));
    auto* d8=new QDockWidget("Name Gallery",this); d8->setObjectName("nameGalleryDock"); d8->setWidget(new NameGallery(m_canvas));
    auto* d9=new QDockWidget("Clipart",this); d9->setObjectName("clipartGalleryDock"); d9->setWidget(wrapScroll(new ClipartGallery(m_canvas)));
    auto* d10=new QDockWidget("Pages & Layers",this); d10->setObjectName("pageLayerDock"); d10->setWidget(wrapScroll(new PageLayerGallery(m_canvas)));
    auto* d11=new QDockWidget("SmartShapes",this); d11->setObjectName("smartShapeDock"); d11->setWidget(wrapScroll(new SmartShapeGallery(m_canvas)));
    auto* d12=new QDockWidget("Animation",this); d12->setObjectName("animDock"); d12->setWidget(wrapScroll(new AnimationPanel(m_canvas)));
    auto* d13=new QDockWidget("Bitmaps",this); d13->setObjectName("bitmapDock"); d13->setWidget(wrapScroll(new BitmapGallery(m_canvas)));
    for(auto* d:{d4,d5,d6,d7,d8,d9,d10,d11,d12,d13}){ d->setFeatures(QDockWidget::DockWidgetMovable|QDockWidget::DockWidgetFloatable|QDockWidget::DockWidgetClosable); d->setAllowedAreas(Qt::RightDockWidgetArea|Qt::LeftDockWidgetArea); }
    for(auto* d:{d4,d5,d6,d7,d8,d9,d10,d11,d12,d13}) addDockWidget(Qt::RightDockWidgetArea,d);
    // Galleries tab together (all hidden until opened via the Galleries menu).
    tabifyDockWidget(d4,d5); tabifyDockWidget(d5,d6); tabifyDockWidget(d6,d7); tabifyDockWidget(d7,d9); tabifyDockWidget(d9,d11); tabifyDockWidget(d11,d13);
    tabifyDockWidget(d4,d10); tabifyDockWidget(d4,d8); tabifyDockWidget(d10,d12);
    for(auto* d:{d4,d5,d6,d7,d8,d9,d10,d11,d12,d13}) d->hide();
    for(QDockWidget* d : findChildren<QDockWidget*>()) if(!d->windowTitle().isEmpty()) d->setWindowTitle(d->windowTitle().toUpper());
    // left-edge resize handle (a thin strip between the canvas and the panel)
    m_panelGrip=new QWidget(m_canvas); m_panelGrip->setObjectName("panelGrip");
    m_panelGrip->setAttribute(Qt::WA_StyledBackground,true);
    m_panelGrip->setCursor(Qt::SplitHCursor); m_panelGrip->installEventFilter(this);
    positionRightPanel(); rightPanel->show(); rightPanel->raise(); m_panelGrip->show();
}

// Pin the floating glass right panel to the right edge of the canvas (full height), plus its resize grip.
void MainWindow::positionRightPanel(){
    if(!m_rightPanel || !m_canvas) return;
    const int cw=m_canvas->width(), ch=m_canvas->height();
    const int W = qBound(240, m_rightPanelW, qMax(240, cw-160));   // LOCAL clamp — don't overwrite the member
    m_rightPanel->setGeometry(cw-W, 0, W, ch);
    if(m_panelGrip){ m_panelGrip->setGeometry(cw-W-4, 0, 8, ch); m_panelGrip->raise(); }
    m_rightPanel->raise();
    if(m_panelGrip) m_panelGrip->raise();   // keep the handle on top so the whole strip stays grabbable
}

// Reveal a Design-tab section (0=Properties 1=Layers 2=Colour): show the panel, switch to Design, expand + scroll.
void MainWindow::revealPanelSection(int which){
    if(!m_rightPanel) return;
    m_rightPanel->show(); m_rightPanel->raise();
    if(m_rpTabs) m_rpTabs->setCurrentIndex(0);
    CollapsibleSection* sec = which==1? m_secLayers : which==2? m_secColour : m_secProps;
    if(sec){ sec->setOpen(true);
        if(m_designScroll) QTimer::singleShot(0,this,[this,sec]{ m_designScroll->ensureWidgetVisible(sec); }); }
}

// File-open filter for Import Image: standard rasters + PSD, plus camera RAW when LibRaw is loaded.
static QString imageImportFilter(){
    QString f = "Images (*.png *.jpg *.jpeg *.bmp *.gif *.tif *.tiff *.webp *.psd *.heic *.heif *.avif";
    if(xara::Raw::available()) for(const QString& e : xara::Raw::extensions()) f += " *."+e;
    return f + ")";
}

void MainWindow::buildMenus() {
    m_menuBar=new QMenuBar;   // embedded into the custom DS title bar by buildTitleBar()
    m_menuBar->setObjectName("embeddedMenu");
    auto* mFile=m_menuBar->addMenu("&File");
    mFile->addAction("New",this,[this]{newFile();},QKeySequence("Ctrl+N"));
    mFile->addAction("Close Tab",this,[this]{ confirmAndCloseTab(m_activeTab); },QKeySequence("Ctrl+W"));
    mFile->addAction("New from Template…",this,[this]{
        bool ok; QString sel=QInputDialog::getItem(this,"New from Template","Template:",Canvas::templateNames(),1,false,&ok);
        if(!ok) return; int kind=qMax(0,Canvas::templateNames().indexOf(sel));
        newDocumentTab();   // open the template in its own tab (don't clobber the active document)
        m_suppressDirty=true; m_canvas->setDocument(Canvas::templateDocument(kind)); m_suppressDirty=false;
        m_canvas->refreshAll(); m_dirty=true; updateTitle();   // template = unsaved content → mark dirty (warn on close, recoverable)
        statusBar()->showMessage("Created "+sel+" — publish it via File ▸ Publish Website.",4000); });
    mFile->addAction("Open…",this,[this]{openFile();},QKeySequence("Ctrl+O"));
    mFile->addAction("Start screen…",this,[this]{ showStartScreen(); });
    mFile->addAction("Import Image…",this,[this]{ QString p=QFileDialog::getOpenFileName(this,"Import Image","",imageImportFilter(),nullptr,QFileDialog::DontUseNativeDialog); if(p.isEmpty())return; if(m_tabs.isEmpty()) ensureLandingTab();
        const bool ok = p.endsWith(".gif",Qt::CaseInsensitive) ? m_canvas->importAnimatedGif(p) : m_canvas->importImage(p);
        if(!ok) statusBar()->showMessage("Could not load "+p,4000); },QKeySequence("Ctrl+Shift+I"));
    mFile->addAction("Import RTF…",this,[this]{ QString p=QFileDialog::getOpenFileName(this,"Import RTF","","Rich Text (*.rtf)",nullptr,QFileDialog::DontUseNativeDialog); if(!p.isEmpty()){ if(m_tabs.isEmpty()) ensureLandingTab(); if(!m_canvas->importRtfFile(p)) statusBar()->showMessage("Could not import "+p,4000); else statusBar()->showMessage("Imported "+p,4000); } });
    mFile->addAction("Import Word (DOCX)…",this,[this]{ QString p=QFileDialog::getOpenFileName(this,"Import DOCX","","Word document (*.docx)",nullptr,QFileDialog::DontUseNativeDialog); if(!p.isEmpty()){ if(m_tabs.isEmpty()) ensureLandingTab(); if(!m_canvas->importDocx(p)) statusBar()->showMessage("Could not import "+p,4000); else statusBar()->showMessage("Imported "+p,4000); } });
    mFile->addAction("Import CorelDRAW (CDR)…",this,[this]{ QString p=QFileDialog::getOpenFileName(this,"Import CDR","","CorelDRAW (*.cdr)",nullptr,QFileDialog::DontUseNativeDialog); if(!p.isEmpty()){ if(m_tabs.isEmpty()) ensureLandingTab(); if(!m_canvas->importCdr(p)) statusBar()->showMessage("Could not import "+p,4000); } });
    mFile->addAction("Import SVG…",this,[this]{ QString p=QFileDialog::getOpenFileName(this,"Import SVG","","SVG (*.svg)",nullptr,QFileDialog::DontUseNativeDialog); if(!p.isEmpty()){ if(m_tabs.isEmpty()) ensureLandingTab(); if(!m_canvas->importSvgFile(p)) statusBar()->showMessage("Could not import "+p,4000); else statusBar()->showMessage("Imported "+p,4000); } });
    mFile->addAction("Import EPS/AI/PDF/DXF/WMF/EMF…",this,[this]{ QString p=QFileDialog::getOpenFileName(this,"Import Vector","","Vector (*.eps *.ai *.pdf *.dxf *.wmf *.emf)",nullptr,QFileDialog::DontUseNativeDialog); if(!p.isEmpty()){ if(m_tabs.isEmpty()) ensureLandingTab(); if(!m_canvas->importVectorFile(p)) statusBar()->showMessage("Could not import "+p,4000); else statusBar()->showMessage("Imported "+p,4000); } });
    mFile->addAction("Import EPUB…",this,[this]{ QString p=QFileDialog::getOpenFileName(this,"Import EPUB","","EPUB e-book (*.epub)",nullptr,QFileDialog::DontUseNativeDialog); if(!p.isEmpty()){ if(m_tabs.isEmpty()) ensureLandingTab(); if(!m_canvas->importEpub(p)) statusBar()->showMessage("Could not import "+p,4000); else statusBar()->showMessage("Imported "+p,4000); } });
    mFile->addAction("Save",this,[this]{saveFile();},QKeySequence("Ctrl+S"));
    mFile->addAction("Save As…",this,[this]{saveFileAs();},QKeySequence("Ctrl+Shift+S"));
    mFile->addAction("Save Compressed XAR…",this,[this]{ QString p=QFileDialog::getSaveFileName(this,"Save Compressed XAR","drawing.xar","Xara (*.xar)",nullptr,QFileDialog::DontUseNativeDialog);
        if(p.isEmpty())return; if(!p.endsWith(".xar",Qt::CaseInsensitive))p+=".xar"; if(m_tabs.isEmpty()) ensureLandingTab();
        if(m_canvas->saveCompressed(p)){ m_dirty=false; addRecentFile(p); updateTitle(); statusBar()->showMessage("Saved compressed: "+p,4000); } else statusBar()->showMessage("Compressed save failed",4000); });
    mFile->addSeparator();
    mFile->addAction("Page Setup…",this,[this]{pageSetup();});
    mFile->addAction("Page Background Colour…",this,[this]{ QColor c=ColorEditor::pick(this, m_canvas->doc().pageColor, m_canvas); if(c.isValid()) m_canvas->setPageColor(c); });
    mFile->addAction("Export…",this,[this]{exportImage();},QKeySequence("Ctrl+E"));
    mFile->addAction("Export Presentation…",this,[this]{
        QStringList tr={"Cut (no transition)","Fade","Slide","Zoom"}; bool ok2;
        QString tsel=QInputDialog::getItem(this,"Export Presentation","Slide transition:",tr,1,false,&ok2); if(!ok2)return;
        int trn=qMax(0,tr.indexOf(tsel));
        QString p=QFileDialog::getSaveFileName(this,"Export Presentation","presentation.html","HTML (*.html)",nullptr,QFileDialog::DontUseNativeDialog); if(p.isEmpty())return; if(!p.endsWith(".html",Qt::CaseInsensitive))p+=".html"; bool ok=m_canvas->exportPresentation(p,trn); statusBar()->showMessage(ok?("Exported "+p):"Export failed",4000); });
    mFile->addAction("Page Properties (Web)…",this,[this]{
        const int pg=m_canvas->currentPage(); PageProps pp=m_canvas->pageProps(pg);
        QDialog dlg(this); dlg.setWindowTitle(QString("Page %1 Properties").arg(pg+1));
        auto* form=new QFormLayout(&dlg);
        auto* eName=new QLineEdit(pp.name,&dlg); eName->setPlaceholderText("HTML file name (e.g. about → about.html; blank = auto)");
        auto* eTitle=new QLineEdit(pp.title,&dlg); eTitle->setPlaceholderText("Page <title> (blank = use the site title)");
        auto* eDesc=new QLineEdit(pp.description,&dlg); eDesc->setPlaceholderText("Meta description for this page");
        auto* eKeys=new QLineEdit(pp.keywords,&dlg); eKeys->setPlaceholderText("Meta keywords for this page");
        form->addRow("File name:",eName); form->addRow("Title:",eTitle); form->addRow("Description:",eDesc); form->addRow("Keywords:",eKeys);
        form->addRow(new QLabel("Tip: link an object to another page with the URL  page:N", &dlg));
        auto* bb=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dlg);
        form->addRow(bb); connect(bb,&QDialogButtonBox::accepted,&dlg,&QDialog::accept); connect(bb,&QDialogButtonBox::rejected,&dlg,&QDialog::reject);
        if(dlg.exec()==QDialog::Accepted){ PageProps np; np.name=eName->text().trimmed(); np.title=eTitle->text().trimmed();
            np.description=eDesc->text().trimmed(); np.keywords=eKeys->text().trimmed(); m_canvas->setPageProps(pg,np); } });
    mFile->addAction("Website Properties (SEO)…",this,[this]{
        QDialog dlg(this); dlg.setWindowTitle("Website Properties");
        auto* form=new QFormLayout(&dlg);
        auto* eTitle=new QLineEdit(m_canvas->webTitle(),&dlg);
        eTitle->setPlaceholderText("Page title (defaults to file name)");
        auto* eDesc=new QPlainTextEdit(m_canvas->webDescription(),&dlg);
        eDesc->setPlaceholderText("Search-engine description (recommended 50–160 chars)");
        eDesc->setFixedHeight(72);
        auto* eKey=new QLineEdit(m_canvas->webKeywords(),&dlg);
        eKey->setPlaceholderText("comma, separated, keywords");
        auto* eUrl=new QLineEdit(m_canvas->webBaseUrl(),&dlg); eUrl->setPlaceholderText("https://example.com/  (for canonical, og:url, sitemap.xml)");
        auto* eAuthor=new QLineEdit(m_canvas->webAuthor(),&dlg); eAuthor->setPlaceholderText("Author / site owner");
        auto* favBtn=new QPushButton(m_canvas->hasFavicon()?"Favicon set ✓ — change…":"Choose favicon…",&dlg);
        connect(favBtn,&QPushButton::clicked,&dlg,[this,favBtn]{ QString p=QFileDialog::getOpenFileName(this,"Choose Favicon","",imageImportFilter(),nullptr,QFileDialog::DontUseNativeDialog);
            if(p.isEmpty())return; QImage im(p); if(im.isNull())return; if(im.width()>256||im.height()>256) im=im.scaled(64,64,Qt::KeepAspectRatio,Qt::SmoothTransformation);
            m_canvas->setFavicon(im); favBtn->setText("Favicon set ✓ — change…"); });
        auto* cFluid=new QCheckBox("Modular (fluid) layout — page scales smoothly to the browser width",&dlg);
        cFluid->setChecked(m_canvas->fluidSite());
        auto* cPresent=new QCheckBox("Presentation — reveal content layer-by-layer (click / arrow keys advance)",&dlg);
        cPresent->setChecked(m_canvas->presentSteps());
        auto* ePw=new QLineEdit(m_canvas->webPassword(),&dlg); ePw->setPlaceholderText("Leave empty for a public site"); ePw->setEchoMode(QLineEdit::Password);
        auto* eHead=new QPlainTextEdit(m_canvas->webHeadHtml(),&dlg); eHead->setPlaceholderText("Raw HTML injected before </head> — analytics, gtag, Search-Console <meta>, custom CSS/JS"); eHead->setFixedHeight(56);
        auto* eBody=new QPlainTextEdit(m_canvas->webBodyHtml(),&dlg); eBody->setPlaceholderText("Raw HTML injected before </body> — chat widgets, cookie-consent scripts"); eBody->setFixedHeight(56);
        form->addRow("Title:",eTitle);
        form->addRow("Description:",eDesc);
        form->addRow("Keywords:",eKey);
        form->addRow("Base URL:",eUrl);
        form->addRow("Author:",eAuthor);
        form->addRow("Favicon:",favBtn);
        form->addRow("Layout:",cFluid);
        form->addRow("",cPresent);
        form->addRow("Password:",ePw);
        form->addRow("Head HTML:",eHead);
        form->addRow("Body HTML:",eBody);
        auto* bb=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dlg);
        connect(bb,&QDialogButtonBox::accepted,&dlg,&QDialog::accept);
        connect(bb,&QDialogButtonBox::rejected,&dlg,&QDialog::reject);
        form->addRow(bb);
        if(dlg.exec()!=QDialog::Accepted)return;
        m_canvas->setWebMeta(eTitle->text(),eDesc->toPlainText(),eKey->text());
        m_canvas->setWebSeoExtra(eUrl->text(),eAuthor->text());
        m_canvas->setWebPassword(ePw->text());
        m_canvas->setFluidSite(cFluid->isChecked());
        m_canvas->setPresentSteps(cPresent->isChecked());
        m_canvas->setWebRaw(eHead->toPlainText(),eBody->toPlainText());
        statusBar()->showMessage("Website properties updated — embedded on Publish Website.",4000); });
    mFile->addAction("Publish Website…",this,[this]{
        QString p=QFileDialog::getSaveFileName(this,"Publish Website","index.html","HTML (*.html)",nullptr,QFileDialog::DontUseNativeDialog);
        if(p.isEmpty())return; if(!p.endsWith(".html",Qt::CaseInsensitive))p+=".html";
        bool ok=m_canvas->exportWebsite(p); statusBar()->showMessage(ok?("Published website: "+p):"Publish failed",4000); });
    mFile->addAction("Preview in Browser",this,[this]{
        QString dir=QDir::tempPath()+"/xara_preview"; QDir().mkpath(dir); QString idx=dir+"/index.html";
        if(m_canvas->exportWebsite(idx)) QDesktopServices::openUrl(QUrl::fromLocalFile(idx));
        else statusBar()->showMessage("Preview export failed",3000); });
    mFile->addAction("Preview Web Page…",this,[this]{
        const Document& d=m_canvas->doc(); int DW=int(d.pageSize.width()), PH=int(d.pageSize.height()); if(DW<1||PH<1)return;
        QImage img(DW,PH,QImage::Format_ARGB32_Premultiplied); img.fill(d.pageColor);
        { QPainter g(&img); g.setRenderHint(QPainter::Antialiasing); g.setRenderHint(QPainter::SmoothPixmapTransform); paintDocument(g,const_cast<Document&>(d)); }
        QPixmap pm=QPixmap::fromImage(img);
        QDialog dlg(this); dlg.setWindowTitle("Web Page Preview (responsive widths)"); dlg.resize(760,720);
        auto* V=new QVBoxLayout(&dlg); auto* top=new QHBoxLayout;
        auto* wc=new QComboBox; QVector<int> widths=d.breakpoints; if(!widths.contains(DW)) widths.append(DW); std::sort(widths.begin(),widths.end());
        for(int w: widths) wc->addItem(w>=DW?QString("%1 px (full)").arg(w):QString("%1 px").arg(w), w);
        wc->setCurrentIndex(wc->count()-1);
        top->addWidget(new QLabel("Preview width:")); top->addWidget(wc); top->addStretch(1); V->addLayout(top);
        auto* lab=new QLabel; lab->setAlignment(Qt::AlignTop|Qt::AlignHCenter);
        auto* scroll=new QScrollArea; scroll->setWidget(lab); scroll->setAlignment(Qt::AlignHCenter); V->addWidget(scroll,1);
        auto refresh=[=]{ int w=wc->currentData().toInt(); QPixmap sp=pm.scaledToWidth(w,Qt::SmoothTransformation); lab->setPixmap(sp); lab->resize(sp.size()); };
        connect(wc,QOverload<int>::of(&QComboBox::currentIndexChanged),&dlg,[=](int){ refresh(); }); refresh();
        dlg.exec(); });
    mFile->addAction("Add Spot Colour…",this,[this]{
        bool ok; QString nm=QInputDialog::getText(this,"Spot Colour","Ink name (e.g. Pantone 185 C):",QLineEdit::Normal,"Spot 1",&ok); if(!ok||nm.trimmed().isEmpty())return;
        QColor c=ColorEditor::pick(this, QColor(220,0,40), m_canvas); if(!c.isValid())return;
        m_canvas->addSpotColor(nm.trimmed(),c); statusBar()->showMessage("Spot colour added — it gets its own plate in Export Colour Separations.",5000); });
    mFile->addAction("Export Colour Separations…",this,[this]{
        QString p=QFileDialog::getSaveFileName(this,"Export CMYK Separations","separations.pdf","PDF (*.pdf)",nullptr,QFileDialog::DontUseNativeDialog);
        if(p.isEmpty())return; if(!p.endsWith(".pdf",Qt::CaseInsensitive))p+=".pdf";
        bool ok=m_canvas->exportSeparations(p); statusBar()->showMessage(ok?("Wrote C/M/Y/K plates: "+p):"Separation export failed",4000); });
    mFile->addAction("Export Composite CMYK PDF…",this,[this]{
        QString p=QFileDialog::getSaveFileName(this,"Export Composite CMYK PDF","composite-cmyk.pdf","PDF (*.pdf)",nullptr,QFileDialog::DontUseNativeDialog);
        if(p.isEmpty())return; if(!p.endsWith(".pdf",Qt::CaseInsensitive))p+=".pdf";
        bool ok=m_canvas->exportCmykPdf(p); statusBar()->showMessage(ok?("Wrote composite device-CMYK PDF (with output intent): "+p):"CMYK PDF export failed",5000); });
    mFile->addAction("Export with Crop Marks…",this,[this]{
        bool ok; double bleed=QInputDialog::getDouble(this,"Crop Marks","Bleed (px):",9,0,100,0,&ok); if(!ok)return;
        QString p=QFileDialog::getSaveFileName(this,"Export with Crop Marks","print.pdf","PDF (*.pdf)",nullptr,QFileDialog::DontUseNativeDialog);
        if(p.isEmpty())return; if(!p.endsWith(".pdf",Qt::CaseInsensitive))p+=".pdf";
        bool w=m_canvas->exportWithMarks(p,bleed,18); statusBar()->showMessage(w?("Wrote print-ready PDF: "+p):"Export failed",4000); });
    mFile->addAction("Export N-up Imposition Sheet…",this,[this]{
        QDialog dlg(this); dlg.setWindowTitle("N-up Imposition"); auto* form=new QFormLayout(&dlg);
        auto* nup=new QComboBox(&dlg); nup->addItems({"2-up (2×1)","4-up (2×2)","6-up (3×2)","8-up (4×2)","9-up (3×3)"});
        form->addRow("Layout:",nup);
        auto* bb=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dlg);
        connect(bb,&QDialogButtonBox::accepted,&dlg,&QDialog::accept); connect(bb,&QDialogButtonBox::rejected,&dlg,&QDialog::reject); form->addRow(bb);
        if(dlg.exec()!=QDialog::Accepted) return;
        static const int gc[][2]={{2,1},{2,2},{3,2},{4,2},{3,3}}; const int gi=qBound(0,nup->currentIndex(),4);
        QString p=QFileDialog::getSaveFileName(this,"Export Imposition Sheet","imposition.png","PNG (*.png)",nullptr,QFileDialog::DontUseNativeDialog);
        if(p.isEmpty())return; if(!p.endsWith(".png",Qt::CaseInsensitive))p+=".png";
        QImage sheet=m_canvas->renderImposedSheet(gc[gi][0],gc[gi][1], m_canvas->pageSize()*2.0);
        statusBar()->showMessage(!sheet.isNull() && sheet.save(p)?("Wrote imposition sheet: "+p):"Imposition export failed",4000); });
    mFile->addAction("Export Booklet (saddle-stitch)…",this,[this]{
        QString p=QFileDialog::getSaveFileName(this,"Export Booklet","booklet.pdf","PDF (*.pdf)",nullptr,QFileDialog::DontUseNativeDialog);
        if(p.isEmpty())return; if(!p.endsWith(".pdf",Qt::CaseInsensitive))p+=".pdf";
        statusBar()->showMessage(m_canvas->exportBooklet(p)?("Wrote booklet imposition: "+p):"Booklet export failed",4000); });
    mFile->addAction("Export Poster (Tiled)…",this,[this]{
        QDialog dlg(this); dlg.setWindowTitle("Poster / Tiled Print"); auto* form=new QFormLayout(&dlg);
        auto* sw=new QSpinBox(&dlg); sw->setRange(100,5000); sw->setValue(794); sw->setSuffix(" px");   // ~A4 portrait @96dpi
        auto* sh=new QSpinBox(&dlg); sh->setRange(100,5000); sh->setValue(1123); sh->setSuffix(" px");
        auto* ov=new QSpinBox(&dlg); ov->setRange(0,400); ov->setValue(24); ov->setSuffix(" px");
        form->addRow("Sheet width:",sw); form->addRow("Sheet height:",sh); form->addRow("Overlap:",ov);
        form->addRow(new QLabel(QString("Page is %1×%2 px.").arg(int(m_canvas->doc().pageSize.width())).arg(int(m_canvas->doc().pageSize.height())),&dlg));
        auto* bb=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dlg);
        form->addRow(bb); connect(bb,&QDialogButtonBox::accepted,&dlg,&QDialog::accept); connect(bb,&QDialogButtonBox::rejected,&dlg,&QDialog::reject);
        if(dlg.exec()!=QDialog::Accepted) return;
        QString p=QFileDialog::getSaveFileName(this,"Export Poster","poster.pdf","PDF (*.pdf)",nullptr,QFileDialog::DontUseNativeDialog);
        if(p.isEmpty())return; if(!p.endsWith(".pdf",Qt::CaseInsensitive))p+=".pdf";
        statusBar()->showMessage(m_canvas->exportPosterPdf(p, sw->value(), sh->value(), ov->value())?("Wrote poster tiles: "+p):"Poster export failed",4000); });
    mFile->addAction("Print…",this,[this]{printDocument();},QKeySequence("Ctrl+P"));
    mFile->addSeparator();
    mFile->addAction("Quit",this,[this]{close();},QKeySequence("Ctrl+Q"));

    auto* mEdit=m_menuBar->addMenu("&Edit");
    { auto* aUndo=mEdit->addAction("Undo",this,[this]{m_canvas->undo();},QKeySequence("Ctrl+Z"));
      auto* aRedo=mEdit->addAction("Redo",this,[this]{m_canvas->redo();},QKeySequence("Ctrl+Shift+Z"));
      // Named undo/redo: reflect the pending operation each time the Edit menu opens.
      connect(mEdit,&QMenu::aboutToShow,this,[this,aUndo,aRedo]{
          const QString u=m_canvas->undoLabel(), r=m_canvas->redoLabel();
          aUndo->setText(u.isEmpty()?"Undo":QString("Undo %1").arg(u)); aUndo->setEnabled(m_canvas->canUndo());
          aRedo->setText(r.isEmpty()?"Redo":QString("Redo %1").arg(r)); aRedo->setEnabled(m_canvas->canRedo()); }); }
    mEdit->addSeparator();
    mEdit->addAction("Cut",this,[this]{m_canvas->cutSelection();},QKeySequence("Ctrl+X"));
    mEdit->addAction("Copy",this,[this]{m_canvas->copySelection();},QKeySequence("Ctrl+C"));
    mEdit->addAction("Paste",this,[this]{ if(!m_canvas->pasteClipboard() && !m_canvas->pasteRichText() && !m_canvas->pasteBitmapFromClipboard()) statusBar()->showMessage("Clipboard is empty.",3000); },QKeySequence("Ctrl+V"));
    mEdit->addAction("Paste as Rich Text",this,[this]{ if(!m_canvas->pasteRichText()) statusBar()->showMessage("No formatted text on the clipboard.",3000); },QKeySequence("Ctrl+Shift+V"));
    mEdit->addAction("Paste in Place",this,[this]{ if(!m_canvas->pasteInPlace()) statusBar()->showMessage("Nothing to paste.",3000); },QKeySequence("Ctrl+Shift+Alt+V"));
    mEdit->addAction("Paste Attributes",this,[this]{ if(!m_canvas->pasteAttributes()) statusBar()->showMessage("Copy an object, select target(s), then Paste Attributes.",4000); else statusBar()->showMessage("Pasted appearance onto the selection.",3000); });
    mEdit->addAction("Duplicate",this,[this]{m_canvas->duplicateSelection();},QKeySequence("Ctrl+D"));
    mEdit->addAction("Clone (in place)",this,[this]{m_canvas->duplicateSelection(true);},QKeySequence("Ctrl+K"));
    mEdit->addAction("Delete",this,[this]{m_canvas->deleteSelection();},QKeySequence::Delete);
    mEdit->addSeparator();
    mEdit->addAction("Select All",this,[this]{m_canvas->selectAll();},QKeySequence("Ctrl+A"));
    mEdit->addAction("Select None",this,[this]{m_canvas->selectNone();},QKeySequence("Esc"));
    mEdit->addSeparator();
    mEdit->addAction("Find && Replace…",this,[this]{ FindReplace::show(this,m_canvas); },QKeySequence("Ctrl+F"));
    mEdit->addAction("Text Styles…",this,[this]{ TextStyleDialog::show(this,m_canvas); },QKeySequence("Ctrl+Shift+Y"));
    mEdit->addAction("Character Formatting…",this,[this]{ RichTextDialog::show(this,m_canvas); },QKeySequence("Ctrl+Shift+C"));
    mEdit->addAction("Line & Letter Spacing…",this,[this]{
        double lead=1.18, track=0; xara::Shape* s=m_canvas->firstSelected(); if(s && s->type==xara::ShapeType::Text){ lead=s->lineSpacing; track=s->tracking; }
        bool ok; double L=QInputDialog::getDouble(this,"Text Spacing","Line spacing (× font height):",lead,0.3,4.0,2,&ok); if(!ok)return;
        double T=QInputDialog::getDouble(this,"Text Spacing","Letter spacing / tracking (px):",track,-20,50,1,&ok); if(!ok)return;
        m_canvas->setTextSpacing(L,T); });

    auto* mObj=m_menuBar->addMenu("A&rrange");
    mObj->addAction("Group",this,[this]{m_canvas->groupSelection();},QKeySequence("Ctrl+G"));
    mObj->addAction("Ungroup",this,[this]{m_canvas->ungroupSelection();},QKeySequence("Ctrl+Shift+G"));
    mObj->addAction("Apply ClipView (clip to top object)",this,[this]{m_canvas->applyClipView();});
    mObj->addAction("Remove ClipView",this,[this]{m_canvas->removeClipView();});
    mObj->addSeparator();
    mObj->addAction("Lock",this,[this]{m_canvas->lockSelection();},QKeySequence("Ctrl+L"));
    mObj->addAction("Unlock All",this,[this]{ for(auto& s:m_canvas->doc().shapes) s.locked=false; m_canvas->refreshAll(); });
    mObj->addSeparator();
    mObj->addAction("Create Live Copy",this,[this]{m_canvas->makeLiveCopy();},QKeySequence("Ctrl+Shift+K"));
    mObj->addAction("Soft Group",this,[this]{m_canvas->makeSoftGroup();},QKeySequence("Ctrl+Alt+G"));
    mObj->addAction("Remove Soft Group",this,[this]{m_canvas->ungroupSoftGroup();});
    mObj->addAction("Repel Text Under Object",this,[this]{
        bool any=false; m_canvas->applyToSelection([&](xara::Shape& s){ s.repel=!s.repel; any=s.repel; },"Repel text");
        m_canvas->reflowText(); m_canvas->refreshAll();
        statusBar()->showMessage(any?"Object now repels overlapping text.":"Repel removed.",3000); });
    mObj->addAction("Anchor Graphic Inline to Text…",this,[this]{
        bool ok; int pos=QInputDialog::getInt(this,"Anchor to Text","Character position in the text frame:",0,0,100000,1,&ok);
        if(ok && m_canvas->anchorSelectionToText(pos)) statusBar()->showMessage("Graphic anchored inline; it now flows with the text.",4000); });
    mObj->addSeparator();
    auto* mComb=mObj->addMenu("Combine Shapes");
    mComb->addAction("Add (Union)",this,[this]{m_canvas->combineSelection(0);},QKeySequence("Ctrl+Shift+A"));
    mComb->addAction("Subtract",this,[this]{m_canvas->combineSelection(1);},QKeySequence("Ctrl+Shift+S"));
    mComb->addAction("Intersect",this,[this]{m_canvas->combineSelection(2);},QKeySequence("Ctrl+Shift+I"));
    mComb->addAction("Difference",this,[this]{m_canvas->combineSelection(3);});
    mComb->addAction("Slice (knife)",this,[this]{ if(m_canvas->selection().size()<2){ statusBar()->showMessage("Select the object to slice plus the knife shape(s) on top.",4000); return; }
        m_canvas->sliceSelection(); statusBar()->showMessage("Sliced — the lower object was cut into inside/outside pieces by the upper outline.",4000); });
    mComb->addSeparator();
    mComb->addAction("Join Shapes",this,[this]{m_canvas->joinShapesSelection();});
    mComb->addAction("Break Apart",this,[this]{m_canvas->breakApartSelection();},QKeySequence("Ctrl+Shift+B"));
    mObj->addAction("Convert to Editable Path",this,[this]{m_canvas->convertToPath();},QKeySequence("Ctrl+Shift+P"));
    mObj->addAction("Convert to Bitmap",this,[this]{ if(m_canvas->rasterizeSelection()) statusBar()->showMessage("Selection flattened to a bitmap.",3000); else statusBar()->showMessage("Select objects to convert to a bitmap.",3000); });
    mObj->addAction("Convert Shadow/Contour to Shapes",this,[this]{ if(m_canvas->expandEffectsSelection()) statusBar()->showMessage("Shadow/contour converted to editable shapes.",3000); });
    mObj->addAction("Convert Stroke to Path",this,[this]{m_canvas->strokeToPath();});
    mObj->addAction("Roughen…",this,[this]{ if(m_canvas->selection().isEmpty()){ statusBar()->showMessage("Select a shape to roughen.",3000); return; }
        bool ok; double a=QInputDialog::getDouble(this,"Roughen","Roughness (doc units):",3,0.1,100,1,&ok); if(!ok) return;
        m_canvas->roughenSelection(a, 8); });
    mObj->addAction("Fit Text to Curve",this,[this]{m_canvas->fitTextToCurve();});
    mObj->addAction("Detach Text from Curve",this,[this]{m_canvas->detachTextFromCurve();});
    mObj->addAction("Flip Text to Other Side of Curve",this,[this]{m_canvas->toggleTextPathSide();});
    mObj->addAction("Fit Text Inside Shape",this,[this]{m_canvas->fitTextInShape();});
    { auto* aOvp=mObj->addAction("Overprint (prepress)"); aOvp->setCheckable(true);
      aOvp->setToolTip("Overprinting objects ADD their ink in colour separations instead of knocking out the plates below (e.g. black text on colour).");
      connect(mObj,&QMenu::aboutToShow,this,[this,aOvp]{ Shape* s=m_canvas->firstSelected(); aOvp->setChecked(s&&s->overprint); });
      connect(aOvp,&QAction::toggled,this,[this](bool on){ m_canvas->applyToSelection([on](Shape& s){ s.overprint=on; },"Overprint"); }); }
    mObj->addAction("Detach Text from Shape",this,[this]{m_canvas->detachTextFromShape();});
    mObj->addAction("Link Text Frames",this,[this]{ if(m_canvas->selection().size()<2){ statusBar()->showMessage("Select two or more text frames (in flow order) to link.",3000); return; }
        m_canvas->linkTextFrames(); statusBar()->showMessage("Text frames linked — the story flows through them in selection order.",3000); });
    mObj->addAction("Unlink Text Frames",this,[this]{m_canvas->unlinkTextFrames();});
    mObj->addSeparator();
    mObj->addAction("Bring to Front",this,[this]{m_canvas->toFront();},QKeySequence("Ctrl+Shift+]"));
    mObj->addAction("Raise",this,[this]{m_canvas->raiseSelection();},QKeySequence("Ctrl+]"));
    mObj->addAction("Lower",this,[this]{m_canvas->lowerSelection();},QKeySequence("Ctrl+["));
    mObj->addAction("Send to Back",this,[this]{m_canvas->toBack();},QKeySequence("Ctrl+Shift+["));
    mObj->addSeparator();
    auto* mAlign=mObj->addMenu("Align");
    const char* an[]={"Left","Centre H","Right","Top","Centre V","Bottom"};
    for(int i=0;i<6;++i) mAlign->addAction(an[i],this,[this,i]{m_canvas->alignSelection(i);});
    mAlign->addSeparator();
    mAlign->addAction("Distribute Horizontally",this,[this]{m_canvas->distributeSelection(true);});
    mAlign->addAction("Distribute Vertically",this,[this]{m_canvas->distributeSelection(false);});
    mAlign->addAction("Distribute — Equal Gaps (Horizontal)",this,[this]{m_canvas->distributeSelection(true,true);});
    mAlign->addAction("Distribute — Equal Gaps (Vertical)",this,[this]{m_canvas->distributeSelection(false,true);});
    mAlign->addSeparator();
    mAlign->addAction("Alignment…",this,[this]{ AlignDialog::show(this,m_canvas); });
    mObj->addAction("Repeat…",this,[this]{ RepeatDialog::show(this,m_canvas); });
    { auto* mFit=mObj->addMenu("Auto-Fit to Page");
      mFit->addAction("Fit Width",this,[this]{ m_canvas->autoFitToPage(false); });
      mFit->addAction("Fit Width && Height",this,[this]{ m_canvas->autoFitToPage(true); }); }
    mObj->addAction("Round Corners…",this,[this]{ bool ok=false; double r=QInputDialog::getDouble(this,"Round Corners","Corner radius (px):",8,0,500,1,&ok); if(ok) m_canvas->roundSelectionCorners(r); });
    mObj->addAction("Radial Array…",this,[this]{ bool ok=false; int n=QInputDialog::getInt(this,"Radial Array","Number of copies (incl. original):",8,2,180,1,&ok); if(!ok)return;
        double deg=QInputDialog::getDouble(this,"Radial Array","Total sweep (degrees):",360,-3600,3600,1,&ok); if(ok) m_canvas->radialArray(n, deg); });
    { auto* mLay=mObj->addMenu("Layer");   // master-page + solo layer (the model supported these; no UI before)
      mLay->addAction("Make Active Layer Master",this,[this]{ m_canvas->setLayerMaster(m_canvas->activeLayer(), true); statusBar()->showMessage("Active layer is now a master — its objects appear on every page.",4000); });
      mLay->addAction("Clear Master (Normal Layer)",this,[this]{ m_canvas->setLayerMaster(m_canvas->activeLayer(), false); });
      mLay->addAction("Solo Active Layer (toggle)",this,[this]{ m_canvas->soloLayer(m_canvas->activeLayer()); });
      { auto* aNP=mLay->addAction("Active Layer is Non-Printing"); aNP->setCheckable(true);
        connect(mLay,&QMenu::aboutToShow,this,[this,aNP]{ aNP->setChecked(!m_canvas->layerPrintable(m_canvas->activeLayer())); });
        connect(aNP,&QAction::triggered,this,[this](bool on){ m_canvas->setLayerPrintable(m_canvas->activeLayer(), !on);
          statusBar()->showMessage(on?"Active layer is non-printing — shown on screen, hidden from print/export.":"Active layer will print/export.",4000); }); } }
    auto* mTrans=mObj->addMenu("Transform");
    mTrans->addAction("Flip Horizontal",this,[this]{m_canvas->flipSelection(true);});
    mTrans->addAction("Flip Vertical",this,[this]{m_canvas->flipSelection(false);});
    mTrans->addAction("Rotate 90° CW",this,[this]{m_canvas->rotateSelection90(true);});
    mTrans->addAction("Rotate 90° CCW",this,[this]{m_canvas->rotateSelection90(false);});

    // ---- Insert: pages + objects ----
    auto* mIns=m_menuBar->addMenu("&Insert");
    mIns->addAction("New Page",this,[this]{ m_canvas->addPage(); });
    mIns->addAction("Table from CSV…",this,[this]{ QString p=QFileDialog::getOpenFileName(this,"Import CSV","","CSV files (*.csv *.tsv *.txt);;All files (*)",nullptr,QFileDialog::DontUseNativeDialog);
        if(p.isEmpty())return; if(m_canvas->importCsvAsTable(p)) statusBar()->showMessage("CSV imported as a table.",4000); });
    mIns->addAction("Duplicate Current Page",this,[this]{ m_canvas->duplicatePage(m_canvas->currentPage()); });
    mIns->addAction("Delete Current Page",this,[this]{ m_canvas->deletePage(m_canvas->currentPage()); });
    mIns->addAction("Move Page Up",this,[this]{ m_canvas->movePage(m_canvas->currentPage(),-1); });
    mIns->addAction("Move Page Down",this,[this]{ m_canvas->movePage(m_canvas->currentPage(),+1); });
    mIns->addSeparator();
    mIns->addAction("Image…",this,[this]{ QString p=QFileDialog::getOpenFileName(this,"Import Image","",imageImportFilter(),nullptr,QFileDialog::DontUseNativeDialog); if(!p.isEmpty()) m_canvas->importImage(p); });
    mIns->addAction("Symbol…",this,[this]{ SymbolDialog::show(this,m_canvas); });
    mIns->addAction("QR Code…",this,[this]{ bool ok; QString t=QInputDialog::getText(this,"QR Code","URL or text to encode:",QLineEdit::Normal,"https://",&ok);
        if(ok && !t.trimmed().isEmpty()){ if(!m_canvas->insertQrCode(t.trimmed())) statusBar()->showMessage("QR generation failed (is libqrencode installed?).",4000); } });
    mIns->addAction("Mail / Data Merge (from CSV)…",this,[this]{
        if(QMessageBox::question(this,"Mail Merge","The current page becomes a TEMPLATE — use {{FieldName}} placeholders in text.\nEach CSV row (first row = field names) makes one page. Continue?")!=QMessageBox::Yes) return;
        QString p=QFileDialog::getOpenFileName(this,"Merge Data CSV","","CSV files (*.csv *.tsv *.txt)",nullptr,QFileDialog::DontUseNativeDialog);
        if(p.isEmpty())return; if(m_canvas->mailMerge(p)) statusBar()->showMessage("Mail merge complete.",4000); });
    mIns->addAction("Web Embed…",this,[this]{
        bool ok; QStringList kinds={"YouTube video","Google Map","Contact form","Raw HTML / iframe"};
        QString k=QInputDialog::getItem(this,"Web Embed","Insert on web publish:",kinds,0,false,&ok); if(!ok)return;
        QString html;
        if(k==kinds[0]){ QString u=QInputDialog::getText(this,"YouTube","Video URL or ID:",QLineEdit::Normal,"",&ok).trimmed(); if(!ok||u.isEmpty())return;
            QString id=u; int p=u.indexOf("v="); if(p>=0){ id=u.mid(p+2); int amp=id.indexOf('&'); if(amp>0)id=id.left(amp);} else if(u.contains("youtu.be/")) id=u.section("youtu.be/",1).section('?',0,0);
            html=QString("<iframe width=\"100%%\" height=\"100%%\" src=\"https://www.youtube.com/embed/%1\" frameborder=\"0\" allowfullscreen></iframe>").arg(id.toHtmlEscaped()); }
        else if(k==kinds[1]){ QString q=QInputDialog::getText(this,"Google Map","Place / address:",QLineEdit::Normal,"",&ok).trimmed(); if(!ok||q.isEmpty())return;
            html=QString("<iframe width=\"100%%\" height=\"100%%\" style=\"border:0\" loading=\"lazy\" src=\"https://maps.google.com/maps?q=%1&output=embed\"></iframe>").arg(QString(QUrl::toPercentEncoding(q))); }
        else if(k==kinds[2]){ html="<form style=\"font-family:sans-serif\" onsubmit=\"return false\">"
            "<input placeholder=\"Name\" style=\"display:block;width:100%;margin:4px 0;padding:6px\">"
            "<input placeholder=\"Email\" style=\"display:block;width:100%;margin:4px 0;padding:6px\">"
            "<textarea placeholder=\"Message\" style=\"display:block;width:100%;margin:4px 0;padding:6px;height:80px\"></textarea>"
            "<button style=\"padding:8px 16px\">Send</button></form>"; }
        else { html=QInputDialog::getMultiLineText(this,"Raw HTML","Embed HTML:","",&ok); if(!ok||html.trimmed().isEmpty())return; }
        m_canvas->setSelectionEmbed(html); statusBar()->showMessage("Web embed placed — it appears when you Publish Website.",4000); });
    { QAction* nw=mIns->addAction("Link Opens in New Window"); nw->setCheckable(true);
      connect(mIns,&QMenu::aboutToShow,this,[this,nw]{ Shape* s=m_canvas->firstSelected(); nw->setChecked(s&&s->webNewWindow); });
      connect(nw,&QAction::toggled,this,[this](bool on){ m_canvas->applyToSelection([on](Shape& s){ s.webNewWindow=on; },"Link target"); }); }
    mIns->addAction("Buy-Now Button (e-commerce)…",this,[this]{
        if(m_canvas->selection().isEmpty()){ statusBar()->showMessage("Select an object to attach a Buy-Now button.",3000); return; }
        Shape* s=m_canvas->firstSelected(); bool ok;
        QString u=QInputDialog::getText(this,"Buy-Now Button","Checkout / product URL (empty removes it):",QLineEdit::Normal, s?s->webBuyUrl:QString(),&ok); if(!ok)return;
        m_canvas->applyToSelection([u](Shape& s){ s.webBuyUrl=u.trimmed(); },"Buy button");
        statusBar()->showMessage(u.trimmed().isEmpty()?"Buy-Now button removed.":"Buy-Now button added — appears when you Publish Website.",4000); });
    { QAction* lb=mIns->addAction("Pop-up Photo (Lightbox)"); lb->setCheckable(true);
      connect(mIns,&QMenu::aboutToShow,this,[this,lb]{ lb->setChecked(m_canvas->selectionLightbox()); });
      connect(lb,&QAction::triggered,this,[this,lb]{
        if(m_canvas->selection().isEmpty()){ statusBar()->showMessage("Select a photo or object first to make it a pop-up.",3000); lb->setChecked(false); return; }
        m_canvas->setSelectionLightbox(lb->isChecked());
        statusBar()->showMessage(lb->isChecked()?"Pop-up photo: clicking it on the published page opens it full-screen.":"Pop-up photo turned off.",4000); }); }
    mIns->addAction("Web Animation…",this,[this]{
        if(m_canvas->selection().isEmpty()){ statusBar()->showMessage("Select an object first to animate it on scroll.",3000); return; }
        QStringList kinds={"None","Fade in","Slide up","Slide down","Slide left","Slide right","Zoom in","Parallax"};
        QDialog dlg(this); dlg.setWindowTitle("Web Animation");
        auto* form=new QFormLayout(&dlg);
        auto* eKind=new QComboBox(&dlg); eKind->addItems(kinds); eKind->setCurrentIndex(qBound(0,m_canvas->selectionWebAnim(),kinds.size()-1));
        auto* eDelay=new QDoubleSpinBox(&dlg); eDelay->setRange(0,5); eDelay->setSingleStep(0.1); eDelay->setSuffix(" s"); eDelay->setDecimals(2);
        form->addRow("Effect (on scroll into view):",eKind);
        form->addRow("Start delay:",eDelay);
        auto* bb=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dlg);
        connect(bb,&QDialogButtonBox::accepted,&dlg,&QDialog::accept); connect(bb,&QDialogButtonBox::rejected,&dlg,&QDialog::reject);
        form->addRow(bb);
        if(dlg.exec()!=QDialog::Accepted)return;
        m_canvas->setSelectionWebAnim(eKind->currentIndex(),eDelay->value());
        statusBar()->showMessage(eKind->currentIndex()==0?"Web animation removed.":("Web animation set: "+kinds[eKind->currentIndex()]+" — plays on the published page."),4000); });
    mIns->addAction("Make Slider / Carousel",this,[this]{
        if(m_canvas->selectionSlider()){ m_canvas->clearSelectionSlider(); statusBar()->showMessage("Slider ungrouped — objects are separate again.",4000); return; }
        if(m_canvas->selection().size()<2){ statusBar()->showMessage("Select 2+ images/objects to combine into a slider.",3500); return; }
        m_canvas->makeSliderFromSelection();
        statusBar()->showMessage("Slider created — the selected objects cross-fade as a carousel when you Publish Website.",4500); });
    mIns->addAction("Make FlexGroup (reflows on mobile)",this,[this]{
        if(m_canvas->selectionFlex()){ m_canvas->clearSelectionFlex(); statusBar()->showMessage("FlexGroup ungrouped.",4000); return; }
        if(m_canvas->selection().size()<2){ statusBar()->showMessage("Select 2+ objects to combine into a fluid FlexGroup.",3500); return; }
        m_canvas->makeFlexFromSelection();
        statusBar()->showMessage("FlexGroup created — on Publish Website the items flow in a row that wraps as the screen narrows (below the page).",5500); });
    mIns->addAction("Video / Sound…",this,[this]{
        QString p=QFileDialog::getOpenFileName(this,"Insert Video or Sound","",
            "Media (*.mp4 *.webm *.ogv *.mov *.m4v *.mp3 *.ogg *.oga *.wav *.m4a *.aac *.gif);;All files (*)",nullptr,QFileDialog::DontUseNativeDialog);
        if(p.isEmpty())return;
        QFile mf(p); if(!mf.open(QIODevice::ReadOnly)){ statusBar()->showMessage("Could not read the media file.",3000); return; }
        QByteArray data=mf.readAll(); mf.close();
        if(data.isEmpty()){ statusBar()->showMessage("The media file is empty.",3000); return; }
        static const QMap<QString,QString> mimes={
            {"mp4","video/mp4"},{"m4v","video/mp4"},{"mov","video/quicktime"},{"webm","video/webm"},{"ogv","video/ogg"},
            {"mp3","audio/mpeg"},{"ogg","audio/ogg"},{"oga","audio/ogg"},{"wav","audio/wav"},{"m4a","audio/mp4"},{"aac","audio/aac"},
            {"gif","image/gif"}};
        QString ext=QFileInfo(p).suffix().toLower();
        QString mime=mimes.value(ext, ext.isEmpty()?"application/octet-stream":("video/"+ext));
        m_canvas->setSelectionMedia(data,mime);
        statusBar()->showMessage(QString("Inserted %1 (%2 KB) — plays with native controls when you Publish Website.").arg(QFileInfo(p).fileName()).arg(data.size()/1024),5000); });
    mIns->addAction("Table of Contents",this,[this]{
        const QString toc=m_canvas->tocText();
        if(toc.trimmed()=="Contents"){ statusBar()->showMessage("No headings found — style text as 'Heading' or add text objects to build a TOC.",4000); return; }
        Shape t; t.type=ShapeType::Text; t.text=toc; t.font=QFont("DejaVu Sans",18); t.textFrame=true; t.rect=QRectF(0,0,420,560);
        t.fill.type=FillType::Solid; t.fill.color=Qt::black; t.stroke.enabled=false; t.name="Table of Contents"; t.transform=QTransform::fromTranslate(40,40);
        m_canvas->addShapes({t},"Insert TOC");
        statusBar()->showMessage("Table of Contents inserted (headings + page numbers).",4000); });
    mIns->addAction("Tab Stop (aligned + leader)…",this,[this]{
        if(m_canvas->selection().isEmpty()){ statusBar()->showMessage("Select a text object to add a tab stop.",4000); return; }
        QDialog dlg(this); dlg.setWindowTitle("Add Tab Stop"); auto* form=new QFormLayout(&dlg);
        auto* pos=new QDoubleSpinBox(&dlg); pos->setRange(1,4000); pos->setValue(200); pos->setSuffix(" px");
        auto* align=new QComboBox(&dlg); align->addItems({"Left","Centre","Right","Decimal"}); align->setCurrentIndex(2);
        auto* lead=new QComboBox(&dlg); lead->addItems({"None","Dots ……","Dashes ----","Underscore ___"});  lead->setCurrentIndex(1);
        form->addRow("Position:",pos); form->addRow("Align:",align); form->addRow("Leader:",lead);
        auto* bb=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dlg);
        connect(bb,&QDialogButtonBox::accepted,&dlg,&QDialog::accept); connect(bb,&QDialogButtonBox::rejected,&dlg,&QDialog::reject); form->addRow(bb);
        if(dlg.exec()!=QDialog::Accepted) return;
        static const QChar leaders[]={ QChar(), QChar('.'), QChar('-'), QChar('_') };
        m_canvas->addTabStopToSelection(pos->value(), align->currentIndex(), leaders[qBound(0,lead->currentIndex(),3)]);
        statusBar()->showMessage("Tab stop added — press Tab in the text to jump to it.",4000); });
    mIns->addAction("Clear Tab Stops",this,[this]{ m_canvas->clearTabStopsOnSelection(); });
    { auto* mList=mIns->addMenu("List");
      mList->addAction("Bulleted (•)",this,[this]{ m_canvas->setSelectionListStyle(1,0); statusBar()->showMessage("Bulleted list applied.",3000); });
      mList->addAction("Numbered (1.)",this,[this]{ m_canvas->setSelectionListStyle(2,0); statusBar()->showMessage("Numbered list applied.",3000); });
      mList->addAction("Lettered (a.)",this,[this]{ m_canvas->setSelectionListStyle(3,0); statusBar()->showMessage("Lettered list applied.",3000); });
      mList->addAction("None",this,[this]{ m_canvas->setSelectionListStyle(0,0); });
      mList->addSeparator();
      mList->addAction("Start Number / Letter…",this,[this]{
        Shape* s=m_canvas->firstSelected(); int cur=(s && s->type==ShapeType::Text)? s->listStart : 1;
        bool ok; int n=QInputDialog::getInt(this,"List Start","Number the first item from (1 = A/1):",cur,1,9999,1,&ok);
        if(ok){ m_canvas->setSelectionListStart(n); statusBar()->showMessage(QString("Numbered/lettered list restarts at %1.").arg(n),3500); } }); }
    mIns->addAction("Paragraph Spacing…",this,[this]{
        if(m_canvas->selection().isEmpty()){ statusBar()->showMessage("Select a text object first.",3500); return; }
        bool ok=true; double before=QInputDialog::getDouble(this,"Paragraph Spacing","Space before paragraph (px):",0,0,400,1,&ok); if(!ok) return;
        double after=QInputDialog::getDouble(this,"Paragraph Spacing","Space after paragraph (px):",0,0,400,1,&ok); if(!ok) return;
        m_canvas->setSelectionParaSpacing(before,after); statusBar()->showMessage("Paragraph spacing applied (single-column text frames).",3500); });
    mIns->addSeparator();
    mIns->addAction("Contact Form…",this,[this]{
        bool ok; QString fields=QInputDialog::getMultiLineText(this,"Contact Form",
            "Fields — one 'Label | type' per line (type: text/email/tel/number/textarea/url/date):",
            "Name | text\nEmail | email\nMessage | textarea",&ok); if(!ok||fields.trimmed().isEmpty()) return;
        QString action=QInputDialog::getText(this,"Contact Form","Submit action (URL or mailto:you@example.com):",QLineEdit::Normal,"mailto:you@example.com",&ok); if(!ok) return;
        m_canvas->setSelectionForm(fields, action);
        statusBar()->showMessage("Contact form added — on Publish Website it becomes a real <form> with inputs.",5000); });
    mIns->addAction("Mouse-over Pop-up (trigger → panel)",this,[this]{
        if(m_canvas->selection().size()<2){ statusBar()->showMessage("Select the trigger object first, then the pop-up panel object.",4500); return; }
        m_canvas->linkPopup();
        statusBar()->showMessage("Pop-up linked — on Publish, hovering the first object reveals the second.",5000); });
    mIns->addAction("Presentation Build Order…",this,[this]{
        if(m_canvas->selection().isEmpty()){ statusBar()->showMessage("Select the object(s) to set a build (reveal) order for.",4000); return; }
        bool ok; int order=QInputDialog::getInt(this,"Presentation Build Order","Build order within the layer (0 = with the layer, 1,2,3… = reveal in sequence):",1,0,999,1,&ok); if(!ok) return;
        m_canvas->setSelectionBuildOrder(order);
        statusBar()->showMessage("Build order set — in a Presentation, these objects reveal in order within their layer.",5000); });
    mIns->addAction("Web Button…",this,[this]{
        bool ok; QString label=QInputDialog::getText(this,"Web Button","Button label:",QLineEdit::Normal,"Click me",&ok); if(!ok)return;
        QString url=QInputDialog::getText(this,"Web Button","Link URL:",QLineEdit::Normal,"https://",&ok); if(!ok)return;
        double cx=m_canvas->pageSize().width()/2-90, cy=m_canvas->pageSize().height()/2-26;
        Shape r; r.type=ShapeType::Rect; r.rect=QRectF(0,0,180,52); r.cornerRadius=10;
        r.fill.type=FillType::Linear; r.fill.stops={{0,QColor(90,160,255)},{1,QColor(40,90,200)}}; r.fill.p0=QPointF(0,0); r.fill.p1=QPointF(0,52);
        r.stroke.enabled=false; r.shadow.on=true; r.shadow.blur=8; r.shadow.offset=QPointF(0,3); r.url=url; r.name="Button"; r.transform=QTransform::fromTranslate(cx,cy);
        Shape t; t.type=ShapeType::Text; t.text=label; t.font=QFont("DejaVu Sans",17,QFont::Bold); t.fill.type=FillType::Solid; t.fill.color=Qt::white; t.stroke.enabled=false;
        t.textFrame=true; t.textAlign=1; t.rect=QRectF(0,0,180,52); t.url=url; t.transform=QTransform::fromTranslate(cx,cy+15);
        m_canvas->addShapes({r,t},"Web button"); m_canvas->makeSoftGroup(); });
    mIns->addAction("Navigation Bar…",this,[this]{
        bool ok; QString s=QInputDialog::getText(this,"Navigation Bar","Comma-separated labels:",QLineEdit::Normal,"Home, About, Products, Contact",&ok); if(!ok)return;
        QStringList labels=s.split(',',Qt::SkipEmptyParts); if(labels.isEmpty())return;
        QVector<Shape> shapes; double x=40,y=40,bw=120,gap=8;
        for(QString lab: labels){ lab=lab.trimmed(); QString anc="#"+lab.toLower().replace(' ','-');
            Shape r; r.type=ShapeType::Rect; r.rect=QRectF(0,0,bw,40); r.cornerRadius=6; r.fill.type=FillType::Solid; r.fill.color=QColor(45,55,70); r.stroke.enabled=false; r.url=anc; r.name="Nav: "+lab; r.transform=QTransform::fromTranslate(x,y);
            Shape t; t.type=ShapeType::Text; t.text=lab; t.font=QFont("DejaVu Sans",14); t.fill.type=FillType::Solid; t.fill.color=Qt::white; t.stroke.enabled=false; t.textFrame=true; t.textAlign=1; t.rect=QRectF(0,0,bw,40); t.url=anc; t.transform=QTransform::fromTranslate(x,y+11);
            shapes<<r<<t; x+=bw+gap; }
        m_canvas->addShapes(shapes,"Navigation bar"); m_canvas->makeSoftGroup(); });
    mIns->addAction("Page Nav Bar (auto-links pages)",this,[this]{
        m_canvas->addNavBar();
        statusBar()->showMessage("Nav bar added — on Publish Website it becomes a styled <nav> linking every page (current page highlighted).",5000); });
    mIns->addAction("Full-Width Background Band",this,[this]{
        if(m_canvas->selection().isEmpty()){ statusBar()->showMessage("Select a rectangle/photo to publish as a full-browser-width band.",4000); return; }
        m_canvas->toggleWebBand();
        statusBar()->showMessage("Full-width band toggled — on Publish it spans the whole browser width behind the page content.",5000); });
    mIns->addAction("Website Variant (mobile position)…",this,[this]{
        QRectF b=m_canvas->selectionBoundsDoc();
        if(b.isNull()){ statusBar()->showMessage("Select an object first to give it a mobile-layout variant.",4000); return; }
        int belowPx=600; double vx=b.left(),vy=b.top(),vw=b.width();
        { int eb; double ex,ey,ew; if(m_canvas->selectionVariant(eb,ex,ey,ew) && eb>0){ belowPx=eb; vx=ex; vy=ey; vw=ew; } }
        QDialog dlg(this); dlg.setWindowTitle("Website Variant");
        auto* form=new QFormLayout(&dlg);
        auto* sBelow=new QSpinBox(&dlg); sBelow->setRange(0,4096); sBelow->setValue(belowPx); sBelow->setSuffix(" px"); sBelow->setSpecialValueText("(no variant)");
        auto* sX=new QSpinBox(&dlg); sX->setRange(-9999,9999); sX->setValue(int(vx)); sX->setSuffix(" px");
        auto* sY=new QSpinBox(&dlg); sY->setRange(-9999,99999); sY->setValue(int(vy)); sY->setSuffix(" px");
        auto* sW=new QSpinBox(&dlg); sW->setRange(1,9999); sW->setValue(int(vw)); sW->setSuffix(" px");
        form->addRow("Apply below width:",sBelow);
        form->addRow("Mobile X:",sX); form->addRow("Mobile Y:",sY); form->addRow("Mobile width:",sW);
        auto* bb=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dlg);
        connect(bb,&QDialogButtonBox::accepted,&dlg,&QDialog::accept);
        connect(bb,&QDialogButtonBox::rejected,&dlg,&QDialog::reject);
        form->addRow(bb);
        if(dlg.exec()!=QDialog::Accepted) return;
        m_canvas->setSelectionVariant(sBelow->value(),sX->value(),sY->value(),sW->value());
        statusBar()->showMessage(sBelow->value()>0?QString("Variant set — below %1px the object moves/resizes on Publish Website.").arg(sBelow->value())
                                                   :"Variant cleared.",4500); });
    mIns->addAction("Menu Bar (dropdown)…",this,[this]{
        QString tree=m_canvas->selectionMenuTree();
        if(tree.trimmed().isEmpty())
            tree="Home | index.html\nProducts |\n  Widgets | products.html#widgets\n  Gadgets | products.html#gadgets\nAbout | about.html\nContact | contact.html\n";
        QDialog dlg(this); dlg.setWindowTitle("Menu Bar");
        auto* v=new QVBoxLayout(&dlg);
        v->addWidget(new QLabel("One item per line as  Label | link.html  — indent 2 spaces to make a dropdown submenu:",&dlg));
        auto* ed=new QPlainTextEdit(tree,&dlg); ed->setMinimumSize(420,220);
        QFont mono("monospace"); mono.setStyleHint(QFont::TypeWriter); ed->setFont(mono);
        v->addWidget(ed);
        auto* bb=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dlg);
        connect(bb,&QDialogButtonBox::accepted,&dlg,&QDialog::accept);
        connect(bb,&QDialogButtonBox::rejected,&dlg,&QDialog::reject);
        v->addWidget(bb);
        if(dlg.exec()!=QDialog::Accepted) return;
        if(m_canvas->selectionBoundsDoc().isNull()) m_canvas->addNavBar();   // no selection → host it on a new full-width bar
        m_canvas->setSelectionMenuTree(ed->toPlainText());
        statusBar()->showMessage("Menu bar set — publishes as a multi-level dropdown <nav>.",4500); });
    { auto* mField=mIns->addMenu("Field (auto-updating)");
      auto addField=[this](const QString& tok){ Shape t; t.type=ShapeType::Text; t.text=tok; t.font=QFont("DejaVu Sans",14);
          t.fill.type=FillType::Solid; t.fill.color=Qt::black; t.stroke.enabled=false;
          QRectF pr=m_canvas->doc().pageRect(0); t.transform=QTransform::fromTranslate(pr.center().x()-30, pr.bottom()-34);
          m_canvas->addShapes({t},"Insert field");
          statusBar()->showMessage("Field inserted — it resolves per page on render/export.",4000); };
      mField->addAction("Page Number",this,[addField]{ addField("{page}"); });
      mField->addAction("Page X of Y",this,[addField]{ addField("Page {page} of {pages}"); });
      mField->addAction("Date",this,[addField]{ addField("{date}"); }); }

    // ---- Utilities: galleries (dock panels) + options ----
    auto* mUtil=m_menuBar->addMenu("&Utilities");
    auto* mGal=mUtil->addMenu("Galleries");
    auto dockToggle=[&](QMenu* m, const QString& objName, const QString& label, const QString& sc){
        auto* a=m->addAction(label); a->setCheckable(true);
        if(!sc.isEmpty()) a->setShortcut(QKeySequence(sc));
        connect(a,&QAction::triggered,this,[this,objName](bool on){ if(auto* d=findChild<QDockWidget*>(objName)) d->setVisible(on); });
        // reflect current visibility when the menu opens
        connect(m,&QMenu::aboutToShow,this,[this,a,objName]{ if(auto* d=findChild<QDockWidget*>(objName)) a->setChecked(d->isVisible()); });
        return a; };
    dockToggle(mGal,"colorGalleryDock","Colour Gallery (named)","F9");
    mGal->addAction("Swatches",this,[this]{ revealPanelSection(2); });
    dockToggle(mGal,"pageLayerDock","Pages & Layers","F10");
    mGal->addAction("Object List",this,[this]{ revealPanelSection(1); });
    { auto* a=mGal->addAction("Properties / Fills"); a->setShortcut(QKeySequence("F11"));
      connect(a,&QAction::triggered,this,[this]{ if(m_rightPanel){ m_rightPanel->setVisible(!m_rightPanel->isVisible()); positionRightPanel(); } }); }
    mGal->addSeparator();
    dockToggle(mGal,"fillGalleryDock","Fill Gallery","");
    dockToggle(mGal,"lineGalleryDock","Line Gallery","");
    dockToggle(mGal,"fontsGalleryDock","Fonts Gallery","Shift+F9");
    dockToggle(mGal,"nameGalleryDock","Name Gallery","");
    dockToggle(mGal,"clipartGalleryDock","Clipart Gallery","");
    dockToggle(mGal,"smartShapeDock","SmartShapes","");
    dockToggle(mGal,"animDock","Animation","");
    dockToggle(mGal,"bitmapDock","Bitmap Gallery","");
    mUtil->addSeparator();
    mUtil->addAction("Customise Keyboard Shortcuts…",this,[this]{ customizeShortcuts(); });
    mUtil->addAction("Optimize Photos",this,[this]{ m_canvas->optimizePhotos(); });   // downsample embedded photos to display resolution
    mUtil->addAction("Edit Brush…",this,[this]{   // Xara's Edit Brush: nib rotation + scatter of the selected brush stroke
        Stroke cur=m_canvas->firstSelected()? m_canvas->firstSelected()->stroke : m_canvas->currentStroke();
        QDialog dlg(this); dlg.setWindowTitle("Edit Brush"); auto* form=new QFormLayout(&dlg);
        auto* rl=new QCheckBox("Rotate stamps to the path",&dlg); rl->setChecked(cur.brushRotateToLine);
        auto* rot=new QDoubleSpinBox(&dlg); rot->setRange(-180,180); rot->setValue(cur.brushRotate); rot->setSuffix("°");
        auto* sc=new QDoubleSpinBox(&dlg); sc->setRange(0,200); sc->setValue(cur.brushScatter); sc->setSuffix(" px");
        auto* sv=new QDoubleSpinBox(&dlg); sv->setRange(0,1); sv->setSingleStep(0.05); sv->setValue(cur.brushSizeVar);
        form->addRow(rl); form->addRow("Rotation:",rot); form->addRow("Scatter:",sc); form->addRow("Size variation:",sv);
        auto* bb=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dlg); form->addRow(bb);
        connect(bb,&QDialogButtonBox::accepted,&dlg,&QDialog::accept); connect(bb,&QDialogButtonBox::rejected,&dlg,&QDialog::reject);
        if(dlg.exec()!=QDialog::Accepted) return;
        const bool rlv=rl->isChecked(); const double rv=rot->value(), scv=sc->value(), svv=sv->value();
        m_canvas->applyToSelection([rlv,rv,scv,svv](Shape& s){ s.stroke.brushRotateToLine=rlv; s.stroke.brushRotate=rv; s.stroke.brushScatter=scv; s.stroke.brushSizeVar=svv; },"Edit brush");
        Stroke cs=m_canvas->currentStroke(); cs.brushRotateToLine=rlv; cs.brushRotate=rv; cs.brushScatter=scv; cs.brushSizeVar=svv; m_canvas->setCurrentStroke(cs); });
    { auto* aSLW=mUtil->addAction("Scale Line Widths on Resize"); aSLW->setCheckable(true);
      connect(mUtil,&QMenu::aboutToShow,this,[this,aSLW]{ aSLW->setChecked(m_canvas->scaleLineWidths()); });
      connect(aSLW,&QAction::triggered,this,[this](bool on){ m_canvas->setScaleLineWidths(on); }); }
    mUtil->addSeparator();
    auto* aSnapU=mUtil->addAction("Snap to Objects",this,[this]{ m_canvas->setSnap(!m_canvas->snapEnabled()); if(m_sSnap) m_sSnap->setText(m_canvas->snapEnabled()?"Snap ●":"Snap ○"); });
    aSnapU->setCheckable(true); aSnapU->setChecked(m_canvas->snapEnabled());
    mUtil->addAction("Word Count…",this,[this]{ int words=0,chars=0,objs=0;
        for(const auto& s: m_canvas->doc().shapes) if(s.type==ShapeType::Text){ objs++; chars+=s.text.size(); words+=s.text.split(QRegularExpression("\\s+"),Qt::SkipEmptyParts).size(); }
        QMessageBox::information(this,"Word Count",QString("Text objects: %1\nWords: %2\nCharacters: %3").arg(objs).arg(words).arg(chars)); });
    mUtil->addAction("Proofing (Spell Check)…",this,[this]{
        QStringList issues;
        for(const auto& s: m_canvas->doc().shapes){ if(s.type!=ShapeType::Text||s.text.isEmpty()) continue;
            QString nm=s.name.isEmpty()?QString("\"%1\"").arg(s.text.left(16).simplified()):s.name;
            if(s.text.contains("  ")) issues<<QString("• %1 — double space").arg(nm);
            if(s.text.contains(" ,")||s.text.contains(" .")||s.text.contains(" ;")||s.text.contains(" !")||s.text.contains(" ?")) issues<<QString("• %1 — space before punctuation").arg(nm);
            QStringList w=s.text.split(QRegularExpression("\\s+"),Qt::SkipEmptyParts);
            for(int i=1;i<w.size();++i){ QString a=w[i-1],b=w[i]; a.remove(QRegularExpression("[^A-Za-z']")); b.remove(QRegularExpression("[^A-Za-z']"));
                if(a.size()>1 && a.compare(b,Qt::CaseInsensitive)==0){ issues<<QString("• %1 — repeated word \"%2\"").arg(nm).arg(b); break; } }
            QString t=s.text.trimmed(); if(!t.isEmpty() && t[0].isLetter() && t[0].isLower()) issues<<QString("• %1 — sentence starts lower-case").arg(nm);
            QStringList bad=Speller::instance().check(s.text,12);
            if(!bad.isEmpty()) issues<<QString("• %1 — possible spelling: %2").arg(nm).arg(bad.join(", ")); }
        QString dictNote = Speller::instance().ready()
            ? QString("Spell-check dictionary: %1 words.\n\n").arg(Speller::instance().size())
            : QString("(No system dictionary found — structural checks only.)\n\n");
        QMessageBox::information(this,"Proofing", dictNote + (issues.isEmpty()? "No proofing issues found in document text." : QString("%1 issue(s):\n\n%2").arg(issues.size()).arg(issues.join('\n'))));
    });
    { auto* aLive=mUtil->addAction("Live Spell-check (squiggles)"); aLive->setCheckable(true); aLive->setChecked(m_canvas->spellLive());
      aLive->setToolTip("Underline misspelled words while editing text; right-click a squiggle for suggestions.");
      connect(aLive,&QAction::toggled,this,[this](bool on){ m_canvas->setSpellLive(on); }); }
    { auto* mCase=mUtil->addMenu("Change Case");
      mCase->addAction("UPPERCASE",this,[this]{ m_canvas->applyToSelection([](Shape& sh){ if(sh.type==ShapeType::Text) sh.text=sh.text.toUpper(); },"UPPERCASE"); });
      mCase->addAction("lowercase",this,[this]{ m_canvas->applyToSelection([](Shape& sh){ if(sh.type==ShapeType::Text) sh.text=sh.text.toLower(); },"lowercase"); });
      mCase->addAction("Title Case",this,[this]{ m_canvas->applyToSelection([](Shape& sh){ if(sh.type!=ShapeType::Text) return; QString t=sh.text.toLower(); bool cap=true; for(int i=0;i<t.size();++i){ if(t[i].isLetter()){ if(cap){ t[i]=t[i].toUpper(); cap=false; } } else cap=true; } sh.text=t; },"Title Case"); });
      mCase->addAction("Sentence case",this,[this]{ m_canvas->applyToSelection([](Shape& sh){ if(sh.type!=ShapeType::Text) return; QString t=sh.text.toLower(); bool cap=true; for(int i=0;i<t.size();++i){ if(t[i].isLetter()){ if(cap){ t[i]=t[i].toUpper(); cap=false; } } else if(t[i]=='.'||t[i]=='!'||t[i]=='?'||t[i]=='\n') cap=true; } sh.text=t; },"Sentence case"); }); }
    { auto* mQuote=mUtil->addMenu("Smart Quotes");
      auto* aType=mQuote->addAction("Curl quotes as I type"); aType->setCheckable(true); aType->setChecked(m_canvas->smartQuotes());
      connect(aType,&QAction::toggled,this,[this](bool on){ m_canvas->setSmartQuotes(on); });
      mQuote->addAction("Convert selection to smart quotes",this,[this]{ m_canvas->convertSmartQuotes(); statusBar()->showMessage("Straight quotes curled.",3000); }); }
    // OpenType features (R9): toggle HarfBuzz-shaped ligatures / small-caps / figure styles on selected point text.
    { auto* mOT=mUtil->addMenu("OpenType Features");
      struct Feat { const char* label; const char* tag; };
      static const Feat feats[] = { {"Standard Ligatures","liga"}, {"Discretionary Ligatures","dlig"},
          {"Small Capitals","smcp"}, {"Oldstyle Figures","onum"}, {"Tabular Figures","tnum"},
          {"Fractions","frac"}, {"Slashed Zero","zero"}, {"Superscript","sups"}, {"Subscript","subs"} };
      for(const Feat& ft : feats){ const QString tag=ft.tag; auto* a=mOT->addAction(ft.label); a->setCheckable(true);
          connect(a,&QAction::triggered,this,[this,tag](bool on){
              m_canvas->applyToSelection([tag,on](Shape& s){ if(s.type!=ShapeType::Text) return;
                  s.otFeatures.removeAll(tag); if(on) s.otFeatures.append(tag); }, "OpenType feature");
              m_canvas->update(); });
          connect(mOT,&QMenu::aboutToShow,this,[this,a,tag]{ Shape* s=m_canvas->firstSelected();
              a->setChecked(s && s->type==ShapeType::Text && s->otFeatures.contains(tag)); }); } }
    mUtil->addAction("Web Address (Link)…",this,[this]{
        Shape* s=m_canvas->firstSelected(); QString cur=s?s->url:QString();
        bool ok; QString u=QInputDialog::getText(this,"Web Address","Hyperlink URL for the selected object(s):",QLineEdit::Normal,cur,&ok);
        if(ok) m_canvas->applyToSelection([u](Shape& sh){ sh.url=u; },"Set link"); });
    mUtil->addAction("Adjust Photo…",this,[this]{ PhotoDialog::show(this,m_canvas); });
    mUtil->addAction("Levels…",this,[this]{ LevelsDialog::show(this,m_canvas); });
    mUtil->addAction("Scope Adjustments to Region (Mask Painter)",this,[this]{
        auto sel=m_canvas->selection();
        if(sel.isEmpty()){ statusBar()->showMessage("Select a photo, then paint a region (Region Painter or Magnetic Lasso) to scope adjustments to.",5500); return; }
        m_canvas->setPhotoMaskFromRegion(sel.first()); });
    mUtil->addAction("Clear Adjustment Mask",this,[this]{
        auto sel=m_canvas->selection(); if(sel.isEmpty()) return;
        if(m_canvas->hasPhotoMask(sel.first())){ m_canvas->clearPhotoMask(sel.first()); statusBar()->showMessage("Adjustment mask cleared — adjustments apply to the whole photo again.",3500); }
        else statusBar()->showMessage("This photo has no adjustment mask.",3000); });
    mUtil->addAction("Crop Photo (on canvas)",this,[this]{ if(m_canvas->beginCropSelection()) statusBar()->showMessage("Drag the handles to crop; double-click or Enter to apply, Esc to cancel.",6000); },QKeySequence("Ctrl+Shift+X"));
    mUtil->addAction("Straighten Photo (on canvas)",this,[this]{ if(m_canvas->beginStraightenSelection()) statusBar()->showMessage("Drag the line onto something that should be level; double-click or Enter to apply, Esc to cancel.",6000); });
    mUtil->addAction("Cutout Background (auto remove)…",this,[this]{
        if(m_canvas->selection().isEmpty()){ statusBar()->showMessage("Select a photo to cut its background out.",4000); return; }
        bool ok; int tol=QInputDialog::getInt(this,"Cutout Background","Colour tolerance (higher removes more):",40,2,200,1,&ok); if(!ok) return;
        m_canvas->cutoutBackgroundSelection(tol);
        statusBar()->showMessage("Background removed — pixels matching the photo's border colour are now transparent.",5000); });
    mUtil->addAction("Cutout Studio (guided)…",this,[this]{ CutoutStudioDialog::show(this,m_canvas); });
    mUtil->addAction("Perspective / Keystone…",this,[this]{ PerspectiveDialog::show(this,m_canvas); });
    mUtil->addAction("Mesh Gradient Fill…",this,[this]{ MeshGradientDialog::show(this,m_canvas); });
    mUtil->addAction("Recolour / Replace Colour…",this,[this]{ RecolourDialog::show(this,m_canvas); });
    mUtil->addAction("Add Transparency Stop…",this,[this]{
        bool ok; double pos=QInputDialog::getDouble(this,"Transparency Stop","Position along the gradient (0–100%):",50,0,100,0,&ok); if(!ok)return;
        double tr=QInputDialog::getDouble(this,"Transparency Stop","Transparency at this stop (0=opaque … 100=clear):",50,0,100,0,&ok); if(!ok)return;
        if(m_canvas->addTransparencyStop(pos/100.0, tr/100.0)) statusBar()->showMessage("Added a transparency stop.",3000); });
    mUtil->addAction("Gradient Profile…",this,[this]{
        bool ok; double bias=QInputDialog::getDouble(this,"Gradient Profile","Bias (-1 … +1 — shifts the midpoint):",0,-1,1,2,&ok); if(!ok)return;
        double gain=QInputDialog::getDouble(this,"Gradient Profile","Gain (-1 … +1 — eases / steepens):",0,-1,1,2,&ok); if(!ok)return;
        m_canvas->setSelectionFillProfile(bias,gain); statusBar()->showMessage("Gradient profile applied to the selection's graduation.",3500); });
    mUtil->addAction("Contour Colour Graduation…",this,[this]{
        if(m_canvas->selection().isEmpty()){ statusBar()->showMessage("Select a contoured object first.",3500); return; }
        QColor outer=QColorDialog::getColor(QColor(0,0,0,0),this,"Outer contour colour (Cancel = clear graduation)",QColorDialog::ShowAlphaChannel);
        bool ok=true; double tr=QInputDialog::getDouble(this,"Contour Transparency","Contour transparency (0 … 1):",0,0,1,2,&ok); if(!ok) return;
        m_canvas->setSelectionContourGrad(outer.isValid()?outer:QColor(), tr);
        statusBar()->showMessage(outer.isValid()?"Contour graduates from its colour to the chosen outer colour.":"Contour graduation cleared.",4000); });
    mUtil->addAction("Fractal Fill…",this,[this]{
        if(m_canvas->selection().isEmpty()){ statusBar()->showMessage("Select an object first.",3000); return; }
        QColor a=ColorEditor::pick(this, QColor(40,80,200), m_canvas); if(!a.isValid()) return;
        QColor b=ColorEditor::pick(this, QColor(240,245,255), m_canvas); if(!b.isValid()) return;
        m_canvas->applyFractalFill(a,b); statusBar()->showMessage("Applied a fractal cloud fill.",3000); });
    mUtil->addAction("Extract Colours from Photo",this,[this]{ int n=m_canvas->addPaletteFromSelectedPhoto(6); if(n>0) statusBar()->showMessage(QString("Added %1 colours to the palette").arg(n),4000); });
    mUtil->addAction("Blur Object…",this,[this]{ if(m_canvas->selection().isEmpty()){ statusBar()->showMessage("Select an object to blur.",3000); return; }
        bool ok; double r=QInputDialog::getDouble(this,"Blur","Blur radius (0 = sharp):",8,0,200,1,&ok); if(!ok) return;
        m_canvas->applyToSelection([r](xara::Shape& s){ s.blur=r; },"Blur"); });
    mUtil->addAction("Panorama Stitch (selected photos)",this,[this]{ if(m_canvas->stitchSelectedPhotos()) statusBar()->showMessage("Stitched panorama",4000); });
    mUtil->addAction("Trace Bitmap (selected photo)",this,[this]{ m_canvas->traceBitmapSelection(); });
    mUtil->addAction("Create Brush from Selection",this,[this]{ m_canvas->createBrushFromSelection(); });
    mUtil->addAction("Content-Aware Resize…",this,[this]{
        Shape* s=m_canvas->firstSelected();
        QImage src = s ? (!s->fill.origImage.isNull()? s->fill.origImage : s->fill.image) : QImage();
        if(!s || s->fill.type!=FillType::Bitmap || src.isNull()){ QMessageBox::information(this,"Content-Aware Resize","Select a photo / bitmap-filled object first."); return; }
        bool ok; int pct=QInputDialog::getInt(this,"Content-Aware Resize","New width (% of original) — salient content is preserved, not squashed:",80,20,100,5,&ok);
        if(!ok) return;
        QImage out=contentAwareResize(src, qMax(1,src.width()*pct/100), src.height());
        const double newAspect = double(out.width())/qMax(1,out.height());
        m_canvas->applyToSelection([out,newAspect](Shape& sh){ if(sh.fill.type!=FillType::Bitmap) return;
            sh.fill.origImage=out; sh.fill.image=out;
            sh.rect.setWidth(sh.rect.height()*newAspect); },"Content-aware resize");
        statusBar()->showMessage(QString("Content-aware resized to %1×%2 px").arg(out.width()).arg(out.height()),4000); });
    mUtil->addAction("Photo Info (EXIF)…",this,[this]{
        QString p=QFileDialog::getOpenFileName(this,"Photo Info","","Images (*.jpg *.jpeg *.tif *.tiff)",nullptr,QFileDialog::DontUseNativeDialog);
        if(p.isEmpty())return; auto ex=readExif(p);
        QString body; for(auto it=ex.constBegin();it!=ex.constEnd();++it) body+=QString("%1:  %2\n").arg(it.key(),-14).arg(it.value());
        QMessageBox::information(this,"Photo Info", ex.isEmpty()? "No EXIF metadata found in this file." : body); });
    mUtil->addAction("Options…",this,[this]{ OptionsDialog::show(this,m_canvas); });

    // ---- Window: bars + galleries visibility ----
    auto* mWin=m_menuBar->addMenu("&Window");
    auto barToggle=[&](const QString& objName, const QString& label){
        auto* a=mWin->addAction(label); a->setCheckable(true);
        connect(a,&QAction::triggered,this,[this,objName](bool on){ if(auto* t=findChild<QToolBar*>(objName)) t->setVisible(on); });
        connect(mWin,&QMenu::aboutToShow,this,[this,a,objName]{ if(auto* t=findChild<QToolBar*>(objName)) a->setChecked(t->isVisible()); });
        return a; };
    barToggle("topbar","Standard Toolbar");
    barToggle("infobarTb","Tool Options Bar (InfoBar)");
    barToggle("colourTb","Colour Line");
    mWin->addSeparator();
    mWin->addAction("Properties panel",this,[this]{ if(m_rightPanel){ m_rightPanel->setVisible(!m_rightPanel->isVisible()); positionRightPanel(); } });
    mWin->addAction("Layers",this,[this]{ revealPanelSection(1); });
    mWin->addAction("Colour",this,[this]{ revealPanelSection(2); });
    mWin->addSeparator();
    mWin->addAction("Fit Window",this,[this]{m_canvas->zoomFit();},QKeySequence("Ctrl+0"));

    auto* mView=m_menuBar->addMenu("&View");
    mView->addAction("Zoom In",this,[this]{m_canvas->zoomIn();},QKeySequence("Ctrl++"));
    mView->addAction("Zoom Out",this,[this]{m_canvas->zoomOut();},QKeySequence("Ctrl+-"));
    mView->addAction("Fit",this,[this]{m_canvas->zoomFit();},QKeySequence("Ctrl+0"));
    mView->addAction("100%",this,[this]{m_canvas->zoomReset();},QKeySequence("Ctrl+1"));
    mView->addAction("Zoom to Selection",this,[this]{m_canvas->zoomToSelection();},QKeySequence("Ctrl+Shift+0"));
    mView->addAction("Zoom to Drawing",this,[this]{m_canvas->zoomToDrawing();});
    mView->addAction("Previous Zoom",this,[this]{m_canvas->previousZoom();});
    mView->addSeparator();
    { auto* aOut=mView->addAction("Outline (Wireframe)"); aOut->setCheckable(true); aOut->setChecked(m_canvas->outlineView());
      aOut->setToolTip("Draw objects as hairline outlines only (screen view; exports render normally).");
      connect(aOut,&QAction::toggled,this,[this](bool on){ m_canvas->setOutlineView(on); statusBar()->showMessage(on?"Outline (wireframe) view":"Normal view",2500); }); }
    { auto* aFS=mView->addAction("Full Screen"); aFS->setCheckable(true); aFS->setShortcut(QKeySequence(Qt::Key_F11));
      connect(aFS,&QAction::toggled,this,[this,aFS](bool on){ if(on) showFullScreen(); else showNormal(); aFS->setChecked(isFullScreen()||on); }); }
    mView->addSeparator();
    auto* aRul=mView->addAction("Rulers",this,[this]{m_canvas->setShowRulers(!m_canvas->showRulers());});
    aRul->setCheckable(true); aRul->setChecked(m_canvas->showRulers()); aRul->setShortcut(QKeySequence("Ctrl+R"));
    mView->addAction("Set Ruler Origin at Selection",this,[this]{ QRectF b=m_canvas->selectionBoundsDoc();
        if(b.isNull()) statusBar()->showMessage("Select an object to set the ruler origin at its top-left.",4000);
        else m_canvas->setRulerOrigin(b.topLeft()); });
    mView->addAction("Reset Ruler Origin",this,[this]{ m_canvas->resetRulerOrigin(); });
    auto* aSnap=mView->addAction("Snap to objects",this,[this]{ m_canvas->setSnap(!m_canvas->snapEnabled()); if(m_sSnap) m_sSnap->setText(m_canvas->snapEnabled()?"Snap ●":"Snap ○"); });
    aSnap->setCheckable(true); aSnap->setChecked(m_canvas->snapEnabled());
    mView->addSeparator();
    auto* aGrid=mView->addAction("Show Grid",this,[this](bool on){ m_canvas->setShowGrid(on); });
    aGrid->setCheckable(true); aGrid->setChecked(m_canvas->showGrid()); aGrid->setShortcut(QKeySequence("Ctrl+Shift+R"));
    auto* aSnapG=mView->addAction("Snap to Grid",this,[this](bool on){ m_canvas->setSnapToGrid(on); });
    aSnapG->setCheckable(true); aSnapG->setChecked(m_canvas->snapToGrid());
    auto* aIso=mView->addAction("Isometric Grid",this,[this](bool on){ m_canvas->setGridIso(on); if(on) m_canvas->setShowGrid(true); });
    aIso->setCheckable(true); aIso->setChecked(m_canvas->gridIso());
    auto* aProof=mView->addAction("CMYK Soft Proof",this,[this](bool on){ m_canvas->setSoftProof(on);
        const bool icc=xara::Icc::instance().available();
        statusBar()->showMessage(on?(icc?("CMYK soft-proof on — ICC profile: "+QFileInfo(xara::Icc::instance().currentCmykProfile()).fileName())
                                         :"CMYK soft-proof on (approximate — no Little-CMS)"):"CMYK soft-proof off",3500); });
    aProof->setCheckable(true); aProof->setChecked(m_canvas->softProof()); aProof->setShortcut(QKeySequence("Ctrl+Shift+P"));
    { auto* aGamut=mView->addAction("Gamut Warning",this,[this](bool on){ m_canvas->setGamutWarn(on);
        statusBar()->showMessage(on?"Gamut warning on — out-of-CMYK-gamut areas shown grey":"Gamut warning off",3500); });
      aGamut->setCheckable(true); aGamut->setChecked(m_canvas->gamutWarn()); }
    { auto* aTac=mView->addAction("Ink-Limit (TAC) Warning",this,[this](bool on){ m_canvas->setTacWarn(on);
        statusBar()->showMessage(on?QString("Ink-limit warning on — areas over %1%% total ink shown green").arg(m_canvas->tacLimit()):"Ink-limit warning off",3500); });
      aTac->setCheckable(true); aTac->setChecked(m_canvas->tacWarn());
      mView->addAction("Set Ink Limit…",this,[this]{ bool ok; int v=QInputDialog::getInt(this,"Ink Limit","Total-area-coverage limit (%):",m_canvas->tacLimit(),100,400,10,&ok); if(ok) m_canvas->setTacLimit(v); }); }
    { auto* mProf=mView->addMenu("CMYK Profile");   // choose the ICC print profile for proof + separations
      auto rebuild=[this,mProf]{ mProf->clear();
        auto& icc=xara::Icc::instance();
        if(!icc.available()){ auto* a=mProf->addAction("(Little-CMS not available — using naive CMYK)"); a->setEnabled(false); return; }
        auto* grp=new QActionGroup(mProf); const QString cur=icc.currentCmykProfile();
        for(const QString& p: icc.cmykProfiles()){ auto* a=mProf->addAction(QFileInfo(p).fileName()); a->setCheckable(true); grp->addAction(a);
            if(p==cur) a->setChecked(true);
            connect(a,&QAction::triggered,this,[this,p]{ if(xara::Icc::instance().setCmykProfile(p)){ m_canvas->update();
                statusBar()->showMessage("CMYK profile: "+QFileInfo(p).fileName(),3000); } }); }
        mProf->addSeparator();
        mProf->addAction("Open Profile…",this,[this]{ QString p=QFileDialog::getOpenFileName(this,"Open ICC CMYK Profile","/usr/share/color/icc","ICC profiles (*.icc *.icm)",nullptr,QFileDialog::DontUseNativeDialog);
            if(p.isEmpty()) return; if(xara::Icc::instance().setCmykProfile(p)){ m_canvas->update(); statusBar()->showMessage("CMYK profile: "+QFileInfo(p).fileName(),3000); }
            else QMessageBox::warning(this,"CMYK Profile","Not a usable CMYK ICC profile."); });
        mProf->addSeparator();
        auto* mIntent=mProf->addMenu("Rendering Intent");
        auto* ig=new QActionGroup(mIntent); const QStringList inames={"Perceptual","Relative Colorimetric","Saturation","Absolute Colorimetric"};
        for(int i=0;i<inames.size();++i){ auto* a=mIntent->addAction(inames[i]); a->setCheckable(true); ig->addAction(a); if(i==icc.renderingIntent()) a->setChecked(true);
            connect(a,&QAction::triggered,this,[this,i]{ auto& ic=xara::Icc::instance(); ic.setRenderingIntent(i, ic.blackPointCompensation()); m_canvas->update(); statusBar()->showMessage("Rendering intent updated.",2500); }); }
        auto* bpcA=mProf->addAction("Black Point Compensation"); bpcA->setCheckable(true); bpcA->setChecked(icc.blackPointCompensation());
        connect(bpcA,&QAction::toggled,this,[this](bool on){ auto& ic=xara::Icc::instance(); ic.setRenderingIntent(ic.renderingIntent(), on); m_canvas->update(); statusBar()->showMessage(on?"Black point compensation on.":"Black point compensation off.",2500); }); };
      connect(mProf,&QMenu::aboutToShow,this,rebuild); rebuild(); }
    auto* mUnits=mView->addMenu("Units");
    { const char* un[]={"Pixels","Millimetres","Centimetres","Inches","Points"}; auto* ug=new QActionGroup(this);
      for(int i=0;i<5;++i){ auto* a=mUnits->addAction(un[i]); a->setCheckable(true); ug->addAction(a); if(i==0)a->setChecked(true);
        connect(a,&QAction::triggered,this,[this,i]{ m_canvas->setUnits(Canvas::Units(i)); if(auto*c=findChild<QComboBox*>("unitCombo"))c->setCurrentIndex(i); }); } }
    mView->addSeparator();
    auto* mGuides=mView->addMenu("Guides");
    mGuides->addAction("Add Horizontal Guide…",this,[this]{ bool ok; double y=QInputDialog::getDouble(this,"Horizontal Guide","Y position (px):",m_canvas->pageSize().height()/2,-1e5,1e5,1,&ok); if(ok) m_canvas->addGuide(true,y); });
    mGuides->addAction("Add Vertical Guide…",this,[this]{ bool ok; double x=QInputDialog::getDouble(this,"Vertical Guide","X position (px):",m_canvas->pageSize().width()/2,-1e5,1e5,1,&ok); if(ok) m_canvas->addGuide(false,x); });
    mGuides->addAction("Clear Guides",this,[this]{ m_canvas->clearGuides(); });

    auto* mHelp=m_menuBar->addMenu("&Help");
    mHelp->addAction("Load Sample Artwork",this,[this]{
        newDocumentTab(); seedDemo();
        m_dirty=true; m_recoveryDirty=true; refreshTabText(m_activeTab); updateTitle(); });   // on-demand demo (no longer auto-loaded)
    mHelp->addSeparator();
    mHelp->addAction("About",this,[this]{about();});

    // Record factory shortcuts, then apply any user overrides (Batch R5).
    captureShortcutDefaults();
    applyShortcutOverrides();
}

// ---- file ops --------------------------------------------------------------
void MainWindow::newFile(){ newDocumentTab(); snapshotRecovery(); }   // File▸New opens a fresh document tab
void MainWindow::openFile(){ QString p=QFileDialog::getOpenFileName(this,"Open","","Xara (*.xar)",nullptr,QFileDialog::DontUseNativeDialog); if(p.isEmpty()) return;
    if(m_tabs.isEmpty()) ensureLandingTab();   // opened from the empty state (title-bar File menu) → materialise a tab
    // Reuse the current tab only if it is a pristine blank Untitled; otherwise open the file in a new tab.
    const bool pristine = !m_dirty && m_canvas->filePath().isEmpty() && m_canvas->doc().shapes.isEmpty();
    if(!pristine) newDocumentTab();
    m_suppressDirty=true; const bool ok=m_canvas->load(p); m_suppressDirty=false;
    if(ok){ m_dirty=false; addRecentFile(p); updateTitle(); enterDocState(); } snapshotRecovery(); }
bool MainWindow::saveFile(){ if(m_tabs.isEmpty()) ensureLandingTab();   // Save from the empty state → materialise + reveal the (blank) document, don't act on the hidden canvas
    QString p=m_canvas->filePath(); if(p.isEmpty()) return saveFileAs(); m_canvas->save(p); m_dirty=false; addRecentFile(p); updateTitle(); snapshotRecovery(); return true; }
bool MainWindow::saveFileAs(){ if(m_tabs.isEmpty()) ensureLandingTab(); QString p=QFileDialog::getSaveFileName(this,"Save As","drawing.xar","Xara (*.xar)",nullptr,QFileDialog::DontUseNativeDialog); if(p.isEmpty())return false; if(!p.endsWith(".xar",Qt::CaseInsensitive)) p+=".xar"; m_canvas->save(p); m_dirty=false; addRecentFile(p); updateTitle(); snapshotRecovery(); return true; }

// ---- recent files + Start screen (DS4) ----
QStringList MainWindow::recentFiles() const {
    QStringList out;
    for(const QString& p : QSettings("XarQ","XarQ").value("recentFiles").toStringList())
        if(QFile::exists(p) && !out.contains(p)) out<<p;
    return out;
}
void MainWindow::addRecentFile(const QString& path){
    if(path.isEmpty()) return;
    QSettings s("XarQ","XarQ"); QStringList r=s.value("recentFiles").toStringList();
    r.removeAll(path); r.prepend(path); while(r.size()>16) r.removeLast();
    s.setValue("recentFiles", r);
}
void MainWindow::openPath(const QString& p){
    if(p.isEmpty() || !QFile::exists(p)) return;
    if(m_tabs.isEmpty()) ensureLandingTab();   // empty state (welcome shown, no tab yet) → materialise a tab to load into
    const bool pristine = !m_dirty && m_canvas->filePath().isEmpty() && m_canvas->doc().shapes.isEmpty();
    if(!pristine) newDocumentTab();
    const bool svg = p.endsWith(".svg",Qt::CaseInsensitive);
    m_suppressDirty=true; bool ok = svg ? (m_canvas->newDocument(), m_canvas->importSvgFile(p)) : m_canvas->load(p); m_suppressDirty=false;
    if(ok){ if(!svg){ m_dirty=false; addRecentFile(p); } updateTitle(); snapshotRecovery(); enterDocState(); }
}
void MainWindow::newFromTemplate(int kind){
    newDocumentTab();
    m_suppressDirty=true; m_canvas->setDocument(Canvas::templateDocument(kind)); m_suppressDirty=false;
    m_canvas->refreshAll(); m_dirty = (kind!=0); updateTitle();   // Blank(0) stays clean; content templates are unsaved
}
void MainWindow::showStartScreen(){ StartScreen::show(this); }
// Post-boot launch dialogs: restore any crashed-out work, then (on a clean
// interactive launch with the pref on) show the welcome screen. Called after the
// branded splash finishes, or directly when the splash is suppressed.
void MainWindow::runStartupSequence(){
    offerRecovery();                       // may restore leftover work into one or more tabs
    if(!m_tabs.isEmpty()){ enterDocState(); return; }   // recovery brought documents back → show the workspace
    // Nothing to restore: show the welcome screen unless the user unticked "Show this screen on startup"
    // (startscreen.cpp persists it as showWelcome); when off, drop straight into a blank landing tab.
    if(QSettings("XarQ","XarQ").value("showWelcome",true).toBool()) enterEmptyState();
    else ensureLandingTab();
}

// ---- empty state: the welcome screen merged into the canvas area -----------
// Show/hide every piece of document chrome. The title bar is deliberately left alone (it hosts the File
// menu, which must stay reachable from the empty state). Guards each widget for null.
void MainWindow::setDocChromeVisible(bool on){
    auto vis=[](QWidget* w,bool s){ if(w) w->setVisible(s); };
    vis(m_tabBar,on); vis(m_railOverlay,on); vis(m_rightPanel,on); vis(m_panelGrip,on); vis(m_zoomPill,on);
    vis(findChild<QToolBar*>("infobarTb"),on);
    vis(findChild<QToolBar*>("colourTb"),on);
    vis(findChild<QToolBar*>("topbar"),on);
    vis(statusBar(),on);
    if(on){ positionRailOverlay(); positionRightPanel(); positionZoomPill(); }
}
void MainWindow::enterDocState(){
    if(m_centerStack) m_centerStack->setCurrentIndex(0);   // page 0 = canvas
    setDocChromeVisible(true);
}
void MainWindow::enterEmptyState(){
    if(!m_centerStack) return;
    // Rebuild the welcome panel each time so its recent-files list is always fresh.
    if(m_welcomePanel){ m_centerStack->removeWidget(m_welcomePanel); m_welcomePanel->deleteLater(); m_welcomePanel=nullptr; }
    m_welcomePanel=StartScreen::buildPanel(this,[this]{ /* a choice was made; the new/open path calls enterDocState() itself */ });
    m_welcomePanel->setAcceptDrops(true); m_welcomePanel->installEventFilter(this);   // accept a file dropped onto the welcome screen
    m_centerStack->addWidget(m_welcomePanel);
    m_centerStack->setCurrentWidget(m_welcomePanel);
    setDocChromeVisible(false);
    m_activeTab=-1; updateTitle();
}
// Create the first document tab from the current canvas state, but only if none exists yet. At launch the
// landing tab is deferred (so the welcome screen isn't shown over a blank "Untitled"); it's materialised
// here once the user picks a document/template/file — or as a blank fallback when they skip the welcome.
void MainWindow::ensureLandingTab(){
    if(!m_tabs.isEmpty()) return;
    m_tabs.append(DocTab{ m_canvas->captureSession(), m_dirty });
    m_activeTab=0;
    { QSignalBlocker b(m_tabBar); m_tabBar->addTab(tabTitleFor(m_canvas->filePath(), m_dirty)); m_tabBar->setCurrentIndex(0); }
    updateTitle();
    enterDocState();   // headless/deferred path: reveal the canvas page + document chrome
}
// A file dropped onto the welcome screen: open native docs / SVGs as a document, else materialise a blank
// tab and import the file into it (mirrors the canvas drop dispatch). Always leaves the empty state.
void MainWindow::acceptWelcomeDrop(const QString& path){
    if(path.isEmpty()) return;
    if(path.endsWith(".xar",Qt::CaseInsensitive) || path.endsWith(".svg",Qt::CaseInsensitive)){ openPath(path); return; }
    if(m_tabs.isEmpty()) ensureLandingTab();                 // reveal a blank workspace to import into
    if(!m_canvas->importImage(path) && !m_canvas->dropImportByExtension(path))
        statusBar()->showMessage("Could not open "+path,4000);
}

// ---- multi-document tabs (R6) ----------------------------------------------
void MainWindow::setupTabBar(QWidget*& centralOut){
    m_tabBar=new QTabBar; m_tabBar->setObjectName("docTabBar");
    m_tabBar->setDocumentMode(true); m_tabBar->setTabsClosable(true); m_tabBar->setExpanding(false);
    m_tabBar->setUsesScrollButtons(true); m_tabBar->setElideMode(Qt::ElideRight);
    connect(m_tabBar,&QTabBar::currentChanged,this,[this](int i){ switchToTab(i); });   // programmatic changes are QSignalBlocker-wrapped
    connect(m_tabBar,&QTabBar::tabCloseRequested,this,[this](int i){ confirmAndCloseTab(i); });
    auto* container=new QWidget;
    auto* v=new QVBoxLayout(container); v->setContentsMargins(0,0,0,0); v->setSpacing(0);
    v->addWidget(m_tabBar); v->addWidget(m_canvas,1);
    // Wrap the tabbar+canvas as page 0 of a stack; the embedded welcome panel becomes a separate page
    // (added lazily in enterEmptyState). Default to the canvas page so headless/render paths are unaffected.
    m_centerStack=new QStackedWidget;
    m_centerStack->addWidget(container);
    m_centerStack->setCurrentIndex(0);
    centralOut=m_centerStack;
}
int MainWindow::tabCount() const { return m_tabs.size(); }
QString MainWindow::tabTitleFor(const QString& path, bool dirty) const {
    const QString name = path.isEmpty()? QStringLiteral("Untitled") : QFileInfo(path).fileName();
    return dirty ? name+" *" : name;
}
void MainWindow::refreshTabText(int idx){
    if(!m_tabBar || idx<0 || idx>=m_tabs.size() || idx>=m_tabBar->count()) return;
    const bool dirty = (idx==m_activeTab)? m_dirty : m_tabs[idx].dirty;
    const QString path = (idx==m_activeTab)? m_canvas->filePath() : m_tabs[idx].session.path;
    QSignalBlocker b(m_tabBar); m_tabBar->setTabText(idx, tabTitleFor(path,dirty));
}
void MainWindow::stashActiveTab(){
    if(m_activeTab<0 || m_activeTab>=m_tabs.size()) return;
    m_tabs[m_activeTab].session = m_canvas->captureSession();
    m_tabs[m_activeTab].dirty   = m_dirty;
}
void MainWindow::newDocumentTab(){
    stashActiveTab();
    m_suppressDirty=true; m_canvas->newDocument(); m_suppressDirty=false;
    m_tabs.append(DocTab{ m_canvas->captureSession(), false });
    m_activeTab=m_tabs.size()-1; m_dirty=false;
    { QSignalBlocker b(m_tabBar); m_tabBar->addTab(tabTitleFor(QString(),false)); m_tabBar->setCurrentIndex(m_activeTab); }
    updateTitle();
    enterDocState();   // a new document is now open → leave the empty state if we were in it
}
void MainWindow::switchToTab(int idx){
    if(idx<0 || idx>=m_tabs.size() || idx==m_activeTab) return;
    stashActiveTab();
    m_activeTab=idx;
    m_suppressDirty=true; m_canvas->loadSession(m_tabs[idx].session); m_suppressDirty=false;
    m_dirty=m_tabs[idx].dirty;
    { QSignalBlocker b(m_tabBar); m_tabBar->setCurrentIndex(idx); }
    updateTitle();
    statusBar()->showMessage(tabTitleFor(m_canvas->filePath(),false),1500);
}
// UI entry point: confirm if dirty, then close. (closeTab itself is the unconditional mechanism, so it stays testable headless.)
void MainWindow::confirmAndCloseTab(int idx){
    if(idx<0 || idx>=m_tabs.size()) return;
    const bool dirty = (idx==m_activeTab)? m_dirty : m_tabs[idx].dirty;
    if(dirty){ const auto a=QMessageBox::question(this,"Close Document",
        "This document has unsaved changes. Close it anyway?",QMessageBox::Yes|QMessageBox::No,QMessageBox::No);
        if(a!=QMessageBox::Yes) return; }
    closeTab(idx);
}
void MainWindow::closeTab(int idx){
    if(idx<0 || idx>=m_tabs.size()) return;
    if(m_tabs.size()==1){                                  // never zero tabs — reset the sole tab to a blank document
        m_suppressDirty=true; m_canvas->newDocument(); m_suppressDirty=false;
        m_tabs[0]=DocTab{ m_canvas->captureSession(), false }; m_dirty=false; m_activeTab=0;
        refreshTabText(0); updateTitle(); snapshotRecovery(); return;
    }
    if(idx==m_activeTab){                                  // close the visible tab → activate a neighbour
        const int nxt = (idx==m_tabs.size()-1)? idx-1 : idx+1;
        m_suppressDirty=true; m_canvas->loadSession(m_tabs[nxt].session); m_suppressDirty=false;
        m_dirty=m_tabs[nxt].dirty;
        m_tabs.remove(idx);
        m_activeTab = (nxt>idx)? nxt-1 : nxt;              // the neighbour shifts left if it was after idx
    } else {                                              // close a background tab → active stays, maybe re-indexes
        if(idx<m_activeTab) m_activeTab--;
        m_tabs.remove(idx);
    }
    { QSignalBlocker b(m_tabBar); m_tabBar->removeTab(idx); m_tabBar->setCurrentIndex(m_activeTab); }
    updateTitle();
}

// Autosave + crash recovery (Gap 7, made tab-aware in R6): each dirty tab gets its own recovery
// snapshot recovery-<k>.xar, with recovery.manifest listing "<snapshotFile>\t<realPath>" per line.
QString MainWindow::recoveryDir() const {
    QString dir=QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if(dir.isEmpty()) dir=QDir::tempPath(); QDir().mkpath(dir);
    return dir;
}
QString MainWindow::recoveryPath() const { return recoveryDir()+"/recovery.xar"; }        // legacy single-slot (restore-only)
QString MainWindow::recoveryManifest() const { return recoveryDir()+"/recovery.manifest"; }

// Snapshot every dirty tab to its own recovery file (writing the *session* document, so inactive tabs
// are preserved too) and rewrite the manifest. Crucially this NEVER touches m_dirty or the canvas file
// path — a recovery snapshot must not be mistaken for a real save.
void MainWindow::snapshotRecovery(){
    stashActiveTab();                                        // fold the live active document into m_tabs
    int n=0; QStringList manifest;
    for(int i=0;i<m_tabs.size();++i){ const DocTab& t=m_tabs[i];
        const bool dirty = (i==m_activeTab)? m_dirty : t.dirty;
        if(!dirty || (t.session.doc.shapes.isEmpty() && t.session.path.isEmpty())) continue;   // skip blank untitled, but keep an emptied *saved* doc
        const QString rp=recoveryDir()+QStringLiteral("/recovery-%1.xar").arg(n);
        if(xara::saveXar(t.session.doc, rp)){ manifest << (rp+"\t"+t.session.path); ++n; }
    }
    for(int k=n;;++k){ const QString rp=recoveryDir()+QStringLiteral("/recovery-%1.xar").arg(k); if(!QFile::exists(rp)) break; QFile::remove(rp); }
    QFile::remove(recoveryPath());                          // retire the legacy single file if present
    if(n>0){ QFile f(recoveryManifest()); if(f.open(QIODevice::WriteOnly|QIODevice::Text)) f.write(manifest.join('\n').toUtf8()); }
    else QFile::remove(recoveryManifest());
    m_recoveryDirty=false;
}
void MainWindow::clearRecovery(){
    QFile::remove(recoveryManifest()); QFile::remove(recoveryPath());
    for(int k=0;;++k){ const QString rp=recoveryDir()+QStringLiteral("/recovery-%1.xar").arg(k); if(!QFile::exists(rp)) break; QFile::remove(rp); }
    m_recoveryDirty=false;
}
void MainWindow::autosave(){
    if(!m_recoveryDirty) return;
    snapshotRecovery();
    statusBar()->showMessage("Autosaved recovery snapshot.",1500);
}
// A clean exit means no crash — discard the recovery snapshots (they exist only to survive a crash),
// but first warn if any tab still has unsaved work.
void MainWindow::closeEvent(QCloseEvent* e){
    stashActiveTab();
    int dirtyCount=0; for(int i=0;i<m_tabs.size();++i) if((i==m_activeTab)? m_dirty : m_tabs[i].dirty) ++dirtyCount;
    if(dirtyCount>0){
        const auto a=QMessageBox::question(this,"Quit",
            QString("%1 document%2 %3 unsaved changes. Quit without saving?")
                .arg(dirtyCount).arg(dirtyCount==1?"":"s").arg(dirtyCount==1?"has":"have"),
            QMessageBox::Yes|QMessageBox::No, QMessageBox::No);
        if(a!=QMessageBox::Yes){ e->ignore(); return; }
    }
    clearRecovery();
    e->accept();
}
void MainWindow::offerRecovery(){
    QVector<QPair<QString,QString>> entries;                 // (snapshotFile, realPath)
    { QFile f(recoveryManifest());
      if(f.exists() && f.open(QIODevice::ReadOnly|QIODevice::Text))
        for(const QString& ln: QString::fromUtf8(f.readAll()).split('\n',Qt::SkipEmptyParts)){
            const QString rp=ln.section('\t',0,0), orig=ln.section('\t',1,1);
            if(QFile::exists(rp)) entries.append({rp,orig}); } }
    if(entries.isEmpty() && QFile::exists(recoveryPath())) entries.append({recoveryPath(),QString()});  // legacy format
    if(entries.isEmpty()) return;
    const auto ans=QMessageBox::question(this,"Recover Work",
        QString("XarQ found %1 unsaved document%2 from a previous session.\nRecover %3?")
            .arg(entries.size()).arg(entries.size()==1?"":"s").arg(entries.size()==1?"it":"them"),
        QMessageBox::Yes|QMessageBox::No, QMessageBox::Yes);
    if(ans!=QMessageBox::Yes){ clearRecovery(); return; }
    bool first=true;
    for(const auto& e: entries){
        Document d; d.pageSize=m_canvas->doc().pageSize;
        if(!xara::loadXar(d, e.first)) continue;
        // Reuse the lone startup tab for the first recovered doc (it holds only the throwaway seeded demo).
        const bool reuseBlank = first && tabCount()==1 && !m_dirty && m_canvas->filePath().isEmpty();
        if(!reuseBlank) newDocumentTab();
        m_suppressDirty=true; m_canvas->setDocument(d, e.second); m_suppressDirty=false;
        m_dirty=true; refreshTabText(m_activeTab); updateTitle();
        first=false;
    }
    statusBar()->showMessage("Recovered unsaved work — save each document to keep it.",6000);
}

// ---- keyboard shortcut customization (Batch R5) ----------------------------
namespace {
// Human-readable command label from an action's text (drop the '&' accelerators and trailing ellipsis).
static QString cleanActionText(QString t){ t.remove('&'); t.remove(QChar(0x2026)); t.remove("..."); return t.simplified(); }
// Walk the menu bar and return every leaf (triggerable) command as (path, action). The path
// ("Menu/Sub/Command") is a stable key for QSettings and for matching across launches.
static QList<QPair<QString,QAction*>> collectCommands(QMenuBar* mb){
    QList<QPair<QString,QAction*>> out;
    std::function<void(QMenu*,const QString&)> walk=[&](QMenu* menu, const QString& prefix){
        for(QAction* a: menu->actions()){
            if(a->isSeparator()) continue;
            if(a->menu()){ walk(a->menu(), prefix+"/"+cleanActionText(a->text())); continue; }
            const QString t=cleanActionText(a->text()); if(t.isEmpty()) continue;
            out.append({prefix+"/"+t, a});
        }
    };
    for(QAction* ma: mb->actions()) if(ma->menu()) walk(ma->menu(), cleanActionText(ma->text()));
    return out;
}
}

void MainWindow::captureShortcutDefaults(){
    m_defaultShortcuts.clear();
    for(const auto& pr: collectCommands(m_menuBar)) m_defaultShortcuts.insert(pr.first, pr.second->shortcut());
}

void MainWindow::applyShortcutOverrides(){
    QSettings s("XarQ","Shortcuts");
    for(const auto& pr: collectCommands(m_menuBar)){
        const QString v=s.value("sc/"+pr.first).toString();
        if(!v.isEmpty()) pr.second->setShortcut(QKeySequence(v));   // only override when the user set one
    }
}

QStringList MainWindow::commandPaths() const {
    QStringList out; for(const auto& pr: collectCommands(m_menuBar)) out<<pr.first; return out;
}
QKeySequence MainWindow::shortcutForCommand(const QString& path) const {
    for(const auto& pr: collectCommands(m_menuBar)) if(pr.first==path) return pr.second->shortcut();
    return QKeySequence();
}

void MainWindow::customizeShortcuts(){
    const auto cmds=collectCommands(m_menuBar);
    QDialog dlg(this); dlg.setWindowTitle("Customise Keyboard Shortcuts"); dlg.setModal(true); dlg.resize(560,560);
    auto* V=new QVBoxLayout(&dlg);
    V->addWidget(new QLabel("Click a shortcut field and press the keys you want. Clear a field to remove the shortcut.\nDuplicate shortcuts are rejected."));
    auto* filter=new QLineEdit; filter->setPlaceholderText("Filter commands…"); V->addWidget(filter);
    auto* table=new QTableWidget((int)cmds.size(),2,&dlg);
    table->setHorizontalHeaderLabels({"Command","Shortcut"});
    table->horizontalHeader()->setStretchLastSection(true); table->verticalHeader()->setVisible(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers); table->setSelectionMode(QAbstractItemView::NoSelection);
    QVector<QKeySequenceEdit*> edits(cmds.size());
    for(int i=0;i<cmds.size();++i){
        auto* it=new QTableWidgetItem(cmds[i].first); it->setFlags(Qt::ItemIsEnabled); table->setItem(i,0,it);
        auto* ed=new QKeySequenceEdit(cmds[i].second->shortcut()); edits[i]=ed; table->setCellWidget(i,1,ed);
    }
    table->resizeColumnToContents(0); V->addWidget(table,1);
    // live filter (hides non-matching rows)
    connect(filter,&QLineEdit::textChanged,&dlg,[&,table](const QString& q){
        for(int i=0;i<cmds.size();++i) table->setRowHidden(i, !q.isEmpty() && !cmds[i].first.contains(q,Qt::CaseInsensitive)); });
    auto* bb=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel|QDialogButtonBox::RestoreDefaults);
    V->addWidget(bb);
    connect(bb->button(QDialogButtonBox::RestoreDefaults),&QPushButton::clicked,&dlg,[&]{
        for(int i=0;i<cmds.size();++i) edits[i]->setKeySequence(m_defaultShortcuts.value(cmds[i].first)); });
    connect(bb,&QDialogButtonBox::accepted,&dlg,[&]{
        QHash<QString,int> seen;                                   // reject duplicate non-empty shortcuts
        for(int i=0;i<edits.size();++i){ const QString ks=edits[i]->keySequence().toString(); if(ks.isEmpty()) continue;
            if(seen.contains(ks)){ QMessageBox::warning(&dlg,"Duplicate shortcut",
                QString("“%1” is assigned to both:\n • %2\n • %3\n\nEach shortcut must be unique.").arg(ks, cmds[seen[ks]].first, cmds[i].first)); return; }
            seen.insert(ks,i); }
        QSettings s("XarQ","Shortcuts");
        for(int i=0;i<cmds.size();++i){ const QKeySequence k=edits[i]->keySequence();
            cmds[i].second->setShortcut(k);
            if(k==m_defaultShortcuts.value(cmds[i].first)) s.remove("sc/"+cmds[i].first);   // back to factory → drop override
            else s.setValue("sc/"+cmds[i].first, k.toString()); }
        dlg.accept(); statusBar()->showMessage("Keyboard shortcuts updated.",3000);
    });
    connect(bb,&QDialogButtonBox::rejected,&dlg,&QDialog::reject);
    dlg.exec();
}
void MainWindow::exportImage(){ if(m_tabs.isEmpty()) ensureLandingTab();   // Export from the empty state → reveal the document first, don't export the hidden blank canvas
    QString p=QFileDialog::getSaveFileName(this,"Export","export.png",
        "PNG (*.png);;JPEG (*.jpg *.jpeg);;WebP (*.webp);;BMP (*.bmp);;TIFF (*.tif *.tiff);;SVG (*.svg);;EPS (*.eps);;PDF (*.pdf);;Web page (*.html);;Animated GIF (*.gif)",nullptr,QFileDialog::DontUseNativeDialog); if(p.isEmpty())return;
    bool ok;
    if(p.endsWith(".svg",Qt::CaseInsensitive)) ok=m_canvas->exportSvg(p);
    else if(p.endsWith(".eps",Qt::CaseInsensitive)) ok=m_canvas->exportEps(p);
    else if(p.endsWith(".pdf",Qt::CaseInsensitive)) ok=m_canvas->exportPdf(p);
    else if(p.endsWith(".html",Qt::CaseInsensitive)||p.endsWith(".htm",Qt::CaseInsensitive)) ok=m_canvas->exportHtml(p);
    else if(p.endsWith(".gif",Qt::CaseInsensitive)){
        QDialog dlg(this); dlg.setWindowTitle("Animated GIF"); auto* form=new QFormLayout(&dlg);
        auto* delay=new QSpinBox(&dlg); delay->setRange(20,10000); delay->setValue(120); delay->setSuffix(" ms/frame");
        auto* loops=new QSpinBox(&dlg); loops->setRange(0,1000); loops->setValue(0); loops->setSpecialValueText("Forever"); loops->setSuffix(" loops");
        form->addRow("Frame delay:",delay); form->addRow("Repeat:",loops);
        form->addRow(new QLabel("Each layer is one frame. Set per-frame timing in the Page & Layer gallery."));
        auto* bb=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dlg);
        connect(bb,&QDialogButtonBox::accepted,&dlg,&QDialog::accept); connect(bb,&QDialogButtonBox::rejected,&dlg,&QDialog::reject); form->addRow(bb);
        if(dlg.exec()!=QDialog::Accepted) return;
        ok=m_canvas->exportGif(p, delay->value(), loops->value()); }
    else {                              // raster (png/jpg/bmp/tiff): show the Bitmap Export options dialog
        const bool jpeg = p.endsWith(".jpg",Qt::CaseInsensitive)||p.endsWith(".jpeg",Qt::CaseInsensitive);
        QDialog dlg(this); dlg.setWindowTitle("Export Bitmap");
        auto* form=new QFormLayout(&dlg);
        auto* area=new QComboBox(&dlg); area->addItems({"Page","Drawing (content)","Selection"});
        if(!m_canvas->selection().isEmpty()) area->setCurrentIndex(2); else area->setCurrentIndex(1);
        auto* scale=new QDoubleSpinBox(&dlg); scale->setRange(0.1,8.0); scale->setSingleStep(0.5); scale->setValue(2.0); scale->setSuffix("×");
        auto* qual=new QSpinBox(&dlg); qual->setRange(1,100); qual->setValue(90); qual->setEnabled(jpeg);
        auto* transp=new QCheckBox("Transparent background",&dlg); transp->setChecked(!jpeg); transp->setEnabled(!jpeg);
        auto* indexed=new QCheckBox("Reduce to 256-colour palette (smaller PNG)",&dlg); indexed->setEnabled(!jpeg);
        auto* dither=new QCheckBox("Dither (Floyd–Steinberg)",&dlg); dither->setEnabled(false);
        connect(indexed,&QCheckBox::toggled,dither,[dither,jpeg](bool on){ dither->setEnabled(on && !jpeg); });
        form->addRow("Area:",area); form->addRow("Resolution:",scale);
        if(jpeg) form->addRow("JPEG quality:",qual); form->addRow(transp);
        if(!jpeg){ form->addRow(indexed); form->addRow(dither); }
        auto* bb=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dlg);
        connect(bb,&QDialogButtonBox::accepted,&dlg,&QDialog::accept); connect(bb,&QDialogButtonBox::rejected,&dlg,&QDialog::reject);
        form->addRow(bb);
        if(dlg.exec()!=QDialog::Accepted) return;
        ok=m_canvas->exportRaster(p, area->currentIndex(), scale->value(), jpeg?qual->value():-1, transp->isChecked(), indexed->isChecked(), dither->isChecked());
    }
    statusBar()->showMessage(ok?("Exported "+p):"Export failed",4000); }

// Xara-style Print dialog: Printer + Pages (all / current / selected objects / range) + Copies & collate +
// Layout (fit / actual / scale, orientation, centre) + Output (quality, greyscale) + a live page preview.
void MainWindow::printDocument(){
    if(m_tabs.isEmpty()) ensureLandingTab();   // Print from the empty state → reveal the document first, don't print the hidden blank canvas
    QPrinter printer(QPrinter::HighResolution);
    const Document& docref = m_canvas->doc();
    const int npages = m_canvas->pageCount();
    const bool hasSel = !m_canvas->selection().isEmpty();

    QDialog dlg(this); dlg.setObjectName("printDialog"); dlg.setWindowTitle("Print"); dlg.setModal(true);
    auto* outer=new QVBoxLayout(&dlg);
    auto* mid=new QHBoxLayout; outer->addLayout(mid,1);
    auto* left=new QVBoxLayout; mid->addLayout(left,0);

    // ---- Printer ----
    auto* gPrinter=new QGroupBox("Printer"); auto* fPrinter=new QFormLayout(gPrinter);
    auto* printerCombo=new QComboBox;
    for(const QPrinterInfo& pi: QPrinterInfo::availablePrinters()) printerCombo->addItem(pi.printerName());
    printerCombo->addItem("Print to file (PDF)…");
    { int idx=printerCombo->findText(QPrinterInfo::defaultPrinterName()); if(idx>=0) printerCombo->setCurrentIndex(idx); }
    auto* setupBtn=new QToolButton; setupBtn->setText("Setup…"); setupBtn->setCursor(Qt::PointingHandCursor);
    { auto* w=new QWidget; auto* row=new QHBoxLayout(w); row->setContentsMargins(0,0,0,0); row->setSpacing(6);
      row->addWidget(printerCombo,1); row->addWidget(setupBtn); fPrinter->addRow("Name", w); }
    left->addWidget(gPrinter);

    // ---- Pages ----
    auto* gPages=new QGroupBox("Pages"); auto* fPages=new QVBoxLayout(gPages);
    auto* rAll=new QRadioButton(npages>1? "All pages" : "The page");
    auto* rCur=new QRadioButton("Current page"); rCur->setEnabled(npages>1);
    auto* rSel=new QRadioButton("Selected objects"); rSel->setEnabled(hasSel);
    auto* rRange=new QRadioButton("Page range:"); rRange->setEnabled(npages>1);
    auto* rangeEdit=new QLineEdit; rangeEdit->setPlaceholderText("e.g.  1, 3, 5-7"); rangeEdit->setEnabled(false);
    auto* rangeMsg=new QLabel; rangeMsg->setObjectName("infoHint");
    rAll->setChecked(true);
    fPages->addWidget(rAll); fPages->addWidget(rCur); fPages->addWidget(rSel);
    { auto* r=new QHBoxLayout; r->addWidget(rRange); r->addWidget(rangeEdit,1); fPages->addLayout(r); }
    fPages->addWidget(rangeMsg);
    left->addWidget(gPages);

    auto parseRange=[npages](const QString& s, bool* ok)->QVector<int>{
        QVector<int> out; *ok=true;
        const auto parts=s.split(',',Qt::SkipEmptyParts);
        for(const QString& part : parts){ QString p=part.trimmed(); if(p.isEmpty()) continue;
            if(p.contains('-')){ const auto ab=p.split('-'); if(ab.size()!=2){ *ok=false; continue; }
                bool a1,b1; int a=ab[0].trimmed().toInt(&a1), b=ab[1].trimmed().toInt(&b1);
                if(!a1||!b1){ *ok=false; continue; } if(a>b) std::swap(a,b);
                for(int k=a;k<=b;++k){ if(k>=1&&k<=npages) out.push_back(k-1); else *ok=false; } }
            else { bool o; int k=p.toInt(&o); if(!o){ *ok=false; continue; } if(k>=1&&k<=npages) out.push_back(k-1); else *ok=false; } }
        if(out.isEmpty()) *ok=false; return out; };

    // ---- Copies ----
    auto* gCopies=new QGroupBox("Copies"); auto* fCopies=new QFormLayout(gCopies);
    auto* copiesSpin=new QSpinBox; copiesSpin->setRange(1,999); copiesSpin->setValue(1);
    auto* collate=new QCheckBox("Collate"); collate->setChecked(true); collate->setEnabled(false);
    fCopies->addRow("Number of copies", copiesSpin); fCopies->addRow("", collate);
    left->addWidget(gCopies);

    // ---- Layout ----
    auto* gLayout=new QGroupBox("Layout"); auto* fLayout=new QFormLayout(gLayout);
    auto* scaleMode=new QComboBox; scaleMode->addItems({"Fit to page","Actual size (100%)","Custom scale"});
    auto* scaleSpin=new QSpinBox; scaleSpin->setRange(10,400); scaleSpin->setValue(100); scaleSpin->setSuffix(" %"); scaleSpin->setEnabled(false);
    auto* orient=new QComboBox; orient->addItems({"Portrait","Landscape"});
    if(m_canvas->pageSize().width()>m_canvas->pageSize().height()) orient->setCurrentIndex(1);
    auto* center=new QCheckBox("Centre on page"); center->setChecked(true);
    fLayout->addRow("Scaling", scaleMode); fLayout->addRow("Scale", scaleSpin);
    fLayout->addRow("Orientation", orient); fLayout->addRow("", center);
    left->addWidget(gLayout);

    // ---- Output ----
    auto* gOut=new QGroupBox("Output"); auto* fOut=new QFormLayout(gOut);
    auto* dpi=new QComboBox; dpi->addItems({"Draft — 150 dpi","Normal — 300 dpi","High — 600 dpi"}); dpi->setCurrentIndex(1);
    auto* grayscale=new QCheckBox("Print in greyscale");
    fOut->addRow("Quality", dpi); fOut->addRow("Colour", grayscale);
    left->addWidget(gOut); left->addStretch();

    // ---- Preview ----
    auto* rightCol=new QVBoxLayout; mid->addLayout(rightCol,0);
    { auto* l=new QLabel("PREVIEW"); l->setObjectName("panelTitle"); rightCol->addWidget(l); }
    auto* preview=new QLabel; preview->setObjectName("printPreview"); preview->setFixedSize(280,372); preview->setAlignment(Qt::AlignCenter);
    rightCol->addWidget(preview);
    auto* pvPrev=new QToolButton; pvPrev->setText("◀"); auto* pvLbl=new QLabel; pvLbl->setAlignment(Qt::AlignCenter); pvLbl->setMinimumWidth(110);
    auto* pvNext=new QToolButton; pvNext->setText("▶");
    { auto* r=new QHBoxLayout; r->addStretch(); r->addWidget(pvPrev); r->addWidget(pvLbl); r->addWidget(pvNext); r->addStretch(); rightCol->addLayout(r); }
    rightCol->addStretch();
    int pvPage = qBound(0, m_canvas->currentPage(), qMax(0,npages-1));

    // content extent for the current print scope (a page rect, or the selection bounds)
    auto contentFor=[&](bool sel, int page, Document& sdOut)->QRectF{
        if(sel){ sdOut=docref; sdOut.shapes.clear();
            for(int i: m_canvas->selection()) if(i>=0 && i<docref.shapes.size()) sdOut.shapes.push_back(docref.shapes[i]);
            QRectF b; for(const Shape& s: sdOut.shapes) b = b.isNull()? s.docBounds() : b.united(s.docBounds()); return b; }
        return docref.pageRect(page); };

    auto renderPreview=[&]{
        const bool land=orient->currentIndex()==1;
        const double paperWmm= land?297.0:210.0, paperHmm= land?210.0:297.0;   // A4 preview paper
        const int W=preview->width()-6, H=preview->height()-6;
        const double ppm=qMin(W/paperWmm, H/paperHmm);
        const int pw=int(paperWmm*ppm), ph=int(paperHmm*ppm);
        QPixmap pm(pw,ph); pm.fill(Qt::transparent);
        QPainter p(&pm); p.setRenderHint(QPainter::Antialiasing); p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.fillRect(QRect(0,0,pw,ph), Qt::white);
        Document sd; const bool sel=rSel->isChecked();
        const QRectF content=contentFor(sel, pvPage, sd);
        if(!content.isNull() && content.width()>=1 && content.height()>=1){
            const double MM_PER_PX=25.4/96.0, docWmm=content.width()*MM_PER_PX, docHmm=content.height()*MM_PER_PX;
            const int mode=scaleMode->currentIndex();
            const double eff = mode==0? qMin(paperWmm/docWmm, paperHmm/docHmm) : mode==1? 1.0 : scaleSpin->value()/100.0;
            const double placedWpx=docWmm*eff*ppm, placedHpx=docHmm*eff*ppm;
            const double ox= center->isChecked()? (pw-placedWpx)/2.0 : 0, oy= center->isChecked()? (ph-placedHpx)/2.0 : 0;
            p.save(); p.translate(ox,oy); const double s2=placedWpx/content.width(); p.scale(s2,s2);
            p.translate(-content.left(),-content.top()); p.setClipRect(content);
            paintDocument(p, sel? sd : docref); p.restore();
        }
        p.setPen(QPen(QColor(0,0,0,50))); p.setBrush(Qt::NoBrush); p.drawRect(QRect(0,0,pw-1,ph-1));
        preview->setPixmap(pm);
        pvLbl->setText(sel? "Selection" : QString("Page %1 / %2").arg(pvPage+1).arg(npages));
        pvPrev->setEnabled(!sel && pvPage>0); pvNext->setEnabled(!sel && pvPage<npages-1);
    };

    // ---- wiring ----
    connect(scaleMode,QOverload<int>::of(&QComboBox::currentIndexChanged),&dlg,[&](int i){ scaleSpin->setEnabled(i==2); renderPreview(); });
    connect(scaleSpin,QOverload<int>::of(&QSpinBox::valueChanged),&dlg,[&]{ renderPreview(); });
    connect(orient,QOverload<int>::of(&QComboBox::currentIndexChanged),&dlg,[&]{ renderPreview(); });
    connect(center,&QCheckBox::toggled,&dlg,[&]{ renderPreview(); });
    connect(copiesSpin,QOverload<int>::of(&QSpinBox::valueChanged),&dlg,[&](int v){ collate->setEnabled(v>1); });
    for(QRadioButton* r : {rAll,rCur,rSel,rRange}) connect(r,&QRadioButton::toggled,&dlg,[&]{ renderPreview(); });
    connect(rRange,&QRadioButton::toggled,&dlg,[&](bool on){ rangeEdit->setEnabled(on); if(on) rangeEdit->setFocus(); });
    connect(rangeEdit,&QLineEdit::textChanged,&dlg,[&]{ if(!rRange->isChecked()){ rangeMsg->clear(); return; }
        bool ok; parseRange(rangeEdit->text(),&ok);
        rangeMsg->setText(rangeEdit->text().trimmed().isEmpty()? "" : ok? "✓ valid range" : "✗ invalid range");
        rangeMsg->setStyleSheet(ok? "color:#2FBF71;font-style:normal" : "color:#F0524B;font-style:normal"); });
    connect(pvPrev,&QToolButton::clicked,&dlg,[&]{ if(pvPage>0){ pvPage--; renderPreview(); } });
    connect(pvNext,&QToolButton::clicked,&dlg,[&]{ if(pvPage<npages-1){ pvPage++; renderPreview(); } });
    connect(setupBtn,&QToolButton::clicked,&dlg,[&]{
        if(!printerCombo->currentText().startsWith("Print to file")) printer.setPrinterName(printerCombo->currentText());
        QPageSetupDialog(&printer,&dlg).exec();
        if(printer.pageLayout().orientation()==QPageLayout::Landscape) orient->setCurrentIndex(1);
        renderPreview(); });

    auto* bb=new QDialogButtonBox;
    auto* printBtn=bb->addButton("Print",QDialogButtonBox::AcceptRole); printBtn->setObjectName("ctaExport"); printBtn->setDefault(true);
    bb->addButton(QDialogButtonBox::Cancel);
    outer->addWidget(bb);
    connect(bb,&QDialogButtonBox::accepted,&dlg,&QDialog::accept);
    connect(bb,&QDialogButtonBox::rejected,&dlg,&QDialog::reject);

    renderPreview();
    if(dlg.exec()!=QDialog::Accepted) return;

    // ---- apply + print ----
    const bool toFile = printerCombo->currentText().startsWith("Print to file");
    if(toFile){ QString fn=QFileDialog::getSaveFileName(this,"Print to File","printout.pdf","PDF (*.pdf)",nullptr,QFileDialog::DontUseNativeDialog);
        if(fn.isEmpty()) return; if(!fn.endsWith(".pdf",Qt::CaseInsensitive)) fn+=".pdf";
        printer.setOutputFormat(QPrinter::PdfFormat); printer.setOutputFileName(fn); }
    else printer.setPrinterName(printerCombo->currentText());
    printer.setCopyCount(copiesSpin->value());
    printer.setCollateCopies(collate->isEnabled() && collate->isChecked());
    printer.setColorMode(grayscale->isChecked()? QPrinter::GrayScale : QPrinter::Color);
    { const int r[]={150,300,600}; printer.setResolution(r[qBound(0,dpi->currentIndex(),2)]); }
    printer.setPageOrientation(orient->currentIndex()==1? QPageLayout::Landscape : QPageLayout::Portrait);

    QVector<int> pageList; bool selectionMode=false;
    if(rSel->isChecked()) selectionMode=true;
    else if(rCur->isChecked()) pageList={ qBound(0,m_canvas->currentPage(),qMax(0,npages-1)) };
    else if(rRange->isChecked()){ bool ok; pageList=parseRange(rangeEdit->text(),&ok);
        if(!ok||pageList.isEmpty()){ QMessageBox::warning(this,"Print","The page range is not valid.\nUse numbers and hyphens, e.g. 1, 3, 5-7."); return; } }
    else for(int i=0;i<npages;++i) pageList.push_back(i);

    QPainter g(&printer);
    if(!g.isActive()){ QMessageBox::warning(this,"Print","Could not start the print job."); return; }
    g.setRenderHint(QPainter::Antialiasing); g.setRenderHint(QPainter::SmoothPixmapTransform);
    const int mode=scaleMode->currentIndex();
    auto printOne=[&](const Document& d, const QRectF& content){
        if(content.isNull()||content.width()<1) return;
        const QRectF pd=printer.pageRect(QPrinter::DevicePixel);
        double s;
        if(mode==0) s=qMin(pd.width()/content.width(), pd.height()/content.height());   // fit to page
        else s=(printer.resolution()/96.0) * (mode==2? scaleSpin->value()/100.0 : 1.0);  // actual / custom (doc unit = 1/96")
        const double ox= center->isChecked()? pd.left()+(pd.width()-content.width()*s)/2.0 : pd.left();
        const double oy= center->isChecked()? pd.top()+(pd.height()-content.height()*s)/2.0 : pd.top();
        g.save(); g.translate(ox,oy); g.scale(s,s); g.translate(-content.left(),-content.top());
        g.setClipRect(content); paintDocument(g, d); g.restore();
    };
    if(selectionMode){ Document sd; const QRectF b=contentFor(true,0,sd); printOne(sd, b); }
    else for(int k=0;k<pageList.size();++k){ if(k>0) printer.newPage(); printOne(docref, docref.pageRect(pageList[k])); }
    g.end();
    statusBar()->showMessage(toFile? "Printed to file." : "Sent to printer.", 4000);
}
void MainWindow::pageSetup(){
    QDialog dlg(this); dlg.setWindowTitle("Page Setup");
    auto* form=new QFormLayout(&dlg);
    struct P{const char* n; double w,h;};   // millimetres
    static const P presets[]={{"A4",210,297},{"A3",297,420},{"A5",148,210},{"Letter",215.9,279.4},
        {"Legal",215.9,355.6},{"Tabloid",279.4,431.8},{"Custom",0,0}};
    const double MM=96.0/25.4;
    auto* preset=new QComboBox; for(auto&p:presets) preset->addItem(p.n);
    auto* wsp=new QDoubleSpinBox; wsp->setRange(1,40000); wsp->setDecimals(0); wsp->setSuffix(" px"); wsp->setValue(m_canvas->pageSize().width());
    auto* hsp=new QDoubleSpinBox; hsp->setRange(1,40000); hsp->setDecimals(0); hsp->setSuffix(" px"); hsp->setValue(m_canvas->pageSize().height());
    auto* orient=new QComboBox; orient->addItems({"Portrait","Landscape"});
    if(m_canvas->pageSize().width()>m_canvas->pageSize().height()) orient->setCurrentIndex(1);
    auto applyPreset=[=]{ int i=preset->currentIndex(); if(presets[i].w<=0) return;
        double w=presets[i].w*MM, h=presets[i].h*MM; if(orient->currentIndex()==1) std::swap(w,h);
        wsp->setValue(qRound(w)); hsp->setValue(qRound(h)); };
    connect(preset,QOverload<int>::of(&QComboBox::activated),&dlg,[=]{ applyPreset(); });
    connect(orient,QOverload<int>::of(&QComboBox::activated),&dlg,[=]{ applyPreset(); });
    form->addRow("Paper size", preset);
    form->addRow("Orientation", orient);
    form->addRow("Width", wsp);
    form->addRow("Height", hsp);
    auto* bb=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);
    connect(bb,&QDialogButtonBox::accepted,&dlg,&QDialog::accept); connect(bb,&QDialogButtonBox::rejected,&dlg,&QDialog::reject);
    form->addRow(bb);
    if(dlg.exec()==QDialog::Accepted){ m_canvas->setPageSize(QSizeF(wsp->value(),hsp->value())); m_canvas->zoomFit(); }
}
void MainWindow::about(){
    QMessageBox box(this); box.setWindowTitle("About XarQ"); box.setTextFormat(Qt::RichText);
    box.setIconPixmap(QIcon(":/xarq/assets/logo/xarq-mark.svg").pixmap(72,72));
    box.setText("<b style='font-size:17px'><span style='color:#5488FF'>X</span>arQ</b><br>"
        "<span style='color:#8B94A3'>Vector · photo · layout studio</span><br><br>"
        "A modern Qt6 rebuild of Xara Xtreme — the <b>X</b> of Xara, the <b>Q</b> of Qt.<br>"
        "Renders through the open libGDraw / AGG rasteriser pipeline.<br>"
        "All classic Xara tools, reimagined.");
    box.exec();
}
void MainWindow::updateTitle(){
    if(m_activeTab<0){ setWindowTitle("XarQ"); refreshDocStatus(); return; }   // empty state: no document open
    QString p=m_canvas->filePath(); setWindowTitle(p.isEmpty()?"XarQ — Untitled":("XarQ — "+QFileInfo(p).fileName()));
    if(m_tabBar) refreshTabText(m_activeTab); refreshDocStatus(); }

// DS TitleBar composition: the wordmark sits at the far left of the menu bar; the active
// document name + a save-state Badge sit at the far right. Both use menu-bar corner widgets.
// DS TitleBar — a custom widget set as the window's menu area (setMenuWidget) so the document name +
// status badge can sit truly CENTERED between the menus (left) and the Share/cloud controls (right),
// exactly like ui_kits/editor. The real QMenuBar is embedded live, so all menus/shortcuts keep working.
void MainWindow::buildTitleBar(){
    auto* bar=new QWidget; bar->setObjectName("titleBar"); bar->setFixedHeight(40);
    auto* h=new QHBoxLayout(bar); h->setContentsMargins(12,0,10,0); h->setSpacing(10);

    // Every item is added with AlignVCenter so it keeps its natural height and sits on the bar's centre-line
    // (without it, QLabels/badges stretch to the full 40px bar height — the "Unsaved" pill ballooned).
    auto* word=new QLabel("<span style='color:#5488FF'>X</span>arQ"); word->setObjectName("appTitle");
    word->setTextFormat(Qt::RichText);
    h->addWidget(word, 0, Qt::AlignVCenter);
    h->addWidget(m_menuBar, 0, Qt::AlignVCenter);

    h->addStretch(1);   // ← centering strut (left)
    m_docName=new QLabel; m_docName->setObjectName("docName");
    m_docBadge=new QLabel; m_docBadge->setObjectName("docBadge");
    h->addWidget(m_docName, 0, Qt::AlignVCenter); h->addWidget(m_docBadge, 0, Qt::AlignVCenter);
    h->addStretch(1);   // ← centering strut (right)

    auto* share=new QToolButton; share->setObjectName("titleGhost"); share->setText("↗  Share");
    share->setCursor(Qt::PointingHandCursor); share->setToolTip("Share");
    connect(share,&QToolButton::clicked,this,[this]{ statusBar()->showMessage("Sharing is available when a document is published to the web.",2500); });
    auto* cloud=new QToolButton; cloud->setObjectName("titleGhost"); cloud->setText("☁");
    cloud->setCursor(Qt::PointingHandCursor); cloud->setToolTip("Sync status");
    h->addWidget(share, 0, Qt::AlignVCenter); h->addWidget(cloud, 0, Qt::AlignVCenter);

    setMenuWidget(bar);
    refreshDocStatus();
}

void MainWindow::refreshDocStatus(){
    if(!m_docName || !m_docBadge) return;
    if(m_activeTab<0){   // empty state — no document open
        m_docName->setText("No document");
        m_docBadge->setText("");
        m_docBadge->setProperty("tone","ok");
        m_docBadge->style()->unpolish(m_docBadge); m_docBadge->style()->polish(m_docBadge);
        if(m_sSaved){ m_sSaved->setText("");
            m_sSaved->setProperty("tone","ok");
            m_sSaved->style()->unpolish(m_sSaved); m_sSaved->style()->polish(m_sSaved); }
        return;
    }
    const QString p=m_canvas->filePath();
    m_docName->setText(p.isEmpty()? "Untitled" : QFileInfo(p).fileName());
    m_docBadge->setText(m_dirty ? "Unsaved" : "Saved");
    m_docBadge->setProperty("tone", m_dirty ? "warning" : "ok");
    m_docBadge->style()->unpolish(m_docBadge); m_docBadge->style()->polish(m_docBadge);   // re-evaluate QSS on tone change
    if(m_sSaved){ m_sSaved->setText(m_dirty ? "● Unsaved" : "✓ Saved");
        m_sSaved->setProperty("tone", m_dirty ? "warning" : "ok");
        m_sSaved->style()->unpolish(m_sSaved); m_sSaved->style()->polish(m_sSaved); }
}

void MainWindow::seedDemo() {
    Document d;
    // gradient hero panel with soft shadow
    { Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,400,260); s.cornerRadius=22; s.transform=QTransform::fromTranslate(80,80);
      s.fill.type=FillType::Linear; s.fill.stops={{0,QColor(96,170,255)},{1,QColor(40,80,180)}}; s.fill.p0=QPointF(0,0); s.fill.p1=QPointF(400,260);
      s.stroke.enabled=false; s.shadow.on=true; s.shadow.blur=26; s.shadow.offset=QPointF(0,16); s.name="Hero panel"; d.shapes.push_back(s); }
    // beveled "button"
    { Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,170,56); s.cornerRadius=12; s.transform=QTransform::fromTranslate(118,300);
      s.fill.type=FillType::Linear; s.fill.stops={{0,QColor(90,200,140)},{1,QColor(40,150,90)}}; s.fill.p0=QPointF(0,0); s.fill.p1=QPointF(0,56);
      s.bevel.on=true; s.bevel.depth=6; s.stroke.enabled=false; s.shadow.on=true; s.shadow.blur=10; s.shadow.offset=QPointF(0,5); s.name="Button"; d.shapes.push_back(s); }
    // beveled gold star
    { Shape s; s.type=ShapeType::Polygon; s.polySides=5; s.polyStar=true; s.rect=QRectF(0,0,160,160); s.transform=QTransform::fromTranslate(600,90);
      s.fill.type=FillType::Solid; s.fill.color=QColor(255,200,70); s.extrude.on=true; s.extrude.depth=22; s.extrude.angle=40; s.stroke.enabled=false; s.shadow.on=true; s.shadow.blur=14; s.shadow.offset=QPointF(4,8); s.name="Star"; d.shapes.push_back(s); }
    // contoured hexagon
    { Shape s; s.type=ShapeType::Polygon; s.polySides=6; s.rect=QRectF(0,0,130,130); s.transform=QTransform::fromTranslate(820,300);
      s.fill.type=FillType::Solid; s.fill.color=QColor(120,110,240); s.contour.on=true; s.contour.width=12; s.contour.steps=4; s.contour.color=QColor(120,110,240,120); s.stroke.enabled=false; s.name="Hex"; d.shapes.push_back(s); }
    // translucent radial orb
    { Shape s; s.type=ShapeType::Ellipse; s.rect=QRectF(0,0,200,200); s.transform=QTransform::fromTranslate(560,290);
      s.fill.type=FillType::Radial; s.fill.stops={{0,QColor(255,130,190)},{1,QColor(180,40,120)}}; s.fill.p0=QPointF(75,75); s.fill.p1=QPointF(200,200);
      s.opacity=0.8; s.stroke.enabled=false; s.name="Orb"; d.shapes.push_back(s); }
    // title text
    // XarQ wordmark — Manrope ExtraBold, the "X of Xara" accented (per the brand), with a clean tagline below.
    { Shape s; s.type=ShapeType::Text; s.text="XarQ"; QFont wf("Montserrat",56); wf.setWeight(QFont::ExtraBold); s.font=wf;
      s.transform=QTransform::fromTranslate(86,438);
      s.fill.type=FillType::Solid; s.fill.color=QColor(0xEE,0xF1,0xF5);
      TextRun rx; rx.start=0; rx.len=1; rx.font=wf; rx.color=QColor(0x54,0x88,0xFF);
      TextRun rr; rr.start=1; rr.len=3; rr.font=wf; rr.color=QColor(0xEE,0xF1,0xF5);
      s.textRuns={rx,rr}; s.stroke.enabled=false; s.name="Wordmark"; d.shapes.push_back(s); }
    { Shape s; s.type=ShapeType::Text; s.text="vector · photo · layout studio"; s.font=QFont("Montserrat",19); s.transform=QTransform::fromTranslate(90,520);
      s.fill.type=FillType::Solid; s.fill.color=QColor(0x8B,0x94,0xA3); s.stroke.enabled=false; s.name="Tagline"; d.shapes.push_back(s); }
    // moulded (perspective-warped) card — demonstrates the Mould/Envelope tool
    { Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,210,140); s.cornerRadius=10;
      s.fill.type=FillType::Linear; s.fill.stops={{0,QColor(255,150,90)},{1,QColor(225,70,120)}}; s.fill.p0=QPointF(0,0); s.fill.p1=QPointF(210,140);
      s.stroke.enabled=false; s.shadow.on=true; s.shadow.blur=16; s.shadow.offset=QPointF(0,10);
      QPolygonF from; from<<QPointF(0,0)<<QPointF(210,0)<<QPointF(210,140)<<QPointF(0,140);
      QPolygonF to;   to  <<QPointF(560,560)<<QPointF(760,520)<<QPointF(720,690)<<QPointF(540,700);
      QTransform t; QTransform::quadToQuad(from,to,t); s.transform=t; s.name="Moulded card"; d.shapes.push_back(s); }
    m_canvas->setDocument(d);
}

} // namespace xara
