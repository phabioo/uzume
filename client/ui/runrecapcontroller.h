#pragma once
// RunRecapController: the Recap tab of a run (Uzume 0.22 package 2, design board 4jh-app-2). It loads the complete event log and the
// screenshots of the opened run (RunRecapLoader), applies the `recap` mappings of the plugins of the run that are on this PC
// (hub copy of the pinned version first, else the installed one), and presents the RecapModel to QML. Export as image (PNG of the
// grabbed page) and as HTML (one self-contained file), "Share link" (copies uzume://run/... and shows a note).
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>
#include <functional>

#include "hubconnection.h"
#include "runclient.h"
#include "runrecap.h"

namespace uzume::plugins {
class PluginManager;
}

namespace uzume::ui {

class PluginsController;

// The recap mappings of the plugins of `run`: for every plugin the manifest of its pinned hub copy if it is on this PC, else the
// installed one; plugins that are not on this PC have no entry (their events are not shown).
RecapMappings recapMappingsFor(const RunInfo& run, plugins::PluginManager* manager);

class RunRecapController : public QObject {
  Q_OBJECT
  QML_ELEMENT
  QML_UNCREATABLE("Provided by the RunsController")

  Q_PROPERTY(QString runId READ runId NOTIFY changed)
  Q_PROPERTY(QString state READ state NOTIFY changed)          // idle | loading | ready | error
  Q_PROPERTY(QString errorText READ errorText NOTIFY changed)
  Q_PROPERTY(QVariantMap model READ model NOTIFY changed)      // RecapModel::toVariant() (empty map until ready)
  Q_PROPERTY(bool recapEmpty READ recapEmpty NOTIFY changed)   // ready, but nothing to draw
  Q_PROPERTY(QString message READ message NOTIFY messageChanged)
  Q_PROPERTY(QString staleNotice READ staleNotice NOTIFY changed)  // a reload failed, the last loaded recap stays shown
  Q_PROPERTY(bool messageIsError READ messageIsError NOTIFY messageChanged)

 public:
  RunRecapController(RunClient* client, PluginsController* plugins, HubConnection* connection, QObject* parent = nullptr);

  QString runId() const { return runId_; }
  QString state() const { return state_; }
  QString errorText() const { return errorText_; }
  QVariantMap model() const { return modelVariant_; }
  bool recapEmpty() const { return state_ == QLatin1String("ready") && model_.isEmpty(); }
  QString staleNotice() const { return staleNotice_; }
  QString message() const { return message_; }
  bool messageIsError() const { return messageIsError_; }

  // (Re)loads the recap of `runId` and shows it; a second call for the same run while loading is ignored.
  Q_INVOKABLE void load(const QString& runId);
  Q_INVOKABLE void reload();
  Q_INVOKABLE void clear();
  Q_INVOKABLE void dismissMessage();
  // PNG of a grabbed page (`image` = the QImage of Item.grabToImage's result) written to a path or file:// URL.
  Q_INVOKABLE void saveImage(const QVariant& image, const QString& pathOrUrl);
  Q_INVOKABLE void saveHtml(const QString& pathOrUrl);
  // Copies the run's app link to the clipboard and shows the note; returns the link ("" when the server address is unusable).
  Q_INVOKABLE QString shareLink();
  Q_INVOKABLE void reportDialogMissing();  // the native file dialog (QtQuick.Dialogs) is not installed
  // Glyphs of one lane grouped by what overlaps on a track of that width (see groupRecapEvents).
  Q_INVOKABLE QVariantList groupEvents(const QVariantList& events, double trackWidth, double glyphWidth) const;
  Q_INVOKABLE QString serverName() const;
  Q_INVOKABLE QString suggestedFileName(const QString& extension) const;

  // Computes the recap of any run of the active server without touching the displayed one (debug MCP tool run_recap).
  // done(ok, error, model) runs once on the event loop.
  void compute(const QString& runId, std::function<void(bool ok, const QString& error, const RecapModel& model)> done);

 signals:
  void changed();
  void messageChanged();

 private:
  void setMessage(const QString& text, bool error);
  void scheduleRefresh();
  static QString plainError(const QString& technical);
  static QString localPath(const QString& pathOrUrl);

  QPointer<RunClient> client_;
  PluginsController* plugins_;
  QPointer<HubConnection> conn_;
  QString runId_;
  QString state_ = QStringLiteral("idle");
  QString errorText_;
  RecapModel model_;
  QVariantMap modelVariant_;
  QString staleNotice_;
  QString message_;
  bool messageIsError_ = false;
  quint64 gen_ = 0;
  RunRecapLoader* loader_ = nullptr;  // of the displayed recap
  QTimer refreshTimer_;
  QTimer messageTimer_;  // auto-dismisses a status message
};

}  // namespace uzume::ui
