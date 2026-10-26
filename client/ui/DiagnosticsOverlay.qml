import QtQuick
import Uzume.App

// Diagnostics block of the stage (board 3g-6): F3 opens the UzStageDiagnostics block in the top right corner of the stage,
// fed from the DiagnosticsModel: the summary (core, renderer, resolution, FPS, frame time, audio, session) and a "Details" section
// (open by default, remembered): encoder / decoder, bitrate, resolution and a table per person (path Direct / Relay, RTT, loss and
// the reason for a relay in a sentence); without a Session there is no Details section. Positioned by the game screen. "Copy" puts the whole text on the clipboard, including
// the encoder / decoder and link lines and the fallback hint that do not fit the block. In the Multiview only your tile is
// emulated on this device, so the emulation rows are the same.
UzStageDiagnostics {
    id: root
    objectName: "diagnosticsOverlay"
    required property DiagnosticsModel model
    property bool multi: false
    readonly property var emu: model.emulation
    readonly property var groups: model.streaming

    // [[label, value], [label, value, "warn"]]
    readonly property var modelRows: {
        const r = []
        const e = root.emu
        if (e.valid === true) {
            r.push([qsTr("Core"), e.core || ""])
            r.push([qsTr("Renderer"), (e.renderer || "") + (e.fallback === true ? " · " + qsTr("fallback") : ""), e.fallback === true ? "warn" : undefined])
            r.push([qsTr("Resolution"), (e.resolution || "") + (e.resolutionSub ? " · " + e.resolutionSub : "")])
            r.push([qsTr("FPS"), e.fps || ""])
            r.push([qsTr("Frame"), e.frame || ""])
            r.push([qsTr("Audio"), (e.audio || "") + (e.audioSub ? " · " + e.audioSub : "")])
        } else {
            r.push([qsTr("Game"), qsTr("No game running")])
        }
        if (root.multi) {   // remote tiles are not emulated here: one row each says where the emulation runs
            const ts = root.model.tiles || []
            for (let t = 0; t < ts.length; ++t)
                if (ts[t].local !== true && ts[t].note) r.push([qsTr("Tile %1").arg(ts[t].index), ts[t].note])
        }
        r.push([qsTr("Session"), root.sessionSummary])
        return r
    }
    rows: modelRows

    // Summary of the Session: how many watch your game, or whose game you watch.
    readonly property string sessionSummary: {
        let watching = 0, hosts = []
        for (const g of root.groups) {
            for (const p of (g.participants || [])) {
                if (p.role === "viewer") ++watching
                else if (p.role === "host" && p.name !== qsTr("You")) hosts.push(p.name)
            }
        }
        if (root.groups.length === 0) return qsTr("No session")
        if (watching > 0) return watching === 1 ? qsTr("1 watching") : qsTr("%1 watching").arg(watching)
        if (hosts.length > 0) return qsTr("Watching %1").arg(hosts.join(", "))
        return qsTr("Shared · nobody watching")
    }

    // Details: encoder / decoder, bitrate, resolution and one table row per person with a link (the "You" row has none).
    detailRows: {
        const out = []
        for (const g of root.groups) for (const d of (g.details || [])) out.push(d)
        return out
    }
    people: {
        const out = []
        for (const g of root.groups) {
            for (const p of (g.participants || [])) {
                if ((p.path || "") === "" && (p.rtt || "") === "" && (p.loss || "") === "") continue
                out.push({ name: p.name, path: p.path || "", rtt: p.rtt || "", loss: p.loss || "", reason: p.reason || "" })
            }
        }
        return out
    }
    blockWidth: 340
    detailsOpen: model.streamingOpen
    onDetailsToggled: (open) => model.streamingOpen = open

    // The full text for "Copy"
    function fullText() {
        const lines = [root.title]
        for (const r of root.modelRows) lines.push(r[0] + ": " + r[1])
        if (root.emu.valid === true && root.emu.fallback === true && root.emu.fallbackText) lines.push(root.emu.fallbackText)
        for (const g of root.groups) {
            for (const p of (g.participants || [])) {
                lines.push(p.name + " (" + p.role + ") · " + p.pill)
                if (p.line2) lines.push("  " + p.line2)
                if (p.line3) lines.push("  " + p.line3)
            }
        }
        return lines.join("\n")
    }
    TextEdit { id: clip; visible: false }
    onCopyRequested: {
        clip.text = root.fullText()
        clip.selectAll()
        clip.copy()
        clip.text = ""
    }
}
