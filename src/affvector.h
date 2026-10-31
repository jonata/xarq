#pragma once
// Serif Affinity documents (.afdesign / .afphoto / .afpub / .aftemplate / .af) — a first-party reader
// for the real artwork, not just the embedded preview. The container and its tagged object tree are
// parsed here and re-emitted as one standard-SVG string per spread, which the caller hands to XarQ's
// SVG importer (Canvas::importVectorSvgPages) exactly like the other native vector formats.
#include <QString>
#include <QStringList>
#include <QVector>
#include <QByteArray>
#include <cstdint>

namespace xara {

// True when the bytes start with the Affinity container signature (00 FF 4B 41 + "nsrP").
bool affinityIsContainer(const uint8_t* p, int64_t n);

// The document's own preview PNG, located exactly (header thmb_offset + the 29-byte "Thmb" block)
// rather than by scanning for a PNG signature — an .afdesign also stores placed images verbatim, so
// a signature scan can return a user asset instead of the page preview. Empty when the file was
// saved without "Save thumbnails with documents".
QByteArray affinityPreviewPng(const uint8_t* p, int64_t n);

// One SVG document per spread (page). Empty when the file is not an Affinity container, when the
// object tree cannot be parsed, or when nothing drawable came out — the caller then falls back to
// the embedded preview.
QStringList affinityToSvgPages(const uint8_t* p, int64_t n);

// Diagnostics for the self-test / status text: how many drawables the last conversion emitted and
// how many nodes it had to skip (raster layers, unsupported live shapes).
struct AffinityPageInfo {            // in the page's own coordinates
    QVector<double> guidesH, guidesV;
    double marginL=0, marginT=0, marginR=0, marginB=0, width=0, height=0;
};
struct AffinityStats {
    QVector<AffinityPageInfo> pageInfo;
    double bleed=0;
    int artboards=0;     // artboards imported as pages of their own
    int pixelMasks=0;    // MRst pixel masks applied
    int extraMasks=0;    // further pixel masks on one object, beyond the one the model holds
    int ignoredAdjustments=0;  // adjustment layers of a kind not read
    int ignoredFilters=0;      // live filters other than blur and noise
    int pages=0; int shapes=0; int skippedRaster=0; int skippedShape=0; int images=0; int texts=0;
    int rasters=0;   // pixel layers assembled from their 256x256 channel planes
    int masks=0;     // AdCh clip masks re-emitted as SVG clipPaths (vector layers use these far more than pixel ones)
    // Integrity signals the format hands us for free — used by the self-test as a real oracle:
    // every chunk is CRC32-verified, the object tree should consume doc.dat to the exact last byte,
    // and a path's computed cubic-Bezier bounds must reproduce the file's own stored CvsB.
    int streamsOk=0; int streamsBad=0;
    bool exactParse=false; long long treeBytes=0; long long treeConsumed=0;
    int bboxOk=0; int bboxBad=0;
};
QStringList affinityToSvgPages(const uint8_t* p, int64_t n, AffinityStats* stats);

// Decode and CRC32-verify EVERY stream the container holds. The stored checksum covers the fully
// restored bytes (post-decompress, post-predictor), so this is an exact integrity check on the whole
// container implementation — the self-test and the diagnostics use it as an oracle. Returns false when
// the file is not a readable Affinity container at all.
bool affinityVerifyStreams(const uint8_t* p, int64_t n, int* okOut, int* badOut);
inline bool affinityVerifyStreams(const QByteArray& b, int* okOut, int* badOut){
    return affinityVerifyStreams(reinterpret_cast<const uint8_t*>(b.constData()), (int64_t)b.size(), okOut, badOut);
}

// Whether this build can decompress zstd chunks (container v11 / Affinity 2.x documents).
bool affinityZstdAvailable();

inline bool affinityIsContainer(const QByteArray& b){ return affinityIsContainer(reinterpret_cast<const uint8_t*>(b.constData()), (int64_t)b.size()); }
inline QByteArray affinityPreviewPng(const QByteArray& b){ return affinityPreviewPng(reinterpret_cast<const uint8_t*>(b.constData()), (int64_t)b.size()); }
inline QStringList affinityToSvgPages(const QByteArray& b){ return affinityToSvgPages(reinterpret_cast<const uint8_t*>(b.constData()), (int64_t)b.size()); }
inline QStringList affinityToSvgPages(const QByteArray& b, AffinityStats* st){ return affinityToSvgPages(reinterpret_cast<const uint8_t*>(b.constData()), (int64_t)b.size(), st); }

}   // namespace xara
