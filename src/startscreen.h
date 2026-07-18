// startscreen.h — XarQ welcome / launcher screen (DS4). Shown on a clean interactive launch:
// wordmark + mark, "New document", a template-preset grid, and recent files. Matches the XarQ
// design system's `ui_kits/start` (functional core; no marketing sidebar/upsell).
#pragma once
#include <functional>

class QWidget;

namespace xara {
class MainWindow;

class StartScreen {
public:
    // Build the full welcome UI (sidebar + main area) as a standalone widget so it can be either
    // embedded in the canvas area (empty state) or hosted inside the modal dialog. `onChosen` (if set)
    // is invoked after any choice that creates/opens a document — the modal uses it to accept()/close.
    static QWidget* buildPanel(MainWindow* mw, std::function<void()> onChosen);
    static void show(MainWindow* mw);   // modal; acts on the MainWindow (new/template/open), else just closes
};

} // namespace xara
