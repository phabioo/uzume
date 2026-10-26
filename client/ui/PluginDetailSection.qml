import QtQuick
import QtQuick.Layouts
import Uzume.App

// Plugin sections in the game detail column (design 4h-2): one block per plugin that set its "detail" slot, under a header
// with the plugin name (and the origin marker for non-official plugins). Nothing is shown while no plugin has content.
ColumnLayout {
    id: root
    required property PlayerController player
    readonly property var sections: player.plugins.detailSections
    property bool placed: true       // false while the host shows another tab
    visible: placed && sections.length > 0
    spacing: Theme.space3

    // Collapsed sections (plugin ids), kept while the pane lives (3c-7).
    property var collapsed: ({})
    function toggle(id: string) {
        const c = Object.assign({}, root.collapsed)
        c[id] = !c[id]
        root.collapsed = c
    }

    Repeater {
        model: root.sections
        delegate: Rectangle {
            id: sec
            required property var modelData
            readonly property string marker: root.player.plugins.originMarker(modelData.origin || "")
            readonly property bool isCollapsed: root.collapsed[modelData.pluginId] === true
            objectName: "pluginDetail_" + modelData.pluginId
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            implicitHeight: secCol.implicitHeight + 2 * Theme.space15
            color: "transparent"
            border.width: 1
            border.color: Theme.rule
            ColumnLayout {
                id: secCol
                anchors.fill: parent
                anchors.margins: Theme.space15
                spacing: Theme.space1
                RowLayout {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    spacing: Theme.space1
                    Eyebrow {
                        objectName: "pluginDetailName"
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        elide: Text.ElideRight
                        text: qsTr("Drawn by %1").arg(sec.modelData.name)
                    }
                    PluginOriginPill {
                        objectName: "pluginDetailOrigin"
                        visible: sec.marker !== ""
                        origin: sec.modelData.origin || "local"
                        text: sec.marker
                    }
                    UzButton {
                        textFormat: Text.PlainText
                        objectName: "pluginDetailCollapse"
                        kind: "link"
                        font.pixelSize: Theme.roles.meta11.size
                        text: sec.isCollapsed ? qsTr("Expand ▾") : qsTr("Collapse ▴")
                        onClicked: root.toggle(sec.modelData.pluginId)
                    }
                }
                PluginBlock {
                    visible: !sec.isCollapsed
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    Layout.preferredHeight: sec.isCollapsed ? 0 : implicitHeight
                    node: sec.modelData.tree
                    dark: Theme.dark
                    pluginId: sec.modelData.pluginId
                    onAction: (action, value) => root.player.plugins.sendAction(sec.modelData.pluginId, "detail", action, value)
                }
            }
        }
    }
}
