import QtQuick

// Value cell of a binding: key caps for a set value (keyboard keys and gamepad combos like the Kbd of the boards), plain text for
// "Press a key…", "not set" and names. `text` stays the readable value.
Item {
    id: kt
    property string text: ""
    property bool caps: false
    property alias font: plain.font
    property alias color: plain.color
    property alias elide: plain.elide
    property alias verticalAlignment: plain.verticalAlignment
    implicitHeight: 24
    implicitWidth: kt.caps ? kbd.implicitWidth : plain.implicitWidth
    clip: true
    Text {
        id: plain
        visible: !kt.caps
        anchors.fill: parent
        verticalAlignment: Text.AlignVCenter
        text: kt.text
        textFormat: Text.PlainText
        font.family: Theme.sans
    }
    UzKbd {
        id: kbd
        visible: kt.caps
        anchors.verticalCenter: parent.verticalCenter
        keys: kt.text
    }
}
