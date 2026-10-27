import QtQuick
import QtQuick.Layouts
import Uzume.App

// Server problems (runs-and-server.md 8k-2), the version states: "server older than the app" and "app older than the server",
// from what the app already has (the server's version in the handshake, this app's version; `player.versionRelation`). A line with
// the right action sits under the Library header. Only a difference in the major or minor number counts: the patch number moves
// with every beta and is no problem. Admins get the action (Update server opens Settings › Updates; Update Uzume installs or opens
// Updates), members are told whom to ask; games on this PC keep running either way. The other states of 8k-2 (service stopped,
// security code changed, starting) belong to the connection and pairing screens (app start and accounts).
// objectNames: serverProblemLine, serverProblemAction, serverProblemDevice.
ColumnLayout {
    id: root
    objectName: "serverProblemLine"
    required property PlayerController player
    function minorOf(v: string): string {
        const m = /^v?(\d+)\.(\d+)/.exec(v)
        return m ? m[1] + "." + m[2] : ""
    }
    readonly property string hubMinor: root.minorOf(root.player.hubVersion)
    readonly property string appMinor: root.minorOf(root.player.updates.currentVersion)
    readonly property string kind: root.hubMinor === "" || root.appMinor === "" || root.hubMinor === root.appMinor ? ""
                                   : root.player.versionRelation === "server_older" ? "server_older"
                                   : root.player.versionRelation === "app_older" ? "app_older" : ""
    readonly property string serverName: root.player.hubName !== "" ? root.player.hubName : qsTr("The server")
    property bool suppressed: false   // the game page is open
    visible: root.kind !== "" && !suppressed
    spacing: Theme.space05

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space2
        UzStatusIcon {
            Layout.fillWidth: true
            kind: "warning"
            small: true
            maxWidth: 720
            text: root.kind === "server_older"
                  ? qsTr("This app needs a newer server. %1 runs %2; this app is %3. %4")
                      .arg(root.serverName).arg(root.player.hubVersion).arg(root.player.updates.currentVersion)
                      .arg(root.player.isAdmin ? "" : qsTr("Ask an admin to update %1.").arg(root.serverName))
                  : qsTr("%1 needs a newer app. %1 runs %2; this app is %3. Games on this PC still run in the meantime.")
                      .arg(root.serverName).arg(root.player.hubVersion).arg(root.player.updates.currentVersion)
        }
        UzButton {
            objectName: "serverProblemAction"
            size: "sm"
            visible: root.kind === "app_older" || (root.kind === "server_older" && root.player.isAdmin)
            text: root.kind === "app_older" ? qsTr("Update Uzume") : qsTr("Update server")
            onClicked: {
                if (root.kind === "app_older" && root.player.updates.canInstall) root.player.updates.install()
                else root.player.showSettingsSection(root.kind === "server_older" ? "server:updates" : "updates")
            }
        }
    }
    UzMono {
        objectName: "serverProblemDevice"
        Layout.fillWidth: true
        visible: root.kind === "server_older" && root.player.isAdmin
        wrapMode: Text.Wrap
        text: qsTr("On the device: uzume-server update")
    }
}
