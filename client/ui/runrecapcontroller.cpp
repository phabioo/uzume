#include "runrecapcontroller.h"

#include <QClipboard>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>
#include <QtGlobal>

#include "joinlink.h"
#include "plugin_manager.h"
#include "pluginscontroller.h"

namespace uzume::ui {

RecapMappings recapMappingsFor(const RunInfo& run, plugins::PluginManager* manager) {
  RecapMappings out;
  if (manager == nullptr) return out;
  const QList<plugins::PluginInfo> copies = manager->hubCopies();
  for (const RunPluginRef& ref : run.plugins) {
    const plugins::Manifest* found = nullptr;
    std::optional<plugins::PluginInfo> installed;
    for (const plugins::PluginInfo& c : copies)
      if (c.manifest.id == ref.pluginId && c.manifest.version.toString() == ref.version) {
        found = &c.manifest;
        break;
      }
    if (found == nullptr) {
      installed = manager->info(ref.pluginId);
      if (installed) found = &installed->manifest;
    }
    if (found == nullptr) continue;
    QMap<QString, QString> m;
    for (const plugins::RecapEntry& e : found->recap) m.insert(e.type, e.glyph);
    out.insert(ref.pluginId, m);
  }
  return out;
}

RunRecapController::RunRecapController(RunClient* client, PluginsController* plugins, HubConnection* connection, QObject* parent)
    : QObject(parent), client_(client), plugins_(plugins), conn_(connection) {
  refreshTimer_.setSingleShot(true);
  refreshTimer_.setInterval(2000);
  connect(&refreshTimer_, &QTimer::timeout, this, [this] {
    if (state_ == QLatin1String("ready") && !runId_.isEmpty()) load(runId_);
  });
  messageTimer_.setSingleShot(true);
  messageTimer_.setInterval(6000);
  connect(&messageTimer_, &QTimer::timeout, this, [this] { setMessage({}, false); });
  if (client_) {
    connect(client_, &RunClient::eventAdded, this, [this](const QString& id) {
      if (id == runId_ && state_ == QLatin1String("ready")) scheduleRefresh();
    });
  }
  connect(connection, &HubConnection::stateChanged, this, [this] {
    if (conn_ && conn_->state() != HubConnection::State::Connected) clear();
  });
}

void RunRecapController::scheduleRefresh() { refreshTimer_.start(); }

QVariantList RunRecapController::groupEvents(const QVariantList& events, double trackWidth, double glyphWidth) const {
  return groupRecapEvents(events, trackWidth, glyphWidth);
}

// Plain words for the user; the technical text only goes to the log.
QString RunRecapController::plainError(const QString& technical) {
  qWarning().noquote() << "run recap: load failed:" << technical;
  const QString t = technical.toLower();
  for (const char* k : {"refused", "reach", "timed out", "timeout", "host", "network", "did not answer", "closed", "unreachable", "socket"})
    if (t.contains(QLatin1String(k))) return tr("The server cannot be reached right now.");
  return tr("The recap could not be loaded.");
}

QString RunRecapController::serverName() const { return conn_ ? conn_->hubInfo().name : QString(); }

void RunRecapController::compute(const QString& runId, std::function<void(bool, const QString&, const RecapModel&)> done) {
  const auto run = client_ ? client_->run(runId.toLower()) : std::nullopt;
  if (!run) {
    QTimer::singleShot(0, this, [done = std::move(done)] { done(false, QStringLiteral("unknown run"), RecapModel()); });
    return;
  }
  auto* loader = new RunRecapLoader(conn_, this);
  const RunInfo info = *run;
  const RecapMappings maps = recapMappingsFor(info, plugins_ ? plugins_->manager() : nullptr);
  loader->load(info.runId, [loader, info, maps, done = std::move(done)](const RecapFetch& f) {
    if (!f.ok) {
      done(false, f.error, RecapModel());
    } else {
      RecapInput in;
      in.run = info;
      in.events = f.events;
      in.screenshots = f.screenshots;
      in.mappings = maps;
      done(true, QString(), buildRecap(in));
    }
    loader->deleteLater();
  });
}

void RunRecapController::load(const QString& runId) {
  const QString id = runId.toLower();
  if (id.isEmpty()) return;
  const bool same = id == runId_;
  if (same && state_ == QLatin1String("loading")) return;
  if (!same) {
    setMessage({}, false);  // a note about another run does not belong here
    model_ = RecapModel();
    modelVariant_.clear();
  }
  runId_ = id;
  const quint64 gen = ++gen_;
  if (!same || state_ != QLatin1String("ready")) {  // a refresh of what is shown keeps showing it
    model_ = RecapModel();
    modelVariant_.clear();
    state_ = QStringLiteral("loading");
  }
  errorText_.clear();
  staleNotice_.clear();
  emit changed();
  const auto run = client_ ? client_->run(id) : std::nullopt;
  if (!run) {
    if (client_ && !client_->loaded()) {  // opened from a link before the list of runs arrived: wait for it
      connect(
          client_, &RunClient::runsChanged, this,
          [this, id, gen] {
            if (gen != gen_ || runId_ != id) return;
            state_ = QStringLiteral("idle");
            load(id);
          },
          Qt::SingleShotConnection);
      return;
    }
    state_ = QStringLiteral("error");
    errorText_ = tr("This run is not on the server any more.");
    emit changed();
    return;
  }
  delete loader_;
  loader_ = new RunRecapLoader(conn_, this);
  const RunInfo info = *run;
  const RecapMappings maps = recapMappingsFor(info, plugins_ ? plugins_->manager() : nullptr);
  loader_->load(id, [this, gen, info, maps](const RecapFetch& f) {
    if (gen != gen_) return;
    if (!f.ok) {
      const QString plain = plainError(f.error);
      if (state_ == QLatin1String("ready")) {  // keep what was loaded for this run, with a notice
        staleNotice_ = plain;
      } else {
        state_ = QStringLiteral("error");
        errorText_ = plain;
        model_ = RecapModel();
        modelVariant_.clear();
      }
    } else {
      RecapInput in;
      in.run = client_ && client_->run(info.runId) ? *client_->run(info.runId) : info;  // the newest run data (status, play time)
      in.events = f.events;
      in.screenshots = f.screenshots;
      in.mappings = maps;
      model_ = buildRecap(in);
      modelVariant_ = model_.toVariant();
      state_ = QStringLiteral("ready");
    }
    emit changed();
  });
}

void RunRecapController::reload() {
  if (runId_.isEmpty()) return;
  setMessage({}, false);
  const QString id = runId_;
  if (state_ != QLatin1String("ready")) state_ = QStringLiteral("idle");  // a loaded recap stays shown while it refreshes
  load(id);
}

void RunRecapController::clear() {
  ++gen_;
  refreshTimer_.stop();
  delete loader_;
  loader_ = nullptr;
  runId_.clear();
  state_ = QStringLiteral("idle");
  errorText_.clear();
  staleNotice_.clear();
  model_ = RecapModel();
  modelVariant_.clear();
  setMessage({}, false);
  emit changed();
}

void RunRecapController::setMessage(const QString& text, bool error) {
  if (message_ == text && messageIsError_ == error) return;
  message_ = text;
  messageIsError_ = error;
  if (text.isEmpty()) messageTimer_.stop();
  else messageTimer_.start();
  emit messageChanged();
}

void RunRecapController::dismissMessage() { setMessage({}, false); }

QString RunRecapController::localPath(const QString& pathOrUrl) {
  if (pathOrUrl.startsWith(QLatin1String("file:"), Qt::CaseInsensitive)) return QUrl(pathOrUrl).toLocalFile();
  return pathOrUrl;
}

QString RunRecapController::suggestedFileName(const QString& extension) const {
  QString base = modelVariant_.value(QStringLiteral("runName")).toString();
  base.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|\\x00-\\x1f]")), QStringLiteral("_"));
  base = base.trimmed();
  if (base.isEmpty()) base = tr("Run");
  return tr("%1 - recap.%2").arg(base, extension);
}

void RunRecapController::saveImage(const QVariant& image, const QString& pathOrUrl) {
  const QImage img = image.value<QImage>();
  const QString path = localPath(pathOrUrl);
  if (img.isNull() || path.isEmpty()) {
    setMessage(tr("The recap image could not be saved."), true);
    return;
  }
  QSaveFile f(path);
  if (!f.open(QIODevice::WriteOnly) || !img.save(&f, "PNG") || !f.commit()) {
    setMessage(tr("The recap image could not be saved: %1").arg(f.errorString()), true);
    return;
  }
  setMessage(tr("Saved the recap image as %1.").arg(QFileInfo(path).fileName()), false);
}

void RunRecapController::saveHtml(const QString& pathOrUrl) {
  const QString path = localPath(pathOrUrl);
  if (state_ != QLatin1String("ready") || path.isEmpty()) {
    setMessage(tr("The recap is not loaded."), true);
    return;
  }
  QSaveFile f(path);
  if (!f.open(QIODevice::WriteOnly) || f.write(recapHtml(model_, serverName()).toUtf8()) < 0 || !f.commit()) {
    setMessage(tr("The recap file could not be saved: %1").arg(f.errorString()), true);
    return;
  }
  setMessage(tr("Saved the recap as %1.").arg(QFileInfo(path).fileName()), false);
}

void RunRecapController::reportDialogMissing() { setMessage(tr("The file dialog is not available in this installation."), true); }

QString RunRecapController::shareLink() {
  const QString link = conn_ ? makeRunLink(conn_->address(), runId_) : QString();
  if (link.isEmpty()) {
    setMessage(tr("The link could not be made."), true);
    return {};
  }
  if (QClipboard* cb = QGuiApplication::clipboard()) cb->setText(link);
  setMessage(tr("Link copied. It opens this recap for everyone on %1.").arg(serverName().isEmpty() ? tr("this server") : serverName()), false);
  return link;
}

}  // namespace uzume::ui
