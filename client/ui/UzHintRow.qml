import QtQuick

// Hint row (design-system.md "Focus is always visible"): the bottom row of a screen, 44 px with a `rule-2` line above, showing
// the keys of the last input device: GamepadHint glyphs for `device: "gamepad"`, Kbd keys for "keyboard". The app does not
// track the last input device yet, so the owner sets `device`.
// hints: [{ pad: "A", kbd: "Enter", label: qsTr("Choose") }, ...]  (a hint without kbd / pad is left out for that device)
Item {
    id: root
    property string device: "keyboard"     // keyboard | gamepad
    property var hints: []
    // One line of 44 px; hints that do not fit (a gamepad combo is long) wrap to further lines and the row grows with them.
    implicitHeight: Math.max(44, row.childrenRect.height + 2 * Theme.space1)
    // Width of all hints in one line (a Flow does not report it), so a parent that sizes the row by it gets the one-line layout.
    readonly property real naturalWidth: {
        let w = 0, n = 0
        for (let i = 0; i < row.children.length; ++i) {
            const c = row.children[i]
            if (c.sourceComponent !== undefined) { w += c.implicitWidth; ++n }
        }
        return w + Math.max(0, n - 1) * Theme.space2
    }
    implicitWidth: root.naturalWidth + 2 * Theme.space3

    Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: Theme.rule2 }
    Flow {
        id: row
        objectName: "hintRowKeys"
        anchors.verticalCenter: parent.verticalCenter
        anchors.left: parent.left
        anchors.leftMargin: Theme.space3
        width: Math.max(0, root.width - 2 * Theme.space3)
        spacing: Theme.space2
        Repeater {
            model: root.hints.filter(h => root.device === "gamepad" ? h.pad !== undefined : h.kbd !== undefined)
            delegate: Loader {
                required property var modelData
                sourceComponent: root.device === "gamepad" ? padComp : kbdComp
                Component { id: padComp; UzGamepadHint { k: modelData.pad; label: modelData.label || "" } }
                Component { id: kbdComp; UzKbd { keys: modelData.kbd; label: modelData.label || ""; maxWidth: row.width } }
            }
        }
    }
}
