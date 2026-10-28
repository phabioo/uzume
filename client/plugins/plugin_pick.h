#pragma once
// uz.ui.pick_file: reads the file the person chose in the file dialog. The plugin gets the content and the base name, never the
// path. Files larger than the plugin's max_bytes are refused without reading them.
#include <QByteArray>
#include <QString>

namespace uzume::plugins {

struct PickedFile {
  QByteArray content;
  QString name;   // base name only
  QString error;  // "" = ok; "too_large" | "read_failed"
};

PickedFile readPickedFile(const QString& path, qint64 maxBytes);

}  // namespace uzume::plugins
