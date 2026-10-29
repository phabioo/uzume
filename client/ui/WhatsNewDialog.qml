import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// What's new (accounts-and-server.md 6a-app-1 and 6a-app-2): one dialog "Uzume <version>" shown once after an update, with the
// release notes of the installed version from the signed updates index (`player.updates.whatsNewNotes`: groups `### Added`,
// bullets `- line`). Nothing is invented: a bullet gets an App / Server / Both tag only when the notes carry a prefix (`App:`,
// `Server:`, `Both:`; the older `Player:` and `Hub:` too), otherwise the tag column stays empty. Members do not see Server items.
// Admins also get the server-version line when the server is older than this app ("<name> runs 0.16.4. Update it to get the server
// items.") with "Open Updates". Closing remembers the version, so it is shown once per version. The index carries no release date:
// the label has none.
// objectNames: whatsNewDialog, whatsNewGroup_<n>, whatsNewItem_<n>_<m>, whatsNewServerLine, whatsNewOpenUpdates, whatsNewDone.
UzDialog {
    id: root
    objectName: "whatsNewDialog"
    required property PlayerController player
    readonly property bool admin: root.player.isAdmin
    readonly property string version: root.player.updates.currentVersion
    readonly property bool want: root.player.updates.whatsNewPending
                                 && ["library", "runs", "settings", "server", "plugin"].indexOf(root.player.screen) >= 0
    readonly property var groups: root.parse(root.player.updates.whatsNewNotes, root.admin)
    readonly property bool serverOlder: root.admin && root.player.versionRelation === "server_older"

    // [{ title, items: [{ text, tag }] }]; `tag` is "App" | "Server" | "Both" | "" (not carried by the notes).
    function parse(notes: string, admin: bool): var {
        const out = []
        let cur = null
        for (const raw of String(notes).split("\n")) {
            const line = raw.trim()
            if (line === "") continue
            if (line.indexOf("###") === 0) {
                cur = { title: line.replace(/^#+\s*/, ""), items: [] }
                out.push(cur)
                continue
            }
            if (line.indexOf("- ") !== 0 && line.indexOf("* ") !== 0) continue
            let text = line.slice(2).trim()
            let tag = ""
            const m = /^(server|hub|app|player|both)\s*:\s*/i.exec(text)
            if (m) {
                const k = m[1].toLowerCase()
                tag = k === "server" || k === "hub" ? "Server" : k === "both" ? "Both" : "App"
                text = text.slice(m[0].length)
            }
            text = text.replace(/`/g, "")  // inline code marks of the Markdown notes are shown as plain text
            if (text === "" || (!admin && tag === "Server")) continue
            if (cur === null) { cur = { title: "", items: [] }; out.push(cur) }
            cur.items.push({ text: text, tag: tag })
        }
        return out.filter(g => g.items.length > 0)
    }
    function tagText(tag: string): string {
        return tag === "Server" ? qsTr("Server") : tag === "Both" ? qsTr("Both") : tag === "App" ? qsTr("App") : ""
    }
    function groupTitle(t: string): string {
        switch (t.toLowerCase()) {
        case "added": return qsTr("Added")
        case "changed": return qsTr("Changed")
        case "fixed": return qsTr("Fixed")
        case "removed": return qsTr("Removed")
        case "security": return qsTr("Security")
        default: return t
        }
    }

    size: "lg"
    label: qsTr("What's new")
    title: qsTr("Uzume %1").arg(root.player.updates.shown(root.version))
    onWantChanged: if (root.want) root.open()
    Component.onCompleted: if (root.want) root.open()
    onClosed: root.player.updates.dismissWhatsNew()

    Flickable {
        id: scroller
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(body.implicitHeight, 320)
        contentWidth: width
        contentHeight: body.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}
        ColumnLayout {
            id: body
            width: scroller.width - Theme.space2
            spacing: Theme.space2
            Repeater {
                model: root.groups
                delegate: ColumnLayout {
                    id: group
                    required property var modelData
                    required property int index
                    objectName: "whatsNewGroup_" + group.index
                    Layout.fillWidth: true
                    spacing: Theme.space05
                    UzSectionLabel { Layout.fillWidth: true; text: root.groupTitle(group.modelData.title) }
                    Repeater {
                        model: group.modelData.items
                        delegate: RowLayout {
                            id: item
                            required property var modelData
                            required property int index
                            objectName: "whatsNewItem_" + group.index + "_" + item.index
                            Layout.fillWidth: true
                            spacing: Theme.space15
                            Text {
                                Layout.fillWidth: true
                                wrapMode: Text.Wrap
                                textFormat: Text.PlainText
                                text: item.modelData.text
                                font.family: Theme.sans
                                font.pixelSize: Theme.roles.input.size
                                color: Theme.ink
                            }
                            UzTag { visible: item.modelData.tag !== ""; Layout.alignment: Qt.AlignTop; text: root.tagText(item.modelData.tag) }
                        }
                    }
                }
            }
        }
    }
    UzStatusIcon {
        objectName: "whatsNewServerLine"
        Layout.fillWidth: true
        visible: root.serverOlder
        kind: "warning"
        small: true
        maxWidth: 480
        text: qsTr("%1 runs %2. Update it to get the server items.").arg(root.player.hubName !== "" ? root.player.hubName : qsTr("The server")).arg(root.player.updates.shown(root.player.hubVersion))
    }

    actions: [
        UzButton {
            objectName: "whatsNewOpenUpdates"
            visible: root.admin
            text: qsTr("Open Updates")
            onClicked: { root.close(); root.player.showSettingsSection("server:updates") }
        },
        UzButton { objectName: "whatsNewDone"; kind: "primary"; text: qsTr("Got it"); onClicked: root.close() }
    ]
}
