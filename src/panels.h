// panels.h — the dockable side panels: Properties inspector (fill/stroke/effects/
// geometry), Layers (object list) and a quick Colour/Swatches strip. They observe
// the Canvas selection and push edits back through it.
#pragma once
#include <QWidget>
#include "canvas.h"

class QListWidget; class QComboBox; class QSlider; class QDoubleSpinBox;
class QSpinBox; class QCheckBox; class QPushButton; class QLabel; class QVBoxLayout;
class QHBoxLayout; class QTimer;

namespace xara {

class ColorButton; class GradientBar;

// ---- InfoBar: Xara's signature per-tool options bar (horizontal, top) -------
// Rebuilds its controls whenever the active tool changes; the Selector bar's
// X/Y/W/H/angle fields also re-sync with the selection.
class InfoBar : public QWidget {
    Q_OBJECT
public:
    explicit InfoBar(Canvas* c, QWidget* parent=nullptr);
public slots:
    void rebuild();    // build the controls for the current tool
    void refresh();    // sync selector fields with the current selection
private:
    Canvas* m_c; bool m_block=false;
    QHBoxLayout* m_lay=nullptr;
    QDoubleSpinBox *m_x=nullptr,*m_y=nullptr,*m_w=nullptr,*m_h=nullptr,*m_rot=nullptr;
    QCheckBox* m_lock=nullptr;
    QTimer* m_syncTimer=nullptr;   // debounced rebuild: re-sync per-tool controls after canvas-driven model changes settle
};

// ---- Colour Line: the bottom palette strip (click=fill, right-click=line) ---
class ColourLine : public QWidget {
    Q_OBJECT
public:
    explicit ColourLine(Canvas* c, QWidget* parent=nullptr);
    QSize sizeHint() const override;
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
private:
    Canvas* m_c; int m_hover=-1;
    QPoint m_pressPos; int m_pressIdx=-2; Qt::MouseButton m_pressBtn=Qt::NoButton; bool m_dragging=false;
    int swatchAt(const QPoint& p) const;
    void applyColour(int idx, bool line);
};

// ---- Properties / inspector -----------------------------------------------
class PropertiesPanel : public QWidget {
    Q_OBJECT
public:
    explicit PropertiesPanel(Canvas* c, QWidget* parent=nullptr);
public slots:
    void refresh();
private:
    Canvas* m_c;
    bool    m_block=false;
    void apply(const std::function<void(Shape&)>& fn, const QString& label);
    // fill
    QComboBox* m_fillType; ColorButton* m_fillColor; GradientBar* m_gradBar; QPushButton* m_addStop;
    QComboBox* m_fillSpread; QCheckBox* m_fillRainbow; QWidget* m_gradOpts;
    QWidget* m_meshRow; ColorButton* m_meshCol[4];
    QWidget* m_bmpRow; QPushButton* m_bmpLoad; QCheckBox* m_bmpTile; QCheckBox* m_bmpContone;
    // stroke
    QCheckBox* m_strokeOn; ColorButton* m_strokeColor; QDoubleSpinBox* m_strokeW; QComboBox* m_cap, *m_join;
    // object
    QSlider* m_opacity; QLabel* m_opacityLbl; QComboBox* m_blend;
    // transparency
    QComboBox* m_transpType; QComboBox* m_transpEffect; QSlider* m_transpAmt; QLabel* m_transpAmtLbl;
    // geometry
    QWidget* m_geomRow; QLabel* m_geomLbl; QDoubleSpinBox* m_corner; QSpinBox* m_sides; QCheckBox* m_star;
    // effects
    QCheckBox* m_shadowOn; QDoubleSpinBox* m_shadowBlur; ColorButton* m_shadowColor; QComboBox* m_shadowType=nullptr;
    QDoubleSpinBox* m_fillProfB=nullptr; QDoubleSpinBox* m_fillProfG=nullptr;
    QCheckBox* m_contourOn; QDoubleSpinBox* m_contourW; QSpinBox* m_contourSteps;
    QCheckBox* m_bevelOn; QDoubleSpinBox* m_bevelDepth;
    QLabel* m_titleLbl;
};

// ---- Layers ----------------------------------------------------------------
class LayersPanel : public QWidget {
    Q_OBJECT
public:
    explicit LayersPanel(Canvas* c, QWidget* parent=nullptr);
public slots:
    void refresh();
private:
    Canvas* m_c; QListWidget* m_list; bool m_block=false;
    void onRowChanged();
};

// ---- Colour / swatches -----------------------------------------------------
class ColorPanel : public QWidget {
    Q_OBJECT
public:
    explicit ColorPanel(Canvas* c, QWidget* parent=nullptr);
private:
    Canvas* m_c;
};

} // namespace xara
