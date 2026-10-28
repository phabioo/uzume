import QtQuick
import QtQuick.Layouts
import Uzume.App

// "Go back to <version>?" (design 6c-2): what happens, what stays, what is turned off. Modal; "Cancel" has the focus, Esc
// cancels. While a game or Session runs the primary is muted and the dialog says why. The Player closes, installs the
// older version and starts again; settings and saves are not touched.
// A UzDialog (design-system.md "Dialog", size lg) in a thin host that keeps the openFor() / close() interface.
Item {
    id: root
    required property var updates
    property string version: ""
    property var info: ({})
    readonly property bool shown: version !== ""
    readonly property bool blocked: root.updates.busy || !root.info.canGoBack

    function openFor(v) {
        info = updates.rollbackInfo(v)
        version = v
    }
    function close() { version = "" }

    visible: shown
    anchors.fill: parent
    z: 100

    UzDialog {
        id: dialog
        size: "lg"
        title: qsTr("Go back to %1?").arg(root.updates.shown(root.version))
        text: qsTr("Uzume closes, installs %1 and starts again. About 30 s.").arg(root.updates.shown(root.version))

        Binding { target: dialog; property: "shown"; value: root.shown }
        onClosed: root.close()
        Component.onCompleted: frame.objectName = "rollbackDialog"
        onShownChanged: if (shown) Qt.callLater(() => cancelButton.forceActiveFocus())

        UzLabel {
            objectName: "rollbackBusy"
            visible: root.updates.busy
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            text: "⊘ " + (root.info.busyText || "")
            color: Theme.accentText
            wrapMode: Text.WordWrap
        }
        UzLabel {
            objectName: "rollbackUnsupported"
            visible: !root.info.canGoBack
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            text: root.updates.goBackUnavailableText
            color: Theme.accentText
            wrapMode: Text.WordWrap
        }

        // What stays and what changes: label column + text, hairlines between the rows (a settings-row look, no cards).
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 0
            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.rule }
            Repeater {
                model: [
                    { name: "Settings", title: qsTr("Settings"),
                      text: qsTr("Kept. Newer settings are ignored and come back after the update.") },
                    { name: "Saves", title: qsTr("Saves and states"),
                      text: qsTr("Not touched. They are on your server.") },
                    { name: "Plugins", title: qsTr("Plugins"),
                      text: root.info.pluginLines && root.info.pluginLines.length > 0
                            ? root.info.pluginLines.join("\n") : qsTr("All keep working."),
                      warn: root.info.pluginLines && root.info.pluginLines.length > 0 },
                    { name: "Updates", title: qsTr("Updates"),
                      text: qsTr("Paused until you resume them. %1 stays available.").arg(root.updates.shown(root.updates.currentVersion)) }
                ]
                delegate: ColumnLayout {
                    id: infoRow
                    required property var modelData
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    spacing: 0
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: Theme.space1
                        Layout.bottomMargin: Theme.space1
                        spacing: Theme.space2
                        UzLabel {
                            Layout.preferredWidth: 120
                            Layout.alignment: Qt.AlignTop
                            text: infoRow.modelData.title
                            color: Theme.muted
                            font.pixelSize: Theme.roles.input.size
                            elide: Text.ElideRight
                        }
                        UzLabel {
                            objectName: "rollbackRow" + infoRow.modelData.name
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            text: infoRow.modelData.text
                            color: infoRow.modelData.warn === true ? Theme.accentText : Theme.ink
                            font.pixelSize: Theme.roles.input.size
                            wrapMode: Text.WordWrap
                        }
                    }
                    Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.rule2 }
                }
            }
        }

        actions: [
            UzButton {
                id: cancelButton
                objectName: "rollbackCancel"
                text: qsTr("Cancel")
                KeyNavigation.tab: goButton
                KeyNavigation.backtab: goButton
                Keys.onReturnPressed: root.close()
                onClicked: root.close()
            },
            UzButton {
                id: goButton
                objectName: "rollbackConfirm"
                kind: "primary"
                enabled: !root.blocked
                // Shrinks (and elides) a long version label by itself.
                width: Math.max(0, Math.min(implicitWidth, 360, dialog.frame.width - 2 * 28 - cancelButton.width - Theme.space1))
                text: qsTr("Go back to %1").arg(root.updates.shown(root.version))
                KeyNavigation.tab: cancelButton
                KeyNavigation.backtab: cancelButton
                Keys.onReturnPressed: if (!root.blocked) clicked()
                onClicked: {
                    const v = root.version
                    root.close()
                    root.updates.goBack(v)
                }
            }
        ]
    }
}
