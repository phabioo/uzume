import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// Settings > This PC > Plugins (board 4i-app-2, round 3 4a-3 / 4a-4): installed plugins first (hairline rows: monogram, name, version, origin pill,
// one subtitle line, Update, toggle, "›" for the detail sheet), then "Available on <Hub>" with Install. Installing a plugin
// that needs another one offers both; turning on an unverified plugin or one whose dependency is off asks first.
// Developer mode and the local folder installs sit behind "Developer settings".
ColumnLayout {
    id: root
    required property PlayerController player
    readonly property PluginsController ctl: player.plugins
    readonly property var list: ctl.plugins
    property bool pathPromptOpen: false
    property string pathMode: "file"   // file | install (folder) | link
    property string confirmUninstallId: ""
    property string confirmEnableId: ""
    property string hubName: ""
    readonly property Item overlayParent: Window.window ? Window.window.contentItem : null   // popups are placed in window coordinates
    property string confirmInstallId: ""      // Hub plugin with missing dependencies: "Install both?"
    property string sheetPluginId: ""
    property bool devOpen: false
    property string checkedAt: ""             // ISO time of the last successful Hub list
    readonly property var available: ctl.hubPlugins.filter(p => p.installedVersion === "")
    readonly property int firstAvailableIndex: {
        const l = ctl.hubPlugins
        for (let i = 0; i < l.length; ++i) if (l[i].installedVersion === "") return i
        return -1
    }
    readonly property var confirmInstallPlugin: {
        for (const p of ctl.hubPlugins) if (p.id === root.confirmInstallId) return p
        return null
    }
    readonly property var confirmEnablePlugin: {
        for (const p of list) if (p.id === root.confirmEnableId) return p
        return null
    }
    readonly property string hubLabel: hubName !== "" ? hubName : qsTr("your server")
    Connections {
        target: root.ctl
        function onHubPluginsChanged() { if (root.ctl.hubStatus === "ready") root.checkedAt = new Date().toISOString() }
    }
    Component.onCompleted: if (root.ctl.hubSupported && root.ctl.hubStatus === "idle") root.ctl.refreshHubPlugins()
    function monogram(name: string): string {
        const w = name.split(/[^A-Za-z0-9]+/).filter(s => s.length > 0)
        return ((w[0] ? w[0][0] : "?") + (w[1] ? w[1][0] : "")).toUpperCase()
    }
    function requestEnable(p) {
        if (p.needsConfirm || p.depsOff.length > 0) root.confirmEnableId = p.id
        else root.ctl.setPluginEnabled(p.id, true)
    }
    property string highlightId: ""            // row shown with an accent border for a moment ("Plugin settings" from a plugin screen)
    Timer { id: highlightTimer; interval: 3000; onTriggered: root.highlightId = "" }
    onHighlightIdChanged: if (highlightId !== "") highlightTimer.restart()
    spacing: Theme.space15

    DialogsGate { id: dialogsGate }   // the dialogs below need QtQuick.Dialogs and Qt.labs.folderlistmodel, else the path field is used
    Loader {
        id: dialogLoader
        active: false
        source: "PluginFolderDialog.qml"
        onLoaded: item.plugins = root.ctl
    }
    Loader {
        id: fileDialogLoader
        active: false
        source: "PluginFileDialog.qml"
        onLoaded: item.plugins = root.ctl
    }
    function openFolderDialog(mode: string) {
        root.pathMode = mode
        dialogLoader.active = dialogsGate.usable
        if (dialogLoader.status === Loader.Ready && dialogLoader.item !== null) {
            dialogLoader.item.mode = mode
            dialogLoader.item.open()
        } else {
            root.pathPromptOpen = true  // QtQuick.Dialogs not installed: type the path
        }
    }
    function openFileDialog() {
        root.pathMode = "file"
        fileDialogLoader.active = dialogsGate.usable
        if (fileDialogLoader.status === Loader.Ready && fileDialogLoader.item !== null) {
            fileDialogLoader.item.open()
        } else {
            root.pathPromptOpen = true
        }
    }

    UzSectionLabel { Layout.fillWidth: true; text: qsTr("Plugins") }
    UzLabel {
        textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
        Layout.fillWidth: true
        text: qsTr("If a plugin fails it is paused; your game keeps running.")
        color: Theme.muted
        font.pixelSize: Theme.roles.input.size
        wrapMode: Text.WordWrap
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.topMargin: Theme.space05
        UzSectionLabel { objectName: "pluginsInstalledTitle"; Layout.fillWidth: true; text: qsTr("Installed · %1").arg(root.list.length) }
        UzButton {
            textFormat: Text.PlainText
            objectName: "pluginInstallButton"
            kind: "link"
            text: qsTr("Install from file…")
            onClicked: root.openFileDialog()
        }
    }

    // ---- installed list
    Rectangle {
        objectName: "pluginsList"
        visible: root.list.length > 0
        Layout.fillWidth: true
        implicitHeight: listCol.implicitHeight
        color: "transparent"
        Rectangle { width: parent.width; height: 1; color: Theme.rule }
        ColumnLayout {
            id: listCol
            anchors.left: parent.left
            anchors.right: parent.right
            spacing: 0
            // The model is the row count: rows are updated in place, not rebuilt on every refresh (a rebuilt Repeater
            // inside a Qt 6.4 layout can leave the layout with a deleted item).
            PluginPool {
                count: root.list.length
                delegate: Item {
                    id: row
                    visible: index < root.list.length   // pooled (PluginPool)
                    // A pooled row that outlives its plugin keeps the last data (hidden), so no binding sees undefined fields.
                    readonly property var current: root.list[index]
                    property var modelData: current || ({})
                    onCurrentChanged: if (current) modelData = current
                    required property int index
                    objectName: "pluginRow_" + (modelData.id || "")
                    Layout.fillWidth: true
                    implicitHeight: Math.max(Theme.row48 + Theme.space1, rowLine.implicitHeight + 2 * Theme.space1 + (problemLabel.visible ? problemLabel.implicitHeight + Theme.space05 : 0))
                    Rectangle {
                        objectName: "pluginRowHighlight"
                        visible: root.highlightId === row.modelData.id
                        anchors.fill: parent
                        anchors.margins: 1
                        color: "transparent"
                        border.width: 2
                        border.color: Theme.accent
                    }
                    Rectangle {
                        visible: row.index > 0
                        anchors.top: parent.top
                        width: parent.width
                        height: 1
                        color: Theme.rule2
                    }
                    TapHandler {
                        onTapped: point => {
                            // The open sheet covers part of the list (a wide font can put a row under its buttons): taps on the
                            // sheet must not also switch the row below it.
                            if (sheet.opened) {
                                const p = sheet.contentItem.mapFromItem(null, point.scenePosition)
                                if (p.x >= 0 && p.y >= 0 && p.x < sheet.contentItem.width && p.y < sheet.contentItem.height) {
                                    return
                                }
                            }
                            root.sheetPluginId = row.modelData.id
                        }
                    }
                    RowLayout {
                        id: rowLine
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.topMargin: Theme.space1
                        anchors.leftMargin: 0
                        anchors.rightMargin: 0
                        spacing: Theme.space15
                        Rectangle {
                            Layout.preferredWidth: Theme.row32
                            Layout.preferredHeight: Theme.row32
                            Layout.alignment: Qt.AlignTop
                            color: Theme.pressed
                            Text {
                                textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                                anchors.centerIn: parent
                                text: root.monogram(row.modelData.name)
                                color: Theme.ink
                                font.pixelSize: Theme.roles.input.size
                                font.weight: Font.Medium
                            }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            spacing: 2
                            RowLayout {
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                spacing: Theme.space1
                                UzLabel {
                                    textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                                    objectName: "pluginName"
                                    text: row.modelData.name
                                    font.pixelSize: Theme.roles.body.size
                                    font.weight: Font.Medium
                                    elide: Text.ElideRight
                                    Layout.maximumWidth: 260
                                }
                                UzMono {
                                    textFormat: Text.PlainText
                                    objectName: "pluginVersion"
                                    text: row.modelData.version
                                    color: Theme.muted
                                }
                                PluginOriginPill {
                                    objectName: "pluginOrigin"
                                    origin: row.modelData.origin
                                    text: row.modelData.originText
                                }
                                UzTag {
                                    objectName: "pluginWrites"
                                    textFormat: Text.PlainText
                                    visible: row.modelData.writesGame === true
                                    small: true
                                    tone: "amber"
                                    icon: "warn"
                                    text: qsTr("Changes the game")
                                }
                                UzTag {
                                    textFormat: Text.PlainText
                                    visible: row.modelData.devLinked
                                    small: true
                                    tone: "neutral"
                                    text: qsTr("Linked folder")
                                }
                                // uz.ui.badge(n) of a running plugin that declares ui.screen (its run tab shows it too).
                                UzTag {
                                    textFormat: Text.PlainText
                                    objectName: "pluginBadge"
                                    readonly property int count: {
                                        const e = root.ctl.sidebarEntries
                                        for (let i = 0; i < e.length; ++i) if (e[i].pluginId === row.modelData.id) return e[i].badge || 0
                                        return 0
                                    }
                                    visible: count > 0
                                    small: true
                                    tone: "info"
                                    text: String(count)
                                }
                                Item { Layout.fillWidth: true }
                            }
                            UzLabel {
                                textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                                objectName: "pluginOverride"
                                visible: row.modelData.overrideText !== ""
                                Layout.fillWidth: true
                                text: row.modelData.overrideText
                                color: Theme.muted
                                font.pixelSize: Theme.roles.small.size
                                wrapMode: Text.WordWrap
                            }
                            UzLabel {
                                textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                                objectName: "pluginSubtitle"
                                Layout.fillWidth: true
                                text: row.modelData.subtitle
                                color: Theme.muted
                                font.pixelSize: Theme.roles.small.size
                                elide: Text.ElideRight
                            }
                        }
                        UzButton {
                            textFormat: Text.PlainText
                            objectName: "pluginUnlink"
                            visible: row.modelData.overrideText !== ""
                            Layout.alignment: Qt.AlignVCenter
                            size: "sm"
                            text: qsTr("Unlink developer folder")
                            onClicked: root.ctl.unlinkDevFolder(row.modelData.id)
                        }
                        UzButton {
                            textFormat: Text.PlainText
                            objectName: "pluginUpdate"
                            visible: row.modelData.updateVersion !== ""
                            Layout.alignment: Qt.AlignVCenter
                            size: "sm"
                            text: qsTr("Update to %1").arg(row.modelData.updateVersion)
                            onClicked: root.ctl.installHubPlugin(row.modelData.id, row.modelData.updateVersion)
                        }
                        UzToggle {
                            id: toggle
                            objectName: "pluginToggle"
                            Layout.alignment: Qt.AlignVCenter
                            enabled: row.modelData.canToggle
                            opacity: enabled ? 1 : 0.4
                            checked: row.modelData.enabled
                            onToggled: {
                                if (checked) {
                                    // Unverified or a dependency that is off: ask first (the toggle stays off until confirmed).
                                    checked = Qt.binding(function() { return row.modelData.enabled })
                                    root.requestEnable(row.modelData)
                                } else {
                                    root.ctl.setPluginEnabled(row.modelData.id, false)
                                }
                            }
                        }
                        UzButton {
                            textFormat: Text.PlainText
                            objectName: "pluginOpen"
                            Layout.alignment: Qt.AlignVCenter
                            kind: "link"
                            text: "›"
                            onClicked: root.sheetPluginId = row.modelData.id
                        }
                    }
                    UzLabel {
                        textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                        id: problemLabel
                        objectName: "pluginProblems"
                        visible: row.modelData.problemsText !== ""
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: rowLine.bottom
                        anchors.leftMargin: Theme.row32 + Theme.space15
                        anchors.rightMargin: 0
                        text: "! " + row.modelData.problemsText
                        color: Theme.accentText
                        font.pixelSize: Theme.roles.small.size
                        wrapMode: Text.WordWrap
                    }
                }
            }
        }
    }

    // ---- empty state: the Hub list below is already loaded, so say what it offers (P19)
    Rectangle {
        objectName: "pluginsEmpty"
        visible: root.list.length === 0
        Layout.fillWidth: true
        implicitHeight: emptyCol.implicitHeight + 2 * Theme.space3
        color: "transparent"
        Rectangle { width: parent.width; height: 1; color: Theme.rule }
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.rule2 }
        ColumnLayout {
            id: emptyCol
            anchors.centerIn: parent
            width: parent.width - 2 * Theme.space3
            spacing: Theme.space1
            UzLabel { textFormat: Text.PlainText; Layout.alignment: Qt.AlignHCenter; text: qsTr("No plugins installed"); font.pixelSize: Theme.roles.itemTitle.size; font.weight: Theme.roles.itemTitle.weight }
            UzLabel {
                textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                objectName: "pluginsEmptyText"
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                text: {
                    if (!root.ctl.hubSupported)
                        return qsTr("Plugins add panels and tools to your games. Install a .uzplugin file from this PC.")
                    const n = root.ctl.hubPlugins.length
                    if (n === 0) return qsTr("%1 offers no plugins yet. Its admin chooses what is offered. You can also install a .uzplugin file from this PC.").arg(root.hubLabel)
                    return qsTr("%1 offers %2. They are listed below under Available on %1.")
                        .arg(root.hubLabel).arg(Theme.plural(n, qsTr("plugin"), qsTr("plugins")))
                }
                color: Theme.muted
                font.pixelSize: Theme.roles.input.size
            }
        }
    }

    // ---- available on the Hub
    RowLayout {
        visible: root.ctl.hubSupported
        Layout.fillWidth: true
        Layout.topMargin: Theme.space1
        spacing: Theme.space15
        UzSectionLabel { objectName: "hubPluginsTitle"; Layout.fillWidth: true; text: qsTr("Available on %1 · %2").arg(root.hubLabel).arg(root.available.length) }
        UzLabel {
            textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
            objectName: "hubPluginsStatus"
            text: root.ctl.hubStatus === "loading" ? qsTr("Loading…")
                  : (root.checkedAt !== "" ? qsTr("Checked %1").arg(root.player.whenText(root.checkedAt)) : "")
            visible: text !== ""
            color: Theme.muted
            font.pixelSize: Theme.roles.small.size
        }
        UzButton {
            textFormat: Text.PlainText
            objectName: "hubPluginsRefresh"
            kind: "link"
            text: qsTr("Refresh")
            onClicked: root.ctl.refreshHubPlugins()
        }
    }
    Rectangle {
        objectName: "hubPluginsPanel"
        visible: root.ctl.hubSupported
        Layout.fillWidth: true
        implicitHeight: hubCol.implicitHeight
        color: "transparent"
        Rectangle { width: parent.width; height: 1; color: Theme.rule }
        ColumnLayout {
            id: hubCol
            anchors.left: parent.left
            anchors.right: parent.right
            spacing: 0
            RowLayout {
                objectName: "hubPluginsError"
                visible: root.ctl.hubStatus === "failed"
                Layout.fillWidth: true
                Layout.margins: Theme.space2
                spacing: Theme.space15
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    UzLabel { textFormat: Text.PlainText; text: qsTr("Could not reach %1").arg(root.hubLabel); font.pixelSize: Theme.roles.body.size; font.weight: Font.Medium }
                    UzLabel {
                        textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                        Layout.fillWidth: true
                        text: qsTr("Available plugins could not be loaded. Installed plugins keep working.")
                        color: Theme.muted
                        font.pixelSize: Theme.roles.small.size
                        wrapMode: Text.WordWrap
                    }
                }
                UzButton { textFormat: Text.PlainText; objectName: "hubPluginsRetry"; size: "sm"; text: qsTr("Retry"); onClicked: root.ctl.refreshHubPlugins() }
            }
            UzLabel {
                textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                objectName: "hubPluginsEmpty"
                visible: root.available.length === 0 && root.ctl.hubStatus === "ready"
                Layout.fillWidth: true
                Layout.margins: Theme.space2
                text: root.ctl.hubPlugins.length === 0 ? qsTr("%1 offers no plugins yet. Its admin chooses what is offered.").arg(root.hubLabel)
                                                       : qsTr("Everything %1 offers is installed.").arg(root.hubLabel)
                color: Theme.muted
                font.pixelSize: Theme.roles.input.size
                wrapMode: Text.WordWrap
            }
            // The full offer list; rows that are installed stay hidden (the list keeps its identity, so the rows are not
            // rebuilt on every refresh of the installed plugins).
            PluginPool {
                count: root.ctl.hubPlugins.length
                delegate: Item {
                    id: hrow
                    // A pooled row that outlives its plugin keeps the last data (hidden), so no binding sees undefined fields.
                    readonly property var current: root.ctl.hubPlugins[index]
                    property var modelData: current || ({})
                    onCurrentChanged: if (current) modelData = current
                    required property int index
                    readonly property bool shown: modelData.installedVersion === ""
                    objectName: shown ? "hubPluginRow_" + modelData.id : ""
                    visible: shown && index < root.ctl.hubPlugins.length   // pooled (PluginPool)
                    Layout.fillWidth: true
                    Layout.preferredHeight: shown ? Theme.row48 + Theme.space1 : 0
                    implicitHeight: Theme.row48 + Theme.space1
                    Rectangle {
                        visible: hrow.index > root.firstAvailableIndex
                        anchors.top: parent.top
                        width: parent.width
                        height: 1
                        color: Theme.rule2
                    }
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 0
                        anchors.rightMargin: 0
                        spacing: Theme.space15
                        Rectangle {
                            Layout.preferredWidth: Theme.row32
                            Layout.preferredHeight: Theme.row32
                            color: Theme.pressed
                            Text {
                                textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                                anchors.centerIn: parent
                                text: root.monogram(hrow.modelData.name)
                                color: Theme.ink
                                font.pixelSize: Theme.roles.input.size
                                font.weight: Font.Medium
                            }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            spacing: 2
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.space1
                                UzLabel {
                                    textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                                    objectName: "hubPluginName"
                                    text: hrow.modelData.name
                                    font.pixelSize: Theme.roles.body.size
                                    font.weight: Font.Medium
                                    elide: Text.ElideRight
                                    Layout.maximumWidth: 260
                                }
                                UzMono { textFormat: Text.PlainText; objectName: "hubPluginVersion"; text: hrow.modelData.version; color: Theme.muted }
                                PluginOriginPill {
                                    objectName: "hubPluginOrigin"
                                    origin: hrow.modelData.origin
                                    text: hrow.modelData.originText
                                }
                                UzTag {
                                    objectName: "hubPluginWrites"
                                    visible: hrow.modelData.writesGame === true
                                    small: true
                                    tone: "amber"
                                    icon: "warn"
                                    text: qsTr("Changes the game")
                                }
                                Item { Layout.fillWidth: true }
                            }
                            UzLabel {
                                textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                                objectName: "hubPluginPermissions"
                                Layout.fillWidth: true
                                text: qsTr("Can: %1").arg(hrow.modelData.permissionsText) + (hrow.modelData.sizeText !== "" ? " · " + hrow.modelData.sizeText : "")
                                color: Theme.muted
                                font.pixelSize: Theme.roles.small.size
                                wrapMode: Text.Wrap       // the whole list, never cut off
                            }
                            UzLabel {
                                textFormat: Text.PlainText
                                objectName: "hubPluginWritesNote"
                                visible: hrow.modelData.writesGame === true
                                Layout.fillWidth: true
                                text: hrow.modelData.writesNote
                                color: Theme.ink2
                                font.pixelSize: Theme.roles.small.size
                                wrapMode: Text.Wrap
                            }
                        }
                        UzButton {
                            textFormat: Text.PlainText
                            objectName: "hubPluginInstall"
                            size: "sm"
                            enabled: hrow.modelData.state === "install"
                            text: hrow.modelData.state === "busy" ? qsTr("Installing…") : qsTr("Install")
                            onClicked: {
                                if (hrow.modelData.needsInstall.length > 0) root.confirmInstallId = hrow.modelData.id
                                else root.ctl.installHubPlugin(hrow.modelData.id, hrow.modelData.version)
                            }
                        }
                    }
                }
            }
        }
    }

    // ---- plugin language
    ColumnLayout {
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        spacing: 0
        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.rule2 }
        UzSettingsRow {
            objectName: "pluginLanguageRow"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            resetMode: "none"
            label: qsTr("Plugin language")
            description: qsTr("Language of plugin texts. Auto uses the 3DS system language for 3DS games, otherwise the language of this computer. Applies when the next game starts.")
            descriptionName: "pluginLanguageHint"
            ctrl: "select"
            controlName: "pluginLanguageSelect"
            values: root.ctl.pluginLanguageOptions
            current: root.ctl.pluginLanguage
            onValuePicked: value => root.ctl.pluginLanguage = value
        }
    }

    // ---- developer settings (the button moves under the text when both do not fit, wide fonts)
    GridLayout {
        id: devRow
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        Layout.topMargin: Theme.space1
        columns: devRow.width < devLabel.implicitWidth + devButton.implicitWidth + columnSpacing ? 1 : 2
        columnSpacing: Theme.space1
        rowSpacing: Theme.space05
        UzLabel {
            id: devLabel
            textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: qsTr("Developer mode and local folders:")
            color: Theme.muted
            font.pixelSize: Theme.roles.input.size
        }
        UzButton {
            id: devButton
            textFormat: Text.PlainText
            objectName: "pluginDeveloperSettings"
            kind: "link"
            Layout.minimumWidth: 0
            text: root.devOpen ? qsTr("Hide developer settings") : qsTr("Developer settings")
            onClicked: root.devOpen = !root.devOpen
        }
    }
    ColumnLayout {
        visible: root.devOpen
        Layout.fillWidth: true
        spacing: Theme.space15
        UzSettingsRow {
            objectName: "pluginDeveloperModeRow"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            resetMode: "none"
            label: qsTr("Developer mode")
            description: qsTr("Allows unsigned local plugins, adds the Plugin console tab in game and reloads plugins when their files change.")
            descriptionName: "pluginDeveloperModeHint"
            ctrl: "toggle"
            toggleName: "pluginDeveloperModeToggle"
            checked: root.ctl.developerMode
            onToggled: on => root.ctl.developerMode = on
        }
        Flow {
            Layout.fillWidth: true
            spacing: Theme.space15
            UzButton {
                textFormat: Text.PlainText
                objectName: "pluginInstallFolderButton"
                text: qsTr("Install from folder…")
                onClicked: root.openFolderDialog("install")
            }
        }
    }
    RowLayout {
        visible: root.pathPromptOpen
        Layout.fillWidth: true
        spacing: Theme.space15
        UzField {
            id: pathField
            objectName: "pluginFolderPathField"
            Layout.fillWidth: true
            placeholderText: root.pathMode === "file" ? qsTr("Plugin file (.uzplugin) or folder path") : root.pathMode === "link" ? qsTr("Folder with manifest.json") : qsTr("Plugin folder path")
            onAccepted: applyPath.clicked()
        }
        UzButton {
            textFormat: Text.PlainText
            id: applyPath
            objectName: "pluginFolderApplyButton"
            text: root.pathMode === "link" ? qsTr("Load") : qsTr("Install")
            onClicked: {
                if (root.pathMode === "link") root.ctl.loadUnpacked(pathField.text)
                else if (root.pathMode === "file") root.ctl.installFromPath(pathField.text)
                else root.ctl.installFromFolder(pathField.text)
                root.pathPromptOpen = false
            }
        }
    }
    UzLabel {
        textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
        objectName: "pluginsMessage"
        visible: root.ctl.message !== ""
        Layout.fillWidth: true
        text: root.ctl.message
        color: root.ctl.messageIsError ? Theme.danger : Theme.ink2
        font.pixelSize: Theme.roles.small.size
        wrapMode: Text.WordWrap
    }

    // ---- Developer console (boards 4d-1 and 4d-2): Developer mode only
    property alias consoleBlock: devConsoleBlock
    property alias developerConsole: devConsoleItem
    ColumnLayout {
        id: devConsoleBlock
        objectName: "pluginDeveloperConsole"
        visible: root.ctl.developerMode
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        Layout.topMargin: Theme.space15
        spacing: Theme.space1
        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.rule2 }
        UzSectionLabel { Layout.fillWidth: true; text: qsTr("Developer console") }
        UzLabel {
            textFormat: Text.PlainText
            objectName: "pluginDeveloperConsoleHint"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            font.pixelSize: Theme.roles.small.size
            color: Theme.muted
            text: qsTr("Load a plugin from a folder, reload it and read its log. Ctrl+Shift+D opens this console from anywhere; Ctrl+R reloads while it has the focus.")
        }
        PluginConsole {
            id: devConsoleItem
            player: root.player
            settingsMode: true
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            Layout.preferredHeight: implicitHeight
            onLoadUnpackedRequested: root.openFolderDialog("link")
        }
    }

    // ---- detail sheet
    PluginDetailSheet {
        id: sheet
        player: root.player
        hubName: root.hubName
        overlayParent: root.overlayParent
        pluginId: root.sheetPluginId
        onEnableRequested: id => root.confirmEnableId = id
        onClosed: root.sheetPluginId = ""
    }
    onSheetPluginIdChanged: if (sheetPluginId !== "") sheet.open()

    // ---- dialogs (400 wide, centered): install with a dependency, turn on
    component PluginDialog: Popup {
        id: dlg
        property string contentName: ""
        dim: true
        property alias bodyItem: slot.data
        parent: root.overlayParent
        anchors.centerIn: parent
        width: Theme.dialog
        modal: true
        padding: Theme.space3
        closePolicy: Popup.CloseOnEscape
        background: Rectangle {
            color: Theme.surface
            border.width: 1
            border.color: Theme.rule
        }
        contentItem: ColumnLayout {
            id: slot
            objectName: dlg.contentName
            spacing: Theme.space15
        }
    }

    PluginDialog {
        id: installDialog
        contentName: "pluginInstallDialog"
        visible: root.confirmInstallPlugin !== null
        onClosed: root.confirmInstallId = ""
        bodyItem: [
            UzLabel {
                textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                Layout.fillWidth: true
                text: root.confirmInstallPlugin ? qsTr("Install %1?").arg(root.confirmInstallPlugin.name) : ""
                font.pixelSize: Theme.roles.itemTitle.size
                font.weight: Font.Medium
                wrapMode: Text.WordWrap
            },
            UzLabel {
                textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                Layout.fillWidth: true
                text: root.confirmInstallPlugin
                      ? (root.confirmInstallPlugin.origin === "unverified" ? root.confirmInstallPlugin.originText + ". " : "")
                        + qsTr("It can: %1.").arg(root.confirmInstallPlugin.permissionsText) : ""
                color: Theme.ink2
                font.pixelSize: Theme.roles.input.size
                wrapMode: Text.WordWrap
            },
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: depRow.implicitHeight + 2 * Theme.space15
                color: "transparent"
                Rectangle { width: parent.width; height: 1; color: Theme.rule2 }
                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.rule2 }
                RowLayout {
                    id: depRow
                    anchors.fill: parent
                    anchors.topMargin: Theme.space15
                    anchors.bottomMargin: Theme.space15
                    spacing: Theme.space15
                    Text { textFormat: Text.PlainText; text: "✓"; color: Theme.accentText; font.pixelSize: Theme.roles.body.size }
                    UzLabel {
                        textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                        objectName: "pluginInstallDependency"
                        Layout.fillWidth: true
                        text: {
                            const p = root.confirmInstallPlugin
                            if (!p) return ""
                            const d = p.needsInstall.map(x => x.name + " " + x.version).join(", ")
                            return qsTr("Also install %1 · needed by %2").arg(d).arg(p.name)
                        }
                        wrapMode: Text.WordWrap
                        font.pixelSize: Theme.roles.input.size
                    }
                }
            },
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.space1
                Item { Layout.fillWidth: true }
                UzButton { textFormat: Text.PlainText; objectName: "pluginInstallCancel";  text: qsTr("Cancel"); onClicked: installDialog.close() }
                UzButton {
                    textFormat: Text.PlainText
                    objectName: "pluginInstallBoth"
                    kind: "primary"
                    text: qsTr("Install both")
                    onClicked: {
                        const p = root.confirmInstallPlugin
                        installDialog.close()
                        if (p) root.ctl.installHubPluginWithDependencies(p.id, p.version)
                    }
                }
            }
        ]
    }

    PluginDialog {
        id: enableDialog
        contentName: "pluginEnableConfirm"
        visible: root.confirmEnablePlugin !== null
        onClosed: root.confirmEnableId = ""
        bodyItem: [
            UzLabel {
                textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                Layout.fillWidth: true
                text: root.confirmEnablePlugin ? qsTr("Turn on %1?").arg(root.confirmEnablePlugin.name) : ""
                font.pixelSize: Theme.roles.itemTitle.size
                font.weight: Font.Medium
                wrapMode: Text.WordWrap
            },
            UzLabel {
                textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                objectName: "pluginUnverifiedWarning"
                visible: root.confirmEnablePlugin !== null && root.confirmEnablePlugin.needsConfirm
                Layout.fillWidth: true
                text: qsTr("Uzume has not checked this plugin. Turn it on only if you trust the admin of your server and where the file came from.")
                color: Theme.ink2
                font.pixelSize: Theme.roles.input.size
                wrapMode: Text.WordWrap
            },
            UzLabel {
                textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                objectName: "pluginEnablePermissions"
                Layout.fillWidth: true
                text: root.confirmEnablePlugin ? qsTr("It will be able to: %1").arg(root.confirmEnablePlugin.permissionsText) : ""
                color: Theme.ink2
                font.pixelSize: Theme.roles.input.size
                wrapMode: Text.WordWrap
            },
            UzLabel {
                textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                objectName: "pluginEnableDependency"
                visible: root.confirmEnablePlugin !== null && root.confirmEnablePlugin.depsOff.length > 0
                Layout.fillWidth: true
                text: root.confirmEnablePlugin ? qsTr("It needs %1, which is off. Both turn on.").arg(root.confirmEnablePlugin.depsOff.join(", ")) : ""
                color: Theme.ink2
                font.pixelSize: Theme.roles.input.size
                wrapMode: Text.WordWrap
            },
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.space1
                Item { Layout.fillWidth: true }
                UzButton { textFormat: Text.PlainText; objectName: "pluginEnableCancel";  text: qsTr("Cancel"); onClicked: enableDialog.close() }
                UzButton {
                    textFormat: Text.PlainText
                    objectName: "pluginEnableConfirmButton"
                    kind: "primary"
                    text: root.confirmEnablePlugin && root.confirmEnablePlugin.depsOff.length > 0 ? qsTr("Turn on both") : qsTr("Turn on")
                    onClicked: {
                        const id = root.confirmEnableId
                        enableDialog.close()
                        root.ctl.setPluginEnabled(id, true)
                    }
                }
            }
        ]
    }
}
