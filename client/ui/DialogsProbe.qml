import QtQuick
import QtQuick.Dialogs
import Qt.labs.folderlistmodel

// Probe for the file and folder dialogs of QtQuick.Dialogs. Without a native (portal) dialog Qt falls back to its own QML dialog, which
// needs Qt.labs.folderlistmodel, and opening that without the module crashes the app. Loading this file through a Loader only
// succeeds when both modules are installed (linux: qml6-module-qtquick-dialogs, qml6-module-qt-labs-folderlistmodel); the callers
// then use the path field instead of the dialog. The imports are real ones so windeployqt --qmldir sees them.
Item {}
