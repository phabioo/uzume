import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// 3p: Player settings. Section column (jump list, D2) | one scrolling page with Updates, Hubs, Appearance,
// Diagnostics. Clicking a section scrolls to it and highlights the item. Saved on this device only.
Rectangle {
    id: root
    required property PlayerController player
    readonly property var updates: root.player.updates
    property string currentSection: "account"
    property string collectMessage: ""
    property string collectedPath: ""
    property string editingHubId: ""
    // Admins can switch between this PC and the server ("Settings for"); members and users without a server see this PC only.
    readonly property bool canSwitchScope: root.player.isAdmin && root.player.hubAddress !== ""
    property string scope: "pc"   // pc | server
    readonly property var sectionDefs: [
        { id: "account", name: qsTr("My account") },
        { id: "updates", name: qsTr("Updates") },
        { id: "hubs", name: qsTr("Servers") },
        { id: "appearance", name: qsTr("Appearance") },
        { id: "audio", name: qsTr("Audio & video") },
        { id: "library", name: qsTr("Library folders") },
        { id: "storage", name: qsTr("Storage") },
        { id: "plugins", name: qsTr("Plugins") },
        { id: "diagnostics", name: qsTr("Diagnostics") }
    ]
    color: Theme.ground

    // Open help column (boards 9c-1, 6l-1): only on windows wide enough to keep the rows in their wide layout.
    readonly property bool helpOn: root.width >= 1480
    readonly property var helpItems: {
        switch (root.currentSection) {
        case "appearance": return [{ text: qsTr("Theme is per PC; your favourites and collections follow your account.") }]
        case "storage": return [{ title: qsTr("Nothing on the server"), text: qsTr("Clean-up only removes copies on this PC: games that download again, unused cores. Saves are always on %1 and never part of a clean-up.").arg(root.player.hubName !== "" ? root.player.hubName : qsTr("your server")) }]
        case "library": return [{ text: qsTr("Uzume tells the server which games you have; the files stay on this PC.") }]
        default: return [{ text: qsTr("These settings only affect this PC and your account.") }]
        }
    }


    readonly property var savedHubs: root.player.hubs.filter(function (h) { return h.saved })
    readonly property string activeHubName: {
        for (let i = 0; i < root.savedHubs.length; ++i) {
            if (root.savedHubs[i].connected) {
                return root.savedHubs[i].name
            }
        }
        return ""
    }

    // "Plugin settings" / "Notice settings…" of a plugin surface: scroll to Plugins, highlight the plugin's row.
    Connections {
        target: root.player
        function onPluginSettingsRequested(pluginId: string) {
            Qt.callLater(function () {
                root.jumpTo("plugins")
                secPlugins.highlightId = pluginId
            })
        }
    }

    onCanSwitchScopeChanged: if (!canSwitchScope) scope = "pc"
    Connections {
        target: root.player
        function onSettingsSectionRequested(section: string) {
            // "server:<page>" opens the Server scope on that page (admins with the admin area); anyone else gets the This PC section.
            if (section.startsWith("server:")) {
                const page = section.substring(7)
                if (root.canSwitchScope && root.player.admin.available && serverPane.pages.some(function (p) { return p.id === page })) {
                    serverPane.page = page
                    root.scope = "server"
                    return
                }
                section = page
            }
            root.scope = "pc"
            Qt.callLater(function () { root.jumpTo(section) })
        }
    }

    // B / Esc / Left inside the content: back to the column (the shell moves to the AppNav when this returns false).
    function handleBack(): bool { return secNav.backFromContent() }

    function sectionItem(name: string): Item {
        switch (name) {
        case "account": return secAccount
        case "hubs": return secHubs
        case "appearance": return secAppearance
        case "audio": return secAudio
        case "library": return secLibrary
        case "storage": return secStorage
        case "plugins": return secPlugins
        case "diagnostics": return secDiagnostics
        default: return secUpdates
        }
    }
    // "Edit Hub address…" of the connection check: scroll to Hubs and open the inline edit of the active Hub.
    function editActiveHubAddress() {
        root.jumpTo("hubs")
        for (let i = 0; i < root.savedHubs.length; ++i) {
            if (root.savedHubs[i].current) {
                root.editingHubId = root.savedHubs[i].hubId
                return
            }
        }
    }
    function jumpTo(name: string) {
        root.currentSection = name
        const target = Math.max(0, Math.min(sectionItem(name).y + content.y - Theme.space2, flick.contentHeight - flick.height))
        scrollAnim.stop()
        scrollAnim.to = target
        scrollAnim.start()
    }
    function syncCurrentFromScroll() {
        if (scrollAnim.running) {
            return
        }
        const probe = flick.contentY + 80
        let best = "account"
        for (const n of ["account", "updates", "hubs", "appearance", "audio", "library", "storage", "plugins", "diagnostics"]) {
            if (sectionItem(n).y + content.y <= probe) {
                best = n
            }
        }
        if (flick.contentY + flick.height >= flick.contentHeight - 2) {
            // At the end of the page the last sections cannot reach the probe line. A section the user jumped to
            // stays selected as long as its top is on screen (R-P18); otherwise the last section takes over.
            const cur = sectionItem(root.currentSection)
            const curTop = cur.y + content.y
            best = (curTop >= flick.contentY - 2 && curTop < flick.contentY + flick.height) ? root.currentSection : "diagnostics"
        }
        root.currentSection = best
    }
    function libraryNavText() {
        const n = root.player.libraryFolders.length
        if (n === 0) return qsTr("No folders")
        const g = root.player.libraryGameCount
        return Theme.plural(n, qsTr("folder"), qsTr("folders")) + " · " + Theme.plural(g, qsTr("game"), qsTr("games"))
    }
    function updatesLine(): string {
        const u = root.updates
        if (u.updateAvailable) {
            return qsTr("▲ Update available · %1").arg(u.availableVersion)
        }
        switch (u.state) {
        case "checking": return qsTr("Checking…")
        case "up_to_date": return qsTr("Up to date")
        case "error": return qsTr("Check failed")
        case "disabled": return qsTr("Off")
        default: return u.currentVersion
        }
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

        // Second column (board 9c-1): the This PC / Server switch, the scope line and the sections.
        Item {
            Layout.fillHeight: true
            Layout.preferredWidth: Theme.sidebar
            Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.rule }
            Flickable {
                id: secFlick
                anchors.fill: parent
                contentWidth: width
                contentHeight: secNav.implicitHeight + Theme.space7
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                UzColumnNav {
                    id: secNav
                    objectName: "settingsColumnNav"
                    content: flick
                    flickable: secFlick
                    onContentRequested: secNav.focusFirstIn(flick)
                    x: Theme.space2
                    y: Theme.space5 + Theme.space1
                    width: parent.width - 2 * Theme.space2 - 1
                    spacing: 0

                    // Settings for: This PC / Server (admins only).
                    ColumnLayout {
                        objectName: "settingsScopeRow"
                        visible: root.canSwitchScope
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        Layout.bottomMargin: 14
                        spacing: Theme.space05
                        UzLabel { text: qsTr("Settings for"); color: Theme.muted; font.pixelSize: Theme.roles.fieldLabel.size; font.weight: Theme.roles.fieldLabel.weight }
                        UzSegmented {
                            objectName: "settingsScope"
                            Layout.fillWidth: true
                            stretch: true
                            options: [{ value: "pc", label: qsTr("This PC"), name: "scopePc" }, { value: "server", label: qsTr("Server"), name: "scopeServer" }]
                            current: root.scope
                            onPicked: (value) => root.scope = value
                        }
                    }
                    UzLabel {
                        objectName: "settingsScopeHint"
                        Layout.fillWidth: true
                        Layout.leftMargin: Theme.space15
                        Layout.rightMargin: Theme.space15
                        Layout.bottomMargin: 14
                        text: root.scope === "server" ? qsTr("Affects everyone on %1.").arg(root.player.hubName) : qsTr("Only affects this PC and your account.")
                        color: Theme.muted
                        font.pixelSize: Theme.roles.small.size
                        lineHeight: Theme.roles.small.line
                        lineHeightMode: Text.FixedHeight
                        wrapMode: Text.WordWrap
                    }

                    Repeater {
                        // Count model, delegates by index: a JS array model rebuilt on a scope change loops the layout (Qt 6.4).
                        model: root.scope !== "pc" ? 0 : root.sectionDefs.length
                        delegate: UzColumnNavRow {
                            id: sec
                            required property int index
                            readonly property var modelData: root.sectionDefs[sec.index]
                            objectName: "section_" + modelData.id
                            label: sec.modelData.name
                            selected: root.currentSection === modelData.id
                            onClicked: root.jumpTo(sec.modelData.id)
                            UzLabel {
                                objectName: "sectionStatus_" + sec.modelData.id
                                Layout.fillWidth: true
                                font.pixelSize: Theme.roles.small.size
                                elide: Text.ElideRight
                                color: sec.modelData.id === "updates" && root.updates.updateAvailable ? Theme.accentText : Theme.dim
                                text: sec.modelData.id === "account" ? (root.player.userName !== "" ? root.player.userName : qsTr("This PC only"))
                                      : sec.modelData.id === "updates" ? root.updatesLine()
                                      : sec.modelData.id === "hubs" ? (root.activeHubName !== ""
                                                                       ? qsTr("%1 saved · %2 active").arg(root.savedHubs.length).arg(root.activeHubName)
                                                                       : qsTr("%1 saved").arg(root.savedHubs.length))
                                      : sec.modelData.id === "appearance" ? (root.player.appearance === "light" ? qsTr("Light")
                                                                             : root.player.appearance === "system" ? qsTr("System") : qsTr("Dark"))
                                      : sec.modelData.id === "audio" ? (root.player.audio.muted ? qsTr("Muted") : qsTr("Volume %1 %").arg(root.player.audio.volume))
                                      : sec.modelData.id === "library" ? root.libraryNavText()
                                      : sec.modelData.id === "storage" ? (root.player.localStorage.report.usedText !== undefined ? qsTr("%1 used").arg(root.player.localStorage.report.usedText) : qsTr("%1 of %2").arg(root.player.romCache.usedText).arg(root.player.romCache.limitText))
                                      : sec.modelData.id === "plugins" ? root.player.plugins.summary
                                      : qsTr("Logs and support")
                            }
                        }
                    }

                    // Settings for: Server (admins): the seven server pages of ServerSettingsPane.
                    Repeater {
                        model: root.scope === "server" ? serverPane.pages : []
                        delegate: serverPane.navDelegate
                    }
                }
            }
        }

        Flickable {
            id: flick
            objectName: "settingsScroll"
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: width
            contentHeight: content.implicitHeight + Theme.space7 + Theme.space2
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { }
            onContentYChanged: root.syncCurrentFromScroll()
            NumberAnimation { id: scrollAnim; target: flick; property: "contentY"; duration: Theme.durList; easing.type: Easing.OutCubic }

            ColumnLayout {
                id: content
                x: Theme.space5 + Theme.space1
                y: Theme.space5 + Theme.space1
                width: Math.min(Theme.contentMaxWidth, flick.width - 2 * (Theme.space5 + Theme.space1))   // left-aligned, never wider than 880 (P7); descriptions are capped per row
                // Hub card and update card sit on the label edge (14 in) and end where the controls end (72 in) on wide pages.
                readonly property bool wide: width >= Theme.settingsNarrowBelow
                spacing: Theme.space4

                // Page header: the one UzPageHeader (section label, title Light 40, once per page).
                UzPageHeader {
                    objectName: "settingsHeader"
                    titleName: "settingsTitle"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    number: 1
                    label: root.scope === "server" ? qsTr("Server") : qsTr("This PC")
                    labelMeta: root.scope === "server" ? root.player.hubName : ""
                    title: root.scope === "server" ? qsTr("Server settings") : qsTr("Settings")
                }
                // Server settings (3q-app-1 to 3q-app-7): the pages are chosen in the second column, shown here.
                ServerSettingsPane {
                    id: serverPane
                    visible: root.scope === "server"
                    player: root.player
                }

                // ------------------------------------------------------------------ MY ACCOUNT
                SettingsAccountSection {
                    id: secAccount
                    objectName: "accountSection"
                    visible: root.scope === "pc"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    player: root.player
                    host: root
                }

                // ------------------------------------------------------------------ UPDATES
                ColumnLayout {
                    id: secUpdates
                    visible: root.scope === "pc"
                    objectName: "updatesSection"
                    Layout.fillWidth: true
                    spacing: Theme.space15

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        UzSectionLabel { text: qsTr("Updates"); Layout.fillWidth: true }
                        UzTag {
                            objectName: "updatesPill"
                            visible: root.updates.state === "up_to_date" && !root.updates.updateAvailable
                            tone: "ok"
                            text: qsTr("Up to date · %1").arg(root.updates.currentVersion)
                        }
                    }

                    // After going back (6c-3): automatic updates are paused until the user resumes them.
                    Rectangle {
                        objectName: "updatesPausedCard"
                        visible: root.updates.paused
                        Layout.fillWidth: true
                        Layout.leftMargin: Theme.settingsDotSlot
                        Layout.rightMargin: content.wide ? Theme.settingsResetWidth : 0
                        implicitHeight: pausedCol.implicitHeight + 2 * Theme.space2
                        color: Theme.selected
                        Rectangle { width: 2; height: parent.height; color: Theme.accent }   // marker-start, no amber frame

                        ColumnLayout {
                            id: pausedCol
                            anchors.fill: parent
                            anchors.margins: Theme.space2
                            anchors.leftMargin: Theme.space3
                            spacing: Theme.space15
                            UzLabel {
                                objectName: "updatesPausedTitle"
                                Layout.fillWidth: true
                                text: root.updates.pausedTitle
                                font.pixelSize: Theme.roles.itemTitle.size
                                font.weight: Font.DemiBold
                                color: Theme.ink2
                                wrapMode: Text.WordWrap
                            }
                            UzLabel {
                                objectName: "updatesPausedDetail"
                                Layout.fillWidth: true
                                text: root.updates.pausedDetail
                                font.pixelSize: Theme.roles.input.size
                                color: Theme.ink2
                                wrapMode: Text.WordWrap
                            }
                            RowLayout {
                                spacing: Theme.space15
                                UzButton {
                                    objectName: "updatesUpdateToSkipped"
                                    visible: root.updates.skippedVersion !== "" && root.updates.canInstall
                                    implicitHeight: 36
                                    kind: "primary"
                                    busyOnClick: true
                                    text: qsTr("Update to %1").arg(root.updates.skippedVersion)
                                    onClicked: root.updates.updateToSkipped()
                                }
                                UzButton {
                                    objectName: "updatesResume"
                                    implicitHeight: 36
                                    text: qsTr("Resume updates")
                                    onClicked: root.updates.resumeUpdates()
                                }
                            }
                        }
                    }

                    // Update card
                    Rectangle {
                        objectName: "updateCard"
                        Layout.fillWidth: true
                        Layout.leftMargin: Theme.settingsDotSlot
                        Layout.rightMargin: content.wide ? Theme.settingsResetWidth : 0   // ends where the controls end (P5)
                        implicitHeight: cardCol.implicitHeight + 2 * Theme.space2
                        color: "transparent"
                        border.width: 1
                        border.color: root.updates.updateAvailable ? Theme.accent : Theme.rule2

                        ColumnLayout {
                            id: cardCol
                            anchors.fill: parent
                            anchors.margins: 18
                            spacing: Theme.space15

                            RowLayout {
                                spacing: Theme.space15
                                UzLabel {
                                    objectName: "updateCardTitle"
                                    text: root.updates.updateAvailable ? qsTr("▲ Update available")
                                          : (root.updates.state === "checking" ? qsTr("Checking for updates…")
                                             : root.updates.effectiveChannel === "off" ? qsTr("Updates are off")
                                             : qsTr("Uzume %1 is up to date").arg(root.updates.currentVersion))
                                    font.pixelSize: Theme.roles.itemTitle.size
                                    font.weight: Font.DemiBold
                                    color: root.updates.updateAvailable ? Theme.accentText : Theme.ink
                                }
                                UzMono {
                                    objectName: "updatesAvailableVersion"
                                    visible: root.updates.updateAvailable
                                    text: root.updates.availableVersion
                                    color: Theme.ink
                                    font.pixelSize: Theme.roles.input.size
                                }
                                Item { Layout.fillWidth: true }
                                UzSpinner { visible: root.updates.checking; size: 14 }
                            }
                            UzLabel {
                                objectName: "updatesStatus"
                                Layout.fillWidth: true
                                text: root.updates.updateAvailable
                                      ? qsTr("You have %1. Installs on the next start, never during a game.").arg(root.updates.currentVersion)
                                      : (root.updates.state === "up_to_date" || root.updates.state === "idle") && root.updates.effectiveChannel !== "off"
                                        ? qsTr("%1 channel · checked %2").arg(root.updates.effectiveChannel === "beta" ? qsTr("Beta") : qsTr("Stable")).arg(root.updates.lastCheckText)
                                      : root.updates.state === "idle" && root.updates.effectiveChannel === "off"
                                        ? qsTr("Choose Stable or Beta to check for updates.")
                                      : root.updates.statusText
                                wrapMode: Text.WordWrap
                                font.pixelSize: Theme.roles.input.size
                                color: root.updates.state === "error" ? Theme.toneColor("error") : Theme.ink2
                            }
                            Rectangle {
                                Layout.fillWidth: true
                                implicitHeight: 6
                                radius: 0
                                visible: root.updates.state === "downloading"
                                color: Theme.rule2
                                Rectangle {
                                    width: parent.width * root.updates.progress
                                    height: parent.height
                                    radius: 0
                                    color: Theme.accent
                                    Behavior on width { NumberAnimation { duration: Theme.durFast } }
                                }
                            }
                            RowLayout {
                                spacing: Theme.space15
                                visible: root.updates.notesUrl !== "" || root.updates.canInstall
                                UzButton {
                                    objectName: "updatesNotes"
                                    visible: root.updates.notesUrl !== ""
                                    implicitHeight: 36
                                    text: qsTr("What's new")
                                    onClicked: root.updates.openNotes()
                                }
                                UzButton {
                                    objectName: "updatesInstall"
                                    visible: root.updates.canInstall
                                    implicitHeight: 36
                                    kind: "primary"
                                    busyOnClick: true
                                    text: root.updates.state === "ready" ? qsTr("Restart and update") : qsTr("Install and restart")
                                    onClicked: root.updates.install()
                                }
                            }
                        }
                    }

                    // Rows
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        spacing: 0
                        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.rule2 }

                        UzSettingsRow {
                            objectName: "updateChannelRow"
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            resetMode: "none"
                            label: qsTr("Update channel")
                            descriptionName: "updateChannelHint"
                            description: root.updates.compiledChannel === "beta" ? qsTr("Stable: tested releases. Beta: earlier, less tested. This build is a beta.")
                                                                 : qsTr("Stable: tested releases. Beta: earlier, less tested.")
                            ctrl: "segment"
                            controlName: "updateChannelSegment"
                            values: [
                                { value: "off", label: qsTr("Off"), name: "updateChannelOff" },
                                { value: "stable", label: qsTr("Stable"), name: "updateChannelStable" },
                                { value: "beta", label: qsTr("Beta"), name: "updateChannelBeta" }
                            ]
                            current: root.updates.channelSetting === "default" ? root.updates.effectiveChannel : root.updates.channelSetting
                            onValuePicked: value => root.updates.setChannel(value)
                        }

                        UzSettingsRow {
                            objectName: "updateAutoInstallRow"
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            resetMode: "none"
                            label: qsTr("Install updates automatically")
                            description: root.updates.paused
                                         ? qsTr("Paused after going back. “Resume updates” turns it on again.")
                                         : root.updates.autoInstallAvailable
                                           ? qsTr("Downloaded in the background, applied on the next start.")
                                           : qsTr("Stable updates are only installed after you confirm.")
                            ctrl: "toggle"
                            toggleName: "updateAutoInstallToggle"
                            controlEnabled: root.updates.autoInstallAvailable && !root.updates.paused
                            checked: root.updates.autoInstall && !root.updates.paused
                            onToggled: on => root.updates.setAutoInstall(on)
                        }

                        UzSettingsRow {
                            objectName: "updatesLastCheckRow"
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            resetMode: "none"
                            label: qsTr("Last checked")
                            descriptionName: "updatesLastCheck"
                            description: root.updates.cadenceText !== ""
                                         ? qsTr("%1 · %2").arg(root.updates.lastCheckText).arg(root.updates.cadenceText)
                                         : root.updates.lastCheckText
                            ctrl: "buttons"
                            buttons: [{ name: "updatesCheckNow", text: qsTr("Check now"), busy: root.updates.checking, busyOnClick: true,
                                        enabled: !root.updates.checking && root.updates.state !== "downloading" }]
                            onButtonClicked: root.updates.checkNow()
                        }

                        UzSettingsRow {
                            objectName: "versionRow"
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            resetMode: "none"
                            label: qsTr("Version")
                            description: root.player.platformText
                            ctrl: "value"
                            controlName: "updatesVersion"
                            valueText: root.updates.currentVersion
                        }
                    }

                    // EARLIER VERSIONS (6c): go back to one of the last releases. A copy that cannot install by itself
                    // (portable, Linux, development) lists the releases with links only.
                    ColumnLayout {
                        objectName: "earlierVersions"
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        Layout.topMargin: Theme.space15
                        spacing: Theme.space15

                        UzSectionLabel { Layout.fillWidth: true; text: qsTr("Earlier versions · %1").arg(root.updates.earlierCount) }
                        UzLabel {
                            Layout.fillWidth: true
                            text: qsTr("Go back if something stopped working after an update. Settings stay; saves and states are on your server and are not touched. The list shows the last %1 releases.").arg(5)
                            wrapMode: Text.WordWrap
                            font.pixelSize: Theme.roles.input.size
                            color: Theme.ink2
                        }
                        UzLabel {
                            objectName: "earlierUnavailable"
                            visible: !root.updates.canGoBack && root.updates.earlierCount > 0
                            Layout.fillWidth: true
                            text: root.updates.goBackUnavailableText
                            wrapMode: Text.WordWrap
                            font.pixelSize: Theme.roles.input.size
                            color: Theme.accentText
                        }
                        UzLabel {
                            objectName: "earlierEmpty"
                            visible: root.updates.earlierCount === 0
                            Layout.fillWidth: true
                            text: root.updates.effectiveChannel === "off"
                                  ? qsTr("Choose Stable or Beta to see earlier versions.")
                                  : qsTr("No earlier version to go back to. The list fills after the next update check.")
                            wrapMode: Text.WordWrap
                            font.pixelSize: Theme.roles.input.size
                            color: Theme.muted
                        }
                        Repeater {
                            model: root.updates.earlierVersions
                            delegate: Rectangle {
                                id: earlierRow
                                required property var modelData
                                objectName: "earlierRow-" + modelData.version
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                implicitHeight: Theme.row48
                                color: "transparent"
                                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.rule2 }

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 16
                                    anchors.rightMargin: 12
                                    spacing: Theme.space15
                                    UzMono {
                                        text: earlierRow.modelData.version
                                        color: Theme.ink
                                        font.pixelSize: Theme.roles.body.size
                                    }
                                    UzLabel {
                                        objectName: "earlierState-" + earlierRow.modelData.version
                                        Layout.fillWidth: true
                                        Layout.minimumWidth: 0
                                        text: earlierRow.modelData.kind === "running" ? (earlierRow.modelData.text !== "" ? qsTr("Running · %1").arg(earlierRow.modelData.text) : qsTr("Running"))
                                                                                      : earlierRow.modelData.text
                                        elide: Text.ElideRight
                                        font.pixelSize: Theme.roles.input.size
                                        color: earlierRow.modelData.kind === "skipped" ? Theme.accentText : Theme.muted
                                    }
                                    UzButton {
                                        objectName: "earlierGoBack-" + earlierRow.modelData.version
                                        visible: earlierRow.modelData.kind === "earlier" && root.updates.canGoBack
                                        implicitHeight: 34
                                        text: qsTr("Go back…")
                                        onClicked: rollbackDialog.openFor(earlierRow.modelData.version)
                                    }
                                    UzButton {
                                        objectName: "earlierRelease-" + earlierRow.modelData.version
                                        visible: earlierRow.modelData.kind !== "running" && !(earlierRow.modelData.kind === "earlier" && root.updates.canGoBack)
                                                 && earlierRow.modelData.notesUrl !== ""
                                        implicitHeight: 34
                                        kind: "link"
                                        text: qsTr("View release")
                                        onClicked: root.updates.openRelease(earlierRow.modelData.notesUrl)
                                    }
                                }
                            }
                        }
                    }
                }

                // ------------------------------------------------------------------ HUBS
                ColumnLayout {
                    id: secHubs
                    visible: root.scope === "pc"
                    objectName: "hubsSection"
                    Layout.fillWidth: true
                    spacing: Theme.space15

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        Layout.rightMargin: content.wide ? Theme.settingsResetWidth - 8 : 0   // "+ Add hub" ends where the controls end (P1)
                        Eyebrow { objectName: "hubsEyebrow"; text: qsTr("Servers · %1").arg(root.savedHubs.length); Layout.fillWidth: true }
                        UzButton {
                            objectName: "settingsAddHubButton"
                            kind: "link"
                            text: qsTr("+ Add server")
                            onClicked: root.player.switchHub()
                        }
                    }

                    // Own row (not in the header): a long label must never widen the settings column.
                    UzButton {
                        objectName: "settingsLocalHubButton"
                        visible: root.player.localHub.available && !root.player.localHub.onThisPc && root.player.localHub.mode === ""
                        Layout.alignment: Qt.AlignLeft
                        Layout.leftMargin: Theme.settingsDotSlot
                        Layout.minimumWidth: 0
                        kind: "link"
                        text: qsTr("Set up a server on this PC")
                        onClicked: { root.player.switchHub(); root.player.localHub.start() }
                    }

                    RowLayout {
                        objectName: "settingsThisPcServerRow"
                        visible: root.player.localHub.mode !== ""
                        Layout.fillWidth: true
                        Layout.leftMargin: Theme.settingsDotSlot
                        Layout.rightMargin: content.wide ? Theme.settingsResetWidth : 0
                        spacing: Theme.space15
                        Text {
                            objectName: "settingsThisPcServerName"
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            elide: Text.ElideRight
                            color: Theme.ink
                            font.pixelSize: Theme.roles.body.size
                            text: root.player.localHub.onThisPcOnly ? qsTr("On this PC") : qsTr("This PC · shared")
                        }
                        UzButton {
                            objectName: "settingsShareButton"
                            visible: root.player.localHub.shareSupported && root.player.localHub.onThisPcOnly
                            kind: "outline"
                            text: qsTr("Share with others…")
                            onClicked: root.player.localHub.openShareDialog("intro")
                        }
                        UzButton {
                            objectName: "settingsShareAgainButton"
                            visible: root.player.localHub.shareAgainAvailable
                            kind: "outline"
                            text: qsTr("Share again…")
                            onClicked: root.player.localHub.shareAgain()
                        }
                    }

                    // Settings > Network does not exist yet: the connection check lives next to the Hubs it checks.
                    UzButton {
                        objectName: "settingsConnectionCheckButton"
                        visible: root.player.connectionCheck.available
                        Layout.alignment: Qt.AlignLeft
                        Layout.leftMargin: Theme.settingsDotSlot
                        Layout.minimumWidth: 0
                        kind: "link"
                        text: qsTr("Connection check…")
                        onClicked: root.player.connectionCheck.show()
                    }

                    LocalHubCard {
                        objectName: "localHubSettingsCard"
                        player: root.player
                        Layout.leftMargin: Theme.settingsDotSlot
                        Layout.rightMargin: content.wide ? Theme.settingsResetWidth : 0
                    }

                    // Stable model keyed by hub id: hubsChanged updates rows in place instead of rebuilding every delegate
                    // (a JS-array model recreates the nested Layouts on each change; Qt 6.4 crashes in
                    // QQuickGridLayoutBase::rearrange when that happens while the layouts are being polished).
                    ListModel {
                        id: hubsModel
                        dynamicRoles: true
                        function sync(list) {
                            for (var i = 0; i < list.length; i++) {
                                var found = -1
                                for (var j = i; j < count; j++) {
                                    if (get(j).hubId === list[i].hubId) { found = j; break }
                                }
                                if (found < 0) insert(i, list[i])
                                else {
                                    if (found !== i) move(found, i, 1)
                                    set(i, list[i])
                                }
                            }
                            if (count > list.length) remove(list.length, count - list.length)
                        }
                    }
                    Connections {
                        target: root.player
                        function onHubsChanged() { hubsModel.sync(root.player.hubs) }
                    }
                    Component.onCompleted: hubsModel.sync(root.player.hubs)

                    Repeater {
                        model: hubsModel
                        delegate: Rectangle {
                            id: hubRow
                            required property var model
                            readonly property var modelData: model
                            required property int index
                            property bool confirmRemove: false
                            readonly property bool editing: root.editingHubId === modelData.hubId && modelData.hubId !== ""
                            readonly property bool lost: modelData.pairingLost === true && !modelData.connected
                            visible: modelData.saved
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            Layout.leftMargin: Theme.settingsDotSlot
                            Layout.rightMargin: content.wide ? Theme.settingsResetWidth : 0
                            Layout.preferredHeight: visible ? rowCol.implicitHeight + 28 : 0
                            color: Theme.surface
                            border.width: modelData.connected ? 1.5 : 1
                            border.color: modelData.connected ? Theme.accent : Theme.rule

                            ColumnLayout {
                                id: rowCol
                                anchors.fill: parent
                                anchors.margins: 14
                                anchors.leftMargin: 16
                                anchors.rightMargin: 16
                                spacing: Theme.space15
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 24
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        Layout.minimumWidth: 0
                                        spacing: 2
                                        RowLayout {
                                            spacing: Theme.space1
                                            UzLabel {
                                                objectName: "hubName_" + hubRow.modelData.hubId
                                                text: hubRow.modelData.name
                                                font.pixelSize: Theme.roles.itemTitle.size
                                                font.weight: Font.Medium
                                                elide: Text.ElideRight
                                                Layout.maximumWidth: 280
                                            }
                                            UzTag {
                                                objectName: "hubState_" + hubRow.modelData.hubId
                                                small: true
                                                tone: hubRow.modelData.connected ? "ok" : (hubRow.lost ? "amber" : (hubRow.modelData.tone === "neutral" ? "neutral" : hubRow.modelData.tone))
                                                text: hubRow.modelData.connected ? qsTr("● Connected · active")
                                                      : hubRow.lost ? qsTr("Pairing lost")
                                                      : (hubRow.modelData.status === "idle" ? qsTr("Not connected") : hubRow.modelData.statusText)
                                            }
                                        }
                                        UzMono {
                                            Layout.fillWidth: true
                                            text: hubRow.modelData.detail
                                            elide: Text.ElideRight
                                            HoverHandler { id: detailHover }
                                            ToolTip.visible: detailHover.hovered && (hubRow.modelData.lastConnectedFull || "") !== ""
                                            ToolTip.text: hubRow.modelData.lastConnectedFull || ""
                                            ToolTip.delay: 400
                                        }
                                    }
                                    // Control column (280, right edge = the controls of the rows): [Switch | Pair again] [Edit] [⋯].
                                    RowLayout {
                                        Layout.preferredWidth: content.wide ? Theme.settingsControlWidth - 16 : -1
                                        Layout.alignment: Qt.AlignVCenter
                                        spacing: 6
                                        visible: !hubRow.confirmRemove
                                        Item { Layout.fillWidth: true }
                                        UzButton {
                                            objectName: "hubSwitch_" + hubRow.modelData.hubId
                                            visible: !hubRow.modelData.connected && !hubRow.lost
                                            implicitHeight: 32
                                            hPadding: 24
                                            busy: hubRow.modelData.status === "connecting"
                                            busyOnClick: true
                                            text: qsTr("Switch")
                                            enabled: hubRow.modelData.status !== "connecting"
                                            onClicked: root.player.switchToHub(hubRow.modelData.hubId)
                                        }
                                        UzButton {
                                            objectName: "hubPairAgain_" + hubRow.modelData.hubId
                                            visible: hubRow.lost
                                            implicitHeight: 32
                                            hPadding: 24
                                            busyOnClick: true
                                            text: qsTr("Pair again")
                                            onClicked: root.player.switchToHub(hubRow.modelData.hubId)
                                        }
                                        UzButton {
                                            objectName: "hubEdit_" + hubRow.modelData.hubId
                                            implicitHeight: 32
                                            hPadding: 24
                                            text: qsTr("Edit")
                                            onClicked: {
                                                hubEditAddress.text = hubRow.modelData.host + ":" + hubRow.modelData.port
                                                hubEditError.extra = ""
                                                root.editingHubId = hubRow.editing ? "" : hubRow.modelData.hubId
                                            }
                                        }
                                        MoreMenu {
                                            objectName: "hubMenu_" + hubRow.modelData.hubId
                                            tooltip: qsTr("More actions for %1").arg(hubRow.modelData.name)
                                            items: [{ name: "remove", label: qsTr("Remove server…"), danger: true, divider: false }]
                                            onActivated: name => { if (name === "remove") hubRow.confirmRemove = true }
                                        }
                                    }
                                }

                                // Connect automatically: part of the active Hub's card (3p-3).
                                // Anchored (not a RowLayout): the toggle's right edge is the card's inner right edge, the same
                                // edge as the [Edit][⋯] group above, at every width.
                                Item {
                                    visible: hubRow.modelData.connected && !hubRow.confirmRemove
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                    implicitHeight: Math.max(autoText.implicitHeight, autoToggle.implicitHeight)
                                    ColumnLayout {
                                        id: autoText
                                        anchors.left: parent.left
                                        anchors.right: autoToggle.left
                                        anchors.rightMargin: 24
                                        anchors.verticalCenter: parent.verticalCenter
                                        spacing: 2
                                        UzLabel { text: qsTr("Connect automatically on startup"); font.pixelSize: Theme.roles.body.size; Layout.fillWidth: true }
                                        UzLabel { text: qsTr("Uses this server when Uzume starts."); color: Theme.muted; font.pixelSize: Theme.roles.small.size; Layout.fillWidth: true }
                                    }
                                    UzToggle {
                                        id: autoToggle
                                        objectName: "settingsAutoConnectToggle"
                                        implicitWidth: 40   // the track only: a Switch without text still reserves indicator + spacing
                                        anchors.right: parent.right
                                        anchors.verticalCenter: parent.verticalCenter
                                        checked: root.player.autoConnect
                                        onToggled: root.player.autoConnect = checked
                                        Accessible.name: qsTr("Connect automatically on startup")
                                    }
                                }

                                // Inline edit: one field "host:port" (hub id, fingerprint, sign-in and user stay).
                                ColumnLayout {
                                    id: editBox
                                    objectName: "hubEditRow_" + hubRow.modelData.hubId
                                    visible: hubRow.editing && !hubRow.confirmRemove
                                    Layout.fillWidth: true
                                    spacing: Theme.space1
                                    // "host" or "host:port"; the port defaults to 8443 when left out.
                                    readonly property var parts: {
                                        const t = hubEditAddress.text.trim()
                                        const k = t.lastIndexOf(":")
                                        if (k < 0 || t.indexOf("://") >= 0 && k === t.indexOf("://")) return { host: t, port: "8443" }
                                        return { host: t.slice(0, k), port: t.slice(k + 1) }
                                    }
                                    readonly property var messages: root.player.validateHubAddress(parts.host, parts.port)
                                    readonly property bool valid: messages.length === 0 && hubEditAddress.text.trim() !== ""
                                    readonly property bool changed: hubEditAddress.text.trim() !== hubRow.modelData.host + ":" + hubRow.modelData.port

                                    Rectangle { Layout.fillWidth: true; height: 1; color: Theme.rule2 }
                                    Eyebrow { text: qsTr("Edit · %1").arg(hubRow.modelData.name) }
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: Theme.space15
                                        UzLabel { text: qsTr("Address"); color: Theme.muted; font.pixelSize: Theme.roles.input.size; Layout.preferredWidth: 70 }
                                        UzField {
                                            id: hubEditAddress
                                            objectName: "hubEditAddress_" + hubRow.modelData.hubId
                                            Layout.fillWidth: true
                                            Layout.minimumWidth: 0
                                            implicitHeight: 36
                                            invalid: !editBox.valid && hubEditAddress.text.trim() !== ""
                                            placeholderText: qsTr("hub.example.com:8443")
                                            inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoPredictiveText
                                            onAccepted: hubEditSave.clicked()
                                            Keys.onEscapePressed: root.editingHubId = ""   // Esc cancels the inline edit (R-P4)
                                        }
                                        UzButton {
                                            objectName: "hubEditCancel_" + hubRow.modelData.hubId
                                            implicitHeight: 36
                                            text: qsTr("Cancel")
                                            onClicked: root.editingHubId = ""
                                        }
                                        UzButton {
                                            id: hubEditSave
                                            objectName: "hubEditSave_" + hubRow.modelData.hubId
                                            implicitHeight: 36
                                            kind: "primary"
                                            busyOnClick: true
                                            text: qsTr("Save")
                                            enabled: editBox.valid && editBox.changed
                                            onClicked: {
                                                if (!editBox.valid) {
                                                    return
                                                }
                                                if (root.player.editHub(hubRow.modelData.hubId, editBox.parts.host, editBox.parts.port)) {
                                                    hubEditError.extra = ""
                                                    root.editingHubId = ""
                                                } else {
                                                    hubEditError.extra = qsTr("Another saved server already uses this address")
                                                }
                                            }
                                        }
                                    }
                                    UzLabel {
                                        id: hubEditError
                                        objectName: "hubEditError_" + hubRow.modelData.hubId
                                        property string extra: ""
                                        Layout.fillWidth: true
                                        visible: text !== "" && hubEditAddress.text.trim() !== ""
                                        text: editBox.messages.length > 0 ? editBox.messages.join(" · ") : extra
                                        wrapMode: Text.WordWrap
                                        font.pixelSize: Theme.roles.small.size
                                        color: Theme.danger
                                    }
                                    UzLabel {
                                        objectName: "hubEditHint_" + hubRow.modelData.hubId
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                        font.pixelSize: Theme.roles.small.size
                                        color: Theme.muted
                                        text: qsTr("host:port, without https://. If the server shows a different certificate, Uzume asks you to compare it.")
                                    }
                                }

                                RowLayout {
                                    objectName: "hubRemoveConfirm_" + hubRow.modelData.hubId
                                    Layout.fillWidth: true
                                    spacing: Theme.space15
                                    visible: hubRow.confirmRemove
                                    UzLabel {
                                        Layout.fillWidth: true
                                        Layout.minimumWidth: 0
                                        wrapMode: Text.WordWrap
                                        font.pixelSize: Theme.roles.small.size
                                        color: Theme.ink
                                        text: hubRow.modelData.connected
                                              ? qsTr("Remove %1 from Uzume? Running Sessions end and you return to the connection screen. Saves on the server are not deleted.").arg(hubRow.modelData.name)
                                              : qsTr("Remove %1 from Uzume? Saves on the server are not deleted.").arg(hubRow.modelData.name)
                                    }
                                    UzButton {
                                        objectName: "hubRemoveCancel_" + hubRow.modelData.hubId
                                        implicitHeight: 32
                                        text: qsTr("Cancel")
                                        onClicked: hubRow.confirmRemove = false
                                    }
                                    UzButton {
                                        objectName: "hubRemoveConfirmButton_" + hubRow.modelData.hubId
                                        implicitHeight: 32
                                        kind: "primary"
                                        busyOnClick: true
                                        text: qsTr("Remove")
                                        onClicked: root.player.removeHub(hubRow.modelData.hubId)
                                    }
                                }
                            }
                        }
                    }

                    UzLabel {
                        objectName: "hubsEmpty"
                        visible: root.player.hubs.length === 0
                        Layout.fillWidth: true
                        text: qsTr("No server saved yet.")
                        color: Theme.muted
                        font.pixelSize: Theme.roles.input.size
                    }
                }

                // ------------------------------------------------------------------ APPEARANCE
                ColumnLayout {
                    id: secAppearance
                    visible: root.scope === "pc"
                    objectName: "appearanceSection"
                    Layout.fillWidth: true
                    spacing: Theme.space15
                    UzSectionLabel { Layout.fillWidth: true; text: qsTr("Appearance") }
                    Rectangle { Layout.fillWidth: true; height: 1; color: Theme.rule; Layout.topMargin: -Theme.space05 }
                    UzSettingsRow {
                        objectName: "themeRow"
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        resetMode: "none"
                        label: qsTr("Theme")
                        description: qsTr("Sumi is dark, Kinari light. The game view always stays dark.")
                        ctrl: "segment"
                        controlName: "appearanceSegment"
                        values: [
                            { value: "dark", label: qsTr("Dark"), name: "appearanceDark" },
                            { value: "light", label: qsTr("Light"), name: "appearanceLight" },
                            { value: "system", label: qsTr("System"), name: "appearanceSystem" }
                        ]
                        current: root.player.appearance
                        onValuePicked: value => root.player.appearance = value
                    }
                    UzSettingsRow {
                        objectName: "textSizeRow"
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        resetMode: "none"
                        label: qsTr("Text size")
                        description: qsTr("TV makes all text larger and every target at least 44 px.")
                        ctrl: "segment"
                        controlName: "textSizeSegment"
                        values: [
                            { value: "100", label: qsTr("100 %"), name: "textSize100" },
                            { value: "125", label: qsTr("125 %"), name: "textSize125" },
                            { value: "tv", label: qsTr("TV"), name: "textSizeTv" }
                        ]
                        current: root.player.textSize
                        onValuePicked: value => root.player.textSize = value
                    }
                    UzSettingsRow {
                        objectName: "reduceMotionRow"
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        resetMode: "none"
                        label: qsTr("Reduce motion")
                        description: qsTr("Stops the ensō, slides and fades. Follows the system setting until you change it.")
                        ctrl: "toggle"
                        controlName: "reduceMotionToggle"
                        checked: root.player.reduceMotion
                        onToggled: on => root.player.reduceMotion = on
                    }
                }

                // ------------------------------------------------------------------ AUDIO & VIDEO (6g-1)
                ColumnLayout {
                    id: secAudio
                    visible: root.scope === "pc"
                    objectName: "audioSection"
                    Layout.fillWidth: true
                    spacing: Theme.space15
                    UzSectionLabel { Layout.fillWidth: true; text: qsTr("Audio & video") }
                    Rectangle { Layout.fillWidth: true; height: 1; color: Theme.rule; Layout.topMargin: -Theme.space05 }
                    UzSettingsRow {
                        objectName: "audioOutputRow"
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        resetMode: "none"
                        label: qsTr("Output")
                        description: qsTr("Where the app plays sound. A running game switches on its next start; if the device is gone, the system default is used.")
                        ctrl: "select"
                        controlName: "audioOutputSelect"
                        values: root.player.audio.outputDevices
                        current: root.player.audio.outputDevice
                        onValuePicked: value => root.player.audio.outputDevice = value
                    }
                    UzSettingsRow {
                        objectName: "audioMuteBackgroundRow"
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        resetMode: "none"
                        label: qsTr("Mute when in the background")
                        description: qsTr("Sessions you watch stay audible.")
                        ctrl: "toggle"
                        toggleName: "audioMuteBackgroundToggle"
                        checked: root.player.audio.muteInBackground
                        onToggled: on => root.player.audio.muteInBackground = on
                    }
                    UzSettingsRow {
                        objectName: "audioVolumeStepRow"
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        resetMode: "none"
                        label: qsTr("Volume steps")
                        description: qsTr("How much the volume hotkeys and the Sound slider change at a time.")
                        ctrl: "segment"
                        controlName: "audioVolumeStepSegment"
                        values: [
                            { value: "5", label: qsTr("5 %"), name: "audioStep5" },
                            { value: "10", label: qsTr("10 %"), name: "audioStep10" }
                        ]
                        current: String(root.player.audio.volumeStep)
                        onValuePicked: value => root.player.audio.volumeStep = Number(value)
                    }
                    UzSettingsRow {
                        objectName: "audioShowVolumeRow"
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        resetMode: "none"
                        label: qsTr("Show volume on screen")
                        description: qsTr("A short notice in the game when you change the volume with a hotkey.")
                        ctrl: "toggle"
                        toggleName: "audioShowVolumeToggle"
                        checked: root.player.audio.showVolume
                        onToggled: on => root.player.audio.showVolume = on
                    }
                }

                // ------------------------------------------------------------------ LIBRARY FOLDERS
                LibraryFoldersSection {
                    id: secLibrary
                    visible: root.scope === "pc"
                    objectName: "libraryFoldersSection"
                    Layout.fillWidth: true
                    player: root.player
                }

                // ------------------------------------------------------------------ STORAGE (overview, clean-up, ROM cache)
                ColumnLayout {
                    id: secStorage
                    visible: root.scope === "pc"
                    objectName: "storageSection"
                    Layout.fillWidth: true
                    spacing: Theme.space15
                    property bool confirmClear: false
                    property string clearResult: ""
                    UzSectionLabel { Layout.fillWidth: true; text: qsTr("Storage") }
                    Rectangle { Layout.fillWidth: true; height: 1; color: Theme.rule; Layout.topMargin: -Theme.space05 }
                    LocalStorageBlock {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        player: root.player
                    }
                    UzSettingsRow {
                        objectName: "romCacheLimitRow"
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        resetMode: "none"
                        label: qsTr("Size limit")
                        meta: qsTr("Used: %1").arg(root.player.romCache.usedText)
                        descriptionName: "romCacheLimitHint"
                        description: qsTr("Downloaded games are kept on this device. When the limit is exceeded, the least recently played games are removed; the running game and downloads in progress are never removed. They are downloaded again when needed.")
                        ctrl: "select"
                        controlName: "romCacheLimitSelect"
                        values: root.player.romCache.limitOptions
                        current: String(root.player.romCache.limitBytes)
                        onValuePicked: value => root.player.setRomCacheLimit(Number(value))
                    }
                    UzSettingsRow {
                        objectName: "romCacheClearRow"
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        resetMode: "none"
                        label: qsTr("Clear ROM cache")
                        descriptionName: "romCacheClearHint"
                        description: secStorage.clearResult !== "" ? secStorage.clearResult
                                     : qsTr("Removes all downloaded games except the running one. Saves are not affected.")
                        ctrl: "buttons"
                        buttons: [{ name: "romCacheClear", text: qsTr("Clear…"), enabled: root.player.romCache.clearableBytes > 0 && !secStorage.confirmClear }]
                        onButtonClicked: { secStorage.clearResult = ""; secStorage.confirmClear = true }
                    }
                    RowLayout {
                        objectName: "romCacheClearConfirm"
                        Layout.fillWidth: true
                        spacing: Theme.space15
                        visible: secStorage.confirmClear
                        UzLabel {
                            objectName: "romCacheClearConfirmText"
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            wrapMode: Text.WordWrap
                            font.pixelSize: Theme.roles.small.size
                            color: Theme.ink
                            text: qsTr("Remove cached ROMs and free %1? Games are downloaded again when you play them. Saves are not deleted.").arg(root.player.romCache.clearableText)
                        }
                        UzButton {
                            objectName: "romCacheClearCancel"
                            implicitHeight: 32
                            text: qsTr("Cancel")
                            onClicked: secStorage.confirmClear = false
                        }
                        UzButton {
                            objectName: "romCacheClearConfirmButton"
                            implicitHeight: 32
                            kind: "primary"
                            text: qsTr("Clear cache")
                            onClicked: {
                                const r = root.player.clearRomCache()
                                secStorage.confirmClear = false
                                secStorage.clearResult = qsTr("Freed %1 (%2 removed).").arg(r.freedText).arg(Theme.plural(r.removedFiles, qsTr("file"), qsTr("files")))
                            }
                        }
                    }
                }

                // ------------------------------------------------------------------ PLUGINS
                PluginsSection {
                    id: secPlugins
                    visible: root.scope === "pc"
                    objectName: "pluginsSection"
                    Layout.fillWidth: true
                    player: root.player
                    hubName: root.activeHubName
                }

                // ------------------------------------------------------------------ DIAGNOSTICS
                ColumnLayout {
                    id: secDiagnostics
                    visible: root.scope === "pc"
                    objectName: "diagnosticsSection"
                    Layout.fillWidth: true
                    spacing: Theme.space15
                    UzSectionLabel { Layout.fillWidth: true; text: qsTr("Diagnostics") }
                    Rectangle { Layout.fillWidth: true; height: 1; color: Theme.rule; Layout.topMargin: -Theme.space05 }
                    UzSettingsRow {
                        objectName: "logFileRow"
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        resetMode: "none"
                        label: qsTr("Log file")
                        descriptionName: "logFilePath"
                        description: root.player.logFile !== "" ? root.player.logFile : qsTr("File logging is not active")
                        ctrl: "buttons"
                        buttons: [
                            { name: "logCopyPath", text: copied.running ? qsTr("Copied") : qsTr("Copy path"), visible: root.player.logFile !== "" },
                            { name: "logFolderOpen", text: qsTr("Open folder"), visible: root.player.logFile !== "" }
                        ]
                        onButtonClicked: name => {
                            if (name === "logCopyPath") {
                                clip.text = root.player.logFile
                                clip.selectAll()
                                clip.copy()
                                copied.restart()
                            } else if (name === "logFolderOpen") {
                                root.player.openLogFolder()
                            }
                        }
                        Timer { id: copied; interval: 1500 }
                    }
                    TextEdit { id: clip; visible: false }
                    UzSettingsRow {
                        objectName: "collectLogsRow"
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        resetMode: "none"
                        label: qsTr("Collect logs")
                        descriptionName: "collectLogsStatus"
                        description: root.collectMessage !== "" ? root.collectMessage
                            : root.player.collectingDiagnostics ? qsTr("Collecting…")
                            : root.player.connectionCheck.hasAttachment
                              ? qsTr("Packs the logs and system information into one zip file to send along with a bug report. Includes the last connection check. Contains IP addresses and host names, no passwords or tokens.")
                            : qsTr("Packs the logs and system information into one zip file to send along with a bug report. Contains IP addresses and host names, no passwords or tokens.")
                        ctrl: "buttons"
                        buttons: [
                            { name: "collectLogs", text: qsTr("Collect logs…"), visible: !root.player.collectingDiagnostics },
                            { name: "collectShow", text: qsTr("Show in folder"), visible: root.collectedPath !== "" }
                        ]
                        onButtonClicked: name => {
                            if (name === "collectLogs") {
                                collectDialog.open()
                            } else if (name === "collectShow") {
                                root.player.showInFolder(root.collectedPath)
                            }
                        }
                    }
                    UzSettingsRow {
                        objectName: "connectionCheckRow"
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        resetMode: "none"
                        label: qsTr("Connection check")
                        description: root.player.connectionCheck.available
                            ? qsTr("Tests whether Sessions can run directly between PCs or only through the relay of %1.").arg(root.activeHubName !== "" ? root.activeHubName : qsTr("the server"))
                            : qsTr("Connect to a server first.")
                        ctrl: "buttons"
                        buttons: [
                            { name: "connectionCheck", text: qsTr("Run check…"), visible: root.player.connectionCheck.available }
                        ]
                        onButtonClicked: name => { if (name === "connectionCheck") root.player.connectionCheck.show() }
                    }
                    UzSettingsRow {
                        objectName: "reportProblemRow"
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        resetMode: "none"
                        label: qsTr("Report a problem")
                        description: qsTr("Sends a short description with logs and settings to the admins of your server. You see what is included before it goes out.")
                        ctrl: "buttons"
                        buttons: [{ name: "report", text: qsTr("Report a problem…") }]
                        onButtonClicked: name => { if (name === "report") root.player.report.show() }
                    }
                    Loader {
                        id: collectDialog
                        active: false
                        source: "CollectLogsDialog.qml"
                        function open() {
                            if (item) {
                                item.prepare()
                                item.open()
                            } else {
                                active = true
                            }
                        }
                        onLoaded: {
                            item.player = root.player
                            item.prepare()
                            item.open()
                        }
                        onStatusChanged: if (status === Loader.Error) {
                            root.collectMessage = qsTr("The file dialog is not available in this installation.")
                        }
                    }
                    Connections {
                        target: root.player
                        function onDiagnosticsCollected(ok, path, message) {
                            root.collectedPath = ok ? path : ""
                            root.collectMessage = ok ? qsTr("Saved %1").arg(path) : qsTr("Could not save the bundle: %1").arg(message)
                        }
                    }
                }

                // Below 1440 px the help is a "Help" disclosure at the end of the content, not a fourth column.
                UzHelpColumn {
                    objectName: "settingsHelpDisclosure"
                    inline: true
                    visible: root.scope === "pc" && !root.helpOn
                    Layout.fillWidth: true
                    items: root.helpItems
                }
            }
        }
        // Open column: help of the current section (boards 9c-1, 6l-1).
        UzHelpColumn {
            objectName: "settingsHelp"
            visible: root.helpOn
            Layout.preferredWidth: Theme.openColumn
            Layout.fillHeight: true
            Layout.topMargin: Theme.space5 + Theme.space1
            Layout.rightMargin: Theme.space5 + Theme.space1
            label: qsTr("Uzume · Settings · This PC")
            items: root.helpItems
        }
    }

    RollbackDialog {
        id: rollbackDialog
        objectName: "rollbackDialogHost"
        updates: root.updates
    }
}
