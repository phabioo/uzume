#pragma once
// StatesController: save states, rewind and run-ahead of the running game (0.20 package 4; play-and-plugins.md 3c-2, 3g, 3g-4,
// 3g-5). A state is the core's serialization of the game at one moment, kept per person, server, game, save slot and number
// (core/statestore.h): A = Auto (the progress kept before a state is loaded) and 1 to 4. It is never the game's save: a state
// changes nothing in the save files, and loading one never writes to them.
//
// Order of everything the person can do: run rule (rules.states / rules.rewind of the run the game was started in)  ->  the core
// supports states  ->  the emulation command queue (EmulationRunner::submit: applied between two frames)  ->  the local cache
// (StateStore)  ->  the server (StateApi, `states_v1`; a failed upload stays pending and is retried). Without a server, or without
// `states_v1`, states live on this PC only. Everything here runs on the GUI thread; results of the emulation thread are queued over.
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>
#include <functional>
#include <memory>
#include <optional>

#include "playersettings.h"
#include "runtypes.h"
#include "stateapi.h"
#include "statestore.h"

namespace uzume::ui {

class GameSession;

class StatesController : public QObject {
  Q_OBJECT
  QML_ELEMENT
  QML_UNCREATABLE("Provided by the PlayerController")
  // The game the list is about (the running game, else the one open on the game page) can save states at all (its core can).
  Q_PROPERTY(bool supported READ supported NOTIFY changed)
  // The list belongs to the running game: Save and Load work. Otherwise it is only shown (the game page) and can delete.
  Q_PROPERTY(bool playing READ playing NOTIFY changed)
  // [{n, label ("A", "1" ... "4"), empty, whenText, whenFull, coreText, tag ("" | "Older core" | "Newer core" | "Other core version" |
  //   "Other core"), canLoad, loadLabel ("Load" | "Try to load"), onServer, pending, sizeText, device, previewUrl, current}]; always
  // five rows, Auto first.
  Q_PROPERTY(QVariantList slotRows READ slotRows NOTIFY changed)
  Q_PROPERTY(int slot READ slot WRITE setSlot NOTIFY changed)  // the slot Save and Load work on (1 to 4); Shift+F6 moves it on
  Q_PROPERTY(QString summary READ summary NOTIFY changed)      // "3 OF 4 · ON HOME SERVER"
  // Why saving and loading are off right now (a run rule, ...), or "". The text is what the toast says.
  Q_PROPERTY(QString blockedText READ blockedText NOTIFY changed)
  Q_PROPERTY(bool busy READ busy NOTIFY changed)
  Q_PROPERTY(QVariantMap loadRequest READ loadRequest NOTIFY loadRequestChanged)  // {n, label, whenText, title}; empty = none
  Q_PROPERTY(bool askBeforeLoad READ askBeforeLoad WRITE setAskBeforeLoad NOTIFY askBeforeLoadChanged)
  // The Toast of the last action: {text, kind (done | warning | error), action ("Undo" | ""), token}; empty = none.
  Q_PROPERTY(QVariantMap toast READ toast NOTIFY toastChanged)
  // "Rewinding · 18 s left" while the rewind key is held, else "".
  Q_PROPERTY(QString rewindText READ rewindText NOTIFY rewindTextChanged)
  Q_PROPERTY(bool rewindAvailable READ rewindAvailable NOTIFY changed)
 public:
  struct GameContext {
    QString gameId;
    QString title;
    QString systemId;
    QString saveSlot = QStringLiteral("default");  // the game's save slot (Main, a run's slot ...): states are kept per save slot
    QString coreId;
    QString coreVersion;
    bool states = false;    // core profile `states.supported`
    bool runAhead = false;  // core profile `states.run_ahead`
    QString runId;          // run the game was started in, empty = none
  };
  struct Result {
    bool ok = false;
    QString error;    // "no_game" | "unsupported" | "rule_states" | "rule_rewind" | "empty" | "other_core" | "unavailable" | "failed" | ...
    QString message;  // the toast text
    int n = 0;
  };
  using Done = std::function<void(const Result&)>;
  // The run rules of the run the running game belongs to; nullopt = no run (everything allowed).
  using RulesFn = std::function<std::optional<RunRules>()>;
  // A game page view: fills the context of a game that does not run (save slot, core, support).
  using ViewResolver = std::function<GameContext(const QString& gameId)>;

  static constexpr int kUndoMs = 6000;
  static constexpr int kToastMs = 3000;

  StatesController(GameSession* session, PlayerSettings* settings, QObject* parent = nullptr);
  ~StatesController() override;

  // --- wiring (GUI thread) ---
  void setLocation(const QString& statesDir);  // StateStore::userStatesDir of the signed-in person; empty = none
  void setApi(StateApi* api);
  void setRulesFn(RulesFn fn) { rulesFn_ = std::move(fn); }
  void setRuleOwnerFn(std::function<QString()> fn) { ruleOwnerFn_ = std::move(fn); }  // name of the person who set the rules
  void setRunEventFn(std::function<void(const QString& type, const QString& text, const QJsonObject& payload)> fn) { runEventFn_ = std::move(fn); }
  void setViewResolver(ViewResolver fn) { viewResolver_ = std::move(fn); }
  void setServerName(const QString& name) { serverName_ = name; }
  void setDeviceName(const QString& name) { deviceName_ = name; }

  // A game runs / ended. gameStarted applies the rewind and run-ahead settings (see applyOptions).
  void gameStarted(const GameContext& ctx);
  void gameStopped();
  bool gameRunning() const { return running_; }
  const GameContext& context() const { return ctx_; }
  // Rewind length in seconds (0 = off) and run-ahead frames (0 to 2) the person chose; the controller applies them to the running
  // game when the core and the run rules allow it.
  void applyOptions(int rewindSeconds, int runAhead);
  // The rules of the run changed (run_updated): re-evaluates rewind, the blocked text and the open load request.
  void rulesChanged();
  // The connection to the server changed: refresh and retry what is pending.
  void connectionChanged();

  // --- the list ---
  Q_INVOKABLE void view(const QString& gameId);  // game page: the states of a game that may not run; the running game wins
  Q_INVOKABLE void refresh();
  bool supported() const { return ctx().states; }
  bool playing() const { return running_ && ctx().gameId == viewGame_; }
  QVariantList slotRows() const { return slots_; }
  int slot() const { return slot_; }
  void setSlot(int n);
  Q_INVOKABLE int nextSlot();  // Shift+F6: moves to the next state slot and says so
  QString summary() const { return summary_; }
  QString blockedText() const;
  bool busy() const { return busy_ > 0; }
  QVariantMap loadRequest() const { return loadRequest_; }
  bool askBeforeLoad() const;
  void setAskBeforeLoad(bool on);
  QVariantMap toast() const { return toast_; }
  QVariantList toastLog() const { return toastLog_; }  // the last 20 toasts, oldest first (debug MCP toasts_list)
  QString rewindText() const { return rewindText_; }
  bool rewindAvailable() const;
  QList<StateRecord> records() const;  // the viewed game's states (not the ones waiting to be deleted), by number

  // --- actions (QML, hotkeys, MCP) ---
  Q_INVOKABLE void save(int n);          // "Save to N"; 0 = the Auto slot (internal)
  Q_INVOKABLE void saveToSlot() { save(slot_); }  // F6
  // "Load N", F8: asks first unless the person turned that off. `resumeAfter`: this very load continues a paused game
  // (the quick menu is the pause itself); the intent travels with the request and is gone when it is cancelled or answered.
  Q_INVOKABLE void requestLoad(int n, bool resumeAfter = false);
  Q_INVOKABLE void confirmLoad();        // the dialog's Load
  Q_INVOKABLE void cancelLoad();
  Q_INVOKABLE void loadSlot(bool resumeAfter = false) { requestLoad(slot_, resumeAfter); }  // F8
  Q_INVOKABLE void undoLoad();           // the toast's Undo: back to the Auto state taken before the load
  Q_INVOKABLE void remove(int n);        // the menu's Delete
  Q_INVOKABLE void dismissToast();
  Q_INVOKABLE void rewindKey(bool down); // hold
  // Same with a result for the debug MCP; `force` loads without the dialog.
  void save(int n, Done done);
  void load(int n, bool force, Done done, bool resumeAfter = false);
  void remove(int n, Done done);
  // Rewind by hand (MCP): steps back up to `snapshots` kept states at once (each is kRewindIntervalFrames frames), also while the
  // game is paused. Result.n = how many it stepped back; error "empty" when the buffer holds nothing yet.
  void rewindBy(int snapshots, Done done);

  static QString whyBlocked(const QString& errorCode);
  // Typical size of one state of a system, for the memory estimate of the rewind length (0 = unknown).
  static qint64 typicalStateBytes(const QString& systemId);
  static QString versionTag(const QString& recordVersion, const QString& currentVersion);  // "" | "Older core" | "Newer core" | "Other core version"
  static bool rewindAllowedFor(const QString& systemId) { return systemId != QLatin1String("3ds"); }

 signals:
  void changed();
  void loadRequestChanged();
  void askBeforeLoadChanged();
  void toastChanged();
  void rewindTextChanged();
  void stateLoaded(int n, bool resumeAfter);   // a state was loaded into the running game (MCP and tests); resumeAfter as requested for that load
  void stateSaved(int n);

 private:
  struct Rules {
    bool inRun = false;
    RunRules rules;
  };
  const GameContext& ctx() const { return running_ && (viewGame_.isEmpty() || viewGame_ == ctx_.gameId) ? ctx_ : view_; }
  Rules rules() const;
  QString gate(bool rewind) const;  // error code, "" = allowed
  StateStore* store() { return store_.get(); }
  const StateStore* store() const { return store_.get(); }
  bool serverStates() const { return api_ && api_->available(); }
  void rebuild();
  void syncWithServer();
  void applyServerList(const StateApiResult& r, const QString& gameId, const QString& saveSlot);
  void uploadRecord(const StateRecord& rec);
  void syncPending();
  void deleteOnServer(const StateRecord& rec);
  void ensureBlob(const StateRecord& rec, std::function<void(const QByteArray&, const QString& err)> done);
  void doSave(int n, bool silent, Done done);
  void doLoad(int n, bool isUndo, Done done, bool resumeAfter = false);
  void finishSave(int n, const QByteArray& state, const QImage& preview, bool silent, Done done);
  void showToast(const QString& text, const QString& kind = QStringLiteral("done"), const QString& action = QString(), int ms = kToastMs);
  void applyRewind();
  void postRunEvent(const StateRecord& rec);
  void finish(const Done& done, Result r, const QString& toastKind = QString());
  static QByteArray previewPng(const QImage& frame);

  QPointer<GameSession> session_;
  PlayerSettings* settings_;
  std::unique_ptr<StateStore> store_;
  QPointer<StateApi> api_;
  RulesFn rulesFn_;
  std::function<QString()> ruleOwnerFn_;
  std::function<void(const QString&, const QString&, const QJsonObject&)> runEventFn_;
  ViewResolver viewResolver_;
  QString serverName_;
  QString deviceName_;

  bool running_ = false;
  GameContext ctx_;   // the running game
  GameContext view_;  // the game page's game when nothing runs there
  QString viewGame_;  // game id the list is about
  int slot_ = 1;
  int wantedRewindSeconds_ = 0;
  int wantedRunAhead_ = 0;
  int busy_ = 0;
  QVariantList slots_;
  QString summary_;
  QVariantMap loadRequest_;
  QVariantMap toast_;
  QVariantList toastLog_;
  quint64 toastToken_ = 0;
  QTimer toastTimer_;
  QString rewindText_;
  bool rewindHeld_ = false;
  bool lastRewindAllowed_ = true;
  qint64 lastQuotaBytes_ = 0;
  qint64 lastUsedBytes_ = 0;
  QSet<QString> uploading_;  // "<game>/<slot>/<n>"
  QSet<QString> listing_;    // "<game>/<slot>" with a server list in flight
};

}  // namespace uzume::ui
