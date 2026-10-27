import QtQuick
import QtQuick.Layouts

// list block: rows { id?, title, sub?, meta?, image?, tone?, action?, value? }; a row is a focus stop when it has an action.
// `selected` is a row id. Emits action(row.action, row.value).
ColumnLayout {
    id: root
    property var node: ({})
    property bool dark: true
    property string pluginId: ""
    signal action(string action, var value)

    Layout.minimumWidth: 0
    spacing: 0
    readonly property var rowList: node.rows || []
    PluginPool {
        count: root.rowList.length
        delegate: PluginFocusable {
            id: row
            required property int index
            readonly property var modelData: root.rowList[index] || ({})
            visible: index < root.rowList.length   // pooled (PluginPool)
            objectName: "pluginListRow_" + index
            readonly property bool clickable: !!modelData.action
            readonly property bool isSelected: modelData.id !== undefined && String(modelData.id) === String(root.node.selected)
            dark: root.dark
            actionable: clickable
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            clip: true
            implicitHeight: Math.max(Theme.row44, content.implicitHeight + Theme.space15)
            onActivated: root.action(modelData.action, modelData.value)

            Rectangle {
                anchors.fill: parent
                color: row.isSelected ? Theme.blockColor("selection", root.dark) : (row.hovered ? Theme.blockColor("hover", root.dark) : "transparent")
                Rectangle { visible: row.isSelected; width: 2; height: parent.height; color: Theme.blockColor("accent", root.dark) }
                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.tok("rule2", root.dark) }
            }
            RowLayout {
                id: content
                anchors.fill: parent
                anchors.leftMargin: Theme.space15
                anchors.rightMargin: Theme.space15
                spacing: Theme.space15
                PluginImage {
                    visible: !!row.modelData.image
                    dark: root.dark
                    pluginId: root.pluginId
                    src: row.modelData.image || ""
                    size: "m"
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    spacing: 1
                    Text {
                        textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                        Layout.fillWidth: true
                        text: row.modelData.title || ""
                        elide: Text.ElideRight
                        font.pixelSize: Theme.roles.itemTitleSm.size
                        font.weight: Theme.roles.itemTitleSm.weight
                        color: Theme.blockColor("text", root.dark)
                    }
                    Text {
                        textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                        visible: !!row.modelData.sub
                        Layout.fillWidth: true
                        text: row.modelData.sub || ""
                        elide: Text.ElideRight
                        font.pixelSize: Theme.roles.small.size
                        color: Theme.blockColor("textMeta", root.dark)
                    }
                }
                Text {
                    textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                    visible: !!row.modelData.meta
                    Layout.maximumWidth: Math.max(40, content.width * 0.4)
                    elide: Text.ElideRight
                    text: row.modelData.meta || ""
                    font.family: Theme.mono
                    font.pixelSize: Theme.roles.meta11.size
                    color: row.modelData.tone ? Theme.toneFg(row.modelData.tone, root.dark) : Theme.blockColor("textMuted", root.dark)
                }
                Text {
                    textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                    visible: row.clickable
                    text: "›"
                    font.pixelSize: Theme.roles.headerTitle.size
                    color: Theme.blockColor("textFaint", root.dark)
                }
            }
        }
    }
}
