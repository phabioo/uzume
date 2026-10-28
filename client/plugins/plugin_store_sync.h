#pragma once
// PluginStoreSync: syncs the local PluginStore with the plugin user store of the connected server (plugin_store_v1,
// GET/PUT /plugins/{plugin_id}/store/{key}). Plugins never wait for it: uz.store reads and writes the local copy; this class
// pushes dirty keys (base revision = the server revision the copy is based on), pulls newer server values of the plugins it is
// asked about, and turns a 409 store_conflict into a kept second version (PluginStore::markServerConflict). Offline or without
// the feature it does nothing and tries again later. UI thread.
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QTimer>
#include <functional>
#include <memory>

#include "hubconnection.h"
#include "plugin_store.h"

namespace uzume::plugins {

class PluginStoreSync : public QObject {
  Q_OBJECT
 public:
  PluginStoreSync(std::shared_ptr<PluginStore> store, HubConnection* connection, QObject* parent = nullptr);

  static constexpr const char* kFeature = "plugin_store_v1";
  bool supported() const;  // connected and the server advertises plugin_store_v1

  // Pulls the keys of `pluginId` that are newer on the server, after pushing what is dirty. Safe to call often.
  void pullPlugin(const QString& pluginId);
  // Pushes the dirty keys of all plugins soon (debounced); used after a plugin wrote.
  void pushSoon();
  // Runs one round now (tests): push, then pull the requested plugins. `done` is called when the round is over.
  void syncNow(std::function<void()> done = {});
  bool busy() const { return busy_; }
  void setRetryMs(int ms) { retryMs_ = ms; }
  void setDebounceMs(int ms) { debounce_.setInterval(ms); }

 signals:
  // The server's version of a key replaced the local one (the plugin gets the app event store.changed).
  void keyAdopted(const QString& pluginId, const QString& key);
  // Both versions of a key are kept (the plugin gets the app event store.conflict; uz.store.conflicts(key)).
  void keyConflicted(const QString& pluginId, const QString& key);

 private:
  struct Work;
  void startRound(std::function<void()> done);
  void pushNext(std::shared_ptr<Work> w);
  void pullNext(std::shared_ptr<Work> w);
  void pullKeys(std::shared_ptr<Work> w);
  void finishRound(std::shared_ptr<Work> w, bool ok);
  void request(const QByteArray& method, const QString& path, const QByteArray& body, std::function<void(const HttpResult&)> cb);

  std::shared_ptr<PluginStore> store_;
  QPointer<HubConnection> conn_;
  QSet<QString> pullWanted_;
  QTimer debounce_;
  QTimer retry_;
  int retryMs_ = 30000;
  bool busy_ = false;
  bool again_ = false;
  std::function<void()> done_;
};

}  // namespace uzume::plugins
