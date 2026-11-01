// canvas_io.inc — persistence and export (included by canvas.cpp).
#include <QSvgGenerator>
#include <QPdfWriter>
#include <QPageSize>
#include <QFileInfo>
#include <QFile>
#include <QTextDocument>
#include <QEventLoop>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QXmlStreamReader>
#include <QPainter>
#include <dlfcn.h>
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
    m_doc.guidesH=m_guideHpos; m_doc.guidesV=m_guideVpos;
    if(!xara::saveXar(m_doc, path)) return false;
    m_path=path; return true;
}
bool Canvas::load(const QString& path) {
    Document d; d.pageSize=m_doc.pageSize;
    if(!xara::loadXar(d, path)) return false;
    setDocument(d, path); m_guideHpos=d.guidesH; m_guideVpos=d.guidesV; zoomFit(); return true;
}
// Write the current document to `path` WITHOUT retargeting the file path — used for autosave/recovery
// snapshots, which must not hijack the tab's real file identity.
bool Canvas::saveSnapshot(const QString& path) const { return xara::saveXar(m_doc, path); }
bool Canvas::saveCompressed(const QString& path) { m_doc.guidesH=m_guideHpos; m_doc.guidesV=m_guideVpos; if(!xara::saveXar(m_doc, path, true)) return false; m_path=path; return true; }

bool Canvas::exportPng(const QString& path, int scale) {
    reflowText();
    QRectF b = m_doc.contentBounds(); if(b.isNull()) b=QRectF(QPointF(0,0),m_doc.pageSize);
    b.adjust(-8,-8,8,8);
    if(qEnvironmentVariableIsSet("XARA_PAGEEXPORT")){ b=m_doc.pageRect(qEnvironmentVariableIntValue("XARA_PAGEINDEX")); scale=1; }  // diag: full-page @1x for oracle compare
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
// Doc-space rectangle for a given export area: 0=Page(chosen/current), 1=Drawing(content), 2=Selection.
QRectF Canvas::exportAreaRect(int areaMode, int pageIndex){
    QRectF b;
    if(areaMode==2){ b=selectionBoundsDoc(); if(b.isNull()) b=m_doc.contentBounds(); }         // Selection
    else if(areaMode==1){ b=m_doc.contentBounds(); if(!b.isNull()) b.adjust(-8,-8,8,8); }      // Drawing (content)
    else { const int pg=qBound(0, pageIndex>=0? pageIndex : currentPage(), qMax(0, m_doc.pageCount-1));
           b=m_doc.pageRect(pg); }                                                              // Page (chosen, else the current page — not always page 1)
    if(b.isNull() || b.width()<1 || b.height()<1) b=QRectF(QPointF(0,0), m_doc.pageSize);
    return b;
}
// Render an export area to an ARGB image (no format encoding). maxDim>0 caps the longest side so the
// Export dialog's live preview stays responsive; the real export passes 0 (full resolution).
QImage Canvas::exportRasterImage(int areaMode, double scale, bool transparent, int pageIndex, int maxDim){
    reflowText();
    const QRectF b = exportAreaRect(areaMode, pageIndex);
    if(scale<=0) scale=1;
    int W=qMax(1,int(b.width()*scale)), H=qMax(1,int(b.height()*scale));
    const qint64 cap=64LL*1024*1024;                                                            // pixel cap → avoid runaway allocations
    if(qint64(W)*H>cap){ double f=std::sqrt(double(cap)/(double(W)*double(H))); W=qMax(1,int(W*f)); H=qMax(1,int(H*f)); }
    if(maxDim>0 && qMax(W,H)>maxDim){ double f=double(maxDim)/qMax(W,H); W=qMax(1,int(W*f)); H=qMax(1,int(H*f)); }
    QImage img(QSize(W,H), QImage::Format_ARGB32_Premultiplied);
    img.fill(transparent? Qt::transparent : Qt::white);
    QPainter g(&img); g.setRenderHint(QPainter::Antialiasing); g.setRenderHint(QPainter::SmoothPixmapTransform);
    g.scale(double(W)/b.width(), double(H)/b.height()); g.translate(-b.topLeft());
    if(hasTextFields()||hasTextChains()){ Document rd=resolvedForRender(); paintDocument(g, rd); } else paintDocument(g, m_doc);
    g.end();
    return img;
}
bool Canvas::exportRaster(const QString& path, int areaMode, double scale, int quality, bool transparent, bool indexed, bool dither, int pageIndex){
    QImage img = exportRasterImage(areaMode, scale, transparent, pageIndex, 0);
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
    // Aggregate decompression-bomb guard: the per-entry 64 MB cap alone lets a small archive with many
    // central-directory records (optionally all pointing at one local file) expand to terabytes of RAM.
    qint64 totalDec=0; const qint64 kZipAggCap=256LL*1024*1024;
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
        if(totalDec + dec.size() > kZipAggCap) break;    // stop before the cumulative decoded size runs away
        totalDec += dec.size();
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

// A layered document read by one of the paint-app importers: an empty document takes its page; otherwise
// it comes in as one group, scaled to fit the page. `note` is added to the status line.
bool Canvas::adoptLayers(const Document& pd, const QString& path, const char* undo, const QString& note){
    if(pd.shapes.isEmpty()) return false;
    const QString fileName=QFileInfo(path).fileName();
    const QString noteText=note.isEmpty()?QString():QStringLiteral(" %1.").arg(note);
    if(m_doc.shapes.isEmpty()){
        m_doc.pageSize=pd.pageSize; m_doc.pageCount=1;
        warnMissingFonts(pd.shapes);
        if(pd.shapes.size()>1){ Shape g; g.type=ShapeType::Group; g.name=fileName; g.children=pd.shapes; addShapes({g}, undo); }
        else addShapes(pd.shapes, undo);
        zoomFit();
        emit statusMessage(QStringLiteral("Imported %1.%2").arg(fileName,noteText));
        return true;
    }
    QRectF b; for(const Shape& s: pd.shapes) b = b.isNull()? s.docBounds() : b.united(s.docBounds());
    if(b.isNull()||b.width()<1||b.height()<1) return false;
    const double sc=qMin(1.0, qMin(m_doc.pageSize.width()*0.9/b.width(), m_doc.pageSize.height()*0.9/b.height()));
    const QPointF pc(m_doc.pageSize.width()/2, m_doc.pageSize.height()/2), bc=b.center();
    QTransform place; place.translate(pc.x(),pc.y()); place.scale(sc,sc); place.translate(-bc.x(),-bc.y());
    Shape g; g.type=ShapeType::Group; g.name=fileName; g.children=pd.shapes; g.transform=place;
    addShapes({g}, undo);
    emit statusMessage(QStringLiteral("Imported %1.%2").arg(fileName,noteText));
    return true;
}
bool Canvas::importPsp(const QString& path){
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false; const QByteArray data=f.readAll(); f.close();
    PspImportStats st;
    const Document pd=importPspDocument(data,&st);
    QStringList notes;
    if(st.skippedText) notes<<QStringLiteral("%1 text object(s) not read").arg(st.skippedText);
    if(st.skippedShapes) notes<<QStringLiteral("%1 ellipse or polygon shape(s) not read").arg(st.skippedShapes);
    if(st.unsupportedPaints) notes<<QStringLiteral("%1 gradient or pattern paint(s) not read").arg(st.unsupportedPaints);
    if(st.adjustments) notes<<QStringLiteral("%1 adjustment layer(s) not applied").arg(st.adjustments);
    if(st.maskLayers) notes<<QStringLiteral("%1 mask layer(s) not applied").arg(st.maskLayers);
    if(st.artMedia) notes<<QStringLiteral("%1 art media layer(s) not read").arg(st.artMedia);
    if(st.groups) notes<<QStringLiteral("%1 layer group(s) flattened into the layer list").arg(st.groups);
    if(adoptLayers(pd,path,"Import PaintShop Pro",notes.join(QStringLiteral(", ")))) return true;
    emit statusMessage("No readable layers found in the PaintShop Pro file."); return false;
}
bool Canvas::importClip(const QString& path){
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false; const QByteArray data=f.readAll(); f.close();
    ClipImportStats st;
    const Document pd=importClipDocument(data,&st);
    QStringList notes;
    if(st.skippedCorrections) notes<<QStringLiteral("%1 correction layer(s) not applied").arg(st.skippedCorrections);
    if(st.tones) notes<<QStringLiteral("%1 screen tone(s) drawn as even shade").arg(st.tones);
    if(adoptLayers(pd,path,"Import Clip Studio",notes.join(QStringLiteral(", ")))) return true;
    const QImage pv=clipPreview(data);
    if(!pv.isNull()){
        Document one; one.pageSize=pv.size(); one.shapes.push_back(psdBitmapShape(pv,0,0,QFileInfo(path).fileName()));
        return adoptLayers(one,path,"Import Clip Studio",QStringLiteral("Only the file's saved preview could be read"));
    }
    emit statusMessage("No readable layers found in the Clip Studio file."); return false;
}
bool Canvas::importImage(const QString& path) {
    if(path.endsWith(".apng",Qt::CaseInsensitive) || path.endsWith(".png",Qt::CaseInsensitive)){
        QFile af(path); if(af.open(QIODevice::ReadOnly)){ const QByteArray head=af.read(1<<16); af.close();
            if(head.contains(QByteArray("acTL")) && importApng(path)) return true; } }   // animated PNG → frames
    if(path.endsWith(".psd",Qt::CaseInsensitive) || path.endsWith(".psb",Qt::CaseInsensitive)){
        const bool psb=path.endsWith(".psb",Qt::CaseInsensitive);   // Photoshop "Large Document Format"
        QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false;
        PsdImportStats st;
        Document pd=importPsdDocument(f.readAll(),&st); if(pd.shapes.isEmpty()) return false;
        const QString fileName=QFileInfo(path).fileName();
        QStringList notes;
        if(st.rasterText) notes<<QStringLiteral("%1 text layer(s) kept as pixels (font missing or warped)").arg(st.rasterText);
        if(st.skippedAdjustments) notes<<QStringLiteral("%1 colour lookup(s) in an unsupported format not applied").arg(st.skippedAdjustments);
        const QString noteText=notes.isEmpty()?QString():QStringLiteral(" %1.").arg(notes.join(QStringLiteral(", ")));
        if(m_doc.shapes.isEmpty()){       // an empty document takes the file's page, like every other vector import
            m_doc.pageSize=pd.pageSize; m_doc.pageCount=1;
            warnMissingFonts(pd.shapes);
            m_guideHpos+=st.guidesH; m_guideVpos+=st.guidesV;
            if(pd.shapes.size()>1){ Shape g; g.type=ShapeType::Group; g.name=fileName; g.children=pd.shapes; addShapes({g}, psb?"Import PSB":"Import PSD"); }
            else addShapes(pd.shapes, psb?"Import PSB":"Import PSD");
            zoomFit();
            emit statusMessage(QStringLiteral("Imported %1.%2").arg(fileName,noteText));
            return true;
        }
        QRectF b; for(const Shape& s: pd.shapes) b = b.isNull()? s.docBounds() : b.united(s.docBounds());
        if(b.isNull()||b.width()<1||b.height()<1) return false;
        const double maxW=m_doc.pageSize.width()*0.9, maxH=m_doc.pageSize.height()*0.9;
        const double sc=qMin(1.0, qMin(maxW/b.width(), maxH/b.height()));
        const QPointF pc(m_doc.pageSize.width()/2, m_doc.pageSize.height()/2), bc=b.center();
        QTransform place; place.translate(pc.x(),pc.y()); place.scale(sc,sc); place.translate(-bc.x(),-bc.y());
        const char* lbl = psb ? "Import PSB" : "Import PSD";
        if(pd.shapes.size()>1){ Shape g; g.type=ShapeType::Group; g.name=QFileInfo(path).fileName(); g.children=pd.shapes; g.transform=place;
            addShapes({g}, lbl); }
        else { Shape s=pd.shapes[0]; s.name=QFileInfo(path).fileName(); s.transform = s.transform * place; addShapes({s}, lbl); }
        return true;
    }
    QImage img;
    // Whole-slide microscopy (.svs/.ndpi/…): prefer OpenSlide's bounded pyramid overview over Qt's TIFF
    // plugin, which would otherwise try to read the full gigapixel base level of a real slide.
    { static const char* const slideExt[]={".svs",".ndpi",".scn",".mrxs",".vms",".vmu",".svslide",".bif"};
      for(const char* e : slideExt) if(path.endsWith(QLatin1String(e),Qt::CaseInsensitive)){ img=xara::ExtCodec::decode(path); break; } }
    if(img.isNull()) img = (xara::Raw::isRawFile(path) && xara::Raw::available()) ? xara::Raw::decode(path) : QImage(path);
    if(img.isNull()) img = xara::RasterExtra::decode(path);   // native decoders for formats Qt can't read (QOI/PCX/SGI/HDR/PFM/PAM/DDS)
    if(img.isNull()) img = xara::ExtCodec::decode(path);       // runtime-optional libs: JPEG 2000 (OpenJPEG), JPEG XL (libjxl)
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
    m_doc.layers.clear(); m_doc.shapes.clear();   // a GIF animation replaces the document — clear old artwork too, else it bleeds onto the frame-layers
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

// Import a FictionBook .fb2 (or a zipped .fb2.zip/.fbz): a single XML file whose <body> holds the
// prose as <section>/<p>. Flattened into an editable wrapping text frame, like importDocx. The
// <binary> elements (base64 cover/illustrations, often tens of MB) are skipped wholesale.
bool Canvas::importFb2(const QString& path){
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false;
    QByteArray data=f.readAll(); f.close();
    if(data.size()>=2 && data[0]=='P' && data[1]=='K'){            // .fb2.zip / .fbz wrapper
        const QMap<QString,QByteArray> zip=readZipEntries(data);
        QByteArray inner;
        for(auto it=zip.begin(); it!=zip.end(); ++it)
            if(it.key().endsWith(".fb2",Qt::CaseInsensitive)){ inner=it.value(); break; }
        if(inner.isEmpty() && !zip.isEmpty()) inner=zip.begin().value();
        if(inner.isEmpty()){ emit statusMessage("Not a readable FictionBook archive."); return false; }
        data=inner;
    }
    // Feed the RAW bytes to QXmlStreamReader — FB2 is commonly encoding="windows-1251", and the
    // declaration must drive the codec (converting to QString up front would corrupt it).
    QString title, text; bool inBody=false;
    QXmlStreamReader r(data); r.setNamespaceProcessing(false);
    while(!r.atEnd()){ r.readNext();
        if(r.isStartElement()){
            const QString el=r.name().toString().section(':',-1);
            if(el=="binary"){ r.skipCurrentElement(); continue; }   // base64 image payloads: never read
            // IncludeChildElements: a <book-title> containing inline markup would otherwise raise
            // ErrorOnUnexpectedElement and abort the whole parse, losing the entire book.
            if(el=="book-title" && title.isEmpty()){ title=r.readElementText(QXmlStreamReader::IncludeChildElements).simplified(); continue; }
            if(el=="body") inBody=true;
            else if(inBody && (el=="empty-line")) text+='\n';
        } else if(r.isEndElement()){
            const QString el=r.name().toString().section(':',-1);
            if(el=="body") inBody=false;
            else if(inBody && (el=="p"||el=="title"||el=="subtitle"||el=="section"||el=="v")) text+='\n';
        } else if(r.isCharacters() && inBody) text += r.text();     // keep whitespace runs: they separate inline markup
    }
    text.replace(QRegularExpression("[ \\t]+")," ");
    text.replace(QRegularExpression("\n{3,}"),"\n\n");
    text=text.trimmed();
    if(text.isEmpty()){ emit statusMessage("FictionBook contained no readable text."); return false; }
    if(!title.isEmpty()) text = title + "\n\n" + text;
    Shape s; s.type=ShapeType::Text; s.text=text; s.textFrame=true;
    s.name = title.isEmpty()? QFileInfo(path).fileName() : title;
    s.fill.type=FillType::Solid; s.fill.color=Qt::black; s.stroke.enabled=false;
    const double w=qMin(m_doc.pageSize.width()*0.8, 520.0);
    s.rect=QRectF(0,0,w,qMax(80.0,m_doc.pageSize.height()*0.8));
    s.transform=QTransform::fromTranslate(m_doc.pageSize.width()*0.1, m_doc.pageSize.height()*0.08);
    addShapes({s},"Import FictionBook");
    emit statusMessage("Imported FictionBook text.");
    return true;
}
// Reconstruct a QImage from a CF_DIB payload (a BITMAPINFOHEADER + palette + pixels, with NO 14-byte
// BITMAPFILEHEADER — as stored in a RIFF DISP chunk). We synthesise the file header so QImage can decode it.
static QImage cdrDibToImage(const QByteArray& dib){
    if(dib.size()<40) return QImage();
    const uchar* p=(const uchar*)dib.constData();
    auto u16=[&](int o)->quint32{ return (quint32)(p[o] | (p[o+1]<<8)); };
    auto u32=[&](int o)->quint32{ return (quint32)(p[o] | (p[o+1]<<8) | (p[o+2]<<16) | ((quint32)p[o+3]<<24)); };
    const quint32 biSize=u32(0);
    if(biSize<40 || biSize>124 || (int)biSize>dib.size()) return QImage();   // BITMAPINFOHEADER..V5
    const int bpp=(int)u16(14); const quint32 comp=u32(16); const quint32 clrUsed=u32(32);
    quint32 clr = (bpp<=8) ? (clrUsed ? clrUsed : (1u<<bpp)) : clrUsed;
    quint32 colorBytes = clr*4 + ((comp==3 && biSize==40) ? 12u : 0u);       // BI_BITFIELDS masks follow a v3 header
    const quint32 off = 14u + biSize + colorBytes;
    const quint32 fileSize = 14u + (quint32)dib.size();
    QByteArray bmp; bmp.reserve(14+dib.size());
    auto le32=[&](quint32 v){ char b[4]={char(v&0xff),char((v>>8)&0xff),char((v>>16)&0xff),char((v>>24)&0xff)}; bmp.append(b,4); };
    bmp.append("BM",2); le32(fileSize); le32(0); le32(off); bmp.append(dib);
    QImage im; im.loadFromData(bmp, "BMP"); return im;
}
// Find the first RIFF chunk with the given 4-char id (walking into nested RIFF/LIST containers).
static QByteArray cdrRiffChunk(const QByteArray& d, const char id[4]){
    const int n=d.size(); if(n<12 || qstrncmp(d.constData(),"RIFF",4)!=0) return QByteArray();
    const uchar* p=(const uchar*)d.constData();
    auto u32=[&](int o)->quint32{ return (o>=0 && o+3<n)? (quint32)(p[o]|(p[o+1]<<8)|(p[o+2]<<16)|((quint32)p[o+3]<<24)) : 0u; };
    QByteArray found;
    std::function<void(int,int,int)> walk=[&](int off,int end,int depth){
        if(depth>64) return;                            // bound recursion: deeply nested LIST containers must not overflow the stack
        while(off+8<=end && found.isEmpty()){
            const char* cid=d.constData()+off; const quint32 size=u32(off+4); const int body=off+8;
            if(body+(qint64)size>end) break;
            if(qstrncmp(cid,id,4)==0){ found=QByteArray(d.constData()+body,(int)size); return; }
            if(qstrncmp(cid,"RIFF",4)==0 || qstrncmp(cid,"LIST",4)==0) walk(body+4, body+(int)size, depth+1);
            off = body + (int)size + ((int)size & 1);   // chunks are word-aligned
        }
    };
    walk(12, qMin(n, 8+(int)u32(4)), 0);
    return found;
}
// Best-effort CorelDRAW .cdr import: real .cdr vector parsing is proprietary/huge, so we extract the
// embedded preview bitmap and place it as a photo placeholder. Order of preference:
//   1) RIFF .cdr (v7–X5): the CF_DIB thumbnail in the DISP chunk — the document's own preview;
//   2) newer ZIP-wrapped .cdr (X6+): previews/thumbnail.png|jpg;
//   3) last resort: any standalone JPEG/PNG embedded in the bytes.
bool Canvas::importCdr(const QString& path){
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray data=f.readAll(); f.close();
    const QString name=QFileInfo(path).fileName();
    // A compressed exchange file (.ccx, .cpx, and any .cdx that turns out to be one) is CMX underneath.
    if(data.size()>=12 && data.mid(8,4)=="CDRX" && importCmx(path)) return true;

    // A .cdr can take a second or two to parse. Unlike our other importers, libcdr is single-threaded and is
    // NOT safe to run on a worker thread (it hangs off the GUI thread), so the parse blocks the UI. To keep
    // the app from ever *looking* hung/crashed, show a wait cursor + a status message and PAINT them before
    // the blocking parse. Pathological/malformed files are bounded by the riffDepthOk pre-check inside
    // cdrToSvgPages + the RIFF/embedded-scan caps, so this can't spin forever.
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    struct CursorGuard { ~CursorGuard(){ QGuiApplication::restoreOverrideCursor(); } } _cursorGuard;
    emit statusMessage(QStringLiteral("Importing CorelDRAW file… %1").arg(name));
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);   // let the cursor + message paint first

    // Preferred: editable vectors via libcdr (parses .cdr → SVG → our SVG importer). No-op without libcdr.
    if(xara::cdrVectorAvailable()){
        const QStringList pages = xara::cdrToSvgPages(data);
        if(!pages.isEmpty()){
            QVector<Shape> imported;
            if(pages.size()==1){ QString err; imported = parseSvg(pages[0].toUtf8(), &err, QString()); }
            else {   // multi-page .cdr: stack the pages vertically so nothing overlaps
                double stackTop=0;
                for(const QString& svg : pages){ QString err; QVector<Shape> sh=parseSvg(svg.toUtf8(), &err, QString());
                    if(sh.isEmpty()) continue;
                    QRectF b; for(const Shape& s : sh){ const QRectF db=s.docBounds(); b=b.isNull()?db:b.united(db); }
                    if(!b.isNull()){ const QTransform t=QTransform::fromTranslate(0, stackTop-b.top()); for(Shape& s:sh) s.transform=s.transform*t; stackTop+=b.height()+60; }
                    imported += sh; }
            }
            if(!imported.isEmpty()){
                addShapes(imported, "Import CDR");
                zoomFit();
                emit statusMessage(QStringLiteral("Imported CorelDRAW vectors — %1 objects%2 (via libcdr).")
                    .arg(imported.size()).arg(pages.size()>1?QStringLiteral(", %1 pages").arg(pages.size()):QString()));
                return true;
            }
        }
        // libcdr present but this file yielded no vectors → fall through to the preview extractor.
    }
    // Preview-bitmap fallback: DISP DIB / zip thumbnail / embedded JPEG-PNG.
    QImage best;
    auto considerImg=[&](const QImage& im){ if(!im.isNull()
        && (qint64)im.width()*im.height() > (qint64)best.width()*best.height()) best=im; };
    auto consider=[&](const QByteArray& b){ QImage im; if(im.loadFromData(b)) considerImg(im); };
    if(data.size()>=12 && qstrncmp(data.constData(),"RIFF",4)==0){
        const QByteArray disp=cdrRiffChunk(data,"DISP");
        if(disp.size()>4) considerImg(cdrDibToImage(disp.mid(4)));
    }
    if(best.isNull()){ const QMap<QString,QByteArray> zip=readZipEntries(data);
        if(!zip.isEmpty()) for(auto it=zip.begin(); it!=zip.end(); ++it){ const QString n=it.key().toLower();
            if(n.contains("preview")||n.contains("thumbnail")||n.endsWith(".png")||n.endsWith(".jpg")||n.endsWith(".jpeg")) consider(it.value()); } }
    if(best.isNull()){   // last resort — scan for an embedded standalone JPEG (FFD8FF..FFD9) / PNG, decode-capped
        const QByteArray png=QByteArray::fromHex("89504E470D0A1A0A"), jpg=QByteArray::fromHex("FFD8FF");
        int tries=0; const int MAX_TRIES=64;
        for(int i=data.indexOf(png); i>=0 && tries<MAX_TRIES; i=data.indexOf(png,i+8)){ ++tries; consider(data.mid(i, qMin(data.size()-i, 8<<20))); }
        for(int i=data.indexOf(jpg); i>=0 && tries<MAX_TRIES; i=data.indexOf(jpg,i+3)){ ++tries; consider(data.mid(i, qMin(data.size()-i, 8<<20))); }
    }
    if(best.isNull()){ emit statusMessage("No importable preview found in the .cdr (vector .cdr parsing is unsupported)."); return false; }
    const double W=best.width(), H=best.height();
    const double sc=qMin(1.0, qMin(m_doc.pageSize.width()*0.9/W, m_doc.pageSize.height()*0.9/H));
    Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,W,H); s.name=name+" (preview)";
    s.fill.type=FillType::Bitmap; s.fill.image=best; s.fill.origImage=best; s.fill.tiled=false;
    s.fill.p0=QPointF(0,H); s.fill.p1=QPointF(W,H); s.fill.p2=QPointF(0,0); s.stroke.enabled=false;
    s.transform=QTransform::fromScale(sc,sc)*QTransform::fromTranslate((m_doc.pageSize.width()-W*sc)/2,(m_doc.pageSize.height()-H*sc)/2);
    addShapes({s},"Import CDR preview");
    emit statusMessage("Imported the .cdr preview bitmap (vector content isn't parsed).");
    return true;
}
// Shared consumer for the librevenge-backed vector importers (WPG/Visio/FreeHand/Publisher): take one
// standard-SVG string per page, run each through parseSvg, stack multi-page documents vertically so
// nothing overlaps, and add the whole lot as a single undo-able import. Mirrors importCdr's vector path.
bool Canvas::importVectorSvgPages(const QStringList& pages, const QString& undoLabel,
                                  const QString& statusVerb, const QString& viaLib){
    if(pages.isEmpty()) return false;

    // The page we adopt must be the VIEWPORT the content is laid out in, not the viewBox extent:
    // parseSvg maps viewBox -> width/height, and librevenge writes those in DIFFERENT units (width in
    // inches, viewBox in points), so taking the viewBox would size the page 33% — or, for a WPG, 21x —
    // wrong. Only adopt into an EMPTY document; File > Import into an existing drawing is unchanged.
    const bool adoptPage = m_doc.shapes.isEmpty();
    QSizeF pageSz = svgViewportSize(pages[0].toUtf8());
    if(!(pageSz.width() > 1.0 && pageSz.height() > 1.0 && pageSz.width() < 1e7 && pageSz.height() < 1e7))
        pageSz = QSizeF();

    QVector<Shape> imported;
    if(pages.size()==1){ QString err; imported = parseSvg(pages[0].toUtf8(), &err, QString()); }
    else if(adoptPage && !pageSz.isEmpty()){
        // Real pages: lay each one out on ITS OWN XarQ page, using the same advance Document::pageRect
        // uses. Re-basing to each page's content bounding box (what we used to do) both destroys the
        // page margins and drifts out of step with the page rects, so page 2's artwork lands on page 1.
        const double adv = pageSz.height() + m_doc.pageGap;
        for(int i = 0; i < pages.size(); ++i){
            QString err; QVector<Shape> sh = parseSvg(pages[i].toUtf8(), &err, QString());
            if(sh.isEmpty()) continue;                       // a blank page still consumes its slot
            const QTransform t = QTransform::fromTranslate(0, i * adv);
            for(Shape& s : sh) s.transform = s.transform * t;
            imported += sh;
        }
    }
    else {   // importing into an existing drawing: stack by content so nothing overlaps what is there
        double stackTop=0;
        for(const QString& svg : pages){ QString err; QVector<Shape> sh=parseSvg(svg.toUtf8(), &err, QString());
            if(sh.isEmpty()) continue;
            QRectF b; for(const Shape& s : sh){ const QRectF db=s.docBounds(); b=b.isNull()?db:b.united(db); }
            if(!b.isNull()){ const QTransform t=QTransform::fromTranslate(0, stackTop-b.top()); for(Shape& s:sh) s.transform=s.transform*t; stackTop+=b.height()+60; }
            imported += sh; }
    }
    if(imported.isEmpty()) return false;
    // Every format that comes through this bridge can name fonts the machine does not have; the
    // warning used to fire only for a directly-opened .svg, so an Affinity or CDR document silently
    // reflowed in a substitute face with no explanation.
    warnMissingFonts(imported);

    // This bridge only ever called addShapes(), so every library-routed import (Affinity, CDR, Visio,
    // PageMaker, ODF, iWork…) landed on a default A4 page no matter what the file said — an
    // 18773x4080 banner included — while the status line still reported "N pages".
    if(adoptPage && !pageSz.isEmpty()){
        // Sanity-check the declared page against what actually came out. Some librevenge writers emit
        // a viewport wildly inconsistent with their own content (a .wpg here declares 5x4 units for
        // artwork 82x61 units across); adopting that would put the whole drawing off the page. Keep
        // the default rather than a page the source itself contradicts.
        QRectF b;
        for(const Shape& s2 : imported){ const QRectF db=s2.docBounds(); b = b.isNull()? db : b.united(db); }
        const bool sane = b.isNull()
                       || (b.width() <= pageSz.width()*8.0 && b.height() <= pageSz.height()*8.0*qMax(1,pages.size()));
        if(sane){
            m_doc.pageSize  = pageSz;
            m_doc.pageCount = qBound(1, pages.size(), 4096);
        }
    }
    addShapes(imported, undoLabel);
    zoomFit();
    emit statusMessage(QStringLiteral("%1 — %2 objects%3 (via %4).")
        .arg(statusVerb).arg(imported.size())
        .arg(pages.size()>1?QStringLiteral(", %1 pages").arg(pages.size()):QString()).arg(viaLib));
    return true;
}

// WordPerfect Graphics / Visio / FreeHand / Publisher — editable vectors via their DLP libraries
// (libwpg/libvisio/libfreehand/libmspub) → librevenge SVG → our SVG importer. Like libcdr these parsers
// are single-threaded and block the GUI, so we show a wait cursor + status and paint them before the
// blocking parse. Each is a no-op (reports unsupported) when its -dev package wasn't present at build.
bool Canvas::importWpg(const QString& path){
    if(!xara::wpgVectorAvailable()){ emit statusMessage("WordPerfect Graphics import needs libwpg (not built in)."); return false; }
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false; const QByteArray data=f.readAll(); f.close();
    emit statusMessage(QStringLiteral("Importing WordPerfect Graphics… %1").arg(QFileInfo(path).fileName()));
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    struct CursorGuard{ ~CursorGuard(){ QGuiApplication::restoreOverrideCursor(); } } _cg;
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);   // paint the cursor + message before the blocking parse
    if(importVectorSvgPages(xara::wpgToSvgPages(data), QStringLiteral("Import WPG"),
                            QStringLiteral("Imported WordPerfect Graphics"), QStringLiteral("libwpg"))) return true;
    emit statusMessage("No importable vector content found in the WPG file."); return false;
}
bool Canvas::importVisio(const QString& path){
    if(!xara::visioVectorAvailable()){ emit statusMessage("Visio import needs libvisio (not built in)."); return false; }
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false; const QByteArray data=f.readAll(); f.close();
    emit statusMessage(QStringLiteral("Importing Visio drawing… %1").arg(QFileInfo(path).fileName()));
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    struct CursorGuard{ ~CursorGuard(){ QGuiApplication::restoreOverrideCursor(); } } _cg;
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    if(importVectorSvgPages(xara::visioToSvgPages(data), QStringLiteral("Import Visio"),
                            QStringLiteral("Imported Visio drawing"), QStringLiteral("libvisio"))) return true;
    emit statusMessage("No importable vector content found in the Visio file."); return false;
}
bool Canvas::importFreehand(const QString& path){
    if(!xara::freehandVectorAvailable()){ emit statusMessage("FreeHand import needs libfreehand (not built in)."); return false; }
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false; const QByteArray data=f.readAll(); f.close();
    emit statusMessage(QStringLiteral("Importing FreeHand drawing… %1").arg(QFileInfo(path).fileName()));
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    struct CursorGuard{ ~CursorGuard(){ QGuiApplication::restoreOverrideCursor(); } } _cg;
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    if(importVectorSvgPages(xara::freehandToSvgPages(data), QStringLiteral("Import FreeHand"),
                            QStringLiteral("Imported FreeHand drawing"), QStringLiteral("libfreehand"))) return true;
    emit statusMessage("No importable vector content found in the FreeHand file."); return false;
}
bool Canvas::importMsPub(const QString& path){
    if(!xara::mspubVectorAvailable()){ emit statusMessage("Publisher import needs libmspub (not built in)."); return false; }
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false; const QByteArray data=f.readAll(); f.close();
    emit statusMessage(QStringLiteral("Importing Publisher document… %1").arg(QFileInfo(path).fileName()));
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    struct CursorGuard{ ~CursorGuard(){ QGuiApplication::restoreOverrideCursor(); } } _cg;
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    if(importVectorSvgPages(xara::mspubToSvgPages(data), QStringLiteral("Import Publisher"),
                            QStringLiteral("Imported Publisher document"), QStringLiteral("libmspub"))) return true;
    emit statusMessage("No importable vector content found in the Publisher file."); return false;
}
// More DLP-library vector importers — all share the wait-cursor pattern (librevenge parsers block the GUI)
// and are no-ops (report unsupported) when their -dev package wasn't present at build.
bool Canvas::importPageMaker(const QString& path){
    if(!xara::pagemakerVectorAvailable()){ emit statusMessage("PageMaker import needs libpagemaker (not built in)."); return false; }
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false; const QByteArray data=f.readAll(); f.close();
    emit statusMessage(QStringLiteral("Importing PageMaker document… %1").arg(QFileInfo(path).fileName()));
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    struct CursorGuard{ ~CursorGuard(){ QGuiApplication::restoreOverrideCursor(); } } _cg;
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    if(importVectorSvgPages(xara::pagemakerToSvgPages(data), QStringLiteral("Import PageMaker"),
                            QStringLiteral("Imported PageMaker document"), QStringLiteral("libpagemaker"))) return true;
    emit statusMessage("No importable vector content found in the PageMaker file."); return false;
}
bool Canvas::importZmf(const QString& path){
    if(!xara::zmfVectorAvailable()){ emit statusMessage("Zoner Draw import needs libzmf (not built in)."); return false; }
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false; const QByteArray data=f.readAll(); f.close();
    emit statusMessage(QStringLiteral("Importing Zoner Draw drawing… %1").arg(QFileInfo(path).fileName()));
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    struct CursorGuard{ ~CursorGuard(){ QGuiApplication::restoreOverrideCursor(); } } _cg;
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    if(importVectorSvgPages(xara::zmfToSvgPages(data), QStringLiteral("Import Zoner Draw"),
                            QStringLiteral("Imported Zoner Draw drawing"), QStringLiteral("libzmf"))) return true;
    emit statusMessage("No importable vector content found in the Zoner Draw file."); return false;
}
bool Canvas::importQxp(const QString& path){
    if(!xara::qxpVectorAvailable()){ emit statusMessage("QuarkXPress import needs libqxp (not built in)."); return false; }
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false; const QByteArray data=f.readAll(); f.close();
    emit statusMessage(QStringLiteral("Importing QuarkXPress document… %1").arg(QFileInfo(path).fileName()));
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    struct CursorGuard{ ~CursorGuard(){ QGuiApplication::restoreOverrideCursor(); } } _cg;
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    if(importVectorSvgPages(xara::qxpToSvgPages(data), QStringLiteral("Import QuarkXPress"),
                            QStringLiteral("Imported QuarkXPress document"), QStringLiteral("libqxp"))) return true;
    emit statusMessage("No importable vector content found in the QuarkXPress file."); return false;
}
bool Canvas::importStarOffice(const QString& path){
    if(!xara::starofficeVectorAvailable()){ emit statusMessage("StarOffice import needs libstaroffice (not built in)."); return false; }
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false; const QByteArray data=f.readAll(); f.close();
    emit statusMessage(QStringLiteral("Importing StarOffice drawing… %1").arg(QFileInfo(path).fileName()));
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    struct CursorGuard{ ~CursorGuard(){ QGuiApplication::restoreOverrideCursor(); } } _cg;
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    if(importVectorSvgPages(xara::starofficeToSvgPages(data), QStringLiteral("Import StarOffice"),
                            QStringLiteral("Imported StarOffice drawing"), QStringLiteral("libstaroffice"))) return true;
    emit statusMessage("No importable vector content found in the StarOffice file."); return false;
}
bool Canvas::importIWork(const QString& path){
    if(!xara::iworkVectorAvailable()){ emit statusMessage("iWork import needs libetonyek (not built in)."); return false; }
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false; const QByteArray data=f.readAll(); f.close();
    const int kind = xara::iworkDocumentKind(data);      // 1=Keynote, 2=Pages, 3=Numbers
    const QString name=QFileInfo(path).fileName();
    emit statusMessage(QStringLiteral("Importing %1… %2")
        .arg(kind==2?"Pages document": kind==3?"Numbers spreadsheet":"Keynote presentation", name));
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    struct CursorGuard{ ~CursorGuard(){ QGuiApplication::restoreOverrideCursor(); } } _cg;
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    if(kind==2 || kind==3){                              // word-processor / spreadsheet → an editable text frame
        QString label; const QString text=xara::iworkToText(data,&label).trimmed();
        if(text.isEmpty()){ emit statusMessage(QStringLiteral("No readable text found in the %1 file.").arg(label.isEmpty()?"iWork":label)); return false; }
        Shape s; s.type=ShapeType::Text; s.text=text; s.textFrame=true; s.name=name;
        s.fill.type=FillType::Solid; s.fill.color=Qt::black; s.stroke.enabled=false;
        const double w=qMin(m_doc.pageSize.width()*0.8, 520.0);
        s.rect=QRectF(0,0,w,qMax(80.0,m_doc.pageSize.height()*0.8));
        s.transform=QTransform::fromTranslate(m_doc.pageSize.width()*0.1, m_doc.pageSize.height()*0.08);
        addShapes({s}, QStringLiteral("Import %1").arg(label.isEmpty()?"iWork":label));
        emit statusMessage(QStringLiteral("Imported %1 text.").arg(label.isEmpty()?"iWork":label));
        return true;
    }
    if(importVectorSvgPages(xara::iworkToSvgPages(data), QStringLiteral("Import Keynote"),
                            QStringLiteral("Imported Keynote presentation"), QStringLiteral("libetonyek"))) return true;
    emit statusMessage("No importable content found in the iWork file."); return false;
}
bool Canvas::importMwaw(const QString& path){
    if(!xara::mwawVectorAvailable()){ emit statusMessage("Legacy Mac import needs libmwaw (not built in)."); return false; }
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false; const QByteArray data=f.readAll(); f.close();
    emit statusMessage(QStringLiteral("Importing legacy Mac drawing… %1").arg(QFileInfo(path).fileName()));
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    struct CursorGuard{ ~CursorGuard(){ QGuiApplication::restoreOverrideCursor(); } } _cg;
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    if(importVectorSvgPages(xara::mwawToSvgPages(data), QStringLiteral("Import Mac Drawing"),
                            QStringLiteral("Imported legacy Mac drawing"), QStringLiteral("libmwaw"))) return true;
    emit statusMessage("No importable vector content found in the file."); return false;
}
// GDAL/OGR geospatial vector — GDAL opens the FILE itself (many GIS formats are multi-file), so this takes
// a path, not bytes. Can be slow on large datasets → wait cursor. No-op without libgdal.
bool Canvas::importOgr(const QString& path){
    if(!xara::ogrAvailable()){ emit statusMessage("GIS vector import needs GDAL (not built in)."); return false; }
    emit statusMessage(QStringLiteral("Importing GIS vector… %1").arg(QFileInfo(path).fileName()));
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    struct CursorGuard{ ~CursorGuard(){ QGuiApplication::restoreOverrideCursor(); } } _cg;
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    const QString svg=xara::ogrToSvg(path);
    if(!svg.isEmpty() && importVectorSvgPages(QStringList{svg}, QStringLiteral("Import GIS Vector"),
                            QStringLiteral("Imported GIS vector data"), QStringLiteral("GDAL"))) return true;
    emit statusMessage("No importable vector features found in the file."); return false;
}
// Graphviz DOT — libgvc runs the `dot` layout engine then renders SVG. No-op without libgvc.
bool Canvas::importDot(const QString& path){
    if(!xara::dotAvailable()){ emit statusMessage("DOT import needs Graphviz (not built in)."); return false; }
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false; const QByteArray data=f.readAll(); f.close();
    const QString svg=xara::dotToSvg(data);
    if(!svg.isEmpty() && importVectorSvgPages(QStringList{svg}, QStringLiteral("Import DOT graph"),
                            QStringLiteral("Imported Graphviz graph"), QStringLiteral("graphviz"))) return true;
    emit statusMessage("Could not lay out the DOT graph."); return false;
}
bool Canvas::importWmfFile(const QString& path){
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false; const QByteArray data=f.readAll(); f.close();
    // Preferred: libwmf plays the metafile through its SVG device → our SVG importer (best fidelity).
    if(xara::wmfAvailable()){
        const QByteArray svg=xara::wmfToSvg(data);
        if(!svg.isEmpty() && importVectorSvgPages(QStringList{QString::fromUtf8(svg)}, QStringLiteral("Import WMF"),
                                QStringLiteral("Imported Windows Metafile"), QStringLiteral("libwmf"))) return true;
    }
    // Fallback: XarQ's own native WMF parser (prepress.cpp).
    const Document d=xara::importWmf(data);
    if(!d.shapes.isEmpty()){ addShapes(d.shapes, QStringLiteral("Import WMF")); zoomFit();
        emit statusMessage(QStringLiteral("Imported Windows Metafile — %1 objects (native).").arg(d.shapes.size())); return true; }
    emit statusMessage("No importable vector content found in the WMF file."); return false;
}
// Native single-file vector importers (vecextra.cpp): each parses the file into an SVG document string,
// then reuses importVectorSvgPages → parseSvg → editable Shapes. All bounds-hardened, no external process.
bool Canvas::importShapefile(const QString& path){
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false; const QByteArray data=f.readAll(); f.close();
    const QString svg=xara::shapefileToSvg(data);
    if(!svg.isEmpty() && importVectorSvgPages(QStringList{svg}, QStringLiteral("Import Shapefile"),
                            QStringLiteral("Imported ESRI Shapefile"), QStringLiteral("native"))) return true;
    emit statusMessage("No importable geometry found in the Shapefile."); return false;
}
bool Canvas::importGpx(const QString& path){
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false; const QByteArray data=f.readAll(); f.close();
    const QString svg=xara::gpxToSvg(data);
    if(!svg.isEmpty() && importVectorSvgPages(QStringList{svg}, QStringLiteral("Import GPX"),
                            QStringLiteral("Imported GPX track"), QStringLiteral("native"))) return true;
    emit statusMessage("No importable tracks/routes/waypoints found in the GPX file."); return false;
}
bool Canvas::importXfig(const QString& path){
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false; const QByteArray data=f.readAll(); f.close();
    const QString svg=xara::xfigToSvg(data);
    if(!svg.isEmpty() && importVectorSvgPages(QStringList{svg}, QStringLiteral("Import Xfig"),
                            QStringLiteral("Imported Xfig drawing"), QStringLiteral("native"))) return true;
    emit statusMessage("No importable content found in the Xfig file."); return false;
}
bool Canvas::importCgm(const QString& path){
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false; const QByteArray data=f.readAll(); f.close();
    const QString svg=xara::cgmToSvg(data);
    if(!svg.isEmpty() && importVectorSvgPages(QStringList{svg}, QStringLiteral("Import CGM"),
                            QStringLiteral("Imported CGM metafile"), QStringLiteral("native"))) return true;
    emit statusMessage("No importable primitives found in the CGM file."); return false;
}
bool Canvas::importSvgz(const QString& path){
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false; const QByteArray data=f.readAll(); f.close();
    const QString svg=xara::svgzToSvg(data);
    if(!svg.isEmpty() && importVectorSvgPages(QStringList{svg}, QStringLiteral("Import SVGZ"),
                            QStringLiteral("Imported compressed SVG"), QStringLiteral("native"))) return true;
    // Some writers (ImageMagick among them) store a plain raster under the .svgz name. If the
    // decompressed payload is not SVG but is an image we can read, place it rather than failing.
    { const QByteArray inner = xara::gunzipBytes(data);
      QImage ras; if(!inner.isEmpty()) ras.loadFromData(inner);
      if(!ras.isNull()){
          placeZipComposite(ras, QFileInfo(path).fileName(), QStringLiteral("Import SVGZ image"));
          emit statusMessage("Imported the raster image stored inside the .svgz.");
          return true; } }
    emit statusMessage("Could not decompress the SVGZ file."); return false;
}
// Corel Presentation Exchange (.cmx): editable vectors via libcdr's CMXDocument → SVG → our importer.
// Like .cdr, libcdr is single-threaded and blocks the GUI, so paint a wait cursor first. No-op without libcdr.
bool Canvas::importCmx(const QString& path){
    if(!xara::cdrVectorAvailable()){ emit statusMessage("CMX import needs libcdr (not built in)."); return false; }
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false; const QByteArray data=f.readAll(); f.close();
    emit statusMessage(QStringLiteral("Importing Corel CMX… %1").arg(QFileInfo(path).fileName()));
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    struct CursorGuard{ ~CursorGuard(){ QGuiApplication::restoreOverrideCursor(); } } _cg;
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    if(importVectorSvgPages(xara::cmxToSvgPages(data), QStringLiteral("Import CMX"),
                            QStringLiteral("Imported Corel Presentation Exchange"), QStringLiteral("libcdr"))) return true;
    emit statusMessage("No importable vector content found in the CMX file."); return false;
}
bool Canvas::importXd(const QString& path){
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false; const QByteArray data=f.readAll(); f.close();
    const QMap<QString,QByteArray> zip=readZipEntries(data);
    xara::XdStats st;
    const QStringList pages=xara::xdToSvgPages(zip,&st);
    if(!pages.isEmpty() && importVectorSvgPages(pages, QStringLiteral("Import Adobe XD"),
                            QStringLiteral("Imported Adobe XD"), QStringLiteral("native"))){
        if(st.pasteboardObjects>0)
            emit statusMessage(QStringLiteral("Imported Adobe XD — %1 artboards; %2 objects left on the pasteboard were not imported.").arg(st.artboards).arg(st.pasteboardObjects));
        return true;
    }
    emit statusMessage("No readable artwork found in the Adobe XD file."); return false;
}
bool Canvas::importProcreate(const QString& path){
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false; const QByteArray data=f.readAll(); f.close();
    const QMap<QString,QByteArray> zip=readZipEntries(data);
    ProcreateImportStats st;
    const Document pd=importProcreateDocument(zip,&st);
    QStringList notes;
    if(st.badTiles) notes<<QStringLiteral("%1 damaged tile(s) left blank").arg(st.badTiles);
    if(adoptLayers(pd,path,"Import Procreate",notes.join(QStringLiteral(", ")))) return true;
    QImage pv; pv.loadFromData(zip.value(QStringLiteral("QuickLook/Thumbnail.png")));
    if(!pv.isNull()){
        Document one; one.pageSize=pv.size(); one.shapes.push_back(psdBitmapShape(pv,0,0,QFileInfo(path).fileName()));
        return adoptLayers(one,path,"Import Procreate",QStringLiteral("Only the file's saved preview could be read"));
    }
    emit statusMessage("No readable layers found in the Procreate file."); return false;
}
bool Canvas::importCptFile(const QString& path){
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false; const QByteArray data=f.readAll(); f.close();
    CptImportStats st;
    const QImage img=importCpt(data,&st);
    if(!img.isNull()){
        Document one; one.pageSize=img.size(); one.shapes.push_back(psdBitmapShape(img,0,0,QFileInfo(path).fileName()));
        if(adoptLayers(one,path,"Import PHOTO-PAINT",QString())) return true;
    }
    emit statusMessage(st.unknownTiles
        ? QStringLiteral("This PHOTO-PAINT file uses a compression XarQ can't read yet (%1 tiles). Save it as TIFF or PSD from PHOTO-PAINT.").arg(st.unknownTiles)
        : QStringLiteral("No readable image found in the PHOTO-PAINT file."));
    return false;
}
bool Canvas::importPxd(const QString& path){
    QMap<QString,QByteArray> files;
    const QFileInfo info(path);
    if(info.isDir()){                  // a .pxd straight off a Mac is a folder
        const QDir root(path);
        QDirIterator it(path, QDir::Files, QDirIterator::Subdirectories);
        qint64 total=0; const qint64 cap=qint64(1)<<31;
        while(it.hasNext() && total<cap){ const QString p=it.next(); QFile f(p); if(f.size()>(qint64(1)<<30)||!f.open(QIODevice::ReadOnly)) continue; const QByteArray b=f.readAll(); total+=b.size(); files.insert(root.relativeFilePath(p),b); }
    } else {
        QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false;
        files=readZipEntries(f.readAll());
    }
    PxdImportStats st;
    const Document pd=importPxdDocument(files,&st);
    QStringList notes;
    if(st.adjustedLayers||st.adjustmentLayers) notes<<QStringLiteral("%1 colour adjustment(s) not applied").arg(st.adjustedLayers+st.adjustmentLayers);
    if(st.otherEffects) notes<<QStringLiteral("%1 effect(s) not applied").arg(st.otherEffects);
    if(st.innerShadows) notes<<QStringLiteral("%1 inner shadow(s) not applied").arg(st.innerShadows);
    if(st.unknownLayers) notes<<QStringLiteral("%1 layer(s) of an unknown kind left out").arg(st.unknownLayers);
    if(adoptLayers(pd,path,"Import Pixelmator",notes.join(QStringLiteral(", ")))) return true;
    emit statusMessage("No readable layers found in the Pixelmator file."); return false;
}
bool Canvas::importFxg(const QString& path){
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false; const QByteArray data=f.readAll(); f.close();
    const QString svg=xara::fxgToSvg(data, QFileInfo(path).absolutePath());
    if(!svg.isEmpty() && importVectorSvgPages(QStringList{svg}, QStringLiteral("Import FXG"),
                            QStringLiteral("Imported Adobe FXG"), QStringLiteral("native"))) return true;
    emit statusMessage("No importable artwork found in the FXG file."); return false;
}
bool Canvas::importGerber(const QString& path){
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false; const QByteArray data=f.readAll(); f.close();
    const QString svg=xara::gerberToSvg(data);
    if(!svg.isEmpty() && importVectorSvgPages(QStringList{svg}, QStringLiteral("Import Gerber"),
                            QStringLiteral("Imported Gerber PCB layer"), QStringLiteral("native"))) return true;
    emit statusMessage("No importable image found in the Gerber file."); return false;
}
bool Canvas::importExcellon(const QString& path){
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false; const QByteArray data=f.readAll(); f.close();
    const QString svg=xara::excellonToSvg(data);
    if(!svg.isEmpty() && importVectorSvgPages(QStringList{svg}, QStringLiteral("Import Excellon"),
                            QStringLiteral("Imported Excellon drill file"), QStringLiteral("native"))) return true;
    emit statusMessage("No importable holes found in the Excellon file."); return false;
}
bool Canvas::importLottie(const QString& path){
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false; const QByteArray data=f.readAll(); f.close();
    const QString svg=xara::lottieToSvg(data);
    if(!svg.isEmpty() && importVectorSvgPages(QStringList{svg}, QStringLiteral("Import Lottie"),
                            QStringLiteral("Imported Lottie animation (static)"), QStringLiteral("native"))) return true;
    return false;   // quiet: .json fall-through handles the not-Lottie case
}
bool Canvas::importSketch(const QString& path){
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false; const QByteArray data=f.readAll(); f.close();
    const QString svg=xara::sketchToSvg(data);
    if(!svg.isEmpty() && importVectorSvgPages(QStringList{svg}, QStringLiteral("Import Sketch"),
                            QStringLiteral("Imported Sketch document"), QStringLiteral("native"))) return true;
    emit statusMessage("No importable content found in the Sketch file."); return false;
}
bool Canvas::importTgs(const QString& path){
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false; const QByteArray data=f.readAll(); f.close();
    const QString svg=xara::tgsToSvg(data);
    if(!svg.isEmpty() && importVectorSvgPages(QStringList{svg}, QStringLiteral("Import Sticker"),
                            QStringLiteral("Imported Telegram sticker (static)"), QStringLiteral("native"))) return true;
    return false;   // quiet: not our format
}
bool Canvas::importTopoJson(const QString& path){
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false; const QByteArray data=f.readAll(); f.close();
    const QString svg=xara::topojsonToSvg(data);
    if(!svg.isEmpty() && importVectorSvgPages(QStringList{svg}, QStringLiteral("Import TopoJSON"),
                            QStringLiteral("Imported TopoJSON map"), QStringLiteral("native"))) return true;
    emit statusMessage("No importable geometry found in the TopoJSON file."); return false;
}
bool Canvas::importVml(const QString& path){
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false; const QByteArray data=f.readAll(); f.close();
    const QString svg=xara::vmlToSvg(data);
    if(!svg.isEmpty() && importVectorSvgPages(QStringList{svg}, QStringLiteral("Import VML"),
                            QStringLiteral("Imported VML drawing"), QStringLiteral("native"))) return true;
    emit statusMessage("No importable content found in the VML file."); return false;
}
bool Canvas::importXps(const QString& path){
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false; const QByteArray data=f.readAll(); f.close();
    const QString svg=xara::xpsToSvg(data);
    if(!svg.isEmpty() && importVectorSvgPages(QStringList{svg}, QStringLiteral("Import XPS"),
                            QStringLiteral("Imported XPS page"), QStringLiteral("native"))) return true;
    emit statusMessage("No importable content found in the XPS file."); return false;
}
bool Canvas::importScribus(const QString& path){
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false; const QByteArray data=f.readAll(); f.close();
    const QString svg=xara::scribusToSvg(data);
    if(!svg.isEmpty() && importVectorSvgPages(QStringList{svg}, QStringLiteral("Import Scribus"),
                            QStringLiteral("Imported Scribus document"), QStringLiteral("native"))) return true;
    emit statusMessage("No importable content found in the Scribus file."); return false;
}
// Serif Affinity (.afdesign/.afphoto/.afpub/.aftemplate/.af) — the REAL artwork: the container's
// doc.dat object tree becomes one SVG per spread (paths, live shapes, fills, gradients, strokes,
// groups, text, placed images) and goes through the normal SVG importer. Pixel layers come across
// too: their 256x256 channel planes are reassembled from the d/* tile streams and embedded as PNGs.
// Anything still undecoded (layer masks on a raster, adjustment layers) is counted and reported
// rather than dropped in silence, and any document we cannot parse at all (including v11 files on a
// build without zstd) falls back to the page preview the file carries, exactly like importCdr does.
bool Canvas::importAffinity(const QString& path){
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false; const QByteArray data=f.readAll(); f.close();
    if(!xara::affinityIsContainer(data)){ emit statusMessage("Not an Affinity document."); return false; }
    emit statusMessage(QStringLiteral("Importing Affinity document… %1").arg(QFileInfo(path).fileName()));
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    struct CursorGuard{ ~CursorGuard(){ QGuiApplication::restoreOverrideCursor(); } } _cg;
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    xara::AffinityStats st;
    const QStringList pages=xara::affinityToSvgPages(data,&st);
    const bool freshDoc=m_doc.shapes.isEmpty();
    if(!pages.isEmpty() && importVectorSvgPages(pages, QStringLiteral("Import Affinity"),
                            QStringLiteral("Imported Affinity document"), QStringLiteral("native"))){
        // A fresh document takes the file's guides, margins (as guides, per page) and bleed.
        if(freshDoc){
            for(int i=0;i<st.pageInfo.size();++i){
                const xara::AffinityPageInfo& pi=st.pageInfo[i];
                const QRectF pr=m_doc.pageRect(i);
                auto addH=[&](double y){ if(!m_guideHpos.contains(y)) m_guideHpos.push_back(y); };
                auto addV=[&](double x){ if(!m_guideVpos.contains(x)) m_guideVpos.push_back(x); };
                for(double y: pi.guidesH) addH(pr.top()+y);
                for(double x: pi.guidesV) addV(pr.left()+x);
                if(pi.marginL>0||pi.marginT>0||pi.marginR>0||pi.marginB>0){
                    addV(pr.left()+pi.marginL); addV(pr.left()+pi.width-pi.marginR);
                    addH(pr.top()+pi.marginT); addH(pr.top()+pi.height-pi.marginB);
                }
            }
            m_doc.bleed=st.bleed;
            update();
        }
        // Report what actually happened. The stats have always recorded skipped SHAPES (live-shape
        // kinds we draw as their bounding box, layers whose clip we could not express) as well as
        // skipped rasters, but only the raster count was ever shown — so the commonest silent loss
        // was also the one the user was never told about.
        QStringList detail;
        if(st.rasters>0)       detail << QStringLiteral("%1 pixel layer%2").arg(st.rasters).arg(st.rasters==1?"":"s");
        if(st.masks>0)         detail << QStringLiteral("%1 mask%2").arg(st.masks).arg(st.masks==1?"":"s");
        if(st.skippedRaster>0) detail << QStringLiteral("%1 raster layer%2 not decoded").arg(st.skippedRaster).arg(st.skippedRaster==1?"":"s");
        if(st.skippedShape>0)  detail << QStringLiteral("%1 object%2 approximated").arg(st.skippedShape).arg(st.skippedShape==1?"":"s");
        if(st.ignoredAdjustments>0) detail << QStringLiteral("%1 adjustment%2 not applied").arg(st.ignoredAdjustments).arg(st.ignoredAdjustments==1?"":"s");
        if(st.ignoredFilters>0) detail << QStringLiteral("%1 live filter%2 not applied").arg(st.ignoredFilters).arg(st.ignoredFilters==1?"":"s");
        if(st.extraMasks>0)    detail << QStringLiteral("%1 extra mask%2 not applied").arg(st.extraMasks).arg(st.extraMasks==1?"":"s");
        if(!st.exactParse)     detail << QStringLiteral("document structure only partly readable");
        if(!detail.isEmpty())
            emit statusMessage(QStringLiteral("Imported Affinity document — %1 objects, %2 pages (%3).")
                .arg(st.shapes).arg(st.pages).arg(detail.join(QStringLiteral(", "))));
        return true;
    }
    // Fallback: the document's own 512px preview, located exactly from the header (never by scanning
    // for a PNG signature — placed images are stored verbatim and would be found instead).
    QImage prev; prev.loadFromData(xara::affinityPreviewPng(data), "PNG");
    if(prev.isNull()){
        emit statusMessage(xara::affinityZstdAvailable()
            ? QStringLiteral("Could not read this Affinity document (no artwork and no embedded preview).")
            : QStringLiteral("Could not read this Affinity document — Affinity 2.x files need zstd, which this build lacks."));
        return false;
    }
    const double W=prev.width(), H=prev.height();
    if(!(W>0)||!(H>0)) return false;
    const double sc=qMin(1.0, qMin(m_doc.pageSize.width()*0.9/W, m_doc.pageSize.height()*0.9/H));
    Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,W,H); s.name=QFileInfo(path).fileName()+" (preview)";
    s.fill.type=FillType::Bitmap; s.fill.image=prev; s.fill.origImage=prev; s.fill.tiled=false;
    s.fill.p0=QPointF(0,H); s.fill.p1=QPointF(W,H); s.fill.p2=QPointF(0,0); s.stroke.enabled=false;
    s.transform=QTransform::fromScale(sc,sc)*QTransform::fromTranslate((m_doc.pageSize.width()-W*sc)/2,(m_doc.pageSize.height()-H*sc)/2);
    addShapes({s},"Import Affinity preview");
    emit statusMessage("Imported the Affinity page preview (the artwork could not be parsed).");
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

// ---- Batch 3 importers: comic archives + geometry (GeoJSON / KML / HP-GL) → editable vectors ----

// Map a source-space bounding box into a page rect (centred, aspect-preserving, optional Y-flip for
// data whose Y grows upward — geographic lat / plotter units).
static QTransform xqFitTransform(const QRectF& b, QSizeF page, double margin, bool flipY){
    if(b.isEmpty() || b.width()<=0 || b.height()<=0) return QTransform();
    const double availW=page.width()-2*margin, availH=page.height()-2*margin;
    double sc=qMin(availW/b.width(), availH/b.height());
    if(!std::isfinite(sc) || sc<=0) sc=1;
    const double offx=(availW-b.width()*sc)/2, offy=(availH-b.height()*sc)/2;
    QTransform t;
    t.translate(margin+offx, margin+offy);
    t.scale(sc,sc);
    if(flipY){ t.translate(0,b.height()); t.scale(1,-1); }
    t.translate(-b.left(),-b.top());
    return t;
}

// Shared comic-archive page builder: image entries in filename order → one full-page Bitmap per image.
bool Canvas::importComicEntries(const QMap<QString,QByteArray>& entries, const QString& srcName){
    QStringList names;
    for(auto it=entries.begin(); it!=entries.end(); ++it){ const QString l=it.key().toLower();
        if(l.endsWith(".jpg")||l.endsWith(".jpeg")||l.endsWith(".png")||l.endsWith(".gif")
         ||l.endsWith(".webp")||l.endsWith(".bmp")||l.endsWith(".tif")||l.endsWith(".tiff"))
            names<<it.key(); }
    names.sort(Qt::CaseInsensitive);
    if(names.isEmpty()) return false;
    Document d; QVector<Shape> shapes; int page=0; double advH=0;
    for(const QString& n: names){ if(page>=1000) break;
        QImage img=QImage::fromData(entries.value(n)); if(img.isNull()) continue;
        if(page==0){ d.pageSize=QSizeF(img.width(),img.height()); advH=d.pageSize.height()+d.pageGap; }
        const double W=img.width(), H=img.height();
        const double sc=qMin(1.0, qMin(d.pageSize.width()/W, d.pageSize.height()/H));   // fit within the page
        Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,W,H); s.name=QFileInfo(n).fileName();
        s.fill.type=FillType::Bitmap; s.fill.image=img; s.fill.origImage=img; s.fill.tiled=false;
        s.fill.p0=QPointF(0,H); s.fill.p1=QPointF(W,H); s.fill.p2=QPointF(0,0);
        s.stroke.enabled=false;
        s.transform = QTransform::fromScale(sc,sc)
                    * QTransform::fromTranslate((d.pageSize.width()-W*sc)/2, page*advH + (d.pageSize.height()-H*sc)/2);
        shapes<<s; ++page;
    }
    if(shapes.isEmpty()) return false;
    d.pageCount=page; d.shapes=shapes;
    setDocument(d); zoomFit();
    emit statusMessage(QString("Imported %1: %2 page(s).").arg(srcName).arg(page));
    return true;
}

bool Canvas::importCbz(const QString& path){
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray data=f.readAll(); f.close();
    const QMap<QString,QByteArray> zip=readZipEntries(data);
    if(zip.isEmpty()) return false;
    return importComicEntries(zip, "comic");
}

// Extract every entry of a RAR/7z/tar/zip archive via dlopen'd libarchive (reads what its filters
// support; absent lib → empty). First-party policy: link a library, never shell out to unrar.
static QMap<QString,QByteArray> xqLibarchiveExtract(const QByteArray& data){
    QMap<QString,QByteArray> out;
    struct LA { void* lib=nullptr; bool ok=false;
        void* (*read_new)()=nullptr;
        int (*sup_filter)(void*)=nullptr; int (*sup_format)(void*)=nullptr;
        int (*open_mem)(void*,const void*,size_t)=nullptr;
        int (*next_header)(void*,void**)=nullptr;
        const char* (*entry_path)(void*)=nullptr; long long (*entry_size)(void*)=nullptr;
        long (*read_data)(void*,void*,size_t)=nullptr; int (*read_free)(void*)=nullptr;
        LA(){ lib=dlopen("libarchive.so.13",RTLD_NOW|RTLD_LOCAL); if(!lib) lib=dlopen("libarchive.so",RTLD_NOW|RTLD_LOCAL); if(!lib) return;
            read_new    =(void*(*)())               dlsym(lib,"archive_read_new");
            sup_filter  =(int(*)(void*))             dlsym(lib,"archive_read_support_filter_all");
            sup_format  =(int(*)(void*))             dlsym(lib,"archive_read_support_format_all");
            open_mem    =(int(*)(void*,const void*,size_t))dlsym(lib,"archive_read_open_memory");
            next_header =(int(*)(void*,void**))      dlsym(lib,"archive_read_next_header");
            entry_path  =(const char*(*)(void*))     dlsym(lib,"archive_entry_pathname");
            entry_size  =(long long(*)(void*))       dlsym(lib,"archive_entry_size");
            read_data   =(long(*)(void*,void*,size_t))dlsym(lib,"archive_read_data");
            read_free   =(int(*)(void*))             dlsym(lib,"archive_read_free");
            ok=read_new&&sup_filter&&sup_format&&open_mem&&next_header&&entry_path&&entry_size&&read_data&&read_free; } };
    static LA la; if(!la.ok) return out;
    void* a=la.read_new(); if(!a) return out;
    la.sup_filter(a); la.sup_format(a);
    if(la.open_mem(a, data.constData(), size_t(data.size()))==0){   // ARCHIVE_OK == 0
        void* entry=nullptr; int guard=0;
        while(la.next_header(a,&entry)==0 && guard++<20000){
            const char* nm=la.entry_path(entry); if(!nm) continue;
            long long sz=la.entry_size(entry); if(sz<=0 || sz>256LL*1024*1024) continue;
            QByteArray buf; buf.resize(int(sz)); long long got=0;
            while(got<sz){ long r=la.read_data(a, buf.data()+got, size_t(sz-got)); if(r<=0) break; got+=r; }
            if(got==sz) out.insert(QString::fromUtf8(nm), buf);
        }
    }
    la.read_free(a);
    return out;
}

bool Canvas::importCbr(const QString& path){
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray data=f.readAll(); f.close();
    QMap<QString,QByteArray> entries=xqLibarchiveExtract(data);
    if(entries.isEmpty()){ const QMap<QString,QByteArray> zip=readZipEntries(data); if(!zip.isEmpty()) entries=zip; }  // fallback: zip-based .cbr
    if(entries.isEmpty()) return false;
    return importComicEntries(entries, "comic");
}

// ---- Animated PNG (APNG): rebuild each frame as a standalone PNG (Qt decodes it), composite per fcTL ----
bool Canvas::importApng(const QString& path){
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray data=f.readAll(); f.close();
    const uchar SIG[8]={0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
    if(data.size()<8 || memcmp(data.constData(),SIG,8)!=0) return false;
    const uchar* p=reinterpret_cast<const uchar*>(data.constData()); const qint64 n=data.size();
    auto be32=[&](qint64 o)->quint32{ return (quint32(p[o])<<24)|(quint32(p[o+1])<<16)|(quint32(p[o+2])<<8)|p[o+3]; };
    QByteArray ihdr, plte, trns; quint32 fullW=0, fullH=0; bool sawActl=false;
    struct AFrame { quint32 w=0,h=0,x=0,y=0; int delayMs=100, dispose=0, blend=0; QByteArray dat; };
    QVector<AFrame> frames;
    qint64 o=8;
    while(o+12<=n){
        const quint32 len=be32(o); const char* type=reinterpret_cast<const char*>(p+o+4); const qint64 dOff=o+8;
        if(dOff+qint64(len)+4>n) break;
        const uchar* d=p+dOff;
        if(!memcmp(type,"IHDR",4)){ ihdr=QByteArray(reinterpret_cast<const char*>(d), qMin<quint32>(len,13));
            if(len>=8){ fullW=be32(dOff); fullH=be32(dOff+4); } }
        else if(!memcmp(type,"PLTE",4)) plte=QByteArray(reinterpret_cast<const char*>(d),len);
        else if(!memcmp(type,"tRNS",4)) trns=QByteArray(reinterpret_cast<const char*>(d),len);
        else if(!memcmp(type,"acTL",4)) sawActl=true;
        else if(!memcmp(type,"fcTL",4)){ if(len>=26){ AFrame fr; fr.w=be32(dOff+4); fr.h=be32(dOff+8); fr.x=be32(dOff+12); fr.y=be32(dOff+16);
            const int dn=(d[20]<<8)|d[21], dd=(d[22]<<8)|d[23]; fr.delayMs = dd? qMax(10, dn*1000/dd) : (dn>0?dn:100); if(fr.delayMs<=0) fr.delayMs=100;
            fr.dispose=d[24]; fr.blend=d[25]; if(frames.size()<512) frames.push_back(fr); } }   // cap frame count (crafted-file OOM guard)
        else if(!memcmp(type,"IDAT",4)){ if(!frames.isEmpty()) frames.last().dat.append(reinterpret_cast<const char*>(d),len); }
        else if(!memcmp(type,"fdAT",4)){ if(len>=4 && !frames.isEmpty()) frames.last().dat.append(reinterpret_cast<const char*>(d+4),len-4); }
        else if(!memcmp(type,"IEND",4)) break;
        o = dOff + len + 4;
    }
    if(!sawActl || frames.size()<2 || ihdr.size()<13 || fullW==0 || fullH==0 || (qint64)fullW*fullH>256LL*1024*1024)
        return false;                                  // not an animation → single still
    // Standalone-PNG-per-frame builder (Qt does the actual inflate + unfilter).
    auto chunk=[](QByteArray& out,const char* type,const QByteArray& d){
        const quint32 len=d.size(); out.append(char(len>>24));out.append(char(len>>16));out.append(char(len>>8));out.append(char(len));
        QByteArray td; td.append(type,4); td.append(d); out.append(td);
        const quint32 c=(quint32)crc32(crc32(0,nullptr,0),reinterpret_cast<const Bytef*>(td.constData()),td.size());
        out.append(char(c>>24));out.append(char(c>>16));out.append(char(c>>8));out.append(char(c)); };
    auto decodeFrame=[&](const AFrame& fr)->QImage{
        QByteArray png; png.append(reinterpret_cast<const char*>(SIG),8);
        QByteArray ih=ihdr; ih[0]=char(fr.w>>24);ih[1]=char(fr.w>>16);ih[2]=char(fr.w>>8);ih[3]=char(fr.w);
                            ih[4]=char(fr.h>>24);ih[5]=char(fr.h>>16);ih[6]=char(fr.h>>8);ih[7]=char(fr.h);
        chunk(png,"IHDR",ih);
        if(!plte.isEmpty()) chunk(png,"PLTE",plte);
        if(!trns.isEmpty()) chunk(png,"tRNS",trns);
        chunk(png,"IDAT",fr.dat); chunk(png,"IEND",QByteArray());
        return QImage::fromData(png,"PNG"); };
    QImage canvas(int(fullW),int(fullH),QImage::Format_ARGB32_Premultiplied); if(canvas.isNull()) return false;
    canvas.fill(Qt::transparent);
    QVector<QImage> outFrames; QVector<int> delays;
    const qint64 APNG_PIXEL_BUDGET=256LL*1024*1024; qint64 accPix=0;    // each held frame is a full-canvas copy → bound the total
    for(const AFrame& fr: frames){
        if(accPix + (qint64)fullW*fullH > APNG_PIXEL_BUDGET) break;     // stop before another full-canvas copy blows the budget
        accPix += (qint64)fullW*fullH;
        QImage sub=decodeFrame(fr); if(sub.isNull()) continue;
        sub=sub.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        const QImage prev = (fr.dispose==2)? canvas.copy() : QImage();
        { QPainter g(&canvas);
          g.setCompositionMode(fr.blend==0? QPainter::CompositionMode_Source : QPainter::CompositionMode_SourceOver);
          g.drawImage(QPointF(fr.x,fr.y), sub); }
        outFrames.push_back(canvas.convertToFormat(QImage::Format_ARGB32));
        delays.push_back(fr.delayMs);
        if(fr.dispose==1){ QPainter g(&canvas); g.setCompositionMode(QPainter::CompositionMode_Clear);
                           g.fillRect(QRectF(fr.x,fr.y,fr.w,fr.h),Qt::transparent); }
        else if(fr.dispose==2 && !prev.isNull()) canvas=prev;
    }
    if(outFrames.size()<2) return false;
    pushUndo("Import APNG");
    const double W=outFrames[0].width(), H=outFrames[0].height();
    const double maxW=m_doc.pageSize.width()*0.9, maxH=m_doc.pageSize.height()*0.9;
    const double sc=qMin(1.0, qMin(maxW/W, maxH/H));
    const QTransform place=QTransform::fromScale(sc,sc)*QTransform::fromTranslate((m_doc.pageSize.width()-W*sc)/2,(m_doc.pageSize.height()-H*sc)/2);
    m_doc.layers.clear(); m_doc.shapes.clear();
    const QString base=QFileInfo(path).completeBaseName();
    for(int i=0;i<outFrames.size();++i){
        Layer L; L.name=QString("%1 %2").arg(base).arg(i+1); L.visible=(i==0); L.frameDelayMs=delays.value(i,100); m_doc.layers.push_back(L);
        Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,W,H); s.name=L.name; s.layer=i;
        s.fill.type=FillType::Bitmap; s.fill.image=outFrames[i]; s.fill.origImage=outFrames[i]; s.fill.tiled=false;
        s.fill.p0=QPointF(0,H); s.fill.p1=QPointF(W,H); s.fill.p2=QPointF(0,0);
        s.stroke.enabled=false; s.transform=place; m_doc.shapes.push_back(s);
    }
    m_sel.clear(); emit documentChanged(); emit selectionChanged(); update();
    emit statusMessage(QString("Imported APNG: %1 frame(s).").arg(outFrames.size()));
    return true;
}

// ---- OpenDocument (.odg/.odp/.odt/.ods + flat .fodg/… + OpenOffice.org 1.x .sxd/.sxw): unzip →
//      content.xml → draw: primitive shapes, with a text fallback for text-centric documents ----
static double xqOdfLen(const QString& s){                            // "5cm","2.5mm","10pt","3in","100px", bare
    QString t=s.trimmed(); if(t.isEmpty()) return 0; int i=0;
    while(i<t.size() && (t[i].isDigit()||t[i]=='.'||t[i]=='-'||t[i]=='+'||t[i]=='e'||t[i]=='E')) ++i;
    const double num=t.left(i).toDouble(); const QString u=t.mid(i).trimmed().toLower(); const double dpi=96.0;
    if(u=="cm") return num*dpi/2.54;   if(u=="mm") return num*dpi/25.4;  if(u=="in") return num*dpi;
    if(u=="pt") return num*dpi/72.0;   if(u=="pc") return num*dpi/6.0;   return num; }

// Parse an ODF draw:transform list (ODF 1.2 §19.228) into a QTransform. Three divergences from SVG,
// each pinned against LibreOffice: (1) the list applies LEFT-TO-RIGHT (first listed applies first),
// the opposite of SVG; (2) translate/matrix e,f carry real UNITS ("5cm"), not user units; (3) rotate
// and the skews are in RADIANS and turn counter-clockwise on a y-down page, so the angle is negated
// for Qt (whose positive rotation is clockwise on screen).
static QTransform xqOdfTransform(const QString& str){
    QTransform t; if(str.trimmed().isEmpty()) return t;
    static const QRegularExpression re("([A-Za-z]+)\\s*\\(([^)]*)\\)");
    auto it=re.globalMatch(str);
    while(it.hasNext()){
        const auto m=it.next(); const QString fn=m.captured(1);
        const QStringList a=m.captured(2).split(QRegularExpression("[\\s,]+"),Qt::SkipEmptyParts);
        auto L=[&](int i){ return i<a.size()? xqOdfLen(a[i]) : 0.0; };            // ODF length (unit-aware)
        auto N=[&](int i,double d){ return i<a.size()? a[i].toDouble() : d; };    // bare number / radians
        QTransform u;
        if      (fn=="translate") u.translate(L(0), L(1));
        else if (fn=="scale")   { const double sx=N(0,1.0); u.scale(sx, a.size()>1? N(1,sx) : sx); }
        else if (fn=="rotate")    u.rotateRadians(-N(0,0.0));
        else if (fn=="skewX")     u.shear(-std::tan(N(0,0.0)), 0);
        else if (fn=="skewY")     u.shear(0, -std::tan(N(0,0.0)));
        else if (fn=="matrix" && a.size()>=6) u=QTransform(N(0,1),N(1,0),N(2,0),N(3,1),L(4),L(5));
        else continue;
        t = t * u;                                   // left-to-right composition
    }
    if(!std::isfinite(t.m11())||!std::isfinite(t.m12())||!std::isfinite(t.m21())||
       !std::isfinite(t.m22())||!std::isfinite(t.dx())||!std::isfinite(t.dy())) return QTransform();
    return t;
}

// --- OpenRaster (.ora) / Krita (.kra): app-native layered raster in a ZIP container ----------------
// Both wrap layers + a flattened "mergedimage.png" in a ZIP. OpenRaster describes its stack in a standard
// stack.xml (layer PNGs under data/), which we reverse-engineer into one editable photo object per layer;
// Krita's per-layer data is tiled binary, so we import its flattened composite. mergedimage is the fallback.
static QByteArray xqZipFind(const QMap<QString,QByteArray>& zip, const QString& name){
    for(auto it=zip.begin(); it!=zip.end(); ++it){
        const QString k=it.key();
        if(k.compare(name,Qt::CaseInsensitive)==0 || k.endsWith('/'+name, Qt::CaseInsensitive)) return it.value();
    }
    return {};
}
// Place a flattened bitmap as a single photo object, scaled to fit the page (the composite path).
void Canvas::placeZipComposite(const QImage& img, const QString& name, const QString& undoLabel){
    const double W=img.width(), H=img.height();
    const double sc=qMin(1.0, qMin(m_doc.pageSize.width()*0.9/qMax(1.0,W), m_doc.pageSize.height()*0.9/qMax(1.0,H)));
    Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,W,H); s.name=name;
    s.fill.type=FillType::Bitmap; s.fill.image=img; s.fill.origImage=img; s.fill.tiled=false;
    s.fill.p0=QPointF(0,H); s.fill.p1=QPointF(W,H); s.fill.p2=QPointF(0,0); s.stroke.enabled=false;
    s.transform=QTransform::fromScale(sc,sc)*QTransform::fromTranslate((m_doc.pageSize.width()-W*sc)/2,(m_doc.pageSize.height()-H*sc)/2);
    addShapes({s}, undoLabel);
    zoomFit();
}
bool Canvas::importOra(const QString& path){
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray data=f.readAll(); f.close();
    const QMap<QString,QByteArray> zip=readZipEntries(data);
    if(zip.isEmpty()){ emit statusMessage("Not a readable OpenRaster (.ora) archive."); return false; }
    // Parse stack.xml → layers (listed top-first in the file). Nested groups are flattened.
    struct OraLayer { QString src, name; double x=0, y=0, opacity=1.0; };
    QVector<OraLayer> layers; double cw=0, ch=0;
    { const QByteArray sx=xqZipFind(zip,"stack.xml"); QXmlStreamReader r(sx);
      while(!r.atEnd()){ r.readNext(); if(!r.isStartElement()) continue;
          const QXmlStreamAttributes a=r.attributes();
          if(r.name()==QLatin1String("image")){ cw=a.value(QLatin1String("w")).toDouble(); ch=a.value(QLatin1String("h")).toDouble(); }
          else if(r.name()==QLatin1String("layer")){
              OraLayer l; l.src=a.value(QLatin1String("src")).toString(); l.name=a.value(QLatin1String("name")).toString();
              l.x=a.value(QLatin1String("x")).toDouble(); l.y=a.value(QLatin1String("y")).toDouble();
              if(a.hasAttribute("opacity")) l.opacity=a.value(QLatin1String("opacity")).toDouble();
              if(!l.src.isEmpty() && layers.size()<4096) layers.push_back(l); } } }   // cap layer COUNT (pixel budget below caps total decode)
    // Resolve a layer's bytes: exact src first, then basename (so "data/sub/x.png" isn't shadowed by "data/x.png").
    auto findLayer=[&](const QString& src)->QByteArray{ QByteArray b=xqZipFind(zip,src); if(b.isEmpty()) b=xqZipFind(zip,QFileInfo(src).fileName()); return b; };
    // PNG dimensions straight from the IHDR (ORA layers are PNG) — header-only, so the size-derivation pass
    // can't be turned into a decode bomb.
    auto pngSize=[](const QByteArray& b)->QSize{
        if(b.size()<24 || quint8(b[0])!=0x89 || b[1]!='P' || b[2]!='N' || b[3]!='G') return {};
        auto be=[&](int o){ return (quint32(quint8(b[o]))<<24)|(quint32(quint8(b[o+1]))<<16)|(quint32(quint8(b[o+2]))<<8)|quint8(b[o+3]); };
        const quint32 w=be(16), h=be(20);
        return (w==0||h==0||w>65535||h>65535) ? QSize() : QSize(int(w),int(h)); };
    // Sanitise the declared canvas size — a hostile stack.xml can inject inf/NaN/≤0 (Qt's toDouble parses "inf"/"nan").
    if(!qIsFinite(cw) || cw<=0) cw=0;
    if(!qIsFinite(ch) || ch<=0) ch=0;
    QVector<Shape> shapes;
    if(!layers.isEmpty()){
        // If the canvas size is missing, derive it from the union of ALL layer extents (header-only reads).
        if(cw<=0 || ch<=0){ double uw=0, uh=0;
            for(const OraLayer& l : layers){ const QSize sz=pngSize(findLayer(l.src)); if(!sz.isValid()) continue;
                const double lx=qIsFinite(l.x)?l.x:0, ly=qIsFinite(l.y)?l.y:0;
                uw=qMax(uw, lx+sz.width()); uh=qMax(uh, ly+sz.height()); }
            if(cw<=0) cw=uw; if(ch<=0) ch=uh; }
        if(!qIsFinite(cw) || cw<=0) cw=1;
        if(!qIsFinite(ch) || ch<=0) ch=1;
        const double sc=qMin(1.0, qMin(m_doc.pageSize.width()*0.9/cw, m_doc.pageSize.height()*0.9/ch));
        const double ox=(m_doc.pageSize.width()-cw*sc)/2, oy=(m_doc.pageSize.height()-ch*sc)/2;
        const qint64 kPixBudget=256LL*1024*1024; qint64 accPix=0;   // cumulative decode budget (many-huge-layers OOM guard)
        for(int i=layers.size()-1; i>=0; --i){ const OraLayer& l=layers[i];   // add bottom-first for correct z-order
            QImage im; const QByteArray lb=findLayer(l.src);
            if(lb.isEmpty() || !im.loadFromData(lb) || im.isNull()) continue;
            accPix += (qint64)im.width()*im.height(); if(accPix > kPixBudget) break;
            const double lw=im.width(), lh=im.height();
            const double lx=qIsFinite(l.x)?l.x:0, ly=qIsFinite(l.y)?l.y:0;   // never let inf/NaN reach the transform
            Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,lw,lh); s.name=l.name.isEmpty()?QStringLiteral("Layer"):l.name;
            s.fill.type=FillType::Bitmap; s.fill.image=im; s.fill.origImage=im; s.fill.tiled=false;
            s.fill.p0=QPointF(0,lh); s.fill.p1=QPointF(lw,lh); s.fill.p2=QPointF(0,0); s.stroke.enabled=false;
            s.opacity=qBound(0.0, qIsFinite(l.opacity)?l.opacity:1.0, 1.0);
            s.transform=QTransform::fromTranslate(lx,ly)*QTransform::fromScale(sc,sc)*QTransform::fromTranslate(ox,oy);
            shapes.push_back(s); }
    }
    if(!shapes.isEmpty()){ addShapes(shapes,"Import OpenRaster"); zoomFit();
        emit statusMessage(QStringLiteral("Imported OpenRaster — %1 editable layers.").arg(shapes.size())); return true; }
    // Fallback: the flattened composite.
    QImage merged; const QByteArray mb=xqZipFind(zip,"mergedimage.png");
    if(!mb.isEmpty() && merged.loadFromData(mb)){
        placeZipComposite(merged, QFileInfo(path).fileName(), QStringLiteral("Import OpenRaster"));
        emit statusMessage(QStringLiteral("Imported OpenRaster composite %1×%2.").arg(merged.width()).arg(merged.height())); return true; }
    emit statusMessage("No importable content found in the .ora."); return false;
}
bool Canvas::importKra(const QString& path){
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray data=f.readAll(); f.close();
    const QMap<QString,QByteArray> zip=readZipEntries(data);
    if(zip.isEmpty()){ emit statusMessage("Not a readable Krita (.kra) archive."); return false; }
    QImage merged; QByteArray mb=xqZipFind(zip,"mergedimage.png"); if(mb.isEmpty()) mb=xqZipFind(zip,"preview.png");
    if(mb.isEmpty() || !merged.loadFromData(mb)){ emit statusMessage("No importable image found in the .kra."); return false; }
    placeZipComposite(merged, QFileInfo(path).fileName()+QStringLiteral(" (flattened)"), QStringLiteral("Import Krita"));
    emit statusMessage(QStringLiteral("Imported Krita composite %1×%2 (layers flattened).").arg(merged.width()).arg(merged.height()));
    return true;
}
// Flatten an ODF content.xml's BODY text (text:p / text:h paragraphs, tabs, line-breaks) into a string.
// The `inBody` gate keeps out <office:automatic-styles>/<office:meta> text; used as the fallback for a
// text-centric ODF (ODT / OpenOffice.org 1.x .sxw) that carries no draw: vector shapes.
static QString xqOdfExtractText(const QByteArray& content){
    QString text; QXmlStreamReader r(content); r.setNamespaceProcessing(false); bool inBody=false;
    while(!r.atEnd()){ r.readNext();
        if(r.isStartElement()){ const QString ln=r.name().toString().section(':',-1);
            if(ln=="body") inBody=true;
            else if(inBody){ if(ln=="tab") text+='\t'; else if(ln=="line-break") text+='\n'; } }
        else if(r.isEndElement()){ const QString ln=r.name().toString().section(':',-1);
            if(ln=="body") inBody=false;
            else if(inBody && (ln=="p"||ln=="h")) text+='\n'; }
        else if(r.isCharacters() && inBody && !r.isWhitespace()) text += r.text();
    }
    return text.trimmed();
}
bool Canvas::importOdg(const QString& path){
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray data=f.readAll(); f.close();
    // Packaged ODF is a ZIP holding content.xml; a "flat" ODF (.fodg/.fodt/.fodp) is a single XML file.
    const QMap<QString,QByteArray> zip=readZipEntries(data);
    QByteArray content = zip.isEmpty()? QByteArray() : zip.value("content.xml");
    if(content.isEmpty() && data.contains("office:document")) content=data;   // flat ODF (not zipped)
    if(content.isEmpty()) return false;
    QVector<Shape> shapes;                       // finished TOP-LEVEL shapes
    // <draw:g> becomes a real XarQ Group. ODF groups are purely STRUCTURAL: the spec gives draw:g no
    // draw:transform and children carry absolute page coordinates, so the group takes an identity
    // transform and children keep their own — the render is unchanged, only the structure becomes
    // selectable/enterable (mirrors the SVG importer's foldSvgGroups).
    struct OpenG { Shape g; bool nest=true; };
    QVector<OpenG> gopen;
    auto emitShape=[&](const Shape& sh){ if(gopen.isEmpty()) shapes<<sh; else gopen.back().g.children.push_back(sh); };
    // Closing MOVES the finished subtree into its parent. Copying it (the obvious version) makes a
    // deeply nested document quadratic, since every close re-copies everything below it.
    auto closeGroup=[&](){ if(gopen.isEmpty()) return; OpenG o=std::move(gopen.back()); gopen.pop_back();
        if(o.g.children.isEmpty()) return;                                   // drop empty groups
        if(o.nest){ if(gopen.isEmpty()) shapes.push_back(std::move(o.g)); else gopen.back().g.children.push_back(std::move(o.g)); }
        else for(Shape& ch : o.g.children){ if(gopen.isEmpty()) shapes.push_back(std::move(ch)); else gopen.back().g.children.push_back(std::move(ch)); } };
    // Resolve the graphic style each shape references (draw:style-name → <style:graphic-properties>), so
    // imported shapes keep their REAL fill/stroke colours + widths instead of a single hardcoded default.
    struct OdfStyle { QString parent; int fill=-1, stroke=-1; QColor fillColor, strokeColor; double strokeWidth=-1; QString fillGrad; };
    struct OdfGrad  { QString style="linear"; QColor c1, c2; double angleDeg=0, cx=0.5, cy=0.5; };
    QHash<QString,OdfStyle> styleMap;
    QHash<QString,OdfGrad>  gradMap;   // <draw:gradient> colour ramps, referenced by draw:fill-gradient-name
    auto pct=[](const QString& s, double dflt)->double{ QString t=s.trimmed(); if(t.isEmpty()) return dflt; bool p=t.endsWith('%'); double v=t.remove('%').toDouble(); return p? v/100.0 : v; };
    auto collectStyles=[&](const QByteArray& bytes){
        if(bytes.isEmpty()) return;
        QXmlStreamReader r(bytes); r.setNamespaceProcessing(false); QString cur;
        while(!r.atEnd()){ if(r.readNext()!=QXmlStreamReader::StartElement) continue;
            const QString el=r.name().toString().section(':',-1); const QXmlStreamAttributes a=r.attributes();
            auto A=[&](const char* q){ QString v=a.value(QLatin1String(q)).toString(); if(v.isEmpty()) v=a.value(QString::fromLatin1(q).section(':',-1)).toString(); return v; };
            if(el=="style"){ cur=A("style:name"); if(!cur.isEmpty()) styleMap[cur].parent=A("style:parent-style-name"); }
            else if((el=="graphic-properties"||el=="drawing-page-properties") && !cur.isEmpty()){
                OdfStyle& s=styleMap[cur];
                const QString fill=A("draw:fill");         if(!fill.isEmpty()) s.fill=(fill=="none")?0:(fill=="gradient")?2:1;
                const QString fc=A("draw:fill-color");     if(!fc.isEmpty())   s.fillColor=QColor(fc.trimmed());
                const QString fg=A("draw:fill-gradient-name"); if(!fg.isEmpty()) s.fillGrad=fg;
                const QString st=A("draw:stroke");         if(!st.isEmpty())   s.stroke=(st=="none")?0:1;
                const QString sc=A("svg:stroke-color");    if(!sc.isEmpty())   s.strokeColor=QColor(sc.trimmed());
                const QString sw=A("svg:stroke-width");    if(!sw.isEmpty())   s.strokeWidth=xqOdfLen(sw);
            }
            else if(el=="gradient"){                       // <draw:gradient draw:name=… start/end-color, style, angle>
                const QString nm=A("draw:name"); if(nm.isEmpty()) continue;
                OdfGrad g; const QString stl=A("draw:style"); if(!stl.isEmpty()) g.style=stl;
                g.c1=QColor(A("draw:start-color").trimmed()); g.c2=QColor(A("draw:end-color").trimmed());
                const QString ang=A("draw:angle").trimmed();
                if(!ang.isEmpty()){ if(ang.endsWith("deg")) g.angleDeg=ang.chopped(3).toDouble(); else g.angleDeg=ang.toDouble()/10.0; }  // legacy draw:angle = 1/10 degree
                g.cx=pct(A("draw:cx"),0.5); g.cy=pct(A("draw:cy"),0.5);
                gradMap[nm]=g;
            }
        }
    };
    collectStyles(zip.value("styles.xml"));   // named/default styles + <draw:gradient> defs (may be parents)
    collectStyles(content);                   // <office:automatic-styles> (the per-shape graphic styles)
    // Flatten a style + its parent chain into effective fill/stroke (depth-capped against cyclic parents).
    auto resolve=[&](const QString& name)->OdfStyle{
        OdfStyle e; QString n=name; int guard=0;
        while(!n.isEmpty() && guard++<64){ auto it=styleMap.constFind(n); if(it==styleMap.constEnd()) break; const OdfStyle& s=it.value();
            if(e.fill<0&&s.fill>=0) e.fill=s.fill;                 if(!e.fillColor.isValid()&&s.fillColor.isValid()) e.fillColor=s.fillColor;
            if(e.fillGrad.isEmpty()&&!s.fillGrad.isEmpty()) e.fillGrad=s.fillGrad;
            if(e.stroke<0&&s.stroke>=0) e.stroke=s.stroke;         if(!e.strokeColor.isValid()&&s.strokeColor.isValid()) e.strokeColor=s.strokeColor;
            if(e.strokeWidth<0&&s.strokeWidth>=0) e.strokeWidth=s.strokeWidth; n=s.parent; }
        return e;
    };
    QXmlStreamReader xml(content); xml.setNamespaceProcessing(false);   // ODF names are svg:x / draw:points — keep the prefixes literal
    auto styleFor=[&](const QXmlStreamAttributes& at)->OdfStyle{        // resolved style + this shape's inline overrides
        auto AA=[&](const char* q){ QString v=at.value(QLatin1String(q)).toString(); if(v.isEmpty()) v=at.value(QString::fromLatin1(q).section(':',-1)).toString(); return v; };
        OdfStyle st=resolve(AA("draw:style-name"));
        const QString fill=AA("draw:fill");      if(!fill.isEmpty()) st.fill=(fill=="none")?0:(fill=="gradient")?2:1;
        const QString fg=AA("draw:fill-gradient-name"); if(!fg.isEmpty()) st.fillGrad=fg;
        const QString fc=AA("draw:fill-color");  if(!fc.isEmpty())   st.fillColor=QColor(fc.trimmed());
        const QString stk=AA("draw:stroke");     if(!stk.isEmpty())  st.stroke=(stk=="none")?0:1;
        const QString sc=AA("svg:stroke-color"); if(!sc.isEmpty())   st.strokeColor=QColor(sc.trimmed());
        const QString sw=AA("svg:stroke-width"); if(!sw.isEmpty())   st.strokeWidth=xqOdfLen(sw);
        return st;
    };
    auto mkPath=[&](const QPainterPath& pth, bool closedArea, const OdfStyle& st, const QTransform& xf=QTransform()){ Shape s; s.type=ShapeType::Path; s.path=pth; s.name="Shape"; s.transform=xf;
        auto git = (st.fill==2 && !st.fillGrad.isEmpty()) ? gradMap.constFind(st.fillGrad) : gradMap.constEnd();
        if(git!=gradMap.constEnd() && git.value().c1.isValid() && git.value().c2.isValid()){   // draw:fill="gradient" → real ramp
            const OdfGrad& g=git.value(); const QRectF bb=pth.boundingRect(); s.fill.stops.clear();
            const bool radial = g.style.contains("radial")||g.style.contains("ellipsoid")||g.style.contains("square")||g.style.contains("rect");
            if(radial){ s.fill.type=FillType::Radial;
                s.fill.stops.push_back({0.0,g.c2}); s.fill.stops.push_back({1.0,g.c1});   // ODF radial: end-colour at centre, start-colour at the rim
                const QPointF c(bb.left()+g.cx*bb.width(), bb.top()+g.cy*bb.height());
                s.fill.p0=c; s.fill.p1=c+QPointF(bb.width()*0.5,0); s.fill.p2=c+QPointF(0,bb.height()*0.5);
            } else { s.fill.type=FillType::Linear;
                if(g.style.contains("axial")){ s.fill.stops.push_back({0.0,g.c2}); s.fill.stops.push_back({0.5,g.c1}); s.fill.stops.push_back({1.0,g.c2}); }
                else { s.fill.stops.push_back({0.0,g.c1}); s.fill.stops.push_back({1.0,g.c2}); }
                const double th=g.angleDeg*M_PI/180.0; const QPointF dir(std::cos(th),std::sin(th)); const QPointF c=bb.center();
                double half=0.5*(std::abs(bb.width()*dir.x())+std::abs(bb.height()*dir.y())); if(half<1e-3) half=1.0;
                s.fill.p0=c-dir*half; s.fill.p1=c+dir*half;
            }
        } else {
            const bool filled = (st.fill==1) || (st.fill<0 && closedArea);   // explicit solid, or default: closed shapes fill
            if(filled){ s.fill.type=FillType::Solid; s.fill.color = st.fillColor.isValid()? st.fillColor : QColor(210,225,245); }
            else s.fill.type=FillType::None;
        }
        if(st.stroke==0) s.stroke.enabled=false;
        else { s.stroke.enabled=true; s.stroke.color = st.strokeColor.isValid()? st.strokeColor : QColor(40,44,52);
               s.stroke.width = st.strokeWidth>0? st.strokeWidth : 1.0; }
        emitShape(s); };
    while(!xml.atEnd()){
        const auto tok=xml.readNext();
        if(tok==QXmlStreamReader::EndElement){
            const QString eq=xml.name().toString(); const QString e2 = eq.contains(':')? eq.section(':',1) : eq;
            if(e2=="g") closeGroup();
            continue;
        }
        if(tok!=QXmlStreamReader::StartElement) continue;
        const QString elq=xml.name().toString(); const QString el = elq.contains(':')? elq.section(':',1) : elq;
        const QXmlStreamAttributes a=xml.attributes();
        if(el=="g"){                                    // <draw:g> — open a group (depth-capped like SVG)
            if(gopen.size()>=4096){ xml.skipCurrentElement(); continue; }   // bound the PARSE stack too
            OpenG o; o.g.type=ShapeType::Group; o.g.name="Group";
            { QString gn=a.value(QLatin1String("draw:name")).toString(); if(!gn.isEmpty()) o.g.name=gn; }
            o.nest = gopen.size()<64;                   // past the cap, children fold into the parent
            gopen.push_back(o); continue;
        }
        auto A=[&](const char* q)->QString{ QString v=a.value(QLatin1String(q)).toString();       // try "svg:x" then bare "x"
            if(v.isEmpty()){ const QString ln=QString::fromLatin1(q).section(':',-1); v=a.value(ln).toString(); } return v; };
        const QTransform oxf = xqOdfTransform(A("draw:transform"));   // ODF affine, kept live in Shape::transform
        if(el=="rect"){ const double x=xqOdfLen(A("svg:x")),y=xqOdfLen(A("svg:y")),w=xqOdfLen(A("svg:width")),h=xqOdfLen(A("svg:height"));
            if(w>0&&h>0){ QPainterPath pp; pp.addRect(x,y,w,h); mkPath(pp,true,styleFor(a),oxf); } }
        else if(el=="ellipse"||el=="circle"){ const double cx=xqOdfLen(A("svg:cx")),cy=xqOdfLen(A("svg:cy"));
            double rx=xqOdfLen(A("svg:rx")),ry=xqOdfLen(A("svg:ry")); if(el=="circle"){ rx=ry=xqOdfLen(A("svg:r")); }
            if(rx>0&&ry>0){ QPainterPath pp; pp.addEllipse(QPointF(cx,cy),rx,ry); mkPath(pp,true,styleFor(a),oxf); }
            else { const double x=xqOdfLen(A("svg:x")),y=xqOdfLen(A("svg:y")),w=xqOdfLen(A("svg:width")),h=xqOdfLen(A("svg:height"));
                   if(w>0&&h>0){ QPainterPath pp; pp.addEllipse(x,y,w,h); mkPath(pp,true,styleFor(a),oxf); } } }
        else if(el=="line"){ const double x1=xqOdfLen(A("svg:x1")),y1=xqOdfLen(A("svg:y1")),x2=xqOdfLen(A("svg:x2")),y2=xqOdfLen(A("svg:y2"));
            QPainterPath pp; pp.moveTo(x1,y1); pp.lineTo(x2,y2); mkPath(pp,false,styleFor(a),oxf); }
        else if(el=="polyline"||el=="polygon"||el=="path"){
            // draw:points / svg:d live in a viewBox coord space, placed by svg:x/y/width/height + svg:viewBox.
            const double px=xqOdfLen(A("svg:x")),py=xqOdfLen(A("svg:y")),pw=xqOdfLen(A("svg:width")),ph=xqOdfLen(A("svg:height"));
            double vx=0,vy=0,vw=0,vh=0; { const QStringList v=A("svg:viewBox").split(QRegularExpression("[ ,]+"),Qt::SkipEmptyParts);
                if(v.size()==4){ vx=v[0].toDouble(); vy=v[1].toDouble(); vw=v[2].toDouble(); vh=v[3].toDouble(); } }
            const bool haveBox = vw>0 && vh>0 && pw>0 && ph>0;
            if(el=="path"){                                          // curved shapes: reuse the SVG path-data parser
                const QString d=A("svg:d"); if(d.isEmpty()) continue;
                QPainterPath raw=xara::parseSvgPathData(d); if(raw.elementCount()==0) continue;
                QPainterPath pp=raw;
                if(haveBox){ QTransform t; t.translate(px,py); t.scale(pw/vw,ph/vh); t.translate(-vx,-vy); pp=t.map(raw); }
                mkPath(pp, d.contains('z')||d.contains('Z'), styleFor(a), oxf);
            } else {
                const QString pts=A("draw:points"); if(pts.isEmpty()) continue;
                QPainterPath pp; bool first=true;
                for(const QString& tp: pts.split(QRegularExpression("\\s+"),Qt::SkipEmptyParts)){
                    const QStringList xy=tp.split(','); if(xy.size()<2) continue;
                    double X=xy[0].toDouble(), Y=xy[1].toDouble();
                    if(haveBox){ X=px+(X-vx)/vw*pw; Y=py+(Y-vy)/vh*ph; }
                    if(first){ pp.moveTo(X,Y); first=false; } else pp.lineTo(X,Y); }
                if(!first){ const bool area=(el=="polygon"); if(area) pp.closeSubpath(); mkPath(pp,area,styleFor(a),oxf); } }
        }
    }
    while(!gopen.isEmpty()) closeGroup();          // a truncated document can leave groups open
    if(!shapes.isEmpty()){
        addShapes(shapes, "Import OpenDocument"); zoomFit();
        emit statusMessage(QString("Imported OpenDocument: %1 shape(s).").arg(shapes.size()));
        return true;
    }
    // No vector shapes — a text-centric ODF (ODT / OpenOffice.org 1.x .sxw). Flatten its body text into
    // an editable wrapping frame, the same way importDocx handles Word documents.
    const QString text=xqOdfExtractText(content);
    if(text.isEmpty()){ emit statusMessage("OpenDocument had no importable drawing or text."); return false; }
    Shape s; s.type=ShapeType::Text; s.text=text; s.textFrame=true; s.name=QFileInfo(path).fileName();
    s.fill.type=FillType::Solid; s.fill.color=Qt::black; s.stroke.enabled=false;
    const double w=qMin(m_doc.pageSize.width()*0.8, 520.0);
    s.rect=QRectF(0,0,w,qMax(80.0,m_doc.pageSize.height()*0.8));
    s.transform=QTransform::fromTranslate(m_doc.pageSize.width()*0.1, m_doc.pageSize.height()*0.08);
    addShapes({s},"Import OpenDocument");
    emit statusMessage("Imported OpenDocument text.");
    return true;
}

// Shared tail: fit source-space paths to the page and add them as editable Path shapes.
static QVector<Shape> xqPathsToShapes(QVector<QPainterPath>& paths, const QVector<bool>& closed,
                                      QSizeF page, bool flipY, const QColor& areaFill){
    QVector<Shape> out;
    QRectF bbox;
    for(const QPainterPath& p: paths){ if(p.elementCount()==0) continue;
        bbox = bbox.isNull()? p.boundingRect() : bbox.united(p.boundingRect()); }
    const QTransform t=xqFitTransform(bbox, page, 40, flipY);
    for(int i=0;i<paths.size();++i){ QPainterPath p=paths[i]; if(p.elementCount()==0) continue;
        p=t.map(p);
        Shape s; s.type=ShapeType::Path; s.path=p; s.name=QStringLiteral("Path");
        const bool area = (i<closed.size() && closed[i]);
        if(area && areaFill.isValid()){ s.fill.type=FillType::Solid; s.fill.color=areaFill; }
        else s.fill.type=FillType::None;
        s.stroke.enabled=true; s.stroke.color=QColor(40,44,52); s.stroke.width=1.0;
        out<<s;
    }
    return out;
}

bool Canvas::importGeoJson(const QString& path){
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray data=f.readAll(); f.close();
    QJsonParseError err; const QJsonDocument jd=QJsonDocument::fromJson(data,&err);
    if(err.error!=QJsonParseError::NoError || !jd.isObject()) return false;
    const QJsonObject root=jd.object();
    QJsonArray feats;
    const QString rt=root.value("type").toString();
    if(rt=="FeatureCollection") feats=root.value("features").toArray();
    else if(rt=="Feature") feats.append(root);
    else { QJsonObject wrap; wrap.insert("type","Feature"); wrap.insert("geometry",root); feats.append(wrap); }

    QVector<QPainterPath> paths; QVector<bool> closed;
    auto ring=[&](const QJsonArray& coords, bool close){ QPainterPath p; bool first=true;
        for(const QJsonValue& cv: coords){ const QJsonArray xy=cv.toArray(); if(xy.size()<2) continue;
            const QPointF pt(xy[0].toDouble(), xy[1].toDouble());
            if(first){ p.moveTo(pt); first=false; } else p.lineTo(pt); }
        if(close && !first) p.closeSubpath();
        if(!first){ paths<<p; closed<<close; } };
    std::function<void(const QJsonObject&)> geom=[&](const QJsonObject& g){
        const QString gt=g.value("type").toString(); const QJsonArray c=g.value("coordinates").toArray();
        if(gt=="Point"){ if(c.size()>=2){ const QPointF pt(c[0].toDouble(),c[1].toDouble());
                QPainterPath p; p.addEllipse(pt, 0.0001, 0.0001); paths<<p; closed<<false; } }
        else if(gt=="LineString") ring(c,false);
        else if(gt=="MultiPoint"){ for(const QJsonValue& v: c){ const QJsonArray xy=v.toArray(); if(xy.size()>=2){
                QPainterPath p; p.addEllipse(QPointF(xy[0].toDouble(),xy[1].toDouble()),0.0001,0.0001); paths<<p; closed<<false; } } }
        else if(gt=="MultiLineString"){ for(const QJsonValue& v: c) ring(v.toArray(),false); }
        else if(gt=="Polygon"){ for(const QJsonValue& v: c) ring(v.toArray(),true); }
        else if(gt=="MultiPolygon"){ for(const QJsonValue& poly: c) for(const QJsonValue& v: poly.toArray()) ring(v.toArray(),true); }
        else if(gt=="GeometryCollection"){ for(const QJsonValue& v: g.value("geometries").toArray()) geom(v.toObject()); } };
    for(const QJsonValue& fv: feats){ const QJsonObject o=fv.toObject().value("geometry").toObject(); if(!o.isEmpty()) geom(o); }
    if(paths.isEmpty()) return false;
    QVector<Shape> shapes=xqPathsToShapes(paths, closed, m_doc.pageSize, /*flipY*/true, QColor(210,225,245));  // lat grows up
    if(shapes.isEmpty()) return false;
    addShapes(shapes, "Import GeoJSON"); zoomFit();
    emit statusMessage(QString("Imported GeoJSON: %1 shape(s).").arg(shapes.size()));
    return true;
}

bool Canvas::importKml(const QString& path){
    QByteArray data;
    if(path.endsWith(".kmz",Qt::CaseInsensitive)){                    // KMZ = zip; find the doc.kml
        QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false; const QByteArray z=f.readAll(); f.close();
        const QMap<QString,QByteArray> zip=readZipEntries(z);
        if(zip.contains("doc.kml")) data=zip.value("doc.kml");
        else for(auto it=zip.begin(); it!=zip.end(); ++it) if(it.key().endsWith(".kml",Qt::CaseInsensitive)){ data=it.value(); break; }
    } else { QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false; data=f.readAll(); }
    if(data.isEmpty()) return false;
    QVector<QPainterPath> paths; QVector<bool> closed;
    QXmlStreamReader xml(data); int polyDepth=0;
    while(!xml.atEnd()){ const auto tok=xml.readNext();
        if(tok==QXmlStreamReader::StartElement){ const QString n=xml.name().toString();
            if(n=="Polygon"||n=="LinearRing") ++polyDepth;
            else if(n=="coordinates"){ const QString text=xml.readElementText();
                QPainterPath p; bool first=true;
                const QStringList tuples=text.simplified().split(' ',Qt::SkipEmptyParts);
                for(const QString& tp: tuples){ const QStringList xy=tp.split(','); if(xy.size()<2) continue;
                    const QPointF pt(xy[0].toDouble(), xy[1].toDouble());
                    if(first){ p.moveTo(pt); first=false; } else p.lineTo(pt); }
                if(!first){ const bool close=polyDepth>0; if(close) p.closeSubpath(); paths<<p; closed<<close; } }
        } else if(tok==QXmlStreamReader::EndElement){ const QString n=xml.name().toString();
            if((n=="Polygon"||n=="LinearRing") && polyDepth>0) --polyDepth; }
    }
    if(paths.isEmpty()) return false;
    QVector<Shape> shapes=xqPathsToShapes(paths, closed, m_doc.pageSize, /*flipY*/true, QColor(210,225,245));
    if(shapes.isEmpty()) return false;
    addShapes(shapes, "Import KML"); zoomFit();
    emit statusMessage(QString("Imported KML: %1 shape(s).").arg(shapes.size()));
    return true;
}

bool Canvas::importHpgl(const QString& path){
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false;
    const QString s=QString::fromLatin1(f.readAll()); f.close();
    QPainterPath cur; double curx=0,cury=0; bool penDown=false, absolute=true, started=false;
    int i=0; const int N=s.size();
    while(i<N){
        while(i<N && !s[i].isLetter()) ++i;
        if(i+1>=N) break;
        const QString cmd=s.mid(i,2).toUpper(); i+=2;
        QString params;
        while(i<N){ const QChar c=s[i];
            if(c.isDigit()||c=='-'||c=='+'||c=='.'||c==','||c==' '||c=='\t'||c=='\r'||c=='\n'){ params+=c; ++i; } else break; }
        if(cmd=="PU"||cmd=="PD"||cmd=="PA"||cmd=="PR"){
            if(cmd=="PU") penDown=false; else if(cmd=="PD") penDown=true;
            else if(cmd=="PA") absolute=true; else if(cmd=="PR") absolute=false;
            QList<double> nums; const auto parts=params.split(QRegularExpression("[,\\s]+"),Qt::SkipEmptyParts);
            for(const QString& p: parts) nums<<p.toDouble();
            for(int k=0;k+1<nums.size();k+=2){
                const double nx=absolute? nums[k]   : curx+nums[k];
                const double ny=absolute? nums[k+1] : cury+nums[k+1];
                if(penDown){ if(!started){ cur.moveTo(curx,cury); started=true; } cur.lineTo(nx,ny); }
                else { cur.moveTo(nx,ny); started=true; }
                curx=nx; cury=ny;
            }
        }
    }
    if(cur.elementCount()==0) return false;
    QVector<QPainterPath> paths{cur}; QVector<bool> closed{false};
    QVector<Shape> shapes=xqPathsToShapes(paths, closed, m_doc.pageSize, /*flipY*/true, QColor());   // plotter Y grows up; stroke-only
    if(shapes.isEmpty()) return false;
    addShapes(shapes, "Import HP-GL"); zoomFit();
    emit statusMessage(QString("Imported HP-GL plot: %1 path(s).").arg(shapes.size()));
    return true;
}
