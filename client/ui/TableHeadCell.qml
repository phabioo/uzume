import QtQuick
import QtQuick.Layouts

// Table header cell (role table-header: Mono 500 10, +12 %, uppercase, `dim`) of the mapping, hotkeys and storage-style tables.
Text {
    Layout.minimumWidth: 0
    textFormat: Text.PlainText
    elide: Text.ElideRight
    font.family: Theme.mono
    font.pixelSize: Theme.roles.tableHeader.size
    font.weight: Font.Medium
    font.letterSpacing: Theme.roles.tableHeader.spacing
    font.capitalization: Font.AllUppercase
    color: Theme.dim
}
