import QtQuick
import QtQuick.Layouts
import Uzume.App

// Head of a page (PageHeader without actions): section label (number, name, meta), title (Plex Sans Light 40/40) and the lead
// line (14/22 ink-2), 16 px apart. With `avatar` set the person's avatar (40) sits before the title.
ColumnLayout {
    id: root
    property int number: 1
    property string label: ""
    property string meta: ""
    property string title: ""
    property string text: ""
    property string avatar: ""      // name of a person: shows the avatar
    property int person: 1
    spacing: Theme.space2
    Layout.fillWidth: true

    UzSectionLabel { objectName: "startSectionLabel"; Layout.fillWidth: true; keepNumber: true; number: root.number; text: root.label; meta: root.meta }
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space2
        UzAvatar { visible: root.avatar !== ""; Layout.alignment: Qt.AlignVCenter; name: root.avatar; person: root.person; size: 40 }
        Text {
            objectName: "startTitle"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            wrapMode: Text.Wrap
            text: root.title
            textFormat: Text.PlainText
            font.family: Theme.sans
            font.pixelSize: Theme.roles.title.size
            font.weight: Font.Light
            font.letterSpacing: Theme.roles.title.spacing
            lineHeight: Theme.roles.title.line
            lineHeightMode: Text.FixedHeight
            color: Theme.ink
        }
    }
    Text {
        objectName: "startLead"
        visible: root.text !== ""
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        wrapMode: Text.Wrap
        text: root.text
        textFormat: Text.PlainText
        font.family: Theme.sans
        font.pixelSize: Theme.roles.body.size
        lineHeight: Theme.roles.body.line
        lineHeightMode: Text.FixedHeight
        color: Theme.ink2
    }
}
