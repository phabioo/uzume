import QtQuick

// Metric (accounts-and-server.md "Ma blocks" → Metrics): a column of the metrics row without a box: Mono label, a Light 28/32
// value, a thin Progress and one muted line (CPU 18 % · Memory 1.2 GB of 4 GB · Disk 61 % · Backup disk 92 %). A warning puts
// the "!" square (Yamabuki plane) before the label. No coloured bars. `leftRule` draws the `rule-2` line between two columns
// (not on the first). `fraction` < 0 hides the bar, `line` empty hides the muted line.
Item {
    id: root
    property string label: ""
    property string value: ""
    property real fraction: -1
    property string line: ""
    property bool warning: false
    property bool leftRule: false
    implicitWidth: 160
    implicitHeight: col.implicitHeight
    Accessible.role: Accessible.StaticText
    Accessible.name: root.label + ": " + root.value + (root.line !== "" ? ", " + root.line : "") + (root.warning ? ", " + qsTr("Warning") : "")

    Rectangle {
        visible: root.leftRule
        width: 1
        height: parent.height
        color: Theme.rule2
    }
    Column {
        id: col
        x: root.leftRule ? Theme.space3 : 0
        width: parent.width - x - (root.leftRule ? 0 : Theme.space3)
        spacing: Theme.space1
        Row {
            spacing: Theme.space1
            height: 16
            Rectangle {
                objectName: "metricWarning"
                visible: root.warning
                anchors.verticalCenter: parent.verticalCenter
                width: 14
                height: 14
                radius: 0
                color: Theme.accent
                Text {
                    anchors.centerIn: parent
                    text: "!"
                    font.family: Theme.sans
                    font.weight: Font.Bold
                    font.pixelSize: Theme.roles.meta.size
                    color: Theme.inkOnAccent
                }
            }
            Text {
                objectName: "metricLabel"
                anchors.verticalCenter: parent.verticalCenter
                text: root.label
                textFormat: Text.PlainText
                font.family: Theme.mono
                font.pixelSize: Theme.roles.sectionLabel.size
                font.weight: Font.Medium
                font.letterSpacing: Theme.roles.sectionLabel.spacing
                font.capitalization: Font.AllUppercase
                color: Theme.muted
            }
        }
        Text {
            objectName: "metricValue"
            width: parent.width
            elide: Text.ElideRight
            text: root.value
            textFormat: Text.PlainText
            font.family: Theme.sans
            font.pixelSize: 28
            font.weight: Font.Light
            lineHeight: 32
            lineHeightMode: Text.FixedHeight
            color: Theme.ink
        }
        UzProgress {
            objectName: "metricBar"
            visible: root.fraction >= 0
            width: parent.width
            thin: true
            value: root.fraction
            Accessible.name: root.label
        }
        Text {
            objectName: "metricLine"
            visible: root.line !== ""
            width: parent.width
            elide: Text.ElideRight
            text: root.line
            textFormat: Text.PlainText
            font.family: Theme.sans
            font.pixelSize: Theme.roles.small.size
            color: Theme.muted
        }
    }
}
