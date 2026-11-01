// Adobe FXG (Flash XML Graphics) → SVG, for the SVG importer.
#pragma once
#include <QByteArray>
#include <QString>

namespace xara {
// One standard-SVG page, or an empty string if `data` isn't FXG. `baseDir` resolves @Embed() images.
QString fxgToSvg(const QByteArray& data, const QString& baseDir);
}
