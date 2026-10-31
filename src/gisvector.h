#pragma once
// gisvector.h — optional vector import via two heavyweight libraries, each isolated to gisvector.cpp:
//   • GDAL/OGR   — the geospatial vector cluster: DGN, MapInfo TAB/MIF, GeoPackage, S-57, GML, FileGDB…
//     (produces an SVG string from a FILE path, since many GIS formats are multi-file).
//   • Graphviz   — DOT/GV graph description, laid out by the `dot` engine and rendered to SVG.
// Both compile to stubs (report unavailable) when their -dev package is absent at configure time.
#include <QString>
#include <QByteArray>
namespace xara {
bool ogrAvailable();  QString ogrToSvg(const QString& path);   // GDAL/OGR: any supported GIS vector → SVG
bool dotAvailable();  QString dotToSvg(const QByteArray& data);// Graphviz: DOT source → laid-out SVG
}
