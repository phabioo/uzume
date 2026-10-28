#include "playersettings.h"

#include "fsutil.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStringList>
#include <algorithm>

#include "savestore.h"

namespace uzume {

namespace {
// "auto" or a language tag like "en" / "de-DE" (letters and digits in 2-8 character parts).
bool isValidPluginLanguage(const QString& tag) {
  static const QRegularExpression re(QStringLiteral("^(auto|[a-z]{2,3}(-[A-Za-z0-9]{2,8})*)$"));
  return re.match(tag).hasMatch();
}

constexpr const char* kAppearanceKey = "appearance";
constexpr const char* kUpdateChannelKey = "update_channel";
constexpr const char* kUpdateChannelDefaultKey = "update_channel_default";
constexpr const char* kUpdateAutoKey = "update_auto_install";
constexpr const char* kUpdateHistoryKey = "update_history";
constexpr const char* kUpdatePausedKey = "update_paused";
constexpr const char* kUpdateSkipKey = "update_skip";
constexpr const char* kWhatsNewKey = "whats_new_version";
constexpr const char* kDiagOpenKey = "diagnostics_open";
constexpr const char* kDeveloperModeKey = "developer_mode";
constexpr const char* kPluginReloadOnChangeKey = "plugin_reload_on_change";
constexpr const char* kDiscordPresenceKey = "discord_presence";
constexpr const char* kPlayOnThisPcKey = "play_on_this_pc";
constexpr const char* kLocalServerModeKey = "local_server_mode";
constexpr const char* kLocalServerPortKey = "local_server_port";
constexpr const char* kLocalServerDataDirKey = "local_server_data_dir";
constexpr const char* kLocalServerHubIdKey = "local_server_hub_id";
constexpr const char* kPendingShareKey = "pending_share";
constexpr const char* kDisplayNameKey = "display_name";
constexpr const char* kPluginLanguageKey = "plugin_language";
constexpr const char* kDiagEmulationKey = "diagnostics_emulation_open";
constexpr const char* kDiagStreamingKey = "diagnostics_streaming_open";
constexpr const char* kNotifyKey = "notifications";  // { modes: {kind: toast|list}, quiet_fullscreen, sound }
constexpr const char* kPluginHudCornersKey = "plugin_hud_corners";  // { plugin_id: corner }
constexpr const char* kPluginHudHiddenKey = "plugin_hud_hidden";    // [plugin_id]
constexpr const char* kPluginHudAllHiddenKey = "plugin_hud_all_hidden";
constexpr const char* kPluginHotkeysKey = "plugin_hotkeys";          // { "<plugin_id>/<hotkey_id>": key | "none" }
constexpr const char* kPluginToastMuteKey = "plugin_toast_mute";    // { plugin_id: mode }
constexpr const char* kSessionVisibilityKey = "session_visibility";
constexpr const char* kViewerQualityKey = "viewer_quality";
constexpr const char* kBottomWindowKey = "bottom_window";
constexpr const char* kLibrarySortKey = "library_sort";
constexpr const char* kLibraryReadyFirstKey = "library_ready_first";
constexpr const char* kStateLoadAskKey = "state_load_ask";
constexpr const char* kLibraryViewKey = "library_view";
constexpr const char* kRomCacheLimitKey = "rom_cache_limit_bytes";
constexpr const char* kVariantCacheLimitKey = "variant_cache_limit_bytes";
constexpr const char* kVariantUnplayedDaysKey = "variant_unplayed_days";
constexpr const char* kLibraryFoldersKey = "library_folders";
constexpr const char* kLastPlayedKey = "last_played";  // { hub_id: { game_id: epoch ms } }
constexpr const char* kAudioVolumeKey = "audio_volume";
constexpr const char* kTextSizeKey = "text_size";
constexpr const char* kReduceMotionKey = "reduce_motion";
constexpr const char* kAudioMutedKey = "audio_muted";
constexpr const char* kAudioMuteBgKey = "audio_mute_in_background";
constexpr const char* kAudioStepKey = "audio_volume_step";
constexpr const char* kAudioShowVolumeKey = "audio_show_volume";
constexpr const char* kMcpEnabledKey = "mcp_enabled";
constexpr const char* kMcpPortKey = "mcp_port";
constexpr const char* kAudioDeviceKey = "audio_output_device";
constexpr const char* kAudioVoiceGainKey = "audio_voice_gain";
constexpr const char* kAudioPluginGainKey = "audio_plugin_gain";
constexpr const char* kAudioMixAllKey = "audio_mix_all_tiles";
constexpr const char* kAudioTileGainsKey = "audio_tile_gains";
constexpr const char* kSaveSlotsKey = "save_slots";  // { hub_id: { game_id: slot } }
}

PlayerSettings::PlayerSettings(const QString& baseDir)
    : path_(QDir(baseDir).filePath(QStringLiteral("settings/player.json"))) {
  raw_ = fsutil::readJsonObject(path_);  // a corrupt file is moved aside before anything can overwrite it
  appearance_ = parseAppearance(raw_.value(QLatin1String(kAppearanceKey)).toString());
  const QString ts = raw_.value(QLatin1String(kTextSizeKey)).toString();
  if (ts == QLatin1String("125") || ts == QLatin1String("tv")) {
    textSize_ = ts;
  }
  if (raw_.value(QLatin1String(kReduceMotionKey)).isBool()) {
    reduceMotion_ = raw_.value(QLatin1String(kReduceMotionKey)).toBool();
  }
  const QString ch = raw_.value(QLatin1String(kUpdateChannelKey)).toString();
  if (ch == QLatin1String("stable") || ch == QLatin1String("beta") || ch == QLatin1String("off")) {
    updateChannel_ = ch;
  } else if (ch == QLatin1String("test")) {
    updateChannel_ = QStringLiteral("beta");  // old name; rewritten as "beta" on the next save
    raw_.insert(QLatin1String(kUpdateChannelKey), updateChannel_);
  }
  const QString chDef = raw_.value(QLatin1String(kUpdateChannelDefaultKey)).toString();
  if (chDef == QLatin1String("stable") || chDef == QLatin1String("beta")) updateChannelDefault_ = chDef;
  if (raw_.value(QLatin1String(kUpdateAutoKey)).isBool()) {
    updateAutoInstall_ = raw_.value(QLatin1String(kUpdateAutoKey)).toBool();
  }
  updateSkip_ = raw_.value(QLatin1String(kUpdateSkipKey)).toString();
  whatsNewVersion_ = raw_.value(QLatin1String(kWhatsNewKey)).toString();
  diagOpen_ = raw_.value(QLatin1String(kDiagOpenKey)).toBool(false);
  developerMode_ = raw_.value(QLatin1String(kDeveloperModeKey)).toBool(false);
  pluginReloadOnChange_ = raw_.value(QLatin1String(kPluginReloadOnChangeKey)).toBool(true);
  discordPresence_ = raw_.value(QLatin1String(kDiscordPresenceKey)).toBool(false);
  playOnThisPc_ = raw_.value(QLatin1String(kPlayOnThisPcKey)).toBool(false);
  if (const QString m = raw_.value(QLatin1String(kLocalServerModeKey)).toString(); m == QLatin1String("child") || m == QLatin1String("service")) {
    localServerMode_ = m;
  }
  if (const int port = raw_.value(QLatin1String(kLocalServerPortKey)).toInt(0); port > 0 && port <= 65535) localServerPort_ = port;
  localServerDataDir_ = raw_.value(QLatin1String(kLocalServerDataDirKey)).toString();
  localServerHubId_ = raw_.value(QLatin1String(kLocalServerHubIdKey)).toString();
  displayName_ = raw_.value(QLatin1String(kDisplayNameKey)).toString();
  if (const QString lang = raw_.value(QLatin1String(kPluginLanguageKey)).toString(); isValidPluginLanguage(lang)) pluginLanguage_ = lang;
  pluginHudAllHidden_ = raw_.value(QLatin1String(kPluginHudAllHiddenKey)).toBool(false);
  if (const QJsonValue v = raw_.value(QLatin1String(kAudioVolumeKey)); v.isDouble()) {
    audioVolume_ = std::clamp(static_cast<int>(v.toDouble()), 0, 100);
  }
  audioMuted_ = raw_.value(QLatin1String(kAudioMutedKey)).toBool(false);
  audioMuteInBackground_ = raw_.value(QLatin1String(kAudioMuteBgKey)).toBool(true);
  if (const int st = raw_.value(QLatin1String(kAudioStepKey)).toInt(5); st == 5 || st == 10) audioVolumeStep_ = st;
  audioShowVolume_ = raw_.value(QLatin1String(kAudioShowVolumeKey)).toBool(true);
  mcpEnabled_ = raw_.value(QLatin1String(kMcpEnabledKey)).toBool(false);
  {
    const int port = raw_.value(QLatin1String(kMcpPortKey)).toInt(kDefaultMcpPort);
    mcpPort_ = port >= 1024 && port <= 65535 ? port : kDefaultMcpPort;
  }
  audioOutputDevice_ = raw_.value(QLatin1String(kAudioDeviceKey)).toString();
  audioVoiceGain_ = std::clamp(raw_.value(QLatin1String(kAudioVoiceGainKey)).toInt(100), 0, 100);
  audioPluginGain_ = std::clamp(raw_.value(QLatin1String(kAudioPluginGainKey)).toInt(100), 0, 100);
  audioMixAll_ = raw_.value(QLatin1String(kAudioMixAllKey)).toBool(false);
  const QJsonObject tg = raw_.value(QLatin1String(kAudioTileGainsKey)).toObject();
  for (auto it = tg.begin(); it != tg.end(); ++it) {
    if (it.value().isDouble()) audioTileGains_.insert(it.key(), std::clamp(it.value().toInt(100), 0, 100));
  }
  diagEmulationOpen_ = raw_.value(QLatin1String(kDiagEmulationKey)).toBool(true);
  diagStreamingOpen_ = raw_.value(QLatin1String(kDiagStreamingKey)).toBool(true);
  const QString sort = raw_.value(QLatin1String(kLibrarySortKey)).toString();
  if (isValidLibrarySort(sort)) librarySort_ = sort;
  libraryReadyFirst_ = raw_.value(QLatin1String(kLibraryReadyFirstKey)).toBool(false);
  stateLoadAsk_ = raw_.value(QLatin1String(kStateLoadAskKey)).toBool(true);
  const QString view = raw_.value(QLatin1String(kLibraryViewKey)).toString();
  if (isValidLibraryView(view)) libraryView_ = view;
  if (const QJsonValue v = raw_.value(QLatin1String(kRomCacheLimitKey)); v.isDouble() && v.toDouble() >= 0) {
    romCacheLimit_ = static_cast<qint64>(v.toDouble());
  }
  if (const QJsonValue v = raw_.value(QLatin1String(kVariantCacheLimitKey)); v.isDouble() && v.toDouble() >= 0) {
    variantCacheLimit_ = static_cast<qint64>(v.toDouble());
  }
  if (const int d = raw_.value(QLatin1String(kVariantUnplayedDaysKey)).toInt(0); d == 30 || d == 60 || d == 90) variantUnplayedDays_ = d;
  for (const QJsonValue& v : raw_.value(QLatin1String(kLibraryFoldersKey)).toArray()) {
    const QString f = QDir::cleanPath(v.toString());
    if (QDir::isAbsolutePath(f) && !libraryFolders_.contains(f)) libraryFolders_.append(f);
  }
  const QString vq = raw_.value(QLatin1String(kViewerQualityKey)).toString();
  if (vq == QLatin1String("auto") || vq == QLatin1String("high") || vq == QLatin1String("medium") || vq == QLatin1String("low")) {
    viewerQuality_ = vq;
  }
  const QString vis = raw_.value(QLatin1String(kSessionVisibilityKey)).toString();
  if (vis == QLatin1String("private") || vis == QLatin1String("hub_users") || vis == QLatin1String("invite_only")) {
    sessionVisibility_ = vis;
  }
}

QString PlayerSettings::appearanceName(Appearance a) {
  switch (a) {
    case Appearance::Light: return QStringLiteral("light");
    case Appearance::System: return QStringLiteral("system");
    case Appearance::Dark: break;
  }
  return QStringLiteral("dark");
}

PlayerSettings::Appearance PlayerSettings::parseAppearance(const QString& name, Appearance fallback) {
  const QString n = name.trimmed().toLower();
  if (n == QLatin1String("dark")) return Appearance::Dark;
  if (n == QLatin1String("light")) return Appearance::Light;
  if (n == QLatin1String("system")) return Appearance::System;
  return fallback;
}

bool PlayerSettings::setAppearance(Appearance a) {
  appearance_ = a;
  raw_.insert(QLatin1String(kAppearanceKey), appearanceName(a));
  return save();
}

bool PlayerSettings::setTextSize(const QString& size) {
  if (size != QLatin1String("100") && size != QLatin1String("125") && size != QLatin1String("tv")) {
    return false;
  }
  textSize_ = size;
  raw_.insert(QLatin1String(kTextSizeKey), size);
  return save();
}

bool PlayerSettings::setReduceMotion(bool on) {
  reduceMotion_ = on;
  raw_.insert(QLatin1String(kReduceMotionKey), on);
  return save();
}

bool PlayerSettings::setUpdateChannel(const QString& channelIn) {
  const QString channel = channelIn == QLatin1String("test") ? QStringLiteral("beta") : channelIn;
  if (!channel.isEmpty() && channel != QLatin1String("stable") && channel != QLatin1String("beta") && channel != QLatin1String("off")) {
    return false;
  }
  updateChannel_ = channel;
  if (channel.isEmpty()) {
    raw_.remove(QLatin1String(kUpdateChannelKey));
  } else {
    raw_.insert(QLatin1String(kUpdateChannelKey), channel);
  }
  return save();
}

bool PlayerSettings::setUpdateChannelDefault(const QString& channel) {
  if (channel != QLatin1String("stable") && channel != QLatin1String("beta")) return false;
  updateChannelDefault_ = channel;
  raw_.insert(QLatin1String(kUpdateChannelDefaultKey), channel);
  return save();
}

bool PlayerSettings::setUpdateAutoInstall(bool on) {
  updateAutoInstall_ = on;
  raw_.insert(QLatin1String(kUpdateAutoKey), on);
  return save();
}

bool PlayerSettings::clearUpdateAutoInstall() {
  updateAutoInstall_.reset();
  raw_.remove(QLatin1String(kUpdateAutoKey));
  return save();
}

QJsonArray PlayerSettings::updateHistory() const { return raw_.value(QLatin1String(kUpdateHistoryKey)).toArray(); }

bool PlayerSettings::setUpdateHistory(const QJsonArray& history) {
  raw_.insert(QLatin1String(kUpdateHistoryKey), history);
  return save();
}

QJsonObject PlayerSettings::updatePause() const { return raw_.value(QLatin1String(kUpdatePausedKey)).toObject(); }

bool PlayerSettings::setUpdatePause(const QJsonObject& pause) {
  if (pause.isEmpty()) {
    raw_.remove(QLatin1String(kUpdatePausedKey));
  } else {
    raw_.insert(QLatin1String(kUpdatePausedKey), pause);
  }
  return save();
}

bool PlayerSettings::setUpdateSkip(const QString& version) {
  updateSkip_ = version;
  if (version.isEmpty()) {
    raw_.remove(QLatin1String(kUpdateSkipKey));
  } else {
    raw_.insert(QLatin1String(kUpdateSkipKey), version);
  }
  return save();
}

bool PlayerSettings::setWhatsNewVersion(const QString& version) {
  whatsNewVersion_ = version;
  if (version.isEmpty()) {
    raw_.remove(QLatin1String(kWhatsNewKey));
  } else {
    raw_.insert(QLatin1String(kWhatsNewKey), version);
  }
  return save();
}

bool PlayerSettings::setDiagnosticsOpen(bool open) {
  diagOpen_ = open;
  raw_.insert(QLatin1String(kDiagOpenKey), open);
  return save();
}

bool PlayerSettings::setDeveloperMode(bool on) {
  developerMode_ = on;
  raw_.insert(QLatin1String(kDeveloperModeKey), on);
  return save();
}

bool PlayerSettings::setPluginReloadOnChange(bool on) {
  pluginReloadOnChange_ = on;
  raw_.insert(QLatin1String(kPluginReloadOnChangeKey), on);
  return save();
}

bool PlayerSettings::setDiscordPresence(bool on) {
  discordPresence_ = on;
  raw_.insert(QLatin1String(kDiscordPresenceKey), on);
  return save();
}

bool PlayerSettings::setAudioVolume(int percent) {
  audioVolume_ = std::clamp(percent, 0, 100);
  raw_.insert(QLatin1String(kAudioVolumeKey), audioVolume_);
  return save();
}

bool PlayerSettings::setAudioMuted(bool muted) {
  audioMuted_ = muted;
  raw_.insert(QLatin1String(kAudioMutedKey), muted);
  return save();
}

bool PlayerSettings::setAudioMuteInBackground(bool on) {
  audioMuteInBackground_ = on;
  raw_.insert(QLatin1String(kAudioMuteBgKey), on);
  return save();
}

bool PlayerSettings::setAudioVolumeStep(int percent) {
  if (percent != 5 && percent != 10) return false;
  audioVolumeStep_ = percent;
  raw_.insert(QLatin1String(kAudioStepKey), percent);
  return save();
}

bool PlayerSettings::setAudioShowVolume(bool on) {
  audioShowVolume_ = on;
  raw_.insert(QLatin1String(kAudioShowVolumeKey), on);
  return save();
}

bool PlayerSettings::setMcpEnabled(bool on) {
  mcpEnabled_ = on;
  raw_.insert(QLatin1String(kMcpEnabledKey), on);
  return save();
}

bool PlayerSettings::setMcpPort(int port) {
  if (port < 1024 || port > 65535) return false;
  mcpPort_ = port;
  raw_.insert(QLatin1String(kMcpPortKey), port);
  return save();
}

bool PlayerSettings::setAudioVoiceGain(int percent) {
  audioVoiceGain_ = std::clamp(percent, 0, 100);
  raw_.insert(QLatin1String(kAudioVoiceGainKey), audioVoiceGain_);
  return save();
}

bool PlayerSettings::setAudioPluginGain(int percent) {
  audioPluginGain_ = std::clamp(percent, 0, 100);
  raw_.insert(QLatin1String(kAudioPluginGainKey), audioPluginGain_);
  return save();
}

bool PlayerSettings::setAudioMixAll(bool on) {
  audioMixAll_ = on;
  raw_.insert(QLatin1String(kAudioMixAllKey), on);
  return save();
}

int PlayerSettings::audioTileGain(const QString& userId) const { return audioTileGains_.value(userId, 100); }

bool PlayerSettings::setAudioTileGain(const QString& userId, int percent) {
  if (userId.isEmpty()) return false;
  percent = std::clamp(percent, 0, 100);
  audioTileGains_.insert(userId, percent);
  QJsonObject o = raw_.value(QLatin1String(kAudioTileGainsKey)).toObject();
  o.insert(userId, percent);
  raw_.insert(QLatin1String(kAudioTileGainsKey), o);
  return save();
}

bool PlayerSettings::setAudioOutputDevice(const QString& id) {
  audioOutputDevice_ = id;
  if (id.isEmpty()) {
    raw_.remove(QLatin1String(kAudioDeviceKey));
  } else {
    raw_.insert(QLatin1String(kAudioDeviceKey), id);
  }
  return save();
}

bool PlayerSettings::setPlayOnThisPcChosen(bool chosen) {
  playOnThisPc_ = chosen;
  raw_.insert(QLatin1String(kPlayOnThisPcKey), chosen);
  return save();
}

namespace {
bool putOrRemove(QJsonObject& raw, const char* key, const QString& value) {
  if (value.isEmpty()) {
    raw.remove(QLatin1String(key));
  } else {
    raw.insert(QLatin1String(key), value);
  }
  return true;
}
}  // namespace

bool PlayerSettings::setLocalServerMode(const QString& mode) {
  if (!mode.isEmpty() && mode != QLatin1String("child") && mode != QLatin1String("service")) return false;
  localServerMode_ = mode;
  putOrRemove(raw_, kLocalServerModeKey, mode);
  return save();
}

bool PlayerSettings::setLocalServerPort(int port) {
  if (port < 0 || port > 65535) return false;
  localServerPort_ = port;
  if (port == 0) {
    raw_.remove(QLatin1String(kLocalServerPortKey));
  } else {
    raw_.insert(QLatin1String(kLocalServerPortKey), port);
  }
  return save();
}

bool PlayerSettings::setLocalServerDataDir(const QString& dir) {
  localServerDataDir_ = dir;
  putOrRemove(raw_, kLocalServerDataDirKey, dir);
  return save();
}

bool PlayerSettings::setLocalServerHubId(const QString& hubId) {
  localServerHubId_ = hubId;
  putOrRemove(raw_, kLocalServerHubIdKey, hubId);
  return save();
}

bool PlayerSettings::setDisplayName(const QString& name) {
  displayName_ = name.trimmed();
  putOrRemove(raw_, kDisplayNameKey, displayName_);
  return save();
}

PlayerSettings::PendingShare PlayerSettings::pendingShare() const {
  PendingShare p;
  const QJsonObject o = raw_.value(QLatin1String(kPendingShareKey)).toObject();
  p.port = o.value(QLatin1String("port")).toInt(0);
  p.join = o.value(QLatin1String("join")).toString();
  p.dataDir = o.value(QLatin1String("data_dir")).toString();
  if (p.port <= 0 || p.port > 65535 || p.dataDir.isEmpty()) return {};
  if (p.join != QLatin1String("internet")) p.join = QStringLiteral("home");
  return p;
}

bool PlayerSettings::setPendingShare(const PendingShare& p) {
  if (!p.valid()) return false;
  raw_.insert(QLatin1String(kPendingShareKey),
              QJsonObject{{QStringLiteral("port"), p.port},
                          {QStringLiteral("join"), p.join == QLatin1String("internet") ? p.join : QStringLiteral("home")},
                          {QStringLiteral("data_dir"), p.dataDir}});
  return save();
}

bool PlayerSettings::clearPendingShare() {
  raw_.remove(QLatin1String(kPendingShareKey));
  return save();
}

bool PlayerSettings::setPluginLanguage(const QString& tag) {
  if (!isValidPluginLanguage(tag)) return false;
  pluginLanguage_ = tag;
  raw_.insert(QLatin1String(kPluginLanguageKey), tag);
  return save();
}

bool PlayerSettings::setDiagnosticsEmulationOpen(bool open) {
  diagEmulationOpen_ = open;
  raw_.insert(QLatin1String(kDiagEmulationKey), open);
  return save();
}

bool PlayerSettings::setDiagnosticsStreamingOpen(bool open) {
  diagStreamingOpen_ = open;
  raw_.insert(QLatin1String(kDiagStreamingKey), open);
  return save();
}

bool PlayerSettings::isValidLibrarySort(const QString& key) {
  static const QStringList keys = {QStringLiteral("name_asc"),  QStringLiteral("name_desc"), QStringLiteral("added_desc"),
                                   QStringLiteral("added_asc"), QStringLiteral("size_desc"), QStringLiteral("size_asc"),
                                   QStringLiteral("system"),    QStringLiteral("played"),    QStringLiteral("playtime")};
  return keys.contains(key);
}

bool PlayerSettings::setLibrarySort(const QString& key) {
  if (!isValidLibrarySort(key)) return false;
  librarySort_ = key;
  raw_.insert(QLatin1String(kLibrarySortKey), key);
  return save();
}

bool PlayerSettings::setLibraryReadyFirst(bool on) {
  libraryReadyFirst_ = on;
  raw_.insert(QLatin1String(kLibraryReadyFirstKey), on);
  return save();
}

bool PlayerSettings::setStateLoadAsk(bool on) {
  stateLoadAsk_ = on;
  raw_.insert(QLatin1String(kStateLoadAskKey), on);
  return save();
}

bool PlayerSettings::isValidLibraryView(const QString& view) {
  return view == QLatin1String("list") || view == QLatin1String("grid");
}

bool PlayerSettings::setLibraryView(const QString& view) {
  if (!isValidLibraryView(view)) return false;
  libraryView_ = view;
  raw_.insert(QLatin1String(kLibraryViewKey), view);
  return save();
}

bool PlayerSettings::setLibraryFolders(const QStringList& folders) {
  QStringList clean;
  for (const QString& raw : folders) {
    if (raw.trimmed().isEmpty() || !QDir::isAbsolutePath(raw)) return false;
    const QString f = QDir::cleanPath(raw);
    if (!clean.contains(f)) clean.append(f);
  }
  libraryFolders_ = clean;
  raw_.insert(QLatin1String(kLibraryFoldersKey), QJsonArray::fromStringList(clean));
  return save();
}

bool PlayerSettings::setRomCacheLimitBytes(qint64 bytes) {
  if (bytes < 0) return false;
  romCacheLimit_ = bytes;
  raw_.insert(QLatin1String(kRomCacheLimitKey), static_cast<double>(bytes));
  return save();
}

bool PlayerSettings::setVariantCacheLimitBytes(qint64 bytes) {
  if (bytes < 0) return false;
  variantCacheLimit_ = bytes;
  raw_.insert(QLatin1String(kVariantCacheLimitKey), static_cast<double>(bytes));
  return save();
}

bool PlayerSettings::setVariantUnplayedDays(int days) {
  if (days != 0 && days != 30 && days != 60 && days != 90) return false;
  variantUnplayedDays_ = days;
  raw_.insert(QLatin1String(kVariantUnplayedDaysKey), days);
  return save();
}

qint64 PlayerSettings::lastPlayed(const QString& hubId, const QString& gameId) const {
  return static_cast<qint64>(raw_.value(QLatin1String(kLastPlayedKey)).toObject().value(hubId).toObject().value(gameId).toDouble(0));
}

QHash<QString, qint64> PlayerSettings::lastPlayedAll(const QString& hubId) const {
  QHash<QString, qint64> out;
  const QJsonObject hub = raw_.value(QLatin1String(kLastPlayedKey)).toObject().value(hubId).toObject();
  for (auto it = hub.begin(); it != hub.end(); ++it) {
    const qint64 ms = static_cast<qint64>(it.value().toDouble(0));
    if (ms > 0) out.insert(it.key(), ms);
  }
  return out;
}

bool PlayerSettings::setLastPlayed(const QString& hubId, const QString& gameId, qint64 msecs) {
  if (hubId.isEmpty() || gameId.isEmpty() || msecs <= 0) return false;
  QJsonObject all = raw_.value(QLatin1String(kLastPlayedKey)).toObject();
  QJsonObject hub = all.value(hubId).toObject();
  hub.insert(gameId, static_cast<double>(msecs));
  all.insert(hubId, hub);
  raw_.insert(QLatin1String(kLastPlayedKey), all);
  return save();
}

bool PlayerSettings::isNotificationKind(const QString& kind) {
  return kind == QLatin1String("invite") || kind == QLatin1String("session_started") || kind == QLatin1String("run_event") ||
         kind == QLatin1String("update") || kind == QLatin1String("server_problem");
}

QString PlayerSettings::notificationMode(const QString& kind) const {
  const QJsonObject modes = raw_.value(QLatin1String(kNotifyKey)).toObject().value(QStringLiteral("modes")).toObject();
  return modes.value(kind).toString() == QLatin1String("list") ? QStringLiteral("list") : QStringLiteral("toast");
}

bool PlayerSettings::setNotificationMode(const QString& kind, const QString& mode) {
  if (!isNotificationKind(kind) || (mode != QLatin1String("toast") && mode != QLatin1String("list"))) return false;
  QJsonObject n = raw_.value(QLatin1String(kNotifyKey)).toObject();
  QJsonObject modes = n.value(QStringLiteral("modes")).toObject();
  modes.insert(kind, mode);
  n.insert(QStringLiteral("modes"), modes);
  raw_.insert(QLatin1String(kNotifyKey), n);
  return save();
}

bool PlayerSettings::notifyQuietFullscreen() const {
  return raw_.value(QLatin1String(kNotifyKey)).toObject().value(QStringLiteral("quiet_fullscreen")).toBool(true);
}

bool PlayerSettings::setNotifyQuietFullscreen(bool on) {
  QJsonObject n = raw_.value(QLatin1String(kNotifyKey)).toObject();
  n.insert(QStringLiteral("quiet_fullscreen"), on);
  raw_.insert(QLatin1String(kNotifyKey), n);
  return save();
}

bool PlayerSettings::notifySound() const { return raw_.value(QLatin1String(kNotifyKey)).toObject().value(QStringLiteral("sound")).toBool(false); }

bool PlayerSettings::setNotifySound(bool on) {
  QJsonObject n = raw_.value(QLatin1String(kNotifyKey)).toObject();
  n.insert(QStringLiteral("sound"), on);
  raw_.insert(QLatin1String(kNotifyKey), n);
  return save();
}

QHash<QString, QString> PlayerSettings::pluginHudCorners() const {
  QHash<QString, QString> out;
  const QJsonObject o = raw_.value(QLatin1String(kPluginHudCornersKey)).toObject();
  for (auto it = o.begin(); it != o.end(); ++it) {
    const QString c = it.value().toString();
    if (c == QLatin1String("top-left") || c == QLatin1String("bottom-left") || c == QLatin1String("bottom-right")) out.insert(it.key(), c);
  }
  return out;
}

bool PlayerSettings::setPluginHudCorner(const QString& pluginId, const QString& corner) {
  if (pluginId.isEmpty() || (corner != QLatin1String("top-left") && corner != QLatin1String("bottom-left") && corner != QLatin1String("bottom-right")))
    return false;
  QJsonObject o = raw_.value(QLatin1String(kPluginHudCornersKey)).toObject();
  o.insert(pluginId, corner);
  raw_.insert(QLatin1String(kPluginHudCornersKey), o);
  return save();
}

QStringList PlayerSettings::pluginHudHidden() const {
  QStringList out;
  const QJsonArray a = raw_.value(QLatin1String(kPluginHudHiddenKey)).toArray();
  for (const QJsonValue& v : a)
    if (v.isString() && !v.toString().isEmpty()) out << v.toString();
  return out;
}

bool PlayerSettings::setPluginHudHidden(const QString& pluginId, bool hidden) {
  if (pluginId.isEmpty()) return false;
  QStringList ids = pluginHudHidden();
  ids.removeAll(pluginId);
  if (hidden) ids << pluginId;
  raw_.insert(QLatin1String(kPluginHudHiddenKey), QJsonArray::fromStringList(ids));
  return save();
}

bool PlayerSettings::setPluginHudAllHidden(bool hidden) {
  pluginHudAllHidden_ = hidden;
  raw_.insert(QLatin1String(kPluginHudAllHiddenKey), hidden);
  return save();
}

QHash<QString, QString> PlayerSettings::pluginToastMutes() const {
  QHash<QString, QString> out;
  const QJsonObject o = raw_.value(QLatin1String(kPluginToastMuteKey)).toObject();
  for (auto it = o.begin(); it != o.end(); ++it) {
    const QString m = it.value().toString();
    if (m == QLatin1String("always") || m == QLatin1String("important")) out.insert(it.key(), m);
  }
  return out;
}

bool PlayerSettings::setPluginToastMute(const QString& pluginId, const QString& mode) {
  if (pluginId.isEmpty() || (!mode.isEmpty() && mode != QLatin1String("always") && mode != QLatin1String("important"))) return false;
  QJsonObject o = raw_.value(QLatin1String(kPluginToastMuteKey)).toObject();
  if (mode.isEmpty()) o.remove(pluginId);
  else o.insert(pluginId, mode);
  raw_.insert(QLatin1String(kPluginToastMuteKey), o);
  return save();
}

QHash<QString, QString> PlayerSettings::pluginHotkeyOverrides() const {
  QHash<QString, QString> out;
  const QJsonObject o = raw_.value(QLatin1String(kPluginHotkeysKey)).toObject();
  for (auto it = o.begin(); it != o.end(); ++it)
    if (it.value().isString() && !it.value().toString().isEmpty()) out.insert(it.key(), it.value().toString());
  return out;
}

bool PlayerSettings::setPluginHotkeyOverride(const QString& pluginId, const QString& hotkeyId, const QString& key) {
  if (pluginId.isEmpty() || hotkeyId.isEmpty()) return false;
  QJsonObject o = raw_.value(QLatin1String(kPluginHotkeysKey)).toObject();
  const QString k = pluginId + QLatin1Char('/') + hotkeyId;
  if (key.isEmpty()) o.remove(k);
  else o.insert(k, key);
  if (o.isEmpty()) raw_.remove(QLatin1String(kPluginHotkeysKey));
  else raw_.insert(QLatin1String(kPluginHotkeysKey), o);
  return save();
}

bool PlayerSettings::setSessionVisibility(const QString& visibility) {
  if (visibility != QLatin1String("private") && visibility != QLatin1String("hub_users") && visibility != QLatin1String("invite_only")) {
    return false;
  }
  sessionVisibility_ = visibility;
  raw_.insert(QLatin1String(kSessionVisibilityKey), visibility);
  return save();
}

bool PlayerSettings::setViewerQuality(const QString& pick) {
  if (pick != QLatin1String("auto") && pick != QLatin1String("high") && pick != QLatin1String("medium") && pick != QLatin1String("low")) {
    return false;
  }
  viewerQuality_ = pick;
  raw_.insert(QLatin1String(kViewerQualityKey), pick);
  return save();
}

PlayerSettings::WindowGeometry PlayerSettings::bottomWindowGeometry() const {
  const QJsonObject o = raw_.value(QLatin1String(kBottomWindowKey)).toObject();
  WindowGeometry g;
  g.x = o.value(QLatin1String("x")).toInt();
  g.y = o.value(QLatin1String("y")).toInt();
  g.width = o.value(QLatin1String("width")).toInt();
  g.height = o.value(QLatin1String("height")).toInt();
  g.screen = o.value(QLatin1String("screen")).toString();
  if (!g.valid()) return {};
  return g;
}

bool PlayerSettings::setBottomWindowGeometry(const WindowGeometry& g) {
  if (!g.valid()) {
    raw_.remove(QLatin1String(kBottomWindowKey));
  } else {
    QJsonObject o;
    o.insert(QLatin1String("x"), g.x);
    o.insert(QLatin1String("y"), g.y);
    o.insert(QLatin1String("width"), g.width);
    o.insert(QLatin1String("height"), g.height);
    o.insert(QLatin1String("screen"), g.screen);
    raw_.insert(QLatin1String(kBottomWindowKey), o);
  }
  return save();
}

QString PlayerSettings::saveSlot(const QString& hubId, const QString& gameId) const {
  const QString slot = raw_.value(QLatin1String(kSaveSlotsKey)).toObject().value(hubId).toObject().value(gameId).toString();
  return SaveStore::isValidSlotName(slot) ? slot : QStringLiteral("default");
}

bool PlayerSettings::setSaveSlot(const QString& hubId, const QString& gameId, const QString& slot) {
  if (!SaveStore::isValidSlotName(slot) || hubId.isEmpty() || gameId.isEmpty()) {
    return false;
  }
  QJsonObject all = raw_.value(QLatin1String(kSaveSlotsKey)).toObject();
  QJsonObject hub = all.value(hubId).toObject();
  if (slot == QLatin1String("default")) {
    hub.remove(gameId);
  } else {
    hub.insert(gameId, slot);
  }
  if (hub.isEmpty()) {
    all.remove(hubId);
  } else {
    all.insert(hubId, hub);
  }
  if (all.isEmpty()) {
    raw_.remove(QLatin1String(kSaveSlotsKey));
  } else {
    raw_.insert(QLatin1String(kSaveSlotsKey), all);
  }
  return save();
}

bool PlayerSettings::save() const {
  if (!QDir().mkpath(QFileInfo(path_).absolutePath())) {
    return false;
  }
  QSaveFile f(path_);
  if (!f.open(QIODevice::WriteOnly)) {
    return false;
  }
  f.write(QJsonDocument(raw_).toJson(QJsonDocument::Indented));
  return f.commit();
}

}  // namespace uzume
