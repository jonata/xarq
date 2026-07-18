// qr.h — QR-code generation via a dlopen wrapper over the system libqrencode (like icc/raw/hyphen).
// Returns an empty image when the library isn't installed, so callers degrade gracefully.
#pragma once
#include <QImage>
#include <QString>

namespace xara {
// Encode `text` as a QR code (EC level M, byte mode, auto version). Rendered black-on-white with a
// 4-module quiet zone, each module `scale` px. Empty QImage if libqrencode is unavailable / encoding fails.
QImage makeQrCode(const QString& text, int scale = 8);
bool qrAvailable();
}
