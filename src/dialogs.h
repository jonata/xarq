// dialogs.h — modal dialogs (Color Editor, Options, …). Kept separate from the
// dockable panels so the growing UI surface stays organised.
#pragma once
#include <QColor>
class QWidget;

namespace xara {
class Canvas;

// A modern colour picker: 2D saturation/value square, hue strip, alpha, the four
// colour models (RGB/HSV/HSL/CMYK) with a hex field, old/new preview, and the
// document's named-colour swatches as quick-picks. Replaces QColorDialog.
class ColorEditor {
public:
    static QColor pick(QWidget* parent, const QColor& initial, Canvas* canvas=nullptr, bool allowAlpha=true);
};

// Full tabbed program/document options (Batch H).
class OptionsDialog {
public:
    static void show(QWidget* parent, Canvas* canvas);
};

// Find & Replace across the document's text objects (Batch E).
class FindReplace {
public:
    static void show(QWidget* parent, Canvas* canvas);
};

// Non-destructive photo adjustments for the selected bitmap object (Batch K).
class PhotoDialog {
public:
    static void show(QWidget* parent, Canvas* canvas);
};

// Input Levels for the selected photo: histogram + draggable black/gamma/white markers + live preview.
class LevelsDialog {
public:
    static void show(QWidget* parent, Canvas* canvas);
};

// Selection-wide recolour (hue/sat/lightness) + replace-colour (Batch ~AAA).
class RecolourDialog {
public:
    static void show(QWidget* parent, Canvas* canvas);
};

// Guided Cutout Studio: paint keep/remove scribbles → graph-cut background removal (re-editable mask).
class CutoutStudioDialog {
public:
    static void show(QWidget* parent, Canvas* canvas);
};

// Photo perspective / keystone correction: vertical + horizontal keystone sliders with live preview.
class PerspectiveDialog {
public:
    static void show(QWidget* parent, Canvas* canvas);
};

// Mesh gradient editor: an arbitrary R×C grid of colour nodes (add rows/cols, recolour each node).
class MeshGradientDialog {
public:
    static void show(QWidget* parent, Canvas* canvas);
};

// Cascading named text styles: create / apply / edit (Batch T1).
class TextStyleDialog {
public:
    static void show(QWidget* parent, Canvas* canvas);
};

// Rich text: format a character range of the selected Text object (Batch T2).
class RichTextDialog {
public:
    static void show(QWidget* parent, Canvas* canvas);
};

// Alignment dialog — align/distribute relative to selection or page (Batch M).
class AlignDialog {
public:
    static void show(QWidget* parent, Canvas* canvas);
};

// Repeat objects into a grid array (Batch M).
class RepeatDialog {
public:
    static void show(QWidget* parent, Canvas* canvas);
};

// Insert a special character / symbol into the selected text (Batch V).
class SymbolDialog {
public:
    static void show(QWidget* parent, Canvas* canvas);
};

} // namespace xara
