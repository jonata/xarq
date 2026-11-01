// Adobe XD (.xd) → SVG pages, one per artboard, for the SVG importer.
#pragma once
#include <QByteArray>
#include <QMap>
#include <QString>
#include <QStringList>

namespace xara {
struct XdStats { int artboards = 0, pasteboardObjects = 0; QStringList names; };
// `zip` is the unpacked archive. Empty if it isn't an XD document, or has no artwork we can read.
QStringList xdToSvgPages(const QMap<QString,QByteArray>& zip, XdStats* stats = nullptr);
}
