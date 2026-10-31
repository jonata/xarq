// startscreen.cpp — XarQ welcome / start screen. Redesigned per the "Welcome Screen" handoff:
// a compact recovery banner (demoted), a hero grid of template cards, an always-present search, a
// purposeful "no recent files" empty state, and a faded coming-soon grid background. Shown embedded in
// the canvas area on a clean launch (empty state) and hosted inside a modal for File ▸ Start screen.
#include "startscreen.h"
#include "mainwindow.h"
#include "canvas.h"
#include "icons.h"
#include <QtWidgets>
#include <QSettings>
#include <QDesktopServices>
#include <QUrl>
#include <QMouseEvent>
#include <functional>

namespace xara {

// ---- design tokens (from the XarQ token set, mirrored in the handoff) ---------------------------
namespace tok {
    static const QColor app(0x08,0x09,0x0B), chrome(0x11,0x13,0x18), panel(0x17,0x1A,0x20), well(0x23,0x27,0x2F);
    static const QColor strong(0xEE,0xF1,0xF5), body(0xD2,0xD8,0xE0), sec(0x8B,0x94,0xA3), tert(0x6E,0x77,0x87);
    static const QColor accent(0x2D,0x6F,0xF7), accent3(0x7F,0xA5,0xFF), warning(0xF5,0xB2,0x3D);
}

// Shared QSS for the panel (#startPanel) and the modal (#startDialog). ID selectors so they beat the
// global theme; everything between the grid background and the leaf widgets is made transparent so the
// grid shows through (labels are already transparent via the global theme).
static const char* kStartQss = R"QSS(
    #startPanel, #startDialog { background:#08090B; }
    #startSidebar { background:#111318; border-right:1px solid rgba(255,255,255,0.06); }
    #startMain, #startContent { background:transparent; }
    QScrollArea#startScroll { background:transparent; border:none; }
    QScrollArea#startScroll > QWidget > QWidget { background:transparent; }

    QLabel#h1 { font-size:29px; font-weight:800; color:#EEF1F5; letter-spacing:-0.5px; }
    QLabel#sub { font-size:14px; color:#8B94A3; }
    QLabel#eyebrow { font-size:11px; font-weight:700; color:#6E7787; letter-spacing:1.6px; }
    QLabel#link { font-size:12px; color:#5488FF; }
    QLabel#ver { font-size:11px; color:#6E7787; }

    /* Controls use the design-system --radius-md (3px). Primary = --grad-accent gradient (wt 600, dark
       border + top highlight); Outline = accent-300 text on an accent-600 border. */
    QPushButton#btnPrimary { background:qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #4680FF, stop:1 #2D6FF7); border:1px solid rgba(0,0,0,0.20); border-top-color:rgba(255,255,255,0.18); border-radius:3px; color:#FFFFFF; font-size:14px; font-weight:600; padding:0 18px; min-height:36px; max-height:36px; text-align:center; }
    QPushButton#btnPrimary:hover { background:qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #5A90FF, stop:1 #3D7DFF); }
    QPushButton#btnPrimary:pressed { background:qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #2D6FF7, stop:1 #235FE0); }
    QPushButton#btnOutline { background:transparent; border:1px solid #235FE0; border-radius:3px; color:#7FA5FF; font-size:13px; font-weight:600; padding:0 14px; min-height:28px; max-height:28px; text-align:center; }
    QPushButton#btnOutline:hover { background:rgba(45,111,247,0.12); border-color:#2D6FF7; color:#9DB8FF; }
    QPushButton#btnSecondary { background:#2A2F39; border:1px solid rgba(255,255,255,0.10); border-radius:3px; color:#EEF1F5; font-size:12px; font-weight:600; padding:0 12px; }
    QPushButton#btnSecondary:hover { background:#323844; }
    QPushButton#btnGhost { background:transparent; border:none; border-radius:3px; color:#D2D8E0; font-size:12px; font-weight:600; padding:0 10px; }
    QPushButton#btnGhost:hover { background:rgba(255,255,255,0.06); color:#EEF1F5; }
    QPushButton#sideLink { background:transparent; border:none; border-top:1px solid rgba(255,255,255,0.06); text-align:left; }

    QLineEdit#search { background:#23272F; border:1px solid rgba(255,255,255,0.10); border-radius:3px; color:#D2D8E0; padding:6px 10px; font-size:13px; selection-background-color:#2D6FF7; }
    QLineEdit#search:focus { border:1px solid #2D6FF7; }

    QFrame#tplCard { background:#171A20; border:1px solid rgba(255,255,255,0.06); border-radius:5px; }
    QFrame#tplCard:hover { border:1px solid rgba(255,255,255,0.18); background:#1B1F27; }
    QFrame#tplCardDashed { background:transparent; border:1px dashed rgba(255,255,255,0.16); border-radius:5px; }
    QFrame#tplCardDashed:hover { border:1px dashed rgba(255,255,255,0.32); background:rgba(255,255,255,0.02); }
    QLabel#cardTitle { font-size:13px; font-weight:600; color:#EEF1F5; background:transparent; }
    QLabel#cardMeta { font-size:11px; color:#6E7787; background:transparent; }
    QLabel#cardTile { background:transparent; }

    QFrame#recBanner { background:rgba(245,178,61,0.15); border:1px solid rgba(245,178,61,0.32); border-radius:5px; }
    QLabel#recTitle { font-size:13px; font-weight:600; color:#EEF1F5; background:transparent; }
    QLabel#recChip { background:#23272F; border-radius:5px; padding:2px 8px; color:#8B94A3; font-size:11px; }
    QLabel#recIcon { background:rgba(245,178,61,0.18); border-radius:8px; }

    QFrame#emptyPanel { background:#171A20; border:1px dashed rgba(255,255,255,0.10); border-radius:5px; }
    QLabel#emptyTitle { font-size:14px; font-weight:600; color:#EEF1F5; background:transparent; }
    QLabel#emptyBody { font-size:12px; color:#8B94A3; background:transparent; }
    QLabel#emptyIcon { background:#23272F; border-radius:26px; }

    QFrame#footer { background:#111318; border-top:1px solid rgba(255,255,255,0.06); }
    QCheckBox#showChk { color:#6E7787; font-size:11px; }
)QSS";

// The main-area background: the coming-soon 54px grid, faded by a top-right radial mask, plus a soft blue
// glow — both non-interactive, painted below the (transparent) content that scrolls over it.
class GridBg : public QWidget {
public:
    explicit GridBg(QWidget* parent=nullptr) : QWidget(parent) {}
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
        const int W=width(), H=height();
        p.fillRect(rect(), tok::app);
        if (W<=0 || H<=0) return;
        // grid lines drawn into a scratch layer, then masked so they read only top-right and fade out
        QImage layer(size(), QImage::Format_ARGB32_Premultiplied); layer.fill(Qt::transparent);
        { QPainter g(&layer);
          g.setPen(QPen(QColor(255,255,255,15), 1));               // --border-subtle ≈ rgba(255,255,255,0.06)
          for (int x=-1; x<=W; x+=54) g.drawLine(x,0,x,H);
          for (int y=-1; y<=H; y+=54) g.drawLine(0,y,W,y);
          g.setCompositionMode(QPainter::CompositionMode_DestinationIn);
          QRadialGradient mask(QPointF(W*0.76, H*-0.05), qMax(W,H)*1.20);   // ~radial(130% 105% at 76% -5%)
          mask.setColorAt(0.00, QColor(0,0,0,255)); mask.setColorAt(0.26, QColor(0,0,0,255));
          mask.setColorAt(0.80, QColor(0,0,0,0));   mask.setColorAt(1.00, QColor(0,0,0,0));
          g.fillRect(layer.rect(), mask); }
        p.drawImage(0,0,layer);
        QRadialGradient glow(QPointF(W*0.82, H*0.04), 780);        // ~radial(900×620 at 82% 4%)
        glow.setColorAt(0.0, QColor(45,111,247,38)); glow.setColorAt(0.6, QColor(45,111,247,0));
        p.fillRect(rect(), glow);
    }
};

// 44×44 rounded, tinted icon tile used on a template card.
static QPixmap tilePix(const QString& icon, const QColor& bg, const QColor& fg){
    QPixmap pm(88,88); pm.setDevicePixelRatio(2.0); pm.fill(Qt::transparent);
    QPainter p(&pm); p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen); p.setBrush(bg); p.drawRoundedRect(QRectF(0,0,44,44),10,10);
    p.drawPixmap(QPointF((44-22)/2.0,(44-22)/2.0), actionIcon(icon,22,fg).pixmap(22,22));
    return pm;
}

// A moc-free clickable behaviour for a plain widget: invokes cb on a left-release inside the widget.
// (A QObject subclass with only a virtual eventFilter override needs no Q_OBJECT / moc.)
class ClickFilter : public QObject {
public:
    std::function<void()> cb;
    explicit ClickFilter(QObject* p=nullptr) : QObject(p) {}
protected:
    bool eventFilter(QObject* o, QEvent* e) override {
        if(e->type()==QEvent::MouseButtonRelease){
            auto* me=static_cast<QMouseEvent*>(e); auto* wdg=qobject_cast<QWidget*>(o);
            if(me->button()==Qt::LeftButton && wdg && wdg->rect().contains(me->position().toPoint())){ if(cb) cb(); return true; }
        }
        return QObject::eventFilter(o,e);
    }
};

// A template card: an icon tile + title + mono meta, in a hover-lifting tile. It's a QFrame (which sizes
// to its layout — a QPushButton would not) with a ClickFilter for the click and QSS `:hover` for the border.
static QFrame* mkCard(const QString& title, const QString& meta, const QString& icon,
                      const QColor& tileBg, const QColor& tileFg, bool dashed, std::function<void()> onClick){
    auto* b=new QFrame; b->setObjectName(dashed?"tplCardDashed":"tplCard");
    b->setCursor(Qt::PointingHandCursor); b->setMinimumHeight(76); b->setAttribute(Qt::WA_Hover);
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* h=new QHBoxLayout(b); h->setContentsMargins(16,14,16,14); h->setSpacing(14);
    auto* tile=new QLabel; tile->setObjectName("cardTile"); tile->setFixedSize(44,44);
    tile->setPixmap(tilePix(icon,tileBg,tileFg)); tile->setAttribute(Qt::WA_TransparentForMouseEvents);
    h->addWidget(tile);
    auto* tv=new QVBoxLayout; tv->setContentsMargins(0,0,0,0); tv->setSpacing(3);
    auto* t=new QLabel(title); t->setObjectName("cardTitle"); t->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto* m=new QLabel(meta);  m->setObjectName("cardMeta");  m->setAttribute(Qt::WA_TransparentForMouseEvents);
    { QFont mf("JetBrains Mono"); if(!QFontDatabase::families().contains("JetBrains Mono")) mf=QFont("Ubuntu Mono"); mf.setPixelSize(11); m->setFont(mf); }
    tv->addWidget(t); tv->addWidget(m); h->addLayout(tv,1);
    if(!dashed){ auto* sh=new QGraphicsDropShadowEffect(b); sh->setBlurRadius(16); sh->setOffset(0,3); sh->setColor(QColor(0,0,0,90)); b->setGraphicsEffect(sh); }
    auto* cf=new ClickFilter(b); cf->cb=std::move(onClick); b->installEventFilter(cf);
    return b;
}

// A flat sidebar/footer link row built from click-through labels inside a borderless QPushButton.
static QPushButton* mkLinkRow(const QString& text){
    auto* b=new QPushButton; b->setObjectName("sideLink"); b->setCursor(Qt::PointingHandCursor);
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* h=new QHBoxLayout(b); h->setContentsMargins(6,16,6,8); h->setSpacing(8);
    auto* g=new QLabel; g->setPixmap(actionIcon("globe",15,tok::tert).pixmap(15,15)); g->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto* t=new QLabel(text); t->setStyleSheet("color:#6E7787;font-size:12px;font-weight:600;background:transparent;"); t->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto* a=new QLabel; a->setPixmap(actionIcon("arrow-up-right",13,tok::tert).pixmap(13,13)); a->setAttribute(Qt::WA_TransparentForMouseEvents);
    h->addWidget(g); h->addWidget(t); h->addStretch(); h->addWidget(a);
    return b;
}

QWidget* StartScreen::buildPanel(MainWindow* mw, std::function<void()> onChosen){
    auto* w=new QWidget; w->setObjectName("startPanel");
    w->setStyleSheet(QString::fromUtf8(kStartQss));
    auto* row=new QHBoxLayout(w); row->setContentsMargins(0,0,0,0); row->setSpacing(0);

    // ---- sidebar --------------------------------------------------------------------------------
    auto* side=new QFrame; side->setObjectName("startSidebar"); side->setFixedWidth(264);
    auto* sv=new QVBoxLayout(side); sv->setContentsMargins(16,18,16,18); sv->setSpacing(0);
    auto* newBtn=new QPushButton("New document"); newBtn->setObjectName("btnPrimary");
    newBtn->setFixedHeight(38); newBtn->setCursor(Qt::PointingHandCursor); newBtn->setDefault(true); newBtn->setAutoDefault(false);   // design size lg=38px; fixed so the default-button frame can't inflate it
    newBtn->setIcon(actionIcon("plus",16,Qt::white)); newBtn->setIconSize(QSize(16,16));
    sv->addWidget(newBtn);
    sv->addSpacing(8);
    auto* openBtn=new QPushButton("Open a file…"); openBtn->setObjectName("btnOutline");
    openBtn->setFixedHeight(30); openBtn->setCursor(Qt::PointingHandCursor);   // design size md=30px (shorter than the lg primary, per the mock)
    openBtn->setIcon(actionIcon("folder-open",14,QColor(0x7F,0xA5,0xFF))); openBtn->setIconSize(QSize(14,14));   // accent-300, matches the outline text
    sv->addWidget(openBtn);
    sv->addStretch();
    auto* link=mkLinkRow("xarq.org"); sv->addWidget(link);
    row->addWidget(side);

    // ---- main (grid background) -----------------------------------------------------------------
    auto* main=new GridBg; main->setObjectName("startMain");
    auto* mvl=new QVBoxLayout(main); mvl->setContentsMargins(0,0,0,0); mvl->setSpacing(0);

    auto* scroll=new QScrollArea; scroll->setObjectName("startScroll"); scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame); scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->viewport()->setAutoFillBackground(false);
    auto* content=new QWidget; content->setObjectName("startContent"); content->setMaximumWidth(1080);
    scroll->setWidget(content);
    auto* mv=new QVBoxLayout(content); mv->setContentsMargins(34,26,34,26); mv->setSpacing(0);

    // header: title + subtitle | search
    { auto* hr=new QHBoxLayout; hr->setSpacing(24);
      auto* tv=new QVBoxLayout; tv->setSpacing(7);
      auto* h1=new QLabel("Welcome to XarQ"); h1->setObjectName("h1");
      auto* sub=new QLabel("Start a new document, or pick up where you left off."); sub->setObjectName("sub");
      tv->addWidget(h1); tv->addWidget(sub); hr->addLayout(tv); hr->addStretch();
      auto* search=new QLineEdit; search->setObjectName("search"); search->setFixedWidth(280); search->setMinimumHeight(30);
      search->setPlaceholderText("Search files and templates");
      search->addAction(actionIcon("search",15,tok::tert), QLineEdit::LeadingPosition);
      hr->addWidget(search, 0, Qt::AlignBottom);
      mv->addLayout(hr);
      // the live filter is wired at the end of the function (search is found via findChild on `content`)
    }

    // ---- recovery banner (only on a clean launch that left unsaved work) -------------------------
    const auto recov = (mw->tabCount()==0) ? mw->recoveryEntries() : QVector<QPair<QString,QString>>{};
    if(!recov.isEmpty()){
        mv->addSpacing(22);
        auto* card=new QFrame; card->setObjectName("recBanner");
        auto* ch=new QHBoxLayout(card); ch->setContentsMargins(14,12,14,12); ch->setSpacing(14);
        auto* ic=new QLabel; ic->setObjectName("recIcon"); ic->setFixedSize(32,32); ic->setAlignment(Qt::AlignCenter);
        ic->setPixmap(actionIcon("history",17,tok::warning).pixmap(17,17)); ch->addWidget(ic,0,Qt::AlignVCenter);
        auto* mid=new QVBoxLayout; mid->setSpacing(6);
        auto* ttl=new QLabel(QString("%1 unsaved document%2 recovered from your last session")
                               .arg(recov.size()).arg(recov.size()==1?"":"s")); ttl->setObjectName("recTitle");
        mid->addWidget(ttl);
        auto* chips=new QHBoxLayout; chips->setSpacing(6);
        for(const auto& e : recov){ const QString orig=e.second;
            const QString name = orig.isEmpty()? QStringLiteral("Untitled document") : QFileInfo(orig).fileName();
            const QString meta = orig.isEmpty()? QStringLiteral("never saved") : QFileInfo(orig).absolutePath();
            auto* chip=new QLabel(name+"  <span style='color:#6E7787'>· "+meta.toHtmlEscaped()+"</span>");
            chip->setObjectName("recChip"); chip->setTextFormat(Qt::RichText);
            QFont mf("JetBrains Mono"); if(!QFontDatabase::families().contains("JetBrains Mono")) mf=QFont("Ubuntu Mono"); mf.setPixelSize(11); chip->setFont(mf);
            chips->addWidget(chip); }
        chips->addStretch(); mid->addLayout(chips); ch->addLayout(mid,1);
        auto* recBtn=new QPushButton(recov.size()==1?"Recover":"Recover all"); recBtn->setObjectName("btnSecondary");
        recBtn->setMinimumHeight(24); recBtn->setCursor(Qt::PointingHandCursor);
        auto* disc=new QPushButton("Discard"); disc->setObjectName("btnGhost"); disc->setMinimumHeight(24); disc->setCursor(Qt::PointingHandCursor);
        ch->addWidget(recBtn,0,Qt::AlignVCenter); ch->addWidget(disc,0,Qt::AlignVCenter);
        mv->addWidget(card);
        QObject::connect(recBtn,&QPushButton::clicked,w,[mw,onChosen]{ mw->recoverAll(); if(onChosen) onChosen(); });
        QObject::connect(disc,&QPushButton::clicked,w,[mw,card]{ mw->discardRecovery(); card->hide(); });
    }

    // ---- start a new document: eyebrow + Browse link + 3-col grid of template cards --------------
    mv->addSpacing(30);
    { auto* hr=new QHBoxLayout;
      auto* eye=new QLabel("START A NEW DOCUMENT"); eye->setObjectName("eyebrow");
      auto* browse=new QPushButton("Browse all templates →"); browse->setObjectName("btnGhost");
      browse->setStyleSheet("color:#5488FF;background:transparent;border:none;font-size:12px;"); browse->setCursor(Qt::PointingHandCursor);
      hr->addWidget(eye); hr->addStretch(); hr->addWidget(browse); mv->addLayout(hr);
      QObject::connect(browse,&QPushButton::clicked,w,[mw,w,onChosen]{
          bool ok; const QStringList names=Canvas::templateNames();
          const QString sel=QInputDialog::getItem(w,"New from Template","Template:",names,1,false,&ok);
          if(ok){ mw->newFromTemplate(qMax(0,names.indexOf(sel))); if(onChosen) onChosen(); } });
    }
    mv->addSpacing(14);
    auto* grid=new QGridLayout; grid->setHorizontalSpacing(14); grid->setVerticalSpacing(14);
    mv->addLayout(grid);

    struct CardDef { QString title, meta, icon; QColor bg, fg; bool dashed; int action; };  // action: 0..4 template, 5 custom
    const QList<CardDef> defs = {
        {"Blank document","Empty canvas","file",           QColor(45,111,247,41),  QColor(0x7F,0xA5,0xFF), false, 0},
        {"Landing page","Web · hero + sections","monitor",  QColor(139,92,246,41),  QColor(0xA8,0x89,0xFB), false, 1},
        {"Business card","Print · 85 × 55 mm","credit-card",QColor(34,195,184,41),  QColor(0x3F,0xD2,0xC7), false, 2},
        {"Photo portfolio","Photo · grid","images",         QColor(226,85,161,41),  QColor(0xEC,0x74,0xB6), false, 3},
        {"Presentation","1920 × 1080 px","presentation",    QColor(245,178,61,41),  QColor(0xF5,0xB2,0x3D), false, 4},
        {"Custom size","Set your own","plus",               tok::well,              tok::sec,               true,  5},
    };
    // keep (button, haystack, def) so search can re-flow the grid to matches only
    struct CardEntry { QWidget* btn; QString hay; };
    QList<CardEntry> entries;
    for(const CardDef& d : defs){
        const int act=d.action;
        auto onClick=[mw,act,w,onChosen]{
            if(act<5){ mw->newFromTemplate(act); }
            else {                                   // Custom size — ask for W×H (px)
                QDialog dlg(w); dlg.setWindowTitle("Custom document size");
                auto* fl=new QFormLayout(&dlg);
                auto* sw=new QSpinBox; sw->setRange(1,20000); sw->setValue(1200); sw->setSuffix(" px");
                auto* sh=new QSpinBox; sh->setRange(1,20000); sh->setValue(800);  sh->setSuffix(" px");
                fl->addRow("Width:",sw); fl->addRow("Height:",sh);
                auto* bb=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);
                fl->addRow(bb); QObject::connect(bb,&QDialogButtonBox::accepted,&dlg,&QDialog::accept); QObject::connect(bb,&QDialogButtonBox::rejected,&dlg,&QDialog::reject);
                if(dlg.exec()!=QDialog::Accepted) return;
                mw->newBlankSized(sw->value(), sh->value());
            }
            if(onChosen) onChosen(); };
        auto* c=mkCard(d.title,d.meta,d.icon,d.bg,d.fg,d.dashed,onClick);
        entries.append({c, (d.title+" "+d.meta).toLower()});
    }
    auto relayout=[grid,entries](const QString& q){
        while(auto* it=grid->takeAt(0)){ delete it; }        // detach (widgets kept, re-added below)
        int i=0; const QString needle=q.trimmed().toLower();
        for(const CardEntry& e : entries){ const bool ok=needle.isEmpty()||e.hay.contains(needle);
            e.btn->setVisible(ok); if(ok){ grid->addWidget(e.btn, i/3, i%3); ++i; } }
    };
    relayout(QString());

    // ---- recent files: empty state --------------------------------------------------------------
    mv->addSpacing(32);
    { auto* eye=new QLabel("RECENT FILES"); eye->setObjectName("eyebrow"); mv->addWidget(eye); mv->addSpacing(14); }
    const QStringList recents=mw->recentFiles();
    if(recents.isEmpty()){
        auto* panel=new QFrame; panel->setObjectName("emptyPanel");
        auto* ev=new QVBoxLayout(panel); ev->setContentsMargins(24,40,24,40); ev->setSpacing(14); ev->setAlignment(Qt::AlignHCenter);
        auto* ic=new QLabel; ic->setObjectName("emptyIcon"); ic->setFixedSize(52,52); ic->setAlignment(Qt::AlignCenter);
        ic->setPixmap(actionIcon("folder-open",24,tok::tert).pixmap(24,24)); ev->addWidget(ic,0,Qt::AlignHCenter);
        auto* t=new QLabel("No recent files yet"); t->setObjectName("emptyTitle"); t->setAlignment(Qt::AlignHCenter); ev->addWidget(t);
        auto* bdy=new QLabel("Create a new document or open an existing file to get started."); bdy->setObjectName("emptyBody"); bdy->setAlignment(Qt::AlignHCenter); ev->addWidget(bdy);
        auto* ob=new QPushButton("Open a file…"); ob->setObjectName("btnGhost"); ob->setMinimumHeight(24); ob->setCursor(Qt::PointingHandCursor);
        ob->setIcon(actionIcon("folder-open",15,tok::sec)); ob->setIconSize(QSize(15,15));
        ev->addWidget(ob,0,Qt::AlignHCenter);
        mv->addWidget(panel);
        QObject::connect(ob,&QPushButton::clicked,w,[mw,w,onChosen]{
            const QString p=QFileDialog::getOpenFileName(w,"Open","",MainWindow::supportedOpenFilter(),nullptr,QFileDialog::DontUseNativeDialog|QFileDialog::HideNameFilterDetails);
            if(!p.isEmpty()){ mw->openPath(p); if(onChosen) onChosen(); } });
    } else {
        auto* list=new QListWidget; list->setObjectName("startRecent");
        list->setStyleSheet("QListWidget#startRecent{background:#171A20;border:1px solid rgba(255,255,255,0.08);border-radius:5px;padding:6px;}"
                            "QListWidget#startRecent::item{padding:8px 10px;border-radius:3px;color:#D2D8E0;}"
                            "QListWidget#startRecent::item:hover{background:#1D2027;}"
                            "QListWidget#startRecent::item:selected{background:rgba(45,111,247,0.24);color:#EEF1F5;}");
        for(const QString& p : recents){ QFileInfo fi(p);
            auto* it=new QListWidgetItem(fi.fileName()+"   —   "+fi.absolutePath()); it->setData(Qt::UserRole,p); list->addItem(it); }
        QObject::connect(list,&QListWidget::itemClicked,w,[mw,onChosen](QListWidgetItem* it){ mw->openPath(it->data(Qt::UserRole).toString()); if(onChosen) onChosen(); });
        mv->addWidget(list,1);
    }
    mv->addStretch();

    mvl->addWidget(scroll,1);

    // ---- footer: startup preference (left) + version string (right, mono) -----------------------
    auto* footer=new QFrame; footer->setObjectName("footer");
    auto* fh=new QHBoxLayout(footer); fh->setContentsMargins(34,12,34,12);
    auto* showChk=new QCheckBox("Show this screen on startup"); showChk->setObjectName("showChk");
    showChk->setChecked(QSettings("XarQ","XarQ").value("showWelcome",true).toBool());
    QObject::connect(showChk,&QCheckBox::toggled,w,[](bool on){ QSettings("XarQ","XarQ").setValue("showWelcome",on); });
    fh->addWidget(showChk); fh->addStretch();
    auto* ver=new QLabel("XarQ · Qt6 · open pipeline"); ver->setObjectName("ver");
    { QFont mf("JetBrains Mono"); if(!QFontDatabase::families().contains("JetBrains Mono")) mf=QFont("Ubuntu Mono"); mf.setPixelSize(11); ver->setFont(mf); }
    fh->addWidget(ver);
    mvl->addWidget(footer);

    row->addWidget(main,1);

    // ---- wiring: sidebar buttons + live search ---------------------------------------------------
    QObject::connect(newBtn,&QPushButton::clicked,w,[mw,onChosen]{ mw->newFromTemplate(0); if(onChosen) onChosen(); });
    auto openFile=[mw,w,onChosen]{
        const QString p=QFileDialog::getOpenFileName(w,"Open","",MainWindow::supportedOpenFilter(),nullptr,QFileDialog::DontUseNativeDialog|QFileDialog::HideNameFilterDetails);
        if(!p.isEmpty()){ mw->openPath(p); if(onChosen) onChosen(); } };
    QObject::connect(openBtn,&QPushButton::clicked,w,openFile);
    QObject::connect(link,&QPushButton::clicked,w,[]{ QDesktopServices::openUrl(QUrl("https://xarq.org")); });
    if(auto* search=content->findChild<QLineEdit*>()){
        QObject::connect(search,&QLineEdit::textChanged,w,[relayout](const QString& t){ relayout(t); });
    }
    return w;
}

void StartScreen::show(MainWindow* mw){
    QDialog dlg(mw); dlg.setObjectName("startDialog"); dlg.setWindowTitle("Welcome to XarQ");
    dlg.setModal(true); dlg.resize(1120,720);
    dlg.setStyleSheet(QString::fromUtf8(kStartQss));
    auto* outer=new QVBoxLayout(&dlg); outer->setContentsMargins(0,0,0,0); outer->setSpacing(0);
    QWidget* panel=buildPanel(mw,[&dlg]{ dlg.accept(); });
    outer->addWidget(panel,1);
    dlg.exec();
}

} // namespace xara
