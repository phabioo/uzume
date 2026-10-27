// Core run rules in the UI (0.20 package 6): the draft map of the Start and Rules dialogs, the one-line summary, the log text of
// rules.changed and the warning line of the Rules dialog. Pure logic, no window.
#include <QJsonObject>
#include <QtTest>

#include "runrules.h"

using namespace uzume;
using namespace uzume::ui;

class RunRulesTest : public QObject {
  Q_OBJECT
 private slots:
  void defaultsAreEverythingOff() {
    const RunRules r = runrules::fromMap({});
    QVERIFY(!r.states);
    QVERIFY(!r.rewind);
    QCOMPARE(r.cheats, QStringLiteral("off"));
    QCOMPARE(r.memoryWrite, QStringLiteral("off"));
    const QVariantMap m = runrules::toMap(r);
    QCOMPARE(m.value("states").toBool(), false);
    QCOMPARE(m.value("cheats").toString(), QStringLiteral("off"));
    QCOMPARE(m.value("memoryWrite").toString(), QStringLiteral("off"));
  }

  void setRuleAcceptsOnlyKnownKeysAndLevels() {
    RunRules r;
    QVERIFY(runrules::setRule(r, "states", true));
    QVERIFY(r.states);
    QVERIFY(runrules::setRule(r, "rewind", true));
    QVERIFY(runrules::setRule(r, "cheats", "logged"));
    QVERIFY(r.cheatsAllowed());
    QVERIFY(runrules::setRule(r, "memoryWrite", "logged"));
    QVERIFY(r.memoryWriteAllowed());
    QVERIFY(runrules::setRule(r, "memory_write", "off"));  // the wire name works too
    QVERIFY(!r.memoryWriteAllowed());
    QVERIFY(runrules::setRule(r, "cheats", false));  // a bool counts as off / logged
    QCOMPARE(r.cheats, QStringLiteral("off"));
    const RunRules before = r;
    QVERIFY(!runrules::setRule(r, "cheats", "allowed"));  // unknown level
    QVERIFY(!runrules::setRule(r, "cheat", "logged"));    // unknown key
    QVERIFY(r == before);
  }

  void mapRoundTripAndWireJson() {
    RunRules r;
    r.states = true;
    r.cheats = QStringLiteral("logged");
    QVERIFY(runrules::fromMap(runrules::toMap(r)) == r);
    const QJsonObject j = runRulesToJson(r);  // what PUT /runs/{id}/rules sends: the whole object
    QCOMPARE(j.size(), 4);
    QCOMPARE(j.value("states").toBool(), true);
    QCOMPARE(j.value("rewind").toBool(), false);
    QCOMPARE(j.value("cheats").toString(), QStringLiteral("logged"));
    QCOMPARE(j.value("memory_write").toString(), QStringLiteral("off"));
  }

  void summaryLineNamesEveryRule() {
    RunRules r;
    QCOMPARE(runrules::summaryLine(r), QStringLiteral("Save states off · Rewind off · Cheats off · Memory changes off"));
    r.states = true;
    r.cheats = QStringLiteral("logged");
    QCOMPARE(runrules::summaryLine(r), QStringLiteral("Save states on · Rewind off · Cheats allowed, logged · Memory changes off"));
  }

  void changeTextListsOnlyWhatChanged() {
    RunRules a, b;
    QCOMPARE(runrules::changeText(a, b), QString());
    b.cheats = QStringLiteral("logged");
    QCOMPARE(runrules::changeText(a, b), QStringLiteral("Cheats: Off → Allowed, logged"));
    b.states = true;
    QCOMPARE(runrules::changeText(a, b), QStringLiteral("Save states: Off → On; Cheats: Off → Allowed, logged"));
  }

  void changeEventTextFromThePayload() {
    const QJsonObject before{{"states", false}, {"rewind", false}, {"cheats", "off"}, {"memory_write", "off"}};
    QJsonObject after = before;
    after.insert("cheats", "logged");
    QCOMPARE(runrules::changeEventText("Lena", QJsonObject{{"before", before}, {"after", after}}), QStringLiteral("Lena changed Cheats: Off → Allowed, logged"));
    QCOMPARE(runrules::changeEventText("Lena", QJsonObject{{"before", before}, {"after", before}}), QStringLiteral("Lena changed the run rules"));
    QCOMPARE(runrules::changeEventText("Lena", QJsonObject()), QStringLiteral("Lena changed the run rules"));
  }

  void draftWarningSaysWhatHappensAtOnce() {
    RunRules saved, draft;
    QCOMPARE(runrules::draftWarning(saved, draft), QString());
    draft.cheats = QStringLiteral("logged");  // allowing: everybody sees it in the log
    QVERIFY(runrules::draftWarning(saved, draft).contains(QStringLiteral("run log")));
    saved.cheats = QStringLiteral("logged");
    draft = saved;
    QCOMPARE(runrules::draftWarning(saved, draft), QString());
    draft.cheats = QStringLiteral("off");  // turning off: active cheats go off at once
    QVERIFY(runrules::draftWarning(saved, draft).contains(QStringLiteral("turn off")));
    saved.memoryWrite = QStringLiteral("logged");
    QVERIFY(runrules::draftWarning(saved, draft).contains(QStringLiteral("memory")));
    draft.states = true;  // a switch alone says nothing
    RunRules s2, d2;
    d2.states = true;
    QCOMPARE(runrules::draftWarning(s2, d2), QString());
  }
};

QTEST_MAIN(RunRulesTest)
#include "run_rules_test.moc"
