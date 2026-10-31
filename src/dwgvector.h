#pragma once
// dwgvector.h — AutoCAD DWG import via GNU LibreDWG (the free DWG reader), isolated to dwgvector.cpp.
// DWG is a closed binary CAD format with no in-tree native decoder; libredwg is a linked library (first-party
// policy: a real library C API, never a shelled-out CLI). We walk libredwg's parsed model-space entity tree
// and re-emit the 2D geometry as an in-memory DXF ENTITIES stream, then hand it to the existing hardened
// importDxf() so all coordinate/normalisation/robustness logic lives in one place.
// Compiles to a stub (reports unavailable, returns an empty Document) when libredwg is absent at configure time.
#include "model.h"
#include <QString>
namespace xara {
bool dwgAvailable();                       // true iff built against libredwg
Document dwgToDocument(const QString& path);   // DWG file → Document of stroked Path shapes (empty on failure)
}
