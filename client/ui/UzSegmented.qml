import QtQuick
import QtQuick.Layouts
import Uzume.App

// Segmented choice (design-system.md "Segmented"): square, one `rule-control` frame, items 28 px (sm 26) with 12 px text; the active
// option is an `ink` plane with `ground` text (r6). Also the This PC / Server and Layout switches.
// options: [{ value, label, name, disabled, underline }] (name = objectName of the segment).
Rectangle {
    id: root
    property var options: []
    property string current: ""
    property bool stretch: false
    property string accessibleName: ""   // spoken name of the group (the label next to it)
    property int segmentHeight: Theme.row28
    signal picked(string value)

    // Never demands more than the room it is given (long labels elide) so a layout around it cannot grow past its parent.
    Layout.minimumWidth: 0
    implicitHeight: segmentHeight + 2
    implicitWidth: row.implicitWidth + 2
    radius: 0
    color: "transparent"
    border.width: 1
    border.color: Theme.ruleControl

    // Keyboard (R-P4): one tab stop for the whole control; Left/Right (Up/Down, Home/End) move the selection like a radio group.
    activeFocusOnTab: true
    Accessible.role: Accessible.RadioButton
    Accessible.name: {
        if (accessibleName !== "") return accessibleName + ": " + selectedLabel
        return selectedLabel
    }
    readonly property string selectedLabel: {
        for (const o of options) if (o.value === current) return o.label
        return ""
    }
    function step(delta) {
        const enabled = options.filter(o => o.disabled !== true)
        if (enabled.length === 0) return
        let i = enabled.findIndex(o => o.value === current)
        i = i < 0 ? (delta > 0 ? 0 : enabled.length - 1) : Math.max(0, Math.min(enabled.length - 1, i + delta))
        if (enabled[i].value !== current) picked(enabled[i].value)
    }
    Keys.onPressed: (event) => {
        switch (event.key) {
        case Qt.Key_Left: case Qt.Key_Up: step(-1); event.accepted = true; break
        case Qt.Key_Right: case Qt.Key_Down: step(1); event.accepted = true; break
        case Qt.Key_Home: step(-1000); event.accepted = true; break
        case Qt.Key_End: step(1000); event.accepted = true; break
        }
    }
    UzFocusRing { visible: root.activeFocus }

    RowLayout {
        id: row
        anchors.fill: parent
        anchors.margins: 1
        spacing: 0
        Repeater {
            // The option count, not the array: `options` is usually a binding that yields a new array on every change, and a
            // rebuilt Repeater inside a Qt 6.4 layout can leave the layout with a deleted item (crash in rearrange).
            model: root.options.length
            delegate: Rectangle {
                id: seg
                required property int index
                readonly property var modelData: root.options[index] || ({})
                objectName: modelData.name || ""
                readonly property bool active: root.current === modelData.value
                // fillWidth + maximumWidth = preferred: never grows, but may shrink (a non-fill item is never shrunk by the layout)
                Layout.fillWidth: true
                Layout.maximumWidth: root.stretch ? 100000 : implicitWidth
                Layout.fillHeight: true
                Layout.minimumWidth: 0
                Layout.preferredWidth: root.stretch ? 1 : implicitWidth
                clip: true
                implicitWidth: segLabel.implicitWidth + 28
                radius: 0
                color: active ? Theme.ink : (segHover.hovered && seg.modelData.disabled !== true ? Theme.hover : "transparent")
                Behavior on color { ColorAnimation { duration: Theme.durFast } }
                Text {
                    id: segLabel
                    anchors.centerIn: parent
                    width: Math.min(implicitWidth, parent.width - 12)
                    elide: Text.ElideRight
                    horizontalAlignment: Text.AlignHCenter
                    text: seg.modelData.label
                    textFormat: Text.PlainText
                    font.family: Theme.sans
                    font.pixelSize: Theme.roles.small.size
                    font.weight: seg.active ? Font.Medium : Font.Normal
                    color: seg.modelData.disabled === true ? Theme.disabledInk : (seg.active ? Theme.ground : (segHover.hovered ? Theme.ink : Theme.muted))
                }
                Rectangle {
                    visible: seg.modelData.underline === true
                    anchors.bottom: parent.bottom
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: parent.width - 16
                    height: 2
                    color: seg.active ? Theme.ground : Theme.ink
                }
                HoverHandler { id: segHover; cursorShape: seg.modelData.disabled === true ? Qt.ArrowCursor : Qt.PointingHandCursor }
                TapHandler { enabled: seg.modelData.disabled !== true; onTapped: root.picked(seg.modelData.value) }
            }
        }
    }
}
