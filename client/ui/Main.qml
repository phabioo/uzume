import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

ApplicationWindow {
    id: window
    required property PlayerController player

    visible: true
    width: 1280
    height: 800
    minimumWidth: 960
    minimumHeight: 600
    title: qsTr("Uzume")
    color: Theme.ground

    // Appearance (Settings): Dark | Light | System; the game view is always dark.
    Binding {
        target: Theme
        property: "dark"
        value: window.player.darkMode || window.player.screen === "game"
    }

    // Text size (100 % / 125 % / TV) and Reduce motion (Settings > Appearance): one scale factor and one motion switch.
    Binding { target: Theme; property: "textScale"; value: window.player.textSize === "100" ? 1.0 : 1.25 }
    Binding { target: Theme; property: "tv"; value: window.player.textSize === "tv" }
    Binding { target: Theme; property: "reduceMotion"; value: window.player.reduceMotion }

    // Update banner (S6): never shown in the game view; the install action is refused while a game runs.
    header: Rectangle {
        objectName: "updateBanner"
        visible: window.player.updates.bannerText !== "" && window.player.screen !== "game"
        implicitHeight: visible ? 44 : 0
        height: implicitHeight
        color: Theme.surface
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.rule }
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 20
            anchors.rightMargin: 20
            spacing: 12
            StatusDot { tone: "ok" }
            UzLabel {
                objectName: "updateBannerText"
                Layout.fillWidth: true
                text: window.player.updates.bannerText
                font.pixelSize: Theme.roles.input.size
                elide: Text.ElideRight
            }
            UzButton {
                objectName: "updateBannerAction"
                visible: window.player.updates.bannerAction !== "none"
                implicitHeight: 30
                kind: "primary"
                text: window.player.updates.bannerActionLabel
                onClicked: window.player.updates.bannerAction === "link" ? window.player.updates.openNotes()
                                                                         : window.player.updates.install()
            }
            UzButton {
                objectName: "updateBannerDismiss"
                implicitHeight: 30
                kind: "link"
                text: qsTr("Later")
                onClicked: window.player.updates.dismissBanner()
            }
        }
    }

    // Claude debugging (ADR 0032): visible on every screen, the game included, while the MCP server listens.
    // A plain item at the bottom (not the window footer): the window layout stays as it was while the band is off.
    Rectangle {
        id: mcpBandItem
        objectName: "mcpBand"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        z: 500
        visible: window.player.mcp.listening
        height: visible ? 36 : 0
        color: Theme.surface
        Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: Theme.rule }
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 20
            anchors.rightMargin: 20
            spacing: 12
            StatusDot { tone: "warn" }
            UzLabel {
                objectName: "mcpBandText"
                Layout.fillWidth: true
                text: qsTr("Claude can control this app")
                font.pixelSize: Theme.roles.input.size
                elide: Text.ElideRight
            }
            UzButton {
                objectName: "mcpBandStop"
                implicitHeight: 26
                text: qsTr("Stop")
                onClicked: window.player.mcp.stop()
            }
        }
    }

    StackLayout {
        id: stack
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: mcpBandItem.visible ? mcpBandItem.top : parent.bottom
        currentIndex: ["connection", "pairing", "library", "game", "settings", "emulation", "controllers", "runs", "server", "start"].indexOf(window.player.screen)
        focus: true

        // Back rule (readable-pass.md, design-system.md "Focus is always visible"): B / Esc go one level back on a main page.
        // Dialogs and menus close first (they take the key themselves), inside the content of a page with a second column the
        // key returns to the column (Left does too, when the control under the focus does not use it), from the column or a page
        // without one it moves to the AppNav, and from the AppNav it goes to the Library. In the game Esc pauses (GameScreen).
        Keys.onPressed: (event) => {
            if (!window.navHotkeys) return
            const back = event.key === Qt.Key_Escape || event.key === Qt.Key_Back
            const left = event.key === Qt.Key_Left && event.modifiers === Qt.NoModifier
            if (!back && !left) return
            if (event.isAutoRepeat) { event.accepted = true; return }
            if (window.stepBack(left)) event.accepted = true
        }

        ConnectionScreen { player: window.player }
        PairingScreen { player: window.player }
        LibraryScreen { player: window.player }
        GameScreen { player: window.player }
        // Created only while shown: a hidden StackLayout child whose Repeaters change keeps stale layout items and
        // crashed Qt Layouts on the next switch to Settings.
        Loader {
            id: settingsLoader
            active: window.player.screen === "settings"
            sourceComponent: SettingsScreen { player: window.player }
        }
        EmulationScreen { player: window.player }
        ControllersScreen { player: window.player }
        RunsScreen { player: window.player }
        ServerScreen { player: window.player }
        StartScreen { player: window.player }
    }

    // The page that is shown now (the Settings page lives in a Loader).
    function currentPage(): Item {
        const p = stack.children[stack.currentIndex]
        return p && p.item ? p.item : p
    }
    function findByName(item: Item, name: string): Item {
        if (!item) return null
        if (item.objectName === name) return item
        const kids = item.children
        for (let i = 0; i < kids.length; ++i) {
            const hit = window.findByName(kids[i], name)
            if (hit) return hit
        }
        return null
    }
    // One level back. `onlyContent` (Left): only content -> column. Returns true when something moved.
    function stepBack(onlyContent: bool): bool {
        const page = window.currentPage()
        if (!page) return false
        if (page.handleBack !== undefined && page.handleBack()) return true
        if (onlyContent) return false
        const nav = window.findByName(page, "appNav")
        if (nav && !nav.hasFocus()) return nav.focusActive()
        if (nav && window.player.screen !== "library") { window.player.showLibrary(); return true }
        return false
    }
    // A page that opens without a focused control keeps the key handling above.
    Connections {
        target: window.player
        function onScreenChanged() {
            Qt.callLater(function () {
                const f = window.activeFocusItem
                let inside = false
                for (let p = f; p; p = p.parent) { if (p === stack) { inside = true; break } }
                if (!inside && window.navHotkeys) stack.forceActiveFocus()
            })
        }
    }

    // First start, joining and setting up servers (0.18 package 7): the start screen replaces the connection screens for the first
    // start. The people on this PC (Ctrl+Shift+U) are a full-window screen above every page.
    Component.onCompleted: window.player.onboarding.startScreenEnabled = true
    SwitchUserScreen { anchors.fill: parent; player: window.player }

    // Hotkeys (frame.md "0-4 G"): Ctrl+1 to 4 Library, Runs, Emulation, Controllers; Ctrl+, Settings. Not in the game view,
    // not before a server is connected.
    readonly property bool navHotkeys: ["library", "runs", "emulation", "controllers", "settings", "server"].indexOf(window.player.screen) >= 0
    Shortcut { sequence: "Ctrl+1"; enabled: window.navHotkeys; onActivated: window.player.showLibrary() }
    Shortcut { sequence: "Ctrl+2"; enabled: window.navHotkeys && window.player.runs.available; onActivated: window.player.showRuns() }
    Shortcut { sequence: "Ctrl+3"; enabled: window.navHotkeys; onActivated: window.player.showEmulation() }
    Shortcut { sequence: "Ctrl+4"; enabled: window.navHotkeys; onActivated: window.player.showControllers() }
    Shortcut { sequence: "Ctrl+,"; enabled: window.navHotkeys; onActivated: window.player.showSettings() }
    // Switch user (8d-1): from every screen with a server, also in the game view (it asks to quit the game first).
    Shortcut {
        sequence: "Ctrl+Shift+U"
        enabled: (window.navHotkeys || window.player.screen === "game") && window.player.accounts.phase === "idle"
        onActivated: window.player.accounts.open()
    }
    // An invite link pasted anywhere in the window (8c-1). A text field that has the focus keeps its own paste.
    TextEdit { id: clipboardProbe; visible: false }
    Shortcut {
        sequences: [StandardKey.Paste]
        enabled: window.player.accounts.phase === "idle"
        onActivated: {
            clipboardProbe.text = ""
            clipboardProbe.paste()
            const text = clipboardProbe.text.trim()
            if (text !== "") window.player.onboarding.openLink(text)
        }
    }

    // 8c-3a: an invite that arrived while a game runs waits until the game ended; bottom left, over every screen.
    UzToast {
        objectName: "linkWaitingToast"
        visible: window.player.onboarding.linkWaiting
        z: 80
        x: Theme.space3
        y: parent.height - height - Theme.space3
        kind: "warning"
        text: qsTr("The invite opens when you quit. Your save uploads first.")
        action: qsTr("Dismiss")
        closable: false
        onActionTriggered: window.player.onboarding.dismissPendingLink()
    }
    // 8c-2 "Add only": nothing changes now, the server is in Settings > This PC > Servers.
    UzToast {
        id: linkAddedToast
        objectName: "linkAddedToast"
        property string serverName: ""
        visible: serverName !== ""
        z: 80
        x: Theme.space3
        y: parent.height - height - Theme.space3
        kind: "done"
        text: qsTr("%1 is added. You find it in Settings › This PC › Servers.").arg(serverName)
        onCloseRequested: serverName = ""
        Timer { running: linkAddedToast.visible; interval: 8000; onTriggered: linkAddedToast.serverName = "" }
    }
    Connections {
        target: window.player.onboarding
        function onLinkAdded(serverName: string) { linkAddedToast.serverName = serverName }
    }

    // Connection check (6e): modal over every screen; "Edit Hub address…" opens the inline edit of the Hub in Settings.
    ConnectionCheckDialog { player: window.player }
    Connections {
        target: window.player.connectionCheck
        function onEditHubRequested() {
            window.player.showSettings()
            if (settingsLoader.item) settingsLoader.item.editActiveHubAddress()
        }
    }

    // fb.ui.open("screen") outside the game view: the plugin's tab of the opened run, else the plugin screen sheet (in the game
    // view GameScreen shows the sheet). "Open" in Settings > Plugins takes the same way.
    property string screenSheetPlugin: ""
    Connections {
        target: window.player.plugins
        function onOpenRequested(pluginId, slot) {
            if (slot === "screen" && window.player.screen !== "game") window.player.openPluginScreen(pluginId)
        }
    }
    Connections {
        target: window.player
        function onPluginScreenRequested(pluginId) { window.screenSheetPlugin = pluginId }
        function onScreenChanged() { window.screenSheetPlugin = "" }
    }
    PluginScreenSheet {
        id: appScreenSheet
        objectName: "appPluginScreenSheet"
        player: window.player
        pluginId: window.screenSheetPlugin
        backText: qsTr("← Back")
        anchors.fill: parent
        z: 50
        onCloseRequested: window.screenSheetPlugin = ""
    }

    // Page transition: short fade-in of the page that becomes current. Never in the game view.
    NumberAnimation { id: pageFade; property: "opacity"; from: 0; to: 1; duration: Theme.durPage; easing.type: Easing.OutCubic }
    Connections {
        target: window.player
        function onScreenChanged() {
            if (window.player.screen === "game") {
                return
            }
            const page = stack.children[stack.currentIndex]
            if (page !== undefined) {
                pageFade.target = page
                pageFade.restart()
            }
        }
    }

    // Emulation > Uzume > "Fullscreen on start": the window goes fullscreen while a game is shown and returns afterwards.
    property int visibilityBeforeGame: Window.Windowed
    property bool fullscreenApplied: false
    Connections {
        target: window.player
        function onScreenChanged() {
            if (window.player.screen === "game" && window.player.fullscreenOnStart && !window.fullscreenApplied) {
                window.visibilityBeforeGame = window.visibility
                window.fullscreenApplied = true
                window.visibility = Window.FullScreen
            } else if (window.player.screen !== "game" && window.fullscreenApplied) {
                window.fullscreenApplied = false
                window.visibility = window.visibilityBeforeGame === Window.FullScreen ? Window.Windowed : window.visibilityBeforeGame
            }
        }
    }

    ConflictDialog { player: window.player }
    UzConfirmDialog { controller: window.player.admin; parent: Window.contentItem; z: 200 }   // "Confirm it's you": opens for any admin request that needs it
    ShareDialogs { player: window.player; parent: Window.contentItem; z: 190 }
    ReportProblemDialog { player: window.player }  // Settings > Diagnostics and the game "⋯" menu open it
    WhatsNewDialog { player: window.player }   // once per new app version (release notes of the signed updates index)
    SetupAssistant { player: window.player }   // player.showSetupAssistant()

    // Tooltips (R-P12): the shared tooltip of the attached ToolTip type is restyled once with the theme tokens
    // (popup surface, popup border, text) instead of the default white box.
    Component {
        id: tipBackground
        Rectangle {
            radius: 0
            color: Theme.surface
            border.width: 1
            border.color: Theme.rule
        }
    }
    Component {
        id: tipContent
        Text {
            property var tip: null
            text: tip ? tip.text : ""
            textFormat: Text.PlainText
            font.pixelSize: Theme.roles.small.size
            color: Theme.ink
            wrapMode: Text.Wrap
        }
    }
    Item {
        id: tipHost
        visible: false
        Component.onCompleted: {
            const tip = ToolTip.toolTip
            if (tip === null || tip === undefined) return
            tip.background = tipBackground.createObject(tipHost)
            tip.contentItem = tipContent.createObject(tipHost, { tip: tip })
            tip.padding = 8
            tip.topPadding = 5
            tip.bottomPadding = 5
            tip.z = 1000
        }
    }
}
