import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// Runs in the Uzume design (runs-and-server.md 4j Runs, 4jh Run board): the table of runs with the detail of the selected run
// under it, invites above it, and the run view with Tabs lg (Board, Log, plugin tabs last). List, invite and log belong to
// Uzume; plugins only build on them.
Rectangle {
    id: root
    objectName: "runsScreen"
    required property PlayerController player
    readonly property RunsController runs: root.player.runs
    readonly property bool inDetail: root.runs.openRunId !== ""
    readonly property string serverName: root.player.hubName !== "" ? root.player.hubName : qsTr("Server")
    property string confirmAction: ""      // finished | abandoned | leave
    property string selectedRunId: ""
    readonly property int maxPeople: 8     // a run holds 1 to 8 people
    // The open column (vertical label, counts) only at 1440 px and wider; the empty list keeps it from 1100 px on.
    readonly property bool openColumnOn: root.width >= (!root.inDetail && root.runRows.length === 0 ? 1100 : Theme.columnsCompactMax)
    color: Theme.ground

    onVisibleChanged: if (!visible && root.runs) root.runs.closeRun()
    function ask(action: string) {
        root.confirmAction = action
        confirmDialog.open()
    }

    // ---- rows of the runs table and of the board, rebuilt whenever a model changes -------------------------------------------
    property int listRev: 0
    property int partRev: 0
    Connections {
        target: root.runs.active
        function onModelReset() { root.listRev++ }
        function onRowsInserted() { root.listRev++ }
        function onRowsRemoved() { root.listRev++ }
        function onDataChanged() { root.listRev++ }
    }
    Connections {
        target: root.runs.finished
        function onModelReset() { root.listRev++ }
        function onRowsInserted() { root.listRev++ }
        function onRowsRemoved() { root.listRev++ }
        function onDataChanged() { root.listRev++ }
    }
    Connections {
        target: root.runs.participants
        function onModelReset() { root.partRev++ }
        function onRowsInserted() { root.partRev++ }
        function onRowsRemoved() { root.partRev++ }
        function onDataChanged() { root.partRev++ }
    }

    function rowOf(m: var, over: bool): var {
        const names = []
        for (let i = 0; i < m.participants.length; ++i) names.push(m.participants[i].name)
        return { runId: m.runId, name: m.name, game: m.game, rules: m.rulesName + (m.rulesVersion !== "" ? " " + m.rulesVersion : ""),
                 people: names.join(", "), peopleCount: names.length, updated: m.timeText, statusText: m.statusText, played: m.playTimeText,
                 tone: m.tone, over: over }
    }
    function buildRows(): var {
        void root.listRev
        const out = []
        for (let i = 0; i < root.runs.active.count; ++i) out.push(root.rowOf(root.runs.active.get(i), false))
        for (let j = 0; j < root.runs.finished.count; ++j) out.push(root.rowOf(root.runs.finished.get(j), true))
        return out
    }
    readonly property var runRows: root.buildRows()
    readonly property int selectedIndex: {
        for (let i = 0; i < root.runRows.length; ++i) if (root.runRows[i].runId === root.selectedRunId) return i
        return root.runRows.length > 0 ? 0 : -1
    }
    readonly property var selectedRow: root.selectedIndex >= 0 ? root.runRows[root.selectedIndex] : null

    readonly property var boardRows: {
        void root.partRev
        const out = []
        const p = root.runs.participants
        for (let i = 0; i < p.count; ++i) out.push(p.get(i))
        return out
    }

    // Cell of the runs table (one component, the column key says what it shows).
    Component {
        id: runCell
        Item {
            id: cell
            property var row: null
            property var column: null
            readonly property string key: cell.column ? cell.column.key : ""
            readonly property bool picked: cell.row !== null && cell.row.runId === root.selectedRunId
            height: Theme.row44
            opacity: cell.row && cell.row.over ? 0.6 : 1

            Column {
                visible: cell.key === "name"
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width
                Text {
                    objectName: "runRowName"
                    width: parent.width
                    elide: Text.ElideRight
                    text: cell.row ? cell.row.name : ""
                    textFormat: Text.PlainText
                    font.family: Theme.sans
                    font.pixelSize: Theme.roles.body.size
                    font.weight: cell.picked ? Font.Medium : Font.Normal
                    color: Theme.ink
                }
                Text {
                    visible: cell.row !== null && cell.row.game !== ""
                    width: parent.width
                    elide: Text.ElideRight
                    text: cell.row ? cell.row.game : ""
                    textFormat: Text.PlainText
                    font.family: Theme.sans
                    font.pixelSize: Theme.roles.small.size
                    color: Theme.muted
                }
            }
            UzTag {
                visible: cell.key === "rules" && cell.row !== null && cell.row.rules !== ""
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(implicitWidth, parent.width)
                text: cell.row ? cell.row.rules : ""
            }
            Text {
                visible: cell.key === "rules" && cell.row !== null && cell.row.rules === ""
                anchors.verticalCenter: parent.verticalCenter
                text: "—"
                font.family: Theme.sans
                font.pixelSize: Theme.roles.small.size
                color: Theme.muted
            }
            Text {
                visible: cell.key === "people"
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width
                elide: Text.ElideRight
                text: cell.row ? cell.row.people : ""
                textFormat: Text.PlainText
                font.family: Theme.sans
                font.pixelSize: Theme.roles.input.size
                color: Theme.muted
            }
            Text {
                objectName: "runRowPlayTime"
                visible: cell.key === "played"
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width
                elide: Text.ElideRight
                text: cell.row ? cell.row.played : ""
                textFormat: Text.PlainText
                font.family: Theme.mono
                font.pixelSize: Theme.roles.meta11.size
                color: Theme.muted
            }
            Text {
                visible: cell.key === "updated"
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width
                elide: Text.ElideRight
                text: cell.row ? cell.row.updated : ""
                textFormat: Text.PlainText
                font.family: Theme.mono
                font.pixelSize: Theme.roles.meta11.size
                color: Theme.muted
            }
            UzTag {
                objectName: "runRowStatus"
                visible: cell.key === "status"
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(implicitWidth, parent.width)
                tone: cell.row ? cell.row.tone : "neutral"
                disabled: cell.row !== null && cell.row.over
                text: cell.row ? cell.row.statusText : ""
            }
        }
    }

    // Cell of the board table of a run: person, save slot, status.
    Component {
        id: boardCell
        Item {
            id: bc
            property var row: null
            property var column: null
            readonly property string key: bc.column ? bc.column.key : ""
            height: Theme.row44

            Row {
                objectName: bc.row ? "runParticipant_" + bc.row.userId : ""
                visible: bc.key === "name"
                anchors.verticalCenter: parent.verticalCenter
                spacing: Theme.space1
                UzAvatar {
                    anchors.verticalCenter: parent.verticalCenter
                    size: 20
                    name: bc.row ? bc.row.name : ""
                    person: bc.row && bc.row.color >= 0 ? bc.row.color + 1 : 6
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.max(0, bc.width - 20 - Theme.space1)
                    elide: Text.ElideRight
                    text: bc.row ? (bc.row.me ? qsTr("%1 (you)").arg(bc.row.name) : bc.row.name) : ""
                    textFormat: Text.PlainText
                    font.family: Theme.sans
                    font.pixelSize: Theme.roles.body.size
                    font.weight: Font.Medium
                    color: Theme.ink
                }
            }
            Text {
                objectName: "runParticipantSlot"
                visible: bc.key === "slot"
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width
                elide: Text.ElideRight
                text: bc.row ? bc.row.slotText : ""
                textFormat: Text.PlainText
                font.family: Theme.sans
                font.pixelSize: Theme.roles.input.size
                color: Theme.ink2
            }
            Text {
                visible: bc.key === "game"
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width
                elide: Text.ElideRight
                text: bc.row ? bc.row.gameTitle : ""
                textFormat: Text.PlainText
                font.family: Theme.sans
                font.pixelSize: Theme.roles.input.size
                color: Theme.muted
            }
            UzTag {
                visible: bc.key === "status"
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(implicitWidth, parent.width)
                tone: bc.row && bc.row.statusText === qsTr("Joined") ? "ok" : "neutral"
                text: bc.row ? bc.row.statusText : ""
            }
        }
    }

    // One line of a definition list: Mono label at the left, value (ink-2, 13) beside it, a hairline under it.
    component DefRow: Item {
        id: def
        property string label: ""
        property string value: ""
        property string valueName: ""
        Layout.fillWidth: true
        implicitHeight: Math.max(32, defValue.implicitHeight + Theme.space1)
        Text {
            x: 0
            width: 96
            anchors.verticalCenter: parent.verticalCenter
            elide: Text.ElideRight
            text: def.label
            textFormat: Text.PlainText
            font.family: Theme.mono
            font.pixelSize: Theme.roles.tableHeader.size
            font.weight: Font.Medium
            font.letterSpacing: Theme.roles.tableHeader.spacing
            font.capitalization: Font.AllUppercase
            color: Theme.dim
        }
        Text {
            id: defValue
            objectName: def.valueName
            x: 96 + Theme.space15
            width: Math.max(0, def.width - x)
            anchors.verticalCenter: parent.verticalCenter
            wrapMode: Text.Wrap
            text: def.value
            textFormat: Text.PlainText
            font.family: Theme.sans
            font.pixelSize: Theme.roles.input.size
            lineHeight: Theme.roles.small.line
            lineHeightMode: Text.FixedHeight
            color: Theme.ink2
        }
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.rule2 }
    }

    UzPageHints {
        id: pageHints
        player: root.player
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
    }

    RowLayout {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: pageHints.top
        spacing: 0

        UzAppNav {
            Layout.fillHeight: true
            Layout.preferredWidth: Theme.sidebar
            player: root.player
        }

        Flickable {
            id: flick
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumWidth: 0
            contentWidth: width
            contentHeight: page.y + page.implicitHeight + Theme.space5
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}

            ColumnLayout {
                id: page
                x: Theme.space7
                y: 48
                width: Math.max(0, Math.min(Theme.contentMaxWidth, flick.width - 2 * Theme.space7))
                spacing: Theme.space3

                // ---------------------------------------------------------------- shared: message line and busy line
                RowLayout {
                    objectName: "runsMessage"
                    visible: root.runs.message !== ""
                    Layout.fillWidth: true
                    spacing: Theme.space2
                    UzStatusIcon {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        kind: root.runs.messageIsError ? "error" : "done"
                        small: true
                        text: root.runs.message
                        maxWidth: Math.max(0, page.width - 120)
                    }
                    UzButton { kind: "link"; text: qsTr("Dismiss"); onClicked: root.runs.dismissMessage() }
                }
                RowLayout {
                    visible: root.runs.busy
                    spacing: Theme.space1
                    UzSpinner { size: 14; color: Theme.accent }
                    UzLabel {
                        objectName: "runsBusy"
                        text: root.runs.busyText
                        color: Theme.muted
                        font.pixelSize: Theme.roles.input.size
                    }
                }

                // ================================================================ list (4j-app-1, 4j-app-2)
                ColumnLayout {
                    visible: !root.inDetail
                    Layout.fillWidth: true
                    spacing: Theme.space3

                    UzPageHeader {
                        Layout.fillWidth: true
                        label: root.player.isAdmin ? qsTr("Play · everyone on %1").arg(root.serverName) : qsTr("Play · runs you are in")
                        title: qsTr("Runs")
                        UzButton {
                            objectName: "runsStartButton"
                            kind: "primary"
                            text: qsTr("New run…")
                            onClicked: startDialog.openStart()
                        }
                    }

                    Text {
                        objectName: "runsPcOnlyNote"
                        visible: root.player.localHub.onThisPcOnly
                        Layout.fillWidth: true
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        font.family: Theme.sans
                        font.pixelSize: Theme.roles.small.size
                        color: Theme.muted
                        text: qsTr("Chat and people need a server.")
                    }

                    // Invites: hairline rows with the answer next to them.
                    ColumnLayout {
                        visible: root.runs.invites.count > 0
                        Layout.fillWidth: true
                        spacing: Theme.space1
                        UzSectionLabel {
                            Layout.fillWidth: true
                            text: qsTr("Run invites · %1").arg(root.runs.invites.count)
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.rule }
                            Repeater {
                                model: root.runs.invites
                                delegate: Item {
                                    id: card
                                    required property var model
                                    objectName: "runInvite_" + card.model.runId
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                    implicitHeight: cardRow.implicitHeight + Theme.space15 + 1
                                    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.rule2 }
                                    RowLayout {
                                        id: cardRow
                                        x: Theme.space15
                                        y: Theme.space15 / 2
                                        width: parent.width - Theme.space15
                                        spacing: Theme.space2
                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            Layout.minimumWidth: 0
                                            spacing: 0
                                            UzLabel {
                                                objectName: "runInviteHeadline"
                                                Layout.fillWidth: true
                                                text: card.model.headline
                                                wrapMode: Text.WordWrap
                                                font.weight: Font.Medium
                                            }
                                            UzLabel {
                                                Layout.fillWidth: true
                                                text: card.model.subText + " · " + card.model.joinedText
                                                color: Theme.muted
                                                font.pixelSize: Theme.roles.small.size
                                                wrapMode: Text.WordWrap
                                            }
                                        }
                                        UzButton {
                                            objectName: "runInviteDecline"
                                            size: "sm"
                                            text: qsTr("Decline")
                                            enabled: !root.runs.busy
                                            onClicked: root.runs.declineRun(card.model.runId)
                                        }
                                        UzButton {
                                            objectName: "runInviteJoin"
                                            size: "sm"
                                            kind: "primary"
                                            text: qsTr("Join")
                                            enabled: !root.runs.busy
                                            onClicked: joinDialog.openFor(card.model.runId, card.model.name, card.model.inviteLine)
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // The table: run (name, game), rules, people, updated, status.
                    UzTable {
                        id: runTable
                        objectPrefix: "runTable"
                        visible: root.runRows.length > 0
                        Layout.fillWidth: true
                        columns: {
                            const c = [{ key: "name", title: qsTr("Run"), fill: 1.3, custom: true }]
                            if (page.width >= 640) c.push({ key: "rules", title: qsTr("Rules"), width: 150, custom: true })
                            if (page.width >= 520) c.push({ key: "people", title: qsTr("People"), fill: 1, custom: true })
                            if (page.width >= 560) c.push({ key: "played", title: qsTr("Play time"), width: 80, custom: true })
                            c.push({ key: "updated", title: qsTr("Updated"), width: 96, custom: true })
                            c.push({ key: "status", title: qsTr("Status"), width: 112, custom: true })
                            return c
                        }
                        rows: root.runRows
                        cellDelegate: runCell
                        currentIndex: root.selectedIndex
                        onPicked: (index, row) => root.selectedRunId = row.runId
                        onActivated: (index, row) => root.runs.openRun(row.runId)
                    }

                    // Detail of the selected run: save slot, rules, people, then Open run.
                    ColumnLayout {
                        objectName: "runSelectedDetail"
                        visible: root.selectedRow !== null
                        Layout.fillWidth: true
                        spacing: Theme.space1
                        UzSectionLabel {
                            Layout.fillWidth: true
                            number: 2
                            text: root.selectedRow ? root.selectedRow.name : ""
                            meta: root.selectedRow ? (root.selectedRow.rules !== "" ? root.selectedRow.rules : qsTr("Plain run")) : ""
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.rule }
                            DefRow {
                                label: qsTr("Save slot")
                                value: root.selectedRow ? qsTr("Run: %1 · each person’s Main slot is not touched").arg(root.selectedRow.name) : ""
                            }
                            DefRow {
                                label: qsTr("Rules")
                                value: root.selectedRow ? (root.selectedRow.rules !== "" ? root.selectedRow.rules : qsTr("No rules")) : ""
                            }
                            DefRow {
                                label: qsTr("People")
                                value: root.selectedRow ? qsTr("%1 · %2 of %3").arg(root.selectedRow.people !== "" ? root.selectedRow.people : "—")
                                                          .arg(root.selectedRow.peopleCount).arg(root.maxPeople) : ""
                            }
                        }
                        UzButton {
                            objectName: "runOpen"
                            Layout.topMargin: Theme.space05
                            size: "sm"
                            text: qsTr("Open run")
                            onClicked: root.runs.openRun(root.selectedRow.runId)
                        }
                    }

                    UzLabel {
                        visible: !root.player.isAdmin && root.runRows.length > 0
                        Layout.fillWidth: true
                        text: qsTr("Members can start runs too.")
                        color: Theme.muted
                        font.pixelSize: Theme.roles.small.size
                        wrapMode: Text.WordWrap
                    }

                    UzStateBlock {
                        objectName: "runsEmpty"
                        visible: root.runs.loaded && root.runs.invites.count === 0 && root.runRows.length === 0
                        Layout.fillWidth: true
                        title: qsTr("No runs yet")
                        text: qsTr("A run is a playthrough with its own save slot, for 1-8 people.")
                        action: qsTr("New run…")
                        onActionClicked: startDialog.openStart()
                    }
                }

                // ================================================================ run view (4jh-app-1, 4j-app-4)
                ColumnLayout {
                    id: detailCol
                    visible: root.inDetail
                    Layout.fillWidth: true
                    spacing: Theme.space3
                    readonly property var d: root.runs.detail
                    readonly property bool plain: detailCol.d.plain === true
                    // The run actions of the "⋯" menu. Rebuilt only when their content changes, so the menu is not reset on
                    // every update of the run.
                    property var menuItems: []
                    property string menuJson: ""
                    function rebuildMenu() {
                        const d = detailCol.d
                        const out = []
                        if (d.canStart === true) out.push({ name: "start", label: qsTr("Start run now") })
                        if (d.canPause === true) out.push({ name: "pause", label: qsTr("Pause run") })
                        if (d.canResume === true) out.push({ name: "resume", label: qsTr("Resume run") })
                        if (d.canLeave === true) out.push({ name: "leave", label: qsTr("Leave run…"), danger: true, divider: out.length > 0 })
                        if (d.canFinish === true) out.push({ name: "finish", label: qsTr("Finish run…") })
                        if (d.canAbandon === true) out.push({ name: "abandon", label: qsTr("Abandon run…"), danger: true, divider: true })
                        const j = JSON.stringify(out)
                        if (j === detailCol.menuJson) return
                        detailCol.menuJson = j
                        detailCol.menuItems = out
                    }
                    onDChanged: rebuildMenu()
                    Component.onCompleted: rebuildMenu()

                    UzButton {
                        objectName: "runsBack"
                        kind: "link"
                        text: qsTr("‹ Runs")
                        onClicked: root.runs.closeRun()
                    }

                    // One primary (Play), Invite as the secondary, the state actions in the "⋯" menu (P24, 4j-3).
                    UzPageHeader {
                        objectName: "runDetailHeader"
                        Layout.fillWidth: true
                        label: detailCol.plain ? qsTr("Plain run") : qsTr("Run · %1").arg(detailCol.d.pluginsText || "")
                        title: detailCol.d.name || ""
                        UzButton {
                            objectName: "runPlay"
                            visible: detailCol.d.canPlay === true
                            kind: "primary"
                            text: detailCol.d.playLabel || qsTr("Play run")
                            enabled: !root.runs.busy
                            onClicked: root.runs.playRun(detailCol.d.runId)
                        }
                        UzButton {
                            objectName: "runInvitePlayers"
                            visible: detailCol.d.canInvite === true
                            text: qsTr("Invite players")
                            onClicked: startDialog.openInvite(detailCol.d.runId)
                        }
                        Item {
                            width: Theme.row36
                            height: Theme.row36
                            visible: detailCol.menuItems.length > 0
                            MoreMenu {
                                objectName: "runMenu"
                                anchors.centerIn: parent
                                enabled: !root.runs.busy
                                tooltip: qsTr("More actions for this run")
                                items: detailCol.menuItems
                                onActivated: name => {
                                    const id = detailCol.d.runId
                                    if (name === "start" || name === "resume") root.runs.setRunStatus(id, "active")
                                    else if (name === "pause") root.runs.setRunStatus(id, "paused")
                                    else if (name === "finish") root.ask("finished")
                                    else if (name === "abandon") root.ask("abandoned")
                                    else if (name === "leave") root.ask("leave")
                                }
                            }
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: Theme.space05
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.space15
                            UzTag { objectName: "runDetailStatus"; tone: detailCol.d.tone || "neutral"; text: detailCol.d.statusText || "" }
                            UzLabel {
                                objectName: "runDetailLine"
                                Layout.fillWidth: true
                                text: detailCol.d.line || ""
                                elide: Text.ElideRight
                                color: Theme.muted
                                font.pixelSize: Theme.roles.small.size
                            }
                        }
                        UzLabel {
                            visible: (detailCol.d.note || "") !== ""
                            Layout.fillWidth: true
                            text: detailCol.d.note || ""
                            wrapMode: Text.WordWrap
                            color: Theme.muted
                            font.pixelSize: Theme.roles.small.size
                        }
                    }

                    // Tabs: Board, Log, then one tab per run plugin (named after its screen), the plugin tab last.
                    readonly property var tabDefs: {
                        const out = [{ value: "board", label: qsTr("Board") }, { value: "log", label: qsTr("Log") }]
                        const t = root.runs.runTabs
                        for (let i = 0; i < t.length; ++i)
                            out.push({ value: "plugin:" + t[i].pluginId, label: t[i].title, badge: t[i].badge > 0 ? t[i].badge : 0 })
                        return out
                    }
                    readonly property var tabLabels: detailCol.tabDefs.map(t => t.label)
                    readonly property var tabCounts: {
                        const c = {}
                        for (const t of detailCol.tabDefs) if (t.badge > 0) c[t.label] = t.badge
                        return c
                    }
                    readonly property string tabActive: {
                        for (const t of detailCol.tabDefs) if (t.value === root.runs.runTab) return t.label
                        return qsTr("Board")
                    }
                    UzTabs {
                        objectName: "runTabs"
                        Layout.fillWidth: true
                        size: "lg"
                        items: detailCol.tabLabels
                        counts: detailCol.tabCounts
                        active: detailCol.tabActive
                        onPicked: label => {
                            for (const t of detailCol.tabDefs) if (t.label === label) { root.runs.runTab = t.value; return }
                        }
                    }

                    // ---------------------------------------------------------- Board: person, save slot, game, status
                    ColumnLayout {
                        objectName: "runBoardTab"
                        visible: root.runs.runTab === "board"
                        Layout.fillWidth: true
                        spacing: Theme.space3
                        PluginRunSection { Layout.fillWidth: true; player: root.player; statusOnly: true }
                        UzTable {
                            objectPrefix: "runBoard"
                            Layout.fillWidth: true
                            selectable: false
                            columns: {
                                const c = [{ key: "name", title: qsTr("Person"), fill: 1, custom: true }]
                                if (page.width >= 480) c.push({ key: "slot", title: qsTr("Save slot"), fill: 1.2, custom: true })
                                if (page.width >= 600) c.push({ key: "gameTitle", title: qsTr("Game"), fill: 1 })
                                if (page.width >= 520) c.push({ key: "playTimeText", title: qsTr("Play time"), width: 80, mono: true })
                                if (page.width >= 700) c.push({ key: "lastPlayedText", title: qsTr("Last played"), width: 104 })
                                c.push({ key: "status", title: qsTr("Status"), width: 104, custom: true })
                                return c
                            }
                            rows: root.boardRows
                            cellDelegate: boardCell
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: Theme.space1
                            UzSectionLabel {
                                Layout.fillWidth: true
                                number: 2
                                text: qsTr("Save slot")
                                meta: detailCol.d.saveSlotText || ""
                            }
                            UzLabel {
                                Layout.fillWidth: true
                                text: qsTr("Each person plays in “%1”. Their Main slot is not touched.").arg(detailCol.d.saveSlotText || "")
                                wrapMode: Text.WordWrap
                                color: Theme.ink2
                                lineHeight: Theme.roles.body.line
                                lineHeightMode: Text.FixedHeight
                            }
                        }
                    }

                    // ---------------------------------------------------------- Plugin tab (the plugin's screen or run tree)
                    ColumnLayout {
                        objectName: "runPluginTab"
                        visible: root.runs.runTab.indexOf("plugin:") === 0
                        Layout.fillWidth: true
                        PluginRunSection { Layout.fillWidth: true; player: root.player; pluginId: root.runs.runTab.substring(7) }
                    }

                    // ---------------------------------------------------------- Log
                    ColumnLayout {
                        objectName: "runLogTab"
                        visible: root.runs.runTab === "log"
                        Layout.fillWidth: true
                        spacing: Theme.space1
                        UzSectionLabel {
                            objectName: "runEventsTitle"
                            Layout.fillWidth: true
                            text: qsTr("Event log · %1").arg(root.runs.events.count)
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.rule }
                            UzLabel {
                                visible: root.runs.events.count === 0
                                Layout.fillWidth: true
                                Layout.topMargin: Theme.space15
                                Layout.bottomMargin: Theme.space15
                                leftPadding: Theme.space15
                                text: qsTr("No events yet.")
                                color: Theme.muted
                                font.pixelSize: Theme.roles.input.size
                            }
                            Repeater {
                                model: root.runs.events
                                delegate: Item {
                                    id: erow
                                    required property var model
                                    objectName: "runEvent_" + erow.model.seq
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                    implicitHeight: Math.max(Theme.row44, evLayout.implicitHeight + Theme.space15)
                                    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.rule2 }
                                    RowLayout {
                                        id: evLayout
                                        x: Theme.space15
                                        width: parent.width - Theme.space15
                                        anchors.verticalCenter: parent.verticalCenter
                                        spacing: Theme.space15
                                        UzMono {
                                            objectName: "runEventTime"
                                            Layout.preferredWidth: 130
                                            text: erow.model.timeText
                                            color: Theme.muted
                                            font.pixelSize: Theme.roles.meta11.size
                                            elide: Text.ElideRight
                                            HoverHandler { id: evHover }
                                            ToolTip.visible: evHover.hovered && erow.model.fullTimeText !== ""
                                            ToolTip.text: erow.model.fullTimeText
                                        }
                                        UzLabel {
                                            Layout.fillWidth: true
                                            text: erow.model.text
                                            wrapMode: Text.WordWrap
                                            font.pixelSize: Theme.roles.input.size
                                        }
                                        UzLabel {
                                            Layout.preferredWidth: 170
                                            visible: page.width > 700
                                            text: erow.model.sourceText
                                            elide: Text.ElideRight
                                            color: Theme.muted
                                            font.pixelSize: Theme.roles.small.size
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                // Admins: every run of every person with the actions the state allows (package 8, RunsAdminPanel).
                RunsAdminPanel {
                    Layout.fillWidth: true
                    visible: root.player.admin.available
                    player: root.player
                    runId: root.runs.openRunId
                }
            }
        }

        // Open column (board 4j-app-1): the vertical label and the counts, only where the page keeps its width.
        UzOpenColumn {
            objectName: "runsOpenColumn"
            visible: root.openColumnOn
            Layout.preferredWidth: Theme.openColumn
            Layout.fillHeight: true
            Layout.topMargin: 48
            Layout.bottomMargin: Theme.space3
            Layout.rightMargin: Theme.space7
            label: qsTr("Uzume · Runs · %1").arg(root.serverName)
            lines: root.inDetail
                   ? [Theme.plural(root.runs.participants.count, qsTr("person"), qsTr("people"))]
                   : (root.runRows.length > 0
                      ? [Theme.plural(root.runRows.length, qsTr("run"), qsTr("runs")), qsTr("%1 active").arg(root.runs.active.count)]
                      : [])
        }
    }

    RunJoinDialog {
        id: joinDialog
        runs: root.runs
    }
    RunStartDialog {
        id: startDialog
        runs: root.runs
        pcOnly: root.player.localHub.onThisPcOnly
    }

    // Finish / abandon / leave ask first.
    UzDialog {
        id: confirmDialog
        objectName: "runConfirmDialog"
        label: qsTr("Runs · %1").arg(root.runs.detail.name || "")
        title: root.confirmAction === "finished" ? qsTr("Finish this run?")
               : (root.confirmAction === "abandoned" ? qsTr("Abandon this run?") : qsTr("Leave this run?"))
        text: root.confirmAction === "leave" ? qsTr("You stop taking part. The others continue; your events stay in the log.")
              : qsTr("It ends for everyone. No new events are recorded afterwards and the run cannot be resumed.")
        Component.onCompleted: confirmDialog.frame.objectName = "runConfirmFrame"
        actions: [
            UzButton { objectName: "runConfirmCancel"; text: qsTr("Cancel"); onClicked: confirmDialog.close() },
            UzButton {
                objectName: "runConfirmOk"
                kind: root.confirmAction === "finished" ? "primary" : "danger"
                text: root.confirmAction === "finished" ? qsTr("Finish run") : (root.confirmAction === "abandoned" ? qsTr("Abandon run") : qsTr("Leave run"))
                onClicked: {
                    const action = root.confirmAction
                    const id = root.runs.openRunId
                    root.confirmAction = ""
                    confirmDialog.close()
                    if (action === "leave") root.runs.leaveRun(id)
                    else root.runs.setRunStatus(id, action)
                }
            }
        ]
    }
}
