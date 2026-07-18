// startscreen.cpp — see startscreen.h.
#include "startscreen.h"
#include "mainwindow.h"
#include "canvas.h"
#include <QtWidgets>
#include <QSettings>
#include <functional>

namespace xara {

// Shared look for BOTH the embedded panel (#startPanel, merged into the canvas area) and the modal
// dialog (#startDialog). Applied on the panel widget in buildPanel() and on the dialog in show().
static const char* kStartQss = R"QSS(
    #startDialog, #startPanel { background:#111318; }
    #startSidebar { background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #1B1E24, stop:1 #14161A); border-right:1px solid rgba(255,255,255,0.06); }
    #startMain { background:#111318; }
    QLabel#startWord { font-size:30px; font-weight:800; color:#EEF1F5; letter-spacing:-0.5px; }
    QLabel#startTag { font-size:11px; color:#8B94A3; letter-spacing:1.2px; }
    QLabel#startGreet { font-size:26px; font-weight:800; color:#EEF1F5; letter-spacing:-0.5px; }
    QLabel#startSub { font-size:13px; color:#8B94A3; }
    QLabel#startSection { font-size:11px; font-weight:700; color:#8B94A3; letter-spacing:1.4px; }
    QToolButton#startCard { background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #1F232B, stop:1 #191C22); border:1px solid rgba(255,255,255,0.08); border-radius:5px; color:#D2D8E0; font-size:13px; font-weight:600; padding:10px; text-align:center; }
    QToolButton#startCard:hover { background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #262A33, stop:1 #1F232B); border-color:#2D6FF7; }
    QToolButton#startNav { background:transparent; border:none; border-radius:3px; color:#8B94A3; padding:8px 12px; text-align:left; font-size:13px; }
    QToolButton#startNav:hover { background:#23272F; color:#D2D8E0; }
    QListWidget#startRecent { background:#14161A; border:1px solid rgba(255,255,255,0.08); border-radius:6px; padding:5px; }
    QListWidget#startRecent::item { padding:8px 10px; border-radius:3px; }
    QListWidget#startRecent::item:hover { background:#1D2027; }
    QListWidget#startRecent::item:selected { background:rgba(45,111,247,0.24); color:#EEF1F5; }
)QSS";

// One clickable preset/recent card — a raised graphite tile with a title + subtitle that lifts on hover.
static QToolButton* card(const QString& title, const QString& sub, const QColor& swatch){
    auto* b=new QToolButton; b->setObjectName("startCard");
    b->setCursor(Qt::PointingHandCursor); b->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    b->setMinimumSize(150,96); b->setMaximumHeight(110);
    // colour chip icon
    QPixmap pm(28,28); pm.fill(Qt::transparent);
    { QPainter p(&pm); p.setRenderHint(QPainter::Antialiasing); p.setPen(Qt::NoPen);
      p.setBrush(swatch); p.drawRoundedRect(QRectF(1,1,26,26),6,6); }
    b->setIcon(QIcon(pm)); b->setIconSize(QSize(28,28));
    b->setText(title+"\n"+sub);
    return b;
}

// Build the full welcome UI (sidebar + main area) as a standalone widget. Used BOTH embedded in the
// canvas area (empty state) and inside the modal dialog. Any choice that opens/creates a document
// invokes onChosen() (if set) — the modal wires that to close itself; the embedded case leaves it empty
// (the MainWindow's own new/open path swaps the canvas back in).
QWidget* StartScreen::buildPanel(MainWindow* mw, std::function<void()> onChosen){
    auto* w=new QWidget; w->setObjectName("startPanel");
    w->setStyleSheet(QString::fromUtf8(kStartQss));

    auto* row=new QHBoxLayout(w); row->setContentsMargins(0,0,0,0); row->setSpacing(0);

    // ---- left sidebar ----
    auto* side=new QFrame; side->setObjectName("startSidebar"); side->setFixedWidth(248);
    auto* sv=new QVBoxLayout(side); sv->setContentsMargins(22,26,22,22); sv->setSpacing(14);
    { auto* logo=new QHBoxLayout; logo->setSpacing(10);
      auto* mark=new QLabel; mark->setPixmap(QIcon(":/xarq/assets/logo/xarq-mark.svg").pixmap(38,38));
      // Wordmark per DS: Manrope ExtraBold, the "X of Xara" accented (accent-400), the rest text-strong.
      auto* word=new QLabel("<span style='color:#5488FF'>X</span>arQ"); word->setObjectName("startWord"); word->setTextFormat(Qt::RichText);
      logo->addWidget(mark); logo->addWidget(word); logo->addStretch(); sv->addLayout(logo); }
    { auto* tag=new QLabel("VECTOR · PHOTO · LAYOUT"); tag->setObjectName("startTag"); sv->addWidget(tag); }
    sv->addSpacing(10);
    auto* newBtn=new QPushButton("  New document"); newBtn->setDefault(true); newBtn->setMinimumHeight(38);
    newBtn->setCursor(Qt::PointingHandCursor); sv->addWidget(newBtn);
    sv->addSpacing(6);
    auto navBtn=[&](const QString& t){ auto* b=new QToolButton; b->setObjectName("startNav"); b->setText(t);
        b->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed); b->setCursor(Qt::PointingHandCursor); sv->addWidget(b); return b; };
    QToolButton* navOpen=navBtn("Open a file…");
    // (The template grid and recent-files list live in the main area below; no separate nav buttons for them.)
    sv->addStretch();
    { auto* ver=new QLabel("XarQ · Qt6 · open pipeline"); ver->setObjectName("startTag"); sv->addWidget(ver); }
    row->addWidget(side);

    // ---- main area ----
    auto* main=new QWidget; main->setObjectName("startMain");
    auto* mv=new QVBoxLayout(main); mv->setContentsMargins(30,28,30,24); mv->setSpacing(6);
    { auto* g=new QLabel("Welcome to XarQ"); g->setObjectName("startGreet"); mv->addWidget(g); }
    { auto* s=new QLabel("Start a new document or open a recent file."); s->setObjectName("startSub"); mv->addWidget(s); }
    mv->addSpacing(16);
    { auto* sec=new QLabel("START A NEW DOCUMENT"); sec->setObjectName("startSection"); mv->addWidget(sec); }
    mv->addSpacing(4);

    // preset grid from the app's templates
    auto* grid=new QGridLayout; grid->setSpacing(12);
    const QStringList tmpl=Canvas::templateNames();
    const QStringList subs={ "Empty canvas", "Web · hero + sections", "Print · 85×55 mm", "Photo · grid" };
    const QColor chips[]={ QColor(0x54,0x88,0xFF), QColor(0x8B,0x5C,0xF6), QColor(0x22,0xC3,0xB8), QColor(0xE2,0x55,0xA1) };
    for(int i=0;i<tmpl.size();++i){
        auto* c=card(tmpl[i], i<subs.size()?subs[i]:"", chips[i%4]);
        QObject::connect(c,&QToolButton::clicked,w,[mw,i,onChosen]{ mw->newFromTemplate(i); if(onChosen) onChosen(); });
        grid->addWidget(c, i/4, i%4);
    }
    mv->addLayout(grid);
    mv->addSpacing(18);

    { auto* sec=new QLabel("RECENT FILES"); sec->setObjectName("startSection"); mv->addWidget(sec); }
    mv->addSpacing(4);
    const QStringList recents=mw->recentFiles();
    if(recents.isEmpty()){
        auto* empty=new QLabel("No recent files. Create a new document to get started.");
        empty->setObjectName("startSub"); mv->addWidget(empty); mv->addStretch();
    } else {
        auto* list=new QListWidget; list->setObjectName("startRecent");
        for(const QString& p : recents){ QFileInfo fi(p);
            auto* it=new QListWidgetItem(fi.fileName()+"   —   "+fi.absolutePath()); it->setData(Qt::UserRole,p); list->addItem(it); }
        QObject::connect(list,&QListWidget::itemActivated,w,[mw,onChosen](QListWidgetItem* it){ mw->openPath(it->data(Qt::UserRole).toString()); if(onChosen) onChosen(); });
        QObject::connect(list,&QListWidget::itemClicked,w,[mw,onChosen](QListWidgetItem* it){ mw->openPath(it->data(Qt::UserRole).toString()); if(onChosen) onChosen(); });
        mv->addWidget(list,1);
    }

    // footer — the "Show this screen on startup" preference (the modal adds its own "Continue to workspace")
    auto* footer=new QHBoxLayout;
    auto* showChk=new QCheckBox("Show this screen on startup");
    showChk->setChecked(QSettings("XarQ","XarQ").value("showWelcome",true).toBool());
    QObject::connect(showChk,&QCheckBox::toggled,w,[](bool on){ QSettings("XarQ","XarQ").setValue("showWelcome",on); });
    footer->addWidget(showChk); footer->addStretch();
    mv->addLayout(footer);
    row->addWidget(main,1);

    // wiring
    QObject::connect(newBtn,&QPushButton::clicked,w,[mw,onChosen]{ mw->newFromTemplate(0); if(onChosen) onChosen(); });
    QObject::connect(navOpen,&QToolButton::clicked,w,[mw,w,onChosen]{
        const QString p=QFileDialog::getOpenFileName(w,"Open","","XarQ (*.xar);;SVG (*.svg)",nullptr,QFileDialog::DontUseNativeDialog);
        if(!p.isEmpty()){ mw->openPath(p); if(onChosen) onChosen(); } });

    return w;
}

void StartScreen::show(MainWindow* mw){
    QDialog dlg(mw); dlg.setObjectName("startDialog"); dlg.setWindowTitle("Welcome to XarQ");
    dlg.setModal(true); dlg.resize(920,600);
    dlg.setStyleSheet(QString::fromUtf8(kStartQss));

    auto* outer=new QVBoxLayout(&dlg); outer->setContentsMargins(0,0,0,0); outer->setSpacing(0);
    // A choice inside the panel accepts the modal; "Continue to workspace" (below) rejects it.
    QWidget* panel=buildPanel(mw,[&dlg]{ dlg.accept(); });
    outer->addWidget(panel,1);

    // modal-only footer button — skip the welcome and drop straight into a blank workspace.
    auto* bar=new QHBoxLayout; bar->setContentsMargins(30,4,30,16); bar->addStretch();
    auto* skip=new QPushButton("Continue to workspace"); skip->setCursor(Qt::PointingHandCursor);
    QObject::connect(skip,&QPushButton::clicked,&dlg,[&dlg]{ dlg.reject(); });
    bar->addWidget(skip);
    outer->addLayout(bar);

    dlg.exec();
}

} // namespace xara
