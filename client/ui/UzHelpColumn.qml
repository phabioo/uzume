import QtQuick

// Help column (design-system.md "HelpColumn"): the open column of long admin pages with a HELP slot: a Mono "HELP" head on a
// `rule` underline, paragraphs [{ title?, text }] separated by `rule-2` lines (12 px text, muted), `lines` at the foot.
// Extra content goes into the default slot below the paragraphs.
// `inline`: below 1440 px (Theme.columnsCompactMax) the help is no column but a "Help" disclosure at the end of the content
// (closed by default; `open` opens it); it fills the width it is given and has no open column.
Item {
    id: root
    property bool inline: false
    property bool open: false
    property string label: ""
    property var items: []
    property var lines: []
    default property alias extra: extraBox.data
    implicitWidth: root.inline ? 0 : Theme.openColumn
    implicitHeight: root.inline
                    ? disclosure.height + (root.open ? help.implicitHeight + (foot.visible ? foot.implicitHeight + Theme.space2 : 0) : 0)
                    : Math.max(420, help.implicitHeight + foot.implicitHeight + Theme.space3)
    width: root.inline ? implicitWidth : Theme.openColumn

    UzOpenColumn { visible: !root.inline; anchors.fill: parent; label: root.label }
    UzDisclosure {
        id: disclosure
        objectName: "helpDisclosure"
        visible: root.inline
        title: qsTr("Help")
        onOpenChanged: root.open = disclosure.open
        Component.onCompleted: disclosure.open = root.open
    }
    onOpenChanged: if (disclosure.open !== root.open) disclosure.open = root.open
    Column {
        id: help
        objectName: "helpColumnBody"
        visible: !root.inline || root.open
        y: root.inline ? disclosure.height : 0
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.rightMargin: root.inline ? 0 : 28
        Text {
            visible: !root.inline
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
        visible: root.lines.length > 0 && (!root.inline || root.open)
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
