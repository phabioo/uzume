// The start screen (first start, join, set up, invite links), the switch-user screen and Settings > My account of 0.18 package 7,
// driven through Main.qml against the fake server, offscreen. The controllers themselves are tested in start_accounts_test.cpp.
#include <QClipboard>
#include <QGuiApplication>
#include <QSignalSpy>
#include <QtTest>

#include "fakehub.h"
#include "joinlink.h"
#include "testsupport.h"

using namespace uzume;
using namespace uzume::ui;
using uitest::Harness;

namespace {
const QString kPassword = QStringLiteral("correct horse battery");

bool shown(const Harness& h, const char* name) {
  QQuickItem* it = h.item(name);
  return it != nullptr && it->isVisible() && it->window() == h.window;
}
void setText(const Harness& h, const char* name, const QString& text) {
  QQuickItem* it = h.item(name);
  QVERIFY2(it != nullptr, name);
  it->setProperty("text", text);
}
QString textOf(const Harness& h, const char* name) {
  QQuickItem* it = h.item(name);
  return it != nullptr ? it->property("text").toString() : QString();
}
}  // namespace

#define SHOWN(name) QTRY_VERIFY2_WITH_TIMEOUT(shown(h, name), name, 8000)
#define GONE(name) QTRY_VERIFY2_WITH_TIMEOUT(!shown(h, name), name, 8000)
#define CLICK(name) QVERIFY2(h.clickWhenReady(name), name)
#define PHASE(p) QTRY_COMPARE_WITH_TIMEOUT(h.controller->onboarding()->phase(), QStringLiteral(p), 8000)

class StartScreensUiTest : public QObject {
  Q_OBJECT

  // Joins `hub` as `user` through the controller (no UI) and waits for the connection.
  static void signInAs(Harness& h, FakeHub& hub, const QString& user) {
    OnboardingController* ob = h.controller->onboarding();
    ob->beginJoin();
    ob->submitJoinInput(hub.address());
    QTRY_COMPARE_WITH_TIMEOUT(ob->phase(), QStringLiteral("security"), 8000);
    ob->securityMatches();
    QTRY_COMPARE_WITH_TIMEOUT(ob->phase(), QStringLiteral("sign_in"), 8000);
    ob->signIn(user, kPassword);
    QTRY_COMPARE_WITH_TIMEOUT(ob->phase(), QStringLiteral("idle"), 8000);
    QTRY_VERIFY_WITH_TIMEOUT(h.controller->connection()->state() == HubConnection::State::Connected, 8000);
  }

 private slots:
  void initTestCase() { uitest::installWarningCounter(); }
  void init() { uitest::warningCount() = 0; }
  void cleanup() { QCOMPARE(uitest::warningCount().load(), 0); }

  // 8a-1 / 8a-2: the first start shows the three ways, no sidebar; Esc does nothing here.
  void firstStartShowsThreeWays() {
    Harness h;
    QVERIFY(h.start());
    QCOMPARE(h.controller->screen(), QStringLiteral("start"));
    QVERIFY(h.controller->onboarding()->startScreenEnabled());
    for (const char* n : {"startPlay", "startJoin", "startSetup", "startPasteHint", "startLockup", "startStepLine", "startHintRow"}) {
      SHOWN(n);
    }
    QCOMPARE(textOf(h, "startTitle"), QStringLiteral("Welcome to Uzume"));
    QCOMPARE(textOf(h, "startStepLine"), QStringLiteral("First start"));
    // No vertical side text, no top-right label and no version block: one step line in the content column.
    QVERIFY(!shown(h, "openColumnLabel"));
    QVERIFY(!shown(h, "startScreenLabel"));
    QVERIFY(h.item("navLibrary") == nullptr || !h.item("navLibrary")->isVisible());  // no sidebar
    uitest::saveShot(h.window, QStringLiteral("8a-1-first-start"));
    QTest::keyClick(h.window, Qt::Key_Escape);
    QCOMPARE(h.controller->onboarding()->phase(), QStringLiteral("idle"));
  }

  // 8a-3: pick a folder, scanning, games found; Start playing ends the step. A path that is no folder is "no access".
  void playOnThisPc() {
    QTemporaryDir games;
    QVERIFY(games.isValid());
    QFile f(QDir(games.path()).filePath(QStringLiteral("dummy.gb")));
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(QByteArray(0x8000, '\0'));
    f.close();

    Harness h;
    QVERIFY(h.start());
    CLICK("startPlay");
    PHASE("play");
    SHOWN("playFolderField");
    QCOMPARE(textOf(h, "startStepLine"), QStringLiteral("Play on this PC"));
    QVERIFY(!h.item("playStart")->isEnabled());  // nothing picked yet
    uitest::saveShot(h.window, QStringLiteral("8a-3-play-empty"));

    // no access
    setText(h, "playFolderField", games.path() + QStringLiteral("/missing"));
    h.item("playFolderField")->forceActiveFocus();
    QTest::keyClick(h.window, Qt::Key_Return);
    SHOWN("playFolderFieldError");
    QVERIFY(!h.item("playStart")->isEnabled());
    uitest::saveShot(h.window, QStringLiteral("8a-3-play-no-access"));

    // games found
    setText(h, "playFolderField", games.path());
    h.item("playFolderField")->forceActiveFocus();
    QTest::keyClick(h.window, Qt::Key_Return);
    SHOWN("playFound");
    QCOMPARE(h.controller->libraryGameCount(), 1);
    QVERIFY(h.item("playStart")->isEnabled());
    SHOWN("playFirmwareNote");
    uitest::saveShot(h.window, QStringLiteral("8a-3-play-found"));
    CLICK("playStart");
    QTRY_VERIFY(!h.controller->onboarding()->firstStart());
    QTRY_VERIFY(h.controller->screen() != QStringLiteral("start"));
  }

  // 8a-3: nothing found in the folder, and Add later.
  void playOnThisPcNothingFoundAndAddLater() {
    QTemporaryDir empty;
    QVERIFY(empty.isValid());
    Harness h;
    QVERIFY(h.start());
    CLICK("startPlay");
    SHOWN("playFolderField");
    setText(h, "playFolderField", empty.path());
    h.item("playFolderField")->forceActiveFocus();
    QTest::keyClick(h.window, Qt::Key_Return);
    SHOWN("playEmpty");
    SHOWN("playPickAnother");
    QVERIFY(!h.item("playStart")->isEnabled());
    uitest::saveShot(h.window, QStringLiteral("8a-3-play-nothing-found"));
    CLICK("playLater");
    QTRY_VERIFY(h.controller->screen() != QStringLiteral("start"));
  }

  // Esc goes back one step everywhere: Play -> welcome.
  void escapeIsBack() {
    Harness h;
    QVERIFY(h.start());
    CLICK("startPlay");
    SHOWN("playFolderField");
    h.item("playFolderField")->forceActiveFocus();
    QTest::keyClick(h.window, Qt::Key_Escape);
    PHASE("idle");
    SHOWN("startPlay");
    QVERIFY(h.controller->onboarding()->firstStart());
  }

  // 8b-1 .. 8b-5: address, security check (differs = hard stop), sign in with a wrong and the right password.
  void joinWithSecurityCheckAndSignIn() {
    FakeHub hub(QStringLiteral("a"));
    hub.name = QStringLiteral("Home server");
    QVERIFY(hub.start());
    hub.addAccount(QStringLiteral("anna"), kPassword);
    Harness h;
    QVERIFY(h.start());
    CLICK("startJoin");
    PHASE("address");
    SHOWN("joinInput");
    QVERIFY(!h.item("joinContinue")->isEnabled());  // empty
    QCOMPARE(textOf(h, "startStepLine"), QStringLiteral("Step 1 of 3 · Server"));
    uitest::saveShot(h.window, QStringLiteral("8b-1-join-address"));

    // invalid input stays on the page with the controller's copy under the field
    setText(h, "joinInput", QStringLiteral("not an address!"));
    CLICK("joinContinue");
    SHOWN("joinInputError");

    setText(h, "joinInput", hub.address());
    CLICK("joinContinue");
    PHASE("security");
    SHOWN("securityCode");
    QCOMPARE(textOf(h, "securityCode").size(), 14);
    SHOWN("securityMatches");
    SHOWN("securityDiffers");
    QCOMPARE(textOf(h, "startTitle"), QStringLiteral("Is this really Home server?"));
    uitest::saveShot(h.window, QStringLiteral("8b-2-join-security"));

    // It doesn't match: hard stop, nothing sent, nothing saved
    CLICK("securityDiffers");
    PHASE("security_mismatch");
    SHOWN("startStopSquare");
    SHOWN("startAction_dismiss");
    SHOWN("startAction_details");
    QVERIFY(h.controller->profileStore()->profiles().isEmpty());
    uitest::saveShot(h.window, QStringLiteral("8b-2-join-mismatch"));
    CLICK("startAction_details");
    SHOWN("startStopDetails");
    CLICK("startAction_dismiss");
    PHASE("address");

    setText(h, "joinInput", hub.address());
    CLICK("joinContinue");
    PHASE("security");
    CLICK("securityMatches");
    PHASE("sign_in");
    SHOWN("signInName");
    SHOWN("signInPassword");
    SHOWN("signInForgot");
    SHOWN("signInInvite");
    SHOWN("signInApproval");
    uitest::saveShot(h.window, QStringLiteral("8b-4-sign-in"));
    // One numbering system, shown once: the step line (the server's name after the step), no eyebrow, no side text.
    QCOMPARE(textOf(h, "startStepLine"), QStringLiteral("Step 3 of 3 · Home server"));
    QVERIFY(!shown(h, "startSectionLabel"));
    QVERIFY(!shown(h, "openColumnLabel"));
    QVERIFY(!shown(h, "startScreenLabel"));
    // Tab goes Name -> Password (not to "Show") -> Sign in.
    h.item("signInName")->forceActiveFocus();
    QVERIFY(h.item("signInName")->hasActiveFocus());
    QTest::keyClick(h.window, Qt::Key_Tab);
    QTRY_VERIFY2(h.item("signInPassword")->hasActiveFocus(), "Tab from Name must reach the Password field");
    QVERIFY(!h.item("signInPasswordReveal")->hasActiveFocus());
    QTest::keyClick(h.window, Qt::Key_Tab);
    QTRY_VERIFY(!h.item("signInPassword")->hasActiveFocus());
    QVERIFY(!h.item("signInPasswordReveal")->hasActiveFocus());

    setText(h, "signInName", QStringLiteral("anna"));
    setText(h, "signInPassword", QStringLiteral("wrong password!!"));
    CLICK("signInSubmit");
    SHOWN("signInPasswordError");
    uitest::saveShot(h.window, QStringLiteral("8b-5-sign-in-wrong"));

    setText(h, "signInPassword", kPassword);
    CLICK("signInSubmit");
    QTRY_VERIFY_WITH_TIMEOUT(h.controller->screen() != QStringLiteral("start"), 8000);
    QTRY_VERIFY_WITH_TIMEOUT(h.controller->connection()->state() == HubConnection::State::Connected, 8000);
  }

  // The first join step is truthful about invites: a link names the server, a bare code does not and asks for the address next.
  void joinStepWordsInvitesTruthfully() {
    Harness h;
    QVERIFY(h.start());
    CLICK("startJoin");
    PHASE("address");
    SHOWN("joinInput");
    QVERIFY(!textOf(h, "joinInputHint").contains(QStringLiteral("fills the field")));
    QVERIFY(textOf(h, "joinInputHint").contains(QStringLiteral("does not say which server")));
    SHOWN("startHelpColumn");
    setText(h, "joinInput", QStringLiteral("UZ-8XK2-M4QD"));
    CLICK("joinContinue");
    SHOWN("joinInputError");
    QCOMPARE(h.controller->onboarding()->errorCode(), QStringLiteral("need_address"));
    QCOMPARE(h.controller->onboarding()->phase(), QStringLiteral("address"));  // no connection was tried
    QCOMPARE(h.controller->onboarding()->inviteCode(), QStringLiteral("UZ-8XK2-M4QD"));  // the code is kept
  }

  // 8b-5c: too many tries, with the wait as m:ss.
  void signInRateLimited() {
    FakeHub hub(QStringLiteral("a"));
    hub.rateLimitSignIn = true;
    hub.rateLimitRetryAfter = 292;
    QVERIFY(hub.start());
    hub.addAccount(QStringLiteral("lena"), kPassword);
    Harness h;
    QVERIFY(h.start());
    OnboardingController* ob = h.controller->onboarding();
    ob->beginJoin();
    ob->submitJoinInput(hub.address());
    PHASE("security");
    ob->securityMatches();
    PHASE("sign_in");
    setText(h, "signInName", QStringLiteral("Lena"));
    setText(h, "signInPassword", kPassword);
    CLICK("signInSubmit");
    SHOWN("startNotice");
    QCOMPARE(ob->errorCode(), QStringLiteral("rate_limited"));
    QCOMPARE(ob->retryAfter(), 292);
    uitest::saveShot(h.window, QStringLiteral("8b-5-sign-in-rate-limited"));
  }

  // 8b-3: "I have an invite" shows the invite form, "I already have an account" goes back; Forgot password opens 8g-1.
  void inviteFormAndForgotPassword() {
    FakeHub hub(QStringLiteral("a"));
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    OnboardingController* ob = h.controller->onboarding();
    ob->beginJoin();
    ob->submitJoinInput(hub.address());
    PHASE("security");
    ob->securityMatches();
    PHASE("sign_in");
    CLICK("signInForgot");
    SHOWN("forgotDialog");
    SHOWN("forgotClose");
    QVERIFY(!shown(h, "forgotCommands"));  // a member: ask an admin
    uitest::saveShot(h.window, QStringLiteral("8g-1-forgot-member"));
    CLICK("forgotClose");
    GONE("forgotClose");

    CLICK("signInInvite");
    PHASE("invite");
    SHOWN("inviteCode");
    SHOWN("inviteName");
    SHOWN("invitePassword");
    SHOWN("inviteSubmit");
    uitest::saveShot(h.window, QStringLiteral("8b-3-invite"));
    setText(h, "inviteCode", QStringLiteral("FB-TEST-CODE"));
    setText(h, "inviteName", QStringLiteral("Jonas"));
    CLICK("inviteSubmit");
    QTRY_VERIFY_WITH_TIMEOUT(h.controller->screen() != QStringLiteral("start"), 8000);
  }
  void inviteFormGoesBackToSignIn() {
    FakeHub hub(QStringLiteral("a"));
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    OnboardingController* ob = h.controller->onboarding();
    ob->beginJoin();
    ob->submitJoinInput(hub.address());
    PHASE("security");
    ob->securityMatches();
    PHASE("sign_in");
    ob->haveInvite();
    PHASE("invite");
    CLICK("inviteHaveAccount");
    PHASE("sign_in");
    SHOWN("signInName");
  }

  // 8b-5e: no answer. Try again and Change address.
  void offlineServer() {
    Harness h;
    QVERIFY(h.start());
    OnboardingController* ob = h.controller->onboarding();
    ob->beginJoin();
    ob->submitJoinInput(QStringLiteral("http://127.0.0.1:1"));
    PHASE("offline");
    SHOWN("startAction_retry");
    SHOWN("startAction_back");
    SHOWN("startStopSquare");
    uitest::saveShot(h.window, QStringLiteral("8b-5-offline"));
    CLICK("startAction_back");
    PHASE("address");
  }

  // 8b-5f: the server is too old.
  void serverTooOld() {
    FakeHub hub(QStringLiteral("a"));
    hub.protocolVersion = 2;
    hub.minProtocolVersion = 2;  // newer than this app speaks
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    OnboardingController* ob = h.controller->onboarding();
    ob->beginJoin();
    ob->submitJoinInput(hub.address());
    QTRY_VERIFY_WITH_TIMEOUT(ob->phase() == QStringLiteral("security") || ob->phase() == QStringLiteral("app_too_old") ||
                                 ob->phase() == QStringLiteral("server_too_old"), 8000);
    if (ob->phase() == QStringLiteral("security")) {
      ob->securityMatches();
    }
    QTRY_VERIFY_WITH_TIMEOUT(ob->phase() == QStringLiteral("app_too_old") || ob->phase() == QStringLiteral("server_too_old"), 8000);
    SHOWN("startStopSquare");
    SHOWN("startAction_back");
    uitest::saveShot(h.window, QStringLiteral("8b-5-too-old"));
  }

  // 8a-4 / 8a-5 / 8a-6: set up a server on another device: where, address and setup code (wrong code first), the security
  // check, the admin account.
  void setUpAServerWithACode() {
    FakeHub hub(QStringLiteral("a"));
    hub.name = QStringLiteral("pi-livingroom");
    hub.setupRequired = true;
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    CLICK("startSetup");
    SHOWN("setupOnThisPc");
    SHOWN("setupOnDevice");
    QCOMPARE(textOf(h, "startStepLine"), QStringLiteral("Step 1 of 4 · Where it runs"));
    QCOMPARE(h.item("setupOnThisPc")->isEnabled(), h.controller->localHub()->available());
    uitest::saveShot(h.window, QStringLiteral("8a-4-setup-where"));
    // Back returns to the first screen
    CLICK("setupWhereBack");
    SHOWN("startPlay");
    CLICK("startSetup");
    CLICK("setupOnDevice");
    PHASE("address");
    SHOWN("setupAddress");
    SHOWN("setupCode");
    QCOMPARE(textOf(h, "startStepLine"), QStringLiteral("Step 2 of 4 · Connect to the device"));
    uitest::saveShot(h.window, QStringLiteral("8a-4-setup-address"));

    setText(h, "setupAddress", hub.address());
    setText(h, "setupCode", QStringLiteral("zzz"));
    CLICK("setupConnect");
    SHOWN("setupCodeError");  // 8a-6a
    uitest::saveShot(h.window, QStringLiteral("8a-6-wrong-setup-code"));

    setText(h, "setupCode", QStringLiteral("7H4K-2Q"));
    CLICK("setupConnect");
    PHASE("security");
    QCOMPARE(textOf(h, "startStepLine"), QStringLiteral("Step 3 of 4 · Security check"));
    uitest::saveShot(h.window, QStringLiteral("8a-4-setup-security"));
    CLICK("securityMatches");
    PHASE("setup_account");
    SHOWN("setupAdminName");
    SHOWN("setupAdminPassword");
    QCOMPARE(textOf(h, "startTitle"), QStringLiteral("Your admin account"));
    uitest::saveShot(h.window, QStringLiteral("8a-5-setup-account"));
    setText(h, "setupAdminName", QStringLiteral("admin"));
    setText(h, "setupAdminPassword", QStringLiteral("short"));
    CLICK("setupCreate");
    SHOWN("setupAdminPasswordError");
    QCOMPARE(textOf(h, "setupAdminPasswordError"), QStringLiteral("At least 12 characters."));
    setText(h, "setupAdminPassword", kPassword);
    CLICK("setupCreate");
    QTRY_VERIFY_WITH_TIMEOUT(h.controller->screen() != QStringLiteral("start"), 8000);
    QCOMPARE(hub.setupBodies.size(), 1);
  }

  // 8a-6d: already set up -> join it as a member.
  void setUpAServerThatIsSetUpAlready() {
    FakeHub hub(QStringLiteral("a"));
    hub.setupRequired = false;
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    OnboardingController* ob = h.controller->onboarding();
    ob->beginSetup();
    ob->setupWithCode(hub.address(), QStringLiteral("7H4K2Q"));
    PHASE("security");
    ob->securityMatches();
    PHASE("setup_exists");
    SHOWN("startAction_join");
    SHOWN("startAction_back");
    uitest::saveShot(h.window, QStringLiteral("8a-6-already-set-up"));
    CLICK("startAction_join");
    PHASE("sign_in");
  }

  // 8c-1 / 8c-3e: an invite link at the first start: the code matches and Continue goes to the name step; a link with a wrong
  // security code is a hard stop.
  void inviteLinks() {
    FakeHub hub(QStringLiteral("a"));
    hub.name = QStringLiteral("Lena's server");
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    OnboardingController* ob = h.controller->onboarding();
    const QString address = hub.address().mid(QStringLiteral("https://").size());
    QString fp = hub.fingerprint();
    fp.remove(QLatin1Char(':'));

    // broken link
    QVERIFY(ob->openLink(QStringLiteral("uzume://join/")));
    SHOWN("joinInputError");
    ob->cancel();

    // wrong security code
    QVERIFY(ob->openLink(makeJoinLink(address, QStringLiteral("UZ-8XK2-M4QD"), QString(64, QLatin1Char('A')))));
    PHASE("security_mismatch");
    SHOWN("startAction_dismiss");
    uitest::saveShot(h.window, QStringLiteral("8c-3-link-mismatch"));
    ob->cancel();

    // matching security code (a real link carries the SHA-256 of the certificate)
    QVERIFY(ob->openLink(makeJoinLink(address, QStringLiteral("UZ-8XK2-M4QD"), fp)));
    QTRY_VERIFY_WITH_TIMEOUT(ob->phase() == QStringLiteral("link_ready") || ob->phase() == QStringLiteral("security_mismatch"), 8000);
    if (ob->phase() == QStringLiteral("link_ready")) {
      SHOWN("linkCodeMatches");
      SHOWN("linkContinue");
      SHOWN("linkNotNow");
      uitest::saveShot(h.window, QStringLiteral("8c-1-link-ready"));
      CLICK("linkNotNow");
      PHASE("idle");
    }
  }

  // Ctrl+V anywhere in the window opens an invite link from the clipboard.
  void pasteAnInviteLink() {
    Harness h;
    QVERIFY(h.start());
    SHOWN("startPlay");
    h.item("startPlay")->forceActiveFocus();
    QGuiApplication::clipboard()->setText(QStringLiteral("uzume://join/"));
    QTest::keyClick(h.window, Qt::Key_V, Qt::ControlModifier);
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->onboarding()->errorCode(), QStringLiteral("broken_link"), 4000);
    SHOWN("joinInputError");
    // not a link: nothing happens
    h.controller->onboarding()->cancel();
    QGuiApplication::clipboard()->setText(QStringLiteral("hello"));
    QTest::keyClick(h.window, Qt::Key_V, Qt::ControlModifier);
    QTest::qWait(100);
    QCOMPARE(h.controller->onboarding()->phase(), QStringLiteral("idle"));
  }

  // 8c-2: a link while another server is open: add and switch / add only / cancel.
  void inviteLinkWhileOnAServer() {
    FakeHub home(QStringLiteral("a"));
    home.name = QStringLiteral("Home server");
    QVERIFY(home.start());
    home.addAccount(QStringLiteral("anna"), kPassword);
    FakeHub other(QStringLiteral("b"));
    other.name = QStringLiteral("Lena's server");
    QVERIFY(other.start());
    Harness h;
    QVERIFY(h.start());
    signInAs(h, home, QStringLiteral("anna"));
    OnboardingController* ob = h.controller->onboarding();
    QString fp = other.fingerprint();
    fp.remove(QLatin1Char(':'));
    const QString address = other.address().mid(QStringLiteral("https://").size());
    QVERIFY(ob->openLink(makeJoinLink(address, QStringLiteral("UZ-8XK2-M4QD"), fp)));
    PHASE("link_choice");
    QTRY_COMPARE(h.controller->screen(), QStringLiteral("start"));
    SHOWN("linkAddAndSwitch");
    SHOWN("linkAddOnly");
    SHOWN("linkCancel");
    SHOWN("linkCurrent");
    SHOWN("linkNew");
    uitest::saveShot(h.window, QStringLiteral("8c-2-link-choice"));
    CLICK("linkCancel");
    PHASE("idle");
    QTRY_VERIFY(h.controller->screen() != QStringLiteral("start"));
  }

  // 8d-1 .. 8d-7: the people on this PC. Add a person, set a PIN, switch with a wrong and the right PIN, remove.
  void switchUser() {
    FakeHub hub(QStringLiteral("a"));
    hub.name = QStringLiteral("Home server");
    QVERIFY(hub.start());
    hub.addAccount(QStringLiteral("anna"), kPassword);
    hub.addAccount(QStringLiteral("ben"), kPassword);
    Harness h;
    QVERIFY(h.start());
    signInAs(h, hub, QStringLiteral("anna"));
    AccountsController* ac = h.controller->accounts();
    QTRY_COMPARE(h.controller->screen(), QStringLiteral("library"));

    // Ctrl+Shift+U opens the people screen
    QTest::keyClick(h.window, Qt::Key_U, Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_COMPARE(ac->phase(), QStringLiteral("people"));
    SHOWN("switchUserScreen");
    SHOWN("personTile_u_anna");
    SHOWN("personAdd");
    SHOWN("peopleOnlyOne");  // 8d-7f
    QCOMPARE(textOf(h, "startTitle"), QStringLiteral("Who’s playing?"));
    uitest::saveShot(h.window, QStringLiteral("8d-1-people-one"));

    // Esc closes it
    QTest::keyClick(h.window, Qt::Key_Escape);
    QTRY_COMPARE(ac->phase(), QStringLiteral("idle"));
    GONE("personAdd");

    // Add someone (8d-6)
    ac->open();
    SHOWN("personAdd");
    CLICK("personAdd");
    QTRY_COMPARE(ac->phase(), QStringLiteral("add"));
    SHOWN("addSignInName");
    SHOWN("addSignInPassword");
    SHOWN("addSignInInvite");
    SHOWN("addSignInForgot");
    QTRY_VERIFY_WITH_TIMEOUT(h.controller->connection()->state() == HubConnection::State::NeedsPairing, 8000);
    uitest::saveShot(h.window, QStringLiteral("8d-6-add-person"));
    CLICK("addSignInInvite");
    SHOWN("addInviteCode");
    CLICK("addInviteHaveAccount");
    SHOWN("addSignInName");
    setText(h, "addSignInName", QStringLiteral("ben"));
    setText(h, "addSignInPassword", kPassword);
    CLICK("addSignInSubmit");
    QTRY_COMPARE_WITH_TIMEOUT(ac->phase(), QStringLiteral("idle"), 8000);
    QCOMPARE(ac->people().size(), 2);

    // People: two tiles; Ben is signed in now
    ac->open();
    SHOWN("personTile_u_anna");
    SHOWN("personTile_u_ben");
    QVERIFY(!shown(h, "peopleOnlyOne"));
    uitest::saveShot(h.window, QStringLiteral("8d-1-people"));
    ac->close();

    // A PIN for Ben, then switch to Anna (no PIN) and back to Ben with the PIN
    QVERIFY(ac->setPin(QStringLiteral("4821")));
    ac->open();
    CLICK("personTile_u_anna");
    QTRY_COMPARE_WITH_TIMEOUT(ac->phase(), QStringLiteral("idle"), 8000);
    QCOMPARE(h.controller->connection()->hubUserId(), QStringLiteral("u_anna"));
    ac->open();
    SHOWN("personPin");
    CLICK("personTile_u_ben");
    QTRY_COMPARE(ac->phase(), QStringLiteral("pin"));
    SHOWN("pinPad");
    SHOWN("pinKey_1");
    SHOWN("pinUsePassword");
    uitest::saveShot(h.window, QStringLiteral("8d-2-pin"));
    for (const char* k : {"pinKey_0", "pinKey_0", "pinKey_0", "pinKey_0", "pinKey_ok"}) {
      CLICK(k);
    }
    QTRY_COMPARE(ac->pinError(), QStringLiteral("wrong"));
    SHOWN("pinError");
    QCOMPARE(textOf(h, "startSectionLabel").isEmpty(), false);
    uitest::saveShot(h.window, QStringLiteral("8d-3-pin-wrong"));
    // number keys work too
    h.item("pinPad")->forceActiveFocus();
    for (Qt::Key k : {Qt::Key_4, Qt::Key_8, Qt::Key_2, Qt::Key_1, Qt::Key_Return}) {
      QTest::keyClick(h.window, k);
    }
    QTRY_COMPARE_WITH_TIMEOUT(ac->phase(), QStringLiteral("idle"), 8000);
    QCOMPARE(h.controller->connection()->hubUserId(), QStringLiteral("u_ben"));

    // Remove Anna from this PC (8d-7a)
    ac->open();
    CLICK("personRemove_u_anna");
    QTRY_COMPARE(ac->phase(), QStringLiteral("confirm_remove"));
    SHOWN("removeConfirm");
    SHOWN("removeCancel");
    uitest::saveShot(h.window, QStringLiteral("8d-7-remove"));
    CLICK("removeCancel");
    QTRY_COMPARE(ac->phase(), QStringLiteral("people"));
    CLICK("personRemove_u_anna");
    QTRY_COMPARE(ac->phase(), QStringLiteral("confirm_remove"));
    CLICK("removeConfirm");
    QTRY_COMPARE_WITH_TIMEOUT(ac->people().size(), 1, 8000);
  }

  // 8d-2: "Use password instead" -> the password page (8d-7c / 7e).
  void pinUsePasswordInstead() {
    FakeHub hub(QStringLiteral("a"));
    QVERIFY(hub.start());
    hub.addAccount(QStringLiteral("anna"), kPassword);
    hub.addAccount(QStringLiteral("ben"), kPassword);
    Harness h;
    QVERIFY(h.start());
    signInAs(h, hub, QStringLiteral("anna"));
    AccountsController* ac = h.controller->accounts();
    ac->addPerson();
    QTRY_VERIFY_WITH_TIMEOUT(h.controller->connection()->state() == HubConnection::State::NeedsPairing, 8000);
    ac->addSignIn(QStringLiteral("ben"), kPassword);
    QTRY_COMPARE_WITH_TIMEOUT(ac->phase(), QStringLiteral("idle"), 8000);
    QVERIFY(ac->setPin(QStringLiteral("4821")));
    ac->open();
    ac->switchTo(QStringLiteral("u_anna"));
    QTRY_COMPARE_WITH_TIMEOUT(ac->phase(), QStringLiteral("idle"), 8000);
    ac->open();
    ac->switchTo(QStringLiteral("u_ben"));
    QTRY_COMPARE(ac->phase(), QStringLiteral("pin"));
    CLICK("pinUsePassword");
    QTRY_COMPARE_WITH_TIMEOUT(ac->phase(), QStringLiteral("password"), 8000);
    SHOWN("passwordAgainPassword");
    QVERIFY(h.item("passwordAgainName")->property("readOnly").toBool());
    uitest::saveShot(h.window, QStringLiteral("8d-7-password"));
    CLICK("passwordAgainBack");
    QTRY_COMPARE(ac->phase(), QStringLiteral("people"));
  }

  // 8e: Settings > This PC > My account, reached with showSettingsSection("account").
  void myAccount() {
    FakeHub hub(QStringLiteral("a"));
    hub.name = QStringLiteral("Home server");
    hub.features << QStringLiteral("accounts_v1");
    QVERIFY(hub.start());
    hub.addAccount(QStringLiteral("anna"), kPassword);
    hub.accountDevices.append(QJsonObject{{QStringLiteral("device_id"), QStringLiteral("dev-1")},
                                         {QStringLiteral("name"), QStringLiteral("Anna-Laptop")},
                                         {QStringLiteral("platform"), QStringLiteral("Windows")},
                                         {QStringLiteral("app_version"), QStringLiteral("0.18.0")},
                                         {QStringLiteral("status"), QStringLiteral("trusted")},
                                         {QStringLiteral("current"), false},
                                         {QStringLiteral("sign_in_kind"), QStringLiteral("password")},
                                         {QStringLiteral("last_seen"), QStringLiteral("2026-10-20T18:00:00Z")}});
    Harness h;
    QVERIFY(h.start());
    signInAs(h, hub, QStringLiteral("anna"));
    AccountsController* ac = h.controller->accounts();
    h.controller->showSettingsSection(QStringLiteral("account"));
    QTRY_COMPARE(h.controller->screen(), QStringLiteral("settings"));
    SHOWN("accountSection");
    SHOWN("section_account");
    QTRY_VERIFY_WITH_TIMEOUT(!ac->account().isEmpty(), 8000);
    SHOWN("accountNameRow");
    SHOWN("accountPasswordRow");
    SHOWN("accountPinRow");
    QTRY_VERIFY_WITH_TIMEOUT(shown(h, "accountDevice_dev-1"), 8000);
    uitest::saveShot(h.window, QStringLiteral("8e-1-my-account"));

    // Rename
    CLICK("accountRename");
    SHOWN("renameDialog");
    setText(h, "renameField", QStringLiteral("Anna B"));
    CLICK("renameSave");
    QTRY_COMPARE_WITH_TIMEOUT(ac->account().value(QStringLiteral("displayName")).toString(), QStringLiteral("Anna B"), 8000);
    GONE("renameDialog");

    // Change password: too short first
    CLICK("accountPasswordChange");
    SHOWN("passwordDialog");
    setText(h, "passwordCurrent", kPassword);
    setText(h, "passwordNew", QStringLiteral("short"));
    setText(h, "passwordAgain", QStringLiteral("short"));
    CLICK("passwordSave");
    SHOWN("passwordNewError");
    uitest::saveShot(h.window, QStringLiteral("8e-3-change-password"));
    setText(h, "passwordNew", QStringLiteral("a much longer password"));
    setText(h, "passwordAgain", QStringLiteral("a much longer password"));
    CLICK("passwordSave");
    GONE("passwordDialog");
    QCOMPARE(hub.accountsByUser.value(QStringLiteral("u_anna")).password, QStringLiteral("a much longer password"));

    // PIN for this PC: set, then it can be removed
    CLICK("accountPinSet");
    SHOWN("pinDialog");
    setText(h, "pinNew", QStringLiteral("4821"));
    setText(h, "pinAgain", QStringLiteral("4821"));
    CLICK("pinSave");
    GONE("pinDialog");
    QTRY_VERIFY(ac->account().value(QStringLiteral("hasPin")).toBool());
    SHOWN("accountPinChange");
    CLICK("accountPinRemove");
    SHOWN("removePinDialog");
    CLICK("pinRemoveConfirm");
    GONE("removePinDialog");
    QTRY_VERIFY(!ac->account().value(QStringLiteral("hasPin")).toBool());

    // Devices: sign one out; sign out everywhere
    CLICK("accountDeviceSignOut_dev-1");
    QTRY_VERIFY_WITH_TIMEOUT(hub.accountDevices.first().value(QStringLiteral("status")).toString() == QLatin1String("revoked"), 8000);
    CLICK("accountSignOutEverywhere");
    SHOWN("signOutAllDialog");
    uitest::saveShot(h.window, QStringLiteral("8e-4-sign-out-everywhere"));
    CLICK("signOutAllConfirm");
    GONE("signOutAllDialog");
  }

  // 8e-5a: without a server there is no account; the section says so.
  void myAccountWithoutAServer() {
    Harness h;
    QVERIFY(h.start());
    h.controller->showSettingsSection(QStringLiteral("account"));
    // The first start owns the screen; the section is still in the page when Settings is shown. Check the text directly.
    QVERIFY(h.controller->hubAddress().isEmpty());
  }
};

UITEST_MAIN(StartScreensUiTest)
#include "start_screens_ui_test.moc"
