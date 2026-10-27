import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// Join a run: pick the game you play in it (the library games that fit the plugins of the run). A UzDialog lg
// (design-system.md "Dialog"): label, title, the plugins of the run, the game list, for a randomized run the RANDOMIZER block (randomizer.md
// 10f-1, 10f-2, 10f-5: the run's world and pinned engine, "Bring my own variant", the dump check, the save slot) and the actions.
UzDialog {
    id: root
    objectName: "runJoinDialog"
    required property RunsController runs
    property string runId: ""
    property string runName: ""
    property string inviteLine: ""
    readonly property bool noGame: root.runs.games.count === 0
    property int rev: 0
    readonly property VariantsController variants: root.runs.variants
    // The randomizer block of the run ({} for an ordinary run, see RunsController.randomizerOf).
    readonly property var rz: root.rev >= 0 && root.runId !== "" ? root.runs.randomizerOf(root.runId) : ({})
    readonly property bool randomized: root.rz.mode !== undefined
    readonly property string pickedSha: {
        const g = root.runs.games
        for (let i = 0; i < g.count; ++i) if (g.get(i).id === picker.selectedId) return g.get(i).sha ?? ""
        return ""
    }
    // "same" | "different" | "" (unknown: no pin in the run, or the game's hash is not known)
    readonly property string dump: !root.randomized || (root.rz.baseSha256 ?? "") === "" || root.pickedSha === "" ? ""
                                   : (root.pickedSha === root.rz.baseSha256 ? "same" : "different")
    property bool bringOwn: false
    property string ownVariantId: ""
    readonly property var ownRows: {
        const loaded = root.variants.variants   // the list of the game set with setGame(): re-read when it changes
        if (!root.randomized || picker.selectedId === "" || loaded === undefined) return []
        return root.variants.variantsOf(picker.selectedId).filter(v => v.isRun !== true)
    }
    readonly property var ownOptions: {
        const out = [{ value: "", label: qsTr("Build the run’s variant"), hint: root.variants.presetLabel(root.rz.presetId ?? "") }]
        for (const v of root.ownRows) {
            const match = v.toolVersion === root.rz.toolVersion
            const preset = (v.presetId ?? "") !== "" ? root.variants.presetLabel(v.presetId) : ""
            out.push({ value: v.id, label: v.name, hint: match ? (preset !== "" ? qsTr("%1 · same engine").arg(preset) : qsTr("Same engine")) : qsTr("Older engine · doesn’t match"),
                       disabled: !match })
        }
        return out
    }
    function shortSha(sha: string): string { return sha.length > 8 ? sha.substring(0, 4).toUpperCase() + "…" + sha.substring(sha.length - 4).toUpperCase() : sha.toUpperCase() }

    size: "lg"
    label: qsTr("Runs · Invite")
    title: qsTr("Join “%1”?").arg(root.runName)
    text: root.inviteLine !== "" ? root.inviteLine : qsTr("Which game do you play in this run?")

    function openFor(id: string, name: string, line: string) {
        root.runId = id
        root.runName = name
        root.inviteLine = line || ""
        root.runs.prepareGames(id)
        root.bringOwn = false
        root.ownVariantId = ""
        root.rev++
        picker.selectedId = root.runs.games.count === 1 ? root.runs.games.get(0).id : ""
        root.open()
    }

    // The library or the server's plugin list may arrive after the dialog opened: a single fitting game is picked then, too,
    // but never over a choice the user made.
    Connections {
        target: root.runs.games
        function onModelReset() {
            if (root.shown && picker.selectedId === "" && root.runs.games.count === 1)
                picker.selectedId = root.runs.games.get(0).id
        }
    }

    Connections {
        target: picker
        function onSelectedIdChanged() {
            root.ownVariantId = ""
            if (root.randomized && picker.selectedId !== "") root.variants.setGame(picker.selectedId)
        }
    }

    ColumnLayout {
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        spacing: Theme.space1
        visible: jpRep.count > 0
        UzSectionLabel { Layout.fillWidth: true; text: qsTr("Plugins of this run") }
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.rule }
        Repeater {
            id: jpRep
            model: root.runs.joinPlugins
            delegate: Item {
                id: jp
                required property var model
                objectName: "runJoinPlugin_" + jp.model.id
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                implicitHeight: jpCol.implicitHeight + Theme.space15
                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.rule2 }
                ColumnLayout {
                    id: jpCol
                    x: Theme.space15
                    width: parent.width - Theme.space15
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 0
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.space1
                        UzLabel {
                            Layout.fillWidth: true
                            text: jp.model.name + " " + jp.model.version
                            elide: Text.ElideRight
                            font.weight: Font.Medium
                        }
                        PluginOriginPill {
                            objectName: "runJoinPluginOrigin"
                            origin: jp.model.origin
                            text: jp.model.originText
                        }
                    }
                    UzLabel {
                        Layout.fillWidth: true
                        text: qsTr("It will be able to: %1").arg(jp.model.permissionsText)
                        wrapMode: Text.WordWrap
                        color: Theme.muted
                        font.pixelSize: Theme.roles.small.size
                    }
                }
            }
        }
    }
    UnverifiedNote {
        objectName: "runJoinUnverified"
        visible: root.runs.joinUnverified
        text: root.runs.unverifiedWarning
        Layout.fillWidth: true
    }
    ColumnLayout {
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        spacing: Theme.space1
        UzSectionLabel { Layout.fillWidth: true; text: qsTr("Your game") }
        GamePickList {
            id: picker
            objectName: "runJoinGames"
            Layout.fillWidth: true
            rows: root.runs.games
        }
    }
    // 10f-5a: the picked file is not the dump the run was made for.
    ColumnLayout {
        objectName: "runJoinDump"
        visible: root.randomized && root.dump !== ""
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        spacing: Theme.space05
        UzStatusIcon {
            objectName: "runJoinDumpLine"
            kind: root.dump === "same" ? "done" : "warning"
            small: true
            text: root.dump === "same" ? qsTr("Same dump as the run.") : qsTr("Your file of this game is a different dump.")
            maxWidth: 480
        }
        UzMono {
            objectName: "runJoinDumpHashes"
            visible: root.dump === "different"
            Layout.fillWidth: true
            elide: Text.ElideRight
            text: qsTr("RUN EXPECTS %1 · YOURS %2").arg(root.shortSha(root.rz.baseSha256 ?? "")).arg(root.shortSha(root.pickedSha))
        }
    }

    // RANDOMIZER (10f-1, 10f-2, 10f-5c)
    ColumnLayout {
        objectName: "runJoinRandomizer"
        visible: root.randomized
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        spacing: Theme.space1
        UzSectionLabel { Layout.fillWidth: true; text: qsTr("Randomizer") }
        Text {
            objectName: "runJoinRandomizerLine"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            text: {
                const world = root.rz.mode === "same" ? qsTr("Same world for everyone") : qsTr("Each person gets their own")
                const preset = (root.rz.presetId ?? "") !== "" ? root.variants.presetLabel(root.rz.presetId) : ""
                return (preset !== "" ? qsTr("%1 · %2.").arg(world).arg(preset) : qsTr("%1.").arg(world)) + " " + qsTr("Spoilers stay hidden.")
            }
            font.family: Theme.sans
            font.pixelSize: Theme.roles.input.size
            lineHeight: Theme.roles.small.line
            lineHeightMode: Text.FixedHeight
            color: Theme.ink2
        }
        UzMono {
            objectName: "runJoinEngine"
            Layout.fillWidth: true
            elide: Text.ElideRight
            text: qsTr("FVX %1 · PINNED BY THE RUN").arg(root.rz.toolVersion ?? "")
        }
        // Own variants are off in this run (10f-5c).
        Text {
            objectName: "runJoinOwnOff"
            visible: root.rz.allowOwn !== true
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            text: qsTr("%1’s run builds everyone’s variant. Own variants are off.").arg(root.rz.creatorName ?? qsTr("The creator"))
            font.family: Theme.sans
            font.pixelSize: Theme.roles.small.size
            color: Theme.muted
        }
        UzCheckbox {
            objectName: "runJoinBringOwn"
            visible: root.rz.allowOwn === true
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            text: qsTr("Bring my own variant")
            checked: root.bringOwn
            onToggled: {
                root.bringOwn = checked
                if (!checked) root.ownVariantId = ""
            }
        }
        ColumnLayout {
            visible: root.rz.allowOwn === true && root.bringOwn
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            spacing: Theme.space05
            Text {
                Layout.fillWidth: true
                textFormat: Text.PlainText
                elide: Text.ElideRight
                text: qsTr("Your variant")
                font.family: Theme.sans
                font.pixelSize: Theme.roles.fieldLabel.size
                font.weight: Font.Medium
                color: Theme.ink
            }
            UzSelect {
                objectName: "runJoinOwnVariant"
                Layout.fillWidth: true
                accessibleName: qsTr("Your variant")
                model: root.ownOptions
                current: root.ownVariantId
                onPicked: value => root.ownVariantId = value
            }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                text: qsTr("%1’s run allows your own variant if it uses the run’s engine version.").arg(root.rz.creatorName ?? qsTr("The creator"))
                font.family: Theme.sans
                font.pixelSize: Theme.roles.small.size
                color: Theme.muted
            }
        }
    }
    // The save slot the person gets, between two hairlines.
    ColumnLayout {
        visible: root.runName !== ""
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        spacing: Theme.space05
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.rule2 }
        UzSectionLabel { Layout.fillWidth: true; Layout.topMargin: Theme.space05; text: qsTr("Save slot") }
        Text {
            objectName: "runJoinSlotNote"
            Layout.fillWidth: true
            Layout.bottomMargin: Theme.space05
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            text: root.randomized ? qsTr("You get Run: %1. Main and your variants are not touched.").arg(root.runName)
                                  : qsTr("You get Run: %1. Main is not touched.").arg(root.runName)
            font.family: Theme.sans
            font.pixelSize: Theme.roles.input.size
            lineHeight: Theme.roles.small.line
            lineHeightMode: Text.FixedHeight
            color: Theme.ink2
        }
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.rule2 }
    }
    UzStatusIcon {
        objectName: "runJoinWhy"
        visible: root.noGame
        kind: "warning"
        small: true
        text: qsTr("Join is off until you have the game. Ask a server admin to upload it, or add a library folder that contains it.")
        maxWidth: root.frame.width - 2 * 28 - 22
    }

    actions: [
        UzButton {
            objectName: "runJoinCancel"
            text: qsTr("Cancel")
            onClicked: root.close()
        },
        UzButton {
            objectName: "runJoinConfirm"
            kind: "primary"
            text: qsTr("Join")
            enabled: picker.selectedId !== "" && root.dump !== "different"
            onClicked: {
                if (root.randomized) {
                    const own = root.bringOwn && root.ownVariantId !== ""
                    root.runs.joinRun(root.runId, picker.selectedId, own ? "own" : "run", own ? root.ownVariantId : "")
                } else {
                    root.runs.joinRun(root.runId, picker.selectedId)
                }
                root.close()
            }
        }
    ]
}
