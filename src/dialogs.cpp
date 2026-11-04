// dialogs.cpp — modal dialogs. The Color Editor is the signature piece: an
// SV square + hue strip + alpha, every colour model, hex, and named-colour swatches.
#include "dialogs.h"
#include "canvas.h"
#include "photoops.h"
#include <QtWidgets>
#include <QSettings>
#include <QRegularExpression>
#include <functional>

namespace xara {

// ---- SV square: saturation (x) × value (y) for a fixed hue --------------------
class SVSquare : public QWidget {
public:
    std::function<void(double,double)> onPick;     // (s, v)
    SVSquare(){ setFixedSize(220,200); setCursor(Qt::CrossCursor); }
    void setHSV(double h,double s,double v){ if(qAbs(h-m_h)>1e-9){ m_h=h; m_img=QImage(); } m_s=s; m_v=v; update(); }
protected:
    void rebuild(){ const int W=width(), H=height(); m_img=QImage(W,H,QImage::Format_RGB32);
        for(int y=0;y<H;++y){ QRgb* r=(QRgb*)m_img.scanLine(y); double v=1.0-double(y)/(H-1);
            for(int x=0;x<W;++x){ double s=double(x)/(W-1); r[x]=QColor::fromHsvF(m_h,s,v).rgb(); } } }
    void paintEvent(QPaintEvent*) override {
        if(m_img.isNull()||m_img.size()!=size()) rebuild();
        QPainter g(this); g.drawImage(0,0,m_img);
        double x=m_s*(width()-1), y=(1.0-m_v)*(height()-1);
        g.setRenderHint(QPainter::Antialiasing); g.setPen(QPen(Qt::white,2)); g.setBrush(Qt::NoBrush);
        g.drawEllipse(QPointF(x,y),6,6); g.setPen(QPen(Qt::black,1)); g.drawEllipse(QPointF(x,y),7,7); }
    void grab(const QPointF& p){ double s=qBound(0.0,p.x()/(width()-1),1.0), v=qBound(0.0,1.0-p.y()/(height()-1),1.0);
        m_s=s; m_v=v; if(onPick) onPick(s,v); update(); }
    void mousePressEvent(QMouseEvent* e) override { grab(e->position()); }
    void mouseMoveEvent(QMouseEvent* e) override { if(e->buttons()&Qt::LeftButton) grab(e->position()); }
    double m_h=0,m_s=1,m_v=1; QImage m_img;
};

// ---- vertical hue strip ------------------------------------------------------
class HueStrip : public QWidget {
public:
    std::function<void(double)> onHue;
    HueStrip(){ setFixedSize(22,200); setCursor(Qt::PointingHandCursor); }
    void setHue(double h){ m_h=h; update(); }
protected:
    void paintEvent(QPaintEvent*) override { QPainter g(this); const int H=height();
        for(int y=0;y<H;++y){ g.setPen(QColor::fromHsvF(1.0-double(y)/(H-1),1,1)); g.drawLine(1,y,width()-2,y); }
        double y=(1.0-m_h)*(H-1); g.setRenderHint(QPainter::Antialiasing);
        g.setPen(QPen(Qt::white,2)); g.setBrush(Qt::NoBrush); g.drawRect(QRectF(0.5,y-2.5,width()-1,5));
        g.setPen(QPen(Qt::black,1)); g.drawRect(QRectF(0.5,y-3.5,width()-1,7)); }
    void grab(double yy){ m_h=qBound(0.0,1.0-yy/(height()-1),1.0); if(onHue) onHue(m_h); update(); }
    void mousePressEvent(QMouseEvent* e) override { grab(e->position().y()); }
    void mouseMoveEvent(QMouseEvent* e) override { if(e->buttons()&Qt::LeftButton) grab(e->position().y()); }
    double m_h=0;
};

// small swatch button
static QPushButton* swatch(const QColor& c, const QString& tip){
    auto* b=new QPushButton; b->setFixedSize(20,20); b->setToolTip(tip); b->setCursor(Qt::PointingHandCursor);
    b->setStyleSheet(QString("background:%1;border:1px solid #222;border-radius:3px;").arg(c.name()));
    return b;
}

QColor ColorEditor::pick(QWidget* parent, const QColor& initial, Canvas* canvas, bool allowAlpha){
    QColor cur = initial.isValid()? initial : QColor(Qt::white);

    QDialog dlg(parent); dlg.setWindowTitle("Colour Editor"); dlg.setModal(true);
    auto* top=new QHBoxLayout(&dlg);

    // left: SV square + hue + alpha
    auto* sv=new SVSquare; auto* hue=new HueStrip;
    auto* alpha=new QSlider(Qt::Vertical); alpha->setRange(0,255); alpha->setFixedHeight(200); alpha->setValue(cur.alpha());
    alpha->setVisible(allowAlpha);
    auto* left=new QHBoxLayout; left->setSpacing(8); left->addWidget(sv); left->addWidget(hue); left->addWidget(alpha);
    top->addLayout(left);

    // right: model + fields + hex + preview + named swatches
    auto* right=new QVBoxLayout; right->setSpacing(8); top->addLayout(right);

    auto* preview=new QFrame; preview->setFixedHeight(34); preview->setFrameShape(QFrame::StyledPanel);
    auto* prevLay=new QHBoxLayout(preview); prevLay->setContentsMargins(0,0,0,0); prevLay->setSpacing(0);
    auto* oldSw=new QLabel; auto* newSw=new QLabel; oldSw->setAutoFillBackground(false); newSw->setAutoFillBackground(false);
    prevLay->addWidget(oldSw); prevLay->addWidget(newSw);
    right->addWidget(preview);

    auto* modelCombo=new QComboBox; modelCombo->addItems({"RGB","HSV","HSL","CMYK"}); right->addWidget(modelCombo);
    auto* fields=new QFormLayout; right->addLayout(fields);
    QVector<QSpinBox*> sp; QVector<QLabel*> lbl;
    for(int i=0;i<4;++i){ auto* s=new QSpinBox; s->setRange(0,255); auto* l=new QLabel; fields->addRow(l,s); sp<<s; lbl<<l; }
    auto* hex=new QLineEdit; hex->setMaxLength(9); hex->setPlaceholderText("#RRGGBB"); fields->addRow("Hex",hex);

    bool block=false;
    auto syncPreview=[&]{ oldSw->setStyleSheet(QString("background:rgba(%1,%2,%3,%4)").arg(initial.red()).arg(initial.green()).arg(initial.blue()).arg(initial.alpha()));
        newSw->setStyleSheet(QString("background:rgba(%1,%2,%3,%4)").arg(cur.red()).arg(cur.green()).arg(cur.blue()).arg(cur.alpha())); };

    auto refreshFields=[&]{ block=true; int m=modelCombo->currentIndex();
        const char* names[4][4]={{"R","G","B","A"},{"H","S","V","A"},{"H","S","L","A"},{"C","M","Y","K"}};
        int max[4][4]={{255,255,255,255},{359,255,255,255},{359,255,255,255},{100,100,100,100}};
        int n = (m==3)?4:(allowAlpha?4:3);
        for(int i=0;i<4;++i){ bool vis=i<n; sp[i]->setVisible(vis); lbl[i]->setVisible(vis);
            lbl[i]->setText(names[m][i]); sp[i]->setRange(0,max[m][i]); }
        if(m==0){ sp[0]->setValue(cur.red()); sp[1]->setValue(cur.green()); sp[2]->setValue(cur.blue()); sp[3]->setValue(cur.alpha()); }
        else if(m==1){ int h,s,v,a; cur.getHsv(&h,&s,&v,&a); sp[0]->setValue(qMax(0,h)); sp[1]->setValue(s); sp[2]->setValue(v); sp[3]->setValue(a); }
        else if(m==2){ int h,s,l,a; cur.getHsl(&h,&s,&l,&a); sp[0]->setValue(qMax(0,h)); sp[1]->setValue(s); sp[2]->setValue(l); sp[3]->setValue(a); }
        else { int c,mm,y,k,a; cur.getCmyk(&c,&mm,&y,&k,&a); sp[0]->setValue(c*100/255); sp[1]->setValue(mm*100/255); sp[2]->setValue(y*100/255); sp[3]->setValue(k*100/255); }
        hex->setText(allowAlpha&&cur.alpha()<255? cur.name(QColor::HexArgb) : cur.name(QColor::HexRgb));
        block=false; };

    auto syncPickers=[&]{ double h,s,v; h=qMax(0.0,cur.hueF()); s=cur.saturationF(); v=cur.valueF();
        sv->setHSV(h,s,v); hue->setHue(h); alpha->setValue(cur.alpha()); };

    auto setCur=[&](QColor c){ if(allowAlpha) c.setAlpha(cur.alpha()); else c.setAlpha(255); cur=c; syncPickers(); refreshFields(); syncPreview(); };

    sv->onPick=[&](double s,double v){ double h=qMax(0.0,cur.hueF()); QColor c=QColor::fromHsvF(h,s,v); c.setAlpha(cur.alpha()); cur=c; refreshFields(); syncPreview(); };
    hue->onHue=[&](double h){ QColor c=QColor::fromHsvF(h,cur.saturationF(),cur.valueF()); c.setAlpha(cur.alpha()); cur=c; sv->setHSV(h,cur.saturationF(),cur.valueF()); refreshFields(); syncPreview(); };
    QObject::connect(alpha,&QSlider::valueChanged,&dlg,[&](int a){ if(block)return; cur.setAlpha(a); refreshFields(); syncPreview(); });
    QObject::connect(modelCombo,QOverload<int>::of(&QComboBox::currentIndexChanged),&dlg,[&]{ refreshFields(); });
    auto fieldsChanged=[&]{ if(block)return; int m=modelCombo->currentIndex(); int a=allowAlpha?(m==3?cur.alpha():sp[3]->value()):255; QColor c;
        if(m==0) c=QColor(sp[0]->value(),sp[1]->value(),sp[2]->value(),a);
        else if(m==1) c=QColor::fromHsv(sp[0]->value(),sp[1]->value(),sp[2]->value(),a);
        else if(m==2) c=QColor::fromHsl(sp[0]->value(),sp[1]->value(),sp[2]->value(),a);
        else c=QColor::fromCmyk(sp[0]->value()*255/100,sp[1]->value()*255/100,sp[2]->value()*255/100,sp[3]->value()*255/100,cur.alpha());
        cur=c; syncPickers(); hex->setText(allowAlpha&&cur.alpha()<255?cur.name(QColor::HexArgb):cur.name(QColor::HexRgb)); syncPreview(); };
    for(auto* s: sp) QObject::connect(s,QOverload<int>::of(&QSpinBox::valueChanged),&dlg,[&]{ fieldsChanged(); });
    QObject::connect(hex,&QLineEdit::editingFinished,&dlg,[&]{ QColor c(hex->text().trimmed()); if(c.isValid()){ setCur(c); } });

    // named-colour quick-picks from the document + a default ramp
    auto* swRow=new QGridLayout; swRow->setSpacing(3); right->addLayout(swRow);
    int col=0,row=0; auto addSw=[&](const QColor& c,const QString& tip){ auto* b=swatch(c,tip);
        QObject::connect(b,&QPushButton::clicked,&dlg,[&,c]{ setCur(c); }); swRow->addWidget(b,row,col); if(++col>=10){col=0;row++;} };
    if(canvas){ const auto& nc=canvas->doc().namedColors; for(int i=0;i<nc.size();++i) addSw(canvas->doc().resolveNamed(i), nc[i].name); }
    const QColor ramp[]={Qt::black,QColor(64,64,64),QColor(128,128,128),QColor(192,192,192),Qt::white,
        Qt::red,QColor(255,140,0),Qt::yellow,Qt::green,QColor(0,160,160),Qt::blue,QColor(120,40,200),QColor(255,0,200)};
    for(auto& c: ramp) addSw(c, c.name());

    right->addStretch();
    auto* bb=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);
    QObject::connect(bb,&QDialogButtonBox::accepted,&dlg,&QDialog::accept);
    QObject::connect(bb,&QDialogButtonBox::rejected,&dlg,&QDialog::reject);
    right->addWidget(bb);

    syncPickers(); refreshFields(); syncPreview();
    return dlg.exec()==QDialog::Accepted ? cur : QColor();
}

// ============================================================================
// OptionsDialog — full tabbed program/document options (manual p607-632)
// ============================================================================
void OptionsDialog::show(QWidget* parent, Canvas* canvas){
    QSettings cfg("XarQ","XarQ");
    QDialog dlg(parent); dlg.setWindowTitle("Options"); dlg.setModal(true); dlg.resize(460,420);
    auto* V=new QVBoxLayout(&dlg);
    auto* tabs=new QTabWidget; V->addWidget(tabs,1);

    // -- General --
    { auto* w=new QWidget; auto* f=new QFormLayout(w);
      auto* undoSp=new QSpinBox; undoSp->setRange(1,1000); undoSp->setValue(cfg.value("general/undoLevels",200).toInt()); f->addRow("Undo levels",undoSp);
      auto* recentSp=new QSpinBox; recentSp->setRange(0,20); recentSp->setValue(cfg.value("general/recentFiles",8).toInt()); f->addRow("Recent files",recentSp);
      auto* save=new QCheckBox("Confirm before closing unsaved documents"); save->setChecked(cfg.value("general/confirmClose",true).toBool()); f->addRow(save);
      QObject::connect(&dlg,&QDialog::accepted,&dlg,[=,&cfg]{ cfg.setValue("general/undoLevels",undoSp->value()); cfg.setValue("general/recentFiles",recentSp->value()); cfg.setValue("general/confirmClose",save->isChecked()); });
      tabs->addTab(w,"General"); }

    // -- Grid & Ruler --
    { auto* w=new QWidget; auto* f=new QFormLayout(w);
      auto* showGrid=new QCheckBox("Show grid"); showGrid->setChecked(canvas->showGrid()); f->addRow(showGrid);
      auto* snapGrid=new QCheckBox("Snap to grid"); snapGrid->setChecked(canvas->snapToGrid()); f->addRow(snapGrid);
      auto* spacing=new QDoubleSpinBox; spacing->setRange(2,1000); spacing->setSuffix(" px"); spacing->setValue(canvas->gridSpacing()); f->addRow("Grid spacing",spacing);
      auto* showRul=new QCheckBox("Show rulers"); showRul->setChecked(canvas->showRulers()); f->addRow(showRul);
      auto* snapObj=new QCheckBox("Snap to objects"); snapObj->setChecked(canvas->snapEnabled()); f->addRow(snapObj);
      QObject::connect(&dlg,&QDialog::accepted,&dlg,[=]{ canvas->setShowGrid(showGrid->isChecked()); canvas->setSnapToGrid(snapGrid->isChecked()); canvas->setGridSpacing(spacing->value()); canvas->setShowRulers(showRul->isChecked()); canvas->setSnap(snapObj->isChecked()); });
      tabs->addTab(w,"Grid && Ruler"); }

    // -- Units --
    { auto* w=new QWidget; auto* f=new QFormLayout(w);
      auto* unit=new QComboBox; unit->addItems({"Pixels","Millimetres","Centimetres","Inches","Points"}); unit->setCurrentIndex(int(canvas->units())); f->addRow("Page units",unit);
      QObject::connect(&dlg,&QDialog::accepted,&dlg,[=]{ canvas->setUnits(Canvas::Units(unit->currentIndex())); });
      tabs->addTab(w,"Units"); }

    // -- Page --
    { auto* w=new QWidget; auto* f=new QFormLayout(w);
      auto* wsp=new QDoubleSpinBox; wsp->setRange(1,40000); wsp->setDecimals(0); wsp->setSuffix(" px"); wsp->setValue(canvas->pageSize().width()); f->addRow("Page width",wsp);
      auto* hsp=new QDoubleSpinBox; hsp->setRange(1,40000); hsp->setDecimals(0); hsp->setSuffix(" px"); hsp->setValue(canvas->pageSize().height()); f->addRow("Page height",hsp);
      QObject::connect(&dlg,&QDialog::accepted,&dlg,[=]{ canvas->setPageSize(QSizeF(wsp->value(),hsp->value())); });
      tabs->addTab(w,"Page Size"); }

    // -- Backups --
    { auto* w=new QWidget; auto* f=new QFormLayout(w);
      auto* autosave=new QCheckBox("Automatic backups"); autosave->setChecked(cfg.value("backups/auto",true).toBool()); f->addRow(autosave);
      auto* interval=new QSpinBox; interval->setRange(1,120); interval->setSuffix(" min"); interval->setValue(cfg.value("backups/interval",10).toInt()); f->addRow("Backup interval",interval);
      QObject::connect(&dlg,&QDialog::accepted,&dlg,[=,&cfg]{ cfg.setValue("backups/auto",autosave->isChecked()); cfg.setValue("backups/interval",interval->value()); });
      tabs->addTab(w,"Backups"); }

    // -- Mouse / Tune-ups --
    { auto* w=new QWidget; auto* f=new QFormLayout(w);
      auto* wheelZoom=new QCheckBox("Mouse wheel zooms (else scrolls)"); wheelZoom->setChecked(cfg.value("mouse/wheelZoom",true).toBool()); f->addRow(wheelZoom);
      auto* nudge=new QDoubleSpinBox; nudge->setRange(0.1,100); nudge->setSuffix(" px"); nudge->setValue(cfg.value("mouse/nudge",1.0).toDouble()); f->addRow("Arrow-key nudge",nudge);
      auto* quality=new QComboBox; quality->addItems({"Outline","Low","Normal","High (anti-aliased)"}); quality->setCurrentIndex(cfg.value("view/quality",3).toInt()); f->addRow("Render quality",quality);
      QObject::connect(&dlg,&QDialog::accepted,&dlg,[=,&cfg]{ cfg.setValue("mouse/wheelZoom",wheelZoom->isChecked()); cfg.setValue("mouse/nudge",nudge->value()); cfg.setValue("view/quality",quality->currentIndex()); });
      tabs->addTab(w,"Mouse && View"); }

    auto* bb=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);
    QObject::connect(bb,&QDialogButtonBox::accepted,&dlg,&QDialog::accept);
    QObject::connect(bb,&QDialogButtonBox::rejected,&dlg,&QDialog::reject);
    V->addWidget(bb);
    dlg.exec();
}

// ============================================================================
// Photo adjustments (manual p346 "Colouring photos", p390 "Brightness levels")
// ============================================================================
QImage adjustPhoto(const QImage& src0, int bright, int contrast, int sat, bool gray, bool autoLevel, bool redeye,
                   int blur, int sharpen, int vignette, int lens, int hdr, int blackPt, int whitePt, double gamma){
    if(src0.isNull()) return src0;
    QImage src=src0.convertToFormat(QImage::Format_ARGB32);
    // Input Levels: remap [black..white] → [0..255] with a midtone gamma, precomputed as a 256-entry LUT.
    const bool doLevels = (blackPt>0 || whitePt<255 || gamma!=1.0);
    int levLut[256];
    if(doLevels){ const int bp=qBound(0,blackPt,254), wp=qBound(bp+1,whitePt,255); const double ig=1.0/qBound(0.1,gamma,9.99);
        for(int v=0;v<256;++v){ double t=double(v-bp)/double(wp-bp); t=qBound(0.0,t,1.0); t=std::pow(t,ig); levLut[v]=qBound(0,int(t*255.0+0.5),255); } }
    // auto-levels: find per-channel min/max and stretch to 0..255
    int lo[3]={255,255,255}, hi[3]={0,0,0};
    if(autoLevel){ for(int y=0;y<src.height();y+=2){ const QRgb* r=(const QRgb*)src.constScanLine(y);
        for(int x=0;x<src.width();x+=2){ QRgb p=r[x]; int c[3]={qRed(p),qGreen(p),qBlue(p)}; for(int k=0;k<3;++k){ lo[k]=qMin(lo[k],c[k]); hi[k]=qMax(hi[k],c[k]); } } }
        for(int k=0;k<3;++k) if(hi[k]<=lo[k]) hi[k]=lo[k]+1; }
    const double cf = std::pow((100.0+contrast)/100.0, 2);   // contrast factor
    const double sf = (100.0+sat)/100.0;                      // saturation factor
    QImage out(src.size(), QImage::Format_ARGB32);
    for(int y=0;y<src.height();++y){ const QRgb* r=(const QRgb*)src.constScanLine(y); QRgb* o=(QRgb*)out.scanLine(y);
        for(int x=0;x<src.width();++x){ QRgb p=r[x]; double rr=qRed(p), gg=qGreen(p), bb=qBlue(p);
            if(doLevels){ rr=levLut[int(rr)]; gg=levLut[int(gg)]; bb=levLut[int(bb)]; }   // input Levels first
            if(autoLevel){ rr=(rr-lo[0])*255.0/(hi[0]-lo[0]); gg=(gg-lo[1])*255.0/(hi[1]-lo[1]); bb=(bb-lo[2])*255.0/(hi[2]-lo[2]); }
            rr+=bright; gg+=bright; bb+=bright;                         // brightness
            rr=(rr-128)*cf+128; gg=(gg-128)*cf+128; bb=(bb-128)*cf+128; // contrast
            double lum=0.299*rr+0.587*gg+0.114*bb;                     // saturation / grayscale
            if(gray){ rr=gg=bb=lum; } else { rr=lum+(rr-lum)*sf; gg=lum+(gg-lum)*sf; bb=lum+(bb-lum)*sf; }
            if(redeye){ double mx=qMax(gg,bb); if(rr>mx*1.4 && rr>60){ rr=mx*0.8; gg=mx*0.85; bb=mx*0.85; } }   // de-red red-eye pixels
            o[x]=qRgba(qBound(0,(int)(rr+0.5),255),qBound(0,(int)(gg+0.5),255),qBound(0,(int)(bb+0.5),255),qAlpha(p)); } }
    // spatial filters (after the per-pixel adjustments)
    if(lens!=0)   out = xara::lensCorrect(out, lens/200.0);            // −100..100 → k1 −0.5..0.5 (barrel/pincushion)
    if(hdr>0)     out = xara::tonemapHDR(out, 1.0 + hdr/50.0);         // 0..100 → exposure 1..3×
    if(blur>0)    out = xara::gaussianBlurImage(out, blur);
    if(sharpen>0) out = xara::unsharpMask(out, sharpen/50.0);          // 0..100 → 0..2× amount
    if(vignette>0)out = xara::vignette(out, vignette/100.0);
    return out;
}

void PhotoDialog::show(QWidget* parent, Canvas* canvas){
    Shape* sel=canvas->firstSelected();
    if(!sel || sel->fill.type!=FillType::Bitmap || (sel->fill.image.isNull()&&sel->fill.origImage.isNull())){
        QMessageBox::information(parent,"Adjust Photo","Select a photo / bitmap object first.\nUse File ▸ Import Image… to place one."); return; }
    if(sel->fill.origImage.isNull()) canvas->applyToSelection([](Shape& s){ s.fill.origImage=s.fill.image; },"Photo source");
    QImage orig = canvas->firstSelected()->fill.origImage;

    QDialog dlg(parent); dlg.setWindowTitle("Adjust Photo"); dlg.setModal(true);
    auto* f=new QFormLayout(&dlg);
    auto slider=[&](const QString& n){ auto* s=new QSlider(Qt::Horizontal); s->setRange(-100,100); s->setValue(0); auto* l=new QLabel("0");
        QObject::connect(s,&QSlider::valueChanged,l,[l](int v){ l->setText(QString::number(v)); }); auto* row=new QHBoxLayout; row->addWidget(s,1); row->addWidget(l); f->addRow(n,row); return s; };
    auto* bright=slider("Brightness"); auto* contrast=slider("Contrast"); auto* sat=slider("Saturation");
    auto* gray=new QCheckBox("Greyscale"); auto* redeye=new QCheckBox("Red-eye removal");
    { auto* hr=new QHBoxLayout; hr->addWidget(gray); hr->addWidget(redeye); hr->addStretch(); f->addRow(hr); }
    // crop margins (% of each edge) — the frame defines the crop window
    auto cropSlider=[&](const QString& n){ auto* s=new QSlider(Qt::Horizontal); s->setRange(0,45); s->setValue(0); auto* l=new QLabel("0%");
        QObject::connect(s,&QSlider::valueChanged,l,[l](int v){ l->setText(QString::number(v)+"%"); }); auto* row=new QHBoxLayout; row->addWidget(s,1); row->addWidget(l); f->addRow(n,row); return s; };
    auto* cL=cropSlider("Crop left"); auto* cR=cropSlider("Crop right"); auto* cT=cropSlider("Crop top"); auto* cB=cropSlider("Crop bottom");
    // spatial filters: blur radius, sharpen amount, vignette strength (0-based)
    auto posSlider=[&](const QString& n,int mx){ auto* s=new QSlider(Qt::Horizontal); s->setRange(0,mx); s->setValue(0); auto* l=new QLabel("0");
        QObject::connect(s,&QSlider::valueChanged,l,[l](int v){ l->setText(QString::number(v)); }); auto* row=new QHBoxLayout; row->addWidget(s,1); row->addWidget(l); f->addRow(n,row); return s; };
    auto* blurS=posSlider("Blur",20); auto* sharpS=posSlider("Sharpen",100); auto* vigS=posSlider("Vignette",100);
    auto* lensS=slider("Lens correction"); auto* hdrS=posSlider("HDR tone-map",100);   // lens: −barrel..+pincushion; HDR: highlight compression
    auto* tempS=slider("Temperature (cool−/warm+)"); auto* tintS=slider("Tint (green−/magenta+)");   // white balance
    bool autoLv=false;
    // Restore the object's stored (non-destructive) adjustment so the dialog re-opens where it left off.
    { const Fill::Adjust ia=sel->fill.adjust;
      bright->setValue(ia.brightness); contrast->setValue(ia.contrast); sat->setValue(ia.saturation);
      gray->setChecked(ia.gray); redeye->setChecked(ia.redeye);
      cL->setValue(ia.cropL); cR->setValue(ia.cropR); cT->setValue(ia.cropT); cB->setValue(ia.cropB);
      blurS->setValue(ia.blur); sharpS->setValue(ia.sharpen); vigS->setValue(ia.vignette); lensS->setValue(ia.lens); hdrS->setValue(ia.hdr);
      tempS->setValue(ia.temp); tintS->setValue(ia.tint); }
    auto rebake=[&]{
        Fill::Adjust a; a.brightness=bright->value(); a.contrast=contrast->value(); a.saturation=sat->value();
        a.gray=gray->isChecked(); a.redeye=redeye->isChecked(); a.blur=blurS->value(); a.sharpen=sharpS->value(); a.vignette=vigS->value();
        a.cropL=cL->value(); a.cropR=cR->value(); a.cropT=cT->value(); a.cropB=cB->value(); a.lens=lensS->value(); a.hdr=hdrS->value();
        a.temp=tempS->value(); a.tint=tintS->value();
        if(autoLv){ // Auto-enhance bakes auto-levels once (not a re-editable slider)
            QImage adj=adjustPhoto(orig,a.brightness,a.contrast,a.saturation,a.gray,true,a.redeye,a.blur,a.sharpen,a.vignette,a.lens,a.hdr);
            int W=adj.width(), H=adj.height();
            QRect crop(a.cropL*W/100, a.cropT*H/100, qMax(1,W-(a.cropL+a.cropR)*W/100), qMax(1,H-(a.cropT+a.cropB)*H/100));
            if(crop!=QRect(0,0,W,H)) adj=adj.copy(crop);
            canvas->applyToSelection([&](Shape& s){ if(s.fill.type==FillType::Bitmap){ s.fill.image=adj; s.fill.adjust=a; } },"Adjust photo");
        } else canvas->applyPhotoAdjust(a);   // non-destructive: re-bakes from source + stores the params
    };
    for(QSlider* s:{bright,contrast,sat,cL,cR,cT,cB,blurS,sharpS,vigS,lensS,hdrS}) QObject::connect(s,&QSlider::valueChanged,&dlg,[&]{ rebake(); });
    QObject::connect(gray,&QCheckBox::toggled,&dlg,[&]{ rebake(); });
    QObject::connect(redeye,&QCheckBox::toggled,&dlg,[&]{ rebake(); });
    auto* row=new QHBoxLayout;
    auto* enh=new QPushButton("Auto-enhance"); QObject::connect(enh,&QPushButton::clicked,&dlg,[&]{ autoLv=true; rebake(); });
    auto* reset=new QPushButton("Reset"); QObject::connect(reset,&QPushButton::clicked,&dlg,[&]{ autoLv=false; for(QSlider* s:{bright,contrast,sat,cL,cR,cT,cB,blurS,sharpS,vigS,lensS,hdrS}) s->setValue(0); gray->setChecked(false); redeye->setChecked(false); rebake(); });
    row->addWidget(enh); row->addWidget(reset); row->addStretch(); f->addRow(row);
    auto* bb=new QDialogButtonBox(QDialogButtonBox::Ok); QObject::connect(bb,&QDialogButtonBox::accepted,&dlg,&QDialog::accept); f->addRow(bb);
    dlg.exec();
}

// ============================================================================
// Levels: luminance histogram with draggable black / gamma / white markers.
// ============================================================================
namespace {
class LevelsHist : public QWidget {
public:
    int hist[256]; int hmax=1; int black=0, white=255; double gamma=1.0;
    std::function<void()> onChange;
    explicit LevelsHist(const QImage& img, QWidget* parent=nullptr): QWidget(parent){
        setMinimumSize(300,150); setMouseTracking(true);
        for(int i=0;i<256;++i) hist[i]=0;
        QImage s=img.convertToFormat(QImage::Format_ARGB32);
        for(int y=0;y<s.height();++y){ const QRgb* r=(const QRgb*)s.constScanLine(y);
            for(int x=0;x<s.width();++x){ QRgb p=r[x];
                int l=qBound(0,int(0.299*qRed(p)+0.587*qGreen(p)+0.114*qBlue(p)+0.5),255); ++hist[l]; } }
        for(int i=0;i<256;++i) hmax=qMax(hmax,hist[i]);
    }
private:
    int trackH() const { return 14; }
    int histH()  const { return height()-trackH()-2; }
    double midPos() const { return std::pow(0.5, qBound(0.1,gamma,9.99)); }  // input mapping to 0.5 output (LUT is out=t^(1/gamma))
    int xForV(double v) const { return int(v/255.0*(width()-1)); }
    void paintEvent(QPaintEvent*) override {
        QPainter g(this); g.fillRect(rect(), QColor(18,20,26));
        const int H=histH(), cw=qMax(1, width()/256);
        g.setPen(Qt::NoPen); g.setBrush(QColor(150,160,180));
        for(int i=0;i<256;++i){ int h=int(double(hist[i])/hmax*H); g.drawRect(xForV(i), H-h, cw, h); }
        g.fillRect(0,H,width(),trackH()+2, QColor(30,32,40));
        const double mp = black + midPos()*(white-black);
        auto tri=[&](int x,QColor c){ QPolygon p; p<<QPoint(x-5,H+2+trackH())<<QPoint(x+5,H+2+trackH())<<QPoint(x,H+2);
            g.setBrush(c); g.setPen(QColor(235,235,235)); g.drawPolygon(p); };
        tri(xForV(black),Qt::black); tri(xForV(mp),QColor(140,140,140)); tri(xForV(white),Qt::white);
    }
    void grab(const QPoint& pt){
        const double v=qBound(0.0,double(pt.x())/qMax(1,width()-1)*255.0,255.0);
        const double mp=black+midPos()*(white-black);
        const double db=qAbs(v-black), dm=qAbs(v-mp), dw=qAbs(v-white);
        if(db<=dm && db<=dw)      black=qBound(0,int(v+0.5),white-1);
        else if(dw<=dm)           white=qBound(black+1,int(v+0.5),255);
        else { double p=qBound(0.02,(v-black)/qMax(1.0,double(white-black)),0.98); gamma=qBound(0.1, std::log(p)/std::log(0.5), 9.99); }
        update(); if(onChange) onChange();
    }
    void mousePressEvent(QMouseEvent* e) override { grab(e->pos()); }
    void mouseMoveEvent(QMouseEvent* e) override { if(e->buttons()&Qt::LeftButton) grab(e->pos()); }
};

// Cutout Studio scribble surface: shows the photo at fit-size and lets the user paint KEEP (green)
// and REMOVE (red) hints into an ARGB seed overlay the graph-cut segmenter consumes.
class CutoutPaint : public QWidget {
public:
    QPixmap pm; QImage seed; int mode=1; double radius=16;   // mode 1=keep(green), 2=remove(red)
    explicit CutoutPaint(const QImage& photo, QWidget* parent=nullptr): QWidget(parent){
        QImage disp=photo;
        if(disp.width()>640 || disp.height()>500) disp=disp.scaled(640,500,Qt::KeepAspectRatio,Qt::SmoothTransformation);
        pm=QPixmap::fromImage(disp);
        seed=QImage(pm.size(),QImage::Format_ARGB32); seed.fill(Qt::transparent);
        setFixedSize(pm.size()); setCursor(Qt::CrossCursor);
    }
    void reset(){ seed.fill(Qt::transparent); update(); }
private:
    void dab(const QPointF& p){
        QPainter g(&seed); g.setPen(Qt::NoPen);
        g.setBrush(mode==1? QColor(0,220,0,255) : QColor(230,0,0,255));
        g.drawEllipse(p,radius,radius);
        update();
    }
    void paintEvent(QPaintEvent*) override {
        QPainter g(this); g.drawPixmap(0,0,pm);
        g.setOpacity(0.42); g.drawImage(0,0,seed);
    }
    void mousePressEvent(QMouseEvent* e) override { if(e->button()==Qt::LeftButton) dab(e->position()); }
    void mouseMoveEvent(QMouseEvent* e) override { if(e->buttons()&Qt::LeftButton) dab(e->position()); }
};
}

void LevelsDialog::show(QWidget* parent, Canvas* canvas){
    Shape* sel=canvas->firstSelected();
    if(!sel || sel->fill.type!=FillType::Bitmap || (sel->fill.image.isNull()&&sel->fill.origImage.isNull())){
        QMessageBox::information(parent,"Levels","Select a photo / bitmap object first."); return; }
    if(sel->fill.origImage.isNull()) canvas->applyToSelection([](Shape& s){ s.fill.origImage=s.fill.image; },"Photo source");
    const QImage orig = canvas->firstSelected()->fill.origImage;
    const Fill::Adjust base = canvas->firstSelected()->fill.adjust;   // preserve the object's other adjustments

    QDialog dlg(parent); dlg.setWindowTitle("Levels"); dlg.setModal(true);
    auto* v=new QVBoxLayout(&dlg);
    auto* hist=new LevelsHist(orig); hist->black=base.blackPt; hist->white=base.whitePt; hist->gamma=base.gamma;
    v->addWidget(hist);
    auto* row=new QHBoxLayout;
    auto* bSp=new QSpinBox; bSp->setRange(0,254); bSp->setValue(base.blackPt); bSp->setPrefix("Black ");
    auto* gSp=new QDoubleSpinBox; gSp->setRange(0.10,9.99); gSp->setSingleStep(0.05); gSp->setDecimals(2); gSp->setValue(base.gamma); gSp->setPrefix("Gamma ");
    auto* wSp=new QSpinBox; wSp->setRange(1,255); wSp->setValue(base.whitePt); wSp->setPrefix("White ");
    row->addWidget(bSp); row->addWidget(gSp); row->addWidget(wSp); v->addLayout(row);
    auto* prev=new QLabel; prev->setMinimumSize(300,150); prev->setAlignment(Qt::AlignCenter); prev->setStyleSheet("background:#111"); v->addWidget(prev);

    auto guard=std::make_shared<bool>(false);
    auto refresh=[=]{
        QImage a=adjustPhoto(orig,base.brightness,base.contrast,base.saturation,base.gray,false,base.redeye,
                             base.blur,base.sharpen,base.vignette,base.lens,base.hdr, hist->black,hist->white,hist->gamma);
        if(!a.isNull()) prev->setPixmap(QPixmap::fromImage(a.scaled(prev->width(),prev->height(),Qt::KeepAspectRatio,Qt::SmoothTransformation))); };
    auto syncFromHist=[=]{ *guard=true; bSp->setValue(hist->black); gSp->setValue(hist->gamma); wSp->setValue(hist->white); *guard=false; refresh(); };
    hist->onChange=syncFromHist;
    QObject::connect(bSp,QOverload<int>::of(&QSpinBox::valueChanged),&dlg,[=](int val){ if(*guard)return; hist->black=qMin(val,hist->white-1); hist->update(); refresh(); });
    QObject::connect(wSp,QOverload<int>::of(&QSpinBox::valueChanged),&dlg,[=](int val){ if(*guard)return; hist->white=qMax(val,hist->black+1); hist->update(); refresh(); });
    QObject::connect(gSp,QOverload<double>::of(&QDoubleSpinBox::valueChanged),&dlg,[=](double val){ if(*guard)return; hist->gamma=val; hist->update(); refresh(); });
    refresh();

    auto* bb=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Reset|QDialogButtonBox::Cancel); v->addWidget(bb);
    QObject::connect(bb->button(QDialogButtonBox::Reset),&QPushButton::clicked,&dlg,[=]{ hist->black=0; hist->white=255; hist->gamma=1.0; hist->update(); syncFromHist(); });
    QObject::connect(bb,&QDialogButtonBox::accepted,&dlg,&QDialog::accept);
    QObject::connect(bb,&QDialogButtonBox::rejected,&dlg,&QDialog::reject);
    if(dlg.exec()!=QDialog::Accepted) return;
    Fill::Adjust a=base; a.blackPt=hist->black; a.whitePt=hist->white; a.gamma=hist->gamma;
    canvas->applyPhotoAdjust(a);
}

void CutoutStudioDialog::show(QWidget* parent, Canvas* canvas){
    int si=-1; const auto sel=canvas->selection();
    for(int i:sel){ const Shape& s=canvas->doc().shapes[i];
        if(s.fill.type==FillType::Bitmap && !s.fill.image.isNull()){ si=i; break; } }
    if(si<0){ QMessageBox::information(parent,"Cutout Studio","Select a photo (bitmap object) first."); return; }
    const QImage photo=canvas->doc().shapes[si].fill.image;

    QDialog dlg(parent); dlg.setWindowTitle("Cutout Studio"); dlg.setModal(true);
    auto* v=new QVBoxLayout(&dlg);
    auto* help=new QLabel("Paint GREEN over what to KEEP, RED over what to REMOVE, then Apply. "
                          "A few strokes on each is enough — the cut snaps to the nearest edges.");
    help->setWordWrap(true); v->addWidget(help);
    auto* paint=new CutoutPaint(photo);
    auto* scroll=new QScrollArea; scroll->setWidget(paint); scroll->setAlignment(Qt::AlignCenter);
    scroll->setMinimumSize(qMin(660,paint->width()+24), qMin(520,paint->height()+24)); v->addWidget(scroll,1);
    auto* row=new QHBoxLayout;
    auto* keep=new QRadioButton("Keep (green)"); keep->setChecked(true);
    auto* rem =new QRadioButton("Remove (red)");
    QObject::connect(keep,&QRadioButton::toggled,&dlg,[=](bool on){ if(on) paint->mode=1; });
    QObject::connect(rem, &QRadioButton::toggled,&dlg,[=](bool on){ if(on) paint->mode=2; });
    auto* sz=new QSpinBox; sz->setRange(3,80); sz->setValue(16); sz->setPrefix("Brush "); sz->setSuffix(" px");
    QObject::connect(sz,QOverload<int>::of(&QSpinBox::valueChanged),&dlg,[=](int val){ paint->radius=val; });
    auto* rst=new QPushButton("Clear strokes");
    QObject::connect(rst,&QPushButton::clicked,&dlg,[=]{ paint->reset(); });
    row->addWidget(keep); row->addWidget(rem); row->addStretch(); row->addWidget(sz); row->addWidget(rst);
    v->addLayout(row);
    auto* bb=new QDialogButtonBox(QDialogButtonBox::Apply|QDialogButtonBox::Cancel);
    bb->button(QDialogButtonBox::Apply)->setText("Apply Cutout");
    v->addWidget(bb);
    QObject::connect(bb->button(QDialogButtonBox::Apply),&QPushButton::clicked,&dlg,&QDialog::accept);
    QObject::connect(bb,&QDialogButtonBox::rejected,&dlg,&QDialog::reject);
    if(dlg.exec()!=QDialog::Accepted) return;
    canvas->cutoutStudioApply(si, paint->seed);
}

void PerspectiveDialog::show(QWidget* parent, Canvas* canvas){
    int si=-1; const auto sel=canvas->selection();
    for(int i:sel){ const Shape& s=canvas->doc().shapes[i]; if(s.fill.type==FillType::Bitmap && !s.fill.image.isNull()){ si=i; break; } }
    if(si<0){ QMessageBox::information(parent,"Perspective Correction","Select a photo (bitmap object) first."); return; }
    const QImage full=canvas->doc().shapes[si].fill.image;
    QImage disp=full; if(disp.width()>520||disp.height()>420) disp=disp.scaled(520,420,Qt::KeepAspectRatio,Qt::SmoothTransformation);

    QDialog dlg(parent); dlg.setWindowTitle("Perspective / Keystone"); dlg.setModal(true);
    auto* v=new QVBoxLayout(&dlg);
    v->addWidget(new QLabel("Straighten converging verticals/horizontals in the photo. Drag the sliders until the edges look parallel."));
    auto* prev=new QLabel; prev->setAlignment(Qt::AlignCenter); prev->setMinimumSize(disp.width(),disp.height()); prev->setStyleSheet("background:#111"); v->addWidget(prev);
    auto* f=new QFormLayout;
    auto* vSl=new QSlider(Qt::Horizontal); vSl->setRange(-60,60); vSl->setValue(0);
    auto* hSl=new QSlider(Qt::Horizontal); hSl->setRange(-60,60); hSl->setValue(0);
    f->addRow("Vertical (top ↔ bottom)",vSl); f->addRow("Horizontal (left ↔ right)",hSl); v->addLayout(f);
    // Live preview: the same projective quad the canvas applies, on the display-size copy.
    auto refresh=[=]{ double kv=vSl->value()/100.0, kh=hSl->value()/100.0;
        if(double ks=qAbs(kv)+qAbs(kh); ks>0.85){ kv*=0.85/ks; kh*=0.85/ks; }   // same convexity clamp as correctPerspective
        const int W=disp.width(),H=disp.height(); const double hw=W/2.0,hh=H/2.0,cx=hw,cy=hh;
        QPolygonF s; s<<QPointF(0,0)<<QPointF(W,0)<<QPointF(W,H)<<QPointF(0,H);
        QPolygonF d; d<<QPointF(cx-hw*(1+kv),cy-hh*(1+kh))<<QPointF(cx+hw*(1+kv),cy-hh*(1-kh))<<QPointF(cx+hw*(1-kv),cy+hh*(1-kh))<<QPointF(cx-hw*(1-kv),cy+hh*(1+kh));
        QTransform t; QImage out(W,H,QImage::Format_ARGB32); out.fill(Qt::transparent);
        if(QTransform::quadToQuad(s,d,t)){ QPainter g(&out); g.setRenderHints(QPainter::SmoothPixmapTransform); g.setTransform(t); g.drawImage(0,0,disp); }
        prev->setPixmap(QPixmap::fromImage(out)); };
    QObject::connect(vSl,&QSlider::valueChanged,&dlg,[=]{ refresh(); });
    QObject::connect(hSl,&QSlider::valueChanged,&dlg,[=]{ refresh(); });
    refresh();
    auto* bb=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel); v->addWidget(bb);
    QObject::connect(bb,&QDialogButtonBox::accepted,&dlg,&QDialog::accept);
    QObject::connect(bb,&QDialogButtonBox::rejected,&dlg,&QDialog::reject);
    if(dlg.exec()!=QDialog::Accepted) return;
    canvas->correctPerspective(si, vSl->value()/100.0, hSl->value()/100.0);
}

void MeshGradientDialog::show(QWidget* parent, Canvas* canvas){
    int si=-1; const auto sel=canvas->selection();
    for(int i:sel){ const Shape& s=canvas->doc().shapes[i]; if(s.type!=ShapeType::Group && s.type!=ShapeType::Text){ si=i; break; } }
    if(si<0){ QMessageBox::information(parent,"Mesh Gradient","Select a shape (not a group or text) to fill with a mesh gradient."); return; }
    const Shape& s0=canvas->doc().shapes[si];

    auto seed=[](int r,int c, QColor tl,QColor tr,QColor bl,QColor br){          // bilinear grid from the 4 corners
        QVector<QColor> v(r*c);
        for(int y=0;y<r;++y){ double ty=r>1? double(y)/(r-1):0;
          for(int x=0;x<c;++x){ double tx=c>1? double(x)/(c-1):0;
            auto mix=[&](int a,int b,int cc,int d){ return int((1-ty)*((1-tx)*a+tx*b)+ty*((1-tx)*cc+tx*d)+0.5); };
            v[y*c+x]=QColor(mix(tl.red(),tr.red(),bl.red(),br.red()), mix(tl.green(),tr.green(),bl.green(),br.green()), mix(tl.blue(),tr.blue(),bl.blue(),br.blue())); } }
        return v; };
    int R=3, C=3; QVector<QColor> nodes;
    // Clamp rows/cols to the model's own [2,8] BEFORE any R*C multiply — a hand-edited .xar can carry huge
    // meshRows/meshCols that would overflow signed int (defeating the size guard) and blow up the widget grid.
    if(s0.fill.type==FillType::Mesh && s0.fill.meshRows>=2 && s0.fill.meshCols>=2){
        R=qBound(2, s0.fill.meshRows, 8); C=qBound(2, s0.fill.meshCols, 8);
        if(s0.fill.meshColors.size()>=R*C) nodes=s0.fill.meshColors.mid(0,R*C);
    }
    if(nodes.size()!=R*C) nodes=seed(R,C, QColor(70,130,220),QColor(110,200,170),QColor(230,120,150),QColor(250,220,120));

    QDialog dlg(parent); dlg.setWindowTitle("Mesh Gradient"); dlg.setModal(true);
    auto* v=new QVBoxLayout(&dlg);
    v->addWidget(new QLabel("A grid of colour nodes blended across the shape. Click any node to recolour it; add rows/columns for finer control."));
    auto* row=new QHBoxLayout;
    auto* rSp=new QSpinBox; rSp->setRange(2,8); rSp->setValue(R); rSp->setPrefix("Rows ");   // match the model's [2,8]
    auto* cSp=new QSpinBox; cSp->setRange(2,8); cSp->setValue(C); cSp->setPrefix("Cols ");
    row->addWidget(rSp); row->addWidget(cSp); row->addStretch(); v->addLayout(row);
    auto* gridW=new QWidget; auto* grid=new QGridLayout(gridW); grid->setSpacing(3); v->addWidget(gridW);

    std::function<void()> rebuild=[&]{
        QLayoutItem* it; while((it=grid->takeAt(0))){ delete it->widget(); delete it; }
        for(int y=0;y<R;++y) for(int x=0;x<C;++x){ const int idx=y*C+x;
            auto* b=new QToolButton; b->setFixedSize(42,30);
            b->setStyleSheet(QString("background:%1;border:1px solid #555").arg(nodes[idx].name()));
            QObject::connect(b,&QToolButton::clicked,&dlg,[b,idx,canvas,&nodes,&dlg]{ QColor c=ColorEditor::pick(&dlg, nodes[idx], canvas, false);
                if(c.isValid()){ nodes[idx]=c; b->setStyleSheet(QString("background:%1;border:1px solid #555").arg(c.name())); } });
            grid->addWidget(b,y,x); }
    };
    auto doResize=[&]{ const int nr=rSp->value(), nc=cSp->value();
        QColor tl=nodes[0], tr=nodes[C-1], bl=nodes[(R-1)*C], br=nodes[R*C-1];      // keep corners, re-seed interior
        R=nr; C=nc; nodes=seed(R,C,tl,tr,bl,br); rebuild(); dlg.adjustSize(); };
    QObject::connect(rSp,QOverload<int>::of(&QSpinBox::valueChanged),&dlg,[&]{ doResize(); });
    QObject::connect(cSp,QOverload<int>::of(&QSpinBox::valueChanged),&dlg,[&]{ doResize(); });
    rebuild();
    auto* bb=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel); v->addWidget(bb);
    QObject::connect(bb,&QDialogButtonBox::accepted,&dlg,&QDialog::accept);
    QObject::connect(bb,&QDialogButtonBox::rejected,&dlg,&QDialog::reject);
    if(dlg.exec()!=QDialog::Accepted) return;
    canvas->setSelectionMeshGrid(R, C, nodes);
}

void RecolourDialog::show(QWidget* parent, Canvas* canvas){
    if(canvas->selection().isEmpty()){ QMessageBox::information(parent,"Recolour","Select one or more objects first."); return; }
    QDialog dlg(parent); dlg.setWindowTitle("Recolour"); dlg.setModal(true);
    auto* f=new QFormLayout(&dlg);
    auto sl=[&](const QString& n,int lo,int hi,int v){ auto* s=new QSlider(Qt::Horizontal); s->setRange(lo,hi); s->setValue(v);
        auto* lab=new QLabel(QString::number(v)); QObject::connect(s,&QSlider::valueChanged,lab,[lab](int x){ lab->setText(QString::number(x)); });
        auto* row=new QHBoxLayout; row->addWidget(s,1); row->addWidget(lab); f->addRow(n,row); return s; };
    auto* hue=sl("Hue rotate °",-180,180,0); auto* sat=sl("Saturation %",0,200,100); auto* lig=sl("Lightness %",0,200,100);
    auto* applyBtn=new QPushButton("Apply hue / sat / lightness");
    QObject::connect(applyBtn,&QPushButton::clicked,&dlg,[=]{ canvas->recolorSelection(hue->value(), sat->value()/100.0, lig->value()/100.0); });
    f->addRow(applyBtn);
    // Replace colour: pick source + target
    auto* repBtn=new QPushButton("Replace a colour…");
    QObject::connect(repBtn,&QPushButton::clicked,&dlg,[=,&dlg]{
        QColor from=QColorDialog::getColor(Qt::red,&dlg,"Colour to replace"); if(!from.isValid()) return;
        QColor to=QColorDialog::getColor(Qt::blue,&dlg,"Replace with"); if(!to.isValid()) return;
        canvas->replaceColorInSelection(from,to,24); });
    f->addRow(repBtn);
    auto* bb=new QDialogButtonBox(QDialogButtonBox::Close); QObject::connect(bb,&QDialogButtonBox::rejected,&dlg,&QDialog::reject);
    QObject::connect(bb,&QDialogButtonBox::accepted,&dlg,&QDialog::accept); f->addRow(bb);
    dlg.exec();
}

// ============================================================================
// TextStyleDialog — cascading named text styles (Batch T1, manual "Text Styles")
// A style is created from the selected text object's attributes; applying it
// copies those attributes into the target and tags it; editing the style
// re-syncs every member (cascade). The render path never resolves styles, so
// XAR-loaded content is unaffected — the attributes are baked into shapes.
// ============================================================================
void TextStyleDialog::show(QWidget* parent, Canvas* canvas){
    QDialog dlg(parent); dlg.setWindowTitle("Text Styles"); dlg.setModal(true); dlg.resize(320,360);
    auto* V=new QVBoxLayout(&dlg);
    V->addWidget(new QLabel("Named text styles (create from a selected text object,\napply to the selection, or edit to cascade to all members):"));
    auto* list=new QListWidget; V->addWidget(list,1);
    auto refill=[=]{ list->clear(); for(const auto& t: canvas->doc().textStyles){
        auto* it=new QListWidgetItem(QString("%1  —  %2 %3pt").arg(t.name, t.font.family()).arg(t.font.pointSize()));
        it->setData(Qt::UserRole,t.name); it->setForeground(QBrush(t.color)); QFont lf=t.font; lf.setPointSize(11); it->setFont(lf);
        list->addItem(it); } };
    refill();
    auto currentName=[=]()->QString{ auto* it=list->currentItem(); return it? it->data(Qt::UserRole).toString():QString(); };
    auto styleByName=[=](const QString& n)->TextStyle{ for(const auto& t: canvas->doc().textStyles) if(t.name==n) return t; return TextStyle(); };

    auto* row=new QHBoxLayout;
    auto* nw=new QPushButton("New from selection…"); auto* ap=new QPushButton("Apply"); auto* ed=new QPushButton("Edit…");
    row->addWidget(nw); row->addWidget(ap); row->addWidget(ed); V->addLayout(row);

    QObject::connect(nw,&QPushButton::clicked,&dlg,[=,&dlg]{
        if(canvas->selection().isEmpty()){ QMessageBox::information(&dlg,"Text Styles","Select a text object first — the new style copies its font, colour and alignment."); return; }
        bool ok; QString n=QInputDialog::getText(&dlg,"New Text Style","Style name:",QLineEdit::Normal,QString("Style %1").arg(canvas->doc().textStyles.size()+1),&ok);
        if(ok && !n.trimmed().isEmpty()){ canvas->createTextStyle(n.trimmed()); refill();
            for(int i=0;i<list->count();++i) if(list->item(i)->data(Qt::UserRole).toString()==n.trimmed()) list->setCurrentRow(i); } });

    QObject::connect(ap,&QPushButton::clicked,&dlg,[=,&dlg]{
        QString n=currentName(); if(n.isEmpty()){ QMessageBox::information(&dlg,"Text Styles","Pick a style from the list, then Apply it to the selected text."); return; }
        canvas->applyTextStyle(n); });

    QObject::connect(ed,&QPushButton::clicked,&dlg,[=,&dlg]{
        QString n=currentName(); if(n.isEmpty()){ QMessageBox::information(&dlg,"Text Styles","Pick a style to edit."); return; }
        TextStyle st=styleByName(n);
        bool ok; QFont f=QFontDialog::getFont(&ok, st.font, &dlg, "Style font"); if(!ok) return;
        QColor col=ColorEditor::pick(&dlg, st.color, canvas); if(!col.isValid()) col=st.color;
        QStringList al={"Left","Centre","Right","Justify"};
        QString achoice=QInputDialog::getItem(&dlg,"Alignment","Paragraph alignment:",al,qBound(0,st.align,3),false,&ok);
        int ai = ok ? qMax(0,al.indexOf(achoice)) : st.align;
        canvas->updateTextStyle(n, f, col, qBound(0,ai,3)); refill(); });

    QObject::connect(list,&QListWidget::itemDoubleClicked,&dlg,[=]{ QString n=currentName(); if(!n.isEmpty()) canvas->applyTextStyle(n); });

    auto* bb=new QDialogButtonBox(QDialogButtonBox::Close); QObject::connect(bb,&QDialogButtonBox::rejected,&dlg,&QDialog::reject);
    QObject::connect(bb,&QDialogButtonBox::accepted,&dlg,&QDialog::accept); V->addWidget(bb);
    dlg.exec();
}

// ============================================================================
// RichTextDialog — per-character formatting (Batch T2). Pick a character range
// of the selected text object, then set a font and/or colour and/or hyperlink
// on just that range. Backed by Canvas::setTextRunAttr (materialise+coalesce).
// ============================================================================
void RichTextDialog::show(QWidget* parent, Canvas* canvas){
    Shape* s=canvas->firstSelected();
    if(!s || s->type!=ShapeType::Text){ QMessageBox::information(parent,"Character Formatting","Select a text object first."); return; }
    const QString txt=s->text; const int n=txt.size();
    QDialog dlg(parent); dlg.setWindowTitle("Character Formatting"); dlg.setModal(true); dlg.resize(440,320);
    auto* V=new QVBoxLayout(&dlg);
    V->addWidget(new QLabel("Select a character range of the text, then choose a font, colour and/or link to apply to just that span:"));
    // The text, with the current range highlighted live.
    auto* view=new QTextEdit; view->setReadOnly(true); view->setPlainText(txt); view->setMaximumHeight(90); V->addWidget(view);
    auto* f=new QFormLayout;
    auto* from=new QSpinBox; from->setRange(0,n); from->setValue(0);
    auto* to=new QSpinBox;   to->setRange(0,n); to->setValue(n);
    f->addRow("From character",from); f->addRow("To character",to);
    QFont chosenFont=s->font; bool fontSet=false;
    QColor chosenColor=(s->fill.type==FillType::Solid)?s->fill.color:QColor(Qt::black); bool colorSet=false;
    auto* fontBtn=new QPushButton("Font: (unchanged)");
    auto* colBtn=new QPushButton("Colour: (unchanged)");
    auto* link=new QLineEdit; link->setPlaceholderText("optional hyperlink URL for this span");
    f->addRow(fontBtn); f->addRow(colBtn); f->addRow("Link",link);
    // Per-run typography (Batch R3): baseline shift (super/subscript) + letter-spacing override.
    auto* baseSp=new QDoubleSpinBox; baseSp->setRange(-50,50); baseSp->setSingleStep(1); baseSp->setSuffix(" px  (+up / −down)");
    auto* trackSp=new QDoubleSpinBox; trackSp->setRange(-10,40); trackSp->setSingleStep(0.5); trackSp->setSuffix(" px");
    { QVector<double> bl=s->resolveBaselines(); if(!bl.isEmpty()) baseSp->setValue(bl[0]); }
    f->addRow("Baseline shift",baseSp); f->addRow("Letter-spacing",trackSp);
    V->addLayout(f);
    auto highlight=[=]{ int a=qMin(from->value(),to->value()), b=qMax(from->value(),to->value());
        QTextCursor c(view->document()); c.setPosition(qBound(0,a,n)); c.setPosition(qBound(0,b,n),QTextCursor::KeepAnchor);
        QList<QTextEdit::ExtraSelection> es; QTextEdit::ExtraSelection sel; sel.cursor=c;
        sel.format.setBackground(QColor(80,140,255,120)); es<<sel; view->setExtraSelections(es); };
    QObject::connect(from,qOverload<int>(&QSpinBox::valueChanged),&dlg,[=](int){ highlight(); });
    QObject::connect(to,qOverload<int>(&QSpinBox::valueChanged),&dlg,[=](int){ highlight(); });
    highlight();
    QObject::connect(fontBtn,&QPushButton::clicked,&dlg,[&,fontBtn]{ bool ok; QFont ff=QFontDialog::getFont(&ok,chosenFont,&dlg,"Span font");
        if(ok){ chosenFont=ff; fontSet=true; fontBtn->setText(QString("Font: %1 %2pt").arg(ff.family()).arg(ff.pointSize())); } });
    QObject::connect(colBtn,&QPushButton::clicked,&dlg,[&,colBtn]{ QColor cc=ColorEditor::pick(&dlg,chosenColor,canvas);
        if(cc.isValid()){ chosenColor=cc; colorSet=true; colBtn->setText(QString("Colour: %1").arg(cc.name())); } });
    auto* bb=new QDialogButtonBox(QDialogButtonBox::Apply|QDialogButtonBox::Close);
    QObject::connect(bb->button(QDialogButtonBox::Apply),&QPushButton::clicked,&dlg,[&]{
        int a=qMin(from->value(),to->value()), b=qMax(from->value(),to->value()); if(b<=a){ QMessageBox::information(&dlg,"Character Formatting","Choose a non-empty range."); return; }
        QString u=link->text().trimmed();
        if(fontSet||colorSet||!u.isEmpty()) canvas->setTextRunAttr(a, b-a, fontSet?&chosenFont:nullptr, colorSet?&chosenColor:nullptr, u.isEmpty()?nullptr:&u);
        canvas->setTextRunTypography(a, b-a, baseSp->value(), trackSp->value()); });
    QObject::connect(bb,&QDialogButtonBox::rejected,&dlg,&QDialog::reject);
    V->addWidget(bb);
    dlg.exec();
}

// ============================================================================
// AlignDialog / RepeatDialog (manual p99 alignment, p106 repeating objects)
// ============================================================================
void AlignDialog::show(QWidget* parent, Canvas* canvas){
    QDialog dlg(parent); dlg.setWindowTitle("Alignment"); dlg.setModal(true);
    auto* V=new QVBoxLayout(&dlg);
    auto* rel=new QComboBox; rel->addItems({"Within selection","To page"}); V->addWidget(rel);
    auto rowOf=[&](const QString& title,const QStringList& labels,std::function<void(int)> fn){
        auto* gb=new QGroupBox(title); auto* h=new QHBoxLayout(gb);
        for(int i=0;i<labels.size();++i){ auto* b=new QToolButton; b->setText(labels[i]); b->setAutoRaise(true); int mode=i+1;
            QObject::connect(b,&QToolButton::clicked,&dlg,[fn,mode]{ fn(mode); }); h->addWidget(b); }
        V->addWidget(gb); };
    rowOf("Horizontal",{"Left","Centre","Right"},[=](int m){ canvas->alignSelectionAdv(m,0,rel->currentIndex()==1); });
    rowOf("Vertical",{"Top","Middle","Bottom"},[=](int m){ canvas->alignSelectionAdv(0,m,rel->currentIndex()==1); });
    { auto* gb=new QGroupBox("Distribute"); auto* h=new QHBoxLayout(gb);
      auto* dh=new QToolButton; dh->setText("Horizontally"); dh->setAutoRaise(true); QObject::connect(dh,&QToolButton::clicked,&dlg,[=]{ canvas->distributeSelection(true); });
      auto* dv=new QToolButton; dv->setText("Vertically"); dv->setAutoRaise(true); QObject::connect(dv,&QToolButton::clicked,&dlg,[=]{ canvas->distributeSelection(false); });
      h->addWidget(dh); h->addWidget(dv); V->addWidget(gb); }
    auto* bb=new QDialogButtonBox(QDialogButtonBox::Close); QObject::connect(bb,&QDialogButtonBox::rejected,&dlg,&QDialog::accept); V->addWidget(bb);
    dlg.exec();
}

void RepeatDialog::show(QWidget* parent, Canvas* canvas){
    QDialog dlg(parent); dlg.setWindowTitle("Repeat Objects"); dlg.setModal(true);
    auto* f=new QFormLayout(&dlg);
    QRectF b=canvas->selectionBoundsDoc();
    auto* cols=new QSpinBox; cols->setRange(1,200); cols->setValue(3); f->addRow("Columns",cols);
    auto* rows=new QSpinBox; rows->setRange(1,200); rows->setValue(1); f->addRow("Rows",rows);
    auto* dx=new QDoubleSpinBox; dx->setRange(-10000,10000); dx->setValue(b.width()>0?b.width()+10:60); f->addRow("X spacing (px)",dx);
    auto* dy=new QDoubleSpinBox; dy->setRange(-10000,10000); dy->setValue(b.height()>0?b.height()+10:60); f->addRow("Y spacing (px)",dy);
    auto* bb=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);
    QObject::connect(bb,&QDialogButtonBox::accepted,&dlg,&QDialog::accept); QObject::connect(bb,&QDialogButtonBox::rejected,&dlg,&QDialog::reject);
    f->addRow(bb);
    if(dlg.exec()==QDialog::Accepted) canvas->repeatSelection(cols->value(),rows->value(),dx->value(),dy->value());
}

// ============================================================================
// SymbolDialog — insert a special character / symbol (manual p215)
// ============================================================================
void SymbolDialog::show(QWidget* parent, Canvas* canvas){
    QDialog dlg(parent); dlg.setWindowTitle("Insert Symbol"); dlg.setModal(true);
    auto* V=new QVBoxLayout(&dlg);
    static const QStringList syms={
        "←","→","↑","↓","↔","⇒","⇐","⇑","⇓","⟹","↺","↻",
        "±","×","÷","≠","≈","≤","≥","∞","∑","√","∫","π","°","µ","∆","∂","∏","∝","∈",
        "€","£","¥","¢","₿","₹","$","¤",
        "…","—","–","«","»","“","”","‘","’","•","·","§","¶","†","‡","©","®","™",
        "★","☆","♥","♦","♣","♠","✓","✗","✦","❖","➤","➔","☀","☁","☂","☎","✉","✂","✏",
        "α","β","γ","δ","ε","θ","λ","μ","ξ","π","σ","φ","ψ","ω","Ω","Σ","Φ","Δ" };
    // Insert `s`: at the caret if a text object is being edited, else onto the selected
    // text object, else as a new text object. `keepOpen` lets invisible-glyph buttons stay put.
    auto insert=[canvas,&dlg](const QString& s, bool keepOpen){
        if(canvas->editingText()) canvas->textInsert(s);
        else { Shape* sel=canvas->firstSelected();
            if(sel && sel->type==ShapeType::Text) canvas->applyToSelection([s](Shape& sh){ if(sh.type==ShapeType::Text) sh.text+=s; },"Insert symbol");
            else { Shape t; t.type=ShapeType::Text; t.text=s; t.font=QFont("DejaVu Sans",48); t.fill.type=FillType::Solid; t.fill.color=Qt::black; t.stroke.enabled=false;
                t.transform=QTransform::fromTranslate(canvas->pageSize().width()/2-30, canvas->pageSize().height()/2); canvas->addShapes({t},"Insert symbol"); } }
        if(!keepOpen) dlg.accept(); };
    auto* grid=new QGridLayout; grid->setSpacing(2); V->addLayout(grid);
    int col=0,row=0; for(const QString& s: syms){ auto* b=new QToolButton; b->setText(s); b->setFixedSize(30,30);
        QFont f=b->font(); f.setPointSize(13); b->setFont(f);
        QObject::connect(b,&QToolButton::clicked,&dlg,[=]{ insert(s,false); });
        grid->addWidget(b,row,col); if(++col>=12){col=0;row++;} }
    // Named invisible / formatting characters (manual p214): stay open so you can add several.
    auto* named=new QHBoxLayout; V->addLayout(named);
    struct NC{ const char* label; ushort cp; } ncs[]={ {"Non-break space",0x00A0},{"Soft hyphen",0x00AD},{"Non-break hyphen",0x2011},{"Em space",0x2003},{"En space",0x2002},{"Thin space",0x2009},{"Zero-width",0x200B} };
    for(const NC& nc: ncs){ auto* b=new QPushButton(nc.label); ushort cp=nc.cp; QObject::connect(b,&QPushButton::clicked,&dlg,[=]{ insert(QString(QChar(cp)),true); }); named->addWidget(b); }
    auto* bb=new QDialogButtonBox(QDialogButtonBox::Close); QObject::connect(bb,&QDialogButtonBox::rejected,&dlg,&QDialog::reject); V->addWidget(bb);
    dlg.exec();
}

// ============================================================================
// FindReplace — search/replace across text objects (manual p232)
// ============================================================================
void FindReplace::show(QWidget* parent, Canvas* canvas){
    QDialog dlg(parent); dlg.setWindowTitle("Find & Replace"); dlg.setModal(false);
    auto* f=new QFormLayout(&dlg);
    auto* findEd=new QLineEdit; auto* replEd=new QLineEdit;
    auto* caseCb=new QCheckBox("Match case");
    auto* wordCb=new QCheckBox("Whole word only");
    f->addRow("Find",findEd); f->addRow("Replace",replEd); f->addRow(caseCb); f->addRow(wordCb);
    auto* status=new QLabel; status->setStyleSheet("color:#9aa;"); f->addRow(status);
    // Build a matcher honouring both toggles. Whole-word uses \b boundaries on the escaped needle.
    auto rx=[=](const QString& q){ QString pat=wordCb->isChecked()? ("\\b"+QRegularExpression::escape(q)+"\\b")
                                                                  : QRegularExpression::escape(q);
        return QRegularExpression(pat, caseCb->isChecked()?QRegularExpression::NoPatternOption:QRegularExpression::CaseInsensitiveOption); };
    auto contains=[=](const QString& hay, const QString& q){ return rx(q).match(hay).hasMatch(); };
    auto countIn =[=](const QString& hay, const QString& q){ int c=0; auto it=rx(q).globalMatch(hay); while(it.hasNext()){ it.next(); ++c; } return c; };
    auto* findBtn=new QPushButton("Find Next"); auto* replBtn=new QPushButton("Replace"); auto* allBtn=new QPushButton("Replace All"); auto* closeBtn=new QPushButton("Close");
    { auto* bb=new QDialogButtonBox;   // roles only place the buttons in the footer (Close left, Find Next rightmost); each is wired below
      bb->addButton(replBtn,QDialogButtonBox::ActionRole); bb->addButton(allBtn,QDialogButtonBox::ActionRole);
      bb->addButton(findBtn,QDialogButtonBox::AcceptRole); bb->addButton(closeBtn,QDialogButtonBox::RejectRole); f->addRow(bb); }

    static int findFrom=0;
    auto findNext=[=,&dlg]{ QString q=findEd->text(); if(q.isEmpty())return; auto& sh=canvas->doc().shapes; int n=sh.size();
        for(int k=1;k<=n;++k){ int i=(findFrom+k)%n; if(sh[i].type==ShapeType::Text && contains(sh[i].text,q)){ findFrom=i; canvas->select({i}); status->setText(QString("Found in “%1”").arg(sh[i].name.isEmpty()?"text":sh[i].name)); return; } }
        status->setText("Not found"); };
    QObject::connect(findBtn,&QPushButton::clicked,&dlg,[=]{ findNext(); });
    QObject::connect(replBtn,&QPushButton::clicked,&dlg,[=]{ QString q=findEd->text(); if(q.isEmpty())return; auto sel=canvas->selection();
        if(!sel.isEmpty()){ int i=sel.first(); auto& sh=canvas->doc().shapes; if(i<sh.size() && sh[i].type==ShapeType::Text && contains(sh[i].text,q)){
            canvas->pushUndo("Replace"); sh[i].text.replace(rx(q),replEd->text()); canvas->refreshAll(); status->setText("Replaced"); } } });
    QObject::connect(allBtn,&QPushButton::clicked,&dlg,[=]{ QString q=findEd->text(); if(q.isEmpty())return; int cnt=0; canvas->pushUndo("Replace all");
        for(auto& s: canvas->doc().shapes){ if(s.type==ShapeType::Text){ int before=countIn(s.text,q); if(before>0){ s.text.replace(rx(q),replEd->text()); cnt+=before; } } }
        canvas->refreshAll(); status->setText(QString("Replaced %1 occurrence(s)").arg(cnt)); });
    QObject::connect(closeBtn,&QPushButton::clicked,&dlg,&QDialog::accept);
    dlg.exec();
}

} // namespace xara
