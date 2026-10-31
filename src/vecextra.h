#pragma once
// Native VECTOR importers that parse a vector file and emit an SVG document string,
// which the caller feeds to XarQ's SVG parser (importVectorSvgPages) to get editable shapes.
// Same bridge as cdrToSvgPages / wpgToSvgPages etc.
#include <QString>
#include <QByteArray>
#include <cstdint>
namespace xara {
QString shapefileToSvg(const uint8_t* p, int64_t n);   // ESRI Shapefile (.shp)
QString gpxToSvg      (const uint8_t* p, int64_t n);    // GPX (GPS Exchange)
QString xfigToSvg     (const uint8_t* p, int64_t n);    // Xfig (.fig)
QString cgmToSvg      (const uint8_t* p, int64_t n);    // CGM binary (ISO 8632)
QString svgzToSvg     (const uint8_t* p, int64_t n);    // gzipped SVG (.svgz)
QByteArray gunzipBytes(const uint8_t* p, int64_t n);      // raw gzip -> bytes (an .svgz may hold a raster)
QString gerberToSvg   (const uint8_t* p, int64_t n);    // Gerber RS-274X (PCB image)
QString excellonToSvg (const uint8_t* p, int64_t n);    // Excellon / XNC drill
QString lottieToSvg   (const uint8_t* p, int64_t n);    // Lottie / Bodymovin (static first frame)
QString sketchToSvg   (const uint8_t* p, int64_t n);    // Sketch (.sketch ZIP+JSON, first page)
QString tgsToSvg      (const uint8_t* p, int64_t n);    // Telegram sticker (gzipped Lottie)
QString topojsonToSvg (const uint8_t* p, int64_t n);    // TopoJSON (topology-encoded GeoJSON)
QString vmlToSvg      (const uint8_t* p, int64_t n);    // VML (legacy MS vector markup)
QString xpsToSvg      (const uint8_t* p, int64_t n);    // XPS / OpenXPS (OPC FixedPage)
QString scribusToSvg  (const uint8_t* p, int64_t n);    // Scribus (.sla/.scd DTP)
// QByteArray convenience overloads
inline QString shapefileToSvg(const QByteArray& b){ return shapefileToSvg(reinterpret_cast<const uint8_t*>(b.constData()), (int64_t)b.size()); }
inline QString gpxToSvg      (const QByteArray& b){ return gpxToSvg      (reinterpret_cast<const uint8_t*>(b.constData()), (int64_t)b.size()); }
inline QString xfigToSvg     (const QByteArray& b){ return xfigToSvg     (reinterpret_cast<const uint8_t*>(b.constData()), (int64_t)b.size()); }
inline QString cgmToSvg      (const QByteArray& b){ return cgmToSvg      (reinterpret_cast<const uint8_t*>(b.constData()), (int64_t)b.size()); }
inline QString svgzToSvg     (const QByteArray& b){ return svgzToSvg     (reinterpret_cast<const uint8_t*>(b.constData()), (int64_t)b.size()); }
inline QByteArray gunzipBytes(const QByteArray& b){ return gunzipBytes(reinterpret_cast<const uint8_t*>(b.constData()), (int64_t)b.size()); }
inline QString gerberToSvg   (const QByteArray& b){ return gerberToSvg   (reinterpret_cast<const uint8_t*>(b.constData()), (int64_t)b.size()); }
inline QString excellonToSvg (const QByteArray& b){ return excellonToSvg (reinterpret_cast<const uint8_t*>(b.constData()), (int64_t)b.size()); }
inline QString lottieToSvg   (const QByteArray& b){ return lottieToSvg   (reinterpret_cast<const uint8_t*>(b.constData()), (int64_t)b.size()); }
inline QString sketchToSvg   (const QByteArray& b){ return sketchToSvg   (reinterpret_cast<const uint8_t*>(b.constData()), (int64_t)b.size()); }
inline QString tgsToSvg      (const QByteArray& b){ return tgsToSvg      (reinterpret_cast<const uint8_t*>(b.constData()), (int64_t)b.size()); }
inline QString topojsonToSvg (const QByteArray& b){ return topojsonToSvg (reinterpret_cast<const uint8_t*>(b.constData()), (int64_t)b.size()); }
inline QString vmlToSvg      (const QByteArray& b){ return vmlToSvg      (reinterpret_cast<const uint8_t*>(b.constData()), (int64_t)b.size()); }
inline QString xpsToSvg      (const QByteArray& b){ return xpsToSvg      (reinterpret_cast<const uint8_t*>(b.constData()), (int64_t)b.size()); }
inline QString scribusToSvg  (const QByteArray& b){ return scribusToSvg  (reinterpret_cast<const uint8_t*>(b.constData()), (int64_t)b.size()); }
}
