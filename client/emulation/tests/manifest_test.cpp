// Manifest parsing and CoreLocator (without core).
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include "core_locator.h"
#include "system_manifest.h"

using namespace uzume::emu;

class ManifestTest : public QObject {
  Q_OBJECT
 private slots:
  void builtinNds() {
    ManifestRegistry reg;
    QString err;
    QVERIFY2(reg.loadBuiltin(&err), qPrintable(err));
    const SystemManifest* nds = reg.find(QStringLiteral("nds"));
    QVERIFY(nds);
    QCOMPARE(nds->displayName, QStringLiteral("Nintendo DS"));
    QVERIFY(nds->coreId.isEmpty());  // the system manifest names no core
    QCOMPARE(nds->extensions, QStringList{QStringLiteral(".nds")});
    QVERIFY(!nds->firmware.required);
    QCOMPARE(nds->inputProfile, QStringLiteral("nds"));
    QCOMPARE(nds->displayProfile, QStringLiteral("dual_screen"));
    QCOMPARE(nds->display.frameSize(), QSize(256, 384));
    QCOMPARE(nds->display.touchScreenIndex(), 1);
    QCOMPARE(nds->display.screenRect(1), QRect(0, 192, 256, 192));
    QCOMPARE(nds->display.toFrameNormalized(1, {0.5, 0.5}), QPointF(0.5, 0.75));
    QVERIFY(nds->supportsExtension(QStringLiteral("NDS")));
    QVERIFY(reg.forExtension(QStringLiteral(".nds")) == nds);
    QVERIFY(!reg.forExtension(QStringLiteral(".xyz")));
    QVERIFY(nds->firmware.fileById(QStringLiteral("firmware")));
  }

  void builtinGameBoySystems() {
    ManifestRegistry reg;
    QString err;
    QVERIFY2(reg.loadBuiltin(&err), qPrintable(err));
    struct Expect { const char* id; const char* name; int order; const char* ext; QSize size; const char* profile; const char* label; const char* fw; const char* identity; };
    for (const Expect& e : {Expect{"gb", "Game Boy", 300, ".gb", QSize(160, 144), "gb", "GB", "gb_bios", "gb_header"},
                            Expect{"gbc", "Game Boy Color", 310, ".gbc", QSize(160, 144), "gb", "GBC", "gbc_bios", "gb_header"},
                            Expect{"gba", "Game Boy Advance", 320, ".gba", QSize(240, 160), "gba", "GBA", "gba_bios", "gba_header"}}) {
      const SystemManifest* m = reg.find(QString::fromLatin1(e.id));
      QVERIFY2(m, e.id);
      QCOMPARE(m->displayName, QString::fromLatin1(e.name));
      QCOMPARE(m->order, e.order);
      QCOMPARE(m->extensions, QStringList{QString::fromLatin1(e.ext)});
      QCOMPARE(m->display.layout, QStringLiteral("single"));
      QCOMPARE(m->display.screens.size(), 1);
      QCOMPARE(m->display.frameSize(), e.size);
      QCOMPARE(m->display.touchScreenIndex(), -1);
      QCOMPARE(m->displayProfile, QStringLiteral("single_screen"));
      QCOMPARE(m->inputProfile, QString::fromLatin1(e.profile));
      QCOMPARE(m->inputLabel, QString::fromLatin1(e.label));
      QCOMPARE(m->identity, QString::fromLatin1(e.identity));
      QVERIFY(!m->firmware.required);
      QCOMPARE(m->firmware.files.size(), 1);
      QCOMPARE(m->firmware.files.first().id, QString::fromLatin1(e.fw));
      QCOMPARE(m->firmware.files.first().name, QString::fromLatin1(e.fw) + QStringLiteral(".bin"));
      QVERIFY(!m->inputMap.isEmpty());
      QCOMPARE(reg.forExtension(QString::fromLatin1(e.ext)), m);
    }
    // gb and gbc share one input map; gba adds the shoulder buttons.
    QCOMPARE(reg.find(QStringLiteral("gb"))->inputMap, reg.find(QStringLiteral("gbc"))->inputMap);
    QVERIFY(!reg.find(QStringLiteral("gb"))->inputMap.contains(QStringLiteral("l")));
    QCOMPARE(reg.find(QStringLiteral("gba"))->inputMap.value(QStringLiteral("zl")), QStringLiteral("l"));
    QCOMPARE(reg.find(QStringLiteral("gb"))->inputMap.value(QStringLiteral("lup")), QStringLiteral("up"));
    // Listing order: the first system stays the DS.
    QCOMPARE(reg.all().first().systemId, QStringLiteral("nds"));
    QCOMPARE(reg.find(QStringLiteral("nds"))->identity, QStringLiteral("nds_header"));
    QCOMPARE(reg.find(QStringLiteral("3ds"))->identity, QStringLiteral("3ds_ncsd"));
  }

  void builtinStatesAndCheatBlocks() {
    ManifestRegistry reg;
    QString err;
    QVERIFY2(reg.loadBuiltin(&err), qPrintable(err));
    for (const char* id : {"melondsds", "desmume", "azahar", "mgba", "gambatte"}) {
      const CoreProfile* p = reg.profile(QString::fromLatin1(id));
      QVERIFY(p);
      QVERIFY2(p->statesSupported, id);
      for (const QString& f : p->cheatFormats) QVERIFY2(CoreProfile::knownCheatFormats().contains(f), id);
    }
    QVERIFY(reg.profile(QStringLiteral("mgba"))->statesRunAhead);
    QVERIFY(reg.profile(QStringLiteral("gambatte"))->statesRunAhead);
    QVERIFY(!reg.profile(QStringLiteral("azahar"))->statesRunAhead);
    QCOMPARE(reg.profile(QStringLiteral("azahar"))->cheatFormats, QStringList{QStringLiteral("gateway")});
    QCOMPARE(reg.profile(QStringLiteral("desmume"))->cheatFormats, QStringList{QStringLiteral("ar_ds")});
    const CoreProfile* mg = reg.profile(QStringLiteral("mgba"));
    QVERIFY(mg->cheatFormats.contains(QStringLiteral("ar_gba")));  // union
    QCOMPARE(mg->cheatFormatsFor(QStringLiteral("gba")), (QStringList{"gsa", "cb", "ar_gba"}));
    QCOMPARE(mg->cheatFormatsFor(QStringLiteral("gb")), (QStringList{"gsa", "game_genie"}));
    QCOMPARE(mg->cheatFormatsFor(QStringLiteral("gbc")), (QStringList{"gsa", "game_genie"}));
    QVERIFY(mg->cheatFormatsFor(QStringLiteral("nds")).isEmpty());
    QCOMPARE(reg.profile(QStringLiteral("gambatte"))->cheatFormatsFor(QStringLiteral("gb")), (QStringList{"game_genie", "gsa"}));
    QCOMPARE(reg.profile(QStringLiteral("melondsds"))->cheatFormats, QStringList{QStringLiteral("ar_ds")});

    // Parsing: defaults, unknown format, run_ahead without supported.
    auto parse = [](const char* extra, QString* e = nullptr) {
      const QByteArray json = QByteArray(R"({"core_id":"x","library_basename":"x_libretro","system_ids":["gb","gba"],)") + extra + "}";
      return ManifestRegistry::parseProfile(json, e);
    };
    auto none = parse(R"("order":1)");  // no cheats block = no cheats
    QVERIFY(none);
    QVERIFY(!none->statesSupported && !none->statesRunAhead && none->cheatFormats.isEmpty() && !none->supportsCheats());
    auto ok = parse(R"("states":{"supported":true},"cheats":{"formats":["raw","gsa","raw"]})");
    QVERIFY(ok);
    QVERIFY(ok->statesSupported && !ok->statesRunAhead);
    QCOMPARE(ok->cheatFormats, (QStringList{"raw", "gsa"}));
    auto perSys = parse(R"("cheats":{"formats":{"gb":["gsa"],"gba":["cb","gsa"]}})");
    QVERIFY(perSys);
    QCOMPARE(perSys->cheatFormats, (QStringList{"gsa", "cb"}));
    QCOMPARE(perSys->cheatFormatsFor(QStringLiteral("gba")), (QStringList{"cb", "gsa"}));
    QVERIFY(perSys->cheatFormatsFor(QStringLiteral("gbc")).isEmpty());
    QCOMPARE(ok->cheatFormatsFor(QStringLiteral("anything")), (QStringList{"raw", "gsa"}));
    QString perr;
    QVERIFY(!parse(R"("cheats":{"formats":{"nds":["gsa"]}})", &perr));
    QVERIFY(!parse(R"("cheats":{"formats":{"gb":["nope"]}})", &perr));
    QVERIFY(!parse(R"("cheats":{"formats":["nope"]})", &perr));
    QVERIFY(perr.contains(QStringLiteral("unknown format")));
    QVERIFY(!parse(R"("states":{"run_ahead":true})", &perr));
  }

  void builtinMgbaAndGambatte() {
    ManifestRegistry reg;
    QString err;
    QVERIFY2(reg.loadBuiltin(&err), qPrintable(err));
    const CoreProfile* mgba = reg.profile(QStringLiteral("mgba"));
    QVERIFY(mgba);
    QCOMPARE(mgba->displayName, QStringLiteral("mGBA"));
    QCOMPARE(mgba->libraryBasename, QStringLiteral("mgba_libretro"));
    QCOMPARE(mgba->systemIds, (QStringList{"gb", "gbc", "gba"}));
    QCOMPARE(mgba->order, 0);
    QCOMPARE(mgba->saveSource, QStringLiteral("save_ram"));
    QCOMPARE(mgba->saveExtension, QStringLiteral(".sav"));
    QCOMPARE(mgba->sysfileOption, QStringLiteral("mgba_use_bios"));
    QCOMPARE(mgba->sysfileNative, QStringLiteral("ON"));
    QCOMPARE(mgba->sysfileBuiltin, QStringLiteral("OFF"));
    QCOMPARE(mgba->coreOptions.value(QStringLiteral("mgba_sgb_borders")), QStringLiteral("OFF"));
    QCOMPARE(mgba->coreOptions.value(QStringLiteral("mgba_skip_bios")), QStringLiteral("OFF"));
    QCOMPARE(mgba->lockedCoreOptions, QStringList{QStringLiteral("mgba_sgb_borders")});
    QCOMPARE(mgba->alwaysShownCoreOptions, QStringList{QStringLiteral("mgba_color_correction")});

    const CoreProfile* gam = reg.profile(QStringLiteral("gambatte"));
    QVERIFY(gam);
    QCOMPARE(gam->displayName, QStringLiteral("Gambatte"));
    QCOMPARE(gam->libraryBasename, QStringLiteral("gambatte_libretro"));
    QCOMPARE(gam->systemIds, (QStringList{"gb", "gbc"}));
    QCOMPARE(gam->order, 10);
    QCOMPARE(gam->sysfileOption, QStringLiteral("gambatte_gb_bootloader"));
    QCOMPARE(gam->sysfileNative, QStringLiteral("enabled"));
    QCOMPARE(gam->sysfileBuiltin, QStringLiteral("disabled"));
    QCOMPARE(gam->coreOptions.value(QStringLiteral("gambatte_gb_link_mode")), QStringLiteral("Not Connected"));
    QCOMPARE(gam->coreOptions.value(QStringLiteral("gambatte_show_gb_link_settings")), QStringLiteral("disabled"));
    QCOMPARE(gam->lockedCoreOptions.size(), 2 + 1 + 12);
    QVERIFY(gam->lockedCoreOptions.contains(QStringLiteral("gambatte_gb_link_network_port")));
    QVERIFY(gam->lockedCoreOptions.contains(QStringLiteral("gambatte_gb_link_network_server_ip_12")));
    QCOMPARE(gam->alwaysShownCoreOptions, QStringList{QStringLiteral("gambatte_gb_colorization")});

    // Core choice per system: mGBA first for all three, Gambatte only for the Game Boy systems.
    QCOMPARE(reg.profilesForSystem(QStringLiteral("gba")).size(), 1);
    QCOMPARE(reg.profilesForSystem(QStringLiteral("gb")).first().coreId, QStringLiteral("mgba"));
    QCOMPARE(reg.profilesForSystem(QStringLiteral("gbc")).size(), 2);
    const auto eff = reg.resolve(QStringLiteral("gba"), QStringLiteral("mgba"));
    QVERIFY(eff);
    QVERIFY(!eff->experimental);
    QCOMPARE(eff->display.frameSize(), QSize(240, 160));
    QCOMPARE(eff->firmware.sysfileOption, QStringLiteral("mgba_use_bios"));
    QVERIFY(!reg.resolve(QStringLiteral("gba"), QStringLiteral("gambatte"))->coreOptions.contains(QStringLiteral("gambatte_gb_link_mode")));
  }

  void conflictingInputMapsAreALoadError() {
    ManifestRegistry reg;
    const auto a = ManifestRegistry::parse(R"({"system_id":"a","display_name":"A","extensions":[".a"],"input_profile":"p",
      "input":{"map":{"a":"a"}},"display":{"screens":[{"id":"m","width":8,"height":8}]}})");
    auto b = *a;
    b.systemId = QStringLiteral("b");
    QVERIFY(a);
    QVERIFY(reg.add(*a));
    QVERIFY(reg.add(b));  // same profile, identical map
    auto c = b;
    c.systemId = QStringLiteral("c");
    c.inputMap.insert(QStringLiteral("b"), QStringLiteral("b"));
    QString err;
    QVERIFY(!reg.add(c, &err));
    QVERIFY(err.contains(QStringLiteral("input_profile p")));
    c.inputProfile = QStringLiteral("q");  // another profile name is fine
    QVERIFY(reg.add(c));
    QVERIFY(!ManifestRegistry::parse(R"({"system_id":"a","display_name":"A","extensions":[".a"],"input":{"map":{"a":""}},
      "display":{"screens":[{"id":"m","width":8,"height":8}]}})", &err));
  }

  void builtin3dsAndAzahar() {
    ManifestRegistry reg;
    QString err;
    QVERIFY2(reg.loadBuiltin(&err), qPrintable(err));
    const SystemManifest* s = reg.find(QStringLiteral("3ds"));
    QVERIFY(s);
    QCOMPARE(s->extensions, (QStringList{QStringLiteral(".3ds"), QStringLiteral(".cci"), QStringLiteral(".cxi"), QStringLiteral(".3dsx"),
                                         QStringLiteral(".zcci"), QStringLiteral(".zcxi"), QStringLiteral(".z3dsx")}));
    QVERIFY(reg.forExtension(QStringLiteral(".CCI")) == s);
    QVERIFY(!s->firmware.required);
    QVERIFY(s->firmware.files.isEmpty());
    QCOMPARE(s->inputProfile, QStringLiteral("3ds"));
    // Unequal widths: top 400x240 at (0,0), bottom 320x240 centred at (40,240), frame 400x480 like the core's default layout.
    QCOMPARE(s->display.frameSize(), QSize(400, 480));
    QCOMPARE(s->display.screenRect(0), QRect(0, 0, 400, 240));
    QCOMPARE(s->display.screenRect(1), QRect(40, 240, 320, 240));
    // Touch maps the bottom-screen rectangle only.
    QCOMPARE(s->display.touchScreenIndex(), 1);
    QCOMPARE(s->display.toFrameNormalized(1, {0.0, 0.0}), QPointF(40.0 / 400, 0.5));
    QCOMPARE(s->display.toFrameNormalized(1, {1.0, 1.0}), QPointF(360.0 / 400, 1.0));
    QCOMPARE(s->display.toFrameNormalized(1, {0.5, 0.5}), QPointF(0.5, 0.75));
    // nds stays as it was (equal widths, default alignment).
    QCOMPARE(reg.find(QStringLiteral("nds"))->display.screenRect(1), QRect(0, 192, 256, 192));

    const CoreProfile* p = reg.profile(QStringLiteral("azahar"));
    QVERIFY(p);
    QCOMPARE(p->libraryBasename, QStringLiteral("azahar_libretro"));
    QVERIFY(p->requiresHwRender);
    QCOMPARE(p->saveSource, QStringLiteral("core_tree"));
    QCOMPARE(p->saveFormat, QStringLiteral("azahar_sdmc"));
    QVERIFY(p->saveShortDir);
    QCOMPARE(p->saveShortDirSubfolder, QStringLiteral("Azahar"));
    QVERIFY(reg.resolve(QStringLiteral("3ds"), QStringLiteral("azahar"))->saveShortDir);
    QCOMPARE(p->coreOptions.value(QStringLiteral("citra_graphics_api")), QStringLiteral("OpenGL"));
    QCOMPARE(p->coreOptions.value(QStringLiteral("citra_layout_option")), QStringLiteral("default"));
    QCOMPARE(p->coreOptions.value(QStringLiteral("citra_swap_screen")), QStringLiteral("Top"));
    for (const char* k : {"citra_graphics_api", "citra_layout_option", "citra_swap_screen", "citra_swap_screen_mode", "citra_enable_touch_touchscreen"})
      QVERIFY2(p->lockedCoreOptions.contains(QString::fromLatin1(k)), k);
    QVERIFY(p->alwaysShownCoreOptions.contains(QStringLiteral("citra_resolution_factor")));
    QVERIFY(!p->lockedCoreOptions.contains(QStringLiteral("citra_resolution_factor")));
    QCOMPARE(reg.profilesForSystem(QStringLiteral("3ds")).size(), 1);
    const auto eff = reg.resolve(QStringLiteral("3ds"), QStringLiteral("azahar"));
    QVERIFY(eff);
    QVERIFY(eff->requiresHwRender);
    QCOMPARE(eff->saveSource, QStringLiteral("core_tree"));
    QCOMPARE(eff->saveFormat, QStringLiteral("azahar_sdmc"));
    QVERIFY(!eff->experimental);
    QVERIFY(!reg.resolve(QStringLiteral("nds"), QStringLiteral("melondsds"))->requiresHwRender);
  }

  void screenAlignment() {
    QString err;
    const auto m = ManifestRegistry::parse(
        R"({"system_id":"x","display_name":"X","extensions":[".x"],"display":{"layout":"vertical","gap":4,"screens":[
            {"id":"a","width":100,"height":50,"align":"end"},{"id":"b","width":60,"height":50,"align":"center","touch":true},
            {"id":"c","width":20,"height":10}]}})", &err);
    QVERIFY2(m, qPrintable(err));
    QCOMPARE(m->display.frameSize(), QSize(100, 118));
    QCOMPARE(m->display.screenRect(0), QRect(0, 0, 100, 50));   // widest: no shift
    QCOMPARE(m->display.screenRect(1), QRect(20, 54, 60, 50));  // centred
    QCOMPARE(m->display.screenRect(2), QRect(0, 108, 20, 10));  // default: start
    const auto h = ManifestRegistry::parse(
        R"({"system_id":"y","display_name":"Y","extensions":[".y"],"display":{"layout":"horizontal","screens":[
            {"width":10,"height":40},{"width":10,"height":20,"align":"end"}]}})", &err);
    QVERIFY2(h, qPrintable(err));
    QCOMPARE(h->display.screenRect(1), QRect(10, 20, 10, 20));  // bottom-aligned side by side
    QVERIFY(!ManifestRegistry::parse(
        R"({"system_id":"x","display_name":"X","extensions":[".x"],"display":{"screens":[{"width":1,"height":1,"align":"middle"}]}})", &err));
  }

  void coreProfilesAndAliases() {
    ManifestRegistry reg;
    QString err;
    QVERIFY2(reg.loadBuiltin(&err), qPrintable(err));
    const CoreProfile* mel = reg.profile(QStringLiteral("melondsds"));
    QVERIFY(mel);
    QCOMPARE(mel->libraryBasename, QStringLiteral("melondsds_libretro"));
    QVERIFY(reg.profile(QStringLiteral("melonds_ds")) == mel);  // legacy alias
    QCOMPARE(reg.canonicalCoreId(QStringLiteral("melonds_ds")), QStringLiteral("melondsds"));
    QCOMPARE(reg.canonicalCoreId(QStringLiteral("noods")), QStringLiteral("noods"));
    QCOMPARE(mel->coreOptions.value(QStringLiteral("melonds_render_mode")), QStringLiteral("software"));
    QVERIFY(mel->lockedCoreOptions.contains(QStringLiteral("melonds_screen_layout1")));
    QCOMPARE(mel->sysfileOption, QStringLiteral("melonds_sysfile_mode"));
    QCOMPARE(mel->fileOptions.value(QStringLiteral("firmware")), QStringLiteral("melonds_firmware_nds_path"));

    const CoreProfile* des = reg.profile(QStringLiteral("desmume"));
    QVERIFY(des);
    QCOMPARE(des->libraryBasename, QStringLiteral("desmume_libretro"));
    QCOMPARE(des->coreOptions.value(QStringLiteral("desmume_screens_layout")), QStringLiteral("top/bottom"));
    QCOMPARE(des->coreOptions.value(QStringLiteral("desmume_screens_gap")), QStringLiteral("0"));
    QCOMPARE(des->coreOptions.value(QStringLiteral("desmume_pointer_type")), QStringLiteral("touch"));
    QCOMPARE(des->saveSource, QStringLiteral("core_file"));
    QCOMPARE(des->saveExtension, QStringLiteral(".dsv"));
    QCOMPARE(des->saveFormat, QStringLiteral("desmume_dsv"));
    QCOMPARE(mel->saveSource, QStringLiteral("save_ram"));
    QVERIFY(!mel->saveShortDir);
    QVERIFY(des->saveShortDir);  // core_file cores write their own file: short dir (Windows MAX_PATH)
    QVERIFY(des->saveShortDirSubfolder.isEmpty());
    QVERIFY(mel->saveShortDirSubfolder.isEmpty());
    QCOMPARE(mel->saveExtension, QStringLiteral(".sav"));
    QCOMPARE(mel->saveFormat, QStringLiteral("raw"));
    QCOMPARE(des->sysfileOption, QStringLiteral("desmume_use_external_bios"));
    QCOMPARE(des->sysfileNative, QStringLiteral("enabled"));
    QCOMPARE(des->sysfileBuiltin, QStringLiteral("disabled"));

    QCOMPARE(reg.profilesForSystem(QStringLiteral("nds")).size(), 2);
    QVERIFY(reg.profilesForSystem(QStringLiteral("xyz")).isEmpty());
  }

  void resolveProfiledCore() {
    ManifestRegistry reg;
    QVERIFY(reg.loadBuiltin());
    const auto m = reg.resolve(QStringLiteral("nds"), QStringLiteral("melondsds"));
    QVERIFY(m);
    QVERIFY(!m->experimental);
    QCOMPARE(m->coreId, QStringLiteral("melondsds"));
    QCOMPARE(m->coreLibraryBasename, QStringLiteral("melondsds_libretro"));
    QCOMPARE(m->firmware.sysfileOption, QStringLiteral("melonds_sysfile_mode"));
    QCOMPARE(m->firmware.fileById(QStringLiteral("firmware"))->coreOption, QStringLiteral("melonds_firmware_nds_path"));
    QVERIFY(m->firmware.fileById(QStringLiteral("bios7"))->coreOption.isEmpty());
    QCOMPARE(m->display.frameSize(), QSize(256, 384));
    // A legacy id keeps the id the Hub serves, the profile is the same.
    const auto legacy = reg.resolve(QStringLiteral("nds"), QStringLiteral("melonds_ds"));
    QVERIFY(legacy);
    QCOMPARE(legacy->coreId, QStringLiteral("melonds_ds"));
    QVERIFY(!legacy->experimental);
    QVERIFY(legacy->coreAliases.contains(QStringLiteral("melondsds")));
    QCOMPARE(legacy->coreOptions, m->coreOptions);
    // DeSmuME composes the same 256x384 frame.
    const auto d = reg.resolve(QStringLiteral("nds"), QStringLiteral("desmume"));
    QVERIFY(d);
    QCOMPARE(d->display.frameSize(), QSize(256, 384));
    QCOMPARE(d->firmware.sysfileOption, QStringLiteral("desmume_use_external_bios"));
    QVERIFY(!reg.resolve(QStringLiteral("xyz"), QStringLiteral("melondsds")));
  }

  void unknownCoreIsExperimental() {
    ManifestRegistry reg;
    QVERIFY(reg.loadBuiltin());
    const auto m = reg.resolve(QStringLiteral("nds"), QStringLiteral("noods"));
    QVERIFY(m);
    QVERIFY(m->experimental);
    QCOMPARE(m->coreId, QStringLiteral("noods"));
    QCOMPARE(m->coreLibraryBasename, QStringLiteral("noods_libretro"));
    QCOMPARE(m->saveSource, QStringLiteral("auto"));  // SAVE_RAM when the core reports it, else the single file it writes
    QVERIFY(m->coreOptions.isEmpty());
    QVERIFY(m->lockedCoreOptions.isEmpty());
    QVERIFY(m->alwaysShownCoreOptions.isEmpty());
    QVERIFY(m->firmware.sysfileOption.isEmpty());
    for (const FirmwareFile& f : m->firmware.files) QVERIFY(f.coreOption.isEmpty());
    QCOMPARE(m->display.layout, QStringLiteral("single"));
    QVERIFY(m->display.screens.isEmpty());  // raw framebuffer as one screen
    QCOMPARE(m->display.touchScreenIndex(), -1);  // touch off
    QCOMPARE(m->extensions, reg.find(QStringLiteral("nds"))->extensions);
    // A profiled core of another system is experimental for this one.
    ManifestRegistry reg2;
    CoreProfile p;
    p.coreId = QStringLiteral("other-core");
    p.libraryBasename = QStringLiteral("other_libretro");
    p.systemIds = {QStringLiteral("xyz")};
    QVERIFY(reg2.addProfile(p));
    QVERIFY(!reg2.addProfile(p));  // duplicate
  }

  void profileParsingIgnoresSourceAndRejectsInvalid() {
    QString err;
    const auto p = ManifestRegistry::parseProfile(
        R"({"_source":"x","core_id":"a-b","aliases":["a_b"],"library_basename":"a_b_libretro","system_ids":["nds"],
            "core_options":{"k":"v"},"firmware":{"sysfile_option":"o","file_options":{"firmware":"fo"}}})", &err);
    QVERIFY2(p, qPrintable(err));
    QCOMPARE(p->coreId, QStringLiteral("a-b"));
    QCOMPARE(p->sysfileNative, QStringLiteral("native"));
    QCOMPARE(p->fileOptions.value(QStringLiteral("firmware")), QStringLiteral("fo"));
    QVERIFY(!ManifestRegistry::parseProfile(R"({"core_id":"x","library_basename":"x","system_ids":["nds"],"save":{"source":"core_file","extension":"../x"}})", &err));
    QVERIFY(!ManifestRegistry::parseProfile(R"({"core_id":"x","library_basename":"x","system_ids":["nds"],"save":{"source":"bogus"}})", &err));
    // core_tree is valid only with format azahar_sdmc
    QVERIFY(!ManifestRegistry::parseProfile(R"({"core_id":"x","library_basename":"x","system_ids":["nds"],"save":{"source":"core_tree"}})", &err));
    QVERIFY(err.contains(QStringLiteral("azahar_sdmc")));
    QVERIFY(!ManifestRegistry::parseProfile(R"({"core_id":"x","library_basename":"x","system_ids":["nds"],"save":{"source":"core_tree","format":"desmume_dsv"}})", &err));
    const auto tree = ManifestRegistry::parseProfile(R"({"core_id":"x","library_basename":"x","system_ids":["nds"],"save":{"source":"core_tree","format":"azahar_sdmc"}})", &err);
    QVERIFY2(tree, qPrintable(err));
    QCOMPARE(tree->saveSource, QStringLiteral("core_tree"));
    const auto none = ManifestRegistry::parseProfile(R"({"core_id":"x","library_basename":"x","system_ids":["nds"],"save":{"source":"none"},"requires_hw_render":true})", &err);
    QVERIFY2(none, qPrintable(err));
    QCOMPARE(none->saveSource, QStringLiteral("none"));
    QVERIFY(none->requiresHwRender);
    QVERIFY(!ManifestRegistry::parseProfile("not json", &err));
    QVERIFY(!ManifestRegistry::parseProfile(R"({"core_id":"x","system_ids":["nds"]})", &err));
    QVERIFY(err.contains(QStringLiteral("library_basename")));
    QVERIFY(!ManifestRegistry::parseProfile(R"({"core_id":"x","library_basename":"x"})", &err));
  }

  void directoryMatchesBuiltin() {
    ManifestRegistry reg;
    QString err;
    QVERIFY2(reg.loadDirectory(QStringLiteral(FB_MANIFEST_SRC_DIR), &err), qPrintable(err));
    QVERIFY(reg.find(QStringLiteral("nds")));
    QVERIFY(reg.profile(QStringLiteral("desmume")));
    // Duplicate system_id is rejected.
    QVERIFY(!reg.loadDirectory(QStringLiteral(FB_MANIFEST_SRC_DIR), &err));
  }

  void rejectsInvalid() {
    QString err;
    QVERIFY(!ManifestRegistry::parse("not json", &err));
    QVERIFY(!ManifestRegistry::parse("{}", &err));
    QVERIFY(err.contains(QStringLiteral("system_id")));
    QVERIFY(!ManifestRegistry::parse(
        R"({"system_id":"x","display_name":"X","extensions":[],"display":{"screens":[{"width":1,"height":1}]}})", &err));
  }

  void coreLocator() {
    ManifestRegistry reg;
    QVERIFY(reg.loadBuiltin());
    const SystemManifest nds = *reg.resolve(QStringLiteral("nds"), QStringLiteral("melondsds"));
    const QString envName = CoreLocator::environmentVariableFor(nds.coreId);
    QCOMPARE(envName, QStringLiteral("UZUME_MELONDSDS_CORE"));
    QCOMPARE(CoreLocator::environmentVariableFor(QStringLiteral("a-b")), QStringLiteral("UZUME_A_B_CORE"));
    const QByteArray savedEnv = qgetenv(envName.toUtf8().constData());
    qunsetenv(envName.toUtf8().constData());
    const QByteArray savedLegacy = qgetenv("UZUME_MELONDS_DS_CORE");  // alias variable of the same core
    qunsetenv("UZUME_MELONDS_DS_CORE");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("cores")));
    auto touch = [](const QString& p) {
      QFile f(p);
      return f.open(QIODevice::WriteOnly) && f.write("dummy") > 0;
    };

    CoreLocator loc(dir.path());
    CoreLocation r = loc.locate(nds);
    QVERIFY(!r.found());
    QVERIFY(!r.tried.isEmpty());

    const QString appCore = dir.filePath(QStringLiteral("cores/") + CoreLocator::libraryFileName(nds.coreLibraryBasename));
    QVERIFY(touch(appCore));
    r = loc.locate(nds);
    QVERIFY(r.found());
    QCOMPARE(r.source, QStringLiteral("app-dir"));

    const QString envCore = dir.filePath(QStringLiteral("env_core.bin"));
    QVERIFY(touch(envCore));
    qputenv(envName.toUtf8().constData(), envCore.toUtf8());
    r = loc.locate(nds);
    QCOMPARE(r.source, QStringLiteral("env"));
    QCOMPARE(r.path, QDir::cleanPath(envCore));

    const QString explicitCore = dir.filePath(QStringLiteral("explicit_core.bin"));
    QVERIFY(touch(explicitCore));
    loc.setExplicitPath(nds.coreId, explicitCore);
    r = loc.locate(nds);
    QCOMPARE(r.source, QStringLiteral("explicit"));

    // An explicitly set but missing path falls back to env.
    loc.setExplicitPath(nds.coreId, dir.filePath(QStringLiteral("missing.bin")));
    QCOMPARE(loc.locate(nds).source, QStringLiteral("env"));

    // The legacy variable of the alias works for the same core.
    loc.setExplicitPath(nds.coreId, QString());
    qunsetenv(envName.toUtf8().constData());
    qputenv("UZUME_MELONDS_DS_CORE", envCore.toUtf8());
    QCOMPARE(loc.locate(nds).source, QStringLiteral("env"));
    qunsetenv("UZUME_MELONDS_DS_CORE");

    if (savedEnv.isEmpty()) qunsetenv(envName.toUtf8().constData());
    else qputenv(envName.toUtf8().constData(), savedEnv);
    if (!savedLegacy.isEmpty()) qputenv("UZUME_MELONDS_DS_CORE", savedLegacy);
  }
};

QTEST_GUILESS_MAIN(ManifestTest)
#include "manifest_test.moc"
