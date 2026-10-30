import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// What's new (accounts-and-server.md 6a-app-1 and 6a-app-2): one dialog "Uzume <version>" shown once after an update, with the
// release notes of the installed version from the signed updates index (`player.updates.whatsNewNotes`). The notes are what
// scripts/changelog.py `render_index` writes: group lines `Added:` / `Changed:` / `Fixed:` / `Removed:` / `Security:`, bullets
// `- line`, and at most one truncation line `... and N more (see the release notes)`; `### Added` headers are accepted too.
// Groups appear in that fixed order. Layout: a muted mono summary line ("8 new · 3 changed · 12 fixed"), then per group a section
// label with the count as meta and one row per item (leading marker, hairline between rows). An item that starts with a short
// area name and ": " shows the area as a medium-weight line above the text. A bullet gets an App / Server / Both tag only when
// the notes carry a prefix (`App:`, `Server:`, `Both:`; the older `Player:` and `Hub:` too), otherwise the tag column stays
// empty. Members do not see Server items. The list scrolls (bar, fade at the bottom while more is below; Up/Down/PageUp/PageDown
// when the dialog has focus) and uses up to about 62 % of the window height. When the notes were cut ("N more") a muted footer
// row says so and "Full release notes" opens the version's notes_url (only when the index has one). Admins also get the
// server-version line when the server is older than this app ("<name> runs 0.16.4. Update it to get the server items.") with
// "Open Updates". Closing remembers the version, so it is shown once per version. The index carries no release date: the label has none.
// objectNames: whatsNewDialog, whatsNewSummary, whatsNewGroup_<n>, whatsNewItem_<n>_<m>, whatsNewMore, whatsNewFullNotes,
// whatsNewServerLine, whatsNewOpenUpdates, whatsNewDone.
UzDialog {
    id: root
    objectName: "whatsNewDialog"
    required property PlayerController player
    readonly property bool admin: root.player.isAdmin
    readonly property string version: root.player.updates.currentVersion
    readonly property bool want: root.player.updates.whatsNewPending
                                 && ["library", "runs", "settings", "server", "plugin"].indexOf(root.player.screen) >= 0
    readonly property var parsed: root.parse(root.player.updates.whatsNewNotes, root.admin)
    readonly property var groups: root.parsed.groups
    readonly property int more: root.parsed.more
    readonly property string summary: root.summaryText(root.groups)
    readonly property bool serverOlder: root.admin && root.player.versionRelation === "server_older"

    // Room for the list: the window minus the frame margins, the dialog's own parts and the other content items (none of them
    // depends on the list, so there is no binding loop); at least 120 px, so a tiny window still scrolls instead of clipping buttons.
    readonly property real listRoom: {
        let others = 0
        let gaps = 0
        for (const it of [summaryLine, moreRow, serverLine]) {
            if (!it.visible) continue
            others += it.implicitHeight
            gaps += 1
        }
        return Math.max(120, root.height - 2 * Theme.space2 - root.chromeHeight - others - gaps * Theme.space2)
    }
    readonly property var order: ["", "added", "changed", "fixed", "removed", "security"]

    // { groups: [{ key, title, items: [{ text, lead, rest, tag }] }], more }; `tag` is "App" | "Server" | "Both" | "" (not carried
    // by the notes); `lead` is the area name before the first ": " (or ""), `rest` the remaining text.
    function parse(notes: string, admin: bool): var {
        const byKey = {}
        const extra = []
        let cur = null
        let more = 0
        const open = (title) => {
            const key = title.toLowerCase()
            if (byKey[key] === undefined) {
                byKey[key] = { key: key, title: title, items: [] }
                if (root.order.indexOf(key) < 0) extra.push(byKey[key])
            }
            return byKey[key]
        }
        for (const raw of String(notes).split("\n")) {
            const line = raw.trim()
            if (line === "") continue
            const mm = /^(?:\.\.\.|\u2026)\s*and\s+(\d+)\s+more\b/i.exec(line)
            if (mm) { more += parseInt(mm[1]); continue }
            const known = /^(?:#+\s*)?(added|changed|fixed|removed|security)\s*:?\s*$/i.exec(line)
            if (known) { cur = open(known[1]); continue }
            if (line.indexOf("###") === 0) { cur = open(line.replace(/^#+\s*/, "").replace(/:\s*$/, "")); continue }
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
            if (cur === null) cur = open("")
            const lm = /^([^:.;]{2,48}):\s+(.+)$/.exec(text)
            cur.items.push({ text: text, lead: lm ? lm[1] : "", rest: lm ? lm[2].charAt(0).toUpperCase() + lm[2].slice(1) : text, tag: tag })
        }
        const groups = []
        for (const key of root.order) if (byKey[key] !== undefined && byKey[key].items.length > 0) groups.push(byKey[key])
        for (const g of extra) if (g.items.length > 0) groups.push(g)
        return { groups: groups, more: more }
    }
    function summaryText(groups: var): string {
        const n = (key) => { for (const g of groups) if (g.key === key) return g.items.length; return 0 }
        const parts = []
        if (n("added") > 0) parts.push(qsTr("%1 new").arg(n("added")))
        if (n("changed") > 0) parts.push(qsTr("%1 changed").arg(n("changed")))
        if (n("fixed") > 0) parts.push(qsTr("%1 fixed").arg(n("fixed")))
        if (n("removed") > 0) parts.push(qsTr("%1 removed").arg(n("removed")))
        if (n("security") > 0) parts.push(qsTr("%1 security").arg(n("security")))
        return parts.join(" \u00b7 ")
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
    function scrollBy(dy: real) {
        scroller.contentY = Math.max(0, Math.min(Math.max(0, scroller.contentHeight - scroller.height), scroller.contentY + dy))
    }
    Keys.onPressed: (event) => {
        if (!root.shown) return
        switch (event.key) {
        case Qt.Key_Down: root.scrollBy(48); event.accepted = true; break
        case Qt.Key_Up: root.scrollBy(-48); event.accepted = true; break
        case Qt.Key_PageDown: root.scrollBy(scroller.height * 0.9); event.accepted = true; break
        case Qt.Key_PageUp: root.scrollBy(-scroller.height * 0.9); event.accepted = true; break
        }
    }

    size: "lg"
    label: qsTr("What's new")
    title: qsTr("Uzume %1").arg(root.player.updates.shown(root.version))
    onWantChanged: if (root.want) root.open()
    Component.onCompleted: if (root.want) root.open()
    onClosed: root.player.updates.dismissWhatsNew()

    Text {
        id: summaryLine
        objectName: "whatsNewSummary"
        visible: root.summary !== ""
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
        text: root.summary
        font.family: Theme.mono
        font.pixelSize: Theme.roles.meta.size
        font.letterSpacing: Theme.roles.meta.spacing
        font.capitalization: Font.AllUppercase
        color: Theme.muted
    }
    Item {
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(body.implicitHeight, Math.round(root.height * 0.62), root.listRoom)
        Flickable {
            id: scroller
            anchors.fill: parent
            contentWidth: width
            contentHeight: body.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: scroller.contentHeight > scroller.height ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff }
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
                        UzSectionLabel {
                            Layout.fillWidth: true
                            text: root.groupTitle(group.modelData.title)
                            meta: String(group.modelData.items.length)
                        }
                        Repeater {
                            model: group.modelData.items
                            delegate: ColumnLayout {
                                id: item
                                required property var modelData
                                required property int index
                                readonly property string text: item.modelData.text
                                objectName: "whatsNewItem_" + group.index + "_" + item.index
                                Layout.fillWidth: true
                                spacing: 0
                                Rectangle {   // hairline between rows, none above the first
                                    visible: item.index > 0
                                    Layout.fillWidth: true
                                    implicitHeight: 1
                                    color: Theme.rule2
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    Layout.topMargin: Theme.space1
                                    Layout.bottomMargin: Theme.space1
                                    spacing: Theme.space15
                                    Rectangle {   // marker per group kind: Added in the accent, the others muted
                                        Layout.alignment: Qt.AlignTop
                                        Layout.topMargin: 4
                                        implicitWidth: 3
                                        implicitHeight: 14
                                        color: group.modelData.key === "added" ? Theme.accent : Theme.muted
                                        opacity: group.modelData.key === "added" ? 1 : 0.6
                                    }
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 2
                                        Text {
                                            visible: item.modelData.lead !== ""
                                            Layout.fillWidth: true
                                            wrapMode: Text.Wrap
                                            textFormat: Text.PlainText
                                            text: item.modelData.lead
                                            font.family: Theme.sans
                                            font.pixelSize: Theme.roles.itemTitleSm.size
                                            font.weight: Font.Medium
                                            color: Theme.ink
                                        }
                                        Text {
                                            Layout.fillWidth: true
                                            wrapMode: Text.Wrap
                                            textFormat: Text.PlainText
                                            text: item.modelData.rest
                                            font.family: Theme.sans
                                            font.pixelSize: Theme.roles.input.size
                                            color: item.modelData.lead !== "" ? Theme.ink2 : Theme.ink
                                        }
                                    }
                                    UzTag { visible: item.modelData.tag !== ""; Layout.alignment: Qt.AlignTop; text: root.tagText(item.modelData.tag) }
                                }
                            }
                        }
                    }
                }
            }
        }
        Rectangle {   // fade while more content is below
            objectName: "whatsNewFade"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 28
            visible: !scroller.atYEnd
            gradient: Gradient {
                GradientStop { position: 0; color: Qt.rgba(Theme.surface.r, Theme.surface.g, Theme.surface.b, 0) }
                GradientStop { position: 1; color: Theme.surface }
            }
        }
    }
    RowLayout {
        id: moreRow
        objectName: "whatsNewMore"
        readonly property int count: root.more
        visible: root.more > 0
        Layout.fillWidth: true
        spacing: Theme.space15
        Text {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            text: qsTr("%n more changes in the full release notes", "", root.more)
            font.family: Theme.sans
            font.pixelSize: Theme.roles.small.size
            color: Theme.muted
        }
        UzButton {
            objectName: "whatsNewFullNotes"
            visible: root.player.updates.whatsNewNotesUrl !== ""
            kind: "ghost"
            size: "sm"
            text: qsTr("Full release notes")
            onClicked: root.player.updates.openWhatsNewNotes()
        }
    }
    UzStatusIcon {
        id: serverLine
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
