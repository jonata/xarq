// canvas.h — the interactive drawing surface. Renders the Document with a
// pan/zoom view transform on a dark workspace with a white artboard, handles all
// tool interaction (every Xara tool), selection with resize/rotate handles,
// undo/redo, and emits signals so the side panels stay in sync.
#pragma once
#include <QWidget>
#include <QImage>
#include <QSet>
#include <functional>
#include "model.h"
#include "pathedit.h"

namespace xara {

enum class Tool {
    Select, Node, Rect, Ellipse, Polygon, Pen, Line, Freehand, Text,
    Fill, Transparency, Shadow, Contour, Blend, Mould, Zoom, Pan,
    Bevel, Feather, Eraser, Extrude, Clone, Table, Region
};

class Canvas : public QWidget {
    Q_OBJECT
public:
    explicit Canvas(QWidget* parent=nullptr);

    Document&       doc()       { return m_doc; }
    const Document& doc() const { return m_doc; }
    void setDocument(const Document& d, const QString& path=QString());

    // One undo/redo level = a copy-on-write snapshot of the document + selection. Storing the live
    // Document (Qt's implicitly-shared containers) instead of a JSON blob makes pushUndo O(1) at the
    // push (a ref bump); the O(N) detach happens lazily on the next mutation. This matters on heavy
    // imports (a 9k-object .cdr): JSON-serialising the whole doc on every drag-start froze the UI for
    // hundreds of ms. The stack is in-memory only (never written to disk), so a binary snapshot is fine.
    struct UndoSnap { Document doc; QVector<int> sel; };

    // Multi-document tabs: the full per-document editing state, so MainWindow can stash an
    // inactive tab and restore it later into this one shared Canvas. Transient interaction
    // state (crop/clone/mask/pen/node/mould drags) is NOT part of a Session — it resets on load.
    struct Session {
        Document doc;
        QString  path;
        QVector<int> sel;
        QVector<UndoSnap> undo, redo;
        int    activeLayer = 0;
        double zoom = 1.0;
        QPointF origin{60, 60};
        QVector<double> guidesH, guidesV;   // persistent user ruler guides (per-document, not in the XAR model)
    };
    Session captureSession() const;          // snapshot the current editing state
    void    loadSession(const Session& s);   // make `s` the live document (resets transient drags)

    Tool tool() const { return m_tool; }
    void setTool(Tool t);

    Fill   currentFill()   const { return m_fill; }
    Stroke currentStroke() const { return m_stroke; }
    void   setCurrentFill(const Fill& f);
    void   setCurrentStroke(const Stroke& s);
    int    polySides() const { return m_polySides; }
    void   setPolySides(int n){ m_polySides=n; }
    bool   polyStar() const { return m_polyStar; }
    void   setPolyStar(bool s){ m_polyStar=s; }
    // Text tool's current attributes: used for NEW text (so a font chosen with no selection actually sticks).
    QFont  textFont() const { return m_textFont; }
    void   setTextFont(const QFont& f){ m_textFont=f; }
    // Gradient/transparency editing-handle colour (on-canvas overlay only; user-customisable, QSettings-backed).
    QColor gradHandleColor() const { return m_gradHandleColor; }
    void   setGradHandleColor(const QColor& c);
    int    tableRows() const { return m_tableRows; }
    void   setTableRows(int n){ m_tableRows=qMax(1,n); }
    int    tableCols() const { return m_tableCols; }
    void   setTableCols(int n){ m_tableCols=qMax(1,n); }
    // Table cell interaction (T-2/T-3) — public so the selftest can drive it deterministically.
    int    tableEditShape() const { return m_tableShape; }             // index of the table under cell-edit, or -1
    bool   tableCellAt(int shapeIdx, const QPointF& docPt, int& r, int& c) const;  // doc→cell hit-test
    void   tableSelectCell(int shapeIdx, int r, int c, bool extend=false);
    QRect  tableSelRange() const;   // normalized selected cell range (x=col,y=row,w=cols,h=rows); null if none
    void   tableSetCellText(int r, int c, const QString& t);
    void   tableClearCells();                                          // clear text of the selected cells
    void   tableSetCellBg(const QColor& c, bool clear=false);
    void   tableSetCellAlign(int align);
    void   tableSetCellVAlign(int valign);   // 0 middle, 1 top, 2 bottom
    void   tableSetCellBold(bool bold);
    void   tableSetHeaderRow(bool on);       // first row bold + shaded
    bool   tableNavigate(int dr, int dc);                             // move selection; Tab past last cell appends a row
    void   tableInsertRow(int at); void tableDeleteRow(int at);
    void   tableInsertCol(int at); void tableDeleteCol(int at);
    bool   tableMergeCells(); void tableSplitCells();                  // merge selection into one / unmerge
    void   tableSetAppearance(double borderW, const QColor& borderCol, double pad, double spacing);  // table-wide look
    void   tableSetBg(const QColor& c, bool clear);                   // table background (behind cells)
    void   tableDistribute(bool rows);                                // even out row heights / column widths
    bool   importCsvAsTable(const QString& path);                     // build a Table shape from a CSV file
    int    addTableFromRows(const QVector<QStringList>& rows);        // build/insert a Table from parsed cells; returns shape index
    double eraserNib() const { return m_eraserNib; }
    void   setEraserNib(double r){ m_eraserNib=qMax(1.0,r); }
    bool   eraserSoft() const { return m_eraserSoft; }
    void   setEraserSoft(bool on){ m_eraserSoft=on; }
    void   softEraseAt(int si, const QPointF& docPt);   // paint a feathered alpha hole into a photo (soft eraser)
    // Clone/Heal brush (photo retouch)
    double cloneNib() const { return m_cloneNib; }
    void   setCloneNib(double r){ m_cloneNib=qMax(1.0,r); }
    double cloneHardness() const { return m_cloneHardness; }
    void   setCloneHardness(double h){ m_cloneHardness=qBound(0.0,h,1.0); }
    int    cloneMode() const { return m_cloneMode; }       // 0 Clone, 1 Heal, 2 Red-eye
    void   setCloneMode(int m){ m_cloneMode=m; }
    void   redeyeDab(Shape& s, QPointF destImg);           // local red-eye removal under the brush (no source)
    // Opacity-mask paint (Transparency tool sub-mode)
    int    transpMode() const { return m_transpMode; }     // 0 graduated, 1 paint mask
    void   setTranspMode(int m){ m_transpMode=m; resetMaskStroke(); m_dragging=false; m_activeHandle=HNone; }   // also abort any in-progress gradient drag
    double maskNib() const { return m_maskNib; }
    void   setMaskNib(double r){ m_maskNib=qMax(1.0,r); }
    bool   maskErase() const { return m_maskErase; }
    void   setMaskErase(bool e){ m_maskErase=e; }

    QVector<int> selection() const { return m_sel; }
    bool    rotateMode() const { return m_rotateMode; }   // Select-tool two-state handle mode (resize vs rotate/skew)
    QPointF rotPivotDoc() const;                          // current rotation/skew pivot in doc coords (custom or bbox centre)
    Shape*       firstSelected();
    // Select-into-group (edit an object inside a group without ungrouping). While descended, m_sel keeps the
    // top-level group selected (so structural/menu ops stay safe) and the sub-selection lives in m_subSel.
    bool    inGroup() const { return !m_enterPath.isEmpty(); }
    void    exitGroup();                                  // pop one descent level (Esc / click outside the group)
    void select(const QVector<int>& idx);
    void selectAll(); void selectNone(); void deleteSelection();
    void duplicateSelection(bool inPlace=false);   // inPlace: no offset (Ctrl+K clone-in-place)
    void radialArray(int count, double totalDeg);   // rotate `count` copies of the selection around its centre
    void copySelection(); void cutSelection(); bool pasteClipboard();
    bool hasClipboard() const { return !m_clip.isEmpty(); }
    void raiseSelection(); void lowerSelection(); void toFront(); void toBack();
    void groupSelection(); void ungroupSelection();
    void applyClipView();  void removeClipView();       // ClipView: group with the topmost object as the keyhole clipping the rest
    void fitTextToCurve();   // bind a selected Text object to a selected path so its glyphs flow along the curve
    void detachTextFromCurve();
    void fitTextInShape();     // flow a selected Text object's words inside a selected closed shape (area text)
    void detachTextFromShape();
    void   setTextPathOffset(double d);          // slide text-on-curve along its baseline path (doc units from the start)
    void   toggleTextPathSide();                 // flip selected text-on-curve to the reverse (inside/underside) of the path
    bool   textPathStartHandle(QPointF& outView) const;   // view pos of the text-on-curve start handle
    void   setDropCapLines(int n);               // set the drop-cap line span on the selected text frame
    bool   dropCapHandle(QPointF& outView) const;         // view pos of the drop-cap resize handle
    bool   indentMarkerX(int which, double& viewX) const; // ruler indent marker (0 first-line,1 left,2 right) view-x, for a selected text frame
    void makeLiveCopy(); void syncLiveCopies();        // linked duplicates that mirror the master's appearance
    void makeSoftGroup(); void ungroupSoftGroup();     // objects that select together without nesting
    void combineSelection(int op);   // 0 add/union, 1 subtract, 2 intersect, 3 difference
    void sliceSelection();           // knife: cut the lower object by the upper outline(s) into inside+outside pieces
    void   setFreehandSmoothing(double v){ m_freeSmooth=qBound(0.5,v,40.0); }   // Freehand fit tolerance (px)
    double freehandSmoothing() const { return m_freeSmooth; }
    static QPainterPath smoothFreehand(const QVector<QPointF>& pts, double tol);  // freehand smoothing engine (testable)
    bool freehandRedraw(const QVector<QPointF>& strokeDoc);   // reshape the selected open path if the stroke starts on it
    void setSelectionContourGrad(const QColor& outer, double transp);  // contour colour graduation (inner→outer) + independent transparency
    void setSelectionParaSpacing(double before, double after);         // paragraph space-before / space-after on selected text
    void setSelectionListStyle(int style, double indent);              // bullet/numbered/lettered list on selected text frames
    void setSelectionListStart(int start);                             // restart a numbered/lettered list at N (1 = default)
    void setSelectionMeshGrid(int rows, int cols, const QVector<QColor>& nodes);  // arbitrary R×C mesh gradient fill
    void setSelectionFillProfile(double bias, double gain);   // reshape the selection's gradient (Schlick bias/gain, [-1,+1])
    void setSelectionTranspProfile(double bias, double gain); // reshape the selection's graduated transparency (non-linear fade)
    void matchTranspToFillAxes();                             // snap the transparency gradient axis onto the fill gradient axis
    bool hasTextFields() const;                               // any text uses a {page}/{pages}/{date} field
    bool hasTextChains() const;                               // any text frame overflows into another (linked/threaded story)
    void flowTextChains(Document& d) const;                   // distribute each linked story across its chain (mutates the copy d)
    void linkTextFrames();                                    // chain selected text frames into one overflowing story
    void unlinkTextFrames();                                  // clear the chain on selected text frames
    void toggleWebBand();                                     // mark/unmark selection as a full-browser-width background band
    QString tocText() const;                                  // build a real Table of Contents (heading styles + page numbers + dot leaders)
    void addTabStopToSelection(double pos, int align, QChar leader);  // add an explicit tab stop (align + leader) to selected text
    void clearTabStopsOnSelection();                          // remove all explicit tab stops from selected text
    Document resolvedForRender() const;                       // doc copy with field tokens substituted per page + chains flowed
    void breakApartSelection();      // split a multi-subpath shape into separate shapes
    void joinShapesSelection();      // merge selected shapes' outlines into one compound Path
    void convertToPath();            // bake selected shapes into editable Paths
    void strokeToPath();             // outline each selected shape's stroke into an editable filled Path
    void roughenSelection(double amount, double density);   // resample + deterministically jitter outlines (hand-drawn look)
    int  addPaletteFromSelectedPhoto(int n=6);              // extract the selected photo's colour palette → document theme colours
    int  addSpotColor(const QString& name, const QColor& c);// add a named SPOT ink (gets its own separation plate)
    void applyPhotoAdjust(const Fill::Adjust& a);          // re-bake the selected photo from its source + store the params (non-destructive)
    void createTextStyle(const QString& name);             // define/redefine a named text style from the selection + tag it
    void applyTextStyle(const QString& name);              // apply a named style's attributes to the selected text
    void updateTextStyle(const QString& name, const QFont& f, const QColor& c, int align);   // edit a style → cascade to all members
    // Rich text: format a character range [start,start+len) of the selected Text object with a
    // font and/or colour and/or hyperlink (null = leave unchanged). Materialises & re-coalesces runs.
    void setTextRunAttr(int start, int len, const QFont* font, const QColor* color, const QString* url=nullptr);
    void setTextRunTypography(int start, int len, double baseline, double tracking);   // per-run super/subscript + letter-spacing
    // Inkscape-style: if text is being edited on-canvas with an active selection, format ONLY that character range
    // (font and/or colour), then re-coalesce runs. Returns true iff it handled it — the InfoBar/palette then skips
    // its whole-object path. `mut` receives each selected character's font & colour by reference.
    bool formatEditSelection(const std::function<void(QFont&,QColor&)>& mut, const QString& label);
    QFont editSelectionFont() const;   // resolved font of the edit selection's first char (for toggles); else base font
    bool  hasTextEditSelection() const { return editingText() && textHasSelection(); }
    void setTextSpacing(double lineSpacing, double tracking);        // per-object leading + letter-spacing
    void setTextIndents(double firstLine, double left, double right); // per-object paragraph indents (doc px)
    void setTextTabWidth(double w);                                  // per-object tab-stop interval (doc px; 0 = off)
    void setSelectionHideBelow(int px);                              // responsive: hide the selection below a viewport width (0 = show)
    void setSelectionVariant(int belowPx, double x, double y, double w); // Website Variant: alternate pos/size below a breakpoint (0 = clear)
    bool selectionVariant(int& belowPx, double& x, double& y, double& w) const;
    void setSelectionMenuTree(const QString& tree);                  // Buttons & Menus: authored dropdown menu tree on a nav bar
    QString selectionMenuTree() const;
    void setSelectionEmbed(const QString& html);                     // attach web-embed HTML to the selection (or a new placeholder)
    void setSelectionForm(const QString& spec, const QString& action); // attach a contact-form spec (or a new form placeholder)
    void linkPopup();                                                // pair selected objects into a mouse-over pop-up (first=trigger, last=panel)
    void setSelectionLightbox(bool on);                              // pop-up photo: published image opens full-screen (lightbox) on click
    bool selectionLightbox() const;                                  // true if any selected object is a pop-up photo
    void setSelectionWebAnim(int kind, double delay);                // web reveal animation on scroll-into-view (kind 0..7)
    int  selectionWebAnim() const;                                   // animation kind of the selection (0 = none)
    void makeSliderFromSelection();                                  // group selected objects into a cross-fading web carousel
    void clearSelectionSlider();                                     // remove selection from its slider group
    int  selectionSlider() const;                                    // slider group id of the selection (0 = none)
    void makeFlexFromSelection();                                    // group selected objects into a fluid flex-wrap section (reflows on mobile)
    void clearSelectionFlex();                                       // remove selection from its FlexGroup
    int  selectionFlex() const;                                      // FlexGroup id of the selection (0 = none)
    void setSelectionMedia(const QByteArray& data, const QString& mime);  // embed video/audio/GIF as a web media player
    void addNavBar();                                               // drop a navigation-bar object (styled <nav> linking all pages on publish)
    // Page/website templates (W4): build a ready-made document. kind: 0 blank, 1 landing page,
    // 2 business card, 3 photo portfolio. names via templateNames().
    static Document templateDocument(int kind);
    static QStringList templateNames();
    // Web font embedding (P6): resolve each family to a font file (fontconfig) and return
    // @font-face rules embedding it as base64, so published pages render with the exact fonts.
    static QString fontFaceCss(const QStringList& families);
    // Rich-text paste (T3): build a Text shape carrying per-character runs from an HTML/RTF fragment.
    static Shape shapeFromHtml(const QString& html);      // uses QTextDocument to walk fragments → runs
    static Shape shapeFromRtf(const QByteArray& rtf);     // minimal RTF: \b \i \fs \cf + \colortbl + \par
    bool pasteRichText();                                 // clipboard HTML/RTF → styled Text shape (false if none)
    bool pasteBitmapFromClipboard();                      // OS-clipboard image → a new photo object
    bool pasteInPlace();                                  // paste at the copied objects' original coordinates
    bool pasteAttributes();                               // apply the copied object's appearance onto the selection
    void reflowText();                                    // recompute repel obstacles + inline anchored-graphic gaps (derived)
    bool anchorSelectionToText(int charPos);              // anchor the selected graphic inline to the first text frame (T6)
    void alignSelection(int mode);
    void alignSelectionAdv(int hMode, int vMode, bool toPage);   // h/v: 0 none,1 left/top,2 centre,3 right/bottom
    void repeatSelection(int cols, int rows, double dx, double dy);   // grid array of copies
    void distributeSelection(bool horizontal, bool equalGap=false);   // equalGap: equal clear space between edges (not centres)
    void flipSelection(bool horizontal);
    void rotateSelection90(bool clockwise);
    void blendSelection(int steps=6);   // morph N in-between shapes from sel[0] to sel[1]
    void blendAlongPathSelection(int steps=6);   // blend the first two selected objects along the last-selected path (spine)
    void makeLiveBlend(int steps=6);    // group 2 selected shapes into a LIVE (re-editable) blend group
    void makeLiveBlendAlongPath(int steps=6);   // LIVE blend whose steps re-flow along the last-selected shape (spine)
    void setLiveBlendSteps(int steps);  // if a live-blend group is selected, change its in-between count
    bool blendControlEnds(QPointF& aView, QPointF& bView) const;  // the two end-object centres of a selected live blend (view)
    bool blendStepHandle(QPointF& outView) const;   // on-canvas handle to drag the live blend's step count

    void applyToSelection(const std::function<void(Shape&)>& fn, const QString& label);
    QColor sampleColorAt(const QPointF& docPt) const;   // eyedropper: composited document colour at a doc point
    void   skewSelection(double hDeg, double vDeg);      // shear the selection about its centre (incremental)
    void   autoFitToPage(bool bothAxes);                 // scale all objects to fit the page (width-only, or width+height)
    bool   replacePhotoAt(const QPointF& docPt, const QImage& img);  // drop an image onto a photo → swap the fill image (keep frame)
    int    placeBitmap(const QImage& img, const QPointF& centerDoc, const char* undoLabel);  // new photo object centred at centerDoc (shared by paste & drop); returns its index or -1
    QString webTitle() const { return m_doc.webTitle; }
    QString webDescription() const { return m_doc.webDescription; }
    QString webKeywords() const { return m_doc.webKeywords; }
    void   setWebMeta(const QString& title, const QString& desc, const QString& keywords){ pushUndo("Website properties"); m_doc.webTitle=title; m_doc.webDescription=desc; m_doc.webKeywords=keywords; emit documentChanged(); }
    QString webBaseUrl() const { return m_doc.webBaseUrl; }
    QString webAuthor()  const { return m_doc.webAuthor; }
    void   setWebSeoExtra(const QString& baseUrl, const QString& author){ pushUndo("Website SEO"); m_doc.webBaseUrl=baseUrl.trimmed(); m_doc.webAuthor=author.trimmed(); emit documentChanged(); }
    QString webPassword() const { return m_doc.webPassword; }
    void   setWebPassword(const QString& pw){ pushUndo("Website password"); m_doc.webPassword=pw; emit documentChanged(); }
    void   setFavicon(const QImage& im){ pushUndo("Favicon"); m_doc.favicon=im; emit documentChanged(); }
    bool   hasFavicon() const { return !m_doc.favicon.isNull(); }
    bool   fluidSite() const { return m_doc.fluidSite; }
    void   setFluidSite(bool on){ if(m_doc.fluidSite==on) return; pushUndo("Fluid website layout"); m_doc.fluidSite=on; emit documentChanged(); }
    QString webHeadHtml() const { return m_doc.webHeadHtml; }
    QString webBodyHtml() const { return m_doc.webBodyHtml; }
    void   setWebRaw(const QString& head, const QString& body){ pushUndo("Website custom HTML"); m_doc.webHeadHtml=head; m_doc.webBodyHtml=body; emit documentChanged(); }
    bool   presentSteps() const { return m_doc.presentSteps; }
    void   setPresentSteps(bool on){ if(m_doc.presentSteps==on) return; pushUndo("Presentation steps"); m_doc.presentSteps=on; emit documentChanged(); }
    void   setSelectionBuildOrder(int order){ applyToSelection([order](Shape& s){ s.webBuild=qMax(0,order); }, "Build order"); }  // presentation build order within the layer
    void   roundSelectionCorners(double radius);         // Shape Corner tool: round the corners of selected shapes
    // Photo Region tool
    bool   regionEllipse() const { return m_regionEllipse; }
    void   setRegionEllipse(bool e){ m_regionEllipse=e; }
    bool   regionFreehand() const { return m_regionFreehand; }
    void   setRegionFreehand(bool f){ m_regionFreehand=f; }
    bool   regionMagnetic() const { return m_regionMagnetic; }
    void   setRegionMagnetic(bool f){ m_regionMagnetic=f; m_lassoAnchors.clear(); m_lassoShape=-1; update(); }
    void   setShapeRegion(int shapeIdx, const QPainterPath& localRegion);   // clip the object to a local-space region
    QPainterPath magneticLassoRegion(int shapeIdx, const QVector<QPointF>& docAnchors) const;  // edge-snapped selection (livewire) → local path
    int    regionEffect() const { return m_regionEffect; }
    void   setRegionEffect(int kind){ m_regionEffect=qBound(0,kind,5); }   // 0 clip, 1 brighten,2 darken,3 blur,4 sharpen,5 grey
    void   paintEffectRegion(int shapeIdx, const QPainterPath& localRegion, int kind);  // Effect Painter: scoped-adjustment overlay above the photo
    void   clearShapeRegion(int shapeIdx);                                  // remove the region
    void   setPhotoMaskFromRegion(int shapeIdx);                            // Photo Mask Painter: scope non-destructive adjustments to the painted region
    void   clearPhotoMask(int shapeIdx);                                    // remove the adjustment mask (adjustments apply to the whole photo again)
    bool   hasPhotoMask(int shapeIdx) const;
    bool    penHitEndpoint(const QPointF& docPt, int& shapeIdx, bool& atStart) const;   // click near an open path's end node? (public for selftest)
    // Node-editor ops (Shape tool) — public for the InfoBar + selftest.
    void   nodeEdit(int shapeIdx){ beginNodeEdit(shapeIdx); }   // start editing a path's nodes
    void   nodeEditChild(int groupIdx,int childIdx){ beginNodeEditChild(groupIdx,childIdx); }  // edit a group child's nodes in place
    int    nodeSubpaths() const;                          // number of subpaths currently under edit
    int    nodeCount(int sub) const;                      // nodes in a subpath
    void   deleteNode(int sub, int idx);                  // remove a node (drops the subpath if <2 remain)
    void   insertNodeOnSegment(int sub, int idx, double t);  // split the segment after node idx at param t (0..1)
    // Drag-reshape a bezier segment (subpath sub, from node segIdx to segIdx+1): move the curve point at
    // parameter t from grabLocal to targetLocal by adjusting the two interior control handles (least-
    // squares split). out0/in0 are the segment's handle positions at grab time (a straight segment passes
    // its 1/3–2/3 defaults, promoting it to a curve). No undo/select side effects — the caller owns those.
    void   reshapePathSegment(int sub, int segIdx, double t, const QPointF& out0, const QPointF& in0,
                              const QPointF& grabLocal, const QPointF& targetLocal);
    void   setNodeCurved(int sub, int idx, bool curved);  // give a node smooth bezier handles (curve) or remove them (line)
    bool   applyNodeCurve(int sub, int idx, bool curved); // geometry only (no undo/write-back); used for one node or a whole selection
    void   deleteActiveNode();                            // Delete key / InfoBar: act on the grabbed node
    void   selectNodesInRect(const QRectF& docRect);      // marquee-select nodes into the multi-selection set
    int    nodeSelCount() const { return m_nodeSelSet.size(); }
    void   deleteSelectedNodes();                         // delete the multi-selected nodes
    void   reverseNodePath();                             // reverse direction of the edited subpath(s)
    void   breakSelectedNodes();                          // break the path at the selected node(s)
    void   joinSelectedNodes();                           // weld two selected open-path end nodes (or close one path)
    void   setActiveNodeCurved(bool curved);              // Make Curve / Make Line on the grabbed node
    void   setSelectedNodeType(int type);                 // 0 cusp / 1 smooth / 2 symmetric on the grabbed node or selection
    int    activeNodeSub() const { return m_nodeSub; }    // -1 if none grabbed
    void refreshAll(){ update(); emit documentChanged(); emit selectionChanged(); }   // repaint + notify panels after an external doc edit
    // Perf instrument (headless test/benchmark): average ms of one uncached backdrop render vs one
    // cached-blit + overlay render, on the currently loaded document at the current size.
    struct RenderProbe { double fullMs = 0, cachedMs = 0; };
    RenderProbe renderProbe(int iters);
    void setCullEnabled(bool b){ m_cullEnabled = b; m_cacheDirty = true; update(); }   // viewport culling of offscreen shapes (on by default)
    bool cullEnabled() const { return m_cullEnabled; }
    void setDragProxyEnabled(bool b){ m_dragProxyEnabled = b; if(!b) clearDragProxy(); }   // bitmap drag proxy (on by default)
    bool dragProxyEnabled() const { return m_dragProxyEnabled; }
    bool dragProxyActive() const { return m_dragProxyActive; }   // test/introspection hook
    // Headless perf probe: with the current selection, simulate a small move drag, build the proxy, and
    // time N proxy-composite frames vs N full-document-rebuild frames. Returns false if the proxy didn't
    // activate (nothing selected / ineligible). proxyMs & fullMs receive the per-frame millisecond costs.
    bool dragProxyProbe(int iters, double& proxyMs, double& fullMs);
    // Headless probe isolating the selection-outline cost: times drawSelection with the drag-start outline
    // cache (cachedMs) vs the per-frame path rebuild (rebuiltMs). Returns false if the proxy didn't build.
    bool dragOutlineProbe(int iters, double& cachedMs, double& rebuiltMs);
    double snapAdjustProbeMs(int iters);   // headless: mean ms of one move-frame's snapAdjust over the current selection
    void addShapes(const QVector<Shape>& s, const QString& label);   // insert + select + undo (galleries/clipart)
    bool importImage(const QString& path);                           // place an image file as a photo object
    bool importAnimatedGif(const QString& path);                     // one layer (frame) per GIF frame → editable animation
    bool importRtfFile(const QString& path);                         // read an .rtf file into a styled Text shape
    bool importSvgFile(const QString& path);                         // parse an SVG file into editable shapes
    void warnMissingFonts(const QVector<Shape>& shapes);             // status-bar notice if a needed font isn't installed in this session
    static QStringList substitutedFontFamilies(const QVector<Shape>& shapes);  // distinct Text families Qt substituted (not resolvable this session)
    bool importEpub(const QString& path);                            // import an EPUB e-book → one editable text page per chapter
    bool importDocx(const QString& path);                            // import a .docx → an editable text frame (paragraphs/runs)
    bool importFb2(const QString& path);                             // import a FictionBook .fb2 (+.fb2.zip) → an editable text frame
    bool importCdr(const QString& path);                             // import a CorelDRAW .cdr preview bitmap (best-effort placeholder)
    bool mailMerge(const QString& csvPath);                          // data merge: current page = template with {{Field}} → one page per CSV row
    bool insertQrCode(const QString& text);                          // insert a QR code (via libqrencode) as a bitmap object
    bool importVectorFile(const QString& path);                      // EPS/AI/PDF vector import (Module 3) → editable shapes
    bool importCbz(const QString& path);                             // comic-book archive (.cbz zip of images) → one page per image
    bool importCbr(const QString& path);                             // comic-book archive (.cbr/.cb7/.cbt via libarchive) → one page per image
    bool importGeoJson(const QString& path);                         // GeoJSON geometry → editable vector paths
    bool importKml(const QString& path);                             // KML / KMZ geometry → editable vector paths
    bool importHpgl(const QString& path);                            // HP-GL/2 plotter file → editable vector paths
    bool importApng(const QString& path);                            // animated PNG → one layer (frame) per animation frame
    bool importOdg(const QString& path);                             // OpenDocument Drawing (.odg) → editable shapes
    bool importWpg(const QString& path);                             // WordPerfect Graphics (.wpg) → editable vectors (via libwpg)
    bool importVisio(const QString& path);                           // Microsoft Visio (.vsd/.vsdx/.vdx…) → editable vectors (via libvisio)
    bool importFreehand(const QString& path);                        // Macromedia/Adobe FreeHand (.fh*) → editable vectors (via libfreehand)
    bool importMsPub(const QString& path);                           // Microsoft Publisher (.pub) → editable vectors (via libmspub)
    bool importPageMaker(const QString& path);                       // Adobe/Aldus PageMaker (.pmd/.p65) → editable vectors (via libpagemaker)
    bool importZmf(const QString& path);                             // Zoner Draw / Callisto (.zmf) → editable vectors (via libzmf)
    bool importQxp(const QString& path);                             // QuarkXPress (.qxp/.qxd) → editable vectors (via libqxp)
    bool importStarOffice(const QString& path);                      // StarOffice StarDraw/StarImpress (.sda/.sdd) → editable vectors (via libstaroffice)
    bool importIWork(const QString& path);                           // Apple iWork Keynote (.key) → editable vectors (via libetonyek)
    bool importMwaw(const QString& path);                            // legacy Mac MacDraw/ClarisDraw → editable vectors (via libmwaw)
    bool importOgr(const QString& path);                             // GDAL/OGR geospatial vector (DGN/MapInfo/GeoPackage/S-57/GML) → editable paths
    bool importDot(const QString& path);                             // Graphviz DOT graph → laid-out editable vectors (via libgvc)
    bool importOra(const QString& path);                             // OpenRaster (.ora) → one editable photo per layer (stack.xml)
    bool importKra(const QString& path);                             // Krita (.kra) → flattened composite (mergedimage.png)
    bool importWmfFile(const QString& path);                         // Windows Metafile (.wmf): libwmf → SVG (falls back to the native prepress parser)
    bool importShapefile(const QString& path);                       // ESRI Shapefile (.shp) geometry → editable vector paths (native)
    bool importGpx(const QString& path);                             // GPX (GPS Exchange) tracks/routes/waypoints → editable vectors (native)
    bool importXfig(const QString& path);                            // Xfig (.fig) drawing → editable vectors (native)
    bool importCgm(const QString& path);                             // CGM binary (ISO 8632) → editable vectors (native)
    bool importSvgz(const QString& path);                            // gzipped SVG (.svgz) → editable shapes (native inflate → SVG parser)
    bool importXd(const QString& path);                              // Adobe XD → editable vectors, a page per artboard
    bool adoptLayers(const Document& pd, const QString& path, const char* undo, const QString& note);
    bool importProcreate(const QString& path);                       // Procreate → layers
    bool importCptFile(const QString& path);                         // Corel PHOTO-PAINT, with its objects
    bool importPxd(const QString& path);                             // Pixelmator Pro → layers
    bool importClip(const QString& path);                            // Clip Studio Paint → layers
    bool importPsp(const QString& path);                             // PaintShop Pro → layers
    bool importFxg(const QString& path);                             // Adobe FXG → editable vectors
    bool importCmx(const QString& path);                             // Corel Presentation Exchange (.cmx) → editable vectors (via libcdr CMXDocument)
    bool importGerber(const QString& path);                          // Gerber RS-274X PCB image → editable vector paths/pads (native)
    bool importExcellon(const QString& path);                        // Excellon/XNC PCB drill file → editable circles (native)
    bool importLottie(const QString& path);                          // Lottie/Bodymovin animation → static first-frame editable shapes (native)
    bool importSketch(const QString& path);                          // Sketch (.sketch) first page → editable shapes (native)
    bool importTgs(const QString& path);                             // Telegram sticker (.tgs, gzipped Lottie) → static editable shapes (native)
    bool importTopoJson(const QString& path);                        // TopoJSON → editable vector paths (native)
    bool importVml(const QString& path);                             // VML (.vml legacy MS vector markup) → editable shapes (native)
    bool importXps(const QString& path);                             // XPS / OpenXPS (.xps/.oxps) first page → editable shapes (native)
    bool importScribus(const QString& path);                         // Scribus (.sla/.scd) → editable shapes (native)
    bool importAffinity(const QString& path);                        // Serif Affinity (.afdesign/.afphoto/.afpub/.aftemplate/.af) → editable shapes (native)
private:
    bool importComicEntries(const QMap<QString,QByteArray>& entries, const QString& srcName);  // shared CBZ/CBR page builder
    void placeZipComposite(const QImage& img, const QString& name, const QString& undoLabel);   // place a flattened bitmap as one photo
    // Shared consumer for the librevenge-backed vector importers (CDR/WPG/Visio/FreeHand/Publisher):
    // one standard-SVG string per page → parseSvg, multi-page stacked vertically, added as one undo-able import.
    bool importVectorSvgPages(const QStringList& pages, const QString& undoLabel,
                              const QString& statusVerb, const QString& viaLib);
public:
    bool dropImportByExtension(const QString& path);                 // route a dropped file to its importer (svg/pdf/epub/docx/cdr/rtf/gif/xar)
    bool stitchSelectedPhotos();                                     // merge selected overlapping photos into one panorama
    bool traceBitmapSelection();                                     // vectorise the selected photo/bitmap (Trace Bitmap)
    bool rasterizeSelection();                                       // flatten the selection to a single bitmap object (Convert to Bitmap)
    bool createBrushFromSelection();                                 // turn the selected artwork into the current stroke's brush ink
    bool expandEffectsSelection();                                   // convert live shadow/contour of the selection into editable shapes
    void applyFractalFill(const QColor& c0, const QColor& c1);       // give the selection a plasma/fractal cloud fill
    bool addTransparencyStop(double pos, double t);                  // multi-stage: insert an intermediate transparency stop
    bool dropColorAt(const QPointF& docPt, const QColor& c, bool forceLine=false);  // drag-drop a colour onto a shape (interior=fill, edge=line)
    bool cropBitmap(int idx, const QRectF& frac);    // crop a bitmap object to a fractional sub-rect (0..1 of its rect)
    bool beginCropSelection();                       // enter interactive on-canvas crop mode for the selected photo
    void applyCropMode();                            // commit the on-canvas crop
    void cancelCropMode();                           // leave crop mode without cropping
    bool inCropMode() const { return m_cropMode; }
    bool beginStraightenSelection();                 // enter interactive photo-straighten mode for the selected photo
    void applyStraightenMode();                      // level the photo so the reference line becomes horizontal
    void cancelStraightenMode();                     // leave straighten mode without changing the photo
    bool inStraightenMode() const { return m_straightenMode; }
    void setStraightenLine(const QPointF& aDoc, const QPointF& bDoc);   // set the reference line (drives the angle)
    double straightenAngleDeg() const;                                 // angle of the reference line (0 = horizontal)
    void moveMouldCorner(int i, const QPointF& docPt);               // move mould corner i (0=TL,1=TR,2=BR,3=BL) + re-warp (programmatic/testing)
    // Envelope (curved-edge) mould. Mode 0 = Perspective (projective quad, the original), 1 = Envelope
    // (4 cubic-bezier edges → Coons-patch warp baked into geometry). 8 edge control points: edge i owns
    // ctrl[2i] (near corner i) and ctrl[2i+1] (near corner i+1). moveMouldCtrl is public for testing.
    int     mouldMode() const { return m_mouldMode; }
    void    setMouldMode(int mode);
    QPointF mouldCtrl(int i) const { return (i>=0&&i<8)?m_mouldCtrl[i]:QPointF(); }
    void    moveMouldCtrl(int i, const QPointF& docPt);
    // Bake the given cubic-bezier cage (corners[4] + ctrls[8], doc space) into the selection's geometry
    // as warped Path shapes. Undo-able; used by the interactive tool and directly by tests.
    void    applyEnvelopeToSelection(const QPointF corners[4], const QPointF ctrls[8]);
    // ---- In-canvas text editing (live caret + typing, replaces the modal text dialog) ----
    void beginTextEdit(int shapeIdx, int caretIdx=-1);   // caretIdx<0 => caret at end
    void endTextEdit(bool keepEmpty=false);              // commit + leave edit mode (deletes an empty new shape unless keepEmpty)
    bool editingText() const { return m_textEdit>=0 && m_textEdit<m_doc.shapes.size() && m_doc.shapes[m_textEdit].type==ShapeType::Text; }
    int  textEditShape() const { return m_textEdit; }
    int  textCaret() const { return m_caret; }
    bool textHasSelection() const { return m_caretAnchor>=0 && m_caretAnchor!=m_caret; }
    void textInsert(const QString& s);                   // insert / replace selection at the caret
    bool smartQuotes() const { return m_smartQuotes; }
    void setSmartQuotes(bool on){ m_smartQuotes=on; }
    void convertSmartQuotes();                           // curl straight quotes in selected text objects (manual p226)
    bool spellLive() const { return m_spellLive; }
    void setSpellLive(bool on){ m_spellLive=on; update(); }   // live red squiggles under misspellings while editing
    bool outlineView() const { return m_outlineView; }
    void setOutlineView(bool on){ m_outlineView=on; update(); }   // wireframe view (screen only; export unaffected)
    // Misspelled-word char ranges [start,end) in the edited (or a given) text shape — for the squiggle overlay + tests.
    QVector<QPair<int,int>> misspelledRanges(const QString& text) const;
    void textBackspace(); void textDelete();
    void textMoveCaret(int dir, bool extend);            // dir: -1 left, +1 right, -2 home, +2 end, -3 up, +3 down
    void textSelectAll();
    void setCaretAtDocPoint(const QPointF& docPt, bool extend);   // click-to-position within the edited shape
    static bool canInlineEdit(const Shape& s);   // true only for plain point text (exact caret maths); else use the dialog
    void recolorSelection(double hueDeg, double satScale, double lightScale);   // hue-rotate + scale sat/lightness of every colour in the selection
    void cutoutBackgroundSelection(int tol);   // Photo Cutout Studio: auto background removal (edge flood-fill keying + soft matte)
    bool cutoutStudioApply(int si, const QImage& seed);   // guided graph-cut cutout: keep/remove scribbles → re-editable transparency mask
    bool correctPerspective(int si, double kv, double kh);// photo keystone/perspective correction (projective warp; kv=vertical, kh=horizontal)
    void replaceColorInSelection(const QColor& from, const QColor& to, int tol);// swap one colour (within tolerance) for another

    int  selOutlineElemsForTest(){ ensureSelOutlineCache(); return m_selOutlineDoc.elementCount(); }   // --selftest: build+measure the cached silhouette
    bool selCacheValidForTest() const { return m_selCacheValid; }                                       // --selftest: is the outline cache warm?
    QRectF selBoundsForTest(){ ensureSelOutlineCache(); return m_selBoundsDoc; }                         // --selftest: the cached selection box
    // selection geometry (for the InfoBar position/size/rotation fields)
    QRectF selectionBoundsDoc() const { return selBoundsDoc(); }
    double selectionRotation() const;          // degrees, from the first selected shape
    void setSelectionPos(double x, double y);  // move bbox top-left to (x,y)
    void setSelectionSize(double w, double h, bool keepAspect=false);
    void rotateSelection(double deg);          // rotate by deg about the bbox centre

    void zoomIn(); void zoomOut(); void zoomReset(); void zoomFit(); void zoomTo(double z);
    void zoomToRect(const QRectF& docR);  // fit a doc rectangle to the view (marquee zoom)
    void zoomToSelection(); void zoomToDrawing(); void previousZoom();
    double zoom() const { return m_zoom; }
    void   benchForceCrisp(){ m_previewZoom = false; m_cacheDirty = true; }   // --zoombench only: measure the full crisp repaint, not the preview blit
    bool   previewActiveForTest() const { return m_previewZoom; }             // --previewshot only: is the bitmap-preview path armed?
    void   enterGroupForTest(int topIndex){ enterGroup(topIndex,-1); }        // --groupshot only: enter a top-level group (isolation fade)
    int    firstTopLevelGroup() const { for(int i=0;i<m_doc.shapes.size();++i) if(m_doc.shapes[i].type==ShapeType::Group) return i; return -1; }

    void lockSelection(); void unlockSelection();    // Shape.locked: locked objects can't be moved/edited
    // layers
    int  activeLayer() const { return m_activeLayer; }
    void setActiveLayer(int i){ m_activeLayer=qBound(0,i,m_doc.layers.size()-1); }
    void addLayer(); void removeLayer(int i); void renameLayer(int i,const QString& n);
    void setLayerVisible(int i,bool v); void setLayerLocked(int i,bool l); void setLayerMaster(int i,bool m);
    void setLayerPrintable(int i,bool p); bool layerPrintable(int i) const;   // non-printing layer: shown on screen, hidden from print/export
    int  optimizePhotos(double dpiHeadroom=1.5);   // downsample placed photos to their on-canvas display resolution
    bool scaleLineWidths() const { return m_doc.scaleLineWidths; }
    void setScaleLineWidths(bool on){ m_doc.scaleLineWidths=on; }   // resize scales stroke width (Xara default ON)
    void moveSelectionToLayer(int i); void moveLayer(int i,int delta);   // delta +1 up / -1 down
    void soloLayer(int i);   // show only layer i (toggle: if already solo, show all)
    // Per-object edits used by the Page & Layer tree gallery.
    void setShapeVisible(int idx,bool v); void setShapeLocked(int idx,bool l); void renameShape(int idx,const QString& n);
    // Reorder/relayer the whole top-level shape list. `order` lists old shape indices in the NEW
    // paint order (bottom-to-top); `newLayer[k]` is the layer for order[k]. A permutation of 0..N-1
    // (validated; a no-op if malformed) — used by the tree's drag-to-restack. Undo-able.
    void setObjectOrder(const QVector<int>& order,const QVector<int>& newLayer);
    void addGuide(bool horizontal, double pos);       // persistent ruler guide (doc coords)
    void clearGuides(){ m_guideHpos.clear(); m_guideVpos.clear(); update(); }
    const QVector<double>& guidesH() const { return m_guideHpos; }
    const QVector<double>& guidesV() const { return m_guideVpos; }

    bool showRulers() const { return m_showRulers; }
    void setShowRulers(bool b){ m_showRulers=b; update(); }
    bool snapEnabled() const { return m_snap; }
    void setSnap(bool b){ m_snap=b; }

    // measurement units — doc-space is 1/96 inch per pixel ("@96dpi feel")
    enum class Units { Pixels, Millimetres, Centimetres, Inches, Points };
    Units units() const { return m_units; }
    void  setUnits(Units u){ m_units=u; emit unitsChanged(); update(); }
    static double  unitFactor(Units u);            // doc-pixels per 1 unit
    static QString unitSuffix(Units u);
    QString fmtLen(double docPx, bool withSuffix=true) const;  // format a length in the current units

    bool   showGrid() const { return m_showGrid; }
    void   setShowGrid(bool b){ m_showGrid=b; update(); }
    bool   softProof() const { return m_softProof; }                         // CMYK soft-proof (view-only, off by default)
    void   setSoftProof(bool b){ m_softProof=b; update(); }
    bool   gamutWarn() const { return m_gamutWarn; }                          // out-of-CMYK-gamut warning overlay (view-only)
    void   setGamutWarn(bool b){ m_gamutWarn=b; update(); }
    bool   tacWarn() const { return m_tacWarn; }                              // total-ink-coverage (TAC) warning overlay (view-only)
    void   setTacWarn(bool b){ m_tacWarn=b; update(); }
    int    tacLimit() const { return m_tacLimit; }
    void   setTacLimit(int pct){ m_tacLimit=qBound(100,pct,400); if(m_tacWarn) update(); }
    QImage renderImposedSheet(int cols, int rows, const QSizeF& sheetPx) const;   // N-up imposition: pages tiled onto one sheet
    bool   snapToGrid() const { return m_snapGrid; }
    void   setSnapToGrid(bool b){ m_snapGrid=b; }
    double gridSpacing() const { return m_gridStep; }          // doc px between grid lines
    void   setGridSpacing(double px){ m_gridStep=qMax(2.0,px); update(); }
    bool   gridIso() const { return m_gridIso; }               // isometric (30°) grid instead of rectangular
    void   setGridIso(bool b){ m_gridIso=b; update(); }
    void   setPageColor(const QColor& c);                      // document page/background colour
    QSizeF pageSize() const { return m_doc.pageSize; }
    void   setPageSize(QSizeF s){ pushUndo("Page size"); m_doc.pageSize=s; update(); emit documentChanged(); }
    // multi-page
    int  pageCount() const { return m_doc.pageCount; }
    int  currentPage() const;            // page nearest the view centre
    void movePage(int page, int delta);  // reorder: swap page content with the adjacent page (delta ±1)
    PageProps pageProps(int i) const { return (i>=0&&i<m_doc.pageProps.size())? m_doc.pageProps[i] : PageProps{}; }
    void setPageProps(int i, const PageProps& p){ if(i<0)return; pushUndo("Page properties"); while(m_doc.pageProps.size()<=i) m_doc.pageProps.append(PageProps{}); m_doc.pageProps[i]=p; emit documentChanged(); }
    void setRulerOrigin(const QPointF& docPt);   // set the ruler zero-point to a doc position
    void resetRulerOrigin();                     // ruler zero-point back to (0,0)
    void addPage(); void deletePage(int i); void duplicatePage(int i); void gotoPage(int i);

    QPointF mapToView(QPointF docPt) const { return toView(docPt); }  // test helper (--selftest)
    void openRadialForShot(int hoverRing=1, int hoverIdx=1);           // debug/screenshot: force the Ctrl pie open (XARA_RADIAL)
    int  radialCount(int ring) const { return ring==0? m_radialInner.size() : ring==1? m_radialOuter.size() : 0; }   // --selftest
    int  radialHitProbe(const QPointF& v, int& ring) const { return radialHitTest(v,ring); }                          // --selftest
    void radialForceClose();                                                                                          // --selftest (defined in canvas_radial.inc; QTimer is incomplete here)
    static QPointF constrainSquareCorner(const QPointF& a, const QPointF& b);  // Ctrl-draw: square/circle box
    static QPointF constrainLineAngle(const QPointF& a, const QPointF& b);     // Ctrl-draw: snap line to 15°

    void undo(); void redo();
    bool canUndo() const { return !m_undo.isEmpty(); }
    bool canRedo() const { return !m_redo.isEmpty(); }
    int  undoDepth() const { return m_undo.size(); }   // number of undo snapshots (for tests)
    QString undoLabel() const { return m_undoLabels.isEmpty()?QString():m_undoLabels.last(); }   // name of the op the next Undo reverses
    QString redoLabel() const { return m_redoLabels.isEmpty()?QString():m_redoLabels.last(); }
    void pushUndo(const QString& label);

    QString filePath() const { return m_path; }
    bool save(const QString& path); bool load(const QString& path);
    bool saveSnapshot(const QString& path) const;   // write the doc WITHOUT changing the file path (autosave/recovery)
    bool saveCompressed(const QString& path);        // write a deflate-compressed .xar (smaller on disk)
    bool exportPng(const QString& path, int scale=2);
    bool exportRaster(const QString& path, int areaMode, double scale, int quality, bool transparent, bool indexed=false, bool dither=false, int pageIndex=-1);  // Export dialog engine (area/scale/quality/matte + palette optimiser); pageIndex: which page for areaMode==Page (-1 = current)
    QRectF exportAreaRect(int areaMode, int pageIndex=-1);                                        // doc-space rect for a given export area (Page/Drawing/Selection)
    QImage exportRasterImage(int areaMode, double scale, bool transparent, int pageIndex=-1, int maxDim=0);  // render the area to an ARGB image (no encoding); maxDim>0 caps the longest side (for live preview)
    bool exportSvg(const QString& path);
    bool exportEps(const QString& path);   // Encapsulated PostScript (prepress interchange)
    bool exportPdf(const QString& path);
    bool exportCmykPdf(const QString& path);   // composite device-CMYK PDF (PDF/X-style) with an embedded OutputIntent
    bool exportHtml(const QString& path);      // PNG of the page + clickable <a> overlays for linked objects
    bool exportWebsite(const QString& path);   // semantic responsive site: SVG vector layer + real HTML text + breakpoints + multi-page nav
    bool exportSeparations(const QString& path);  // prepress: C/M/Y/K greyscale plates as a multi-page PDF (Module 3)
    bool exportBooklet(const QString& path);       // saddle-stitch 2-up imposition: pages paired for folding into a booklet
    bool exportPosterPdf(const QString& path, double sheetW, double sheetH, double overlap);   // tile an oversized page across sheets
    bool exportWithMarks(const QString& path, double bleedPx, double markLen);  // prepress: crop + registration marks + bleed (Module 3)
    void drawPageWithMarks(QPainter& g, double bleedPx, double markLen);         // artwork + crop/registration marks (shared by export + preview/test)
    bool exportGif(const QString& path, int delayMs=120, int loops=0);   // animated GIF — one frame per layer (loops: 0=forever)
    bool exportPresentation(const QString& path, int transition=1);   // HTML slideshow; transition 0 cut,1 fade,2 slide,3 zoom
    void renderDocument(QPainter& g) const;   // paint the document at doc coords (for printing)
    void newDocument();

signals:
    void selectionChanged();
    void documentChanged();
    // Coalesced form of documentChanged: emitted immediately for one-off edits, but debounced (~45 ms)
    // while a live drag is firing documentChanged every frame. Heavy O(N) galleries/panels connect here
    // so a move/resize of a big selection doesn't rebuild the whole Layers/Name/Colour trees per frame.
    void documentChangedDebounced();
    void toolChanged(Tool t);
    void zoomChanged(double z);
    void mouseDoc(QPointF docPt);
    void statusMessage(QString s);
    void unitsChanged();
    void fileImported(const QString& path);   // a file was dropped onto the canvas & imported → MainWindow records it in Recent Files

protected:
    void paintEvent(QPaintEvent*) override;
    void paintBackdrop(QPainter& g);          // background + pages + grid + document layer — the expensive, cacheable part
    void paintOverlays(QPainter& g);          // guides, selection, tool handles, crop/straighten, rulers — cheap, drawn every frame
    bool manipulatingContent() const;         // a live drag is mutating document geometry ⇒ the doc cache must be rebuilt this frame
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void keyReleaseEvent(QKeyEvent*) override;   // Ctrl release commits the radial ("pie") menu selection
    bool event(QEvent*) override;   // steal ShortcutOverride for editing keys (Delete/Ctrl+A/…) while a text caret is active
    void contextMenuEvent(class QContextMenuEvent*) override;   // right-click object/selection/pasteboard menu
    void dragEnterEvent(QDragEnterEvent*) override;   // accept a dragged colour swatch
    void dropEvent(QDropEvent*) override;             // colour dropped → set fill (interior) or line (edge)

public:
    enum Handle { HNone, TL,T,TR,R,BR,B,BL,L, Rotate, GradP0, GradP1, GradP2, GradMid, SmartParam, SmartParam2, CornerRad, ShadowOff, ShadowBlur, EffectScalar, ExtrudeVec, ExtrudeLight, BevelLight, ShadowRake, EllipseStart, EllipseEnd, PolyCurv, StarDepth, RotPivot, SkewH, SkewV, BlendStep, TextPathStart, DropCapSize };
    bool cornerRadHandle(QPointF& outView) const;   // view pos of the rounded-corner handle (single Rect selected)
    bool shadowBlurHandle(QPointF& outView) const;   // view pos of the shadow blur handle (single shadowed shape)
    bool shadowRakeHandle(QPointF& outView) const;   // view pos of the FLOOR shadow rake/tip handle (lean + length)
    bool shadowColorSwatch(QPointF& outView) const;  // view centre of the shadow colour swatch (double-click / drop to recolour)
    bool contourColorSwatch(QPointF& outView) const; // view centre of the contour colour swatch (double-click / drop to recolour)
    bool contourColor2Swatch(QPointF& outView) const;// view centre of the contour OUTER (graduation) colour swatch (double-click to set)
    bool ellipseRimHandle(bool endPt, QPointF& outView) const;  // view pos of the pie/arc start (endPt=false) or end (true) rim handle
    bool polyCurvHandle(QPointF& outView) const;     // view pos of the polygon edge-curvature handle
    bool starDepthHandle(QPointF& outView) const;    // view pos of the star inner-vertex depth handle (star polygons only)
    bool bevelColorSwatch(QPointF& outView) const;   // view centre of the bevel light-colour swatch (double-click / drop to recolour)
    bool extrudeColorSwatch(QPointF& outView) const; // view centre of the extrude wall-colour swatch (double-click / drop to recolour)
    bool effectHandle(QPointF& outView) const;       // view pos of the scalar handle for the active Contour/Bevel/Feather tool
    bool extrudeHandle(QPointF& outView) const;      // view pos of the extrude vanishing-point handle (depth+direction)
    bool extrudeLightHandle(QPointF& outView) const; // view pos of the extrude light-azimuth handle
    bool bevelLightHandle(QPointF& outView) const;   // view pos of the bevel light-azimuth handle
    void setSmartParam(double p);   // change the live parameter of the selected SmartShape
    void setSmartParam2(double p);  // change the 2nd parameter (depth/thickness/inner-radius, 0..1)
    bool smartHandle2(QPointF& outView) const;  // view pos of the SmartShape 2nd-param handle (bottom-right)
    void setMeshCornerColor(int corner, const QColor& c);   // recolour one corner of the selected Mesh fill (0..3)
    int  meshCorner() const { return m_meshCorner; }
    int  hitShape(const QPointF& docPt) const;                    // topmost object under docPt (public for --selftest)
    int  hitShapeBehind(const QPointF& docPt, int above) const;   // Alt+click: cycle down the z-stack under the cursor
    QTransform viewXf() const;                                    // doc→view (translate + uniform zoom, no rotation)
    QPointF toDoc(const QPointF& w) const;                        // view/widget → document coords
    QPointF toView(const QPointF& d) const;                       // document → view/widget coords
private:

    Handle hitHandle(const QPointF& widgetPt) const;
    QVector<QPointF> handlePoints() const;
    QVector<QPointF> handlePointsForRect(const QRectF& viewRect) const;   // 8 bbox handles + rotate stem, from a view-space rect
    bool smartHandle(QPointF& outView) const;   // view pos of the SmartShape param handle (single Smart selected)
    QVector<QPointF> meshCornerLocals(const Shape& s) const;   // the 4 mesh corner positions in shape-local space
    int  hitMeshCorner(const QPointF& widgetPt) const;         // which mesh corner swatch is under the cursor (-1 none)
    // Clone/Heal: map a doc point to an image-pixel of shape si's bitmap fill; paint one dab.
    bool docToImagePixel(int si, const QPointF& docPt, QPointF& outImg) const;
    void cloneDab(Shape& s, const QImage& srcSnap, QPointF destImg, QPointF srcImg);
    void resetCloneStroke(){ m_cloneStroke=false; m_cloneSnap=QImage(); m_cloneLastImg=QPointF(); }   // end/abort an in-progress stroke
    void resetCloneState(){ resetCloneStroke(); m_cloneHasSrc=false; m_cloneShape=-1; }                // also forget the source (new doc / undo)
    // Opacity-mask paint helpers
    void ensureMask(Shape& s);                                  // create a white (opaque) luminance mask if absent
    bool docToMaskPixel(int si, const QPointF& docPt, QPointF& outImg) const;
    void maskDab(Shape& s, QPointF destImg, double targetGray); // soft round dab toward 0(transparent)/255(opaque)
    void resetMaskStroke(){ m_maskStroke=false; m_maskSnap=QImage(); m_maskShape=-1; m_maskLastImg=QPointF(); }
    QRectF selBoundsDoc() const;
    // Select-into-group helpers. `enteredContainer` resolves m_enterPath to the group we're inside;
    // `containerXf` is that group's local→doc transform (so a child's doc transform = containerXf * child.transform);
    // `selTargets` returns live pointers to the objects the current gesture edits (top-level m_sel or in-group children).
    Shape* enteredContainer();
    const Shape* enteredContainer() const;
    QTransform containerXf() const;
    int  hitChildInGroup(const Shape& group, const QTransform& gx, const QPointF& d) const;
    QVector<Shape*> selTargets();
    void enterGroup(int topIndex, int childHit);   // descend into a top-level group, sub-selecting childHit (or -1 = none)
    void resetGroupEntry();                         // drop all descent state (new doc / undo / load)

    void drawGrid(QPainter& g);
    void drawRulers(QPainter& g);
    void drawSelection(QPainter& g);
    void drawToolOverlay(QPainter& g);
    // Snap the selection bbox to page/object edges & centres; returns the delta to
    // apply and records the alignment guide positions (doc coords) that fired.
    QPointF snapAdjust(const QRectF& bb, QVector<double>& guideX, QVector<double>& guideY) const;
    void commitNewShape(Shape s, const QString& label);
    Shape styledShape() const;

    Document m_doc;
    QString  m_path;
    Tool     m_tool = Tool::Select;
    Fill     m_fill;
    Stroke   m_stroke;
    QVector<int> m_sel;
    QVector<int> m_enterPath;   // Select-into-group: child-index path from a top-level group to the entered container (empty = top level)
    QVector<int> m_subSel;      // selected child indices within the entered container
    int      m_polySides = 5; bool m_polyStar=false;
    QFont    m_textFont{QStringLiteral("DejaVu Sans"), 36};   // Text tool: attributes applied to NEW text
    QColor   m_gradHandleColor{70,150,245};                   // Fill/Transparency editing-handle colour (overlay only)
    int      m_tableRows = 3, m_tableCols = 3;   // default grid for the Table tool (settable via its InfoBar)
    bool     m_regionEllipse = false;            // Photo Region tool: rectangular (false) or elliptical (true) region
    bool     m_regionFreehand = false;           // Photo Region tool: paint a freehand region outline instead of a rect/ellipse
    bool     m_regionMagnetic = false;           // Photo Region tool: magnetic lasso — click anchors, snap to edges, double-click to close
    QVector<QPointF> m_lassoAnchors;             // magnetic-lasso anchor points (doc space)
    int      m_lassoShape = -1;                  // photo the lasso is being drawn on
    int      m_regionEffect = 0;                 // Photo Region tool: 0 clip, else paint a scoped adjustment (Effect Painter)
    // Table cell interaction state (T-2)
    int      m_tableShape = -1;                  // index of the table under cell-edit (-1 = none)
    int      m_cellAR=-1, m_cellAC=-1;           // selection anchor cell (row,col)
    int      m_cellBR=-1, m_cellBC=-1;           // selection extent cell (range = anchor..extent inclusive)
    bool     m_cellRangeDrag=false;              // dragging a rectangular cell range
    void     clearTableSel(){ m_tableShape=-1; m_cellAR=m_cellAC=m_cellBR=m_cellBC=-1; }
    double   m_eraserNib = 14;
    bool     m_eraserSoft = false;               // soft eraser: paint feathered alpha into a photo instead of a hard cut
    int      m_softEraseShape = -1;              // photo being soft-erased during a drag
    int      m_activeLayer = 0;
    // Clone/Heal brush state
    double   m_cloneNib = 28, m_cloneHardness = 0.5;
    int      m_cloneMode = 0;                 // 0 Clone, 1 Heal
    bool     m_cloneHasSrc = false;           // source anchor set (Alt-click)
    QPointF  m_cloneSrcImg;                   // source point, image-pixel space
    int      m_cloneShape = -1;               // photo being retouched
    bool     m_cloneStroke = false;           // a paint stroke is in progress
    QPointF  m_cloneOffset;                   // srcImg - destImg, fixed at stroke start (image px)
    QPointF  m_cloneLastImg;                  // last dab (image px) for gap-filling interpolation
    QImage   m_cloneSnap;                     // pre-stroke image snapshot (clone source, avoids self-smear)
    // Opacity-mask paint state
    int      m_transpMode = 0;                // 0 graduated, 1 paint mask
    double   m_maskNib = 30;
    bool     m_maskErase = true;              // true: paint transparent (toward black); false: restore (toward white)
    bool     m_maskStroke = false;
    int      m_maskShape = -1;
    QPointF  m_maskLastImg;
    QImage   m_maskSnap;                      // pre-stroke mask snapshot

    double  m_zoom = 1.0;
    QPointF m_origin{60, 60};
    double  m_prevZoom = 1.0; QPointF m_prevOrigin{60,60}; bool m_hasPrevZoom = false;   // Previous Zoom

    bool    m_dragging = false, m_panning = false, m_rotating=false;
    bool    m_pressMoved = false;       // has the mouse moved (with a button held) since the last press? — a plain
                                        // click keeps this false so manipulatingContent() stays false and the
                                        // cached document is reused (no full re-raster just to add a selection)
    bool    m_moveUndoPending = false;  // a body-drag was armed on press; push the "Move" undo only on the first
                                        // actual move, so a plain click doesn't serialise the whole document
    QPointF m_pressView, m_lastView, m_pressDocPt, m_panStart;
    Handle  m_activeHandle = HNone;
    QRectF  m_dragStartBounds;
    QRectF  m_textStartRect;        // text-frame rect at resize start (side handles reflow the frame instead of scaling glyphs)
    QPointF m_textDragStart;        // Text tool: press point (a drag from here grows the new point text into a frame)
    bool    m_textDragPending=false;
    int     m_indentDrag=-1;        // dragging a ruler indent marker: 0 first-line, 1 left, 2 right; -1 none
    int     m_tableDivShape=-1;     // table whose row/col divider is being dragged (-1 none)
    int     m_tableColDivider=-1;   // interior column boundary index being dragged (-1 none)
    int     m_tableRowDivider=-1;   // interior row boundary index being dragged (-1 none)
    double  m_tableDivStart=0;      // colW/rowH at drag start
    int     m_blendFromShape=-1;    // Blend tool: the object a drag-to-blend started on (-1 none)
    QPointF m_shadowStartOffset;     // Shadow tool: offset at drag start
    bool    m_shadowDidDrag=false;   // Shadow tool: distinguishes a click (toggle) from a drag (set offset)
    bool    m_cropMode = false;      // interactive on-canvas crop in progress
    int     m_cropShape = -1;        // the bitmap being cropped (index)
    int     m_cropHandle = -1;       // 0..7 crop handle being dragged (-1 none)
    QRectF  m_cropRectDoc;           // the crop rectangle in doc space
    int     cropHandleAt(const QPointF& viewPt) const;   // hit-test the 8 crop handles
    bool    m_straightenMode = false;  // interactive on-canvas photo straighten in progress
    int     m_straightenShape = -1;    // the photo being straightened
    QPointF m_straightenA, m_straightenB;  // the reference "should-be-level" line (doc space)
    int     m_straightenGrab = -1;     // 0=A, 1=B, 2=whole line; -1 none
    QVector<QTransform> m_dragStartXf;
    QVector<double>     m_dragStartWidth;   // stroke widths at resize-drag start (for "scale line widths")
    double  m_rotStartAngle=0;
    double  m_smartStartParam=0;   // SmartShape param at drag start
    double  m_smartStartParam2=0;  // SmartShape 2nd param at drag start
    // Select-tool two-state handles + custom rotation pivot (Xara: re-click the selection to switch
    // the resize handles to rotate/skew; drag the pivot marker to move the centre of rotation).
    bool    m_rotateMode=false;    // false=resize handles, true=rotate(corners)/skew(mid-edges)
    bool    m_rotPivotSet=false;   // false => pivot is the selection bbox centre
    QPointF m_rotPivotDoc;         // custom pivot (doc coords) when m_rotPivotSet
    QPointF m_rotCenterActive;     // pivot captured at drag start (stable while the bbox changes mid-rotate)
    bool    m_toggleRotCandidate=false;  // pressed on the already-selected object (no handle): a no-drag release toggles the mode
    int     m_meshCorner=-1;       // selected mesh-gradient corner (0..3) for the Fill tool, -1 none
    QPointF m_gradStart0, m_gradStart1, m_gradStart2;   // gradient handles (local) at the start of a mid-handle drag
    bool    m_rubber = false; QRectF m_rubberRect;
    bool    m_showRulers = true, m_snap = true;
    Units   m_units = Units::Pixels;
    bool    m_showGrid = false, m_snapGrid = false;
    bool    m_softProof = false;                     // CMYK soft-proof preview (paintEvent-only; never affects export)
    bool    m_gamutWarn = false;                      // out-of-gamut warning overlay (paintEvent-only)
    bool    m_tacWarn = false;                         // total-area-coverage (ink limit) warning overlay (paintEvent-only)
    int     m_tacLimit = 300;                          // ink limit in % (300 = typical coated-stock TAC)
    double  m_gridStep = 37.795275;   // 1 cm @96dpi
    bool    m_gridIso = false;         // isometric grid mode

    // ---- screen-render cache (perf) --------------------------------------
    // The backdrop + document (paintBackdrop) is expensive; it only changes when the document, the view
    // transform, the widget size or a view-mode toggle changes. Cache it and, on overlay-only repaints
    // (mouse hover, selection, guides, coordinate readout, rulers, crop/straighten), just blit the cache
    // and redraw the cheap overlays. Screen-only — the export path (paintDocument) is never cached.
    struct DocCacheKey {
        QTransform xf; QRectF pagesRect; double gridStep=0; unsigned pageColor=0;
        int tacLimit=0, pageCount=0;
        bool outline=false, softProof=false, gamut=false, tac=false, grid=false, gridIso=false;
        int  enterTop=-1;   // top-level index of the entered group (-1 = not in group-isolation), for the fade overlay
        bool operator==(const DocCacheKey& o) const {
            return xf==o.xf && pagesRect==o.pagesRect && gridStep==o.gridStep && pageColor==o.pageColor
                && tacLimit==o.tacLimit && pageCount==o.pageCount && outline==o.outline && softProof==o.softProof
                && gamut==o.gamut && tac==o.tac && grid==o.grid && gridIso==o.gridIso && enterTop==o.enterTop; }
    };
    QImage      m_screenCache;              // cached backdrop+document at device pixels; reused across overlay-only frames
    DocCacheKey m_cacheKey;                 // view/mode key the cache was built for
    bool        m_cacheDirty = true;        // document content changed ⇒ the cache must be rebuilt
    bool        m_cullEnabled = true;       // skip rasterising shapes wholly outside the viewport (canvas only)
    DocCacheKey currentCacheKey() const;
    // Interactive-zoom bitmap preview. A full-quality repaint of a busy document at high zoom can take ~1s and
    // blocks the event loop, freezing the UI. While a wheel-zoom gesture is live we DON'T re-render the document:
    // we blit the last rendered backdrop (m_screenCache), transformed from the view it was rendered at to the
    // current view — an instant scaled preview. A debounce timer then does ONE crisp re-render after motion
    // settles. Only armed when the previous full frame was actually slow, so light docs re-render crisply every
    // frame (no pixelated "pop").
    bool        m_previewZoom = false;      // this frame is an interactive scaled-bitmap preview (no document re-render)
    double      m_lastBackdropMs = 0.0;     // wall-clock of the last full backdrop rebuild (adaptive preview trigger)
    QTimer*     m_zoomSettleTimer = nullptr;// fires ~150 ms after the last zoom step → clear preview + one crisp repaint
    void        noteZoomPanInteraction();   // call on each wheel/interactive-zoom step to arm the preview + settle timer

    // --- Drag proxy (dynamic-object layer) ---------------------------------------------------------
    // While a whole-selection affine drag (move / resize / rotate / skew) is in progress, every selected
    // shape's transform is startXf·M for a SINGLE shared doc-space matrix M. So the moving selection is a
    // fixed picture captured once into m_dragMovingProxy; each frame we blit the static rest of the document
    // (m_dragStaticProxy) and draw the moving bitmap under S = viewInv·M·view. No per-frame vector re-raster.
    bool       m_dragProxyActive = false;   // proxy built and in use this drag
    bool       m_dragProxyTried  = false;   // build attempted this drag (don't retry every frame if ineligible/too big)
    bool       m_dragProxyEnabled = true;   // master toggle (test/opt-out)
    QImage     m_dragStaticProxy;           // screen-sized: full backdrop with the moving shapes skipped
    QImage     m_dragMovingProxy;           // object-bounds bitmap: only the moving shapes, at build-time geometry
    QPointF    m_dragProxyOrigin;           // logical (view) top-left where m_dragMovingProxy sat at build time
    QTransform m_dragProxyBaseXf;           // reference target's transform at build time (M = base⁻¹·current)
    QTransform m_dragProxyViewXf;           // view transform at build time (bail out if zoom/pan changes mid-drag)
    QSize      m_dragProxyViewSize;         // widget size at build time (bail out on resize mid-drag)
    QSet<const void*> m_dragProxySkip;      // honoured by paintBackdrop's document layer when building the static proxy
    QPainterPath m_dragSelOutline;          // selection silhouette captured (doc space) at drag start; drawSelection remaps it
                                            // by M each frame instead of rebuilding each shape's (heavy group) path — see drawSelection
    // Static (non-drag) selection-outline cache: the doc-space selection/entered-group silhouette + its bounds, built
    // ONCE per selection/doc change and reused across the many overlay repaints (e.g. every Ctrl+Alt pie-hover frame).
    // Without it, drawSelection rebuilt the whole group's QPainterPath via Shape::localPath() EVERY frame → a repaint
    // storm that froze the UI on a big grouped document while the pie was open. Overlay-only; never in the export path.
    QPainterPath m_selOutlineDoc; QRectF m_selBoundsDoc; bool m_selCacheValid=false;
    void       ensureSelOutlineCache();     // (re)build m_selOutlineDoc/m_selBoundsDoc when invalid
    bool       dragProxyEligible() const;   // is the current gesture a proxy-able whole-selection affine drag?
    bool       buildDragProxies();          // capture static + moving bitmaps; false ⇒ ineligible/too big (fall back)
    void       clearDragProxy();            // drop the proxy bitmaps and force a full re-render next paint
    void       paintDragProxy(QPainter& g); // blit static + transformed moving bitmap + overlays
    QVector<double> m_guideX, m_guideY;   // active smart-guide lines (doc coords) during a drag
    QVector<double> m_guideHpos, m_guideVpos;   // persistent user guides (doc coords)
    enum GuideDrag { GDNone, GDNewH, GDNewV, GDMoveH, GDMoveV };
    GuideDrag m_guideDrag = GDNone; int m_dragGuideIdx = -1;   // interactive ruler-guide drag state
    // Pen: anchors + their OUTgoing bezier handle (incoming = mirror for smooth points).
    bool    m_penActive=false, m_penHandleDrag=false;
    QVector<QPointF> m_penAnchors, m_penHandles;
    int         m_penResume = -1;      // resuming an existing open path: its shape index (else -1)
    QPainterPath m_penResumeBase;      // the existing path (doc space, oriented so we extend from its END)
    QPainterPath buildPenPath(bool close) const;
    void    commitPen(bool close);     // finalise the pen: new path, or (m_penResume>=0) extend the existing one
    QVector<QPointF> m_freePts;
    bool    m_createConstrain = false;   // Ctrl held mid-draw → square/circle/angle-snap the new shape (preview reads this)
    double m_freeSmooth = 2.5;   // Freehand tool fit tolerance (px): larger = smoother/simpler curve
    // Node editor: parsed bezier nodes of the shape under edit, plus which part is grabbed.
    enum NodePart { NPAnchor, NPIn, NPOut };
    int     m_nodeShape = -1, m_nodeSub = -1, m_nodeIndex = -1;
    int     m_nodeChild = -1;   // >=0: node-editing a path CHILD inside the group m_nodeShape (node-edit-in-group)
    Shape*      nodeTargetShape();    // the shape whose nodes are being edited (a group child when m_nodeChild>=0)
    QTransform  nodeXf() const;       // that shape's effective doc transform (group.transform * child.transform)
    void        beginNodeEditChild(int groupIdx, int childIdx);   // node-edit a path inside a group without ungrouping
    NodePart m_nodePart = NPAnchor;
    // Drag-to-bend a segment (grabbed with no node/handle under the cursor): which segment + press state.
    int     m_bendSub = -1, m_bendIdx = -1; double m_bendT = 0;
    bool    m_bendActive = false;   // the bend drag has passed the move threshold (undo pushed, geometry changing)
    QPointF m_bendOut0, m_bendIn0, m_bendGrab;
    QVector<SubPath> m_nodes;
    QVector<QPair<int,int>> m_nodeSelSet;   // multi-selected nodes (subpath,index) for group move/delete
    void    beginNodeEdit(int shapeIdx);
    void    writeBackNodes();
    QPointF m_curDocPt;
    // In-canvas text editing: which shape is under the caret, the caret char index, and the selection
    // anchor (-1 = no selection). A blink timer toggles m_caretBlink.
    int     m_textEdit = -1, m_caret = 0, m_caretAnchor = -1;
    bool    m_caretBlink = true, m_textEditIsNew = false, m_textDragging = false;
    bool    m_smartQuotes = true;                        // auto-curl straight quotes as typed
    bool    m_spellLive = true;                          // draw live spell-check squiggles under misspelled words
    bool    m_outlineView = false;                       // wireframe/outline view mode (screen only)
    QTimer* m_caretTimer = nullptr;
    QTimer* m_docDebounceTimer = nullptr;   // coalesces per-frame documentChanged into documentChangedDebounced during drags

    // ---- Ctrl radial ("pie" / marking) menu: inner ring = tools, outer ring = the active tool's actions ----
    struct RadialItem { QString label; std::function<void()> action; bool current=false; int toolId=-1; QString iconName; };  // toolId>=0 → tool icon; else iconName → glyph; else the text label
    bool    m_radialActive=false;                // the pie is currently shown
    QPointF m_radialCenter;                      // where it opened (widget/view coords)
    QVector<RadialItem> m_radialInner, m_radialOuter;
    int     m_radialRing=-1, m_radialIndex=-1;   // hovered wedge: ring 0=tools,1=actions; index within it (-1 = none)
    QTimer* m_radialTimer=nullptr;               // press-and-hold delay before the pie appears (distinguishes Ctrl+key combos)
    void radialArm();                            // Ctrl pressed idle → start the hold timer
    void radialDisarm();                         // cancel a pending / open pie without acting
    void radialShowAtCursor();                   // timer fired → build items + show at the cursor
    void radialHide(bool activate);              // close; activate the hovered wedge if `activate`
    void radialHover(const QPointF& viewPt);     // update the hovered wedge from the cursor position
    int  radialHitTest(const QPointF& viewPt, int& ringOut) const;
    void radialBuildItems();                     // populate inner (tools) + outer (active tool's actions)
    void drawRadialMenu(QPainter& g);            // paint the two-ring pie (called from paintOverlays)
    void    textReplaceRange(int a, int b, const QString& ins);   // core edit: replace [a,b) with ins, undoable
    void    syncTextEditToDoc();   // after any doc-replacing op: drop the caret if its shape is gone / not text; else clamp
    void    resetTextEdit();   // clear all caret state + stop the blink timer (out-of-line: QTimer is fwd-declared here)
    // Mould / envelope perspective warp
    int     m_mouldShape = -1, m_mouldDrag = -1;
    QPointF m_mouldCorners[4], m_mouldLocal[4];
    void    initMould(int shapeIdx);
    void    applyMould();
    // Envelope (curved) mould state. m_mouldDrag encodes the grabbed handle: 0..3 corners, 4..11 the
    // eight edge controls. The pristine pre-warp shape is stashed so every edit re-warps from scratch.
    int     m_mouldMode = 0;                 // 0 perspective, 1 envelope
    QPointF m_mouldCtrl[8];                   // edge control points (doc space)
    Shape   m_mouldSource;                    // pristine copy of the moulded shape
    bool    m_mouldHasSource = false;
    void    bakeEnvelope();                    // warp m_mouldSource through the current cage into shape m_mouldShape
    // Drop the mould's cached shape reference. MUST be called whenever the shapes array is mutated
    // structurally (delete/reorder/reload) so a stale m_mouldShape can't bake onto the wrong object.
    void    invalidateMould(){ m_mouldShape=-1; m_mouldDrag=-1; m_mouldHasSource=false; }

    QVector<UndoSnap> m_undo, m_redo;
    QStringList m_undoLabels, m_redoLabels;   // parallel to m_undo/m_redo: the op name at each level (named undo/redo)
    QByteArray m_clip;          // in-app clipboard (JSON array of shapes)
    UndoSnap captureSnap() const;             // cheap COW snapshot of doc+selection (replaces the old JSON serialize)
    void applyUndoSnap(const UndoSnap& s);    // restore a snapshot (replaces the old JSON deserialize)
};

} // namespace xara
