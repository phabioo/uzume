import QtQuick
import QtQuick.Layouts

// Row for a saved (or currently attempted) server in 3a (start and connect screens, usability pass 6): a 56 px status line with
// hairlines (name and address, "last used", one primary Connect, a "⋯" menu with Remove). States that need more room (errors,
// a changed certificate) open below the line.
Rectangle {
    id: card
    required property var hub
    objectName: "hubCard_" + hub.hubId

    signal connectRequested(string hubId)
    signal retryRequested()
    signal removeRequested(string hubId)
    signal trustCertificateRequested(string fingerprint)
    signal cancelCertificateRequested()

    property bool confirmingTrust: false
    property bool showDetails: false
    onStatusChanged: if (status !== "certChanged") confirmingTrust = false
    readonly property string status: hub.status
    readonly property string tone: hub.tone

    readonly property bool plain: status === "idle" || status === "connecting"
    implicitHeight: body.implicitHeight + (plain ? 0 : 14)
    color: "transparent"
    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: card.status === "certChanged" ? Theme.danger : Theme.rule2 }

    ColumnLayout {
        id: body
        anchors.fill: parent
        anchors.bottomMargin: card.plain ? 0 : 14
        spacing: 10

        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 56
            spacing: Theme.space15
            StatusDot {
                objectName: "hubStatusDot"
                tone: card.status === "idle" ? "ok" : card.tone
                Accessible.role: Accessible.StaticText
                Accessible.name: card.status === "idle" ? qsTr("Reachable") : card.hub.statusText
            }
            ColumnLayout {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                spacing: 2
                UzLabel {
                    objectName: "hubCardName"
                    Layout.fillWidth: true
                    text: card.hub.name
                    font.pixelSize: Theme.roles.itemTitle.size
                    font.weight: Font.Medium
                    elide: Text.ElideRight
                }
                UzMono {
                    objectName: "hubCardAddress"
                    Layout.fillWidth: true
                    text: card.hub.addressText || card.hub.detail
                    elide: Text.ElideRight
                }
            }
            UzTag {
                objectName: "hubStatusPill"
                visible: !card.plain
                tone: card.tone
                text: card.hub.statusText
            }
            UzMono {
                objectName: "hubLastUsed"
                visible: card.plain && card.hub.saved && (card.hub.lastConnectedText || "") !== ""
                text: qsTr("Last used %1").arg(card.hub.lastConnectedText || "").toUpperCase()
                elide: Text.ElideRight
                Layout.maximumWidth: 180
            }
            UzButton {
                objectName: "hubConnectButton"
                visible: card.status === "idle" || card.status === "connecting"
                kind: "primary"
                busy: card.status === "connecting"
                busyOnClick: true
                enabled: card.status === "idle"
                text: card.status === "connecting" ? qsTr("Connecting…") : qsTr("Connect")
                onClicked: card.connectRequested(card.hub.hubId)
            }
            MoreMenu {
                objectName: "hubMenu"
                visible: card.status !== "connecting"
                tooltip: qsTr("More actions for %1").arg(card.hub.name)
                items: [{ name: "remove", label: card.hub.saved ? qsTr("Remove") : qsTr("Discard"), danger: true }]
                onActivated: name => { if (name === "remove") card.removeRequested(card.hub.hubId) }
            }
        }

        // Notice on error/warning (certificate changed, hub/player too old, not reachable)
        Rectangle {
            Layout.fillWidth: true
            visible: card.hub.message !== ""
            implicitHeight: msg.implicitHeight + 24
            radius: Theme.radiusNone
            color: card.tone === "warn" ? Theme.ground : Theme.toneBg(card.tone)
            border.width: card.tone === "warn" ? 1 : 0
            border.color: Theme.rule
            UzLabel {
                id: msg
                anchors.fill: parent
                anchors.margins: 12
                text: card.hub.message
                font.pixelSize: Theme.roles.input.size
                wrapMode: Text.WordWrap
                color: card.tone === "error" ? Theme.danger : (card.tone === "warn" ? Theme.ink2 : Theme.ink)
            }
        }

        // The raw technical error behind a "Details" link (R-P20).
        ColumnLayout {
            Layout.fillWidth: true
            visible: (card.hub.messageDetail || "") !== ""
            spacing: 4
            UzButton {
                objectName: "hubErrorDetailsToggle"
                kind: "link"
                text: card.showDetails ? qsTr("Hide details") : qsTr("Details")
                onClicked: card.showDetails = !card.showDetails
            }
            UzMono {
                objectName: "hubErrorDetails"
                visible: card.showDetails
                Layout.fillWidth: true
                text: card.hub.messageDetail || ""
                wrapMode: Text.WrapAnywhere
                textFormat: Text.PlainText
            }
        }

        GridLayout {
            objectName: "certFingerprints"
            Layout.fillWidth: true
            visible: card.status === "certChanged"
            columns: width > 640 ? 2 : 1
            columnSpacing: 20
            rowSpacing: 8
            ColumnLayout {
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                Layout.alignment: Qt.AlignTop
                spacing: 4
                Eyebrow { text: qsTr("Saved (SHA-256)") }
                UzMono { objectName: "certExpectedFingerprint"; Layout.fillWidth: true; text: card.hub.expectedFingerprint || ""; color: Theme.ink; wrapMode: Text.WrapAnywhere }
            }
            ColumnLayout {
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                Layout.alignment: Qt.AlignTop
                spacing: 4
                Eyebrow { text: qsTr("Presented by the server now (SHA-256)") }
                UzMono { objectName: "certObservedFingerprint"; Layout.fillWidth: true; text: card.hub.observedFingerprint || ""; color: Theme.danger; wrapMode: Text.WrapAnywhere }
            }
        }

        UzLabel {
            objectName: "certHint"
            Layout.fillWidth: true
            visible: card.status === "certChanged"
            text: qsTr("Open the server's web Settings page and compare its certificate fingerprint with the one presented now. "
                       + "A server renews its certificate automatically before it expires.")
            font.pixelSize: Theme.roles.input.size
            color: Theme.muted
            wrapMode: Text.WordWrap
        }

        Rectangle {
            objectName: "certConfirmBox"
            Layout.fillWidth: true
            visible: card.status === "certChanged" && card.confirmingTrust
            implicitHeight: confirmCol.implicitHeight + 24
            radius: 0
            color: Theme.toneBg("error")
            ColumnLayout {
                id: confirmCol
                anchors.fill: parent
                anchors.margins: 12
                spacing: 10
                UzLabel {
                    Layout.fillWidth: true
                    text: qsTr("Trust this certificate only if both fingerprints match the server's Settings page. "
                               + "Your saved sign-in is kept and used for the new certificate.")
                    font.pixelSize: Theme.roles.input.size
                    wrapMode: Text.WordWrap
                    color: Theme.danger
                }
                Flow {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    spacing: 10
                    UzButton {
                        objectName: "certConfirmTrust"
                        kind: "primary"
                        text: qsTr("Yes, trust this certificate")
                        onClicked: {
                            card.confirmingTrust = false
                            card.trustCertificateRequested(card.hub.observedFingerprintRaw)
                        }
                    }
                    UzButton {
                        objectName: "certConfirmBack"
                        text: qsTr("Back")
                        onClicked: card.confirmingTrust = false
                    }
                }
            }
        }

        // The action buttons wrap onto further lines when a wide font leaves them too little room.
        // Anchors (not a Flow inside a RowLayout): a Flow reports an implicit width that depends on its own width, and as a
        // layout item it collapsed to a few pixels, which squeezed the whole saved-Hub card (R-P2).
        Item {
            id: actions
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            visible: !card.plain
            implicitHeight: actionFlow.implicitHeight
            Flow {
                id: actionFlow
                anchors.left: parent.left
                anchors.right: parent.right
                spacing: 10
                UzButton {
                    visible: card.status === "unreachable" || card.status === "userDisabled"
                    busyOnClick: true
                    text: qsTr("Retry")
                    onClicked: card.retryRequested()
                }
                UzButton {
                    objectName: "certTrustNew"
                    visible: card.status === "certChanged" && !card.confirmingTrust
                    text: qsTr("Trust new certificate")
                    onClicked: card.confirmingTrust = true
                }
                UzButton {
                    objectName: "certCancel"
                    visible: card.status === "certChanged"
                    text: qsTr("Cancel")
                    onClicked: card.cancelCertificateRequested()
                }
            }
        }
    }
}
