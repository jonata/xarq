#include "xar.h"
#include "render.h"
#include "pathedit.h"
#include <QFile>
#include <QBuffer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>
#include <QHash>
#include <QPainterPath>
#include <QImage>
#include <QPainter>
#include <QFont>
#include <QFontInfo>
#include <vector>
#include <functional>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <zlib.h>

namespace xara {
namespace {

// ---- font substitution -----------------------------------------------------------------------------
// When the document's named font isn't installed, Qt falls back to one generic default (Noto Sans),
// ignoring the font's style. We classify the font (by its PANOSE number, else by name) and substitute a
// same-CLASS installed family, so a serif stays a serif, a script stays a script, etc. — fixing both the
// glyphs and the metrics. Installed fonts are left untouched.
enum class FontClass { Unknown, Serif, Sans, Mono, Script, Decorative };

// PANOSE-1 (10 bytes): FamilyType, SerifStyle, Weight, Proportion, ... (Kernel ccpanose.h). All-zero or
// FamilyType 0/1 (Any/NoFit) -> Unknown (XaraLX itself writes zeros; real Windows files carry real data).
static FontClass classifyPanose(const quint8* p){
    bool z=true; for(int i=0;i<10;i++) if(p[i]){ z=false; break; } if(z) return FontClass::Unknown;
    switch(p[0]){
        case 3: return FontClass::Script;                          // PAN_FAMILY_SCRIPT
        case 4: case 5: return FontClass::Decorative;              // DECORATIVE / PICTORIAL
        case 2:                                                    // PAN_FAMILY_TEXT_DISPLAY (Latin text)
            if(p[3]==9) return FontClass::Mono;                    // Proportion == Monospaced
            if(p[1]>=11 && p[1]<=15) return FontClass::Sans;       // SerifStyle 11..15 = sans
            return FontClass::Serif;                               // 2..10 = serif (0/1 -> default serif)
        default: return FontClass::Unknown;                        // 0/1
    }
}
// Heuristic classification by font NAME (used when PANOSE is absent/zero). mono/script/decorative before
// serif/sans so e.g. "Courier MT" hits mono. Case-insensitive substring tests.
static FontClass classifyByName(const QString& n){
    const QString s=n.toLower();
    auto has=[&](const char* w){ return s.contains(QLatin1String(w)); };
    if(has("mono")||has("courier")||has("consol")||has("typewriter")||has("console")||has("terminal")||has(" code")) return FontClass::Mono;
    if(has("script")||has("calligraph")||has("brush")||has("chancery")||has("cursive")||has("margaret")||has("vivaldi")||has("zapf")||has("hand")) return FontClass::Script;
    if(has("stencil")||has("jokerman")||has("ornament")||has("impact")) return FontClass::Decorative;
    if(has("times")||has("antiqua")||has("garamond")||has("georgia")||has("roman")||has("serif")||has("minion")||has("palatino")||has("bodoni")||has("caslon")||has("calisto")||has("century")||s.endsWith(QLatin1String(" mt"))) return FontClass::Serif;
    if(has("arial")||has("helvet")||has("myriad")||has("verdana")||has("tahoma")||has("sans")||has("frutiger")||has("gill")||has("calibri")||has("segoe")||has("open ")) return FontClass::Sans;
    return FontClass::Sans;
}
static QString pickInstalled(const QStringList& cands){
    for(const QString& c: cands){ QFont f(c);
        if(QString::compare(QFontInfo(f).family(), c, Qt::CaseInsensitive)==0) return c; }
    return cands.isEmpty()? QString() : cands.last();   // last resort: let Qt fall back
}
// A concrete installed substitute family for a class (cached; first installed candidate wins).
static QString substituteFamily(FontClass c){
    static bool init=false; static QString sSerif,sSans,sMono,sScript,sDecor;
    if(!init){ init=true;
        sSerif =pickInstalled({"Liberation Serif","Times New Roman","Noto Serif","DejaVu Serif","Georgia"});
        sSans  =pickInstalled({"Liberation Sans","Arial","DejaVu Sans","Noto Sans","Verdana"});
        sMono  =pickInstalled({"Liberation Mono","DejaVu Sans Mono","Courier New","Courier 10 Pitch"});
        sScript=pickInstalled({"URW Chancery L","Z003","Comic Sans MS","DejaVu Sans"});
        sDecor =pickInstalled({"Comic Sans MS","Tahoma","DejaVu Sans"}); }
    switch(c){ case FontClass::Serif: return sSerif; case FontClass::Mono: return sMono;
               case FontClass::Script: return sScript; case FontClass::Decorative: return sDecor;
               default: return sSans; }
}
// Effective family for a document font name + its PANOSE: the name if installed, else a same-class substitute.
static QString resolveFontFamily(const QString& name, const quint8* panose){
    if(name.isEmpty()) return name;
    if(QString::compare(QFontInfo(QFont(name)).family(), name, Qt::CaseInsensitive)==0) return name;  // installed
    FontClass c=classifyPanose(panose);
    if(c==FontClass::Unknown){ c=classifyByName(name);
        // With no PANOSE we're only GUESSING from the name. The real CDraw engine substitutes a single
        // default SANS for any font it can't find (verified: unresolved "Margaret"/"Myriad Web" render
        // identical width in the oracle) — so never invent a Script/Decorative face from the name alone
        // (that mis-rendered e.g. "Margaret" as Comic Sans). Keep a genuine Serif/Mono name guess.
        if(c==FontClass::Script || c==FontClass::Decorative) c=FontClass::Sans; }
    const QString sub=substituteFamily(c); return sub.isEmpty()? name : sub;
}

// Rasterize a captured brush-nib group (vector shapes, already in screen space) to a tight ARGB
// stamp, so the existing paintBrush() can stamp it along a brush spine. docPerPx returns the doc
// units each ink pixel represents (so the caller can restore the nib's natural size). Null if degenerate.
static QImage rasterizeNib(const QVector<Shape>& objs, double& docPerPx){
    docPerPx=1.0;
    std::function<QRectF(const Shape&)> bnds=[&](const Shape& s)->QRectF{
        QRectF b=s.docBounds();
        for(const Shape& c: s.children){ QRectF cb=bnds(c); if(!cb.isNull()) b=b.isNull()?cb:b.united(cb); }
        return b; };
    QRectF bb; for(const Shape& o: objs){ QRectF ob=bnds(o); if(!ob.isNull()) bb=bb.isNull()?ob:bb.united(ob); }
    if(!bb.isValid() || bb.width()<0.5 || bb.height()<0.5) return QImage();
    double sc=1.0, md=qMax(bb.width(),bb.height()); if(md>512.0) sc=512.0/md;
    int W=qMax(1,(int)std::ceil(bb.width()*sc)), H=qMax(1,(int)std::ceil(bb.height()*sc));
    if((qint64)W*H > (1<<21)) return QImage();
    QImage img(W,H,QImage::Format_ARGB32_Premultiplied); img.fill(Qt::transparent);
    QPainter p(&img); p.setRenderHint(QPainter::Antialiasing);
    p.scale(sc,sc); p.translate(-bb.topLeft());
    for(const Shape& o: objs) paintShape(p,o);
    p.end(); docPerPx=1.0/sc; return img;
}

// ---- record tags (from the Xar spec / engine cxftags.h) --------------------
enum Tag : quint32 {
    // Private XarQ record: a qCompress'd Document JSON carrying features XAR can't express
    // (rich text runs, named styles, web-layout attrs, spot colours…). Real Xara + our own reader
    // skip unknown records via the size field, so its presence never affects other readers.
    TAG_XSTUDIO_DOC=0xE57D0C,
    TAG_UP=0, TAG_DOWN=1, TAG_FILEHEADER=2, TAG_ENDOFFILE=3,
    TAG_STARTCOMPRESSION=30, TAG_ENDCOMPRESSION=31,
    TAG_DOCUMENT=40, TAG_CHAPTER=41, TAG_SPREAD=42, TAG_LAYER=43, TAG_PAGE=44,
    TAG_SPREADINFORMATION=45, TAG_LAYERDETAILS=48, TAG_GUIDELAYERDETAILS=49,
    TAG_DEFINERGBCOLOUR=50, TAG_DEFINECOMPLEXCOLOUR=51,
    // embedded bitmap definitions: UTF-16LE null-terminated name + raw image-file bytes
    TAG_DEFINEBITMAP_BMP=65, TAG_DEFINEBITMAP_GIF=66, TAG_DEFINEBITMAP_JPEG=67,
    TAG_DEFINEBITMAP_PNG=68, TAG_DEFINEBITMAP_BMPZIP=69, TAG_DEFINEBITMAP_JPEG8BPP=71,
    TAG_DEFINEBITMAP_PNG_REAL=4138,
    TAG_BITMAPFILL=157, TAG_CONTONEBITMAPFILL=158, TAG_FRACTALFILL=159,
    TAG_BITMAPTRANSPARENTFILL=171, TAG_FRACTALTRANSPARENTFILL=172,
    TAG_NODE_BITMAP=198, TAG_NODE_CONTONEDBITMAP=199,
    TAG_PATH=100, TAG_PATH_FILLED=101, TAG_PATH_STROKED=102, TAG_PATH_FILLED_STROKED=103,
    TAG_GROUP=104,
    TAG_PATH_RELATIVE=113, TAG_PATH_RELATIVE_FILLED=114, TAG_PATH_RELATIVE_STROKED=115,
    TAG_PATH_RELATIVE_FILLED_STROKED=116, TAG_PATHREF_TRANSFORM=118,
    TAG_FLATFILL=150, TAG_LINECOLOUR=151, TAG_LINEWIDTH=152,
    TAG_LINEARFILL=153, TAG_CIRCULARFILL=154, TAG_ELLIPTICALFILL=155, TAG_CONICALFILL=156,
    TAG_FLATTRANSPARENTFILL=166, TAG_LINEARTRANSPARENTFILL=167,
    TAG_CIRCULARTRANSPARENTFILL=168, TAG_ELLIPTICALTRANSPARENTFILL=169, TAG_CONICALTRANSPARENTFILL=170,
    TAG_LINEARFILLMULTISTAGE=4075, TAG_CIRCULARFILLMULTISTAGE=4076,
    TAG_ELLIPTICALFILLMULTISTAGE=4077, TAG_CONICALFILLMULTISTAGE=4078, TAG_SQUAREFILLMULTISTAGE=4088,
    TAG_LINEARFILL3POINT=4121, TAG_LINEARTRANSPARENTFILL3POINT=4123,
    TAG_FILLEFFECT_FADE=160, TAG_FILLEFFECT_RAINBOW=161, TAG_FILLEFFECT_ALTRAINBOW=162,
    TAG_FILL_REPEATING=163, TAG_FILL_NONREPEATING=164, TAG_FILL_REPEATINGINVERTED=165,
    TAG_TRANSPARENTFILL_REPEATING=180, TAG_TRANSPARENTFILL_NONREPEATING=181, TAG_TRANSPARENTFILL_REPEATINGINVERTED=182,
    TAG_DASHSTYLE=183, TAG_STARTARROW=185, TAG_ENDARROW=186,
    TAG_SQUAREFILL=200, TAG_SQUARETRANSPARENTFILL=201, TAG_THREECOLFILL=202,
    TAG_THREECOLTRANSPARENTFILL=203, TAG_FOURCOLFILL=204, TAG_FOURCOLTRANSPARENTFILL=205,
    TAG_FILL_REPEATING_EXTRA=206, TAG_TRANSPARENTFILL_REPEATING_EXTRA=207,   // newer repeating-graduated variant (Kernel Repeat=4 -> forward tile)
    TAG_LINETRANSPARENCY=173,
    TAG_STARTCAP=174, TAG_ENDCAP=175, TAG_JOINSTYLE=176, TAG_MITRELIMIT=177, TAG_WINDINGRULE=178,
    TAG_FLATFILL_NONE=190, TAG_FLATFILL_BLACK=191, TAG_FLATFILL_WHITE=192,
    TAG_LINECOLOUR_NONE=193, TAG_LINECOLOUR_BLACK=194, TAG_LINECOLOUR_WHITE=195,
    TAG_ELLIPSE_SIMPLE=1000, TAG_ELLIPSE_COMPLEX=1001,
    TAG_RECTANGLE_SIMPLE=1100, TAG_RECTANGLE_SIMPLE_ROUNDED=1104,
    TAG_RECTANGLE_COMPLEX=1108,
    TAG_POLYGON_COMPLEX=1200,
    TAG_REGULAR_SHAPE_PHASE_2=1901,
    TAG_BLEND=105, TAG_BLENDER=106, TAG_BLENDPROFILES=4072, TAG_BLENDERADDITIONAL=4073,
    TAG_SHADOWCONTROLLER=4050, TAG_SHADOW=4051,
    TAG_CONTOURCONTROLLER=4066, TAG_CONTOUR=4067, TAG_BEVEL=4052, TAG_BEVELINK=4057,
    TAG_CLIPVIEWCONTROLLER=4084, TAG_CLIPVIEW=4085, TAG_CLIPVIEW_PATH=4137,
    TAG_VARIABLEWIDTHFUNC=4000, TAG_VARIABLEWIDTHTABLE=4001, TAG_STROKETYPE=4002, TAG_STROKEDEFINITION=4003,
    TAG_FEATHER_EFFECT=4127,
    TAG_FEATHER=4086, TAG_NOISEFILL=4010, TAG_NOISETRANSPARENTFILL=4011, TAG_BRUSHDEFINITION=4080, TAG_BRUSHDATA=4081,
    TAG_MOULD_ENVELOPE=107, TAG_MOULD_PERSPECTIVE=108, TAG_MOULD_GROUP=109,
    TAG_MOULD_PATH=110, TAG_MOULD_BOUNDS=4012,
    TAG_TEXT_STORY_SIMPLE=2100, TAG_TEXT_STORY_COMPLEX=2101,
    TAG_TEXT_STORY_SIMPLE_START_LEFT=2110, TAG_TEXT_STORY_SIMPLE_START_RIGHT=2111,
    TAG_TEXT_STORY_SIMPLE_END_LEFT=2112, TAG_TEXT_STORY_SIMPLE_END_RIGHT=2113,
    TAG_TEXT_STORY_COMPLEX_START_LEFT=2114, TAG_TEXT_STORY_COMPLEX_START_RIGHT=2115,
    TAG_TEXT_STORY_COMPLEX_END_LEFT=2116, TAG_TEXT_STORY_COMPLEX_END_RIGHT=2117,
    TAG_TEXT_LINE=2200, TAG_TEXT_STRING=2201, TAG_TEXT_CHAR=2202, TAG_TEXT_EOL=2203,
    TAG_TEXT_KERN=2204, TAG_TEXT_LINE_INFO=2206,
    TAG_TEXT_STORY_WORD_WRAP_INFO=2150, TAG_TEXT_STORY_INDENT_INFO=2151,
    TAG_FONT_DEF_TRUETYPE=2000, TAG_FONT_DEF_ATM=2001,
    TAG_TEXT_LINESPACE_RATIO=2900, TAG_TEXT_JUSTIFICATION_LEFT=2902,
    TAG_TEXT_JUSTIFICATION_CENTRE=2903, TAG_TEXT_JUSTIFICATION_RIGHT=2904, TAG_TEXT_JUSTIFICATION_FULL=2905,
    TAG_TEXT_FONT_SIZE=2906, TAG_TEXT_FONT_TYPEFACE=2907, TAG_TEXT_BOLD_ON=2908, TAG_TEXT_BOLD_OFF=2909,
    TAG_TEXT_ITALIC_ON=2910, TAG_TEXT_ITALIC_OFF=2911, TAG_TEXT_LINESPACE_ABSOLUTE=2901,
    TAG_TEXT_SCRIPT_ON=2914, TAG_TEXT_SCRIPT_OFF=2915, TAG_TEXT_SUPERSCRIPT_ON=2916, TAG_TEXT_SUBSCRIPT_ON=2917,
    TAG_TEXT_TRACKING=2918, TAG_TEXT_ASPECT_RATIO=2919, TAG_TEXT_BASELINE=2920,
};

// path verb byte: base op in the high bits, bottom bit closes the sub-path
enum { PT_MOVETO=6, PT_LINETO=2, PT_BEZIERTO=4, PT_CLOSE=1 };

const double MP = 750.0;   // millipoints per document unit (doc units == 96-dpi px)

// ---- little-endian writers -------------------------------------------------
void wU8 (QByteArray& b, quint8 v){ b.append((char)v); }
void wU16(QByteArray& b, quint16 v){ char t[2]; qToLittleEndian(v,(uchar*)t); b.append(t,2); }
void wU32(QByteArray& b, quint32 v){ char t[4]; qToLittleEndian(v,(uchar*)t); b.append(t,4); }
void wI32(QByteArray& b, qint32  v){ char t[4]; qToLittleEndian(v,(uchar*)t); b.append(t,4); }
void wF64(QByteArray& b, double  v){ quint64 u; memcpy(&u,&v,8); char t[8]; qToLittleEndian(u,(uchar*)t); b.append(t,8); }
void wAsciiZ(QByteArray& b, const char* s){ b.append(s); b.append('\0'); }
void wUniZ(QByteArray& b, const QString& s){ for(QChar c: s) wU16(b,c.unicode()); wU16(b,0); }

// ---- little-endian readers (bounds-checked) --------------------------------
quint32 rdU32(const QByteArray& d, int o){ return (o+4<=d.size())? qFromLittleEndian<quint32>((const uchar*)d.constData()+o) : 0; }
qint32  rdI32(const QByteArray& d, int o){ return (o+4<=d.size())? qFromLittleEndian<qint32 >((const uchar*)d.constData()+o) : 0; }
quint16 rdU16(const QByteArray& d, int o){ return (o+2<=d.size())? qFromLittleEndian<quint16>((const uchar*)d.constData()+o) : 0; }
double  rdF64(const QByteArray& d, int o){ if(o+8>d.size()) return 0; quint64 u=qFromLittleEndian<quint64>((const uchar*)d.constData()+o); double v; memcpy(&v,&u,8); return v; }
float   rdF32(const QByteArray& d, int o){ if(o+4>d.size()) return 0; quint32 u=qFromLittleEndian<quint32>((const uchar*)d.constData()+o); float v; memcpy(&v,&u,4); return v; }

// =====================================================================  WRITER
struct Writer {
    QByteArray out;
    qint32 seq = 0;                 // sequence number of the last record written
    double pageH = 800;
    QHash<QRgb,qint32> colourSeq;   // colour -> sequence number of its definition record
    quint32 nextStrokeHandle = 1;   // next vector stroke-definition handle
    quint32 pendStrokeHandle = 0;   // handle to reference from the next path's STROKETYPE (0 = none)

    qint32 rec(quint32 tag, const QByteArray& data=QByteArray()){
        wU32(out, tag); wU32(out, (quint32)data.size()); out.append(data);
        return ++seq;
    }
    qint32 toX(double x) const { return (qint32)qRound(x*MP); }
    qint32 toY(double y) const { return (qint32)qRound((pageH - y)*MP); }   // flip to y-up

    // Define a colour (once) and return the COLOURREF (its record sequence number).
    qint32 colourRef(const QColor& c){
        QRgb k = c.rgb();
        auto it = colourSeq.find(k);
        if(it!=colourSeq.end()) return it.value();
        QByteArray d; wU8(d,c.red()); wU8(d,c.green()); wU8(d,c.blue());
        qint32 s = rec(TAG_DEFINERGBCOLOUR, d);
        colourSeq.insert(k, s);
        return s;
    }

    // Embed a bitmap (once) as a PNG DEFINEBITMAP record and return its record sequence number.
    QHash<qint64,qint32> bitmapSeq;
    qint32 bitmapRef(const QImage& img){
        qint64 k = img.cacheKey();
        auto it = bitmapSeq.find(k);
        if(it!=bitmapSeq.end()) return it.value();
        QByteArray png; QBuffer buf(&png); buf.open(QIODevice::WriteOnly); img.save(&buf,"PNG"); buf.close();
        QByteArray d; wUniZ(d, QStringLiteral("img"));   // UTF-16LE null-terminated name
        d.append(png);                                    // then the raw PNG file bytes
        qint32 s = rec(TAG_DEFINEBITMAP_PNG, d);
        bitmapSeq.insert(k, s);
        return s;
    }

    void emitFill(const Shape& s, const QTransform& xf){
        const Fill& f = s.fill;
        auto gradCols = [&](QColor& c0, QColor& c1){
            c0 = f.stops.isEmpty()? f.color : f.stops.first().color;
            c1 = f.stops.isEmpty()? f.color : f.stops.last().color; };
        // multi-stage (>2-stop) linear/radial gradient: start/end + NumCols middle (pos,colour)
        auto emitGrad=[&](quint32 twoPtTag, quint32 multiTag){
            QPointF p0=xf.map(f.p0), p1=xf.map(f.p1);
            if(f.stops.size()>2){
                qint32 r0=colourRef(f.stops.first().color), r1=colourRef(f.stops.last().color);
                QVector<qint32> mid; for(int i=1;i<f.stops.size()-1;++i) mid<<colourRef(f.stops[i].color);
                QByteArray d; wI32(d,toX(p0.x())); wI32(d,toY(p0.y())); wI32(d,toX(p1.x())); wI32(d,toY(p1.y()));
                wI32(d,r0); wI32(d,r1); wU32(d,(quint32)mid.size());
                for(int i=0;i<mid.size();++i){ wF64(d, f.stops[i+1].pos); wI32(d, mid[i]); }
                rec(multiTag,d);
            } else {
                QColor c0,c1; gradCols(c0,c1); qint32 r0=colourRef(c0), r1=colourRef(c1);
                QByteArray d; wI32(d,toX(p0.x())); wI32(d,toY(p0.y())); wI32(d,toX(p1.x())); wI32(d,toY(p1.y()));
                wI32(d,r0); wI32(d,r1); wF64(d,0.0); wF64(d,0.0); rec(twoPtTag,d);
            }
        };
        switch(f.type){
        case FillType::None: rec(TAG_FLATFILL_NONE); break;
        case FillType::Solid: { QByteArray d; wI32(d, colourRef(f.color.isValid()?f.color:QColor(Qt::black))); rec(TAG_FLATFILL,d); } break;
        case FillType::Linear: case FillType::Conical: emitGrad(TAG_LINEARFILL, TAG_LINEARFILLMULTISTAGE); break;
        case FillType::Radial: emitGrad(TAG_CIRCULARFILL, TAG_CIRCULARFILLMULTISTAGE); break;
        case FillType::Bitmap: {
            if(f.image.isNull()){ QByteArray d; wI32(d, colourRef(f.contone? f.color : QColor(150,150,150))); rec(TAG_FLATFILL,d); break; }
            qint32 bref = bitmapRef(f.image);             // emit the embedded bitmap (defined before this fill)
            QPointF p0=xf.map(f.p0), p1=xf.map(f.p1), p2=xf.map(f.p2);
            QByteArray d;
            wI32(d,toX(p0.x())); wI32(d,toY(p0.y())); wI32(d,toX(p1.x())); wI32(d,toY(p1.y())); wI32(d,toX(p2.x())); wI32(d,toY(p2.y()));
            if(f.contone){ wI32(d, colourRef(f.color)); wI32(d, colourRef(f.color2)); }
            wI32(d, bref); wF64(d,0.0); wF64(d,0.0);      // bitmap ref + bias/gain (reader ignores the profile)
            rec(f.contone? TAG_CONTONEBITMAPFILL : TAG_BITMAPFILL, d);
        } break;
        case FillType::Mesh: case FillType::Square: {
            QPointF p0=xf.map(f.p0), p1=xf.map(f.p1), p2=xf.map(f.p2);
            QByteArray d;
            wI32(d,toX(p0.x())); wI32(d,toY(p0.y())); wI32(d,toX(p1.x())); wI32(d,toY(p1.y())); wI32(d,toX(p2.x())); wI32(d,toY(p2.y()));
            if(f.type==FillType::Square){
                wI32(d, colourRef(f.meshColors.value(0, QColor(Qt::black))));
                wI32(d, colourRef(f.meshColors.value(1, QColor(Qt::white))));
                wF64(d,0.0); wF64(d,0.0); rec(TAG_SQUAREFILL,d);          // 48 bytes (3 coords + 2 cols + bias/gain)
            } else {
                const int n = qBound(3, f.meshColors.size(), 4);
                for(int k=0;k<n;++k) wI32(d, colourRef(f.meshColors.value(k, QColor(Qt::black))));
                rec(n>=4? TAG_FOURCOLFILL : TAG_THREECOLFILL, d);        // 3 coords + n colour refs
            }
        } break;
        }
        // winding rule: only emit when non-default (the reader assumes even-odd otherwise).
        if(s.fillRule==Qt::WindingFill){ QByteArray d; wU8(d,0 /*NonZeroWinding*/); rec(TAG_WINDINGRULE,d); }
        if(s.stroke.enabled && s.stroke.width>0 && s.stroke.color.alpha()>0){
            QByteArray d; wI32(d, colourRef(s.stroke.color)); rec(TAG_LINECOLOUR,d);
            QByteArray w; wI32(w, (qint32)qRound(s.stroke.width*MP)); rec(TAG_LINEWIDTH,w);
            // Xara LineCapType (Kernel/attr.h): 0=Butt,1=Round,2=Square; JointType: 0=Mitre,1=Round,2=Bevel.
            quint8 cap = (s.stroke.cap==Qt::RoundCap)?1 : (s.stroke.cap==Qt::SquareCap)?2 : 0;
            quint8 jn  = (s.stroke.join==Qt::RoundJoin)?1 : (s.stroke.join==Qt::BevelJoin)?2 : 0;
            { QByteArray c; wU8(c,cap); rec(TAG_STARTCAP,c); rec(TAG_ENDCAP,c); }
            { QByteArray j; wU8(j,jn);  rec(TAG_JOINSTYLE,j); }
            if(s.stroke.transp > 0.001){ QByteArray t; wU8(t,(quint8)qRound(qBound(0.0,s.stroke.transp,1.0)*255)); wU8(t,1); rec(TAG_LINETRANSPARENCY,t); }
            // vector stroke-shape: STROKETYPE with top byte 0x00 + the stroke-definition handle (emitShape wrote the def).
            if(pendStrokeHandle){ QByteArray d; wU32(d, pendStrokeHandle & 0x00ffffffu); rec(TAG_STROKETYPE,d); }
            // variable-width stroke: STROKETYPE handle 0x02000000 + an embedded ValueFunctionPressure(id=5) table
            // of {u16 pos, u16 pressure} control points (/65535). Reader recombines width×profile.
            if(s.stroke.varWidth && !s.stroke.widthProfile.isEmpty()){
                { QByteArray d; wU32(d, 0x02000000u); rec(TAG_STROKETYPE,d); }
                QByteArray d; wI32(d,5); wU32(d,(quint32)s.stroke.widthProfile.size());
                for(const QPointF& cp: s.stroke.widthProfile){
                    wU16(d,(quint16)qRound(qBound(0.0,cp.x(),1.0)*65535.0));
                    wU16(d,(quint16)qRound(qBound(0.0,cp.y(),1.0)*65535.0)); }
                rec(TAG_VARIABLEWIDTHTABLE,d);
            }
            // art/textured brush: embed the ink bitmap + reference it (seq, spacing×1000, tint flag)
            if(!s.stroke.brushInk.isNull()){ qint32 bref=bitmapRef(s.stroke.brushInk);
                QByteArray d; wI32(d,bref); wI32(d,(qint32)qRound(s.stroke.brushSpacing*1000)); wU8(d, s.stroke.brushTint?1:0);
                rec(TAG_BRUSHDEFINITION,d); }
        } else rec(TAG_LINECOLOUR_NONE);
        // transparency: rich (blend mode / graduated) from transp, else flat from opacity
        if(s.transp.type > 0){
            const Transparency& t = s.transp;
            if(t.geom==0){ QByteArray d; wU8(d,(quint8)qRound(qBound(0.0,t.t0,1.0)*255)); wU8(d,(quint8)t.type); rec(TAG_FLATTRANSPARENTFILL,d); }
            else { QPointF p0=xf.map(t.p0), p1=xf.map(t.p1);
                QByteArray d; wI32(d,toX(p0.x())); wI32(d,toY(p0.y())); wI32(d,toX(p1.x())); wI32(d,toY(p1.y()));
                wU8(d,(quint8)qRound(qBound(0.0,t.t0,1.0)*255)); wU8(d,(quint8)qRound(qBound(0.0,t.t1,1.0)*255)); wU8(d,(quint8)t.type);
                wF64(d,0.0); wF64(d,0.0);
                rec(t.geom==2?TAG_CIRCULARTRANSPARENTFILL:TAG_LINEARTRANSPARENTFILL, d); }
        } else if(s.opacity < 0.999){
            QByteArray d; wU8(d, (quint8)qRound((1.0-s.opacity)*255.0)); wU8(d, 1); rec(TAG_FLATTRANSPARENTFILL,d);
        }
    }

    void emitPath(const QPainterPath& dp, bool closed){
        QVector<quint8> vb; QVector<QPair<qint32,qint32>> cb;
        int subStart = -1;
        auto closeSub = [&](int upto){ if(closed && subStart>=0 && upto>subStart) vb[upto-1] |= PT_CLOSE; };
        const int n = dp.elementCount();
        for(int i=0;i<n;++i){ auto e=dp.elementAt(i);
            if(e.type==QPainterPath::MoveToElement){ closeSub(vb.size()); subStart=vb.size();
                vb.push_back(PT_MOVETO); cb.push_back({toX(e.x),toY(e.y)}); }
            else if(e.type==QPainterPath::LineToElement){ vb.push_back(PT_LINETO); cb.push_back({toX(e.x),toY(e.y)}); }
            else if(e.type==QPainterPath::CurveToElement && i+2<n){ auto e2=dp.elementAt(i+1),e3=dp.elementAt(i+2);
                vb.push_back(PT_BEZIERTO); cb.push_back({toX(e.x),toY(e.y)});
                vb.push_back(PT_BEZIERTO); cb.push_back({toX(e2.x),toY(e2.y)});
                vb.push_back(PT_BEZIERTO); cb.push_back({toX(e3.x),toY(e3.y)}); i+=2; }
        }
        closeSub(vb.size());
        QByteArray d; wU32(d,(quint32)vb.size());
        for(quint8 v: vb) wU8(d,v);
        for(auto& c: cb){ wI32(d,c.first); wI32(d,c.second); }
        rec(closed? TAG_PATH_FILLED_STROKED : TAG_PATH_STROKED, d);
    }

    void emitShape(const Shape& s, const QTransform& parentXf){
        if(!s.visible) return;
        pendStrokeHandle = 0;
        const QTransform full = s.transform * parentXf;   // local -> document
        if(s.type==ShapeType::Group){
            if(s.isClipView && s.clipSplit>0 && s.clipSplit<s.children.size()){
                rec(TAG_CLIPVIEWCONTROLLER); rec(TAG_DOWN);
                for(int i=0;i<s.clipSplit;++i) emitShape(s.children[i], full);   // keyhole(s)
                rec(TAG_CLIPVIEW);                                                // boundary marker (zero-length)
                for(int i=s.clipSplit;i<s.children.size();++i) emitShape(s.children[i], full);  // clipped content
                rec(TAG_UP); return;
            }
            if(s.feather>0.4){   // feathered group -> live-effect feather container (FEATHER_EFFECT 4127)
                QByteArray b; wU8(b,0); wF64(b,96.0);          // Flags + PixelsPerInch
                wUniZ(b,"FeatherEffect"); wUniZ(b,"Feather");  // PostProcessorID + DisplayName
                wI32(b,(qint32)qRound(s.feather*MP)); wF64(b,0.0); wF64(b,0.0);  // FeatherSize + bias/gain (ignored)
                rec(TAG_FEATHER_EFFECT,b); rec(TAG_DOWN);
                for(const Shape& c: s.children) emitShape(c, full);
                rec(TAG_UP); return;
            }
            rec(TAG_GROUP); rec(TAG_DOWN);
            for(const Shape& c: s.children) emitShape(c, full);
            rec(TAG_UP); return;
        }
        // vector stroke-shape: emit the STROKEDEFINITION + its shape subtree first, then reference it from the
        // path's STROKETYPE (top byte 0x00). The def paths are in their own space; the warp normalises by bbox.
        if(!s.stroke.strokeShapePaths.isEmpty()){
            quint32 handle = nextStrokeHandle++;
            QByteArray hd; wU32(hd,handle);
            quint32 fl = 0x200u | (s.stroke.strokeRepeating?0x100u:0u) | (s.stroke.strokeOverrideFill?0x400u:0u);
            wU32(hd,fl); wU32(hd,(quint32)qMax(0,s.stroke.strokeRepeats)); wU32(hd,0); wUniZ(hd,"StrokeShape");
            rec(TAG_STROKEDEFINITION,hd); rec(TAG_DOWN);
            for(int i=0;i<s.stroke.strokeShapePaths.size();++i){
                emitPath(s.stroke.strokeShapePaths[i], true);
                rec(TAG_DOWN);   // colourRef must run INSIDE the DOWN so its DEFINERGBCOLOUR doesn't split PATH from DOWN
                { QByteArray fd; wI32(fd, colourRef(s.stroke.strokeShapeCols.value(i,QColor(Qt::black)))); rec(TAG_FLATFILL,fd); }
                rec(TAG_UP);
            }
            rec(TAG_UP);
            pendStrokeHandle = handle;
        }
        bool closed = (s.type!=ShapeType::Path) || s.fill.type!=FillType::None || s.type==ShapeType::Text;
        emitPath(full.map(s.localPath()), closed);
        rec(TAG_DOWN); emitFill(s, full); rec(TAG_UP);
    }
};

} // anonymous namespace

// Raw DEFLATE (windowBits=-15) — matches the reader's primary zlibInflate path exactly (accurate consumed).
static QByteArray rawDeflate(const QByteArray& src){
    if(src.isEmpty()) return {};
    z_stream zs; memset(&zs,0,sizeof(zs));
    if(deflateInit2(&zs,Z_BEST_COMPRESSION,Z_DEFLATED,-15,8,Z_DEFAULT_STRATEGY)!=Z_OK) return {};
    zs.next_in=(Bytef*)src.constData(); zs.avail_in=(uInt)src.size();
    QByteArray out; char buf[1<<16]; int ret;
    do{ zs.next_out=(Bytef*)buf; zs.avail_out=sizeof(buf); ret=deflate(&zs,Z_FINISH);
        out.append(buf,int(sizeof(buf)-zs.avail_out)); } while(ret==Z_OK);
    deflateEnd(&zs); return (ret==Z_STREAM_END)? out : QByteArray();
}
bool saveXar(const Document& doc, const QString& path, bool compress){
    Writer w; w.pageH = doc.pageSize.height();
    wU32(w.out, 0x41524158u); wU32(w.out, 0x0a0da3a3u);             // "XARA" + £ £ CR LF
    { QByteArray d; d.append("CXN",3); wU32(d,0); wU32(d,0); wU32(d,0);
      wAsciiZ(d,"XarQ"); wAsciiZ(d,"1.0"); wAsciiZ(d,"1"); w.rec(TAG_FILEHEADER,d); }
    const int headLen = w.out.size();       // header + FILEHEADER stay uncompressed (Xara requires this)
    w.rec(TAG_DOWN);
      w.rec(TAG_DOCUMENT); w.rec(TAG_DOWN);
        w.rec(TAG_CHAPTER); w.rec(TAG_DOWN);
          w.rec(TAG_SPREAD); w.rec(TAG_DOWN);
            { QByteArray d; wI32(d,(qint32)qRound(doc.pageSize.width()*MP)); wI32(d,(qint32)qRound(doc.pageSize.height()*MP));
              wI32(d,0); wI32(d,0); wU8(d, 0x02|0x04); w.rec(TAG_SPREADINFORMATION,d); }   // ShowDropShadow|Selected
            { QByteArray d; wU8(d, 0x1|0x4|0x8); wUniZ(d,"Layer 1"); w.rec(TAG_LAYERDETAILS,d); } // Visible|Printable|Active
            w.rec(TAG_LAYER); w.rec(TAG_DOWN);
              for(const Shape& s: doc.shapes) w.emitShape(s, QTransform());
            w.rec(TAG_UP);
          w.rec(TAG_UP);
        w.rec(TAG_UP);
      w.rec(TAG_UP);
    w.rec(TAG_UP);
    // Private lossless sidecar: the full document as compressed JSON, so XarQ round-trips
    // everything the standard XAR records above can't carry. Skipped by other readers.
    { const QByteArray json = QJsonDocument(doc.toJson()).toJson(QJsonDocument::Compact);
      w.rec(TAG_XSTUDIO_DOC, qCompress(json, 9)); }
    if(compress){
        // Deflate the record body (everything after FILEHEADER) into a Xara STARTCOMPRESSION stream.
        QByteArray body = w.out.mid(headLen);
        wU32(body, TAG_ENDCOMPRESSION); wU32(body, 0);          // in-stream ENDCOMPRESSION marker (reader stops here)
        const QByteArray def = rawDeflate(body);
        if(!def.isEmpty()){
            QByteArray out = w.out.left(headLen);
            wU32(out, TAG_STARTCOMPRESSION); wU32(out, 4); wU32(out, (quint32)body.size());   // marker + 4-byte data
            out += def;
            wU32(out, 0); wU32(out, 0);                          // 8 out-of-stream bytes (CRC/len; reader skips them)
            wU32(out, TAG_ENDOFFILE); wU32(out, 0);              // trailing uncompressed EOF
            QFile f(path); if(!f.open(QIODevice::WriteOnly)) return false;
            return f.write(out)==out.size();
        }
        // else fall through to uncompressed on a deflate failure
    }
    w.rec(TAG_ENDOFFILE);
    QFile f(path); if(!f.open(QIODevice::WriteOnly)) return false;
    return f.write(w.out)==w.out.size();
}

// =====================================================================  READER
namespace {

struct Rec { quint32 tag; QByteArray data; };
struct Ctx { Fill fill; bool fillSet=false; Stroke stroke; double opacity=1.0; Transparency transp; Qt::FillRule fillRule=Qt::OddEvenFill; double feather=0.0; double featherBias=0.0, featherGain=0.0; };
struct Frame { Ctx ctx; QVector<Shape> objs; int ownerIndex=-1; bool ownerIsGroup=false;
    int blendSteps=-1;                               // >=0 => this frame's objects are a blend's children
    QVector<QPair<int,int>> blendPairs;              // per-BLENDER ink-object index pairs (chained multi-object blends)
    double blendObjBias=0, blendObjGain=0, blendAttrBias=0, blendAttrGain=0;   // blend profiles (bias/gain, -1..+1)
    bool isText=false;                               // text-story frame (lines/strings) handled specially
    bool skip=false;                                 // invisible / guide layer -> discard its objects at close
    quint32 pathTag=0;                               // path-attribute frame: re-apply fill/stroke flags at close
    bool mouldPending=false; QRectF mldSrc; QVector<QPointF> mldCorners, mldCtrls;   // deferred mould warp (cage read before its content)
    bool inMould=false;                              // inside a mould container -> keep text in the tree (so it warps) rather than hoisting it
    bool brushDef=false; double brushSpacingMP=0, brushScaling=1.0; // brush spine's attr frame: capture nib group + spacing/scaling, stamp at close
    int clipSplit=-1;                                // clip-view controller frame: index in objs where the clipped content begins (after the TAG_CLIPVIEW marker)
    int savedJustify=0;                              // text justification is a tree-scoped attribute: restore it when this group closes
    bool isStrokeDef=false; quint32 sdHandle=0,sdFlags=0,sdData1=0; }; // stroke-definition frame: capture the vector shape subtree (not page content)

QColor lookupCol(const QHash<qint32,QColor>& cols, qint32 ref){
    auto it = cols.find(ref); if(it!=cols.end()) return it.value();
    return (ref==-1)? QColor(Qt::white) : QColor(Qt::black);   // default-colour fallback
}
bool isObjectTag(quint32 t){
    return (t>=TAG_PATH && t<=TAG_GROUP) || (t>=TAG_PATH_RELATIVE && t<=TAG_PATH_RELATIVE_FILLED_STROKED) || t==TAG_PATHREF_TRANSFORM
        || (t>=TAG_ELLIPSE_SIMPLE && t<=TAG_ELLIPSE_COMPLEX)
        || (t>=TAG_RECTANGLE_SIMPLE && t<=TAG_RECTANGLE_COMPLEX) || t==TAG_POLYGON_COMPLEX
        || t==TAG_REGULAR_SHAPE_PHASE_2
        || t==TAG_NODE_BITMAP || t==TAG_NODE_CONTONEDBITMAP
        || t==TAG_BLEND || t==TAG_SHADOWCONTROLLER
        || t==TAG_CONTOURCONTROLLER || t==TAG_BEVEL || t==TAG_CLIPVIEWCONTROLLER || t==TAG_FEATHER_EFFECT
        || t==TAG_MOULD_ENVELOPE || t==TAG_MOULD_PERSPECTIVE || t==TAG_MOULD_GROUP;   // container nodes (group-like)
}

// Inflate a deflate stream. Xara compresses its record stream with RAW deflate (no zlib
// header), so we try windowBits=-15 first, then fall back to a zlib/gzip-wrapped stream.
QByteArray inflateWith(const char* src, int srcLen, int windowBits, int& consumed){
    QByteArray out; consumed=0; z_stream s; memset(&s,0,sizeof(s));
    if(inflateInit2(&s, windowBits)!=Z_OK) return out;
    s.next_in=(Bytef*)src; s.avail_in=srcLen; char buf[1<<16]; int ret;
    do { s.next_out=(Bytef*)buf; s.avail_out=sizeof(buf); ret=inflate(&s,Z_NO_FLUSH);
        if(ret==Z_STREAM_ERROR||ret==Z_DATA_ERROR||ret==Z_MEM_ERROR||ret==Z_NEED_DICT) break;
        out.append(buf, sizeof(buf)-s.avail_out);
    } while(ret!=Z_STREAM_END && s.avail_in>0);
    consumed = (int)s.total_in;
    inflateEnd(&s); return out;
}
QByteArray zlibInflate(const char* src, int srcLen, int& consumed){
    QByteArray out = inflateWith(src, srcLen, -15, consumed);          // raw deflate (Xara)
    if(out.isEmpty()) out = inflateWith(src, srcLen, 47, consumed);    // zlib/gzip auto-detect
    return out;
}

// parse a flat record stream out of `buf` starting at `o`, appending to `recs`,
// stopping at ENDCOMPRESSION/ENDOFFILE. Returns when the (sub)stream is exhausted.
void parseRecs(const QByteArray& buf, int o, QVector<Rec>& recs, bool decompressed){
    while(o+8 <= buf.size()){
        quint32 tag=rdU32(buf,o); quint32 size=rdU32(buf,o+4); o+=8;
        if(decompressed && tag==TAG_ENDCOMPRESSION){ recs.push_back({tag,{}}); return; } // data is outside the stream
        if(o+(int)size > buf.size()){ if(tag==TAG_ENDOFFILE) recs.push_back({tag,{}}); return; }
        if(!decompressed && tag==TAG_STARTCOMPRESSION){
            recs.push_back({tag, buf.mid(o,size)}); o+=size;            // marker + its 4-byte data
            int consumed=0;
            QByteArray inflated = zlibInflate(buf.constData()+o, buf.size()-o, consumed);
            parseRecs(inflated, 0, recs, true);                        // decompressed records (+ ENDCOMPRESSION marker)
            o += consumed + 8;                                         // skip the zlib stream + ENDCOMPRESSION's out-of-stream CRC/len
            continue;                                                  // resume with the trailing uncompressed records (closing UPs, EOF)
        }
        recs.push_back({tag, buf.mid(o,size)}); o+=size;
        if(tag==TAG_ENDOFFILE) return;
    }
}

void applyCtx(Shape& s, const Ctx& c){
    const QTransform inv = s.transform.isIdentity() ? QTransform() : s.transform.inverted();
    // A bitmap *node* sets its own intrinsic fill AFTER the first applyCtx; the attribute
    // write-back (closeTop) must not clobber it. Re-assigning from ctx only when the current
    // fill isn't already a bitmap also keeps the doc->local conversion below from double-applying.
    if(s.fill.type != FillType::Bitmap){
        s.fill = c.fill;
        if(!c.fillSet) s.fill.type = FillType::None;   // untouched document-default fill -> No Colour (Xara default)
        if(!s.transform.isIdentity()){
            if(s.fill.type==FillType::Linear||s.fill.type==FillType::Radial||s.fill.type==FillType::Conical){
                s.fill.p0 = inv.map(s.fill.p0); s.fill.p1 = inv.map(s.fill.p1); }     // doc -> local
            else if(s.fill.type==FillType::Bitmap||s.fill.type==FillType::Mesh||s.fill.type==FillType::Square){
                s.fill.p0 = inv.map(s.fill.p0); s.fill.p1 = inv.map(s.fill.p1); s.fill.p2 = inv.map(s.fill.p2); }  // parallelogram doc -> local
        }
    }
    s.stroke = c.stroke; s.opacity = c.opacity; s.transp = c.transp; s.fillRule = c.fillRule; s.feather = c.feather;
    s.featherBias = c.featherBias; s.featherGain = c.featherGain;
    // Flat *mix* transparency is just object opacity — fold it in (keeps the UI tool's
    // opacity slider + simple round-trip working); leave only rich transparency in transp.
    if(s.transp.type==1 && s.transp.geom==0){ s.opacity *= qBound(0.0,1.0-s.transp.t0,1.0); s.transp = Transparency(); }
    if(!s.transform.isIdentity() && s.transp.isGraduated()){
        s.transp.p0 = inv.map(s.transp.p0); s.transp.p1 = inv.map(s.transp.p1);
        s.transp.p2 = inv.map(s.transp.p2); }   // elliptical/square 2nd axis -> local too (else the ellipse brush goes near-singular)
}

// The path tag encodes fill/stroke state (relative to the base 100/113): bit0=filled, bit1=stroked.
// A _STROKED-only path (bits==2, e.g. 102/115) is an outline and must NOT be filled — otherwise a
// stroke-only closed path gets filled with the inherited colour (the Spitfire/GardenPlan black blob).
// We deliberately leave the base PATH and FILLED variants' fills alone (some artwork relies on them).
void applyPathFlags(Shape& s, quint32 tag){
    quint32 base = (tag>=TAG_PATH_RELATIVE)? TAG_PATH_RELATIVE : TAG_PATH;
    if((tag-base)==2) s.fill.type = FillType::None;   // STROKED only -> drop the fill
}

QPainterPath buildPath(const QVector<quint8>& v, const QVector<QPointF>& p){
    QPainterPath path; const int n=v.size();
    for(int k=0;k<n;){
        quint8 base = v[k] & ~PT_CLOSE;
        if(base==PT_MOVETO){ path.moveTo(p[k]); ++k; }
        else if(base==PT_LINETO){ path.lineTo(p[k]); if(v[k]&PT_CLOSE) path.closeSubpath(); ++k; }
        else if(base==PT_BEZIERTO && k+2<n){ path.cubicTo(p[k],p[k+1],p[k+2]); if(v[k+2]&PT_CLOSE) path.closeSubpath(); k+=3; }
        else ++k;
    }
    return path;
}

// Absolute path: NumCoords(u32) + verbs[N] + coords[N] (x,y INT32 each).
Shape makePathShape(const QByteArray& d, const std::function<double(qint32)>& fx,
                    const std::function<double(qint32)>& fy){
    Shape s; s.type=ShapeType::Path; s.name="Path";
    quint32 n = rdU32(d,0); int o=4;
    if(n==0 || d.size() < 4+(int)n+(int)n*8) return s;
    QVector<quint8> v(n); for(quint32 k=0;k<n;++k) v[k]=(quint8)d[o++];
    QVector<QPointF> p(n); for(quint32 k=0;k<n;++k){ p[k]=QPointF(fx(rdI32(d,o)),fy(rdI32(d,o+4))); o+=8; }
    s.path = buildPath(v,p); return s;
}

// Refined (relative) path: interleaved (verb BYTE + COORD) entries, 9 bytes each.
// Coords after the first are stored relative (curr = prev - delta), MSB-first with the
// X and Y bytes interleaved (b0,b2,b4,b6 -> X; b1,b3,b5,b7 -> Y).
Shape makeRelPathShape(const QByteArray& d, const std::function<double(qint32)>& fx,
                       const std::function<double(qint32)>& fy){
    Shape s; s.type=ShapeType::Path; s.name="Path";
    const int entries = d.size()/9;
    if(entries<=0) return s;
    QVector<quint8> v(entries); QVector<QPointF> p(entries);
    const uchar* base=(const uchar*)d.constData();
    qint32 px=0, py=0; int o=0;
    for(int k=0;k<entries;++k){
        v[k]=base[o++]; const uchar* b=base+o; o+=8;
        qint32 cx=(qint32)(((quint32)b[0]<<24)|((quint32)b[2]<<16)|((quint32)b[4]<<8)|b[6]);
        qint32 cy=(qint32)(((quint32)b[1]<<24)|((quint32)b[3]<<16)|((quint32)b[5]<<8)|b[7]);
        qint32 ax,ay; if(k==0){ ax=cx; ay=cy; } else { ax=px-cx; ay=py-cy; }
        px=ax; py=ay; p[k]=QPointF(fx(ax),fy(ay));
    }
    s.path = buildPath(v,p);
    return s;
}

// generate a closed Path shape from a quickshape centre + axis vectors (doc coords)
Shape axisShape(QPointF c, QPointF major, QPointF minor, int sides, bool ellipse){
    Shape s; s.type=ShapeType::Path; s.name = ellipse?"Ellipse":"Polygon";
    QPainterPath p;
    if(ellipse){
        // 4-bezier ellipse from the two radius vectors
        const double k=0.5522847498;
        QPointF a=major, b=minor;
        QPointF p0=c+a, p1=c+b, p2=c-a, p3=c-b;
        p.moveTo(p0);
        p.cubicTo(p0+k*b, p1+k*a, p1); p.cubicTo(p1-k*a, p2+k*b, p2);
        p.cubicTo(p2-k*b, p3-k*a, p3); p.cubicTo(p3-k*a+2*k*a, p0-k*b, p0);
    } else {
        int nn=qMax(3,sides);
        for(int i=0;i<nn;++i){ double a=-M_PI/2 + i*2*M_PI/nn;
            QPointF pt=c+std::cos(a)*major+std::sin(a)*minor; if(i==0)p.moveTo(pt); else p.lineTo(pt); }
        p.closeSubpath();
    }
    s.path=p; return s;
}

// General regular shape (TAG_REGULAR_SHAPE_PHASE_2): Flags(1) NumSides(u16) MajorAxis(8)
// MinorAxis(8) Matrix(24: 4×FIXED16/65536 + 2×INT32 mp) StellationRadius/Offset/PrimCurv/
// SecCurv(8 each) + 2 edge paths. We generate the N-gon / star outline (curvature ignored)
// in untransformed space, then apply the matrix → doc space. (pageH for the y-up→y-down flip.)
Shape makeRegularShape(const QByteArray& d, double pageH){
    Shape s; s.type=ShapeType::Path; s.name="Shape";
    if(d.size() < 51) return s;
    int flags=(quint8)d[0];
    int n=rdU16(d,1); if(n<3) n=3; if(n>512) n=512;
    QPointF major(rdI32(d,3), rdI32(d,7)), minor(rdI32(d,11), rdI32(d,15));   // untransformed mp
    double a=rdI32(d,19)/65536.0, b=rdI32(d,23)/65536.0, c=rdI32(d,27)/65536.0, dd=rdI32(d,31)/65536.0;
    double e=rdI32(d,35), f=rdI32(d,39);
    double stellR=rdF64(d,43); if(stellR<=0.0 || stellR>1.0) stellR=0.45;
    bool star=(flags & 0x2);
    // NOTE: flag 0x1 = Circular (Kernel IsCircular()->BuildEllipsePath). Rendering those as a true ellipse is
    // SEMANTICALLY correct but was REVERTED: it regressed Brush Test's oracle (0.269->0.279, the circular brush
    // nibs under-cover the oracle's large semi-transparent scatter circles) and improved no measured file — its
    // benefit is masked by the unimplemented scatter-brush variation engine. Left as N-gon until that lands.
    auto toDoc=[&](QPointF u){ double mx=u.x()*a+u.y()*c+e, my=u.x()*b+u.y()*dd+f; return QPointF(mx/MP, pageH-my/MP); };
    // Vertices start at angle PI/n (Xara's NodeRegularShape::BuildPolygonPoints), so an n=4 shape is a
    // rectangle (corners up) rather than a diamond (vertices on the axes); polygons/stars are unchanged
    // in size, just rotated to Xara's orientation.
    QPainterPath p; int verts = star ? 2*n : n;
    for(int i=0;i<verts;++i){ double ang = M_PI/n + (star ? (i*M_PI/n) : (i*2.0*M_PI/n));
        double rf = (star && (i&1)) ? stellR : 1.0;
        QPointF u = rf*std::cos(ang)*major + rf*std::sin(ang)*minor;
        QPointF v = toDoc(u); if(i==0) p.moveTo(v); else p.lineTo(v); }
    p.closeSubpath(); s.path=p;
    return s;
}

// Some Xara logo "overlay" bitmaps are an OPAQUE near-black plate with a sparse bright highlight (e.g.
// scope3's "Xtreme" swoosh) that Xara composites with the black dropped out — yet the file carries no
// transparency/blend that scopes to the placed bitmap. We detect that content signature and luminance-KEY
// the image: alpha := luminance, so the black plate -> transparent and the highlight keeps its colour at
// ~its own brightness (matches the reference swoosh-on-sky). The gate is deliberately tight (must be
// dominantly opaque + near-black AND have a bright highlight AND own an alpha channel) so ordinary photos,
// cutouts and genuine black artwork are left untouched. Returns true if it keyed the image.
static bool luminanceKeyDarkOverlay(QImage& im){
    if(im.isNull() || !im.hasAlphaChannel()) return false;
    QImage a = im.convertToFormat(QImage::Format_ARGB32);
    const int W=a.width(), H=a.height(); if((qint64)W*H < 1) return false;
    qint64 total=0, opaqueDark=0; int maxLum=0;
    for(int y=0;y<H;++y){ const QRgb* r=(const QRgb*)a.constScanLine(y);
        for(int x=0;x<W;++x){ QRgb p=r[x]; ++total;
            int lum=(299*qRed(p)+587*qGreen(p)+114*qBlue(p))/1000;
            if(qAlpha(p)>250 && lum<24) ++opaqueDark;
            if(qAlpha(p)>16 && lum>maxLum) maxLum=lum; } }
    if(total==0 || opaqueDark*100 < total*40 || maxLum < 100) return false;   // not a dark-plate-with-highlight overlay
    for(int y=0;y<H;++y){ QRgb* r=(QRgb*)a.scanLine(y);
        for(int x=0;x<W;++x){ QRgb p=r[x]; int lum=(299*qRed(p)+587*qGreen(p)+114*qBlue(p))/1000;
            r[x]=qRgba(qRed(p),qGreen(p),qBlue(p), lum); } }   // alpha := luminance (black -> transparent)
    im=a; return true;
}

// ============================================================================
// Exact port of Xara's PlasmaFractalFill (Kernel/fracfill.cpp). This is the
// SAME algorithm the reference renderer (CDraw pipeline) used — a recursive
// midpoint-displacement ("plasma") fractal driven by a deterministic integer
// PRNG (no rand()/noise table), so the output reproduces Xara's fractal bitmap
// rather than approximating it. NOTE: this is distinct from the B-spline value
// noise in noise1.cpp, which Xara uses for the separate NOISEFILL attribute.
// ============================================================================
namespace plasma {
    static const qint32 MAXFC  = (1<<28);     // MAX_FRACTAL_COORD: virtual coord space is 0..2^28
    static const qint32 MAXPOT = (1<<16)-1;   // MaxPotential

    struct Gen {
        quint32 s1, s2;                       // 33-bit Wilson PRNG state
        qint32  Seed;
        bool    Tileable, ForceZero, Squashing;
        quint32 CoordMask;
        qint32  xGrain, yGrain, xGrav, yGrav;
        int     Eigen, W, H, Rec;
        quint8* buf;

        static inline quint32 rotl(quint32 v, int n){ n&=31; return n ? ((v<<n)|(v>>(32-n))) : v; }
        // DoRand macro: 33-bit rotate-with-feedback (ARM "Wilson" routine), exact port.
        static inline void rnd(quint32& a, quint32& b){ quint32 t=(a>>1)^(b<<31); b=a&1u; t^=a<<12; a=t^(t>>20); }
        inline qint32 next(){ rnd(s1,s2); return (qint32)s1; }

        // Adjust(): linterp of the two endpoint potentials + a coordinate-hashed random
        // perturbation (scaled by graininess >> recursion) - gravity.  Exact port.
        qint32 adjust(qint32 pa, qint32 pb, qint32 x, qint32 y, qint32 gr, qint32 gv){
            quint32 pot = (quint32)(pa ^ (pb<<16) ^ (qint32)rotl((quint32)x,16) ^ y ^ Seed ^ 0xABCD1234);
            quint32 ts = 1; rnd(pot, ts);
            int r = (x^(x>>5)^(x>>10)^(x>>15)^(x>>20)^(x>>25)^(x>>30)
                    ^ y^(y>>5)^(y>>10)^(y>>15)^(y>>20)^(y>>25)^(y>>30)) & 31;
            pot = rotl(pot, r); rnd(pot, ts);
            qint32 p = (qint32)pot;
            if (Squashing && !(x||y)) p = 0;
            // NB: (p>>17) is a SIGNED arithmetic shift -> a bidirectional perturbation in [-16384,+16383]
            // (matches the source's (INT32)(potential>>17)); an unsigned shift would only ever push up.
            p = ((gr * (p >> 17)) >> Rec) - (gv >> (Rec*2)) + ((pa + pb + 1) >> 1);
            if (p > MAXPOT) p = MAXPOT;
            if (p < 0) p = 0;
            return p;
        }

        // SubDivide(): recursively split the square, computing 4 edge midpoints + centre,
        // plotting the bottom-left corner of each leaf cell.  Greyscale = ~(potential>>8).
        void sub(qint32 x1, qint32 y1, qint32 x2, qint32 y2,
                 qint32 p11, qint32 p12, qint32 p22, qint32 p21, bool plot){
            qint32 x1e=(x1>>Eigen), x2e=(x2>>Eigen), y1e=(y1>>Eigen), y2e=(y2>>Eigen);
            if (x2e<0 || x1e>W || y2e<0 || y1e>H) return;
            if (plot && (quint32)x1e<(quint32)W && (quint32)y1e<(quint32)H)
                buf[y1e*W + x1e] = (quint8)(~(p11>>8));
            if ((x2e-x1e)<2 && (y2e-y1e)<2) return;
            Rec++;
            qint32 xh=(x1+x2)>>1, yh=(y1+y2)>>1;
            qint32 ph1=adjust(p11,p21, xh&CoordMask, y1&CoordMask, xGrain,xGrav);
            qint32 p2h=adjust(p21,p22, x2&CoordMask, yh&CoordMask, yGrain,yGrav);
            qint32 ph2=adjust(p12,p22, xh&CoordMask, y2&CoordMask, xGrain,xGrav);
            qint32 p1h=adjust(p11,p12, x1&CoordMask, yh&CoordMask, yGrain,yGrav);
            qint32 phh=(ph1+p2h+ph2+p1h+2)>>2;
            if (Rec==1){ if (ForceZero) phh=MAXPOT; else if (Tileable) phh=MAXPOT-p11; }
            sub(x1,y1,xh,yh, p11,p1h,phh,ph1, false);
            sub(xh,y1,x2,yh, ph1,phh,p2h,p21, true);
            sub(xh,yh,x2,y2, phh,ph2,p22,p2h, true);
            sub(x1,yh,xh,y2, p1h,p12,ph2,phh, true);
            Rec--;
        }
    };

    // Generate a Dim x Dim 8-bit greyscale plasma. Mirrors the PlasmaFractalFill
    // constructor + DoFill(bitmap, Dim, 0, 0). grain/gravity/squash are the FIXED16
    // field values as doubles.
    static QImage generate(int Dim, qint32 seed, bool tileable, double squash, double grain, double gravity){
        Gen f;
        f.Seed=seed; f.Tileable=tileable; f.ForceZero=(gravity!=0.0); f.Squashing=(squash!=0.0);
        f.CoordMask = tileable ? (quint32)(MAXFC-1) : 0xffffffffu;
        f.xGrav = f.yGrav = (qint32)(quint32)(gravity*MAXPOT);
        f.xGrain = f.yGrain = (qint32)(quint32)(grain*8 + 0.5);
        if (squash>0){ f.xGrav=(qint32)(quint32)(gravity*MAXPOT/squash);    f.xGrain=(qint32)(quint32)((grain*8+0.5)/squash); }
        else if (squash<0){ f.yGrav=(qint32)(quint32)(gravity*MAXPOT/(-squash)); f.yGrain=(qint32)(quint32)((grain*8+0.5)/(-squash)); }
        if (f.xGrain<0) f.xGrain=0; if (f.xGrain>0xffff) f.xGrain=0xffff;
        if (f.yGrain<0) f.yGrain=0; if (f.yGrain>0xffff) f.yGrain=0xffff;
        f.W=Dim; f.H=Dim;
        f.Eigen=0; while (((qint64)Dim << f.Eigen) < MAXFC) f.Eigen++;
        f.s1=(quint32)seed; f.s2=1;                       // SetSeed(Seed)
        qint32 c[4];
        for (int i=0;i<4;i++) c[i]=(qint32)((((quint32)f.next()>>16)*(quint32)MAXPOT+(1<<15))>>16);
        if (f.Tileable && !f.ForceZero){ c[0]=0; }
        else {                                            // boost the lower-contrast diagonal
            qint32 dd1=qAbs(c[0]-c[2]), dd2=qAbs(c[1]-c[3]); int a,b;
            if (dd1<dd2){ a=(c[0]<c[2])?0:2; b=2-a; } else { a=(c[1]<c[3])?1:3; b=4-a; }
            dd2=(MAXPOT-dd1)-dd2;
            if (dd2>0){ c[a]-=dd2/2; c[b]+=dd2/2; }
        }
        QImage img(Dim, Dim, QImage::Format_Grayscale8); img.fill(0);
        std::vector<quint8> flat((size_t)Dim*Dim, 0); f.buf=flat.data();
        f.Rec=0;
        qint32 p11=f.ForceZero?0:c[0];
        qint32 p12=f.ForceZero?0:(f.Tileable?c[0]:c[1]);
        qint32 p22=f.ForceZero?0:(f.Tileable?c[0]:c[2]);
        qint32 p21=f.ForceZero?0:(f.Tileable?c[0]:c[3]);
        f.sub(0,0, MAXFC, MAXFC, p11,p12,p22,p21, true);
        for (int y=0;y<Dim;++y) memcpy(img.scanLine(y), flat.data()+(size_t)y*Dim, Dim);
        return img;
    }
} // namespace plasma

// Perlin/Schlick bias & gain (Kernel/biasgain.h, CProfileBiasGain). The record's
// Bias/Gain are in [-1,+1] (0 = flat/identity); convert to (0,1) and apply.
static inline double schlickBias(double b, double t){ return t/((1.0/b - 2.0)*(1.0-t) + 1.0); }
static double applyBiasGain(double t, double biasP, double gainP){
    if (biasP==0.0 && gainP==0.0) return t;                // exact identity (Xara's linear palette path)
    double b=qBound(0.001,(biasP+1.0)*0.49999+0.00001,0.999), g=qBound(0.001,(gainP+1.0)*0.49999+0.00001,0.999);
    t = schlickBias(b, t);                                 // bias first
    t = (t<0.5) ? schlickBias(1.0-g, 2.0*t)/2.0            // then gain (Schlick)
                : 1.0 - schlickBias(1.0-g, 2.0-2.0*t)/2.0;
    return qBound(0.0, t, 1.0);
}

// Build the coloured fractal tile: generate the plasma greyscale, then contone it
// between the fill's start/end colours (greyscale 0 -> c0, 255 -> c1), shaped by bias/gain.
static QImage makeFractalImage(const QColor& c0, const QColor& c1, int dim, qint32 seed,
                               bool tileable, double squash, double grain, double gravity,
                               double bias, double gain){
    if (dim<4) dim=4;
    QImage grey = plasma::generate(dim, seed, tileable, squash, grain, gravity);
    QRgb ramp[256];
    for (int i=0;i<256;i++){
        double t = applyBiasGain(i/255.0, bias, gain);
        int r=int(c0.red()  +(c1.red()  -c0.red())  *t + 0.5);
        int g=int(c0.green()+(c1.green()-c0.green())*t + 0.5);
        int b=int(c0.blue() +(c1.blue() -c0.blue()) *t + 0.5);
        ramp[i]=qRgb(qBound(0,r,255),qBound(0,g,255),qBound(0,b,255));
    }
    QImage img(dim, dim, QImage::Format_ARGB32);
    for (int y=0;y<dim;++y){ const quint8* gr=grey.constScanLine(y); QRgb* row=(QRgb*)img.scanLine(y);
        for (int x=0;x<dim;++x) row[x]=ramp[gr[x]]; }
    return img;
}

// ---- Xara's B-spline value noise (Kernel/noise1.cpp NoiseGen1) for the NOISEFILL attribute ----
// A DIFFERENT generator from the plasma above: a 2D quadratic-B-spline interpolation of a table
// of random impulses. The impulse table is seeded with the C library rand() (matching the
// reference Linux/glibc build). NOISEFILL is absent from the test corpus, so byte-exactness is
// moot, but this is the genuine generator rather than a gradient stand-in.
namespace bnoise {
    static const int TABLEWIDTH=50, MAXPTS=TABLEWIDTH*TABLEWIDTH;
    static void seedTable(quint32 seed, float* R){ std::srand(seed); for(int i=0;i<MAXPTS;i++) R[i]=(float)std::rand()/(float)RAND_MAX; }
    // VNoise1D(p, scale): exact port. p.x,p.y in [0,1); returns a B-spline blend in [0,1].
    static double vnoise(const float* R, double px, double py, double scale){
        int samplerange=(int)scale; if(samplerange<1) return 0.0;
        float xf=(float)(px*samplerange), yf=(float)(py*samplerange);
        int ax=(int)std::floor(xf), ay=(int)std::floor(yf);
        xf-=ax; yf-=ay;
        float x1=xf*xf, y1=yf*yf, x2=0.5f*x1, y2=0.5f*y1;
        x1=0.5f+xf-x1; y1=0.5f+yf-y1; float x0=0.5f-xf+x2, y0=0.5f-yf+y2;
        int bx=ax+1,by=ay+1,cx=bx+1,cy=by+1;
        ax%=samplerange; ay%=samplerange; bx%=samplerange; by%=samplerange; cx%=samplerange; cy%=samplerange;
        if(samplerange==1) cx=cy=1;
        if(TABLEWIDTH<samplerange){ ax%=TABLEWIDTH;ay%=TABLEWIDTH;bx%=TABLEWIDTH;by%=TABLEWIDTH;cx%=TABLEWIDTH;cy%=TABLEWIDTH; }
        ay*=TABLEWIDTH; by*=TABLEWIDTH; cy*=TABLEWIDTH;
        float p00=R[ax+ay],p01=R[bx+ay],p02=R[cx+ay];
        float r=y0*(x0*p00+x1*p01+x2*p02);
        p00=R[ax+by];p01=R[bx+by];p02=R[cx+by]; r+=y1*(x0*p00+x1*p01+x2*p02);
        p00=R[ax+cy];p01=R[bx+cy];p02=R[cx+cy]; r+=y2*(x0*p00+x1*p01+x2*p02);
        return (double)r;
    }
}
static QImage makeNoiseImage(const QColor& c0, const QColor& c1, qint32 seed, bool /*tileable*/,
                             double grain, double bias, double gain){
    const int S=256;                                       // NoiseFractalFill bitmap is fixed 256x256
    std::vector<float> R(bnoise::MAXPTS); bnoise::seedTable((quint32)seed, R.data());
    double scale = grain>=1.0 ? grain : 1.0;
    QRgb ramp[256];
    for (int i=0;i<256;i++){ double t=applyBiasGain(i/255.0, bias, gain);
        int r=int(c0.red()+(c1.red()-c0.red())*t+0.5), g=int(c0.green()+(c1.green()-c0.green())*t+0.5), b=int(c0.blue()+(c1.blue()-c0.blue())*t+0.5);
        ramp[i]=qRgb(qBound(0,r,255),qBound(0,g,255),qBound(0,b,255)); }
    QImage img(S,S,QImage::Format_ARGB32);
    for (int j=0;j<S;j++){ QRgb* row=(QRgb*)img.scanLine(j);
        for (int i=0;i<S;i++){ double n=bnoise::vnoise(R.data(),(i+0.5)/S,(j+0.5)/S,scale);
            row[i]=ramp[qBound(0,(int)(n*255.0+0.5),255)]; } }
    return img;
}

// ---- envelope mould: warp content through a 4-sided cubic-bezier cage (Coons patch) ----
// Envelope + coonsWarp + warpThroughEnvelope live in pathedit.{h,cpp} — one source of truth shared
// with the interactive Mould tool. (Verified byte-identical across the full corpus, incl. Watch4.)
// Warp a mould's "MouldContent" group through its cage. Curved bezier edges -> bake the content
// geometry through the Coons patch; a straight quad -> a single projective (perspective) transform.
static void applyMould(QVector<Shape>& objs, const QVector<QPointF>& corners, const QVector<QPointF>& ctrls, const QRectF& src, double lim){
    int mc=-1; for(int j=objs.size()-1;j>=0;--j) if(objs[j].name=="MouldContent"){ mc=j; break; }
    if(mc<0 || src.width()<1 || src.height()<1) return;
    // A degenerate cage can make the warp/transform map content far off-page or to ~infinity
    // (near-singular projective quadToQuad), poisoning the whole document's bounds. Reject any result
    // whose extent/position is implausible relative to the page (lim ~ a few page dimensions).
    auto sane=[lim](const QRectF& r){ return qIsFinite(r.x())&&qIsFinite(r.y())&&qIsFinite(r.width())&&qIsFinite(r.height())
        && qAbs(r.x())<2*lim && qAbs(r.y())<2*lim && r.width()<lim && r.height()<lim; };
    bool curved=false; Envelope env;
    if(corners.size()>=4 && ctrls.size()>=8){
        for(int i=0;i<4;++i){ env.cor[i]=corners[i]; env.ec1[i]=ctrls[2*i]; env.ec2[i]=ctrls[2*i+1]; }
        for(int i=0;i<4 && !curved;++i){ QLineF chord(env.cor[i],env.cor[(i+1)&3]); double L=chord.length(); if(L<1) continue;
            auto off=[&](const QPointF& c){ return qAbs((c.x()-chord.x1())*(chord.y2()-chord.y1())-(c.y()-chord.y1())*(chord.x2()-chord.x1()))/L; };
            if(off(env.ec1[i])>1.5||off(env.ec2[i])>1.5) curved=true; }
    }
    if(curved){
        // Collect leaf doc-outlines first; warp them mapping the content's actual bounds onto the cage
        // (the stored MOULD_BOUNDS is in a different space than where the content was laid out).
        QVector<QPair<QPainterPath,Shape>> leaves; QRectF bb;
        std::function<void(const Shape&,const QTransform&)> collect=[&](const Shape& s,const QTransform& acc){
            QTransform docXf=s.transform*acc;
            if(s.type==ShapeType::Group){ for(const Shape& c: s.children) collect(c,docXf); return; }
            QPainterPath dp=docXf.map(s.localPath()); if(dp.isEmpty()) return;
            bb=bb.united(dp.boundingRect()); leaves.push_back(qMakePair(dp,s));
        };
        collect(objs[mc],QTransform());
        QRectF warpSrc = (bb.isValid() && bb.width()>1 && bb.height()>1) ? bb : src;
        QVector<Shape> warped;
        for(const auto& pr : leaves){ QPainterPath wp=warpThroughEnvelope(pr.first, warpSrc, env); if(wp.isEmpty()) continue;
            const Shape& s=pr.second; Shape leaf; leaf.type=ShapeType::Path; leaf.name="Moulded"; leaf.path=wp; leaf.fillRule=s.fillRule;
            if(s.fill.isVisible()){ leaf.fill.type=FillType::Solid; leaf.fill.color=s.fill.representativeColor(); } else leaf.fill.type=FillType::None;
            leaf.stroke=s.stroke; leaf.opacity=s.opacity; leaf.transp=s.transp;
            warped.push_back(leaf); }
        QRectF wbb; for(const Shape& w: warped) wbb=wbb.united(w.docBounds());
        if(!warped.isEmpty() && sane(wbb)){ objs[mc].children=warped; objs[mc].transform=QTransform(); }
    } else {
        QPolygonF dst; for(int k=0;k<4&&k<corners.size();++k) dst<<corners[k];
        QPolygonF s; s<<src.bottomLeft()<<src.topLeft()<<src.topRight()<<src.bottomRight();
        double areaDst=dst.size()==4?qAbs(dst.boundingRect().width()*dst.boundingRect().height()):0;
        QTransform t;
        if(areaDst>1.0 && QTransform::quadToQuad(s,dst,t)){
            objs[mc].transform=t;
            // A near-singular projective map (degenerate/huge dest cage) sends the content to ~infinity
            // and there's no sane placement for it — drop the mould content so it can't poison the
            // document bounds. Sane moulds are unaffected.
            if(!sane(objs[mc].docBounds())){ objs[mc].children.clear(); objs[mc].transform=QTransform(); }
        }
    }
}

void walk(const QVector<Rec>& recs, Document& doc){
    QHash<qint32,QColor> cols;
    QHash<qint32,QImage> bmpBySeq;           // decoded embedded bitmaps, keyed by record seq
    QHash<qint32,QString> fontBySeq;         // typeface name by FONT_DEF record seq
    QHash<qint32,QPainterPath> pathBySeq;   // doc-space outline of each object, for clone (PATHREF) lookups
    // vector stroke-shape definitions (STROKEDEFINITION 4003), keyed by 24-bit handle, referenced by STROKETYPE 0x00.
    struct SSDef { QVector<Shape> shapes; bool repeating=false; int repeats=0; bool overrideFill=false; };
    QHash<quint32,SSDef> strokeShapeDefs;
    quint32 pendSDH=0, pendSDF=0, pendSDD=0; bool pendSD=false;   // pending def header awaiting its DOWN subtree
    // flatten a captured stroke-def subtree into (path, fill-colour) pairs in def space
    std::function<void(const QVector<Shape>&, QVector<QPainterPath>&, QVector<QColor>&)> flattenDef=
    [&](const QVector<Shape>& v, QVector<QPainterPath>& ps, QVector<QColor>& cs){
        for(const Shape& s: v){ if(s.type==ShapeType::Group){ flattenDef(s.children,ps,cs); continue; }
            QPainterPath pd=s.transform.map(s.localPath()); if(pd.elementCount()==0) continue;
            ps.push_back(pd); cs.push_back(s.fill.isVisible()? s.fill.representativeColor() : QColor(Qt::black)); } };
    std::vector<Frame> frames; frames.push_back(Frame{});
    frames.back().ctx.fill.type=FillType::Solid; frames.back().ctx.fill.color=Qt::black;
    frames.back().ctx.stroke.enabled=false;
    double pageH = doc.pageSize.height();
    auto fx=[&](qint32 x){ return x/MP; };
    auto fy=[&](qint32 y){ return pageH - y/MP; };
    auto fix16=[&](const QByteArray& d,int o){ return rdI32(d,o)/65536.0; };   // FIXED16 (16.16) -> double
    std::function<double(qint32)> FX=fx, FY=fy;

    // read a null-terminated UTF-16LE string starting at *o, advancing *o past the terminator
    auto readUniZ=[&](const QByteArray& d, int* o)->QString{ QString s;
        while(*o+1 < d.size()){ quint16 w=qFromLittleEndian<quint16>((const uchar*)d.constData()+*o); *o+=2;
            if(w==0) return s; s.append(QChar(w)); } return s; };

    // ---- text-story layout. We accumulate styled runs per line and lines per story, then lay
    // them out at story close (justification, line spacing, baseline shift, tracking, manual
    // kerns) and emit one Text shape per run — applying the story matrix so rotated/sheared
    // text comes out oriented correctly. A "run" is a maximal span sharing all attributes.
    struct Run { QString text, font; double size=16, baseSize=16; QColor color=Qt::black;
        bool bold=false, italic=false; double baseShift=0, track=0, kernBefore=0, aspect=1; };
    struct TLine { QVector<Run> runs; double maxSize=0, ratio=1.0, absSp=0, distPrev=0; bool paraEnd=false, hasDist=false; int justify=0; };
    struct Attr { QString font="Liberation Sans"; double size=16; bool bold=false, italic=false;
        double scriptOff=0, scriptSize=1, baseline=0, track=0, aspect=1; };
    struct StoryS { bool active=false, lineStarted=false, runOpen=false; QTransform xf; int justify=0;
        double lineRatio=1.0, lineAbs=0, pendKern=0, wrapWidth=0, leftIndent=0, rightIndent=0; bool wordWrap=false;
        bool onPath=false, reversed=false, yReflect=false; QPainterPath basePath;   // text-on-a-path (tags 2110-2117)
        bool runRetro=false;   // inside a text node's CHILD attribute block (STRING->DOWN->FONT_SIZE): style the just-read run
        QVector<TLine> lines; TLine cur; Run run; };
    StoryS st; Attr at;
    QVector<Shape> txtShapes;   // emitted last so live text sits on top of any cached text bitmap

    auto effSize =[&](){ return qMax(1.0, at.size*at.scriptSize); };
    auto effTrack=[&](){ return at.track/1000.0 * effSize(); };          // milli-em -> doc px per char
    auto effShift=[&](){ return at.size*at.scriptOff + at.baseline; };   // baseline shift, +up, doc units
    auto curColor=[&](){ return frames.back().ctx.fill.representativeColor(); };
    auto flushRun=[&](){
        if(st.runOpen && !st.run.text.isEmpty()){ st.cur.runs.push_back(st.run);
            st.cur.maxSize=qMax(st.cur.maxSize, st.run.baseSize); }
        st.runOpen=false; st.run=Run{};
    };
    auto addChar=[&](QChar ch){
        if(!st.active) return;
        st.runRetro=false;                  // a real glyph begins a new styled span (not a trailing attribute child)
        st.cur.justify = st.justify;        // justification is a per-paragraph attribute (capture the value active for this line)
        QColor col=curColor();
        if(st.runOpen && st.run.color!=col) flushRun();   // a colour change splits the run
        if(!st.runOpen){ st.run=Run{}; st.run.font=at.font; st.run.size=effSize(); st.run.baseSize=at.size;
            st.run.color=col; st.run.bold=at.bold; st.run.italic=at.italic; st.run.baseShift=effShift();
            st.run.track=effTrack(); st.run.aspect=at.aspect; st.run.kernBefore=st.pendKern; st.pendKern=0;
            st.runOpen=true; }
        st.run.text.append(ch);
    };
    auto pushLine=[&](){ flushRun(); st.cur.ratio=st.lineRatio; st.cur.absSp=st.lineAbs;
        st.lines.push_back(st.cur); st.cur=TLine{}; };
    auto baseFont=[&](const Run& r)->QFont{      // for measuring (no letter spacing)
        QFont f(r.font); f.setPixelSize(qMax(1,(int)qRound(r.size))); f.setBold(r.bold); f.setItalic(r.italic);
        if(qAbs(r.aspect-1.0)>0.01) f.setStretch(qBound(1,(int)qRound(r.aspect*100.0),400));
        return f;
    };
    auto runFont=[&](const Run& r, double extraTrack)->QFont{   // for rendering (with letter spacing)
        QFont f=baseFont(r);
        double ls=r.track+extraTrack; if(qAbs(ls)>0.01) f.setLetterSpacing(QFont::AbsoluteSpacing, ls);
        return f;
    };
    // Pen advance of a run = base glyph run + tracking after every glyph (matches what addText draws;
    // QFontMetrics::horizontalAdvance ignores letterSpacing, so we add it explicitly).
    auto runAdvance=[&](const Run& r, double extraTrack)->double{
        return QFontMetricsF(baseFont(r)).horizontalAdvance(r.text) + (r.track+extraTrack)*r.text.length();
    };
    auto layoutStory=[&](){
        if(st.active && st.lineStarted) pushLine();
        if(!st.lines.isEmpty()){
            st.lines.last().paraEnd=true;
            auto lineWidth=[&](const TLine& L, double extra)->double{ double w=0;
                for(const Run& r: L.runs) w += r.kernBefore + runAdvance(r,extra);
                return w; };
            double targetW=0; for(const TLine& L: st.lines) targetW=qMax(targetW, lineWidth(L,0));
            // ---- text-on-a-path: lay each glyph along the story's baseline curve (tags 2110-2117) ----
            // The baseline (first NodePath child of the story) is already a doc-space QPainterPath. We
            // flatten it to a polyline, walk arc-length, and place one rotated single-char Text shape per
            // glyph (centre on the path point, rotated by the segment tangent). Successive lines are pushed
            // perpendicular to the path's start direction by the usual line advance (Xara's UnitDirectionVector).
            if(st.onPath && !st.basePath.isEmpty()){
                QPainterPath bp = st.reversed ? st.basePath.toReversed() : st.basePath;
                QPolygonF poly; for(const QPolygonF& p: bp.toSubpathPolygons()) if(p.size()>poly.size()) poly=p;
                if(poly.size()>=2){
                    QVector<double> cum(poly.size(),0.0);
                    for(int k=1;k<poly.size();++k) cum[k]=cum[k-1]+QLineF(poly[k-1],poly[k]).length();
                    double total=cum.last();
                    bool closed = QLineF(poly.first(),poly.last()).length() < 1e-3;
                    QPointF d0=poly[1]-poly[0]; double L0=std::hypot(d0.x(),d0.y()); if(L0<1e-9) L0=1;
                    double perpX=-d0.y()/L0, perpY=d0.x()/L0;          // multi-line offset direction
                    auto sample=[&](double dist, QPointF& pt, double& angDeg){
                        if(closed && total>0) dist = std::fmod(std::fmod(dist,total)+total, total);
                        if(dist<=0){ double sl=QLineF(poly[0],poly[1]).length(); if(sl<1e-9) sl=1;
                            QPointF u=(poly[1]-poly[0])/sl; pt=poly[0]+u*dist; angDeg=std::atan2(u.y(),u.x())*180.0/M_PI; return; }
                        if(dist>=total){ int k=poly.size()-2; double sl=QLineF(poly[k],poly[k+1]).length(); if(sl<1e-9) sl=1;
                            QPointF u=(poly[k+1]-poly[k])/sl; pt=poly[k+1]+u*(dist-total); angDeg=std::atan2(u.y(),u.x())*180.0/M_PI; return; }
                        int k=0; while(k+2<poly.size() && cum[k+1]<=dist) ++k;
                        double sl=QLineF(poly[k],poly[k+1]).length(); if(sl<1e-9) sl=1;
                        double f=(dist-cum[k])/sl; pt=poly[k]+(poly[k+1]-poly[k])*f;
                        angDeg=std::atan2(poly[k+1].y()-poly[k].y(), poly[k+1].x()-poly[k].x())*180.0/M_PI;
                    };
                    double colL = st.leftIndent, colR = total - st.rightIndent, colW = colR - colL;
                    double baseY=0;
                    for(int li=0; li<st.lines.size(); ++li){ const TLine& L=st.lines[li];
                        double ms = L.maxSize>0? L.maxSize : at.size;
                        QFont mf("Liberation Sans"); if(!L.runs.isEmpty()) mf.setFamily(L.runs.first().font);
                        mf.setPixelSize(qMax(1,(int)qRound(ms)));
                        double lineH = L.absSp>0? L.absSp : L.ratio*QFontMetricsF(mf).height();
                        if(li>0) baseY += (L.hasDist ? -L.distPrev : lineH);
                        if(L.runs.isEmpty()) continue;
                        double natW=lineWidth(L,0), extra=0, startDist=colL;
                        if(L.justify==1) startDist=(colL+colR-natW)/2.0;            // centre on path
                        else if(L.justify==2) startDist=colR-natW;                  // right end at path end
                        else if(L.justify==3 && !L.paraEnd){ int nch=0; for(const Run& r:L.runs) nch+=r.text.length();
                            double target=(colW>0?colW:targetW); if(nch>1 && target>natW) extra=(target-natW)/(nch-1); }
                        double offX=baseY*perpX, offY=baseY*perpY;
                        double pen=startDist;
                        for(const Run& r: L.runs){ pen += r.kernBefore;
                            QFont bf=baseFont(r); QFontMetricsF fm(bf); double asc=fm.ascent();
                            for(const QChar ch: r.text){
                                double adv = fm.horizontalAdvance(ch) + r.track + extra;
                                double halfW = adv/2.0;
                                QPointF pt; double angDeg; sample(pen+halfW, pt, angDeg);
                                Shape s; s.type=ShapeType::Text; s.name="Text"; s.text=QString(ch); s.font=bf;
                                s.fill.type=FillType::Solid; s.fill.color=r.color; s.stroke.enabled=false;
                                QTransform T;
                                T.translate(pt.x()+offX, pt.y()+offY);   // land on the path point (+ line offset)
                                T.rotate(angDeg);                         // align to the segment tangent
                                T.translate(-halfW,0);                    // recover glyph LEFT origin from its centre
                                if(st.yReflect) T.scale(1,-1);            // RIGHT variants: mirror across the baseline
                                T.translate(0, -r.baseShift - asc);       // baseline shift (+up) + addText ascent
                                s.transform=T;
                                if(frames.back().inMould) frames.back().objs.push_back(s);
                                else txtShapes.push_back(s);
                                pen += adv;
                            }
                        }
                    }
                }
                st=StoryS{}; at=Attr{}; return;
            }
            // Text column (Xara's model, PositionCharsInLine): left edge at LeftIndent, right edge at
            // StoryWidth-RightIndent. StoryWidth==0 (text at a point) collapses the column onto the anchor,
            // so left/centre/right reduce to x=0 / -natW/2 / -natW exactly as before (back-compatible).
            double colL = st.leftIndent;
            double colR = (st.wrapWidth>0 ? st.wrapWidth : 0.0) - st.rightIndent;
            double colW = colR - colL;                       // JustifyWidth
            double baseY=0;
            for(int li=0; li<st.lines.size(); ++li){ const TLine& L=st.lines[li];
                double ms = L.maxSize>0? L.maxSize : at.size;
                QFont mf("Liberation Sans"); if(!L.runs.isEmpty()) mf.setFamily(L.runs.first().font);
                mf.setPixelSize(qMax(1,(int)qRound(ms)));
                double lineH = L.absSp>0? L.absSp : L.ratio*QFontMetricsF(mf).height();
                // LINE_INFO carries Xara's exact baseline-to-baseline advance; prefer it over the metric guess.
                // DistFromPreviousLine is negative (Xara y-up, lines flow down); negate into our y-down baseY.
                if(li>0) baseY += (L.hasDist ? -L.distPrev : lineH);
                if(L.runs.isEmpty()) continue;
                double natW=lineWidth(L,0), extra=0, x=colL;
                if(L.justify==1) x=(colL+colR-natW)/2.0;               // centre in column
                else if(L.justify==2) x=colR-natW;                    // right edge of column
                else if(L.justify==3 && !L.paraEnd){ int nch=0; for(const Run& r:L.runs) nch+=r.text.length();
                    double target = (st.wrapWidth>0 ? colW : targetW);  // fill column, or widest line for text-at-point
                    if(nch>1 && target>natW) extra=(target-natW)/(nch-1); }
                for(const Run& r: L.runs){ QFont f=runFont(r,extra);
                    x += r.kernBefore;
                    Shape s; s.type=ShapeType::Text; s.name="Text"; s.text=r.text; s.font=f;
                    s.fill.type=FillType::Solid; s.fill.color=r.color; s.stroke.enabled=false;
                    double ty = (baseY - r.baseShift) - QFontMetricsF(f).ascent();
                    s.transform = QTransform::fromTranslate(x, ty) * st.xf;   // run-local -> screen
                    if(frames.back().inMould) frames.back().objs.push_back(s);   // keep in the tree so the mould warps it
                    else txtShapes.push_back(s);                                  // top-level live text: hoist on top
                    x += runAdvance(r, extra);
                }
            }
        }
        st=StoryS{}; at=Attr{};
    };
    auto beginStory=[&](const QByteArray& d, quint32 tag){
        // SIMPLE vs COMPLEX governs the leading matrix shape; the *_START/END_LEFT/RIGHT variants are
        // text-on-a-path (START/END = reading direction, LEFT/RIGHT = which side of the baseline).
        bool complex = (tag==TAG_TEXT_STORY_COMPLEX ||
            tag==TAG_TEXT_STORY_COMPLEX_START_LEFT || tag==TAG_TEXT_STORY_COMPLEX_START_RIGHT ||
            tag==TAG_TEXT_STORY_COMPLEX_END_LEFT   || tag==TAG_TEXT_STORY_COMPLEX_END_RIGHT);
        Attr inh=at;                                   // capture char attrs flowed in BEFORE the story
        layoutStory(); st=StoryS{}; at=Attr{};         // (layoutStory also resets `at`, so capture first)
        // Font/metric attributes set in the group preceding TEXT_STORY are the story's inherited context
        // (Xara flows them in) — carry them across the reset instead of defaulting to 16pt. But run STYLES
        // (bold/italic/script) are per-story: Xara re-emits BOLD_ON inside each bold story, so DON'T inherit
        // them — otherwise a bold heading leaks bold into every following story (GardenPlan).
        at.font=inh.font; at.size=inh.size; at.track=inh.track; at.aspect=inh.aspect;
        st.active=true;
        st.onPath  = (tag>=TAG_TEXT_STORY_SIMPLE_START_LEFT && tag<=TAG_TEXT_STORY_COMPLEX_END_RIGHT); // 2110..2117
        st.reversed = (tag==TAG_TEXT_STORY_SIMPLE_END_LEFT  || tag==TAG_TEXT_STORY_SIMPLE_END_RIGHT ||
                       tag==TAG_TEXT_STORY_COMPLEX_END_LEFT || tag==TAG_TEXT_STORY_COMPLEX_END_RIGHT);
        st.yReflect = (tag==TAG_TEXT_STORY_SIMPLE_START_RIGHT  || tag==TAG_TEXT_STORY_SIMPLE_END_RIGHT ||
                       tag==TAG_TEXT_STORY_COMPLEX_START_RIGHT || tag==TAG_TEXT_STORY_COMPLEX_END_RIGHT);
        double a=1,b=0,c=0,dd=1; qint32 e,f;
        if(complex){ a=fix16(d,0); b=fix16(d,4); c=fix16(d,8); dd=fix16(d,12); e=rdI32(d,16); f=rdI32(d,20); }
        else       { e=rdI32(d,0); f=rdI32(d,4); }
        QTransform M(a,b,c,dd,e/MP,f/MP);                  // Xara matrix: local(y-up) -> doc(y-up)
        QTransform flipLocal(1,0,0,-1,0,0), flipScreen(1,0,0,-1,0,pageH);
        st.xf = flipLocal * M * flipScreen;                // local text(y-down) -> screen
    };

    // pop the top frame, folding its objects into the parent (group children,
    // attribute write-back, or bubble-up for structural levels).
    auto closeTop=[&](){
        if(frames.size()<2) return;
        if(frames.back().isText){ layoutStory(); }   // end of a text story: lay out + emit its lines
        Frame f=frames.back(); frames.pop_back(); Frame& par=frames.back();
        st.justify = f.savedJustify;                  // justification is scoped to its tree group (Xara attribute model)
        if(f.skip) return;   // invisible / guide layer: discard its objects entirely
        if(f.isStrokeDef){   // stroke-shape definition: stash the captured subtree (not page content)
            strokeShapeDefs[f.sdHandle & 0x00ffffffu] = SSDef{ f.objs, (f.sdFlags&0x100)!=0, (int)f.sdData1, (f.sdFlags&0x400)!=0 };
            return; }
        if(f.mouldPending) applyMould(f.objs, f.mldCorners, f.mldCtrls, f.mldSrc,    // warp content now its geometry is read
            qMax(4000.0, 3.0*qMax(doc.pageSize.width(), doc.pageSize.height())));    // plausibility limit ~ a few page dims
        // Blend container: the end shapes are stored; the in-between steps are COMPUTED. A blend over P
        // objects has P-1 BLENDERs forming a chain obj0->obj1->...->obj(P-1) (blendPairs); each pair gets
        // blendSteps intermediates. (The old single start/end collapsed chained blends to their last pair.)
        if(f.blendSteps>=0 && f.objs.size()>=2 && f.ownerIndex>=0 && f.ownerIndex<par.objs.size()){
            const int n=f.objs.size();
            auto clampi=[&](int i){ return qBound(0, i, n-1); };
            QVector<Shape> kids; QSet<int> emitted; int prevEnd=-1;
            QVector<QPair<int,int>> pairs = f.blendPairs;
            if(pairs.isEmpty()) pairs.push_back(qMakePair(0, n-1));   // no BLENDERADDITIONAL -> single end-to-end blend
            for(const auto& pr: pairs){
                int ia=clampi(pr.first), ib=clampi(pr.second);
                if(ia!=prevEnd){ kids.push_back(f.objs[ia]); emitted.insert(ia); }   // start object (unless continuing the chain)
                if(ib!=ia) for(const Shape& m: blendMorph(f.objs[ia], f.objs[ib], f.blendSteps,
                                           f.blendObjBias, f.blendObjGain, f.blendAttrBias, f.blendAttrGain)) kids.push_back(m);
                kids.push_back(f.objs[ib]); emitted.insert(ib); prevEnd=ib;
            }
            for(int j=0;j<n;++j) if(!emitted.contains(j)) kids.push_back(f.objs[j]);   // keep any non-blended children
            par.objs[f.ownerIndex].children=kids; return;
        }
        if(f.ownerIndex>=0 && f.ownerIndex<par.objs.size()){
            if(f.ownerIsGroup){ par.objs[f.ownerIndex].children=f.objs;
                if(par.objs[f.ownerIndex].isClipView && f.clipSplit>0) par.objs[f.ownerIndex].clipSplit=f.clipSplit; }
            else { applyCtx(par.objs[f.ownerIndex], f.ctx);
                if(f.pathTag) applyPathFlags(par.objs[f.ownerIndex], f.pathTag);     // re-suppress fill/stroke per the path tag
                // real Xara brush: rasterize the captured nib group + stamp it along the spine (paintBrush),
                // replacing the fat black pen stroke this spine would otherwise render as.
                if(f.brushDef && !f.objs.isEmpty()){
                    double docPerPx=1.0; QImage ink=rasterizeNib(f.objs, docPerPx);
                    if(!ink.isNull() && ink.height()>=1 && ink.width()>=1){
                        Shape& ow=par.objs[f.ownerIndex];
                        // Xara places the nib at natural size × BrushScaling, pitch = BrushScaling × BrushSpacing (ppbrush.cpp).
                        ow.stroke.enabled=true; ow.stroke.brushInk=ink; ow.stroke.brushTint=false;
                        ow.stroke.brushScale = f.brushScaling * docPerPx;                  // ink px -> doc, natural size
                        double pitch = f.brushScaling * (f.brushSpacingMP>0 ? f.brushSpacingMP/MP
                                                                            : ink.width()*docPerPx);   // default ~ one nib width
                        ow.stroke.brushSpacing = qMax(1.0, pitch);
                    }
                }
            }
        } else for(auto& o: f.objs) par.objs.push_back(o);
    };

    auto gradFill=[&](const QByteArray& d, FillType ft)->Fill{
        Fill f; f.type=ft;
        f.p0=QPointF(fx(rdI32(d,0)),fy(rdI32(d,4))); f.p1=QPointF(fx(rdI32(d,8)),fy(rdI32(d,12)));
        QColor c0=lookupCol(cols,rdI32(d,16)), c1=lookupCol(cols,rdI32(d,20));
        f.stops={{0.0,c0},{1.0,c1}}; f.color=c0; return f; };
    // multi-stage gradient: start/end COORD + start/end REF + NumCols + (pos DOUBLE, REF) per middle stop
    auto multiGradFill=[&](const QByteArray& d, FillType ft)->Fill{
        Fill f; f.type=ft;
        f.p0=QPointF(fx(rdI32(d,0)),fy(rdI32(d,4))); f.p1=QPointF(fx(rdI32(d,8)),fy(rdI32(d,12)));
        QColor c0=lookupCol(cols,rdI32(d,16)), c1=lookupCol(cols,rdI32(d,20));
        quint32 nc=rdU32(d,24); int o=28; QVector<GradStop> st; st.append({0.0,c0});
        for(quint32 k=0;k<nc && o+12<=d.size();++k){ double pos; quint64 b=qFromLittleEndian<quint64>((const uchar*)d.constData()+o);
            memcpy(&pos,&b,8); st.append({qBound(0.0,pos,1.0), lookupCol(cols,rdI32(d,o+8))}); o+=12; }
        st.append({1.0,c1}); f.stops=st; f.color=c0; return f; };

    // DEFINEBITMAP_*: UTF-16LE null-terminated name, then a complete image file. Skip the name,
    // hand the rest to QImage (it sniffs PNG/JPEG/GIF/BMP from the magic). BMPZIP is raw-deflated BMP.
    // JPEG8BPP (an RGB JPEG that Xara stores 8bpp-indexed) prepends a palette: BYTE (count-1) + count
    // RGBTRIPLEs, before the JPEG — skip it (the palette is only Xara's internal storage; the JPEG is RGB).
    auto decodeBitmap=[&](quint32 tag, const QByteArray& d)->QImage{
        int o=0; while(o+1<d.size() && !(d[o]==0 && d[o+1]==0)) o+=2;   // UTF-16 name terminator
        o+=2; if(o>d.size()) o=d.size();
        QByteArray img = d.mid(o);
        if(tag==TAG_DEFINEBITMAP_BMPZIP){ int c=0; QByteArray raw=zlibInflate(img.constData(),img.size(),c); if(!raw.isEmpty()) img=raw; }
        else if(tag==TAG_DEFINEBITMAP_JPEG8BPP){
            int p=0; if(!img.isEmpty()){ int entries=(quint8)img[0]; p=1+(entries+1)*3; }   // skip BYTE + palette
            if(p<0 || p>=img.size() || (quint8)(uchar)img[p]!=0xFF){      // verify we landed on the JPEG SOI
                int s=img.indexOf(QByteArray::fromRawData("\xFF\xD8\xFF",3)); p=(s>=0)?s:0; }  // else scan for it
            img=img.mid(p);
        }
        QImage q; q.loadFromData(img); return q; };

    // Bitmap/contone/fractal fills share a 3-point parallelogram: Start(0), End(8), End2(16).
    // p0=origin (image 0,0), p1=origin+X-axis, p2=origin+Y-axis. Returns a doc-space Fill.
    auto bitmapFill=[&](const QByteArray& d, const QImage& img0, bool contone, QColor c0, QColor c1)->Fill{
        Fill f; f.type=FillType::Bitmap; f.image=img0; f.tiled=true;   // BITMAPFILL attr tiles across the whole path (Xara fill-map default Repeat=RT_Repeating)
        f.p0=QPointF(fx(rdI32(d,0)),fy(rdI32(d,4)));
        f.p1=QPointF(fx(rdI32(d,8)),fy(rdI32(d,12)));
        f.p2=QPointF(fx(rdI32(d,16)),fy(rdI32(d,20)));
        f.contone=contone; f.color=c0; f.color2=c1;
        if(img0.isNull()){ f.type=FillType::Solid; f.color=contone?c0:QColor(150,150,150); }
        return f; };
    // Compute the square power-of-2 fractal bitmap dimension exactly as Xara does:
    // parallelogram size in pixels at the fractal DPI, next power of 2, capped at MaxFractalSize(256).
    auto fractalDim=[&](const QByteArray& d, qint32 dpi)->int{
        double sx=rdI32(d,0), sy=rdI32(d,4), ex=rdI32(d,8), ey=rdI32(d,12), e2x=rdI32(d,16), e2y=rdI32(d,20);
        double fw=std::hypot(ex-sx, ey-sy), fh=std::hypot(e2x-sx, e2y-sy);   // millipoints
        if(dpi<=0) dpi=96; if(dpi>72000) dpi=72000;
        double pw=72000.0/dpi;
        int fpw=int(fw/pw), fph=int(fh/pw); if(fpw==0)fpw=2; if(fph==0)fph=2;
        int dim=4; while(dim<plasma::MAXFC && (dim<fpw || dim<fph)) dim<<=1;
        return dim>256 ? 256 : dim; };
    // FRACTALFILL (159): exact port of Xara's PlasmaFractalFill (see makeFractalImage). 69-byte record:
    // 3 coords + StartCol@24 + EndCol@28 + Seed@32 + Graininess@36 + Gravity@40 + Squash@44 (FIXED16) +
    // Dpi@48 + Tileable@52 + Bias@53 + Gain@61 (doubles). Colours map greyscale 0->Start, 255->End.
    auto fractalFill=[&](const QByteArray& d)->Fill{
        Fill f; f.type=FillType::Linear;   // Linear stops kept as a graceful fallback / for the writer
        f.p0=QPointF(fx(rdI32(d,0)),fy(rdI32(d,4))); f.p1=QPointF(fx(rdI32(d,8)),fy(rdI32(d,12)));
        f.p2=QPointF(fx(rdI32(d,16)),fy(rdI32(d,20)));
        QColor c0=lookupCol(cols,rdI32(d,24)), c1=lookupCol(cols,rdI32(d,28));
        f.stops={{0.0,c0},{1.0,c1}}; f.color=c0; f.color2=c1;
        qint32 seed   = (d.size()>=36)? rdI32(d,32) : 63933;
        double grain  = (d.size()>=40)? rdI32(d,36)/65536.0 : 5.0;
        double gravity= (d.size()>=44)? rdI32(d,40)/65536.0 : 0.0;
        double squash = (d.size()>=48)? rdI32(d,44)/65536.0 : 0.0;
        qint32 dpi    = (d.size()>=52)? rdI32(d,48) : 96;
        bool tileable = (d.size()>=53)? (quint8(d[52])!=0) : true;
        double bias   = (d.size()>=61)? rdF64(d,53) : 0.0;
        double gain   = (d.size()>=69)? rdF64(d,61) : 0.0;
        int dim = fractalDim(d, dpi);
        f.fractal=true; f.image=makeFractalImage(c0,c1, dim, seed, tileable, squash, grain, gravity, bias, gain);
        return f; };
    // NOISEFILL (4010): Xara's NoiseFractalFill — single-octave quadratic B-spline value noise (a DIFFERENT
    // generator from the plasma). 61-byte record: 3 coords + StartCol@24 + EndCol@28 + Grain@32 (FIXED16,
    // before Seed!) + Seed@36 + Dpi@40 + Tileable@44 + Bias@45 + Gain@53. Fixed 256x256 bitmap.
    auto noiseFill=[&](const QByteArray& d)->Fill{
        Fill f; f.type=FillType::Linear;
        f.p0=QPointF(fx(rdI32(d,0)),fy(rdI32(d,4))); f.p1=QPointF(fx(rdI32(d,8)),fy(rdI32(d,12)));
        f.p2=QPointF(fx(rdI32(d,16)),fy(rdI32(d,20)));
        QColor c0=lookupCol(cols,rdI32(d,24)), c1=lookupCol(cols,rdI32(d,28));
        f.stops={{0.0,c0},{1.0,c1}}; f.color=c0; f.color2=c1;
        double grain = (d.size()>=36)? rdI32(d,32)/65536.0 : 5.0;
        qint32 seed  = (d.size()>=40)? rdI32(d,36) : 63933;
        bool tileable= (d.size()>=45)? (quint8(d[44])!=0) : true;
        double bias  = (d.size()>=53)? rdF64(d,45) : 0.0;
        double gain  = (d.size()>=61)? rdF64(d,53) : 0.0;
        f.fractal=true; f.image=makeNoiseImage(c0,c1, seed, tileable, grain, bias, gain);
        return f; };
    // Elliptical/square fills carry 3 coords (centre + 2 axis points) before the 2 colour refs
    // at offset 24/28 (unlike linear/circular, whose colours sit at 16/20).
    auto grad3Fill=[&](const QByteArray& d, FillType ft)->Fill{
        Fill f; f.type=ft;
        f.p0=QPointF(fx(rdI32(d,0)),fy(rdI32(d,4))); f.p1=QPointF(fx(rdI32(d,8)),fy(rdI32(d,12)));
        f.p2=QPointF(fx(rdI32(d,16)),fy(rdI32(d,20)));
        QColor c0=lookupCol(cols,rdI32(d,24)), c1=lookupCol(cols,rdI32(d,28));
        f.stops={{0.0,c0},{1.0,c1}}; f.color=c0; f.meshColors={c0,c1}; return f; };
    // 3-/4-colour mesh fill: Start/End/End2 coords + n colour refs (offset 24, 4 bytes each).
    auto meshFill=[&](const QByteArray& d, int n)->Fill{
        Fill f; f.type=FillType::Mesh;
        f.p0=QPointF(fx(rdI32(d,0)),fy(rdI32(d,4))); f.p1=QPointF(fx(rdI32(d,8)),fy(rdI32(d,12)));
        f.p2=QPointF(fx(rdI32(d,16)),fy(rdI32(d,20)));
        for(int k=0;k<n;++k) f.meshColors.append(lookupCol(cols,rdI32(d,24+k*4)));
        f.color=f.meshColors.isEmpty()?QColor(Qt::black):f.meshColors.first(); return f; };

    for(int i=0;i<recs.size();++i){
        const Rec& r=recs[i]; const QByteArray& d=r.data; qint32 seq=i+1;
        int objBefore = frames.back().objs.size();
        // An explicit fill-geometry record marks the context fill as set; objects that NEVER receive one
        // keep Xara's true default of "No Colour" (transparent), applied in applyCtx (Kernel rndrgn.cpp:5207).
        { static const QSet<quint32> kFillGeomTags={TAG_FLATFILL,TAG_FLATFILL_NONE,TAG_FLATFILL_BLACK,TAG_FLATFILL_WHITE,
              TAG_LINEARFILL,TAG_CONICALFILL,TAG_CIRCULARFILL,TAG_ELLIPTICALFILL,
              TAG_LINEARFILLMULTISTAGE,TAG_CONICALFILLMULTISTAGE,TAG_CIRCULARFILLMULTISTAGE,TAG_ELLIPTICALFILLMULTISTAGE,
              TAG_THREECOLFILL,TAG_FOURCOLFILL,TAG_SQUAREFILL,TAG_SQUAREFILLMULTISTAGE,
              TAG_BITMAPFILL,TAG_CONTONEBITMAPFILL,TAG_FRACTALFILL,TAG_NOISEFILL,TAG_LINEARFILL3POINT};
          if(kFillGeomTags.contains(r.tag)) frames.back().ctx.fillSet=true; }
        switch(r.tag){
        case TAG_DOWN: {
            Frame f; f.ctx=frames.back().ctx; f.savedJustify=st.justify;
            quint32 prev=(i>0)?recs[i-1].tag:0;
            if(st.active && (prev==TAG_TEXT_STRING||prev==TAG_TEXT_CHAR)) st.runRetro=true;  // text node's trailing attribute-child block (e.g. hebrew: size-after-string)
            f.inMould = frames.back().inMould || prev==TAG_MOULD_ENVELOPE || prev==TAG_MOULD_PERSPECTIVE || prev==TAG_MOULD_GROUP;
            if(prev==TAG_STROKEDEFINITION && pendSD){ f.isStrokeDef=true; f.sdHandle=pendSDH; f.sdFlags=pendSDF; f.sdData1=pendSDD; pendSD=false; }
            if(isObjectTag(prev) && !frames.back().objs.isEmpty()){
                f.ownerIndex=frames.back().objs.size()-1;
                f.ownerIsGroup=(prev==TAG_GROUP || prev==TAG_SHADOWCONTROLLER || prev==TAG_BLEND
                              || prev==TAG_CONTOURCONTROLLER || prev==TAG_BEVEL || prev==TAG_CLIPVIEWCONTROLLER
                              || prev==TAG_FEATHER_EFFECT
                              || prev==TAG_MOULD_ENVELOPE || prev==TAG_MOULD_PERSPECTIVE || prev==TAG_MOULD_GROUP);
                if(prev==TAG_BLEND) f.blendSteps=qMin((int)rdU16(recs[i-1].data,0), 256);   // blend children -> morph at close (capped)
                if((prev>=TAG_PATH && prev<=TAG_PATH_FILLED_STROKED) ||
                   (prev>=TAG_PATH_RELATIVE && prev<=TAG_PATH_RELATIVE_FILLED_STROKED)) f.pathTag=prev;
            }
            if(prev==TAG_TEXT_STORY_SIMPLE||prev==TAG_TEXT_STORY_COMPLEX||
               (prev>=TAG_TEXT_STORY_SIMPLE_START_LEFT && prev<=TAG_TEXT_STORY_COMPLEX_END_RIGHT)) f.isText=true;  // 2110..2117
            frames.push_back(f); break; }
        case TAG_UP: closeTop(); break;
        case TAG_SPREADINFORMATION:
            if(d.size()>=8){ qint32 w=rdI32(d,0),h=rdI32(d,4);
                if(w>0&&h>0){ doc.pageSize=QSizeF(w/MP,h/MP); pageH=h/MP; } } break;
        // layer flags (byte 0): bit0=visible. An invisible layer's content must not render (the engine
        // skips it). Guide layers hold guides, not artwork -> also skip. Marks the layer's content frame.
        case TAG_LAYERDETAILS: if(!d.isEmpty() && !((quint8)d[0] & 0x1)) frames.back().skip=true; break;
        case TAG_GUIDELAYERDETAILS: frames.back().skip=true; break;
        case TAG_DEFINERGBCOLOUR:
            if(d.size()>=3) cols[seq]=QColor((quint8)d[0],(quint8)d[1],(quint8)d[2]); break;
        case TAG_DEFINECOMPLEXCOLOUR:
            if(d.size()>=3) cols[seq]=QColor((quint8)d[0],(quint8)d[1],(quint8)d[2]); break; // RGB fallback
        case TAG_DEFINEBITMAP_BMP: case TAG_DEFINEBITMAP_GIF: case TAG_DEFINEBITMAP_JPEG:
        case TAG_DEFINEBITMAP_PNG: case TAG_DEFINEBITMAP_BMPZIP: case TAG_DEFINEBITMAP_JPEG8BPP:
        case TAG_DEFINEBITMAP_PNG_REAL: {
            QImage q=decodeBitmap(r.tag,d); if(!q.isNull()) bmpBySeq[seq]=q; } break;
        // ---- fill / line attributes (set the current context) ----
        case TAG_FLATFILL: { auto& c=frames.back().ctx; c.fill=Fill(); c.fill.type=FillType::Solid; c.fill.color=lookupCol(cols,rdI32(d,0)); } break;
        case TAG_FLATFILL_NONE: frames.back().ctx.fill=Fill(); frames.back().ctx.fill.type=FillType::None; break;
        case TAG_FLATFILL_BLACK: { auto& c=frames.back().ctx; c.fill=Fill(); c.fill.type=FillType::Solid; c.fill.color=Qt::black; } break;
        case TAG_FLATFILL_WHITE: { auto& c=frames.back().ctx; c.fill=Fill(); c.fill.type=FillType::Solid; c.fill.color=Qt::white; } break;
        case TAG_LINEARFILL: case TAG_CONICALFILL:
            frames.back().ctx.fill=gradFill(d, FillType::Linear); break;
        case TAG_CIRCULARFILL: frames.back().ctx.fill=gradFill(d, FillType::Radial); break;
        case TAG_ELLIPTICALFILL: frames.back().ctx.fill=grad3Fill(d, FillType::Radial); break;   // 3 coords -> colours @24/28
        case TAG_LINEARFILLMULTISTAGE: case TAG_CONICALFILLMULTISTAGE: frames.back().ctx.fill=multiGradFill(d, FillType::Linear); break;
        case TAG_CIRCULARFILLMULTISTAGE: frames.back().ctx.fill=multiGradFill(d, FillType::Radial); break;
        case TAG_ELLIPTICALFILLMULTISTAGE: frames.back().ctx.fill=grad3Fill(d, FillType::Radial); break;   // 2-stop approx
        // 3-/4-colour mesh + square/diamond fills
        case TAG_THREECOLFILL: frames.back().ctx.fill=meshFill(d, 3); break;
        case TAG_FOURCOLFILL: frames.back().ctx.fill=meshFill(d, 4); break;
        case TAG_SQUAREFILL: case TAG_SQUAREFILLMULTISTAGE: frames.back().ctx.fill=grad3Fill(d, FillType::Square); break;
        // bitmap / contone / fractal fills (3-point parallelogram + bitmap or colour refs)
        case TAG_BITMAPFILL: frames.back().ctx.fill=bitmapFill(d, bmpBySeq.value(rdI32(d,24)), false, Qt::black, Qt::white); break;
        case TAG_CONTONEBITMAPFILL: frames.back().ctx.fill=bitmapFill(d, bmpBySeq.value(rdI32(d,32)), true,
            lookupCol(cols,rdI32(d,24)), lookupCol(cols,rdI32(d,28))); break;
        case TAG_FRACTALFILL: frames.back().ctx.fill=fractalFill(d); break;   // plasma midpoint-displacement
        case TAG_NOISEFILL:   frames.back().ctx.fill=noiseFill(d);   break;   // B-spline value noise (61-byte layout)
        case TAG_LINEARFILL3POINT: frames.back().ctx.fill=grad3Fill(d, FillType::Linear); break;  // 3 coords -> colours @24/28
        // ---- 0-byte fill-effect markers that modify the current fill ----
        case TAG_FILL_REPEATING: case TAG_TRANSPARENTFILL_REPEATING:
        case TAG_FILL_REPEATING_EXTRA: case TAG_TRANSPARENTFILL_REPEATING_EXTRA:   // 206/207: Kernel maps Repeat=4 -> Tile (forward repeat, == 163)
            frames.back().ctx.fill.spread=QGradient::RepeatSpread; break;
        case TAG_FILL_REPEATINGINVERTED: case TAG_TRANSPARENTFILL_REPEATINGINVERTED: frames.back().ctx.fill.spread=QGradient::ReflectSpread; break;
        case TAG_FILL_NONREPEATING: case TAG_TRANSPARENTFILL_NONREPEATING: frames.back().ctx.fill.spread=QGradient::PadSpread; break;
        case TAG_FILLEFFECT_FADE: frames.back().ctx.fill.rainbow=false; break;
        case TAG_FILLEFFECT_RAINBOW: frames.back().ctx.fill.rainbow=true; frames.back().ctx.fill.rainbowAlt=false; break;
        case TAG_FILLEFFECT_ALTRAINBOW: frames.back().ctx.fill.rainbow=true; frames.back().ctx.fill.rainbowAlt=true; break;
        case TAG_LINECOLOUR: { auto& c=frames.back().ctx; c.stroke.enabled=true; c.stroke.color=lookupCol(cols,rdI32(d,0)); } break;
        case TAG_LINECOLOUR_NONE: frames.back().ctx.stroke.enabled=false; break;
        case TAG_LINECOLOUR_BLACK: { auto& c=frames.back().ctx; c.stroke.enabled=true; c.stroke.color=Qt::black; } break;
        case TAG_LINECOLOUR_WHITE: { auto& c=frames.back().ctx; c.stroke.enabled=true; c.stroke.color=Qt::white; } break;
        case TAG_LINEWIDTH: frames.back().ctx.stroke.width=qMax(0.1, rdI32(d,0)/MP); break;
        // variable-width / pressure-profile strokes. STROKETYPE handle top byte: 0x02=variable-width engine,
        // 0x01=plain line, 0x00=vector stroke (out of scope -> constant pen). The profile (a multiplier of
        // LINEWIDTH along the normalised arc length) comes from VARIABLEWIDTHFUNC (predefined id) or
        // VARIABLEWIDTHTABLE (an embedded ValueFunction). half-width(t) = (width/2) * profile(t).
        case TAG_STROKETYPE: if(d.size()>=4){ auto& st=frames.back().ctx.stroke; quint32 h=rdU32(d,0); int top=(h>>24)&0xff;
            st.varWidth=(top==2); if(top==1){ st.varWidth=false; st.widthProfile.clear(); }
            if(top==0){ auto it=strokeShapeDefs.constFind(h&0x00ffffffu);   // 0x00 = vector stroke -> resolve def
                if(it!=strokeShapeDefs.constEnd()){ st.strokeShapePaths.clear(); st.strokeShapeCols.clear();
                    flattenDef(it->shapes, st.strokeShapePaths, st.strokeShapeCols);
                    st.strokeRepeating=it->repeating; st.strokeRepeats=it->repeats; st.strokeOverrideFill=it->overrideFill; } } } break;
        // vector stroke-shape definition: header {handle, flags, data1, data2} (+ name if flags&0x200), then a
        // DOWN/UP clipart subtree captured by the next frame (isStrokeDef) and stashed in strokeShapeDefs.
        case TAG_STROKEDEFINITION: if(d.size()>=12){ pendSDH=rdU32(d,0); pendSDF=rdU32(d,4); pendSDD=rdU32(d,8); pendSD=true; } break;
        case TAG_VARIABLEWIDTHFUNC: if(d.size()>=4){ auto& st=frames.back().ctx.stroke; qint32 id=rdI32(d,0);
            if(id==0) st.widthProfile.clear();                                  // VarWidth_NotPredefined: no profile
            else if(id==-1) st.widthProfile={{0,1},{1,1}};                      // constant
            else if(id==-2) st.widthProfile={{0,1},{1,0}};                      // linear ramp 1->0
            else if(id==-3){ st.widthProfile.clear();                           // S-ramp 1->0 (raised cosine)
                for(int k=0;k<=16;++k){ double t=k/16.0; st.widthProfile.push_back(QPointF(t,(qCos(t*M_PI)+1.0)/2.0)); } }
            st.varWidth = st.varWidth || !st.widthProfile.isEmpty(); } break;
        // embedded ValueFunction: id@0 selects the subtype. Pressure(5)/PressureS(6) carry {u16 pos,u16 pressure}
        // samples (/65535); Constant(1)=1 float; RampLinear(3)/RampS(4)=2 floats. Anything else -> flat (1.0).
        case TAG_VARIABLEWIDTHTABLE: if(d.size()>=4){ auto& st=frames.back().ctx.stroke; qint32 vfid=rdI32(d,0);
            QVector<QPointF> prof;
            if(vfid==5 || vfid==6){ quint32 n=rdU32(d,4); int o=8;
                for(quint32 k=0;k<n && o+4<=d.size(); ++k,o+=4)
                    prof.push_back(QPointF(rdU16(d,o)/65535.0, rdU16(d,o+2)/65535.0)); }   // pos, pressure (pre-normalised)
            else if(vfid==1){ double v=rdF32(d,4); prof={{0,v},{1,v}}; }                    // constant
            else if(vfid==3){ double v1=rdF32(d,4),v2=rdF32(d,8); prof={{0,v1},{1,v2}}; }   // linear ramp
            else if(vfid==4){ double v1=rdF32(d,4),v2=rdF32(d,8);                           // S ramp (raised cosine)
                for(int k=0;k<=16;++k){ double t=k/16.0, b=(qCos(t*M_PI)+1.0)/2.0; prof.push_back(QPointF(t,b*v1+(1.0-b)*v2)); } }
            else prof={{0,1},{1,1}};                                                        // unknown subtype -> flat
            st.widthProfile=prof; st.varWidth=true; } break;
        // Cap byte is the raw Xara LineCapType enum (Kernel/attr.h): 0=Butt,1=Round,2=Square.
        case TAG_STARTCAP: case TAG_ENDCAP: if(!d.isEmpty()){ int v=(quint8)d[0];
            frames.back().ctx.stroke.cap = (v==1)?Qt::RoundCap : (v==2)?Qt::SquareCap : Qt::FlatCap; } break;
        // Join byte is the raw Xara JointType enum (Kernel/attr.h): 0=Mitre,1=Round,2=Bevel.
        case TAG_JOINSTYLE: if(!d.isEmpty()){ int v=(quint8)d[0];
            frames.back().ctx.stroke.join = (v==1)?Qt::RoundJoin : (v==2)?Qt::BevelJoin : Qt::MiterJoin; } break;
        // winding rule: 0=non-zero, 1=negative, 2=even-odd (default), 3=positive.
        // Qt has no signed-winding variant, so the non-zero family all map to WindingFill.
        case TAG_WINDINGRULE: if(!d.isEmpty()){ int v=(quint8)d[0];
            frames.back().ctx.fillRule = (v==2)?Qt::OddEvenFill : Qt::WindingFill; } break;
        // stroke transparency: value(byte) + type(byte); 0=opaque, 255=transparent
        case TAG_LINETRANSPARENCY: if(d.size()>=2){ int t=(quint8)d[0],ty=(quint8)d[1];
            frames.back().ctx.stroke.transp = (ty==0)?0.0 : t/255.0; } break;
        // brush definition. Real Xara wraps a nib GROUP (4 bytes here) -> mark the spine's attr frame so
        // its captured nib is rasterized + stamped at close. The app's own 9+ byte payload (ink bitmap ref +
        // spacing×1000 + tint) is still honoured for self-authored files.
        case TAG_BRUSHDEFINITION: frames.back().brushDef=true;
            if(d.size()>=9){ auto& st=frames.back().ctx.stroke;
                st.brushInk=bmpBySeq.value(rdI32(d,0)); st.brushSpacing=qMax(0.05,rdI32(d,4)/1000.0);
                st.brushTint=(quint8)d[8]; st.enabled=true; } break;
        // real Xara brush data: Handle(u32)@0, Spacing(i32 mp)@4, Flags@8, RotateAngle(f64)@9, Offset@17,
        // PathOffsetValue@21, Unicode name@25 (null-term), then BrushScaling(f64). Scaling sizes the nib + pitch.
        case TAG_BRUSHDATA: if(d.size()>=8){ frames.back().brushSpacingMP=rdI32(d,4);
            int o=25; while(o+1<d.size() && !(d[o]==0 && d[o+1]==0)) o+=2; o+=2;   // skip the null-terminated UTF-16 name
            if(o+8<=d.size()){ double s=rdF64(d,o); if(s>1e-4 && s<1e4) frames.back().brushScaling=s; } } break;
        // dash pattern: INT32 dash index (-21=solid, other negatives = predefined patterns). The pattern
        // table isn't in the file, so approximate any non-solid style with a generic dash.
        case TAG_DASHSTYLE: if(d.size()>=4){ qint32 dt=rdI32(d,0);
            frames.back().ctx.stroke.dashes = (dt==-21)? QVector<qreal>() : QVector<qreal>{4.0,3.0}; } break;
        // arrowheads: ArrowId(INT32) + ScaleWidth/ScaleHeight FIXED16. id=-1 NULL(none); negative=stock shape
        // (-2 straight..-9 hollow-diamond, see render stockArrowPath); positive=custom (no def read -> straight).
        case TAG_STARTARROW: if(d.size()>=12){ qint32 id=rdI32(d,0);
            frames.back().ctx.stroke.startArrow=(id==-1)?0:id;
            frames.back().ctx.stroke.startArrowW=rdI32(d,4)/65536.0; frames.back().ctx.stroke.startArrowH=rdI32(d,8)/65536.0; } break;
        case TAG_ENDARROW: if(d.size()>=12){ qint32 id=rdI32(d,0);
            frames.back().ctx.stroke.endArrow=(id==-1)?0:id;
            frames.back().ctx.stroke.endArrowW=rdI32(d,4)/65536.0; frames.back().ctx.stroke.endArrowH=rdI32(d,8)/65536.0; } break;
        // ---- transparency (approximated as object opacity; 0=opaque, 255=transparent) ----
        case TAG_FLATTRANSPARENTFILL: if(d.size()>=2){ Transparency tr; tr.type=(quint8)d[1]; tr.geom=0;
            tr.t0=tr.t1=(quint8)d[0]/255.0; frames.back().ctx.transp=tr; } break;
        // graduated transparency: start/end COORD + start/end transparency bytes + type. linear/circular/
        // conical carry the bytes at offset 16/17/18; elliptical (3 axis coords) at 24/25/26.
        case TAG_LINEARTRANSPARENTFILL: case TAG_CIRCULARTRANSPARENTFILL: case TAG_CONICALTRANSPARENTFILL:
            if(d.size()>=19){ Transparency tr; tr.geom=(r.tag==TAG_LINEARTRANSPARENTFILL)?1:2;
                tr.p0=QPointF(fx(rdI32(d,0)),fy(rdI32(d,4))); tr.p1=QPointF(fx(rdI32(d,8)),fy(rdI32(d,12)));
                tr.t0=(quint8)d[16]/255.0; tr.t1=(quint8)d[17]/255.0; tr.type=(quint8)d[18];
                frames.back().ctx.transp=tr; } break;
        case TAG_ELLIPTICALTRANSPARENTFILL:
            if(d.size()>=27){ Transparency tr; tr.geom=2; tr.elliptical=true;
                tr.p0=QPointF(fx(rdI32(d,0)),fy(rdI32(d,4))); tr.p1=QPointF(fx(rdI32(d,8)),fy(rdI32(d,12)));
                tr.p2=QPointF(fx(rdI32(d,16)),fy(rdI32(d,20)));   // 2nd axis endpoint (was ignored -> rendered as a circle)
                tr.t0=(quint8)d[24]/255.0; tr.t1=(quint8)d[25]/255.0; tr.type=(quint8)d[26];
                frames.back().ctx.transp=tr; } break;
        // bitmap/fractal MASKED transparency: no mask raster -> approximate as a linear transparency
        // ramp along the parallelogram axis (start/end bytes sit just after the 3 coords, offset 24).
        case TAG_FRACTALTRANSPARENTFILL: case TAG_NOISETRANSPARENTFILL:
            if(d.size()>=27){ Transparency tr; tr.geom=3;   // per-pixel fractal/noise alpha mask
                tr.p0=QPointF(fx(rdI32(d,0)),fy(rdI32(d,4))); tr.p1=QPointF(fx(rdI32(d,8)),fy(rdI32(d,12)));
                tr.t0=(quint8)d[24]/255.0; tr.t1=(quint8)d[25]/255.0; tr.type=(quint8)d[26];
                frames.back().ctx.transp=tr; } break;
        // bitmap MASK transparency: 3 parallelogram coords, then Transp@24 EndTransp@25 Type@26 BitmapRef@27.
        // The bitmap's luminance modulates per-pixel alpha (a soft drop-shadow etc.); resolve it and render
        // a real mask (geom=4). If the bitmap is missing, fall back to the flat linear approximation.
        case TAG_BITMAPTRANSPARENTFILL:
            if(d.size()>=27){ Transparency tr;
                tr.p0=QPointF(fx(rdI32(d,0)),fy(rdI32(d,4))); tr.p1=QPointF(fx(rdI32(d,8)),fy(rdI32(d,12)));
                tr.p2=QPointF(fx(rdI32(d,16)),fy(rdI32(d,20)));
                tr.t0=(quint8)d[24]/255.0; tr.t1=(quint8)d[25]/255.0; tr.type=(quint8)d[26];
                QImage mb = (d.size()>=31) ? bmpBySeq.value(rdI32(d,27)) : QImage();
                if(!mb.isNull()){ tr.geom=4; tr.maskBmp=mb; } else tr.geom=1;
                frames.back().ctx.transp=tr; } break;
        case TAG_LINEARTRANSPARENTFILL3POINT:   // genuinely a 3-point LINEAR transparency (not a bitmap mask)
            if(d.size()>=27){ Transparency tr; tr.geom=1;
                tr.p0=QPointF(fx(rdI32(d,0)),fy(rdI32(d,4))); tr.p1=QPointF(fx(rdI32(d,8)),fy(rdI32(d,12)));
                tr.t0=(quint8)d[24]/255.0; tr.t1=(quint8)d[25]/255.0; tr.type=(quint8)d[26];
                frames.back().ctx.transp=tr; } break;
        // square transparency: like elliptical (start/end bytes @24/25, type @26) -> graduated radial.
        case TAG_SQUARETRANSPARENTFILL:
            if(d.size()>=27){ Transparency tr; tr.geom=2;
                tr.p0=QPointF(fx(rdI32(d,0)),fy(rdI32(d,4))); tr.p1=QPointF(fx(rdI32(d,8)),fy(rdI32(d,12)));
                tr.t0=(quint8)d[24]/255.0; tr.t1=(quint8)d[25]/255.0; tr.type=(quint8)d[26];
                frames.back().ctx.transp=tr; } break;
        // 3-/4-colour transparency: per-corner bytes after the 3 coords (@24..); approximate as flat mean.
        case TAG_THREECOLTRANSPARENTFILL: case TAG_FOURCOLTRANSPARENTFILL:
            if(d.size()>=28){ int n=(r.tag==TAG_FOURCOLTRANSPARENTFILL)?4:3; int sum=0; for(int k=0;k<n;++k) sum+=(quint8)d[24+k];
                Transparency tr; tr.geom=0; tr.type=(quint8)d[24+n]; tr.t0=tr.t1=(sum/(double)n)/255.0;
                frames.back().ctx.transp=tr; } break;
        // ---- objects ----
        case TAG_PATH: case TAG_PATH_FILLED: case TAG_PATH_STROKED: case TAG_PATH_FILLED_STROKED: {
            Shape s=makePathShape(d,FX,FY);
            if(st.active && st.onPath && st.basePath.isEmpty()){ st.basePath=s.path; break; }  // text-on-path baseline (invisible guide)
            applyCtx(s,frames.back().ctx); applyPathFlags(s,r.tag);
            frames.back().objs.push_back(s); } break;
        case TAG_PATH_RELATIVE: case TAG_PATH_RELATIVE_FILLED:
        case TAG_PATH_RELATIVE_STROKED: case TAG_PATH_RELATIVE_FILLED_STROKED: {
            Shape s=makeRelPathShape(d,FX,FY);
            if(st.active && st.onPath && st.basePath.isEmpty()){ st.basePath=s.path; break; }  // text-on-path baseline (invisible guide)
            applyCtx(s,frames.back().ctx); applyPathFlags(s,r.tag);
            frames.back().objs.push_back(s); } break;
        case TAG_RECTANGLE_SIMPLE: case TAG_RECTANGLE_SIMPLE_ROUNDED: {
            QPointF c(fx(rdI32(d,0)),fy(rdI32(d,4))); double w=rdI32(d,8)/MP, h=rdI32(d,12)/MP;
            Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,w,h); s.name="Rectangle";
            if(r.tag==TAG_RECTANGLE_SIMPLE_ROUNDED && d.size()>=24){ double u; quint64 b=qFromLittleEndian<quint64>((const uchar*)d.constData()+16); memcpy(&u,&b,8); s.cornerRadius=u*qMin(w,h)*0.5; }
            s.transform=QTransform::fromTranslate(c.x()-w/2, c.y()-h/2); applyCtx(s,frames.back().ctx);
            frames.back().objs.push_back(s); } break;
        case TAG_ELLIPSE_SIMPLE: {
            QPointF c(fx(rdI32(d,0)),fy(rdI32(d,4))); double w=rdI32(d,8)/MP, h=rdI32(d,12)/MP;
            Shape s; s.type=ShapeType::Ellipse; s.rect=QRectF(0,0,w,h); s.name="Ellipse";
            s.transform=QTransform::fromTranslate(c.x()-w/2, c.y()-h/2); applyCtx(s,frames.back().ctx);
            frames.back().objs.push_back(s); } break;
        case TAG_ELLIPSE_COMPLEX: {
            QPointF c(fx(rdI32(d,0)),fy(rdI32(d,4)));
            QPointF maj(fx(rdI32(d,8))-c.x(), fy(rdI32(d,12))-c.y()), min(fx(rdI32(d,16))-c.x(), fy(rdI32(d,20))-c.y());
            Shape s=axisShape(c,maj,min,0,true); applyCtx(s,frames.back().ctx); frames.back().objs.push_back(s); } break;
        case TAG_POLYGON_COMPLEX: {
            int sides=rdU16(d,0); QPointF c(fx(rdI32(d,2)),fy(rdI32(d,6)));
            QPointF maj(fx(rdI32(d,10))-c.x(), fy(rdI32(d,14))-c.y()), min(fx(rdI32(d,18))-c.x(), fy(rdI32(d,22))-c.y());
            Shape s=axisShape(c,maj,min,sides,false); applyCtx(s,frames.back().ctx); frames.back().objs.push_back(s); } break;
        case TAG_REGULAR_SHAPE_PHASE_2: {
            Shape s=makeRegularShape(d, pageH); applyCtx(s,frames.back().ctx); frames.back().objs.push_back(s); } break;
        // clone: reference an earlier path by sequence number + apply a matrix (in XAR space).
        case TAG_PATHREF_TRANSFORM: if(d.size()>=28){
            qint32 ref=rdI32(d,0); auto it=pathBySeq.find(ref);
            if(it!=pathBySeq.end()){
                double a=rdI32(d,4)/65536.0, b=rdI32(d,8)/65536.0, cc=rdI32(d,12)/65536.0, dd=rdI32(d,16)/65536.0;
                double e=rdI32(d,20), f=rdI32(d,24);
                QTransform M(a,b,cc,dd,e,f);                    // XAR-space transform
                QTransform F(1.0/MP,0,0,-1.0/MP,0,pageH);       // XAR mp (y-up) -> doc units (y-down)
                QTransform Mp = F.inverted() * M * F;           // same transform expressed in doc space
                Shape s; s.type=ShapeType::Path; s.name="Clone"; s.path=Mp.map(it.value());
                applyCtx(s,frames.back().ctx); frames.back().objs.push_back(s);
            } } break;
        // placed bitmap object: 4 parallelogram corners P0..P3 + bitmap ref (+ 2 colour refs for contone).
        // Render as the parallelogram path, filled with the bitmap mapped P0=origin, P1=+X, P3=+Y.
        case TAG_NODE_BITMAP: case TAG_NODE_CONTONEDBITMAP: if(d.size()>=36){
            QPointF P0(fx(rdI32(d,0)),fy(rdI32(d,4))), P1(fx(rdI32(d,8)),fy(rdI32(d,12)));
            QPointF P2(fx(rdI32(d,16)),fy(rdI32(d,20))), P3(fx(rdI32(d,24)),fy(rdI32(d,28)));
            qint32 bref=rdI32(d,32);
            bool contone=(r.tag==TAG_NODE_CONTONEDBITMAP);
            QColor c0=Qt::black,c1=Qt::white;
            if(contone && d.size()>=44){ c0=lookupCol(cols,rdI32(d,36)); c1=lookupCol(cols,rdI32(d,40)); }
            Shape s; s.type=ShapeType::Path; s.name="Bitmap";
            QPainterPath pp; pp.moveTo(P0); pp.lineTo(P1); pp.lineTo(P2); pp.lineTo(P3); pp.closeSubpath(); s.path=pp;
            applyCtx(s,frames.back().ctx);                 // inherit stroke/transp/opacity, then override fill
            // The 4 corners come in either winding order; pick them by position so the (y-down) image
            // isn't flipped/mirrored: p0=image bottom-left, p1=bottom-right, p2=top-left.
            // Map the image respecting the stored corner ORDER (P0=origin=image(0,0), P1=P0+X-axis,
            // P3=P0+Y-axis), which PRESERVES any flip/mirror the artist applied. bitmapTexXf wants
            // p2=image(0,0), p0=image(0,H), p1=p0+X-axis. (Position-normalizing the corners by min/max
            // would silently un-flip mirrored placements — e.g. Spitfire's deliberately flipped sky photo.)
            Fill f; f.type=FillType::Bitmap; f.image=bmpBySeq.value(bref);
            f.p2=P0; f.p0=P3; f.p1=P3+(P1-P0); f.contone=contone; f.color=c0; f.color2=c1;
            if(f.image.isNull()){ f.type=FillType::Solid; f.color=contone?c0:QColor(150,150,150); }
            else if(!contone){ QImage keyed=f.image; if(luminanceKeyDarkOverlay(keyed)) f.image=keyed; }   // drop opaque-black overlay plates (e.g. scope3 swoosh)
            s.fill=f; s.stroke.enabled=false;
            frames.back().objs.push_back(s); } break;
        // ---- text ----
        // font definition: FullName + TypeFaceName (UTF-16) + PANOSE(10 bytes). No embedded TTF data. If the
        // named font isn't installed, substitute a same-class family (PANOSE, else name heuristic) so the
        // text keeps the right style + metrics instead of Qt's one generic fallback.
        case TAG_FONT_DEF_TRUETYPE: case TAG_FONT_DEF_ATM: {
            int o=0; QString full=readUniZ(d,&o); QString face=readUniZ(d,&o);
            QString name = face.isEmpty()? full : face;
            quint8 pan[10]={0}; if(o+10<=d.size()) for(int k=0;k<10;k++) pan[k]=(quint8)d[o+k];
            fontBySeq[seq] = resolveFontFamily(name, pan); } break;
        // text stories: SIMPLE = coord anchor (translation matrix); COMPLEX = full 2x3 matrix. The
        // START/END_LEFT/RIGHT variants are text-on-path orientation flags (mirror / reverse); they read the
        // same leading matrix (the COMPLEX ones append 2 angles we don't need), so route every variant to
        // beginStory by its simple/complex matrix shape. Previously only 2100/2101/2110/2114 were handled,
        // so e.g. Watch4's COMPLEX_END_RIGHT(2117) story was dropped entirely (text missing).
        case TAG_TEXT_STORY_SIMPLE: case TAG_TEXT_STORY_SIMPLE_START_LEFT:
        case TAG_TEXT_STORY_SIMPLE_START_RIGHT: case TAG_TEXT_STORY_SIMPLE_END_LEFT:
        case TAG_TEXT_STORY_SIMPLE_END_RIGHT:
        case TAG_TEXT_STORY_COMPLEX: case TAG_TEXT_STORY_COMPLEX_START_LEFT:
        case TAG_TEXT_STORY_COMPLEX_START_RIGHT: case TAG_TEXT_STORY_COMPLEX_END_LEFT:
        case TAG_TEXT_STORY_COMPLEX_END_RIGHT: beginStory(d,r.tag); break;
        case TAG_TEXT_LINE: if(st.active){ if(st.lineStarted) pushLine(); st.lineStarted=true; st.runRetro=false; } break;
        case TAG_TEXT_STRING: if(st.active) for(int o=0;o+1<d.size();o+=2)
            addChar(QChar(qFromLittleEndian<quint16>((const uchar*)d.constData()+o))); break;
        case TAG_TEXT_CHAR: if(st.active && d.size()>=2) addChar(QChar(rdU16(d,0))); break;
        // hard return / end of a cached line. Xara re-establishes run styles per line: BOLD_ON/ITALIC_ON
        // are emitted before each styled line's content but there is NO matching OFF, so bold/italic must
        // reset at EOL (otherwise a bold heading leaks bold into the regular body that follows — GardenPlan).
        case TAG_TEXT_EOL: if(st.active){ st.cur.paraEnd=true; flushRun(); at.bold=false; at.italic=false; st.runRetro=false; } break;
        case TAG_TEXT_KERN: if(st.active && d.size()>=4){ flushRun(); st.pendKern += rdI32(d,0)/MP; st.runRetro=false; } break;  // manual kern X
        // FONT_SIZE/TYPEFACE normally style the NEXT run; but a text node can carry them as a trailing
        // attribute CHILD (STRING->DOWN->FONT_SIZE, e.g. hebrew) — then retro-apply to the just-read run.
        case TAG_TEXT_FONT_SIZE: if(d.size()>=4){ flushRun(); at.size=qMax(1.0, rdI32(d,0)/MP);
            if(st.runRetro && !st.cur.runs.isEmpty()){ Run& lr=st.cur.runs.last(); lr.size=effSize(); lr.baseSize=at.size; st.cur.maxSize=qMax(st.cur.maxSize, at.size); } } break;
        case TAG_TEXT_FONT_TYPEFACE: if(d.size()>=4){ flushRun(); at.font=fontBySeq.value(rdI32(d,0), at.font);
            if(st.runRetro && !st.cur.runs.isEmpty()) st.cur.runs.last().font=at.font; } break;
        case TAG_TEXT_BOLD_ON:   flushRun(); at.bold=true;  break;  case TAG_TEXT_BOLD_OFF:   flushRun(); at.bold=false;  break;
        case TAG_TEXT_ITALIC_ON: flushRun(); at.italic=true; break; case TAG_TEXT_ITALIC_OFF: flushRun(); at.italic=false; break;
        case TAG_TEXT_JUSTIFICATION_LEFT:   st.justify=0; break;
        case TAG_TEXT_JUSTIFICATION_CENTRE: st.justify=1; break;
        case TAG_TEXT_JUSTIFICATION_RIGHT:  st.justify=2; break;
        case TAG_TEXT_JUSTIFICATION_FULL:   st.justify=3; break;
        case TAG_TEXT_LINESPACE_RATIO:    if(d.size()>=4) st.lineRatio=qBound(0.25, fix16(d,0), 8.0); break;
        case TAG_TEXT_LINESPACE_ABSOLUTE: if(d.size()>=4){ st.lineAbs=qMax(0.0, rdI32(d,0)/MP); } break;
        // per-line cached metric: Width, Height, DistFromPreviousLine (baseline-to-baseline advance, mp).
        case TAG_TEXT_LINE_INFO: if(st.active && st.lineStarted && d.size()>=12){ st.cur.distPrev=rdI32(d,8)/MP; st.cur.hasDist=true; } break;
        // story-level column geometry: word-wrap width and left/right indents.
        case TAG_TEXT_STORY_WORD_WRAP_INFO: if(st.active && d.size()>=5){ st.wrapWidth=qMax(0.0, rdI32(d,0)/MP); st.wordWrap=(quint8)d[4]!=0; } break;
        case TAG_TEXT_STORY_INDENT_INFO:    if(st.active && d.size()>=8){ st.leftIndent=rdI32(d,0)/MP; st.rightIndent=rdI32(d,4)/MP; } break;
        case TAG_TEXT_SUPERSCRIPT_ON: flushRun(); at.scriptOff= 0.33; at.scriptSize=0.5; break;
        case TAG_TEXT_SUBSCRIPT_ON:   flushRun(); at.scriptOff=-0.10; at.scriptSize=0.5; break;
        case TAG_TEXT_SCRIPT_ON: if(d.size()>=8){ flushRun(); at.scriptOff=fix16(d,0); at.scriptSize=qMax(0.05,fix16(d,4)); } break;
        case TAG_TEXT_SCRIPT_OFF:     flushRun(); at.scriptOff=0; at.scriptSize=1; break;
        case TAG_TEXT_TRACKING:  if(d.size()>=4){ flushRun(); at.track=rdI32(d,0); } break;        // milli-em
        case TAG_TEXT_BASELINE:  if(d.size()>=4){ flushRun(); at.baseline=rdI32(d,0)/MP; } break;  // explicit baseline shift, +up
        case TAG_TEXT_ASPECT_RATIO: if(d.size()>=4){ flushRun(); at.aspect=qBound(0.1, fix16(d,0), 4.0); } break;
        case TAG_GROUP: { Shape g; g.type=ShapeType::Group; g.name="Group"; frames.back().objs.push_back(g); } break;
        // clip-view: a group whose keyhole child/children (before the TAG_CLIPVIEW marker) clip the content
        // children (after it). Zero-length records; the split is the child count when the marker is seen.
        case TAG_CLIPVIEWCONTROLLER: { Shape g; g.type=ShapeType::Group; g.name="ClipView"; g.isClipView=true; frames.back().objs.push_back(g); } break;
        case TAG_CLIPVIEW: frames.back().clipSplit = frames.back().objs.size(); break;
        case TAG_CLIPVIEW_PATH: break;   // reserved/unused (clip recomputed from keyholes); skip
        // blend: a group of the end shapes; the in-between steps are computed at close (see closeTop).
        case TAG_BLEND: { Shape g; g.type=ShapeType::Group; g.name="Blend"; frames.back().objs.push_back(g); } break;
        case TAG_BLENDER: break;   // per-pair blender marker (leaf for non-curve blends); the pair comes from BLENDERADDITIONAL
        // ObjIndexStart/End @8/@12 index the blend's ink children, but the count is INFLATED ×2 because each
        // NodeBlender itself counts as an ink node between consecutive objects -> divide by 2 for the ink ordinal.
        // Append (not overwrite) so a chained P-object blend keeps all P-1 pairs (Watch4's 3-object spiral).
        case TAG_BLENDERADDITIONAL: if(d.size()>=16){ int a=qMax(0,rdI32(d,8)/2), b=qMax(0,rdI32(d,12)/2);
            frames.back().blendPairs.push_back(qMakePair(a,b)); } break;
        // blend profiles: Obj/Attr/Pos bias+gain (6 doubles, -1..+1). Obj reshapes step distribution,
        // Attr reshapes colour interpolation. Stored on the blend frame; applied in blendMorph at close.
        case TAG_BLENDPROFILES: if(d.size()>=32){ frames.back().blendObjBias=rdF64(d,0); frames.back().blendObjGain=rdF64(d,8);
            frames.back().blendAttrBias=rdF64(d,16); frames.back().blendAttrGain=rdF64(d,24); } break;
        // drop shadow: a container that casts a blurred offset silhouette of its children.
        case TAG_SHADOWCONTROLLER: if(d.size()>=13){
            Shape g; g.type=ShapeType::Group; g.name="Shadow"; g.shadow.on=true;
            double blur=rdI32(d,1)/MP, ox=rdI32(d,5)/MP, oy=rdI32(d,9)/MP;
            g.shadow.blur=qBound(0.0,blur,64.0); g.shadow.offset=QPointF(ox,-oy);   // XAR y-up -> screen y-down
            g.shadow.color=QColor(0,0,0,120);
            frames.back().objs.push_back(g); } break;
        case TAG_SHADOW: if(d.size()>=24 && frames.size()>=2){   // darkness DOUBLE @16 (1=opaque); colour always black
            double dk=rdF64(d,16); Frame& cur=frames.back(); Frame& par=frames[frames.size()-2];
            if(cur.ownerIndex>=0 && cur.ownerIndex<par.objs.size() && par.objs[cur.ownerIndex].shadow.on)
                par.objs[cur.ownerIndex].shadow.color.setAlpha(qBound(0,(int)qRound(dk*200),255)); } break;
        // contour: a container that draws stepped rings inset (-) or outset (+) of its children.
        case TAG_CONTOURCONTROLLER: if(d.size()>=9){
            Shape g; g.type=ShapeType::Group; g.name="Contour"; g.contour.on=true;
            g.contour.steps=qBound(1,(int)rdI32(d,0),64); g.contour.width=rdI32(d,4)/MP;
            QColor cc=frames.back().ctx.fill.representativeColor(); cc.setAlpha(150); g.contour.color=cc;
            frames.back().objs.push_back(g); } break;
        // bevel: a container that adds a lit 3-D edge to its children.
        // bevel controller: BevelType, Indent(mp), LightAngle(az deg), bOuter, Contrast(0..100), Tilt(deg).
        // (All params live in BEVEL(4052); the BEVELINK(4057) child is an empty marker — see below.)
        case TAG_BEVEL: if(d.size()>=24){
            Shape g; g.type=ShapeType::Group; g.name="Bevel"; g.bevel.on=true;
            g.bevel.type=rdI32(d,0);
            g.bevel.depth=qBound(1.0, qAbs(rdI32(d,4)/MP), 40.0);
            double la=rdI32(d,8); g.bevel.light=(qAbs(la)<=360)? la : 135.0;
            g.bevel.outer=(d.size()>=16) && rdI32(d,12)!=0;
            g.bevel.contrast=(d.size()>=20) ? qBound(0,(int)rdI32(d,16),100) : 85;
            g.bevel.tilt=(d.size()>=24) ? (double)rdI32(d,20) : 45.0;
            frames.back().objs.push_back(g); } break;
        case TAG_BEVELINK: break;   // empty marker node (NodeBevel); all params are on the BEVEL controller
        case TAG_CONTOUR: break;    // empty marker node (NodeContour, 0 bytes); all params are on CONTOURCONTROLLER(4066)
        // FEATHER (4086): soft edge. Record = FeatherSize(i32 mp) + Bias(f64) + Gain(f64) profile (Schlick,
        // [-1,+1]); the profile reshapes the blurred-edge alpha ramp (see paintFeathered).
        case TAG_FEATHER: if(d.size()>=4){ frames.back().ctx.feather=qBound(0.0, rdI32(d,0)/MP, 64.0);
            frames.back().ctx.featherBias = d.size()>=12? rdF64(d,4)  : 0.0;
            frames.back().ctx.featherGain = d.size()>=20? rdF64(d,12) : 0.0; } break;
        // FEATHER_EFFECT: the newer live-effect feather — a container node wrapping the feathered content.
        // Body: Flags(byte) + PixelsPerInch(f64) + 2 null-term UTF-16 strings (effect id/name) + FeatherSize(i32 mp)
        // + Profile Bias(f64) + Gain(f64). Route size+profile to the group's feather -> existing paintFeathered.
        case TAG_FEATHER_EFFECT: {
            Shape g; g.type=ShapeType::Group; g.name="Feather";
            int o=9;   // skip Flags(1) + PixelsPerInch(8)
            auto skipU16=[&](int& p){ while(p+2<=d.size() && (d[p]||d[p+1])) p+=2; p+=2; };
            skipU16(o); skipU16(o);   // PostProcessorID, DisplayName
            if(o+4<=d.size()) g.feather=qBound(0.0, rdI32(d,o)/MP, 64.0);
            g.featherBias = (o+12<=d.size())? rdF64(d,o+4) : 0.0;
            g.featherGain = (o+20<=d.size())? rdF64(d,o+12): 0.0;
            frames.back().objs.push_back(g); } break;
        // ---- mould (envelope / perspective distortion) ----
        case TAG_MOULD_ENVELOPE: case TAG_MOULD_PERSPECTIVE: {
            Shape g; g.type=ShapeType::Group; g.name="Mould"; frames.back().objs.push_back(g); } break;
        case TAG_MOULD_GROUP: { Shape g; g.type=ShapeType::Group; g.name="MouldContent"; frames.back().objs.push_back(g); } break;
        // MOULD_BOUNDS is the mould's OWN source rect — store it on THIS frame (Xara writes the cage BEFORE
        // the bounds, but the warp is deferred to frame close, so order is irrelevant; a document-global
        // lagged the rect by one mould, warping each component from its NEIGHBOUR's source -> exploded Watch4).
        case TAG_MOULD_BOUNDS: if(d.size()>=16){
            frames.back().mldSrc=QRectF(QPointF(fx(rdI32(d,0)),fy(rdI32(d,4))), QPointF(fx(rdI32(d,8)),fy(rdI32(d,12)))).normalized(); } break;
        case TAG_MOULD_PATH: if(d.size()>=4){    // dest cage; warp deferred to frame close (content + bounds read on the same frame)
            // Parse the cage: 4 on-curve corners + 2 cubic controls per edge (every 3rd bezier pt is on-curve).
            quint32 n=rdU32(d,0); int cbase=4+(int)n; QVector<QPointF> corners, ctrls; int bez=0;
            for(quint32 k=0;k<n && cbase+(int)k*8+8<=d.size();++k){ quint8 vb=(quint8)d[4+k]&~1u;
                QPointF p(fx(rdI32(d,cbase+(int)k*8)), fy(rdI32(d,cbase+(int)k*8+4)));
                if(vb==4){ if(++bez>=3){ corners<<p; bez=0; } else ctrls<<p; } else { corners<<p; bez=0; } }
            frames.back().mouldPending=true;
            frames.back().mldCorners=corners; frames.back().mldCtrls=ctrls; } break;
        default: break;
        }
        // remember each object's doc-space outline by sequence number, so clones can reference it
        if(isObjectTag(r.tag) && frames.back().objs.size()>objBefore){
            const Shape& ns=frames.back().objs.last();
            pathBySeq.insert(seq, ns.transform.map(ns.localPath()));
        }
    }
    while(frames.size()>1) closeTop();   // unwind any frames left open (e.g. UPs after the compressed block)
    doc.shapes = frames.front().objs;
    // A full-page, opaque, solid near-black path stacked ON TOP of existing artwork is a
    // background/registration cover (Xara keeps it at the back; in our flat z-order it would
    // otherwise blot out the drawing — see SimpleSphere [top level] and amurdove's diamond
    // [nested in a group]). Drop covers that sit above real content, at every nesting level;
    // a genuine background sits first, so it is kept.
    {
        const double pw=doc.pageSize.width(), ph=doc.pageSize.height();
        auto nearBlackPath=[&](const Shape& s)->bool{
            if(s.type!=ShapeType::Path || s.fill.type!=FillType::Solid) return false;
            const QColor& c=s.fill.color; return c.red()<40 && c.green()<40 && c.blue()<40;
        };
        auto isFullPageCover=[&](const Shape& s)->bool{
            if(!nearBlackPath(s) || s.opacity<0.99 || s.transp.type!=0 || pw<=1 || ph<=1) return false;
            QRectF b=s.docBounds(); return b.width()>=0.92*pw && b.height()>=0.92*ph;
        };
        std::function<void(QVector<Shape>&)> dropCovers=[&](QVector<Shape>& v){
            bool realContent=false; QVector<QRectF> semiDark;   // bboxes of SEMI-TRANSPARENT near-black shapes seen
            for(int i=0;i<v.size();){
                Shape& s=v[i];
                if(s.type==ShapeType::Group){ dropCovers(s.children); realContent=true; ++i; continue; }
                bool drop=false;
                if(realContent && isFullPageCover(s)) drop=true;
                // Also drop a fully-opaque near-black path that is a positional DUPLICATE of an earlier
                // SEMI-TRANSPARENT near-black twin: that pairing is a base+shade where the opaque "base" is
                // stacked ON TOP, blotting out the lit art between/under it (e.g. SimpleSphere's opaque sphere
                // body over its 42%-shaded twin + facets). Dropping the opaque one keeps the shaded twin, so
                // the sphere stays dark-but-visible. (Requiring a *semi-transparent* twin keeps this from
                // firing on the many docs that legitimately stack opaque dark shapes.)
                else if(realContent && nearBlackPath(s) && s.opacity>=0.99 && s.transp.type==0){
                    QRectF b=s.docBounds(); QPointF c=b.center(); double ba=b.width()*b.height();
                    for(const QRectF& pb : semiDark){
                        if(pb.width()*pb.height()<=1) continue;
                        double sr=ba/(pb.width()*pb.height());
                        if(QLineF(c,pb.center()).length() < 0.02*qMax(pw,ph) && sr>0.8 && sr<1.25){ drop=true; break; }
                    }
                }
                if(nearBlackPath(s) && (s.opacity<0.99 || s.transp.type!=0)) semiDark.push_back(s.docBounds());
                if(drop){ v.removeAt(i); continue; }
                if(!isFullPageCover(s)) realContent=true;
                ++i;
            }
        };
        dropCovers(doc.shapes);
    }
    // Text files embed a low-res cached bitmap of their text; with live text now rendered it's a
    // redundant (and fuzzy) duplicate. Drop a top-level bitmap node that the text mostly covers —
    // but keep a large photo (e.g. a sky) that merely has a small watermark over it.
    if(!txtShapes.isEmpty()){
        QRectF tb; for(const Shape& t: txtShapes) tb=tb.united(t.docBounds());
        for(int i=doc.shapes.size()-1;i>=0;--i){ if(doc.shapes[i].name!="Bitmap") continue;
            QRectF b=doc.shapes[i].docBounds(); double area=b.width()*b.height();
            if(area>1){ QRectF is=b.intersected(tb); if((is.width()*is.height())/area > 0.6) doc.shapes.removeAt(i); } }
    }
    for(const Shape& t: txtShapes) doc.shapes.push_back(t);   // live text on top
}

} // anonymous namespace

bool loadXar(Document& doc, const QString& path){
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false;
    QByteArray buf = f.readAll();
    if(buf.size()<8) return false;
    if(rdU32(buf,0)!=0x41524158u || rdU32(buf,4)!=0x0a0da3a3u) return false;   // magic
    QVector<Rec> recs; parseRecs(buf, 8, recs, false);
    // If this file carries our private lossless sidecar (a file WE saved), restore the full document
    // from it — round-tripping rich runs/styles/web-attrs/etc. Real Xara files never have it, so the
    // standard parse below (byte-identical render path) runs for them.
    for(const Rec& r : recs) if(r.tag==TAG_XSTUDIO_DOC && !r.data.isEmpty()){
        const QByteArray json = qUncompress(r.data);
        if(!json.isEmpty()){ QJsonParseError pe; const QJsonDocument jd = QJsonDocument::fromJson(json, &pe);
            if(pe.error==QJsonParseError::NoError && jd.isObject()){ doc = Document::fromJson(jd.object()); return true; } }
    }
    if(qEnvironmentVariableIsSet("XAR_DEBUG")){
        QHash<quint32,int> hist; for(auto& r: recs) hist[r.tag]++;
        fprintf(stderr,"[xar] %d records. top tags:", (int)recs.size());
        QList<quint32> keys=hist.keys();
        std::sort(keys.begin(),keys.end(),[&](quint32 a,quint32 b){return hist[a]>hist[b];});
        int lim = (qgetenv("XAR_DEBUG")=="2") ? keys.size() : qMin(keys.size(),14);   // =2 -> full histogram
        for(int i=0;i<lim;++i) fprintf(stderr," %u×%d", keys[i], hist[keys[i]]);
        fprintf(stderr,"\n");
    }
    Document nd; nd.pageSize = doc.pageSize;
    walk(recs, nd);
    doc = nd;
    return true;
}

// Public wrapper (declared in xar.h): a seamlessly-tiling plasma cloud with pleasant defaults,
// so the UI can apply Xara-style fractal fills without knowing the plasma parameters. Defined
// here (outside the anonymous namespace) so it has external linkage; makeFractalImage is an
// anonymous-namespace helper of this same namespace and is visible unqualified.
QImage makeFractalFill(const QColor& c0, const QColor& c1, int dim, int seed){
    return makeFractalImage(c0, c1, qBound(4,dim,512), seed, /*tileable*/true,
                            /*squash*/1.0, /*grain*/0.5, /*gravity*/0.0, /*bias*/0.0, /*gain*/0.0);
}

} // namespace xara
