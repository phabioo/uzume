import QtQuick
import QtQuick.Controls.Basic

// Text input (design-system.md "TextField"): 36 px, `rule-control` frame, transparent on Sumi and `surface` on Kinari (set `fill` to
// `Theme.ground` inside dialogs). Focus: Yamabuki frame (ink on Kinari) plus the focus ring. `invalid` draws the danger frame.
// `mono` for hosts, ports, paths and codes. The error line below the field is the caller's (UzStatusIcon / UzFieldHint).
TextField {
    id: control
    property bool invalid: false
    property bool mono: false
    property string accessibleName: ""   // spoken name; falls back to the placeholder
    property color fill: Theme.dark ? "transparent" : Theme.surface
    implicitHeight: Theme.row36
    leftPadding: Theme.space15
    rightPadding: Theme.space15
    font.family: mono ? Theme.mono : Theme.sans
    font.pixelSize: Theme.roles.input.size
    color: enabled ? Theme.ink : Theme.disabledInk
    placeholderTextColor: Theme.muted
    selectionColor: Theme.accent
    selectedTextColor: Theme.inkOnAccent
    verticalAlignment: TextInput.AlignVCenter
    Accessible.role: Accessible.EditableText
    Accessible.name: control.accessibleName !== "" ? control.accessibleName : control.placeholderText
    background: Item {
        Rectangle {
            anchors.fill: parent
            radius: 0
            color: control.enabled ? control.fill : Theme.disabledGround
            border.width: 1
            border.color: !control.enabled ? Theme.disabledRule
                        : control.invalid ? Theme.danger
                        : control.activeFocus ? (Theme.dark ? Theme.accent : Theme.ink)
                        : (control.hovered ? Theme.ink2 : Theme.ruleControl)
            Behavior on border.color { ColorAnimation { duration: Theme.durFast } }
        }
        UzFocusRing { visible: control.activeFocus && control.enabled }
    }
}
