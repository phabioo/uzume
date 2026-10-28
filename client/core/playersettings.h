#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <optional>

namespace uzume {

// Local Player settings: <data>/settings/player.json (not hub-specific, never contains a secret).
// Unknown keys are preserved when saving.
class PlayerSettings {
 public:
  enum class Appearance { Dark, Light, System };

  // baseDir = data directory (ProfileStore::baseDir()). A missing or corrupted file yields the defaults.
  explicit PlayerSettings(const QString& baseDir);

  static QString appearanceName(Appearance a);  // "dark" | "light" | "system"
  static Appearance parseAppearance(const QString& name, Appearance fallback = Appearance::Dark);

  QString filePath() const { return path_; }
  Appearance appearance() const { return appearance_; }
  bool setAppearance(Appearance a);  // persists immediately; false if the file could not be written
  // Text size: "100" (default) | "125" | "tv"; anything else is rejected. Reduce motion: unset = follow the system.
  QString textSize() const { return textSize_; }
  bool setTextSize(const QString& size);
  std::optional<bool> reduceMotion() const { return reduceMotion_; }
  bool setReduceMotion(bool on);

  // Updates (spec 0.3 S6). Channel override: "" = compiled default, otherwise "stable" | "beta" (an old "test" reads as "beta").
  // Automatic install: unset = default of the effective channel (on for beta).
  QString updateChannel() const { return updateChannel_; }
  bool setUpdateChannel(const QString& channel);  // "", "stable", "beta" ("test" = alias); anything else is rejected (false)
  // Resolved default channel of a beta build ("" = not resolved yet, otherwise "stable" | "beta"); written once by the
  // updater on the first verified index load, never by the user. An explicit updateChannel() wins.
  QString updateChannelDefault() const { return updateChannelDefault_; }
  bool setUpdateChannelDefault(const QString& channel);  // "stable" | "beta"; anything else is rejected (false)
  std::optional<bool> updateAutoInstall() const { return updateAutoInstall_; }
  bool setUpdateAutoInstall(bool on);
  bool clearUpdateAutoInstall();  // back to the default of the channel
  // Update rollback (ADR 0029 D4). History: versions this device ran ([{version, first, last}]). Pause: {from, to, at,
  // prev_auto} while automatic updates are paused after going back (empty object = none). Skip: the version the user
  // went back from, never installed automatically. The shapes are interpreted by core/updaterollback.h.
  QJsonArray updateHistory() const;
  bool setUpdateHistory(const QJsonArray& history);
  QJsonObject updatePause() const;
  bool setUpdatePause(const QJsonObject& pause);  // empty object removes it
  QString updateSkip() const { return updateSkip_; }
  bool setUpdateSkip(const QString& version);  // empty removes it

  // "What's new" (0.18): the app version whose release notes were shown or dismissed last; empty = never (fresh install).
  QString whatsNewVersion() const { return whatsNewVersion_; }
  bool setWhatsNewVersion(const QString& version);  // empty removes it

  // Diagnostics overlay (0.6 D5): open/closed state of the overlay and of its two sections, shared by the window,
  // multiview and fullscreen. Defaults: overlay closed, Emulation section open, Streaming section open.
  bool diagnosticsOpen() const { return diagOpen_; }
  bool diagnosticsEmulationOpen() const { return diagEmulationOpen_; }
  bool diagnosticsStreamingOpen() const { return diagStreamingOpen_; }
  bool setDiagnosticsOpen(bool open);
  bool setDiagnosticsEmulationOpen(bool open);
  bool setDiagnosticsStreamingOpen(bool open);

  // Chosen save slot per Hub profile and game (ADR 0012 D7); "default" when nothing valid is stored.
  QString saveSlot(const QString& hubId, const QString& gameId) const;
  bool setSaveSlot(const QString& hubId, const QString& gameId, const QString& slot);  // false: invalid name or not writable

  // Library (3c-2, per device): sort key and "Ready first". Sort keys: name_asc | name_desc | added_desc | added_asc |
  // size_desc | size_asc | system | played. Unknown values fall back to name_asc.
  static bool isValidLibrarySort(const QString& key);
  QString librarySort() const { return librarySort_; }
  bool setLibrarySort(const QString& key);  // false: unknown key
  bool libraryReadyFirst() const { return libraryReadyFirst_; }
  bool setLibraryReadyFirst(bool on);
  // Save states (0.20): ask before a state replaces the running game (default); "Don't ask again" in the load dialog turns it off.
  bool stateLoadAsk() const { return stateLoadAsk_; }
  bool setStateLoadAsk(bool on);
  // Library view (readable UI pass 5): "list" (default) or "grid", per device.
  static bool isValidLibraryView(const QString& view);
  QString libraryView() const { return libraryView_; }
  bool setLibraryView(const QString& view);  // false: unknown value

  // Last played per Hub profile and game, local only (Unix time in ms; 0 = never played on this device).
  qint64 lastPlayed(const QString& hubId, const QString& gameId) const;
  QHash<QString, qint64> lastPlayedAll(const QString& hubId) const;
  bool setLastPlayed(const QString& hubId, const QString& gameId, qint64 msecs);  // false: empty ids or not writable

  // ROM cache size limit in bytes (default 20 GB, 0 = unlimited); finished ROM files beyond it are evicted least recently
  // used first (core/romcache.h). Negative values are rejected.
  static constexpr qint64 kDefaultRomCacheLimitBytes = 20ll * 1024 * 1024 * 1024;
  qint64 romCacheLimitBytes() const { return romCacheLimit_; }
  bool setRomCacheLimitBytes(qint64 bytes);

  // Variant cache (0.21, core/variantcache.h): size limit in bytes (default 8 GB, 0 = unlimited; least recently played variants are
  // dropped first, their recipes stay) and "remove variants not played for N days" (0 = never, 30, 60 or 90).
  static constexpr qint64 kDefaultVariantCacheLimitBytes = 8ll * 1024 * 1024 * 1024;
  qint64 variantCacheLimitBytes() const { return variantCacheLimit_; }
  bool setVariantCacheLimitBytes(qint64 bytes);  // false: negative
  int variantUnplayedDays() const { return variantUnplayedDays_; }
  bool setVariantUnplayedDays(int days);  // false: not 0, 30, 60 or 90

  // Library folders (0.12): folders scanned for local ROM files, per device. Stored as absolute, cleaned paths without
  // duplicates; relative or empty entries are rejected.
  QStringList libraryFolders() const { return libraryFolders_; }
  bool setLibraryFolders(const QStringList& folders);  // false: invalid entry or not writable (nothing changes)

  // Notifications (0.23, ADR 0033 D2), per this PC: how each kind shows. Kinds: invite | session_started | run_event | update |
  // server_problem. Mode "toast" (default) shows a toast and keeps the entry in the bell, "list" only keeps it in the bell.
  // "Quiet in fullscreen" (default on): in fullscreen only @mentions toast. "Sound for toasts" (default off).
  static bool isNotificationKind(const QString& kind);
  QString notificationMode(const QString& kind) const;  // "toast" | "list"; unknown kinds read as "toast"
  bool setNotificationMode(const QString& kind, const QString& mode);  // false: unknown kind or mode
  bool notifyQuietFullscreen() const;
  bool setNotifyQuietFullscreen(bool on);
  bool notifySound() const;
  bool setNotifySound(bool on);

  // Plugin developer mode (settings -> Plugins): allows unsigned local plugins, shows the Plugin console in game. Default off.
  bool developerMode() const { return developerMode_; }
  bool setDeveloperMode(bool on);
  // Developer console "Reload on file change": reload a dev-linked plugin when its files change. Default on.
  bool pluginReloadOnChange() const { return pluginReloadOnChange_; }
  bool setPluginReloadOnChange(bool on);
  // Discord Rich Presence (off by default; key discord_presence).
  bool discordPresence() const { return discordPresence_; }
  bool setDiscordPresence(bool on);

  // First start (0.18 package 7): the person chose "Play on this PC" (or "Add later") on the welcome screen, so it is not shown again.
  bool playOnThisPcChosen() const { return playOnThisPc_; }
  bool setPlayOnThisPcChosen(bool chosen);

  // "Play on this PC" / "Share with others" (0.18 package 10, ADR 0030 D6). Mode: "" (none) | "child" (uzume-server runs as a child
  // process of the app) | "service" (the Windows service). Port: the loopback port of the server. Data dir: empty = the
  // default <data>/server. Hub id: the server of this PC in the profile list. Never contains a password.
  QString localServerMode() const { return localServerMode_; }
  bool setLocalServerMode(const QString& mode);  // "", "child", "service"; anything else is rejected (false)
  int localServerPort() const { return localServerPort_; }
  bool setLocalServerPort(int port);  // 0 (none) or 1..65535
  QString localServerDataDir() const { return localServerDataDir_; }
  bool setLocalServerDataDir(const QString& dir);
  QString localServerHubId() const { return localServerHubId_; }
  bool setLocalServerHubId(const QString& hubId);
  // The name of the person on this PC (used as the display name of the first admin of the server of this PC); empty = none.
  QString displayName() const { return displayName_; }
  bool setDisplayName(const QString& name);

  // "Share with others" started the installer and the app quit: what the next start finishes (or rolls back). join is
  // "home" | "internet". Empty object = none. Never contains a password.
  struct PendingShare {
    int port = 0;
    QString join;
    QString dataDir;
    bool valid() const { return port > 0 && !dataDir.isEmpty(); }
  };
  PendingShare pendingShare() const;
  bool setPendingShare(const PendingShare& p);
  bool clearPendingShare();

  // Plugin UI language (0.17): "auto" (default; 3DS game language or OS language) or an explicit tag such as "en" / "de".
  // Applies when the next game starts. An invalid tag is rejected (false).
  QString pluginLanguage() const { return pluginLanguage_; }
  bool setPluginLanguage(const QString& tag);

  // Plugin UI (0.11): HUD widgets and notices, per plugin id. Corners: "top-left" | "bottom-left" | "bottom-right".
  // Notice mute modes: "always" | "important" (only important notices pass); an empty mode removes the entry.
  QHash<QString, QString> pluginHudCorners() const;
  bool setPluginHudCorner(const QString& pluginId, const QString& corner);
  QStringList pluginHudHidden() const;
  bool setPluginHudHidden(const QString& pluginId, bool hidden);
  bool pluginHudAllHidden() const { return pluginHudAllHidden_; }
  bool setPluginHudAllHidden(bool hidden);
  // Plugin hotkey choices of the user (Controllers > Hotkeys), keyed by plugin id + hotkey id: a portable key text
  // ("Ctrl+H"), or "none" = unassigned. An empty key removes the entry (back to the key the manifest declares).
  QHash<QString, QString> pluginHotkeyOverrides() const;  // "<pluginId>/<hotkeyId>" -> key | "none"
  bool setPluginHotkeyOverride(const QString& pluginId, const QString& hotkeyId, const QString& key);
  QHash<QString, QString> pluginToastMutes() const;
  bool setPluginToastMute(const QString& pluginId, const QString& mode);

  // Window of the bottom screen (0.19, NDS/3DS "Bottom screen in its own window"): position, size and the name of the
  // monitor it was on, remembered on this PC only (not per game). Invalid = never placed (the window opens centered).
  struct WindowGeometry {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    QString screen;  // QScreen::name() of the monitor; the position is only used while that monitor exists
    bool valid() const { return width > 0 && height > 0; }
  };
  WindowGeometry bottomWindowGeometry() const;
  bool setBottomWindowGeometry(const WindowGeometry& g);  // invalid removes it
  // Audio (0.19 stage 1), per PC. Master volume 0..100 % (default 100) and mute apply to everything the app plays; "mute when
  // in the background" (default on) silences the local game while the window is inactive; volume step 5 or 10 (default 5);
  // "show volume on screen" (default on); output device id (base64 of QAudioDevice::id(); empty = system default).
  static constexpr int kDefaultVolume = 100;
  int audioVolume() const { return audioVolume_; }
  bool setAudioVolume(int percent);  // clamped to 0..100
  bool audioMuted() const { return audioMuted_; }
  bool setAudioMuted(bool muted);
  bool audioMuteInBackground() const { return audioMuteInBackground_; }
  bool setAudioMuteInBackground(bool on);
  int audioVolumeStep() const { return audioVolumeStep_; }
  bool setAudioVolumeStep(int percent);  // 5 or 10; anything else is rejected (false)
  bool audioShowVolume() const { return audioShowVolume_; }
  bool setAudioShowVolume(bool on);
  QString audioOutputDevice() const { return audioOutputDevice_; }
  bool setAudioOutputDevice(const QString& id);  // empty = system default
  // Audio stage 2 (0.23): channel gains of the Sound popover (Voice, Plugin sounds; 0..100 %, default 100), "Mix all tiles" in the
  // Multiview (default off) and the gain of each watched person's tile by user id (0..100 %, default 100).
  int audioVoiceGain() const { return audioVoiceGain_; }
  bool setAudioVoiceGain(int percent);
  int audioPluginGain() const { return audioPluginGain_; }
  bool setAudioPluginGain(int percent);
  bool audioMixAll() const { return audioMixAll_; }
  bool setAudioMixAll(bool on);
  int audioTileGain(const QString& userId) const;
  bool setAudioTileGain(const QString& userId, int percent);

  // Voice chat (0.23 package 7, ADR 0033 D7), per PC. Microphone device id (base64 of QAudioDevice::id(); empty = system default),
  // talk mode "ptt" (default) | "vad" (voice activation), activation threshold in dBFS (-70..-10, default -42), noise suppression
  // and echo cancellation (default on), ducking of game and tiles while someone speaks (default on), automatic join for runs and
  // invite-only Sessions (default on), "mic on" (default on; Shift+M) and the volume of each person by user id (0..100 %, default 100).
  QString voiceMicDevice() const { return voiceMicDevice_; }
  bool setVoiceMicDevice(const QString& id);
  QString voiceTalkMode() const { return voiceTalkMode_; }
  bool setVoiceTalkMode(const QString& mode);  // "ptt" | "vad"; anything else is rejected
  int voiceThresholdDb() const { return voiceThresholdDb_; }
  bool setVoiceThresholdDb(int db);
  bool voiceNoiseSuppression() const { return voiceNoiseSuppression_; }
  bool setVoiceNoiseSuppression(bool on);
  bool voiceDucking() const { return voiceDucking_; }
  bool setVoiceDucking(bool on);
  bool voiceAutoJoin() const { return voiceAutoJoin_; }
  bool setVoiceAutoJoin(bool on);
  bool voiceMicOn() const { return voiceMicOn_; }
  bool setVoiceMicOn(bool on);
  int voicePersonGain(const QString& userId) const;
  bool setVoicePersonGain(const QString& userId, int percent);

  // Claude debugging (ADR 0032, Settings > Diagnostics): the local MCP server of the app. Off by default; the port is
  // 1024..65535 (default 47615). The token is not a setting: it lives in the credential store.
  static constexpr int kDefaultMcpPort = 47615;
  bool mcpEnabled() const { return mcpEnabled_; }
  bool setMcpEnabled(bool on);
  int mcpPort() const { return mcpPort_; }
  bool setMcpPort(int port);  // outside 1024..65535: rejected (false)

  // Visibility of the own shared Session: "private" | "hub_users" | "invite_only"; "" = not chosen yet (the caller's default).
  QString sessionVisibility() const { return sessionVisibility_; }
  bool setSessionVisibility(const QString& visibility);  // anything else is rejected (false)
  // Quality a new watched Session starts with on this PC (ADR 0033 D5): "auto" | "high" | "medium" | "low".
  QString viewerQuality() const { return viewerQuality_; }
  bool setViewerQuality(const QString& pick);  // anything else is rejected (false)

  // "Send errors to the server automatically" (ADR 0035 D1), per app: unset = not asked yet (treated as off), true/false = answered.
  std::optional<bool> sendErrors() const;
  bool setSendErrors(bool on);  // the answer; false: not writable

 private:
  bool save() const;

  QString path_;
  QJsonObject raw_;
  Appearance appearance_ = Appearance::Dark;
  QString updateChannel_;
  QString updateChannelDefault_;
  std::optional<bool> updateAutoInstall_;
  QString updateSkip_;
  QString whatsNewVersion_;
  bool diagOpen_ = false;
  bool diagEmulationOpen_ = true;
  bool diagStreamingOpen_ = true;
  QString librarySort_ = QStringLiteral("played");  // recently played; Name order while nothing was played
  bool libraryReadyFirst_ = false;
  bool stateLoadAsk_ = true;
  QString libraryView_ = QStringLiteral("list");
  QString sessionVisibility_;
  QString viewerQuality_ = QStringLiteral("auto");
  QStringList libraryFolders_;
  bool developerMode_ = false;
  bool pluginReloadOnChange_ = true;
  bool discordPresence_ = false;
  bool playOnThisPc_ = false;
  QString localServerMode_;
  int localServerPort_ = 0;
  QString localServerDataDir_;
  QString localServerHubId_;
  QString displayName_;
  QString pluginLanguage_ = QStringLiteral("auto");
  bool pluginHudAllHidden_ = false;
  qint64 romCacheLimit_ = kDefaultRomCacheLimitBytes;
  qint64 variantCacheLimit_ = kDefaultVariantCacheLimitBytes;
  int variantUnplayedDays_ = 0;
  int audioVolume_ = kDefaultVolume;
  bool audioMuted_ = false;
  QString textSize_ = QStringLiteral("100");
  std::optional<bool> reduceMotion_;
  bool audioMuteInBackground_ = true;
  int audioVolumeStep_ = 5;
  bool audioShowVolume_ = true;
  QString audioOutputDevice_;
  int audioVoiceGain_ = 100;
  int audioPluginGain_ = 100;
  bool audioMixAll_ = false;
  QHash<QString, int> audioTileGains_;
  QString voiceMicDevice_;
  QString voiceTalkMode_ = QStringLiteral("ptt");
  int voiceThresholdDb_ = -42;
  bool voiceNoiseSuppression_ = true;
  bool voiceDucking_ = true;
  bool voiceAutoJoin_ = true;
  bool voiceMicOn_ = true;
  QHash<QString, int> voicePersonGains_;
  bool mcpEnabled_ = false;
  int mcpPort_ = kDefaultMcpPort;
};

}  // namespace uzume
