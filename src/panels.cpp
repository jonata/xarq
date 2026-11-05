#include "panels.h"
#include "icons.h"
#include "dialogs.h"
#include <QtWidgets>
#include <QTimer>
#include <QFontDatabase>
#include <QRegularExpression>
#include <climits>

namespace xara {

// ===== ColorButton ==========================================================
class ColorButton : public QPushButton {
    Q_OBJECT
public:
    explicit ColorButton(QWidget* p=nullptr):QPushButton(p){ setFixedSize(46,24); setCursor(Qt::PointingHandCursor);
        connect(this,&QPushButton::clicked,this,[this]{ QColor c=ColorEditor::pick(this,m_col); if(c.isValid()){ setColor(c); emit changed(c);} }); }
    void setColor(const QColor& c){ m_col=c; update(); }
    QColor color() const { return m_col; }
signals: void changed(QColor);
protected:
    void paintEvent(QPaintEvent*) override { QPainter g(this); g.setRenderHint(QPainter::Antialiasing);
        QRectF r=rect().adjusted(1,1,-1,-1);
        // checker for alpha
        g.setPen(Qt::NoPen); for(int y=0;y<r.height();y+=6)for(int x=0;x<r.width();x+=6){ g.setBrush(((x/6+y/6)&1)?QColor(90,90,96):QColor(60,60,66)); g.drawRect(QRectF(r.x()+x,r.y()+y,6,6)); }
        g.setBrush(m_col); g.drawRoundedRect(r,4,4);
        g.setBrush(Qt::NoBrush); g.setPen(QPen(QColor(0,0,0,90),1)); g.drawRoundedRect(r,4,4); }
    QColor m_col=Qt::white;
};

// ===== GradientBar ==========================================================
class GradientBar : public QWidget {
    Q_OBJECT
public:
    explicit GradientBar(QWidget* p=nullptr):QWidget(p){ setFixedHeight(26); setMinimumWidth(120); setCursor(Qt::PointingHandCursor);}
    void setStops(const QVector<GradStop>& s){ m_stops=s; m_sel=qBound(0,m_sel,m_stops.size()-1); update(); }
    QVector<GradStop> stops() const { return m_stops; }
    int selected() const { return m_sel; }
signals: void changed(); void stopSelected(int);
protected:
    void paintEvent(QPaintEvent*) override { QPainter g(this); g.setRenderHint(QPainter::Antialiasing);
        QRectF r=rect().adjusted(2,2,-2,-8); QLinearGradient lg(r.left(),0,r.right(),0);
        for(auto&s:m_stops) lg.setColorAt(qBound(0.0,s.pos,1.0),s.color);
        g.setPen(QPen(QColor(0,0,0,90),1)); g.setBrush(lg); g.drawRoundedRect(r,3,3);
        for(int i=0;i<m_stops.size();++i){ double x=r.left()+m_stops[i].pos*r.width(); QPolygonF tri; tri<<QPointF(x,r.bottom())<<QPointF(x-4,r.bottom()+6)<<QPointF(x+4,r.bottom()+6);
            g.setBrush(m_stops[i].color); g.setPen(QPen(i==m_sel?QColor(80,160,255):QColor(20,20,24),i==m_sel?2:1)); g.drawPolygon(tri); } }
    void mousePressEvent(QMouseEvent* e) override { QRectF r=rect().adjusted(2,2,-2,-8); double t=qBound(0.0,(e->position().x()-r.left())/r.width(),1.0);
        int best=-1; double bd=0.05; for(int i=0;i<m_stops.size();++i){ double d=qAbs(m_stops[i].pos-t); if(d<bd){bd=d;best=i;} }
        if(e->button()==Qt::RightButton){ // right-click removes an interior stop
            if(best>0 && best<m_stops.size()-1){ m_stops.remove(best); m_sel=qBound(0,m_sel,m_stops.size()-1); emit changed(); emit stopSelected(m_sel); update(); } return; }
        if(best<0){ m_stops.append({t,QColor(200,200,200)}); std::sort(m_stops.begin(),m_stops.end(),[](auto&a,auto&b){return a.pos<b.pos;}); for(int i=0;i<m_stops.size();++i) if(qAbs(m_stops[i].pos-t)<1e-6) best=i; emit changed(); }
        m_sel=best; emit stopSelected(best); update(); }
    void mouseMoveEvent(QMouseEvent* e) override { if(!(e->buttons()&Qt::LeftButton)||m_sel<0||m_sel==0||m_sel==m_stops.size()-1)return; QRectF r=rect().adjusted(2,2,-2,-8);
        m_stops[m_sel].pos=qBound(0.02,(e->position().x()-r.left())/r.width(),0.98); emit changed(); update(); }
    void mouseDoubleClickEvent(QMouseEvent*) override { if(m_sel<0)return; QColor c=ColorEditor::pick(this,m_stops[m_sel].color); if(c.isValid()){ m_stops[m_sel].color=c; emit changed(); update(); } }
    QVector<GradStop> m_stops; int m_sel=0;
};

// helper to build a labelled group
static QGroupBox* group(const QString& t){ auto* b=new QGroupBox(t); return b; }
// A tightened QFormLayout for the Properties cards: denser rows + minimal margins (DS: compact inspector).
static QFormLayout* formIn(QWidget* gb){ auto* f=new QFormLayout(gb); f->setVerticalSpacing(4); f->setHorizontalSpacing(8);
    f->setContentsMargins(2,2,2,2); f->setLabelAlignment(Qt::AlignLeft); return f; }

// ===== PropertiesPanel ======================================================
PropertiesPanel::PropertiesPanel(Canvas* c, QWidget* parent):QWidget(parent),m_c(c) {
    auto* scroll=new QScrollArea(this); scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
    auto* host=new QWidget; host->setObjectName("propsHost"); auto* V=new QVBoxLayout(host); V->setContentsMargins(7,7,7,7); V->setSpacing(7);

    m_titleLbl=new QLabel("No selection"); m_titleLbl->setObjectName("panelTitle"); V->addWidget(m_titleLbl);

    // FILL — every type the renderer supports (combo order == FillType enum 0..7)
    { auto* gb=group("Fill"); auto* f=new QVBoxLayout(gb);
      auto* row=new QHBoxLayout; m_fillType=new QComboBox;
      m_fillType->addItems({"No fill","Flat","Linear","Circular","Conical","Bitmap","3/4-Colour","Diamond"});
      m_fillColor=new ColorButton; row->addWidget(m_fillType,1); row->addWidget(m_fillColor); f->addLayout(row);
      m_gradBar=new GradientBar; f->addWidget(m_gradBar);
      // gradient options: repeat mode + rainbow + fractal
      m_gradOpts=new QWidget; auto* go=new QHBoxLayout(m_gradOpts); go->setContentsMargins(0,0,0,0); go->setSpacing(6);
      m_fillSpread=new QComboBox; m_fillSpread->addItems({"Pad","Repeat","Reflect"}); m_fillSpread->setToolTip("Gradient repeat");
      m_fillRainbow=new QCheckBox("Rainbow"); m_fillRainbow->setToolTip("Cycle hues around the colour wheel");
      go->addWidget(new QLabel("Repeat")); go->addWidget(m_fillSpread,1); go->addWidget(m_fillRainbow); f->addWidget(m_gradOpts);
      connect(m_fillType,QOverload<int>::of(&QComboBox::activated),this,[this](int i){ if(m_block)return; apply([&](Shape&s){ FillType t=FillType(i); s.fill.type=t; s.fill.fractal=false;
            bool grad=(t==FillType::Linear||t==FillType::Radial||t==FillType::Conical||t==FillType::Square);
            if(grad && s.fill.stops.size()<2){ QColor base=s.fill.representativeColor(); s.fill.stops={{0.0,base.lighter(135)},{1.0,base.darker(150)}}; }
            if(grad && s.fill.p0==s.fill.p1){ QRectF lb=s.localBounds(); s.fill.p0=lb.topLeft(); s.fill.p1=lb.bottomRight(); }
            if(t==FillType::Mesh && s.fill.meshColors.size()<3){ QColor b=s.fill.representativeColor(); s.fill.meshColors={b.lighter(140),b,b.darker(150),b.lighter(110)}; }
          },"Fill type"); refresh(); });
      connect(m_fillColor,&ColorButton::changed,this,[this](QColor c){ if(m_block)return;
          if(m_c->formatEditSelection([c](QFont&,QColor&col){ col=c; },"Text colour")) return;   // colour just the selected characters
          Fill nf=m_c->currentFill(); nf.color=c; if(nf.type==FillType::None) nf.type=FillType::Solid; m_c->setCurrentFill(nf); apply([&](Shape&s){ s.fill.type= (s.fill.type==FillType::None?FillType::Solid:s.fill.type); if(s.fill.type==FillType::Solid)s.fill.color=c; },"Fill colour"); });
      connect(m_gradBar,&GradientBar::changed,this,[this]{ if(m_block)return; apply([&](Shape&s){ s.fill.stops=m_gradBar->stops(); },"Gradient"); });
      connect(m_fillSpread,QOverload<int>::of(&QComboBox::activated),this,[this](int i){ if(m_block)return; QGradient::Spread sp=i==1?QGradient::RepeatSpread:i==2?QGradient::ReflectSpread:QGradient::PadSpread; apply([&](Shape&s){ s.fill.spread=sp; },"Gradient repeat"); });
      connect(m_fillRainbow,&QCheckBox::toggled,this,[this](bool b){ if(m_block)return; apply([&](Shape&s){ s.fill.rainbow=b; },"Rainbow"); });
      // 3/4-colour mesh corner colours
      m_meshRow=new QWidget; auto* mh=new QHBoxLayout(m_meshRow); mh->setContentsMargins(0,0,0,0); mh->setSpacing(4); mh->addWidget(new QLabel("Corners"));
      for(int k=0;k<4;++k){ m_meshCol[k]=new ColorButton; m_meshCol[k]->setFixedSize(30,22); mh->addWidget(m_meshCol[k]);
        connect(m_meshCol[k],&ColorButton::changed,this,[this,k](QColor c){ if(m_block)return; apply([&](Shape&s){ if(s.fill.type==FillType::Mesh){ if(s.fill.meshColors.size()<4) s.fill.meshColors.resize(4); s.fill.meshColors[k]=c; } },"Mesh colour"); }); }
      mh->addStretch(); f->addWidget(m_meshRow);
      // bitmap fill: load image, tile, tint(contone)
      m_bmpRow=new QWidget; auto* bh=new QHBoxLayout(m_bmpRow); bh->setContentsMargins(0,0,0,0); bh->setSpacing(6);
      m_bmpLoad=new QPushButton("Image…"); m_bmpTile=new QCheckBox("Tile"); m_bmpContone=new QCheckBox("Tint");
      bh->addWidget(m_bmpLoad); bh->addWidget(m_bmpTile); bh->addWidget(m_bmpContone); bh->addStretch(); f->addWidget(m_bmpRow);
      connect(m_bmpLoad,&QPushButton::clicked,this,[this]{ QString p=QFileDialog::getOpenFileName(this,"Bitmap fill","","Images (*.png *.jpg *.jpeg *.bmp *.gif *.tif *.webp)",nullptr,QFileDialog::DontUseNativeDialog); if(p.isEmpty())return; QImage im(p); if(im.isNull())return;
        apply([&](Shape&s){ s.fill.type=FillType::Bitmap; s.fill.image=im; s.fill.origImage=im; s.fill.tiled=false; QRectF lb=s.localBounds(); s.fill.p0=lb.bottomLeft(); s.fill.p1=lb.bottomRight(); s.fill.p2=lb.topLeft(); },"Bitmap fill"); refresh(); });
      connect(m_bmpTile,&QCheckBox::toggled,this,[this](bool b){ if(m_block)return; apply([&](Shape&s){ s.fill.tiled=b; },"Tile"); });
      connect(m_bmpContone,&QCheckBox::toggled,this,[this](bool b){ if(m_block)return; apply([&](Shape&s){ s.fill.contone=b; if(b&&s.fill.color2==s.fill.color)s.fill.color2=Qt::white; },"Tint"); });
      V->addWidget(gb); }

    // STROKE
    { auto* gb=group("Stroke"); auto* f=formIn(gb);
      auto* row=new QHBoxLayout; m_strokeOn=new QCheckBox; m_strokeColor=new ColorButton; row->addWidget(m_strokeOn); row->addWidget(m_strokeColor); row->addStretch();
      f->addRow("Enabled", row);
      m_strokeW=new QDoubleSpinBox; m_strokeW->setRange(0,200); m_strokeW->setDecimals(1); m_strokeW->setSingleStep(0.5); f->addRow("Width", m_strokeW);
      m_cap=new QComboBox; m_cap->addItems({"Butt","Square","Round"}); f->addRow("Cap", m_cap);
      m_join=new QComboBox; m_join->addItems({"Miter","Bevel","Round"}); f->addRow("Join", m_join);
      auto strokeApply=[this]{ if(m_block)return; apply([&](Shape&s){ s.stroke.enabled=m_strokeOn->isChecked(); s.stroke.color=m_strokeColor->color(); s.stroke.width=m_strokeW->value();
            s.stroke.cap=m_cap->currentIndex()==0?Qt::FlatCap:(m_cap->currentIndex()==1?Qt::SquareCap:Qt::RoundCap);
            s.stroke.join=m_join->currentIndex()==0?Qt::MiterJoin:(m_join->currentIndex()==1?Qt::BevelJoin:Qt::RoundJoin); },"Stroke");
          Stroke st=m_c->currentStroke(); st.enabled=m_strokeOn->isChecked(); st.color=m_strokeColor->color(); st.width=m_strokeW->value(); m_c->setCurrentStroke(st); };
      connect(m_strokeOn,&QCheckBox::toggled,this,strokeApply);
      connect(m_strokeColor,&ColorButton::changed,this,[strokeApply](QColor){ strokeApply(); });
      connect(m_strokeW,QOverload<double>::of(&QDoubleSpinBox::valueChanged),this,strokeApply);
      connect(m_cap,QOverload<int>::of(&QComboBox::activated),this,[strokeApply](int){strokeApply();});
      connect(m_join,QOverload<int>::of(&QComboBox::activated),this,[strokeApply](int){strokeApply();});
      V->addWidget(gb); }

    // OBJECT
    { auto* gb=group("Object"); auto* f=formIn(gb);
      m_opacity=new QSlider(Qt::Horizontal); m_opacity->setRange(0,100); m_opacityLbl=new QLabel("100%");
      auto* orow=new QHBoxLayout; orow->addWidget(m_opacity,1); orow->addWidget(m_opacityLbl); f->addRow("Opacity",orow);
      m_blend=new QComboBox; for(auto&p:blendModes()) m_blend->addItem(p.first); f->addRow("Blend",m_blend);
      connect(m_opacity,&QSlider::valueChanged,this,[this](int v){ m_opacityLbl->setText(QString::number(v)+"%"); if(m_block)return; apply([&](Shape&s){ s.opacity=v/100.0; },"Opacity"); });
      connect(m_blend,QOverload<int>::of(&QComboBox::activated),this,[this](int i){ if(m_block)return; apply([&](Shape&s){ s.blend=blendModes()[i].second; },"Blend"); });
      V->addWidget(gb); }

    // TRANSPARENCY (rich: flat / graduated + transparency-effect blend type)
    { auto* gb=group("Transparency"); auto* f=formIn(gb);
      m_transpType=new QComboBox; m_transpType->addItems({"None","Flat","Linear","Circular","Conical"}); f->addRow("Type",m_transpType);
      m_transpAmt=new QSlider(Qt::Horizontal); m_transpAmt->setRange(0,100); m_transpAmtLbl=new QLabel("0%");
      auto* trow=new QHBoxLayout; trow->addWidget(m_transpAmt,1); trow->addWidget(m_transpAmtLbl); f->addRow("Amount",trow);
      m_transpEffect=new QComboBox; m_transpEffect->addItems({"Mix (normal)","Stained glass","Bleach","Contrast","Darken","Lighten","Brightness"}); f->addRow("Effect",m_transpEffect);
      // effect index -> Transparency.type (Kernel fillval.h numbering)
      static const int kEff[]={1,2,3,4,6,7,8};
      auto applyTransp=[this]{ if(m_block)return; int gt=m_transpType->currentIndex(); double amt=m_transpAmt->value()/100.0; int eff=kEff[qBound(0,m_transpEffect->currentIndex(),6)];
          apply([&](Shape&s){ if(gt==0){ s.transp=Transparency(); return; } s.transp.type=eff;
              if(gt==1){ s.transp.geom=0; s.transp.t0=s.transp.t1=amt; }
              else if(gt<=3){ s.transp.geom=(gt==2)?1:2; s.transp.t0=0.0; s.transp.t1=amt;
                     QRectF lb=s.localBounds(); if(s.transp.p0==s.transp.p1){ s.transp.p0=lb.topLeft(); s.transp.p1=(gt==2)?lb.topRight():lb.center()+QPointF(lb.width()/2,0); s.transp.p2=lb.bottomLeft(); } }
              else { s.transp.geom=5; s.transp.t0=0.0; s.transp.t1=amt;   // conical
                     QRectF lb=s.localBounds(); s.transp.p0=lb.center(); s.transp.p1=lb.center()+QPointF(lb.width()/2,0); s.transp.p2=lb.bottomLeft(); }
          },"Transparency"); };
      connect(m_transpType,QOverload<int>::of(&QComboBox::activated),this,[applyTransp,this](int){ applyTransp(); refresh(); });
      connect(m_transpAmt,&QSlider::valueChanged,this,[this,applyTransp](int v){ m_transpAmtLbl->setText(QString::number(v)+"%"); applyTransp(); });
      connect(m_transpEffect,QOverload<int>::of(&QComboBox::activated),this,[applyTransp](int){ applyTransp(); });
      V->addWidget(gb); }

    // GEOMETRY (context)
    { m_geomRow=group("Shape"); auto* f=formIn(m_geomRow);
      m_corner=new QDoubleSpinBox; m_corner->setRange(0,500); f->addRow("Corner radius",m_corner);
      m_sides=new QSpinBox; m_sides->setRange(3,40); f->addRow("Sides",m_sides);
      m_star=new QCheckBox("Star"); f->addRow("",m_star);
      connect(m_corner,QOverload<double>::of(&QDoubleSpinBox::valueChanged),this,[this](double v){ if(m_block)return; apply([&](Shape&s){ s.cornerRadius=v; },"Corner"); });
      connect(m_sides,QOverload<int>::of(&QSpinBox::valueChanged),this,[this](int v){ if(m_block)return; m_c->setPolySides(v); apply([&](Shape&s){ s.polySides=v; },"Sides"); });
      connect(m_star,&QCheckBox::toggled,this,[this](bool b){ if(m_block)return; m_c->setPolyStar(b); apply([&](Shape&s){ s.polyStar=b; },"Star"); });
      V->addWidget(m_geomRow); }

    // EFFECTS
    { auto* gb=group("Effects"); auto* f=formIn(gb);
      auto* srow=new QHBoxLayout; m_shadowOn=new QCheckBox("Shadow"); m_shadowBlur=new QDoubleSpinBox; m_shadowBlur->setRange(0,64); m_shadowColor=new ColorButton;
      m_shadowType=new QComboBox; m_shadowType->addItems({"Wall","Floor","Glow"});
      srow->addWidget(m_shadowOn); srow->addWidget(m_shadowType); srow->addWidget(new QLabel("blur")); srow->addWidget(m_shadowBlur); srow->addWidget(m_shadowColor); f->addRow(srow);
      auto* crow=new QHBoxLayout; m_contourOn=new QCheckBox("Contour"); m_contourW=new QDoubleSpinBox; m_contourW->setRange(-60,60); m_contourSteps=new QSpinBox; m_contourSteps->setRange(1,12);
      crow->addWidget(m_contourOn); crow->addWidget(new QLabel("w")); crow->addWidget(m_contourW); crow->addWidget(new QLabel("steps")); crow->addWidget(m_contourSteps); f->addRow(crow);
      auto sh=[this]{ if(m_block)return; apply([&](Shape&s){ s.shadow.on=m_shadowOn->isChecked(); s.shadow.blur=m_shadowBlur->value(); s.shadow.color=m_shadowColor->color(); s.shadow.type=m_shadowType->currentIndex(); },"Shadow"); };
      connect(m_shadowOn,&QCheckBox::toggled,this,sh); connect(m_shadowBlur,QOverload<double>::of(&QDoubleSpinBox::valueChanged),this,sh); connect(m_shadowColor,&ColorButton::changed,this,[sh](QColor){sh();});
      connect(m_shadowType,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[sh](int){sh();});
      auto cn=[this]{ if(m_block)return; apply([&](Shape&s){ s.contour.on=m_contourOn->isChecked(); s.contour.width=m_contourW->value(); s.contour.steps=m_contourSteps->value(); },"Contour"); };
      connect(m_contourOn,&QCheckBox::toggled,this,cn); connect(m_contourW,QOverload<double>::of(&QDoubleSpinBox::valueChanged),this,cn); connect(m_contourSteps,QOverload<int>::of(&QSpinBox::valueChanged),this,cn);
      auto* brow=new QHBoxLayout; m_bevelOn=new QCheckBox("Bevel"); m_bevelDepth=new QDoubleSpinBox; m_bevelDepth->setRange(1,40);
      brow->addWidget(m_bevelOn); brow->addWidget(new QLabel("depth")); brow->addWidget(m_bevelDepth); brow->addStretch(); f->addRow(brow);
      auto bv=[this]{ if(m_block)return; apply([&](Shape&s){ s.bevel.on=m_bevelOn->isChecked(); s.bevel.depth=m_bevelDepth->value(); },"Bevel"); };
      connect(m_bevelOn,&QCheckBox::toggled,this,bv); connect(m_bevelDepth,QOverload<double>::of(&QDoubleSpinBox::valueChanged),this,bv);
      V->addWidget(gb); }

    V->addStretch();
    scroll->setWidget(host);
    auto* outer=new QVBoxLayout(this); outer->setContentsMargins(0,0,0,0); outer->addWidget(scroll);
    connect(m_c,&Canvas::selectionChanged,this,&PropertiesPanel::refresh);
    connect(m_c,&Canvas::documentChanged,this,&PropertiesPanel::refresh);
    refresh();
}

void PropertiesPanel::apply(const std::function<void(Shape&)>& fn, const QString& label) { m_c->applyToSelection(fn,label); }

void PropertiesPanel::refresh() {
    m_block=true;
    Shape* s=m_c->firstSelected();
    Fill f = s? s->fill : m_c->currentFill();
    Stroke st = s? s->stroke : m_c->currentStroke();
    m_titleLbl->setText(!s? "No selection" : (m_c->selection().size()>1? QString("%1 objects").arg(m_c->selection().size()) : (s->name.isEmpty()?"Object":s->name)));
    m_fillType->setCurrentIndex(int(f.type));
    m_fillColor->setColor(f.representativeColor());
    bool grad=(f.type==FillType::Linear||f.type==FillType::Radial||f.type==FillType::Conical||f.type==FillType::Square);
    m_gradBar->setVisible(grad);
    if(!f.stops.isEmpty()) m_gradBar->setStops(f.stops);
    m_gradOpts->setVisible(grad);
    m_fillSpread->setCurrentIndex(f.spread==QGradient::RepeatSpread?1:f.spread==QGradient::ReflectSpread?2:0);
    m_fillRainbow->setChecked(f.rainbow);
    m_meshRow->setVisible(f.type==FillType::Mesh);
    if(f.type==FillType::Mesh){ int nc=qMax(3,f.meshColors.size()); for(int k=0;k<4;++k){ m_meshCol[k]->setVisible(k<nc); if(k<f.meshColors.size()) m_meshCol[k]->setColor(f.meshColors[k]); } }
    m_bmpRow->setVisible(f.type==FillType::Bitmap);
    if(f.type==FillType::Bitmap){ m_bmpTile->setChecked(f.tiled); m_bmpContone->setChecked(f.contone); }
    // transparency section
    { const Transparency& tp = s? s->transp : Transparency();
      int gt = tp.type==0? 0 : (tp.geom==0?1:(tp.geom==1?2:(tp.geom==5?4:3)));
      m_transpType->setCurrentIndex(gt);
      double amt = (gt==1)? tp.t0 : tp.t1;
      m_transpAmt->setValue(int(amt*100+0.5)); m_transpAmtLbl->setText(QString::number(m_transpAmt->value())+"%");
      static const int kEff[]={1,2,3,4,6,7,8}; int ei=0; for(int k=0;k<7;++k) if(kEff[k]==tp.type){ ei=k; break; }
      m_transpEffect->setCurrentIndex(ei); }
    m_strokeOn->setChecked(st.enabled); m_strokeColor->setColor(st.color); m_strokeW->setValue(st.width);
    m_cap->setCurrentIndex(st.cap==Qt::FlatCap?0:(st.cap==Qt::SquareCap?1:2));
    m_join->setCurrentIndex(st.join==Qt::MiterJoin?0:(st.join==Qt::BevelJoin?1:2));
    m_opacity->setValue(s? int(s->opacity*100):100); m_opacityLbl->setText(QString::number(m_opacity->value())+"%");
    m_blend->setCurrentText(s? blendModeName(s->blend):"Normal");
    bool isRect = s && s->type==ShapeType::Rect; bool isPoly = s && s->type==ShapeType::Polygon;
    m_geomRow->setVisible(s && (isRect||isPoly));
    m_corner->setVisible(isRect); if(isRect) m_corner->setValue(s->cornerRadius);
    m_sides->setVisible(isPoly); m_star->setVisible(isPoly); if(isPoly){ m_sides->setValue(s->polySides); m_star->setChecked(s->polyStar); }
    m_shadowOn->setChecked(s&&s->shadow.on); m_shadowBlur->setValue(s?s->shadow.blur:12); m_shadowColor->setColor(s?s->shadow.color:QColor(0,0,0,120));
    if(m_shadowType) m_shadowType->setCurrentIndex(s?s->shadow.type:0);
    m_contourOn->setChecked(s&&s->contour.on); m_contourW->setValue(s?s->contour.width:8); m_contourSteps->setValue(s?s->contour.steps:1);
    m_bevelOn->setChecked(s&&s->bevel.on); m_bevelDepth->setValue(s?s->bevel.depth:7);
    m_block=false;
}

// ===== LayersPanel ==========================================================
LayersPanel::LayersPanel(Canvas* c, QWidget* parent):QWidget(parent),m_c(c) {
    setObjectName("layersHost");   // compact-density QSS (shared with propsHost/colourHost)
    auto* V=new QVBoxLayout(this); V->setContentsMargins(7,7,7,7); V->setSpacing(6);
    auto* t=new QLabel("Objects"); t->setObjectName("panelTitle"); V->addWidget(t);
    m_list=new QListWidget; m_list->setSelectionMode(QAbstractItemView::ExtendedSelection); V->addWidget(m_list,1);
    auto* row=new QHBoxLayout;
    auto mk=[&](const QString& ic, const QString& tip, std::function<void()> fn){ auto* b=new QToolButton; b->setIcon(actionIcon(ic)); b->setToolTip(tip); b->setAutoRaise(true); connect(b,&QToolButton::clicked,this,[fn]{fn();}); row->addWidget(b); };
    mk("undo","Lower",[c]{c->lowerSelection();}); mk("redo","Raise",[c]{c->raiseSelection();});
    mk("dup","Duplicate",[c]{c->duplicateSelection();}); mk("group","Group",[c]{c->groupSelection();});
    mk("delete","Delete",[c]{c->deleteSelection();}); row->addStretch(); V->addLayout(row);
    connect(m_list,&QListWidget::itemSelectionChanged,this,&LayersPanel::onRowChanged);
    connect(m_list,&QListWidget::itemChanged,this,[this](QListWidgetItem* it){ if(m_block)return; int idx=it->data(Qt::UserRole).toInt(); if(idx<0||idx>=m_c->doc().shapes.size())return; m_c->doc().shapes[idx].visible=(it->checkState()==Qt::Checked); m_c->update(); });
    connect(m_c,&Canvas::documentChangedDebounced,this,&LayersPanel::refresh);   // debounced: rebuilds a row per shape
    connect(m_c,&Canvas::selectionChanged,this,&LayersPanel::refresh);
    refresh();
}
void LayersPanel::refresh() {
    m_block=true; m_list->clear();
    const auto& sh=m_c->doc().shapes; auto sel=m_c->selection();
    // Building a swatch pixmap per object doesn't scale to thousands (e.g. a big imported .cdr):
    // cap the list to the top N objects and note the remainder, so import stays responsive.
    const int CAP=2000; const int lo=qMax(0, sh.size()-CAP);
    for(int i=sh.size()-1;i>=lo;--i){ const Shape&s=sh[i];
        QString nm=s.name.isEmpty()? QString("Object %1").arg(i+1):s.name;
        if(s.type==ShapeType::Text) nm=QString("“%1”").arg(s.text.left(16));
        auto* it=new QListWidgetItem(nm);
        it->setData(Qt::UserRole, i);   // real shape index (list order/cap-independent)
        it->setFlags(it->flags()|Qt::ItemIsUserCheckable);
        it->setCheckState(s.visible?Qt::Checked:Qt::Unchecked);
        QPixmap pm(18,18); pm.fill(Qt::transparent); QPainter g(&pm); g.setRenderHint(QPainter::Antialiasing);
        g.setPen(QPen(QColor(160,165,175),1)); g.setBrush(s.fill.representativeColor()); g.drawRoundedRect(2,3,14,12,3,3);
        it->setIcon(QIcon(pm));
        if(sel.contains(i)) it->setSelected(true);
        m_list->addItem(it);
    }
    if(lo>0){ auto* more=new QListWidgetItem(QString("… and %1 more objects").arg(lo));
        more->setData(Qt::UserRole,-1); more->setFlags(Qt::ItemIsEnabled); more->setForeground(QColor(140,146,158)); m_list->addItem(more); }
    m_block=false;
}
void LayersPanel::onRowChanged() {
    if(m_block)return; QVector<int> sel;
    for(auto* it:m_list->selectedItems()){ int idx=it->data(Qt::UserRole).toInt(); if(idx>=0) sel.push_back(idx);}
    std::sort(sel.begin(),sel.end()); m_c->select(sel);
}

// ===== ColorPanel ===========================================================
ColorPanel::ColorPanel(Canvas* c, QWidget* parent):QWidget(parent),m_c(c) {
    setObjectName("colourHost");   // compact-density QSS (shared with propsHost/layersHost)
    auto* V=new QVBoxLayout(this); V->setContentsMargins(7,7,7,7); V->setSpacing(6);
    auto* t=new QLabel("Swatches"); t->setObjectName("panelTitle"); V->addWidget(t);
    auto* grid=new QGridLayout; grid->setSpacing(4);
    const QColor pal[]={ QColor(244,67,54),QColor(233,30,99),QColor(156,39,176),QColor(103,58,183),QColor(63,81,181),QColor(33,150,243),
        QColor(3,169,244),QColor(0,188,212),QColor(0,150,136),QColor(76,175,80),QColor(139,195,74),QColor(205,220,57),
        QColor(255,235,59),QColor(255,193,7),QColor(255,152,0),QColor(255,87,34),QColor(121,85,72),QColor(96,125,139),
        QColor(0,0,0),QColor(70,70,76),QColor(140,140,148),QColor(200,200,206),QColor(245,245,248),QColor(255,255,255) };
    int n=sizeof(pal)/sizeof(pal[0]);
    for(int i=0;i<n;++i){ auto* b=new QToolButton; b->setFixedSize(26,26); b->setCursor(Qt::PointingHandCursor);
        QPixmap pm(20,20); pm.fill(pal[i]); b->setIcon(QIcon(pm)); b->setIconSize(QSize(20,20)); b->setToolTip(pal[i].name());
        connect(b,&QToolButton::clicked,this,[this,col=pal[i]]{ Fill f=m_c->currentFill(); f.type=FillType::Solid; f.color=col; m_c->setCurrentFill(f); });
        grid->addWidget(b,i/6,i%6); }
    V->addLayout(grid);
    auto* cust=new QPushButton("Custom colour…"); connect(cust,&QPushButton::clicked,this,[this]{ QColor c=ColorEditor::pick(this,m_c->currentFill().color,m_c); if(c.isValid()){ Fill f=m_c->currentFill(); f.type=FillType::Solid; f.color=c; m_c->setCurrentFill(f);} });
    V->addWidget(cust); V->addStretch();
}

// ===== InfoBar: per-tool options bar ========================================
InfoBar::InfoBar(Canvas* c, QWidget* parent):QWidget(parent),m_c(c){
    setObjectName("infobar");
    m_lay=new QHBoxLayout(this); m_lay->setContentsMargins(0,0,8,0); m_lay->setSpacing(6);   // no left/top/bottom margin: the tool tag sits flush
    setMinimumHeight(34);
    connect(c,&Canvas::toolChanged,this,[this](Tool){ rebuild(); });
    // The per-tool controls (Shadow/Contour/Bevel/… on-checkboxes, blur/width spins) mirror the selected
    // object's state, so they must re-sync when the canvas changes the model (e.g. casting a shadow by drag
    // toggles shadow.on). rebuild() only ran on a tool switch, leaving those controls stale. Debounce a
    // rebuild off document/selection changes so it fires once after a drag settles instead of every mouse-move.
    m_syncTimer=new QTimer(this); m_syncTimer->setSingleShot(true);
    connect(m_syncTimer,&QTimer::timeout,this,[this]{ if(m_c->tool()!=Tool::Select) rebuild(); });   // Select keeps its live refresh() fields
    connect(c,&Canvas::selectionChanged,this,[this]{ refresh(); m_syncTimer->start(60); });
    connect(c,&Canvas::documentChanged,this,[this]{ m_syncTimer->start(60); });
    rebuild();
}
void InfoBar::refresh(){
    if(!m_x) return;                         // only the Selector bar has live fields
    m_block=true; QRectF b=m_c->selectionBoundsDoc();
    m_x->setValue(b.left()); m_y->setValue(b.top()); m_w->setValue(b.width()); m_h->setValue(b.height());
    if(m_rot) m_rot->setValue(m_c->selectionRotation());
    m_block=false;
}
void InfoBar::rebuild(){
    QLayoutItem* it; while((it=m_lay->takeAt(0))!=nullptr){ if(it->widget()) it->widget()->deleteLater(); delete it; }
    m_x=m_y=m_w=m_h=m_rot=nullptr; m_lock=nullptr;
    Tool t=m_c->tool();
    auto label=[&](const QString& s){ auto* l=new QLabel(s); l->setObjectName("infoLbl"); m_lay->addWidget(l); return l; };
    auto sep=[&](){ auto* f=new QFrame; f->setObjectName("infoSep"); f->setFrameShape(QFrame::VLine); m_lay->addWidget(f); };
    auto dspin=[&](const QString& lbl,double mn,double mx,double val,const QString& suf,std::function<void(double)> fn){
        if(!lbl.isEmpty()) label(lbl);
        auto* s=new QDoubleSpinBox; s->setRange(mn,mx); s->setDecimals(1); s->setSuffix(suf); s->setValue(val); s->setFixedWidth(74); s->setKeyboardTracking(false);
        s->setButtonSymbols(QAbstractSpinBox::NoButtons); s->setAlignment(Qt::AlignLeft|Qt::AlignVCenter);   // DS NumberField: unit suffix, no spinner arrows (was clipping "px"→"p»")
        connect(s,QOverload<double>::of(&QDoubleSpinBox::valueChanged),this,[this,fn](double v){ if(!m_block) fn(v); });
        m_lay->addWidget(s); return s; };
    auto ispin=[&](const QString& lbl,int mn,int mx,int val,std::function<void(int)> fn){
        if(!lbl.isEmpty()) label(lbl);
        auto* s=new QSpinBox; s->setRange(mn,mx); s->setValue(val); s->setFixedWidth(56); s->setKeyboardTracking(false);
        connect(s,QOverload<int>::of(&QSpinBox::valueChanged),this,[this,fn](int v){ if(!m_block) fn(v); });
        m_lay->addWidget(s); return s; };
    auto combo=[&](const QStringList& items,int cur,std::function<void(int)> fn){
        auto* cb=new QComboBox; cb->addItems(items); cb->setCurrentIndex(cur);
        connect(cb,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this,fn](int v){ if(!m_block) fn(v); });
        m_lay->addWidget(cb); return cb; };
    auto check=[&](const QString& lbl,bool val,std::function<void(bool)> fn){
        auto* cb=new QCheckBox(lbl); cb->setChecked(val);
        connect(cb,&QCheckBox::toggled,this,[this,fn](bool v){ if(!m_block) fn(v); });
        m_lay->addWidget(cb); return cb; };
    auto btn=[&](const QString& txt,std::function<void()> fn){
        auto* b=new QToolButton; b->setText(txt); b->setAutoRaise(true);
        connect(b,&QToolButton::clicked,this,[fn]{ fn(); }); m_lay->addWidget(b); return b; };

    static const QHash<int,QString> names={{int(Tool::Select),"Selector"},{int(Tool::Node),"Shape Edit"},{int(Tool::Rect),"Rectangle"},
        {int(Tool::Ellipse),"Ellipse"},{int(Tool::Polygon),"QuickShape"},{int(Tool::Pen),"Pen"},{int(Tool::Line),"Line"},{int(Tool::Freehand),"Freehand"},
        {int(Tool::Text),"Text"},{int(Tool::Fill),"Fill"},{int(Tool::Transparency),"Transparency"},{int(Tool::Shadow),"Shadow"},{int(Tool::Contour),"Contour"},
        {int(Tool::Blend),"Blend"},{int(Tool::Mould),"Mould"},{int(Tool::Zoom),"Zoom"},{int(Tool::Pan),"Push"},
        {int(Tool::Bevel),"Bevel"},{int(Tool::Feather),"Feather"},{int(Tool::Eraser),"Eraser"},{int(Tool::Extrude),"3D Extrude"},{int(Tool::Clone),"Clone"}};
    auto* chip=new QLabel(names.value(int(t))); chip->setObjectName("infoTool"); m_lay->addWidget(chip); sep();

    Shape* sel=m_c->firstSelected();
    switch(t){
    case Tool::Select: {
        QRectF b=m_c->selectionBoundsDoc();
        m_x=dspin("X",-99999,99999,b.left()," px",[this](double v){ QRectF bb=m_c->selectionBoundsDoc(); m_c->setSelectionPos(v,bb.top()); });
        m_y=dspin("Y",-99999,99999,b.top()," px",[this](double v){ QRectF bb=m_c->selectionBoundsDoc(); m_c->setSelectionPos(bb.left(),v); }); sep();
        m_w=dspin("W",0,99999,b.width()," px",[this](double v){ QRectF bb=m_c->selectionBoundsDoc(); m_c->setSelectionSize(v,bb.height(), m_lock&&m_lock->isChecked()); });
        m_h=dspin("H",0,99999,b.height()," px",[this](double v){ QRectF bb=m_c->selectionBoundsDoc(); m_c->setSelectionSize(bb.width(),v, m_lock&&m_lock->isChecked()); });
        m_lock=check("Lock",false,[](bool){}); sep();
        m_rot=dspin("Angle",-360,360,m_c->selectionRotation(),"°",[this](double v){ m_c->rotateSelection(v-m_c->selectionRotation()); });
        // Skew: absolute-from-0 per selection (each spinner tracks its own last value and applies the delta).
        { auto lastH=std::make_shared<double>(0.0), lastV=std::make_shared<double>(0.0);
          dspin("Skew X",-85,85,0,"°",[this,lastH](double v){ double dv=v-*lastH; *lastH=v; m_c->skewSelection(dv,0); });
          dspin("Skew Y",-85,85,0,"°",[this,lastV](double v){ double dv=v-*lastV; *lastV=v; m_c->skewSelection(0,dv); }); }
        if(sel && sel->type==ShapeType::Smart && m_c->selection().size()==1){ sep();
            static const char* pname[]={"Chevrons","Teeth","Teeth","Tails","Arrow","Points","Callout"};
            const int sk=qBound(0,sel->smartKind,6);
            ispin(pname[sk],2,64,int(sel->smartParam+0.5),[this](int v){ m_c->setSmartParam(v); });
            static const char* p2name[]={"Notch","Inner","Inner","Tail","Shaft","Inner","Tail"};
            const double cur2 = sel->smartParam2>=0 ? sel->smartParam2 : Shape::smartParam2Default(sk);
            dspin(p2name[sk],0.0,1.0,cur2,"",[this](double v){ m_c->setSmartParam2(v); }); }
        break; }
    case Tool::Region: {
        check("Freehand",m_c->regionFreehand(),[this](bool v){ m_c->setRegionFreehand(v); if(v)m_c->setRegionMagnetic(false); rebuild(); });
        check("Magnetic",m_c->regionMagnetic(),[this](bool v){ m_c->setRegionMagnetic(v); if(v)m_c->setRegionFreehand(false); rebuild(); });
        check("Ellipse",m_c->regionEllipse(),[this](bool v){ m_c->setRegionEllipse(v); });
        combo({"Clip","Effect: Brighten","Effect: Darken","Effect: Blur","Effect: Sharpen","Effect: Grey"},m_c->regionEffect(),[this](int i){ m_c->setRegionEffect(i); rebuild(); });
        dspin("Soft edge",0,60,(sel?sel->regionFeather:0)," px",[this](double v){ m_c->applyToSelection([v](Shape&s){ if(!s.regionPath.isEmpty()) s.regionFeather=v; },"Region soft edge"); });
        btn("Clear region",[this]{ auto s=m_c->selection(); if(!s.isEmpty()) m_c->clearShapeRegion(s.first()); });
        { auto* l=new QLabel(m_c->regionMagnetic()? "Magnetic lasso: click edge points, double-click to close"
                                                  : "Drag over a photo to cut it to a region"); l->setObjectName("infoLbl"); m_lay->addWidget(l); }
        break; }
    case Tool::Node: {
        auto* l=new QLabel("Selected node:"); l->setObjectName("infoLbl"); m_lay->addWidget(l);
        btn("Make Curve",[this]{ m_c->setActiveNodeCurved(true); });
        btn("Make Line",[this]{ m_c->setActiveNodeCurved(false); });
        btn("Cusp",[this]{ m_c->setSelectedNodeType(0); });
        btn("Smooth",[this]{ m_c->setSelectedNodeType(1); });
        btn("Symmetric",[this]{ m_c->setSelectedNodeType(2); });
        btn("Delete Node",[this]{ m_c->deleteActiveNode(); });
        btn("Break",[this]{ m_c->breakSelectedNodes(); });
        btn("Join",[this]{ m_c->joinSelectedNodes(); });
        btn("Reverse",[this]{ m_c->reverseNodePath(); });
        break; }
    case Tool::Rect:
        dspin("Rounding",0,300,(sel&&sel->type==ShapeType::Rect)?sel->cornerRadius:0," px",[this](double v){ m_c->applyToSelection([v](Shape&s){ if(s.type==ShapeType::Rect)s.cornerRadius=v; },"Rounding"); });
        break;
    case Tool::Polygon:
        ispin("Sides",3,50,m_c->polySides(),[this](int v){ m_c->setPolySides(v); m_c->applyToSelection([v](Shape&s){ if(s.type==ShapeType::Polygon)s.polySides=v; },"Sides"); });
        check("Star",m_c->polyStar(),[this](bool v){ m_c->setPolyStar(v); m_c->applyToSelection([v](Shape&s){ if(s.type==ShapeType::Polygon)s.polyStar=v; },"Star"); });
        dspin("Depth",0.05,0.95,(sel&&sel->type==ShapeType::Polygon)?sel->polyStarDepth:0.5,"",[this](double v){ m_c->applyToSelection([v](Shape&s){ if(s.type==ShapeType::Polygon)s.polyStarDepth=v; },"Star depth"); });
        dspin("Curve",-2.0,2.0,(sel&&sel->type==ShapeType::Polygon)?sel->polyCurvature:0.0,"",[this](double v){ m_c->applyToSelection([v](Shape&s){ if(s.type==ShapeType::Polygon)s.polyCurvature=v; },"Curvature"); });
        { auto* h=new QLabel("Drag the purple dot to bow the edges; the yellow diamond sets a star's point depth."); h->setObjectName("infoHint"); m_lay->addWidget(h); }
        break;
    case Tool::Ellipse: {
        int em=(sel&&sel->type==ShapeType::Ellipse)?qBound(0,sel->ellipseMode,3):0;
        combo({"Whole","Pie","Arc","Segment"},em,[this](int i){ m_c->applyToSelection([i](Shape&s){ if(s.type==ShapeType::Ellipse){ s.ellipseMode=i; if(i!=0 && s.ellipseSweep>=360.0) s.ellipseSweep=270.0; } },"Ellipse mode"); rebuild(); });
        dspin("Start",0,360,(sel&&sel->type==ShapeType::Ellipse)?sel->ellipseStart:0,"°",[this](double v){ m_c->applyToSelection([v](Shape&s){ if(s.type==ShapeType::Ellipse)s.ellipseStart=v; },"Ellipse start"); });
        dspin("Sweep",1,360,(sel&&sel->type==ShapeType::Ellipse)?sel->ellipseSweep:360,"°",[this](double v){ m_c->applyToSelection([v](Shape&s){ if(s.type==ShapeType::Ellipse){ s.ellipseSweep=v; if(v<360 && s.ellipseMode==0) s.ellipseMode=1; } },"Ellipse sweep"); });
        auto* h=new QLabel("Drag to draw an ellipse; choose Pie / Arc / Segment to open it, then drag the rim handles."); h->setObjectName("infoHint"); m_lay->addWidget(h);
        break; }
    case Tool::Table: {
        ispin("Rows",1,50,m_c->tableRows(),[this](int v){ m_c->setTableRows(v); });
        ispin("Cols",1,50,m_c->tableCols(),[this](int v){ m_c->setTableCols(v); });
        auto* sep=new QLabel("│"); sep->setObjectName("infoLbl"); m_lay->addWidget(sep);
        // These act on the currently cell-selected table (no-ops if none).
        btn("+Row",[this]{ QRect s=m_c->tableSelRange(); m_c->tableInsertRow(s.isNull()?9999:s.bottom()+1); });
        btn("−Row",[this]{ QRect s=m_c->tableSelRange(); if(!s.isNull()) m_c->tableDeleteRow(s.top()); });
        btn("+Col",[this]{ QRect s=m_c->tableSelRange(); m_c->tableInsertCol(s.isNull()?9999:s.right()+1); });
        btn("−Col",[this]{ QRect s=m_c->tableSelRange(); if(!s.isNull()) m_c->tableDeleteCol(s.left()); });
        btn("Merge",[this]{ m_c->tableMergeCells(); });
        btn("Split",[this]{ m_c->tableSplitCells(); });
        btn("≡ Rows",[this]{ m_c->tableDistribute(true); });    // distribute row heights evenly
        btn("⦀ Cols",[this]{ m_c->tableDistribute(false); });   // distribute column widths evenly
        btn("◧",[this]{ m_c->tableSetCellAlign(0); }); btn("▣",[this]{ m_c->tableSetCellAlign(1); }); btn("◨",[this]{ m_c->tableSetCellAlign(2); });
        btn("⊤",[this]{ m_c->tableSetCellVAlign(1); }); btn("⊣",[this]{ m_c->tableSetCellVAlign(0); }); btn("⊥",[this]{ m_c->tableSetCellVAlign(2); });
        btn("B",[this]{ m_c->tableSetCellBold(true); }); btn("¬B",[this]{ m_c->tableSetCellBold(false); });
        { auto* hdr=new QCheckBox("Header"); hdr->setChecked(sel&&sel->type==ShapeType::Table&&sel->table.headerRow); m_lay->addWidget(hdr);
          connect(hdr,&QCheckBox::toggled,this,[this](bool on){ m_c->tableSetHeaderRow(on); }); }
        btn("Cell fill",[this]{ m_c->tableSetCellBg(m_c->currentFill().color); });
        btn("No fill",[this]{ m_c->tableSetCellBg(QColor(),true); });
        // Table-wide appearance (reads the selected table's live values; applies to the cell-selected table).
        { const Table* tt=(sel && sel->type==ShapeType::Table)?&sel->table:nullptr;
          dspin("Border",0,20, tt?tt->borderW:1.0," px",[this](double v){ int i=m_c->tableEditShape(); if(i>=0){ const Table& t=m_c->doc().shapes[i].table; m_c->tableSetAppearance(v,QColor(),t.pad,t.spacing);} });
          dspin("Pad",0,40, tt?tt->pad:5.0," px",[this](double v){ int i=m_c->tableEditShape(); if(i>=0){ const Table& t=m_c->doc().shapes[i].table; m_c->tableSetAppearance(t.borderW,QColor(),v,t.spacing);} });
          dspin("Gap",0,40, tt?tt->spacing:0.0," px",[this](double v){ int i=m_c->tableEditShape(); if(i>=0){ const Table& t=m_c->doc().shapes[i].table; m_c->tableSetAppearance(t.borderW,QColor(),t.pad,v);} });
          btn("Grid col",[this]{ int i=m_c->tableEditShape(); if(i>=0){ const Table& t=m_c->doc().shapes[i].table; m_c->tableSetAppearance(t.borderW,m_c->currentStroke().color,t.pad,t.spacing);} });
          btn("Tbl bg",[this]{ m_c->tableSetBg(m_c->currentFill().color,false); });
          btn("No bg",[this]{ m_c->tableSetBg(QColor(),true); }); }
        break; }
    case Tool::Text: {
        // ---- Inline essentials: font family, size, B/I/U/S, alignment, frame; everything else in "More ▾" ----
        // Attributes fall back to the Text tool's own font (m_c->textFont()) so they also seed NEW text.
        QFont f=(sel&&sel->type==ShapeType::Text)?sel->font:m_c->textFont();
        auto* fc=new QFontComboBox; fc->setCurrentFont(f); fc->setFixedWidth(190);
        // Weight / style selector: lists the faces THIS family actually ships (Light/Regular/Medium/Semibold/
        // Bold/Black…), so a family like 'Arson Pro' can be set to Heavy/Black (900), not just toggled Bold (700)
        // — the same weight choice Inkscape/Xara expose. Repopulated whenever the family changes.
        auto* wc=new QComboBox; wc->setFixedWidth(120); wc->setToolTip("Font weight / style");
        auto stylesFor=[](const QString& fam)->QStringList{ QStringList st=QFontDatabase::styles(fam); if(st.isEmpty()) st<<QStringLiteral("Regular"); return st; };
        auto matchStyle=[](const QStringList& st,const QString& fam,int wt,bool it)->int{   // index of the face closest to (weight,italic)
            int best=0, bestScore=INT_MAX;   // not 1L<<60: long is 32-bit on Windows and 32-bit Haiku
            for(int i=0;i<st.size();++i){ QFont pf=QFontDatabase::font(fam,st[i],12);
                int sc=qAbs(int(pf.weight())-wt)+(pf.italic()!=it?100000:0); if(sc<bestScore){ bestScore=sc; best=i; } }
            return best; };
        auto fillWeights=[wc,stylesFor,matchStyle](const QFont& font){ QSignalBlocker b(wc); wc->clear();
            const QStringList st=stylesFor(font.family()); wc->addItems(st);
            wc->setCurrentIndex(matchStyle(st,font.family(),font.weight(),font.italic())); };
        fillWeights(f);
        // Each control formats the ACTIVE on-canvas text selection if there is one (Inkscape-style, via
        // formatEditSelection), else the whole selected object(s). The trailing setTextFont keeps the pending
        // "type next" style in sync.
        connect(fc,&QFontComboBox::currentFontChanged,this,[this,fillWeights](const QFont& nf){
            if(!m_c->formatEditSelection([nf](QFont&g,QColor&){ g.setFamily(nf.family()); },"Font"))
                m_c->applyToSelection([nf](Shape&s){ if(s.type==ShapeType::Text){ QFont g=s.font; g.setFamily(nf.family()); s.font=g; } },"Font");
            QFont tf=m_c->textFont(); tf.setFamily(nf.family()); m_c->setTextFont(tf);
            fillWeights(tf); });   // new family → refresh the weight list (keeping the current weight/slant)
        connect(wc,QOverload<int>::of(&QComboBox::activated),this,[this,fc,wc](int){
            const QFont probe=QFontDatabase::font(fc->currentFont().family(), wc->currentText(), 12);
            const int wt=probe.weight(); const bool it=probe.italic();
            if(!m_c->formatEditSelection([wt,it](QFont&g,QColor&){ g.setWeight(QFont::Weight(wt)); g.setItalic(it); },"Font weight"))
                m_c->applyToSelection([wt,it](Shape&s){ if(s.type==ShapeType::Text){ QFont g=s.font; g.setWeight(QFont::Weight(wt)); g.setItalic(it); s.font=g; } },"Font weight");
            QFont tf=m_c->textFont(); tf.setWeight(QFont::Weight(wt)); tf.setItalic(it); m_c->setTextFont(tf); });
        m_lay->addWidget(fc); m_lay->addWidget(wc);
        ispin("Size",4,400,f.pointSize()>0?f.pointSize():36,[this](int v){
                       if(!m_c->formatEditSelection([v](QFont&g,QColor&){ g.setPointSize(v); },"Size"))
                           m_c->applyToSelection([v](Shape&s){ if(s.type==ShapeType::Text){ QFont g=s.font; g.setPointSize(v); s.font=g; } },"Size");
                       QFont tf=m_c->textFont(); tf.setPointSize(v); m_c->setTextFont(tf); });
        btn("B",[this]{ const bool nb=!m_c->editSelectionFont().bold();
                        if(!m_c->formatEditSelection([nb](QFont&g,QColor&){ g.setBold(nb); },"Bold"))
                            m_c->applyToSelection([](Shape&s){ if(s.type==ShapeType::Text){ QFont g=s.font; g.setBold(!g.bold()); s.font=g; } },"Bold");
                        QFont tf=m_c->textFont(); tf.setBold(!tf.bold()); m_c->setTextFont(tf); });
        btn("I",[this]{ const bool ni=!m_c->editSelectionFont().italic();
                        if(!m_c->formatEditSelection([ni](QFont&g,QColor&){ g.setItalic(ni); },"Italic"))
                            m_c->applyToSelection([](Shape&s){ if(s.type==ShapeType::Text){ QFont g=s.font; g.setItalic(!g.italic()); s.font=g; } },"Italic");
                        QFont tf=m_c->textFont(); tf.setItalic(!tf.italic()); m_c->setTextFont(tf); });
        btn("U",[this]{ const bool nu=!m_c->editSelectionFont().underline();
                        if(!m_c->formatEditSelection([nu](QFont&g,QColor&){ g.setUnderline(nu); },"Underline"))
                            m_c->applyToSelection([](Shape&s){ if(s.type==ShapeType::Text){ QFont g=s.font; g.setUnderline(!g.underline()); s.font=g; } },"Underline");
                        QFont tf=m_c->textFont(); tf.setUnderline(!tf.underline()); m_c->setTextFont(tf); });
        btn("S̶",[this]{ const bool ns=!m_c->editSelectionFont().strikeOut();
                        if(!m_c->formatEditSelection([ns](QFont&g,QColor&){ g.setStrikeOut(ns); },"Strikethrough"))
                            m_c->applyToSelection([](Shape&s){ if(s.type==ShapeType::Text){ QFont g=s.font; g.setStrikeOut(!g.strikeOut()); s.font=g; } },"Strikethrough");
                        QFont tf=m_c->textFont(); tf.setStrikeOut(!tf.strikeOut()); m_c->setTextFont(tf); }); sep();
        combo({"⯇ Left","≡ Centre","⯈ Right","≣ Justify"},sel?sel->textAlign:0,[this](int v){ m_c->applyToSelection([v](Shape&s){ if(s.type==ShapeType::Text) s.textAlign=v; },"Text align"); });
        check("Frame",sel&&sel->textFrame,[this](bool v){ m_c->applyToSelection([v](Shape&s){ if(s.type==ShapeType::Text){ s.textFrame=v; if(v&&s.rect.width()<20) s.rect.setWidth(320); if(v&&s.rect.height()<20) s.rect.setHeight(200); } },"Text frame"); }); sep();
        // ---- "More ▾" popup: list / paragraph / advanced-typography / web controls (moved off the bar) ----
        auto* more=new QToolButton; more->setText("More ▾"); more->setAutoRaise(true); more->setPopupMode(QToolButton::InstantPopup);
        auto* menu=new QMenu(more);
        auto* pop=new QWidget; auto* pf=new QFormLayout(pop); pf->setContentsMargins(10,8,10,8); pf->setSpacing(6);
        // Style presets
        { auto* cb=new QComboBox; cb->addItems({"Style…","Heading 1","Heading 2","Body","Caption"});
          connect(cb,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this](int i){ if(i<=0)return; struct St{const char* fam;int sz;bool b;}; static const St st[]={{"DejaVu Sans",40,true},{"DejaVu Sans",28,true},{"DejaVu Sans",16,false},{"DejaVu Sans",12,false}};
              const St& z=st[qBound(0,i-1,3)]; m_c->applyToSelection([z](Shape&s){ if(s.type==ShapeType::Text){ QFont g=s.font; g.setFamily(z.fam); g.setPointSize(z.sz); g.setBold(z.b); s.font=g; } },"Text style"); });
          pf->addRow("Style", cb); }
        // Bullets / numbering — prepend a marker to each line of the text block
        { auto* row=new QWidget; auto* rl=new QHBoxLayout(row); rl->setContentsMargins(0,0,0,0); rl->setSpacing(4);
          auto* bu=new QToolButton; bu->setText("•"); bu->setAutoRaise(true);
          connect(bu,&QToolButton::clicked,this,[this]{ m_c->applyToSelection([](Shape&s){ if(s.type!=ShapeType::Text)return; QStringList ls=s.text.split('\n'); for(auto&l:ls){ l.remove(QRegularExpression("^\\s*([\\u2022]|\\d+\\.)\\s*")); if(!l.trimmed().isEmpty()) l="• "+l; } s.text=ls.join('\n'); },"Bullets"); });
          auto* nu=new QToolButton; nu->setText("1."); nu->setAutoRaise(true);
          connect(nu,&QToolButton::clicked,this,[this]{ m_c->applyToSelection([](Shape&s){ if(s.type!=ShapeType::Text)return; QStringList ls=s.text.split('\n'); int n=1; for(auto&l:ls){ l.remove(QRegularExpression("^\\s*([\\u2022]|\\d+\\.)\\s*")); if(!l.trimmed().isEmpty()) l=QString::number(n++)+". "+l; } s.text=ls.join('\n'); },"Numbered list"); });
          rl->addWidget(bu); rl->addWidget(nu); rl->addStretch(); pf->addRow("List", row); }
        // Columns
        { auto* sp=new QSpinBox; sp->setRange(1,6); sp->setValue(sel?sel->textCols:1); sp->setKeyboardTracking(false);
          connect(sp,QOverload<int>::of(&QSpinBox::valueChanged),this,[this](int v){ m_c->applyToSelection([v](Shape&s){ if(s.type==ShapeType::Text){ s.textFrame=true; s.textCols=v; if(s.rect.width()<20) s.rect.setWidth(320); } },"Columns"); });
          pf->addRow("Columns", sp); }
        // Hyphenate / Small caps / Drop cap
        { auto* ck=new QCheckBox("Hyphenate"); ck->setChecked(sel&&sel->hyphenate);
          connect(ck,&QCheckBox::toggled,this,[this](bool v){ m_c->applyToSelection([v](Shape&s){ if(s.type==ShapeType::Text){ s.hyphenate=v; if(v){ s.textFrame=true; if(s.rect.width()<20) s.rect.setWidth(320); } } },"Hyphenate"); });
          pf->addRow(ck); }
        { auto* ck=new QCheckBox("Small caps"); ck->setChecked(sel&&sel->smallCaps);
          connect(ck,&QCheckBox::toggled,this,[this](bool v){ m_c->applyToSelection([v](Shape&s){ if(s.type==ShapeType::Text) s.smallCaps=v; },"Small caps"); });
          pf->addRow(ck); }
        { auto* sp=new QSpinBox; sp->setRange(0,8); sp->setValue(sel?sel->dropCap:0); sp->setKeyboardTracking(false);
          connect(sp,QOverload<int>::of(&QSpinBox::valueChanged),this,[this](int v){ m_c->applyToSelection([v](Shape&s){ if(s.type==ShapeType::Text){ s.dropCap=v; if(v>1){ s.textFrame=true; if(s.rect.width()<20) s.rect.setWidth(320); if(s.rect.height()<20) s.rect.setHeight(200); } } },"Drop cap"); });
          pf->addRow("Drop cap", sp); }
        // Leading / tracking / aspect
        { auto* sp=new QDoubleSpinBox; sp->setRange(0.5,3.0); sp->setDecimals(1); sp->setSuffix("×"); sp->setValue(sel?sel->lineSpacing:1.18); sp->setKeyboardTracking(false);
          connect(sp,QOverload<double>::of(&QDoubleSpinBox::valueChanged),this,[this](double v){ Shape* s=m_c->firstSelected(); m_c->setTextSpacing(v, s?s->tracking:0.0); });
          pf->addRow("Leading", sp); }
        { auto* sp=new QDoubleSpinBox; sp->setRange(-10,80); sp->setDecimals(1); sp->setSuffix(" px"); sp->setValue(sel?sel->tracking:0.0); sp->setKeyboardTracking(false);
          connect(sp,QOverload<double>::of(&QDoubleSpinBox::valueChanged),this,[this](double v){ Shape* s=m_c->firstSelected(); m_c->setTextSpacing(s?s->lineSpacing:1.18, v); });
          pf->addRow("Track", sp); }
        { auto* sp=new QDoubleSpinBox; sp->setRange(20,400); sp->setDecimals(1); sp->setSuffix("%"); sp->setValue(sel?sel->textAspect*100.0:100.0); sp->setKeyboardTracking(false);   // horizontal glyph scale
          connect(sp,QOverload<double>::of(&QDoubleSpinBox::valueChanged),this,[this](double v){ m_c->applyToSelection([v](Shape&s){ if(s.type==ShapeType::Text) s.textAspect=v/100.0; },"Text aspect"); });
          pf->addRow("Aspect", sp); }
        // Paragraph spacing
        { auto* sp=new QDoubleSpinBox; sp->setRange(0,300); sp->setDecimals(1); sp->setSuffix(" px"); sp->setValue(sel?sel->spaceBefore:0); sp->setKeyboardTracking(false);
          connect(sp,QOverload<double>::of(&QDoubleSpinBox::valueChanged),this,[this](double v){ Shape* s=m_c->firstSelected(); m_c->setSelectionParaSpacing(v, s?s->spaceAfter:0.0); });
          pf->addRow("¶ Before", sp); }
        { auto* sp=new QDoubleSpinBox; sp->setRange(0,300); sp->setDecimals(1); sp->setSuffix(" px"); sp->setValue(sel?sel->spaceAfter:0); sp->setKeyboardTracking(false);
          connect(sp,QOverload<double>::of(&QDoubleSpinBox::valueChanged),this,[this](double v){ Shape* s=m_c->firstSelected(); m_c->setSelectionParaSpacing(s?s->spaceBefore:0.0, v); });
          pf->addRow("¶ After", sp); }
        // Paragraph indents (first-line / left margin / right margin, doc px)
        { auto* sp=new QDoubleSpinBox; sp->setRange(-400,400); sp->setDecimals(1); sp->setSuffix(" px"); sp->setValue(sel?sel->firstLineIndent:0); sp->setKeyboardTracking(false);
          connect(sp,QOverload<double>::of(&QDoubleSpinBox::valueChanged),this,[this](double v){ Shape* s=m_c->firstSelected(); m_c->setTextIndents(v, s?s->leftIndent:0, s?s->rightIndent:0); });
          pf->addRow("Indent", sp); }
        { auto* sp=new QDoubleSpinBox; sp->setRange(0,400); sp->setDecimals(1); sp->setSuffix(" px"); sp->setValue(sel?sel->leftIndent:0); sp->setKeyboardTracking(false);
          connect(sp,QOverload<double>::of(&QDoubleSpinBox::valueChanged),this,[this](double v){ Shape* s=m_c->firstSelected(); m_c->setTextIndents(s?s->firstLineIndent:0, v, s?s->rightIndent:0); });
          pf->addRow("L-margin", sp); }
        { auto* sp=new QDoubleSpinBox; sp->setRange(0,400); sp->setDecimals(1); sp->setSuffix(" px"); sp->setValue(sel?sel->rightIndent:0); sp->setKeyboardTracking(false);
          connect(sp,QOverload<double>::of(&QDoubleSpinBox::valueChanged),this,[this](double v){ Shape* s=m_c->firstSelected(); m_c->setTextIndents(s?s->firstLineIndent:0, s?s->leftIndent:0, v); });
          pf->addRow("R-margin", sp); }
        { auto* sp=new QDoubleSpinBox; sp->setRange(0,400); sp->setDecimals(1); sp->setSuffix(" px"); sp->setValue(sel?sel->tabWidth:0); sp->setKeyboardTracking(false);
          connect(sp,QOverload<double>::of(&QDoubleSpinBox::valueChanged),this,[this](double v){ m_c->setTextTabWidth(v); });
          pf->addRow("Tab", sp); }
        // Web / responsive
        { auto* sp=new QSpinBox; sp->setRange(0,1600); sp->setValue(sel?sel->webHideBelow:0); sp->setKeyboardTracking(false);   // hide below this viewport px (0=show)
          connect(sp,QOverload<int>::of(&QSpinBox::valueChanged),this,[this](int v){ m_c->setSelectionHideBelow(v); });
          pf->addRow("Hide below", sp); }
        { auto* cb=new QComboBox; cb->addItems({"SEO: Normal","SEO: H1","SEO: H2","SEO: H3"}); cb->setCurrentIndex(sel?qBound(0,sel->webHeading,3):0);   // publish as a semantic heading
          connect(cb,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this](int i){ m_c->applyToSelection([i](Shape&s){ if(s.type==ShapeType::Text) s.webHeading=i; },"SEO heading"); });
          pf->addRow("SEO", cb); }
        auto* wa=new QWidgetAction(menu); wa->setDefaultWidget(pop); menu->addAction(wa);
        more->setMenu(menu); m_lay->addWidget(more);
        break; }
    case Tool::Fill:
        combo({"None","Flat","Linear","Circular","Conical","Bitmap","3/4-Colour","Diamond"},int(m_c->currentFill().type),
            [this](int v){ Fill f=m_c->currentFill(); f.type=FillType(v); m_c->setCurrentFill(f); m_c->applyToSelection([v](Shape&s){ s.fill.type=FillType(v); },"Fill type"); });
        sep();
        { auto* hb=new QToolButton; hb->setText("Handle ▮"); hb->setAutoRaise(true); hb->setToolTip("Colour of the on-canvas gradient editing handles");
          connect(hb,&QToolButton::clicked,this,[this]{ QColor c=ColorEditor::pick(this, m_c->gradHandleColor(), m_c, false); if(c.isValid()) m_c->setGradHandleColor(c); });
          m_lay->addWidget(hb); }
        break;
    case Tool::Transparency: {
        combo({"Graduated","Paint Mask"}, m_c->transpMode(), [this](int i){ m_c->setTranspMode(i); rebuild(); }); sep();
        if(m_c->transpMode()==1){
            dspin("Brush",2,300,m_c->maskNib()," px",[this](double v){ m_c->setMaskNib(v); });
            combo({"Erase (hide)","Restore (show)"}, m_c->maskErase()?0:1, [this](int i){ m_c->setMaskErase(i==0); });
            auto* h=new QLabel("Paint over a selected object to fade it out (Restore brings it back)."); h->setObjectName("infoHint"); m_lay->addWidget(h);
        } else {
            label("Opacity"); auto* sl=new QSlider(Qt::Horizontal); sl->setRange(0,100); sl->setValue(int((sel?sel->opacity:1.0)*100)); sl->setFixedWidth(160);
            connect(sl,&QSlider::valueChanged,this,[this](int v){ m_c->applyToSelection([v](Shape&s){ s.opacity=v/100.0; },"Opacity"); });
            m_lay->addWidget(sl);
            // Transparency TYPE (blend mode) — the renderer already supports the whole set; expose it here.
            sep(); { static const int mid[]={1,2,3,10,11,13,19,22,25,16,28,31};
                QStringList mn={"Mix","Bleach","Stained Glass","Additive","Subtractive","Contrast","Darken","Lighten","Brightness","Saturation","Luminosity","Hue"};
                int cur=0; if(sel) for(int k=0;k<mn.size();++k) if(sel->transp.type==mid[k]) cur=k;
                label("Mode"); combo(mn, cur, [this](int i){ int ty=mid[qBound(0,i,11)]; m_c->applyToSelection([ty](Shape&s){ s.transp.type=ty; },"Transparency mode"); }); }
            sep(); label("Profile");   // non-linear fade (Schlick bias/gain) + snap the fade axis onto the fill's gradient
            dspin("Bias",-1.0,1.0,(sel?sel->transp.profileBias:0.0),"",[this](double v){ double g=m_c->firstSelected()?m_c->firstSelected()->transp.profileGain:0.0; m_c->setSelectionTranspProfile(v,g); });
            dspin("Gain",-1.0,1.0,(sel?sel->transp.profileGain:0.0),"",[this](double v){ double b=m_c->firstSelected()?m_c->firstSelected()->transp.profileBias:0.0; m_c->setSelectionTranspProfile(b,v); });
            btn("Match Fill",[this]{ m_c->matchTranspToFillAxes(); });
            sep();
            { auto* hb=new QToolButton; hb->setText("Handle ▮"); hb->setAutoRaise(true); hb->setToolTip("Colour of the on-canvas gradient editing handles");
              connect(hb,&QToolButton::clicked,this,[this]{ QColor c=ColorEditor::pick(this, m_c->gradHandleColor(), m_c, false); if(c.isValid()) m_c->setGradHandleColor(c); });
              m_lay->addWidget(hb); }
        }
        break; }
    case Tool::Pen: case Tool::Line: case Tool::Freehand: {
        dspin("Width",0.1,200,m_c->currentStroke().width," px",[this](double v){ Stroke s=m_c->currentStroke(); s.width=v; m_c->setCurrentStroke(s); m_c->applyToSelection([v](Shape&sh){ sh.stroke.enabled=true; sh.stroke.width=v; },"Line width"); });
        // Line transparency (model + renderer already support it; only reachable via XAR before).
        dspin("Transp",0,100,m_c->currentStroke().transp*100," %",[this](double v){ Stroke s=m_c->currentStroke(); s.transp=v/100.0; m_c->setCurrentStroke(s); m_c->applyToSelection([v](Shape&sh){ sh.stroke.transp=v/100.0; },"Line transparency"); });
        // Arrowheads: stock shapes at the start / end of open paths. 0=none; ids map to Kernel stock arrows.
        static const int arrowIds[]={0,-2,-3,-4,-5,-6,-7,-9};
        static const QStringList arrowNames={"None","Straight","Angled","Rounded","Spot","Diamond","Feather","Hollow"};
        auto idToIx=[&](int id){ for(int i=0;i<8;++i) if(arrowIds[i]==id) return i; return 0; };
        const Stroke cs=m_c->currentStroke();
        label("Start"); combo(arrowNames, idToIx(cs.startArrow), [this](int ix){ const int id=arrowIds[qBound(0,ix,7)];
            Stroke s=m_c->currentStroke(); s.startArrow=id; m_c->setCurrentStroke(s); m_c->applyToSelection([id](Shape&sh){ sh.stroke.startArrow=id; },"Start arrow"); });
        label("End"); combo(arrowNames, idToIx(cs.endArrow), [this](int ix){ const int id=arrowIds[qBound(0,ix,7)];
            Stroke s=m_c->currentStroke(); s.endArrow=id; m_c->setCurrentStroke(s); m_c->applyToSelection([id](Shape&sh){ sh.stroke.endArrow=id; },"End arrow"); });
        // Arrowhead SIZE, independent of line width (scales both heads uniformly).
        dspin("Arrow⬍",0.2,8.0, cs.endArrowW>0?cs.endArrowW:1.0, "×", [this](double v){ Stroke s=m_c->currentStroke();
            s.startArrowW=s.startArrowH=s.endArrowW=s.endArrowH=v; m_c->setCurrentStroke(s);
            m_c->applyToSelection([v](Shape&sh){ sh.stroke.startArrowW=sh.stroke.startArrowH=sh.stroke.endArrowW=sh.stroke.endArrowH=v; },"Arrow size"); });
        // Pressure / variable width: a symmetric taper profile (thin→full→thin) applied to the selected stroke.
        check("Taper", sel?sel->stroke.varWidth:false, [this](bool on){ m_c->applyToSelection([on](Shape&sh){ sh.stroke.varWidth=on;
            if(on && sh.stroke.widthProfile.isEmpty()) sh.stroke.widthProfile={ {0.0,0.15},{0.5,1.0},{1.0,0.15} }; },"Variable width"); });
        check("Callig", sel?sel->stroke.calligraphic:false, [this](bool on){ m_c->applyToSelection([on](Shape&sh){ sh.stroke.calligraphic=on; },"Calligraphic"); });
        dspin("Nib°",-90,90,sel?sel->stroke.calligAngle:30,"°",[this](double v){ m_c->applyToSelection([v](Shape&sh){ sh.stroke.calligraphic=true; sh.stroke.calligAngle=v; },"Nib angle"); });
        if(m_c->tool()==Tool::Freehand)
            dspin("Smooth",0.5,40,m_c->freehandSmoothing()," px",[this](double v){ m_c->setFreehandSmoothing(v); });
        break; }
    case Tool::Shadow: {
        check("Shadow",sel?sel->shadow.on:false,[this](bool v){ m_c->applyToSelection([v](Shape&s){ s.shadow.on=v; },"Shadow"); });
        dspin("Blur",0,64,sel?sel->shadow.blur:12," px",[this](double v){ m_c->applyToSelection([v](Shape&s){ s.shadow.on=true; s.shadow.blur=v; },"Shadow blur"); });
        auto* h=new QLabel("Drag an object to cast its shadow; orange handle = blur; double-click (or drop a colour on) the swatch = colour. Floor shadow: drag the teal tip to rake it.");
        h->setObjectName("infoHint"); m_lay->addWidget(h); break; }
    case Tool::Contour: {
        check("Contour",sel?sel->contour.on:false,[this](bool v){ m_c->applyToSelection([v](Shape&s){ s.contour.on=v; },"Contour"); });
        dspin("Width",-100,100,sel?sel->contour.width:8," px",[this](double v){ m_c->applyToSelection([v](Shape&s){ s.contour.on=true; s.contour.width=v; },"Contour width"); });
        ispin("Steps",1,32,sel?sel->contour.steps:4,[this](int v){ m_c->applyToSelection([v](Shape&s){ s.contour.on=true; s.contour.steps=v; },"Contour steps"); });
        auto* h=new QLabel("Click an object to add a contour; drag the orange handle out (or in, for an inner contour) to set the width; double-click the top colour swatch (or drop a colour) to recolour it, or the lower swatch to set an outer graduation colour.");
        h->setObjectName("infoHint"); m_lay->addWidget(h); break; }
    case Tool::Bevel: {
        check("Bevel",sel&&sel->bevel.on,[this](bool v){ m_c->applyToSelection([v](Shape&s){ s.bevel.on=v; if(v&&s.bevel.depth<=0)s.bevel.depth=7; },"Bevel"); });
        dspin("Depth",1,40,sel?sel->bevel.depth:7," px",[this](double v){ m_c->applyToSelection([v](Shape&s){ s.bevel.on=true; s.bevel.depth=v; },"Bevel depth"); });
        static const int bt[]={0,1,2,3,6,8,11}; int cur=0; if(sel){ for(int k=0;k<7;++k) if(bt[k]==sel->bevel.type){cur=k;break;} }
        combo({"Flat","Round","Half-round","Frame","Smooth","Point","Ruffle"},cur,[this](int i){ int tp=bt[qBound(0,i,6)]; m_c->applyToSelection([tp](Shape&s){ s.bevel.on=true; s.bevel.type=tp; },"Bevel type"); });
        dspin("Light",0,360,sel?sel->bevel.light:45,"°",[this](double v){ m_c->applyToSelection([v](Shape&s){ s.bevel.on=true; s.bevel.light=v; },"Bevel light"); });
        ispin("Contrast",0,100,sel?sel->bevel.contrast:85,[this](int v){ m_c->applyToSelection([v](Shape&s){ s.bevel.on=true; s.bevel.contrast=v; },"Bevel contrast"); });
        dspin("Tilt",0,90,sel?sel->bevel.tilt:45,"°",[this](double v){ m_c->applyToSelection([v](Shape&s){ s.bevel.on=true; s.bevel.tilt=v; },"Bevel tilt"); });
        check("Inner",sel&&!sel->bevel.outer,[this](bool v){ m_c->applyToSelection([v](Shape&s){ s.bevel.outer=!v; },"Bevel direction"); });
        { auto* h=new QLabel("Click to bevel; drag the orange handle for depth, the yellow sun to aim the light, or double-click the colour swatch (drop a colour) to tint the light."); h->setObjectName("infoHint"); m_lay->addWidget(h); }
        break; }
    case Tool::Feather: {
        dspin("Feather",0,200,sel?sel->feather:0," px",[this](double v){ m_c->applyToSelection([v](Shape&s){ s.feather=v; },"Feather"); });
        dspin("Bias",-1,1,sel?sel->featherBias:0,"",[this](double v){ m_c->applyToSelection([v](Shape&s){ s.featherBias=qBound(-1.0,v,1.0); },"Feather bias"); });   // non-linear falloff (Schlick)
        dspin("Gain",-1,1,sel?sel->featherGain:0,"",[this](double v){ m_c->applyToSelection([v](Shape&s){ s.featherGain=qBound(-1.0,v,1.0); },"Feather gain"); });
        auto* h=new QLabel("Select an object, then drag the orange corner handle out to soften (feather) its edge.");
        h->setObjectName("infoHint"); m_lay->addWidget(h); break; }
    case Tool::Extrude: {
        check("3D",sel&&sel->extrude.on,[this](bool v){ m_c->applyToSelection([v](Shape&s){ s.extrude.on=v; },"Extrude"); });
        dspin("Depth",0,200,sel?sel->extrude.depth:28," px",[this](double v){ m_c->applyToSelection([v](Shape&s){ s.extrude.on=true; s.extrude.depth=v; },"Extrude depth"); });
        dspin("Angle",-180,180,sel?sel->extrude.angle:35,"°",[this](double v){ m_c->applyToSelection([v](Shape&s){ s.extrude.on=true; s.extrude.angle=v; },"Extrude angle"); });
        ispin("Light",0,360,sel?sel->extrude.light:135,[this](int v){ m_c->applyToSelection([v](Shape&s){ s.extrude.on=true; s.extrude.light=v; },"Extrude light"); });
        dspin("Tilt X",-80,80,sel?sel->extrude.rotX:0,"°",[this](double v){ m_c->applyToSelection([v](Shape&s){ s.extrude.on=true; s.extrude.rotX=v; },"Extrude tilt X"); });
        dspin("Tilt Y",-80,80,sel?sel->extrude.rotY:0,"°",[this](double v){ m_c->applyToSelection([v](Shape&s){ s.extrude.on=true; s.extrude.rotY=v; },"Extrude tilt Y"); });
        dspin("Bevel",0,60,sel?sel->extrude.bevel:0," px",[this](double v){ m_c->applyToSelection([v](Shape&s){ s.extrude.on=true; s.extrude.bevel=v; },"Extrude bevel"); });
        auto* h=new QLabel("Click to extrude; drag the orange handle for depth+direction, the yellow sun to aim the light. Tilt X/Y rotate the solid in true 3-D; Bevel chamfers the front edge.");
        h->setObjectName("infoHint"); m_lay->addWidget(h); break; }
    case Tool::Eraser: {
        dspin("Nib",1,80,m_c->eraserNib()," px",[this](double v){ m_c->setEraserNib(v); });
        check("Soft edge",m_c->eraserSoft(),[this](bool v){ m_c->setEraserSoft(v); rebuild(); });
        { auto* h=new QLabel(m_c->eraserSoft()? "Paint over a photo to softly rub it away (feathered alpha)."
                                              : "Click a shape to punch out a hole (becomes an editable path).");
          h->setObjectName("infoHint"); m_lay->addWidget(h); } break; }
    case Tool::Clone: {
        combo({"Clone","Heal","Red-eye"},m_c->cloneMode(),[this](int i){ m_c->setCloneMode(i); rebuild(); });
        dspin("Brush",2,200,m_c->cloneNib()," px",[this](double v){ m_c->setCloneNib(v); });
        dspin("Hardness",0,1,m_c->cloneHardness(),"",[this](double v){ m_c->setCloneHardness(v); });
        auto* h=new QLabel(m_c->cloneMode()==2 ? "Click/drag over a photo's red eye to de-red it locally."
                                               : "Alt-click a photo to set the source, then paint to copy (Heal blends colour).");
        h->setObjectName("infoHint"); m_lay->addWidget(h); break; }
    case Tool::Blend: { static int steps=6; ispin("Steps",1,64,steps,[this](int v){ steps=v; m_c->setLiveBlendSteps(v); });
        btn("Live",[this]{ m_c->makeLiveBlend(steps); });   // re-editable blend group (drag ends / step handle on canvas)
        dspin("Profile",-1,1,(sel&&sel->blendSteps>0)?sel->blendProfile:0.0,"",[this](double v){ m_c->applyToSelection([v](Shape&s){ if(s.type==ShapeType::Group && s.blendSteps>0) s.blendProfile=qBound(-1.0,v,1.0); },"Blend profile"); });   // ease the steps (Schlick)
        dspin("Nodes",0,1,(sel&&sel->blendSteps>0)?sel->blendNodeShift:0.0,"",[this](double v){ m_c->applyToSelection([v](Shape&s){ if(s.type==ShapeType::Group && s.blendSteps>0) s.blendNodeShift=v; },"Blend node correspondence"); });   // rotate start↔end node mapping
        dspin("Attr",-1,1,(sel&&sel->blendSteps>0)?sel->blendAttrProfile:0.0,"",[this](double v){ m_c->applyToSelection([v](Shape&s){ if(s.type==ShapeType::Group && s.blendSteps>0) s.blendAttrProfile=qBound(-1.0,v,1.0); },"Blend colour profile"); });   // separate ease for colour/thickness
        btn("Blend",[this]{ m_c->blendSelection(steps); });
        btn("Along Path",[this]{ m_c->blendAlongPathSelection(steps); });
        btn("Live Path",[this]{ m_c->makeLiveBlendAlongPath(steps); }); break; }   // re-editable blend distributed along the last-selected spine
    case Tool::Mould: {
        combo({"Perspective","Envelope"},m_c->mouldMode(),[this](int i){ m_c->setMouldMode(i); rebuild(); });
        auto* h=new QLabel(m_c->mouldMode()? "Select a shape, then drag the corner and curved-edge handles to bend it."
                                           : "Select a shape, then drag the corner handles to warp it into perspective.");
        h->setObjectName("infoHint"); m_lay->addWidget(h); break; }
    case Tool::Zoom:
        btn("−",[this]{ m_c->zoomOut(); }); btn("Fit",[this]{ m_c->zoomFit(); }); btn("100%",[this]{ m_c->zoomReset(); }); btn("+",[this]{ m_c->zoomIn(); });
        break;
    default: {
        static const QHash<int,QString> hints={{int(Tool::Node),"Drag a node or handle to reshape; drag the line itself to bend it; double-click the line to add a node."},
            {int(Tool::Ellipse),"Drag on the canvas to draw an ellipse (Ctrl = circle)."},
            {int(Tool::Rect),"Drag on the canvas to draw a rectangle (Ctrl = square)."},
            {int(Tool::Polygon),"Drag to draw a polygon / star (Ctrl = regular)."},
            {int(Tool::Line),"Drag to draw a straight line (Ctrl = snap to 15°)."},
            {int(Tool::Pan),"Drag to push the canvas around."}};
        auto* h=new QLabel(hints.value(int(t))); h->setObjectName("infoHint"); m_lay->addWidget(h); break; }
    }
    m_lay->addStretch();
}

// ===== Colour Line: bottom palette strip ====================================
static QVector<QPair<QString,QColor>> swatchPalette() {
    return {{"White",QColor(255,255,255)},{"Black",QColor(0,0,0)},
        {"Red",QColor(220,40,40)},{"Orange",QColor(240,140,30)},{"Yellow",QColor(245,210,40)},
        {"Lime",QColor(140,205,50)},{"Green",QColor(40,170,80)},{"Teal",QColor(30,170,160)},
        {"Cyan",QColor(40,180,230)},{"Blue",QColor(45,110,210)},{"Indigo",QColor(80,70,190)},
        {"Violet",QColor(150,70,200)},{"Magenta",QColor(210,60,170)},{"Pink",QColor(240,130,170)},
        {"Brown",QColor(140,90,50)},{"Sand",QColor(225,200,150)},
        {"90% grey",QColor(38,38,38)},{"70% grey",QColor(77,77,77)},{"50% grey",QColor(128,128,128)},{"30% grey",QColor(179,179,179)},{"10% grey",QColor(230,230,230)}};
}
static const int kSw=20, kPad=5, kNoCol=22;   // swatch size, padding, no-colour box width
ColourLine::ColourLine(Canvas* c, QWidget* parent):QWidget(parent),m_c(c){
    setObjectName("colourline"); setFixedHeight(kSw+2*kPad); setMouseTracking(true);
    setToolTip("Left-click: fill   Right-click: line colour");
}
QSize ColourLine::sizeHint() const { return QSize(400,kSw+2*kPad); }
int ColourLine::swatchAt(const QPoint& p) const {
    if(p.x()<kPad) return -2;
    if(p.x()<kPad+kNoCol) return -1;                         // the "no colour" box
    const int rel=p.x()-kPad-kNoCol-6; if(rel<0) return -2;
    if(rel%(kSw+2) >= kSw) return -2;                        // pointer is in the 2px gap between swatches
    int i=rel/(kSw+2); return (i>=0 && i<swatchPalette().size())? i : -2;
}
void ColourLine::paintEvent(QPaintEvent*){
    QPainter g(this);
    g.fillRect(rect(), QColor(33,34,38));
    QRect nc(kPad,kPad,kNoCol,kSw); g.setBrush(Qt::white); g.setPen(QPen(QColor(0,0,0,120))); g.drawRect(nc);
    g.setPen(QPen(QColor(220,40,40),2)); g.drawLine(nc.topRight(),nc.bottomLeft());
    auto pal=swatchPalette(); int x=kPad+kNoCol+6;
    for(int i=0;i<pal.size();++i){ QRect r(x,kPad,kSw,kSw);
        g.setPen(QPen(i==m_hover?QColor(255,255,255):QColor(0,0,0,120), i==m_hover?2:1));
        g.setBrush(pal[i].second); g.drawRect(r); x+=kSw+2; }
}
void ColourLine::applyColour(int idx, bool line){
    QColor col; bool none=(idx==-1);
    if(!none){ if(idx<0||idx>=swatchPalette().size())return; col=swatchPalette()[idx].second; }
    if(line){ m_c->applyToSelection([&](Shape&s){ if(none)s.stroke.enabled=false; else { s.stroke.enabled=true; s.stroke.color=col; } },"Line colour");
        Stroke st=m_c->currentStroke(); st.enabled=!none; if(!none)st.color=col; m_c->setCurrentStroke(st); }
    else { // a real colour while editing text with a selection recolours just those characters (Inkscape-style)
        if(!none && m_c->formatEditSelection([col](QFont&,QColor&c){ c=col; },"Text colour")) return;
        m_c->applyToSelection([&](Shape&s){ if(none)s.fill.type=FillType::None; else { s.fill.type=FillType::Solid; s.fill.color=col; } },"Fill colour");
        Fill f=m_c->currentFill(); f.type=none?FillType::None:FillType::Solid; if(!none)f.color=col; m_c->setCurrentFill(f); }
}
void ColourLine::mousePressEvent(QMouseEvent* e){
    m_pressPos=e->pos(); m_pressIdx=swatchAt(e->pos()); m_pressBtn=e->button(); m_dragging=false;
    // (apply happens on release if it wasn't a drag — so a swatch can be dragged onto an object)
}
void ColourLine::mouseReleaseEvent(QMouseEvent* e){
    if(e->button()==m_pressBtn && !m_dragging && m_pressIdx!=-2 && swatchAt(e->pos())==m_pressIdx)
        applyColour(m_pressIdx, m_pressBtn==Qt::RightButton);
    if(e->button()==m_pressBtn){ m_pressIdx=-2; m_dragging=false; }
}
void ColourLine::contextMenuEvent(QContextMenuEvent* e){ e->accept(); }   // right-click handled on release
void ColourLine::mouseMoveEvent(QMouseEvent* e){
    // Start a colour drag once the pointer leaves the swatch's press point (a real colour, not "no colour").
    if(!m_dragging && (e->buttons()&Qt::LeftButton) && m_pressIdx>=0
       && (e->pos()-m_pressPos).manhattanLength()>=QApplication::startDragDistance()){
        m_dragging=true; const QColor col=swatchPalette()[m_pressIdx].second;
        QMimeData* md=new QMimeData; md->setColorData(col);
        QPixmap pm(18,18); pm.fill(col); { QPainter p(&pm); p.setPen(QColor(0,0,0,120)); p.drawRect(0,0,17,17); }
        QDrag* drag=new QDrag(this); drag->setMimeData(md); drag->setPixmap(pm); drag->setHotSpot(QPoint(9,9));
        drag->exec(Qt::CopyAction);
        return;
    }
    int idx=swatchAt(e->pos()); if(idx!=m_hover){ m_hover=idx; update(); }
    if(idx>=0) setToolTip(swatchPalette()[idx].first+"  (drag onto an object to colour it; Shift = line)"); else if(idx==-1) setToolTip("No colour");
}

} // namespace xara
#include "panels.moc"
