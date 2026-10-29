#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include "playersettings.h"

using namespace uzume;
using A = PlayerSettings::Appearance;

class PlayerSettingsTest : public QObject {
  Q_OBJECT
 private slots:
  void defaultsToDark() {
    QTemporaryDir dir;
    PlayerSettings s(dir.path());
    QCOMPARE(s.appearance(), A::Dark);
    QVERIFY(!QFile::exists(s.filePath()));  // nothing is written until something changes
  }

  void localServerSettingsPersistAndValidate() {
    QTemporaryDir dir;
    {
      PlayerSettings s(dir.path());
      QCOMPARE(s.localServerMode(), QString());
      QVERIFY(!s.setLocalServerMode("bogus"));
      QVERIFY(s.setLocalServerMode("child"));
      QVERIFY(s.setLocalServerPort(8765));
      QVERIFY(!s.setLocalServerPort(70000));
      QVERIFY(s.setLocalServerDataDir("/x/server"));
      QVERIFY(s.setLocalServerHubId("hub-1"));
      QVERIFY(!s.pendingShare().valid());
      QVERIFY(!s.setPendingShare({}));
      QVERIFY(s.setPendingShare({8443, "internet", "/x/server"}));
    }
    PlayerSettings again(dir.path());
    QCOMPARE(again.localServerMode(), QStringLiteral("child"));
    QCOMPARE(again.localServerPort(), 8765);
    QCOMPARE(again.localServerDataDir(), QStringLiteral("/x/server"));
    QCOMPARE(again.localServerHubId(), QStringLiteral("hub-1"));
    QCOMPARE(again.pendingShare().port, 8443);
    QCOMPARE(again.pendingShare().join, QStringLiteral("internet"));
    QVERIFY(again.clearPendingShare());
    QVERIFY(!again.pendingShare().valid());
    QVERIFY(again.setLocalServerMode(""));
    QCOMPARE(PlayerSettings(dir.path()).localServerMode(), QString());
  }

  void playOnThisPcChoicePersists() {
    QTemporaryDir dir;
    {
      PlayerSettings s(dir.path());
      QVERIFY(!s.playOnThisPcChosen());
      s.setPlayOnThisPcChosen(true);
      QVERIFY(s.playOnThisPcChosen());
    }
    PlayerSettings again(dir.path());
    QVERIFY(again.playOnThisPcChosen());
  }

  void pluginLanguageDefaultsToAutoAndPersists() {
    QTemporaryDir dir;
    {
      PlayerSettings s(dir.path());
      QCOMPARE(s.pluginLanguage(), QStringLiteral("auto"));
      QVERIFY(!s.setPluginLanguage(QStringLiteral("../x")));
      QVERIFY(!s.setPluginLanguage(QString()));
      QCOMPARE(s.pluginLanguage(), QStringLiteral("auto"));
      QVERIFY(s.setPluginLanguage(QStringLiteral("de")));
    }
    PlayerSettings again(dir.path());
    QCOMPARE(again.pluginLanguage(), QStringLiteral("de"));
  }

  void diagnosticsStatesDefaultAndPersist() {  // 0.6 D5
    QTemporaryDir dir;
    {
      PlayerSettings s(dir.path());
      QVERIFY(!s.diagnosticsOpen());
      QVERIFY(s.diagnosticsEmulationOpen());
      QVERIFY(s.diagnosticsStreamingOpen());
      QVERIFY(s.setDiagnosticsOpen(true));
      QVERIFY(s.setDiagnosticsEmulationOpen(false));
    }
    {
      PlayerSettings s(dir.path());
      QVERIFY(s.diagnosticsOpen());
      QVERIFY(!s.diagnosticsEmulationOpen());
      QVERIFY(s.diagnosticsStreamingOpen());  // untouched stays at the default
      QVERIFY(s.setAppearance(A::Light));      // other keys survive and vice versa
      QVERIFY(s.setDiagnosticsStreamingOpen(false));
    }
    PlayerSettings s(dir.path());
    QCOMPARE(s.appearance(), A::Light);
    QVERIFY(s.diagnosticsOpen() && !s.diagnosticsEmulationOpen() && !s.diagnosticsStreamingOpen());
  }

  void persistsAppearance() {
    QTemporaryDir dir;
    {
      PlayerSettings s(dir.path());
      QVERIFY(s.setAppearance(A::Light));
    }
    QCOMPARE(PlayerSettings(dir.path()).appearance(), A::Light);
    {
      PlayerSettings s(dir.path());
      QVERIFY(s.setAppearance(A::System));
    }
    QCOMPARE(PlayerSettings(dir.path()).appearance(), A::System);
    QVERIFY(PlayerSettings(dir.path()).filePath().endsWith(QLatin1String("settings/player.json")));
  }

  void persistsTextSizeAndReduceMotion() {
    QTemporaryDir dir;
    {
      PlayerSettings s(dir.path());
      QCOMPARE(s.textSize(), QStringLiteral("100"));
      QVERIFY(!s.reduceMotion().has_value());  // follows the system until changed
      QVERIFY(!s.setTextSize(QStringLiteral("200")));
      QVERIFY(s.setTextSize(QStringLiteral("tv")));
      QVERIFY(s.setReduceMotion(false));
    }
    PlayerSettings s(dir.path());
    QCOMPARE(s.textSize(), QStringLiteral("tv"));
    QVERIFY(s.reduceMotion().has_value() && !*s.reduceMotion());
  }

  void corruptedOrUnknownFallsBackToDark() {
    QTemporaryDir dir;
    PlayerSettings probe(dir.path());
    QVERIFY(probe.setAppearance(A::Light));
    QFile f(probe.filePath());
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write("{not json");
    f.close();
    QCOMPARE(PlayerSettings(dir.path()).appearance(), A::Dark);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write("{\"appearance\":\"neon\"}");
    f.close();
    QCOMPARE(PlayerSettings(dir.path()).appearance(), A::Dark);
  }

  void preservesUnknownKeys() {
    QTemporaryDir dir;
    PlayerSettings probe(dir.path());
    QVERIFY(probe.setAppearance(A::Dark));
    QFile f(probe.filePath());
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write("{\"appearance\":\"dark\",\"other\":{\"x\":1}}");
    f.close();
    PlayerSettings s(dir.path());
    QVERIFY(s.setAppearance(A::Light));
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    QCOMPARE(o.value(QStringLiteral("appearance")).toString(), QStringLiteral("light"));
    QCOMPARE(o.value(QStringLiteral("other")).toObject().value(QStringLiteral("x")).toInt(), 1);
  }

  void names() {
    QCOMPARE(PlayerSettings::parseAppearance(QStringLiteral(" System ")), A::System);
    QCOMPARE(PlayerSettings::appearanceName(A::Light), QStringLiteral("light"));
    QCOMPARE(PlayerSettings::parseAppearance(QStringLiteral("x"), A::Light), A::Light);
  }

  void sessionVisibilityRoundTrip() {
    QTemporaryDir dir;
    {
      PlayerSettings s(dir.path());
      QVERIFY(s.sessionVisibility().isEmpty());
      QVERIFY(!s.setSessionVisibility(QStringLiteral("public")));
      QVERIFY(!s.setSessionVisibility(QString()));
      QVERIFY(s.sessionVisibility().isEmpty());
      QVERIFY(s.setSessionVisibility(QStringLiteral("invite_only")));
    }
    QCOMPARE(PlayerSettings(dir.path()).sessionVisibility(), QStringLiteral("invite_only"));
  }

  void viewerQualityRoundTrip() {
    QTemporaryDir dir;
    {
      PlayerSettings s(dir.path());
      QCOMPARE(s.viewerQuality(), QStringLiteral("auto"));
      QVERIFY(!s.setViewerQuality(QStringLiteral("ultra")));
      QVERIFY(!s.setViewerQuality(QString()));
      QCOMPARE(s.viewerQuality(), QStringLiteral("auto"));
      QVERIFY(s.setViewerQuality(QStringLiteral("low")));
    }
    QCOMPARE(PlayerSettings(dir.path()).viewerQuality(), QStringLiteral("low"));
  }

  void bottomWindowGeometryRoundTrip() {
    QTemporaryDir dir;
    {
      PlayerSettings s(dir.path());
      QVERIFY(!s.bottomWindowGeometry().valid());  // never placed
      PlayerSettings::WindowGeometry g;
      g.x = -1920;  // a monitor left of the main one
      g.y = 40;
      g.width = 640;
      g.height = 480;
      g.screen = QStringLiteral("DP-2");
      QVERIFY(s.setBottomWindowGeometry(g));
      QVERIFY(s.setAppearance(A::Light));  // other keys stay
    }
    PlayerSettings s(dir.path());
    const PlayerSettings::WindowGeometry g = s.bottomWindowGeometry();
    QVERIFY(g.valid());
    QCOMPARE(g.x, -1920);
    QCOMPARE(g.y, 40);
    QCOMPARE(g.width, 640);
    QCOMPARE(g.height, 480);
    QCOMPARE(g.screen, QStringLiteral("DP-2"));
    QCOMPARE(s.appearance(), A::Light);
    QVERIFY(s.setBottomWindowGeometry(PlayerSettings::WindowGeometry()));  // invalid removes it
    QVERIFY(!PlayerSettings(dir.path()).bottomWindowGeometry().valid());
  }

  void saveSlotPerHubAndGame() {
    QTemporaryDir dir;
    {
      PlayerSettings s(dir.path());
      QCOMPARE(s.saveSlot(QStringLiteral("hub-a"), QStringLiteral("g1")), QStringLiteral("default"));
      QVERIFY(!s.setSaveSlot(QStringLiteral("hub-a"), QStringLiteral("g1"), QStringLiteral("Bad Name")));
      QVERIFY(s.setSaveSlot(QStringLiteral("hub-a"), QStringLiteral("g1"), QStringLiteral("boss")));
      QVERIFY(s.setSaveSlot(QStringLiteral("hub-b"), QStringLiteral("g1"), QStringLiteral("other")));
      QVERIFY(s.setAppearance(A::Light));
    }
    PlayerSettings s(dir.path());  // persisted, unrelated keys kept
    QCOMPARE(s.saveSlot(QStringLiteral("hub-a"), QStringLiteral("g1")), QStringLiteral("boss"));
    QCOMPARE(s.saveSlot(QStringLiteral("hub-b"), QStringLiteral("g1")), QStringLiteral("other"));
    QCOMPARE(s.saveSlot(QStringLiteral("hub-a"), QStringLiteral("g2")), QStringLiteral("default"));
    QCOMPARE(s.appearance(), A::Light);
    QVERIFY(s.setSaveSlot(QStringLiteral("hub-a"), QStringLiteral("g1"), QStringLiteral("default")));
    QCOMPARE(PlayerSettings(dir.path()).saveSlot(QStringLiteral("hub-a"), QStringLiteral("g1")), QStringLiteral("default"));
  }

  void librarySortAndReadyFirstPersistPerDevice() {
    QTemporaryDir dir;
    {
      PlayerSettings s(dir.path());
      QCOMPARE(s.librarySort(), QStringLiteral("played"));
      QVERIFY(!s.libraryReadyFirst());
      QVERIFY(!s.setLibrarySort(QStringLiteral("random")));
      QCOMPARE(s.librarySort(), QStringLiteral("played"));
      QVERIFY(s.setLibrarySort(QStringLiteral("size_asc")));
      QVERIFY(s.setLibraryReadyFirst(true));
      QVERIFY(s.setAppearance(A::Light));
    }
    PlayerSettings s(dir.path());
    QCOMPARE(s.librarySort(), QStringLiteral("size_asc"));
    QVERIFY(s.libraryReadyFirst());
    QCOMPARE(s.appearance(), A::Light);  // unrelated keys kept
  }

  void romCacheLimitDefaultsTo20GbAndPersists() {
    QTemporaryDir dir;
    {
      PlayerSettings s(dir.path());
      QCOMPARE(s.romCacheLimitBytes(), qint64(20) * 1024 * 1024 * 1024);
      QVERIFY(!s.setRomCacheLimitBytes(-1));
      QCOMPARE(s.romCacheLimitBytes(), PlayerSettings::kDefaultRomCacheLimitBytes);
      QVERIFY(s.setRomCacheLimitBytes(5ll * 1024 * 1024 * 1024));
    }
    {
      PlayerSettings s(dir.path());
      QCOMPARE(s.romCacheLimitBytes(), qint64(5) * 1024 * 1024 * 1024);
      QVERIFY(s.setRomCacheLimitBytes(0));  // unlimited
    }
    PlayerSettings s(dir.path());
    QCOMPARE(s.romCacheLimitBytes(), qint64(0));
  }

  void videoSettingsDefaultAndPersist() {
    QTemporaryDir dir;
    {
      PlayerSettings s(dir.path());
      QCOMPARE(s.videoFullscreenScreen(), QString());
      QVERIFY(s.videoVsync());
      QVERIFY(s.setVideoFullscreenScreen(QStringLiteral("DP-2")));
      QVERIFY(s.setVideoVsync(false));
    }
    {
      PlayerSettings s(dir.path());
      QCOMPARE(s.videoFullscreenScreen(), QStringLiteral("DP-2"));
      QVERIFY(!s.videoVsync());
      QVERIFY(s.setVideoFullscreenScreen(QString()));
    }
    PlayerSettings s(dir.path());
    QCOMPARE(s.videoFullscreenScreen(), QString());
  }

  void audioSettingsDefaultAndPersist() {  // 0.19  // 0.19
    QTemporaryDir dir;
    {
      PlayerSettings s(dir.path());
      QCOMPARE(s.audioVolume(), 100);
      QVERIFY(!s.audioMuted());
      QVERIFY(s.audioMuteInBackground());
      QCOMPARE(s.audioVolumeStep(), 5);
      QVERIFY(s.audioShowVolume());
      QCOMPARE(s.audioOutputDevice(), QString());
      QVERIFY(!s.setAudioVolumeStep(7));  // only 5 and 10
      QCOMPARE(s.audioVolumeStep(), 5);
      QVERIFY(s.setAudioVolume(250));  // clamped
      QCOMPARE(s.audioVolume(), 100);
      QVERIFY(s.setAudioVolume(35));
      QVERIFY(s.setAudioMuted(true));
      QVERIFY(s.setAudioMuteInBackground(false));
      QVERIFY(s.setAudioVolumeStep(10));
      QVERIFY(s.setAudioShowVolume(false));
      QVERIFY(s.setAudioOutputDevice(QStringLiteral("ZGV2")));
    }
    {
      PlayerSettings s(dir.path());
      QCOMPARE(s.audioVolume(), 35);
      QVERIFY(s.audioMuted());
      QVERIFY(!s.audioMuteInBackground());
      QCOMPARE(s.audioVolumeStep(), 10);
      QVERIFY(!s.audioShowVolume());
      QCOMPARE(s.audioOutputDevice(), QStringLiteral("ZGV2"));
      QVERIFY(s.setAudioOutputDevice(QString()));
    }
    PlayerSettings s(dir.path());
    QCOMPARE(s.audioOutputDevice(), QString());
  }

  void lastPlayedIsSeparatedByHub() {
    QTemporaryDir dir;
    {
      PlayerSettings s(dir.path());
      QCOMPARE(s.lastPlayed(QStringLiteral("hub-a"), QStringLiteral("g1")), qint64(0));
      QVERIFY(!s.setLastPlayed(QString(), QStringLiteral("g1"), 5));
      QVERIFY(!s.setLastPlayed(QStringLiteral("hub-a"), QString(), 5));
      QVERIFY(!s.setLastPlayed(QStringLiteral("hub-a"), QStringLiteral("g1"), 0));
      QVERIFY(s.setLastPlayed(QStringLiteral("hub-a"), QStringLiteral("g1"), 1760000000000LL));
      QVERIFY(s.setLastPlayed(QStringLiteral("hub-a"), QStringLiteral("g2"), 1760000001000LL));
      QVERIFY(s.setLastPlayed(QStringLiteral("hub-b"), QStringLiteral("g1"), 1770000000000LL));
    }
    PlayerSettings s(dir.path());
    QCOMPARE(s.lastPlayed(QStringLiteral("hub-a"), QStringLiteral("g1")), 1760000000000LL);
    QCOMPARE(s.lastPlayed(QStringLiteral("hub-b"), QStringLiteral("g1")), 1770000000000LL);
    QCOMPARE(s.lastPlayed(QStringLiteral("hub-b"), QStringLiteral("g2")), qint64(0));  // same game id on another Hub stays separate
    const QHash<QString, qint64> a = s.lastPlayedAll(QStringLiteral("hub-a"));
    QCOMPARE(a.size(), 2);
    QCOMPARE(a.value(QStringLiteral("g2")), 1760000001000LL);
    QVERIFY(s.lastPlayedAll(QStringLiteral("hub-c")).isEmpty());
  }
};

QTEST_GUILESS_MAIN(PlayerSettingsTest)
#include "playersettings_test.moc"
