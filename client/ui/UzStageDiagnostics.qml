import QtQuick
import QtQuick.Layouts

// Stage diagnostics (design-system.md "StageDiagnostics"): the F3 block in the top right corner of the game stage: 232 px on
// `surface` in a `rule` frame, a Mono label with "Copy" (accent text; "Copied" with a check for a moment), label / value rows in
// Mono 10 (labels muted uppercase, values ink, right aligned; a "warn" row carries the Yamabuki "!" square).
// rows: [[label, value], [label, value, "warn"]]. `copyRequested()` is emitted on Copy; the owner puts the text on the clipboard.
Rectangle {
    id: root
    property string title: qsTr("Diagnostics")
    property var rows: []
    property bool copied: false
    signal copyRequested()

    implicitWidth: 232
    implicitHeight: head.height + 1 + grid.implicitHeight + Theme.space1 + Theme.space15
    radius: 0
    color: Theme.surface
    border.width: 1
    border.color: Theme.rule
    Accessible.role: Accessible.Pane
    Accessible.name: root.title

    Timer { id: reset; interval: 2000; onTriggered: root.copied = false }

    Item {
        id: head
        x: 1
        y: 1
        width: parent.width - 2
        height: 36
        Text {
            anchors.left: parent.left
            anchors.leftMargin: Theme.space15
            anchors.verticalCenter: parent.verticalCenter
            text: root.title
            textFormat: Text.PlainText
            font.family: Theme.mono
            font.pixelSize: Theme.roles.meta.size
            font.weight: Font.Medium
            font.letterSpacing: 1.4
            font.capitalization: Font.AllUppercase
            color: Theme.muted
        }
        Item {
            id: copyBtn
            objectName: "diagCopy"
            anchors.right: parent.right
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            width: copyRow.implicitWidth + 2 * Theme.space1
            height: 24
            activeFocusOnTab: true
            Accessible.role: Accessible.Button
            Accessible.name: root.copied ? qsTr("Copied") : qsTr("Copy")
            function trigger() { root.copied = true; reset.restart(); root.copyRequested() }
            Keys.onPressed: (event) => {
                if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) { trigger(); event.accepted = true }
            }
            Rectangle { anchors.fill: parent; color: copyTap.pressed ? Theme.pressed : (copyHover.hovered ? Theme.hover : "transparent") }
            Row {
                id: copyRow
                anchors.centerIn: parent
                spacing: 4
                UzGlyph { visible: root.copied; anchors.verticalCenter: parent.verticalCenter; name: "check"; size: 10; color: Theme.ink }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.copied ? qsTr("Copied") : qsTr("Copy")
                    font.family: Theme.sans
                    font.pixelSize: Theme.roles.small.size
                    font.weight: Font.Medium
                    color: Theme.ink
                }
            }
            HoverHandler { id: copyHover; cursorShape: Qt.PointingHandCursor }
            TapHandler { id: copyTap; onTapped: copyBtn.trigger() }
            UzFocusRing { visible: copyBtn.activeFocus }
        }
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.rule2 }
    }
    GridLayout {
        id: grid
        objectName: "diagRows"
        x: 1 + Theme.space15
        y: head.height + 1 + Theme.space1
        width: parent.width - 2 - 2 * Theme.space15
        columns: 2
        columnSpacing: Theme.space15
        rowSpacing: 2
        Repeater {
            model: root.rows.length * 2
            delegate: Item {
                id: cell
                required property int index
                readonly property var row: root.rows[Math.floor(index / 2)]
                readonly property bool isLabel: index % 2 === 0
                Layout.fillWidth: !isLabel
                Layout.minimumWidth: 0
                implicitHeight: 16
                implicitWidth: isLabel ? labelText.implicitWidth : valueRow.implicitWidth
                Text {
                    id: labelText
                    visible: cell.isLabel
                    text: cell.row[0]
                    textFormat: Text.PlainText
                    font.family: Theme.mono
                    font.pixelSize: Theme.roles.meta.size
                    font.letterSpacing: 0.6
                    font.capitalization: Font.AllUppercase
                    color: Theme.muted
                }
                Row {
                    id: valueRow
                    visible: !cell.isLabel
                    anchors.right: parent.right
                    spacing: 4
                    Rectangle {
                        visible: !cell.isLabel && cell.row[2] === "warn"
                        width: 12; height: 12
                        anchors.verticalCenter: parent.verticalCenter
                        color: Theme.accent
                        Text { anchors.centerIn: parent; text: "!"; font.family: Theme.sans; font.pixelSize: 9; font.weight: Font.Bold; color: Theme.inkOnAccent }
                    }
                    Text {
                        text: cell.isLabel ? "" : cell.row[1]
                        textFormat: Text.PlainText
                        width: Math.min(implicitWidth, Math.max(0, cell.width - 16))
                        elide: Text.ElideRight
                        font.family: Theme.mono
                        font.pixelSize: Theme.roles.meta.size
                        color: Theme.ink
                    }
                }
            }
        }
    }
}
