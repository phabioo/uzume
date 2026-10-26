import QtQuick
import Uzume.App

// One HUD widget (board 5b-2, "HUD widget"): drawn on the stage, so it keeps a stage-appropriate look: `surface` plane, 1 px
// `rule` frame (`ink-2` when lit), padding 8 12, at most 320 wide, square. It takes no input: hover (or focus) only brightens it and shows the "⋯"
// menu button and, for non-official plugins, the "Unverified" / "Local" marker.
Item {
    id: root
    required property var entry                    // PluginsController.hudWidgets entry
    property string marker: ""
    property Item menuAnchor: null                 // the "⋯" button whose menu is open (any widget), if any
    readonly property bool menuOpen: menuAnchor !== null && menuAnchor === menuButton
    // The hover zone includes the "⋯" button and the gap to it, and the button stays while its menu is open.
    readonly property bool lit: hover.containsMouse || zoneHover.hovered || menuButton.hovered || menuOpen
    readonly property int maxWidth: 320
    signal menuRequested(Item anchor)

    readonly property real maxInner: maxWidth - 2 * Theme.space15
    readonly property bool rightSide: entry && entry.corner === "bottom-right"
    readonly property bool topSide: entry && entry.corner === "top-left"
    width: bg.width
    height: bg.height
    objectName: "hudWidget_" + (entry ? entry.pluginId : "")

    Rectangle {
        id: bg
        width: Math.min(root.maxWidth, row.width + 2 * Theme.space15)
        height: row.height + 2 * Theme.space1
        color: Theme.surface
        border.width: 1
        border.color: root.lit ? Theme.ink2 : Theme.rule
        Behavior on border.color { ColorAnimation { duration: Theme.durFast } }

        Row {
            id: row
            x: Theme.space15
            y: Theme.space1
            spacing: Theme.space1
            PluginTabIcon {
                anchors.verticalCenter: parent.verticalCenter
                size: 16
               
                name: root.entry ? (root.entry.name || "") : ""
                icon: root.entry ? (root.entry.icon || "") : ""
            }
            PluginBlock {
                id: block
                objectName: "hudBlock"
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(implicitWidth, root.maxInner - 16 - Theme.space1)
                height: implicitHeight
                node: root.entry ? root.entry.tree : ({})
                dark: Theme.dark
                pluginId: root.entry ? root.entry.pluginId : ""
            }
        }
        // No input to the blocks: this area takes every press; it only tracks the hover.
        MouseArea {
            id: hover
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.AllButtons
            onPressed: (m) => m.accepted = true
        }
        // Passive hover over the frame, the gap and the button (takes no input, so the game keeps its clicks beside the widget).
        Item {
            x: root.rightSide ? -(menuButton.width + 4) : 0
            width: bg.width + menuButton.width + 4
            height: bg.height
            HoverHandler { id: zoneHover }
        }
        Rectangle {
            id: menuButton
            objectName: "hudMenuButton"
            readonly property bool hovered: btnHover.hovered
            visible: root.lit
            // Beside the widget (P45), never over its text: left of it in the right-hand corner, else right of it
            x: root.rightSide ? -width - 4 : parent.width + 4
            anchors.verticalCenter: parent.verticalCenter
            width: 24; height: 24
            color: btnHover.hovered ? Theme.pressed : Theme.surface
            border.width: 1
            border.color: Theme.rule
            Accessible.role: Accessible.Button
            Accessible.name: qsTr("Widget menu")
            UzGlyph { anchors.centerIn: parent; name: "more"; size: 14; color: Theme.ink2 }
            HoverHandler { id: btnHover; cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: root.menuRequested(menuButton) }
        }
    }
    UzTag {
        objectName: "hudMarker"
        small: true
        tone: "accent"
        icon: "none"
        text: root.marker
        visible: root.lit && root.marker !== ""
        // "Unverified" / "Local": above the widget, below it in the top corner so it is never cut off by the play area edge
        x: bg.x
        y: root.topSide ? bg.height + 3 : -height - 3
    }
}
