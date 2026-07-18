// model.h — the vector document model for the modern Qt6 Xara.
//
// Everything the user draws is a Shape. A Shape knows its geometry (as a small set
// of typed parameters), how to produce a QPainterPath in its own local space, its
// fill / stroke / transparency / blend / live-effect attributes, and an affine
// transform placing it on the page. The Document is an ordered list of Shapes
// grouped into Layers. The model is renderer-agnostic (drawn by canvas.cpp via
// QPainter, and convertible to the libGDraw engine path or to SVG).
#pragma once

#include <QColor>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QTransform>
#include <QFont>
#include <QString>
#include <QByteArray>
#include <QVector>
#include <QPointF>
#include <QRectF>
#include <QJsonObject>
#include <QJsonArray>
#include <memory>

namespace xara {

// ----- Fill -----------------------------------------------------------------
// Mesh = 3- or 4-colour fill (a colour at each parallelogram corner; planar for 3, bilinear
// for 4). Square = diamond/square gradient (concentric L∞ contours from p0 outward).
enum class FillType { None, Solid, Linear, Radial, Conical, Bitmap, Mesh, Square };

struct GradStop {
    double  pos = 0.0;        // 0..1
    QColor  color = Qt::white;
    bool operator==(const GradStop& o) const { return pos==o.pos && color==o.color; }
};

// A named text tab stop: a position (doc px from the line origin), an alignment for the text that
// follows the \t, and an optional leader character that fills the gap up to the stop (e.g. '.' for a TOC).
struct TabStop {
    double pos = 0.0;         // x offset from the line's left origin, doc px
    int    align = 0;         // 0 left, 1 centre, 2 right, 3 decimal (align the '.' to the stop)
    QChar  leader = QChar();  // 0 = none; else the fill glyph drawn from the previous run to the stop
    bool operator==(const TabStop& o) const { return pos==o.pos && align==o.align && leader==o.leader; }
};

struct Fill {
    FillType type = FillType::Solid;
    QColor   color{70, 130, 220};            // solid colour
    int      colorRef = -1;                  // >=0: bound to Document.namedColors[colorRef] — edits to it ripple here
    QVector<GradStop> stops{ {0.0, QColor(80,150,240)}, {1.0, QColor(20,40,90)} };
    // Gradient / bitmap geometry, in the shape's LOCAL coordinate space.
    // For a bitmap fill the three points are a parallelogram: p0 = origin (image 0,0),
    // p1 = origin + X-axis (image right edge), p2 = origin + Y-axis (image bottom edge).
    QPointF  p0{0, 0}, p1{1, 0}, p2{0, 1};    // start / end / end2
    double   profileBias = 0.0, profileGain = 0.0;  // Schlick profile ([-1,+1], 0=linear) reshaping the graduation
    QImage   image;                           // bitmap texture (FillType::Bitmap)
    QImage   origImage;                        // unmodified source (photo adjustments re-derive `image` from this; not serialised)
    QImage   adjustMask;                        // Photo Mask Painter: grayscale coverage (255=adjust, 0=keep original); empty = whole image; not serialised
    bool     contone = false;                 // tint a greyscale image between color (0) and color2 (255)
    QColor   color2{Qt::white};               // contone end tint
    QVector<QColor> meshColors;               // Mesh: 3 or 4 corner colours (legacy), or rows*cols grid nodes when meshRows>=2; Square: [centre, edge]
    int      meshRows = 0, meshCols = 0;      // Mesh: >=2×2 => an arbitrary R×C grid of colour nodes (row-major in meshColors); 0 => legacy 3/4-corner
    QGradient::Spread spread = QGradient::PadSpread;   // gradient repeat: Pad / Repeat / Reflect
    bool     rainbow = false;                 // gradient cycles through hue (FILLEFFECT_RAINBOW)
    bool     rainbowAlt = false;              // ... the long way round the colour wheel
    bool     fractal = false;                 // procedural fractal/noise texture (image holds it; type stays Linear as fallback)
    bool     tiled = false;                   // BITMAPFILL attribute TILES across the whole path (Xara fill-map default Repeat=2); a placed NODE_BITMAP is one copy
    // Non-destructive photo adjustments (Gap 4): re-editable params; `image` is re-baked from `origImage`.
    struct Adjust { int brightness=0,contrast=0,saturation=0,blur=0,sharpen=0,vignette=0,cropL=0,cropR=0,cropT=0,cropB=0,lens=0,hdr=0; bool gray=false,redeye=false;
        int blackPt=0, whitePt=255; double gamma=1.0;    // input Levels: black/white clip points + midtone gamma
        int temp=0, tint=0;                               // white balance: temperature (warm+/cool-) + tint (magenta+/green-), [-100,100]
        bool levelsActive() const { return blackPt>0 || whitePt<255 || gamma!=1.0; }
        bool isIdentity() const { return !(brightness||contrast||saturation||blur||sharpen||vignette||cropL||cropR||cropT||cropB||lens||hdr||gray||redeye||temp||tint||levelsActive()); } };
    Adjust   adjust;
    bool isVisible() const { return type != FillType::None; }
    QColor representativeColor() const {
        if (type==FillType::Solid) return color;
        if (type==FillType::Bitmap) return contone ? color : QColor(150,150,150);
        if ((type==FillType::Mesh || type==FillType::Square) && !meshColors.isEmpty()) return meshColors.first();
        return stops.isEmpty()? color : stops.first().color;
    }
};

// ----- Stroke ---------------------------------------------------------------
struct Stroke {
    bool    enabled = true;
    QColor  color = QColor(25, 28, 34);
    int     colorRef = -1;                    // >=0: line colour bound to Document.namedColors[colorRef]
    double  width = 1.5;                      // document units
    Qt::PenCapStyle  cap  = Qt::FlatCap;   // Xara default LineCapButt (Kernel/attr.cpp AttrStartCap)
    Qt::PenJoinStyle join = Qt::RoundJoin;
    QVector<qreal>   dashes;                  // empty = solid
    double  transp = 0.0;                     // stroke transparency 0..1 (0 = opaque)
    int     startArrow = 0;                   // arrowhead at path start: 0=none, negative=stock id (-2..-9), positive=custom
    int     endArrow = 0;                     // arrowhead at path end (same id meaning)
    double  startArrowW = 1.0, startArrowH = 1.0;  // start-arrow scale (FIXED16 factors)
    double  endArrowW = 1.0, endArrowH = 1.0;      // end-arrow scale (heads and tails can differ)
    QImage  brushInk;                         // art/textured brush: ink stamped along the path (null = plain stroke)
    double  brushSpacing = 0.6;               // stamp spacing: fraction of ink width (brushScale==0) or absolute doc units (brushScale>0)
    bool    brushTint = false;                // recolour the ink with the stroke colour (alpha-preserving)
    double  brushScale = 0.0;                 // >0: explicit ink px->doc scale (real Xara nib at natural size); 0: auto-fit ink height to stroke width
    // Edit-Brush attributes (Xara's scatter/rotation controls). All default to the plain rotate-to-tangent stamp.
    bool    brushRotateToLine = true;         // stamp rotated to the path tangent (false = fixed upright)
    double  brushRotate = 0.0;                // extra rotation added to every stamp (deg)
    double  brushScatter = 0.0;               // random per-stamp position jitter radius (doc px)
    double  brushSizeVar = 0.0;               // random per-stamp size variation (0..1 of the base size)
    bool    varWidth = false;                 // variable-width stroke (STROKETYPE 0x02 / a width profile present)
    bool    calligraphic = false;             // calligraphic (flat/angled nib): stroke width follows the nib angle vs. the path direction
    double  calligAngle = 30.0;               // nib angle (deg); thick perpendicular to it, thin parallel
    QVector<QPointF> widthProfile;            // control points (x=arc-length pos 0..1 ascending, y=width multiplier of `width`)
    // Width multiplier at arc-length fraction t (linear interp between control points; clamps at the ends).
    // Mirrors Kernel ValueFunctionPressure::GetValue. Empty profile -> 1.0 (constant).
    double  widthMultAt(double t) const {
        if (widthProfile.isEmpty()) return 1.0;
        if (t <= widthProfile.front().x()) return widthProfile.front().y();
        if (t >= widthProfile.back().x())  return widthProfile.back().y();
        for (int i = 1; i < widthProfile.size(); ++i) {
            const QPointF& a = widthProfile[i-1]; const QPointF& b = widthProfile[i];
            if (t <= b.x()) { double u = (b.x() > a.x()) ? (t-a.x())/(b.x()-a.x()) : 0.0;
                              return a.y() + u*(b.y()-a.y()); }
        }
        return widthProfile.back().y();
    }
    // vector / stroke-shape stroke (STROKETYPE handle 0x00 + STROKEDEFINITION 4003): a vector shape drawn
    // along the path. Stored pre-flattened as (path, fill) pairs in definition space (avoids Stroke<->Shape
    // recursion). Repeated (strokeRepeating) or stretched once; overrideFill -> recolour to the line colour.
    QVector<QPainterPath> strokeShapePaths;
    QVector<QColor>       strokeShapeCols;
    bool strokeRepeating = false;
    int  strokeRepeats = 0;                   // 0 = optimal (compute count from path length)
    bool strokeOverrideFill = false;          // true = discard the shape's own fills, use the line colour
};

// ----- Transparency (rich attribute read from XAR) --------------------------
// Separate from the fill: a transparency type (blend mode) + its own geometry.
// One intermediate stop of a multi-stage graduated transparency (position along the axis + its
// transparency 0..1). Empty stops => the classic 2-point t0..t1 ramp (kept byte-identical for XAR).
struct TranspStop { double pos = 0.5; double t = 0.5; };

struct Transparency {
    int     type = 0;          // 0=none/opaque, 1=mix, 2=stainedglass, 3=bleach, 4=contrast,
                               // 6=darken, 7=lighten, 8=brightness, ... (Xara transparency type)
    int     geom = 0;          // 0=flat, 1=linear, 2=radial, 3=fractal/noise mask, 4=bitmap mask
    double  t0 = 0, t1 = 0;    // start/end transparency 0..1 (0=opaque, 1=fully transparent)
    QVector<TranspStop> stops; // multi-stage: intermediate transparency stops (empty => plain t0..t1 ramp)
    QPointF p0{0,0}, p1{1,0};  // gradient geometry, local space (p0=centre, p1=axis-1 endpoint)
    QPointF p2{0,1};           // axis-2 endpoint (elliptical/anisotropic radial; geom-4 parallelogram Y corner)
    bool    elliptical = false;// geom-2 radial is an ELLIPSE (independent p1,p2 axes), not a circle
    QImage  maskBmp;           // geom-4: greyscale bitmap whose luminance modulates alpha (BITMAPTRANSPARENTFILL)
    double  profileBias = 0.0, profileGain = 0.0;  // Schlick profile ([-1,+1], 0=linear) reshaping the fade (non-linear drag-out)
    bool isGraduated() const { return geom != 0; }
};

// ----- Live drop shadow -----------------------------------------------------
struct Shadow {
    bool    on = false;
    QPointF offset{6, 8};
    double  blur = 12.0;
    QColor  color = QColor(0, 0, 0, 120);
    int     type = 0;      // 0 wall (offset silhouette), 1 floor (raked ground shadow), 2 glow (symmetric halo)
    double  floorShear = 0.7;   // FLOOR (type 1): horizontal lean of the raked ground shadow (the rake direction)
    double  floorScale = 0.5;   // FLOOR (type 1): vertical foreshortening — the shadow's length
};

// ----- Live contour (inner/outer outline) -----------------------------------
struct Contour {
    bool   on = false;
    double width = 8.0;     // >0 outer, <0 inner
    int    steps = 1;
    QColor color = QColor(0,0,0,90);
    QColor color2;          // INVALID by default → classic single-colour fade; valid → graduate inner(color)→outer(color2) across steps
    double transp = 0.0;    // independent contour transparency (0 = opaque as-is); multiplies every ring's alpha
};

// ----- Live bevel (lit 3-D edge) --------------------------------------------
struct Bevel {
    bool   on = false;
    double depth = 7.0;     // Indent (edge thickness), doc px
    double light = 45.0;    // LightAngle azimuth (deg)
    int    type = 0;        // eBevelStyle 0..14 (0=Flat, 1=Round, 2=HalfRound, 3=Frame, 6/7=Smooth, 8..10=Point, 11..14=Ruffle)
    bool   outer = false;   // bOuter: false=inner (carved), true=outer (raised rim)
    int    contrast = 85;   // 0..100 light/shadow strength (highlight linear, shadow quadratic; default 85)
    double tilt = 45.0;     // light tilt (deg); elevation = 90 - tilt
    QColor lightColor = QColor(255,255,255);  // bevel LIGHT colour: tints the highlight (white = neutral); shadow stays dark
};

// 3-D extrude: project the shape back along a direction, drawing shaded side walls.
struct Extrude {
    bool   on = false;
    double depth = 28.0;    // extrusion length, doc px
    double angle = 35.0;    // direction of the extrusion (deg; 0 = +x right, measured CCW with screen y-down)
    int    light = 135;     // light azimuth (deg) for shading the side walls
    QColor color;           // side/back-wall base colour; INVALID (default) => derive from the object's own fill (byte-identical)
    double rotX = 0.0;      // true-3D tilt about the horizontal axis (deg); 0 => classic angle-based projection
    double rotY = 0.0;      // true-3D tilt about the vertical axis (deg)
    double bevel = 0.0;     // chamfered rim between the front face and the side walls (doc px); 0 => square edge
    bool   is3D() const { return rotX!=0.0 || rotY!=0.0; }   // non-zero tilt => orthographic 3-D solid render
};

enum class ShapeType { Rect, Ellipse, Polygon, Path, Text, Group, Smart, Table };

// A run of characters within a Text shape carrying its own font, colour and
// (optionally) hyperlink — the substrate for rich, mixed-format text. When a
// shape's `textRuns` is empty the whole story uses the shape's base font/fill
// (the classic path, kept byte-identical for XAR-loaded text). Runs are stored
// contiguous & sorted; layout clamps them to the current text length so edits
// never corrupt rendering. `url` seeds per-character hyperlinks (T4).
struct TextRun {
    int     start = 0;                 // first character index (into Shape::text)
    int     len   = 0;                 // number of characters
    QFont   font{QStringLiteral("DejaVu Sans"), 36};
    QColor  color = Qt::black;
    QString url;                       // per-run hyperlink (empty = none)
    double  baseline = 0.0;            // baseline shift, doc px (+up = superscript, −down = subscript)
    double  tracking = 0.0;            // per-run letter-spacing override, doc px (0 = inherit shape tracking)
};

// ----- Table (Table tool) ---------------------------------------------------
// A grid of cells laid out in the shape's LOCAL space with origin (0,0); the Shape's
// transform positions it. Column widths + row heights are explicit so rows/cols can be
// resized independently. Cells store plain text (rich per-cell runs come later) plus a
// background, text colour, alignment and merge spans.
struct TableCell {
    QString text;                       // cell content
    QColor  bg = QColor(0,0,0,0);       // cell background (alpha 0 = none)
    QColor  textColor = QColor(30,34,40);
    int     align = 0;                  // 0 left, 1 centre, 2 right
    int     valign = 0;                 // vertical: 0 middle (default), 1 top, 2 bottom
    bool    bold = false;               // per-cell bold
    int     rowSpan = 1, colSpan = 1;   // >1 = merged-region anchor; 0 = covered by a merge above/left
};
struct Table {
    QVector<double> colW;               // column widths (doc units)
    QVector<double> rowH;               // row heights (doc units)
    QVector<TableCell> cells;           // rows*cols, row-major
    QColor  borderColor = QColor(70,74,82);
    bool    headerRow = false;           // first row rendered bold with a shaded fill
    double  borderW = 1.0;
    double  pad = 5.0;                   // inset of cell text from its edges
    QColor  bg = QColor(0,0,0,0);        // table background (shows through cell spacing; alpha 0 = none)
    double  spacing = 0.0;               // gap between cells
    int rows() const { return rowH.size(); }
    int cols() const { return colW.size(); }
    double width()  const { double w=0; for(double c:colW) w+=c; return w; }
    double height() const { double h=0; for(double r:rowH) h+=r; return h; }
    bool valid() const { return rows()>0 && cols()>0 && cells.size()==rows()*cols(); }
    TableCell& at(int r,int c){ return cells[r*cols()+c]; }
    const TableCell& at(int r,int c) const { return cells[r*cols()+c]; }
    double colX(int c) const { double x=0; for(int i=0;i<c && i<cols();++i) x+=colW[i]; return x; }
    double rowY(int r) const { double y=0; for(int i=0;i<r && i<rows();++i) y+=rowH[i]; return y; }
    static Table grid(int r, int c, double w, double h);   // uniform r×c grid spanning w×h
};

// ----- Shape ----------------------------------------------------------------
struct Shape {
    ShapeType type = ShapeType::Rect;
    QString   name;
    QString   url;                            // web hyperlink (empty = none) — used by HTML export
    QString   webEmbed;                       // raw embed HTML (iframe/video/map/form) placed here on web publish (empty = none)
    QTransform transform;                     // local -> document
    Fill      fill;
    Stroke    stroke;
    double    opacity = 1.0;                  // 0..1 object transparency (UI tool)
    QPainter::CompositionMode blend = QPainter::CompositionMode_SourceOver;
    Transparency transp;                      // rich transparency (type/geometry) from XAR
    Shadow    shadow;
    Contour   contour;
    Bevel     bevel;
    Extrude   extrude;
    Table     table;                          // grid data when type==ShapeType::Table
    double    feather = 0.0;                   // feathered (soft) edge radius, doc units (0 = hard)
    double    blur = 0.0;                       // live Gaussian blur radius, doc units (0 = sharp); whole-object soft-focus
    double    featherBias = 0.0, featherGain = 0.0;  // Schlick profile ([-1,+1]) shaping the feather falloff
    Qt::FillRule fillRule = Qt::OddEvenFill;   // path fill rule (Xara default = even-odd)
    bool      visible = true;
    bool      locked = false;
    bool      overprint = false;   // prepress: this object OVERPRINTS (adds its ink) instead of knocking out the plates below
    int       layer = 0;
    int       linkId = 0;      // live copies: members of a linkId share appearance (lowest index = master)
    int       softGroup = 0;   // soft group: members select together without nesting
    // Web-publish layout hints (Module 2) — ignored by the canvas renderer; consumed by web export.
    int       webStretch = 0;  // 0 fixed, 1 stretch width (100% of container), 2 stretch width+height
    int       webPin = 0;      // pin edges bitmask: 1 left, 2 right, 4 top, 8 bottom (both L+R => horizontal stretch)
    bool      webSticky = false;// stays fixed on screen while the page scrolls (position:sticky)
    bool      webLightbox = false;// photo pop-up: clicking the published image opens it full-screen in a lightbox overlay
    bool      webNewWindow = false;// hyperlink opens in a new browser tab (target="_blank")
    QString   webBuyUrl;         // e-commerce: non-empty => publish a "Buy Now" button linking to this checkout URL
    int       webHeading = 0;    // SEO: 0=normal text, 1/2/3 => publish this text object inside an <h1>/<h2>/<h3>
    int       webAnim = 0;     // web reveal animation on scroll-into-view: 0 none,1 fade,2 slide-up,3 slide-down,4 slide-left,5 slide-right,6 zoom,7 parallax
    double    webAnimDelay = 0.0;// animation start delay in seconds
    int       webSlider = 0;   // slider/carousel group id: shapes sharing a non-zero id are cross-faded slides of one carousel on web publish
    QByteArray webMediaData;   // embedded media bytes (video/audio/animated GIF); rendered as an HTML player on web publish
    QString    webMediaMime;   // media MIME type ("video/mp4","audio/mpeg","image/gif"…); non-empty => this shape is a media placeholder
    int        webBuild = 0;     // presentation build order WITHIN a layer: objects reveal in (layer, webBuild) order (0 = with the layer)
    int        webPopupId = 0;      // >0: this object is a pop-up PANEL (hidden until a trigger with the same id is hovered)
    int        webPopupTrigger = 0; // >0: hovering this object shows the pop-up panel whose webPopupId matches
    bool       webNavBar = false;// this shape is a navigation bar: on publish it becomes a styled <nav> linking all site pages
    bool       webBand = false;  // full-browser-width background band: on publish this shape becomes a 100%-viewport-wide stripe BEHIND the fixed content column
    int        webHideBelow = 0; // responsive: hide this element when the viewport is narrower than this many px (0 = always show)
    int        webFlex = 0;      // FlexGroup id: shapes sharing a non-zero id publish as a FLUID flex-wrap row (reflows on mobile), below the fixed stage
    // Website Variant: an alternate position/size applied below a breakpoint width (per-breakpoint override).
    int        webVarBelow = 0;  // viewport width px below which the override applies (0 = no variant)
    double     webVarX = 0, webVarY = 0, webVarW = 0; // override left/top (absolute doc coords) + width, px
    QString    webMenuTree;      // nav bar dropdown menu: indentation-nested "Label | href" lines → multi-level <ul> menu on publish
    QString    webForm;          // contact form: newline "Label|type" lines (type text/email/tel/number/textarea) → published <form>; empty = not a form (action = this shape's url)

    // --- geometry (interpreted per type, all in LOCAL space) ---
    QRectF rect{0, 0, 100, 70};               // Rect / Ellipse bounds; Text box
    double cornerRadius = 0.0;                // rounded Rect
    int    polySides = 5;                     // Polygon
    bool   polyStar = false;                  // Polygon -> star
    double polyStarDepth = 0.5;               // star inner radius fraction
    double polyCurvature = 0.0;               // Polygon/star: bend the straight edges into curves (0 = straight; ±1 = strong bow) — flower/gear
    // Ellipse -> Pie / Arc / Segment: a swept ellipse. Default whole (mode 0, sweep 360) renders byte-identically.
    double ellipseStart = 0.0;                // start angle (deg, Qt convention: 0 = +x/3-o'clock, CCW positive)
    double ellipseSweep = 360.0;              // swept angle (deg); >=360 = whole ellipse
    int    ellipseMode = 0;                   // 0 whole, 1 pie (wedge to centre), 2 arc (open curve), 3 segment (chord)
    int    smartKind = 0;                      // Smart: 0 chevron,1 gear,2 cog,3 banner,4 arrow,5 star,6 callout
    double smartParam = 6;                     // Smart: live parameter (count/teeth/points) — editable via handle 1
    double smartParam2 = -1;                   // Smart: 2nd parameter (depth/thickness/inner-radius), 0..1; <0 = kind default (byte-identical)
    double blendProfile = 0.0;                 // live blend: Schlick POSITION ease across steps ([-1,+1], 0 = linear)
    double blendAttrProfile = 0.0;             // live blend: separate ease for colour/thickness interpolation ([-1,+1], 0 = linear)
    double blendNodeShift = 0.0;               // live blend: rotate node correspondence ([0,1) of the perimeter)
    QPainterPath blendSpine;                   // live blend: if set, steps are distributed along this spine (blend-along-curve)
    static QPainterPath smartPath(int kind, double param, double param2=-1);   // procedural 0..100 box path (parametric SmartShape)
    static double smartParam2Default(int kind);              // normalised 0..1 value equal to that kind's hardcoded default
    QPainterPath path;                        // Path (freehand/pen/line/boolean result)
    QPainterPath regionPath;                  // Photo Region (local space): non-empty clips this object to the region
    double    regionFeather = 0.0;            // soft region edge (doc px); 0 = hard clip
    QString text = QStringLiteral("Text");    // Text content
    QFont   font{QStringLiteral("DejaVu Sans"), 36};
    QString textStyleName;                     // non-empty => member of a named TextStyle (cascade); attributes are copied in
    QVector<TextRun> textRuns;                 // per-character rich runs (empty => whole story uses base font/fill — classic path)
    bool    textFrame = false;                 // word-wrap within rect.width() (a text area, not point text)
    QPainterPath textShapePath;                // FRAME: non-empty => flow text inside this arbitrary closed shape (local space), not rect
    int     textCols = 1;                      // columns within the frame
    double  textColGap = 20;                   // gap between columns
    int     textAlign = 0;                     // 0 left, 1 centre, 2 right, 3 justified
    double  firstLineIndent = 0.0;             // extra left offset on the first line of each paragraph (doc px); 0 = none
    double  leftIndent = 0.0;                  // left margin applied to every line (doc px); 0 = none
    double  rightIndent = 0.0;                 // right margin (narrows the wrap/align width, doc px); 0 = none
    double  tabWidth = 0.0;                    // regular tab-stop interval (doc px); >0 => \t advances to the next stop; 0 = off (byte-identical)
    QVector<TabStop> tabStops;                 // explicit tab stops (per-stop align + leader); non-empty overrides tabWidth for \t handling
    double  spaceBefore = 0.0;                  // extra vertical space before each paragraph (doc px); 0 = none (single-column frames)
    double  spaceAfter = 0.0;                   // extra vertical space after each paragraph (doc px); 0 = none
    double  lineSpacing = 1.18;                 // line-height multiple of the font height (leading); 1.18 = default
    double  tracking = 0.0;                     // extra letter-spacing (doc px); 0 = normal
    double  textAspect = 1.0;                   // horizontal glyph scale (1.0 = normal; 0.5 = condensed, 2.0 = expanded)
    bool    smallCaps = false;                 // render lowercase letters as smaller capitals
    QStringList otFeatures;                    // OpenType feature tags (liga/smcp/onum/tnum/frac…); empty => classic layout
    int     listStyle = 0;                     // 0 none, 1 bullet •, 2 numbered "1.", 3 lettered "a." — each paragraph is a list item (text frames)
    double  listIndent = 0.0;                  // hanging indent for list items (doc px); 0 with listStyle>0 => default gutter
    int     listStart = 1;                     // first item's number/letter for a numbered/lettered list (restart-at-N); 1 = default
    int     dropCap = 0;                       // >1: enlarge first letter to span N lines (text frames)
    bool    hyphenate = false;                  // text frame: break overflowing words at syllable points
    int     nextFrame = -1;                      // linked text: index (in Document.shapes) of the frame this story overflows INTO (-1 = none / chain tail)
    mutable int overflowStart = -1;              // TRANSIENT (not serialised): during frame layout, first char index that overflowed the frame; -1 = all fit
    bool    repel = false;                       // this object repels overlapping text frames (text flows around it)
    QVector<QRectF> textWrapObstacles;          // text frame: LOCAL-space rects of repelling objects (derived; empty => no wrap)
    int     anchorTextIndex = -1;                // GRAPHIC: index (in Document.shapes) of the text frame it flows inline with (-1 none)
    int     anchorChar = 0;                      // GRAPHIC: character position of the inline anchor within that frame
    QVector<QPair<int,QSizeF>> textInlineBoxes;  // FRAME: derived inline-graphic reservations (charPos → box size); empty => no inline gaps
    QPainterPath textPath;                     // Text: non-empty => glyphs flow along this baseline curve (local space)
    double  textPathOffset = 0;                // Text-on-curve: start distance (doc units) along the path; 0 = at the path start
    bool    textPathInside = false;            // Text-on-curve: flip glyphs to the reverse side (underside) of the path — reads upright on a bottom arc
    QVector<Shape> children;                  // Group
    bool   isClipView = false;                // ClipView group: keyhole children clip the content children
    int    blendSteps = 0;                     // LIVE blend group: >0 and exactly 2 children (the ends) => render regenerates this many morphed in-betweens
    int    clipSplit = 0;                     // ClipView: children[0..clipSplit) = keyhole(s); [clipSplit..) = clipped content

    // Build the geometry in local space.
    QPainterPath localPath() const;
    // Rich text: resolve per-character font/colour/url (length == text.size()),
    // filling from textRuns and defaulting uncovered chars to base font/fill.
    void resolveChars(QVector<QFont>& fonts, QVector<QColor>& colors, QVector<QString>& urls) const;
    QVector<double> resolveBaselines() const;   // per-character baseline shift (doc px, +up) from the runs
    // Rich text layout: per-run glyph outlines (local space) tagged with colour.
    // Only meaningful when textRuns is non-empty; mirrors the point/frame layout.
    QVector<QPair<QPainterPath,QColor>> layoutRuns() const;
    // Rich text hyperlinks: local-space bounding rect of each contiguous same-URL span (T4).
    QVector<QPair<QRectF,QString>> runLinkRects() const;
    // Inline anchored graphics (T6): lay out a frame reserving a gap for each textInlineBoxes
    // entry. Returns the text glyph path; if outBoxes != null, fills it with each reserved box's
    // placed LOCAL-space rect (same order as textInlineBoxes sorted by char position).
    QPainterPath layoutFrameInline(QVector<QRectF>* outBoxes = nullptr) const;
    // In-canvas text editing (point text): map a caret character index to its LOCAL baseline point,
    // and a LOCAL point back to the nearest caret index. Line height / ascent size the caret glyph.
    // Exact for plain point text AND for simple word-wrapped frames (via frameVisualLines); other
    // shapes (text-on-curve, drop-cap/list/repel frames) are approximate — the editor gates them out.
    QPointF textCaretLocal(int idx) const;
    int     textIndexAtLocal(const QPointF& localPt) const;
    double  textLineH() const;
    double  textAscent() const;
    // One placed visual line of a wrapped text frame (caret mapping). `startChar` is the offset into
    // `text` of the line's first character; x0/baseY are the line's local origin/baseline; `width` is
    // the wrap width (for alignment); `colBase` is the column's left edge. Parallel to localPath's
    // frame layout (kept in sync by tests) so the render path stays byte-identical.
    // A placed visual line. `str` is what's drawn; for a hyphenated line it ends in a synthetic '-'
    // that is NOT in the source text, so `realLen` (< str.size()) gives the count of real characters the
    // line consumes. realLen<0 means "no synthetic char" — the whole str maps 1:1 to the text.
    struct TextVisualLine { int startChar; QString str; double x0, baseY, width, colBase; int realLen = -1;
        int realCount() const { return realLen<0 ? int(str.size()) : realLen; } };
    QVector<TextVisualLine> frameVisualLines() const;
    QRectF dropCapRectLocal() const;   // block occupied by the drop cap (local space); null if no drop cap
    // Local bounding rect of the geometry (no stroke).
    QRectF localBounds() const;
    // Document-space bounding rect (geometry only, incl. transform & children).
    QRectF docBounds() const;
    // Hit test a document-space point (within tolerance, honours fill/stroke).
    bool hitTest(const QPointF& docPt, double tol) const;

    QJsonObject toJson() const;
    static Shape fromJson(const QJsonObject& o);
};

// ----- Layer ----------------------------------------------------------------
struct Layer {
    QString name = QStringLiteral("Layer 1");
    bool    visible = true;
    bool    locked  = false;
    bool    master  = false;   // a "master page" layer: its objects are drawn on EVERY page
    bool    printable = true;  // false => visible on screen but excluded from print/export (guide/annotation layer)
    int     frameDelayMs = 0;  // animation: per-frame display time (GIF export); 0 = use the export default
};

// Per-page web-publish properties (manual: page name / SEO). All optional; empty => the page falls back
// to an auto filename (base_pN.html) and the document-level SEO. Indexed by page number.
struct PageProps {
    QString name;         // custom HTML filename base for this page (e.g. "about" → about.html); empty => auto
    QString title;        // per-page <title> (empty => document webTitle)
    QString description;  // per-page meta description (empty => document webDescription)
    QString keywords;     // per-page meta keywords (empty => document webKeywords)
    bool empty() const { return name.isEmpty() && title.isEmpty() && description.isEmpty() && keywords.isEmpty(); }
};

// A document-level named/theme colour (palette entry). kind: 0=normal, 1=tint, 2=shade, 3=linked.
// Tint/shade/linked colours derive from a parent (index into namedColors) so editing the parent
// ripples through — Xara's "linked colours" model (manual p203).
struct NamedColor {
    QString name;
    QColor  color = Qt::black;
    int     kind = 0;
    int     parent = -1;
    double  amount = 0.5;          // tint/shade strength 0..1
    bool    theme = false;         // part of the document's theme scheme
    bool    spot = false;          // a spot/Pantone ink — gets its own separation plate (not CMYK)
};

// A document-level named TEXT STYLE (manual p146). Editing a style ripples to every text object using
// it (cascade): styles are applied by copying attributes into the shape, and updateTextStyle re-syncs
// all members — so the render path never has to resolve a style (keeps text render byte-identical).
struct TextStyle {
    QString name;
    QFont   font{QStringLiteral("DejaVu Sans"), 36};
    QColor  color = Qt::black;
    int     align = 0;                     // 0 left, 1 centre, 2 right, 3 justified
};

// ----- Document -------------------------------------------------------------
class Document {
public:
    QVector<Shape>  shapes;        // back-to-front draw order
    QVector<Layer>  layers{ Layer{} };
    QSizeF          pageSize{1123, 794};   // A4 landscape-ish @96dpi feel
    QColor          pageColor = Qt::white;
    int             pageCount = 1;         // pages stacked vertically on the pasteboard (Xara-style)
    double          pageGap = 50;          // gap between stacked pages, doc px
    QPointF         rulerOrigin{0,0};      // doc-space point shown as (0,0) on the rulers (0,0 = default; view-only)
    bool            scaleLineWidths = true; // Xara default: resizing an object also scales its stroke width (& shadow)
    QVector<NamedColor> namedColors;       // document palette (named/theme/tint/shade/linked)
    QVector<TextStyle>  textStyles;        // document named text styles (cascading)
    QVector<int>        breakpoints;       // responsive web breakpoint widths (px), ascending (empty => single layout)
    QString webTitle, webDescription, webKeywords;   // SEO metadata emitted into <title>/<meta> on web publish
    QString webBaseUrl, webAuthor;                   // canonical/og:url base + author (extended SEO + sitemap.xml)
    QVector<PageProps> pageProps;                    // per-page web properties (custom filename + SEO); index = page number
    QString webPassword;                             // non-empty => published site is gated behind a client-side password prompt
    QImage  favicon;                                 // site favicon (written as favicon.png + <link rel=icon> on publish)
    bool    fluidSite = false;              // web publish: whole page scales fluidly to the viewport width (continuous), not a stepped fixed stage
    bool    presentSteps = false;           // web publish: reveal content layer-by-layer as presentation steps (click / arrow keys advance)
    QString webHeadHtml, webBodyHtml;       // site-owner raw HTML injected before </head> / before </body> (analytics, verification, cookie scripts)

    QRectF pageRect(int i) const { double adv=pageSize.height()+pageGap; return QRectF(0, i*adv, pageSize.width(), pageSize.height()); }
    QRectF allPagesRect() const { return QRectF(0,0, pageSize.width(), pageCount*pageSize.height()+(pageCount-1)*pageGap); }

    // Resolve a named colour's effective RGBA, following tint/shade/linked parents.
    QColor resolveNamed(int idx, int depth=0) const;
    // Re-apply every object's bound named colour (Fill/Stroke.colorRef) — call after editing the palette so
    // edits ripple to the artwork (Xara's "linked colours"). No-op for the common case of no bindings.
    void resyncNamedColorBindings();
    // After deleting namedColors[d], fix every index that referred to the palette: Fill/Stroke.colorRef and
    // tint/shade parent chains > d shift down by one; those == d unbind (ref→-1) / detach (parent→-1, kind→0),
    // keeping their already-baked colour. Prevents bound objects silently rebinding to the wrong palette entry.
    void reindexNamedColorsAfterDelete(int d);

    int  addShape(const Shape& s) { shapes.push_back(s); return shapes.size()-1; }
    QRectF contentBounds() const;
    bool layerVisible(int i) const { return i<0 || i>=layers.size() || layers[i].visible; }
    bool layerLocked(int i)  const { return i>=0 && i<layers.size() && layers[i].locked; }

    QJsonObject toJson() const;
    static Document fromJson(const QJsonObject& o);

    // Build the geometry of a shape (and its effects) ready to paint, returning the
    // path already in document space.
    static QPainterPath docPath(const Shape& s);
};

// Produce the `steps` interpolated in-between shapes morphing outline A into outline B
// (doc-space Path shapes). Shared by the Blend tool and the XAR blend reader.
QVector<Shape> blendMorph(const Shape& a, const Shape& b, int steps,
                          double objBias=0, double objGain=0, double attrBias=0, double attrGain=0,
                          double nodeShift=0.0);   // rotate the end-shape's node correspondence by this fraction (0..1) of the perimeter

// Helpers shared with the UI.
QColor lerp(const QColor& a, const QColor& b, double t);
QString blendModeName(QPainter::CompositionMode m);
QVector<QPair<QString, QPainter::CompositionMode>> blendModes();

} // namespace xara
