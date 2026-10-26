import QtQuick

// Open column (design-system.md "Open right column"): the 152 px column at the right of every list page, 56 px from the
// content, with a vertical Mono label ("UZUME · LIBRARY · HOME SERVER", `vertical-label`, dim) at its top edge and optional
// meta lines at the bottom above a 24 px `rule` dash. No frame, no fill.
Item {
    id: root
    property string label: ""
    property var lines: []
    implicitWidth: Theme.openColumn
    implicitHeight: 320
    width: Theme.openColumn

    Item {   // vertical-rl: the text reads top to bottom
        id: vertical
        objectName: "openColumnLabel"
        anchors.right: parent.right
        anchors.top: parent.top
        width: txt.implicitHeight
        height: txt.implicitWidth
        Text {
            id: txt
            rotation: 90
            transformOrigin: Item.TopLeft
            x: txt.implicitHeight
            text: root.label
            textFormat: Text.PlainText
            font.family: Theme.mono
            font.pixelSize: Theme.roles.verticalLabel.size
            font.letterSpacing: Theme.roles.verticalLabel.spacing
            font.capitalization: Font.AllUppercase
            color: Theme.muted
        }
    }
    Column {
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Theme.space05
        spacing: Theme.space05
        visible: root.lines.length > 0
        Rectangle { width: 24; height: 1; color: Theme.rule }
        Repeater {
            model: root.lines
            delegate: Text {
                required property string modelData
                text: modelData
                textFormat: Text.PlainText
                font.family: Theme.mono
                font.pixelSize: Theme.roles.meta.size
                font.letterSpacing: 0.8
                font.capitalization: Font.AllUppercase
                color: Theme.muted
            }
        }
    }
}
