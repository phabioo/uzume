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
                const ids = ["all", "ready", "attention", "download"]
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

        ColumnLayout {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.topMargin: Theme.space4
            anchors.bottomMargin: Theme.space3
            anchors.leftMargin: Theme.space4
            width: Math.max(0, Math.min(parent.width - 2 * Theme.space4, Theme.gridMaxWidth))
            spacing: Theme.space2

            // Header (3c-6): the one UzPageHeader; search and "Upload ROM" are its actions and wrap below the title when the
            // row is too narrow, the title never shortens.
            UzPageHeader {
                id: header
                objectName: "libraryHeader"
                titleName: "libraryTitle"
                eyebrowName: "librarySectionLabel"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                number: root.thisPcOnly ? -1 : 1
                label: root.thisPcOnly ? qsTr("On this PC") + " · " + Theme.plural(root.player.library.totalCount, qsTr("game"), qsTr("games"))
                                         + " · " + Theme.plural(root.player.library.systemCount, qsTr("system"), qsTr("systems"))
                                       : qsTr("Library")
                labelMeta: !root.thisPcOnly && root.player.library.totalCount > 0 ? Theme.plural(root.player.library.totalCount, qsTr("game"), qsTr("games")) : ""
                title: qsTr("Library")
                Item {
                    implicitWidth: 240
                    implicitHeight: Theme.row36
                    UzField {
                        id: search
                        objectName: "searchField"
                        anchors.fill: parent
                        implicitHeight: Theme.row36
                        placeholderText: qsTr("Search…")
                        onTextChanged: root.player.library.filterText = text
                    }
                }
                UzButton {
                    id: uploadButton
                    objectName: "uploadButton"
                    visible: root.player.canUpload && !root.thisPcOnly
                    enabled: !root.player.upload.active
                    kind: "primary"
                    implicitHeight: Theme.row36
                    text: qsTr("Upload ROM")
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

            // Collection chips, status select, sort and "Ready first" on the same row; they wrap under the chips in a
            // narrow column (3c-6). The count of the result is not repeated as text (P11).
            Item {
                id: toolbar
                objectName: "libraryToolbar"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                // Natural width of the chips in one row (the chips wrap in a Flow, so its own implicit width is not it).
                readonly property real chipsNatural: {
                    let w = 0, n = 0
                    for (let i = 0; i < chipRow.children.length; ++i) {
                        const c = chipRow.children[i]
                        if (c.visible && c.implicitWidth > 0) { w += c.implicitWidth; ++n }
                    }
                    return w + Math.max(0, n - 1) * Theme.space1
                }
                // Sort + "Ready first" side by side; in a narrow toolbar they wrap into two lines when they do not fit
                readonly property real controlsNatural: statusSelect.implicitWidth + sortControl.implicitWidth + readyFirst.implicitWidth + 2 * controls.spacing
                readonly property bool narrow: width < chipsNatural + controlsNatural + 24
                implicitHeight: narrow ? (chipRow.visible ? chipRow.height + Theme.space1 : 0) + controls.height : Math.max(Theme.row36, chipRow.visible ? chipRow.height : 0)
                // Collections (3c-0, Uzume 0.19): "All · 10", "Favourites · 3", one chip per collection and "+ Collection"; the menu of
                // the open collection (Rename..., Delete collection...) ends the row. Hidden on a server without collections.
                Flow {
                    id: chipRow
                    objectName: "collectionChips"
                    visible: root.player.library.personalSupported
                    width: toolbar.width
                    spacing: Theme.space1
                    Repeater {
                        model: root.player.library.collectionChips
                        delegate: UzChip {
                            required property var modelData
                            objectName: "collection_" + modelData.value
                            text: modelData.label
                            count: modelData.count
                            maxWidth: Math.max(120, chipRow.width)
                            active: root.player.library.collectionFilter === modelData.value
                            onClicked: root.player.library.collectionFilter = modelData.value
                        }
                    }
                    UzChip {
                        objectName: "collectionNew"
                        text: qsTr("+ Collection")
                        onClicked: collectionDialogs.openNew("")
                    }
                    UzIconButton {
                        id: collectionMore
                        objectName: "collectionMenuButton"
                        visible: !["all", "favorites"].includes(root.player.library.collectionFilter)
                        height: Theme.row28
                        width: Theme.row28
                        glyph: "more"
                        on: collectionMenu.opened
                        Accessible.name: qsTr("Collection menu")
                        onClicked: collectionMenu.openFor(collectionMore)
                    }
                }
                Flow {
                    id: controls
                    width: toolbar.narrow ? toolbar.width : toolbar.controlsNatural
                    spacing: Theme.space2
                    y: toolbar.narrow && chipRow.visible ? chipRow.height + Theme.space1 : 0
                    x: toolbar.narrow ? 0 : toolbar.width - width
                    UzSelect {
                        id: statusSelect
                        objectName: "statusSelect"
                        implicitWidth: 190
                        height: Theme.row36
                        Accessible.name: qsTr("Status")
                        current: root.player.library.filter
                        // The closed field reads "Status: Ready · 3"; the counts follow the search.
                        model: [["all", qsTr("All")], ["ready", qsTr("Ready")], ["attention", qsTr("Needs attention")], ["download", qsTr("Not downloaded")]].map(o => ({
                            value: o[0],
                            label: (o[0] === root.player.library.filter ? qsTr("Status: ") : "") + o[1] + " · " + root.player.library.counts[o[0]] }))
                        onPicked: (value) => root.player.library.filter = value
                    }
                    SortControl {
                        id: sortControl
                        current: root.player.library.sortKey
                        onPicked: (key) => root.player.library.sortKey = key
                    }
                    UzToggle {
                        id: readyFirst
                        objectName: "readyFirstToggle"
                        text: qsTr("Ready first")
                        height: Theme.row36
                        checked: root.player.library.readyFirst
                        enabled: root.player.library.readyFirstApplicable
                        opacity: enabled ? 1 : 0.45
                        onToggled: {
                            root.player.library.readyFirst = checked
                            checked = Qt.binding(() => root.player.library.readyFirst)
                        }
                    }
                }
            }
            // "3 of 10 games": only while a chip or the search narrows the list; the unfiltered count lives in "All · 10".
            UzLabel {
                id: countLabel
                objectName: "libraryCount"
                readonly property bool narrowed: root.player.library.filter !== "all" || root.player.library.filterText.trim() !== ""
                                                 || root.player.library.collectionFilter !== "all"
                visible: narrowed
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                Layout.preferredHeight: visible ? implicitHeight : 0
                color: Theme.muted
                font.pixelSize: Theme.roles.input.size
                elide: Text.ElideRight
                text: root.player.library.countText
            }

            // Core notices (3c-6): one dismissible notice for the cores the Hub delivers at the first start (grouped by core),
            // one for cores that cannot be delivered, one per version mismatch. Part of the column, not pinned over the grid.
            Repeater {
                model: root.player.coreWarnings
                delegate: Rectangle {
                    id: warn
                    required property var modelData
                    objectName: "coreWarning"
                    readonly property bool info: modelData.tone === "info"
                    readonly property bool dismissible: modelData.code === "core_download"
                    Layout.fillWidth: true
                    implicitHeight: warnRow.implicitHeight + 2 * Theme.space1 + 4
                    radius: 0
                    color: warn.info ? Theme.ground : Theme.selected
                    Rectangle { width: 2; height: parent.height; color: warn.info ? Theme.rule : Theme.accent }
                    RowLayout {
                        id: warnRow
                        anchors.fill: parent
                        anchors.margins: Theme.space1
                        anchors.leftMargin: Theme.space15
                        spacing: Theme.space15
                        UzLabel {
                            id: warnText
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            wrapMode: Text.WordWrap
                            text: warn.modelData.text
                            color: warn.info ? Theme.ink2 : Theme.accentText
                            font.pixelSize: Theme.roles.input.size
                        }
                        UzButton {
                            objectName: "coreWarningDismiss"
                            visible: warn.dismissible
                            kind: "link"
                            text: qsTr("Dismiss")
                            onClicked: root.player.dismissCoreNotice(warn.modelData.code)
                        }
                    }
                }
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

            // Admin part of the Library (package 8): Rescan folders with its result line.
            LibraryAdminBar {
                Layout.fillWidth: true
                visible: root.player.admin.available
                player: root.player
            }

            // Continue block (3c-0): last played game and up to two recent ones; hidden until something was played.
            LibraryContinue {
                Layout.fillWidth: true
                player: root.player
            }

            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                // Table header (board S3c0 "TableHead"): Mono table-header role over the column widths of GameTile
                // (system 56, status 192, added 104; 12 px side padding, 16 px gap).
                Item {
                    id: tableHead
                    objectName: "tableHead"
                    visible: gridScroll.visible
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
                                { text: qsTr("Game"), w: -1 },
                                { text: qsTr("System"), w: 56 },
                                { text: qsTr("Status"), w: 192 },
                                { text: qsTr("Played"), w: gridScroll.personal ? 72 : 0 },
                                { text: qsTr("Last played"), w: gridScroll.personal ? 104 : 0 },
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
                    readonly property int cellW: gridW
                    readonly property bool dated: root.player.library.sortKey === "added_desc" || root.player.library.sortKey === "added_asc"
                    readonly property int cellH: Theme.row44
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
                            showAdded: gridScroll.dated
                            showPersonal: gridScroll.personal
                            selectedId: root.player.selectedGameId || ""
                            runningId: root.player.backgroundGame.id || ""
                            onTileClicked: (gameId) => root.player.selectGame(gameId)
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
                            showAdded: gridScroll.dated
                            showPersonal: gridScroll.personal
                            selectedId: root.player.selectedGameId || ""
                            runningId: root.player.backgroundGame.id || ""
                            onTileClicked: (gameId) => root.player.selectGame(gameId)
                        }
                    }
                }

                ColumnLayout {
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
                            visible: root.player.library.collectionFilter !== "all"
                            text: qsTr("Show all games")
                            onClicked: root.player.library.collectionFilter = "all"
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

        DetailPane {
            Layout.fillHeight: true
            Layout.preferredWidth: Theme.detailWidth
            player: root.player
        }
    }

    StartConfirmDialog { player: root.player }
    CollectionDialogs { id: collectionDialogs; player: root.player }
}
