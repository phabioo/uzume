import QtQuick
import QtQuick.Layouts

// Dialog (design-system.md "Dialog"): 400 px (`size: "lg"` 560 px) on `surface` in a 1 px `rule` frame over the 80 % scrim; no
// shadow, no blur, no card inside. Mono label naming the area, title 20/28, text, content (default slot), an error line,
// the action row (primary, secondary at the left, `dangerActions` at the far right; it stacks when it does not fit) and optional hint keys. `busy` dims the
// content (60 %, no input) and keeps the dialog open until the work is done. Like the other dialogs of the app it fills its
// parent (the window content) and is shown with `open()` / hidden with `close()`; Esc closes it unless it is busy.
//   UzDialog { label: qsTr("Saves"); title: qsTr("Restore this save?"); text: qsTr("..."); actions: [UzButton {...}] }
FocusScope {
    id: root
    property string label: ""
    property string title: ""
    property string text: ""
    property string error: ""
    property string size: "md"          // md | lg
    property bool busy: false
    property bool closable: true         // header close button
    property bool shown: false
    readonly property alias frame: frame
    default property alias content: contentCol.data
    property alias actions: mainActions.data
    property alias dangerActions: dangerActions.data
    property alias hints: hintsBox.data
    signal closed()

    function open() { shown = true }
    function close() {
        if (busy || !shown) return
        shown = false
        closed()
    }

    anchors.fill: parent
    z: 100
    visible: shown
    focus: shown
    onShownChanged: if (shown) forceActiveFocus()
    Keys.onEscapePressed: (event) => { if (root.shown && !root.busy) { root.close(); event.accepted = true } }

    Rectangle {   // the scrim: ground at 80 %, swallows clicks
        objectName: "dialogScrim"
        anchors.fill: parent
        color: Theme.scrim
        MouseArea { anchors.fill: parent; hoverEnabled: true; acceptedButtons: Qt.AllButtons; onWheel: (wheel) => wheel.accepted = true }
    }
    Rectangle {
        id: frame
        objectName: "dialogFrame"
        anchors.centerIn: parent
        width: Math.min(root.size === "lg" ? Theme.dialogLg : Theme.dialog, Math.max(0, root.width - 2 * Theme.space2))
        height: Math.min(layout.implicitHeight, Math.max(0, root.height - 2 * Theme.space2))
        radius: 0
        color: Theme.surface
        border.width: 1
        border.color: Theme.rule
        clip: true
        MouseArea { anchors.fill: parent }

        ColumnLayout {
            id: layout
            width: parent.width
            spacing: Theme.space15

            RowLayout {   // header: label + title, close button
                Layout.fillWidth: true
                Layout.topMargin: 28
                Layout.leftMargin: 28
                Layout.rightMargin: 28
                spacing: Theme.space2
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.space15
                    Text {
                        objectName: "dialogLabel"
                        visible: root.label !== ""
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        text: root.label
                        textFormat: Text.PlainText
                        font.family: Theme.mono
                        font.pixelSize: Theme.roles.meta.size
                        font.weight: Font.Medium
                        font.letterSpacing: 1.4
                        font.capitalization: Font.AllUppercase
                        color: Theme.muted
                    }
                    Text {
                        objectName: "dialogTitle"
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        text: root.title
                        textFormat: Text.PlainText
                        font.family: Theme.sans
                        font.pixelSize: Theme.roles.dialogTitle.size
                        font.letterSpacing: Theme.roles.dialogTitle.spacing
                        lineHeight: Theme.roles.dialogTitle.line
                        lineHeightMode: Text.FixedHeight
                        color: Theme.ink
                    }
                }
                UzIconButton {
                    objectName: "dialogClose"
                    visible: root.closable
                    Layout.alignment: Qt.AlignTop
                    Layout.topMargin: -6
                    Layout.rightMargin: -10
                    glyph: "cross"
                    glyphSize: 12
                    enabled: !root.busy
                    Accessible.name: qsTr("Close")
                    onClicked: root.close()
                }
            }
            Text {
                visible: root.text !== ""
                Layout.fillWidth: true
                Layout.leftMargin: 28
                Layout.rightMargin: 28
                wrapMode: Text.Wrap
                text: root.text
                textFormat: Text.PlainText
                font.family: Theme.sans
                font.pixelSize: Theme.roles.body.size
                lineHeight: Theme.roles.body.line
                lineHeightMode: Text.FixedHeight
                color: Theme.ink2
                opacity: root.busy ? 0.6 : 1
            }
            UzStatusIcon {
                objectName: "dialogError"
                visible: root.error !== ""
                Layout.leftMargin: 28
                Layout.rightMargin: 28
                kind: "error"
                small: true
                text: root.error
                maxWidth: frame.width - 56
            }
            ColumnLayout {
                id: contentCol
                objectName: "dialogContent"
                Layout.fillWidth: true
                Layout.leftMargin: 28
                Layout.rightMargin: 28
                spacing: Theme.space2
                opacity: root.busy ? 0.6 : 1
                enabled: !root.busy
            }
            Item {
                // The action row. Primary and secondary buttons sit at the left, `dangerActions` at the far right. When they do not
                // all fit in one line (wide fonts, long labels) they stack inside the dialog, full width: danger or primary first,
                // Cancel (the first of `actions`) below, so no button ever leaves the frame. The children of `actions` and
                // `dangerActions` are placed here by place(); both containers fill this item.
                id: actionsBox
                objectName: "dialogActions"
                // Counts all children, shown or not: `visible` of a child follows this item, so it cannot decide it.
                visible: mainActions.children.length > 0 || dangerActions.children.length > 0
                Layout.fillWidth: true
                Layout.topMargin: Theme.space2 - Theme.space15
                Layout.leftMargin: 28
                Layout.rightMargin: 28
                readonly property real gap: Theme.space1

                function visibleKids(box): var {
                    const r = []
                    for (let i = 0; i < box.children.length; ++i) {
                        if (box.children[i].visible) r.push(box.children[i])
                    }
                    return r
                }
                function natural(k): real { return k.implicitWidth > 0 ? k.implicitWidth : k.width }
                function heightOf(k): real { return k.implicitHeight > 0 ? k.implicitHeight : k.height }
                // Re-evaluated whenever a button appears, disappears or changes its label.
                readonly property var plan: {
                    const m = actionsBox.visibleKids(mainActions)
                    const d = actionsBox.visibleKids(dangerActions)
                    const all = m.concat(d)
                    let sum = 0
                    let maxH = 0
                    for (let i = 0; i < all.length; ++i) {
                        sum += actionsBox.natural(all[i])
                        maxH = Math.max(maxH, actionsBox.heightOf(all[i]))
                    }
                    sum += Math.max(0, all.length - 1) * actionsBox.gap
                    const stacked = all.length > 1 && sum > actionsBox.width
                    let h = maxH
                    if (stacked) {
                        h = (all.length - 1) * actionsBox.gap
                        for (let j = 0; j < all.length; ++j) h += actionsBox.heightOf(all[j])
                    }
                    return { main: m, danger: d, stacked: stacked, height: h, width: actionsBox.width }
                }
                readonly property bool stacked: actionsBox.plan.stacked
                implicitHeight: actionsBox.plan.height
                onPlanChanged: actionsBox.place()
                Component.onCompleted: actionsBox.place()

                function place() {
                    const p = actionsBox.plan
                    const w = actionsBox.width
                    if (p.stacked) {
                        const order = p.danger.concat(p.main.slice().reverse())
                        let y = 0
                        for (let i = 0; i < order.length; ++i) {
                            const k = order[i]
                            k.width = w
                            k.x = 0
                            k.y = y
                            y += actionsBox.heightOf(k) + actionsBox.gap
                        }
                        return
                    }
                    let x = 0
                    for (let i = 0; i < p.main.length; ++i) {
                        const k = p.main[i]
                        k.width = actionsBox.natural(k)
                        k.x = x
                        k.y = (p.height - actionsBox.heightOf(k)) / 2
                        x += k.width + actionsBox.gap
                    }
                    let dx = w
                    for (let j = p.danger.length - 1; j >= 0; --j) {
                        const k = p.danger[j]
                        k.width = actionsBox.natural(k)
                        dx -= k.width
                        k.x = dx
                        k.y = (p.height - actionsBox.heightOf(k)) / 2
                        dx -= actionsBox.gap
                    }
                }
                Item { id: mainActions; anchors.fill: parent }
                Item { id: dangerActions; anchors.fill: parent }
            }
            Item {   // hints: a rule-2 line above, keys below
                id: hintsWrap
                visible: hintsBox.children.length > 0
                Layout.fillWidth: true
                implicitHeight: hintsBox.implicitHeight + Theme.space15 + 1
                Rectangle { width: parent.width; height: 1; color: Theme.rule2 }
                Flow { id: hintsBox; x: 28; y: Theme.space15 + 1; width: parent.width - 56; spacing: Theme.space2 }
            }
            Item { Layout.preferredHeight: hintsWrap.visible ? Theme.space15 : 28 - Theme.space15 }
        }
    }
}
