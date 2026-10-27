import QtQuick

// Repeater for the children of a layout (plugin blocks). The delegates are pooled: the pool only grows, a delegate that is no
// longer needed is hidden by the caller (`visible: index < count`) but never destroyed while its layout lives, and a change of
// `count` never rebuilds the existing ones.
//
// Why: Qt 6.4's QQuickGridLayoutBase::rearrange (RowLayout / ColumnLayout / GridLayout) does not refresh its item list before it
// runs from QQuickLayout::geometryChange (fixed upstream in Qt 6.7, which calls ensureLayoutItemsUpdated first). A delegate that
// a Repeater destroyed after the layout was invalidated, but before the layout's own polish, is still in that list; the next
// size change of the layout (the plugin blocks are nested layouts sized by `anchors.fill` of a Loader, so their parent layout's
// rearrange resizes them synchronously, before they are polished) reads the freed item in QGridLayoutItem::stretchFactor and
// the app dies with SIGSEGV. Changing a Repeater `model` (an integer or a new JS array on every tree update) destroys all its
// delegates, so every fb.ui.set of a table, list or container was exposed. Hidden items are skipped by the layout and stay valid.
Repeater {
    id: pool
    property int count: 0
    model: ListModel { id: slots }
    function grow() {
        while (slots.count < pool.count) slots.append({ "slot": slots.count })
    }
    onCountChanged: pool.grow()
    Component.onCompleted: pool.grow()
}
