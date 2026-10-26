import QtQuick

// Filter / category chip (design-system.md "FilterChip"): 28 px, square, `rule-control` frame, `ink` plane with `ground` text
// when selected (r6: selection is neutral, yellow is for acting) (`active`). Optional counter (count >= 0) is part of the label: "All · 10".
Rectangle {
    id: root
    property string text: ""
    property int count: -1
    property bool badgeAccent: false  // needs attention: a small Yamabuki square with "!" before the label (no yellow text)
    property bool active: false
    signal clicked()

    implicitHeight: Theme.row28
    property real maxWidth: 100000   // the text elides rather than the chip getting wider than this (very wide fonts)
    implicitWidth: Math.min(maxWidth, row.implicitWidth + 2 * Theme.space15)
    radius: 0
    color: active ? (press.pressed ? Theme.ink2 : Theme.ink)
                  : (hover.hovered ? Theme.hover : "transparent")
    border.width: 1
    border.color: active ? Theme.ink : (hover.hovered ? Theme.ink2 : Theme.ruleControl)
    Behavior on color { ColorAnimation { duration: Theme.durFast } }

    Row {
        id: row
        anchors.centerIn: parent
        spacing: 6
        Rectangle {
            objectName: "chipAttentionMark"
            visible: root.badgeAccent && root.count > 0
            anchors.verticalCenter: parent.verticalCenter
            width: 10; height: 10
            color: Theme.accent
            Text { anchors.centerIn: parent; text: "!"; font.family: Theme.sans; font.pixelSize: 8; font.weight: Font.Bold; color: Theme.inkOnAccent }
        }
        Text {
            width: Math.min(implicitWidth, Math.max(0, root.maxWidth - 2 * Theme.space15))
            elide: Text.ElideRight
            anchors.verticalCenter: parent.verticalCenter
            // "All · 10": the count is part of the label, once.
            text: root.count >= 0 ? root.text + " · " + root.count : root.text
            textFormat: Text.PlainText
            font.family: Theme.sans
            font.pixelSize: Theme.roles.small.size
            font.weight: root.active ? Font.Medium : Font.Normal
            color: root.active ? Theme.ground : Theme.ink
        }
    }
    // Keyboard: Tab reaches the chip, Enter/Space activates it, the focus ring is visible.
    activeFocusOnTab: true
    Accessible.role: Accessible.Button
    Accessible.name: root.count >= 0 ? root.text + " · " + root.count : root.text
    Accessible.checkable: true
    Accessible.checked: root.active
    Accessible.onPressAction: root.clicked()
    Keys.onPressed: (event) => {
        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) {
            root.clicked()
            event.accepted = true
        }
    }
    UzFocusRing { visible: root.activeFocus }
    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
    TapHandler { id: press; onTapped: root.clicked() }
}
