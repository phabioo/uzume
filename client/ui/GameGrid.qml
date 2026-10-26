import QtQuick
import Uzume.App

// Virtualized game grid for the Library (list rows or, with `tiles`, the tile grid of 3c-2-r6). It lives inside the Library's Flickable (`scroller`) and only keeps the GridView
// window around the visible part alive: the GridView is as tall as the viewport (plus a buffer) and is moved down with
// the scroll position, so delegates are created and recycled for the visible rows only, however large the Library is.
// The host reserves the full content height, so the scroll bar and the layout around it behave as with a plain grid.
Item {
    id: host
    required property Flickable scroller
    required property int cellWidth
    required property int cellHeight
    property alias model: view.model
    property bool showAdded: false
    property bool showPersonal: false
    property bool tiles: false        // GameCard tiles (Grid view) instead of GameTile rows (List view)
    property int numberOffset: 0      // rows before this grid: the second table of Ready first goes on counting
    property string selectedId: ""
    property string runningId: ""
    readonly property alias count: view.count
    property int createdCount: 0   // tiles instantiated so far (tests: stays small for large libraries)
    readonly property int alive: view.contentItem.children.length
    property int buffer: cellHeight
    signal tileClicked(string gameId)

    // Top of this grid in the coordinates of the scroller's content; re-evaluated when the layout above moves.
    readonly property real topInContent: {
        host.y; host.parent ? host.parent.y : 0; host.scroller.contentHeight; host.height
        return host.mapToItem(host.scroller.contentItem, 0, 0).y
    }
    readonly property real fullHeight: view.contentHeight
    readonly property real windowHeight: Math.min(fullHeight, host.scroller.height + 2 * buffer)
    readonly property real windowTop: Math.max(0, Math.min(fullHeight - windowHeight, host.scroller.contentY - topInContent - buffer))

    implicitHeight: fullHeight

    // The tile that currently exists for this game, else null (tests).
    function tileFor(id) {
        const items = view.contentItem.children
        for (let i = 0; i < items.length; ++i) {
            if (items[i].gameId === id) return items[i]
        }
        return null
    }

    readonly property int columns: Math.max(1, Math.floor(host.width / Math.max(1, host.cellWidth)))

    // Moves the focus by whole rows / columns (Up / Down / Left / Right on a tile); scrolls the target into view first, because
    // only the tiles around the viewport exist.
    function moveFocus(from, dRow, dCol) {
        const to = from + dRow * host.columns + (host.columns > 1 ? dCol : 0)
        if (to < 0 || to >= view.count || to === from) return
        const sc = host.scroller
        const top = host.topInContent + Math.floor(to / host.columns) * host.cellHeight
        if (top < sc.contentY) sc.contentY = Math.max(0, top - Theme.space1)
        else if (top + host.cellHeight > sc.contentY + sc.height) sc.contentY = Math.min(Math.max(0, sc.contentHeight - sc.height), top + host.cellHeight - sc.height + Theme.space1)
        focusLater.target = to
        focusLater.restart()
    }
    Timer {
        id: focusLater
        property int target: -1
        interval: 30
        onTriggered: {
            const items = view.contentItem.children
            for (let i = 0; i < items.length; ++i) {
                if (items[i].index === target) { items[i].forceActiveFocus(Qt.TabFocusReason); return }
            }
        }
    }

    function syncWindow() {
        if (view.contentY !== windowTop) view.contentY = windowTop
    }
    onWindowTopChanged: syncWindow()
    onWindowHeightChanged: syncWindow()

    GridView {
        id: view
        y: host.windowTop
        width: host.width
        height: host.windowHeight
        interactive: false
        currentIndex: -1   // no current item that would stay alive outside the window
        clip: false
        cacheBuffer: 0
        cellWidth: host.cellWidth
        cellHeight: host.cellHeight
        add: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.durList } }
        displaced: Transition { NumberAnimation { properties: "x,y"; duration: Theme.durList; easing.type: Easing.OutCubic } }
        onContentYChanged: host.syncWindow()
        onContentHeightChanged: host.syncWindow()
        delegate: host.tiles ? cardDelegate : rowDelegate
    }
    Component {
        id: rowDelegate
        GameTile {
            width: view.cellWidth
            height: view.cellHeight
            showAdded: host.showAdded
            showPersonal: host.showPersonal
            numberOffset: host.numberOffset
            selected: gameId === host.selectedId
            running: host.runningId !== "" && host.runningId === gameId
            onClicked: host.tileClicked(gameId)
            onNavigate: (dRow, dCol) => host.moveFocus(index, dRow, dCol)
            Component.onCompleted: host.createdCount++
        }
    }
    Component {
        id: cardDelegate
        GameCard {
            width: view.cellWidth
            height: view.cellHeight
            showPersonal: host.showPersonal
            numberOffset: host.numberOffset
            selected: gameId === host.selectedId
            running: host.runningId !== "" && host.runningId === gameId
            onClicked: host.tileClicked(gameId)
            onNavigate: (dRow, dCol) => host.moveFocus(index, dRow, dCol)
            Component.onCompleted: host.createdCount++
        }
    }
}
