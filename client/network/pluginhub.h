#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <optional>

#include "hubconnection.h"

namespace uzume {

inline constexpr qint64 kMaxPluginDownloadBytes = 32ll * 1024 * 1024;  // same limit as a .uzplugin file

// One entry of a plugin's `run_settings` (manifest field and Hub plugin list; the Hub enforces the same rules).
struct RunSettingOption {
  QString value;  // ^[a-z][a-z0-9_]{0,31}$
  QString label;  // i18n key
};
struct RunSetting {
  QString key;    // 1 or 2 dot-separated segments, each ^[a-z][a-z0-9_]{0,31}$
  QString type;   // "boolean" | "enum"
  QString label;  // i18n key ^[A-Za-z0-9_.-]{1,64}$
  QString hint;   // optional i18n key
  QVariant def;   // bool (boolean) or QString, one of the option values (enum)
  QList<RunSettingOption> options;  // enum only, 2..8
};
// Validates a `run_settings` array (1..32 entries). Returns an empty string on success, else the specific problem.
QString parseRunSettings(const QJsonValue& v, QList<RunSetting>& out);
// The settings object of one plugin: key segments nested ("rules.dupes_clause" -> {"rules": {"dupes_clause": ...}}).
// `values` maps key -> bool / QString; a missing key falls back to the entry's default.
QJsonObject buildRunSettings(const QList<RunSetting>& settings, const QVariantMap& values);
// Text for an i18n key: `lang` -> "en" -> the key itself. `strings` is {<lang>: {<key>: text}}.
QString runSettingText(const QJsonObject& strings, const QString& lang, const QString& key);

// One offered plugin version (GET /plugins, handshake feature plugins_v1; 0.11 package 3 contract section 4).
struct HubPluginInfo {
  QString id;
  QString version;
  QString name;
  QString description;
  int api = 1;
  QString origin;  // as the Hub classified it: "official" | "unverified" (the Player checks the signature itself)
  QString sha256;  // lowercase hex of the .uzplugin file: the pin of the download
  qint64 size = 0;
  QStringList permissions;
  QJsonObject requires_;
  QStringList provides;
  QJsonArray games;
  QList<RunSetting> runSettings;  // optional; invalid ones from the Hub are dropped (the plugin stays)
  QJsonObject runSettingsStrings;  // {<lang>: {<i18n key>: text}}
  QString createdAt;
};
// nullopt for an entry with an invalid id, version, hash or origin.
std::optional<HubPluginInfo> parseHubPluginInfo(const QJsonObject& o);

struct PluginDownloadResult {
  bool ok = false;
  QString id;
  QString version;
  QString sha256;    // the pin that was asked for
  QByteArray bytes;  // the file, only when ok (hash checked against the pin)
  QString problem;   // not_supported | not_on_hub | invalid_hash | invalid_size | too_large | download_failed
  QString detail;
};

// Plugin list and file download of the active Hub. Verifies the SHA-256 pin of a download; installing (zip checks,
// signature) is the PluginManager's job. Without plugins_v1 (older Hub) nothing is requested.
class PluginHubClient : public QObject {
  Q_OBJECT
 public:
  enum class State { Idle, Loading, Ready, Failed };

  explicit PluginHubClient(HubConnection* connection, QObject* parent = nullptr);

  bool supported() const;  // connected and the Hub advertises plugins_v1
  void refresh();          // GET /plugins
  State state() const { return state_; }
  const QList<HubPluginInfo>& plugins() const { return plugins_; }
  QString errorMessage() const { return error_; }

  // GET /plugins/<id>/<version>/file; the body must hash to `sha256`. expectedSize 0 = unknown.
  void download(const QString& pluginId, const QString& version, const QString& sha256, qint64 expectedSize = 0);

 signals:
  void stateChanged();
  void downloadFinished(const uzume::PluginDownloadResult& result);

 private:
  QPointer<HubConnection> conn_;
  QList<HubPluginInfo> plugins_;
  State state_ = State::Idle;
  QString error_;
  quint64 generation_ = 0;
};

}  // namespace uzume

Q_DECLARE_METATYPE(uzume::PluginDownloadResult)
