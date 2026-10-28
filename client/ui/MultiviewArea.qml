import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// Multiview with up to four surfaces (boards 3r, ADR 0012 D8, ADR 0026 D2): the local game and/or remote Sessions. Layouts:
// a single surface fills the area; Row (two or three Sessions side by side, equal columns); Grid 2 x 2 (from four Sessions,
// never an empty slot); PiP: one main tile and the others as windows inside the main tile (a corner of the picture, S | M | L,
// dragged to snap to a corner). Area padding 24, tile gap 24. A tile is a MultiviewTile: a 1 px `rule` frame, the 40 x 3 px person
// rule at its top left, the `stage` with the picture and a 40 px bar with the person square, the name, the system chip and game
// name and a right-side status (YOU, PAUSED BY <NAME>, RELAYED, ...). The tile whose audio you hear has the Yamabuki frame (2 px
// in total) and the AUDIO tag; the selected tile (click or keys 1-4, the side panel follows) has an `ink-2` frame. A remote
// selection does not route input: input always goes to your game. A paused remote game is dimmed and says "Paused by <name>"
// (ADR 0026 D3). The screen layout of the game applies to every tile. Every surface is one delegate that is only moved when the
// layout changes, so the local game view is not recreated. The "Add tile" popover lives in the game screen.
Item {
    id: root
    objectName: "multiviewArea"
    required property PlayerController player
    readonly property SessionController ctl: player.sessions
    readonly property string layoutMode: ctl.surfaceCount < 2 ? "single" : ctl.multiviewMode   // single | pip | row | grid
    readonly property var order: ctl.surfaceOrder             // display order, first = main surface
    readonly property int count: ctl.surfaceCount
    readonly property bool tiled: layoutMode === "row" || layoutMode === "grid"
    readonly property string selected: ctl.selectedSurface
    signal localReady(Item view)
    signal escapePressed()
    signal pickerRequested()
    signal tileSelected()

    // Layout metrics
    readonly property real pad: Theme.space3
    readonly property real gap: Theme.space3
    readonly property int cols: layoutMode === "row" ? Math.max(1, count) : 2
    readonly property real cellW: Math.max(0, (width - 2 * pad - (cols - 1) * gap) / cols)
    readonly property real cellH: layoutMode === "grid" ? Math.max(0, (height - 2 * pad - gap) / 2) : Math.max(0, height - 2 * pad)
    // PiP: the window sits inside the main tile's stage, 16 px clear of its frame; corner and size are the user's
    property string pipCorner: "br"            // "br" | "bl" | "tr" | "tl"
    property string pipSize: "M"               // "S" | "M" | "L"
    property bool pipDragging: false
    readonly property real pipInset: pad + 1 + Theme.space2
    readonly property real pipWidth: pipSize === "S" ? 176 : pipSize === "L" ? 320 : 240
    readonly property real pipChrome: 70       // controls row + caption row around the picture
    readonly property real pipSpacing: 8
    readonly property int pipTiles: Math.max(1, count - 1)
    readonly property real pipViewHeight: Math.max(60, Math.min(pipWidth * 1.35, (height - 2 * pipInset - 40 - pipTiles * pipChrome - (pipTiles - 1) * pipSpacing) / pipTiles))
    readonly property real pipTileHeight: pipChrome + pipViewHeight
    function snapCorner(cx, cy) { return (cy < height / 2 ? "t" : "b") + (cx < width / 2 ? "l" : "r") }

    // Plugin tile strips: the visible remote tiles go to the plugins (fb "tiles" event); the strips come back per tile.
    readonly property var remoteTiles: {
        if (!root.visible) return []
        var o = root.ctl.surfaceOrder, info = root.ctl.surfaceInfo, r = []
        for (var i = 0; i < o.length; ++i) {
            if (o[i] === "local") continue
            var si = info[o[i]] || ({})
            r.push({ id: o[i], player: si.who || "", game: si.game || "", user_id: si.userId || "" })
        }
        return r
    }
    onRemoteTilesChanged: root.player.plugins.setTiles(root.remoteTiles)
    property var stripChoice: ({})                 // tile id -> chosen plugin id (several plugins offer a strip)
    PluginMenu {
        id: stripMenu
        property string tileId: ""
        onCloseRequested: open = false
        onPicked: (id) => {
            open = false
            var c = Object.assign({}, root.stripChoice)
            c[stripMenu.tileId] = id
            root.stripChoice = c
        }
    }

    // System code of your own game (the selected game is the running one while the game view is shown)
    readonly property string localSystem: {
        const g = root.player.selectedGame
        return g && g.title === root.ctl.localTitle && g.system ? String(g.system) : ""
    }
    readonly property string ownWho: root.ctl.ownName !== "" ? root.ctl.ownName : qsTr("You")

    // A note on a tile's picture: "Paused by Lena" over the frozen picture (title, sub; `compact` in a PiP window)
    component TileNote: Rectangle {
        id: note
        property string title: ""
        property string sub: ""
        property bool compact: false
        readonly property real maxNoteWidth: Math.max(120, note.parent ? note.parent.width - 16 : 400)
        implicitWidth: Math.min(maxNoteWidth, Math.max(titleText.implicitWidth, subText.visible ? subText.implicitWidth : 0) + 2 * Theme.space2)
        implicitHeight: noteCol.implicitHeight + (compact ? Theme.space1 * 1.5 : Theme.space2)
        radius: 0
        color: Theme.surface
        border.width: 1
        border.color: Theme.rule
        Column {
            id: noteCol
            anchors.centerIn: parent
            width: note.width - 2 * Theme.space2
            spacing: 2
            Text {
                id: titleText
                objectName: "tileCardTitle"
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                maximumLineCount: note.compact ? 1 : 2
                elide: Text.ElideRight
                textFormat: Text.PlainText
                text: note.title
                font.family: Theme.sans
                font.pixelSize: note.compact ? Theme.roles.small.size : Theme.roles.itemTitle.size
                font.weight: Font.Medium
                color: Theme.ink
            }
            Text {
                id: subText
                objectName: "tileCardSub"
                visible: note.sub !== "" && !note.compact
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                maximumLineCount: 2
                elide: Text.ElideRight
                textFormat: Text.PlainText
                text: note.sub
                font.family: Theme.sans
                font.pixelSize: Theme.roles.small.size
                color: Theme.muted
            }
        }
    }

    // One entry per surface, synced incrementally: adding or removing a Session must not recreate the other
    // surfaces (above all not the local game view).
    ListModel { id: surfaceModel }
    function syncSurfaces() {
        var ids = root.ctl.surfaceIds
        for (var i = surfaceModel.count - 1; i >= 0; --i) {
            if (ids.indexOf(surfaceModel.get(i).sid) < 0) surfaceModel.remove(i)
        }
        for (var j = 0; j < ids.length; ++j) {
            var found = false
            for (var k = 0; k < surfaceModel.count; ++k) {
                if (surfaceModel.get(k).sid === ids[j]) { found = true; break }
            }
            if (!found) surfaceModel.append({ sid: ids[j] })
        }
    }
    Component.onCompleted: syncSurfaces()
    Connections {
        target: root.ctl
        function onSurfacesChanged() { root.syncSurfaces() }
    }


    // Corner targets while a PiP window is dragged: dashed 60 x 40 areas in the corners it is not in
    Repeater {
        model: root.layoutMode === "pip" && root.pipDragging ? ["tl", "tr", "bl", "br"] : []
        delegate: Rectangle {
            required property string modelData
            objectName: "pipSnap_" + modelData
            visible: modelData !== root.pipCorner
            x: modelData.charAt(1) === "l" ? root.pipInset : root.width - root.pipInset - width
            y: modelData.charAt(0) === "t" ? root.pipInset : root.height - root.pipInset - height
            width: 60; height: 40
            color: "transparent"
            border.width: 1
            border.color: Theme.dim
            z: 1
        }
    }
    Text {
        objectName: "pipDragHint"
        visible: root.layoutMode === "pip" && root.pipDragging
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: root.pad + 6
        z: 3
        text: qsTr("Drag to snap to a corner · S / M / L size")
        textFormat: Text.PlainText
        font.family: Theme.sans
        font.pixelSize: Theme.roles.small.size
        color: Theme.muted
    }

    Repeater {
        model: surfaceModel
        delegate: Item {
            id: tile
            required property string sid
            readonly property string modelData: sid
            readonly property int slot: root.order.indexOf(modelData)
            readonly property var info: root.ctl.surfaceInfo[modelData] || ({})
            readonly property var link: root.ctl.surfaceLinks[modelData] || null
            readonly property bool isLocal: modelData === "local"
            readonly property bool audible: root.ctl.audibleSurfaces.indexOf(modelData) >= 0
            readonly property bool picked: root.selected === modelData
            readonly property bool pipTile: root.layoutMode === "pip" && slot > 0
            readonly property bool multiple: root.count > 1
            readonly property bool localPaused: isLocal && root.player.gameSession.paused
            readonly property bool remotePaused: !isLocal && info.paused === true
            readonly property bool paused: localPaused || remotePaused
            readonly property bool loading: !isLocal && info.loading === true && !remotePaused
            readonly property real lane: stripLoader.active ? 24 : 0     // row above the picture for the plugin strip
            readonly property string who: isLocal ? root.ownWho : (info.who || "")
            // Voice mark of the person of this tile (speaking bars, mic off, muted by host)
            readonly property string voiceMark: root.player.voice.marks[isLocal ? root.player.voice.selfUserId : (info.userId || "")] || ""
            readonly property color personColor: Theme.playerColor(slot + 1, Theme.dark)
            // Right side of the bar: YOU, PAUSED BY <NAME>, CONNECTING, RELAYED (the AUDIO tag follows it)
            readonly property string status: remotePaused ? qsTr("Paused by %1").arg(info.pausedBy || info.who || "")
                : localPaused ? qsTr("Paused")
                : loading ? qsTr("Connecting")
                : isLocal ? qsTr("You")
                : (link && link.tone !== "ok" ? link.text : "")
            property real dragDx: 0
            property real dragDy: 0
            readonly property bool atTop: root.pipCorner.charAt(0) === "t"
            readonly property bool atLeft: root.pipCorner.charAt(1) === "l"
            readonly property real stagePad: pipTile ? 10 : Theme.space2

            objectName: "surfaceTile_" + modelData
            visible: slot >= 0
            z: pipTile ? 2 : 0
            width: root.tiled ? root.cellW : pipTile ? root.pipWidth : root.width - 2 * root.pad
            height: root.tiled ? root.cellH : pipTile ? root.pipTileHeight : root.height - 2 * root.pad
            x: (root.layoutMode === "row" ? root.pad + Math.max(0, slot) * (root.cellW + root.gap)
              : root.layoutMode === "grid" ? root.pad + (Math.max(0, slot) % 2) * (root.cellW + root.gap)
              : pipTile ? (atLeft ? root.pipInset : root.width - root.pipInset - root.pipWidth)
              : root.pad) + dragDx
            y: (root.layoutMode === "grid" ? root.pad + Math.floor(Math.max(0, slot) / 2) * (root.cellH + root.gap)
              : pipTile ? (atTop ? root.pipInset + (slot - 1) * (root.pipTileHeight + root.pipSpacing)
                               : root.height - root.pipInset - slot * root.pipTileHeight - (slot - 1) * root.pipSpacing)
              : root.pad) + dragDy

            // Frame; the `stage` plane behind the picture (a PiP window sits on the picture, so it has its own `surface`)
            Rectangle {
                id: frame
                anchors.fill: parent
                radius: 0
                color: tile.pipTile ? Theme.surface : "transparent"
                border.width: 1
                border.color: tile.audible && tile.multiple ? Theme.accent : Theme.rule
            }
            Rectangle {
                id: stagePlane
                visible: !tile.pipTile
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.bottom: bar.top
                anchors.margins: 1
                anchors.bottomMargin: 0
                color: tapHover.hovered ? Theme.hover : Theme.stage
            }
            HoverHandler { id: tapHover; cursorShape: Qt.PointingHandCursor }
            TapHandler {
                objectName: "tileTap_" + tile.modelData
                onTapped: { root.ctl.selectSurface(tile.modelData); root.tileSelected() }
            }
            DragHandler {
                id: pipDrag
                enabled: tile.pipTile
                target: null
                onTranslationChanged: { tile.dragDx = translation.x; tile.dragDy = translation.y }
                onActiveChanged: {
                    root.pipDragging = active
                    if (!active) {
                        root.pipCorner = root.snapCorner(tile.x + tile.width / 2, tile.y + tile.height / 2)
                        tile.dragDx = 0
                        tile.dragDy = 0
                    }
                }
            }

            // The picture: local game or the decoded frames of this remote Session.
            Item {
                id: pictureBox
                objectName: "tilePicture_" + tile.modelData
                anchors.fill: parent
                anchors.leftMargin: tile.stagePad + 1
                anchors.rightMargin: tile.stagePad + 1
                anchors.topMargin: (tile.pipTile ? 38 : tile.stagePad + 1) + tile.lane
                anchors.bottomMargin: tile.pipTile ? 32 : 40 + tile.stagePad
                Loader {
                    anchors.fill: parent
                    opacity: tile.remotePaused || tile.loading ? 0.45 : 1
                    sourceComponent: tile.isLocal ? localComp : remoteComp
                }
                // Paused / loading states: remote paused by the host, your own paused game, a Session still connecting
                TileNote {
                    objectName: "tilePausedBanner"
                    visible: tile.paused
                    anchors.centerIn: parent
                    compact: tile.pipTile
                    title: tile.remotePaused ? qsTr("❚❚ Paused by %1").arg(tile.info.pausedBy || tile.info.who || "") : qsTr("❚❚ Paused")
                    sub: tile.remotePaused ? qsTr("Frozen picture · resumes here") : qsTr("Esc or Resume continues")
                }
                // Waiting for a Session that has no picture yet: the ensō, who and what is starting (waiting tile of 3r-5)
                Column {
                    objectName: "tileLoadingCard"
                    visible: tile.loading
                    anchors.centerIn: parent
                    width: Math.min(parent.width, 260)
                    spacing: Theme.space1
                    UzEnso { visible: !tile.pipTile && pictureBox.height > 140; size: 48; anchors.horizontalCenter: parent.horizontalCenter }
                    Text {
                        objectName: "tileCardTitle"
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.Wrap
                        maximumLineCount: 2
                        elide: Text.ElideRight
                        textFormat: Text.PlainText
                        text: qsTr("%1’s game is loading").arg(tile.info.who || "")
                        font.family: Theme.sans
                        font.pixelSize: tile.pipTile ? Theme.roles.small.size : Theme.roles.itemTitle.size
                        font.weight: Font.Medium
                        color: Theme.ink
                    }
                    Text {
                        objectName: "tileCardSub"
                        visible: !tile.pipTile
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        textFormat: Text.PlainText
                        text: qsTr("Connecting…")
                        font.family: Theme.mono
                        font.pixelSize: 10
                        font.letterSpacing: 0.8
                        font.capitalization: Font.AllUppercase
                        color: Theme.dim
                    }
                }
            }
            Component {
                id: localComp
                GameView {
                    session: root.player.gameSession
                    objectName: "gameViewMulti"
                    layout: root.ctl.viewLayout
                    swap: root.ctl.viewSwap
                    strictInteger: root.ctl.integerScale
                    aspectRatio: root.ctl.aspectRatio
                    Component.onCompleted: root.localReady(this)
                    onEscapePressed: root.escapePressed()
                }
            }
            Component {
                id: remoteComp
                RemoteView {
                    objectName: "remoteView"
                    controller: root.ctl
                    control: root.player.control
                    surfaceId: tile.modelData
                    layout: root.ctl.screenLayout
                    swap: root.ctl.screenSwap
                }
            }

            // Audio frame: the second Yamabuki pixel inside the frame (audio-ring), 2 px in total
            Rectangle {
                objectName: "tileAudioFrame_" + tile.modelData
                visible: tile.audible && tile.multiple
                anchors.fill: parent
                anchors.margins: 1
                radius: 0
                color: "transparent"
                border.width: 1
                border.color: Theme.accent
                z: 5
            }
            // Selection: the selected tile (the side panel follows it) has an `ink-2` frame; the audio frame wins
            Rectangle {
                objectName: "tileSelection_" + tile.modelData
                visible: tile.picked && tile.multiple
                anchors.fill: parent
                radius: 0
                color: "transparent"
                border.width: 1
                border.color: tile.audible ? Theme.accent : Theme.ink2
                z: 5
            }
            // The person rule: 40 x 3 px at the top left, always with the name
            Rectangle {
                objectName: "personRule_" + tile.modelData
                x: 0; y: 0
                width: Theme.personMarker; height: 3
                color: tile.personColor
                z: 6
            }

            // Plugin tile strip (remote tiles only): 20 high in its own row above the picture, so it never covers the game image
            property var stripList: []
            function refreshStrips() { tile.stripList = tile.isLocal ? [] : root.player.plugins.tileStrips(tile.modelData) }
            Component.onCompleted: refreshStrips()
            Connections {
                target: root.player.plugins
                function onTileStripsChanged() { tile.refreshStrips() }
            }
            readonly property var strip: {
                var l = tile.stripList, want = root.stripChoice[tile.modelData]
                for (var i = 0; i < l.length; ++i) if (l[i].pluginId === want) return l[i]
                return l.length > 0 ? l[0] : null
            }
            Loader {
                id: stripLoader
                active: !tile.isLocal && tile.strip !== null
                x: tile.stagePad + 1
                y: tile.pipTile ? 38 : tile.stagePad + 1
                z: 6
                sourceComponent: PluginTileStrip {
                    player: root.player
                    tileId: tile.modelData
                    strip: tile.strip
                    maxWidth: Math.max(60, tile.width - stripLoader.x - 12 - (tile.stripList.length > 1 ? 24 : 0))
                }
            }
            Rectangle {
                id: stripMore
                objectName: "tileStripMore_" + tile.modelData
                visible: stripLoader.active && tile.stripList.length > 1 && (tapHover.hovered || moreHover.hovered)
                x: stripLoader.x + (stripLoader.item ? stripLoader.item.width : 0) + 4
                y: stripLoader.y
                z: 6
                width: 20; height: 20; radius: 0
                color: moreHover.hovered ? Theme.pressed : Theme.surface
                border.width: 1
                border.color: Theme.rule
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Choose the strip")
                UzGlyph { anchors.centerIn: parent; name: "more"; size: 14; color: Theme.ink2 }
                HoverHandler { id: moreHover; cursorShape: Qt.PointingHandCursor }
                TapHandler {
                    onTapped: {
                        var items = []
                        for (var i = 0; i < tile.stripList.length; ++i)
                            items.push({ id: tile.stripList[i].pluginId, text: tile.stripList[i].name || tile.stripList[i].pluginId,
                                         checked: tile.strip && tile.strip.pluginId === tile.stripList[i].pluginId })
                        stripMenu.tileId = tile.modelData
                        stripMenu.items = items
                        stripMenu.menuWidth = 220
                        stripMenu.anchorItem = stripMore
                        stripMenu.open = true
                    }
                }
            }

            // Name bar under the picture: key number, person square, name, system chip and game, status, AUDIO tag
            // (not in a PiP window, which has its own rows)
            Item {
                id: bar
                objectName: "tileInfoRow_" + tile.modelData
                visible: !tile.pipTile
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 1
                height: 40
                Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: Theme.rule2 }
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 14
                    anchors.rightMargin: 14
                    spacing: 10
                    UzMono {
                        objectName: "keyBadge_" + tile.modelData
                        visible: tile.multiple
                        text: String(tile.slot + 1)
                        font.pixelSize: 10
                        font.letterSpacing: 0.8
                        color: Theme.dim
                    }
                    Rectangle { Layout.preferredWidth: 8; Layout.preferredHeight: 8; color: tile.personColor }
                    Text {
                        id: chipText
                        objectName: "nameChip_" + tile.modelData
                        Layout.maximumWidth: Math.max(60, bar.width * 0.4)
                        Layout.minimumWidth: 0
                        elide: Text.ElideRight
                        textFormat: Text.PlainText
                        text: tile.who
                        font.family: Theme.sans
                        font.pixelSize: Theme.roles.itemTitleSm.size
                        font.weight: Font.Medium
                        color: Theme.ink
                    }
                    UzVoiceMark {
                        objectName: "tileVoice_" + tile.modelData
                        voice: tile.voiceMark
                        word: false
                    }
                    UzSystemChip {
                        visible: tile.isLocal && root.localSystem !== ""
                        system: root.localSystem
                    }
                    UzMono {
                        objectName: "tileGame_" + tile.modelData
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        elide: Text.ElideRight
                        text: tile.info.game || ""
                        font.pixelSize: 10
                        font.letterSpacing: 0.8
                        font.capitalization: Font.AllUppercase
                        color: Theme.dim
                    }
                    UzMono {
                        objectName: tile.paused ? "pausedPill_" + tile.modelData : "tileStatus_" + tile.modelData
                        visible: tile.status !== ""
                        Layout.minimumWidth: 0
                        Layout.maximumWidth: Math.max(60, bar.width * 0.4)
                        elide: Text.ElideRight
                        text: tile.status
                        font.pixelSize: 10
                        font.letterSpacing: 0.8
                        font.capitalization: Font.AllUppercase
                        color: Theme.dim
                    }
                    Rectangle {
                        objectName: "audioChip_" + tile.modelData
                        visible: tile.audible && tile.multiple
                        Layout.preferredWidth: audioText.implicitWidth + 12
                        Layout.preferredHeight: 18
                        radius: 0
                        color: Theme.accent
                        Text {
                            id: audioText
                            anchors.centerIn: parent
                            text: qsTr("AUDIO")
                            textFormat: Text.PlainText
                            font.family: Theme.mono
                            font.pixelSize: 10
                            font.weight: Font.DemiBold
                            font.letterSpacing: 1.0
                            color: Theme.inkOnAccent
                        }
                    }
                }
            }

            // PiP window: controls row (key number, size steps, audio, swap, remove) and the caption
            RowLayout {
                visible: tile.pipTile
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.margins: 8
                anchors.topMargin: 10
                height: 24
                spacing: 4
                z: 7
                Rectangle {
                    objectName: "pipKeyBadge_" + tile.modelData
                    visible: tile.pipTile
                    Layout.preferredWidth: 22; Layout.preferredHeight: 20
                    color: "transparent"
                    border.width: 1
                    border.color: Theme.rule
                    UzMono { anchors.centerIn: parent; text: String(tile.slot + 1); font.pixelSize: 10; color: Theme.ink2 }
                }
                Row {
                    spacing: 2
                    Repeater {
                        model: ["S", "M", "L"]
                        delegate: Rectangle {
                            required property string modelData
                            objectName: "pipSize_" + modelData + "_" + tile.modelData
                            width: 20; height: 20
                            color: root.pipSize === modelData ? Theme.accent : "transparent"
                            UzMono { anchors.centerIn: parent; text: parent.modelData; font.pixelSize: 10; color: root.pipSize === parent.modelData ? Theme.inkOnAccent : Theme.muted }
                            TapHandler { onTapped: root.pipSize = parent.modelData }
                        }
                    }
                }
                Rectangle {
                    objectName: "pipAudioChip_" + tile.modelData
                    visible: tile.audible && tile.multiple
                    Layout.preferredWidth: 20; Layout.preferredHeight: 20
                    color: Theme.accent
                    UzGlyph { anchors.centerIn: parent; name: "volume"; size: 14; color: Theme.inkOnAccent }
                }
                Item { Layout.fillWidth: true }
                UzButton {
                    objectName: "swapButton_" + tile.modelData
                    kind: "link"
                    focusPolicy: Qt.NoFocus
                    implicitHeight: 24
                    implicitWidth: 24
                    hPadding: 0
                    text: "⇄"
                    Accessible.name: qsTr("Swap with the main picture")
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                    ToolTip.text: qsTr("Swap with the main picture")
                    onClicked: root.ctl.makeMain(tile.modelData)
                }
                UzButton {
                    objectName: "removeButton_" + tile.modelData
                    visible: !tile.isLocal
                    kind: "link"
                    focusPolicy: Qt.NoFocus
                    implicitHeight: 24
                    implicitWidth: 24
                    hPadding: 0
                    text: "×"
                    Accessible.name: qsTr("Remove from multiview")
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                    ToolTip.text: qsTr("Remove from multiview")
                    onClicked: root.ctl.removeSurface(tile.modelData)
                }
            }
            RowLayout {
                visible: tile.pipTile
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 10
                anchors.bottomMargin: 8
                spacing: 6
                Rectangle { Layout.preferredWidth: 8; Layout.preferredHeight: 8; color: tile.personColor }
                Text {
                    objectName: "pipCaption_" + tile.modelData
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    elide: Text.ElideRight
                    textFormat: Text.PlainText
                    text: tile.info.name || ""
                    font.family: Theme.sans
                    font.pixelSize: Theme.roles.small.size
                    color: Theme.ink
                }
            }
        }
    }
}
