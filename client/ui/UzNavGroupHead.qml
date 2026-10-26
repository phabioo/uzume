import QtQuick

// AppNav group head (frame.md "0-4 G"): Plex Mono 500 10/16, uppercase, `dim`, led by a 12 px `rule` dash.
Item {
    id: root
    property string text: ""
    implicitHeight: 16
    Accessible.role: Accessible.StaticText
    Accessible.name: root.text

    Rectangle {
        x: Theme.navInset
        anchors.verticalCenter: parent.verticalCenter
        width: Theme.registerMark
        height: 1
        color: Theme.rule
    }
    Text {
        x: Theme.navInset + Theme.registerMark + Theme.space1
        width: parent.width - x - Theme.space2
        anchors.verticalCenter: parent.verticalCenter
        elide: Text.ElideRight
        text: root.text
        textFormat: Text.PlainText
        font.family: Theme.mono
        font.pixelSize: Theme.roles.sectionLabel.size
        font.weight: Font.Medium
        font.letterSpacing: Theme.roles.sectionLabel.spacing
        font.capitalization: Font.AllUppercase
        color: Theme.muted
    }
}
