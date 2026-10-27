import QtQuick
import QtQuick.Layouts

// table block: columns = labels, rows { cells, action?, value? }. Equal column widths; a row is a focus stop with an action.
ColumnLayout {
    id: root
    property var node: ({})
    property bool dark: true
    property string pluginId: ""
    readonly property var columns: node.columns || []
    readonly property var rowList: node.rows || []
    signal action(string action, var value)

    spacing: 0
    RowLayout {
        Layout.fillWidth: true
        Layout.leftMargin: Theme.space15
        Layout.rightMargin: Theme.space15
        Layout.bottomMargin: Theme.space1
        spacing: Theme.space1
        PluginPool {
            count: root.columns.length
            delegate: Text {
                textFormat: Text.PlainText
                required property int index
                readonly property var modelData: root.columns[index]
                visible: index < root.columns.length
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                Layout.minimumWidth: 0
                text: String(modelData).toUpperCase()
                elide: Text.ElideRight
                font.family: Theme.mono
                font.pixelSize: Theme.roles.tableHeader.size
                font.weight: Theme.roles.tableHeader.weight
                font.letterSpacing: Theme.roles.tableHeader.spacing
                color: Theme.blockColor("textFaint", root.dark)
            }
        }
    }
    PluginPool {
        count: root.rowList.length
        delegate: PluginFocusable {
            id: row
            required property int index
            readonly property var modelData: root.rowList[index] || ({})
            visible: index < root.rowList.length   // pooled (PluginPool)
            objectName: "pluginTableRow_" + index
            dark: root.dark
            actionable: !!modelData.action
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            clip: true
            implicitHeight: Theme.row36
            onActivated: root.action(modelData.action, modelData.value)
            Rectangle {
                anchors.fill: parent
                color: row.hovered ? Theme.blockColor("hover", root.dark) : "transparent"
            }
            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 1
                color: Theme.tok("rule2", root.dark)
            }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.space15
                anchors.rightMargin: Theme.space15
                spacing: Theme.space1
                PluginPool {
                    count: (row.modelData.cells || []).length
                    delegate: Text {
                        textFormat: Text.PlainText
                        required property int index
                        readonly property var modelData: (row.modelData.cells || [])[index]
                        visible: index < (row.modelData.cells || []).length
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        Layout.minimumWidth: 0
                        text: String(modelData)
                        elide: Text.ElideRight
                        font.pixelSize: Theme.roles.input.size
                        color: index === 0 ? Theme.blockColor("text", root.dark) : Theme.blockColor("textSecondary", root.dark)
                    }
                }
            }
        }
    }
}
