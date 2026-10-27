import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// Game view (boards 3g and 3r): GameHeader (RunHeader, 56 px) over the stage and the GamePanel (320, or a 48 px rail). Session =
// your game on the empty stage; Multiview = MultiviewArea (tiles), the panel follows the selected tile. The stage draws only the
// game, the register marks and what the design allows on it: toasts at the bottom left, the F3 diagnostics block at the top right
// and plugin HUD widgets. The quick menu (pause, board 7-3) and the Reset dialog are over a scrim on the stage only, so the header
// stays visible. The menus (⋯, speed, screen layout) live in the header, the Add tile popover and the panel's tab overflow menu here. Fullscreen (F11)
// drops header and panel; the toolbar appears when the mouse moves to the top. Without a local game (watching only) the remote
// Session fills the surface.
// Keys (never forwarded to the core; defaults, configurable in Settings > Hotkeys except Esc): Esc closes an open popover,
// else always pauses (window and fullscreen, ADR 0026 D1) and shows the quick menu; a second Esc or "Resume" continues.
// F11 toggles fullscreen and is the only key that leaves it; F3 the diagnostics block, F5 snapshot, Space speed-up (only when the
// core allows it; also while the Session is shared), 1-4 select a tile in the Multiview (audio moves with "Audio here").
Rectangle {
    id: root
    required property PlayerController player
    readonly property GameSession session: player.gameSession
    readonly property SessionController ctl: player.sessions
    readonly property DiagnosticsModel diag: ctl.diagnostics
    readonly property string tab: ctl.tab
    property Item multiLocal: null
    color: Theme.ground

    // Fullscreen mirrors the window; applyToWindow = false keeps tests and screenshots from resizing it.
    property bool fullscreen: false
    property bool applyToWindow: true
    property bool toolbarPinned: false      // tests/screenshots: toolbar shown without the mouse at the top
    // The toolbar also stays while the game is paused in fullscreen: Resume and Leave fullscreen are in it.
    readonly property bool toolbarShown: topHover.hovered || toolbarPinned || (root.fullscreen && root.session.paused)
    // QWindow::Visibility values (the attached Window type has no enum)
    readonly property int visFullScreen: 5
    readonly property int visWindowed: 2
    property int windowedVisibility: 2
    property bool enteredHere: false

    // The popover or dialog that is open: "" | "reset" (dialog) | "picker" (Add tile) | "tabs" (panel tab overflow).
    property string popover: ""
    property bool panelCollapsed: false
    readonly property bool multiMode: tab !== "session"
    readonly property bool menuOpen: header.menuOpen

    // Layouts of the running system (D12); remote pictures offer the switch for frames with two stacked screens.
    readonly property var screenLayouts: session.active ? session.screenLayouts : (ctl.watching ? ctl.watchLayouts : [])
    readonly property string modeTitle: ctl.multiviewMode === "row" ? qsTr("Multiview · Row")
                                       : ctl.multiviewMode === "grid" ? qsTr("Multiview · Grid 2×2")
                                       : qsTr("Multiview · Picture-in-Picture")

    // In-game plugin screen (fb.ui.open("screen")): plugin id, "" = closed. The game keeps running underneath.
    property string screenSheetPlugin: ""
    function openPluginScreen(pluginId) {
        root.closePopover()
        root.screenSheetPlugin = pluginId
        Qt.callLater(focusLocal)
    }
    function closePluginScreen() {
        if (root.screenSheetPlugin === "") return false
        root.screenSheetPlugin = ""
        Qt.callLater(focusLocal)
        return true
    }
    Connections {
        target: root.player.plugins
        function onOpenRequested(pluginId, slot) {
            if (!root.visible) return
            if (slot === "screen") root.openPluginScreen(pluginId)
            else if (slot === "panel" && !root.fullscreen && !root.multiMode) {
                root.closePluginScreen()
                panel.openTab("plugin:" + pluginId)
            }
        }
    }
    Connections {
        target: root.player.plugins
        function onCloseRequested(pluginId, slot) {
            if (slot === "screen" && root.screenSheetPlugin === pluginId) root.closePluginScreen()
        }
    }
    function focusLocal() {
        if (root.screenSheetPlugin !== "") { sheet.forceActiveFocus(); return }
        if (quickMenu.shown) { quickMenu.restoreFocus(); return }
        var v = root.tab === "session" ? view : root.multiLocal
        if (v) v.forceActiveFocus()
    }
    // Popovers and dialogs: one at a time. The Add tile popover and the tab menu are Popups (they close themselves on Esc and a
    // click outside); Reset is a dialog bound to `popover`.
    function openPopover(name) {
        if (root.popover === name) return
        root.closePopover()
        root.popover = name
        if (name === "picker") pickerPop.openFor(header.addAnchor, "below", "end")
        else if (name === "tabs") tabsMenu.openFor(panel.overflowAnchor, "below", true)
    }
    // A press outside closes a popup before the click on its anchor arrives; that click must not reopen it.
    property double popupClosedAt: 0
    property string popupClosedName: ""
    function popupClosedByPress(name) {
        if (root.popover !== name) return
        root.popover = ""
        root.popupClosedAt = Date.now()
        root.popupClosedName = name
        Qt.callLater(root.focusLocal)
    }
    function togglePopover(name) {
        if (root.popover === "" && root.popupClosedName === name && Date.now() - root.popupClosedAt < 400) { root.popupClosedName = ""; return }
        if (root.popover === name) root.closePopover()
        else root.openPopover(name)
    }
    function closePopover() {
        if (root.popover === "") return false
        root.popover = ""
        pickerPop.close()
        tabsMenu.close()
        Qt.callLater(focusLocal)
        return true
    }
    onVisibleChanged: {
        if (visible) {
            Qt.callLater(focusLocal)
        } else {
            root.closePopover()
            root.hotkeySheetPinned = false
            root.screenSheetPlugin = ""
            if (root.fullscreen && root.enteredHere) root.setFullscreen(false)   // leaving the game view: the window goes back to where it was
        }
    }
    // A tab picked with the keyboard (focus on a header button) keeps the focus there; a mouse click hands it back to the game.
    onTabChanged: { root.closePopover(); if (!header.segmentFocused) Qt.callLater(focusLocal) }
    onFullscreenChanged: root.closePopover()

    function setFullscreen(on) {
        if (on === root.fullscreen) return
        var w = root.Window.window
        if (on && w && root.applyToWindow) {
            root.windowedVisibility = w.visibility === root.visFullScreen ? root.visWindowed : w.visibility
            root.enteredHere = true
            w.visibility = root.visFullScreen
        } else if (!on && w && root.applyToWindow && root.enteredHere) {
            root.enteredHere = false
            w.visibility = root.windowedVisibility
        }
        root.fullscreen = on
        if (on) hintTimer.restart()
        Qt.callLater(focusLocal)
    }
    function toggleFullscreen() { setFullscreen(!root.fullscreen) }
    // Esc (ADR 0026 D1): closes an open popover; else it always pauses or resumes your game, in a window and in fullscreen
    // (the quick menu opens). F11 is the only key that leaves fullscreen. Watching only: nothing to pause.
    function handleEscape() {
        if (!root.visible) return   // game in the background (Library shown): Esc never acts on it
        if (root.closePluginScreen()) return
        if (hudLayer.closeMenu()) return   // a HUD or notice menu takes the Esc first, the game keeps running
        if (root.hotkeySheetPinned) { root.hotkeySheetPinned = false; return }
        if (root.closePopover()) return
        if (root.session.active) root.session.togglePause()
    }
    // "⋯ › Hotkeys": the hold-Select sheet stays over the game until Esc or Close.
    property bool hotkeySheetPinned: false
    // "Change hotkeys": Settings > Hotkeys (hotkeys are a Settings section).
    function openHotkeySettings() {
        root.hotkeySheetPinned = false
        const w = root.Window.window
        root.player.leaveGameView()
        if (w && w.openControllersHotkeys) w.openControllersHotkeys()
        else root.player.showSettingsSection("hotkeys")
    }
    // Quick menu status line: where the game was saved and who watches (and what they see while it is paused).
    readonly property SaveHistoryController saves: root.player.saveHistory
    readonly property string saveStateText: {
        if (!header.saveText) return ""
        const sync = root.saves.uploading ? qsTr("syncing") : (root.saves.online ? qsTr("synced") : qsTr("waiting for the server"))
        return header.saveText + " · " + sync
    }
    readonly property string quickStatusText: {
        const parts = []
        if (root.saveStateText !== "") parts.push(root.saveStateText + ".")
        if (root.saves.message !== "") parts.push(root.saves.message)
        if (root.ctl.shared)
            parts.push(qsTr("%1 watching. Viewers see “Paused by %2”.").arg(root.ctl.viewerCount).arg(root.ctl.ownName !== "" ? root.ctl.ownName : qsTr("the host")))
        else
            parts.push(qsTr("Nobody is watching."))
        return parts.join(" ")
    }
    function resumeGame() { if (root.session.paused) root.session.togglePause() }
    // A row of the quick menu. Rows that act in the panel or on the stage resume the game first; the others keep the menu open.
    function runQuick(name, anchor) {
        if (name === "snapshot") root.saveSnapshot()
        else if (name === "saves") { root.resumeGame(); panel.openSaves() }
        else if (name === "screenshot") root.player.screenshots.capture()
        else if (name === "gameSettings") gameSettings.open(root.player.runningGameId)
        else if (name === "reset") root.openPopover("reset")
        else if (name === "speedup") header.openSpeedMenuAt(anchor)
        else if (name === "sound") header.openSoundAt(anchor)
        else if (name === "screens") header.openLayoutAt(anchor)
        else if (name === "fullscreen") root.toggleFullscreen()
        else if (name === "diagnostics") root.diag.toggle()
        else if (name === "share") { root.resumeGame(); panel.showSharing() }
        else if (name === "multiview") root.ctl.tab = "multiview"
        else if (name === "hotkeys") { root.resumeGame(); root.hotkeySheetPinned = true }
        else if (name === "report") root.player.report.show()
    }
    function saveSnapshot() {
        if (root.session.active && root.player.saveHistory.available && !root.player.saveHistory.busy)
            root.player.saveHistory.createSnapshotInGame("")
    }

    Connections {
        target: root.Window.window
        function onVisibilityChanged() {
            var w = root.Window.window
            if (w && root.applyToWindow && root.visible) {
                // Number(): comparing the visibility enum directly raised a "Type error" in the compiled binding (Qt 6.4)
                root.fullscreen = Number(w.visibility) === root.visFullScreen
            }
        }
    }
    // Resumed: the keys go back to the game
    Connections {
        target: root.session
        function onStateChanged() { if (!root.session.paused) Qt.callLater(root.focusLocal) }
    }

    // Player hotkeys are configurable (Settings > Hotkeys): the controller maps the Qt key to an action name.
    // Hints show the gamepad combo while a gamepad was the last input used, else the key (name kept: hotkeyLabels).
    readonly property var hotkeyLabels: root.player.controllers.hotkeyHints
    // Screen layout (F7): opens the layout menu in the header; in fullscreen (no header) it picks the next layout instead.
    // Single-screen systems have no layout UI: nothing happens. Select + Right on the gamepad always cycles (no menu to steer).
    function openLayoutMenu() {
        if (root.screenLayouts.length < 2) return
        if (header.visible) header.toggleLayoutMenu()
        else root.cycleLayout()
    }
    function cycleLayout() {
        if (root.screenLayouts.length < 2) return
        root.ctl.chooseScreenLayout(root.ctl.nextLayout(root.screenLayouts))
    }
    // Runs a hotkey action (keyboard or gamepad); false = unknown action.
    function runHotkey(action) {
        if (action === "fullscreen") root.toggleFullscreen()
        else if (action === "diagnostics") root.diag.toggle()
        else if (action === "snapshot") root.saveSnapshot()
        else if (action === "pause") { if (root.session.active) root.session.togglePause() }
        else if (action === "hud") root.player.plugins.hudAllHidden = !root.player.plugins.hudAllHidden
        else if (action === "speedup") { if (root.session.fastForwardAvailable) root.session.toggleFastForward() }
        else if (action === "layout") root.openLayoutMenu()
        else if (action === "swap") { if (root.screenLayouts.length > 1) root.ctl.chooseScreenSwap(!root.ctl.screenSwap) }
        else if (action === "nextLayout") root.cycleLayout()
        else if (action === "volumeUp") root.player.audio.volumeUp()
        else if (action === "volumeDown") root.player.audio.volumeDown()
        else if (action === "mute") root.player.audio.toggleMute()
        else if (action === "screenshot") root.player.screenshots.capture()
        else return false
        return true
    }
    Connections {
        target: root.player.controllers
        // Gamepad hotkeys (P1): the buttons are already kept away from the core by the GamepadService.
        function onPadHotkeyTriggered(action) { if (root.visible) root.runHotkey(action === "layout" ? "nextLayout" : action) }
    }
    // A text field of a plugin panel has the focus: plugin keys, Alt+n and F4 are typing, not hotkeys.
    function textFieldFocused() {
        var f = root.Window.activeFocusItem
        return !!f && ("cursorPosition" in f)
    }
    // Plugin hotkeys, Alt+1 ... Alt+9 (panel tab n) and F4 (hide or show all HUD widgets); true = the key was used.
    function runExtraHotkey(e) {
        if (root.textFieldFocused()) return false
        if (e.isAutoRepeat) return false
        if (root.player.plugins.triggerHotkeyKey(e.key, e.modifiers)) return true
        var mods = e.modifiers & (Qt.ControlModifier | Qt.ShiftModifier | Qt.AltModifier | Qt.MetaModifier)
        if (mods === Qt.AltModifier && e.key >= Qt.Key_1 && e.key <= Qt.Key_9) {
            if (!root.fullscreen) panel.openTabNumber(e.key - Qt.Key_0)
            return true
        }
        if (e.key === Qt.Key_F4 && mods === Qt.NoModifier) {
            root.player.plugins.hudAllHidden = !root.player.plugins.hudAllHidden
            return true
        }
        return false
    }
    // Keys pressed in the window of the bottom screen that the game view there did not use (hotkeys, Esc).
    function handleWindowKey(e) {
        if (e.isAutoRepeat) return
        var action = root.player.controllers.hotkeyAction(e.key)
        if (action === "layout" && (e.modifiers & Qt.ShiftModifier)) action = "swap"
        if (action !== "" && action !== "fullscreen" && root.runHotkey(action)) e.accepted = true
        else if (e.key === Qt.Key_Escape && root.session.active) {
            if (!hudLayer.closeMenu()) root.session.togglePause()
            e.accepted = true
        }
    }
    Keys.onPressed: (e) => {
        if (!root.visible) return   // game in the background: hotkeys do not act on it from other screens
        root.player.controllers.noteKeyboardInput()
        if (root.runExtraHotkey(e)) { e.accepted = true; return }
        var action = root.player.controllers.hotkeyAction(e.key)
        if (action !== "") {
            // Shift + the layout key swaps the screens directly
            if (action === "layout" && (e.modifiers & Qt.ShiftModifier)) action = "swap"
            if (!e.isAutoRepeat || action === "volumeUp" || action === "volumeDown") root.runHotkey(action)
            e.accepted = true   // the key never reaches the core (GameSession::isReservedKey)
        } else if (e.key === Qt.Key_Escape) {
            if (!e.isAutoRepeat && !root.textFieldFocused()) root.handleEscape()
            e.accepted = !root.textFieldFocused()
        } else if (e.key >= Qt.Key_1 && e.key <= Qt.Key_4 && root.multiMode) {
            var id = root.ctl.surfaceOrder[e.key - Qt.Key_1]
            if (id !== undefined) root.ctl.selectSurface(id)
            e.accepted = true
        }
    }

    Timer { id: hintTimer; interval: 6000 }

    // "Settings for this game…" of the "⋯" menu: the same dialog as on the game page; the game keeps running behind it.
    GameSettingsDialog {
        id: gameSettings
        objectName: "gameSettingsHost"
        player: root.player
        onClosed: Qt.callLater(root.focusLocal)
    }

    property string layoutToastText: ""
    Timer { id: layoutToastTimer; interval: 2500; onTriggered: root.layoutToastText = "" }
    Connections {
        target: root.ctl
        function onLayoutNotice(text) {
            root.layoutToastText = text
            layoutToastTimer.restart()
        }
    }

    // Bottom screen in its own window (NDS/3DS, local game only): a second top-level window, closed with the game.
    BottomScreenWindow {
        id: bottomWindow
        player: root.player
        onHotkeyKey: (e) => root.handleWindowKey(e)
    }

    GameHeader {
        id: header
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        z: 5
        visible: !root.fullscreen
        height: visible ? Theme.header56 : 0
        player: root.player
        popover: root.popover
        panelShown: !root.panelCollapsed
        hotkeys: root.hotkeyLabels
        onFullscreenRequested: root.toggleFullscreen()
        onResetRequested: root.openPopover("reset")
        onGameSettingsRequested: gameSettings.open(root.player.runningGameId)
        onHotkeysRequested: root.hotkeySheetPinned = true
        onPanelToggled: root.panelCollapsed = !root.panelCollapsed
        onPickerToggled: root.togglePopover("picker")
        onLayoutMenuClosed: Qt.callLater(root.focusLocal)   // the layout hotkey keeps working after the menu
    }

    // Stage and panel
    Item {
        id: body
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom

        RowLayout {
            anchors.fill: parent
            spacing: 0

            Item {
                id: playArea
                objectName: "playArea"
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumWidth: 0

                // The stage plane behind your game; the Multiview tiles sit on the page `ground` and bring their own stage
                Rectangle { anchors.fill: parent; color: root.tab === "session" ? Theme.stage : Theme.ground }

                // Session tab: your game, framed by the register marks (`dim`)
                Item {
                    objectName: "sessionTab"
                    anchors.fill: parent
                    visible: root.tab === "session"

                    UzRegisterMarks { tone: "dim"; inset: Theme.space1 }
                    GameView {
                        id: view
                        objectName: "gameView"
                        anchors.fill: parent
                        anchors.margins: Theme.space3 + Theme.space05
                        session: root.session
                        layout: root.ctl.viewLayout
                        swap: root.ctl.viewSwap
                        strictInteger: root.ctl.integerScale
                        aspectRatio: root.ctl.aspectRatio
                        focus: true
                        onEscapePressed: root.handleEscape()

                        UzLabel {
                            anchors.centerIn: parent
                            visible: !root.session.hasFrame && root.session.state !== GameSession.Failed
                            text: qsTr("Starting emulator…")
                            color: Theme.muted
                        }
                    }
                }

                // Multiview
                ColumnLayout {
                    objectName: "multiviewTab"
                    anchors.fill: parent
                    visible: root.tab !== "session"
                    spacing: 0

                    UzStateBlock {
                        Layout.alignment: Qt.AlignHCenter
                        Layout.margins: Theme.space3
                        Layout.preferredWidth: 360
                        visible: !root.session.active && !root.ctl.watching
                        kind: root.ctl.joining ? "loading" : "empty"
                        title: root.ctl.joining ? qsTr("Joining Session…") : qsTr("Not watching a Session.")
                        text: root.ctl.joining ? "" : qsTr("Add a tile to watch a Session from the server.")
                        action: root.ctl.joining ? "" : qsTr("Add tile")
                        onActionClicked: root.openPopover("picker")
                    }
                    Item {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        MultiviewArea {
                            anchors.fill: parent
                            player: root.player
                            onLocalReady: (v) => { root.multiLocal = v; Qt.callLater(root.focusLocal) }
                            onPickerRequested: root.openPopover("picker")
                            onTileSelected: Qt.callLater(root.focusLocal)
                            onEscapePressed: root.handleEscape()
                        }
                    }
                }

                // F3 diagnostics: the block in the top right corner of the stage (one of the three things the empty stage allows)
                DiagnosticsOverlay {
                    id: diagnosticsOverlay
                    objectName: "diagnosticsOverlay"
                    anchors.top: parent.top
                    anchors.right: parent.right
                    anchors.margins: Theme.space2
                    z: 10
                    visible: root.diag.open
                    model: root.diag
                    multi: root.multiMode
                }

                // Plugin HUD widgets and notices; stays in fullscreen, where the panel is hidden
                PluginHudLayer {
                    id: hudLayer
                    player: root.player
                    bottomInset: root.multiMode ? 42 : 0   // the tile name bars of the Multiview
                    // The bottom-left widget stack: the toasts sit above it (it is 16 px from the edge)
                    property Item leftStack: null
                    Component.onCompleted: {
                        for (var i = 0; i < children.length; ++i)
                            if (children[i].objectName === "hudStack_bottom-left") leftStack = children[i]
                    }
                    onDetailsRequested: panel.openTab("console")
                }

                // Hold-Select hotkey sheet (board 3g-10): while P1 holds the hotkey button alone for a moment, the combos of the
                // current bindings are listed over the stage; releasing it, or pressing a button (the combo fires as usual), closes it.
                Loader {
                    id: padSheet
                    objectName: "padHotkeySheetLoader"
                    // Created only while the button is held: the sheet's grid never rearranges while the stage is set up.
                    active: root.visible && root.tab === "session" && root.session.active
                            && (root.hotkeySheetPinned || (!root.session.paused && root.player.controllers.holdSheetHeld))
                    anchors.centerIn: parent
                    z: 15
                    sourceComponent: UzHotkeySheet {
                        objectName: "padHotkeySheet"
                        width: Math.min(implicitWidth, Math.max(0, playArea.width - 2 * Theme.space3))
                        modifier: root.player.controllers.padHotkeyModifier
                        pinned: root.hotkeySheetPinned
                        onCloseRequested: root.hotkeySheetPinned = false
                        onChangeRequested: root.openHotkeySettings()
                        keys: root.player.controllers.padSheetRows.map(function (r) {
                            if (r.action === "layout" && root.screenLayouts.length < 2) return [r.key, r.label, qsTr("One screen")]
                            if (r.action === "speedup" && !root.session.fastForwardAvailable) return [r.key, r.label, qsTr("Not available")]
                            if (r.action === "diagnostics") return [r.key, qsTr("Diagnostics")]   // toggles the stage block like F3
                            return [r.key, r.label]
                        })
                    }
                }

                // Toasts, bottom left of the stage: persistent notices (Speed-up) have no close mark
                Column {
                    id: toasts
                    objectName: "toastStack"
                    anchors.left: parent.left
                    anchors.bottom: parent.bottom
                    anchors.margins: Theme.space3
                    anchors.bottomMargin: Theme.space3 + hudLayer.bottomInset + (hudLayer.leftStack && hudLayer.leftStack.visible && hudLayer.leftStack.height > 0 ? hudLayer.leftStack.height + Theme.space2 - Theme.space3 + Theme.space2 : 0)
                    spacing: Theme.space1
                    z: 12
                    readonly property real maxToastWidth: Math.max(240, playArea.width - 2 * Theme.space3)

                    // Volume by hotkey ("Show volume on screen"): one line that replaces itself and goes after 1.5 s
                    UzToast {
                        id: volumeToast
                        objectName: "volumeToast"
                        visible: text !== ""
                        width: Math.min(implicitWidth, toasts.maxToastWidth)
                        kind: "done"
                        closable: false
                        text: ""
                        Timer { id: volumeToastTimer; interval: 1500; onTriggered: volumeToast.text = "" }
                        Connections {
                            target: root.player.audio
                            function onVolumeNotice(text) { volumeToast.text = text; volumeToastTimer.restart() }
                        }
                    }
                    // Screenshot hotkey: "Screenshot saved", or that it waits on this PC for the server; goes after 3 s
                    UzToast {
                        id: screenshotToast
                        objectName: "screenshotToast"
                        visible: text !== ""
                        width: Math.min(implicitWidth, toasts.maxToastWidth)
                        kind: "done"
                        closable: false
                        text: ""
                        Timer { id: screenshotToastTimer; interval: 3000; onTriggered: screenshotToast.text = "" }
                        Connections {
                            target: root.player.screenshots
                            function onNotice(text) { screenshotToast.text = text; screenshotToastTimer.restart() }
                        }
                    }
                    UzToast {
                        objectName: "fastForwardIndicator"
                        visible: root.session.fastForward && !root.session.paused
                        width: Math.min(implicitWidth, toasts.maxToastWidth)
                        kind: "done"
                        closable: false
                        text: qsTr("Speed-up ×%1").arg(Math.round(root.session.fastForwardRatio * 10) / 10)
                    }
                    // The save of the running game changed on another device (non-blocking)
                    // "Layout: Side by side" after a layout, swap or bottom-window choice (F7 menu, hotkeys, header)
                    UzToast {
                        objectName: "layoutToast"
                        visible: root.layoutToastText !== ""
                        width: Math.min(implicitWidth, toasts.maxToastWidth)
                        kind: "done"
                        closable: false
                        text: root.layoutToastText
                    }
                    UzToast {
                        objectName: "saveNotice"
                        visible: root.player.saveHistory.notice !== ""
                        width: Math.min(420, toasts.maxToastWidth)
                        kind: "warning"
                        maxLines: 3
                        text: root.player.saveHistory.notice
                        onCloseRequested: root.player.saveHistory.dismissNotice()
                    }
                    UzToast {
                        objectName: "gameMessage"
                        visible: root.ctl.message !== "" && root.multiMode
                        width: Math.min(420, toasts.maxToastWidth)
                        kind: root.ctl.messageIsError ? "error" : "done"
                        maxLines: 2
                        text: root.ctl.message
                        onCloseRequested: root.ctl.dismissMessage()
                    }
                    UzToast {
                        objectName: "fullscreenToast"
                        visible: root.fullscreen && root.player.saveHistory.message !== ""
                        width: Math.min(420, toasts.maxToastWidth)
                        kind: "done"
                        closable: false
                        maxLines: 2
                        text: root.player.saveHistory.message
                    }
                    UzToast {
                        objectName: "fullscreenHint"
                        visible: root.fullscreen && !root.toolbarShown && hintTimer.running
                        width: Math.min(implicitWidth, toasts.maxToastWidth)
                        kind: "done"
                        closable: false
                        maxLines: 2
                        text: root.hotkeyLabels.diagnostics !== "" ? qsTr("Toolbar appears when you move the mouse to the top · %1 diagnostics").arg(root.hotkeyLabels.diagnostics)
                                                                   : qsTr("Toolbar appears when you move the mouse to the top")
                    }
                }

                // Quick menu (board 7-3, ADR 0026 D1): Esc or the Pause buttons pause, the menu shows in a window and in fullscreen
                // (in the Multiview window the paused tile says so itself). Resume continues, F11 and "Leave fullscreen" leave.
                GameQuickMenu {
                    id: quickMenu
                    objectName: "quickMenuHost"
                    gameTitle: root.session.title
                    statusText: root.quickStatusText
                    speedValue: header.speedValue
                    soundValue: header.soundValue
                    screensValue: header.screensValue
                    shareValue: header.shareText
                    padModifier: root.player.controllers.padHotkeyModifier
                    canSpeedUp: root.session.fastForwardAvailable
                    canScreens: root.screenLayouts.length > 1
                    canSnapshot: root.saves.available && !root.saves.busy
                    fullscreen: root.fullscreen
                    quitting: root.player.quitting
                    hotkeys: root.hotkeyLabels
                    Binding { target: quickMenu; property: "shown"; value: root.session.paused && (root.tab === "session" || root.fullscreen) }
                    onResumeRequested: root.resumeGame()
                    onQuitRequested: root.player.requestQuit()
                    onChosen: (name, anchor) => root.runQuick(name, anchor)
                }

                // Reset (header ⋯ menu): the confirmation covers the stage, the header stays
                UzDialog {
                    id: resetDialog
                    objectName: "resetDialog"
                    label: root.session.title
                    title: qsTr("Reset %1?").arg(root.session.title)
                    text: qsTr("Unsaved progress since the last checkpoint is lost.")
                    onClosed: root.closePopover()
                    Binding { target: resetDialog; property: "shown"; value: root.popover === "reset" }
                    actions: [
                        UzButton { objectName: "resetCancel"; focusPolicy: Qt.NoFocus; text: qsTr("Cancel"); onClicked: root.closePopover() }
                    ]
                    dangerActions: [
                        UzButton {
                            objectName: "resetConfirm"
                            kind: "danger"
                            focusPolicy: Qt.NoFocus
                            text: qsTr("Reset")
                            onClicked: { root.session.reset(); root.closePopover() }
                        }
                    ]
                }
            }

            GamePanel {
                id: panel
                visible: !root.fullscreen
                Layout.fillHeight: true
                Layout.preferredWidth: visible ? implicitWidth : 0
                player: root.player
                collapsed: root.panelCollapsed
                multi: root.multiMode
                tile: root.multiMode ? root.ctl.selectedSurface : "local"
                onCollapseToggled: root.panelCollapsed = !root.panelCollapsed
                onSavesClosed: Qt.callLater(root.focusLocal)
                onSelectLocalRequested: root.ctl.selectSurface("local")
                overflowOpen: root.popover === "tabs"
                onOverflowRequested: root.togglePopover("tabs")
            }
        }
    }

    // In-game plugin screen: covers the play area and the panel below the header
    PluginScreenSheet {
        id: sheet
        player: root.player
        pluginId: root.screenSheetPlugin
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        z: 4
        onCloseRequested: root.closePluginScreen()
    }

    // ---- popups opened from the header and the panel ----

    // Add tile: the running sessions that can be put in the Multiview (3r-5). Never opens on its own.
    UzPopover {
        id: pickerPop
        objectName: "pickerPopover"
        popWidth: 360
        onClosed: root.popupClosedByPress("picker")
        SessionPicker {
            Layout.fillWidth: true
            player: root.player
            maxListHeight: Math.max(120, root.height * 0.5 - 60)
        }
    }

    // "+n ▾": the panel tabs that do not fit
    UzMenu {
        id: tabsMenu
        objectName: "panelTabOverflowPopover"
        menuWidth: 250
        items: panel.tabPlan.overflow.map(function (e) { return { label: e.name, hint: panel.originMarker(e.origin), name: "tabOverflow_" + e.id, id: e.id } })
        onChosen: (item) => { root.popover = ""; panel.openTab(item.id) }
        onClosed: root.popupClosedByPress("tabs")
    }

    // ---- fullscreen furniture ----

    // Toolbar: appears when the mouse moves to the top
    Item {
        id: topZone
        objectName: "fullscreenTopZone"
        visible: root.fullscreen
        z: 20
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 96
        HoverHandler { id: topHover }

        Rectangle {
            id: toolbar
            objectName: "fullscreenToolbar"
            visible: root.toolbarShown
            anchors.top: parent.top
            anchors.topMargin: Theme.space2
            anchors.horizontalCenter: parent.horizontalCenter
            implicitWidth: toolbarRow.implicitWidth + 2 * Theme.space2
            width: Math.min(implicitWidth, parent.width - 2 * Theme.space2)
            implicitHeight: Theme.row48 + Theme.space1
            radius: 0
            color: Theme.surface
            border.width: 1
            border.color: Theme.rule

            RowLayout {
                id: toolbarRow
                anchors.fill: parent
                anchors.leftMargin: Theme.space2
                anchors.rightMargin: Theme.space15
                spacing: Theme.space15
                UzLabel {
                    objectName: "fullscreenToolbarTitle"
                    Layout.maximumWidth: 260
                    text: root.tab === "session" ? (root.session.active ? root.session.title : qsTr("Session")) : root.modeTitle
                    elide: Text.ElideRight
                    font.pixelSize: Theme.roles.itemTitleSm.size
                    font.weight: Theme.roles.itemTitleSm.weight
                }
                Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 16; color: Theme.rule }
                LayoutSwitch {
                    objectName: "toolbarLayoutSwitch"
                    layouts: root.screenLayouts
                    current: root.ctl.screenLayout
                    compact: true
                    onPicked: (v) => root.ctl.chooseScreenLayout(v)
                }
                UzButton {
                    objectName: "fullscreenPauseButton"
                    visible: root.session.active
                    size: "sm"
                    enabled: root.session.state === GameSession.Running || root.session.state === GameSession.Paused
                    kind: root.session.paused ? "primary" : "outline"
                    focusPolicy: Qt.NoFocus
                    text: root.session.paused ? qsTr("Resume") : qsTr("Pause")
                    onClicked: root.session.togglePause()
                }
                UzKbd { visible: root.session.active; keys: "Esc" }
                UzButton {
                    objectName: "exitFullscreenButton"
                    size: "sm"
                    focusPolicy: Qt.NoFocus
                    text: qsTr("Leave fullscreen")
                    onClicked: root.setFullscreen(false)
                }
                UzKbd { visible: root.hotkeyLabels.fullscreen !== ""; keys: root.hotkeyLabels.fullscreen || "F11" }
            }
        }
    }
}
