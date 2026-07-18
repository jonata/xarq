// photoops.h — content-aware ("magic") photo resize via seam carving.
// Removes low-energy vertical/horizontal seams so the image changes size while
// salient content stays undistorted. Self-contained (operates on a QImage).
#pragma once
#include <QImage>

#include <QVector>
namespace xara {
// Resize `src` to (targetW x targetH) by carving seams. Only supports shrinking each
// axis (targetW<=src.width(), targetH<=src.height()); other axes are returned scaled.
QImage contentAwareResize(const QImage& src, int targetW, int targetH);

// Stitch overlapping photos (ordered left-to-right) into one panorama. Each adjacent pair is
// aligned by a similarity transform (rotation+scale+translation) estimated from matched corner
// features via RANSAC, falling back to a pure-translation SAD search when matching is weak; the
// images are then composited onto a union canvas with a linear feather cross-fade in the overlaps.
QImage stitchPanorama(const QVector<QImage>& imgs);

// Multi-band (Laplacian-pyramid) blend of two equal-size images. `maskA` is a grayscale weight
// for image A (255 = all A); low-frequency bands cross-fade over a wide region so exposure
// differences vanish while high-frequency detail stays sharp. Used by the stitcher; exposed for testing.
QImage multiBandBlend(const QImage& A, const QImage& B, const QImage& maskA, int levels = 6);

// ---- destructive photo filters (operate on a QImage; render-pipeline-independent) ----
QImage gaussianBlurImage(const QImage& src, double radius);          // 3× box-blur ≈ Gaussian
QImage unsharpMask(const QImage& src, double amount, double radius = 2.0);   // sharpen = src + amount·(src−blur)
QImage vignette(const QImage& src, double strength);                // radial corner darkening (0..1)
QImage lensCorrect(const QImage& src, double k1, double k2 = 0.0);   // Brown radial distortion correction (barrel/pincushion)
QImage tonemapHDR(const QImage& src, double exposure, double white = 2.0);  // extended-Reinhard HDR-look tone-map

// Brown-Lowe exposure/gain compensation: solve per-image gains g minimising Σ N_ij (g_i·I_ij − g_j·I_ji)²
// plus a prior pulling g→1. Iij[i][j] = mean luminance of image i over its overlap with j; Nij[i][j] = overlap
// pixel count (symmetric). Returns n gains (clamped to a sane range). Exposed for testing.
QVector<double> estimateGains(int n, const QVector<QVector<double>>& Iij, const QVector<QVector<double>>& Nij);
}
#include <QPointF>
#include <QTransform>
namespace xara {
// RANSAC similarity fit: returns the transform T minimising |dst - T(src)| over inlier
// correspondences (rotation+uniform-scale+translation). `src`/`dst` must be equal length.
QTransform estimateSimilarityTransform(const QVector<QPointF>& src, const QVector<QPointF>& dst, int* inliers = nullptr);
// Estimate the similarity transform mapping B's pixels into A's coordinate frame (feature
// matching + RANSAC, translation fallback). Exposed for testing the alignment pipeline.
QTransform alignImagePair(const QImage& A, const QImage& B);
// Apply photo adjustments (brightness/contrast/saturation/greyscale/auto-levels/red-eye/blur/
// sharpen/vignette). Used by the Adjust Photo dialog + the non-destructive re-bake path.
QImage adjustPhoto(const QImage& src0, int bright, int contrast, int sat, bool gray, bool autoLevel,
                   bool redeye=false, int blur=0, int sharpen=0, int vignette=0, int lens=0, int hdr=0,
                   int blackPt=0, int whitePt=255, double gamma=1.0);
}
