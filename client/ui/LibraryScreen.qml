import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// 3c: Library (reduced): shell, game grid, detail pane.
Rectangle {
    id: root
    required property PlayerController player
    color: Theme.ground
    // The server of this PC only ("Play on this PC", child process): no upload, invites, Online or Server group (8j-1).
    readonly property bool thisPcOnly: root.player.localHub.onThisPcOnly

    // "Upload ROM": native file dialog (UploadFileDialog.qml, QtQuick.Dialogs) when the module is installed,
    // otherwise a path field. The dialog is its own file so that windeployqt sees the import.
    property bool pathPromptOpen: false
    Loader {
        id: fileDialogLoader
        active: false
        source: "UploadFileDialog.qml"
        onLoaded: item.player = root.player
    }
    function openUploadDialog() {
        fileDialogLoader.active = true
        if (fileDialogLoader.status === Loader.Ready && fileDialogLoader.item !== null) {
            fileDialogLoader.item.open()
        } else {
            root.pathPromptOpen = !root.pathPromptOpen
        }
    }

    // Status filter values (pad L / R cycle them; the Systems… menu offers them): the ids are the library filter values.
    readonly property var statusIds: ["all", "ready", "attention", "download"]
    readonly property var statusLabels: ({ all: qsTr("Any status"), ready: qsTr("Ready"), attention: qsTr("Needs attention"), download: qsTr("Not downloaded") })
    readonly property bool gridView: root.player.libraryView === "grid"
    // Grid view: the detail column opens when a tile is chosen (the grid has five tiles per row at 1280 without it).
    property bool detailOpen: false
    readonly property bool detailShown: !root.gridView || root.detailOpen
    // One scope at a time from the chips and the Systems… menu: everything, favourites, a system, a collection or a status.
    function setScope(kind, value) {
        const lib = root.player.library
        lib.filter = kind === "status" ? value : "all"
        lib.systemFilter = kind === "system" ? value : ""
        lib.collectionFilter = kind === "collection" || kind === "favorites" ? value : "all"
    }
    // The scope the Systems… menu shows as chosen ("" while everything or only the favourites are shown).
    readonly property string scopeValue: {
        const lib = root.player.library
        if (lib.filter !== "all") return "st:" + lib.filter
        if (lib.systemFilter !== "") return "sys:" + lib.systemFilter.toLowerCase()
        if (lib.collectionFilter !== "all" && lib.collectionFilter !== "favorites") return "col:" + lib.collectionFilter
        return ""
    }
    readonly property bool scopeAll: root.player.library.filter === "all" && root.player.library.systemFilter === "" && root.player.library.collectionFilter === "all"
    readonly property bool scopeFavorites: root.player.library.filter === "all" && root.player.library.systemFilter === "" && root.player.library.collectionFilter === "favorites"
    readonly property var scopeItems: {
        const lib = root.player.library
        const out = [{ heading: qsTr("System") }]
        for (const c of lib.systemChips) out.push({ label: c.label + " · " + c.count, value: "sys:" + c.value.toLowerCase(), name: "system_" + c.value.toLowerCase() })
        if (lib.personalSupported) {
            out.push({ separator: true }, { heading: qsTr("Collection") })
            for (const c of lib.collectionChips) {
                if (c.kind === "collection") out.push({ label: c.label + " · " + c.count, value: "col:" + c.value, name: "collection_" + c.value })
            }
            out.push({ label: qsTr("+ New collection…"), value: "__new", name: "collectionNew" })
        }
        out.push({ separator: true }, { heading: qsTr("Status") })
        for (const id of ["ready", "attention", "download"]) {
            out.push({ label: root.statusLabels[id] + " · " + lib.counts[id], value: "st:" + id, name: "status_" + id })
        }
        return out
    }
    // Chip text while a system, collection or status is chosen: its name and count.
    readonly property var scopeCurrent: {
        const v = root.scopeValue
        for (const it of root.scopeItems) {
            if (it.value === v && v !== "") {
                const i = it.label.lastIndexOf(" · ")
                return { text: it.label.substring(0, i), count: parseInt(it.label.substring(i + 3)) }
            }
        }
        return null
    }
    function chooseScope(item) {
        const v = item.value
        if (v === "__new") collectionDialogs.openNew("")
        else if (v.startsWith("st:")) root.setScope("status", v.substring(3))
        else if (v.startsWith("sys:")) root.setScope("system", v.substring(4))
        else if (v.startsWith("col:")) root.setScope("collection", v.substring(4))
    }
    function chooseGame(gameId) {
        root.player.selectGame(gameId)
        if (root.gridView) root.detailOpen = true
    }
    // Esc / B / Left from the page: the open detail column of the grid closes first.
    function handleBack() {
        if (root.gridView && root.detailOpen) {
            root.detailOpen = false
            return true
        }
        return false
    }

    UzPageHints {
        id: pageHints
        player: root.player
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        search: true; filter: true
    }

    // Pad commands (Y, L, R): search field, previous / next status filter.
    Connections {
        target: root.player.controllers
        enabled: root.visible
        function onPadCommand(command: string) {
            if (command === "search") {
                search.forceActiveFocus()
            } else if (command === "filterPrev" || command === "filterNext") {
                const ids = root.statusIds
                const i = ids.indexOf(root.player.library.filter)
                root.player.library.filter = ids[(i + (command === "filterNext" ? 1 : ids.length - 1)) % ids.length]
            }
        }
    }

    RowLayout {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: pageHints.top
        spacing: 0

        UzAppNav {
            Layout.fillHeight: true
            Layout.preferredWidth: 232
            player: root.player
        }

        // The list area takes what the sidebar and the detail pane leave and shrinks first (wide fonts): its content
        // minimum must not push the detail pane past the window edge. The content stops at Theme.gridMaxWidth (1680 +
        // margins), left-aligned; on a 4K screen the space between it and the detail column stays empty (P7).
        Item {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            Layout.preferredWidth: 1
            Layout.fillHeight: true

        // The page column scrolls when banners, the Continue card and the admin block leave the list less than its minimum
        // height; otherwise it fills the area as before.
        Flickable {
            id: pageScroll
            objectName: "libraryPageScroll"
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.topMargin: Theme.space4
            anchors.bottomMargin: Theme.space3
            anchors.leftMargin: Theme.space4
            width: Math.max(0, Math.min(parent.width - 2 * Theme.space4, Theme.gridMaxWidth))
            contentWidth: width
            contentHeight: pageColumn.height
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { }

        ColumnLayout {
            id: pageColumn
            x: 0
            y: 0
            width: pageScroll.width
            height: Math.max(pageScroll.height, implicitHeight)
            spacing: Theme.space2

            // Header (3c-6): the one UzPageHeader; "Add games…" is its one action and wraps below the title when the row is
            // too narrow, the title never shortens.
            UzPageHeader {
                id: header
                objectName: "libraryHeader"
                titleName: "libraryTitle"
                eyebrowName: "librarySectionLabel"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                label: root.thisPcOnly ? qsTr("On this PC") + " · " + Theme.plural(root.player.library.totalCount, qsTr("game"), qsTr("games"))
                                         + " · " + Theme.plural(root.player.library.systemCount, qsTr("system"), qsTr("systems"))
                                       : qsTr("Library")
                title: qsTr("Library")
                meta: !root.thisPcOnly && root.player.library.totalCount > 0
                      ? (Theme.plural(root.player.library.totalCount, qsTr("game"), qsTr("games")) + " · "
                         + Theme.plural(root.player.library.systemCount, qsTr("system"), qsTr("systems"))).toUpperCase() : ""
                UzButton {
                    id: uploadButton
                    objectName: "uploadButton"
                    visible: root.player.canUpload && !root.thisPcOnly
                    enabled: !root.player.upload.active
                    kind: "primary"
                    implicitHeight: Theme.row36
                    text: qsTr("Add games…")
                    onClicked: root.openUploadDialog()
                }
            }

            UzLabel {
                objectName: "libraryOnThisPcNote"
                visible: root.thisPcOnly
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                wrapMode: Text.WordWrap
                text: qsTr("Upload, invites, Online and the Server group appear after Share with others.")
                font.pixelSize: Theme.roles.small.size
                color: Theme.muted
            }

            UzMenu {
                id: collectionMenu
                objectName: "collectionMenu"
                items: [{ label: qsTr("Rename...") }, { separator: true }, { label: qsTr("Delete collection..."), danger: true }]
                onChosen: (item, index) => {
                    const id = root.player.library.collectionFilter
                    if (index === 0) collectionDialogs.openRename(id)
                    else if (index === 2) collectionDialogs.openDelete(id)
                }
            }

            // The one toolbar row: search, the scope chips (All, Favourites, Systems…), then sort and List / Grid at the right.
            // Too narrow for the search to keep 120 px: the chips move to a second line; narrower still: sort and view to a third.
            Item {
                id: toolbarBox
                objectName: "libraryToolbar"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                readonly property int gap: Theme.space1
                // 0: one row, 1: search + sort / view, then the chips, 2: search, chips, then sort / view
                readonly property int mode: width - chipRow.natural - rightRow.natural - 2 * gap >= 120 ? 0
                                            : width - rightRow.natural - gap >= 100 ? 1 : 2
                implicitHeight: mode === 0 ? Theme.row36
                                : mode === 1 ? Theme.row36 + gap + chipRow.height
                                             : Theme.row36 + gap + chipRow.height + gap + rightRow.height
                UzField {
                    id: search
                    objectName: "searchField"
                    x: 0
                    width: toolbarBox.mode === 0 ? Math.min(260, toolbarBox.width - chipRow.natural - rightRow.natural - 2 * toolbarBox.gap)
                           : toolbarBox.mode === 1 ? Math.min(260, toolbarBox.width - rightRow.natural - toolbarBox.gap)
                                                   : Math.min(260, toolbarBox.width)
                    implicitHeight: Theme.row36
                    placeholderText: qsTr("Search games")
                    onTextChanged: root.player.library.filterText = text
                }
                Shortcut {
                    sequences: [StandardKey.Find]
                    enabled: root.visible
                    onActivated: search.forceActiveFocus()
                }
                Flow {
                    id: chipRow
                    objectName: "libraryChips"
                    // Natural width of the visible chips in one line (a Flow reports the width it was given, not this one).
                    readonly property real natural: {
                        let w = 0
                        let n = 0
                        for (let i = 0; i < children.length; ++i) {
                            const c = children[i]
                            if (!c.visible || c.implicitWidth === undefined) continue
                            w += c.implicitWidth
                            ++n
                        }
                        return w + Math.max(0, n - 1) * spacing
                    }
                    width: toolbarBox.mode === 0 ? natural : Math.min(natural, toolbarBox.width)
                    x: toolbarBox.mode === 0 ? search.width + toolbarBox.gap : 0
                    y: toolbarBox.mode === 0 ? (Theme.row36 - height) / 2 : Theme.row36 + toolbarBox.gap
                    spacing: Theme.space1
                    UzFilterChip {
                        objectName: "scopeChip_all"
                        text: qsTr("All")
                        count: root.player.library.totalCount
                        active: root.scopeAll
                        onClicked: root.setScope("all", "")
                    }
                    UzFilterChip {
                        objectName: "scopeChip_favorites"
                        visible: root.player.library.personalSupported
                        text: qsTr("Favourites")
                        count: root.player.library.collectionChips.length > 1 ? root.player.library.collectionChips[1].count : 0
                        active: root.scopeFavorites
                        onClicked: root.setScope("favorites", "favorites")
                    }
                    // Systems, collections and the status filter in one menu; the chip shows the choice while one is active.
                    UzFilterChip {
                        id: scopeChip
                        objectName: "scopeChip_menu"
                        text: root.scopeCurrent ? root.scopeCurrent.text : qsTr("Systems…")
                        count: root.scopeCurrent ? root.scopeCurrent.count : -1
                        badgeAccent: root.scopeValue === "st:attention"
                        active: root.scopeCurrent !== null
                        Accessible.role: Accessible.ComboBox
                        onClicked: scopePicker.openFor(scopeChip)
                        UzMenu {
                            id: scopePicker
                            objectName: "scopePicker"
                            selectable: true
                            value: root.scopeValue
                            items: root.scopeItems
                            onChosen: (item) => root.chooseScope(item)
                        }
                    }
                    UzIconButton {
                        id: collectionMore
                        objectName: "collectionMenuButton"
                        visible: root.player.library.personalSupported && !["all", "favorites"].includes(root.player.library.collectionFilter)
                        height: Theme.row28
                        width: Theme.row28
                        glyph: "more"
                        on: collectionMenu.opened
                        Accessible.name: qsTr("Collection menu")
                        onClicked: collectionMenu.openFor(collectionMore)
                    }
                }
                Flow {
                    id: rightRow
                    objectName: "libraryToolbar2"
                    readonly property real natural: sortControl.implicitWidth + spacing + viewSwitch.implicitWidth
                    width: Math.min(natural, toolbarBox.width)
                    x: Math.max(0, toolbarBox.width - width)
                    y: toolbarBox.mode === 2 ? Theme.row36 + toolbarBox.gap + chipRow.height + toolbarBox.gap : 0
                    spacing: Theme.space1
                    SortControl {
                        id: sortControl
                        compact: true
                        current: root.player.library.sortKey
                        showReadyFirst: true
                        readyFirst: root.player.library.readyFirst
                        readyFirstEnabled: root.player.library.readyFirstApplicable
                        onPicked: (key) => root.player.library.sortKey = key
                        onReadyFirstToggled: (on) => root.player.library.readyFirst = on
                    }
                    Item {   // centres the 30 px switch on the 36 px sort control
                        width: viewSwitch.implicitWidth
                        height: Theme.row36
                        UzSegmented {
                            id: viewSwitch
                            objectName: "libraryViewSwitch"
                            anchors.verticalCenter: parent.verticalCenter
                            accessibleName: qsTr("View")
                            segmentHeight: Theme.row28
                            current: root.player.libraryView
                            options: [{ value: "list", label: qsTr("List"), name: "viewList" }, { value: "grid", label: qsTr("Grid"), name: "viewGrid" }]
                            onPicked: (value) => { root.player.libraryView = value; root.detailOpen = false }
                        }
                    }
                }
            }

            // The one notices line: core download / problem / version notices folded into one sentence, "+N more" for the rest.
            LibraryNotices {
                id: notices
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                player: root.player
            }

            RowLayout {
                objectName: "uploadPathPrompt"
                Layout.fillWidth: true
                visible: root.pathPromptOpen && root.player.canUpload
                spacing: Theme.space1
                UzField {
                    id: uploadPath
                    objectName: "uploadPathField"
                    Layout.fillWidth: true
                    implicitHeight: Theme.row36
                    placeholderText: qsTr("Path of the ROM file to upload")
                    onAccepted: uploadPathButton.clicked()
                }
                UzButton {
                    id: uploadPathButton
                    objectName: "uploadPathButton"
                    implicitHeight: Theme.row36
                    text: qsTr("Upload")
                    enabled: uploadPath.text.trim() !== "" && !root.player.upload.active
                    onClicked: { root.player.uploadRom(uploadPath.text.trim()); root.pathPromptOpen = false }
                }
            }

            Rectangle {
                objectName: "uploadStatus"
                Layout.fillWidth: true
                visible: root.player.upload.active || root.player.upload.message !== ""
                implicitHeight: Theme.row36
                radius: 0
                color: root.player.upload.isError ? Theme.dangerTint : Theme.hover
                Rectangle { width: 2; height: parent.height; color: root.player.upload.isError ? Theme.danger : Theme.rule }
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.space15
                    anchors.rightMargin: Theme.space1
                    spacing: Theme.space1
                    UzLabel {
                        Layout.fillWidth: true
                        text: root.player.upload.active
                              ? qsTr("Uploading %1 · %2 %").arg(root.player.upload.fileName).arg(Math.round(root.player.upload.progress * 100))
                              : root.player.upload.message
                        color: root.player.upload.isError ? Theme.danger : Theme.ink
                        font.pixelSize: Theme.roles.input.size
                        elide: Text.ElideRight
                    }
                    Rectangle {
                        visible: root.player.upload.active
                        Layout.preferredWidth: 120
                        Layout.preferredHeight: 2
                        radius: 0
                        color: Theme.rule
                        Rectangle {
                            width: parent.width * root.player.upload.progress
                            height: parent.height
                            radius: 0
                            color: Theme.accent
                        }
                    }
                    UzButton {
                        visible: !root.player.upload.active
                        kind: "link"
                        text: qsTr("Dismiss")
                        onClicked: root.player.dismissUploadMessage()
                    }
                }
            }

            // The final save write at "stop game" failed: shown until dismissed (the Hub copy is not touched).
            Rectangle {
                objectName: "stopSaveError"
                Layout.fillWidth: true
                visible: root.player.stopSaveError !== ""
                implicitHeight: stopSaveText.implicitHeight + 2 * Theme.space1 + 4
                radius: 0
                color: Theme.dangerTint
                Rectangle { width: 2; height: parent.height; color: Theme.danger }
                RowLayout {
                    anchors.fill: parent
                    anchors.margins: Theme.space1
                    anchors.leftMargin: Theme.space15
                    spacing: Theme.space1
                    UzLabel {
                        id: stopSaveText
                        Layout.fillWidth: true
                        text: root.player.stopSaveError
                        wrapMode: Text.WordWrap
                        color: Theme.danger
                        font.pixelSize: Theme.roles.input.size
                    }
                    UzButton { objectName: "stopSaveErrorDismiss"; kind: "link"; text: qsTr("Dismiss"); onClicked: root.player.dismissStopSaveError() }
                }
            }

            Rectangle {
                objectName: "hubLinkBanner"
                Layout.fillWidth: true
                visible: root.player.sessions.available && root.player.hubLink !== "online"
                implicitHeight: Theme.row36
                radius: 0
                // Lost link or connecting: the accent (attention) tone.
                readonly property bool lost: root.player.hubLink === "offline"
                color: Theme.selected
                Rectangle { width: 2; height: parent.height; color: Theme.accent }
                UzLabel {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.space15
                    verticalAlignment: Text.AlignVCenter
                    text: root.player.hubLink === "connecting" ? qsTr("Connecting to the server for Sessions…")
                          : qsTr("▲ Server connection for Sessions lost · reconnecting…")
                    color: Theme.accentText
                    font.pixelSize: Theme.roles.input.size
                    elide: Text.ElideRight
                }
            }

            Rectangle {
                objectName: "sessionMessage"
                Layout.fillWidth: true
                visible: root.player.sessions.message !== ""
                implicitHeight: Theme.row36
                radius: 0
                color: root.player.sessions.messageIsError ? Theme.dangerTint : Theme.hover
                Rectangle { width: 2; height: parent.height; color: root.player.sessions.messageIsError ? Theme.danger : Theme.rule }
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.space15
                    anchors.rightMargin: Theme.space1
                    UzLabel {
                        Layout.fillWidth: true
                        text: root.player.sessions.message
                        color: root.player.sessions.messageIsError ? Theme.danger : Theme.ink
                        font.pixelSize: Theme.roles.input.size
                        elide: Text.ElideRight
                    }
                    UzButton { kind: "link"; text: qsTr("Dismiss"); onClicked: root.player.sessions.dismissMessage() }
                }
            }

            SessionsSection {
                Layout.fillWidth: true
                player: root.player
            }

            // Version states of the server problems (package 8, 8k-2): server older than the app, app older than the server.
            ServerProblemLine {
                Layout.fillWidth: true
                player: root.player
            }

            // Continue (3c-0): up to three recent games as compact cards; hidden until something was played.
            LibraryContinue {
                id: continueBlock
                Layout.fillWidth: true
                Layout.topMargin: Theme.space1
                player: root.player
            }

            UzSectionLabel {
                objectName: "libraryAllLabel"
                Layout.fillWidth: true
                Layout.topMargin: Theme.space1
                number: continueBlock.visible ? 2 : 1
                text: qsTr("All games")
                meta: root.player.library.countText
            }

            Item {
                objectName: "gameListArea"
                Layout.fillWidth: true
                Layout.fillHeight: true
                // At least the table header and seven rows stay visible; the page column scrolls instead of squeezing the list.
                // The empty/loading/error block centred in it must fit too, or its actions end up below the scroll range.
                Layout.minimumHeight: Math.max(Theme.row36 + 7 * Theme.row44, emptyBlock.visible ? emptyBlock.implicitHeight + 2 * Theme.space3 : 0)
                Layout.preferredHeight: Layout.minimumHeight

                // Table header (board S3c1R6): Mono table-header role over the column widths of GameTile
                // (no 32, system 56, status 200, played 96, added 104; 12 px side padding, 16 px gap). Only in the List view.
                Item {
                    id: tableHead
                    objectName: "tableHead"
                    visible: gridScroll.visible && !root.gridView
                    x: gridColumn.x
                    width: gridColumn.width
                    height: visible ? Theme.row36 : 0
                    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.rule }
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.space15
                        anchors.rightMargin: Theme.space15
                        spacing: Theme.space2
                        Repeater {
                            model: [
                                { text: qsTr("No"), w: 32 },
                                { text: qsTr("Game"), w: -1 },
                                { text: qsTr("System"), w: 56 },
                                { text: qsTr("Status"), w: 200 },
                                { text: qsTr("Played"), w: gridScroll.personal ? 96 : 0 },
                                { text: qsTr("Added"), w: gridScroll.dated ? 104 : 0 }
                            ]
                            delegate: Text {
                                required property var modelData
                                visible: modelData.w !== 0
                                Layout.fillWidth: modelData.w < 0
                                Layout.preferredWidth: modelData.w < 0 ? 1 : modelData.w
                                Layout.minimumWidth: 0
                                elide: Text.ElideRight
                                text: modelData.text
                                textFormat: Text.PlainText
                                font.family: Theme.mono
                                font.pixelSize: Theme.roles.tableHeader.size
                                font.weight: Theme.roles.tableHeader.weight
                                font.letterSpacing: Theme.roles.tableHeader.spacing
                                font.capitalization: Font.AllUppercase
                                color: Theme.dim
                            }
                        }
                    }
                }

                // Ready first: two tables (ready group, rest) with a divider between them; otherwise one table with every game.
                Flickable {
                    id: gridScroll
                    objectName: "gameScroll"
                    anchors.fill: parent
                    anchors.topMargin: tableHead.height
                    visible: root.player.libraryState === "ready" && root.player.library.count > 0
                    clip: true
                    contentWidth: width
                    contentHeight: gridColumn.implicitHeight + Theme.space1
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar { }

                    // The table stops at Theme.gridMaxWidth (1680), left-aligned; the detail column keeps its width. One column of
                    // 44 px rows: the GameGrid is a one-column grid here, so the virtualization of large libraries stays.
                    readonly property int gridW: Math.min(Theme.gridMaxWidth, width - 4)
                    // List: one column of 44 px rows. Grid: tiles of at least 190 px (five per row at 1280 without the detail
                    // column), 4:3 placeholder plus title and status line.
                    readonly property int tileColumns: Math.max(2, Math.floor(gridW / 190))
                    readonly property int cellW: root.gridView ? Math.floor(gridW / tileColumns) : gridW
                    readonly property bool dated: root.player.library.sortKey === "added_desc" || root.player.library.sortKey === "added_asc"
                    readonly property int cellH: root.gridView ? Math.round((cellW - 24) * 3 / 4) + 10 + 24 + 4 + 20 + 28 : Theme.row44
                    readonly property bool personal: root.player.library.personalSupported

                    Column {
                        id: gridColumn
                        x: 0
                        y: 0
                        width: Math.min(Theme.gridMaxWidth, gridScroll.width - 4)
                        spacing: 0

                        GameGrid {
                            id: grid
                            objectName: "gameGrid"
                            width: parent.width
                            scroller: gridScroll
                            cellWidth: gridScroll.cellW
                            cellHeight: gridScroll.cellH
                            model: LibraryGroupModel { library: root.player.library; group: root.player.library.grouped ? 0 : -1 }
                            tiles: root.gridView
                            showAdded: gridScroll.dated
                            showPersonal: gridScroll.personal
                            selectedId: root.player.selectedGameId || ""
                            runningId: root.player.backgroundGame.id || ""
                            onTileClicked: (gameId) => root.chooseGame(gameId)
                        }

                        // Group divider: "NOT READY · n · same sort" (only when both groups have games).
                        RowLayout {
                            objectName: "notReadyDivider"
                            visible: root.player.library.grouped
                            width: parent.width
                            height: visible ? implicitHeight : 0
                            spacing: Theme.space15
                            Eyebrow {
                                Layout.minimumWidth: 0
                                Layout.topMargin: Theme.space2
                                Layout.leftMargin: Theme.space15
                                text: qsTr("NOT READY · %1 · same sort").arg(root.player.library.notReadyGroupCount)
                                color: Theme.dim
                            }
                            Rectangle { Layout.fillWidth: true; Layout.topMargin: Theme.space2; Layout.preferredHeight: 1; color: Theme.rule }
                        }

                        GameGrid {
                            id: gridRest
                            objectName: "gameGridNotReady"
                            visible: root.player.library.grouped
                            width: parent.width
                            height: visible ? implicitHeight : 0
                            scroller: gridScroll
                            cellWidth: gridScroll.cellW
                            cellHeight: gridScroll.cellH
                            model: LibraryGroupModel { library: root.player.library; group: 1 }
                            tiles: root.gridView
                            numberOffset: grid.count
                            showAdded: gridScroll.dated
                            showPersonal: gridScroll.personal
                            selectedId: root.player.selectedGameId || ""
                            runningId: root.player.backgroundGame.id || ""
                            onTileClicked: (gameId) => root.chooseGame(gameId)
                        }
                    }
                }

                ColumnLayout {
                    id: emptyBlock
                    anchors.centerIn: parent
                    width: Math.min(parent.width, 420)
                    spacing: Theme.space15
                    visible: !gridScroll.visible
                    // Empty, loading and error states: UzStateBlock (ensō); the title is the message.
                    UzStateBlock {
                        objectName: "libraryEmptyText"
                        Layout.fillWidth: true
                        kind: root.player.libraryState === "loading" ? "loading"
                              : root.player.libraryState === "error" ? "error" : "empty"
                        readonly property string needle: root.player.library.filterText.trim()
                        readonly property string chip: root.player.library.filter
                        readonly property string coll: root.player.library.collectionFilter
                        title: root.player.libraryState === "loading" ? qsTr("Loading Library…")
                               : root.player.libraryState === "error" ? qsTr("Could not load Library: %1").arg(root.player.libraryError)
                               : root.player.library.totalCount === 0 ? qsTr("No games yet.")
                               : needle !== "" ? (chip === "ready" ? qsTr("No ready games match “%1”").arg(needle)
                                                  : chip === "attention" ? qsTr("No games needing attention match “%1”").arg(needle)
                                                  : chip === "download" ? qsTr("No games not downloaded match “%1”").arg(needle)
                                                  : qsTr("No games match “%1”").arg(needle))
                               : chip === "all" && coll === "favorites" ? qsTr("No favourites yet.")
                               : chip === "all" && coll !== "all" ? qsTr("This collection is empty.")
                               : chip === "attention" ? qsTr("Nothing needs attention.")
                               : qsTr("No results.")
                        action: root.player.libraryState === "error" ? qsTr("Reload") : ""
                        onActionClicked: root.player.reloadLibrary()
                    }
                    // Empty states of the collection row (S3c4): favourites follow the person, an empty collection can be renamed or deleted.
                    UzLabel {
                        objectName: "collectionEmptyHint"
                        visible: root.player.libraryState === "ready" && root.player.library.totalCount > 0 && root.player.library.count === 0
                                 && root.player.library.filterText.trim() === "" && root.player.library.filter === "all"
                                 && root.player.library.collectionFilter !== "all"
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                        color: Theme.dim
                        font.pixelSize: Theme.roles.small.size
                        text: root.player.library.collectionFilter === "favorites"
                              ? qsTr("Favourites are per person and follow you to every PC. Mark a game with Favourite on its page.")
                              : qsTr("Add games from a game page: Collections, then + Collection.")
                    }
                    RowLayout {
                        objectName: "collectionEmptyActions"
                        Layout.alignment: Qt.AlignHCenter
                        visible: root.player.libraryState === "ready" && root.player.library.count === 0
                                 && root.player.library.filterText.trim() === "" && root.player.library.filter === "all"
                                 && !["all", "favorites"].includes(root.player.library.collectionFilter)
                        spacing: Theme.space1
                        UzButton { objectName: "emptyCollectionRename"; text: qsTr("Rename..."); onClicked: collectionDialogs.openRename(root.player.library.collectionFilter) }
                        UzButton { objectName: "emptyCollectionDelete"; text: qsTr("Delete collection..."); onClicked: collectionDialogs.openDelete(root.player.library.collectionFilter) }
                    }
                    UzLabel {
                        objectName: "libraryEmptyHint"
                        visible: root.player.libraryState === "ready" && root.player.library.outsideFilter.count !== undefined
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                        color: Theme.dim
                        font.pixelSize: Theme.roles.small.size
                        text: root.player.library.outsideFilter.count === undefined ? ""
                              : root.player.library.outsideFilter.count === 1
                                ? qsTr("1 match outside this filter: %1 (%2).").arg(root.player.library.outsideFilter.title).arg(root.player.library.outsideFilter.reason)
                                : qsTr("%1 matches outside this filter, for example %2 (%3).").arg(root.player.library.outsideFilter.count)
                                    .arg(root.player.library.outsideFilter.title).arg(root.player.library.outsideFilter.reason)
                    }
                    RowLayout {
                        Layout.alignment: Qt.AlignHCenter
                        visible: root.player.libraryState === "ready" && root.player.library.totalCount > 0
                        spacing: Theme.space1
                        UzButton {
                            objectName: "emptyClearSearch"
                            visible: root.player.library.filterText.trim() !== ""
                            text: qsTr("Clear search")
                            onClicked: search.text = ""
                        }
                        UzButton {
                            objectName: "emptyShowAll"
                            visible: root.player.library.filter !== "all"
                            text: qsTr("Show in All")
                            onClicked: root.player.library.filter = "all"
                        }
                        UzButton {
                            objectName: "emptyShowAllGames"
                            visible: root.player.library.collectionFilter !== "all" || root.player.library.systemFilter !== ""
                            text: qsTr("Show all games")
                            onClicked: { root.player.library.collectionFilter = "all"; root.player.library.systemFilter = "" }
                        }
                    }
                }
            }

            // Admin details of the selected game (Uploaded by, File, SHA-256, Saves; Rename, Delete).
            LibraryAdminDetails {
                Layout.fillWidth: true
                visible: root.player.admin.available && root.player.selectedGameId !== ""
                player: root.player
            }
        }
        }
        }

        DetailPane {
            visible: root.detailShown
            closable: root.gridView
            onCloseRequested: root.detailOpen = false
            Layout.fillHeight: true
            Layout.preferredWidth: root.detailShown ? Theme.detailWidth : 0
            player: root.player
        }
    }

    // "Remove from library" of a local game (scan finding V4): the files stay on this PC, Undo registers them again.
    UzToast {
        id: removedToast
        objectName: "removedToast"
        visible: root.player.removedFromLibrary.title !== undefined
        z: 60
        anchors.left: parent.left
        anchors.leftMargin: 232 + Theme.space3
        anchors.bottom: pageHints.top
        anchors.bottomMargin: Theme.space2
        width: Math.min(560, parent.width - 232 - 2 * Theme.space3)
        kind: "done"
        text: qsTr("Removed from library — the file stays on this PC")
        action: qsTr("Undo")
        onActionTriggered: root.player.undoRemoveFromLibrary()
        onCloseRequested: root.player.dismissRemovedFromLibrary()
        Timer { running: removedToast.visible; interval: 10000; onTriggered: root.player.dismissRemovedFromLibrary() }
    }

    StartConfirmDialog { player: root.player }
    CollectionDialogs { id: collectionDialogs; player: root.player }
}
