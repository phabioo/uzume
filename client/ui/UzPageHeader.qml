import QtQuick

// The one page header (design-system.md "PageHeader", readable-pass.md "Rules for every screen"): an optional Mono eyebrow
// (`label`, with `number` and `labelMeta`), the page title (Plex Sans Light 40/40, -2 %, once per page), an optional `meta`
// line under it and the page actions (buttons, default slot). The title and the meta line wrap and never elide; the actions
// sit at the right of the title baseline when they fit next to it and wrap below the title otherwise. The width is the
// width the page gives it (every main page uses the same content max width, Theme.contentMaxWidth).
// The "01 —" number of the eyebrow only shows at 1440 px and wider (`decorated`; Theme.columnsCompactMax).
Item {
    id: root
    property int number: -1
    property string label: ""
    property string labelMeta: ""      // Mono text at the far right of the eyebrow
    property string title: ""
    property string meta: ""           // one muted line under the title
    property string eyebrowName: "pageEyebrow"
    property string titleName: "pageTitle"   // objectName of the title text (pages keep their own names)
    property bool decorated: Window.width >= Theme.columnsCompactMax
    default property alias actions: actionRow.data
    readonly property bool actionsBelow: root.width > 0 && actionRow.shown
                                         && titleMetrics.advanceWidth + Theme.space3 + actionRow.natural > root.width
    implicitHeight: titleBlock.height + (root.actionsBelow ? Theme.space2 + actionRow.height : 0)
    implicitWidth: Math.max(titleMetrics.advanceWidth, 200) + (actionRow.shown ? Theme.space3 + actionRow.natural : 0)

    TextMetrics {
        id: titleMetrics
        text: root.title
        font: titleText.font
    }

    Column {
        id: titleBlock
        width: root.actionsBelow || !actionRow.shown ? root.width : Math.max(0, root.width - actionRow.natural - Theme.space3)
        spacing: Theme.space1
        UzSectionLabel {
            objectName: root.eyebrowName
            width: root.width
            number: root.decorated ? root.number : -1
            keepNumber: true
            text: root.label !== "" ? root.label : root.title
            meta: root.labelMeta
        }
        Text {
            id: titleText
            objectName: root.titleName
            width: parent.width
            wrapMode: Text.Wrap
            text: root.title
            textFormat: Text.PlainText
            font.family: Theme.sans
            font.pixelSize: Theme.roles.title.size
            font.weight: Font.Light
            font.letterSpacing: Theme.roles.title.spacing
            color: Theme.ink
            lineHeight: Theme.roles.title.line
            lineHeightMode: Text.FixedHeight
        }
        Text {
            objectName: "pageMeta"
            visible: root.meta !== ""
            width: parent.width
            wrapMode: Text.Wrap
            text: root.meta
            textFormat: Text.PlainText
            font.family: Theme.sans
            font.pixelSize: Theme.roles.small.size
            lineHeight: Theme.roles.small.line
            lineHeightMode: Text.FixedHeight
            color: Theme.muted
        }
    }
    Flow {
        id: actionRow
        objectName: "pageActions"
        // Natural width of all visible actions in one line (a Flow reports the width it was given, not this one).
        readonly property real natural: {
            let w = 0
            let n = 0
            for (let i = 0; i < children.length; ++i) {
                const c = children[i]
                if (!c.visible) continue
                w += (c.implicitWidth > 0 ? c.implicitWidth : c.width)
                ++n
            }
            return w + Math.max(0, n - 1) * Theme.space2
        }
        readonly property bool shown: natural > 0
        spacing: Theme.space2
        width: root.actionsBelow ? root.width : natural
        x: root.actionsBelow ? 0 : root.width - width
        y: root.actionsBelow ? titleBlock.height + Theme.space2
                             : titleBlock.height - (children.length > 0 ? height : 0)
    }
}
