import QtQuick
import QtQuick.Layouts

// ColumnNav (design-system.md "ColumnNav"): the second column of Settings, Emulation, Controllers and the server settings.
// A column of UzColumnNavRow items (anywhere below it; section labels and hints may sit between them). It owns the keyboard
// and pad rules of the column:
//   Up / Down  move between the rows, Enter / A open a row (the row's `clicked`), Right asks the page to move into the content
//   (`contentRequested`; the page calls focusFirstIn(content)), Left / B / Esc from the content come back through
//   `backFromContent()`, which focuses the selected row.
// `content` is the Item of the page content (the Flickable) so the nav knows where "inside the content" is.
ColumnLayout {
    id: root
    readonly property bool colnavNav: true
    property Item content: null
    property Flickable flickable: null   // the column's own scroller: a focused row is scrolled into view
    signal contentRequested()

    function collectRows(item: Item, found: var) {
        for (let i = 0; i < item.children.length; ++i) {
            const c = item.children[i]
            if (c.colnavRow === true) {
                if (c.visible) found.push(c)
            } else if (c.visible !== false) {
                root.collectRows(c, found)
            }
        }
    }
    // All visible rows below this item in visual order.
    function rows(): var {
        const found = []
        root.collectRows(root, found)
        found.sort(function (a, b) { return root.mapFromItem(a, 0, 0).y - root.mapFromItem(b, 0, 0).y })
        return found
    }
    function step(row: Item, dir: int) {
        const all = root.rows()
        const at = all.indexOf(row)
        const to = at + dir
        if (at >= 0 && to >= 0 && to < all.length) all[to].forceActiveFocus(dir > 0 ? Qt.TabFocusReason : Qt.BacktabFocusReason)
    }
    function reveal(row: Item) {
        const f = root.flickable
        if (!f) return
        const y = row.mapToItem(f.contentItem, 0, 0).y
        const maxY = Math.max(0, f.contentHeight - f.height)
        if (y < f.contentY) {
            f.contentY = Math.max(0, y - Theme.space2)
        } else if (y + row.height > f.contentY + f.height) {
            f.contentY = Math.min(maxY, y + row.height - f.height + Theme.space2)
        }
    }
    // The selected row, else the first one, takes the focus.
    function focusSelected(): bool {
        const all = root.rows()
        if (all.length === 0) return false
        const sel = all.filter(function (r) { return r.selected });
        (sel.length > 0 ? sel[0] : all[0]).forceActiveFocus(Qt.TabFocusReason)
        return true
    }
    function hasFocus(): bool {
        const f = root.Window.activeFocusItem
        for (let p = f; p; p = p.parent) {
            if (p === root) return true
        }
        return false
    }
    function inContent(): bool {
        if (!root.content) return false
        const f = root.Window.activeFocusItem
        for (let p = f; p; p = p.parent) {
            if (p === root.content) return true
        }
        return false
    }
    // Focus inside the content goes back to the column. True when it did.
    function backFromContent(): bool {
        return root.inContent() && root.focusSelected()
    }
    function firstTabStop(it: Item): var {
        const kids = (it.contentY !== undefined && it.contentItem) ? it.contentItem.children : it.children
        for (let i = 0; i < kids.length; ++i) {
            const c = kids[i]
            if (!c.visible || !c.enabled) continue
            if (c.activeFocusOnTab === true && c.colnavRow !== true) return c
            const hit = root.firstTabStop(c)
            if (hit) return hit
        }
        return null
    }
    // First focusable control (Tab stop) below `item`, in declaration order: the target of Right from a row.
    function focusFirstIn(item: Item): bool {
        const target = item ? root.firstTabStop(item) : null
        if (!target) return false
        target.forceActiveFocus(Qt.TabFocusReason)
        return true
    }
}
