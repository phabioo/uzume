#pragma once
// image://uzplugin/<pluginId>/cache/<path> and image://uzplugin/<pluginId>/asset/<path>: PNG icons of plugins for the
// plugin blocks. Reads only inside the plugin's cache folder or its declared assets; rejects "..", symlinks that leave
// the folder, files larger than 8 MiB and images larger than 4096x4096. Called from QML image threads: the sources are
// kept in a mutex-protected registry that the PluginsController updates.
#include <QHash>
#include <QImage>
#include <QQuickImageProvider>
#include <QString>
#include <QStringList>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>

namespace uzume::ui {

inline constexpr qint64 kMaxPluginImageBytes = 8ll * 1024 * 1024;
inline constexpr int kMaxPluginImageDim = 4096;  // hard limit of the file (width and height)
// What is handed to QML: larger images are scaled down (aspect kept) to this, smaller requestedSize values win.
inline constexpr int kMaxDeliveredImageDim = 2048;
// Source pixels one plugin may have decoded per window: 4096x4096 images are fine one at a time, hundreds are not.
inline constexpr qint64 kPluginDecodePixelsPerWindow = 128ll * 1024 * 1024;
inline constexpr int kPluginDecodeWindowMs = 10000;

// Sliding-window budget of decoded source pixels per plugin (thread-safe).
class PluginPixelBudget {
 public:
  using Clock = std::chrono::steady_clock;
  bool take(const QString& pluginId, qint64 pixels, Clock::time_point now = Clock::now());

 private:
  std::mutex m_mutex;
  QHash<QString, std::deque<std::pair<Clock::time_point, qint64>>> m_spent;
};

struct PluginImageSource {
  QString cacheDir;     // <plugins>/cache/<id>
  QString pluginDir;    // folder the plugin runs from
  QStringList assets;   // manifest "assets": files, or folders whose content is allowed
};

class PluginImageSources {
 public:
  void set(const QHash<QString, PluginImageSource>& sources);
  bool get(const QString& pluginId, PluginImageSource& out) const;

 private:
  mutable std::mutex m_mutex;
  QHash<QString, PluginImageSource> m_sources;
};

class PluginImageProvider : public QQuickImageProvider {
 public:
  explicit PluginImageProvider(std::shared_ptr<PluginImageSources> sources);
  QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override;

  // `id` = "<pluginId>/<cache|asset>/<path>". Returns the absolute file path, or an empty string and sets `error`.
  static QString resolve(const PluginImageSource& src, const QString& id, QString* error);
  // Loads a PNG with the size and dimension limits; a null image and `error` on failure.
  // `requestedSize` (valid = the size QML shows it at) and kMaxDeliveredImageDim bound the returned image, which is scaled
  // while decoding; `budget` (optional) is charged the source pixels of `pluginId`.
  static QImage loadPng(const QString& path, QString* error, const QSize& requestedSize = QSize(), PluginPixelBudget* budget = nullptr,
                        const QString& pluginId = QString());

 private:
  std::shared_ptr<PluginImageSources> m_sources;
  PluginPixelBudget m_budget;
};

}  // namespace uzume::ui
