import QtQuick

// Table row (design-system.md "TableRow"): a 44 px row on a `rule-2` hairline; hover fills `hover`; the selected row has the
// `selected` tint, weight 500 and the 2 px Yamabuki start marker (never the tint alone); a disabled row uses the disabled ink.
// Used by UzTable, which hands over the column spec, the computed pixel `widths` and the row object.
//   columns: [{ key, title, align: "left" | "right" | "center", mono: bool, custom: bool }]
//   row:     { <key>: value, ..., disabled?: bool }   (cells show String(row[key]))
// A column with `custom: true` shows `cellDelegate` instead of text: an Item with `property var row` and `property var column`
// (both are set after it is created, so bindings must cope with null). `cellName(key)` gives the objectName of a cell.
Item {
    id: root
    property var columns: []
    property var widths: []
    property var row: ({})
    property int rowIndex: 0
    property bool selected: false
    property bool disabled: false
    property bool selectable: true
    property bool showFocus: false
    property string namePrefix: "table"
    property Component cellDelegate: null
    signal clicked()

    implicitHeight: Theme.row44
    height: implicitHeight
    objectName: root.namePrefix + "Row_" + root.rowIndex
    Accessible.role: Accessible.ListItem
    Accessible.selected: root.selected
    Accessible.name: root.row && root.columns.length > 0 ? String(root.row[root.columns[0].key] ?? "") : ""

    function cellName(key: string): string { return root.namePrefix + "Cell_" + root.rowIndex + "_" + key }

    Rectangle {
        anchors.fill: parent
        color: root.selected ? Theme.selected : (hover.hovered && root.selectable && !root.disabled ? Theme.hover : "transparent")
    }
    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.rule2 }
    Rectangle {   // marker-start
        objectName: "tableRowMarker"
        visible: root.selected
        width: 2
        height: parent.height
        color: Theme.ink
    }
    Row {
        x: Theme.space15
        height: parent.height
        Repeater {
            model: root.columns
            delegate: Item {
                id: cell
                required property var modelData
                required property int index
                readonly property real cellWidth: root.widths.length > cell.index ? root.widths[cell.index] : 0
                width: cell.cellWidth
                height: root.height
                Text {
                    visible: !cell.modelData.custom
                    objectName: root.cellName(cell.modelData.key)
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.max(0, cell.width - Theme.space2)
                    elide: Text.ElideRight
                    horizontalAlignment: cell.modelData.align === "right" ? Text.AlignRight
                                       : cell.modelData.align === "center" ? Text.AlignHCenter : Text.AlignLeft
                    text: root.row ? String(root.row[cell.modelData.key] ?? "") : ""
                    textFormat: Text.PlainText
                    font.family: cell.modelData.mono ? Theme.mono : Theme.sans
                    font.pixelSize: cell.modelData.mono ? Theme.roles.meta11.size : Theme.roles.body.size
                    font.weight: root.selected && cell.index === 0 ? Font.Medium : Font.Normal
                    color: root.disabled ? Theme.disabledInk : (cell.index === 0 || !cell.modelData.mono ? Theme.ink : Theme.muted)
                }
                Loader {
                    visible: cell.modelData.custom === true
                    active: cell.modelData.custom === true && root.cellDelegate !== null
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.max(0, cell.width - Theme.space2)
                    sourceComponent: root.cellDelegate
                    onLoaded: {
                        item.row = Qt.binding(function () { return root.row })
                        item.column = cell.modelData
                    }
                }
            }
        }
    }
    UzFocusRing { visible: root.showFocus; inset: true }
    HoverHandler { id: hover; cursorShape: root.selectable && !root.disabled ? Qt.PointingHandCursor : Qt.ArrowCursor }
    TapHandler { enabled: root.selectable && !root.disabled; onTapped: root.clicked() }
}
