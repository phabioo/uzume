import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// The small dialogs of the variants on the game page (design 10a-2, 10a-3e/f, 10d-1 d-f, 10d-3, 10d-4 c-e): delete (Keep saves / Delete saves
// too), rename, the spoiler log (confirm first, then a read-only text) and the problem dialog of a failed build (details with the Mono log,
// Try again, Copy details; not enough memory, disk full, a dump the randomizer does not know with Build anyway). All dialogs are opened by
// function; none of them touches the build itself.
// objectNames: variantDeleteDialog, variantDeleteMode, variantDeleteConfirm, variantDeleteCancel, variantRenameDialog, variantRenameField,
// variantRenameSave, variantRenameCancel, variantRenameError, variantSpoilerDialog, variantSpoilerShow, variantSpoilerCancel,
// variantSpoilerLogDialog, variantSpoilerText, variantSpoilerClose, variantProblemDialog, variantProblemMono, variantProblemDetails,
// variantProblemPrimary, variantProblemCopy, variantProblemClose.
Item {
    id: root
    required property PlayerController player
    readonly property VariantsController vc: root.player.variants
    property string gameTitle: ""
    readonly property string serverName: root.player.hubName !== "" ? root.player.hubName : qsTr("the server")
    property var target: ({})          // the variant the open dialog is about
    signal rebuilding(string variantId)   // Try again / Build anyway started a new build: the page shows the build dialog

    // ---- delete
    function askDelete(v: var, saveVersions: int) {
        root.target = v
        deleteDialog.versions = saveVersions
        deleteMode.current = "keep"
        deleteDialog.open()
    }
    // ---- rename
    function askRename(v: var) {
        root.target = v
        renameField.text = v.name
        renameDialog.error = ""
        renameDialog.open()
        renameField.forceActiveFocus()
        renameField.selectAll()
    }
    // ---- spoiler log
    function askSpoiler(v: var) {
        root.target = v
        spoilerDialog.open()
    }
    // ---- problem dialog of a failed build / state
    function showProblem(v: var) {
        root.target = v
        problem.open()
    }

    // ================================================================ Delete a variant (10a-2)
    UzDialog {
        id: deleteDialog
        objectName: "variantDeleteDialog"
        property int versions: 0
        parent: Window.contentItem
        z: 110
        size: "lg"
        label: qsTr("Library · %1").arg(root.gameTitle)
        title: qsTr("Delete %1?").arg(root.target.name ?? "")
        text: deleteDialog.versions > 0
              ? qsTr("The variant’s files go from this PC. Its slot has %1 on %2.").arg(Theme.plural(deleteDialog.versions, qsTr("save version"), qsTr("save versions"))).arg(root.serverName)
              : qsTr("The variant’s files go from this PC.")
        UzMono {
            Layout.fillWidth: true
            elide: Text.ElideRight
            color: Theme.dim
            font.pixelSize: Theme.roles.sectionLabel.size
            font.letterSpacing: Theme.roles.sectionLabel.spacing
            font.capitalization: Font.AllUppercase
            text: qsTr("Saves of %1").arg(root.target.name ?? "")
        }
        UzSegmented {
            id: deleteMode
            objectName: "variantDeleteMode"
            Layout.fillWidth: true
            stretch: true
            accessibleName: qsTr("Saves of %1").arg(root.target.name ?? "")
            current: "keep"
            options: [{ value: "keep", label: qsTr("Keep saves"), name: "variantDeleteKeep" },
                      { value: "all", label: qsTr("Delete saves too"), name: "variantDeleteAll" }]
            onPicked: (value) => deleteMode.current = value
        }
        UzLabel {
            objectName: "variantDeleteHelp"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            wrapMode: Text.WordWrap
            font.pixelSize: Theme.roles.small.size
            color: Theme.muted
            text: deleteMode.current === "keep"
                  ? qsTr("Kept saves stay on %1 in the slot %2, with the recipe. Build it again later to play them.").arg(root.serverName).arg(root.target.name ?? "")
                  : qsTr("The slot and every save version in it are deleted from %1. This can’t be undone.").arg(root.serverName)
        }
        actions: [
            UzButton { id: deleteCancel; objectName: "variantDeleteCancel"; text: qsTr("Cancel"); onClicked: deleteDialog.close() }
        ]
        dangerActions: [
            UzButton {
                objectName: "variantDeleteConfirm"
                kind: "danger"
                destructive: true
                text: qsTr("Delete variant")
                onClicked: {
                    deleteDialog.shown = false
                    root.vc.remove(root.target.id, deleteMode.current === "keep")
                }
            }
        ]
        onShownChanged: if (shown) Qt.callLater(() => deleteCancel.forceActiveFocus())
    }

    // ================================================================ Rename (10a-3e)
    UzDialog {
        id: renameDialog
        objectName: "variantRenameDialog"
        parent: Window.contentItem
        z: 110
        label: qsTr("Library · %1").arg(root.gameTitle)
        title: qsTr("Rename %1").arg(root.target.name ?? "")
        readonly property string nameText: renameField.text.trim()
        readonly property bool taken: root.vc.nameTaken(root.target.gameId ?? "", renameDialog.nameText, root.target.id ?? "")
        readonly property bool canSave: renameDialog.nameText !== "" && renameDialog.nameText.length <= 60 && !renameDialog.taken
        function submit() {
            if (!renameDialog.canSave) return
            if (renameDialog.nameText !== (root.target.name ?? "")) root.vc.rename(root.target.id, renameDialog.nameText)
            renameDialog.shown = false
        }
        UzMono {
            text: qsTr("Name")
            color: Theme.dim
            font.pixelSize: Theme.roles.sectionLabel.size
            font.letterSpacing: Theme.roles.sectionLabel.spacing
            font.capitalization: Font.AllUppercase
        }
        UzField {
            id: renameField
            objectName: "variantRenameField"
            Layout.fillWidth: true
            fill: Theme.ground
            maximumLength: 80
            invalid: renameDialog.taken
            accessibleName: qsTr("Name")
            onAccepted: renameDialog.submit()
        }
        UzStatusIcon {
            objectName: "variantRenameError"
            visible: renameDialog.taken
            small: true
            kind: "error"
            text: qsTr("You already have a variant called %1.").arg(renameDialog.nameText)
            maxWidth: 400
        }
        UzLabel {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            font.pixelSize: Theme.roles.small.size
            color: Theme.muted
            text: qsTr("The slot is renamed with it. Saves stay.")
        }
        actions: [
            UzButton { objectName: "variantRenameSave"; kind: "primary"; text: qsTr("Save"); enabled: renameDialog.canSave; onClicked: renameDialog.submit() },
            UzButton { objectName: "variantRenameCancel"; text: qsTr("Cancel"); onClicked: renameDialog.close() }
        ]
    }

    // ================================================================ Spoiler log (10a-3f), then the text
    UzDialog {
        id: spoilerDialog
        objectName: "variantSpoilerDialog"
        parent: Window.contentItem
        z: 110
        label: qsTr("Library · %1").arg(root.gameTitle)
        title: qsTr("Show the spoiler log?")
        text: qsTr("It lists everything the randomizer changed in %1. Uzume never shows it in game or to others.").arg(root.target.name ?? "")
        actions: [
            UzButton { id: spoilerCancel; objectName: "variantSpoilerCancel"; text: qsTr("Cancel"); onClicked: spoilerDialog.close() },
            UzButton {
                objectName: "variantSpoilerShow"
                kind: "primary"
                text: qsTr("Show spoiler log")
                onClicked: {
                    const r = root.vc.spoilerLog(root.target.id, true)
                    spoilerDialog.shown = false
                    spoilerLog.text = r.ok ? r.text : ""
                    spoilerLog.error = r.ok ? "" : (r.error === "run_not_over" ? qsTr("The spoiler log of a run opens after the run ends.") : qsTr("There is no spoiler log for this variant."))
                    spoilerLog.open()
                }
            }
        ]
        onShownChanged: if (shown) Qt.callLater(() => spoilerCancel.forceActiveFocus())
    }
    UzDialog {
        id: spoilerLog
        objectName: "variantSpoilerLogDialog"
        property string text: ""
        parent: Window.contentItem
        z: 115
        size: "lg"
        label: qsTr("Spoiler log")
        title: root.target.name ?? ""
        Rectangle {
            visible: spoilerLog.text !== ""
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(360, Math.max(120, spoilerFlick.contentHeight + 2 * Theme.space15))
            color: Theme.placeholder
            border.width: 1
            border.color: Theme.placeholderRule
            Flickable {
                id: spoilerFlick
                anchors.fill: parent
                anchors.margins: Theme.space15
                contentWidth: width
                contentHeight: logText.implicitHeight
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar { }
                TextEdit {
                    id: logText
                    objectName: "variantSpoilerText"
                    width: spoilerFlick.width - 12
                    readOnly: true
                    selectByMouse: true
                    wrapMode: TextEdit.Wrap
                    text: spoilerLog.text
                    textFormat: TextEdit.PlainText
                    font.family: Theme.mono
                    font.pixelSize: Theme.roles.meta11.size
                    color: Theme.ink2
                }
            }
        }
        actions: [
            UzButton { id: spoilerClose; objectName: "variantSpoilerClose"; kind: "primary"; text: qsTr("Close"); onClicked: spoilerLog.close() }
        ]
        onShownChanged: if (shown) Qt.callLater(() => spoilerClose.forceActiveFocus())
    }

    // ================================================================ Problems (10d-1 d-f, 10d-3, 10d-4 c-e)
    UzDialog {
        id: problem
        objectName: "variantProblemDialog"
        parent: Window.contentItem
        z: 110
        size: "lg"
        readonly property string code: root.target.error ?? ""
        // "details" (engine failed and the rest), "dump", "memory", "disk", "file", "engine", "offline"
        readonly property string kind: code === "base_mismatch" ? "dump" : code === "not_enough_memory" ? "memory" : code === "disk_full" ? "disk"
                                      : code === "base_missing" ? "file" : (code === "engine_missing" || code === "engine_not_offered" || code === "engine_download_failed") ? "engine"
                                      : code === "server_unreachable" ? "offline" : "details"
        readonly property string name: root.target.name ?? ""
        readonly property string detailsText: root.vc.failureDetails(root.target.id ?? "")
        readonly property string shaLine: {
            const e = root.target.expectedSha ?? "", a = root.target.actualSha ?? ""
            const abbr = (s) => s.length > 10 ? s.slice(0, 4).toUpperCase() + "…" + s.slice(-4).toUpperCase() : s.toUpperCase()
            return e === "" && a === "" ? "" : qsTr("EXPECTED %1 · YOURS %2").arg(abbr(e)).arg(abbr(a))
        }
        label: qsTr("Randomizer · %1").arg(root.gameTitle)
        title: kind === "dump" ? qsTr("The randomizer doesn’t know this dump of %1.").arg(root.gameTitle)
               : kind === "memory" ? qsTr("Not enough memory to build.")
               : kind === "disk" ? qsTr("Not enough space on this PC")
               : kind === "file" ? qsTr("%1 isn’t on this PC").arg(root.gameTitle)
               : kind === "engine" ? qsTr("The randomizer engine isn’t available")
               : kind === "offline" ? qsTr("%1 is offline").arg(root.serverName)
               : qsTr("%1 couldn’t be built").arg(name)
        text: kind === "dump" ? qsTr("It may still work. Main is not touched either way.")
              : kind === "memory" ? qsTr("%1 needs about %2 free; %3 is free now. Close other programs and try again.").arg(root.gameTitle).arg(root.target.neededText ?? "").arg(root.target.freeText ?? "")
              : kind === "disk" ? qsTr("%1 needs %2; %3 is free.").arg(name).arg(root.target.neededText ?? "").arg(root.target.freeText ?? "")
              : kind === "file" ? qsTr("The game file couldn’t be downloaded from %1. Try again.").arg(root.serverName)
              : kind === "engine" ? qsTr("%1 Ask an admin to turn on Offer to apps in Server › Tools.").arg(root.target.errorText ?? "")
              : kind === "offline" ? qsTr("Try again when %1 is back.").arg(root.serverName)
              : (root.target.errorText ?? "") + (root.target.engineError ? "\n" + qsTr("Engine: %1").arg(root.target.engineError) : "")
        UzMono {
            objectName: "variantProblemMono"
            visible: problem.kind === "dump" && problem.shaLine !== ""
            Layout.fillWidth: true
            wrapMode: Text.WrapAnywhere
            font.pixelSize: Theme.roles.meta11.size - 1
            color: Theme.dim
            text: (root.player.selectedGame.system ?? "").toUpperCase() + " · " + problem.shaLine
        }
        Rectangle {
            visible: problem.kind === "details" && problem.detailsText !== ""
            Layout.fillWidth: true
            implicitHeight: detailsCol.implicitHeight + 2 * Theme.space15
            color: Theme.placeholder
            border.width: 1
            border.color: Theme.placeholderRule
            ColumnLayout {
                id: detailsCol
                anchors.fill: parent
                anchors.margins: Theme.space15
                spacing: 0
                UzMono {
                    objectName: "variantProblemDetails"
                    Layout.fillWidth: true
                    wrapMode: Text.WrapAnywhere
                    font.pixelSize: Theme.roles.meta11.size
                    lineHeight: 18
                    lineHeightMode: Text.FixedHeight
                    color: Theme.ink2
                    text: problem.detailsText
                }
            }
        }
        UzLabel {
            visible: problem.kind === "details"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            font.pixelSize: Theme.roles.small.size
            color: Theme.muted
            text: qsTr("Details hold no spoilers: no creature, item or trainer names.")
        }
        function close_() { problem.shown = false; root.vc.dismissFailure(root.target.id ?? "") }
        actions: [
            UzButton {
                id: primaryButton
                objectName: "variantProblemPrimary"
                kind: "primary"
                text: problem.kind === "dump" ? qsTr("Build anyway") : problem.kind === "disk" ? qsTr("Open Storage") : qsTr("Try again")
                onClicked: {
                    const id = root.target.id
                    problem.shown = false
                    if (problem.kind === "disk") {
                        root.player.showSettingsSection("storage")
                        return
                    }
                    root.vc.dismissFailure(id)
                    root.vc.rebuild(id, problem.kind === "dump")
                    root.rebuilding(id)
                }
            },
            UzButton {
                objectName: "variantProblemCopy"
                visible: problem.kind === "details"
                text: qsTr("Copy details")
                onClicked: root.player.saveHistory.copyText(problem.detailsText)
            },
            UzButton {
                objectName: "variantProblemClose"
                text: problem.kind === "dump" ? qsTr("Cancel") : qsTr("Close")
                onClicked: problem.close_()
            }
        ]
        onShownChanged: if (shown) Qt.callLater(() => primaryButton.forceActiveFocus())
        Keys.onPressed: (event) => {
            if (problem.kind === "details" && (event.modifiers & Qt.ControlModifier) && event.key === Qt.Key_C) {
                root.player.saveHistory.copyText(problem.detailsText)
                event.accepted = true
            }
        }
    }
}
