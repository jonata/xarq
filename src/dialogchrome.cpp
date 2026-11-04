#include "dialogchrome.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLayout>
#include <QGridLayout>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>
#include <QPointer>
#include <utility>

namespace xara {
namespace {

// Per-dialog glue, owned by the dialog: drags the window by its title, lights the notch while the close
// button is hovered, and follows title changes.
class ChromeGlue : public QObject {
public:
    QPointer<QPushButton> m_default;   // the footer button Enter presses
    ChromeGlue(QDialog* d, QWidget* title, QLabel* label, QWidget* notch, QToolButton* close)
        : QObject(d), m_dlg(d), m_title(title), m_label(label), m_notch(notch), m_close(close) {
        d->installEventFilter(this); title->installEventFilter(this); label->installEventFilter(this);
        close->installEventFilter(this);
    }
protected:
    bool eventFilter(QObject* o, QEvent* e) override {
        const QEvent::Type t=e->type();
        if(o==m_dlg && t==QEvent::WindowTitleChange){ m_label->setText(m_dlg->windowTitle()); return false; }
        if(o==m_dlg && t==QEvent::Show && m_default){ m_default->setDefault(true); return false; }   // a QDialog settles its default on show
        if(o==m_close && (t==QEvent::Enter || t==QEvent::Leave)){
            const bool on=t==QEvent::Enter;
            m_notch->setProperty("hover",on); m_notch->style()->unpolish(m_notch); m_notch->style()->polish(m_notch); m_notch->update();
            m_close->setIcon(QIcon(on? ":/xarq/assets/chrome/dialog-close-hover.svg" : ":/xarq/assets/chrome/dialog-close.svg"));
            return false; }
        if(o==m_title || o==m_label){
            auto* me=static_cast<QMouseEvent*>(e);
            if(t==QEvent::MouseButtonPress && me->button()==Qt::LeftButton){
                m_grab=me->globalPosition().toPoint()-m_dlg->frameGeometry().topLeft(); m_dragging=true; return true; }
            if(t==QEvent::MouseMove && m_dragging && (me->buttons()&Qt::LeftButton)){ m_dlg->move(me->globalPosition().toPoint()-m_grab); return true; }
            if(t==QEvent::MouseButtonRelease && m_dragging){ m_dragging=false; return true; }
        }
        return false; }
private:
    QDialog* m_dlg; QWidget* m_title; QLabel* m_label; QWidget* m_notch; QToolButton* m_close;
    QPoint m_grab; bool m_dragging=false;
};

// The dialog's own button box: the last visible one sitting directly on the dialog (a box inside a page
// or group belongs to that part of the form, not to the dialog).
QDialogButtonBox* dialogButtonBox(QDialog* d){
    QDialogButtonBox* found=nullptr;
    for(QDialogButtonBox* b : d->findChildren<QDialogButtonBox*>(Qt::FindDirectChildrenOnly))
        if(!b->isHidden() && !b->buttons().isEmpty()) found=b;
    return found;
}

// The footer shows stand-ins for the button box's buttons. Moving the real ones out does not hold: Qt's button
// box watches them and lays every one of them out in itself again. So the box stays, hidden, with its buttons;
// each stand-in clicks its original and copies its text, icon, enabled and visible state whenever they change.
class ButtonMirror : public QObject {
public:
    ButtonMirror(QDialog* d, QDialogButtonBox* box) : QObject(d) { box->installEventFilter(this); }
    void add(QAbstractButton* orig, QPushButton* proxy){
        m_pairs.append({orig,proxy}); orig->installEventFilter(this);
        QObject::connect(proxy,&QPushButton::clicked,orig,[orig]{ orig->click(); });
        sync(orig,proxy); }
    void syncAll(){ for(auto& p : m_pairs) if(p.first && p.second) sync(p.first,p.second); }
protected:
    bool eventFilter(QObject*, QEvent* e) override {
        switch(e->type()){
        case QEvent::EnabledChange: case QEvent::ShowToParent: case QEvent::HideToParent: case QEvent::LayoutRequest:
            QMetaObject::invokeMethod(this,[this]{ syncAll(); },Qt::QueuedConnection); break;   // after the change lands
        default: break; }
        return false; }
private:
    static void sync(QAbstractButton* o, QPushButton* p){
        // Subtitld's footer buttons are bare capitals: no icon, no underlined mnemonic.
        QString t=o->text(); t.replace("&&",QChar(0x1F)).remove('&').replace(QChar(0x1F),"&");
        if(p->text()!=t) p->setText(t);
        p->setEnabled(o->isEnabled()); p->setVisible(!o->isHidden()); }
    QList<std::pair<QPointer<QAbstractButton>,QPointer<QPushButton>>> m_pairs;
};

bool isRejectSide(QDialogButtonBox::ButtonRole r){ return r==QDialogButtonBox::RejectRole || r==QDialogButtonBox::NoRole; }

} // namespace

void DialogChrome::apply(QDialog* d){
    // Subtitld's message dialogs are text alone, so the stock information/warning/question icon goes; a picture
    // the code chose (the About box's logo) stays. Done first: QMessageBox rebuilds its layout when its icon changes.
    if(auto* mb=qobject_cast<QMessageBox*>(d); mb && mb->icon()!=QMessageBox::NoIcon){
        mb->setIcon(QMessageBox::NoIcon);
        if(auto* il=mb->findChild<QLabel*>("qt_msgboxex_icon_label")) il->hide(); }
    QLayout* body=d->layout();
    if(!body) return;                                     // nothing laid out to wrap
    d->setProperty("xqChromed",true);

    // Everything sits in one frame so the background is a single gradient top to bottom, with the 4px
    // corners showing through the translucent window. The 1px vertical margins keep the frame's border.
    auto* frame=new QWidget; frame->setObjectName("dialogFrame"); frame->setAttribute(Qt::WA_StyledBackground,true);
    auto* fl=new QVBoxLayout(frame); fl->setContentsMargins(0,1,0,1); fl->setSpacing(0);

    // Title: the label straight on the gradient, then the notch behind the close button.
    auto* title=new QWidget; title->setObjectName("dialogTitle"); title->setFixedHeight(32);
    auto* tl=new QHBoxLayout(title); tl->setContentsMargins(0,0,0,0); tl->setSpacing(0);
    auto* label=new QLabel(d->windowTitle()); label->setObjectName("dialogTitleLabel");
    tl->addWidget(label,0); tl->addStretch(1);
    auto* notch=new QWidget; notch->setObjectName("dialogTitleNotch"); notch->setAttribute(Qt::WA_StyledBackground,true);
    auto* nl=new QHBoxLayout(notch); nl->setContentsMargins(24,0,1,1); nl->setSpacing(0);   // the icon clears the slant
    auto* close=new QToolButton; close->setObjectName("dialogClose"); close->setFixedSize(34,31);  // 31: the notch's hairline is row 32
    close->setFocusPolicy(Qt::NoFocus); close->setAutoRaise(true); close->setToolTip("Close");
    close->setIcon(QIcon(":/xarq/assets/chrome/dialog-close.svg")); close->setIconSize(QSize(16,16));
    QObject::connect(close,&QToolButton::clicked,d,&QDialog::reject);
    nl->addWidget(close); tl->addWidget(notch,0);
    fl->addWidget(title);

    // Body: the dialog's own layout, moved as it is into a 1px-inset row so the frame's side border shows.
    // Layouts left on the style's default margins move onto Subtitld's 15px text edge.
    QDialogButtonBox* box=dialogButtonBox(d);
    auto* content=new QWidget; content->setObjectName("dialogContent");
    const int defMargin=d->style()->pixelMetric(QStyle::PM_LayoutLeftMargin,nullptr,d);
    if(body->contentsMargins()==QMargins(defMargin,defMargin,defMargin,defMargin)) body->setContentsMargins(15,10,15,12);
    if(auto* mb=qobject_cast<QMessageBox*>(d)){       // a text dialog: Subtitld's roomier padding, text on the 15px edge
        body->setContentsMargins(0,16,15,16);          // the left 15px is QMessageBox's own indent spacer
        if(auto* g=qobject_cast<QGridLayout*>(body)) g->setHorizontalSpacing(0);
        if(auto* tl2=mb->findChild<QLabel*>("qt_msgbox_label")) tl2->setIndent(0); }
    content->setLayout(body);                             // QWidget::setLayout takes a layout off its old widget
    auto* row=new QWidget; auto* rl=new QHBoxLayout(row); rl->setContentsMargins(1,0,1,0); rl->setSpacing(0);
    rl->addWidget(content); fl->addWidget(row,1);

    // Footer: Cancel-like buttons as plain text on the left, the rest on the tab at the right with the
    // affirmative one rightmost. The buttons move out of the box; their signals stay connected to it.
    QPushButton* defaultBtn=nullptr;
    if(box){
        auto* bottom=new QWidget; bottom->setObjectName("dialogBottom");
        auto* bl=new QHBoxLayout(bottom); bl->setContentsMargins(0,0,0,0); bl->setSpacing(0);
        auto* left=new QWidget; left->setObjectName("dialogBottomLeft"); left->setAttribute(Qt::WA_StyledBackground,true);
        auto* ll=new QHBoxLayout(left); ll->setContentsMargins(0,0,0,0); ll->setSpacing(0);
        auto* tab=new QWidget; tab->setObjectName("dialogBottomRight"); tab->setAttribute(Qt::WA_StyledBackground,true);
        auto* rl2=new QHBoxLayout(tab); rl2->setContentsMargins(24,0,5,0); rl2->setSpacing(0);   // clear the diagonal and the right cap
        QList<QAbstractButton*> leftBtns, tabBtns, lastBtns;
        for(QAbstractButton* b : box->buttons()){
            const auto r=box->buttonRole(b);
            if(isRejectSide(r)) leftBtns<<b;
            else if(r==QDialogButtonBox::AcceptRole || r==QDialogButtonBox::YesRole) lastBtns<<b;
            else tabBtns<<b;
        }
        tabBtns+=lastBtns;
        if(tabBtns.isEmpty()){ tabBtns=leftBtns; leftBtns.clear(); }   // a lone Close is the dialog's main button
        auto* mirror=new ButtonMirror(d,box);
        QPushButton* main=nullptr; QPushButton* chosen=nullptr;
        auto stand=[&](QAbstractButton* b, const char* side, QHBoxLayout* into){
            auto* p=new QPushButton; p->setProperty("footer",side);
            into->addWidget(p); mirror->add(b,p);
            if(auto* pb=qobject_cast<QPushButton*>(b); pb && pb->isDefault()) chosen=p;
            return p; };
        for(QAbstractButton* b : leftBtns) stand(b,"left",ll);
        ll->addStretch(1);
        for(QAbstractButton* b : tabBtns) main=stand(b,"tab",rl2);
        // Enter presses the dialog's default button among the visible ones: the one the code chose, else the
        // rightmost on the tab.
        defaultBtn = chosen? chosen : main;
        bl->addWidget(left,1); bl->addWidget(tab,0);
        box->hide();
        fl->addWidget(bottom);
    }

    auto* outer=new QVBoxLayout; outer->setContentsMargins(0,0,0,0); outer->setSpacing(0);
    outer->addWidget(frame);
    d->setLayout(outer);
    (new ChromeGlue(d,title,label,notch,close))->m_default=defaultBtn;

    // A size the code chose was for the body; the title bar comes on top of it.
    if(d->testAttribute(Qt::WA_Resized)) d->resize(d->width(), d->height()+title->height());
    d->setAttribute(Qt::WA_TranslucentBackground);
    d->setWindowFlags(d->windowFlags() | Qt::FramelessWindowHint);
}

bool DialogChrome::eventFilter(QObject* o, QEvent* e){
    if(e->type()==QEvent::Polish){
        auto* d=qobject_cast<QDialog*>(o);
        if(d && d->isWindow() && !d->property("xqChromed").toBool()
           && !(d->property("xqChrome").isValid() && !d->property("xqChrome").toBool())
           && !(d->windowFlags() & Qt::FramelessWindowHint))
            apply(d);
    }
    return QObject::eventFilter(o,e);
}

} // namespace xara
