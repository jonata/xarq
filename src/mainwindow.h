// mainwindow.h — the modern application shell: left icon tool rail, top action
// bar, central Canvas, right inspector docks, status bar.
#pragma once
#include <QMainWindow>
#include <QHash>
#include <QKeySequence>
#include <QVector>
#include <QPair>
#include <functional>
#include "canvas.h"

class QButtonGroup; class QLabel; class QToolButton; class QScrollArea; class QTabWidget; class QMenuBar; class QStackedWidget;
class QGraphicsOpacityEffect; class QPropertyAnimation;
int runSelfTest();   // deterministic test harness (selftest.cpp); befriended so it can reach empty-state internals

namespace xara {
class PropertiesPanel; class LayersPanel; class ColorPanel; class InfoBar; class ColourLine; class CollapsibleSection;

class MainWindow : public QMainWindow {
    Q_OBJECT
    friend int ::runSelfTest();   // the headless test harness drives the private empty-state entry
public:
    MainWindow();
    Canvas* canvas() const { return m_canvas; }
    bool bitmapExportDialog(const QString& initialPath);   // rich two-region Export bitmap dialog (preview + options rail); public for the --shot hook
    // Keyboard shortcut customization — public for introspection/tests.
    void applyShortcutOverrides();                            // apply the user's saved (QSettings) shortcut overrides
    QStringList  commandPaths() const;                        // every menu command's stable path key
    QKeySequence shortcutForCommand(const QString& path) const;  // its current shortcut ("" if unknown)

    // Multi-document tabs — public for introspection/tests.
    int  tabCount() const;                                    // number of open document tabs
    int  activeTab() const { return m_activeTab; }
    void newDocumentTab();                                    // open a fresh blank document in a new tab
    void switchToTab(int idx);                                // stash the active tab, activate `idx`
    void closeTab(int idx);                                   // close a tab (always keeps at least one)

    // XarQ Start / welcome screen (DS4)
    void setShowWelcome(bool b){ m_startupDialogs=b; }        // false in headless/test to suppress launch dialogs
    void setDeferStartup(bool b){ m_deferStartup=b; }         // true when a splash (driven by main) will run the startup sequence
    void runStartupSequence();       // offer recovery + show the welcome screen (called by main() after the splash, or by the ctor)
    void ensureLandingTab();         // create the first tab from the current canvas if none exists yet (deferred launch)
    void showStartScreen();                                   // the welcome launcher
    QStringList recentFiles() const;                          // most-recent-first, QSettings-backed
    void addRecentFile(const QString& path);                  // record a file as recently opened/saved
    void openPath(const QString& p);                          // open ANY supported file into a tab as a document (Start screen + recents)
    static QString supportedOpenFilter();                     // File-dialog filter covering every openable/importable format
    void newFromTemplate(int kind);                           // new document from a named template, in a tab
    void newBlankSized(double w, double h);                    // new blank document at a custom page size, in a tab (Start screen "Custom size")
    // Crash recovery is surfaced on the welcome screen (no modal): the Start screen lists these and calls
    // recoverAll()/discardRecovery() when the user chooses. Public so startscreen.cpp can build the section.
    QVector<QPair<QString,QString>> recoveryEntries() const;  // (snapshotFile, originalPath) per recoverable doc, existing files only
    void recoverAll();                                        // restore every recovery snapshot into a tab, then show the workspace
    void discardRecovery();                                   // delete the recovery snapshots and refresh the welcome screen
protected:
    void closeEvent(class QCloseEvent* e) override;           // warn on unsaved tabs + clear recovery on clean exit
    void changeEvent(class QEvent* e) override;               // keep the frameless maximize/restore glyph in sync
    void resizeEvent(class QResizeEvent* e) override;         // re-elide the centred title for the new width
    bool eventFilter(QObject* o, QEvent* e) override;         // reposition the floating tool rail when the canvas resizes
private:
    void buildToolRail();
    void buildTopBar();
    void buildDocks();
    void buildMenus();
    QToolButton* railButton(Tool t, const QString& tip, const QString& shortcut);

    void newFile(); void openFile(); bool saveFile(); bool saveFileAs(); bool saveTo(const QString& p); void exportImage();
    bool importIntoDoc(const QString& p, const std::function<bool()>& imp);   // import into the current doc + add to Recent on success
    void pageSetup(); void printDocument();
    void about();
    void updateTitle();
    void seedDemo();
    QString recoveryDir() const;     // app-data dir holding recovery snapshots
    QString recoveryPath() const;    // legacy single-slot recovery file (still honoured on restore)
    QString recoveryManifest() const;// lists every tab's recovery file + its real path
    bool m_recoveryDirty{false};     // some tab changed since the last recovery snapshot
    void autosave();                 // periodic recovery snapshot of all dirty tabs
    void snapshotRecovery();         // write every dirty tab to its own recovery file + manifest
    void clearRecovery();            // remove all recovery files + the manifest

    // Keyboard shortcut customization (Batch R5)
    void captureShortcutDefaults();  // record the built-in shortcut of every menu command
    void customizeShortcuts();       // the "Customise Keyboard Shortcuts…" dialog
    QHash<QString,QKeySequence> m_defaultShortcuts;   // command-key → factory shortcut, for Reset

    Canvas* m_canvas{};
    bool m_dirty{false};             // active document changed since the last save/autosave
    QString m_lastCanvasMsg;         // the most recent Canvas::statusMessage — an importer's own
                                     // diagnostic, which must not be buried by a generic line
    bool m_startupDialogs{true};     // show recovery + welcome dialogs on launch (off in headless/test)
    bool m_deferStartup{false};      // if true the ctor skips the startup sequence (main() runs it after the splash)
    class QTimer* m_autosaveTimer{};

    // Multi-document tabs (R6): one Canvas, N stashed sessions. The ACTIVE tab's state lives
    // in the Canvas + m_dirty; inactive tabs live in m_tabs[i].
    struct DocTab { Canvas::Session session; bool dirty=false; };
    QVector<DocTab> m_tabs;
    int  m_activeTab{0};
    class QTabBar* m_tabBar{};
    bool m_suppressDirty{false};     // don't mark dirty during programmatic document swaps (tab switch / new / open)
    void setupTabBar(QWidget*& centralOut);   // build the tabbar+canvas container

    // Empty state (welcome-in-canvas): the central area is a stack — page 0 is the tabbar+canvas, the
    // welcome panel is swapped in (lazily, so recents refresh) when no document is open on interactive launch.
    class QStackedWidget* m_centerStack{};    // page 0 = canvas container, welcome panel = separate page
    QWidget* m_welcomePanel{};                // embedded StartScreen panel (rebuilt on each entry)
    void enterEmptyState();          // interactive launch with no doc: show the embedded welcome, hide chrome
    void enterDocState();            // a document exists: show the canvas page + all document chrome
    void setDocChromeVisible(bool on);// show/hide tabbar, tool rail, panels, toolbars, status bar
    void confirmAndCloseTab(int idx);         // UI: confirm-if-dirty then closeTab
    void acceptWelcomeDrop(const QString& path);   // a file dropped on the welcome panel → open (.xar/.svg) or import into a fresh tab
    void stashActiveTab();           // capture canvas → m_tabs[m_activeTab]
    void refreshTabText(int idx);    // set a tab's label from its filename + dirty '*'
    QString tabTitleFor(const QString& path, bool dirty) const;
    QButtonGroup* m_tools{};
    PropertiesPanel* m_props{};
    LayersPanel* m_layers{};
    ColorPanel* m_colors{};
    InfoBar* m_infobar{};
    ColourLine* m_colourLine{};
    QLabel* m_docName{};   // active doc name shown in the title-bar right corner (DS TitleBar)
    QLabel* m_docBadge{};  // "Unsaved"/"Saved" status pill (DS Badge), reflects m_dirty
    QWidget* m_railOverlay{};  // floating glass tool rail (DS ToolRail), a child overlay of the canvas
    QWidget* m_railInner{};    // the tool-button frame inside the overlay (for its natural height)
    void positionRailOverlay();   // place the glass rail top-left over the canvas, clamped to its height
    QWidget* m_zoomPill{};     // floating glass pill (DS Canvas Toolbar): pan / fit / guides / grid, bottom-centre
    void buildZoomPill();
    void positionZoomPill();
    // DS RightDock: one panel with Design|Prototype tabs over collapsible section cards
    QDockWidget* m_panelDock{};
    QWidget* m_rightPanel{};      // the Design|Prototype panel, floated as a translucent glass overlay over the canvas
    QWidget* m_panelGrip{};       // draggable handle on the panel's left edge to resize it
    int  m_rightPanelW{352};      // current panel width (user-resizable)
    std::function<void()> m_pokeRightPanel;   // reveal the auto-hide (show-on-hover) right panel programmatically
    bool m_gripDrag{false};
    void positionRightPanel();    // pin it to the right edge of the canvas, full height
    QTabWidget* m_rpTabs{};
    CollapsibleSection* m_secProps{}; CollapsibleSection* m_secLayers{}; CollapsibleSection* m_secColour{};
    QScrollArea* m_designScroll{};
    void revealPanelSection(int which);   // 0=Properties 1=Layers 2=Colour: show the dock, Design tab, expand+scroll
    QMenuBar* m_menuBar{};        // the app menu, embedded in the custom DS title bar (not the native menu bar)
    void buildTitleBar();         // frameless custom title bar: quick actions + centered title⇄menu hover-swap + window controls
    void refreshDocStatus();      // update the centred identity + Save button + status "Saved" from filePath + m_dirty
    // --- frameless title bar ("App title bar design"): centre swaps document identity ⇄ menu bar on hover ---
    QToolButton* m_saveBtn{}; QToolButton* m_exportBtn{};
    std::function<void()> m_playSaveWave;            // the green ripple from Save, when a save lands
    QWidget* m_docLayer{}; QWidget* m_menuLayer{};   // the two cross-faded centre layers
    QLabel*  m_pathLabel{}; QLabel* m_titleLabel{}; QString m_titleFull;
    QGraphicsOpacityEffect *m_docFx{}, *m_menuFx{};
    QPropertyAnimation *m_docAnim{}, *m_menuAnim{};
    QToolButton *m_minBtn{}, *m_maxBtn{}, *m_closeBtn{};
    void updateSaveButton();      // green tint when the document is dirty, neutral when saved
    void updateCenterIdentity();  // path line + middle-elided title from the active document
    void setCenterHover(bool on); // cross-fade the identity out and the menu bar in (and back)
    void updateMaxButtonIcon();   // maximize vs restore glyph + square/rounded top corners
    void reelideTitle();          // recompute the middle-elided title for the current centre width
    QLabel* m_sTool{}; QLabel* m_sCoord{}; QLabel* m_sZoom{}; QLabel* m_sSel{}; QLabel* m_sSnap{}; QLabel* m_sSaved{};
};

} // namespace xara
