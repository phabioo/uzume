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
        variants: qsTr("Variants"),
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

    // Variants (design 10g-3): the built randomizer variants on this PC. Their files can go at any time; the recipe rebuilds them.
    readonly property VariantsController vc: root.player.variants
    readonly property var variantRows: root.vc.storage
    readonly property bool variantsShown: root.vc.serverSupport || root.variantRows.length > 0
    property bool confirmFree: false
    property string freeResult: ""
    UzSectionLabel {
        objectName: "storageVariantsLabel"
        Layout.fillWidth: true
        Layout.topMargin: Theme.space1
        visible: root.variantsShown
        text: qsTr("Variants")
        meta: qsTr("%1 · %2 IN CACHE").arg(Theme.plural(root.variantRows.length, qsTr("variant"), qsTr("variants")).toUpperCase()).arg(root.vc.cache.text.toUpperCase())
    }
    UzLabel {
        objectName: "storageVariantsNote"
        visible: root.variantsShown
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        wrapMode: Text.WordWrap
        font.pixelSize: Theme.roles.small.size
        color: Theme.muted
        text: qsTr("Free space: variants are rebuilt when needed. Their recipes and saves stay on %1.").arg(root.player.hubName !== "" ? root.player.hubName : qsTr("the server"))
    }
    RowLayout {
        visible: root.variantsShown
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        spacing: Theme.space15
        UzButton {
            objectName: "storageVariantsFree"
            Layout.fillWidth: false
            enabled: root.vc.cache.bytes > 0 && !root.confirmFree
            text: qsTr("Free %1…").arg(root.vc.cache.text)
            onClicked: { root.freeResult = ""; root.confirmFree = true }
        }
        UzMono {
            objectName: "storageVariantsFreed"
            visible: root.freeResult !== ""
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: root.freeResult
        }
    }
    RowLayout {
        objectName: "storageVariantsFreeConfirm"
        Layout.fillWidth: true
        spacing: Theme.space15
        visible: root.variantsShown && root.confirmFree
        UzLabel {
            objectName: "storageVariantsFreeConfirmText"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            wrapMode: Text.WordWrap
            font.pixelSize: Theme.roles.small.size
            color: Theme.ink
            text: qsTr("Remove the files of all variants and free %1? They rebuild from their recipes when you play them. Saves are not deleted.").arg(root.vc.cache.text)
        }
        UzButton { objectName: "storageVariantsFreeCancel"; implicitHeight: 32; text: qsTr("Cancel"); onClicked: root.confirmFree = false }
        UzButton {
            objectName: "storageVariantsFreeConfirmButton"
            implicitHeight: 32
            kind: "primary"
            text: qsTr("Free space")
            onClicked: {
                root.confirmFree = false
                const r = root.vc.freeCache()
                root.freeResult = qsTr("Freed %1.").arg(r.freedText ?? "")
                root.ls.measure()
            }
        }
    }
    // VARIANT · LAST PLAYED · SIZE
    ColumnLayout {
        objectName: "storageVariantsTable"
        visible: root.variantsShown && root.variantRows.length > 0
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        spacing: 0
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.space15
            Layout.rightMargin: Theme.space15
            spacing: Theme.space15
            Repeater {
                model: [{ t: qsTr("Variant"), w: 0 }, { t: qsTr("Last played"), w: 0 }, { t: qsTr("Size"), w: 64 }]
                delegate: UzMono {
                    required property var modelData
                    Layout.fillWidth: modelData.w === 0
                    Layout.preferredWidth: modelData.w === 0 ? 1 : modelData.w
                    Layout.maximumWidth: modelData.w === 0 ? 100000 : modelData.w
                    Layout.minimumWidth: 0
                    elide: Text.ElideRight
                    horizontalAlignment: modelData.w > 0 ? Text.AlignRight : Text.AlignLeft
                    text: modelData.t
                    color: Theme.dim
                    font.pixelSize: Theme.roles.sectionLabel.size
                    font.weight: Theme.roles.sectionLabel.weight
                    font.letterSpacing: Theme.roles.sectionLabel.spacing
                    font.capitalization: Font.AllUppercase
                }
            }
        }
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; Layout.topMargin: Theme.space05; color: Theme.rule }
        Repeater {
            model: root.variantRows
            delegate: Item {
                id: vrow
                required property var modelData
                required property int index
                objectName: "storageVariantRow_" + index
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                implicitHeight: 48
                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.rule2 }
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.space15
                    anchors.rightMargin: Theme.space15
                    spacing: Theme.space15
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        Layout.preferredWidth: 1
                        spacing: 2
                        UzLabel {
                            objectName: "storageVariantName_" + vrow.index
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            font.pixelSize: Theme.roles.input.size
                            font.weight: Font.Medium
                            text: vrow.modelData.name
                        }
                        UzMono {
                            Layout.fillWidth: true
                            visible: vrow.modelData.subtitle !== ""
                            elide: Text.ElideRight
                            font.pixelSize: Theme.roles.meta11.size
                            color: Theme.dim
                            text: vrow.modelData.subtitle
                        }
                    }
                    UzLabel {
                        objectName: "storageVariantPlayed_" + vrow.index
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        Layout.preferredWidth: 1
                        elide: Text.ElideRight
                        font.pixelSize: Theme.roles.small.size
                        color: vrow.modelData.removed ? Theme.muted : Theme.ink2
                        text: vrow.modelData.removed ? qsTr("Files removed · rebuilds when played") : vrow.modelData.lastPlayedText
                    }
                    UzMono {
                        objectName: "storageVariantSize_" + vrow.index
                        Layout.preferredWidth: 64
                        Layout.maximumWidth: 64
                        Layout.minimumWidth: 0
                        horizontalAlignment: Text.AlignRight
                        elide: Text.ElideRight
                        font.pixelSize: Theme.roles.meta11.size
                        color: Theme.ink2
                        text: vrow.modelData.sizeText
                    }
                }
            }
        }
    }
    UzSettingsRow {
        objectName: "storageVariantsAgeRow"
        visible: root.variantsShown
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        resetMode: "none"
        label: qsTr("Remove variants not played for")
        description: qsTr("Their files go; they rebuild from the recipe the next time you play. This only removes copies on this PC. Saves and recipes are always on %1.").arg(root.player.hubName !== "" ? root.player.hubName : qsTr("the server"))
        descriptionName: "storageVariantsAgeHint"
        ctrl: "select"
        controlName: "storageVariantsAgeSelect"
        values: [{ value: "0", label: qsTr("Never") }, { value: "30", label: qsTr("30 days") }, { value: "60", label: qsTr("60 days") }, { value: "90", label: qsTr("90 days") }]
        current: String(root.vc.cache.unplayedDays)
        onValuePicked: value => root.vc.setUnplayedDays(Number(value))
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
