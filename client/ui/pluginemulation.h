#pragma once
// PluginEmulationService: the app side of fb.memory.write, fb.cheats and fb.save.snapshot (0.20 package 5, plugin API
// "emulation"). The plugin thread calls the plugins::EmulationBridge methods; they check the thread-safe state, queue the work
// and return at once. Everything else happens here on the GUI thread, in a fixed order that no plugin can skip:
//   run rule (rules.cheats / rules.memory_write)  ->  consent per game (11d)  ->  safety copy "Before cheats" / "Before memory
//   changes"  ->  the change goes through the emulation command queue (one queue for all plugins)  ->  save marker
//   (X-Uzume-Cheats-Used) and run log events (cheat.enabled, cheat.disabled, memory.write).
// Consent is asked once per game and kind and kept in <data>/plugins/consent.json; "Cancel" is not remembered across sessions.
#include <QHash>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QSet>
#include <QVariantMap>
#include <chrono>
#include <functional>
#include <map>
#include <memory>

#include "emulation_runner.h"
#include "plugin_emulation.h"
#include "run_bridge.h"
#include "runtypes.h"

namespace uzume {
class HubConnection;
}

namespace uzume::ui {

class GameSession;

class PluginEmulationService : public QObject, public plugins::EmulationBridge {
  Q_OBJECT
  // The consent question the person has to answer: {kind ("cheats" | "memory_write"), title, label, items [string], meta,
  // confirmLabel}; empty = none. Answered with answerConsent().
  Q_PROPERTY(QVariantMap consentRequest READ consentRequest NOTIFY consentRequestChanged)
  // Number of cheats turned on now (header line "3 cheats on").
  Q_PROPERTY(int cheatsOn READ cheatsOn NOTIFY cheatsOnChanged)
 public:
  enum class Kind { Cheats = 0, MemoryWrite = 1 };
  enum class Consent { Ask, Allowed, Denied };
  enum class SnapshotOutcome { Ok, NoSave, Failed };
  using SnapshotDone = std::function<void(SnapshotOutcome, const QString& message)>;
  // Takes the server-side safety copy with this label (SaveSync::snapshotActive); NoSave = there is nothing on a server to protect.
  using SnapshotFn = std::function<void(const QString& label, SnapshotDone done)>;

  struct GameInfo {
    QString id;      // library game id; empty in tests
    QString title;
    QString system;
    QString sha256;
    QString coreName;
    QStringList cheatFormats;
  };
  struct Notice {
    QString pluginId;    // empty = the app itself
    QString pluginName;
    QString tone;        // done | warning | info
    QString title;
    QString text;
  };

  PluginEmulationService(GameSession* session, const QString& dataDir, QObject* parent = nullptr);
  ~PluginEmulationService() override;

  // --- wiring (GUI thread) ---
  void setSnapshotFn(SnapshotFn fn) { snapshotFn_ = std::move(fn); }
  void setCheatsUsedMarker(std::function<void()> fn) { markCheatsUsed_ = std::move(fn); }
  void setConnection(HubConnection* c);
  void setPluginNameFn(std::function<QString(const QString&)> fn) { nameOf_ = std::move(fn); }
  void setAppEventSink(std::function<void(const QString&, const QVariant&)> fn) { appEvent_ = std::move(fn); }

  // A game runs: its identity, the cheat formats of its core and the run (null = no run, everything allowed).
  void gameStarted(const GameInfo& info, std::shared_ptr<plugins::RunBridge> run);
  void gameStopped();
  bool active() const;

  // --- consent ---
  QVariantMap consentRequest() const { return consentRequest_; }
  Q_INVOKABLE void answerConsent(bool allow);
  QString gameKey() const;
  Consent consent(Kind kind) const;
  // Tests and the debug MCP: pre-answers the question for the running game (`allow` true = remembered; false = refused for this
  // session; Ask = forget). `key` empty = the running game.
  void setConsent(Kind kind, Consent value, const QString& key = QString());
  Consent storedConsent(const QString& key, Kind kind) const;
  static QString kindName(Kind k) { return k == Kind::Cheats ? QStringLiteral("cheats") : QStringLiteral("memory_write"); }

  // --- debug MCP ---
  // Applies a list as plugin `pluginId` ("mcp"); with bypassConsent the per-game question is skipped (the rule, the safety
  // copy, the marker and the run log still apply).
  plugins::CheatApplyResult applyCheatsAs(const QString& pluginId, const QList<plugins::CheatItem>& list, bool bypassConsent);
  QList<plugins::CheatItem> activeCheatList() const;
  QStringList cheatFormatList() const { return cheatFormats(); }

  int cheatsOn() const;

  // --- plugins::EmulationBridge (plugin thread, thread-safe) ---
  QString writeMemory(const QString& pluginId, quint64 addr, const QByteArray& bytes) override;
  QStringList cheatFormats() const override;
  QString coreName() const override;
  QString cheatsBlockedReason() const override;
  plugins::CheatApplyResult applyCheats(const QString& pluginId, const QList<plugins::CheatItem>& list) override;
  QString clearCheats(const QString& pluginId) override;
  plugins::CheatDatabaseResult cheatDatabase() override;
  QString snapshot(const QString& pluginId, const QString& pluginName, const QString& reason) override;

  // Plain helpers (tests).
  static bool validCheatCode(const QString& format, const QString& code);

 signals:
  void consentRequestChanged();
  void cheatsOnChanged();
  void runRulesChanged();  // the run now allows or forbids cheats / memory changes (GUI thread)
  void noticeRaised(const uzume::ui::PluginEmulationService::Notice& notice);
  // Two plugins keep writing the same address (shown as a notice and logged).
  void writeConflict(quint64 addr, const QString& pluginA, const QString& pluginB);

 private:
  struct PendingWrite {
    QString pluginId;
    quint64 addr = 0;
    QByteArray bytes;
  };
  struct Rules {
    bool inRun = false;
    RunRules rules;
  };
  Rules rulesNow() const;                       // any thread
  void onRulesChanged();                        // GUI thread
  void announceRulesAllowed(bool cheats, bool memory);  // GUI thread: info toast "<name> allowed cheats in this run"
  void requestConsent(Kind kind, const QString& pluginId);
public:
  // The person acted in a plugin (a click in its UI, a hotkey). The consent question for cheats that a plugin applied by itself at
  // the start of the game (remembered cheats) waits for this: it never pops up before the person does something (design 11d).
  void noteUserAction();
private:
  void finishConsent(Kind kind, bool allow);
  void saveConsent() const;
  void loadConsent();
  void pumpWrites();
  void pumpCheats();
  void takeSnapshot(Kind kind, const QString& label, std::function<void(bool ok)> then);
  void commitCheats();
  void dropCheatsBecauseOf(const QString& reason, const QString& noticeTitle, const QString& noticeText);
  void markUsed();
  void raise(const Notice& n);
  void postApp(const QString& name, const QVariant& v);
  QString nameOf(const QString& pluginId) const;
  void logRun(const QString& type, const QString& text, const QJsonObject& payload);
  plugins::CheatApplyResult applyInternal(const QString& pluginId, const QList<plugins::CheatItem>& list, bool bypassConsent, bool replay);
  void refreshRegions();
  bool mapped(quint64 addr, quint64 len) const;
  void refreshCheatDb(const QString& gameId);

  GameSession* session_;
  QString dataDir_;
  QPointer<HubConnection> conn_;
  SnapshotFn snapshotFn_;
  std::function<void()> markCheatsUsed_;
  std::function<QString(const QString&)> nameOf_;
  std::function<void(const QString&, const QVariant&)> appEvent_;

  mutable QMutex mu_;
  bool active_ = false;
  GameInfo game_;
  std::shared_ptr<plugins::RunBridge> run_;
  QMetaObject::Connection runConn_;
  Consent consent_[2] = {Consent::Ask, Consent::Ask};
  bool sessionDenied_[2] = {false, false};
  bool snapDone_[2] = {false, false};
  bool snapBusy_[2] = {false, false};
  bool usedMarked_ = false;
  QList<PendingWrite> pendingWrites_;
  QList<plugins::CheatItem> wanted_;                 // the active list as accepted from the plugins
  QString wantedBy_;
  QHash<QString, plugins::CheatItem> enabledPrev_;   // enabled entries of the last accepted list, by id (run log diffs)
  bool applyPending_ = false;                        // an apply waits for the consent answer
  QList<plugins::CheatItem> pendingApply_;
  QString pendingApplyPlugin_;
  bool userActed_ = false;                           // the person used a plugin since this game started (mu_)
  QString queuedConsentPlugin_;                      // a consent question waits for the first action (mu_)
  std::vector<std::pair<quint64, quint64>> regions_;  // merged [start, end) of the memory map
  std::map<QString, std::chrono::steady_clock::time_point> lastMemEvent_;
  QSet<QString> memToldPlugins_;
  struct LastWrite {
    QString plugin;
    std::chrono::steady_clock::time_point at;
    int hits = 0;
    std::chrono::steady_clock::time_point reported;
  };
  QHash<quint64, LastWrite> lastWrite_;
  QVariantMap consentRequest_;
  QStringList consentPlugins_;
  Kind consentKind_ = Kind::Cheats;
  bool consentOpen_ = false;
  QHash<QString, std::chrono::steady_clock::time_point> dbFetched_;
  QSet<QString> dbBusy_;
  bool lastAllowed_ = true;
  bool lastMemAllowed_ = true;
  QTimer rulesTimer_;
  RunRules lastRules_;
  QString cacheHubId_;
  bool online_ = false;
  QHash<QString, QString> dbError_;  // game id -> "not_found" | "error: ..." of the last fetch
};

}  // namespace uzume::ui

Q_DECLARE_METATYPE(uzume::ui::PluginEmulationService::Notice)
