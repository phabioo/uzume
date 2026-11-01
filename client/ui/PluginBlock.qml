import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Recursive renderer of a validated plugin block tree (docs: ui spec §4). `node` is the QVariantMap of one block
// ({type, id, children, ...props}); every nested block is a PluginBlock again and bubbles its actions up, so the root
// emits action(action, value) for buttons, toggles, selects, list rows, map markers etc. `dark` is true for in-game
// surfaces (always dark). Plugins choose tones, size names and player numbers only; all mapping is in Theme.
Item {
    id: root
    property var node: ({})
    // PluginChild sets it: only the root block of a view (panel tab, sheet, run section, detail section, tile) prepares the tree.
    property bool nested: false
    property bool dark: true
    property string pluginId: ""
    signal action(string action, var value)

    // Design system: one primary button per view. In document order only the first primary button (an `empty` block's action
    // counts as one, unless the empty block is an error state) stays primary; every later one is drawn as secondary. Each tab
    // of a `tabs` block starts from what came before the block, so a hidden tab never demotes a visible one. The tree the
    // plugin sent stays untouched (console, MCP); the renderer works on `shown`.
    function primariesDemoted(tree) {
        function walk(n, seen) {
            if (!n || typeof n !== "object") return {node: n, seen: seen}
            let out = n
            let s = seen
            if (n.type === "button" && n.variant === "primary") {
                if (s) out = Object.assign({}, n, {variant: "secondary"})
                s = true
            } else if (n.type === "empty" && n.action_label && n.tone !== "danger") {
                if (s) out = Object.assign({}, n, {action_variant: "secondary"})
                s = true
            }
            const kids = n.children
            if (kids && kids.length !== undefined && kids.length > 0) {
                const isTabs = n.type === "tabs"
                let cur = s
                let any = s
                const mapped = []
                for (let i = 0; i < kids.length; ++i) {
                    const r = walk(kids[i], isTabs ? s : cur)
                    mapped.push(r.node)
                    if (isTabs) any = any || r.seen
                    else cur = r.seen
                }
                s = isTabs ? any : cur
                let changed = false
                for (let i = 0; i < kids.length; ++i) if (mapped[i] !== kids[i]) changed = true
                if (changed) out = Object.assign({}, out, {children: mapped})
            }
            return {node: out, seen: s}
        }
        return walk(tree, false).node
    }
    readonly property var shown: nested ? node : primariesDemoted(node)

    readonly property string type: shown.type || ""
    readonly property var kids: shown.children || []
    readonly property bool fillsHeight: type === "scroll" && !shown.max_height
    objectName: shown.id ? "plugin_" + shown.id : "plugin_" + type

    implicitWidth: loader.item ? loader.item.implicitWidth : 0
    implicitHeight: loader.item ? loader.item.implicitHeight : 0

    // Blocks of natural size stay left-aligned in a stretching column; the others take the full width.
    function stretches(t) {
        return ["chip", "badge", "avatar", "image", "button", "segmented"].indexOf(t) < 0
    }
    // Children of a row that give way when the row is narrow (text wraps or elides, containers shrink); the others keep their size.
    function shrinks(t) {
        return ["text", "stat", "bar", "select", "textfield", "textarea", "location", "card", "column", "row", "grid", "list", "table"].indexOf(t) >= 0
    }
    function alignFlag(horizontal) {
        switch (shown.align || (horizontal ? "stretch" : "center")) {
        case "start": return horizontal ? Qt.AlignLeft : Qt.AlignTop
        case "end": return horizontal ? Qt.AlignRight : Qt.AlignBottom
        case "center": return horizontal ? Qt.AlignHCenter : Qt.AlignVCenter
        default: return 0
        }
    }

    Loader {
        id: loader
        anchors.fill: parent
        visible: sourceComponent !== null
        sourceComponent: {
            switch (root.type) {
            case "column": return columnC
            case "row": return rowC
            case "grid": return gridC
            case "tabs": return tabsC
            case "scroll": return scrollC
            case "card": return cardC
            case "section": return sectionC
            case "text": return textC
            case "chip": return chipC
            case "badge": return badgeC
            case "avatar": return avatarC
            case "image": return imageC
            case "bar": return barC
            case "stat": return statC
            case "list": return listC
            case "table": return tableC
            case "button": return buttonC
            case "toggle": return toggleC
            case "select": return selectC
            case "segmented": return segmentedC
            case "textfield": return textfieldC
            case "textarea": return textareaC
            case "empty": return emptyC
            case "location": return locationC
            case "map": return mapC
            default: return null
            }
        }
    }

    // ---- layout blocks
    Component {
        id: columnC
        ColumnLayout {
            spacing: Theme.gapPx(root.shown.gap)
            PluginPool {
                count: root.kids.length
                delegate: PluginChild {
                    required property int index
                    visible: index < root.kids.length   // pooled: surplus delegates are hidden, never destroyed (PluginPool)
                    readonly property var kid: root.kids[index] || ({})
                    node: kid
                    dark: root.dark
                    pluginId: root.pluginId
                    onAction: (a, v) => root.action(a, v)
                    Layout.fillWidth: (root.shown.align || "stretch") === "stretch" && (root.stretches(kid.type) || (kid.type === "image" && !!kid.label))
                    Layout.fillHeight: kid.type === "scroll" && !kid.max_height
                    Layout.alignment: (root.shown.align || "stretch") === "stretch" ? ((root.stretches(kid.type) || (kid.type === "image" && !!kid.label)) ? 0 : Qt.AlignLeft) : root.alignFlag(true)
                }
            }
        }
    }
    Component {
        id: rowC
        RowLayout {
            id: rowLayout
            spacing: Theme.gapPx(root.shown.gap)
            property int fillers: 0
            // A row with an input: its texts keep their natural width, so the input takes the free space (otherwise the
            // texts and the input share it and the left edge of the inputs differs from row to row).
            property bool hasInput: false
            function countFillers() {
                var n = 0
                var input = false
                for (var i = 0; i < root.kids.length; ++i) {
                    const k = root.kids[i] || ({})
                    if (root.shrinks(k.type) || (k.type === "image" && !!k.label)) ++n
                    if (k.type === "textfield" || k.type === "textarea") input = true
                }
                fillers = n
                hasInput = input
            }
            Component.onCompleted: countFillers()
            Connections { target: root; function onKidsChanged() { rowLayout.countFillers() } }
            PluginPool {
                count: root.kids.length
                delegate: PluginChild {
                    required property int index
                    visible: index < root.kids.length   // pooled: surplus delegates are hidden, never destroyed (PluginPool)
                    readonly property var kid: root.kids[index] || ({})
                    // an image with a label gives way as well: its label elides
                    readonly property bool giveWay: root.shrinks(kid.type) || (kid.type === "image" && !!kid.label)
                    node: kid
                    dark: root.dark
                    pluginId: root.pluginId
                    onAction: (a, v) => root.action(a, v)
                    // badges and chips give way last, by eliding; a text never gets narrower than min(its width, 96 px scaled)
                    readonly property bool yields: kid.type === "badge" || kid.type === "chip"
                    // fillWidth + maximumWidth = natural width: never grows, but shrinks (a Layout item without fillWidth would not)
                    Layout.fillWidth: giveWay || yields
                    // an image with a label never grows past its natural width either: otherwise it shares the free space with the
                    // texts beside it and pushes them to the middle of the row
                    Layout.maximumWidth: yields || (kid.type === "image" && !!kid.label) || (rowLayout.hasInput && kid.type === "text") ? implicitWidth : Number.POSITIVE_INFINITY
                    // a chip follows its text and only gives way beyond 160 px scaled (a badge too, so a short badge such as "Nicht unterstützt" next to a long text never elides)
                    Layout.minimumWidth: kid.type === "text" ? Math.min(implicitWidth, Math.round(96 * Theme.textScale))
                                         : kid.type === "chip" ? Math.min(implicitWidth, Math.round(160 * Theme.textScale))
                                         // a badge keeps its text while the text beside it still gets its minimum, else it elides
                                         : kid.type === "badge" ? Math.min(implicitWidth, Math.max(0, rowLayout.width - Math.round(96 * Theme.textScale) - rowLayout.spacing))
                                         : giveWay || yields ? 0 : implicitWidth
                    Layout.fillHeight: root.shown.align === "stretch"
                    Layout.alignment: root.alignFlag(false)
                }
            }
            Item { visible: rowLayout.fillers === 0; Layout.fillWidth: true }
        }
    }
    Component {
        id: gridC
        GridLayout {
            columns: Math.max(1, Math.min(6, root.shown.columns || 1))
            columnSpacing: Theme.gapPx(root.shown.gap)
            rowSpacing: Theme.gapPx(root.shown.gap)
            PluginPool {
                count: root.kids.length
                delegate: PluginChild {
                    required property int index
                    visible: index < root.kids.length   // pooled: surplus delegates are hidden, never destroyed (PluginPool)
                    readonly property var kid: root.kids[index] || ({})
                    node: kid
                    dark: root.dark
                    pluginId: root.pluginId
                    onAction: (a, v) => root.action(a, v)
                    Layout.fillWidth: true
                    Layout.preferredWidth: 1
                    Layout.alignment: Qt.AlignTop
                }
            }
        }
    }
    Component {
        id: tabsC
        ColumnLayout {
            id: tabs
            spacing: Theme.space15
            property int localSel: 0
            readonly property int nodeSel: root.shown.selected || 1
            readonly property int sel: Math.max(1, Math.min(root.kids.length, localSel > 0 ? localSel : nodeSel))
            onNodeSelChanged: localSel = 0
            function select(i) {   // 1-based; LB / RB hook
                const n = Math.max(1, Math.min(root.kids.length, i))
                if (n === sel) return
                localSel = n
                root.action(root.shown.action || "", n)
            }
            function next() { select(sel + 1) }
            function previous() { select(sel - 1) }
            onSelChanged: Qt.callLater(tabStrip.ensureVisible)
            // The tab header never exceeds the width: a clipped, horizontally flickable strip keeps the selected tab in view.
            Flickable {
                id: tabStrip
                objectName: "pluginTabStrip"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                implicitWidth: tabRow.implicitWidth
                implicitHeight: Theme.row36
                clip: true
                contentWidth: tabRow.implicitWidth
                contentHeight: height
                flickableDirection: Flickable.HorizontalFlick
                boundsBehavior: Flickable.StopAtBounds
                function ensureVisible(index) {
                    const it = tabRepeater.itemAt((index === undefined ? tabs.sel : index) - 1)
                    if (!it) return
                    if (it.x < contentX) contentX = Math.max(0, it.x - 8)
                    else if (it.x + it.width > contentX + width) contentX = Math.min(Math.max(0, contentWidth - width), it.x + it.width - width + 8)
                }
                onWidthChanged: Qt.callLater(ensureVisible)
                Component.onCompleted: Qt.callLater(ensureVisible)
                Rectangle { y: parent.height - 1; width: Math.max(parent.width, parent.contentWidth); height: 1; color: Theme.blockColor("border", root.dark) }
                WheelHandler {
                    acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                    onWheel: (e) => {
                        const d = Math.abs(e.angleDelta.x) > Math.abs(e.angleDelta.y) ? e.angleDelta.x : e.angleDelta.y
                        tabStrip.contentX = Math.max(0, Math.min(Math.max(0, tabStrip.contentWidth - tabStrip.width), tabStrip.contentX - d / 2))
                    }
                }
            Row {
                id: tabRow
                spacing: Theme.space05
                Repeater {
                    id: tabRepeater
                    model: (root.shown.tabs || []).length   // a count, so an unchanged tab strip keeps its delegates (and the focus on a tab)
                    delegate: PluginFocusable {
                        id: tab
                        required property int index
                        readonly property var modelData: (root.shown.tabs || [])[index]
                        objectName: "pluginTab_" + (index + 1)
                        readonly property bool active: tabs.sel === index + 1
                        dark: root.dark
                        implicitHeight: Theme.row36
                        implicitWidth: tabLabel.implicitWidth + 2 * Theme.space15
                        width: implicitWidth
                        height: implicitHeight
                        onActivated: tabs.select(index + 1)
                        onActiveFocusChanged: if (activeFocus) tabStrip.ensureVisible(index + 1)
                        Rectangle {
                            anchors.fill: parent
                            color: tab.hovered && !tab.active ? Theme.blockColor("hover", root.dark) : "transparent"
                        }
                        Text {
                            textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                            id: tabLabel
                            anchors.centerIn: parent
                            text: String(tab.modelData)
                            font.pixelSize: Theme.roles.input.size
                            font.weight: tab.active ? Font.Medium : Font.Normal
                            color: tab.active ? Theme.blockColor("text", root.dark) : (tab.hovered ? Theme.blockColor("textSecondary", root.dark) : Theme.blockColor("textMuted", root.dark))
                        }
                        Rectangle {
                            visible: tab.active
                            anchors.bottom: parent.bottom
                            anchors.horizontalCenter: parent.horizontalCenter
                            width: parent.width - Theme.space2
                            height: 2
                            color: Theme.blockColor("accent", root.dark)
                        }
                    }
                }
            }
            }
            PluginChild {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumWidth: 0
                node: root.kids[tabs.sel - 1] || ({})
                dark: root.dark
                pluginId: root.pluginId
                onAction: (a, v) => root.action(a, v)
            }
        }
    }
    Component {
        id: scrollC
        Flickable {
            id: flick
            readonly property real maxH: ({s: 160, m: 280, l: 420})[root.shown.max_height] || 0
            clip: true
            contentWidth: width
            contentHeight: inner.implicitHeight
            implicitWidth: inner.implicitWidth
            implicitHeight: maxH > 0 ? Math.min(maxH, contentHeight) : contentHeight
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {
                policy: ScrollBar.AsNeeded
                contentItem: Rectangle {
                    implicitWidth: 5
                    radius: 0
                    color: Theme.blockColor("borderButton", root.dark)
                    opacity: parent.active ? 1 : 0.6
                }
            }
            function scrollBy(dy) {   // right stick hook
                contentY = Math.max(0, Math.min(Math.max(0, contentHeight - height), contentY + dy))
            }
            ColumnLayout {
                id: inner
                width: flick.width
                spacing: Theme.space1
                PluginPool {
                    count: root.kids.length
                    delegate: PluginChild {
                        required property int index
                        visible: index < root.kids.length   // pooled: surplus delegates are hidden, never destroyed (PluginPool)
                        readonly property var kid: root.kids[index] || ({})
                        node: kid
                        dark: root.dark
                        pluginId: root.pluginId
                        onAction: (a, v) => root.action(a, v)
                        Layout.fillWidth: true
                    }
                }
            }
        }
    }

    // ---- containers with surface
    Component {
        id: cardC
        PluginFocusable {
            id: card
            readonly property bool hasHeader: !!(root.shown.eyebrow || root.shown.title || root.shown.link_label)
            dark: root.dark
            actionable: !!root.shown.action
            implicitWidth: cardCol.implicitWidth + 2 * Theme.space15
            implicitHeight: cardCol.implicitHeight + 2 * Theme.space15
            onActivated: root.action(root.shown.action, root.shown.value)
            Rectangle {
                anchors.fill: parent
                color: card.hovered ? Theme.blockColor("hover", root.dark) : "transparent"
                border.width: 1
                border.color: Theme.blockColor("cardBorder", root.dark)
            }
            ColumnLayout {
                id: cardCol
                anchors.fill: parent
                anchors.margins: Theme.space15
                spacing: Theme.space1
                RowLayout {
                    visible: card.hasHeader
                    Layout.fillWidth: true
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Text {
                            textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                            visible: !!root.shown.eyebrow
                            text: root.shown.eyebrow || ""
                            font.family: Theme.mono
                            font.pixelSize: Theme.roles.sectionLabel.size
                            font.weight: Theme.roles.sectionLabel.weight
                            font.capitalization: Font.AllUppercase
                            font.letterSpacing: Theme.roles.sectionLabel.spacing
                            color: Theme.blockColor("textFaint", root.dark)
                        }
                        Text {
                            textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                            visible: !!root.shown.title
                            Layout.fillWidth: true
                            text: root.shown.title || ""
                            elide: Text.ElideRight
                            font.pixelSize: Theme.roles.itemTitle.size
                            font.weight: Theme.roles.itemTitle.weight
                            color: Theme.blockColor("text", root.dark)
                        }
                    }
                    Text {
                        textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                        visible: !!root.shown.link_label
                        text: (root.shown.link_label || "") + " ›"
                        font.pixelSize: Theme.roles.small.size
                        color: Theme.tok("accentText", root.dark)
                    }
                }
                PluginPool {
                    count: root.kids.length
                    delegate: PluginChild {
                        required property int index
                        visible: index < root.kids.length   // pooled: surplus delegates are hidden, never destroyed (PluginPool)
                        readonly property var kid: root.kids[index] || ({})
                        node: kid
                        dark: root.dark
                        pluginId: root.pluginId
                        onAction: (a, v) => root.action(a, v)
                        Layout.fillWidth: true
                    }
                }
            }
        }
    }
    Component {
        id: sectionC
        ColumnLayout {
            spacing: Theme.space1
            RowLayout {
                Layout.fillWidth: true
                Text {
                    textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                    Layout.fillWidth: true
                    text: root.shown.title || ""
                    elide: Text.ElideRight
                    font.family: Theme.mono
                    font.pixelSize: Theme.roles.sectionLabel.size
                    font.weight: Theme.roles.sectionLabel.weight
                    font.capitalization: Font.AllUppercase
                    font.letterSpacing: Theme.roles.sectionLabel.spacing
                    color: Theme.blockColor("textFaint", root.dark)
                }
                PluginFocusable {
                    visible: !!root.shown.link_label
                    dark: root.dark
                    actionable: !!root.shown.action
                    implicitWidth: linkText.implicitWidth + 8
                    implicitHeight: 22
                    onActivated: root.action(root.shown.action, root.shown.value)
                    Text {
                        textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                        id: linkText
                        anchors.centerIn: parent
                        text: (root.shown.link_label || "") + " ›"
                        font.pixelSize: Theme.roles.small.size
                        color: Theme.tok("accentText", root.dark)
                    }
                }
            }
            PluginPool {
                count: root.kids.length
                delegate: PluginChild {
                    required property int index
                    visible: index < root.kids.length   // pooled: surplus delegates are hidden, never destroyed (PluginPool)
                    readonly property var kid: root.kids[index] || ({})
                    node: kid
                    dark: root.dark
                    pluginId: root.pluginId
                    onAction: (a, v) => root.action(a, v)
                    Layout.fillWidth: true
                }
            }
        }
    }
    Component {
        id: locationC
        Item {
            id: loc
            readonly property string status: root.shown.status || "none"
            readonly property string tone: status === "open" || status === "encounter" ? "info" : (status === "caught" ? "ok" : "neutral")
            readonly property string statusText: !!root.shown.status_label ? String(root.shown.status_label)
                                                : status === "open" ? qsTr("Open") : status === "encounter" ? qsTr("Encounter now")
                                                : status === "caught" ? qsTr("Caught") : status === "missed" ? qsTr("Missed") : qsTr("No route")
            implicitWidth: locCol.implicitWidth + 2 * Theme.space15
            implicitHeight: locCol.implicitHeight + 2 * Theme.space15
            Rectangle {
                anchors.fill: parent
                color: "transparent"
                border.width: 1
                border.color: loc.tone === "neutral" ? Theme.blockColor("cardBorder", root.dark) : Theme.toneLine(loc.tone, root.dark)
            }
            ColumnLayout {
                id: locCol
                anchors.fill: parent
                anchors.margins: Theme.space15
                spacing: Theme.space1
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.space15
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Text {
                            textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                            Layout.fillWidth: true
                            text: root.shown.title || ""
                            elide: Text.ElideRight
                            font.pixelSize: Theme.roles.itemTitle.size
                            font.weight: Theme.roles.itemTitle.weight
                            color: Theme.blockColor("text", root.dark)
                        }
                        Text {
                            textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                            visible: !!root.shown.sub
                            Layout.fillWidth: true
                            text: root.shown.sub || ""
                            elide: Text.ElideRight
                            font.pixelSize: Theme.roles.small.size
                            color: Theme.blockColor("textMeta", root.dark)
                        }
                    }
                    // Status tag (design-system.md "Tag"): "Encounter now" is the accent plane, "Caught" ok, the rest neutral.
                    Item {
                        objectName: "pluginLocationStatus"
                        implicitWidth: statusTag.implicitWidth
                        implicitHeight: statusTag.implicitHeight
                        Layout.maximumWidth: Math.max(60, locCol.width / 2)
                        UzTag {
                            id: statusTag
                            width: Math.min(implicitWidth, Math.max(60, locCol.width / 2))
                            text: loc.statusText
                            tone: loc.status === "encounter" ? "accent" : (loc.status === "caught" ? "ok" : "neutral")
                            variant: loc.status === "encounter" ? "plane" : "outline"
                        }
                    }
                }
                PluginPool {
                    count: root.kids.length
                    delegate: PluginChild {
                        required property int index
                        visible: index < root.kids.length   // pooled: surplus delegates are hidden, never destroyed (PluginPool)
                        readonly property var kid: root.kids[index] || ({})
                        node: kid
                        dark: root.dark
                        pluginId: root.pluginId
                        onAction: (a, v) => root.action(a, v)
                        Layout.fillWidth: true
                    }
                }
            }
        }
    }
    Component {
        id: emptyC
        ColumnLayout {
            spacing: Theme.space1
            readonly property bool isError: root.shown.tone === "danger"
            Item { Layout.preferredHeight: Theme.space1 }
            Text {
                textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                text: root.shown.title || ""
                font.pixelSize: Theme.roles.itemTitle.size
                font.weight: Theme.roles.itemTitle.weight
                color: isError ? Theme.toneFg("danger", root.dark) : Theme.blockColor("text", root.dark)
            }
            Text {
                textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                visible: !!root.shown.text
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                text: root.shown.text || ""
                font.pixelSize: Theme.roles.input.size
                color: Theme.blockColor("textMuted", root.dark)
            }
            PluginButton {
                visible: !!root.shown.action_label
                Layout.alignment: Qt.AlignHCenter
                Layout.topMargin: Theme.space1
                dark: root.dark
                node: ({label: root.shown.action_label || "", variant: isError ? "secondary" : (root.shown.action_variant || "primary"), action: root.shown.action || ""})
                onAction: (a, v) => root.action(a, v)
            }
            Item { Layout.preferredHeight: Theme.space1 }
        }
    }

    // ---- text and small static blocks
    Component {
        id: textC
        Text {
            textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
            readonly property string st: root.shown.style || "body"
            readonly property bool wraps: root.shown.wrap !== false
            // Styles map onto the type roles: title = header-title, heading = item-title, body = input, meta = small,
            // eyebrow = section-label (Mono, uppercase), value = Mono 11.
            readonly property var role: st === "title" ? Theme.roles.headerTitle : st === "heading" ? Theme.roles.itemTitle
                : st === "meta" ? Theme.roles.small : st === "eyebrow" ? Theme.roles.sectionLabel
                : st === "value" ? Theme.roles.meta11 : Theme.roles.input
            text: root.shown.text || ""
            wrapMode: wraps ? Text.Wrap : Text.NoWrap
            elide: wraps ? Text.ElideNone : Text.ElideRight
            font.pixelSize: role.size
            font.weight: st === "value" ? Font.Medium : role.weight
            font.family: st === "eyebrow" || st === "value" ? Theme.mono : Theme.sans
            font.capitalization: st === "eyebrow" ? Font.AllUppercase : Font.MixedCase
            font.letterSpacing: st === "eyebrow" ? role.spacing : 0
            color: root.shown.tone ? Theme.toneFg(root.shown.tone, root.dark)
                   : st === "eyebrow" ? Theme.blockColor("textFaint", root.dark)
                   : st === "meta" ? Theme.blockColor("textMeta", root.dark)
                   : st === "body" ? Theme.blockColor("textSecondary", root.dark) : Theme.blockColor("text", root.dark)
        }
    }
    Component {
        id: chipC
        PluginFocusable {
            id: chip
            readonly property string tone: root.shown.tone || "neutral"
            readonly property bool picked: root.shown.selected === true
            dark: root.dark
            actionable: !!root.shown.action
            implicitHeight: Theme.row28
            implicitWidth: chipLabel.implicitWidth + 2 * Theme.space15
            onActivated: root.action(root.shown.action, root.shown.value)
            // Same look as FilterChip: rule-control frame, the picked chip is a neutral ink plane (yellow is for acting); a tone only colours frame and text.
            Rectangle {
                anchors.fill: parent
                color: chip.picked ? (chip.pressed ? Theme.tok("ink2", root.dark) : Theme.tok("ink", root.dark)) : (chip.hovered ? Theme.blockColor("hover", root.dark) : "transparent")
                border.width: 1
                border.color: chip.picked ? Theme.tok("ink", root.dark)
                              : (chip.tone === "neutral" ? (chip.hovered ? Theme.tok("ink2", root.dark) : Theme.blockColor("borderButton", root.dark)) : Theme.toneLine(chip.tone, root.dark))
                Behavior on color { ColorAnimation { duration: Theme.durFast } }
            }
            Text {
                textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                id: chipLabel
                objectName: "pluginChipLabel"
                anchors.centerIn: parent
                width: Math.min(implicitWidth, Math.max(0, parent.width - 2 * Theme.space15))
                elide: Text.ElideRight
                text: root.shown.text || ""
                font.pixelSize: Theme.roles.small.size
                font.weight: chip.picked ? Font.Medium : Font.Normal
                color: chip.picked ? Theme.tok("ground", root.dark)
                       : (chip.tone === "neutral" ? Theme.blockColor("text", root.dark) : Theme.toneFg(chip.tone, root.dark))
            }
        }
    }
    Component {
        id: badgeC
        UzTag {
            text: root.shown.text || ""
            tone: ({warning: "accent", danger: "danger", ok: "ok"})[root.shown.tone || "neutral"] || "neutral"
            icon: "none"
        }
    }
    Component {
        id: avatarC
        UzAvatar {
            name: (root.shown.initial || "").substring(0, 1)
            person: root.shown.color || 1
            size: root.shown.size === "s" ? 20 : (root.shown.size === "l" ? 40 : 28)
        }
    }
    Component {
        id: imageC
        RowLayout {
            spacing: Theme.space1
            PluginImage {
                node: root.shown
                dark: root.dark
                pluginId: root.pluginId
            }
            Text {
                textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                visible: !!root.shown.label
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                text: root.shown.label || ""
                elide: Text.ElideRight
                font.pixelSize: Theme.roles.small.size
                color: Theme.blockColor("textMuted", root.dark)
            }
        }
    }
    Component {
        id: barC
        ColumnLayout {
            readonly property real v: Math.max(0, Math.min(1, root.shown.value || 0))
            // A thin Progress (Yamabuki fill); only an explicit tone, or a nearly empty bar, changes the colour.
            readonly property string tone: root.shown.tone || (v < 0.2 ? "danger" : "")
            spacing: Theme.space05
            Text {
                textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                visible: !!root.shown.label
                text: root.shown.label || ""
                font.pixelSize: Theme.roles.small.size
                color: Theme.blockColor("textMuted", root.dark)
            }
            Rectangle {
                objectName: "pluginBarTrack"
                Layout.fillWidth: true
                implicitWidth: 80
                implicitHeight: 2
                color: Theme.blockColor("track", root.dark)
                Rectangle {
                    objectName: "pluginBarFill"
                    width: parent.width * parent.parent.v
                    height: parent.height
                    color: parent.parent.tone === "" ? Theme.blockColor("accent", root.dark) : Theme.toneFg(parent.parent.tone, root.dark)
                }
            }
        }
    }
    Component {
        id: statC
        PluginFocusable {
            id: stat
            dark: root.dark
            actionable: !!root.shown.action
            implicitHeight: Theme.row32
            implicitWidth: statLabel.implicitWidth + statValue.implicitWidth + 5 * Theme.space1
            onActivated: root.action(root.shown.action, root.shown.value)
            Rectangle {
                anchors.fill: parent
                color: stat.hovered ? Theme.blockColor("hover", root.dark) : "transparent"
            }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.space1
                anchors.rightMargin: Theme.space1
                spacing: Theme.space15
                Text {
                    textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                    id: statLabel
                    Layout.fillWidth: true
                    text: root.shown.label || ""
                    elide: Text.ElideRight
                    font.pixelSize: Theme.roles.input.size
                    color: Theme.blockColor("textMuted", root.dark)
                }
                Text {
                    textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                    id: statValue
                    text: root.shown.value === undefined ? "" : String(root.shown.value)
                    font.family: Theme.mono
                    font.pixelSize: Theme.roles.meta11.size
                    font.weight: Font.Medium
                    color: Theme.blockColor("text", root.dark)
                }
                Text {
                    textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                    visible: !!root.shown.action
                    text: "›"
                    font.pixelSize: Theme.roles.headerTitle.size
                    color: Theme.blockColor("textFaint", root.dark)
                }
            }
        }
    }

    // ---- blocks with their own file
    Component { id: listC; PluginList { node: root.shown; dark: root.dark; pluginId: root.pluginId; onAction: (a, v) => root.action(a, v) } }
    Component { id: tableC; PluginTable { node: root.shown; dark: root.dark; pluginId: root.pluginId; onAction: (a, v) => root.action(a, v) } }
    Component { id: buttonC; PluginButton { node: root.shown; dark: root.dark; pluginId: root.pluginId; onAction: (a, v) => root.action(a, v) } }
    Component { id: toggleC; PluginToggle { node: root.shown; dark: root.dark; pluginId: root.pluginId; onAction: (a, v) => root.action(a, v) } }
    Component { id: selectC; PluginSelect { node: root.shown; dark: root.dark; pluginId: root.pluginId; onAction: (a, v) => root.action(a, v) } }
    Component { id: segmentedC; PluginSegmented { node: root.shown; dark: root.dark; pluginId: root.pluginId; onAction: (a, v) => root.action(a, v) } }
    Component { id: textareaC; PluginTextArea { node: root.shown; dark: root.dark; pluginId: root.pluginId; onAction: (a, v) => root.action(a, v) } }
    Component { id: textfieldC; PluginTextField { node: root.shown; dark: root.dark; pluginId: root.pluginId; onAction: (a, v) => root.action(a, v) } }
    Component { id: mapC; PluginMapView { node: root.shown; dark: root.dark; pluginId: root.pluginId; onAction: (a, v) => root.action(a, v) } }
}
