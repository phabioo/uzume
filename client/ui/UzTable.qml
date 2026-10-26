import QtQuick

// Table (accounts-and-server.md "Rules", design-system.md "TableRow" and "TableHead"): hairline rows under a Mono small-caps head
// on a `rule` line. The shared table of every admin page (Sessions, Connected apps, Users, Devices, Reports, ...).
//
//   columns: [{ key, title, width, fill, align, mono, custom }]
//       width  fixed pixels; omit or 0 = fills the rest (several fill columns share it by `fill`, default 1)
//       align  "left" (default) | "right" | "center"; mono: Plex Mono 11; custom: shows `cellDelegate` (see UzTableRow)
//   rows:    array of objects keyed by the column keys; { disabled: true } greys a row out and keeps it unselectable
//   currentIndex  selected row (-1 none); `selectable: false` makes a display-only table (no marker, no keys, no hover)
//   picked(index, row)     a click or the arrow keys selected a row
//   activated(index, row)  Enter on the selected row
//   default slot           the details section UNDER the table (labelled section of the selected row), never a side panel
//   `empty` slot / emptyText  shown instead of the rows while `rows` is empty
//
// Keys: Tab focuses the table, Up / Down move the selection over the enabled rows, Home / End jump, Enter activates.
// objectNames (prefix = objectPrefix, default "table"): <prefix>, <prefix>Header, <prefix>Row_<n>, <prefix>Cell_<n>_<key>,
// <prefix>Empty.
FocusScope {
    id: root
    property var columns: []
    property var rows: []
    property int currentIndex: -1
    property bool selectable: true
    property string emptyText: ""
    property string objectPrefix: "table"
    property Component cellDelegate: null
    default property alias details: detailsBox.data
    property alias empty: emptyBox.data
    readonly property int count: root.rows ? root.rows.length : 0
    readonly property var widths: root.computeWidths(root.width, root.columns)
    signal picked(int index, var row)
    signal activated(int index, var row)

    objectName: root.objectPrefix
    implicitHeight: col.implicitHeight
    implicitWidth: 320
    activeFocusOnTab: root.selectable && root.count > 0
    Accessible.role: Accessible.List

    function computeWidths(total: real, cols: var): var {
        let fixed = 0, fill = 0
        for (const c of cols) {
            if (c.width > 0) fixed += c.width
            else fill += (c.fill > 0 ? c.fill : 1)
        }
        const avail = Math.max(0, total - Theme.space15 - fixed)
        return cols.map(c => c.width > 0 ? c.width : (fill > 0 ? avail * (c.fill > 0 ? c.fill : 1) / fill : 0))
    }
    function isEnabled(i: int): bool { return i >= 0 && i < root.count && !(root.rows[i] && root.rows[i].disabled === true) }
    function select(i: int) {
        if (!root.isEnabled(i) || i === root.currentIndex) return
        root.currentIndex = i
        root.picked(i, root.rows[i])
    }
    function step(delta: int) {
        let i = root.currentIndex
        for (let n = 0; n < root.count; ++n) {
            i += delta
            if (i < 0 || i >= root.count) return
            if (root.isEnabled(i)) { root.select(i); return }
        }
    }
    Keys.onPressed: (event) => {
        if (!root.selectable) return
        if (event.key === Qt.Key_Down) { root.currentIndex < 0 ? root.select(root.nextFrom(-1, 1)) : root.step(1); event.accepted = true }
        else if (event.key === Qt.Key_Up) { root.currentIndex < 0 ? root.select(root.nextFrom(root.count, -1)) : root.step(-1); event.accepted = true }
        else if (event.key === Qt.Key_Home) { root.select(root.nextFrom(-1, 1)); event.accepted = true }
        else if (event.key === Qt.Key_End) { root.select(root.nextFrom(root.count, -1)); event.accepted = true }
        else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && root.isEnabled(root.currentIndex)) {
            root.activated(root.currentIndex, root.rows[root.currentIndex])
            event.accepted = true
        }
    }
    function nextFrom(start: int, delta: int): int {
        for (let i = start + delta; i >= 0 && i < root.count; i += delta) if (root.isEnabled(i)) return i
        return -1
    }

    Column {
        id: col
        width: parent.width
        Item {   // head
            objectName: root.objectPrefix + "Header"
            width: parent.width
            height: 32
            Row {
                x: Theme.space15
                height: parent.height
                Repeater {
                    model: root.columns
                    delegate: Text {
                        required property var modelData
                        required property int index
                        objectName: root.objectPrefix + "Head_" + modelData.key
                        width: root.widths.length > index ? root.widths[index] : 0
                        height: 32
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                        rightPadding: Theme.space2
                        horizontalAlignment: modelData.align === "right" ? Text.AlignRight
                                           : modelData.align === "center" ? Text.AlignHCenter : Text.AlignLeft
                        text: modelData.title ?? ""
                        textFormat: Text.PlainText
                        font.family: Theme.mono
                        font.pixelSize: Theme.roles.tableHeader.size
                        font.weight: Font.Medium
                        font.letterSpacing: Theme.roles.tableHeader.spacing
                        font.capitalization: Font.AllUppercase
                        color: Theme.muted
                    }
                }
            }
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.rule }
        }
        Repeater {
            model: root.rows
            delegate: UzTableRow {
                required property var modelData
                required property int index
                width: col.width
                columns: root.columns
                widths: root.widths
                row: modelData
                rowIndex: index
                namePrefix: root.objectPrefix
                cellDelegate: root.cellDelegate
                selectable: root.selectable
                selected: root.selectable && index === root.currentIndex
                disabled: modelData && modelData.disabled === true
                showFocus: root.activeFocus && selected
                onClicked: { root.forceActiveFocus(); root.select(index) }
            }
        }
        Item {   // empty state
            id: emptyWrap
            objectName: root.objectPrefix + "Empty"
            visible: root.count === 0 && (root.emptyText !== "" || emptyBox.children.length > 0)
            width: parent.width
            height: visible ? Math.max(Theme.row44 * 2, emptyBox.childrenRect.height + emptyLabel.height) : 0
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.rule2 }
            Text {
                id: emptyLabel
                visible: root.emptyText !== ""
                x: Theme.space15
                width: parent.width - 2 * Theme.space15
                height: visible ? Theme.row44 * 2 : 0
                verticalAlignment: Text.AlignVCenter
                wrapMode: Text.Wrap
                text: root.emptyText
                textFormat: Text.PlainText
                font.family: Theme.sans
                font.pixelSize: Theme.roles.input.size
                color: Theme.muted
            }
            Item { id: emptyBox; y: emptyLabel.height; width: parent.width }
        }
        Item {   // details section under the table
            id: detailsBox
            width: parent.width
            height: childrenRect.height
            visible: children.length > 0
        }
    }
}
