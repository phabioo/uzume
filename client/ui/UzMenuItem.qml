import QtQuick
import QtQuick.Layouts

// Menu item (design-system.md "Menu" / "Select"): 32 px row, 13 px text, optional hint (muted) and shortcut (Mono). Hover and
// keyboard focus use `hover`; keyboard focus adds the inset focus ring, `selected` the 2 px Yamabuki start marker, `danger`
// the danger text. `checkColumn` reserves 14 px for the check of a selectable list.
Rectangle {
    id: root
    property string text: ""
    property string hint: ""
    property string shortcut: ""
    property bool danger: false
    property bool selected: false
    property bool active: false          // keyboard focus
    property bool checkColumn: false
    property bool itemEnabled: true
    readonly property bool hovered: hover.hovered && root.itemEnabled
    signal triggered()

    implicitHeight: Theme.row32
    implicitWidth: contents.implicitWidth + 2 * Theme.space15
    radius: 0
    color: !root.itemEnabled ? "transparent"
         : (root.danger && (root.hovered || root.active)) ? Theme.dangerTint
         : (root.hovered || root.active) ? Theme.hover : "transparent"
    Accessible.role: Accessible.MenuItem
    Accessible.name: root.text

    Rectangle {  // marker-start
        visible: root.selected
        width: 2
        height: parent.height
        color: Theme.ink
    }
    RowLayout {
        id: contents
        anchors.fill: parent
        anchors.leftMargin: Theme.space15
        anchors.rightMargin: Theme.space15
        spacing: Theme.space1
        Item {
            visible: root.checkColumn
            Layout.preferredWidth: 14
            Layout.preferredHeight: 14
            UzGlyph { anchors.centerIn: parent; visible: root.selected; name: "check"; size: 14; color: Theme.ink }
        }
        Text {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            elide: Text.ElideRight
            text: root.text
            textFormat: Text.PlainText
            font.family: Theme.sans
            font.pixelSize: Theme.roles.input.size
            font.weight: root.selected ? Font.Medium : Font.Normal
            color: !root.itemEnabled ? Theme.disabledInk : root.danger ? Theme.danger : Theme.ink
        }
        Text {
            visible: root.hint !== ""
            text: root.hint
            textFormat: Text.PlainText
            elide: Text.ElideRight
            font.family: Theme.sans
            font.pixelSize: Theme.roles.small.size
            color: root.itemEnabled ? Theme.muted : Theme.disabledInk
        }
        Text {
            visible: root.shortcut !== ""
            text: root.shortcut
            textFormat: Text.PlainText
            font.family: Theme.mono
            font.pixelSize: Theme.roles.meta.size
            font.letterSpacing: 0.6
            color: root.itemEnabled ? Theme.muted : Theme.disabledInk
        }
    }
    UzFocusRing { inset: true; visible: root.active && root.itemEnabled }
    HoverHandler { id: hover; cursorShape: root.itemEnabled ? Qt.PointingHandCursor : Qt.ArrowCursor }
    // WithinBounds grabs the press exclusively: with the default passive grab the release that closes the menu also
    // tapped the control underneath the item (e.g. the Session visibility segment below the "⋯" menu).
    TapHandler { enabled: root.itemEnabled; gesturePolicy: TapHandler.WithinBounds; onTapped: root.triggered() }
}
