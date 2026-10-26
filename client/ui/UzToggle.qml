import QtQuick
import QtQuick.Controls.Basic

// Toggle (design-system.md "Toggle"): 40 x 22 square switch with a 14 px square knob at top 3, left 3 (off) / 21 (on). On =
// Yamabuki plane with an `on-accent` knob, off = `rule-control` frame with a filled `ink-2` knob (r6). `showState` adds the Mono ON / OFF word.
Switch {
    id: control
    property bool showState: false
    implicitHeight: 28
    padding: 0
    spacing: Theme.space15
    hoverEnabled: true
    Accessible.role: Accessible.CheckBox
    Accessible.name: control.text
    Accessible.checkable: true
    Accessible.checked: control.checked
    Accessible.onPressAction: control.toggle()

    indicator: Item {
        implicitWidth: 40
        implicitHeight: 22
        x: control.leftPadding
        y: (control.height - height) / 2
        Rectangle {
            id: track
            anchors.fill: parent
            radius: 0
            border.width: 1
            color: !control.enabled ? Theme.disabledGround
                 : control.checked ? (control.down ? Theme.accentPressed : (control.hovered ? Theme.accentHover : Theme.accent))
                 : (control.down ? Theme.pressed : (control.hovered ? Theme.hover : "transparent"))
            border.color: !control.enabled ? Theme.disabledRule
                        : control.checked ? track.color
                        : (control.hovered ? Theme.ink : Theme.ruleControl)
            Behavior on color { ColorAnimation { duration: Theme.durFast } }
            Rectangle {
                x: control.checked ? 21 : 3
                y: 3
                width: 14
                height: 14
                radius: 0
                color: !control.enabled ? Theme.disabledInk : (control.checked ? Theme.inkOnAccent : Theme.ink2)
                Behavior on x { NumberAnimation { duration: Theme.durFast; easing.type: Easing.OutCubic } }
            }
        }
        UzFocusRing { visible: control.visualFocus }
    }
    contentItem: Item {
        implicitWidth: control.indicator.width + control.spacing + label.implicitWidth + (stateText.visible ? stateText.implicitWidth + control.spacing : 0)
        implicitHeight: label.implicitHeight
        Text {
            id: label
            anchors.left: parent.left
            anchors.leftMargin: control.indicator.width + control.spacing
            anchors.right: stateText.visible ? stateText.left : parent.right
            anchors.verticalCenter: parent.verticalCenter
            text: control.text
            textFormat: Text.PlainText
            font.family: Theme.sans
            font.pixelSize: Theme.roles.input.size
            color: control.enabled ? Theme.ink : Theme.disabledInk
            verticalAlignment: Text.AlignVCenter
            wrapMode: Text.WordWrap
        }
        Text {
            id: stateText
            visible: control.showState
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            text: control.checked ? qsTr("ON") : qsTr("OFF")
            font.family: Theme.mono
            font.pixelSize: Theme.roles.meta.size
            font.weight: Font.Medium
            font.letterSpacing: 0.9
            color: control.enabled ? Theme.muted : Theme.disabledInk
        }
    }
}
