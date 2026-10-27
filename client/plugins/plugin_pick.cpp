#include "plugin_pick.h"

#include <QFile>
#include <QFileInfo>

namespace uzume::plugins {

PickedFile readPickedFile(const QString& path, qint64 maxBytes) {
  PickedFile out;
  const QFileInfo fi(path);
  out.name = fi.fileName();
  if (!fi.isFile()) {
    out.error = QStringLiteral("read_failed");
    return out;
  }
  if (fi.size() > maxBytes) {
    out.error = QStringLiteral("too_large");
    return out;
  }
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) {
    out.error = QStringLiteral("read_failed");
    return out;
  }
  out.content = f.read(maxBytes + 1);  // a file that grew meanwhile still cannot exceed the limit
  if (out.content.size() > maxBytes) {
    out.content.clear();
    out.error = QStringLiteral("too_large");
  }
  return out;
}

}  // namespace uzume::plugins
