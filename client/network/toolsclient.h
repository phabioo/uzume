#pragma once

#include <QFile>
#include <QJsonArray>
#include <QList>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <memory>

#include "hubconnection.h"
#include "toolcache.h"

namespace uzume {

// The server's tools catalog (feature tools_v1, ADR 0031 D3): GET /tools lists tools with their offered versions and, per version, the
// packages per platform (size, SHA-256, cached on the server). Apps download tools only from their server.
struct ToolPlatformInfo {
  QString platform;  // as the server lists it
  qint64 size = 0;
  QString sha256;
  bool cached = false;  // the server holds the file (false: it still has to fetch it)
};
struct ToolVersionInfo {
  QString version;
  QList<ToolPlatformInfo> platforms;
};
struct ToolInfo {
  QString id;  // "randomizer"
  QString name;
  bool offered = false;  // the admin turned on "Offer to apps"
  QString version;       // the version the server offers (newest offered one)
  QList<ToolVersionInfo> versions;
  QString license;
  QString sourceUrl;

  const ToolVersionInfo* versionInfo(const QString& v) const;
};
QList<ToolInfo> parseTools(const QByteArray& body);

enum class ToolProblem { None, NotOnServer, NotOffered, Incompatible, DownloadFailed, InvalidSize, InvalidHash, InstallFailed, Cancelled };
struct ToolPrepareResult {
  QString toolId;
  QString version;
  bool ok = false;
  ToolProblem problem = ToolProblem::None;
  QString detail;  // never file content
};

// Downloads a tool package of the catalog for this platform into the tool cache: GET /tools/{id}/packages/{version}/{platform}
// streamed to a temp file, SHA-256 and size checked against the catalog, then ToolCache::installZip (extract, layout check).
class ToolsClient : public QObject {
  Q_OBJECT
 public:
  ToolsClient(HubConnection* connection, ToolCache* cache, QObject* parent = nullptr);

  void setPlatform(const QString& platform) { platform_ = platform; }  // tests; default CoreCache::currentPlatform()
  QString platform() const { return platform_; }
  bool available() const;  // connected and the server advertises tools_v1
  const QList<ToolInfo>& tools() const { return tools_; }
  const ToolInfo* tool(const QString& id) const;
  bool loaded() const { return loaded_; }
  bool busy() const { return busy_; }

  void refresh();  // GET /tools -> toolsChanged()
  // Offered version of a tool with a package for this platform ("" if none) and that package.
  QString offeredVersion(const QString& toolId, ToolPlatformInfo* platformOut = nullptr) const;
  void prepare(const QString& toolId, const QString& version);  // result via prepared()
  void cancel();

 signals:
  void toolsChanged();
  void refreshFailed(const QString& detail);
  void progress(const QString& toolId, qint64 received, qint64 total);
  void prepared(const uzume::ToolPrepareResult& result);

 private:
  void startDownload(const ToolInfo& tool, const ToolVersionInfo& ver, const ToolPlatformInfo& plat);
  void fail(ToolProblem p, const QString& detail);
  void cleanup();
  const ToolPlatformInfo* platformFor(const ToolVersionInfo& v) const;

  QPointer<HubConnection> conn_;
  ToolCache* cache_;
  QString platform_;
  QList<ToolInfo> tools_;
  bool loaded_ = false;
  bool busy_ = false;
  quint64 generation_ = 0;
  QPointer<QNetworkReply> reply_;
  ToolPrepareResult result_;
  std::unique_ptr<QFile> tmp_;
  QString tmpPath_;
};

}  // namespace uzume

Q_DECLARE_METATYPE(uzume::ToolPrepareResult)
