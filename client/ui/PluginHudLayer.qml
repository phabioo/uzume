import QtQuick
import Uzume.App

// Plugin surfaces on the play area (docs/design/legacy/plugins.md 4e HUD, 4g-2 notices): HUD widgets in the bottom-left,
// bottom-right and top-left corners (top right belongs to the diagnostics overlay), 16 px from the edge, and the notice
// stack at the bottom right above the bottom-right HUD stack. Placed as a sibling of the diagnostics overlay; it takes no
// mouse input outside the widgets, notices and menus. The Hide-all state (F4) is `plugins.hudAllHidden`.
Item {
    id: root
    objectName: "pluginHudLayer"
    required property PlayerController player
    readonly property PluginsController plugins: player.plugins
    property int infoTimeoutMs: 8000            // info notices dismiss themselves
    property int hintMs: 2000                   // "HUD hidden · F4 to show"
    // Space kept free at the bottom: in the Multiview the tile name bar (40 px + frame) sits there and no widget may cover it.
    property real bottomInset: 0
    signal detailsRequested()                   // notice "Details": open the Plugin console tab

    anchors.fill: parent
    z: 9

    // ---- HUD
    readonly property var cornerLists: {
        var r = { "top-left": [], "bottom-left": [], "bottom-right": [] }
        var w = plugins.hudWidgets
        for (var i = 0; i < w.length; ++i) {
            if (w[i].hidden || !r[w[i].corner]) continue
            if (r[w[i].corner].length < 4) r[w[i].corner].push(w[i])
        }
        return r
    }
    readonly property bool hudShown: !plugins.hudAllHidden

    PluginHudStack {
        id: topLeft
        objectName: "hudStack_top-left"
        x: Theme.space2; y: Theme.space2
        visible: root.hudShown
        plugins: root.plugins
        widgets: root.cornerLists["top-left"]
        menuAnchor: root.menuAnchor
        onMenuRequested: (entry, anchor) => root.openHudMenu(entry, anchor)
    }
    PluginHudStack {
        id: bottomLeft
        objectName: "hudStack_bottom-left"
        x: Theme.space2; y: root.height - root.bottomInset - Theme.space2 - height
        visible: root.hudShown
        upwards: true
        plugins: root.plugins
        widgets: root.cornerLists["bottom-left"]
        menuAnchor: root.menuAnchor
        onMenuRequested: (entry, anchor) => root.openHudMenu(entry, anchor)
    }
    PluginHudStack {
        id: bottomRight
        objectName: "hudStack_bottom-right"
        x: root.width - Theme.space2 - width; y: root.height - root.bottomInset - Theme.space2 - height
        visible: root.hudShown
        upwards: true
        plugins: root.plugins
        widgets: root.cornerLists["bottom-right"]
        menuAnchor: root.menuAnchor
        onMenuRequested: (entry, anchor) => root.openHudMenu(entry, anchor)
    }

    // Hidden widgets can always be shown again: this pill (bottom centre) opens a menu with "Show <plugin> widget" and
    // "Show all HUD widgets" while at least one widget or the whole HUD is hidden.
    readonly property var hiddenWidgets: {
        var out = [], w = plugins.hudWidgets
        for (var i = 0; i < w.length; ++i) if (w[i].hidden) out.push(w[i])
        return out
    }
    Rectangle {
        id: restorePill
        objectName: "hudRestoreButton"
        visible: !root.hudShown || root.hiddenWidgets.length > 0
        anchors.horizontalCenter: parent.horizontalCenter
        y: root.height - root.bottomInset - 62 - height     // above the bottom edge of the game, not on it (3h-3, P45)
        width: restoreLabel.implicitWidth + 2 * Theme.space15
        height: Theme.row28
        color: Theme.surface
        border.width: 1
        border.color: Theme.rule
        opacity: restoreHover.hovered || (menu.open && menu.anchorItem === restorePill) ? 1 : 0.7
        UzLabel {
            textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
            id: restoreLabel
            anchors.centerIn: parent
            text: (!root.hudShown ? qsTr("HUD hidden") : (root.hiddenWidgets.length === 1 ? qsTr("1 widget hidden") : qsTr("%1 widgets hidden").arg(root.hiddenWidgets.length))) + " · " + qsTr("Show")
            font.pixelSize: Theme.roles.small.size
            color: Theme.ink2
        }
        HoverHandler { id: restoreHover; cursorShape: Qt.PointingHandCursor }
        // Left click shows everything again at once; the menu with the per-plugin entries stays on the right click.
        TapHandler { acceptedButtons: Qt.LeftButton; onTapped: root.showAllHud() }
        TapHandler { acceptedButtons: Qt.RightButton; onTapped: root.openRestoreMenu(restorePill) }
    }
    function showAllHud() {
        closeMenu()
        var h = root.hiddenWidgets
        plugins.hudAllHidden = false
        for (var i = 0; i < h.length; ++i) plugins.showHud(h[i].pluginId)
    }

    // "HUD hidden · F4 to show" for 2 s after Hide all
    Timer { id: hintTimer; interval: root.hintMs }
    Connections {
        target: root.plugins
        function onHudAllHiddenChanged() { if (root.plugins.hudAllHidden) hintTimer.restart(); else hintTimer.stop() }
    }
    Rectangle {
        objectName: "hudHiddenHint"
        visible: hintTimer.running
        anchors.horizontalCenter: parent.horizontalCenter
        y: Theme.space2
        width: hintLabel.implicitWidth + 2 * Theme.space15
        height: Theme.row28
        color: Theme.surface
        border.width: 1
        border.color: Theme.rule
        UzLabel { textFormat: Text.PlainText; id: hintLabel; anchors.centerIn: parent; text: qsTr("HUD hidden · F4 to show"); font.pixelSize: Theme.roles.small.size; color: Theme.ink2 }
    }

    // ---- notices
    ListModel { id: toastIds }
    function syncToasts() {
        var t = plugins.toasts, ids = [], have = {}
        for (var i = 0; i < t.length; ++i) ids.push(t[i].toastId)
        for (var j = toastIds.count - 1; j >= 0; --j) if (ids.indexOf(toastIds.get(j).tid) < 0) toastIds.remove(j)
        for (var k = 0; k < toastIds.count; ++k) have[toastIds.get(k).tid] = true
        for (var m = 0; m < ids.length; ++m) if (!have[ids[m]]) toastIds.append({ tid: ids[m] })
    }
    function toastData(tid) {
        var t = plugins.toasts
        for (var i = 0; i < t.length; ++i) if (t[i].toastId === tid) return t[i]
        return null
    }
    Component.onCompleted: syncToasts()
    Connections {
        target: root.plugins
        function onToastsChanged() { root.syncToasts(); root.syncMenuToast() }
    }
    readonly property int hiddenToasts: Math.max(0, toastIds.count - 3)

    Column {
        id: toastStack
        objectName: "toastStack"
        width: 390
        spacing: Theme.space1
        x: root.width - Theme.space2 - width
        y: root.height - root.bottomInset - (bottomRight.height > 0 && bottomRight.visible ? bottomRight.height + Theme.space1 : 0) - Theme.space2 - height
        Rectangle {
            objectName: "toastPill"
            visible: root.hiddenToasts > 0
            anchors.right: parent.right
            width: pillLabel.implicitWidth + 2 * Theme.space15
            height: Theme.row28
            color: Theme.surface
            border.width: 1
            border.color: Theme.rule
            UzLabel {
                textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                id: pillLabel
                anchors.centerIn: parent
                text: root.hiddenToasts === 1 ? qsTr("+1 earlier notice") : qsTr("+%1 earlier notices").arg(root.hiddenToasts)
                font.pixelSize: Theme.roles.small.size
                color: Theme.ink2
            }
        }
        Repeater {
            model: toastIds
            delegate: Item {
                id: slot
                required property int tid
                required property int index
                readonly property var toast: root.toastData(tid)
                width: toastStack.width
                visible: toast !== null && index >= toastIds.count - 3
                height: visible && loader.item ? loader.item.height : 0
                Loader {
                    id: loader
                    width: parent.width
                    active: slot.toast !== null
                    sourceComponent: PluginToast {
                        toast: slot.toast
                        marker: root.plugins.originMarker(slot.toast.origin || "")
                        onDismissRequested: root.plugins.dismissToast(slot.tid)
                        onActionPicked: (act) => root.toastAction(slot.toast, act)
                        onMenuRequested: (anchor) => root.openToastMenu(slot.toast, anchor)
                    }
                }
                Timer {
                    interval: root.infoTimeoutMs
                    running: slot.toast !== null && slot.toast.tone === "info" && !(loader.item && loader.item.hovered)
                    onTriggered: root.plugins.dismissToast(slot.tid)
                }
            }
        }
    }

    function toastAction(toast, act) {
        if (toast.host) {
            if (act.action === "restart") plugins.reloadPlugin(act.value)
            else if (act.action === "details") root.detailsRequested()
        } else {
            plugins.sendAction(toast.pluginId, "toast", act.action, act.value)
        }
        plugins.dismissToast(toast.toastId)
    }

    // ---- menus
    // Closes the open HUD or notice menu; true when there was one (Esc closes it before it would pause the game).
    function closeMenu() {
        if (!menu.open) return false
        menu.open = false
        return true
    }
    property bool wasPaused: false
    readonly property Item menuAnchor: menu.open ? menu.anchorItem : null
    // The menu goes when the game resumes, the whole HUD is hidden, or the widget / notice it belongs to is gone.
    Connections {
        target: root.player.gameSession
        function onStateChanged() {
            var p = root.player.gameSession.paused
            if (root.wasPaused && !p) root.closeMenu()
            root.wasPaused = p
        }
    }
    onHudShownChanged: if (!hudShown && menuKind === "hud") closeMenu()
    onCornerListsChanged: {
        if (!menu.open || menuKind !== "hud" || !menuEntry) return
        for (var c in cornerLists)
            for (var i = 0; i < cornerLists[c].length; ++i)
                if (cornerLists[c][i].pluginId === menuEntry.pluginId) return
        closeMenu()
    }
    onHiddenToastsChanged: syncMenuToast()
    function syncMenuToast() {
        if (menu.open && menuKind === "toast" && menuEntry && toastData(menuEntry.toastId) === null) closeMenu()
    }
    property var menuEntry: null                 // HUD widget entry or toast of the open menu
    property string menuKind: ""                 // "hud" | "toast"
    function openHudMenu(entry, anchor) {
        menuEntry = entry
        menuKind = "hud"
        menu.above = false
        menu.menuWidth = 250
        var c = entry.corner
        menu.items = [
            { id: "hide", text: qsTr("Hide this widget"), trailing: plugins.activeHotkeyKey(entry.pluginId, "toggle-hud") },
            { id: "corner", text: qsTr("Move to corner"), choices: [
                { id: "corner-bottom-left", glyph: "↙", tip: qsTr("Bottom left"), checked: c === "bottom-left" },
                { id: "corner-bottom-right", glyph: "↘", tip: qsTr("Bottom right"), checked: c === "bottom-right" },
                { id: "corner-top-left", glyph: "↖", tip: qsTr("Top left"), checked: c === "top-left" } ] },
            { id: "hideAll", text: qsTr("Hide all HUD widgets"), trailing: "F4", divider: true }
        ].concat(showEntries())
        menu.anchorItem = anchor
        menu.open = true
    }
    // "Show <plugin> widget" for every hidden widget.
    function showEntries() {
        var out = [], h = root.hiddenWidgets
        for (var i = 0; i < h.length; ++i) out.push({ id: "show:" + h[i].pluginId, text: qsTr("Show %1 widget").arg(h[i].name || h[i].pluginId) })
        return out
    }
    function openRestoreMenu(anchor) {
        menuEntry = null
        menuKind = "restore"
        menu.above = true
        menu.menuWidth = 250
        var items = root.hudShown ? [{ id: "hideAll", text: qsTr("Hide all HUD widgets"), trailing: "F4" }]
                                  : [{ id: "showAll", text: qsTr("Show all HUD widgets"), trailing: "F4" }]
        menu.items = items.concat(showEntries())
        menu.anchorItem = anchor
        menu.open = true
    }
    function openToastMenu(toast, anchor) {
        menuEntry = toast
        menuKind = "toast"
        menu.above = true
        menu.menuWidth = 250
        var mode = plugins.toastMuteMode(toast.pluginId)
        menu.items = [
            { id: "mute", text: qsTr("Mute %1's notices").arg(toast.pluginName), checked: mode === "always" },
            { id: "important", text: qsTr("Only important ones"), checked: mode === "important" },
            { id: "settings", text: qsTr("Notice settings…"), divider: true }
        ]
        menu.anchorItem = anchor
        menu.open = true
    }
    PluginMenu {
        id: menu
        onCloseRequested: open = false
        onPicked: (id) => {
            open = false
            var e = root.menuEntry
            if (id.indexOf("show:") === 0) { root.plugins.showHud(id.substring(5)); return }
            if (id === "showAll") { root.plugins.hudAllHidden = false; return }
            if (root.menuKind === "restore") { if (id === "hideAll") root.plugins.hudAllHidden = true; return }
            if (!e) return
            if (root.menuKind === "hud") {
                if (id === "hide") root.plugins.hideHud(e.pluginId)
                else if (id === "hideAll") root.plugins.hudAllHidden = true
                else if (id.indexOf("corner-") === 0) root.plugins.setHudCorner(e.pluginId, id.substring(7))
            } else {
                if (id === "mute") root.plugins.muteToasts(e.pluginId, "always")
                else if (id === "important") root.plugins.muteToasts(e.pluginId, "important")
                else if (id === "settings") { root.player.showPluginSettings("") }
            }
        }
    }
}
