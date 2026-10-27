import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// Frame of the full-window screens without a sidebar (frame.md "Conventions", 8a to 8c, accounts-and-server.md 8d): the 84 px top
// bar with the Lockup and a Mono screen label on the right, the content area with register marks, the 152 px open column with
// the vertical Mono label `UZUME · ...` and the 44 px hint row. The page goes into the default slot (one column, at most
// `contentMaxWidth` wide, scrolls when the window is low). `hints` follow the last input device (`device`): the app has no
// gamepad navigation in these screens yet, so the keyboard keys are shown.
//   hints: [{ kbd: "Enter", pad: "A", label: qsTr("Choose") }, ...]
// `grid` (the start and connect screens, usability pass 6): a centred 880 px grid instead of the open column: the page on the
// left, a 260 px help column (`helpItems` [{ title?, text }], `helpLines` at its foot) on the right, the Lockup at the left
// edge of the grid, no screen label in the top bar and no vertical text (the page shows its own step line).
Item {
    id: root
    property string screenLabel: ""
    property string openLabel: ""
    property var openLines: []
    property var hints: []
    property string device: "keyboard"      // keyboard | gamepad
    property real contentMaxWidth: 520
    property real contentSpacing: 28
    property bool grid: false
    property var helpItems: []
    property var helpLines: []
    readonly property real gridWidth: Math.min(880, width - 2 * Theme.space7)
    readonly property bool helpShown: grid && helpItems.length > 0 && gridWidth >= 720
    default property alias content: contentCol.data
    readonly property alias flick: flick
    readonly property alias column: contentCol

    Item {   // top bar: 84 px, lockup 28 px from the left and 32 from the top, label 56 px from the right
        id: topBar
        objectName: "startTopBar"
        x: 0
        y: 0
        width: parent.width
        height: 84
        UzLockup { objectName: "startLockup"; x: root.grid ? Math.max(Theme.space7, (root.width - root.gridWidth) / 2) : 28; y: 32 }
        Text {
            objectName: "startScreenLabel"
            visible: !root.grid
            anchors.right: parent.right
            anchors.rightMargin: Theme.space7
            y: 34
            text: root.screenLabel
            textFormat: Text.PlainText
            font.family: Theme.mono
            font.pixelSize: Theme.roles.meta.size
            font.letterSpacing: Theme.roles.meta.spacing
            font.capitalization: Font.AllUppercase
            color: Theme.dim
        }
    }

    RowLayout {   // content | open column
        x: root.grid ? (parent.width - root.gridWidth) / 2 : Theme.space7
        y: topBar.height
        width: root.grid ? root.gridWidth : parent.width - 2 * Theme.space7
        height: parent.height - topBar.height - hintBar.height
        spacing: Theme.space7

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumWidth: 0
            UzRegisterMarks { inset: 0 }
            Flickable {
                id: flick
                objectName: "startScroll"
                anchors.fill: parent
                anchors.leftMargin: Theme.space3 - 6   // room for the outer focus ring (4 px) inside the clip
                anchors.rightMargin: Theme.space3 - 6
                anchors.topMargin: 40 - 6
                anchors.bottomMargin: Theme.space3 - 6
                contentWidth: width
                contentHeight: contentCol.implicitHeight + 12
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar { }
                ColumnLayout {
                    id: contentCol
                    x: 6
                    y: 6
                    width: Math.min(flick.width - 12, root.contentMaxWidth)
                    spacing: root.contentSpacing
                }
            }
        }
        UzOpenColumn {
            visible: !root.grid
            Layout.fillHeight: true
            Layout.preferredWidth: Theme.openColumn
            label: root.openLabel
            lines: root.openLines
        }
        UzHelpColumn {
            objectName: "startHelpColumn"
            visible: root.helpShown
            Layout.fillHeight: true
            Layout.preferredWidth: 260
            Layout.maximumWidth: 260
            Layout.topMargin: 34
            width: 260
            items: root.helpItems
            lines: root.helpLines
        }
    }

    Item {   // hint row: 44 px, a rule-2 line above, the keys right-aligned
        id: hintBar
        objectName: "startHintRow"
        x: 0
        y: parent.height - height
        width: parent.width
        height: 44
        Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: Theme.rule2 }
        Row {
            anchors.right: parent.right
            anchors.rightMargin: Theme.space7
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.space3
            Repeater {
                model: root.hints.filter(h => root.device === "gamepad" ? h.pad !== undefined : h.kbd !== undefined)
                delegate: Loader {
                    required property var modelData
                    sourceComponent: root.device === "gamepad" ? padComp : kbdComp
                    Component { id: padComp; UzGamepadHint { k: modelData.pad; label: modelData.label || "" } }
                    Component { id: kbdComp; UzKbd { keys: modelData.kbd; label: modelData.label || "" } }
                }
            }
        }
    }
}
