// prepress.h — print/interop importers & exporters (Module 3): EPS/AI + PDF vector
// import, PSD flattened import, CMYK colour separations, crop/registration marks.
#pragma once
#include <QMap>
#include "model.h"
#include <QByteArray>
#include <QImage>

namespace xara {

// EPS / classic-AI (PostScript vector subset) → a Document of Path shapes.
// Handles moveto/lineto/curveto/closepath, fill/stroke, rgb/gray/hsb/cmyk colour,
// gsave/grestore, translate/scale/rotate/concat, and %%BoundingBox. Text/images skipped.
Document importEps(const QByteArray& data);

// PDF (single page, content-stream vectors) → a Document of Path shapes. Parses the object
// table, FlateDecode streams, and the content operators (m l c re f S rg g k cm q Q).
Document importPdf(const QByteArray& data);

// AI: modern Adobe Illustrator files are PDF-compatible → route to importPdf when a %PDF header
// is present, else fall back to the classic PostScript path (importEps).
Document importAi(const QByteArray& data);

// AutoCAD DXF (ASCII): parse the ENTITIES section (LINE / LWPOLYLINE / POLYLINE / CIRCLE / ARC)
// into stroked Path shapes.
Document importDxf(const QByteArray& data);

// Windows Metafile (WMF): placeable/standard header + the common vector records
// (rect / ellipse / polygon / polyline / moveto-lineto, pen & brush objects) → editable Path shapes.
Document importWmf(const QByteArray& data);

// Enhanced Metafile (EMF): 32-bit records — polygon(16)/polyline(16)/rect/ellipse/moveto/lineto
// with CREATEPEN/CREATEBRUSHINDIRECT/SELECTOBJECT (incl. stock objects) → editable Path shapes.
Document importEmf(const QByteArray& data);

// PSD (Photoshop) flattened composite → QImage (the merged preview: RAW or PackBits RLE).
QImage importPsd(const QByteArray& data);
// Raster-only PostScript/EPS written by ImageMagick's "DisplayImage" prologue: the pixels are a
// plain hex stream rather than drawing operators, so they can be recovered without an interpreter.
QImage psDisplayImageRaster(const QByteArray& data);
// Raster-only PDF: the page is a single image XObject rather than drawing operators.
QImage pdfRasterImage(const QByteArray& data);

// PSD → a Document: each Photoshop layer becomes a positioned bitmap shape (opacity + blend mode)
// so layered files import editable. Falls back to a single flattened-composite shape if there are
// no layers (or they can't be parsed).
struct PsdImportStats {
    int groups = 0, vectorLayers = 0, textLayers = 0, rasterText = 0, adjustments = 0, skippedAdjustments = 0, effectLayers = 0;
    QVector<double> guidesH, guidesV;
};
Document importPsdDocument(const QByteArray& data, PsdImportStats* stats = nullptr);
// The PSD route's bitmap layer and blend keys ("mul ", "scrn", "hue "…), for the other layered formats.
Shape psdBitmapShape(const QImage& img, int x, int y, const QString& name);
void psdApplyBlend(Shape& s, const QByteArray& key);
// A greyscale mask over `area` of the document, as the object's transparency (luminance = opacity).
void psdDocMask(Shape& s, const QImage& gray, const QRect& area);

// PaintShop Pro → a Document: raster layers keep their blend, opacity and mask; vector layers' polylines
// stay editable. What the format has but this doesn't read is counted, so the importer can say so.
struct PspImportStats {
    int vectorShapes = 0, skippedText = 0, skippedShapes = 0, unsupportedPaints = 0;
    int adjustments = 0, groups = 0, maskLayers = 0, artMedia = 0, dissolve = 0;
};
Document importPspDocument(const QByteArray& data, PspImportStats* stats = nullptr);

// Clip Studio Paint → a Document: a bitmap per layer, in folders, with blend, opacity, masks and clipping.
struct ClipImportStats { int pixelLayers = 0, paper = 0, folders = 0, masks = 0, clipped = 0, corrections = 0, fills = 0, placedImages = 0, textObjects = 0, vectorStrokes = 0, tones = 0, skippedCorrections = 0; };
Document importClipDocument(const QByteArray& data, ClipImportStats* stats = nullptr);
QImage clipPreview(const QByteArray& data);

// Procreate → a Document: its layers and groups, with blend, opacity, masks and clipping. `zip` is the unpacked file.
struct ProcreateImportStats { int layers = 0, groups = 0, masks = 0, clipped = 0, badTiles = 0; };
Document importProcreateDocument(const QMap<QString,QByteArray>& zip, ProcreateImportStats* stats = nullptr);

// GIMP's XCF → a Document: its layers and groups with blend mode, opacity, visibility and masks; high bit
// depths and linear-light images come in as 8-bit sRGB. gzip-compressed files are unpacked, and bzip2 ones
// when built with bzip2 (stats->bzip2 says one was refused).
struct XcfImportStats { int layers = 0, groups = 0, masks = 0, unknownModes = 0, dissolve = 0, badTiles = 0; bool linear = false, bzip2 = false; };
Document importXcfDocument(const QByteArray& data, XcfImportStats* stats = nullptr);
bool xcfBzip2Supported();

// Krita → a Document: paint layers (with transparency masks), groups and vector layers, with blend, opacity
// and visibility. `zip` is the unpacked file. Filter masks and clone, file, fill and adjustment layers are counted.
struct KraImportStats { int layers = 0, groups = 0, vectorLayers = 0, masks = 0, filters = 0, otherLayers = 0, unknownModes = 0, badTiles = 0, unsupportedColour = 0; };
Document importKraDocument(const QMap<QString,QByteArray>& zip, KraImportStats* stats = nullptr);

// OpenRaster → a Document: its layers and nested stacks with opacity, visibility, blend and isolation.
struct OraImportStats { int layers = 0, groups = 0, unknownModes = 0, missing = 0; };
Document importOraDocument(const QMap<QString,QByteArray>& zip, OraImportStats* stats = nullptr);

// Paint.NET → a Document: its layers with name, opacity, visibility and blend mode. Layers that can't be
// read are counted; with none readable, the file's thumbnail stands in.
struct PdnImportStats { int layers = 0, hidden = 0, empty = 0, unknownModes = 0, missing = 0, badChunks = 0; bool thumbnailOnly = false; };
Document importPdnDocument(const QByteArray& data, PdnImportStats* stats = nullptr);

// MediBang Paint / FireAlpaca (.mdp) → a Document: layers and folders with name, opacity, visibility, blend
// mode and clipping. Layer masks aren't applied (counted); with no layer readable, the thumbnail stands in.
struct MdpImportStats { int layers = 0, groups = 0, hidden = 0, empty = 0, clipped = 0, masks = 0, unknownModes = 0, unknownTypes = 0, badTiles = 0; bool thumbnailOnly = false; };
Document importMdpDocument(const QByteArray& data, MdpImportStats* stats = nullptr);

// Corel PHOTO-PAINT → a Document: the background, then each object with its mask, opacity and visibility.
// Tiles that can't be read are counted; with nothing else readable, the file's thumbnail stands in.
struct CptImportStats { int objects = 0, hidden = 0, savedMasks = 0, unknownTiles = 0, unknownDepth = 0; bool thumbnailOnly = false; };
Document importCptDocument(const QByteArray& data, CptImportStats* stats = nullptr);

// Pixelmator Pro → a Document: raster, shape and text layers, groups, blends, masks, shadows and blur. `files`
// is the unpacked file or folder, by relative path.
struct PxdImportStats { int rasterLayers = 0, vectorLayers = 0, textLayers = 0, groups = 0, masks = 0, clipped = 0, adjustmentLayers = 0, adjustedLayers = 0, otherEffects = 0, innerShadows = 0, unappliedMasks = 0, unknownLayers = 0; };
Document importPxdDocument(const QMap<QString,QByteArray>& files, PxdImportStats* stats = nullptr);

// Convert an sRGB colour to naive CMYK (0..1 each). Shared by import (setcmykcolor) + separations.
void rgbToCmyk(const QColor& c, double& C, double& M, double& Y, double& K);
QByteArray deflateZlib(const QByteArray& src);   // zlib deflate → PDF /FlateDecode-compatible bytes

// Approximate CMYK soft-proof (no ICC): how a colour/image would look printed on coated offset stock —
// our own separation + dot gain + cream paper + gamut compression. View-only; see prepress.cpp.
QColor softProofCmyk(const QColor& in);
void   softProofImage(QImage& im);   // apply the proof to every opaque pixel, in place
void   gamutWarnImage(QImage& im);   // paint every out-of-CMYK-gamut pixel a flat warning grey (view-only)
int    totalAreaCoverage(const QColor& c);   // total ink coverage 0..400 (%) = round((C+M+Y+K)*100)
void   tacWarnImage(QImage& im, int limitPct);   // paint pixels whose ink coverage exceeds limitPct a warning colour

} // namespace xara
