import QtQuick

// Key cap (design-system.md "GamepadHint" / "Kbd"): a square 1 px `ink-2` frame, min 20 x 20, Mono 500 10 letter. `pressed`
// fills it with ink; `disabled` uses the disabled tokens. `glyph` (a UzGlyph arrow name, or two joined with "/") replaces the
// letter for the D-pad directions.
Rectangle {
    id: root
    property string text: ""
    property string glyph: ""
    property bool pressed: false
    property bool disabled: false
    readonly property var glyphs: glyph === "" ? [] : glyph.split("/")

    implicitHeight: 20
    implicitWidth: Math.max(20, (glyphs.length > 0 ? glyphs.length * 12 + (glyphs.length - 1) * 2 + 6 : label.implicitWidth + (text.length > 1 ? 12 : 8)))
    radius: 0
    color: root.pressed && !root.disabled ? Theme.ink : "transparent"
    border.width: 1
    border.color: root.disabled ? Theme.disabledRule : (root.pressed ? Theme.ink : Theme.ink2)

    Text {
        id: label
        anchors.centerIn: parent
        visible: root.glyphs.length === 0
        text: root.text
        textFormat: Text.PlainText
        font.family: Theme.mono
        font.pixelSize: Theme.roles.meta.size
        font.weight: Font.Medium
        font.letterSpacing: root.text.length > 1 ? 0.6 : 0
        font.capitalization: Font.AllUppercase
        color: root.disabled ? Theme.disabledInk : (root.pressed ? Theme.ground : Theme.ink)
    }
    Row {
        anchors.centerIn: parent
        spacing: 2
        visible: root.glyphs.length > 0
        Repeater {
            model: root.glyphs
            delegate: UzGlyph {
                required property string modelData
                name: modelData
                size: 12
                color: root.disabled ? Theme.disabledInk : (root.pressed ? Theme.ground : Theme.ink)
            }
        }
    }
}
