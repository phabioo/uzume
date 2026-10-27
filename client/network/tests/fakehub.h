#pragma once

// In-process fake Uzume server for tests (HTTPS with a test certificate from tests/testdata, or HTTP).
// Test code only; tokens/credentials are dummy values.

#include <functional>
#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QSslCertificate>
#include <QSslKey>
#include <QPointer>
#include <QTcpServer>

class QSslSocket;

// Fake save slot of the Hub: current checkpoint + conflicts. Test code only.
struct FakeConflict {
  QString id;
  QString status = QStringLiteral("open");  // open | resolved_hub | resolved_local
  int hubRevision = 0;
  QString hubSha;
  QString hubDeviceId, hubDeviceName;
  QByteArray securedContent;
  int securedVersion = 0;
  int securedBase = 0;
  QString securedDeviceId, securedDeviceName;
};
// History version of a slot (saves_v2: labels, before_restore, manual_snapshot).
struct FakeVersion {
  int version = 0;
  int revision = 0;
  QByteArray content;
  QString deviceId, deviceName, reason = QStringLiteral("session_end");
  QString label;
};
struct FakeSlot {
  QList<FakeVersion> history;  // oldest first
  int revision = 0;
  QByteArray content;
  QString deviceId, deviceName, reason = QStringLiteral("checkpoint");
  int nextVersion = 1;
  QList<FakeConflict> conflicts;
};

struct FakeRequest {
  QByteArray method;
  QString path;
  QHash<QString, QByteArray> headers;  // names lowercase
  QByteArray body;
  qint64 arrivalMs = 0;  // monotonic ms (QElapsedTimer clock) when the request was complete
};

class FakeHub : public QTcpServer {
  Q_OBJECT
 public:
  enum class Decision { Pending, Approve, Deny, Expire };

  // certName: "a" or "b" (tests/testdata/test-cert-<x>.pem); empty = HTTP without TLS.
  explicit FakeHub(const QString& certName, QObject* parent = nullptr);

  bool start();
  QString address() const;      // e.g. https://127.0.0.1:PORT
  QString fingerprint() const;  // same format as hub settings; empty for HTTP
  bool setCertificate(const QString& certName);  // "a"|"b"|"c": new connections use this certificate (renewal)

  // Configuration
  QString hubId = QStringLiteral("hub-test-1");
  QString name = QStringLiteral("Test server");
  int protocolVersion = 1;
  int minProtocolVersion = 1;
  Decision decision = Decision::Pending;
  bool rateLimitPairing = false;
  bool revoked = false;
  int tokenLifetime = 900;
  QJsonObject handshakeExtra;      // merged into the handshake response (e.g. problems)
  QJsonObject games;               // {"games":[...]} (raw response)
  QMap<QString, QByteArray> roms;  // sha256 -> content
  qint64 truncateFirstRomAt = -1;  // first ROM response aborts after this many body bytes
  bool ignoreRange = false;        // always responds 200 with the full content

  // Invites (users_v1), uploads (uploads_v1), systems/firmware (firmware_v1)
  QString inviteCode = QStringLiteral("FB-TEST-CODE");  // the only valid code (normalized to upper case)
  bool inviteDirect = true;                              // true: 200 approved; false: 202 pending (poll like pairing)
  QStringList takenNames;                                // display names -> 409 display_name_taken (case-insensitive)
  bool rateLimitInvites = false;
  QJsonObject lastInviteBody;
  // Accounts (accounts_v1) and set-up with a code (setup_code_v1). The caller of the account endpoints is the person the
  // device of the last token request signed in as (deviceUsers); the handshake reports that person as `user`.
  struct FakeAccount {
    QString userId, username, displayName, role = QStringLiteral("user"), password;
    bool passwordRequired = false;
  };
  bool setupRequired = false;                  // `setup_required` of the info endpoint
  QString setupCode = QStringLiteral("7H4K2Q");  // the one valid setup code (normalized)
  bool setupCodeExpired = false;               // the right code answers 410 setup_code_expired
  bool rateLimitSetup = false;
  bool rateLimitSignIn = false;
  int rateLimitRetryAfter = 0;                 // `retry_after` of the rate limit answers (0 = none)
  bool signInUnsupported = false;              // POST /auth/sign-in answers 404 (server before accounts_v1)
  bool holdSignIn = false;                     // sign-in and set-up requests stay unanswered
  QStringList conflictDevices;                 // device IDs that get 409 conflict at sign-in once
  QStringList disabledUsers;                   // user names whose correct password answers 401 user_disabled
  QMap<QString, FakeAccount> accountsByUser;   // user_id -> account (seed with addAccount())
  QMap<QString, QString> deviceUsers;          // device_id -> user_id, set by sign-in and set-up (not by pairing or invites)
  QList<QJsonObject> signInBodies, setupBodies;
  QList<QJsonObject> accountDevices;           // AccountDevice objects as served; "status":"revoked" after a sign-out
  bool requirePasswords = false;               // members cannot remove their password
  void addAccount(const QString& username, const QString& password, const QString& role = QStringLiteral("user"));

  // Local setup (local_setup_v1): /local/status, /local/setup, /local/pair, PUT /local/settings
  bool localAdminExists = false;
  bool localNetworkSharing = false;
  QString localImportDir;
  QJsonObject lastLocalBody;       // body of the last /local/setup or /local/pair
  QJsonObject lastLocalSettings;   // body of the last PUT /local/settings
  QString localImportDirError;     // non-empty: PUT import_dir answers 400 import_dir_unreadable with this message
  // 0.18 package 10: /local/setup creates the admin account (user u_admin_1, role admin) with the given password and
  // /local/pair then checks the password against it (without such an account: the fixed "correct horse").
  bool localChild = false;         // status says child:true and PUT network_sharing answers 409 child_mode
  int localSetupCalls = 0;
  int localPairCalls = 0;
  QJsonObject lastHandshakeBody;  // request body of the last POST /handshake
  bool userDisabled = false;                             // token endpoint answers 401 user_disabled
  bool uploadsAllowed = true;                            // false: POST /games -> 403 uploads_disabled
  struct Upload {
    QString filename, title, sha256;
    qint64 size = 0;
  };
  QList<Upload> uploads;
  QJsonObject systems;                                   // {"systems":[...]} (raw response of GET /systems)
  QMap<QString, QByteArray> firmwareFiles;               // "<system>/<file_id>" -> bytes
  int firmwareDownloads = 0;
  // Core packages (cores_v1): "<core>/<version>/<platform>" -> CorePackage JSON as served; files "<core>/<version>/<platform>/<name>"
  // -> bytes (a missing entry answers 404 core_file_not_available).
  QMap<QString, QJsonObject> corePackages;
  QMap<QString, QByteArray> coreFiles;
  int coreFileDownloads = 0;
  bool holdCoreFiles = false;     // core file requests stay unanswered
  int coreFilesHeld = 0;          // requests held that way
  int coreFilesAbandoned = 0;     // of those, connections the client closed
  // Plugins (plugins_v1): GET /plugins answers {"plugins": pluginList}; files "<id>/<version>" -> bytes (missing: 404).
  QJsonArray pluginList;
  QMap<QString, QByteArray> pluginFiles;
  int pluginFileDownloads = 0;
  int pluginListRequests = 0;
  bool pluginListFails = false;  // GET /plugins answers 500
  int corePackageRequests = 0;
  // Plugin user store (plugin_store_v1): GET /plugins/{id}/store -> {keys:[{key, revision}]}, GET/PUT/DELETE /plugins/{id}/store/{key}. A PUT
  // with a base_revision other than the stored one answers 409 {error, revision, value}. Key "<plugin>/<key>".
  struct FakeStoreEntry {
    qint64 revision = 0;
    QJsonValue value;
  };
  QMap<QString, FakeStoreEntry> pluginStore;
  bool handlePluginStore(QSslSocket* sock, const FakeRequest& req);
  // Save states (states_v1): GET /games/{id}/states[?slot=], GET/PUT/DELETE /games/{id}/states/{slot}/{n}, GET .../preview. Key
  // "<game>/<slot>/<n>". A PUT parses the multipart body (meta, state, preview); stateQuotaBytes caps the sum of the blobs (413).
  struct FakeState {
    QByteArray state, preview;
    QString coreId, coreVersion, deviceName = QStringLiteral("Test PC"), createdAt;
  };
  QMap<QString, FakeState> states;
  qint64 stateQuotaBytes = 1ll << 30;
  int statePuts = 0, stateGets = 0, stateDeletes = 0;
  bool handleStates(QSslSocket* sock, const FakeRequest& req);
  int count(const QString& pathPrefix, const QByteArray& method) const;

  // Local ROMs (local_roms_v1): POST /games/register, DELETE /games/{id}/registration, resumable /uploads.
  struct ResumableUpload {
    QString id, filename, title, system, sha256;
    qint64 size = 0;
    QByteArray data;  // received so far
  };
  QList<QJsonObject> registerBodies;           // bodies of POST /games/register
  QStringList unregisteredGames;               // game ids of DELETE /games/{id}/registration
  QMap<QString, ResumableUpload> uploadSessions;
  QStringList putRanges;                       // Content-Range headers of every PUT, in order
  qint64 uploadChunkSize = 1024;               // chunk_size announced to the Player
  int failPutNumber = -1;                      // the Nth (1-based) chunk PUT answers 503 and is not stored
  int uploadSessionsCreated = 0;

  // Problem reports (reports_v1): POST /reports with a multipart body (meta part first, then the files).
  struct ReceivedReport {
    QJsonObject meta;
    QMap<QString, QByteArray> files;  // file name -> content
    QByteArray authorization;
  };
  QList<ReceivedReport> reports;
  int reportFailNext = 0;            // next N report POSTs answer reportFailStatus and are not stored
  int reportFailStatus = 503;
  int reportDropNext = 0;            // next N report POSTs are read completely, then the connection is closed unanswered
  bool holdReports = false;          // report POSTs stay unanswered (cancel tests)
  int reportsHeld = 0;
  int reportUnauthorizedNext = 0;    // next N report POSTs answer 401
  int reportRequests() const { return count(QStringLiteral("/api/v1/reports"), "POST"); }

  // Admin area (admin_v1, accounts_v1): `adminUser` makes the handshake report role "admin" and advertise both features.
  // GET /admin/status serves `adminStatus` (a sample when empty); /admin/status/log serves `adminLog`; every other
  // /admin/... route answers {"ok":true} and is counted in `adminCalls` (method + " " + path, without the query). Paths in
  // `confirmRequiredPaths` answer 403 confirmation_required until POST /account/confirm succeeded with `accountPassword`
  // (300 s); 3 wrong passwords lock confirming for `confirmLockSeconds` (429 rate_limited with retry_after).
  bool adminUser = false;
  QJsonObject adminStatus;
  bool adminStatusDown = false;           // GET /admin/status answers 503
  int adminStatusRequests = 0;
  QByteArray adminLog = "line 1\nline 2\n";
  QStringList confirmRequiredPaths;       // exact paths, e.g. "/api/v1/admin/users/u_2/disable"
  QString accountPassword = QStringLiteral("correct horse battery");
  bool accountHasPassword = true;
  int confirmLockSeconds = 300;
  int confirmPosts = 0;                   // POST /account/confirm received
  int confirmDeletes = 0;
  bool confirmed() const;
  QMap<QString, int> adminCalls;
  QByteArray lastAdminBody;
  // Fixed answers for admin routes (key "METHOD /api/v1/admin/..." without the query): status and JSON body (empty = no body).
  // `adminBodies` keeps the last request body of every admin route (key as in `adminCalls`).
  struct AdminRoute {
    int status = 200;
    QByteArray body;
  };
  QMap<QString, AdminRoute> adminRoutes;
  QMap<QString, QByteArray> adminBodies;
  // Called with the call key after the request was counted and before the answer is chosen: a test changes `adminRoutes` here to make a
  // route stateful (a backup that starts, then runs, then is done).
  std::function<void(const QString&)> adminHook;
  static QJsonObject sampleAdminStatus();

  // Users, devices, invites, pairing requests and reports (admin pages Users and Reports): with `adminDirectory` the routes below
  // serve a small stateful sample (users Tester = admin and signed in, Lena, Jonas; devices; two waiting requests; two reports with
  // files) and change it as the real server would. The arrays can be edited before the first request. Calls are counted in
  // `adminCalls` like every other admin route; `lastAdminBody` holds the last body.
  bool adminDirectory = false;
  QJsonArray dirUsers, dirDevices, dirInvites, dirRequests, dirReports;
  QMap<QString, QByteArray> reportFiles;   // "<report id>/<file name>"
  void fillDirectory();

  // Saves (saves_v1)
  QStringList features{QStringLiteral("saves_v1")};  // handshake features
  // game_id -> slot "default"; other slots under the key "<game_id>/<slot>" (slotKey()).
  QMap<QString, FakeSlot> saves;
  static QString slotKey(const QString& gameId, const QString& slot) {
    return slot == QLatin1String("default") ? gameId : gameId + QLatin1Char('/') + slot;
  }
  // Adds a history version to a slot (creates the slot object if needed; revision of the slot is not changed).
  void addHistory(const QString& gameId, const QString& slot, const QByteArray& content, const QString& reason,
                  const QString& label = QString(), const QString& deviceName = QStringLiteral("Laptop Office"));
  int uploadCount = 0;   // save file uploads that succeeded
  qint64 uploadLimit = 64LL * 1024 * 1024;  // 413 above this
  int deleteCount = 0;   // snapshot deletions that succeeded
  int restoreCount = 0;  // restore requests that succeeded
  int renameCount = 0;       // slot renames that succeeded (saves_v5)
  int slotDeleteCount = 0;   // slot deletions that succeeded (saves_v5)
  QStringList sessionRunningGames;  // game ids with a running Session: upload / rename / delete -> 409 session_running
  int failSaveRequests = 0;                          // next N save requests answer 503
  QString callerDeviceId;                            // device_id of the last token request
  // Simulates another device that uploaded a new checkpoint.
  void setHubSave(const QString& gameId, const QByteArray& content, const QString& deviceId = QStringLiteral("other-device"),
                  const QString& deviceName = QStringLiteral("Laptop Office"));

  // Sessions (sessions_v1): minimal in-memory REST + WSS (own WebSocket implementation on the same TLS port)
  QMap<QString, QJsonObject> sessions;  // session_id -> Session JSON (as REST)
  QJsonArray fakeUsers;                 // GET /users (empty: only the test user)
  bool publishCapabilityMissing = false;
  bool joinFull = false;
  QJsonArray helloTurnServers;            // `turn_servers` of hello_ack (omitted when empty)
  QJsonArray joinTurnServers;             // `turn_servers` of the join response (omitted when empty)
  bool refuseWs = false;                  // WSS upgrade answered with 401
  QList<QJsonObject> wsReceived;          // envelopes received from the Player (all connections)
  QByteArray lastWsAuthorization;
  int wsConnections = 0;                  // upgrades accepted so far
  int wsOpen() const { return static_cast<int>(wsClients_.size()); }
  void sendWs(const QString& type, const QJsonObject& payload);  // to every open WSS connection
  // GET /sessions/<id> answers are held (body of that moment, visibility optionally overridden) until released:
  // models a request served before a later PATCH whose answer arrives after the PATCH answer.
  bool holdSessionGet = false;
  int heldSessionGets() const { return static_cast<int>(heldGets_.size()); }
  void releaseHeldSessionGets(const QString& visibilityOverride = {});
  void closeWsClients();                                         // drops the connections (reconnect tests)

  // Runs (runs_v1): in-memory REST + WSS model. Tests seed `runs` (Run JSON as served) and `runEvents`, or let the
  // routes create them. The caller is `runsMe`.
  QString runsMe = QStringLiteral("u_test_1");
  QString runsMeName = QStringLiteral("Tester");
  QMap<QString, QJsonObject> runs;               // run_id -> Run JSON
  QMap<QString, QList<QJsonObject>> runEvents;   // run_id -> events, ascending, seq from 1
  QMap<QString, QJsonObject> runBoards;          // "<run_id>/<plugin_id>" -> tree
  int runStateConflicts = 0;                     // next N state PUTs answer 409 state_conflict
  qint64 runConflictRev = 7;                     // the state_rev such a 409 reports
  int runEventDropResponses = 0;                 // next N event POSTs are stored but the connection is dropped unanswered
  int runFailNext = 0;                           // next N run requests answer runFailStatus
  int runFailStatus = 503;
  int runHelloCatchUpMax = 500;                  // more missing events than this: none are sent after hello
  QJsonObject lastRunBody;                       // body of the last run POST/PUT
  QList<QJsonObject> runStatePuts;               // bodies of the state PUTs (accepted or answered 409)
  QList<QJsonObject> runEventPosts;              // bodies of every event POST (also retried ones)
  int runBoardPuts = 0;
  QList<QJsonObject> runRulesPuts;               // bodies of PUT /runs/{id}/rules (run_rules_v1)
  bool runRulesForbidden = false;                // the next rules PUTs answer 403 forbidden
  static QString makeUuid();
  // Appends an event (Hub event when userId is empty) and pushes run_event to the WSS clients; returns the seq.
  qint64 addRunEvent(const QString& runId, const QString& type, const QString& text, const QString& userId = {},
                     const QString& pluginId = {}, const QString& clientId = {}, bool push = true);
  void pushRun(const QString& runId);            // run_updated to the WSS clients
  QJsonObject makeRun(const QString& name, const QStringList& userIds, const QString& creator, const QJsonArray& plugins = {});

  // Favourites and collections (Uzume 0.19): GET/POST /collections, PATCH/DELETE /collections/{id}, PUT/DELETE
  // /collections/{id}/games/{game}, PUT/DELETE /games/{game}/favorite. In-memory; every request is in `requests`.
  // `collections` holds {id, name, game_ids, created_at} as served; `favoriteGames` the games marked favourite.
  QJsonArray collections;
  QStringList favoriteGames;
  bool collectionsUnsupported = true;   // the routes answer 404 (a server before 0.19); tests of 0.19 turn it off
  int collectionFailNext = 0;           // next N collection / favourite changes answer 503
  bool handleCollections(QSslSocket* sock, const FakeRequest& req);

  // Screenshots (Uzume 0.19): POST/GET /games/{id}/screenshots (own), GET /runs/{id}/screenshots, GET /screenshots/{id}/content,
  // DELETE /screenshots/{id}. In-memory, the caller is u_test_1 "Tester". A body that is no PNG answers 400 invalid_image, one over
  // `screenshotMaxBytes` 413 payload_too_large.
  struct FakeScreenshot {
    QJsonObject meta;  // as served
    QByteArray png;
  };
  QList<FakeScreenshot> screenshots;  // oldest first
  bool screenshotsUnsupported = true;  // the routes answer 404 (a server before 0.19); tests of 0.19 turn it off
  int screenshotFailNext = 0;          // next N uploads answer 503
  qint64 screenshotMaxBytes = 16ll * 1024 * 1024;
  bool handleScreenshots(QSslSocket* sock, const FakeRequest& req);

  // Test-defined routes (tools catalog, variants): called for every authenticated request before the built-in routes; return true
  // after filling status/body/contentType to answer, false to fall through.
  std::function<bool(const FakeRequest& req, int& status, QByteArray& body, QByteArray& contentType)> customRoute;

  // Observation
  QList<FakeRequest> requests;
  int count(const QString& pathPrefix) const;
  int tokenRequests() const { return tokenRequests_; }
  QList<QByteArray> issuedAccessTokens;

  static const QByteArray kDeviceCredential;
  static const QByteArray kPollToken;

 protected:
  void incomingConnection(qintptr handle) override;

 private:
  struct HeldGet {
    QPointer<QSslSocket> sock;
    QJsonObject session;
  };
  QList<HeldGet> heldGets_;
  void handle(QSslSocket* sock, const FakeRequest& req);
  void respond(QSslSocket* sock, int status, const QByteArray& body, const QByteArray& contentType = "application/json",
               const QList<QPair<QByteArray, QByteArray>>& extra = {}, qint64 truncateAt = -1);
  void respondError(QSslSocket* sock, int status, const QString& code);
  void handleSaves(QSslSocket* sock, const FakeRequest& req);
  void handleAdmin(QSslSocket* sock, const FakeRequest& req);
  bool handleDirectory(QSslSocket* sock, const FakeRequest& req, const QString& path, const QJsonObject& body);
  bool handleLocalRoms(QSslSocket* sock, const FakeRequest& req);
  bool handleSignInAndSetup(QSslSocket* sock, const FakeRequest& req);
  void handleAccount(QSslSocket* sock, const FakeRequest& req);
  QJsonObject accountJson(const FakeAccount& a) const;
  void respondRateLimited(QSslSocket* sock);
  FakeAccount* callerAccount();
  void handleSessions(QSslSocket* sock, const FakeRequest& req);
  void handleRuns(QSslSocket* sock, const FakeRequest& req);
  void sendRunCatchUp(QSslSocket* sock, const QJsonObject& hello);
  void upgradeWs(QSslSocket* sock, const FakeRequest& req);
  void onWsData(QSslSocket* sock);
  void wsWrite(QSslSocket* sock, quint8 opcode, const QByteArray& payload);
  QList<QSslSocket*> wsClients_;
  QHash<QSslSocket*, QByteArray> wsBuffers_;
  QJsonObject slotJson(const QString& key, const FakeSlot& s) const;  // key: slotKey()
  QJsonObject conflictJson(const QString& key, const FakeSlot& s, const FakeConflict& c) const;
  QJsonObject versionJson(const FakeVersion& v) const;
  QJsonArray wsFeatures() const {
    QJsonArray a;
    for (const QString& f : features) {
      a.append(f);
    }
    if (!features.contains(QStringLiteral("sessions_v1"))) {
      a.append(QStringLiteral("sessions_v1"));
    }
    return a;
  }
  bool bearerIs(const FakeRequest& req, const QByteArray& prefix) const;

  QString certName_;
  QSslCertificate cert_;
  QSslKey key_;
  bool approvedDelivered_ = false;
  QDateTime confirmedUntil_;
  QDateTime confirmLockedUntil_;
  int confirmWrong_ = 0;
  int tokenRequests_ = 0;
  int screenshotSeq_ = 0;
  bool truncated_ = false;
};
