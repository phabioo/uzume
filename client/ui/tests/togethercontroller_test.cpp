// TogetherController (0.23, ADR 0033 D3) without QML: scope choice, ready check transitions from Hub messages, and the shared pause that
// resumes only what it paused. The Hub socket is never opened; the messages are emitted directly.
#include <QSignalSpy>
#include <QtTest>

#include "togethercontroller.h"

using namespace uzume;
using namespace uzume::ui;
using Scope = TogetherController::Scope;
using Inputs = TogetherController::ScopeInputs;

namespace {
ReadyCheckMessage check(const QString& status, const QString& id = QStringLiteral("r1"), const QString& by = QStringLiteral("u_lena"),
                        const QString& lena = QStringLiteral("pending"), const QString& max = QStringLiteral("pending")) {
  return {QStringLiteral("c1"), QStringLiteral("run"), id, by, status, 0, {{QStringLiteral("u_lena"), lena}, {QStringLiteral("u_max"), max}}};
}
PauseStateMessage pauseState(bool paused, const QString& by = QStringLiteral("u_lena"), const QString& id = QStringLiteral("r1")) {
  return {QStringLiteral("run"), id, paused, by};
}
}  // namespace

class TogetherControllerTest : public QObject {
  Q_OBJECT

  struct Fixture {
    HubSocket socket{nullptr};
    TogetherController tc{&socket};
    Scope scope{QStringLiteral("run"), QStringLiteral("r1"), true};
    bool gamePaused = false;
    int pauses = 0, resumes = 0;
    Fixture() {
      tc.setFeatureCheck([] { return true; });
      tc.setSelf([] { return QStringLiteral("u_max"); });
      tc.setNameResolver([](const QString& id) { return id == QLatin1String("u_lena") ? QStringLiteral("Lena") : id == QLatin1String("u_max") ? QStringLiteral("Max") : QString(); });
      tc.setScopeProvider([this] { return scope; });
      tc.setGameHooks({[this] { return gamePaused; },
                       [this] { gamePaused = true; ++pauses; },
                       [this] { gamePaused = false; ++resumes; }});
      tc.refresh();
    }
  };

 private slots:
  void scopeChoiceOrder() {
    Inputs in;
    QVERIFY(TogetherController::pickScope(in).kind.isEmpty());
    in.watched = {QStringLiteral("s9"), QStringLiteral("s8")};
    in.focus = QStringLiteral("s8");
    Scope s = TogetherController::pickScope(in);
    QCOMPARE(s.kind, QStringLiteral("session"));
    QCOMPARE(s.id, QStringLiteral("s8"));  // the focused Session of several
    QVERIFY(!s.hostsGame);
    in.focus = QStringLiteral("local");
    QCOMPARE(TogetherController::pickScope(in).id, QStringLiteral("s9"));
    in.ownSessionId = QStringLiteral("own1");  // my shared Session beats a watched one
    s = TogetherController::pickScope(in);
    QCOMPARE(s.id, QStringLiteral("own1"));
    QVERIFY(s.hostsGame);
    in.gameRunId = QStringLiteral("r1");  // the run of the game beats both, but only while the game runs
    QCOMPARE(TogetherController::pickScope(in).id, QStringLiteral("own1"));
    in.gameActive = true;
    s = TogetherController::pickScope(in);
    QCOMPARE(s.kind, QStringLiteral("run"));
    QCOMPARE(s.id, QStringLiteral("r1"));
    QVERIFY(s.hostsGame);
    in.boardRunId = QStringLiteral("r2");  // the open run board is the explicit context; it hosts my game only if it is my run
    s = TogetherController::pickScope(in);
    QCOMPARE(s.id, QStringLiteral("r2"));
    QVERIFY(!s.hostsGame);
    in.boardRunId = QStringLiteral("r1");
    QVERIFY(TogetherController::pickScope(in).hostsGame);
  }

  void availabilityFollowsScopeAndFeature() {
    Fixture f;
    QVERIFY(f.tc.available());
    f.tc.setFeatureCheck([] { return false; });
    QVERIFY(!f.tc.available());
    f.tc.setFeatureCheck([] { return true; });
    f.scope = {};
    QSignalSpy spy(&f.tc, &TogetherController::scopeChanged);
    f.tc.refresh();
    QVERIFY(!f.tc.available());
    QCOMPARE(spy.size(), 1);
  }

  void checkOpensAnswersAndCloses() {
    Fixture f;
    QSignalSpy changed(&f.tc, &TogetherController::checkChanged);
    f.socket.readyCheckReceived(check(QStringLiteral("open")));
    QVERIFY(f.tc.checkOpen());
    QCOMPARE(f.tc.checkTitle(), QStringLiteral("Ready check · Lena"));
    QCOMPARE(f.tc.myState(), QStringLiteral("pending"));
    QVERIFY(!f.tc.iStarted());
    QVERIFY(f.tc.awaitsAnswer());
    QVERIFY(f.tc.secondsLeft() > 0 && f.tc.secondsLeft() <= 30);
    const QVariantList members = f.tc.checkMembers();
    QCOMPARE(members.size(), 2);
    QCOMPARE(members[0].toMap().value("stateText").toString(), QStringLiteral("Waiting"));
    QVERIFY(members[1].toMap().value("me").toBool());
    f.tc.answer(true);  // not connected: nothing is sent, the card follows my answer at once
    QCOMPARE(f.tc.myState(), QStringLiteral("ready"));
    QVERIFY(!f.tc.awaitsAnswer());
    f.socket.readyCheckReceived(check(QStringLiteral("open"), QStringLiteral("r1"), QStringLiteral("u_lena"), QStringLiteral("not_ready"), QStringLiteral("ready")));
    QCOMPARE(f.tc.checkMembers()[0].toMap().value("stateText").toString(), QStringLiteral("Not yet"));
    QCOMPARE(f.tc.checkMembers()[1].toMap().value("stateText").toString(), QStringLiteral("Ready"));
    f.socket.readyCheckReceived(check(QStringLiteral("all_ready"), QStringLiteral("r1"), QStringLiteral("u_lena"), QStringLiteral("ready"), QStringLiteral("ready")));
    QVERIFY(!f.tc.checkOpen());
    QCOMPARE(f.tc.notice(), QStringLiteral("Everyone is ready"));
    QVERIFY(changed.size() >= 3);
  }

  void starterSeesCancelAndResultsCloseTheCard() {
    Fixture f;
    f.socket.readyCheckReceived(check(QStringLiteral("open"), QStringLiteral("r1"), QStringLiteral("u_max")));
    QVERIFY(f.tc.iStarted());
    QCOMPARE(f.tc.checkTitle(), QStringLiteral("Ready check · Max"));
    f.socket.readyCheckReceived(check(QStringLiteral("cancelled"), QStringLiteral("r1"), QStringLiteral("u_max")));
    QVERIFY(!f.tc.checkOpen());
    QCOMPARE(f.tc.notice(), QStringLiteral("Ready check cancelled"));
    f.socket.readyCheckReceived(check(QStringLiteral("open")));
    f.socket.readyCheckReceived(check(QStringLiteral("expired")));
    QVERIFY(!f.tc.checkOpen());
    QCOMPARE(f.tc.notice(), QStringLiteral("Ready check expired"));
  }

  void noticeGoesAfterThreeSeconds() {
    Fixture f;
    qint64 t = 1000;
    f.tc.setClock([&t] { return t; });
    f.socket.readyCheckReceived(check(QStringLiteral("open")));
    f.socket.readyCheckReceived(check(QStringLiteral("all_ready")));
    QVERIFY(!f.tc.notice().isEmpty());
    t += 2900;
    f.tc.tick();
    QVERIFY(!f.tc.notice().isEmpty());
    t += 200;
    f.tc.tick();
    QVERIFY(f.tc.notice().isEmpty());
  }

  void countdownAndLostFinalMessage() {
    Fixture f;
    qint64 t = 1000;
    f.tc.setClock([&t] { return t; });
    f.socket.readyCheckReceived(check(QStringLiteral("open")));
    QCOMPARE(f.tc.secondsLeft(), 30);
    t += 10000;
    f.tc.tick();
    QCOMPARE(f.tc.secondsLeft(), 20);
    t += 22500;  // the Hub's final message never came
    f.tc.tick();
    QVERIFY(!f.tc.checkOpen());
  }

  void otherScopesAreIgnored() {
    Fixture f;
    f.socket.readyCheckReceived(check(QStringLiteral("open"), QStringLiteral("other-run")));
    QVERIFY(!f.tc.checkOpen());
    f.socket.pauseStateReceived(pauseState(true, QStringLiteral("u_lena"), QStringLiteral("other-run")));
    QVERIFY(!f.tc.paused());
    QCOMPARE(f.pauses, 0);
  }

  void scopeLossDropsTheCheck() {
    Fixture f;
    f.socket.readyCheckReceived(check(QStringLiteral("open")));
    f.scope = {QStringLiteral("run"), QStringLiteral("r2"), true};
    f.tc.refresh();
    QVERIFY(!f.tc.checkOpen());
  }

  void sharedPausePausesAndResumesOnlyWhatItPaused() {
    Fixture f;
    f.socket.pauseStateReceived(pauseState(true));
    QVERIFY(f.tc.paused());
    QVERIFY(f.tc.holdActive());
    QVERIFY(f.gamePaused);
    QCOMPARE(f.tc.pausedText(), QStringLiteral("Lena paused for everyone"));
    f.socket.pauseStateReceived(pauseState(false, QString()));
    QVERIFY(!f.tc.paused());
    QVERIFY(!f.gamePaused);
    QCOMPARE(f.resumes, 1);
    // A game the person paused themselves stays paused when the shared pause ends
    f.gamePaused = true;
    f.socket.pauseStateReceived(pauseState(true));
    QVERIFY(!f.tc.holdActive());
    QCOMPARE(f.pauses, 1);  // no second pause call
    f.socket.pauseStateReceived(pauseState(false, QString()));
    QVERIFY(f.gamePaused);
    QCOMPARE(f.resumes, 1);
  }

  void gameStartingInAPausedScopeIsPaused() {
    Fixture f;
    f.scope.hostsGame = false;  // watching the run: no game of mine yet
    f.tc.refresh();
    f.socket.pauseStateReceived(pauseState(true));
    QCOMPARE(f.pauses, 0);
    f.scope.hostsGame = true;  // my game starts inside the same run
    f.tc.refresh();
    QCOMPARE(f.pauses, 1);
    QVERIFY(f.gamePaused);
    f.socket.pauseStateReceived(pauseState(false, QString()));
    QVERIFY(!f.gamePaused);
  }

  void ownPauseTextAndManualResumeReleasesTheHold() {
    Fixture f;
    f.socket.pauseStateReceived(pauseState(true, QStringLiteral("u_max")));
    QCOMPARE(f.tc.pausedText(), QStringLiteral("You paused for everyone"));
    f.gamePaused = false;  // resumed by hand while the shared pause is on
    f.tc.noteLocalPauseChanged();
    QVERIFY(!f.tc.holdActive());
    QVERIFY(f.tc.paused());
    f.gamePaused = true;  // paused locally again
    f.socket.pauseStateReceived(pauseState(false, QString()));
    QVERIFY(f.gamePaused);  // the shared pause did not own it any more
  }

  void hostPauseNameIsTheRequesterOnlyWhileHeld() {
    Fixture f;
    QCOMPARE(f.tc.hostPauseName(), QString());
    f.socket.pauseStateReceived(pauseState(true, QStringLiteral("u_lena")));
    QCOMPARE(f.tc.hostPauseName(), QStringLiteral("Lena"));  // viewers read "Paused by Lena"
    f.socket.pauseStateReceived(pauseState(false, QString()));
    QCOMPARE(f.tc.hostPauseName(), QString());  // a normal pause shows the host again
    f.gamePaused = true;  // paused by hand: the shared pause holds nothing
    f.socket.pauseStateReceived(pauseState(true, QStringLiteral("u_lena")));
    QCOMPARE(f.tc.hostPauseName(), QString());
  }

  void viewerDoesNotPauseItsGame() {
    Fixture f;
    f.scope = {QStringLiteral("session"), QStringLiteral("s1"), false};
    f.tc.refresh();
    f.socket.pauseStateReceived({QStringLiteral("session"), QStringLiteral("s1"), true, QStringLiteral("u_lena")});
    QVERIFY(f.tc.paused());
    QVERIFY(!f.tc.holdActive());
    QCOMPARE(f.pauses, 0);
  }

  void leavingTheScopeReleasesTheHold() {
    Fixture f;
    f.socket.pauseStateReceived(pauseState(true));
    QVERIFY(f.gamePaused);
    f.scope = {};
    f.tc.refresh();
    QVERIFY(!f.tc.paused());
    QVERIFY(!f.gamePaused);  // a game never stays stopped by a pause nobody can end
  }
};

QTEST_GUILESS_MAIN(TogetherControllerTest)
#include "togethercontroller_test.moc"
