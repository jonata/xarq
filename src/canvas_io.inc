// canvas_io.inc — persistence and export (included by canvas.cpp).
#include <QSvgGenerator>
#include <QPdfWriter>
#include <QPageSize>
#include <QFileInfo>
#include <QFile>
#include <QTextDocument>
#include <zlib.h>
#include "xar.h"
#include "gif.h"

// Sanitise a hyperlink before writing it into exported HTML/SVG: block script-bearing schemes
// (javascript:/vbscript:/data:/file:), tolerating whitespace/control chars used to hide them.
// Prevents a malicious document's link from becoming stored XSS in a published page.
static QString safeHref(const QString& u){
    QString t=u.trimmed(); QString low=t; low.remove(QRegularExpression("[\\x00-\\x20]")); low=low.toLower();
    if(low.startsWith("javascript:")||low.startsWith("vbscript:")||low.startsWith("data:")||low.startsWith("file:")) return QStringLiteral("#");
    return t;
}

// Native format is XAR (the real Xara binary format) — see xar.cpp.
bool Canvas::save(const QString& path) {
    if(!xara::saveXar(m_doc, path)) return false;
    m_path=path; return true;
}
bool Canvas::load(const QString& path) {
    Document d; d.pageSize=m_doc.pageSize;
    if(!xara::loadXar(d, path)) return false;
    setDocument(d, path); zoomFit(); return true;
}
// Write the current document to `path` WITHOUT retargeting the file path — used for autosave/recovery
// snapshots, which must not hijack the tab's real file identity.
bool Canvas::saveSnapshot(const QString& path) const { return xara::saveXar(m_doc, path); }
bool Canvas::saveCompressed(const QString& path) { if(!xara::saveXar(m_doc, path, true)) return false; m_path=path; return true; }

bool Canvas::exportPng(const QString& path, int scale) {
    reflowText();
    QRectF b = m_doc.contentBounds(); if(b.isNull()) b=QRectF(QPointF(0,0),m_doc.pageSize);
    b.adjust(-8,-8,8,8);
    if(qEnvironmentVariableIsSet("XARA_PAGEEXPORT")){ b=QRectF(QPointF(0,0),m_doc.pageSize); scale=1; }  // diag: full-page @1x for oracle compare
    QImage img(QSize(int(b.width()*scale), int(b.height()*scale)), QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter g(&img); g.setRenderHint(QPainter::Antialiasing); g.setRenderHint(QPainter::SmoothPixmapTransform);
    g.scale(scale,scale); g.translate(-b.topLeft());
    if(hasTextFields()||hasTextChains()){ Document rd=resolvedForRender(); paintDocument(g, rd); } else paintDocument(g, m_doc);
    g.end();
    return img.save(path);
}

// Bitmap Export with options (the Export dialog's engine): area (0 Page / 1 Drawing / 2 Selection),
// an output scale, JPEG quality (-1 = default), and a transparent-vs-white-matte background.
bool Canvas::exportRaster(const QString& path, int areaMode, double scale, int quality, bool transparent, bool indexed, bool dither){
    reflowText();
    QRectF b;
    if(areaMode==2){ b=selectionBoundsDoc(); if(b.isNull()) b=m_doc.contentBounds(); }         // Selection
    else if(areaMode==1){ b=m_doc.contentBounds(); if(!b.isNull()) b.adjust(-8,-8,8,8); }      // Drawing (content)
    else b=QRectF(QPointF(0,0), m_doc.pageSize);                                                // Page
    if(b.isNull() || b.width()<1 || b.height()<1) b=QRectF(QPointF(0,0), m_doc.pageSize);
    if(scale<=0) scale=1;
    int W=qMax(1,int(b.width()*scale)), H=qMax(1,int(b.height()*scale));
    const qint64 cap=64LL*1024*1024;                                                            // pixel cap → avoid runaway allocations
    if(qint64(W)*H>cap){ double f=std::sqrt(double(cap)/(double(W)*double(H))); W=qMax(1,int(W*f)); H=qMax(1,int(H*f)); }
    QImage img(QSize(W,H), QImage::Format_ARGB32_Premultiplied);
    img.fill(transparent? Qt::transparent : Qt::white);
    QPainter g(&img); g.setRenderHint(QPainter::Antialiasing); g.setRenderHint(QPainter::SmoothPixmapTransform);
    g.scale(double(W)/b.width(), double(H)/b.height()); g.translate(-b.topLeft());
    if(hasTextFields()||hasTextChains()){ Document rd=resolvedForRender(); paintDocument(g, rd); } else paintDocument(g, m_doc);
    g.end();
    const bool jpeg = path.endsWith(".jpg",Qt::CaseInsensitive)||path.endsWith(".jpeg",Qt::CaseInsensitive);
    QImage out = (transparent && !jpeg)? img : img.convertToFormat(QImage::Format_RGB32);       // JPEG has no alpha → always matte
    if(indexed && !jpeg)   // optimiser: reduce to a ≤256-colour indexed palette (PNG-8/GIF), with optional Floyd–Steinberg dithering
        out = out.convertToFormat(QImage::Format_Indexed8, Qt::AutoColor | (dither? Qt::DiffuseDither : Qt::ThresholdDither) | (dither?Qt::PreferDither:Qt::AvoidDither));
    if(jpeg && quality>=0) return out.save(path, nullptr, qBound(1,quality,100));
    return out.save(path);
}

void Canvas::renderDocument(QPainter& g) const { paintDocument(g, m_doc); }

// ---- EPUB import ----------------------------------------------------------------------------------
// Read a ZIP archive (EPUB is a ZIP) via its central directory → name→uncompressed-bytes. Written
// defensively (bounds-checked, size-capped) so a truncated/hostile file returns {} instead of crashing.
static QMap<QString,QByteArray> readZipEntries(const QByteArray& z){
    QMap<QString,QByteArray> out; const int n=z.size(); const uchar* p=(const uchar*)z.constData();
    auto u16=[&](int o)->int{ return (o>=0 && o+1<n)? (p[o] | (p[o+1]<<8)) : 0; };
    auto u32=[&](int o)->quint32{ return (o>=0 && o+3<n)? (quint32)(p[o] | (p[o+1]<<8) | (p[o+2]<<16) | ((quint32)p[o+3]<<24)) : 0u; };
    int eocd=-1; for(int i=n-22; i>=0 && i>n-22-65536; --i){ if(u32(i)==0x06054b50u){ eocd=i; break; } }
    if(eocd<0) return out;
    quint32 cdOff=u32(eocd+16); int cnt=u16(eocd+10);
    if(cdOff>=(quint32)n) return out;
    int pos=(int)cdOff;
    for(int e=0; e<cnt && pos>=0 && pos+46<=n; ++e){
        if(u32(pos)!=0x02014b50u) break;
        const int method=u16(pos+10);
        const quint32 comp=u32(pos+20), uncomp=u32(pos+24);
        const int nameLen=u16(pos+28), extraLen=u16(pos+30), cmtLen=u16(pos+32);
        const quint32 lho=u32(pos+42);
        const QString name=QString::fromUtf8(z.constData()+pos+46, qMin(nameLen, n-(pos+46)));
        pos += 46 + nameLen + extraLen + cmtLen;
        if(lho+30>(quint32)n) continue;
        const int dataOff=(int)lho + 30 + u16(lho+26) + u16(lho+28);
        if(dataOff<0 || comp>64u*1024*1024 || dataOff+(int)comp>n) continue;
        QByteArray raw(z.constData()+dataOff, (int)comp), dec;
        if(method==0) dec=raw;
        else if(method==8){
            if(uncomp==0 || uncomp>64u*1024*1024) continue;
            dec.resize((int)uncomp);
            z_stream zs; memset(&zs,0,sizeof(zs));
            if(inflateInit2(&zs,-15)!=Z_OK) continue;
            zs.next_in=(Bytef*)raw.constData(); zs.avail_in=raw.size();
            zs.next_out=(Bytef*)dec.data(); zs.avail_out=uncomp;
            const int ret=inflate(&zs,Z_FINISH); inflateEnd(&zs);
            if(ret!=Z_STREAM_END && ret!=Z_OK){ continue; }
            dec.resize((int)(uncomp - zs.avail_out));
        } else continue;
        out.insert(name, dec);
    }
    return out;
}
// Import an EPUB e-book: locate its OPF via META-INF/container.xml, follow the spine, and turn each
// chapter's XHTML into an editable text frame on its own page. Images/CSS are not laid out (text only).
bool Canvas::importEpub(const QString& path){
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray data=f.readAll(); f.close();
    const QMap<QString,QByteArray> zip=readZipEntries(data);
    if(zip.isEmpty()) return false;
    QString opfPath;
    { const QString cx=QString::fromUtf8(zip.value("META-INF/container.xml"));
      auto m=QRegularExpression("full-path=\"([^\"]+)\"").match(cx); if(m.hasMatch()) opfPath=m.captured(1); }
    if(opfPath.isEmpty()) for(auto it=zip.begin(); it!=zip.end(); ++it) if(it.key().endsWith(".opf",Qt::CaseInsensitive)){ opfPath=it.key(); break; }
    if(opfPath.isEmpty() || !zip.contains(opfPath)) return false;
    const QString opfDir = opfPath.contains('/')? opfPath.left(opfPath.lastIndexOf('/')+1) : QString();
    const QString opf=QString::fromUtf8(zip.value(opfPath));
    // manifest: id → href (attribute order varies, so match each <item …> tag then pull id/href out of it)
    QMap<QString,QString> manifest;
    { auto g=QRegularExpression("<item\\b[^>]*>").globalMatch(opf);
      while(g.hasNext()){ const QString tag=g.next().captured(0);
        auto mi=QRegularExpression("\\bid=\"([^\"]+)\"").match(tag), mh=QRegularExpression("\\bhref=\"([^\"]+)\"").match(tag);
        if(mi.hasMatch() && mh.hasMatch()) manifest.insert(mi.captured(1), mh.captured(1)); } }
    // spine gives reading order
    QStringList chapters;
    { auto g=QRegularExpression("<itemref\\b[^>]*\\bidref=\"([^\"]+)\"").globalMatch(opf);
      while(g.hasNext()){ const QString h=manifest.value(g.next().captured(1)); if(!h.isEmpty()) chapters<<h; } }
    if(chapters.isEmpty()) for(const QString& h: manifest.values()) if(h.endsWith(".xhtml",Qt::CaseInsensitive)||h.endsWith(".html",Qt::CaseInsensitive)||h.endsWith(".htm",Qt::CaseInsensitive)) chapters<<h;
    if(chapters.isEmpty()) return false;
    Document d; d.pageSize=QSizeF(794,1123);                       // A4-ish portrait
    const double m=60, adv=d.pageSize.height()+d.pageGap;
    QVector<Shape> shapes; int page=0;
    for(const QString& ch: chapters){ if(page>=500) break;
        QString key=opfDir+ch; int hash=key.indexOf('#'); if(hash>=0) key=key.left(hash);
        const QByteArray xhtml=zip.value(key); if(xhtml.isEmpty()) continue;
        QTextDocument td; td.setHtml(QString::fromUtf8(xhtml));
        QString text=td.toPlainText().trimmed(); if(text.size()>200000) text=text.left(200000);
        if(text.isEmpty()) continue;
        Shape t; t.type=ShapeType::Text; t.textFrame=true; t.text=text;
        t.font=QFont("DejaVu Sans",12); t.fill.type=FillType::Solid; t.fill.color=Qt::black; t.stroke.enabled=false;
        t.rect=QRectF(0,0,d.pageSize.width()-2*m, d.pageSize.height()-2*m);
        t.transform=QTransform::fromTranslate(m, m + page*adv);
        shapes<<t; ++page;
    }
    if(shapes.isEmpty()) return false;
    d.pageCount=page; d.shapes=shapes;
    setDocument(d); reflowText(); zoomFit();
    emit statusMessage(QString("Imported EPUB: %1 chapter page(s) as editable text.").arg(page));
    return true;
}

bool Canvas::importImage(const QString& path) {
    if(path.endsWith(".psd",Qt::CaseInsensitive)){
        QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false;
        Document pd=importPsdDocument(f.readAll()); if(pd.shapes.isEmpty()) return false;
        QRectF b; for(const Shape& s: pd.shapes) b = b.isNull()? s.docBounds() : b.united(s.docBounds());
        if(b.isNull()||b.width()<1||b.height()<1) return false;
        const double maxW=m_doc.pageSize.width()*0.9, maxH=m_doc.pageSize.height()*0.9;
        const double sc=qMin(1.0, qMin(maxW/b.width(), maxH/b.height()));
        const QPointF pc(m_doc.pageSize.width()/2, m_doc.pageSize.height()/2), bc=b.center();
        QTransform place; place.translate(pc.x(),pc.y()); place.scale(sc,sc); place.translate(-bc.x(),-bc.y());
        if(pd.shapes.size()>1){ Shape g; g.type=ShapeType::Group; g.name=QFileInfo(path).fileName(); g.children=pd.shapes; g.transform=place;
            addShapes({g}, "Import PSD"); }
        else { Shape s=pd.shapes[0]; s.name=QFileInfo(path).fileName(); s.transform = s.transform * place; addShapes({s}, "Import PSD"); }
        return true;
    }
    QImage img = (xara::Raw::isRawFile(path) && xara::Raw::available()) ? xara::Raw::decode(path) : QImage(path);
    if(img.isNull()) return false;
    // EXIF orientation: auto-rotate/mirror a camera photo to its intended upright form (every mainstream
    // viewer does this; the tag was decoded but ignored before).
    { const QString o=readExif(path).value("Orientation");
      if     (o=="Mirror H")     img=img.mirrored(true,false);
      else if(o=="Rotate 180")   img=img.mirrored(true,true);
      else if(o=="Mirror V")     img=img.mirrored(false,true);
      else if(o=="Rotate 90CW")  { QTransform t; t.rotate(90);  img=img.transformed(t,Qt::SmoothTransformation); }
      else if(o=="Rotate 90CCW") { QTransform t; t.rotate(270); img=img.transformed(t,Qt::SmoothTransformation); }
      else if(o=="Mirror+90CW")  { QTransform t; t.rotate(90);  img=img.mirrored(true,false).transformed(t,Qt::SmoothTransformation); }
      else if(o=="Mirror+90CCW") { QTransform t; t.rotate(270); img=img.mirrored(true,false).transformed(t,Qt::SmoothTransformation); } }
    double W=img.width(), H=img.height();
    double maxW=m_doc.pageSize.width()*0.9, maxH=m_doc.pageSize.height()*0.9;
    double sc=qMin(1.0, qMin(maxW/W, maxH/H));        // scale down to fit the page
    Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,W,H); s.name=QFileInfo(path).fileName();
    s.fill.type=FillType::Bitmap; s.fill.image=img; s.fill.origImage=img; s.fill.tiled=false;
    s.fill.p0=QPointF(0,H); s.fill.p1=QPointF(W,H); s.fill.p2=QPointF(0,0);   // image spans the rect (no tiling)
    s.stroke.enabled=false;
    s.transform = QTransform::fromScale(sc,sc) * QTransform::fromTranslate((m_doc.pageSize.width()-W*sc)/2,(m_doc.pageSize.height()-H*sc)/2);
    addShapes({s},"Import image");
    return true;
}

// Import an animated GIF: one layer (frame) per GIF frame, each holding a full-frame Bitmap. The
// existing GIF export (one layer = one frame) is the inverse, so a round-trip preserves the animation.
bool Canvas::importAnimatedGif(const QString& path) {
    // QMovie is the canonical animation reader (jumpToFrame + currentImage), more robust than
    // QImageReader for multi-frame GIFs.
    // Bounded to defend against decompression bombs (a small GIF with a huge logical screen and many
    // delta frames composites to gigabytes): cap the frame count and the cumulative decoded pixels.
    static const int    MAX_FRAMES = 512;
    static const qint64 MAX_PIXELS = 256LL*1024*1024;     // ~1 GB of ARGB32 across all held frames
    QMovie mov(path); QVector<QImage> frames; QVector<int> perFrameMs;
    if(mov.isValid() && mov.frameCount()>1){
        qint64 pixels=0;
        for(int i=0;i<mov.frameCount() && frames.size()<MAX_FRAMES;++i){ if(!mov.jumpToFrame(i)) break; QImage f=mov.currentImage();
            if(f.isNull()) break;
            pixels += qint64(f.width())*f.height(); if(pixels>MAX_PIXELS){ emit statusMessage("Animated GIF too large — truncated for import."); break; }
            frames.push_back(f.convertToFormat(QImage::Format_ARGB32));
            const int d=mov.nextFrameDelay(); perFrameMs.push_back(d>0?qMax(20,d):100); }
    }
    if(frames.size()<2) return importImage(path);              // still image / single frame
    pushUndo("Import animated GIF");
    const double W=frames[0].width(), H=frames[0].height();
    const double maxW=m_doc.pageSize.width()*0.9, maxH=m_doc.pageSize.height()*0.9;
    const double sc=qMin(1.0, qMin(maxW/W, maxH/H));
    const QTransform place = QTransform::fromScale(sc,sc) * QTransform::fromTranslate((m_doc.pageSize.width()-W*sc)/2,(m_doc.pageSize.height()-H*sc)/2);
    m_doc.layers.clear();
    const QString base=QFileInfo(path).completeBaseName();
    for(int i=0;i<frames.size();++i){
        Layer L; L.name=QString("%1 %2").arg(base).arg(i+1); L.visible=(i==0); L.frameDelayMs=(i<perFrameMs.size()?perFrameMs[i]:100); m_doc.layers.push_back(L);
        Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,W,H); s.name=L.name; s.layer=i;
        s.fill.type=FillType::Bitmap; s.fill.image=frames[i]; s.fill.origImage=frames[i]; s.fill.tiled=false;
        s.fill.p0=QPointF(0,H); s.fill.p1=QPointF(W,H); s.fill.p2=QPointF(0,0);
        s.stroke.enabled=false; s.transform=place; m_doc.shapes.push_back(s);
    }
    m_sel.clear(); emit documentChanged(); emit selectionChanged(); update();
    return true;
}

// Saddle-stitch booklet imposition: pad the page count up to a multiple of 4 and pair pages so that,
// printed 2-up double-sided and folded down the middle, they read in order. Each PDF page is one printed
// SIDE (landscape, two document pages side by side); blank slots (past the doc's real page count) stay empty.
// Poster / tiled print: a page larger than the output sheet is split across a grid of sheets, each printed
// at 1:1 with an `overlap` glue margin, as one multi-page PDF (one sheet per page). Assemble by hand.
bool Canvas::exportPosterPdf(const QString& path, double sheetW, double sheetH, double overlap){
    reflowText();
    const double PW=m_doc.pageSize.width(), PH=m_doc.pageSize.height();
    if(sheetW<10||sheetH<10) return false;
    overlap=qBound(0.0, overlap, qMin(sheetW,sheetH)*0.5-1);
    const double stepX=qMax(1.0,sheetW-overlap), stepY=qMax(1.0,sheetH-overlap);
    const int cols=qMax(1,int(std::ceil((PW-overlap)/stepX))), rows=qMax(1,int(std::ceil((PH-overlap)/stepY)));
    QPdfWriter pdf(path); pdf.setResolution(96);
    pdf.setPageSize(QPageSize(QSizeF(sheetW/96.0*25.4, sheetH/96.0*25.4), QPageSize::Millimeter, "Sheet", QPageSize::ExactMatch));
    pdf.setPageMargins(QMarginsF(0,0,0,0)); pdf.setCreator("XarQ"); pdf.setTitle(QFileInfo(path).completeBaseName());
    QPainter g(&pdf); if(!g.isActive()) return false;
    g.setRenderHint(QPainter::Antialiasing); g.setRenderHint(QPainter::SmoothPixmapTransform);
    bool first=true;
    for(int r=0;r<rows;++r) for(int c=0;c<cols;++c){
        if(!first) pdf.newPage(); first=false;
        g.save();
        g.setClipRect(QRectF(0,0,sheetW,sheetH));
        g.translate(-c*stepX, -r*stepY);                         // this tile's top-left → the sheet origin
        g.setClipRect(QRectF(0,0,PW,PH), Qt::IntersectClip);     // don't bleed adjacent stacked pages into the poster
        paintDocument(g, m_doc);
        g.restore();
        // Faint tile label + trim guides so the sheets can be aligned when assembling.
        g.save(); g.setPen(QPen(QColor(150,150,150),0.5,Qt::DashLine)); g.setBrush(Qt::NoBrush);
        g.drawRect(QRectF(0.5,0.5,sheetW-1,sheetH-1));
        g.setPen(QColor(120,120,120)); QFont f=g.font(); f.setPointSizeF(7); g.setFont(f);
        g.drawText(QRectF(4,4,sheetW-8,14), Qt::AlignLeft, QString("Sheet r%1 c%2  (overlap %3px)").arg(r+1).arg(c+1).arg(int(overlap)));
        g.restore();
    }
    g.end();
    emit statusMessage(QString("Poster: %1×%2 = %3 sheets.").arg(cols).arg(rows).arg(cols*rows));
    return true;
}
bool Canvas::exportBooklet(const QString& path) {
    reflowText();
    const int real = qMax(1, m_doc.pageCount);
    int P = ((real + 3) / 4) * 4;                    // pad to a multiple of 4
    // Booklet slot order: (P,1),(2,P-1),(P-2,3),(4,P-3),...  — each consecutive pair = one printed side.
    QVector<int> order; { int lo=1, hi=P;
        while(lo<hi){ order<<hi<<lo; ++lo; --hi; order<<lo<<hi; ++lo; --hi; } }
    const QRectF p0=m_doc.pageRect(0); const double pw=p0.width(), ph=p0.height();
    QPdfWriter pdf(path); pdf.setResolution(96);
    pdf.setPageSize(QPageSize(QSizeF((2*pw)/96.0*25.4, ph/96.0*25.4), QPageSize::Millimeter, "Sheet", QPageSize::ExactMatch));
    pdf.setPageMargins(QMarginsF(0,0,0,0)); pdf.setCreator("XarQ"); pdf.setTitle(QFileInfo(path).completeBaseName()+" booklet");
    QPainter g(&pdf); g.setRenderHint(QPainter::Antialiasing); g.setRenderHint(QPainter::SmoothPixmapTransform);
    bool first=true;
    for(int i=0;i+1<order.size();i+=2){
        if(!first) pdf.newPage(); first=false;
        for(int half=0; half<2; ++half){
            const int pageNo=order[i+half]; if(pageNo<1 || pageNo>real) continue;   // blank slot
            const QRectF pr=m_doc.pageRect(pageNo-1);
            g.save(); g.translate(half*pw, 0);
            g.fillRect(QRectF(0,0,pw,ph), m_doc.pageColor.isValid()?m_doc.pageColor:QColor(Qt::white));
            g.setClipRect(QRectF(0,0,pw,ph)); g.translate(-pr.left(),-pr.top()); paintDocument(g, m_doc); g.restore();
        }
    }
    g.end(); return true;
}

// N-up imposition: tile the document's pages onto one sheet (cols×rows cells), each page scaled to
// fit its cell. Used for proof sheets / booklet-style print layouts (manual "imposition").
QImage Canvas::renderImposedSheet(int cols, int rows, const QSizeF& sheetPx) const {
    cols=qMax(1,cols); rows=qMax(1,rows);
    const int W=qMax(1,int(sheetPx.width())), H=qMax(1,int(sheetPx.height()));
    QImage sheet(W,H,QImage::Format_ARGB32_Premultiplied); if(sheet.isNull()) return QImage(); sheet.fill(Qt::white);
    QPainter g(&sheet); g.setRenderHint(QPainter::Antialiasing); g.setRenderHint(QPainter::SmoothPixmapTransform);
    const int per=cols*rows, npages=qMax(1,m_doc.pageCount);
    const double cw=double(W)/cols, ch=double(H)/rows, margin=6.0;
    for(int i=0;i<per && i<npages;++i){
        const int cx=i%cols, cy=i/cols; const QRectF cell(cx*cw+margin, cy*ch+margin, cw-2*margin, ch-2*margin);
        const QRectF pr=m_doc.pageRect(i); if(pr.width()<1||pr.height()<1) continue;
        const double s=qMin(cell.width()/pr.width(), cell.height()/pr.height());
        g.save();
        g.translate(cell.left()+(cell.width()-pr.width()*s)/2, cell.top()+(cell.height()-pr.height()*s)/2);
        g.scale(s,s); g.translate(-pr.left(),-pr.top());
        g.fillRect(pr, m_doc.pageColor.isValid()?m_doc.pageColor:QColor(Qt::white));
        g.setClipRect(pr); paintDocument(g, m_doc);
        g.restore();
        g.setPen(QPen(QColor(200,200,200),1)); g.setBrush(Qt::NoBrush);
        g.drawRect(QRectF(cell.left()+(cell.width()-pr.width()*s)/2, cell.top()+(cell.height()-pr.height()*s)/2, pr.width()*s, pr.height()*s));
    }
    g.end(); return sheet;
}

// Import a .docx (Office Open XML): unzip, read word/document.xml, and flatten its paragraphs/runs
// into an editable wrapping text frame. Formatting beyond paragraph breaks + tabs is not carried.
bool Canvas::importDocx(const QString& path){
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray data=f.readAll(); f.close();
    const QMap<QString,QByteArray> zip=readZipEntries(data);
    const QByteArray xml = zip.value("word/document.xml");
    if(xml.isEmpty()){ emit statusMessage("Not a readable .docx (no word/document.xml)."); return false; }
    QString text; QXmlStreamReader r(xml);
    while(!r.atEnd()){ r.readNext();
        if(r.isStartElement()){
            const QString ln=r.name().toString();
            // Skip paragraph/run PROPERTIES wholesale: <w:pPr> holds <w:tabs><w:tab .../></w:tabs> tab-STOP
            // definitions whose element name is also "tab" — reading them would inject spurious '\t' into the text.
            if(ln=="pPr"||ln=="rPr"){ r.skipCurrentElement(); continue; }
            if(ln=="t") text += r.readElementText();
            else if(ln=="tab") text += '\t';         // now only run-level tab characters (<w:r><w:tab/></w:r>)
            else if(ln=="br"||ln=="cr") text += '\n';
        } else if(r.isEndElement() && r.name().toString()=="p") text += '\n';
    }
    text=text.trimmed();
    if(text.isEmpty()){ emit statusMessage(".docx contained no text."); return false; }
    Shape s; s.type=ShapeType::Text; s.text=text; s.textFrame=true; s.name=QFileInfo(path).fileName();
    s.fill.type=FillType::Solid; s.fill.color=Qt::black; s.stroke.enabled=false;
    const double w=qMin(m_doc.pageSize.width()*0.8, 520.0);
    s.rect=QRectF(0,0,w,qMax(80.0,m_doc.pageSize.height()*0.8));
    s.transform=QTransform::fromTranslate(m_doc.pageSize.width()*0.1, m_doc.pageSize.height()*0.08);
    addShapes({s},"Import DOCX");
    return true;
}
// Best-effort CorelDRAW .cdr import: real .cdr vector parsing is proprietary/huge, so we extract the
// embedded preview bitmap (ZIP-based .cdr → previews/*.png|jpg; RIFF .cdr → largest embedded JPEG/PNG)
// and place it as a photo placeholder. Honest fallback, mirroring how apps handle unknown .cdr versions.
bool Canvas::importCdr(const QString& path){
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray data=f.readAll(); f.close();
    QImage best;
    auto consider=[&](const QByteArray& b){ QImage im; if(im.loadFromData(b) && !im.isNull()
        && (qint64)im.width()*im.height() > (qint64)best.width()*best.height()) best=im; };
    const QMap<QString,QByteArray> zip=readZipEntries(data);   // newer zip-wrapped .cdr
    if(!zip.isEmpty()) for(auto it=zip.begin(); it!=zip.end(); ++it){ const QString n=it.key().toLower();
        if(n.contains("preview")||n.endsWith(".png")||n.endsWith(".jpg")||n.endsWith(".jpeg")) consider(it.value()); }
    if(best.isNull()){   // RIFF .cdr: scan for embedded JPEG (FFD8FF..FFD9) / PNG signatures
        const QByteArray png=QByteArray::fromHex("89504E470D0A1A0A"), jpg=QByteArray::fromHex("FFD8FF");
        // Cap decode attempts: a crafted .cdr densely packed with signatures (e.g. megabytes of repeated
        // FFD8FF) would otherwise trigger millions of multi-MB copies + decodes and hang the app.
        int tries=0; const int MAX_TRIES=64;
        for(int i=data.indexOf(png); i>=0 && tries<MAX_TRIES; i=data.indexOf(png,i+8)){ ++tries; consider(data.mid(i, qMin(data.size()-i, 8<<20))); }
        for(int i=data.indexOf(jpg); i>=0 && tries<MAX_TRIES; i=data.indexOf(jpg,i+3)){ ++tries; consider(data.mid(i, qMin(data.size()-i, 8<<20))); }
    }
    if(best.isNull()){ emit statusMessage("No importable preview found in the .cdr (vector .cdr parsing is unsupported)."); return false; }
    const double W=best.width(), H=best.height();
    const double sc=qMin(1.0, qMin(m_doc.pageSize.width()*0.9/W, m_doc.pageSize.height()*0.9/H));
    Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,W,H); s.name=QFileInfo(path).fileName()+" (preview)";
    s.fill.type=FillType::Bitmap; s.fill.image=best; s.fill.origImage=best; s.fill.tiled=false;
    s.fill.p0=QPointF(0,H); s.fill.p1=QPointF(W,H); s.fill.p2=QPointF(0,0); s.stroke.enabled=false;
    s.transform=QTransform::fromScale(sc,sc)*QTransform::fromTranslate((m_doc.pageSize.width()-W*sc)/2,(m_doc.pageSize.height()-H*sc)/2);
    addShapes({s},"Import CDR preview");
    emit statusMessage("Imported the .cdr preview bitmap (vector content isn't parsed).");
    return true;
}
// Data/mail merge: the current page is a TEMPLATE containing {{Field}} placeholders; a CSV whose first
// row names the fields produces one page per data row, with placeholders substituted. Undo-able.
bool Canvas::mailMerge(const QString& csvPath){
    QFile f(csvPath); if(!f.open(QIODevice::ReadOnly|QIODevice::Text)){ emit statusMessage("Could not open CSV."); return false; }
    const QString text=QString::fromUtf8(f.readAll()); f.close();
    QVector<QStringList> rows; QStringList cur; QString field; bool inQ=false;
    auto push=[&]{ if(cur.size()<256) cur<<field; field.clear(); };
    for(int i=0;i<text.size() && rows.size()<10000;++i){ const QChar ch=text[i];
        if(inQ){ if(ch=='"'){ if(i+1<text.size()&&text[i+1]=='"'){ field+='"'; ++i; } else inQ=false; } else field+=ch; }
        else if(ch=='"') inQ=true; else if(ch==',') push();
        else if(ch=='\n'||ch=='\r'){ if(ch=='\r'&&i+1<text.size()&&text[i+1]=='\n')++i; push(); if(!(cur.size()==1&&cur[0].isEmpty())) rows<<cur; cur.clear(); }
        else field+=ch; }
    if(!field.isEmpty()||!cur.isEmpty()){ push(); if(!(cur.size()==1&&cur[0].isEmpty())) rows<<cur; }
    if(rows.size()<2){ emit statusMessage("CSV needs a header row + at least one data row."); return false; }
    const QStringList headers=rows[0]; const int n=rows.size()-1;
    const QVector<Shape> tmpl=m_doc.shapes; const double adv=m_doc.pageSize.height()+m_doc.pageGap;
    std::function<void(Shape&,const QStringList&)> subst=[&](Shape& s,const QStringList& vals){
        auto rep=[&](QString& str){ for(int h=0;h<headers.size()&&h<vals.size();++h) str.replace("{{"+headers[h].trimmed()+"}}", vals[h]); };
        if(s.type==ShapeType::Text) rep(s.text);
        if(s.type==ShapeType::Table) for(TableCell& c: s.table.cells) rep(c.text);
        for(Shape& c: s.children) subst(c,vals); };
    pushUndo("Mail merge");
    QVector<Shape> out; out.reserve(tmpl.size()*n);
    for(int r=0;r<n;++r){ const QStringList& vals=rows[r+1];
        for(Shape s: tmpl){ subst(s,vals); s.transform = s.transform * QTransform::fromTranslate(0, r*adv); out.push_back(s); } }
    m_doc.shapes=out; m_doc.pageCount=n; m_sel.clear();
    emit documentChanged(); emit selectionChanged(); update();
    emit statusMessage(QString("Mail merge: %1 records → %1 pages.").arg(n));
    return true;
}
// Insert a QR code (generated by libqrencode) as a placed bitmap object at the page centre.
bool Canvas::insertQrCode(const QString& text){
    if(text.trimmed().isEmpty()) return false;
    QImage qr=xara::makeQrCode(text, 8);
    if(qr.isNull()){ emit statusMessage("QR generation unavailable (libqrencode not installed)."); return false; }
    const double W=qr.width(), H=qr.height();
    Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,W,H); s.name="QR code";
    s.fill.type=FillType::Bitmap; s.fill.image=qr; s.fill.origImage=qr; s.fill.tiled=false;
    s.fill.p0=QPointF(0,H); s.fill.p1=QPointF(W,H); s.fill.p2=QPointF(0,0); s.stroke.enabled=false;
    s.transform=QTransform::fromTranslate(m_doc.pageSize.width()/2-W/2, m_doc.pageSize.height()/2-H/2);
    addShapes({s},"Insert QR code");
    return true;
}
// Import an RTF file as a styled Text shape (reuses the paste-path RTF reader).
bool Canvas::importRtfFile(const QString& path) {
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false;
    Shape s=shapeFromRtf(f.readAll()); f.close();
    if(s.text.trimmed().isEmpty()) return false;
    s.name=QFileInfo(path).fileName();
    s.transform=QTransform::fromTranslate(m_doc.pageSize.width()*0.1, m_doc.pageSize.height()*0.1);
    addShapes({s},"Import RTF");
    return true;
}

// Composite device-CMYK PDF (PDF/X-style): each page is rendered, converted to DeviceCMYK (via the
// loaded ICC profile if available, else our naive rgbToCmyk), and written as one DeviceCMYK image
// XObject. A /OutputIntent (GTS_PDFX) is attached to the Catalog, embedding the CMYK ICC profile when
// one is loaded. Hand-written PDF because Qt's QPdfWriter only emits DeviceRGB.
bool Canvas::exportCmykPdf(const QString& path) {
    reflowText();
    const int P = qMax(1, m_doc.pageCount);
    QByteArray iccBytes;
    if(xara::Icc::instance().available() && !xara::Icc::instance().currentCmykProfile().isEmpty()){
        QFile f(xara::Icc::instance().currentCmykProfile()); if(f.open(QIODevice::ReadOnly)) iccBytes=f.readAll(); }
    const bool embedIcc = !iccBytes.isEmpty();
    const int OICC=4, base=5;                          // obj 1 Catalog,2 Pages,3 OutputIntent,4 ICC(optional),5.. page triples
    QMap<int,QByteArray> obj;
    QByteArray kids; for(int k=0;k<P;++k) kids += QByteArray::number(base+k*3+2)+" 0 R ";
    obj[1] = "<</Type/Catalog/Pages 2 0 R/OutputIntents[3 0 R]>>";
    obj[2] = "<</Type/Pages/Count "+QByteArray::number(P)+"/Kids["+kids.trimmed()+"]>>";
    QByteArray oi = "<</Type/OutputIntent/S/GTS_PDFX/OutputConditionIdentifier(CMYK)/Info(XarQ composite CMYK)";
    if(embedIcc) oi += "/DestOutputProfile "+QByteArray::number(OICC)+" 0 R";
    obj[3] = oi + ">>";
    if(embedIcc) obj[OICC] = "<</N 4/Length "+QByteArray::number(iccBytes.size())+">>\nstream\n"+iccBytes+"\nendstream";
    for(int k=0;k<P;++k){
        const QRectF pr=m_doc.pageRect(k);
        const int W=qBound(1,int(pr.width()),8000), H=qBound(1,int(pr.height()),8000);
        QImage rgb(W,H,QImage::Format_ARGB32_Premultiplied); rgb.fill(m_doc.pageColor);
        { QPainter g(&rgb); g.setRenderHint(QPainter::Antialiasing); g.setRenderHint(QPainter::SmoothPixmapTransform);
          g.translate(-pr.left(),-pr.top()); paintDocument(g,m_doc); }
        QByteArray cmyk; cmyk.resize(qint64(W)*H*4); char* d=cmyk.data();
        QVector<QImage> planes; const bool icc = xara::Icc::instance().available() && xara::Icc::instance().toCmykPlanes(rgb,planes) && planes.size()==4;
        for(int y=0;y<H;++y){ const QRgb* src=reinterpret_cast<const QRgb*>(rgb.constScanLine(y));
            const uchar* pc=icc?planes[0].constScanLine(y):nullptr,* pm=icc?planes[1].constScanLine(y):nullptr,
                       * py=icc?planes[2].constScanLine(y):nullptr,* pk=icc?planes[3].constScanLine(y):nullptr;
            for(int x=0;x<W;++x){ int C,M,Y,K;
                if(icc){ C=pc[x]; M=pm[x]; Y=py[x]; K=pk[x]; }
                else { double c,m,yy,kk; xara::rgbToCmyk(QColor::fromRgb(src[x]),c,m,yy,kk); C=int(c*255+0.5); M=int(m*255+0.5); Y=int(yy*255+0.5); K=int(kk*255+0.5); }
                *d++=char(qBound(0,C,255)); *d++=char(qBound(0,M,255)); *d++=char(qBound(0,Y,255)); *d++=char(qBound(0,K,255)); } }
        const QByteArray comp = xara::deflateZlib(cmyk);
        const bool useFlate = !comp.isEmpty() && comp.size()<cmyk.size();
        const QByteArray imgData = useFlate? comp : cmyk;
        const int imgN=base+k*3, cN=base+k*3+1, pN=base+k*3+2;
        obj[imgN] = "<</Type/XObject/Subtype/Image/Width "+QByteArray::number(W)+"/Height "+QByteArray::number(H)
            +"/ColorSpace/DeviceCMYK/BitsPerComponent 8"+(useFlate?QByteArray("/Filter/FlateDecode"):QByteArray())
            +"/Length "+QByteArray::number(imgData.size())+">>\nstream\n"+imgData+"\nendstream";
        const QByteArray cont = "q "+QByteArray::number(W)+" 0 0 "+QByteArray::number(H)+" 0 0 cm /Im Do Q";
        obj[cN] = "<</Length "+QByteArray::number(cont.size())+">>\nstream\n"+cont+"\nendstream";
        obj[pN] = "<</Type/Page/Parent 2 0 R/MediaBox[0 0 "+QByteArray::number(W)+" "+QByteArray::number(H)+"]"
            "/Resources<</XObject<</Im "+QByteArray::number(imgN)+" 0 R>>>>/Contents "+QByteArray::number(cN)+" 0 R>>";
    }
    QByteArray out="%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
    // PDF/X conformance: an Info dict declaring the GTS_PDFXVersion (with the OutputIntent above, this
    // makes the file a valid PDF/X-3 composite-CMYK deliverable for print).
    const int INFO = base+P*3;                          // Info dictionary object number
    obj[INFO] = "<</GTS_PDFXVersion(PDF/X-3:2002)/GTS_PDFXConformance(PDF/X-3:2002)/Trapped/False/Creator(XarQ)/Producer(XarQ)/Title("
                + QFileInfo(path).completeBaseName().toUtf8().replace('\\',"\\\\").replace('(',"\\(").replace(')',"\\)") + ")>>";
    const int count = INFO+1;                           // one past the highest object number used
    QMap<int,qint64> xoff;                             // 64-bit byte offsets — a big multi-page CMYK export can exceed 2 GB
    for(auto it=obj.constBegin(); it!=obj.constEnd(); ++it){ xoff[it.key()]=out.size();
        out += QByteArray::number(it.key())+" 0 obj\n"+it.value()+"\nendobj\n"; }
    const qint64 xrefPos=out.size();
    out += "xref\n0 "+QByteArray::number(count)+"\n0000000000 65535 f \n";
    for(int n=1;n<count;++n){ if(xoff.contains(n)){ QByteArray o=QByteArray::number(xoff[n]); while(o.size()<10) o.prepend('0'); out+=o+" 00000 n \n"; }
        else out += "0000000000 00000 f \n"; }
    out += "trailer\n<</Size "+QByteArray::number(count)+"/Root 1 0 R/Info "+QByteArray::number(INFO)+" 0 R>>\nstartxref\n"+QByteArray::number(xrefPos)+"\n%%EOF\n";
    QFile f(path); if(!f.open(QIODevice::WriteOnly)) return false; f.write(out); f.close(); return true;
}

bool Canvas::exportPdf(const QString& path) {
    reflowText();
    QPdfWriter pdf(path);
    pdf.setResolution(96);                       // 1 doc px == 1 device px @96dpi
    pdf.setPageSize(QPageSize(QSizeF(m_doc.pageSize.width()/96.0*25.4, m_doc.pageSize.height()/96.0*25.4),
                              QPageSize::Millimeter, "Page", QPageSize::ExactMatch));
    pdf.setPageMargins(QMarginsF(0,0,0,0));
    pdf.setCreator("XarQ"); pdf.setTitle(QFileInfo(path).completeBaseName());
    QPainter g(&pdf); g.setRenderHint(QPainter::Antialiasing); g.setRenderHint(QPainter::SmoothPixmapTransform);
    const int pages = qMax(1, m_doc.pageCount);
    for (int i=0;i<pages;++i){                    // one PDF page per document page (stacked vertically on the pasteboard)
        if (i>0) pdf.newPage();
        const QRectF pr = m_doc.pageRect(i);
        g.save();
        g.setClipRect(QRectF(0,0,pr.width(),pr.height()));
        g.fillRect(QRectF(0,0,pr.width(),pr.height()), m_doc.pageColor);
        g.translate(-pr.left(), -pr.top());       // this page's top-left → device (0,0)
        paintDocument(g, m_doc);
        g.restore();
    }
    g.end();
    return true;
}

// Draw the artwork (extended into the bleed) plus corner crop marks and edge registration
// targets, with the trim box at (margin,margin). Shared by exportWithMarks + the selftest.
void Canvas::drawPageWithMarks(QPainter& g, double bleed, double markLen){
    const double W=m_doc.pageSize.width(), H=m_doc.pageSize.height();
    const double gap=4, margin=bleed+markLen+gap;
    g.save(); g.translate(margin,margin);
    g.setClipRect(QRectF(-bleed,-bleed, W+2*bleed, H+2*bleed));    // artwork bleeds past the trim
    g.fillRect(QRectF(0,0,W,H), m_doc.pageColor);
    paintDocument(g, m_doc);
    g.restore();
    g.setPen(QPen(Qt::black, 0.75)); g.setBrush(Qt::NoBrush); g.setRenderHint(QPainter::Antialiasing);
    const double tl=margin, tt=margin, tr=margin+W, tb=margin+H, off=bleed+gap;
    auto H2=[&](double x0,double x1,double y){ g.drawLine(QPointF(x0,y),QPointF(x1,y)); };
    auto V2=[&](double y0,double y1,double x){ g.drawLine(QPointF(x,y0),QPointF(x,y1)); };
    H2(tl-off-markLen, tl-off, tt); V2(tt-off-markLen, tt-off, tl);   // TL
    H2(tr+off, tr+off+markLen, tt); V2(tt-off-markLen, tt-off, tr);   // TR
    H2(tl-off-markLen, tl-off, tb); V2(tb+off, tb+off+markLen, tl);   // BL
    H2(tr+off, tr+off+markLen, tb); V2(tb+off, tb+off+markLen, tr);   // BR
    auto reg=[&](double cx,double cy){ double r=markLen*0.4; g.drawEllipse(QPointF(cx,cy),r,r);
        g.drawLine(QPointF(cx-r*1.6,cy),QPointF(cx+r*1.6,cy)); g.drawLine(QPointF(cx,cy-r*1.6),QPointF(cx,cy+r*1.6)); };
    reg((tl+tr)/2, tt-off-markLen*0.5); reg((tl+tr)/2, tb+off+markLen*0.5);
    reg(tl-off-markLen*0.5, (tt+tb)/2); reg(tr+off+markLen*0.5, (tt+tb)/2);
}

// Prepress export with bleed + crop/registration marks (Module 3) → PDF sized to page+marks.
bool Canvas::exportWithMarks(const QString& path, double bleed, double markLen){
    reflowText();
    const double W=m_doc.pageSize.width(), H=m_doc.pageSize.height(), gap=4, margin=bleed+markLen+gap;
    const int OW=int(W+2*margin), OH=int(H+2*margin);
    QPdfWriter pdf(path); pdf.setResolution(96);
    pdf.setPageSize(QPageSize(QSizeF(OW/96.0*25.4, OH/96.0*25.4), QPageSize::Millimeter,"Page",QPageSize::ExactMatch));
    pdf.setPageMargins(QMarginsF(0,0,0,0)); pdf.setCreator("XarQ"); pdf.setTitle(QFileInfo(path).completeBaseName());
    QPainter g(&pdf); g.setRenderHint(QPainter::Antialiasing); g.setRenderHint(QPainter::SmoothPixmapTransform);
    drawPageWithMarks(g, bleed, markLen);
    g.end();
    return true;
}

// Prepress colour separations (Module 3): rasterise the page, split into C/M/Y/K ink plates,
// and write each as a labelled greyscale page in one PDF (darker = more ink, a printer's proof).
bool Canvas::exportSeparations(const QString& path) {
    reflowText();
    const int W=qBound(1,int(m_doc.pageSize.width()),10000), H=qBound(1,int(m_doc.pageSize.height()),10000);
    // Overprint: top-level objects flagged overprint ADD their ink to the plates (they don't knock out the
    // colours below). Render the base (knockout) composite from everything else, then add each overprinter.
    Document baseDoc; baseDoc.pageSize=m_doc.pageSize; baseDoc.pageColor=m_doc.pageColor; baseDoc.pageCount=m_doc.pageCount; baseDoc.layers=m_doc.layers;
    QVector<QPair<int,Shape>> overprints;   // keep z-index so we can mask each overprinter by objects stacked above it
    for(int i=0;i<m_doc.shapes.size();++i){ const Shape& s=m_doc.shapes[i]; if(s.overprint) overprints.push_back({i,s}); else baseDoc.shapes.push_back(s); }
    QImage rgb(W,H,QImage::Format_ARGB32_Premultiplied); if(rgb.isNull()) return false; rgb.fill(Qt::white);
    { QPainter g(&rgb); g.setRenderHint(QPainter::Antialiasing); g.setRenderHint(QPainter::SmoothPixmapTransform); paintDocument(g,baseDoc); }
    // Build the four ink plates: plate grey = 255*(1-ink) so no-ink is white, full ink is black.
    QImage plate[4]; for(int i=0;i<4;++i){ plate[i]=QImage(W,H,QImage::Format_Grayscale8); plate[i].fill(255); }
    QVector<QImage> iccPlanes;
    if(xara::Icc::instance().available() && xara::Icc::instance().toCmykPlanes(rgb, iccPlanes) && iccPlanes.size()==4){
        // Real ICC separation (system FOGRA/SWOP/GRACoL profile) — ink coverage 0..255 → positive plate 255-ink.
        for(int c=0;c<4;++c) for(int y=0;y<H;++y){ const uchar* ink=iccPlanes[c].constScanLine(y); uchar* p=plate[c].scanLine(y);
            for(int x=0;x<W;++x) p[x]=uchar(255-ink[x]); }
    } else {
        for(int y=0;y<H;++y){ const QRgb* src=reinterpret_cast<const QRgb*>(rgb.constScanLine(y));
            uchar* pc=plate[0].scanLine(y); uchar* pm=plate[1].scanLine(y); uchar* py=plate[2].scanLine(y); uchar* pk=plate[3].scanLine(y);
            for(int x=0;x<W;++x){ double C,M,Y,K; xara::rgbToCmyk(QColor::fromRgb(src[x]), C,M,Y,K);
                pc[x]=uchar(qBound(0,int(255*(1-C)+0.5),255)); pm[x]=uchar(qBound(0,int(255*(1-M)+0.5),255));
                py[x]=uchar(qBound(0,int(255*(1-Y)+0.5),255)); pk[x]=uchar(qBound(0,int(255*(1-K)+0.5),255)); } }
    }
    // Overprint pass: each overprinting object's ink is ADDED to the plates (not knocked out). But an
    // overprinter hidden UNDER a later opaque non-overprint object must not deposit ink where it is occluded,
    // so mask its coverage by the transmittance (1-alpha) of the non-overprint objects stacked above it.
    for(const auto& pr : overprints){
        const int oidx=pr.first; const Shape& ov=pr.second;
        Document od; od.pageSize=m_doc.pageSize; od.pageColor=QColor(0,0,0,0); od.pageCount=m_doc.pageCount; od.layers=m_doc.layers; od.shapes.push_back(ov);
        QImage oi(W,H,QImage::Format_ARGB32_Premultiplied); oi.fill(Qt::transparent);
        { QPainter g(&oi); g.setRenderHint(QPainter::Antialiasing); g.setRenderHint(QPainter::SmoothPixmapTransform); paintDocument(g,od); }
        oi=oi.convertToFormat(QImage::Format_ARGB32);   // straight alpha for correct colour read
        // Occluder alpha: composite of every non-overprint object above this one (typical case = none → no mask).
        QImage occ; { Document cd; cd.pageSize=m_doc.pageSize; cd.pageColor=QColor(0,0,0,0); cd.pageCount=m_doc.pageCount; cd.layers=m_doc.layers;
            for(int j=oidx+1;j<m_doc.shapes.size();++j){ const Shape& s=m_doc.shapes[j]; if(!s.overprint) cd.shapes.push_back(s); }
            if(!cd.shapes.isEmpty()){ occ=QImage(W,H,QImage::Format_ARGB32_Premultiplied); occ.fill(Qt::transparent);
                { QPainter g(&occ); g.setRenderHint(QPainter::Antialiasing); g.setRenderHint(QPainter::SmoothPixmapTransform); paintDocument(g,cd); }   // end painter before convert
                occ=occ.convertToFormat(QImage::Format_ARGB32); } }
        const bool masked=!occ.isNull();
        for(int y=0;y<H;++y){ const QRgb* src=reinterpret_cast<const QRgb*>(oi.constScanLine(y));
            const QRgb* om = masked ? reinterpret_cast<const QRgb*>(occ.constScanLine(y)) : nullptr;
            uchar* pc=plate[0].scanLine(y); uchar* pm=plate[1].scanLine(y); uchar* py=plate[2].scanLine(y); uchar* pk=plate[3].scanLine(y);
            for(int x=0;x<W;++x){ const int a=qAlpha(src[x]); if(a<8) continue;
                double cov=a/255.0; if(om) cov *= 1.0 - qAlpha(om[x])/255.0;   // hidden under an opaque object above → no ink
                if(cov<=0.003) continue;
                double C,M,Y,K; xara::rgbToCmyk(QColor(qRed(src[x]),qGreen(src[x]),qBlue(src[x])), C,M,Y,K);
                auto add=[&](uchar* p,double ink){ const int cur=255-p[x]; p[x]=uchar(255-qBound(0,int(cur+ink*cov*255+0.5),255)); };
                add(pc,C); add(pm,M); add(py,Y); add(pk,K); } }
    }
    // Spot-ink plates: for each document spot colour, a coverage plate of the objects that use it
    // (fill or stroke matches the spot RGB). Spot inks print on their own plate, not via CMYK.
    struct SpotPlate { QString name; QImage plate; };
    QVector<SpotPlate> spots;
    for(const NamedColor& nc : m_doc.namedColors){ if(!nc.spot) continue;
        const QRgb sr = nc.color.rgb();
        std::function<bool(const Shape&)> uses = [&](const Shape& s)->bool{
            if(s.fill.type!=FillType::None && s.fill.color.rgb()==sr) return true;
            if(s.stroke.enabled && s.stroke.color.rgb()==sr) return true;
            for(const Shape& c: s.children) if(uses(c)) return true; return false; };
        Document td; td.pageSize=m_doc.pageSize; td.pageColor=Qt::white; for(const Shape& s: m_doc.shapes) if(uses(s)) td.shapes.push_back(s);
        if(td.shapes.isEmpty()) continue;
        QImage cov(W,H,QImage::Format_ARGB32_Premultiplied); cov.fill(Qt::transparent);
        { QPainter cg(&cov); cg.setRenderHint(QPainter::Antialiasing); cg.setRenderHint(QPainter::SmoothPixmapTransform); paintDocument(cg,td); }
        QImage sp(W,H,QImage::Format_Grayscale8); sp.fill(255);
        for(int y=0;y<H;++y){ const QRgb* src=reinterpret_cast<const QRgb*>(cov.constScanLine(y)); uchar* d=sp.scanLine(y);
            for(int x=0;x<W;++x) d[x]=uchar(255-qAlpha(src[x])); }        // ink where the spot objects covered
        spots.push_back({nc.name.isEmpty()?QStringLiteral("Spot"):nc.name, sp});
    }
    const int M=26;   // margin for crop/registration marks (each mark prints black = 100% on every plate)
    QPdfWriter pdf(path); pdf.setResolution(96);
    pdf.setPageSize(QPageSize(QSizeF((W+2*M)/96.0*25.4, (H+2*M)/96.0*25.4), QPageSize::Millimeter, "Plate", QPageSize::ExactMatch));
    pdf.setPageMargins(QMarginsF(0,0,0,0)); pdf.setCreator("XarQ"); pdf.setTitle(QFileInfo(path).completeBaseName()+" separations");
    QPainter g(&pdf); g.setRenderHint(QPainter::Antialiasing); g.setRenderHint(QPainter::SmoothPixmapTransform);
    // Registration target (concentric circle + crosshair) — the operator aligns plates by these.
    auto regMark=[&](double cx,double cy){ g.setPen(QPen(Qt::black,0.9)); g.setBrush(Qt::NoBrush);
        g.drawEllipse(QPointF(cx,cy),5.5,5.5); g.drawEllipse(QPointF(cx,cy),2.5,2.5);
        g.drawLine(QPointF(cx-9,cy),QPointF(cx+9,cy)); g.drawLine(QPointF(cx,cy-9),QPointF(cx,cy+9)); };
    const char* names[4]={"Cyan","Magenta","Yellow","Black (K)"};
    bool first=true;
    auto page=[&](const QImage& plate, const QString& label){ if(!first) pdf.newPage(); first=false;
        g.drawImage(QRectF(M,M,W,H), plate);
        g.setPen(QPen(Qt::black,0.9));
        for(int cx: {0,W}) for(int cy: {0,H}){        // L-shaped corner crop marks just outside the trim
            const double px=M+cx, py=M+cy, sx=cx?1:-1, sy=cy?1:-1;
            g.drawLine(QPointF(px+sx*6,py),QPointF(px+sx*18,py)); g.drawLine(QPointF(px,py+sy*6),QPointF(px,py+sy*18)); }
        regMark(M+W/2.0, M/2.0); regMark(M+W/2.0, M+H+M/2.0); regMark(M/2.0, M+H/2.0); regMark(M+W+M/2.0, M+H/2.0);
        // Prepress control strip: a density ramp of this plate's ink (grey = 255*(1-density)) in the bottom margin,
        // so the operator can read + calibrate ink density on press. Plates are positive greyscale.
        { static const double dens[]={1.0,0.85,0.75,0.50,0.25,0.10,0.03}; const double pw=13, ph=10, sy=M+H+7;
          for(int k=0;k<7;++k){ const int v=int(255*(1.0-dens[k])+0.5); g.setPen(QPen(Qt::black,0.4)); g.setBrush(QColor(v,v,v));
              g.drawRect(QRectF(M+k*(pw+2), sy, pw, ph)); } }
        g.setPen(Qt::black); QFont f("DejaVu Sans",8); g.setFont(f);
        g.drawText(QRectF(M+4,2,260,M-4), Qt::AlignLeft|Qt::AlignVCenter, label); };
    for(int i=0;i<4;++i) page(plate[i], names[i]);
    for(const SpotPlate& s : spots) page(s.plate, "Spot: "+s.name);
    g.end();
    return true;
}

bool Canvas::exportGif(const QString& path, int delayMs, int loops) {
    QRectF b=m_doc.contentBounds(); if(b.isNull()||b.width()<1) b=QRectF(QPointF(0,0),m_doc.pageSize); b.adjust(-4,-4,4,4);
    const int W=qBound(2, int(b.width()), 800), H=qMax(2, int(b.height()*W/b.width()));
    const double sc=double(W)/b.width();
    QVector<QImage> frames; QVector<int> perFrameMs;
    QVector<bool> savedVis; for(auto& l:m_doc.layers) savedVis.push_back(l.visible);
    const int nF=qMax(1, m_doc.layers.size());
    for(int fi=0;fi<nF;++fi){
        if(m_doc.layers.size()>1) for(int k=0;k<m_doc.layers.size();++k) m_doc.layers[k].visible=(k==fi);
        QImage img(W,H,QImage::Format_ARGB32_Premultiplied); img.fill(Qt::white);
        QPainter g(&img); g.setRenderHint(QPainter::Antialiasing); g.setRenderHint(QPainter::SmoothPixmapTransform);
        g.scale(sc,sc); g.translate(-b.topLeft()); paintDocument(g,m_doc); g.end();
        frames.push_back(img);
        // per-frame timing: the layer's own delay overrides the default (0 => default)
        const int d = (fi<m_doc.layers.size() && m_doc.layers[fi].frameDelayMs>0) ? m_doc.layers[fi].frameDelayMs : delayMs;
        perFrameMs.push_back(d);
    }
    for(int k=0;k<m_doc.layers.size();++k) m_doc.layers[k].visible=savedVis[k];
    update();
    return writeAnimatedGif(path, frames, delayMs, loops, perFrameMs);
}

bool Canvas::exportPresentation(const QString& path, int transition) {
    QFileInfo fi(path); const QString base=fi.completeBaseName(), dir=fi.absolutePath();
    const int n=qMax(1, m_doc.pageCount);
    QStringList slides;
    for(int pi=0;pi<n;++pi){ QRectF pr=m_doc.pageRect(pi);
        const int W=qMax(1,int(pr.width())), H=qMax(1,int(pr.height()));
        QImage img(W,H,QImage::Format_ARGB32_Premultiplied); img.fill(m_doc.pageColor);
        { QPainter g(&img); g.setRenderHint(QPainter::Antialiasing); g.setRenderHint(QPainter::SmoothPixmapTransform); g.translate(-pr.topLeft()); paintDocument(g,m_doc); }
        QString pn=QString("%1_%2.png").arg(base).arg(pi+1); if(!img.save(dir+"/"+pn)) return false; slides<<pn; }
    // Transition: 0 cut, 1 fade, 2 slide, 3 zoom — driven by a body class + CSS on the active `.on` slide.
    const char* tcls[]={"tcut","tfade","tslide","tzoom"}; const QString bodyCls=tcls[qBound(0,transition,3)];
    QString html="<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\"><title>"+base.toHtmlEscaped()+"</title>\n"
        "<style>html,body{margin:0;height:100%;background:#111;overflow:hidden}"
        ".slide{position:absolute;inset:0;background-position:center;background-repeat:no-repeat;background-size:contain;"
        "opacity:0;transform:scale(1);transition:opacity .5s ease,transform .5s ease}.slide.on{opacity:1;z-index:1}"
        ".tcut .slide{transition:none}"
        ".tslide .slide{transform:translateX(7%)}.tslide .slide.on{transform:translateX(0)}"
        ".tzoom .slide{transform:scale(.92)}.tzoom .slide.on{transform:scale(1)}"
        ".nav{position:fixed;bottom:10px;right:14px;color:#888;font:13px sans-serif;z-index:9}</style></head>\n<body class=\""+bodyCls+"\">\n";
    for(const QString& pn: slides) html+="<div class=\"slide\" style=\"background-image:url('"+pn+"')\"></div>\n";
    html+="<div class=\"nav\">← →  to navigate</div>\n"
        "<script>let i=0;const s=document.querySelectorAll('.slide');function show(n){i=(n+s.length)%s.length;s.forEach((e,k)=>e.classList.toggle('on',k==i))}"
        "document.onkeydown=e=>{if(e.key=='ArrowRight'||e.key==' ')show(i+1);if(e.key=='ArrowLeft')show(i-1)};document.onclick=()=>show(i+1);show(0)</script>\n</body></html>\n";
    QFile f(path); if(!f.open(QIODevice::WriteOnly|QIODevice::Text)) return false; f.write(html.toUtf8()); f.close();
    return true;
}

bool Canvas::exportHtml(const QString& path) {
    reflowText();
    QFileInfo fi(path); const QString base=fi.completeBaseName(), dir=fi.absolutePath(), pngName=base+".png";
    QRectF pageR(QPointF(0,0), m_doc.pageSize);
    const int W=qBound(1,int(pageR.width()),10000), H=qBound(1,int(pageR.height()),10000);
    QImage img(W,H,QImage::Format_ARGB32_Premultiplied); if(img.isNull()) return false; img.fill(m_doc.pageColor);
    { QPainter g(&img); g.setRenderHint(QPainter::Antialiasing); g.setRenderHint(QPainter::SmoothPixmapTransform); paintDocument(g,m_doc); }
    if(!img.save(dir+"/"+pngName)) return false;
    // Render a brightened "rollover" image of a single linked object (transparent bg) for hover swap.
    auto renderHover=[&](const Shape& s)->QString{
        QRectF b=s.docBounds(); int w=qMax(1,int(b.width())+2), h=qMax(1,int(b.height())+2);
        QImage im(w,h,QImage::Format_ARGB32_Premultiplied); im.fill(Qt::transparent);
        { QPainter g(&im); g.setRenderHint(QPainter::Antialiasing); g.setRenderHint(QPainter::SmoothPixmapTransform);
          g.translate(-b.topLeft()+QPointF(1,1)); Document tmp; tmp.shapes.push_back(s); tmp.pageSize=m_doc.pageSize; paintDocument(g,tmp); }
        for(int y=0;y<im.height();++y){ QRgb* ln=reinterpret_cast<QRgb*>(im.scanLine(y));
            for(int x=0;x<im.width();++x){ QColor c=QColor::fromRgba(ln[x]); if(c.alpha()==0) continue;
                c=QColor::fromHsv(c.hsvHue(), c.hsvSaturation(), qMin(255,int(c.value()*1.18)), c.alpha()); ln[x]=c.rgba(); } }
        QString hn=QString("%1_ro%2.png").arg(base).arg(reinterpret_cast<quintptr>(&s),0,16);
        im.save(dir+"/"+hn); return hn; };
    QString html="<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\"><title>"+base.toHtmlEscaped()+"</title>\n"
        "<style>body{margin:0;background:#222}.stage{position:relative;margin:0 auto;width:"+QString::number(W)+"px;height:"+QString::number(H)+
        "px;background:url('"+pngName+"') no-repeat;background-size:contain}"
        ".lnk{position:absolute;display:block;cursor:pointer;background-size:contain;background-repeat:no-repeat;transition:filter .12s,transform .12s}"
        ".lnk:hover{filter:brightness(1.06) drop-shadow(0 2px 5px rgba(0,0,0,.35));transform:translateY(-1px)}</style></head>\n<body>\n<div class=\"stage\">\n";
    int anchorN=0;
    for(const Shape& s: m_doc.shapes){
        if(!s.name.isEmpty() && s.url.isEmpty()){ // named object => anchor target for internal #links
            QRectF b=s.docBounds(); html+=QString("  <a id=\"%1\" style=\"position:absolute;left:%2px;top:%3px\"></a>\n")
                .arg(QString(s.name).replace(QRegularExpression("[^A-Za-z0-9_]"),"_")).arg(int(b.left())).arg(int(b.top())); anchorN++; }
        // Per-character run hyperlinks (T4): overlay a transparent <a> over each linked span.
        if(s.type==ShapeType::Text && !s.textRuns.isEmpty()){
            for(const auto& lr : s.runLinkRects()){
                QRectF db=s.transform.mapRect(lr.first);   // local span rect → doc space
                html+=QString("  <a class=\"tlnk\" href=\"%1\" style=\"position:absolute;display:block;left:%2px;top:%3px;width:%4px;height:%5px\" title=\"%1\"></a>\n")
                    .arg(safeHref(lr.second).toHtmlEscaped(), QString::number(int(db.left())), QString::number(int(db.top())),
                         QString::number(qMax(1,int(db.width()))), QString::number(qMax(1,int(db.height()))));  // single-pass: %-encoded href not re-scanned
            }
        }
        if(s.url.isEmpty()) continue; QRectF b=s.docBounds();
        QString hover=renderHover(s);
        html+=QString("  <a class=\"lnk\" href=\"%1\" style=\"left:%2px;top:%3px;width:%4px;height:%5px\" title=\"%6\""
                      " onmouseover=\"this.style.backgroundImage='url(%7)'\" onmouseout=\"this.style.backgroundImage=''\"></a>\n")
            .arg(safeHref(s.url).toHtmlEscaped(), QString::number(int(b.left())), QString::number(int(b.top())),
                 QString::number(qMax(1,int(b.width()))), QString::number(qMax(1,int(b.height()))), s.name.toHtmlEscaped(), hover); }  // single-pass
    html+="</div>\n</body></html>\n";
    QFile f(path); if(!f.open(QIODevice::WriteOnly|QIODevice::Text)) return false; f.write(html.toUtf8()); f.close();
    return true;
}

// Semantic responsive website publish (Module 2). Each page → its own HTML file with:
//  • a vector layer (inline <svg> from writeSvg: shapes, gradients, photos as <image>),
//  • real, selectable HTML text for every top-level Text object (styled, run-aware, linked),
//  • per-object stretch/pin/sticky CSS, breakpoint media queries that scale the stage, and
//  • a nav bar linking the pages of a multi-page document.
bool Canvas::exportWebsite(const QString& path) {
    reflowText();
    QFileInfo fi(path); const QString dir=fi.absolutePath(), base=fi.completeBaseName();
    if(!dir.isEmpty()) QDir().mkpath(dir);   // create the site folder if it doesn't exist (Xara publishes into a new folder)
    const double DW=m_doc.pageSize.width(), PH=m_doc.pageSize.height();
    auto esc=[](const QString& s){ return s.toHtmlEscaped(); };
    // For COSMETIC text/attribute values (names, alt, title) that get chained through QString::arg BEFORE
    // later .arg() calls: also neutralise '%' → &#37; so a name like "A%7B" can't be re-scanned as a
    // placeholder (renders identically in the browser). NOT for hrefs — those need the literal '%'.
    auto escn=[](const QString& s){ return s.toHtmlEscaped().replace('%', QLatin1String("&#37;")); };
    // A font family goes inside a single-quoted CSS string within a double-quoted HTML attribute.
    // esc() guards the attribute (") but not the CSS string ('), so strip ' and \ to prevent CSS injection.
    auto cssFam=[&](const QString& f){ QString e=f; e.remove('\\').remove('\''); return esc(e); };
    auto ppFor=[&](int i)->PageProps{ return (i>=0 && i<m_doc.pageProps.size())? m_doc.pageProps[i] : PageProps{}; };
    auto pageFile=[&](int i){ const QString nm=ppFor(i).name;   // per-page custom HTML filename (sanitised to [a-z0-9-_])
        if(!nm.isEmpty()){ QString s; for(QChar ch: nm.toLower()) s += (ch.isLetterOrNumber()||ch=='-'||ch=='_')? ch : QChar('-');
            while(s.contains("--")) s.replace("--","-"); if(!s.isEmpty()) return s+".html"; }
        return i==0? base+".html" : QString("%1_p%2.html").arg(base).arg(i+1); };
    // "link to a page in this website": a url of the form "page:N" resolves to that page's HTML file.
    auto resolveUrl=[&](const QString& u)->QString{ if(u.startsWith("page:")){ bool ok=false; int n=u.mid(5).toInt(&ok);
        if(ok && n>=1 && n<=m_doc.pageCount) return pageFile(n-1); } return u; };

    // Shared CSS: centred fixed-width stage, scaled down to fit narrow viewports at each breakpoint.
    QVector<int> bps=m_doc.breakpoints; std::sort(bps.begin(),bps.end());
    QString media;
    for(int bp: bps){ if(bp<=0||bp>=DW) continue; double sc=double(bp)/DW;
        media+=QString("@media(max-width:%1px){.stage{transform:scale(%2)}.wrap{height:%3px}}\n")
            .arg(bp).arg(sc,0,'f',4).arg(int(PH*sc)); }
    // Collect the fonts the page uses and embed them as @font-face (P6) so it renders faithfully anywhere.
    QStringList fams; QSet<QString> fseen;
    { std::function<void(const Shape&)> gf=[&](const Shape& s){ if(s.type==ShapeType::Text){
          if(!fseen.contains(s.font.family())){ fseen.insert(s.font.family()); fams<<s.font.family(); }
          for(const TextRun& r: s.textRuns) if(!fseen.contains(r.font.family())){ fseen.insert(r.font.family()); fams<<r.font.family(); } }
          for(const Shape& ch: s.children) gf(ch); };
      for(const Shape& s: m_doc.shapes) gf(s); }
    const QString faceCss = fontFaceCss(fams);
    QString css= faceCss +
        "*{box-sizing:border-box}body{margin:0;font-family:'DejaVu Sans',Arial,sans-serif;background:#f4f4f4}\n"
        ".nav{position:sticky;top:0;z-index:200;background:#222;padding:8px 14px}.nav a{color:#eee;margin-right:16px;text-decoration:none}.nav a:hover{color:#fff;text-decoration:underline}\n"
        ".wrap{width:100%;overflow:hidden;height:"+QString::number(int(PH))+"px}\n"
        ".stage{position:relative;width:"+QString::number(int(DW))+"px;height:"+QString::number(int(PH))+"px;margin:0 auto;transform-origin:top left;background:#fff}\n"
        ".stage svg.bg{position:absolute;left:0;top:0}\n"
        ".txt{position:absolute;white-space:pre-wrap;line-height:1.18}\n"
        ".sticky{position:fixed!important;z-index:150}\n"
        ".anim{opacity:0;transition:opacity .7s ease,transform .7s ease;will-change:opacity,transform}\n"
        ".anim.a-up{transform:translateY(40px)}.anim.a-down{transform:translateY(-40px)}\n"
        ".anim.a-left{transform:translateX(40px)}.anim.a-right{transform:translateX(-40px)}\n"
        ".anim.a-zoom{transform:scale(.86)}.anim.in{opacity:1;transform:none}\n"
        ".animwrap svg{display:block}\n"
        ".slider{position:absolute;overflow:hidden}\n"
        ".slider .slide{position:absolute;inset:0;opacity:0;transition:opacity .6s ease}.slider .slide.on{opacity:1}\n"
        ".slider .slide img{width:100%;height:100%;object-fit:cover;display:block}\n"
        ".slider .sl-nav{position:absolute;top:50%;transform:translateY(-50%);background:rgba(0,0,0,.4);color:#fff;border:0;font-size:22px;line-height:1;cursor:pointer;padding:6px 10px;z-index:3}\n"
        ".slider .sl-prev{left:6px}.slider .sl-next{right:6px}\n"
        ".slider .sl-dots{position:absolute;bottom:8px;left:0;right:0;text-align:center;z-index:3}\n"
        ".slider .sl-dots i{display:inline-block;width:9px;height:9px;border-radius:50%;background:rgba(255,255,255,.5);margin:0 3px;cursor:pointer}.slider .sl-dots i.on{background:#fff}\n"
        ".xnav{position:absolute;display:flex;align-items:center;gap:4px;padding:0 16px;box-sizing:border-box}\n"
        ".xnav a{color:inherit;text-decoration:none;padding:8px 14px;border-radius:5px;font-weight:600;font-size:15px;opacity:.82}\n"
        ".xnav a:hover{opacity:1;background:rgba(127,127,127,.22)}.xnav a.on{opacity:1;background:rgba(127,127,127,.3)}\n"
        ".xmenu{position:absolute;display:flex;align-items:center;padding:0 8px;box-sizing:border-box}\n"
        ".xmenu ul{list-style:none;margin:0;padding:0;display:flex}\n"
        ".xmenu li{position:relative}\n"
        ".xmenu li ul{display:none;position:absolute;left:0;top:100%;flex-direction:column;min-width:170px;background:inherit;box-shadow:0 8px 20px rgba(0,0,0,.28)}\n"
        ".xmenu li li ul{left:100%;top:0}\n"
        ".xmenu li:hover>ul,.xmenu li:focus-within>ul{display:flex}\n"
        ".xmenu a{display:block;color:inherit;text-decoration:none;padding:9px 16px;white-space:nowrap;font-weight:600;font-size:15px;opacity:.9}\n"
        ".xmenu a:hover{opacity:1;background:rgba(127,127,127,.22)}\n"
        ".xmenu li>a.has-sub::after{content:'▾';font-size:11px;opacity:.7;margin-left:6px}\n"
        ".flexsection{display:flex;flex-wrap:wrap;gap:16px;justify-content:center;align-items:stretch;"
            "max-width:"+QString::number(int(DW))+"px;margin:24px auto;padding:0 16px;box-sizing:border-box}\n"
        ".flexsection .flexitem{flex:1 1 240px;min-width:170px;max-width:100%}\n"
        ".flexsection .flexitem img{width:100%;height:auto;display:block;border-radius:6px}\n"+media
        + (m_doc.presentSteps? QString(".pstep.pshide{display:none!important}\n"
            ".phint{position:fixed;left:50%;bottom:12px;transform:translateX(-50%);background:rgba(0,0,0,.62);color:#fff;"
            "font:13px/1 sans-serif;padding:6px 13px;border-radius:14px;z-index:9999;pointer-events:none}\n") : QString());

    // Full-width background bands: a page with a band gets position:relative on .wrap so the 100vw band
    // divs anchor to it. Gated on docHasBands so a band-free document's CSS stays byte-identical.
    const bool docHasBands = [&]{ for(const Shape& s: m_doc.shapes) if(s.visible && s.webBand) return true; return false; }();
    if(docHasBands) css += ".wrap{position:relative}.band{position:absolute;left:0;right:0;z-index:0;background-size:100% 100%}\n";
    // Contact-form widget CSS (gated so a form-free document's CSS stays byte-identical).
    const bool docHasForms = [&]{ for(const Shape& s: m_doc.shapes) if(s.visible && !s.webForm.isEmpty()) return true; return false; }();
    if(docHasForms) css += ".wform{position:absolute;display:flex;flex-direction:column;gap:8px}"
        ".wform label{font:600 13px/1.2 sans-serif;color:#333}.wform input,.wform textarea{font:14px sans-serif;padding:8px;border:1px solid #bbb;border-radius:5px;width:100%;box-sizing:border-box}"
        ".wform button{align-self:flex-start;padding:9px 20px;border:0;border-radius:5px;background:#2d6ff7;color:#fff;font:600 14px sans-serif;cursor:pointer}.wform button:hover{background:#1b5fe0}\n";

    // Nav across pages — auto-generated top bar, SUPPRESSED when the document has a Nav Bar object
    // (the user's placed/styled nav bar replaces it, so they don't double up).
    bool hasNavObj=false; for(const Shape& s: m_doc.shapes) if(s.visible && s.webNavBar){ hasNavObj=true; break; }
    QString nav; if(m_doc.pageCount>1 && !hasNavObj){ nav="<nav class=\"nav\">"; for(int i=0;i<m_doc.pageCount;++i)
        nav+=QString("<a href=\"%1\">Page %2</a>").arg(pageFile(i)).arg(i+1); nav+="</nav>\n"; }

    // Fluid (Modular) site: continuously scale the designed stage to the viewport width, capped at 100% —
    // smooth fluid scaling instead of the stepped breakpoint media queries (which remain as the no-JS fallback).
    // Empty when fluidSite is off, so the fixed-stage export stays byte-identical.
    QString fluidJs;
    if(m_doc.fluidSite)
        fluidJs = QString("<script>(function(){var DW=%1,PH=%2;function f(){var ws=document.querySelectorAll('.wrap');"
                  "for(var i=0;i<ws.length;i++){var w=ws[i],st=w.querySelector('.stage');if(!st)continue;"
                  "var s=Math.min(1,w.clientWidth/DW);st.style.transformOrigin='top left';st.style.transform='scale('+s+')';"
                  "w.style.height=(PH*s)+'px';}}window.addEventListener('resize',f);window.addEventListener('load',f);f();})();</script>\n")
                  .arg(int(DW)).arg(int(PH));

    // Presentation controller: reveal cumulative layer-steps on click / arrow keys. Empty when off.
    QString presentJs;
    if(m_doc.presentSteps)
        presentJs = "<div class=\"phint\">Click or &#8594; to advance &middot; &#8592; back</div>\n<script>(function(){"
            "var st=[].slice.call(document.querySelectorAll('.pstep')),mx=0;"
            "st.forEach(function(e){var s=+e.getAttribute('data-step')||0;if(s>mx)mx=s;});var c=0;"
            "function r(){st.forEach(function(e){var s=+e.getAttribute('data-step')||0;e.classList.toggle('pshide',s>c);});}"
            "function nx(){if(c<mx){c++;r();}}function pv(){if(c>0){c--;r();}}"
            "addEventListener('keydown',function(e){if(e.key==='ArrowRight'||e.key===' '||e.key==='PageDown'){nx();e.preventDefault();}"
            "else if(e.key==='ArrowLeft'||e.key==='PageUp'){pv();}});"
            "addEventListener('click',function(e){var t=e.target;if(t&&t.closest&&t.closest('a,button,input,select,textarea,video,audio,.lb,.slider'))return;nx();});r();})();</script>\n";

    // Mouse-over pop-ups: hovering a [data-poptrig] shows the matching [data-poppanel] object(s). Empty when
    // the document has no pop-ups, so a pop-up-free export stays byte-identical.
    QString popupJs;
    { bool hasPop=false; for(const Shape& s: m_doc.shapes) if(s.visible && (s.webPopupId>0 || s.webPopupTrigger>0)){ hasPop=true; break; }
      if(hasPop) popupJs = "<script>(function(){var m={};"
        "[].forEach.call(document.querySelectorAll('[data-poppanel]'),function(p){p.style.display='none';var id=p.getAttribute('data-poppanel');(m[id]=m[id]||[]).push(p);});"
        "[].forEach.call(document.querySelectorAll('[data-poptrig]'),function(t){var id=t.getAttribute('data-poptrig');"
        "function sh(){(m[id]||[]).forEach(function(p){p.style.display='';});}function hd(){(m[id]||[]).forEach(function(p){p.style.display='none';});}"
        "t.addEventListener('mouseenter',sh);t.addEventListener('mouseleave',hd);t.addEventListener('click',function(e){e.preventDefault();sh();});"
        "(m[id]||[]).forEach(function(p){p.addEventListener('mouseenter',sh);p.addEventListener('mouseleave',hd);});});})();</script>\n"; }

    // Presentation build order: when any object carries a webBuild, reveal by a DENSE (layer, build) rank so
    // objects within a layer step in sequence. No webBuild anywhere → keep the raw-layer step (byte-identical).
    bool anyBuild=false; for(const Shape& s: m_doc.shapes) if(s.webBuild>0){ anyBuild=true; break; }
    QMap<QPair<int,int>,int> buildRank;
    if(anyBuild){ QList<QPair<int,int>> keys; for(const Shape& s: m_doc.shapes){ if(!s.visible) continue;
            QPair<int,int> k(s.layer,s.webBuild); if(!keys.contains(k)) keys.push_back(k); }
        std::sort(keys.begin(),keys.end()); for(int i=0;i<keys.size();++i) buildRank[keys[i]]=i; }

    bool okAll=true;
    for(int pi=0; pi<m_doc.pageCount; ++pi){
        const QRectF pr=m_doc.pageRect(pi); const double pTop=pr.top();
        // Partition top-level shapes: real HTML text vs the vector/photo layer.
        QVector<Shape> vec; QVector<const Shape*> texts, embeds, photos, anims, media, navbars, bands, named, forms;
        QMap<int, QVector<const Shape*>> sliderMap;              // slider group id → its slides (in document order)
        QMap<int, QVector<const Shape*>> flexMap;               // FlexGroup id → its items (fluid, reflow below the stage)
        for(const Shape& s: m_doc.shapes){ if(!s.visible) continue;
            if(s.webNavBar){ navbars.push_back(&s); continue; }                   // nav bar appears on EVERY page (site navigation)
            // Own each shape by the page its centre falls in, so a shape straddling the inter-page
            // gap lands on exactly ONE page (intersects() would duplicate it onto both).
            const int home=qBound(0, int(s.docBounds().center().y()/(PH+m_doc.pageGap)), m_doc.pageCount-1);
            if(home!=pi) continue;
            if(!s.name.trimmed().isEmpty()) named.push_back(&s);                  // named object → published as a jump-link anchor (#name), in addition to its normal render
            if(!s.webForm.isEmpty()) forms.push_back(&s);                         // contact-form widget → published <form>
            else if(s.webBand) bands.push_back(&s);                               // full-width background band — emitted OUTSIDE the fixed stage
            else if(s.webFlex) flexMap[s.webFlex].push_back(&s);                  // FlexGroup item → fluid reflow section below the stage
            else if(s.webSlider) sliderMap[s.webSlider].push_back(&s);            // carousel slide — grouped, emitted as one cross-fading slider
            else if(!s.webMediaData.isEmpty() && !s.webMediaMime.isEmpty()) media.push_back(&s);  // embedded video/audio/GIF → HTML player
            else if(!s.webEmbed.isEmpty()) embeds.push_back(&s);                  // iframe/video/map/form placeholder
            else if(s.webLightbox) photos.push_back(&s);                          // pop-up photo (lightbox) — emitted as clickable <img>
            else if(s.type==ShapeType::Text && s.textPath.isEmpty()) texts.push_back(&s);
            else if(s.webAnim) anims.push_back(&s);                               // animated vector shape → its own inline <svg> so it can reveal on scroll
            else vec.push_back(s); }
        // animation reveal class for a given kind (parallax uses a data attribute, handled inline)
        auto animCls=[](int a)->QString{ switch(a){ case 1:return "anim a-fade"; case 2:return "anim a-up"; case 3:return "anim a-down";
            case 4:return "anim a-left"; case 5:return "anim a-right"; case 6:return "anim a-zoom"; default:return QString(); } };
        bool anyAnim=false;
        // Vector layer as one inline SVG sized to the page (viewBox in doc coords → page-local via offset).
        QByteArray svg = writeSvg(vec, QRectF(0, pTop, DW, PH));
        QString svgInline = QString::fromUtf8(svg);
        svgInline.remove(QRegularExpression("<\\?xml[^>]*\\?>\\s*"));
        svgInline.replace("<svg ", "<svg class=\"bg\" ");

        QString body; body.reserve(4096);
        // Responsive show/hide: a shape with webHideBelow gets a unique id + a media query that hides it
        // below that viewport width (Website Variants primitive). Collected, then injected into <style>.
        QString respCss; int respId=0;
        auto respAttr=[&](const Shape& s)->QString{
            QString pop;   // pop-up data attributes on the object's own (box-generating) element
            if(s.webPopupTrigger>0) pop += " data-poptrig=\""+QString::number(s.webPopupTrigger)+"\"";
            if(s.webPopupId>0)      pop += " data-poppanel=\""+QString::number(s.webPopupId)+"\"";
            if(s.webHideBelow<=0 && s.webVarBelow<=0) return pop;
            const QString id=QString("rv%1").arg(respId++);
            if(s.webHideBelow>0)
                respCss+=QString("@media(max-width:%1px){#%2{display:none!important}}\n").arg(s.webHideBelow).arg(id);
            // Website Variant: below the given width, move/resize the element to its override box. Make the top
            // page-local against the shape's OWN home page — for ordinary objects home==pi so this equals pTop
            // (unchanged), but a nav bar is emitted on every page positioned by its home page, so pTop would be wrong.
            if(s.webVarBelow>0){
                const double sTop=m_doc.pageRect(qBound(0,int(s.docBounds().center().y()/(PH+m_doc.pageGap)),m_doc.pageCount-1)).top();
                respCss+=QString("@media(max-width:%1px){#%2{left:%3px!important;top:%4px!important;width:%5px!important;right:auto}}\n")
                    .arg(s.webVarBelow).arg(id).arg(int(s.webVarX)).arg(int(s.webVarY-sTop)).arg(int(qMax(1.0,s.webVarW))); }
            return pop+" id=\""+id+"\""; };
        // Presentation: wrap an emitted element in a reveal-step div keyed by its layer. Identity when off
        // (so the non-presentation export stays byte-identical). display:contents => layout-neutral wrapper.
        auto psWrap=[&](const Shape& s, const QString& html)->QString{
            if(!m_doc.presentSteps) return html;
            const int step = anyBuild ? buildRank.value({s.layer,s.webBuild}, s.layer) : s.layer;
            return "<div class=\"pstep\" data-step=\""+QString::number(step)+"\" style=\"display:contents\">"+html+"</div>"; };
        for(const Shape* tp: texts){ const Shape& s=*tp; QRectF b=s.docBounds();
            const double L=b.left(), T=b.top()-pTop, W=b.width();
            // stretch / pin / sticky → position CSS
            QString pos=QString("left:%1px;top:%2px;").arg(int(L)).arg(int(T));
            QString size = (s.textFrame? QString("width:%1px;").arg(int(qMax(1.0,s.rect.width()))) : QString("width:%1px;").arg(int(qMax(1.0,W))));
            if(s.webStretch==1 || ((s.webPin&1)&&(s.webPin&2))){ pos=QString("left:0;right:0;top:%1px;").arg(int(T)); size="width:auto;"; }
            else if(s.webPin&2){ pos=QString("right:%1px;top:%2px;").arg(int(DW-b.right())).arg(int(T)); }
            QString ac=animCls(s.webAnim); if(!ac.isEmpty()||s.webAnim==7) anyAnim=true;
            QString cls = QString(s.webSticky? "txt sticky" : "txt")+(ac.isEmpty()?"":" "+ac);
            QString pxAttr = s.webAnim==7? " data-parallax=\"0.3\"":"";
            QString style=pos+size
                +QString("font-family:'%1',sans-serif;font-size:%2px;")
                    .arg(cssFam(s.font.family())).arg(s.font.pointSizeF()>0?s.font.pointSizeF():16)
                +(s.font.bold()?"font-weight:bold;":"")+(s.font.italic()?"font-style:italic;":"")
                +QString("text-align:%1;").arg(s.textAlign==1?"center":s.textAlign==2?"right":s.textAlign==3?"justify":"left")
                +QString("color:%1;").arg((s.fill.type==FillType::Solid?s.fill.color:QColor(Qt::black)).name())
                +(s.webAnimDelay>0?QString("transition-delay:%1s;").arg(s.webAnimDelay,0,'f',2):QString());
            // content: run-aware spans, else escaped text
            QString content;
            if(!s.textRuns.isEmpty()){ QVector<QFont> fs; QVector<QColor> cs; QVector<QString> us; s.resolveChars(fs,cs,us);
                int n=s.text.size(), i=0;
                while(i<n){ int j=i+1; while(j<n && fs[j]==fs[i] && cs[j]==cs[i] && us[j]==us[i]) ++j;
                    QString sp=QString("<span style=\"font-family:'%1';font-size:%2px;%3%4color:%5\">%6</span>")
                        .arg(cssFam(fs[i].family())).arg(fs[i].pointSizeF()>0?fs[i].pointSizeF():16)
                        .arg(fs[i].bold()?"font-weight:bold;":"").arg(fs[i].italic()?"font-style:italic;":"")
                        .arg(cs[i].name()).arg(esc(s.text.mid(i,j-i)));
                    if(!us[i].isEmpty()) sp=QString("<a href=\"%1\">%2</a>").arg(esc(safeHref(us[i])), sp);  // single-pass: don't re-scan the %-encoded URL
                    content+=sp; i=j; } }
            else content=esc(s.text);
            // SEO: publish the text inside a semantic <h1>/<h2>/<h3> when tagged (visually unchanged — inherits the div font).
            if(s.webHeading>=1 && s.webHeading<=3) content=QString("<h%1 style=\"margin:0;font:inherit;color:inherit\">%2</h%1>").arg(s.webHeading).arg(content);
            QString div=QString("  <div class=\"%1\" style=\"%2\"%3>%4</div>\n").arg(cls,style,pxAttr+respAttr(s),content);
            const QString tgt = s.webNewWindow ? " target=\"_blank\" rel=\"noopener\"" : QString();
            if(!s.url.isEmpty() && s.textRuns.isEmpty()) div=QString("  <a href=\"%1\"%2>%3</a>\n").arg(esc(safeHref(resolveUrl(s.url))), tgt, div.trimmed());
            // E-commerce: a Buy-Now button linking to the object's checkout URL (opens in a new tab).
            if(!s.webBuyUrl.isEmpty()){ QRectF bb=s.docBounds();
                div += QString("  <a class=\"buynow\" href=\"%1\" target=\"_blank\" rel=\"noopener\" style=\"position:absolute;left:%2px;top:%3px\">Buy&nbsp;Now</a>\n")
                    .arg(esc(safeHref(s.webBuyUrl))).arg(int(bb.left())).arg(int(bb.bottom())+4); }
            body+=psWrap(s,div);
        }
        // Contact-form widgets → a real <form> with typed inputs. Each webForm line is "Label|type";
        // type is whitelisted so a hostile spec can't inject an attribute, labels/values are escaped.
        for(const Shape* fp: forms){ const Shape& s=*fp; QRectF b=s.docBounds();
            QString fields; int fi=0;
            for(const QString& lineRaw: s.webForm.split('\n')){ const QString line=lineRaw.trimmed(); if(line.isEmpty()) continue;
                const QStringList parts=line.split('|'); const QString label=parts.value(0).trimmed(); if(label.isEmpty()) continue;
                QString type=parts.value(1).trimmed().toLower();
                static const QStringList allowed={"text","email","tel","number","textarea","url","date"};
                if(!allowed.contains(type)) type="text";
                const QString nm=QString("f%1").arg(fi++);
                fields += "<label>"+esc(label)+"</label>";
                if(type=="textarea") fields += "<textarea name=\""+nm+"\" rows=\"4\"></textarea>";
                else fields += "<input type=\""+type+"\" name=\""+nm+"\">";
            }
            const QString action = s.url.isEmpty()? QString("#") : esc(safeHref(s.url));
            QString ac=animCls(s.webAnim); if(!ac.isEmpty()||s.webAnim==7) anyAnim=true;
            const QString style=QString("left:%1px;top:%2px;width:%3px").arg(int(b.left())).arg(int(b.top()-pTop)).arg(int(qMax(80.0,b.width())));
            body+=psWrap(s,"  <form class=\"wform"+(ac.isEmpty()?QString():" "+ac)+"\" action=\""+action+"\" method=\"post\" style=\""+style+"\""+respAttr(s)+">\n"
                +fields+"<button type=\"submit\">Send</button>\n</form>\n");
        }
        // Web-embed objects: emit the author's raw embed HTML (iframe/video/map/form) at the object's slot.
        for(const Shape* ep: embeds){ const Shape& s=*ep; QRectF b=s.docBounds();
            QString ac=animCls(s.webAnim); if(!ac.isEmpty()||s.webAnim==7) anyAnim=true;
            QString adly=s.webAnimDelay>0?QString("transition-delay:%1s;").arg(s.webAnimDelay,0,'f',2):QString();
            QString pxAttr=s.webAnim==7?" data-parallax=\"0.3\"":"";
            body+=psWrap(s,QString("  <div class=\"%1\" style=\"position:absolute;left:%2px;top:%3px;width:%4px;height:%5px;overflow:hidden;%6\"%7>%8</div>\n")
                .arg(QString("embed")+(ac.isEmpty()?"":" "+ac)).arg(int(b.left())).arg(int(b.top()-pTop))
                .arg(int(qMax(1.0,b.width()))).arg(int(qMax(1.0,b.height()))).arg(adly).arg(pxAttr+respAttr(s)).arg(s.webEmbed));
        }
        // Embedded media: emit a native <video>/<audio> player (or animated <img> for a GIF) with the bytes
        // inlined as a data URI — self-contained, like the fonts and bitmaps this exporter already embeds.
        for(const Shape* mp: media){ const Shape& s=*mp; QRectF b=s.docBounds();
            const QString uri = "data:"+s.webMediaMime+";base64,"+QString::fromLatin1(s.webMediaData.toBase64());
            const QString pos = QString("position:absolute;left:%1px;top:%2px;width:%3px;height:%4px")
                .arg(int(b.left())).arg(int(b.top()-pTop)).arg(int(qMax(1.0,b.width()))).arg(int(qMax(1.0,b.height())));
            const QString rid=respAttr(s); QString el;
            if(s.webMediaMime.startsWith("video"))
                el=QString("<video controls playsinline%4 style=\"%1;object-fit:contain;background:#000\">"
                           "<source src=\"%2\" type=\"%3\">Your browser does not support the video tag.</video>")
                    .arg(pos, uri, esc(s.webMediaMime), rid);
            else if(s.webMediaMime.startsWith("audio"))
                el=QString("<audio controls%3 src=\"%1\" style=\"%2\"></audio>").arg(uri, pos, rid);
            else   // animated GIF or other image media
                el=QString("<img%4 src=\"%1\" alt=\"%2\" style=\"%3;object-fit:contain\">").arg(uri, escn(s.name), pos, rid);
            body+=psWrap(s,"  "+el+"\n");
        }
        // Build a nested <ul> dropdown from an indentation "Label | href" menu tree (2 spaces / 1 tab = one level deep).
        // Built by concatenation (no chained QString::arg), so user labels/hrefs can't trigger the %-rescan bug.
        auto menuHtml=[&](const QString& tree)->QString{
            struct MI{ int d; QString label, href; };
            QVector<MI> items; QVector<int> indentStack;   // widths of the open ancestors → logical depth (any consistent indent step)
            for(QString ln: tree.split('\n')){
                QString t=ln; t.replace('\t',QLatin1String("  "));
                int lead=0; while(lead<t.size() && t[lead]==' ') ++lead;
                const QString bd=t.mid(lead).trimmed(); if(bd.isEmpty()) continue;
                const int bar=bd.indexOf('|');
                const QString label = (bar<0? bd : bd.left(bar).trimmed());
                const QString href  = (bar<0? QString() : bd.mid(bar+1).trimmed());
                if(label.isEmpty()) continue;
                // logical depth = number of strictly-shallower open ancestors (so 2-space or 4-space or tab
                // indentation all nest correctly and equal-indent siblings share a depth)
                while(!indentStack.isEmpty() && lead<=indentStack.back()) indentStack.pop_back();
                items.push_back({indentStack.size(), label, href}); indentStack.push_back(lead); }
            if(items.isEmpty()) return QString();
            QString out; int prev=0; bool first=true;
            for(int k=0;k<items.size();++k){ int d=items[k].d;
                if(first){ out+="<ul>"; first=false; d=0; }
                else if(d>prev){ d=prev+1; out+="<ul>"; }          // clamp deepening to one level
                else if(d==prev){ out+="</li>"; }
                else { out+="</li>"; for(int j=0;j<prev-d;++j) out+="</ul></li>"; }
                const bool hasSub = (k+1<items.size() && items[k+1].d>d);
                const QString acls = hasSub? QStringLiteral(" class=\"has-sub\"") : QString();
                const MI& m=items[k];
                out += m.href.isEmpty()? ("<li><a"+acls+">"+esc(m.label)+"</a>")
                                       : ("<li><a"+acls+" href=\""+esc(safeHref(m.href))+"\">"+esc(m.label)+"</a>");   // safeHref: block javascript:/data:/… schemes (stored XSS)
                prev=d; }
            out+="</li>"; for(int j=0;j<prev;++j) out+="</ul></li>"; out+="</ul>";
            return out; };
        // Navigation bar: a styled <nav> at the object's slot linking every page (current page highlighted).
        for(const Shape* np: navbars){ const Shape& s=*np; QRectF b=s.docBounds();
            const QColor bg=(s.fill.type==FillType::Solid?s.fill.color:QColor(34,38,48));
            const bool lightBg = bg.lightnessF()>0.55; const QString fg = lightBg? "#1a1d24":"#eef1f7";
            // Position by the bar's Y offset WITHIN its own page, so it lands consistently on every page.
            const int navHome=qBound(0, int(b.center().y()/(PH+m_doc.pageGap)), m_doc.pageCount-1);
            const double navTop=b.top()-m_doc.pageRect(navHome).top();
            // stretch full width when the object is a stretchy web element, else honour its box
            QString box = (s.webStretch==1)? QString("left:0;right:0;top:%1px;height:%2px").arg(int(navTop)).arg(int(qMax(24.0,b.height())))
                                           : QString("left:%1px;top:%2px;width:%3px;height:%4px").arg(int(b.left())).arg(int(navTop)).arg(int(qMax(1.0,b.width()))).arg(int(qMax(24.0,b.height())));
            // Buttons & Menus: an authored dropdown menu tree replaces the auto page-links nav.
            if(!s.webMenuTree.isEmpty()){ const QString mh=menuHtml(s.webMenuTree); if(!mh.isEmpty()){
                body+=psWrap(s,"  <nav class=\"xmenu"+QString(s.webSticky?" sticky":"")+"\""+respAttr(s)
                      +" style=\""+box+";background:"+bg.name()+";color:"+fg+"\">"+mh+"</nav>\n");
                continue; } }
            QString links; for(int i=0;i<m_doc.pageCount;++i){
                const QString label = (i==0)? QStringLiteral("Home") : QString("Page %1").arg(i+1);
                links+=QString("<a href=\"%1\"%2>%3</a>").arg(pageFile(i), i==pi?" class=\"on\"":QString(), label); }
            body+=psWrap(s,QString("  <nav class=\"xnav%1\"%6 style=\"%2;background:%3;color:%4\">%5</nav>\n")
                .arg(s.webSticky?QStringLiteral(" sticky"):QString(), box, bg.name(), fg, links, respAttr(s)));  // single-pass: %-bearing hrefs in `links` not re-scanned
        }
        // Pop-up photos (lightbox): each is a clickable <img> that opens full-screen. The thumbnail is
        // the shape rendered at its on-page box (so cropping/effects match); the full view prefers the
        // native-resolution bitmap when the object is a placed photo.
        auto encodePng=[](const QImage& im)->QString{ if(im.isNull()) return QString();
            QByteArray png; QBuffer buf(&png); buf.open(QIODevice::WriteOnly);
            if(!im.save(&buf,"PNG")||png.isEmpty()) return QString();
            return "data:image/png;base64,"+QString::fromLatin1(png.toBase64()); };
        auto renderThumb=[&](const Shape& s)->QImage{ QRectF b=s.docBounds();
            int W=int(std::ceil(b.width())), H=int(std::ceil(b.height())); if(W<1||H<1) return QImage();
            QImage im(W,H,QImage::Format_ARGB32_Premultiplied); im.fill(Qt::transparent);
            QPainter g(&im); g.setRenderHint(QPainter::Antialiasing); g.setRenderHint(QPainter::SmoothPixmapTransform);
            g.translate(-b.left(),-b.top()); paintShape(g,s); g.end(); return im; };
        bool anyLb=false, vecChanged=false;
        for(const Shape* pp: photos){ const Shape& s=*pp; QRectF b=s.docBounds();
            QString thumbUri=encodePng(renderThumb(s));
            // Full view = the adjusted, cropped, *serialised* bitmap (fill.image), so the pop-up matches the
            // thumbnail's edits and is deterministic across save/reload (origImage is not persisted).
            QImage full=(s.fill.type==FillType::Bitmap && !s.fill.image.isNull())? s.fill.image : QImage();
            QString fullUri=encodePng(full); if(fullUri.isEmpty()) fullUri=thumbUri;
            if(thumbUri.isEmpty()){ vec.push_back(s); vecChanged=true; continue; }   // encode failed: fall back to the vector layer
            QString ac=animCls(s.webAnim); if(!ac.isEmpty()||s.webAnim==7) anyAnim=true;
            QString adly=s.webAnimDelay>0?QString("transition-delay:%1s;").arg(s.webAnimDelay,0,'f',2):QString();
            QString pxAttr=s.webAnim==7?" data-parallax=\"0.3\"":"";
            body+=psWrap(s,QString("  <a href=\"#\" class=\"%8\" data-full=\"%5\" onclick=\"return lbOpen(this)\" title=\"%6\""
                          " style=\"position:absolute;left:%1px;top:%2px;width:%3px;height:%4px;%9\"%10%11>"
                          "<img src=\"%7\" alt=\"%6\" style=\"width:100%;height:100%;object-fit:fill;cursor:zoom-in\"></a>\n")
                .arg(int(b.left())).arg(int(b.top()-pTop)).arg(int(qMax(1.0,b.width()))).arg(int(qMax(1.0,b.height())))
                .arg(fullUri).arg(escn(s.name)).arg(thumbUri).arg(QString("lb")+(ac.isEmpty()?"":" "+ac)).arg(adly).arg(pxAttr).arg(respAttr(s)));
            anyLb=true;
        }
        // Animated vector shapes: each rendered as its own inline <svg> in an absolutely-positioned wrapper
        // so it can reveal on scroll independently of the static background layer.
        for(const Shape* ap: anims){ const Shape& s=*ap; QRectF b=s.docBounds();
            if(b.width()<1||b.height()<1){ vec.push_back(s); vecChanged=true; continue; }  // sub-pixel (e.g. thin line): keep it in the bg SVG
            QByteArray asvg=writeSvg({s}, b); QString ai=QString::fromUtf8(asvg);
            ai.remove(QRegularExpression("<\\?xml[^>]*\\?>\\s*"));
            QString ac=animCls(s.webAnim); anyAnim=true;
            QString adly=s.webAnimDelay>0?QString("transition-delay:%1s;").arg(s.webAnimDelay,0,'f',2):QString();
            QString pxAttr=s.webAnim==7?" data-parallax=\"0.3\"":"";
            body+=psWrap(s,QString("  <div class=\"%1\" style=\"position:absolute;left:%2px;top:%3px;width:%4px;height:%5px;%6\"%7>%8</div>\n")
                .arg(QString("animwrap")+(ac.isEmpty()?"":" "+ac)).arg(int(b.left())).arg(int(b.top()-pTop))
                .arg(int(qMax(1.0,b.width()))).arg(int(qMax(1.0,b.height()))).arg(adly).arg(pxAttr+respAttr(s)).arg(ai));
        }
        // Image sliders/carousels: each group's slides share a frame (union of their bounds); every slide is
        // rendered at the frame size (so relative positions are preserved) and stacked to cross-fade.
        bool anySlider=false;
        for(auto it=sliderMap.begin(); it!=sliderMap.end(); ++it){
            const QVector<const Shape*>& slides=it.value();
            QRectF fr; for(const Shape* sp: slides) fr = fr.isNull()? sp->docBounds() : fr.united(sp->docBounds());
            int FW=int(std::ceil(fr.width())), FH=int(std::ceil(fr.height()));
            if(FW<1||FH<1){ for(const Shape* sp: slides){ vec.push_back(*sp); vecChanged=true; } continue; }  // degenerate frame: fall back to bg
            QString inner; int emitted=0;
            for(const Shape* sp: slides){ const Shape& s=*sp;
                QImage im(FW,FH,QImage::Format_ARGB32_Premultiplied); im.fill(Qt::transparent);
                { QPainter g(&im); g.setRenderHint(QPainter::Antialiasing); g.setRenderHint(QPainter::SmoothPixmapTransform);
                  g.translate(-fr.left(),-fr.top()); paintShape(g,s); }
                QString uri=encodePng(im); if(uri.isEmpty()) continue;
                inner+=QString("<div class=\"slide%1\"><img src=\"%2\" alt=\"%3\"></div>").arg(emitted==0?" on":"").arg(uri).arg(escn(s.name));
                ++emitted;
            }
            if(!emitted) continue;
            QString dots; for(int k=0;k<emitted;++k) dots+=QString("<i class=\"%1\"></i>").arg(k==0?"on":"");
            // Responsive: hide the whole slider when ANY slide asks to hide (largest webHideBelow governs).
            const Shape* hideSlide=slides.first(); for(const Shape* sp: slides) if(sp->webHideBelow>hideSlide->webHideBelow) hideSlide=sp;
            body+=psWrap(*slides.first(),QString("  <div class=\"slider\"%7 style=\"left:%1px;top:%2px;width:%3px;height:%4px\">%5"
                          "<button class=\"sl-nav sl-prev\" aria-label=\"Previous\">&#8249;</button>"
                          "<button class=\"sl-nav sl-next\" aria-label=\"Next\">&#8250;</button>"
                          "<div class=\"sl-dots\">%6</div></div>\n")
                .arg(int(fr.left())).arg(int(fr.top()-pTop)).arg(FW).arg(FH).arg(inner).arg(dots).arg(respAttr(*hideSlide)));
            anySlider=true;
        }
        // If a lightbox photo had to fall back into the vector layer, re-render the SVG including it.
        if(vecChanged){ svg=writeSvg(vec,QRectF(0,pTop,DW,PH)); svgInline=QString::fromUtf8(svg);
            svgInline.remove(QRegularExpression("<\\?xml[^>]*\\?>\\s*")); svgInline.replace("<svg ","<svg class=\"bg\" "); }
        // Presentation: split the vector background into one transparent full-stage SVG per layer, each a reveal
        // step. Off => the single combined background (byte-identical).
        QString stageBg = svgInline;
        if(m_doc.presentSteps){ stageBg.clear();
            QMap<int,QVector<Shape>> byL; for(const Shape& vs: vec) byL[vs.layer].push_back(vs);
            for(auto it=byL.begin(); it!=byL.end(); ++it){
                QByteArray ls=writeSvg(it.value(), QRectF(0,pTop,DW,PH));
                QString li=QString::fromUtf8(ls); li.remove(QRegularExpression("<\\?xml[^>]*\\?>\\s*")); li.replace("<svg ","<svg class=\"bg\" ");
                stageBg += "<div class=\"pstep\" data-step=\""+QString::number(it.key())+"\" style=\"position:absolute;inset:0\">"+li+"</div>\n"; } }
        QString lbHtml; if(anyLb) lbHtml=
            "<div id=\"lightbox\" style=\"display:none;position:fixed;inset:0;"
            "background:rgba(0,0,0,.85);z-index:9999;align-items:center;justify-content:center\">"
            "<span id=\"lbPrev\" style=\"position:absolute;left:18px;top:50%;transform:translateY(-50%);font-size:48px;color:#fff;cursor:pointer;user-select:none\">\xE2\x80\xB9</span>"
            "<img style=\"max-width:92%;max-height:92%;box-shadow:0 0 40px #000\">"
            "<span id=\"lbNext\" style=\"position:absolute;right:18px;top:50%;transform:translateY(-50%);font-size:48px;color:#fff;cursor:pointer;user-select:none\">\xE2\x80\xBA</span>"
            "<span id=\"lbClose\" style=\"position:absolute;right:20px;top:12px;font-size:30px;color:#fff;cursor:pointer\">\xC3\x97</span></div>\n"
            "<script>(function(){"
            "var gal=[].slice.call(document.querySelectorAll('[data-full]')).map(function(a){return a.getAttribute('data-full')||(a.querySelector('img')||{}).src;});"
            "var o=document.getElementById('lightbox'),img=o.querySelector('img'),idx=0;"
            "function show(i){if(!gal.length)return;idx=(i%gal.length+gal.length)%gal.length;img.src=gal[idx];o.style.display='flex';}"
            "window.lbOpen=function(a){var f=a.getAttribute('data-full');var i=gal.indexOf(f);show(i<0?0:i);return false;};"
            "var single=gal.length<2;document.getElementById('lbPrev').style.display=single?'none':'';document.getElementById('lbNext').style.display=single?'none':'';"
            "document.getElementById('lbPrev').onclick=function(e){e.stopPropagation();show(idx-1);};"
            "document.getElementById('lbNext').onclick=function(e){e.stopPropagation();show(idx+1);};"
            "document.getElementById('lbClose').onclick=function(){o.style.display='none';};"
            "o.onclick=function(e){if(e.target===o)o.style.display='none';};"
            "document.addEventListener('keydown',function(e){if(o.style.display!=='flex')return;"
            "if(e.key==='ArrowLeft')show(idx-1);else if(e.key==='ArrowRight')show(idx+1);else if(e.key==='Escape')o.style.display='none';});"
            "})();</script>\n";
        QString animJs; if(anyAnim) animJs=
            "<script>(function(){var els=document.querySelectorAll('.anim');"
            "if('IntersectionObserver' in window){var io=new IntersectionObserver(function(es){es.forEach(function(e){"
            "if(e.isIntersecting){e.target.classList.add('in');io.unobserve(e.target);}});},{threshold:0.15});"
            "els.forEach(function(e){io.observe(e);});}else{els.forEach(function(e){e.classList.add('in');});}"
            "var px=document.querySelectorAll('[data-parallax]');if(px.length){window.addEventListener('scroll',function(){"
            "var y=window.pageYOffset||document.documentElement.scrollTop;px.forEach(function(e){"
            "e.style.transform='translateY('+(y*parseFloat(e.getAttribute('data-parallax'))).toFixed(1)+'px)';});},{passive:true});}})();</script>\n";
        QString sliderJs; if(anySlider) sliderJs=
            "<script>(function(){document.querySelectorAll('.slider').forEach(function(sl){"
            "var s=sl.querySelectorAll('.slide'),d=sl.querySelectorAll('.sl-dots i'),i=0,n=s.length;if(!n)return;"
            "function go(k){s[i].classList.remove('on');if(d[i])d[i].classList.remove('on');i=(k+n)%n;s[i].classList.add('on');if(d[i])d[i].classList.add('on');}"
            "var p=sl.querySelector('.sl-prev'),x=sl.querySelector('.sl-next');"
            "if(p)p.onclick=function(){go(i-1);};if(x)x.onclick=function(){go(i+1);};"
            "d.forEach(function(t,j){t.onclick=function(){go(j);};});"
            "if(n>1)setInterval(function(){go(i+1);},4000);});})();</script>\n";
        // FlexGroups: FLUID reflow sections emitted OUTSIDE the fixed/scaled stage (the only place real
        // reflow works). Each item is rendered to a PNG; the flex row wraps as the real viewport narrows.
        QString flexHtml;
        for(auto it=flexMap.begin(); it!=flexMap.end(); ++it){
            QString items;
            for(const Shape* sp: it.value()){ const Shape& s=*sp; QRectF b=s.docBounds();
                int W=int(std::ceil(b.width())), H=int(std::ceil(b.height())); if(W<1||H<1) continue;
                QImage im(W,H,QImage::Format_ARGB32_Premultiplied); im.fill(Qt::transparent);
                { QPainter g(&im); g.setRenderHint(QPainter::Antialiasing); g.setRenderHint(QPainter::SmoothPixmapTransform);
                  g.translate(-b.left(),-b.top()); paintShape(g,s); }
                const QString uri=encodePng(im); if(uri.isEmpty()) continue;   // single-pass: uri(base64) & respAttr have no '%', name is escn'd
                items+=QString("<div class=\"flexitem\"%3><img src=\"%1\" alt=\"%2\"></div>").arg(uri, escn(s.name), respAttr(s));
            }
            if(!items.isEmpty()) flexHtml+="<section class=\"flexsection\">"+items+"</section>\n";
        }
        // Full-width background bands: each becomes a 100vw stripe positioned at its page-local Y range,
        // BEHIND the fixed content column (z-index 0). Solid → CSS colour; linear gradient → CSS gradient;
        // anything else → the shape rasterised as a stretched background image.
        QString bandsHtml; const bool pageHasBands = !bands.isEmpty();
        for(const Shape* bp: bands){ const Shape& s=*bp; QRectF b=s.docBounds();
            const int T=int(b.top()-pTop), H=int(qMax(1.0,b.height()));
            QString bg;
            if(s.fill.type==FillType::Solid) bg="background:"+s.fill.color.name()+";";
            else if(s.fill.type==FillType::Linear && s.fill.stops.size()>=2){
                QStringList cs; for(const GradStop& gs: s.fill.stops) cs<<(gs.color.name()+" "+QString::number(int(gs.pos*100))+"%");
                bg="background:linear-gradient(90deg,"+cs.join(",")+");"; }
            else { int W=int(std::ceil(b.width())), H2=int(std::ceil(b.height())); if(W<1)W=1; if(H2<1)H2=1;
                QImage im(W,H2,QImage::Format_ARGB32_Premultiplied); im.fill(Qt::transparent);
                { QPainter g(&im); g.setRenderHint(QPainter::Antialiasing); g.setRenderHint(QPainter::SmoothPixmapTransform);
                  g.translate(-b.left(),-b.top()); paintShape(g,s); }
                const QString uri=encodePng(im);
                bg = uri.isEmpty()? ("background:"+s.fill.representativeColor().name()+";") : ("background-image:url("+uri+");"); }
            bandsHtml+=QString("<div class=\"band\" style=\"top:%1px;height:%2px;%3\"%4></div>\n").arg(T).arg(H).arg(bg, respAttr(s));
        }
        // Named web anchors: each named object publishes a zero-size <a id="slug"> target at its position, so
        // links to "#slug" jump to it. Empty when nothing on the page is named → the export stays byte-identical.
        QString anchorsHtml; QSet<QString> usedAnchors;
        auto slugify=[](const QString& nm)->QString{ QString o; for(const QChar c: nm.toLower()){
                if(c.isLetterOrNumber()) o+=c; else if(c==QLatin1Char(' ')||c==QLatin1Char('-')||c==QLatin1Char('_')) o+=QLatin1Char('-'); }
            while(o.contains("--")) o.replace("--","-"); while(o.startsWith('-')) o.remove(0,1); while(o.endsWith('-')) o.chop(1); return o; };
        for(const Shape* np: named){ const Shape& s=*np; const QString id=slugify(s.name);
            if(id.isEmpty() || usedAnchors.contains(id)) continue; usedAnchors.insert(id);
            const QRectF b=s.docBounds();
            anchorsHtml+=QString("<a class=\"anchor\" id=\"%1\" style=\"position:absolute;left:%2px;top:%3px;width:0;height:0;scroll-margin-top:80px\"></a>\n")
                .arg(id).arg(int(b.left())).arg(int(b.top()-pTop)); }
        // Per-page SEO overrides the document-level metadata (falls back to it when a field is empty).
        const PageProps pp = ppFor(pi);
        const QString seoTitle = !pp.title.isEmpty()? pp.title : (m_doc.webTitle.isEmpty()? base : m_doc.webTitle);
        const QString seoDesc  = !pp.description.isEmpty()? pp.description : m_doc.webDescription;
        const QString seoKeys  = !pp.keywords.isEmpty()? pp.keywords : m_doc.webKeywords;
        QString seo;
        if(!seoDesc.isEmpty()) seo += "<meta name=\"description\" content=\""+esc(seoDesc)+"\">\n"
                                      "<meta property=\"og:description\" content=\""+esc(seoDesc)+"\">\n";
        if(!seoKeys.isEmpty())    seo += "<meta name=\"keywords\" content=\""+esc(seoKeys)+"\">\n";
        seo += "<meta property=\"og:title\" content=\""+esc(seoTitle)+"\">\n<meta property=\"og:type\" content=\"website\">\n";
        // Twitter card mirrors the Open Graph tags; author + canonical/og:url from the site's base URL.
        seo += "<meta name=\"twitter:card\" content=\"summary\">\n<meta name=\"twitter:title\" content=\""+esc(seoTitle)+"\">\n";
        if(!seoDesc.isEmpty()) seo += "<meta name=\"twitter:description\" content=\""+esc(seoDesc)+"\">\n";
        if(!m_doc.webAuthor.isEmpty())      seo += "<meta name=\"author\" content=\""+esc(m_doc.webAuthor)+"\">\n";
        if(!m_doc.webBaseUrl.isEmpty()){ QString bu=m_doc.webBaseUrl; if(!bu.endsWith('/')) bu+='/'; const QString pageUrl=bu+pageFile(pi);
            seo += "<link rel=\"canonical\" href=\""+esc(pageUrl)+"\">\n<meta property=\"og:url\" content=\""+esc(pageUrl)+"\">\n";
            if(!m_doc.favicon.isNull()) seo += "<meta property=\"og:image\" content=\""+esc(bu+"favicon.png")+"\">\n"; }
        if(!m_doc.favicon.isNull()) seo += "<link rel=\"icon\" type=\"image/png\" href=\"favicon.png\">\n";
        // Progressive enhancement: reveal-animated content is opacity:0 until the JS observer fires. If the
        // visitor has JavaScript disabled it would stay invisible, so un-hide it via <noscript>.
        const QString nojs = anyAnim? QString("<noscript><style>.anim{opacity:1!important;transform:none!important}</style></noscript>\n") : QString();
        const QString buyCss = ".buynow{display:inline-block;background:#2d6ff7;color:#fff;font:600 14px sans-serif;padding:8px 16px;border-radius:6px;text-decoration:none;box-shadow:0 2px 6px rgba(0,0,0,.25)}.buynow:hover{filter:brightness(1.08)}\n";
        // Client-side password gate (matches Xara's web password-protect — obfuscated, not cryptographically secure).
        QString pwGate; if(!m_doc.webPassword.isEmpty()){
            const QString enc = QString::fromLatin1(m_doc.webPassword.toUtf8().toBase64());
            pwGate = "<script>(function(){var k='"+enc+"';if(sessionStorage.getItem('xqpw')!==k){var a=prompt('Password:');"
                     "if(a===null||btoa(unescape(encodeURIComponent(a)))!==k){document.body.innerHTML='<p style=\\'font:16px sans-serif;padding:2em\\'>Password required.</p>';return;}"
                     "sessionStorage.setItem('xqpw',k);}})();</script>\n"; }
        QString html="<!DOCTYPE html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">\n"
            "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
            "<title>"+esc(seoTitle)+(m_doc.pageCount>1?QString(" — Page %1").arg(pi+1):QString())+"</title>\n"+seo+"<style>\n"+css+respCss+buyCss+"</style>\n"+nojs
            +(m_doc.webHeadHtml.isEmpty()?QString():m_doc.webHeadHtml+"\n")   // site-owner raw <head> HTML (analytics/verification) — emitted verbatim
            +"</head>\n<body>\n"+pwGate
            // A page with bands: white paper on .wrap, transparent .stage so bands show behind the content column.
            // No bands → both inline styles are empty and this is byte-identical to the classic single-column stage.
            +nav+"<div class=\"wrap\""+(pageHasBands?QString(" style=\"background:#fff\""):QString())+">"+bandsHtml
                +"<div class=\"stage\""+(pageHasBands?QString(" style=\"background:transparent\""):QString())+">\n"+stageBg+"\n"+anchorsHtml+body+"</div></div>\n"+flexHtml+lbHtml+animJs+sliderJs+fluidJs+presentJs+popupJs
            +(m_doc.webBodyHtml.isEmpty()?QString():m_doc.webBodyHtml+"\n")    // site-owner raw end-of-<body> HTML
            +"</body></html>\n";
        QFile f(dir+"/"+pageFile(pi)); if(!f.open(QIODevice::WriteOnly|QIODevice::Text)){ okAll=false; continue; }
        f.write(html.toUtf8()); f.close();
    }
    // Favicon file (if set).
    if(!m_doc.favicon.isNull()) m_doc.favicon.save(dir+"/favicon.png","PNG");
    // sitemap.xml + robots.txt (SEO): list every published page. Absolute URLs when a base URL is set.
    { QString bu=m_doc.webBaseUrl; if(!bu.isEmpty() && !bu.endsWith('/')) bu+='/';
      QString sm="<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<urlset xmlns=\"http://www.sitemaps.org/schemas/sitemap/0.9\">\n";
      for(int pi=0; pi<m_doc.pageCount; ++pi) sm+="  <url><loc>"+(bu+pageFile(pi)).toHtmlEscaped()+"</loc></url>\n";
      sm+="</urlset>\n";
      QFile sf(dir+"/sitemap.xml"); if(sf.open(QIODevice::WriteOnly|QIODevice::Text)){ sf.write(sm.toUtf8()); sf.close(); }
      QString rb="User-agent: *\nAllow: /\n"; if(!bu.isEmpty()) rb+="Sitemap: "+bu+"sitemap.xml\n";
      QFile rf(dir+"/robots.txt"); if(rf.open(QIODevice::WriteOnly|QIODevice::Text)){ rf.write(rb.toUtf8()); rf.close(); } }
    return okAll;
}

bool Canvas::exportSvg(const QString& path) {
    // Semantic SVG: each Shape → a native SVG element (rect/ellipse/path/text/g/image),
    // gradients into <defs> — editable + round-trips through the importer. (Live effects
    // like shadow/feather/bevel are not expressed; the geometry/fills/text are.)
    reflowText();
    QRectF b=m_doc.contentBounds(); if(b.isNull()) b=QRectF(QPointF(0,0),m_doc.pageSize); b.adjust(-8,-8,8,8);
    QByteArray svg = writeSvg(m_doc.shapes, b);
    QFile f(path); if(!f.open(QIODevice::WriteOnly)) return false;
    return f.write(svg) == svg.size();
}

// Encapsulated PostScript export (prepress interchange). Paths + solid/approximated fills + strokes, with
// doc(y-down) flipped to PS(y-up). Gradients approximate to their mid stop; live effects are not expressed.
bool Canvas::exportEps(const QString& path) {
    reflowText();
    QRectF b=m_doc.contentBounds(); if(b.isNull()) b=QRectF(QPointF(0,0),m_doc.pageSize); b.adjust(-8,-8,8,8);
    const double W=b.width(), H=b.height();
    auto X=[&](double x){ return x-b.left(); };
    auto Y=[&](double y){ return b.bottom()-y; };      // flip vertical: doc top → PS top
    QString ps;
    ps += "%!PS-Adobe-3.0 EPSF-3.0\n";
    ps += QString("%%%%BoundingBox: 0 0 %1 %2\n").arg(int(std::ceil(W))).arg(int(std::ceil(H)));
    ps += "%%Creator: XarQ\n%%EndComments\n";
    std::function<void(const Shape&)> emitPs=[&](const Shape& s){
        if(!s.visible) return;
        if(s.type==ShapeType::Group){ for(const Shape& ch: s.children) emitPs(ch); return; }
        const QPainterPath dp=Document::docPath(s);
        if(dp.elementCount()==0) return;
        ps += "newpath\n";
        for(int i=0;i<dp.elementCount();++i){ const QPainterPath::Element e=dp.elementAt(i);
            if(e.type==QPainterPath::MoveToElement) ps+=QString("%1 %2 moveto\n").arg(X(e.x),0,'f',2).arg(Y(e.y),0,'f',2);
            else if(e.type==QPainterPath::LineToElement) ps+=QString("%1 %2 lineto\n").arg(X(e.x),0,'f',2).arg(Y(e.y),0,'f',2);
            else if(e.type==QPainterPath::CurveToElement && i+2<dp.elementCount()){
                const QPainterPath::Element c2=dp.elementAt(i+1), en=dp.elementAt(i+2);
                ps+=QString("%1 %2 %3 %4 %5 %6 curveto\n").arg(X(e.x),0,'f',2).arg(Y(e.y),0,'f',2)
                     .arg(X(c2.x),0,'f',2).arg(Y(c2.y),0,'f',2).arg(X(en.x),0,'f',2).arg(Y(en.y),0,'f',2); i+=2; } }
        const bool doFill = s.fill.type!=FillType::None;
        const bool doStroke = s.stroke.enabled && s.stroke.width>0;
        QColor fc = s.fill.type==FillType::Solid? s.fill.color : (s.fill.stops.isEmpty()? s.fill.color : s.fill.stops[s.fill.stops.size()/2].color);
        auto setrgb=[&](const QColor& c){ ps+=QString("%1 %2 %3 setrgbcolor\n").arg(c.redF(),0,'f',3).arg(c.greenF(),0,'f',3).arg(c.blueF(),0,'f',3); };
        if(doFill && doStroke){ ps+="gsave\n"; setrgb(fc); ps+="fill\ngrestore\n"; setrgb(s.stroke.color); ps+=QString("%1 setlinewidth\nstroke\n").arg(s.stroke.width,0,'f',2); }
        else if(doFill){ setrgb(fc); ps+="fill\n"; }
        else if(doStroke){ setrgb(s.stroke.color); ps+=QString("%1 setlinewidth\nstroke\n").arg(s.stroke.width,0,'f',2); }
    };
    for(const Shape& s: m_doc.shapes) emitPs(s);
    ps += "showpage\n%%EOF\n";
    QFile f(path); if(!f.open(QIODevice::WriteOnly|QIODevice::Text)) return false;
    return f.write(ps.toLatin1()) > 0;
}
