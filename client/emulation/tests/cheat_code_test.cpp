// Cheat codes before retro_cheat_set (issue #453): what an imported .cht (CRLF, quotes, disabled entry, empty code)
// turns into. A recording sink stands in for retro_cheat_reset / retro_cheat_set. Needs no core.
#include <QtTest>

#include "cheat_code.h"

using namespace uzume::emu;

namespace {
struct Entry {
  QString format;
  QString code;
  bool enabled = true;
};
struct Sink {
  int resets = 0;
  QList<std::tuple<unsigned, bool, QString>> sets;
  bool hasCheats = false;
  int send(const QList<Entry>& l) {
    return sendCheatList(l, &hasCheats, [&] { ++resets; }, [&](unsigned i, bool e, const QString& c) { sets.append({i, e, c}); });
  }
};
}  // namespace

class CheatCodeTest : public QObject {
  Q_OBJECT
 private slots:
  void cleanupOnly() {
    QCOMPARE(normalizeCheatCode("ar_ds", "0209f358   00000064"), QString("0209f358   00000064"));  // case and spaces untouched
    QCOMPARE(normalizeCheatCode("ar_ds", "\"0209F358 00000064\"\r\n"), QString("0209F358 00000064"));
    QCOMPARE(normalizeCheatCode("ar_ds", " 0209F358 00000064 \r\n\r\n0209F35C 00000001\r\n"), QString("0209F358 00000064\n0209F35C 00000001"));
    QCOMPARE(normalizeCheatCode("ar_ds", "0209F358 00000064+0209F35C 00000001"), QString("0209F358 00000064+0209F35C 00000001"));
    QCOMPARE(normalizeCheatCode("gsa", "AAAA+BBBB"), QString("AAAA+BBBB"));
    QCOMPARE(normalizeCheatCode("game_genie", " SLXPLOVS \r\n SLXPLOVS "), QString("SLXPLOVS\nSLXPLOVS"));
  }
  void emptyAndTooLong() {
    QVERIFY(normalizeCheatCode("ar_ds", " \r\n\t \"\" ").isEmpty());
    QVERIFY(normalizeCheatCode("ar_ds", QString(kMaxCheatCodeChars + 1, QLatin1Char('A'))).isEmpty());
    QVERIFY(normalizeCheatCode("ar_ds", QString::fromLatin1("0209F358 00000064\0 1", 20)).contains("00000064"));
  }
  // The .cht of the issue: one AR DS code, enable = false -> the core is not called at all.
  void disabledOnlyNeverReachesTheCore() {
    Sink s;
    QCOMPARE(s.send({{"ar_ds", "0209F358 00000064", false}}), 0);
    QCOMPARE(s.resets, 0);
    QVERIFY(s.sets.isEmpty());
  }
  void importedChtReachesTheCoreClean() {
    Sink s;
    const QList<Entry> l = {{"ar_ds", "\"0209F358 00000064\"\r\n", true},
                            {"ar_ds", "0209F35C 00000001", false},  // disabled: never passed as enabled
                            {"ar_ds", " \r\n ", true},              // empty: dropped
                            {"ar_ds", "02000000 00000001\r\n02000004 00000002\r\n", true}};
    QCOMPARE(s.send(l), 2);
    QCOMPARE(s.resets, 1);
    QCOMPARE(s.sets.size(), 2);
    QCOMPARE(std::get<0>(s.sets[0]), 0u);
    QVERIFY(std::get<1>(s.sets[0]));
    QCOMPARE(std::get<2>(s.sets[0]), QString("0209F358 00000064"));
    QCOMPARE(std::get<0>(s.sets[1]), 1u);  // consecutive after the dropped entries
    QCOMPARE(std::get<2>(s.sets[1]), QString("02000000 00000001\n02000004 00000002"));
    // Turning everything off afterwards still clears the core once, then stays quiet.
    QCOMPARE(s.send({}), 0);
    QCOMPARE(s.resets, 2);
    QCOMPARE(s.send({}), 0);
    QCOMPARE(s.resets, 2);
  }
};

QTEST_APPLESS_MAIN(CheatCodeTest)
#include "cheat_code_test.moc"
