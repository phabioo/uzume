import QtQuick
import QtQuick.Layouts

// Label / value rows of the stage diagnostics (design-system.md "StageDiagnostics"): labels in muted Mono caps at the left, values in
// ink Mono right aligned and elided; a "warn" row carries the Yamabuki "!" square. rows: [[label, value], [label, value, "warn"]].
GridLayout {
    id: root
    property var rows: []
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
            implicitHeight: Math.max(16, (isLabel ? labelText.implicitHeight : valueText.implicitHeight))
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
                    id: valueText
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
