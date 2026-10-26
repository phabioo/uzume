import QtQuick
import QtQuick.Controls.Basic

// SettingsDisclosure (design-system.md "SettingsRow, SettingsDisclosure"): "More" / "Less" toggle for advanced rows, with an
// optional Mono "N ADVANCED" count. 32 px, ink 13/500, hover fill. `title` replaces More / Less with a fixed name ("Help").
Button {
    id: control
    property bool open: false
    property string title: ""
    property int count: -1
    implicitHeight: Theme.row32
    padding: 0
    leftPadding: Theme.space1
    rightPadding: Theme.space1
    hoverEnabled: true
    checkable: false
    Accessible.role: Accessible.Button
    Accessible.name: control.title !== "" ? control.title : (control.open ? qsTr("Less") : qsTr("More"))
    contentItem: Row {
        spacing: Theme.space1
        UzGlyph { anchors.verticalCenter: parent.verticalCenter; name: control.open ? "chevronUp" : "chevronDown"; size: 14; color: Theme.ink }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: control.title !== "" ? control.title : (control.open ? qsTr("Less") : qsTr("More"))
            font.family: Theme.sans
            font.pixelSize: Theme.roles.input.size
            font.weight: Font.Medium
            color: Theme.ink
        }
        Text {
            visible: control.count >= 0
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("%1 ADVANCED").arg(control.count)
            font.family: Theme.mono
            font.pixelSize: Theme.roles.meta.size
            font.letterSpacing: 0.6
            color: Theme.muted
        }
    }
    background: Item {
        Rectangle { anchors.fill: parent; color: control.down ? Theme.pressed : (control.hovered ? Theme.hover : "transparent") }
        UzFocusRing { visible: control.visualFocus }
    }
    onClicked: open = !open
}
