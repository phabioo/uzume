import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// Quick menu (board 7-3, ADR 0026 D1): what Esc, the Pause button and Start on a pad open. It replaces the old pause dialog: a 720 px
// surface with a rule frame on the scrim over the stage (the header and the panel stay). Label "Paused · <title>", the title
// "Quick menu", a status line (save state, who watches), "▶ Resume" as the one primary with the focus, four groups in two columns
// (Game, Play | Together, Help) and a footer with the pad hints and "Quit game…". Up / Down move inside a column, Left / Right switch
// the column, Enter (A) activates, Esc (B) resumes. The rows with "▸" open the speed, sound or screen menus of the header anchored
// at the row. GameScreen owns the state and runs the actions: this item only shows values and reports `chosen(name, anchor)`.
// objectNames: quickMenu, quickMenuTitle, quickMenuStatus, quickMenuResume, quickMenuQuit, quickMenuFullscreenNote, quickRow_<name>.
FocusScope {
    id: root
    property bool shown: false
    property string gameTitle: ""
    property string statusText: ""
    property string speedValue: ""
    property string soundValue: ""
    property string screensValue: ""
    property string shareValue: ""
    property string padModifier: ""
    property bool canSpeedUp: false
    property bool canScreens: false
    property bool canSnapshot: true
    property bool canStates: false
    property bool fullscreen: false
    property bool quitting: false
    property var hotkeys: ({})
    signal resumeRequested()
    signal quitRequested()
    signal chosen(string name, Item anchor)

    readonly property var leftGroups: [
        { title: qsTr("Game"), rows: [
            { name: "saveState", label: qsTr("Save state"), key: root.hotkeys.saveState || "F6", enabled: root.canStates, hidden: !root.canStates },
            { name: "loadState", label: qsTr("Load state"), key: root.hotkeys.loadState || "F8", enabled: root.canStates, hidden: !root.canStates },
            { name: "snapshot", label: qsTr("Save snapshot"), key: root.hotkeys.snapshot || "F5", enabled: root.canSnapshot },
            { name: "saves", label: qsTr("Manage saves…") },
            { name: "screenshot", label: qsTr("Screenshot"), key: root.hotkeys.screenshot || "F12" },
            { name: "gameSettings", label: qsTr("Settings for this game…") },
            { name: "reset", label: qsTr("Reset game…"), danger: true }
        ] },
        { title: qsTr("Together"), rows: [
            { name: "share", label: qsTr("Share…"), value: root.shareValue },
            { name: "multiview", label: qsTr("Multiview") }
        ] }
    ]
    readonly property var rightGroups: [
        { title: qsTr("Play"), rows: [
            { name: "speedup", label: qsTr("Speed-up"), value: root.speedValue, menu: true, hidden: !root.canSpeedUp },
            { name: "sound", label: qsTr("Sound"), value: root.soundValue, menu: true },
            { name: "screens", label: qsTr("Screens"), value: root.screensValue, menu: true, hidden: !root.canScreens },
            { name: "fullscreen", label: root.fullscreen ? qsTr("Leave fullscreen") : qsTr("Fullscreen"), key: root.hotkeys.fullscreen || "F11" },
            { name: "diagnostics", label: qsTr("Diagnostics"), key: root.hotkeys.diagnostics || "F3" }
        ] },
        { title: qsTr("Help"), rows: [
            { name: "hotkeys", label: qsTr("Hotkeys"), value: qsTr("Hold %1").arg(root.padModifier !== "" ? root.padModifier : qsTr("Select")) },
            { name: "report", label: qsTr("Report a problem…") }
        ] }
    ]

    anchors.fill: parent
    z: 100
    visible: shown
    focus: shown
    onShownChanged: if (shown) Qt.callLater(root.takeFocus)
    onVisibleChanged: if (visible) Qt.callLater(root.takeFocus)   // the game view comes back while the game is still paused
    Keys.onEscapePressed: (event) => { if (root.visible) { root.resumeRequested(); event.accepted = true } }

    property Item lastRow: null
    function takeFocus() {
        if (!root.shown || !root.visible) return   // the game is in the background (Library shown): nothing takes the keys
        if (root.lastRow && root.lastRow.visible && root.lastRow.enabled) root.lastRow.forceActiveFocus(Qt.TabFocusReason)
        else resumeButton.forceActiveFocus(Qt.TabFocusReason)
    }
    // After a menu, dialog or popover that was opened from a row closed: the focus goes back to the menu.
    function restoreFocus() { if (root.visible && !root.activeFocus) root.takeFocus() }
    function focusFirst() { root.lastRow = null; root.takeFocus() }
    function rowsOf(col: Item): var {
        const out = []
        for (let g = 0; g < col.children.length; ++g) {
            const grp = col.children[g]
            for (let i = 0; i < grp.children.length; ++i)
                if (grp.children[i].isQuickRow === true && grp.children[i].visible && grp.children[i].enabled) out.push(grp.children[i])
        }
        return out
    }
    function move(from: Item, key: int) {
        const lists = [root.rowsOf(leftCol), root.rowsOf(rightCol)]
        let target = null
        if (from === resumeButton) {
            if (key === Qt.Key_Down && lists[0].length > 0) target = lists[0][0]
        } else if (from === quitButton) {
            if (key === Qt.Key_Up) target = lists[0].length > 0 ? lists[0][lists[0].length - 1] : resumeButton
        } else {
            const c = from.col
            const i = lists[c].indexOf(from)
            if (key === Qt.Key_Up) target = i > 0 ? lists[c][i - 1] : resumeButton
            else if (key === Qt.Key_Down) target = i < lists[c].length - 1 ? lists[c][i + 1] : quitButton
            else if (key === Qt.Key_Left && c === 1) target = lists[0][Math.min(i, lists[0].length - 1)]
            else if (key === Qt.Key_Right && c === 0) target = lists[1][Math.min(i, lists[1].length - 1)]
        }
        if (target) target.forceActiveFocus(Qt.TabFocusReason)
    }

    Rectangle {   // the scrim: swallows clicks, the stage stays dimmed behind
        objectName: "quickMenuScrim"
        anchors.fill: parent
        color: Theme.scrim
        MouseArea { anchors.fill: parent; hoverEnabled: true; acceptedButtons: Qt.AllButtons; onWheel: (wheel) => wheel.accepted = true }
    }

    Rectangle {
        id: frame
        objectName: "quickMenu"
        anchors.centerIn: parent
        width: Math.min(720, Math.max(0, root.width - 2 * Theme.space2))
        height: Math.min(chrome.implicitHeight + 2 * 24, Math.max(0, root.height - 2 * Theme.space2))
        color: Theme.surface
        border.width: 1
        border.color: Theme.rule
        clip: true
        MouseArea { anchors.fill: parent }
        Accessible.role: Accessible.Dialog
        Accessible.name: qsTr("Quick menu")

        ColumnLayout {
            id: chrome
            x: 28
            y: 24
            width: parent.width - 56
            height: parent.height - 48
            spacing: 18

            RowLayout {   // head: label, title, status | Resume
                Layout.fillWidth: true
                spacing: Theme.space2
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    spacing: Theme.space05
                    Text {
                        objectName: "quickMenuLabel"
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        text: qsTr("Paused · %1").arg(root.gameTitle)
                        textFormat: Text.PlainText
                        font.family: Theme.mono
                        font.pixelSize: Theme.roles.meta.size
                        font.weight: Font.Medium
                        font.letterSpacing: 1.4
                        font.capitalization: Font.AllUppercase
                        color: Theme.dim
                    }
                    Text {
                        objectName: "quickMenuTitle"
                        Layout.fillWidth: true
                        text: qsTr("Quick menu")
                        textFormat: Text.PlainText
                        font.family: Theme.sans
                        font.pixelSize: Theme.roles.dialogTitle.size
                        font.letterSpacing: Theme.roles.dialogTitle.spacing
                        lineHeight: Theme.roles.dialogTitle.line
                        lineHeightMode: Text.FixedHeight
                        color: Theme.ink
                    }
                    Text {
                        objectName: "quickMenuStatus"
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        text: root.statusText
                        textFormat: Text.PlainText
                        font.family: Theme.sans
                        font.pixelSize: Theme.roles.small.size
                        lineHeight: Theme.roles.small.line
                        lineHeightMode: Text.FixedHeight
                        color: Theme.muted
                    }
                }
                UzButton {
                    id: resumeButton
                    objectName: "quickMenuResume"
                    Layout.alignment: Qt.AlignTop
                    kind: "primary"
                    implicitHeight: Math.max(44, Theme.row44)
                    hPadding: 40
                    text: "▶ " + qsTr("Resume")
                    value: "Esc"
                    onClicked: root.resumeRequested()
                    Keys.onPressed: (event) => {
                        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) { root.resumeRequested(); event.accepted = true }
                        else if (event.key === Qt.Key_Down) { root.move(resumeButton, event.key); event.accepted = true }
                    }
                }
            }

            Flickable {
                id: flick
                objectName: "quickMenuScroll"
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumHeight: 0
                Layout.preferredHeight: groups.implicitHeight
                contentWidth: width
                contentHeight: groups.implicitHeight
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar { policy: flick.contentHeight > flick.height + 1 ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff }
                function ensureVisible(item: Item) {
                    const y = item.mapToItem(groups, 0, 0).y
                    if (y < contentY) contentY = y
                    else if (y + item.height > contentY + height) contentY = y + item.height - height
                }

                Row {
                    id: groups
                    width: flick.width - (flick.contentHeight > flick.height + 1 ? 14 : 0)
                    spacing: 28
                    readonly property real colWidth: (width - spacing) / 2
                    Column {
                        id: leftCol
                        objectName: "quickMenuLeft"
                        width: groups.colWidth
                        spacing: 16
                        Repeater { model: root.leftGroups; delegate: groupDelegate }
                    }
                    Column {
                        id: rightCol
                        objectName: "quickMenuRight"
                        width: groups.colWidth
                        spacing: 16
                        Repeater { model: root.rightGroups; delegate: groupDelegate }
                    }
                }
            }

            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.rule2 }
            RowLayout {   // footer: pad hints, Quit game…
                Layout.fillWidth: true
                Layout.topMargin: -4
                spacing: Theme.space2
                Row {
                    spacing: Theme.space2
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    UzGamepadHint { k: "A"; label: qsTr("Choose") }
                    UzGamepadHint { k: "B"; label: qsTr("Resume") }
                }
                Row {
                    visible: root.quitting
                    spacing: Theme.space1
                    UzSpinner { size: 14; color: Theme.accent; anchors.verticalCenter: parent.verticalCenter }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Saving and syncing…")
                        textFormat: Text.PlainText
                        font.family: Theme.sans
                        font.pixelSize: Theme.roles.buttonSm.size
                        color: Theme.ink
                    }
                }
                UzButton {
                    id: quitButton
                    objectName: "quickMenuQuit"
                    visible: !root.quitting
                    size: "sm"
                    text: qsTr("Quit game…")
                    onClicked: root.quitRequested()
                    onActiveFocusChanged: if (activeFocus) root.lastRow = null
                    Keys.onPressed: (event) => {
                        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) { root.quitRequested(); event.accepted = true }
                        else if (event.key === Qt.Key_Up) { root.move(quitButton, event.key); event.accepted = true }
                    }
                }
            }
        }
    }

    Component {
        id: groupDelegate
        Column {
            id: group
            required property var modelData
            width: parent ? parent.width : 0
            spacing: 0
            Text {
                text: group.modelData.title
                textFormat: Text.PlainText
                bottomPadding: 6
                font.family: Theme.mono
                font.pixelSize: Theme.roles.meta.size
                font.weight: Font.Medium
                font.letterSpacing: 1.54
                font.capitalization: Font.AllUppercase
                color: Theme.dim
            }
            Repeater {
                model: group.modelData.rows.filter(r => r.hidden !== true)
                delegate: rowDelegate
            }
        }
    }

    Component {
      id: rowDelegate
      Button {
        id: qr
        required property var modelData
        required property int index
        readonly property var row: modelData
        readonly property int col: parent && parent.parent === rightCol ? 1 : 0
        width: parent ? parent.width : 0
        readonly property bool isQuickRow: true
        objectName: "quickRow_" + (row.name ?? "")
        implicitHeight: Math.max(44, Theme.row44)
        enabled: row.enabled !== false
        hoverEnabled: true
        padding: 0
        activeFocusOnTab: true
        Accessible.name: (row.label ?? "") + (row.value ? " " + row.value : "")
        onClicked: root.chosen(row.name, qr)
        onActiveFocusChanged: if (activeFocus) { root.lastRow = qr; flick.ensureVisible(qr) }
        Keys.onPressed: (event) => {
            if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) { qr.clicked(); event.accepted = true }
            else if (event.key === Qt.Key_Up || event.key === Qt.Key_Down || event.key === Qt.Key_Left || event.key === Qt.Key_Right) {
                root.move(qr, event.key)
                event.accepted = true
            }
        }
        background: Item {
            Rectangle {
                anchors.fill: parent
                color: qr.down ? Theme.pressed : (qr.hovered && qr.enabled ? Theme.hover : "transparent")
            }
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.rule2 }
            UzFocusRing { inset: true; visible: qr.visualFocus }
        }
        contentItem: RowLayout {
            spacing: Theme.space15
            Text {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                Layout.leftMargin: Theme.space15
                elide: Text.ElideRight
                text: qr.row.label ?? ""
                textFormat: Text.PlainText
                font.family: Theme.sans
                font.pixelSize: 14
                color: !qr.enabled ? Theme.disabledInk : qr.row.danger === true ? Theme.danger : Theme.ink2
            }
            Text {
                visible: (qr.row.value ?? "") !== ""
                text: qr.row.value ?? ""
                textFormat: Text.PlainText
                elide: Text.ElideRight
                Layout.maximumWidth: qr.width / 2
                font.family: Theme.mono
                font.pixelSize: Theme.roles.meta.size
                font.weight: Font.Medium
                font.letterSpacing: 0.66
                font.capitalization: Font.AllUppercase
                color: qr.enabled ? Theme.ink : Theme.disabledInk
            }
            UzGlyph {
                visible: qr.row.menu === true
                name: "right"
                size: 12
                color: Theme.ink2
            }
            UzKeyCap {
                visible: (qr.row.key ?? "") !== ""
                text: qr.row.key ?? ""
                disabled: !qr.enabled
            }
            Item { Layout.preferredWidth: Theme.space05 - 4 }
        }
      }
    }
}
