#include "pluginimageprovider.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <atomic>

Q_LOGGING_CATEGORY(lcPluginImage, "uzume.plugins.image")

namespace uzume::ui {

void PluginImageSources::set(const QHash<QString, PluginImageSource>& sources) {
  std::lock_guard<std::mutex> l(m_mutex);
  m_sources = sources;
}

bool PluginImageSources::get(const QString& pluginId, PluginImageSource& out) const {
  std::lock_guard<std::mutex> l(m_mutex);
  auto it = m_sources.constFind(pluginId);
  if (it == m_sources.constEnd()) return false;
  out = *it;
  return true;
}

PluginImageProvider::PluginImageProvider(std::shared_ptr<PluginImageSources> sources)
    : QQuickImageProvider(QQuickImageProvider::Image), m_sources(std::move(sources)) {}

namespace {
QString fail(QString* error, const QString& msg) {
  if (error) *error = msg;
  return {};
}

bool insideDir(const QString& canonicalFile, const QString& canonicalDir) {
  const QString base = canonicalDir.endsWith(QLatin1Char('/')) ? canonicalDir : canonicalDir + QLatin1Char('/');
  return canonicalFile.startsWith(base);
}
}  // namespace

QString PluginImageProvider::resolve(const PluginImageSource& src, const QString& id, QString* error) {
  const int a = id.indexOf(QLatin1Char('/'));
  const int b = a < 0 ? -1 : id.indexOf(QLatin1Char('/'), a + 1);
  if (b < 0) return fail(error, QStringLiteral("expected <plugin>/<cache|asset>/<path>"));
  const QString kind = id.mid(a + 1, b - a - 1);
  const QString rel = id.mid(b + 1);
  static const QRegularExpression ok(QStringLiteral("^[A-Za-z0-9._/-]{1,200}$"));
  if (!ok.match(rel).hasMatch() || rel.contains(QLatin1String("..")) || rel.startsWith(QLatin1Char('/')) || rel.contains(QLatin1String("//")))
    return fail(error, QStringLiteral("invalid path"));
  if (!rel.endsWith(QLatin1String(".png"), Qt::CaseInsensitive)) return fail(error, QStringLiteral("only PNG images are supported"));

  QString baseDir;
  if (kind == QLatin1String("cache")) {
    baseDir = src.cacheDir;
  } else if (kind == QLatin1String("asset")) {
    bool declared = false;
    for (const QString& as : src.assets) {
      const QString a2 = QDir::cleanPath(as);
      if (rel == a2 || rel.startsWith(a2 + QLatin1Char('/'))) declared = true;
    }
    if (!declared) return fail(error, QStringLiteral("asset is not declared in the manifest"));
    baseDir = src.pluginDir;
  } else {
    return fail(error, QStringLiteral("unknown image kind"));
  }
  if (baseDir.isEmpty()) return fail(error, QStringLiteral("no such folder"));
  const QFileInfo fi(QDir(baseDir).filePath(rel));
  const QString canonicalBase = QFileInfo(baseDir).canonicalFilePath();
  const QString canonicalFile = fi.canonicalFilePath();  // resolves symlinks; empty when the file does not exist
  if (canonicalBase.isEmpty() || canonicalFile.isEmpty()) return fail(error, QStringLiteral("file not found"));
  if (!insideDir(canonicalFile, canonicalBase)) return fail(error, QStringLiteral("path leaves the plugin folder"));
  if (!QFileInfo(canonicalFile).isFile()) return fail(error, QStringLiteral("not a file"));
  return canonicalFile;
}

bool PluginPixelBudget::take(const QString& pluginId, qint64 pixels, Clock::time_point now) {
  std::lock_guard<std::mutex> lock(m_mutex);
  auto& q = m_spent[pluginId];
  while (!q.empty() && now - q.front().first > std::chrono::milliseconds(kPluginDecodeWindowMs)) q.pop_front();
  qint64 total = pixels;
  for (const auto& e : q) total += e.second;
  if (total > kPluginDecodePixelsPerWindow) return false;
  q.emplace_back(now, pixels);
  return true;
}

QImage PluginImageProvider::loadPng(const QString& path, QString* error, const QSize& requestedSize, PluginPixelBudget* budget,
                                    const QString& pluginId) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) {
    if (error) *error = QStringLiteral("cannot open the file");
    return {};
  }
  if (f.size() > kMaxPluginImageBytes) {
    if (error) *error = QStringLiteral("file is larger than 8 MiB");
    return {};
  }
  QImageReader r(&f, "png");
  const QSize sz = r.size();
  if (!sz.isValid() || sz.width() > kMaxPluginImageDim || sz.height() > kMaxPluginImageDim) {
    if (error) *error = QStringLiteral("image is not a PNG or larger than 4096x4096");
    return {};
  }
  if (budget != nullptr && !budget->take(pluginId, static_cast<qint64>(sz.width()) * sz.height())) {
    if (error) *error = QStringLiteral("too many image pixels decoded for this plugin, try again shortly");
    return {};
  }
  QSize target(kMaxDeliveredImageDim, kMaxDeliveredImageDim);
  if (requestedSize.isValid() && requestedSize.width() > 0 && requestedSize.height() > 0) target = target.boundedTo(requestedSize);
  if (sz.width() > target.width() || sz.height() > target.height()) r.setScaledSize(sz.scaled(target, Qt::KeepAspectRatio));
  const QImage img = r.read();
  if (img.isNull() && error) *error = QStringLiteral("cannot decode the PNG");
  return img;
}

QImage PluginImageProvider::requestImage(const QString& id, QSize* size, const QSize& requestedSize) {
  const int slash = id.indexOf(QLatin1Char('/'));
  PluginImageSource src;
  QString err;
  QImage img;
  if (slash == 0) {
    // Empty plugin id: the view lost its plugin. The QML side no longer asks; log the first one only.
    static std::atomic<bool> loggedEmpty{false};
    if (loggedEmpty.exchange(true)) return {};
    err = QStringLiteral("empty plugin id");
  } else if (slash > 0 && m_sources->get(id.left(slash), src)) {
    const QString path = resolve(src, id, &err);
    if (!path.isEmpty()) img = loadPng(path, &err, requestedSize, &m_budget, id.left(slash));
  } else {
    err = QStringLiteral("unknown plugin");
  }
  if (img.isNull()) qCDebug(lcPluginImage) << "image rejected:" << id << err;
  if (size) *size = img.size();
  return img;
}

}  // namespace uzume::ui
