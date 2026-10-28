#pragma once
// SessionController: Sessions for the QML UI (ADR 0006 D6, ADR 0012 D8). Owns the Hub WSS client (HubSocket), the
// session REST client, the SessionHost (own shared Session) and one SessionViewer per watched remote Session
// (up to kMaxSurfaces surfaces in total, the local game counts as one) and exposes plain properties/actions to
// QML. A surface is identified by "local" or by the Session id of a remote Session. Hub logic stays in network/,
// media in media/.

#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <vector>
#include <QtQml/qqmlregistration.h>
#include <memory>

#include "audiomixer.h"
#include "audiooutput.h"
#include "diagnosticsmodel.h"
#include "gamesession.h"
#include "gpuencodebridge.h"
#include "hubconnection.h"
#include "hubsocket.h"
#include "mediastats.h"
#include "onlineroster.h"
#include "playersettings.h"
#include "profilestore.h"
#include "sessionapi.h"
#include "sessionhost.h"
#include "sessionviewer.h"

namespace uzume::ui {

// Role of this app in a Session (ADR 0033 D5/D8): the host emulates and shares; a viewer watches, chats and talks but never sends
// input or host commands; a player is a viewer with a granted controller (the hand-over package; no code path yet).
enum class SessionRole { Host, Viewer, Player };
inline QString sessionRoleToken(SessionRole r) {
  return r == SessionRole::Host ? QStringLiteral("host") : (r == SessionRole::Player ? QStringLiteral("player") : QStringLiteral("viewer"));
}

class SessionController : public QObject {
  Q_OBJECT
  QML_ELEMENT
  QML_UNCREATABLE("Provided by the PlayerController")

  // Hub link: "online" | "connecting" | "offline" (WSS); a running media path does not depend on it.
  Q_PROPERTY(QString hubLink READ hubLink NOTIFY linkChanged)
  Q_PROPERTY(bool available READ available NOTIFY linkChanged)  // Hub advertises sessions_v1
  // 3c: Sessions of others [{sessionId, title, meta, invited, visibility}]
  Q_PROPERTY(QVariantList sessions READ sessions NOTIFY sessionsChanged)
  // 3g: own Session
  Q_PROPERTY(bool shared READ shared NOTIFY shareChanged)
  Q_PROPERTY(bool shareBusy READ shareBusy NOTIFY shareChanged)
  Q_PROPERTY(QString visibility READ visibility NOTIFY shareChanged)
  Q_PROPERTY(QString ownName READ ownName NOTIFY shareChanged)  // your display name in the own Session ("Paused by <name>"); empty if not shared
  Q_PROPERTY(int viewerCount READ viewerCount NOTIFY shareChanged)
  Q_PROPERTY(QVariantList participants READ participants NOTIFY shareChanged)
  Q_PROPERTY(QString participantsTitle READ participantsTitle NOTIFY shareChanged)
  Q_PROPERTY(QVariantList userResults READ userResults NOTIFY userResultsChanged)
  Q_PROPERTY(QString userSearchHint READ userSearchHint NOTIFY userResultsChanged)  // empty-state text of the invite search
  Q_PROPERTY(QString message READ message NOTIFY messageChanged)
  Q_PROPERTY(bool messageIsError READ messageIsError NOTIFY messageChanged)
  // Watching
  Q_PROPERTY(bool watching READ watching NOTIFY watchChanged)
  Q_PROPERTY(bool joining READ joining NOTIFY watchChanged)
  Q_PROPERTY(QString watchedWho READ watchedWho NOTIFY watchChanged)  // first remote surface
  Q_PROPERTY(QString watchedGame READ watchedGame NOTIFY watchChanged)
  // Multiview surfaces (ADR 0012 D8): ids ("local" | Session id) in creation order (changes only when the set
  // changes), display order (index 0 is the main surface), info {id: {kind, name, meta}}.
  Q_PROPERTY(QStringList surfaceIds READ surfaceIds NOTIFY surfacesChanged)
  Q_PROPERTY(QStringList surfaceOrder READ surfaceOrder NOTIFY surfacesChanged)
  Q_PROPERTY(QVariantMap surfaceInfo READ surfaceInfo NOTIFY surfacesChanged)
  Q_PROPERTY(int surfaceCount READ surfaceCount NOTIFY surfacesChanged)
  Q_PROPERTY(int maxSurfaces READ maxSurfaces CONSTANT)
  Q_PROPERTY(bool canAddSurface READ canAddSurface NOTIFY watchChanged)  // fewer than four surfaces and no join running
  Q_PROPERTY(QStringList shownSessionIds READ shownSessionIds NOTIFY surfacesChanged)  // remote Sessions on a surface
  // Connection pill per remote surface {sessionId: {text, tone}} (D9): "Direct" | "Relayed (TURN)" | "Connecting" ...
  Q_PROPERTY(QVariantMap surfaceLinks READ surfaceLinks NOTIFY surfaceLinksChanged)
  // "pip" | "row" | "grid" for two or more surfaces (none below); "grid" is offered from four (gridAvailable), a smaller
  // count shows it disabled ("Grid 2×2 · from 4"), ADR 0026 D2.
  Q_PROPERTY(QStringList availableLayouts READ availableLayouts NOTIFY surfacesChanged)
  Q_PROPERTY(bool gridAvailable READ gridAvailable NOTIFY surfacesChanged)
  Q_PROPERTY(QString mainSurface READ mainSurface NOTIFY surfacesChanged)
  Q_PROPERTY(bool hasLocalGame READ hasLocalGame NOTIFY gameChanged)
  Q_PROPERTY(QString localTitle READ localTitle NOTIFY gameChanged)
  // Views: tab "session" | "multiview" (diagnostics is an overlay toggle, not a view); layout "pip" | "row" | "grid" (Row for two or three surfaces, Grid 2 x 2 from four; availableLayouts is empty below two surfaces)
  Q_PROPERTY(QString tab READ tab WRITE setTab NOTIFY viewChanged)
  Q_PROPERTY(QString multiviewMode READ multiviewMode WRITE setMultiviewMode NOTIFY viewChanged)
  Q_PROPERTY(QString audioFocus READ audioFocus NOTIFY viewChanged)  // effective surface id: "local" | Session id
  // 0.23 mixer: "Mix all tiles" (off = only audioFocus is audible), the surfaces that are mixed in (the AUDIO tag), and the
  // watched people for the Mixer popover: [{sid, userId, who, person, gain 0..100}].
  Q_PROPERTY(bool mixAllTiles READ mixAllTiles WRITE setMixAllTiles NOTIFY audioMixChanged)
  Q_PROPERTY(QStringList audibleSurfaces READ audibleSurfaces NOTIFY audioMixChanged)
  Q_PROPERTY(QVariantList mixerTiles READ mixerTiles NOTIFY audioMixChanged)
  // Multiview: the selected tile (surface id) the side panel follows; your game's tile unless another one was chosen.
  // Selecting a remote tile does not route input: input always goes to your game.
  Q_PROPERTY(QString selectedSurface READ selectedSurface NOTIFY selectionChanged)
  // In-game screen layout (0.6 D12): "stacked" | "side" | "top"; applies to the running game and the remote pictures, reset
  // to "stacked" when a game starts. Which layouts exist comes from the system (GameSession::screenLayouts).
  // Picture options of the running game (0.19, Emulation > Picture): effective game > system > global value.
  Q_PROPERTY(bool integerScale READ integerScale NOTIFY viewChanged)
  Q_PROPERTY(QString aspectRatio READ aspectRatio NOTIFY viewChanged)
  Q_PROPERTY(QString screenLayout READ screenLayout WRITE setScreenLayout NOTIFY viewChanged)
  // 0.19 package 6: "Swap screens" (bottom screen where the top would be) and "Bottom screen in its own window" (local game
  // only; while on, the main view shows the top screen only). Like the layout they start from the Emulation options
  // (uzume.screen_swap, uzume.bottom_window) at game start; the in-game choice applies to the running game only.
  // viewLayout / viewSwap = what the main game view draws right now. watchLayouts = what a watched two-screen frame offers.
  Q_PROPERTY(bool screenSwap READ screenSwap WRITE setScreenSwap NOTIFY viewChanged)
  Q_PROPERTY(bool bottomWindow READ bottomWindow WRITE setBottomWindow NOTIFY viewChanged)
  Q_PROPERTY(QString viewLayout READ viewLayout NOTIFY viewChanged)
  Q_PROPERTY(bool viewSwap READ viewSwap NOTIFY viewChanged)
  Q_PROPERTY(QStringList watchLayouts READ watchLayouts CONSTANT)
  // Diagnostics overlay (3t-3y): states, formatted values.
  Q_PROPERTY(uzume::ui::DiagnosticsModel* diagnostics READ diagnostics CONSTANT)
  // 3g: "Lena is relayed via the hub (TURN) · may lag slightly." (empty when nobody is relayed)
  Q_PROPERTY(QString relayHint READ relayHint NOTIFY shareChanged)
  // 3g-12: "Sending 2 streams · 3.1 Mbit/s" (empty without a running encoder); the host's People tab shows it under the list.
  Q_PROPERTY(QString sendingLine READ sendingLine NOTIFY shareChanged)
  // Quality (ADR 0033 D5) a new watched Session starts with on this PC: "auto" | "high" | "medium" | "low".
  Q_PROPERTY(QString defaultQuality READ defaultQuality NOTIFY qualityDefaultChanged)

 public:
  SessionController(HubConnection* conn, ProfileStore* profiles, GameSession* game, QObject* parent = nullptr);
  ~SessionController() override;

  QString hubLink() const;
  bool available() const;
  QVariantList sessions() const;
  // Online roster (ADR 0033 D1): people who are online on the server, from the presence updates.
  OnlineRoster* roster() { return &roster_; }
  QString localRunId() const { return hasLocalGame() ? runId_ : QString(); }  // run of the running game, "" = not a run game
  // "Invite" in the online list: my Session is shared and this person is not invited to it yet.
  bool canInvite(const QString& userId) const;
  bool shared() const { return shared_; }
  bool shareBusy() const { return shareBusy_; }
  QString visibility() const { return visibility_; }
  QString ownName() const { return shared_ ? own_.owner.displayName : QString(); }
  // A shared pause (TogetherController) holds the host's game: viewers read "Paused by <name>"; empty = the host's own name.
  void setHostPauseBy(const QString& name) {
    if (hostPauseBy_ == name) return;
    hostPauseBy_ = name;
    updateHostState();
  }
  QString ownSessionId() const { return shared_ ? own_.sessionId : QString(); }  // id of the shared Session, "" = nothing shared
  int viewerCount() const;
  QVariantList participants() const;
  QString participantsTitle() const;
  QVariantList userResults() const { return userResults_; }
  QString userSearchHint() const { return userHint_; }
  QString message() const { return message_; }
  bool messageIsError() const { return messageIsError_; }
  static constexpr int kMaxSurfaces = 4;
  bool watching() const { return !remotes_.empty(); }
  bool joining() const { return joining_; }
  QString watchedWho() const { return remotes_.empty() ? QString() : remotes_.front()->info.owner.displayName; }
  QString watchedGame() const { return remotes_.empty() ? QString() : remotes_.front()->info.gameTitle; }
  QStringList surfaceIds() const;
  QStringList surfaceOrder() const { return order_; }
  QVariantMap surfaceInfo() const;
  QVariantMap surfaceLinks() const { return surfaceLinks_; }
  int surfaceCount() const { return static_cast<int>(order_.size()); }
  int maxSurfaces() const { return kMaxSurfaces; }
  bool canAddSurface() const { return !joining_ && surfaceCount() < kMaxSurfaces; }
  QStringList shownSessionIds() const;
  QStringList availableLayouts() const;
  bool gridAvailable() const { return surfaceCount() >= kMaxSurfaces; }
  QString mainSurface() const { return order_.isEmpty() ? QString() : order_.first(); }
  bool hasLocalGame() const { return !gameId_.isEmpty(); }
  QString localTitle() const { return gameTitle_; }
  QString tab() const;
  void setTab(const QString& tab);
  QString multiviewMode() const;
  void setMultiviewMode(const QString& mode);
  QString audioFocus() const;
  QString selectedSurface() const;
  // Newest decoded frame / frame counter of a remote surface (empty / 0 if the Session is not shown).
  QImage remoteFrame(const QString& sessionId) const;
  quint64 remoteFrameNumber(const QString& sessionId) const;
  // Frames of silence/audio handed to the audio output on behalf of a surface (tests: muted surfaces stay at 0).
  qint64 audioFedFrames(const QString& surface) const { return fedFrames_.value(surface); }
  QString screenLayout() const { return screenLayout_; }
  void setScreenLayout(const QString& layout);
  bool screenSwap() const { return screenSwap_; }
  void setScreenSwap(bool on);
  bool bottomWindow() const { return bottomWindow_; }
  void setBottomWindow(bool on);
  QString viewLayout() const { return bottomWindow_ ? QStringLiteral("top") : screenLayout_; }
  bool viewSwap() const { return screenSwap_ && !bottomWindow_; }
  QStringList watchLayouts() const;
  // The user's choices in game (menu, hotkeys): like the setters, plus a notice for the toast ("Layout: Side by side").
  Q_INVOKABLE void chooseScreenLayout(const QString& layout);
  Q_INVOKABLE void chooseScreenSwap(bool on);
  Q_INVOKABLE void chooseBottomWindow(bool on);
  Q_INVOKABLE QString layoutLabel(const QString& layout) const;
  Q_INVOKABLE QString nextLayout(const QStringList& layouts) const;  // the layout after the current one (wraps around)
  bool integerScale() const { return integerScale_; }
  QString aspectRatio() const { return aspectRatio_; }
  void setPictureOptions(bool integerScale, const QString& aspectRatio);
  DiagnosticsModel* diagnostics() const { return diagnostics_; }
  QString relayHint() const { return relayHint_; }
  // The Player's settings object (owned by the PlayerController): persists the open/closed states of the overlay.
  void setPlayerSettings(PlayerSettings* settings);  // diagnostics states + session visibility (migrates the legacy player-settings.json)

  // Local game context (PlayerController): presence, share feed, audio focus.
  // `runId`: the game was started as a run game (presence carries it, Uzume 0.19).
  void gameStarted(const QString& gameId, const QString& title, const QString& runId = {});
  void gameEnded();  // ends a shared Session too

  // Tests: Hub objects and fake statistics (no media path needed).
  HubSocket* socket() { return &socket_; }
  SessionApi* api() { return &api_; }
  SessionHost* host() { return &host_; }
  SessionViewer* viewer(const QString& sessionId);  // nullptr if the Session is not shown
  // Fake statistics: local (nullptr keeps the real ones) and remote per Session id (missing ids keep the real ones).
  void setStatsOverride(const SessionStats* local, const QHash<QString, SessionStats>& remotes);
  // Tests/screenshots: acts as if this Session were shared (no Hub request, no game needed), with the viewers the info lists.
  void adoptSharedSessionForTest(const SessionInfo& info) {
    shared_ = true;
    applyOwnSession(info);
    updateLinks();
  }
  // Fake host-side viewer links (tests/screenshots); an empty list with `on` = true means "no viewer links".
  void refreshDiagnostics();  // also runs every 500 ms while the overlay is open; tests and screenshots call it directly
  void setLinksOverride(bool on, const QList<ViewerLinkStats>& links);

  // People of a Session for the People tab and the chat header: [{userId, name, role: "HOST" | "VIEWER", me, color, connection, tone}].
  // My own Session: me as host and the viewers; a watched Session: its owner and me (the other viewers are not known to a viewer).
  // Each entry also has roleKey ("host" | "viewer"), and on my own row of a watched Session qualityPick ("auto" | "high" | "medium" | "low")
  // and sessionId for the Quality segmented.
  Q_INVOKABLE QVariantList sessionPeople(const QString& sessionId) const;
  // visibility token (private | hub_users | invite_only) of my shared Session or of a Session I watch; empty for others
  QString sessionVisibility(const QString& sessionId) const;
  // My role in a Session: "host" for my shared Session, "viewer" for a watched one, "" otherwise.
  Q_INVOKABLE QString role(const QString& sessionId) const;
  // Quality of a watched Session: the pick token, "" if the Session is not watched. setQuality sends it to the host and keeps it as the
  // default of this PC for new Sessions.
  Q_INVOKABLE QString quality(const QString& sessionId) const;
  Q_INVOKABLE void setQuality(const QString& sessionId, const QString& pick);
  QString defaultQuality() const;
  QString sendingLine() const { return sendingLine_; }
  // The rows of the own Session's viewers for tools and tests: [{viewer_id, name, role, rtt_ms, tier, pick, kbps, connection}].
  QVariantList viewerStates() const;
  Q_INVOKABLE void refreshSessions();
  Q_INVOKABLE void shareSession();
  Q_INVOKABLE void stopSharing();
  Q_INVOKABLE void setVisibility(const QString& visibility);
  Q_INVOKABLE void searchUsers(const QString& text);
  Q_INVOKABLE void invite(const QString& userId);
  Q_INVOKABLE void withdrawInvite(const QString& userId);
  Q_INVOKABLE void removeViewer(const QString& viewerId);
  Q_INVOKABLE void watch(const QString& sessionId);  // "Watch Session", "Join" and "Add": adds a surface (max four)
  Q_INVOKABLE void decline(const QString& sessionId);
  Q_INVOKABLE void leaveWatch();                        // leaves every remote Session
  Q_INVOKABLE void removeSurface(const QString& sessionId);  // "Remove": leaves that Session only
  Q_INVOKABLE void makeMain(const QString& surface);    // "Swap": the surface takes the main position
  // Master volume (AudioController) for watched sessions: software gain 0..1; the output device applies on the next start.
  AudioMixer* audioMixer() const { return mixer_; }
  bool mixAllTiles() const { return mixer_->mixAll(); }
  void setMixAllTiles(bool on);
  QStringList audibleSurfaces() const;
  QVariantList mixerTiles() const;
  Q_INVOKABLE void setTileGain(const QString& surface, int percent);  // per person (user id), remembered on this PC
  Q_INVOKABLE void audioHere(const QString& surface);   // "local" | Session id
  Q_INVOKABLE void selectSurface(const QString& surface);  // "local" | Session id; the side panel follows
  Q_INVOKABLE void dismissMessage();
  // "Watch" from the online roster: the Session id comes from a presence update, so the Session list may not know it yet (it is then
  // refreshed first).
  Q_INVOKABLE void watchFromRoster(const QString& sessionId);

 signals:
  // A notification for the bell (kind, text, action label, action data), see NotificationCenter. Only live events: the first list after a
  // (re)connect does not produce any.
  void notice(const QString& kind, const QString& text, const QString& actionLabel, const QString& actionData);
  void layoutNotice(const QString& text);  // toast text after a layout, swap or bottom-window choice of the user
  void linkChanged();
  void qualityDefaultChanged();
  void sessionsChanged();
  void shareChanged();
  void userResultsChanged();
  void messageChanged();
  void watchChanged();
  void gameChanged();
  void viewChanged();
  void audioMixChanged();
  void surfacesChanged();
  void remoteFrameChanged(const QString& sessionId);
  void surfaceLinksChanged();
  void selectionChanged();

 private:
  void onConnectionState(HubConnection::State s);
  void onHelloAck();
  void onSessionUpdated(const SessionInfo& s);
  void onSessionEnded(const SessionEnded& e);
  void onViewerJoined(const ViewerJoined& v);
  void onViewerLeft(const ViewerLeft& v);
  void onSignal(const SessionSignal& s);
  void applyOwnSession(const SessionInfo& s);
  void applyOwnSession(const SessionInfo& s, quint64 requestVisGen);
  quint64 requestVisToken() const;
  void closeShare(const QString& note);
  struct Remote;
  Remote* remote(const QString& sessionId) const;
  void closeRemote(const QString& sessionId, const QString& note, bool callHub);
  void cancelJoin();
  void say(const QString& text, bool error);
  void presence();
  void pumpAudio();
  void applyAudioRouting();
  void updateLinks();
  QList<ViewerLinkStats> viewerLinks() const;
  PlayerSettings* settings_ = nullptr;  // owned by the PlayerController
  void saveSettings() const;
  void updateGpuEncode();
  void updateHostState();
  QString hostPauseBy_;  // name shown to viewers instead of the host's while a shared pause holds the game  // tells the viewers of the own Session whether the game is paused
  static QString visibilityLabel(const QString& v);
  QString effectiveTab() const;
  QString errorText(const SessionApiResult& r, const QString& what) const;

  HubConnection* conn_;
  ProfileStore* profiles_;
  GameSession* game_;
  SessionApi api_;
  HubSocket socket_;
  SessionHost host_;
  AudioMixer ownMixer_;         // fallback when the GameSession has no shared mixer (tests)
  AudioMixer* mixer_ = nullptr;

  QHash<QString, SessionInfo> sessions_;  // visible Sessions (not own)
  OnlineRoster roster_;
  bool sessionsLoaded_ = false;  // the Session list of this connection arrived: later new Sessions are news
  QString pendingWatch_;         // watchFromRoster waits for the refreshed list
  int visInFlight_ = 0;          // visibility PATCHes not answered yet (a session_update meanwhile must not revert the choice)
  quint64 visGen_ = 0;           // bumped by every local visibility change (stale REST answers keep the newer one)
  quint64 sessionEventGen_ = 0;  // bumped by every live session event; stale GET /sessions results are dropped
  QTimer minuteTimer_;
  QTimer diagTimer_;  // 500 ms: link pills of the side panel, and the diagnostics values while the overlay is open
  QTimer audioTimer_;
  QTimer messageTimer_;
  QElapsedTimer audioClock_;
  qint64 audioDueFrames_ = 0;

  QString gameId_;
  QString runId_;  // run of the local game ("" = not a run game)
  QString gameTitle_;

  bool shared_ = false;
  std::shared_ptr<GpuEncodeBridge> gpu_;  // GPU-direct encoding of the own Session (ADR 0019); null = readback frames
  bool shareBusy_ = false;
  quint64 shareGen_ = 0;  // bumped by closeShare; a publish answer from an older generation is discarded
  QString visibility_ = QStringLiteral("hub_users");
  SessionInfo own_;
  QList<ViewerJoined> pendingViewers_;
  QList<SessionSignal> pendingHostSignals_;
  QList<HubUser> users_;
  QElapsedTimer usersAge_;
  QString userQuery_;
  QVariantList userResults_;
  QString userHint_;
  bool usersLoaded_ = false;

  // One watched remote Session = one viewer join = one SessionViewer.
  struct Remote {
    SessionInfo info;
    QString viewerId;
    SessionViewer* viewer = nullptr;  // owned (parent: the controller), deleted with deleteLater
    QImage frame;
    quint64 frameNr = 0;
    bool paused = false;  // the host paused its game (fb-diag state signal, ADR 0026 D3)
    QString pausedBy;     // name from the signal; empty = the Session owner
    qint64 pausedAtMs = 0;
    AudioOutput audio;  // this tile's input of the mixer (48 kHz stereo from the viewer)
  };
  std::vector<std::unique_ptr<Remote>> remotes_;  // creation order
  QStringList order_;                             // display order of the surfaces; first = main
  bool joining_ = false;
  SessionInfo joinInfo_;
  QList<SessionSignal> earlySignals_;
  QHash<QString, qint64> fedFrames_;

  QString tab_ = QStringLiteral("session");
  QString mode_ = QStringLiteral("pip");  // chosen layout: "pip" | "row" | "grid"
  bool rowForced_ = false;                // "Row" picked on purpose with four surfaces (else four surfaces show the grid)
  QString focusPref_ = QStringLiteral("local");
  QString selected_ = QStringLiteral("local");

  QString message_;
  bool messageIsError_ = false;
  QString screenLayout_ = QStringLiteral("stacked");
  bool screenSwap_ = false;
  bool bottomWindow_ = false;
  bool integerScale_ = false;
  QString aspectRatio_ = QStringLiteral("core");
  DiagnosticsModel* diagnostics_ = nullptr;  // owned (parent: this)
  QString relayHint_;
  QString sendingLine_;
  QVariantMap linkByViewer_;  // viewerId -> {text, tone}
  QVariantMap surfaceLinks_;  // remote Session id -> {text, tone}
  bool linksOverrideOn_ = false;
  QList<ViewerLinkStats> overrideLinks_;
  bool override_ = false;
  bool overrideLocalSet_ = false;
  SessionStats overrideLocal_;
  QHash<QString, SessionStats> overrideRemotes_;
};

}  // namespace uzume::ui
