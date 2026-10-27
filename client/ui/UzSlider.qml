import QtQuick
import QtQuick.Controls.Basic

// Slider (design-system.md "Slider"): a hairline `rule` track, a 3 px Yamabuki fill, a square ink thumb (10 x 18, 12 wide on
// hover, Yamabuki while pressed) and the Mono value (52 px) at the right. Keys: Left / Right / Up / Down one step (Shift or
// PageUp / PageDown a tenth of the range), Home / End the ends; gamepad left / right arrive as Left / Right. `muted` shows the
// off label and an empty fill; `lead` takes an item (a person square) before the track for mixer rows.
Slider {
    id: control
    property string label: ""
    property string unit: " %"
    property string valueText: ""        // overrides the formatted value
    property string offLabel: "OFF"
    property bool muted: false
    property alias lead: leadBox.data
    property bool upDownSteps: true      // false: Up / Down are left to the parent (a popover moves to the next slider)
    readonly property string shown: muted ? offLabel : (valueText !== "" ? valueText : Math.round(value * 1000) / 1000 + unit)
    readonly property real big: Math.max(stepSize, (to - from) / 10)

    from: 0
    to: 100
    stepSize: 1
    snapMode: Slider.SnapAlways
    implicitWidth: 320
    implicitHeight: 24
    hoverEnabled: true
    Accessible.name: control.label
    Accessible.description: control.shown

    function nudge(delta) { control.value = Math.max(from, Math.min(to, control.value + delta)); control.moved() }
    Keys.onPressed: (event) => {
        if (!control.enabled) return
        switch (event.key) {
        case Qt.Key_Up: if (!control.upDownSteps) return; nudge(event.modifiers & Qt.ShiftModifier ? big : stepSize); break
        case Qt.Key_Right: nudge(event.modifiers & Qt.ShiftModifier ? big : stepSize); break
        case Qt.Key_Down: if (!control.upDownSteps) return; nudge(-(event.modifiers & Qt.ShiftModifier ? big : stepSize)); break
        case Qt.Key_Left: nudge(-(event.modifiers & Qt.ShiftModifier ? big : stepSize)); break
        case Qt.Key_PageUp: nudge(big); break
        case Qt.Key_PageDown: nudge(-big); break
        case Qt.Key_Home: control.value = from; control.moved(); break
        case Qt.Key_End: control.value = to; control.moved(); break
        default: return
        }
        event.accepted = true
    }

    // Layout: [label 96] [lead] [track] [value 52]; the Slider padding makes the thumb centre travel the full hairline.
    readonly property real trackStart: (label !== "" ? 96 + Theme.space15 : 0) + (leadBox.width > 0 ? leadBox.width + Theme.space15 : 0)
    readonly property real thumbWidth: 10
    leftPadding: trackStart - thumbWidth / 2
    rightPadding: 52 + Theme.space15 - thumbWidth / 2

    background: Item {
        implicitWidth: 320
        implicitHeight: 24
        Text {
            visible: control.label !== ""
            anchors.verticalCenter: parent.verticalCenter
            width: 96
            elide: Text.ElideRight
            text: control.label
            textFormat: Text.PlainText
            font.family: Theme.sans
            font.pixelSize: Theme.roles.body.size
            color: control.enabled ? Theme.ink : Theme.disabledInk
        }
        Item {
            id: leadBox
            x: control.label !== "" ? 96 + Theme.space15 : 0
            anchors.verticalCenter: parent.verticalCenter
            width: childrenRect.width
            height: childrenRect.height
        }
        Item {
            id: track
            objectName: "sliderTrack"
            x: control.trackStart
            width: parent.width - control.trackStart - 52 - Theme.space15
            height: parent.height
            Rectangle {   // hairline
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width
                height: 1
                color: !control.enabled ? Theme.disabledRule : (control.hovered ? Theme.ink2 : Theme.ruleControl)
            }
            Rectangle {   // fill
                objectName: "sliderFill"
                anchors.verticalCenter: parent.verticalCenter
                width: control.muted ? 0 : control.visualPosition * parent.width
                height: 3
                color: control.enabled ? Theme.accent : Theme.disabledInk
            }
        }
        Text {
            objectName: "sliderValue"
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            width: 52
            horizontalAlignment: Text.AlignRight
            text: control.shown
            textFormat: Text.PlainText
            font.family: Theme.mono
            font.pixelSize: 11
            font.letterSpacing: control.muted ? 0.66 : 0
            color: !control.enabled ? Theme.disabledInk : (control.muted ? Theme.muted : Theme.ink)
        }
    }
    handle: Item {
        id: thumb
        objectName: "sliderThumb"
        x: control.leftPadding + control.visualPosition * (control.availableWidth - width)
        y: (control.height - height) / 2
        width: control.thumbWidth
        height: 18
        Rectangle {
            readonly property bool wide: control.pressed || control.hovered
            objectName: "sliderThumbFace"
            anchors.centerIn: parent
            width: wide ? 12 : 10
            height: 18
            radius: 0
            color: !control.enabled ? Theme.disabledInk : control.pressed ? Theme.accent : (control.muted ? Theme.muted : Theme.ink)
            border.width: control.pressed ? 1 : 0
            border.color: Theme.inkOnAccent
        }
        UzFocusRing { visible: control.visualFocus }
    }
}
