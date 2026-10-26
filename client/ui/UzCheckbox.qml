import QtQuick
import QtQuick.Controls.Basic

// Checkbox (design-system.md "Checkbox"): an 18 px square with a 1 px `rule-control` frame; checked is an `ink` plane with a
// `ground` check (r6: selection is neutral), indeterminate (`tristate`, checkState PartiallyChecked) a dash. Label 14, optional `hint` (12, muted) and
// `error` (square + sentence in danger) below it. Space toggles.
CheckBox {
    id: control
    property string hint: ""
    property string error: ""
    property bool invalid: error !== ""
    readonly property bool mixed: checkState === Qt.PartiallyChecked

    spacing: 10
    padding: 0
    hoverEnabled: true
    Accessible.role: Accessible.CheckBox
    Accessible.name: control.text
    Accessible.checkable: true
    Accessible.checked: control.checked
    Accessible.onPressAction: control.toggle()
    implicitHeight: Math.max(22, box.height) + (sub.visible ? sub.implicitHeight + 2 : 0)

    indicator: Item {
        implicitWidth: 18
        implicitHeight: 18
        x: control.leftPadding
        y: control.topPadding + 2
        Rectangle {
            id: box
            objectName: "checkboxBox"
            anchors.fill: parent
            radius: 0
            readonly property bool filled: control.checked || control.mixed
            color: !control.enabled ? Theme.disabledGround
                 : filled ? (control.down ? Theme.ink2 : Theme.ink)
                 : (control.down ? Theme.pressed : (control.hovered ? Theme.hover : "transparent"))
            border.width: 1
            border.color: !control.enabled ? Theme.disabledRule
                        : filled ? box.color
                        : control.invalid ? Theme.danger
                        : (control.hovered ? Theme.ink : Theme.ruleControl)
            UzGlyph {
                anchors.centerIn: parent
                visible: box.filled
                name: control.mixed ? "dash" : "check"
                size: 12
                strokeWidth: 2
                color: control.enabled ? Theme.ground : Theme.disabledInk
            }
        }
        UzFocusRing { visible: control.visualFocus }
    }
    contentItem: Item {
        implicitWidth: label.implicitWidth + control.indicator.width + control.spacing
        implicitHeight: label.implicitHeight + (sub.visible ? sub.implicitHeight + 2 : 0)
        Text {
            id: label
            x: control.indicator.width + control.spacing
            width: parent.width - x
            text: control.text
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            font.family: Theme.sans
            font.pixelSize: Theme.roles.body.size
            color: control.enabled ? Theme.ink : Theme.disabledInk
        }
        Row {
            id: sub
            visible: control.error !== "" || control.hint !== ""
            x: control.indicator.width + control.spacing
            y: label.implicitHeight + 2
            spacing: 6
            Rectangle { visible: control.error !== ""; anchors.verticalCenter: parent.verticalCenter; width: 10; height: 10; color: Theme.danger }
            Text {
                width: Math.max(0, control.width - sub.x - (control.error !== "" ? 16 : 0))
                wrapMode: Text.Wrap
                text: control.error !== "" ? control.error : control.hint
                textFormat: Text.PlainText
                font.family: Theme.sans
                font.pixelSize: Theme.roles.small.size
                color: control.error !== "" ? Theme.danger : Theme.muted
            }
        }
    }
}
