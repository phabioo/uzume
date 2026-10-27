import QtQuick
import QtQuick.Layouts
import Uzume.App

// Settings > This PC > Storage, the overview above the ROM cache rows (play-and-plugins.md 6l-1, 0.19): "<data folder> · MEASURED hh:mm"
// with "Measure again", the storage bar (Downloaded games, Save cache, Save states, Screenshots waiting for upload, Cores and firmware,
// free space; ink tints, hairline legend with the values) and the clean-up suggestions with sizes. Every suggestion asks first, inline.
// Saves are never offered. The measurement runs in a worker thread (LocalStorageController); the numbers stay while it runs again.
// Suggestions: games not played in 60 days (they download again when you play), save states of games you deleted (this PC's copies) and
// unused cores. A suggestion with nothing to clean is hidden; "Nothing to clean up." shows once.
// objectNames: storageOverview, storageDataPath, storageMeasure, localStorageBar, localStorageKind_<games|saves|states|screenshots|cores>,
// localStorageFree, storageCleanupLabel, storageCleanup_<id>, storageCleanupButton_<id>, storageCleanupConfirm_<id>,
// storageCleanupConfirmText_<id>, storageCleanupCancel_<id>, storageCleanupConfirmButton_<id>, storageCleanupResult, storageNothingToClean
// (<id> = old_games | deleted_game_states | unused_cores).
ColumnLayout {
    id: root
    objectName: "storageOverview"
    required property PlayerController player
    readonly property var ls: root.player.localStorage
    readonly property var report: root.ls.report
    readonly property bool measured: root.report.dataPath !== undefined
    property string confirmId: ""
    property string resultText: ""
    spacing: Theme.space15

    readonly property var kindLabels: ({
        games: qsTr("Downloaded games"),
        saves: qsTr("Save cache"),
        states: qsTr("Save states"),
        screenshots: qsTr("Screenshots waiting for upload"),
        cores: qsTr("Cores and firmware")
    })
    readonly property var cleanupInfo: ({
        old_games: {
            label: qsTr("Games not played in 60 days"),
            text: qsTr("They download again when you play."),
            confirm: qsTr("Remove these games from this PC and free %1? They download again when you play. Saves are not deleted."),
            button: qsTr("Remove games")
        },
        deleted_game_states: {
            label: qsTr("States of games you deleted"),
            text: qsTr("Save states on this PC for games that are no longer in your library. Saves are never touched."),
            confirm: qsTr("Remove these save states from this PC and free %1? The saves of your games are not touched."),
            button: qsTr("Remove states")
        },
        unused_cores: {
            label: qsTr("Unused cores"),
            text: qsTr("Older versions of cores. The newest version and the core of the running game stay."),
            confirm: qsTr("Remove unused cores and free %1? A core downloads again when a game needs it. Saves are not deleted."),
            button: qsTr("Remove cores")
        }
    })
    readonly property var kinds: (root.report.kinds ?? []).map(k => ({ id: k.id, label: root.kindLabels[k.id] ?? k.id, text: k.text, bytes: k.bytes }))
    readonly property var suggestions: root.report.cleanup ?? []

    Component.onCompleted: root.ls.measure()
    onVisibleChanged: if (root.visible) root.ls.measure()

    RowLayout {
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        UzSectionLabel {
            objectName: "storageDataPath"
            Layout.fillWidth: true
            text: root.measured ? qsTr("%1 · measured %2").arg(root.report.dataPath).arg(root.report.measuredText)
                                : (root.ls.measuring ? qsTr("Measuring…") : qsTr("Not measured yet"))
        }
        UzButton {
            objectName: "storageMeasure"
            kind: "link"
            enabled: !root.ls.measuring
            text: root.ls.measuring ? qsTr("Measuring…") : qsTr("Measure again")
            onClicked: root.ls.measure()
        }
    }

    UzStorageBar {
        Layout.fillWidth: true
        visible: root.measured
        kinds: root.kinds
        totalBytes: root.report.totalBytes ?? 0
        freeText: root.report.freeText ?? ""
        barName: "localStorageBar"
        rowPrefix: "localStorageKind_"
        freeName: "localStorageFree"
    }

    UzSectionLabel {
        objectName: "storageCleanupLabel"
        Layout.fillWidth: true
        Layout.topMargin: Theme.space1
        visible: root.measured
        text: qsTr("Clean-up")
    }
    Repeater {
        model: root.suggestions
        delegate: ColumnLayout {
            id: sug
            required property var modelData
            readonly property var info: root.cleanupInfo[modelData.id] ?? ({ label: modelData.id, text: "", confirm: "", button: "" })
            Layout.fillWidth: true
            spacing: Theme.space15
            UzSettingsRow {
                objectName: "storageCleanup_" + sug.modelData.id
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                resetMode: "none"
                label: sug.info.label
                meta: sug.modelData.text
                description: ((sug.modelData.id === "old_games" || sug.modelData.id === "deleted_game_states") ? Theme.plural(sug.modelData.count, qsTr("game"), qsTr("games")) : Theme.plural(sug.modelData.count, qsTr("old version"), qsTr("old versions")))
                             + " · " + sug.info.text
                ctrl: "buttons"
                buttons: [{ name: "storageCleanupButton_" + sug.modelData.id, text: qsTr("Clean up…"), enabled: root.confirmId !== sug.modelData.id }]
                onButtonClicked: { root.resultText = ""; root.confirmId = sug.modelData.id }
            }
            RowLayout {
                objectName: "storageCleanupConfirm_" + sug.modelData.id
                Layout.fillWidth: true
                spacing: Theme.space15
                visible: root.confirmId === sug.modelData.id
                UzLabel {
                    objectName: "storageCleanupConfirmText_" + sug.modelData.id
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    wrapMode: Text.WordWrap
                    font.pixelSize: Theme.roles.small.size
                    color: Theme.ink
                    text: sug.info.confirm.arg(sug.modelData.text)
                }
                UzButton {
                    objectName: "storageCleanupCancel_" + sug.modelData.id
                    implicitHeight: 32
                    text: qsTr("Cancel")
                    onClicked: root.confirmId = ""
                }
                UzButton {
                    objectName: "storageCleanupConfirmButton_" + sug.modelData.id
                    implicitHeight: 32
                    kind: "primary"
                    text: sug.info.button
                    onClicked: {
                        const id = sug.modelData.id
                        root.confirmId = ""
                        const r = root.ls.cleanUp(id)
                        if (r.ok) root.resultText = qsTr("Freed %1.").arg(r.freedText)
                    }
                }
            }
        }
    }
    UzMono {
        objectName: "storageCleanupResult"
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        visible: root.resultText !== ""
        text: root.resultText
    }
    UzMono {
        objectName: "storageNothingToClean"
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        visible: root.measured && root.suggestions.length === 0 && root.resultText === ""
        text: qsTr("Nothing to clean up.")
    }
}
