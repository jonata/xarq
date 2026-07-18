// icons.h — crisp vector tool icons drawn at runtime with QPainter (no asset
// files). One clean line-art glyph per tool, in the app's monochrome style.
#pragma once
#include <QIcon>
#include "canvas.h"
namespace xara {
QIcon toolIcon(Tool t, int size=22, const QColor& col=QColor(220,223,230));
QIcon actionIcon(const QString& name, int size=20, const QColor& col=QColor(210,214,222));
} // namespace xara
