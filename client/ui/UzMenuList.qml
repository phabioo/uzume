import QtQuick
import QtQuick.Layouts

// Menu list (design-system.md "Menu" / "MenuList"): `surface` plane in a 1 px `rule` frame, no shadow, min width 200
// (Theme.menu). items: [{ label, hint?, shortcut?, danger?, disabled?, value?, name? }], a heading is { heading: "TEXT" } and a
// separator { separator: true }. With `selectable` the item whose value equals `value` carries the check and the start marker.
// Up / Down move the keyboard focus (`activeIndex`), Enter / Space choose, `footer` takes extra content (hint keys).
// Used inside a Popup (UzMenu, MoreMenu) or in place.
FocusScope {
    id: root
    property var items: []
    property bool selectable: false
    property var value: undefined
    property int activeIndex: -1
    property alias footer: footerBox.data
    signal chosen(var item, int index)

    implicitWidth: Math.max(Theme.menu, col.implicitWidth + 2)
    implicitHeight: col.implicitHeight + 2 + 2 * Theme.space05 + (footerBox.children.length > 0 ? footerBox.implicitHeight : 0)

    function isItem(i) {
        const it = root.items[i]
        return it !== undefined && !it.separator && it.heading === undefined && it.disabled !== true
    }
    function move(dir) {
        const n = root.items.length
        if (n === 0) return
        let i = root.activeIndex
        for (let k = 0; k < n; ++k) {
            i = ((i + dir) % n + n) % n
            if (isItem(i)) { root.activeIndex = i; return }
        }
    }
    function choose(i) {
        if (isItem(i)) root.chosen(root.items[i], i)
    }
    Keys.onPressed: (event) => {
        if (event.key === Qt.Key_Down) { move(1); event.accepted = true }
        else if (event.key === Qt.Key_Up) { move(-1); event.accepted = true }
        else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) && root.activeIndex >= 0) {
            choose(root.activeIndex); event.accepted = true
        }
    }

    Rectangle {
        anchors.fill: parent
        radius: 0
        color: Theme.surface
        border.width: 1
        border.color: Theme.rule
    }
    ColumnLayout {
        id: col
        x: 1
        y: 1 + Theme.space05
        width: parent.width - 2
        spacing: 0
        Repeater {
            model: root.items
            delegate: Loader {
                id: entry
                required property var modelData
                required property int index
                Layout.fillWidth: true
                sourceComponent: modelData.separator ? sepComp : modelData.heading !== undefined ? headComp : itemComp
                Component {
                    id: sepComp
                    Item {
                        implicitHeight: 1 + 2 * Theme.space05
                        Rectangle { x: 0; y: Theme.space05; width: parent.width; height: 1; color: Theme.rule2 }
                    }
                }
                Component {
                    id: headComp
                    Text {
                        topPadding: 6
                        bottomPadding: 2
                        leftPadding: Theme.space15
                        text: entry.modelData.heading
                        textFormat: Text.PlainText
                        font.family: Theme.mono
                        font.pixelSize: Theme.roles.meta.size
                        font.weight: Font.Medium
                        font.letterSpacing: 1.4
                        font.capitalization: Font.AllUppercase
                        lineHeight: 15
                        lineHeightMode: Text.FixedHeight
                        color: Theme.muted
                    }
                }
                Component {
                    id: itemComp
                    UzMenuItem {
                        objectName: entry.modelData.name !== undefined ? "menuItem_" + entry.modelData.name : ""
                        text: entry.modelData.label
                        hint: entry.modelData.hint || ""
                        shortcut: entry.modelData.shortcut || ""
                        danger: entry.modelData.danger === true
                        itemEnabled: entry.modelData.disabled !== true
                        checkColumn: root.selectable
                        selected: root.selectable && entry.modelData.value === root.value
                        active: entry.index === root.activeIndex
                        onTriggered: root.chosen(entry.modelData, entry.index)
                    }
                }
            }
        }
    }
    Item {
        id: footerBox
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 1
        implicitHeight: childrenRect.height
        height: childrenRect.height
    }
}
