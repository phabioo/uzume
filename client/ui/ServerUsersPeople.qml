import QtQuick
import QtQuick.Layouts
import Uzume.App

// Users › People (3n-app-1, 3n-app-3, 3n-app-1-r6): role chips with counts (All · Admins · Members · Open invites), table
// avatar + person (since), role, what the person does now, play time, devices and "⋯" (open invites: Withdraw); the selected person's details section under
// the table with the actions Make admin / Make member, Reset password, Turn off / Turn on, Remove and a new pairing code.
// Admins cannot be turned off or removed (the server refuses); the reason is shown next to the disabled action.
ColumnLayout {
    id: root
    required property var page
    readonly property var u: root.page.selected
    readonly property bool isSelf: root.page.isSelf(root.u)
    readonly property bool isAdmin: root.u !== null && root.u.role === "admin"
    readonly property bool soleAdmin: root.isAdmin && root.page.adminCount <= 1
    readonly property string name: root.u !== null ? root.u.display_name : ""
    // Why an action is not available (empty = available).
    readonly property string offReason: root.u === null ? ""
        : root.soleAdmin ? qsTr("Make someone else admin first.")
        : root.isAdmin ? qsTr("Admins can't be turned off. Make %1 a member first.").arg(root.name) : ""
    readonly property string roleReason: root.u === null ? ""
        : root.isAdmin && root.isSelf ? qsTr("You can't change your own role.")
        : root.soleAdmin ? qsTr("Make someone else admin first.")
        : !root.isAdmin && root.u.disabled ? qsTr("Turn %1 on first.").arg(root.name) : ""
    spacing: Theme.space2

    // The "⋯" menu of a person: the same actions as the details section below the list.
    function menuFor(u: var): var {
        if (!u) return []
        const self = root.page.isSelf(u)
        const admin = u.role === "admin"
        const sole = admin && root.page.adminCount <= 1
        const items = []
        items.push({ name: "rename", label: qsTr("Rename…") })
        if (admin) items.push({ name: "makeMember", label: qsTr("Make member…"), enabled: !self && !sole })
        else items.push({ name: "makeAdmin", label: qsTr("Make admin…"), enabled: !u.disabled })
        items.push({ name: "resetPassword", label: qsTr("Reset password…"), enabled: !self })
        if (!admin) items.push({ name: "pairingCode", label: qsTr("New pairing code…"), enabled: !u.disabled })
        if (u.disabled) items.push({ name: "turnOn", label: qsTr("Turn on") })
        else items.push({ name: "turnOff", label: qsTr("Turn off…"), enabled: !admin })
        if (!admin) items.push({ name: "remove", label: qsTr("Remove user…"), danger: true, divider: true })
        return items
    }
    function runAction(name: string, u: var) {
        if (!u) return
        switch (name) {
        case "rename": root.page.renameUser(u); break
        case "makeMember": root.page.makeMember(u); break
        case "makeAdmin": root.page.makeAdmin(u); break
        case "resetPassword": root.page.resetPassword(u); break
        case "pairingCode": root.page.newPairingCode(u); break
        case "turnOn": root.page.turnOn(u, false); break
        case "turnOff": root.page.turnOff(u); break
        case "remove": root.page.removeUser(u); break
        }
    }

    Flow {
        objectName: "usersRoleChips"
        Layout.fillWidth: true
        spacing: Theme.space1
        Repeater {
            model: [{ key: "all", label: qsTr("All") }, { key: "admin", label: qsTr("Admins") },
                    { key: "member", label: qsTr("Members") }, { key: "invited", label: qsTr("Open invites") }]
            delegate: UzFilterChip {
                required property var modelData
                objectName: "usersRoleChip_" + modelData.key
                text: modelData.label
                count: root.page.roleCounts[modelData.key]
                active: root.page.roleFilter === modelData.key
                onClicked: root.page.roleFilter = modelData.key
            }
        }
    }

    UzTable {
        id: table
        objectPrefix: "peopleTable"
        Layout.fillWidth: true
        emptyText: root.page.roleFilter === "invited" ? qsTr("No invite is open right now.") : qsTr("Nobody is here yet.")
        columns: [
            { key: "person", title: qsTr("Name"), fill: 1.4, custom: true },
            { key: "role", title: qsTr("Role"), width: 90 },
            { key: "status", title: qsTr("Now"), fill: 1 },
            { key: "played", title: qsTr("Play time"), width: 80, align: "right", mono: true },
            { key: "devices", title: qsTr("Devices"), width: 80, align: "right", mono: true },
            { key: "action", title: "", width: 110, align: "right", custom: true }
        ]
        rows: root.page.shownRows
        currentIndex: root.page.selectedIndex
        onPicked: (i, row) => { if (!row.invite) root.page.selectedId = row.id }
        cellDelegate: Component {
            Item {
                id: cell
                property var row: null
                property var column: null
                readonly property bool isPerson: cell.column !== null && cell.column.key === "person"
                implicitHeight: cell.isPerson ? Math.max(col.implicitHeight, 36) : 32
                // Name column: avatar, name, since / expiry.
                UzAvatar {
                    visible: cell.isPerson
                    anchors.verticalCenter: parent.verticalCenter
                    size: 28
                    name: cell.row ? cell.row.person : ""
                    person: cell.row ? cell.row.order : 1
                    status: cell.row && !cell.row.invite ? (cell.row.online ? "online" : "offline") : ""
                }
                Column {
                    id: col
                    visible: cell.isPerson
                    x: 28 + Theme.space15
                    width: parent.width - x
                    Text {
                        objectName: "personName_" + (cell.row ? cell.row.id : "")
                        width: parent.width
                        elide: Text.ElideRight
                        text: cell.row ? cell.row.person : ""
                        textFormat: Text.PlainText
                        font.family: Theme.sans
                        font.pixelSize: Theme.roles.body.size
                        color: Theme.ink
                    }
                    Text {
                        width: parent.width
                        elide: Text.ElideRight
                        text: cell.row ? cell.row.since : ""
                        textFormat: Text.PlainText
                        font.family: Theme.mono
                        font.pixelSize: Theme.roles.small.size
                        color: Theme.muted
                    }
                }
                // Action column: Withdraw for an open invite (the server never lists the code, so there is nothing to copy), "⋯" for a person.
                UzButton {
                    objectName: "withdrawOpenInvite_" + (cell.row ? cell.row.inviteId : "")
                    visible: !cell.isPerson && cell.row !== null && cell.row.invite === true
                    anchors.right: parent.right
                    anchors.rightMargin: Theme.space2
                    anchors.verticalCenter: parent.verticalCenter
                    size: "sm"
                    kind: "ghost"
                    text: qsTr("Withdraw")
                    onClicked: root.page.withdrawInvite({ id: cell.row.inviteId })
                }
                MoreMenu {
                    objectName: "personMenu_" + (cell.row ? cell.row.id : "")
                    visible: !cell.isPerson && cell.row !== null && cell.row.invite !== true
                    anchors.right: parent.right
                    anchors.rightMargin: Theme.space2
                    anchors.verticalCenter: parent.verticalCenter
                    tooltip: qsTr("More actions for %1").arg(cell.row ? cell.row.person : "")
                    items: cell.row && cell.row.invite !== true ? root.menuFor(root.page.userById(cell.row.id)) : []
                    onActivated: (name) => root.runAction(name, root.page.userById(cell.row.id))
                }
            }
        }

        ColumnLayout {   // details section of the selected person
            objectName: "peopleDetails"
            width: parent ? parent.width : 0
            visible: root.u !== null && root.page.roleFilter !== "invited"
            spacing: Theme.space2
            Item { Layout.preferredHeight: Theme.space2 }
            UzSectionLabel { Layout.fillWidth: true; Layout.leftMargin: Theme.space15; Layout.rightMargin: Theme.space15; text: root.name }
            GridLayout {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.space15
                Layout.rightMargin: Theme.space15
                columns: 2
                columnSpacing: Theme.space3
                rowSpacing: Theme.space1
                Text { text: qsTr("Role"); font.family: Theme.sans; font.pixelSize: Theme.roles.input.size; color: Theme.muted }
                Text { objectName: "detailRole"; text: root.isAdmin ? qsTr("Admin") : qsTr("Member"); font.family: Theme.sans; font.pixelSize: Theme.roles.body.size; color: Theme.ink }
                Text { text: qsTr("Last seen"); font.family: Theme.sans; font.pixelSize: Theme.roles.input.size; color: Theme.muted }
                Text {
                    objectName: "detailLastSeen"
                    text: root.u === null ? "" : root.u.last_seen_at ? root.page.agoText(root.u.last_seen_at) : qsTr("Never")
                    font.family: Theme.sans; font.pixelSize: Theme.roles.body.size; color: Theme.ink
                }
                Text { text: qsTr("Joined"); font.family: Theme.sans; font.pixelSize: Theme.roles.input.size; color: Theme.muted }
                Text {
                    text: root.u === null ? "" : root.page.dateText(root.u.created_at)
                    font.family: Theme.sans; font.pixelSize: Theme.roles.body.size; color: Theme.ink
                }
                Text { text: qsTr("Play time"); font.family: Theme.sans; font.pixelSize: Theme.roles.input.size; color: Theme.muted }
                Text {
                    objectName: "detailPlayTime"
                    text: root.u === null ? "" : root.page.playText(root.u.play_seconds)
                    font.family: Theme.sans; font.pixelSize: Theme.roles.body.size; color: Theme.ink
                }
                Text { text: qsTr("Sessions"); font.family: Theme.sans; font.pixelSize: Theme.roles.input.size; color: Theme.muted }
                Text {
                    objectName: "detailSessions"
                    text: root.u === null ? "" : Number(root.u.sessions ?? 0) > 0 ? String(root.u.sessions) : "–"
                    font.family: Theme.sans; font.pixelSize: Theme.roles.body.size; color: Theme.ink
                }
                Text { text: qsTr("Last played"); font.family: Theme.sans; font.pixelSize: Theme.roles.input.size; color: Theme.muted }
                Text {
                    objectName: "detailLastPlayed"
                    text: root.u === null ? "" : root.u.last_played_at ? root.page.agoText(root.u.last_played_at) : qsTr("Never")
                    font.family: Theme.sans; font.pixelSize: Theme.roles.body.size; color: Theme.ink
                }
                Text { text: qsTr("Devices"); font.family: Theme.sans; font.pixelSize: Theme.roles.input.size; color: Theme.muted }
                Text {
                    objectName: "detailDevices"
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: {
                        if (root.u === null) return ""
                        const names = root.page.devicesOf(root.u)
                        return names.length > 0 ? names.join(", ") : qsTr("None")
                    }
                    textFormat: Text.PlainText
                    font.family: Theme.sans; font.pixelSize: Theme.roles.body.size; color: Theme.ink
                }
            }
            Flow {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.space15
                Layout.rightMargin: Theme.space15
                spacing: Theme.space1
                UzButton {
                    objectName: "personRename"
                    text: qsTr("Rename…")
                    onClicked: root.page.renameUser(root.u)
                }
                UzButton {
                    objectName: "personMakeAdmin"
                    visible: !root.isAdmin
                    text: qsTr("Make admin…")
                    enabled: root.roleReason === ""
                    onClicked: root.page.makeAdmin(root.u)
                }
                UzButton {
                    objectName: "personMakeMember"
                    visible: root.isAdmin
                    text: qsTr("Make member…")
                    enabled: root.roleReason === ""
                    onClicked: root.page.makeMember(root.u)
                }
                UzButton {
                    objectName: "personResetPassword"
                    text: qsTr("Reset password…")
                    enabled: !root.isSelf
                    onClicked: root.page.resetPassword(root.u)
                }
                UzButton {
                    objectName: "personPairingCode"
                    visible: !root.isAdmin
                    text: qsTr("New pairing code…")
                    enabled: root.u !== null && !root.u.disabled
                    onClicked: root.page.newPairingCode(root.u)
                }
                UzButton {
                    objectName: "personTurnOff"
                    visible: root.u !== null && !root.u.disabled
                    text: qsTr("Turn off…")
                    enabled: root.offReason === ""
                    onClicked: root.page.turnOff(root.u)
                }
                UzButton {
                    objectName: "personTurnOn"
                    visible: root.u !== null && root.u.disabled
                    text: qsTr("Turn on")
                    onClicked: root.page.turnOn(root.u, false)
                }
                UzButton {
                    objectName: "personRemove"
                    kind: "danger"
                    visible: !root.isAdmin
                    text: qsTr("Remove…")
                    onClicked: root.page.removeUser(root.u)
                }
            }
            UzStatusIcon {
                objectName: "personHint"
                Layout.leftMargin: Theme.space15
                Layout.rightMargin: Theme.space15
                visible: text !== ""
                kind: "warning"
                small: true
                text: root.offReason !== "" ? root.offReason : root.roleReason
                maxWidth: 560
            }
            Item { Layout.preferredHeight: Theme.space2 }
        }
    }
}
