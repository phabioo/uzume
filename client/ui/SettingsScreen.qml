import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// 3p: Settings, the one place for everything you set on this PC. Section column (grouped: You, Playing, This PC) | the page of the
// selected section, which starts at the top and scrolls on its own. Emulation, Controls and Hotkeys are the former Emulation and
// Controllers screens (EmulationContent, ControllersContent, HotkeysContent). Saved on this device only.
Rectangle {
    id: root
    required property PlayerController player
    readonly property var updates: root.player.updates
    property string currentSection: "account"
    property string collectMessage: ""
    property string collectedPath: ""
    property string editingHubId: ""
    // The sections under their group heads. "library" (the old Library folders section) is an alias of "storage".
    readonly property var sectionGroups: [
        { head: qsTr("You"), items: [
            { id: "account", name: qsTr("Account") },
            { id: "appearance", name: qsTr("Appearance") } ] },
        { head: qsTr("Playing"), items: [
            { id: "audio", name: qsTr("Picture & sound") },
            { id: "controllers", name: qsTr("Controls") },
            { id: "hotkeys", name: qsTr("Hotkeys") },
            { id: "emulation", name: qsTr("Emulation") } ] },
        { head: qsTr("This PC"), items: [
            { id: "storage", name: qsTr("Folders & storage") },
            { id: "hubs", name: qsTr("Servers") },
            { id: "plugins", name: qsTr("Plugins") },
            { id: "updates", name: qsTr("Updates") },
            { id: "diagnostics", name: qsTr("Help & diagnostics") } ] }
    ]
    readonly property var sectionAliases: ({ "library": "storage" })
    function sectionName(id: string): string {
        for (const g of root.sectionGroups) {
            for (const it of g.items) {
                if (it.id === id) return it.name
            }
        }
        return ""
    }
    function isSection(id: string): bool { return root.sectionName(id) !== "" }
    color: Theme.ground
    // A page that opens without a focused control takes the focus itself (as the former Emulation and Controllers pages did).
    onVisibleChanged: if (visible) keyScope.forceActiveFocus()
    Component.onCompleted: Qt.callLater(function () { if (root.visible && !keyScope.activeFocus) keyScope.forceActiveFocus() })

    // Open help column (boards 9c-1, 6l-1): only on windows wide enough to keep the rows in their wide layout.
    // Controls shows the input test in this column from 1440 px on instead of the help.
    readonly property bool inputTestColumn: root.currentSection === "controllers" && root.width >= Theme.columnsCompactMax
    readonly property bool helpOn: root.width >= 1480 && !root.inputTestColumn
    readonly property var helpItems: {
        switch (root.currentSection) {
        case "appearance": return [{ text: qsTr("Theme is per PC; your favourites and collections follow your account.") }]
        case "storage": return [{ text: qsTr("Uzume tells the server which games you have; the files stay on this PC.") },
                                { title: qsTr("Nothing on the server"), text: qsTr("Clean-up only removes copies on this PC: games that download again, unused cores. Saves are always on %1 and never part of a clean-up.").arg(root.player.hubName !== "" ? root.player.hubName : qsTr("your server")) }]
        case "emulation": return [{ title: qsTr("Here and per game"), text: qsTr("Emulation is how games look and feel on this PC. A game can keep its own values under Game overrides.") },
                                  { text: qsTr("Only options the core reports are listed. Uzume itself controls render mode, screen layout and firmware.") }]
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

    // "Plugin settings" / "Notice settings…" of a plugin surface: open Plugins, highlight the plugin's row.
    Connections {
        target: root.player
        function onPluginSettingsRequested(pluginId: string) {
            Qt.callLater(function () {
                root.jumpTo("plugins")
                secPlugins.highlightId = pluginId
            })
        }
    }

    Connections {
        target: root.player
        function onSettingsSectionRequested(section: string) {
            // The server's own settings are a tab of the Server screen (showSettingsSection handles "server:<page>" there); here a
            // "server:<page>" that got through opens the This PC section of the same name.
            if (section.startsWith("server:")) section = section.substring(7)
            root.jumpTo(section)
        }
    }

    // B / Esc / Left inside the content: back to the column (the shell moves to the AppNav when this returns false).
    function handleBack(): bool { return secNav.backFromContent() }

    // "Edit Hub address…" of the connection check: open Servers and the inline edit of the active Hub.
    function editActiveHubAddress() {
        root.jumpTo("hubs")
        for (let i = 0; i < root.savedHubs.length; ++i) {
            if (root.savedHubs[i].current) {
                root.editingHubId = root.savedHubs[i].hubId
                return
            }
        }
    }
    // Shows one section (an old id works through its alias); its page starts at the top.
    function jumpTo(name: string) {
        const leaving = root.keyContent()
        if (leaving) leaving.cancelCaptures()   // a half-finished "Press a key…" does not outlive its section
        const id = root.sectionAliases[name] !== undefined ? root.sectionAliases[name] : name
        root.currentSection = root.isSection(id) ? id : "account"
        flick.contentY = 0
    }
    function libraryNavText() {
        const n = root.player.libraryFolders.length
        if (n === 0) return qsTr("No folders")
        const g = root.player.libraryGameCount
        return Theme.plural(n, qsTr("folder"), qsTr("folders")) + " · " + Theme.plural(g, qsTr("game"), qsTr("games"))
    }
    // Storage on this PC: percent of the volume in use (the report of the storage block), -1 while nothing is measured.
    readonly property real storageUsedPercent: {
        const r = root.player.localStorage.report
        return r.totalBytes > 0 && r.freeBytes >= 0 ? Math.round((r.totalBytes - r.freeBytes) * 100 / r.totalBytes) : -1
    }
    readonly property bool storageNearlyFull: root.storageUsedPercent >= 90
    // The meta line of a section in the column.
    function sectionStatus(id: string): string {
        switch (id) {
        case "account": return root.player.userName !== "" ? root.player.userName : qsTr("This PC only")
        case "appearance": return (root.player.appearance === "light" ? qsTr("Kinari") : root.player.appearance === "system" ? qsTr("System") : qsTr("Sumi"))
                                  + " · " + (root.player.textSize === "tv" ? qsTr("text TV") : qsTr("text %1 %").arg(root.player.textSize))
        case "audio": return root.player.audio.muted ? qsTr("Muted") : qsTr("Volume %1 %").arg(root.player.audio.volume)
        case "controllers": {
            const pads = root.player.controllers.devices.filter(function (d) { return d.kind === "gamepad" }).length
            return pads > 0 ? qsTr("Keyboard · %1").arg(Theme.plural(pads, qsTr("gamepad"), qsTr("gamepads"))) : qsTr("Keyboard")
        }
        case "hotkeys": return root.player.controllers.hotkeysChangedCount > 0 ? qsTr("%1 changed").arg(root.player.controllers.hotkeysChangedCount) : qsTr("Defaults")
        case "emulation": return root.player.emulation.changedCount > 0 ? qsTr("%1 changed").arg(root.player.emulation.changedCount) : qsTr("Defaults")
        case "storage": return root.storageNearlyFull ? qsTr("%1 % full").arg(root.storageUsedPercent) : root.libraryNavText()
        case "hubs": return root.activeHubName !== "" ? qsTr("%1 saved · %2 active").arg(root.savedHubs.length).arg(root.activeHubName)
                                                      : qsTr("%1 saved").arg(root.savedHubs.length)
        case "plugins": return root.player.plugins.summary
        case "updates": return root.updatesLine()
        default: return qsTr("Logs and support")
        }
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

    // The content of Controls and Hotkeys that takes key presses (capture of a binding, input test), else null.
    function keyContent(): var {
        if (root.currentSection === "controllers") return secControllers.item
        if (root.currentSection === "hotkeys") return secHotkeys.item
        return null
    }

    // Holds the focus of the page so that key presses reach Controls and Hotkeys wherever the focus sits inside Settings.
    FocusScope {
        id: keyScope
        anchors.fill: parent
        Keys.onPressed: event => {
            const c = root.keyContent()
            if (c && c.handleKeyPressed(event)) event.accepted = true
        }
        Keys.onReleased: event => {
            const c = root.keyContent()
            if (c) c.handleKeyReleased(event)
        }
        onActiveFocusChanged: if (!activeFocus) root.player.controllers.clearTestKeys()

        UzPageHints {
            id: pageHints
            player: root.player
            search: root.currentSection === "emulation"
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

            // Second column (board 9c-1): the sections.
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

                        // Groups by count and rows by index: a JS array model rebuilt on a change loops the layout (Qt 6.4).
                        Repeater {
                            model: root.sectionGroups.length
                            delegate: ColumnLayout {
                                id: grp
                                required property int index
                                readonly property var group: root.sectionGroups[grp.index]
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                spacing: 0
                                UzSectionLabel {
                                    objectName: "sectionHead_" + grp.index
                                    Layout.fillWidth: true
                                    Layout.leftMargin: Theme.space15
                                    Layout.topMargin: grp.index === 0 ? 0 : Theme.space3
                                    Layout.bottomMargin: Theme.space05
                                    text: grp.group.head
                                }
                                Repeater {
                                    model: grp.group.items.length
                                    delegate: UzColumnNavRow {
                                        id: sec
                                        required property int index
                                        readonly property var modelData: grp.group.items[sec.index]
                                        objectName: "section_" + modelData.id
                                        label: sec.modelData.name
                                        selected: root.currentSection === modelData.id
                                        // Board 9c-1: the storage section carries the warning square while the volume is nearly full.
                                        warn: sec.modelData.id === "storage" && root.storageNearlyFull
                                        warnName: "sectionWarn_" + sec.modelData.id
                                        onClicked: root.jumpTo(sec.modelData.id)
                                        UzLabel {
                                            objectName: "sectionStatus_" + sec.modelData.id
                                            Layout.fillWidth: true
                                            font.pixelSize: Theme.roles.small.size
                                            elide: Text.ElideRight
                                            color: sec.modelData.id === "updates" && root.updates.updateAvailable ? Theme.accentText : Theme.dim
                                            text: root.sectionStatus(sec.modelData.id)
                                        }
                                    }
                                }
                            }
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
                        label: qsTr("Settings")
                        labelMeta: qsTr("This PC")
                        title: root.sectionName(root.currentSection)
                    }

                    // ------------------------------------------------------------------ MY ACCOUNT
                    SettingsAccountSection {
                        id: secAccount
                        width: content.width   // laid out at the page width from the first pass
                        objectName: "accountSection"
                        visible: root.currentSection === "account"
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        Layout.minimumWidth: 0
                        player: root.player
                        host: root
                    }

                    // ------------------------------------------------------------------ UPDATES
                    ColumnLayout {
                        id: secUpdates
                        width: content.width   // laid out at the page width from the first pass
                        visible: root.currentSection === "updates"
                        objectName: "updatesSection"
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        Layout.minimumWidth: 0
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
                        width: content.width   // laid out at the page width from the first pass
                        visible: root.currentSection === "hubs"
                        objectName: "hubsSection"
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        Layout.minimumWidth: 0
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

                        // The settings of the server itself are a tab of the Server screen (admins only).
                        UzButton {
                            objectName: "settingsServerSettingsButton"
                            visible: root.player.isAdmin && root.player.hubAddress !== "" && root.player.admin.available
                            Layout.alignment: Qt.AlignLeft
                            Layout.leftMargin: Theme.settingsDotSlot
                            Layout.minimumWidth: 0
                            kind: "link"
                            text: qsTr("Server settings")
                            onClicked: root.player.showServerPage("settings")
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
                        width: content.width   // laid out at the page width from the first pass
                        visible: root.currentSection === "appearance"
                        objectName: "appearanceSection"
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        Layout.minimumWidth: 0
                        spacing: Theme.space15
                        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.rule }
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
                        width: content.width   // laid out at the page width from the first pass
                        visible: root.currentSection === "audio"
                        objectName: "audioSection"
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        Layout.minimumWidth: 0
                        spacing: Theme.space15
                        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.rule }
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
                        width: content.width   // laid out at the page width from the first pass
                        visible: root.currentSection === "storage"
                        objectName: "libraryFoldersSection"
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        Layout.minimumWidth: 0
                        player: root.player
                    }

                    // ------------------------------------------------------------------ STORAGE (overview, clean-up, ROM cache)
                    ColumnLayout {
                        id: secStorage
                        width: content.width   // laid out at the page width from the first pass
                        visible: root.currentSection === "storage"
                        objectName: "storageSection"
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        Layout.minimumWidth: 0
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

                    // ------------------------------------------------------------------ CONTROLS, HOTKEYS, EMULATION
                    // Created only while shown: they own live models (controller capture, core options) and are heavy.
                    Loader {
                        id: secControllers
                        width: content.width   // laid out at the page width from the first pass
                        objectName: "controllersSection"
                        active: root.currentSection === "controllers"
                        visible: active
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1   // the width comes from the page, never from the content
                        Layout.minimumWidth: 0
                        sourceComponent: ControllersContent { player: root.player; inputTestColumn: root.inputTestColumn }
                    }
                    Loader {
                        id: secHotkeys
                        width: content.width   // laid out at the page width from the first pass
                        objectName: "hotkeysSection"
                        active: root.currentSection === "hotkeys"
                        visible: active
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1   // the width comes from the page, never from the content
                        Layout.minimumWidth: 0
                        sourceComponent: HotkeysContent { player: root.player }
                    }
                    Loader {
                        id: secEmulation
                        width: content.width   // laid out at the page width from the first pass
                        objectName: "emulationSection"
                        active: root.currentSection === "emulation"
                        visible: active
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1   // the width comes from the page, never from the content
                        Layout.minimumWidth: 0
                        sourceComponent: EmulationContent { player: root.player }
                    }

                    // ------------------------------------------------------------------ PLUGINS
                    PluginsSection {
                        id: secPlugins
                        width: content.width   // laid out at the page width from the first pass
                        visible: root.currentSection === "plugins"
                        objectName: "pluginsSection"
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        Layout.minimumWidth: 0
                        player: root.player
                        hubName: root.activeHubName
                    }

                    // ------------------------------------------------------------------ DIAGNOSTICS
                    ColumnLayout {
                        id: secDiagnostics
                        width: content.width   // laid out at the page width from the first pass
                        visible: root.currentSection === "diagnostics"
                        objectName: "diagnosticsSection"
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        Layout.minimumWidth: 0
                        spacing: Theme.space15
                        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.rule }
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
                        UzSectionLabel { Layout.fillWidth: true; Layout.topMargin: Theme.space3; text: qsTr("Claude debugging (MCP)") }
                        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.rule; Layout.topMargin: -Theme.space05 }
                        UzSettingsRow {
                            objectName: "mcpEnableRow"
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            resetMode: "none"
                            label: qsTr("Allow Claude to control this app")
                            description: qsTr("Starts a local debug server on this PC only (127.0.0.1). A Claude Code session on this PC can then read the app, its plugins and the running game and control them. A band shows while it is on.")
                            ctrl: "toggle"
                            toggleName: "mcpEnableToggle"
                            checked: root.player.mcp.enabled
                            onToggled: on => root.player.mcp.enabled = on
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.space15
                            UzLabel { text: qsTr("Port"); color: Theme.muted; font.pixelSize: Theme.roles.small.size }
                            UzField {
                                objectName: "mcpPortField"
                                mono: true
                                implicitWidth: 110
                                text: String(root.player.mcp.port)
                                inputMethodHints: Qt.ImhDigitsOnly
                                validator: IntValidator { bottom: 1024; top: 65535 }
                                onEditingFinished: { const n = parseInt(text); if (!isNaN(n)) root.player.mcp.port = n; text = String(root.player.mcp.port) }
                            }
                            Item { Layout.fillWidth: true }
                        }
                        UzSettingsRow {
                            objectName: "mcpTokenRow"
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            resetMode: "none"
                            label: qsTr("Connect Claude Code")
                            descriptionName: "mcpStatus"
                            description: root.player.mcp.error !== "" ? root.player.mcp.error
                                : root.player.mcp.listening ? qsTr("Listening on 127.0.0.1:%1. Copy the setup command and run it in Claude Code on this PC.").arg(root.player.mcp.port)
                                : qsTr("Off")
                            ctrl: "buttons"
                            buttons: [
                                { name: "mcpCopy", text: mcpCopied.running ? qsTr("Copied") : qsTr("Copy setup command"), visible: root.player.mcp.listening },
                                { name: "mcpRenew", text: qsTr("New token"), visible: root.player.mcp.listening }
                            ]
                            onButtonClicked: name => {
                                if (name === "mcpCopy") { root.player.mcp.copySetupCommand(); mcpCopied.restart() }
                                else if (name === "mcpRenew") root.player.mcp.renewToken()
                            }
                            Timer { id: mcpCopied; interval: 1500 }
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
                        visible: !root.helpOn && !root.inputTestColumn
                        Layout.fillWidth: true
                        items: root.helpItems
                    }
                }
            }
            // Input test of Controls: a column at 1440 px and wider (board 9a-1).
        Rectangle {
            objectName: "inputTestColumn"
            visible: root.inputTestColumn
            Layout.fillHeight: true
            Layout.preferredWidth: Theme.inputTestWidth
            Layout.minimumWidth: Theme.inputTestWidth   // never squeezed below the 288 px grid + margins by wide text
            Layout.maximumWidth: Theme.inputTestWidth
            color: Theme.ground
            Rectangle { anchors.left: parent.left; width: 1; height: parent.height; color: Theme.rule }
            Loader {
                anchors.fill: parent
                anchors.margins: 24
                anchors.topMargin: Theme.space4
                active: root.inputTestColumn && secControllers.item !== null
                sourceComponent: secControllers.item ? secControllers.item.inputTestComponent : null
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

    }   // keyScope

    RollbackDialog {
        id: rollbackDialog
        objectName: "rollbackDialogHost"
        updates: root.updates
    }
}
