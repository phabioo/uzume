import QtQuick
import QtQuick.Controls.Basic

// Icon button (design-system.md "IconButton"): 32 px square ghost control for header buttons (volume, bell, close) that may
// anchor a popover. `glyph` is a UzGlyph name; `on` = its popover is open (pressed plane). `count` draws the neutral `badge` fill with `ground` text
// (9+ above nine). `outlined` draws a `rule-control` frame (stronger on hover); the button is at least `targetMin` (32 px).
Button {
    id: control
    property string glyph: "more"
    property bool on: false
    property int count: 0
    property int glyphSize: 16
    property bool outlined: false
    implicitWidth: Theme.targetMin
    implicitHeight: Theme.targetMin
    padding: 0
    hoverEnabled: true
    Accessible.name: control.text

    contentItem: Item {
        UzGlyph {
            anchors.centerIn: parent
            name: control.glyph
            size: control.glyphSize
            color: control.enabled ? Theme.ink2 : Theme.disabledInk
        }
        Rectangle {
            visible: control.count > 0
            objectName: "iconButtonCount"
            x: parent.width - width
            y: 2
            width: Math.max(16, countText.implicitWidth + 6)
            height: 16
            radius: 0
            color: Theme.badge
            Text {
                id: countText
                anchors.centerIn: parent
                text: control.count > 9 ? "9+" : String(control.count)
                font.family: Theme.mono
                font.pixelSize: 11
                font.weight: Font.DemiBold
                color: Theme.ground
            }
        }
    }
    background: Item {
        Rectangle {
            anchors.fill: parent
            radius: 0
            color: control.down || control.on ? Theme.pressed : (control.hovered && control.enabled ? Theme.hover : "transparent")
            border.width: control.outlined ? 1 : 0
            border.color: !control.enabled ? Theme.disabledRule : (control.activeFocus ? Theme.ink : (control.hovered || control.down ? Theme.ink2 : Theme.ruleControl))
        }
        UzFocusRing { visible: control.visualFocus }
    }
}
