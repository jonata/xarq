#pragma once
#include <QObject>

class QDialog;

namespace xara {

// Subtitld's dialog design, given to every XarQ dialog without touching the ~150 places that build one:
// a frameless window painted as one gradient, the title top-left, the close button in a slanted notch
// top-right, Cancel as plain text bottom-left and the default button on a sky tab bottom-right.
// Installed on qApp; it wraps each dialog on its first Polish, before it is shown. A dialog that is
// already frameless (it draws its own chrome) is left alone, as is one with xqChrome=false.
class DialogChrome : public QObject {
public:
    using QObject::QObject;
    static void apply(QDialog* d);
protected:
    bool eventFilter(QObject* o, QEvent* e) override;
};

} // namespace xara
