import QtQuick

// Help column (design-system.md "HelpColumn"): the open column of long admin pages with a HELP slot: a Mono "HELP" head on a
// `rule` underline, paragraphs [{ title?, text }] separated by `rule-2` lines (12 px text, muted), `lines` at the foot.
// Extra content goes into the default slot below the paragraphs.
Item {
    id: root
    property string label: ""
    property var items: []
    property var lines: []
    default property alias extra: extraBox.data
    implicitWidth: Theme.openColumn
    implicitHeight: Math.max(420, help.implicitHeight + foot.implicitHeight + Theme.space3)
    width: Theme.openColumn

    UzOpenColumn { anchors.fill: parent; label: root.label }
    Column {
        id: help
        objectName: "helpColumnBody"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.rightMargin: 28
        Text {
            width: parent.width
            bottomPadding: Theme.space1
            text: qsTr("Help")
            textFormat: Text.PlainText
            font.family: Theme.mono
            font.pixelSize: Theme.roles.meta.size
            font.weight: Font.Medium
            font.letterSpacing: 1.4
            font.capitalization: Font.AllUppercase
            color: Theme.muted
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.rule }
        }
        Repeater {
            model: root.items
            delegate: Item {
                id: it
                required property var modelData
                width: help.width
                implicitHeight: itemCol.implicitHeight + 2 * Theme.space15
                height: implicitHeight
                Column {
                    id: itemCol
                    y: Theme.space15
                    width: parent.width
                    spacing: 2
                    Text {
                        visible: it.modelData.title !== undefined
                        width: parent.width
                        wrapMode: Text.Wrap
                        text: it.modelData.title || ""
                        textFormat: Text.PlainText
                        font.family: Theme.sans
                        font.pixelSize: 12
                        font.weight: Font.Medium
                        color: Theme.ink
                    }
                    Text {
                        width: parent.width
                        wrapMode: Text.Wrap
                        text: it.modelData.text
                        textFormat: Text.PlainText
                        font.family: Theme.sans
                        font.pixelSize: 12
                        lineHeight: 18
                        lineHeightMode: Text.FixedHeight
                        color: Theme.muted
                    }
                }
                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.rule2 }
            }
        }
        Item { id: extraBox; width: parent.width; implicitHeight: childrenRect.height; height: childrenRect.height }
    }
    Column {
        id: foot
        visible: root.lines.length > 0
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        spacing: Theme.space05
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
