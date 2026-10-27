import QtQuick
import QtQuick.Layouts
import Uzume.App

// One row of the save timeline (docs/design/legacy/source/save-row.dc.html, dark; light follows Theme): rail 20 | text | actions.
// kind: "current" (filled node, "✓ Current"), "snap" (diamond, "◆ Snapshot") or "plain" (hollow node).
// The confirmation opens under the row, indented 32: confirm = "restore" (accent box) or "delete" (danger box).
ColumnLayout {
    id: root
    property string kind: "plain"
    property string versionText: ""
    property string label: ""
    property string meta: ""
    property bool cheats: false           // 11g: made from a save written with cheats or memory writes on
    property string autoKind: ""          // 11g: "" | "cheats" | "memory": the app's automatic safety copy; no Delete, Restore is the one action
    property bool showRestore: true
    property bool showDelete: false
    property bool restoreEnabled: true
    property bool deleteEnabled: true
    property bool muted: false            // read-only (Hub offline, restore blocked): the actions look disabled
    property string confirm: ""           // "" | "restore" | "delete"
    property string confirmTitle: ""
    property string confirmBody: ""
    property string confirmOk: ""
    property bool last: false
    signal restoreClicked()
    signal deleteClicked()
    signal confirmed()
    signal cancelled()

    readonly property bool isCurrent: kind === "current"
    readonly property bool isSnap: kind === "snap"
    readonly property bool isAuto: autoKind !== ""

    objectName: "saveRow"
    Layout.fillWidth: true
    Layout.minimumWidth: 0
    spacing: 0

    // Version row
    Item {
        id: versionRow
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        implicitHeight: stacked ? textCol.implicitHeight + 7 + 6 + Theme.row32 + 9 : Math.max(textCol.implicitHeight + 7 + 9, Theme.row44)

        // Natural (unclamped) width of the buttons. When the text column would get less than 90 px beside them (wide
        // fonts, narrow window) the actions move under the text instead of sticking out of the row.
        readonly property real actionsNaturalWidth: (restoreBtn.visible ? restoreBtn.implicitWidth : 0)
            + (deleteBtn.visible ? deleteBtn.implicitWidth : 0) + (restoreBtn.visible && deleteBtn.visible ? actions.spacing : 0)
        readonly property bool stacked: actionsNaturalWidth > 0 && width - 20 - 12 - 8 - actionsNaturalWidth < 90

        // Rail with node
        Item {
            id: rail
            width: 20
            height: parent.height
            Rectangle { x: 9.5; width: 1; height: 14; color: Theme.rule; visible: true }
            Rectangle { x: 9.5; y: 14 + 12; width: 1; height: parent.height - 26; color: Theme.rule; visible: !root.last }
            // current: 12px filled with a 3px gap ring and a 1px accent ring
            Rectangle {
                visible: root.isCurrent
                x: 4; y: 14; width: 12; height: 12
                color: Theme.accent
                Rectangle { anchors.centerIn: parent; width: 18; height: 18; color: "transparent"; border.width: 1; border.color: Theme.accent }
            }
            // snapshot: 9px diamond
            Rectangle {
                visible: root.isSnap
                x: 5.5; y: 15.5; width: 9; height: 9
                rotation: 45
                color: Theme.accent
            }
            // other: 10px hollow, 2px border
            Rectangle {
                visible: !root.isCurrent && !root.isSnap
                x: 5; y: 15; width: 10; height: 10
                color: Theme.ground
                border.width: 2
                border.color: Theme.dim
            }
        }

        // Text
        ColumnLayout {
            id: textCol
            anchors.left: rail.right
            anchors.leftMargin: 12
            anchors.right: versionRow.stacked ? parent.right : actions.left
            anchors.rightMargin: versionRow.stacked ? 0 : 8
            y: 7
            spacing: 3
            Flow {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                spacing: 8
                UzMono {
                    objectName: "historyTitle"
                    text: root.versionText
                    color: Theme.ink
                    font.pixelSize: Theme.roles.input.size
                    font.weight: Font.Medium
                }
                UzTag {
                    visible: root.isCurrent
                    small: true
                    tone: "ok"
                    text: qsTr("✓ Current")
                }
                UzTag {
                    visible: root.isSnap && !root.isAuto
                    small: true
                    tone: "warn"
                    text: qsTr("◆ Snapshot")
                }
                UzTag {
                    objectName: "historyAutoTag"
                    visible: root.isAuto
                    small: true
                    tone: "ok"
                    text: root.autoKind === "memory" ? qsTr("Before memory changes") : qsTr("Before cheats")
                }
                UzTag {
                    objectName: "historyCheatsTag"
                    visible: root.cheats && !root.isAuto
                    small: true
                    tone: "danger"
                    icon: "warn"
                    text: qsTr("Cheats")
                }
                UzLabel {
                    visible: root.label !== "" && !root.isAuto
                    objectName: "historyLabel"
                    width: Math.min(implicitWidth, textCol.width)
                    text: "“" + root.label + "”"
                    font.pixelSize: Theme.roles.input.size
                    font.weight: Font.Medium
                    elide: Text.ElideRight
                }
            }
            UzLabel {
                objectName: "historyMeta"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                text: root.meta
                color: Theme.muted
                font.pixelSize: Theme.roles.small.size
                wrapMode: Text.WordWrap
            }
        }

        // Actions
        Row {
            id: actions
            x: versionRow.stacked ? 32 : parent.width - width
            y: versionRow.stacked ? 7 + textCol.implicitHeight + 6 : 6
            spacing: 6
            // Stacked: every button gets at most its share of the row, the text elides.
            readonly property real maxButtonWidth: versionRow.stacked
                ? (versionRow.width - 32 - (restoreBtn.visible && deleteBtn.visible ? spacing : 0)) / ((restoreBtn.visible ? 1 : 0) + (deleteBtn.visible ? 1 : 0))
                : 100000
            UzButton {
                id: restoreBtn
                objectName: "restoreButton"
                visible: root.showRestore && !root.isCurrent
                enabled: root.restoreEnabled
                size: "sm"
                kind: root.isAuto ? "primary" : "outline"
                hPadding: Theme.space2
                on: root.confirm === "restore"
                width: Math.min(implicitWidth, actions.maxButtonWidth)
                opacity: enabled && !root.muted ? 1 : 0.5
                text: root.isAuto ? (root.autoKind === "memory" ? qsTr("Restore before changes") : qsTr("Restore before cheats")) : qsTr("Restore")
                Accessible.name: qsTr("Restore %1").arg(root.versionText)
                onClicked: root.restoreClicked()
            }
            UzButton {
                id: deleteBtn
                objectName: "deleteButton"
                visible: root.showDelete
                enabled: root.deleteEnabled
                size: "sm"
                hPadding: Theme.space2
                kind: "danger"
                width: Math.min(implicitWidth, actions.maxButtonWidth)
                opacity: enabled && !root.muted ? 1 : 0.5
                text: qsTr("Delete")
                Accessible.name: qsTr("Delete snapshot %1").arg(root.versionText)
                onClicked: root.deleteClicked()
            }
        }

        Rectangle { anchors.bottom: parent.bottom; anchors.left: rail.right; anchors.leftMargin: 12; anchors.right: parent.right; height: 1; color: Theme.rule }
    }

    // Confirmation under the row: a tinted plane with a 2 px marker (Yamabuki for restore, danger for delete), no frame.
    Rectangle {
        id: confirmBox
        objectName: root.confirm === "delete" ? "deleteConfirmBox" : "restoreConfirmBox"
        visible: root.confirm !== ""
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        Layout.topMargin: Theme.space1
        Layout.bottomMargin: Theme.space15
        Layout.leftMargin: 32
        implicitHeight: confirmCol.implicitHeight + 2 * Theme.space15
        color: root.confirm === "delete" ? Theme.dangerTint : Theme.selected
        Rectangle { width: 2; height: parent.height; color: root.confirm === "delete" ? Theme.danger : Theme.accent }
        Column {
            id: confirmCol
            x: Theme.space15 + 2
            y: Theme.space15
            width: parent.width - 2 * Theme.space15 - 2
            spacing: Theme.space15
            Column {
                width: parent.width
                spacing: Theme.space05
                UzLabel {
                    objectName: "confirmTitle"
                    width: parent.width
                    wrapMode: Text.WordWrap
                    text: root.confirmTitle
                    font.pixelSize: Theme.roles.itemTitleSm.size
                    font.weight: Theme.roles.itemTitleSm.weight
                    color: root.confirm === "delete" ? Theme.danger : Theme.ink
                }
                UzLabel {
                    objectName: "confirmBody"
                    width: parent.width
                    wrapMode: Text.WordWrap
                    text: root.confirmBody
                    font.pixelSize: Theme.roles.small.size
                    color: Theme.ink2
                }
            }
            Flow {
                width: parent.width
                spacing: Theme.space1
                layoutDirection: Qt.RightToLeft
                UzButton {
                    id: okBtn
                    objectName: root.confirm === "delete" ? "deleteConfirm" : "restoreConfirm"
                    size: "sm"
                    kind: root.confirm === "delete" ? "danger" : "primary"
                    text: root.confirmOk
                    onClicked: root.confirmed()
                }
                UzButton {
                    id: cancelBtn
                    objectName: root.confirm === "delete" ? "deleteCancel" : "restoreCancel"
                    size: "sm"
                    text: qsTr("Cancel")
                    onClicked: root.cancelled()
                }
            }
        }
    }
}
