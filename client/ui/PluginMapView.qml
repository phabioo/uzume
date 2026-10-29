import QtQuick
import QtQuick.Layouts

// map block (docs/design/legacy/plugins.md "Map view"): background image of any aspect fit inside the view (contain), markers in image
// coordinates, zoom and pan, clusters per `area` below 60 % zoom, popover above the selected marker, filter segment,
// collapsible legend. Emits action(action, {kind: "route"|"player", id}) on selection and
// action(action, {kind: "filter", value}) on a filter change. Gamepad hooks: zoomIn, zoomOut, fit, panBy, moveFocus.
Item {
    id: root
    property var node: ({})
    property bool dark: true
    property string pluginId: ""
    signal action(string action, var value)

    readonly property string imageSrc: node.image || ""
    readonly property real iw: Math.max(1, node.image_w || 1)
    readonly property real ih: Math.max(1, node.image_h || 1)
    readonly property var players: node.players || []
    readonly property var routes: node.routes || []
    readonly property var popoverRows: node.popover || []

    // Local state that follows the node again whenever the plugin sends a new value.
    property string localFilter: ""
    readonly property string nodeFilter: node.filter || "all"
    onNodeFilterChanged: localFilter = ""
    readonly property string flt: localFilter !== "" ? localFilter : nodeFilter
    property string localSel: ""
    property string dismissedId: ""
    readonly property string nodeSel: node.selected === undefined || node.selected === null ? "" : String(node.selected)
    onNodeSelChanged: { localSel = ""; dismissedId = "" }
    readonly property string selId: localSel !== "" ? localSel : nodeSel
    readonly property bool popoverOpen: selId !== "" && dismissedId !== selId && popoverRows.length > 0
    property bool legendOpen: node.legend === true   // collapsed by default so it does not cover markers (P47)

    // View transform: zoomPct 1 = fit (whole image visible, centered; limited by width or height, whichever is tighter); `s` is screen pixels per image pixel.
    readonly property real minPct: 0.25
    readonly property real maxPct: 4
    property real zoomPct: 1
    property real ox: 0
    property real oy: 0
    property bool userMoved: false
    readonly property real fitScale: (view.width > 0 && view.height > 0) ? Math.min(view.width / iw, view.height / ih) : 1
    readonly property real s: fitScale * zoomPct
    readonly property bool clustered: zoomPct < 0.6

    implicitWidth: 720
    implicitHeight: 440
    clip: true   // nothing of the map or its controls is drawn outside the block
    // Narrow side panel (280 to 340 px): short filter labels; the zoom buttons wrap below the filter when both do not fit.
    readonly property bool compactBar: width < 340
    readonly property bool wrapBar: width < filterBar.width + zoomBar.width + 30

    function clampPan() {
        const cw = iw * s, ch = ih * s
        ox = cw <= view.width ? (view.width - cw) / 2 : Math.max(view.width - cw, Math.min(0, ox))
        oy = ch <= view.height ? (view.height - ch) / 2 : Math.max(view.height - ch, Math.min(0, oy))
    }
    function fit() {
        userMoved = false
        zoomPct = 1
        ox = 0
        oy = 0
        clampPan()
    }
    function zoomAt(pct, cx, cy) {
        const np = Math.max(minPct, Math.min(maxPct, pct))
        const ratio = np / zoomPct
        ox = cx - (cx - ox) * ratio
        oy = cy - (cy - oy) * ratio
        zoomPct = np
        userMoved = true
        clampPan()
    }
    function zoomIn() { zoomAt(zoomPct * 1.25, view.width / 2, view.height / 2) }
    function zoomOut() { zoomAt(zoomPct / 1.25, view.width / 2, view.height / 2) }
    function panBy(dx, dy) {
        ox += dx
        oy += dy
        userMoved = true
        clampPan()
    }
    function centerOn(x, y, pct) {
        zoomPct = Math.max(minPct, Math.min(maxPct, pct))
        ox = view.width / 2 - x * s
        oy = view.height / 2 - y * s
        userMoved = true
        clampPan()
    }
    function selectMarker(kind, id) {
        localSel = String(id)
        dismissedId = ""
        root.action(node.action || "", {kind: kind, id: id})
    }
    function closePopover() { dismissedId = selId }

    // ---- data shown after the filter
    readonly property int meColor: {
        for (let i = 0; i < players.length; ++i) {
            if (players[i].id === node.me) return players[i].color || 0
        }
        return 0
    }
    function ownedByMe(r) {
        const ic = r.icons || []
        for (let i = 0; i < ic.length; ++i) {
            if (ic[i].owner === meColor) return true
        }
        return false
    }
    readonly property var shownRoutes: flt === "me" ? routes.filter(r => ownedByMe(r)) : routes
    readonly property var singleRoutes: clustered ? shownRoutes.filter(r => !r.area) : shownRoutes
    readonly property var shownPlayers: {
        const list = flt === "me" && node.me !== undefined ? players.filter(p => p.id === node.me) : players
        const seen = {}
        const out = []
        for (let i = 0; i < list.length; ++i) {
            const key = Math.round(list[i].x) + "," + Math.round(list[i].y)
            seen[key] = (seen[key] || []).concat([i])
        }
        for (let i = 0; i < list.length; ++i) {
            const group = seen[Math.round(list[i].x) + "," + Math.round(list[i].y)]
            const k = group.indexOf(i)
            out.push({p: list[i], dx: (k - (group.length - 1) / 2) * 14})
        }
        return out
    }
    readonly property var clusterList: {
        if (!clustered) return []
        const byArea = {}
        const order = []
        for (let i = 0; i < shownRoutes.length; ++i) {
            const r = shownRoutes[i]
            if (!r.area) continue
            let c = byArea[r.area]
            if (!c) {
                c = {id: "cluster:" + r.area, area: r.area, n: 0, x: 0, y: 0, icon: "", owner: 0}
                byArea[r.area] = c
                order.push(c)
            }
            c.n++
            c.x += r.x
            c.y += r.y
            const ic = r.icons || []
            if (ic.length > 0) {
                c.icon = ic[ic.length - 1].src
                c.owner = ic[ic.length - 1].owner
            }
        }
        for (let i = 0; i < order.length; ++i) {
            order[i].x /= order[i].n
            order[i].y /= order[i].n
        }
        return order
    }
    // Screen anchor of the selected marker (center of a route chip, middle of a player avatar).
    readonly property var selAnchor: {
        const id = selId
        for (let i = 0; i < shownRoutes.length; ++i) {
            if (String(shownRoutes[i].id) === id && !(clustered && shownRoutes[i].area)) return {x: ox + shownRoutes[i].x * s, y: oy + shownRoutes[i].y * s}
        }
        for (let i = 0; i < shownPlayers.length; ++i) {
            if (String(shownPlayers[i].p.id) === id) return {x: ox + shownPlayers[i].p.x * s + shownPlayers[i].dx, y: oy + shownPlayers[i].p.y * s - 20}
        }
        return null
    }

    // ---- keyboard / controller navigation
    function allMarkers() {
        const out = []
        for (const rep of [playerRep, routeRep, clusterRep]) {
            for (let i = 0; i < rep.count; ++i) {
                const it = rep.itemAt(i)
                if (it) out.push(it)
            }
        }
        return out
    }
    function moveFocus(dx, dy) {
        const all = allMarkers()
        let cur = null
        for (const m of all) {
            if (m.activeFocus) cur = m
        }
        const fx = cur ? cur.x + cur.width / 2 : view.width / 2
        const fy = cur ? cur.y + cur.height / 2 : view.height / 2
        let best = null
        let bestScore = 1e18
        for (const m of all) {
            if (m === cur) continue
            const vx = m.x + m.width / 2 - fx
            const vy = m.y + m.height / 2 - fy
            const along = vx * dx + vy * dy
            if (along <= 1) continue
            const perp = Math.abs(vx * dy - vy * dx)
            const score = along + 2 * perp
            if (score < bestScore) { bestScore = score; best = m }
        }
        if (!best) return false
        best.forceActiveFocus()
        const cx = best.x + best.width / 2, cy = best.y + best.height / 2
        if (cx < 24 || cx > view.width - 24 || cy < 24 || cy > view.height - 24) panBy(view.width / 2 - cx, view.height / 2 - cy)
        return true
    }

    // ---- surface
    Rectangle {
        anchors.fill: parent
        color: Theme.blockColor("mapWater", root.dark)
        border.width: 1
        border.color: Theme.blockColor("cardBorder", root.dark)
    }

    Item {
        id: view
        objectName: "pluginMapView"
        anchors.fill: parent
        anchors.margins: 1
        clip: true
        activeFocusOnTab: root.imageSrc !== ""
        onWidthChanged: root.userMoved ? root.clampPan() : root.fit()
        onHeightChanged: root.userMoved ? root.clampPan() : root.fit()
        Component.onCompleted: root.fit()

        Keys.onPressed: (event) => {
            switch (event.key) {
            case Qt.Key_Left: event.accepted = root.moveFocus(-1, 0) || true; break
            case Qt.Key_Right: event.accepted = root.moveFocus(1, 0) || true; break
            case Qt.Key_Up: event.accepted = root.moveFocus(0, -1) || true; break
            case Qt.Key_Down: event.accepted = root.moveFocus(0, 1) || true; break
            case Qt.Key_Plus: case Qt.Key_Equal: root.zoomIn(); event.accepted = true; break
            case Qt.Key_Minus: root.zoomOut(); event.accepted = true; break
            case Qt.Key_0: root.fit(); event.accepted = true; break
            case Qt.Key_Escape:
                if (root.popoverOpen) { root.closePopover(); event.accepted = true }
                break
            }
        }

        Rectangle {  // placeholder land while the image is missing or still loading
            visible: mapImage.status !== Image.Ready && root.imageSrc !== ""
            x: root.ox
            y: root.oy
            width: root.iw * root.s
            height: root.ih * root.s
            color: Theme.blockColor("mapLand", root.dark)
        }
        Image {
            id: mapImage
            objectName: "pluginMapImage"
            x: root.ox
            y: root.oy
            width: root.iw * root.s
            height: root.ih * root.s
            source: Theme.pluginImageUrl(root.pluginId, root.imageSrc)
            sourceSize: Qt.size(2048, 2048)  // bounds the decoded map (aspect kept by the provider)
            smooth: false
            asynchronous: true
            fillMode: Image.Stretch
        }

        WheelHandler {
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            onWheel: (event) => root.zoomAt(root.zoomPct * Math.pow(1.0015, event.angleDelta.y), event.x, event.y)
        }
        DragHandler {
            id: drag
            target: null
            property real startX: 0
            property real startY: 0
            onActiveChanged: if (active) { startX = root.ox; startY = root.oy }
            onTranslationChanged: if (active) {
                root.ox = startX + translation.x
                root.oy = startY + translation.y
                root.userMoved = true
                root.clampPan()
            }
        }
        TapHandler {  // click on the empty map closes the popover
            onTapped: { view.forceActiveFocus(); if (root.popoverOpen) root.closePopover() }
        }

        // ---- markers
        Repeater {
            id: clusterRep
            model: root.clusterList
            delegate: PluginFocusable {
                id: cm
                required property var modelData
                objectName: "pluginMapCluster_" + modelData.area
                readonly property string markerId: modelData.id
                dark: root.dark
                implicitHeight: 30
                implicitWidth: clusterRow.implicitWidth + 20
                width: implicitWidth
                height: implicitHeight
                x: root.ox + modelData.x * root.s - width / 2
                y: root.oy + modelData.y * root.s - height / 2
                onActivated: root.centerOn(modelData.x, modelData.y, 0.9)
                Rectangle {
                    anchors.fill: parent
                    radius: 0
                    color: Theme.blockColor("popup", root.dark)
                    border.width: 1
                    border.color: cm.hovered ? Theme.blockColor("accent", root.dark) : Theme.blockColor("popupBorder", root.dark)
                }
                Row {
                    id: clusterRow
                    anchors.centerIn: parent
                    spacing: 6
                    PluginImage {
                        visible: !!cm.modelData.icon
                        anchors.verticalCenter: parent.verticalCenter
                        dark: root.dark
                        pluginId: root.pluginId
                        src: cm.modelData.icon
                        size: "s"
                        owner: cm.modelData.owner || 0
                    }
                    Text {
                        textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                        anchors.verticalCenter: parent.verticalCenter
                        text: cm.modelData.n
                        font.pixelSize: Theme.roles.small.size
                        font.weight: Font.DemiBold
                        color: Theme.blockColor("text", root.dark)
                    }
                    Text {
                        textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                        anchors.verticalCenter: parent.verticalCenter
                        text: cm.modelData.area
                        font.pixelSize: 10
                        font.weight: Font.DemiBold
                        color: Theme.blockColor("textMuted", root.dark)
                    }
                }
            }
        }
        Repeater {
            id: routeRep
            model: root.singleRoutes
            delegate: PluginFocusable {
                id: rm
                required property var modelData
                objectName: "pluginMapRoute_" + modelData.id
                readonly property string markerId: String(modelData.id)
                readonly property var icons: modelData.icons || []
                readonly property bool selected: root.selId === markerId
                dark: root.dark
                implicitHeight: 28
                implicitWidth: routeRow.implicitWidth + 16
                width: implicitWidth
                height: implicitHeight
                x: root.ox + modelData.x * root.s - width / 2
                y: root.oy + modelData.y * root.s - height / 2
                opacity: root.flt === "open" && modelData.done === true ? 0.35 : 1
                onActivated: root.selectMarker("route", modelData.id)
                Rectangle {
                    anchors.fill: parent
                    radius: 0
                    color: Theme.blockColor("popup", root.dark)
                    border.width: rm.selected ? 2 : 1
                    border.color: rm.selected ? Theme.blockColor("accent", root.dark) : Theme.blockColor("popupBorder", root.dark)
                }
                Row {
                    id: routeRow
                    anchors.centerIn: parent
                    spacing: 5
                    Repeater {
                        model: rm.icons
                        delegate: PluginImage {
                            required property var modelData
                            anchors.verticalCenter: parent.verticalCenter
                            dark: root.dark
                            pluginId: root.pluginId
                            src: modelData.src || ""
                            size: "s"
                            owner: modelData.owner || 0
                        }
                    }
                    Text {
                        textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                        visible: rm.modelData.number !== undefined
                        anchors.verticalCenter: parent.verticalCenter
                        text: rm.modelData.number === undefined ? "" : String(rm.modelData.number)
                        font.family: Theme.mono
                        font.pixelSize: Theme.roles.small.size
                        font.weight: Font.DemiBold
                        color: Theme.blockColor("textSecondary", root.dark)
                    }
                    Text {
                        textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                        objectName: "pluginMapOpenLabel"
                        visible: rm.icons.length === 0
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("open")
                        font.pixelSize: Theme.roles.small.size
                        color: Theme.blockColor("accent", root.dark)
                    }
                }
            }
        }
        Repeater {
            id: playerRep
            model: root.shownPlayers
            delegate: PluginFocusable {
                id: pm
                required property var modelData
                objectName: "pluginMapPlayer_" + modelData.p.id
                readonly property string markerId: String(modelData.p.id)
                readonly property bool selected: root.selId === markerId
                dark: root.dark
                implicitWidth: 34
                implicitHeight: 40
                width: implicitWidth
                height: implicitHeight
                x: root.ox + modelData.p.x * root.s + modelData.dx - width / 2
                y: root.oy + modelData.p.y * root.s - height
                onActivated: root.selectMarker("player", modelData.p.id)
                Rectangle {  // pointer
                    width: 9
                    height: 9
                    rotation: 45
                    x: (parent.width - width) / 2
                    y: 26
                    color: Theme.playerColor(pm.modelData.p.color || 1, root.dark)
                }
                Rectangle {
                    x: 2
                    y: 0
                    width: 30
                    height: 30
                    radius: 0
                    color: Theme.blockColor("mapWater", root.dark)
                    border.width: pm.selected ? 2 : 0
                    border.color: Theme.blockColor("accent", root.dark)
                    Rectangle {
                        anchors.centerIn: parent
                        width: 26
                        height: 26
                        radius: 0
                        color: Theme.playerColor(pm.modelData.p.color || 1, root.dark)
                        Text {
                            textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                            anchors.centerIn: parent
                            text: (pm.modelData.p.initial || "").substring(0, 1).toUpperCase()
                            font.pixelSize: 12
                            font.weight: Font.DemiBold
                            color: Theme.tok("ground", root.dark)
                        }
                    }
                }
            }
        }

        // ---- popover above the selected marker; flips below near the top edge
        Rectangle {
            id: pop
            objectName: "pluginMapPopover"
            readonly property var a: root.selAnchor
            readonly property bool flipped: a ? a.y - 40 - height < 8 : false
            visible: root.popoverOpen && a !== null
            z: 20
            width: Math.min(340, Math.max(160, view.width - 16))
            height: popCol.implicitHeight + 24
            x: a ? Math.max(8, Math.min(view.width - width - 8, a.x - width / 2)) : 0
            y: a ? (flipped ? a.y + 40 : a.y - 40 - height) : 0
            radius: 0
            color: Theme.blockColor("popup", root.dark)
            border.width: 1
            border.color: Theme.blockColor("popupBorder", root.dark)
            MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onWheel: (w) => w.accepted = true }
            Rectangle {  // pointer
                width: 12
                height: 12
                rotation: 45
                x: pop.a ? Math.max(14, Math.min(pop.width - 26, pop.a.x - pop.x - 6)) : 0
                y: pop.flipped ? -6 : pop.height - 6
                z: -1
                color: Theme.blockColor("popup", root.dark)
                border.width: 1
                border.color: Theme.blockColor("popupBorder", root.dark)
            }
            ColumnLayout {
                id: popCol
                anchors.fill: parent
                anchors.margins: 12
                spacing: 8
                PluginPool {
                    count: root.popoverRows.length
                    delegate: RowLayout {
                        required property int index
                        readonly property var modelData: root.popoverRows[index] || ({})
                        visible: index < root.popoverRows.length   // pooled (PluginPool)
                        Layout.fillWidth: true
                        spacing: 8
                        Rectangle {
                            Layout.preferredWidth: 8
                            Layout.preferredHeight: 8
                            radius: 0
                            color: Theme.playerColor(modelData.owner || 1, root.dark)
                        }
                        Text {
                            textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                            Layout.preferredWidth: 56
                            text: modelData.name || ""
                            elide: Text.ElideRight
                            font.pixelSize: Theme.roles.small.size
                            font.weight: Font.Medium
                            color: Theme.blockColor("text", root.dark)
                        }
                        PluginImage {
                            dark: root.dark
                            pluginId: root.pluginId
                            src: modelData.src || ""
                            size: "s"
                            imgState: modelData.src ? "alive" : "empty"
                        }
                        Text {
                            textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                            Layout.fillWidth: true
                            text: modelData.status || ""
                            wrapMode: Text.WordWrap     // shown in full, not cut off (P47)
                            maximumLineCount: 3
                            font.pixelSize: Theme.roles.small.size
                            color: Theme.blockColor("textMuted", root.dark)
                        }
                    }
                }
            }
        }

        // ---- controls
        PluginSegmented {
            id: filterBar
            objectName: "pluginMapFilter"
            visible: root.imageSrc !== ""
            x: 10
            y: 10
            z: 30
            dark: root.dark
            implicitHeight: 32
            node: ({options: [{value: "all", label: qsTr("All")}, {value: "me", label: root.compactBar ? qsTr("Me") : qsTr("Only me")}, {value: "open", label: root.compactBar ? qsTr("Open") : qsTr("Open routes")}],
                    selected: root.flt, action: "filter"})
            onAction: (a, v) => { root.localFilter = v; root.action(root.node.action || "", {kind: "filter", value: v}) }
        }
        Row {
            id: zoomBar
            objectName: "pluginMapZoomBar"
            visible: root.imageSrc !== ""
            anchors.right: parent.right
            anchors.rightMargin: 10
            y: root.wrapBar ? 10 + filterBar.height + 6 : 10
            z: 30
            spacing: 4
            Repeater {
                model: [{name: "pluginMapZoomIn", label: "+", tip: 1}, {name: "pluginMapZoomOut", label: "−", tip: 2}, {name: "pluginMapFit", label: "Fit", tip: 3}]
                delegate: PluginFocusable {
                    id: zb
                    required property var modelData
                    objectName: modelData.name
                    dark: root.dark
                    implicitWidth: 32
                    implicitHeight: 32
                    width: 32
                    height: 32
                    onActivated: modelData.tip === 1 ? root.zoomIn() : (modelData.tip === 2 ? root.zoomOut() : root.fit())
                    Rectangle {
                        anchors.fill: parent
                        color: zb.hovered ? Theme.blockColor("hover", root.dark) : Theme.blockColor("popup", root.dark)
                        border.width: 1
                        border.color: Theme.blockColor("popupBorder", root.dark)
                        Text {
                            textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                            anchors.centerIn: parent
                            text: zb.modelData.label
                            font.pixelSize: 16
                            color: Theme.blockColor("text", root.dark)
                        }
                    }
                }
            }
        }
        Rectangle {
            id: legend
            objectName: "pluginMapLegend"
            visible: root.imageSrc !== ""
            x: 10
            y: parent.height - height - 10
            z: 30
            width: legendCol.implicitWidth + 20
            height: legendCol.implicitHeight + 16
            color: Theme.blockColor("popup", root.dark)
            border.width: 1
            border.color: Theme.blockColor("popupBorder", root.dark)
            ColumnLayout {
                id: legendCol
                x: 10
                y: 8
                spacing: 6
                PluginFocusable {
                    objectName: "pluginMapLegendToggle"
                    dark: root.dark
                    implicitWidth: legendHead.implicitWidth
                    implicitHeight: 18
                    onActivated: root.legendOpen = !root.legendOpen
                    Text {
                        textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                        id: legendHead
                        text: (root.legendOpen ? "▾ " : "▸ ") + qsTr("Legend")
                        font.pixelSize: Theme.roles.small.size
                        font.weight: Font.DemiBold
                        color: Theme.blockColor("textMuted", root.dark)
                    }
                }
                RowLayout {
                    visible: root.legendOpen
                    spacing: 8
                    Rectangle { width: 8; height: 8; radius: 0; color: Theme.playerColor(1, root.dark) }
                    Text { textFormat: Text.PlainText; text: qsTr("Caught · dot = owner"); font.pixelSize: Theme.roles.small.size; color: Theme.blockColor("textSecondary", root.dark) }
                }
                RowLayout {
                    visible: root.legendOpen
                    spacing: 8
                    PluginImage { dark: root.dark; size: "s"; imgState: "dead" }
                    Text { textFormat: Text.PlainText; text: qsTr("Dead"); font.pixelSize: Theme.roles.small.size; color: Theme.blockColor("textSecondary", root.dark) }
                }
                RowLayout {
                    visible: root.legendOpen
                    spacing: 8
                    PluginImage { dark: root.dark; size: "s"; imgState: "missed" }
                    Text { textFormat: Text.PlainText; text: qsTr("Missed"); font.pixelSize: Theme.roles.small.size; color: Theme.blockColor("textSecondary", root.dark) }
                }
            }
        }

        Column {
            objectName: "pluginMapLoading"
            visible: root.imageSrc === ""
            anchors.centerIn: parent
            spacing: 6
            Text {
                textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                anchors.horizontalCenter: parent.horizontalCenter
                text: qsTr("Map not read yet")
                font.pixelSize: Theme.roles.input.size
                font.weight: Font.Medium
                color: Theme.blockColor("textMuted", root.dark)
            }
            Text {
                textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                anchors.horizontalCenter: parent.horizontalCenter
                text: qsTr("It appears once the game has loaded it.")
                font.pixelSize: Theme.roles.small.size
                color: Theme.blockColor("textFaint", root.dark)
            }
        }
    }
}
