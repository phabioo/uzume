#include "fakehub.h"

#include <QCryptographicHash>
#include <QDeadlineTimer>

#include <QFile>
#include <QFileInfo>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSslConfiguration>
#include <QSslSocket>
#include <memory>

#include "hubhttp.h"

const QByteArray FakeHub::kDeviceCredential = "fbd_TEST_DUMMY_CREDENTIAL";
const QByteArray FakeHub::kPollToken = "fbp_TEST_DUMMY_POLL";

FakeHub::FakeHub(const QString& certName, QObject* parent) : QTcpServer(parent), certName_(certName) {
  games = QJsonObject{{QStringLiteral("games"), QJsonArray()}};
  if (!certName.isEmpty()) {
    const QString dir = QStringLiteral(FB_TEST_DATA_DIR);
    QFile c(dir + QStringLiteral("/test-cert-") + certName + QStringLiteral(".pem"));
    QFile k(dir + QStringLiteral("/test-key-") + certName + QStringLiteral(".pem"));
    if (c.open(QIODevice::ReadOnly) && k.open(QIODevice::ReadOnly)) {
      cert_ = QSslCertificate(&c, QSsl::Pem);
      key_ = QSslKey(&k, QSsl::Ec, QSsl::Pem);
    }
  }
}

bool FakeHub::setCertificate(const QString& certName) {
  const QString dir = QStringLiteral(FB_TEST_DATA_DIR);
  QFile c(dir + QStringLiteral("/test-cert-") + certName + QStringLiteral(".pem"));
  QFile k(dir + QStringLiteral("/test-key-") + certName + QStringLiteral(".pem"));
  if (!c.open(QIODevice::ReadOnly) || !k.open(QIODevice::ReadOnly)) {
    return false;
  }
  cert_ = QSslCertificate(&c, QSsl::Pem);
  key_ = QSslKey(&k, QSsl::Ec, QSsl::Pem);
  certName_ = certName;
  return !cert_.isNull() && !key_.isNull();
}

bool FakeHub::start() {
  if (!certName_.isEmpty() && (cert_.isNull() || key_.isNull())) {
    return false;
  }
  return listen(QHostAddress::LocalHost, 0);
}

QString FakeHub::address() const {
  return QStringLiteral("%1://127.0.0.1:%2").arg(certName_.isEmpty() ? QStringLiteral("http") : QStringLiteral("https")).arg(serverPort());
}

QString FakeHub::fingerprint() const { return uzume::HubHttp::fingerprint(cert_); }

int FakeHub::count(const QString& pathPrefix, const QByteArray& method) const {
  int n = 0;
  for (const FakeRequest& r : requests) {
    if (r.method == method && r.path.startsWith(pathPrefix)) {
      ++n;
    }
  }
  return n;
}

int FakeHub::count(const QString& pathPrefix) const {
  int n = 0;
  for (const FakeRequest& r : requests) {
    if (r.path.startsWith(pathPrefix)) {
      ++n;
    }
  }
  return n;
}

void FakeHub::incomingConnection(qintptr descriptor) {
  auto* sock = new QSslSocket(this);
  if (!sock->setSocketDescriptor(descriptor)) {
    delete sock;
    return;
  }
  connect(sock, &QSslSocket::disconnected, sock, &QObject::deleteLater);
  auto buffer = std::make_shared<QByteArray>();
  connect(sock, &QSslSocket::disconnected, this, [this, sock]() {
    wsClients_.removeAll(sock);
    wsBuffers_.remove(sock);
  });
  connect(sock, &QSslSocket::readyRead, this, [this, sock, buffer]() {
    if (wsBuffers_.contains(sock)) {
      wsBuffers_[sock].append(sock->readAll());
      onWsData(sock);
      return;
    }
    buffer->append(sock->readAll());
    const int headEnd = buffer->indexOf("\r\n\r\n");
    if (headEnd < 0) {
      return;
    }
    const QList<QByteArray> lines = buffer->left(headEnd).split('\n');
    const QList<QByteArray> first = lines.first().trimmed().split(' ');
    if (first.size() < 2) {
      sock->disconnectFromHost();
      return;
    }
    FakeRequest req;
    req.method = first.at(0);
    req.path = QString::fromLatin1(first.at(1));
    for (qsizetype i = 1; i < lines.size(); ++i) {
      const int colon = lines.at(i).indexOf(':');
      if (colon > 0) {
        req.headers.insert(QString::fromLatin1(lines.at(i).left(colon).trimmed().toLower()), lines.at(i).mid(colon + 1).trimmed());
      }
    }
    const qint64 len = req.headers.value(QStringLiteral("content-length"), "0").toLongLong();
    if (buffer->size() - (headEnd + 4) < len) {
      return;
    }
    req.body = buffer->mid(headEnd + 4, static_cast<qsizetype>(len));
    buffer->clear();
    req.arrivalMs = QDeadlineTimer::current().deadlineNSecs() / 1000000;
    requests.append(req);
    handle(sock, req);
  });
  if (!certName_.isEmpty()) {
    QSslConfiguration cfg = QSslConfiguration::defaultConfiguration();
    cfg.setLocalCertificate(cert_);
    cfg.setPrivateKey(key_);
    cfg.setPeerVerifyMode(QSslSocket::VerifyNone);
    sock->setSslConfiguration(cfg);
    sock->startServerEncryption();
  }
}

bool FakeHub::bearerIs(const FakeRequest& req, const QByteArray& prefix) const {
  return req.headers.value(QStringLiteral("authorization")).startsWith("Bearer " + prefix);
}

void FakeHub::respond(QSslSocket* sock, int status, const QByteArray& body, const QByteArray& contentType,
                      const QList<QPair<QByteArray, QByteArray>>& extra, qint64 truncateAt) {
  QByteArray head = "HTTP/1.1 " + QByteArray::number(status) + " X\r\nContent-Length: " + QByteArray::number(body.size()) +
                    "\r\nConnection: close\r\n";
  if (!body.isEmpty()) {
    head += "Content-Type: " + contentType + "\r\n";
  }
  for (const auto& h : extra) {
    head += h.first + ": " + h.second + "\r\n";
  }
  head += "\r\n";
  sock->write(head);
  sock->write(truncateAt >= 0 ? body.left(static_cast<qsizetype>(truncateAt)) : body);
  sock->disconnectFromHost();
}

void FakeHub::respondError(QSslSocket* sock, int status, const QString& code) {
  respond(sock, status,
          QJsonDocument(QJsonObject{{QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), code}, {QStringLiteral("message"), code}}}})
              .toJson(QJsonDocument::Compact));
}

bool FakeHub::confirmed() const { return confirmedUntil_.isValid() && confirmedUntil_ > QDateTime::currentDateTimeUtc(); }

QJsonObject FakeHub::sampleAdminStatus() {
  const QDateTime now = QDateTime::currentDateTimeUtc();
  return QJsonObject{
      {QStringLiteral("hub_name"), QStringLiteral("Home server")},
      {QStringLiteral("hub_version"), QStringLiteral("0.18.0")},
      {QStringLiteral("uptime_seconds"), 12 * 86400 + 4 * 3600 + 120},
      {QStringLiteral("public_address"), QStringLiteral("server.example.com:8443")},
      {QStringLiteral("time"), now.toString(Qt::ISODate)},
      {QStringLiteral("problems"),
       QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("backup_disk")},
                              {QStringLiteral("title"), QStringLiteral("Backup disk 92 %")},
                              {QStringLiteral("detail"), QStringLiteral("Backup disk is 92 % full. Tonight's backup may not fit.")},
                              {QStringLiteral("severity"), QStringLiteral("warn")}}}},
      {QStringLiteral("cpu_percent"), 18.4},
      {QStringLiteral("memory"), QJsonObject{{QStringLiteral("process_bytes"), 80LL * 1024 * 1024},
                                             {QStringLiteral("system_total_bytes"), 4LL * 1024 * 1024 * 1024},
                                             {QStringLiteral("system_used_bytes"), 1288490188LL}}},
      {QStringLiteral("disk"), QJsonObject{{QStringLiteral("total_bytes"), 100LL * 1000 * 1000 * 1000},
                                           {QStringLiteral("free_bytes"), 39LL * 1000 * 1000 * 1000}}},
      {QStringLiteral("backup_disk"), QJsonObject{{QStringLiteral("folder"), QStringLiteral("/mnt/backup")},
                                                  {QStringLiteral("total_bytes"), 50LL * 1000 * 1000 * 1000},
                                                  {QStringLiteral("free_bytes"), 4LL * 1000 * 1000 * 1000}}},
      {QStringLiteral("sessions"),
       QJsonArray{QJsonObject{{QStringLiteral("game_title"), QStringLiteral("Harbor Rally")},
                              {QStringLiteral("owner"), QStringLiteral("Max")},
                              {QStringLiteral("device_name"), QStringLiteral("Desktop-LivingRoom")},
                              {QStringLiteral("visibility"), QStringLiteral("hub_users")},
                              {QStringLiteral("started_at"), now.addSecs(-1500).toString(Qt::ISODate)},
                              {QStringLiteral("viewers"), 2}}}},
      {QStringLiteral("viewers"), 2},
      {QStringLiteral("turn"), QJsonObject{{QStringLiteral("allocations"), 3}, {QStringLiteral("relay_ports"), 100}}},
      {QStringLiteral("players"),
       QJsonArray{QJsonObject{{QStringLiteral("device_name"), QStringLiteral("Desktop-LivingRoom")},
                              {QStringLiteral("user"), QStringLiteral("Max")},
                              {QStringLiteral("version"), QStringLiteral("0.18.0")},
                              {QStringLiteral("state"), QStringLiteral("in_session")},
                              {QStringLiteral("outdated"), false}},
                  QJsonObject{{QStringLiteral("device_name"), QStringLiteral("Laptop-Jonas")},
                              {QStringLiteral("user"), QStringLiteral("Jonas")},
                              {QStringLiteral("version"), QStringLiteral("0.16.2")},
                              {QStringLiteral("state"), QStringLiteral("idle")},
                              {QStringLiteral("outdated"), true}}}},
      {QStringLiteral("errors"),
       QJsonArray{QJsonObject{{QStringLiteral("time"), now.addSecs(-600).toString(Qt::ISODate)},
                              {QStringLiteral("level"), QStringLiteral("warn")},
                              {QStringLiteral("message"), QStringLiteral("backup folder is almost full")}}}},
      {QStringLiteral("metrics_enabled"), false}};
}

void FakeHub::handleAdmin(QSslSocket* sock, const FakeRequest& req) {
  const auto json = [](const QJsonObject& o) { return QJsonDocument(o).toJson(QJsonDocument::Compact); };
  const auto errorWith = [&](int status, const QString& code, const QString& message, int retryAfter = 0) {
    QJsonObject e{{QStringLiteral("code"), code}, {QStringLiteral("message"), message}};
    if (retryAfter > 0) e.insert(QStringLiteral("retry_after"), retryAfter);
    respond(sock, status, json({{QStringLiteral("error"), e}}));
  };
  const QString path = req.path.section(QLatin1Char('?'), 0, 0);
  const QJsonObject body = QJsonDocument::fromJson(req.body).object();

  if (path == QLatin1String("/api/v1/account/confirm")) {
    if (req.method == "DELETE") {
      ++confirmDeletes;
      confirmedUntil_ = QDateTime();
      respond(sock, 204, {});
      return;
    }
    ++confirmPosts;
    if (!accountHasPassword) {
      errorWith(409, QStringLiteral("password_not_set"), QStringLiteral("This account has no password"));
      return;
    }
    const QDateTime now = QDateTime::currentDateTimeUtc();
    if (confirmLockedUntil_.isValid() && confirmLockedUntil_ > now) {
      errorWith(429, QStringLiteral("rate_limited"), QStringLiteral("Too many tries"), static_cast<int>(now.secsTo(confirmLockedUntil_)));
      return;
    }
    if (body.value(QStringLiteral("password")).toString() != accountPassword) {
      if (++confirmWrong_ >= 3) {
        confirmWrong_ = 0;
        confirmLockedUntil_ = now.addSecs(confirmLockSeconds);
        errorWith(429, QStringLiteral("rate_limited"), QStringLiteral("Too many tries"), confirmLockSeconds);
      } else {
        errorWith(401, QStringLiteral("invalid_credentials"), QStringLiteral("Invalid credentials"));
      }
      return;
    }
    confirmWrong_ = 0;
    confirmedUntil_ = now.addSecs(body.value(QStringLiteral("duration_seconds")).toInt(300));
    respond(sock, 200, json({{QStringLiteral("confirmed_until"), confirmedUntil_.toString(Qt::ISODate)}}));
    return;
  }
  if (!adminUser) {
    errorWith(403, QStringLiteral("forbidden"), QStringLiteral("Admins only"));
    return;
  }
  if (confirmRequiredPaths.contains(path) && !confirmed()) {
    errorWith(403, QStringLiteral("confirmation_required"), QStringLiteral("Confirm it's you first"));
    return;
  }
  if (path == QLatin1String("/api/v1/admin/status") && req.method == "GET") {
    ++adminStatusRequests;
    if (adminStatusDown) {
      errorWith(503, QStringLiteral("internal"), QStringLiteral("down"));
      return;
    }
    respond(sock, 200, json(adminStatus.isEmpty() ? sampleAdminStatus() : adminStatus));
    return;
  }
  if (path == QLatin1String("/api/v1/admin/status/log") && req.method == "GET") {
    ++adminCalls[QStringLiteral("GET ") + path];
    respond(sock, 200, adminLog, "text/plain",
            {{"Content-Disposition", "attachment; filename=\"uzume-server-log-20261022-081500.txt\""}});
    return;
  }
  if (adminDirectory && handleDirectory(sock, req, path, body)) return;
  const QString callKey = QString::fromLatin1(req.method) + QLatin1Char(' ') + path;
  ++adminCalls[callKey];
  if (req.method != "GET") lastAdminBody = req.body;
  adminBodies[callKey] = req.body;
  if (adminHook) adminHook(callKey);
  if (adminRoutes.contains(callKey)) {
    const AdminRoute& route = adminRoutes[callKey];
    respond(sock, route.status, route.body);
    return;
  }
  respond(sock, 200, json({{QStringLiteral("ok"), true}, {QStringLiteral("path"), path}}));
}

void FakeHub::fillDirectory() {
  if (!dirUsers.isEmpty()) return;
  const auto at = [](int minutesAgo) { return QDateTime::currentDateTimeUtc().addSecs(-60 * minutesAgo).toString(Qt::ISODate); };
  const auto user = [&](const char* id, const char* name, const char* role, int devices, int seenMinutes, bool disabled = false) {
    QJsonObject o{{QStringLiteral("id"), QString::fromLatin1(id)}, {QStringLiteral("username"), QString::fromLatin1(name).toLower()},
                  {QStringLiteral("display_name"), QString::fromLatin1(name)}, {QStringLiteral("role"), QString::fromLatin1(role)},
                  {QStringLiteral("disabled"), disabled}, {QStringLiteral("created_at"), at(60 * 24 * 40)},
                  {QStringLiteral("devices"), devices}};
    if (seenMinutes >= 0) o.insert(QStringLiteral("last_seen_at"), at(seenMinutes));
    return o;
  };
  dirUsers = QJsonArray{user("u_test_1", "Tester", "admin", 1, 0), user("u_2", "Lena", "user", 1, 95), user("u_3", "Jonas", "user", 1, 35)};
  const auto device = [&](const char* id, const char* userId, const char* userName, const char* name, bool online, int seen) {
    return QJsonObject{{QStringLiteral("id"), QString::fromLatin1(id)}, {QStringLiteral("user_id"), QString::fromLatin1(userId)},
                       {QStringLiteral("user_name"), QString::fromLatin1(userName)}, {QStringLiteral("name"), QString::fromLatin1(name)},
                       {QStringLiteral("platform"), QStringLiteral("windows")}, {QStringLiteral("arch"), QStringLiteral("x86_64")},
                       {QStringLiteral("player_version"), QStringLiteral("0.17.0")}, {QStringLiteral("status"), QStringLiteral("trusted")},
                       {QStringLiteral("online"), online}, {QStringLiteral("created_at"), at(60 * 24 * 30)},
                       {QStringLiteral("last_seen_at"), at(seen)}};
  };
  dirDevices = QJsonArray{device("d_1", "u_test_1", "Tester", "Desktop-LivingRoom", true, 0), device("d_2", "u_2", "Lena", "Lena-Laptop", false, 95),
                          device("d_3", "u_3", "Jonas", "Jonas-Laptop", false, 35)};
  const auto request = [&](const char* id, const char* name, const char* userId) {
    QJsonObject o{{QStringLiteral("id"), QString::fromLatin1(id)}, {QStringLiteral("device_id"), QStringLiteral("dev_") + QString::fromLatin1(id)},
                  {QStringLiteral("device_name"), QString::fromLatin1(name)}, {QStringLiteral("platform"), QStringLiteral("windows")},
                  {QStringLiteral("arch"), QStringLiteral("x86_64")}, {QStringLiteral("player_version"), QStringLiteral("0.17.0")},
                  {QStringLiteral("protocol_version"), 1}, {QStringLiteral("status"), QStringLiteral("pending")},
                  {QStringLiteral("created_at"), at(20)}, {QStringLiteral("expires_at"), at(-60 * 24)}};
    if (userId && *userId) o.insert(QStringLiteral("user_id"), QString::fromLatin1(userId));
    return o;
  };
  dirRequests = QJsonArray{request("r_1", "Lena-PC", "u_2"), request("r_2", "Guest-PC", "")};
  dirInvites = QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("i_1")}, {QStringLiteral("status"), QStringLiteral("active")},
                                      {QStringLiteral("authorize_device"), false}, {QStringLiteral("created_at"), at(30)},
                                      {QStringLiteral("expires_at"), at(-60 * 24 * 3)}},
                          QJsonObject{{QStringLiteral("id"), QStringLiteral("i_2")}, {QStringLiteral("status"), QStringLiteral("redeemed")},
                                      {QStringLiteral("authorize_device"), true}, {QStringLiteral("created_at"), at(60 * 24 * 5)},
                                      {QStringLiteral("expires_at"), at(60 * 24 * 4)}, {QStringLiteral("redeemed_by"), QStringLiteral("Jonas")}}};
  const auto report = [&](int id, const char* status, const char* title, const char* who, int ago, const QStringList& files, const char* event) {
    QJsonArray fl;
    qint64 total = 0;
    for (const QString& f : files) {
      const QByteArray content = QByteArray("content of ") + f.toUtf8() + "\n";
      reportFiles.insert(QString::number(id) + QLatin1Char('/') + f, content);
      fl.append(QJsonObject{{QStringLiteral("name"), f}, {QStringLiteral("size"), content.size()}});
      total += content.size();
    }
    QJsonObject o{{QStringLiteral("id"), id}, {QStringLiteral("status"), QString::fromLatin1(status)}, {QStringLiteral("title"), QString::fromLatin1(title)},
                  {QStringLiteral("user"), QString::fromLatin1(who)}, {QStringLiteral("device"), QStringLiteral("Lena-PC")},
                  {QStringLiteral("player_version"), QStringLiteral("0.17.0")}, {QStringLiteral("os"), QStringLiteral("Windows 11")},
                  {QStringLiteral("created_at"), at(ago)}, {QStringLiteral("size"), total}, {QStringLiteral("file_count"), files.size()},
                  {QStringLiteral("description"), QString::fromLatin1(title) + QStringLiteral(". With three tiles open it happens every few seconds.")},
                  {QStringLiteral("files"), fl}};
    if (*event) {
      o.insert(QStringLiteral("linked_event_title"), QString::fromLatin1(event));
      o.insert(QStringLiteral("linked_event_time"), at(ago + 2));
    }
    return o;
  };
  dirReports = QJsonArray{report(12, "new", "Sound crackles in Multiview", "Lena", 40, {QStringLiteral("app-log.txt"), QStringLiteral("diagnostics.json")}, "Audio underrun"),
                          report(11, "seen", "Touch offset on bottom screen", "Jonas", 60 * 24 * 4, {}, "")};
}

bool FakeHub::handleDirectory(QSslSocket* sock, const FakeRequest& req, const QString& path, const QJsonObject& body) {
  fillDirectory();
  const auto json = [](const QJsonObject& o) { return QJsonDocument(o).toJson(QJsonDocument::Compact); };
  const auto err = [&](int status, const QString& code, const QString& message) {
    respond(sock, status, json({{QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), code}, {QStringLiteral("message"), message}}}}));
  };
  const QString rel = path.mid(QStringLiteral("/api/v1/admin/").size());
  const QStringList seg = rel.split(QLatin1Char('/'), Qt::SkipEmptyParts);
  if (seg.isEmpty()) return false;
  const QByteArray m = req.method;
  const auto count = [&]() { ++adminCalls[QString::fromLatin1(m) + QLatin1Char(' ') + path]; if (!req.body.isEmpty()) lastAdminBody = req.body; };
  const auto find = [](QJsonArray& arr, const QString& key, const QString& value) {
    for (int i = 0; i < arr.size(); ++i) if (arr.at(i).toObject().value(key).toVariant().toString() == value) return i;
    return -1;
  };
  const auto edit = [](QJsonArray& arr, int i, const QString& key, const QJsonValue& v) {
    QJsonObject o = arr.at(i).toObject();
    o.insert(key, v);
    arr.replace(i, o);
  };
  const QString kind = seg.value(0);
  const QString id = seg.value(1);
  const QString action = seg.value(2);

  if (kind == QLatin1String("users")) {
    // a person's saves (/admin/users/{id}/games/...) and the saves export (/admin/users/{id}/saves/export) use adminRoutes
    if (action == QLatin1String("games") || action == QLatin1String("saves")) return false;
    if (seg.size() == 1 && m == "GET") {
      count();
      respond(sock, 200, json({{QStringLiteral("users"), dirUsers}}));
      return true;
    }
    if (seg.size() == 1 && m == "POST") {   // add a person directly: the person and a one-time pairing code
      count();
      const QString dn = body.value(QStringLiteral("display_name")).toString().trimmed();
      if (dn.isEmpty()) { err(400, QStringLiteral("bad_request"), QStringLiteral("Name is required")); return true; }
      const QJsonObject nu{{QStringLiteral("id"), QStringLiteral("u_new")}, {QStringLiteral("username"), dn.toLower()},
                           {QStringLiteral("display_name"), dn}, {QStringLiteral("role"), QStringLiteral("user")},
                           {QStringLiteral("disabled"), false}, {QStringLiteral("created_at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                           {QStringLiteral("devices"), 0}};
      dirUsers.append(nu);
      respond(sock, 201, json({{QStringLiteral("user"), nu},
                               {QStringLiteral("pairing_code"), QJsonObject{{QStringLiteral("code"), QStringLiteral("UZ-NEW1-2345")},
                                                                             {QStringLiteral("expires_at"), QDateTime::currentDateTimeUtc().addDays(1).toString(Qt::ISODate)}}}}));
      return true;
    }
    const int i = find(dirUsers, QStringLiteral("id"), id);
    if (seg.size() < 2 || i < 0) {
      count();
      err(404, QStringLiteral("not_found"), QStringLiteral("Unknown user"));
      return true;
    }
    const QJsonObject u = dirUsers.at(i).toObject();
    const bool isAdmin = u.value(QStringLiteral("role")).toString() == QLatin1String("admin");
    count();
    if (m == "PATCH" && seg.size() == 2) {   // rename
      edit(dirUsers, i, QStringLiteral("display_name"), body.value(QStringLiteral("display_name")).toString());
      respond(sock, 200, json(dirUsers.at(i).toObject()));
    } else if (m == "DELETE" && seg.size() == 2) {
      if (isAdmin) { err(403, QStringLiteral("forbidden"), QStringLiteral("Admins cannot be deleted")); return true; }
      dirUsers.removeAt(i);
      respond(sock, 204, {});
    } else if (action == QLatin1String("disable")) {
      if (isAdmin) { err(403, QStringLiteral("forbidden"), QStringLiteral("Admins cannot be disabled")); return true; }
      edit(dirUsers, i, QStringLiteral("disabled"), true);
      respond(sock, 200, json(dirUsers.at(i).toObject()));
    } else if (action == QLatin1String("enable")) {
      edit(dirUsers, i, QStringLiteral("disabled"), false);
      respond(sock, 200, json(dirUsers.at(i).toObject()));
    } else if (action == QLatin1String("role")) {
      const QString role = body.value(QStringLiteral("role")).toString();
      int admins = 0;
      for (const QJsonValue& v : std::as_const(dirUsers)) admins += v.toObject().value(QStringLiteral("role")).toString() == QLatin1String("admin") ? 1 : 0;
      if (role == QLatin1String("user") && isAdmin && (admins <= 1 || id == QLatin1String("u_test_1"))) {
        err(403, QStringLiteral("forbidden"), QStringLiteral("Make someone else admin first."));
        return true;
      }
      if (role == QLatin1String("admin") && body.value(QStringLiteral("password")).toString().size() < 12) {
        err(400, QStringLiteral("bad_request"), QStringLiteral("Password must have at least 12 characters"));
        return true;
      }
      edit(dirUsers, i, QStringLiteral("role"), role);
      respond(sock, 200, json(dirUsers.at(i).toObject()));
    } else if (action == QLatin1String("password-reset")) {
      respond(sock, 200, json({{QStringLiteral("temporary_password"), QStringLiteral("PLCH-OLDR-PASS-WORD")}, {QStringLiteral("must_change_password"), true}}));
    } else if (action == QLatin1String("code")) {
      respond(sock, 201, json({{QStringLiteral("code"), QStringLiteral("UZ-ABCD-2345")},
                               {QStringLiteral("expires_at"), QDateTime::currentDateTimeUtc().addDays(1).toString(Qt::ISODate)}}));
    } else {
      err(404, QStringLiteral("not_found"), QStringLiteral("Unknown route"));
    }
    return true;
  }
  if (kind == QLatin1String("devices")) {
    count();
    if (seg.size() == 1 && m == "GET") {
      respond(sock, 200, json({{QStringLiteral("devices"), dirDevices}, {QStringLiteral("min_protocol_version"), 1}}));
      return true;
    }
    const int i = find(dirDevices, QStringLiteral("id"), id);
    if (i < 0) { err(404, QStringLiteral("not_found"), QStringLiteral("Unknown device")); return true; }
    if (m == "PATCH" && seg.size() == 2) {   // rename
      edit(dirDevices, i, QStringLiteral("name"), body.value(QStringLiteral("name")).toString());
      respond(sock, 200, json(dirDevices.at(i).toObject()));
      return true;
    }
    if (action == QLatin1String("revoke")) {
      edit(dirDevices, i, QStringLiteral("status"), QStringLiteral("revoked"));
      edit(dirDevices, i, QStringLiteral("online"), false);
      respond(sock, 200, json(dirDevices.at(i).toObject()));
      return true;
    }
    err(404, QStringLiteral("not_found"), QStringLiteral("Unknown route"));
    return true;
  }
  if (kind == QLatin1String("invites")) {
    count();
    if (seg.size() == 1 && m == "GET") {
      respond(sock, 200, json({{QStringLiteral("invites"), dirInvites}}));
    } else if (seg.size() == 1 && m == "POST") {
      const QJsonObject inv{{QStringLiteral("id"), QStringLiteral("i_new")}, {QStringLiteral("status"), QStringLiteral("active")},
                            {QStringLiteral("authorize_device"), body.value(QStringLiteral("authorize_device")).toBool()},
                            {QStringLiteral("created_at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                            {QStringLiteral("expires_at"), QDateTime::currentDateTimeUtc().addDays(7).toString(Qt::ISODate)}};
      dirInvites.prepend(inv);
      respond(sock, 201, json({{QStringLiteral("invite"), inv},
                               {QStringLiteral("code"), QJsonObject{{QStringLiteral("code"), QStringLiteral("UZ-8XK2-M4QD")},
                                                                    {QStringLiteral("expires_at"), inv.value(QStringLiteral("expires_at"))}}}}));
    } else if (m == "DELETE") {
      const int i = find(dirInvites, QStringLiteral("id"), id);
      if (i < 0 || dirInvites.at(i).toObject().value(QStringLiteral("status")).toString() != QLatin1String("active")) {
        err(404, QStringLiteral("not_found"), QStringLiteral("Unknown invite"));
        return true;
      }
      edit(dirInvites, i, QStringLiteral("status"), QStringLiteral("revoked"));
      respond(sock, 204, {});
    } else {
      err(404, QStringLiteral("not_found"), QStringLiteral("Unknown route"));
    }
    return true;
  }
  if (kind == QLatin1String("pairing-requests")) {
    count();
    if (seg.size() == 1 && m == "GET") {
      respond(sock, 200, json({{QStringLiteral("requests"), dirRequests}, {QStringLiteral("min_protocol_version"), 1}}));
      return true;
    }
    const int i = find(dirRequests, QStringLiteral("id"), id);
    if (i < 0) { err(409, QStringLiteral("conflict"), QStringLiteral("The request is no longer open")); return true; }
    if (action == QLatin1String("allow") && find(dirUsers, QStringLiteral("id"), body.value(QStringLiteral("user_id")).toString()) < 0) {
      err(400, QStringLiteral("bad_request"), QStringLiteral("Unknown user"));
      return true;
    }
    dirRequests.removeAt(i);
    respond(sock, 204, {});
    return true;
  }
  if (kind == QLatin1String("reports")) {
    count();
    if (seg.size() == 1 && m == "GET") {
      const QString want = req.path.contains(QLatin1String("status=new")) ? QStringLiteral("new") : req.path.contains(QLatin1String("status=seen")) ? QStringLiteral("seen") : QString();
      QJsonArray out;
      int cNew = 0, cSeen = 0;
      for (const QJsonValue& v : std::as_const(dirReports)) {
        QJsonObject o = v.toObject();
        const QString st = o.value(QStringLiteral("status")).toString();
        (st == QLatin1String("new") ? cNew : cSeen)++;
        if (!want.isEmpty() && st != want) continue;
        o.remove(QStringLiteral("description"));
        o.remove(QStringLiteral("files"));
        out.append(o);
      }
      respond(sock, 200, json({{QStringLiteral("reports"), out}, {QStringLiteral("count_all"), cNew + cSeen}, {QStringLiteral("count_new"), cNew},
                               {QStringLiteral("count_seen"), cSeen}}));
      return true;
    }
    const int i = find(dirReports, QStringLiteral("id"), id);
    if (i < 0) { err(404, QStringLiteral("not_found"), QStringLiteral("Unknown report")); return true; }
    if (seg.size() == 2 && m == "GET") {
      respond(sock, 200, json(dirReports.at(i).toObject()));
    } else if (seg.size() == 2 && m == "DELETE") {
      dirReports.removeAt(i);
      respond(sock, 204, {});
    } else if (action == QLatin1String("seen")) {
      edit(dirReports, i, QStringLiteral("status"), QStringLiteral("seen"));
      respond(sock, 200, json(dirReports.at(i).toObject()));
    } else if (action == QLatin1String("files")) {
      const QString name = QUrl::fromPercentEncoding(seg.value(3).toUtf8());
      if (!reportFiles.contains(id + QLatin1Char('/') + name)) { err(404, QStringLiteral("not_found"), QStringLiteral("Unknown file")); return true; }
      respond(sock, 200, reportFiles.value(id + QLatin1Char('/') + name), "application/octet-stream");
    } else if (action == QLatin1String("archive")) {
      respond(sock, 200, QByteArray("PK-fake-zip-of-report-") + id.toUtf8(), "application/zip");
    } else {
      err(404, QStringLiteral("not_found"), QStringLiteral("Unknown route"));
    }
    return true;
  }
  return false;
}

void FakeHub::handle(QSslSocket* sock, const FakeRequest& req) {
  const auto json = [](const QJsonObject& o) { return QJsonDocument(o).toJson(QJsonDocument::Compact); };
  const QJsonObject body = QJsonDocument::fromJson(req.body).object();

  if (req.method == "GET" && req.path == QLatin1String("/.well-known/uzume")) {
    respond(sock, 200, json({{QStringLiteral("hub_id"), hubId},
                             {QStringLiteral("name"), name},
                             {QStringLiteral("hub_version"), QStringLiteral("0.1.0")},
                             {QStringLiteral("protocol_version"), protocolVersion},
                             {QStringLiteral("min_protocol_version"), minProtocolVersion},
                             {QStringLiteral("api_base"), QStringLiteral("/api/v1")},
                             {QStringLiteral("setup_required"), setupRequired}}));
  } else if (handleSignInAndSetup(sock, req)) {
    // answered (or held)
  } else if (req.method == "POST" && req.path == QLatin1String("/api/v1/pairing/requests")) {
    if (rateLimitPairing) {
      respondError(sock, 429, QStringLiteral("rate_limited"));
      return;
    }
    approvedDelivered_ = false;
    respond(sock, 202, json({{QStringLiteral("request_id"), QStringLiteral("req-1")},
                             {QStringLiteral("poll_token"), QString::fromLatin1(kPollToken)},
                             {QStringLiteral("status"), QStringLiteral("pending")},
                             {QStringLiteral("expires_in"), 600}}));
  } else if (req.method == "GET" && req.path == QLatin1String("/api/v1/pairing/requests/req-1")) {
    if (!bearerIs(req, kPollToken)) {
      respondError(sock, 401, QStringLiteral("unauthorized"));
      return;
    }
    switch (decision) {
      case Decision::Pending: respond(sock, 200, json({{QStringLiteral("status"), QStringLiteral("pending")}})); break;
      case Decision::Deny: respond(sock, 200, json({{QStringLiteral("status"), QStringLiteral("denied")}})); break;
      case Decision::Expire: respond(sock, 200, json({{QStringLiteral("status"), QStringLiteral("expired")}})); break;
      case Decision::Approve:
        if (approvedDelivered_) {
          respondError(sock, 404, QStringLiteral("not_found"));
        } else {
          approvedDelivered_ = true;  // credential exactly once
          respond(sock, 200, json({{QStringLiteral("status"), QStringLiteral("approved")},
                                   {QStringLiteral("hub_id"), hubId},
                                   {QStringLiteral("user_id"), QStringLiteral("u_test_1")},
                                   {QStringLiteral("device_credential"), QString::fromLatin1(kDeviceCredential)}}));
        }
        break;
    }
  } else if (req.method == "POST" && req.path == QLatin1String("/api/v1/invites/redeem")) {
    lastInviteBody = body;
    if (rateLimitInvites) {
      respondError(sock, 429, QStringLiteral("rate_limited"));
      return;
    }
    if (body.value(QStringLiteral("code")).toString() != inviteCode) {
      respondError(sock, 404, QStringLiteral("invite_invalid"));
      return;
    }
    const QString dn = body.value(QStringLiteral("display_name")).toString();
    for (const QString& t : std::as_const(takenNames)) {
      if (t.compare(dn, Qt::CaseInsensitive) == 0) {
        respondError(sock, 409, QStringLiteral("display_name_taken"));
        return;
      }
    }
    approvedDelivered_ = false;
    if (inviteDirect) {
      respond(sock, 200, json({{QStringLiteral("status"), QStringLiteral("approved")},
                               {QStringLiteral("hub_id"), hubId},
                               {QStringLiteral("user_id"), QStringLiteral("u_invited_1")},
                               {QStringLiteral("device_credential"), QString::fromLatin1(kDeviceCredential)}}));
    } else {
      respond(sock, 202, json({{QStringLiteral("request_id"), QStringLiteral("req-1")},
                               {QStringLiteral("poll_token"), QString::fromLatin1(kPollToken)},
                               {QStringLiteral("status"), QStringLiteral("pending")},
                               {QStringLiteral("expires_in"), 600}}));
    }
  } else if (req.method == "GET" && req.path == QLatin1String("/api/v1/local/status")) {
    respond(sock, 200, json({{QStringLiteral("admin_exists"), localAdminExists},
                             {QStringLiteral("network_sharing"), localNetworkSharing},
                             {QStringLiteral("import_dir"), localImportDir},
                             {QStringLiteral("child"), localChild},
                             {QStringLiteral("hub_version"), QStringLiteral("0.9.0")}}));
  } else if (req.method == "POST" && (req.path == QLatin1String("/api/v1/local/setup") || req.path == QLatin1String("/api/v1/local/pair"))) {
    lastLocalBody = body;
    const bool setup = req.path.endsWith(QLatin1String("/setup"));
    (setup ? localSetupCalls : localPairCalls)++;
    const auto admin = accountsByUser.constFind(QStringLiteral("u_admin_1"));
    const QString expected = admin != accountsByUser.constEnd() ? admin->password : QStringLiteral("correct horse");
    if (setup && localAdminExists) {
      respondError(sock, 409, QStringLiteral("admin_exists"));
    } else if (!setup && body.value(QStringLiteral("password")).toString() != expected) {
      respondError(sock, 401, QStringLiteral("invalid_credentials"));
    } else {
      if (setup) {
        localAdminExists = true;
        FakeAccount a;
        a.userId = QStringLiteral("u_admin_1");
        a.username = body.value(QStringLiteral("username")).toString();
        a.displayName = a.username;
        a.role = QStringLiteral("admin");
        a.password = body.value(QStringLiteral("password")).toString();
        accountsByUser.insert(a.userId, a);
      }
      deviceUsers.insert(body.value(QStringLiteral("device_id")).toString(), QStringLiteral("u_admin_1"));
      approvedDelivered_ = true;
      respond(sock, 200, json({{QStringLiteral("status"), QStringLiteral("approved")},
                               {QStringLiteral("hub_id"), hubId},
                               {QStringLiteral("user_id"), QStringLiteral("u_admin_1")},
                               {QStringLiteral("device_credential"), QString::fromLatin1(kDeviceCredential)}}));
    }
  } else if (req.method == "POST" && req.path == QLatin1String("/api/v1/auth/token")) {
    ++tokenRequests_;
    callerDeviceId = body.value(QStringLiteral("device_id")).toString();
    if (userDisabled) {
      respondError(sock, 401, QStringLiteral("user_disabled"));
    } else if (revoked) {
      respondError(sock, 401, QStringLiteral("device_revoked"));
    } else if (body.value(QStringLiteral("device_credential")).toString().toUtf8() != kDeviceCredential) {
      respondError(sock, 401, QStringLiteral("invalid_credentials"));
    } else {
      const QByteArray token = "fba_TEST_" + QByteArray::number(tokenRequests_);
      issuedAccessTokens.append(token);
      respond(sock, 200, json({{QStringLiteral("access_token"), QString::fromLatin1(token)},
                               {QStringLiteral("token_type"), QStringLiteral("Bearer")},
                               {QStringLiteral("expires_in"), tokenLifetime}}));
    }
  } else if (req.method == "POST" && req.path == QLatin1String("/api/v1/auth/revoke")) {
    if (!bearerIs(req, "fba_")) {
      respondError(sock, 401, QStringLiteral("unauthorized"));
      return;
    }
    revoked = true;
    respond(sock, 204, {});
  } else if (!bearerIs(req, "fba_")) {
    respondError(sock, 401, QStringLiteral("unauthorized"));
  } else if (req.path.startsWith(QLatin1String("/api/v1/admin/")) || req.path.startsWith(QLatin1String("/api/v1/account/confirm"))) {
    // "Confirm it's you" belongs to the admin fake; the other /account paths are handled by handleAccount().
    handleAdmin(sock, req);
  } else if (req.method == "PUT" && req.path == QLatin1String("/api/v1/local/settings")) {
    lastLocalSettings = body;
    if (body.contains(QStringLiteral("import_dir")) && !localImportDirError.isEmpty()) {
      respond(sock, 400, json({{QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), QStringLiteral("import_dir_unreadable")},
                                                                    {QStringLiteral("message"), localImportDirError}}}}));
      return;
    }
    if (localChild && body.contains(QStringLiteral("network_sharing"))) {
      respondError(sock, 409, QStringLiteral("child_mode"));
      return;
    }
    if (body.contains(QStringLiteral("network_sharing"))) localNetworkSharing = body.value(QStringLiteral("network_sharing")).toBool();
    if (body.contains(QStringLiteral("import_dir"))) localImportDir = body.value(QStringLiteral("import_dir")).toString();
    respond(sock, 200, json({{QStringLiteral("admin_exists"), localAdminExists},
                             {QStringLiteral("network_sharing"), localNetworkSharing},
                             {QStringLiteral("import_dir"), localImportDir},
                             {QStringLiteral("hub_version"), QStringLiteral("0.9.0")}}));
  } else if (req.method == "POST" && req.path == QLatin1String("/api/v1/handshake")) {
    lastHandshakeBody = body;
    QJsonObject res{{QStringLiteral("hub_version"), QStringLiteral("0.1.0")},
                    {QStringLiteral("protocol_version"), protocolVersion},
                    {QStringLiteral("min_protocol_version"), minProtocolVersion},
                    {QStringLiteral("compatible"), true},
                    {QStringLiteral("problems"), QJsonArray()}};
    QJsonArray feats;
    for (const QString& f : features) {
      feats.append(f);
    }
    if (adminUser) {
      for (const QString& f : {QStringLiteral("admin_v1"), QStringLiteral("accounts_v1")}) {
        if (!features.contains(f)) feats.append(f);
      }
      res.insert(QStringLiteral("user"), QJsonObject{{QStringLiteral("id"), QStringLiteral("u_test_1")},
                                                      {QStringLiteral("display_name"), QStringLiteral("Tester")},
                                                      {QStringLiteral("role"), QStringLiteral("admin")}});
    }
    res.insert(QStringLiteral("features"), feats);
    if (const auto du = deviceUsers.constFind(callerDeviceId); du != deviceUsers.constEnd() && accountsByUser.contains(*du)) {
      const FakeAccount& a = accountsByUser[*du];
      res.insert(QStringLiteral("user"), QJsonObject{{QStringLiteral("id"), a.userId},
                                                     {QStringLiteral("display_name"), a.displayName},
                                                     {QStringLiteral("role"), a.role}});
    }
    for (auto it = handshakeExtra.begin(); it != handshakeExtra.end(); ++it) {
      res.insert(it.key(), it.value());
    }
    respond(sock, 200, json(res));
  } else if (req.path == QLatin1String("/api/v1/ws")) {
    upgradeWs(sock, req);
  } else if (req.path == QLatin1String("/api/v1/sessions") || req.path.startsWith(QLatin1String("/api/v1/sessions/")) ||
             req.path == QLatin1String("/api/v1/users")) {
    handleSessions(sock, req);
  } else if (req.path.startsWith(QLatin1String("/api/v1/runs/")) && req.path.contains(QLatin1String("/screenshots")) && handleScreenshots(sock, req)) {
    // answered
  } else if (req.path == QLatin1String("/api/v1/runs") || req.path.startsWith(QLatin1String("/api/v1/runs/"))) {
    handleRuns(sock, req);
  } else if (req.method == "POST" && req.path == QLatin1String("/api/v1/reports")) {
    if (reportUnauthorizedNext > 0) {
      --reportUnauthorizedNext;
      respondError(sock, 401, QStringLiteral("unauthorized"));
      return;
    }
    if (holdReports) {
      ++reportsHeld;
      return;
    }
    if (reportDropNext > 0) {
      --reportDropNext;
      sock->abort();
      return;
    }
    if (reportFailNext > 0) {
      --reportFailNext;
      respondError(sock, reportFailStatus, reportFailStatus == 429 ? QStringLiteral("rate_limited") : reportFailStatus == 413 ? QStringLiteral("payload_too_large") : QStringLiteral("internal"));
      return;
    }
    // multipart/form-data: split on the boundary of the Content-Type header.
    const QByteArray ctype = req.headers.value(QStringLiteral("content-type"));
    const qsizetype bi = ctype.indexOf("boundary=");
    if (!ctype.startsWith("multipart/form-data") || bi < 0) {
      respondError(sock, 400, QStringLiteral("bad_request"));
      return;
    }
    const QByteArray delim = "--" + ctype.mid(bi + 9);
    ReceivedReport rep;
    rep.authorization = req.headers.value(QStringLiteral("authorization"));
    bool first = true;
    qsizetype pos = req.body.indexOf(delim);
    while (pos >= 0) {
      pos += delim.size();
      if (req.body.mid(pos, 2) == "--") break;
      pos += 2;  // CRLF
      const qsizetype hdrEnd = req.body.indexOf("\r\n\r\n", pos);
      const qsizetype next = req.body.indexOf("\r\n" + delim, hdrEnd);
      if (hdrEnd < 0 || next < 0) {
        respondError(sock, 400, QStringLiteral("bad_request"));
        return;
      }
      const QByteArray headers = req.body.mid(pos, hdrEnd - pos);
      const QByteArray content = req.body.mid(hdrEnd + 4, next - hdrEnd - 4);
      if (first) {
        if (!headers.contains("name=\"meta\"")) {
          respondError(sock, 400, QStringLiteral("bad_request"));
          return;
        }
        rep.meta = QJsonDocument::fromJson(content).object();
        first = false;
      } else {
        const qsizetype fi = headers.indexOf("filename=\"");
        const QByteArray rest = headers.mid(fi + 10);
        rep.files.insert(QString::fromUtf8(rest.left(rest.indexOf('"'))), content);
      }
      pos = next + 2;
    }
    reports.append(rep);
    respond(sock, 201, json({{QStringLiteral("id"), reports.size()}}));
  } else if (req.path == QLatin1String("/api/v1/account") || req.path.startsWith(QLatin1String("/api/v1/account/"))) {
    handleAccount(sock, req);
  } else if (handleScreenshots(sock, req)) {
    // answered
  } else if (handleLocalRoms(sock, req)) {
    // answered
  } else if (handleCollections(sock, req)) {
    // answered
  } else if (req.path == QLatin1String("/api/v1/saves") || req.path.startsWith(QLatin1String("/api/v1/games/"))) {
    handleSaves(sock, req);
  } else if (req.method == "GET" && req.path == QLatin1String("/api/v1/games")) {
    respond(sock, 200, json(games));
  } else if (req.method == "POST" && req.path.startsWith(QLatin1String("/api/v1/games?"))) {
    const QUrlQuery q(req.path.mid(req.path.indexOf(QLatin1Char('?')) + 1));
    if (!uploadsAllowed) {
      respondError(sock, 403, QStringLiteral("uploads_disabled"));
      return;
    }
    const QString sha = QString::fromLatin1(QCryptographicHash::hash(req.body, QCryptographicHash::Sha256).toHex());
    const QString filename = q.queryItemValue(QStringLiteral("filename"), QUrl::FullyDecoded);
    const QString title = q.queryItemValue(QStringLiteral("title"), QUrl::FullyDecoded);
    QJsonArray list = games.value(QStringLiteral("games")).toArray();
    for (const QJsonValue& v : std::as_const(list)) {
      if (v.toObject().value(QStringLiteral("rom")).toObject().value(QStringLiteral("sha256")).toString() == sha) {
        QJsonObject e{{QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), QStringLiteral("conflict")},
                                                            {QStringLiteral("message"), QStringLiteral("This ROM is already in the library")}}},
                      {QStringLiteral("existing_game_id"), v.toObject().value(QStringLiteral("id")).toString()}};
        respond(sock, 409, json(e));
        return;
      }
    }
    uploads.append({filename, title, sha, req.body.size()});
    const QJsonObject game{{QStringLiteral("id"), QStringLiteral("up-%1").arg(uploads.size())},
                           {QStringLiteral("title"), title.isEmpty() ? QFileInfo(filename).completeBaseName() : title},
                           {QStringLiteral("system"), QStringLiteral("nds")},
                           {QStringLiteral("rom"), QJsonObject{{QStringLiteral("sha256"), sha},
                                                               {QStringLiteral("size"), req.body.size()},
                                                               {QStringLiteral("filename"), filename}}},
                           {QStringLiteral("uploaded_by"), QStringLiteral("u_test_1")},
                           {QStringLiteral("added_at"), QStringLiteral("2026-01-01T12:00:00Z")}};
    list.append(game);
    games.insert(QStringLiteral("games"), list);
    respond(sock, 201, json(game));
  } else if (req.method == "GET" && req.path.startsWith(QLatin1String("/api/v1/cores/"))) {
    // /api/v1/cores/<core>/packages/<version>/<platform>[/files/<name>]
    const QStringList parts = req.path.mid(14).split(QLatin1Char('/'));
    if (parts.size() < 4 || parts.at(1) != QLatin1String("packages")) {
      respondError(sock, 404, QStringLiteral("not_found"));
      return;
    }
    const QString key = parts.at(0) + QLatin1Char('/') + parts.at(2) + QLatin1Char('/') + parts.at(3);
    if (parts.size() == 4) {
      ++corePackageRequests;
      if (!corePackages.contains(key)) {
        respondError(sock, 404, QStringLiteral("core_package_not_found"));
        return;
      }
      respond(sock, 200, json(corePackages.value(key)));
    } else if (parts.size() == 6 && parts.at(4) == QLatin1String("files")) {
      const QString fkey = key + QLatin1Char('/') + parts.at(5);
      if (!coreFiles.contains(fkey)) {
        respondError(sock, 404, QStringLiteral("core_file_not_available"));
        return;
      }
      if (holdCoreFiles) {  // never answered: the test checks that the client drops the connection
        ++coreFilesHeld;
        connect(sock, &QSslSocket::disconnected, this, [this]() { ++coreFilesAbandoned; });
        return;
      }
      ++coreFileDownloads;
      respond(sock, 200, coreFiles.value(fkey), "application/octet-stream");
    } else {
      respondError(sock, 404, QStringLiteral("not_found"));
    }
  } else if (req.method == "GET" && req.path == QLatin1String("/api/v1/plugins")) {
    ++pluginListRequests;
    if (pluginListFails) {
      respondError(sock, 500, QStringLiteral("internal"));
      return;
    }
    respond(sock, 200, json({{QStringLiteral("plugins"), pluginList}}));
  } else if (req.method == "GET" && req.path.startsWith(QLatin1String("/api/v1/plugins/")) && req.path.endsWith(QLatin1String("/file"))) {
    // /api/v1/plugins/<id>/<version>/file
    const QStringList parts = req.path.mid(16).split(QLatin1Char('/'));
    const QString key = parts.size() == 3 ? parts.at(0) + QLatin1Char('/') + parts.at(1) : QString();
    if (key.isEmpty() || !pluginFiles.contains(key)) {
      respondError(sock, 404, QStringLiteral("plugin_not_found"));
      return;
    }
    ++pluginFileDownloads;
    respond(sock, 200, pluginFiles.value(key), "application/zip");
  } else if (req.method == "GET" && req.path == QLatin1String("/api/v1/systems")) {
    respond(sock, 200, json(systems));
  } else if (req.method == "GET" && req.path.startsWith(QLatin1String("/api/v1/systems/"))) {
    // /api/v1/systems/<system>/firmware/<file>
    const QStringList parts = req.path.mid(16).split(QLatin1Char('/'));
    if (parts.size() != 3 || parts.at(1) != QLatin1String("firmware") || !firmwareFiles.contains(parts.at(0) + QLatin1Char('/') + parts.at(2))) {
      respondError(sock, 404, QStringLiteral("not_found"));
      return;
    }
    ++firmwareDownloads;
    respond(sock, 200, firmwareFiles.value(parts.at(0) + QLatin1Char('/') + parts.at(2)), "application/octet-stream");
  } else if (req.method == "GET" && req.path.startsWith(QLatin1String("/api/v1/roms/"))) {
    const QByteArray data = roms.value(req.path.mid(13));
    if (data.isEmpty()) {
      respondError(sock, 404, QStringLiteral("not_found"));
      return;
    }
    const QByteArray range = req.headers.value(QStringLiteral("range"));
    if (!ignoreRange && range.startsWith("bytes=") && range.endsWith("-")) {
      const qint64 from = range.mid(6, range.size() - 7).toLongLong();
      if (from >= data.size()) {
        respondError(sock, 416, QStringLiteral("bad_request"));
        return;
      }
      respond(sock, 206, data.mid(static_cast<qsizetype>(from)), "application/octet-stream",
              {{"Content-Range", "bytes " + QByteArray::number(from) + "-" + QByteArray::number(data.size() - 1) + "/" +
                                     QByteArray::number(data.size())}});
      return;
    }
    qint64 cut = -1;
    if (truncateFirstRomAt >= 0 && !truncated_) {
      truncated_ = true;
      cut = truncateFirstRomAt;
    }
    respond(sock, 200, data, "application/octet-stream", {}, cut);
  } else {
    respondError(sock, 404, QStringLiteral("not_found"));
  }
}

// ---------------------------------------------------------------- saves

void FakeHub::setHubSave(const QString& gameId, const QByteArray& content, const QString& deviceId, const QString& deviceName) {
  FakeSlot& s = saves[gameId];
  s.revision += 1;
  s.content = content;
  s.deviceId = deviceId;
  s.deviceName = deviceName;
  s.reason = QStringLiteral("checkpoint");
}

void FakeHub::addHistory(const QString& gameId, const QString& slot, const QByteArray& content, const QString& reason, const QString& label,
                         const QString& deviceName) {
  FakeSlot& s = saves[slotKey(gameId, slot)];
  FakeVersion v;
  v.version = s.nextVersion++;
  v.revision = std::max(1, s.revision);
  v.content = content;
  v.deviceId = QStringLiteral("other-device");
  v.deviceName = deviceName;
  v.reason = reason;
  v.label = label;
  s.history.append(v);
}

QJsonObject FakeHub::versionJson(const FakeVersion& v) const {
  return {{QStringLiteral("version"), v.version},
          {QStringLiteral("revision"), v.revision},
          {QStringLiteral("sha256"), QString::fromLatin1(QCryptographicHash::hash(v.content, QCryptographicHash::Sha256).toHex())},
          {QStringLiteral("size"), v.content.size()},
          {QStringLiteral("device_id"), v.deviceId},
          {QStringLiteral("device_name"), v.deviceName},
          {QStringLiteral("created_at"), QStringLiteral("2026-01-01T12:%1:00Z").arg(v.version % 60, 2, 10, QLatin1Char('0'))},
          {QStringLiteral("reason"), v.reason},
          {QStringLiteral("label"), v.label.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(v.label)}};
}

QJsonObject FakeHub::slotJson(const QString& key, const FakeSlot& s) const {
  const QString gameId = key.section(QLatin1Char('/'), 0, 0);
  const QString slot = key.contains(QLatin1Char('/')) ? key.section(QLatin1Char('/'), 1) : QStringLiteral("default");
  QJsonArray conflicts;
  for (const FakeConflict& c : s.conflicts) {
    if (c.status == QLatin1String("open")) {
      conflicts.append(conflictJson(key, s, c));
    }
  }
  return {{QStringLiteral("game_id"), gameId},
          {QStringLiteral("slot"), slot},
          {QStringLiteral("current"),
           QJsonObject{{QStringLiteral("revision"), s.revision},
                       {QStringLiteral("sha256"), QString::fromLatin1(QCryptographicHash::hash(s.content, QCryptographicHash::Sha256).toHex())},
                       {QStringLiteral("size"), s.content.size()},
                       {QStringLiteral("device_id"), s.deviceId},
                       {QStringLiteral("device_name"), s.deviceName},
                       {QStringLiteral("created_at"), QStringLiteral("2026-01-01T12:00:00Z")},
                       {QStringLiteral("reason"), s.reason}}},
          {QStringLiteral("open_conflicts"), conflicts}};
}

QJsonObject FakeHub::conflictJson(const QString& key, const FakeSlot&, const FakeConflict& c) const {
  return {{QStringLiteral("id"), c.id},
          {QStringLiteral("game_id"), key.section(QLatin1Char('/'), 0, 0)},
          {QStringLiteral("slot"), key.contains(QLatin1Char('/')) ? key.section(QLatin1Char('/'), 1) : QStringLiteral("default")},
          {QStringLiteral("status"), c.status},
          {QStringLiteral("hub"), QJsonObject{{QStringLiteral("revision"), c.hubRevision},
                                              {QStringLiteral("sha256"), c.hubSha},
                                              {QStringLiteral("device_id"), c.hubDeviceId},
                                              {QStringLiteral("device_name"), c.hubDeviceName},
                                              {QStringLiteral("created_at"), QStringLiteral("2026-01-01T12:00:00Z")}}},
          {QStringLiteral("secured"),
           QJsonObject{{QStringLiteral("version"), c.securedVersion},
                       {QStringLiteral("sha256"), QString::fromLatin1(QCryptographicHash::hash(c.securedContent, QCryptographicHash::Sha256).toHex())},
                       {QStringLiteral("base_revision"), c.securedBase},
                       {QStringLiteral("device_id"), c.securedDeviceId},
                       {QStringLiteral("device_name"), c.securedDeviceName},
                       {QStringLiteral("created_at"), QStringLiteral("2026-01-01T12:05:00Z")}}},
          {QStringLiteral("created_at"), QStringLiteral("2026-01-01T12:05:00Z")}};
}

bool FakeHub::handleLocalRoms(QSslSocket* sock, const FakeRequest& req) {
  const auto json = [](const QJsonObject& o) { return QJsonDocument(o).toJson(QJsonDocument::Compact); };
  const QString P = QStringLiteral("/api/v1/");
  const auto gameWithSha = [this](const QString& sha) {
    for (const QJsonValue& v : games.value(QStringLiteral("games")).toArray()) {
      for (const QJsonValue& r : v.toObject().value(QStringLiteral("roms")).toArray()) {
        if (r.toObject().value(QStringLiteral("sha256")).toString() == sha) return v.toObject();
      }
      if (v.toObject().value(QStringLiteral("rom")).toObject().value(QStringLiteral("sha256")).toString() == sha) return v.toObject();
    }
    return QJsonObject();
  };
  if (req.method == "POST" && req.path == P + QStringLiteral("games/register")) {
    const QJsonObject body = QJsonDocument::fromJson(req.body).object();
    registerBodies.append(body);
    const QString sha = body.value(QStringLiteral("sha256")).toString();
    const QJsonObject existing = gameWithSha(sha);
    if (!existing.isEmpty()) {
      respond(sock, 200, json(existing));
      return true;
    }
    const QJsonObject rom{{QStringLiteral("sha256"), sha},
                          {QStringLiteral("size"), body.value(QStringLiteral("size"))},
                          {QStringLiteral("filename"), body.value(QStringLiteral("filename"))},
                          {QStringLiteral("stored"), false}};
    QJsonObject game{{QStringLiteral("id"), QStringLiteral("reg-%1").arg(registerBodies.size())},
                     {QStringLiteral("title"), QFileInfo(body.value(QStringLiteral("filename")).toString()).completeBaseName()},
                     {QStringLiteral("system"), body.value(QStringLiteral("system")).toString(QStringLiteral("nds"))},
                     {QStringLiteral("rom"), rom},
                     {QStringLiteral("roms"), QJsonArray{rom}},
                     {QStringLiteral("on_hub"), false},
                     {QStringLiteral("uploaded_by"), QStringLiteral("u_test_1")},
                     {QStringLiteral("added_at"), QStringLiteral("2026-01-01T12:00:00Z")}};
    if (body.contains(QStringLiteral("game_code"))) game.insert(QStringLiteral("game_code"), body.value(QStringLiteral("game_code")));
    if (body.contains(QStringLiteral("game_version"))) game.insert(QStringLiteral("game_version"), body.value(QStringLiteral("game_version")));
    QJsonArray list = games.value(QStringLiteral("games")).toArray();
    list.append(game);
    games.insert(QStringLiteral("games"), list);
    respond(sock, 201, json(game));
    return true;
  }
  if (req.method == "DELETE" && req.path.startsWith(P + QStringLiteral("games/")) && req.path.endsWith(QLatin1String("/registration"))) {
    const QString id = req.path.mid((P + QStringLiteral("games/")).size()).section(QLatin1Char('/'), 0, 0);
    unregisteredGames.append(id);
    QJsonArray out;
    for (const QJsonValue& v : games.value(QStringLiteral("games")).toArray()) {
      if (v.toObject().value(QStringLiteral("id")).toString() != id) out.append(v);
    }
    games.insert(QStringLiteral("games"), out);
    respond(sock, 204, {});
    return true;
  }
  if (req.path == P + QStringLiteral("uploads") && req.method == "POST") {
    const QJsonObject body = QJsonDocument::fromJson(req.body).object();
    const QString sha = body.value(QStringLiteral("sha256")).toString();
    const QJsonObject stored = gameWithSha(sha);
    if (!stored.isEmpty() && stored.value(QStringLiteral("on_hub")).toBool(true)) {
      respond(sock, 409, json({{QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), QStringLiteral("conflict")}}},
                               {QStringLiteral("existing_game_id"), stored.value(QStringLiteral("id")).toString()}}));
      return true;
    }
    for (const ResumableUpload& u : std::as_const(uploadSessions)) {
      if (u.sha256 == sha) {
        respond(sock, 201, json({{QStringLiteral("upload_id"), u.id}, {QStringLiteral("offset"), u.data.size()}, {QStringLiteral("chunk_size"), uploadChunkSize}}));
        return true;
      }
    }
    ResumableUpload u;
    u.id = QStringLiteral("up%1").arg(++uploadSessionsCreated);
    u.filename = body.value(QStringLiteral("filename")).toString();
    u.title = body.value(QStringLiteral("title")).toString();
    u.system = body.value(QStringLiteral("system")).toString();
    u.sha256 = sha;
    u.size = body.value(QStringLiteral("size")).toVariant().toLongLong();
    uploadSessions.insert(u.id, u);
    respond(sock, 201, json({{QStringLiteral("upload_id"), u.id}, {QStringLiteral("offset"), 0}, {QStringLiteral("chunk_size"), uploadChunkSize}}));
    return true;
  }
  if (req.path.startsWith(P + QStringLiteral("uploads/"))) {
    const QString id = req.path.mid((P + QStringLiteral("uploads/")).size());
    if (!uploadSessions.contains(id)) {
      respondError(sock, 404, QStringLiteral("not_found"));
      return true;
    }
    ResumableUpload& u = uploadSessions[id];
    if (req.method == "GET") {
      respond(sock, 200, json({{QStringLiteral("upload_id"), id}, {QStringLiteral("offset"), u.data.size()}, {QStringLiteral("size"), u.size}}));
      return true;
    }
    if (req.method == "DELETE") {
      uploadSessions.remove(id);
      respond(sock, 204, {});
      return true;
    }
    if (req.method == "PUT") {
      const QString range = QString::fromLatin1(req.headers.value(QStringLiteral("content-range")));
      putRanges.append(range);
      if (failPutNumber == putRanges.size()) {
        respondError(sock, 503, QStringLiteral("internal"));
        return true;
      }
      // "bytes a-b/total"
      const QString spec = range.mid(6);
      const qint64 a = spec.section(QLatin1Char('-'), 0, 0).toLongLong();
      if (a != u.data.size()) {
        respond(sock, 409, json({{QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), QStringLiteral("offset_mismatch")}}}, {QStringLiteral("offset"), u.data.size()}}));
        return true;
      }
      u.data.append(req.body);
      if (u.data.size() < u.size) {
        respond(sock, 200, json({{QStringLiteral("offset"), u.data.size()}}));
        return true;
      }
      const QString sha = QString::fromLatin1(QCryptographicHash::hash(u.data, QCryptographicHash::Sha256).toHex());
      if (sha != u.sha256) {
        respondError(sock, 400, QStringLiteral("hash_mismatch"));
        return true;
      }
      roms.insert(sha, u.data);
      QJsonObject rom{{QStringLiteral("sha256"), sha}, {QStringLiteral("size"), u.size}, {QStringLiteral("filename"), u.filename}, {QStringLiteral("stored"), true}};
      QJsonObject game = gameWithSha(sha);
      QJsonArray list;
      bool replaced = false;
      for (const QJsonValue& v : games.value(QStringLiteral("games")).toArray()) {
        if (!game.isEmpty() && v.toObject().value(QStringLiteral("id")) == game.value(QStringLiteral("id"))) {
          game.insert(QStringLiteral("rom"), rom);
          game.insert(QStringLiteral("roms"), QJsonArray{rom});
          game.insert(QStringLiteral("on_hub"), true);  // same game_id as the local-only game
          list.append(game);
          replaced = true;
        } else {
          list.append(v);
        }
      }
      if (!replaced) {
        game = QJsonObject{{QStringLiteral("id"), QStringLiteral("new-%1").arg(id)},
                           {QStringLiteral("title"), u.title.isEmpty() ? QFileInfo(u.filename).completeBaseName() : u.title},
                           {QStringLiteral("system"), u.system.isEmpty() ? QStringLiteral("nds") : u.system},
                           {QStringLiteral("rom"), rom},
                           {QStringLiteral("roms"), QJsonArray{rom}},
                           {QStringLiteral("on_hub"), true},
                           {QStringLiteral("uploaded_by"), QStringLiteral("u_test_1")},
                           {QStringLiteral("added_at"), QStringLiteral("2026-01-01T12:00:00Z")}};
        list.append(game);
      }
      games.insert(QStringLiteral("games"), list);
      const qint64 total = u.size;
      uploadSessions.remove(id);
      respond(sock, 200, json({{QStringLiteral("offset"), total}, {QStringLiteral("game"), game}}));
      return true;
    }
  }
  return false;
}

void FakeHub::handleSaves(QSslSocket* sock, const FakeRequest& req) {
  const auto json = [](const QJsonObject& o) { return QJsonDocument(o).toJson(QJsonDocument::Compact); };
  if (failSaveRequests > 0) {
    --failSaveRequests;
    respondError(sock, 503, QStringLiteral("internal"));
    return;
  }
  const QString sha256Hdr = QString::fromLatin1(req.headers.value(QStringLiteral("x-uzume-content-sha256")));
  if (req.path == QLatin1String("/api/v1/saves") && req.method == "GET") {
    QJsonArray arr;
    for (auto it = saves.cbegin(); it != saves.cend(); ++it) {
      QJsonObject o = slotJson(it.key(), it.value());
      o.insert(QStringLiteral("open_conflict_count"), o.value(QStringLiteral("open_conflicts")).toArray().size());
      o.remove(QStringLiteral("open_conflicts"));
      arr.append(o);
    }
    respond(sock, 200, json({{QStringLiteral("saves"), arr}}));
    return;
  }
  // /api/v1/games/{id}/saves/{slot}[/content|/conflicts/{cid}/resolve]
  const QString query = req.path.section(QLatin1Char('?'), 1);
  const QStringList parts = req.path.section(QLatin1Char('?'), 0, 0).split(QLatin1Char('/'), Qt::SkipEmptyParts);
  if (parts.size() < 6 || parts.at(4) != QLatin1String("saves")) {
    respondError(sock, 404, QStringLiteral("not_found"));
    return;
  }
  const QString gameId = parts.at(3);
  const QString slotName = parts.at(5);
  const QString key = slotKey(gameId, slotName);
  const bool exists = saves.contains(key) && saves.value(key).revision > 0;
  const bool sessionRunning = sessionRunningGames.contains(gameId);
  if (parts.size() == 7 && parts.at(6) == QLatin1String("rename") && req.method == "POST") {
    const QString newName = QJsonDocument::fromJson(req.body).object().value(QStringLiteral("name")).toString();
    const QString newKey = slotKey(gameId, newName);
    if (slotName == QLatin1String("default")) {
      respondError(sock, 409, QStringLiteral("save_slot_protected"));
    } else if (newName.isEmpty() || newName == slotName) {
      respondError(sock, 400, QStringLiteral("bad_request"));
    } else if (!exists) {
      respondError(sock, 404, QStringLiteral("not_found"));
    } else if (saves.contains(newKey) && saves.value(newKey).revision > 0) {
      respondError(sock, 409, QStringLiteral("save_slot_exists"));
    } else if (sessionRunning) {
      respondError(sock, 409, QStringLiteral("session_running"));
    } else {
      saves.insert(newKey, saves.take(key));
      ++renameCount;
      respond(sock, 200, json(slotJson(newKey, saves.value(newKey))));
    }
  } else if (parts.size() == 6 && req.method == "DELETE") {
    int confirm = -1;
    for (const QString& kv : query.split(QLatin1Char('&'), Qt::SkipEmptyParts)) {
      if (kv.startsWith(QLatin1String("confirm_revision="))) {
        confirm = kv.mid(17).toInt();
      }
    }
    if (slotName == QLatin1String("default") && !features.contains(QStringLiteral("saves_v6"))) {
      respondError(sock, 409, QStringLiteral("save_slot_protected"));
    } else if (confirm < 0) {
      respondError(sock, 400, QStringLiteral("bad_request"));
    } else if (!exists) {
      respondError(sock, 404, QStringLiteral("not_found"));
    } else if (sessionRunning) {
      respondError(sock, 409, QStringLiteral("session_running"));
    } else if (confirm != saves.value(key).revision) {
      respondError(sock, 409, QStringLiteral("save_conflict_stale"));
    } else {
      bool open = false;
      for (const FakeConflict& c : saves.value(key).conflicts) {
        open = open || c.status == QLatin1String("open");
      }
      if (open) {
        respondError(sock, 409, QStringLiteral("conflict"));
      } else {
        saves.remove(key);
        ++slotDeleteCount;
        respond(sock, 204, {});
      }
    }
  } else if (parts.size() == 6 && req.method == "GET") {
    exists ? respond(sock, 200, json(slotJson(key, saves.value(key)))) : respondError(sock, 404, QStringLiteral("not_found"));
  } else if (parts.size() == 7 && parts.at(6) == QLatin1String("content") && req.method == "GET") {
    if (!exists) {
      respondError(sock, 404, QStringLiteral("not_found"));
      return;
    }
    const FakeSlot& s = saves.value(key);
    const QByteArray sha = QCryptographicHash::hash(s.content, QCryptographicHash::Sha256).toHex();
    respond(sock, 200, s.content, "application/octet-stream",
            {{"ETag", "\"" + sha + "\""}, {"X-Uzume-Save-Revision", QByteArray::number(s.revision)}});
  } else if (parts.size() == 6 && req.method == "PUT") {
    const QString actual = QString::fromLatin1(QCryptographicHash::hash(req.body, QCryptographicHash::Sha256).toHex());
    if (actual != sha256Hdr) {
      respondError(sock, 400, QStringLiteral("bad_request"));
      return;
    }
    const int base = req.headers.value(QStringLiteral("x-uzume-base-revision")).toInt();
    FakeSlot& s = saves[key];
    const QString curSha = QString::fromLatin1(QCryptographicHash::hash(s.content, QCryptographicHash::Sha256).toHex());
    if (s.revision > 0 && curSha == actual) {
      respond(sock, 200, json(slotJson(key, s)));
    } else if (base == s.revision) {
      s.revision += 1;
      s.content = req.body;
      s.deviceId = callerDeviceId;
      s.deviceName = QStringLiteral("Test Device");
      s.reason = QString::fromLatin1(req.headers.value(QStringLiteral("x-uzume-sync-reason")));
      respond(sock, 200, json(slotJson(key, s)));
    } else {
      FakeConflict* c = nullptr;
      for (FakeConflict& x : s.conflicts) {
        if (x.status == QLatin1String("open") && x.securedDeviceId == callerDeviceId) {
          c = &x;
        }
      }
      if (c == nullptr) {
        s.conflicts.append(FakeConflict{});
        c = &s.conflicts.last();
        c->id = QStringLiteral("c%1").arg(s.conflicts.size());
      }
      c->hubRevision = s.revision;
      c->hubSha = curSha;
      c->hubDeviceId = s.deviceId;
      c->hubDeviceName = s.deviceName;
      c->securedContent = req.body;
      c->securedVersion = s.nextVersion++;
      c->securedBase = base;
      c->securedDeviceId = callerDeviceId;
      c->securedDeviceName = QStringLiteral("Test Device");
      respond(sock, 409,
              json({{QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), QStringLiteral("save_conflict")},
                                                          {QStringLiteral("message"), QStringLiteral("stale")}}},
                    {QStringLiteral("conflict"), conflictJson(key, s, *c)}}));
    }
  } else if (parts.size() == 7 && parts.at(6) == QLatin1String("history") && req.method == "GET") {
    if (!exists) {
      respondError(sock, 404, QStringLiteral("not_found"));
      return;
    }
    QJsonArray versions;
    const FakeSlot& s = saves.value(key);
    for (qsizetype k = s.history.size() - 1; k >= 0; --k) {
      versions.append(versionJson(s.history.at(k)));
    }
    respond(sock, 200, json({{QStringLiteral("versions"), versions}}));
  } else if (parts.size() == 7 && parts.at(6) == QLatin1String("snapshots") && req.method == "POST") {
    if (!exists) {
      respondError(sock, 404, QStringLiteral("not_found"));
      return;
    }
    FakeSlot& s = saves[key];
    FakeVersion v;
    v.version = s.nextVersion++;
    v.revision = s.revision;
    v.content = s.content;
    v.deviceId = s.deviceId;
    v.deviceName = s.deviceName;
    v.reason = QStringLiteral("manual_snapshot");
    v.label = QJsonDocument::fromJson(req.body).object().value(QStringLiteral("label")).toString();
    s.history.append(v);
    respond(sock, 201, json(versionJson(v)));
  } else if (parts.size() == 7 && parts.at(6) == QLatin1String("upload") && req.method == "POST") {
    if (req.body.size() > uploadLimit) {
      respondError(sock, 413, QStringLiteral("payload_too_large"));
      return;
    }
    const QString actual = QString::fromLatin1(QCryptographicHash::hash(req.body, QCryptographicHash::Sha256).toHex());
    if (req.body.isEmpty() || actual != sha256Hdr) {
      respondError(sock, 400, QStringLiteral("bad_request"));
      return;
    }
    if (sessionRunning) {
      respondError(sock, 409, QStringLiteral("session_running"));
      return;
    }
    FakeSlot& s = saves[key];
    if (req.headers.value(QStringLiteral("x-uzume-expected-revision")).toInt() != (exists ? s.revision : 0)) {
      respondError(sock, 409, QStringLiteral("save_conflict_stale"));
      return;
    }
    if (exists) {
      FakeVersion before;
      before.version = s.nextVersion++;
      before.revision = s.revision;
      before.content = s.content;
      before.deviceId = s.deviceId;
      before.deviceName = s.deviceName;
      before.reason = QStringLiteral("before_upload");
      s.history.append(before);
    }
    s.revision += 1;
    s.content = req.body;
    s.deviceId = callerDeviceId;
    s.deviceName = QStringLiteral("Test Device");
    s.reason = QStringLiteral("upload");
    ++uploadCount;
    respond(sock, 200, json(slotJson(key, s)));
  } else if (parts.size() == 8 && parts.at(6) == QLatin1String("history") && req.method == "DELETE") {
    if (!exists) {
      respondError(sock, 404, QStringLiteral("not_found"));
      return;
    }
    FakeSlot& s = saves[key];
    const int want = parts.at(7).toInt();
    qsizetype found = -1;
    for (qsizetype k = 0; k < s.history.size(); ++k) {
      if (s.history.at(k).version == want) {
        found = k;
      }
    }
    if (found < 0) {
      respondError(sock, 404, QStringLiteral("not_found"));
      return;
    }
    if (s.history.at(found).reason != QLatin1String("manual_snapshot") && !features.contains(QStringLiteral("saves_v6"))) {
      respondError(sock, 409, QStringLiteral("save_not_snapshot"));
      return;
    }
    s.history.removeAt(found);
    ++deleteCount;
    respond(sock, 204, {});
  } else if (parts.size() == 9 && parts.at(6) == QLatin1String("history") && parts.at(8) == QLatin1String("restore") &&
             req.method == "POST") {
    if (!exists) {
      respondError(sock, 404, QStringLiteral("not_found"));
      return;
    }
    FakeSlot& s = saves[key];
    const int want = parts.at(7).toInt();
    const FakeVersion* src = nullptr;
    for (const FakeVersion& v : s.history) {
      if (v.version == want) {
        src = &v;
      }
    }
    if (src == nullptr) {
      respondError(sock, 404, QStringLiteral("not_found"));
      return;
    }
    if (QJsonDocument::fromJson(req.body).object().value(QStringLiteral("expected_revision")).toInt() != s.revision) {
      respondError(sock, 409, QStringLiteral("save_conflict_stale"));
      return;
    }
    FakeVersion before;
    before.version = s.nextVersion++;
    before.revision = s.revision;
    before.content = s.content;
    before.deviceId = s.deviceId;
    before.deviceName = s.deviceName;
    before.reason = QStringLiteral("before_restore");
    const QByteArray restored = src->content;
    s.history.append(before);
    s.revision += 1;
    s.content = restored;
    s.deviceId = callerDeviceId;
    s.deviceName = QStringLiteral("Test Device");
    s.reason = QStringLiteral("restore");
    ++restoreCount;
    respond(sock, 200, json(slotJson(key, s)));
  } else if (parts.size() == 9 && parts.at(6) == QLatin1String("conflicts") && parts.at(8) == QLatin1String("resolve") &&
             req.method == "POST") {
    const QJsonObject body = QJsonDocument::fromJson(req.body).object();
    FakeSlot& s = saves[key];
    FakeConflict* c = nullptr;
    for (FakeConflict& x : s.conflicts) {
      if (x.id == parts.at(7) && x.status == QLatin1String("open")) {
        c = &x;
      }
    }
    if (c == nullptr || body.value(QStringLiteral("expected_revision")).toInt() != s.revision) {
      respondError(sock, 409, QStringLiteral("save_conflict_stale"));
      return;
    }
    if (body.value(QStringLiteral("resolution")).toString() == QLatin1String("use_local")) {
      s.revision += 1;
      s.content = c->securedContent;
      s.deviceId = c->securedDeviceId;
      s.deviceName = c->securedDeviceName;
      c->status = QStringLiteral("resolved_local");
    } else {
      c->status = QStringLiteral("resolved_hub");
    }
    respond(sock, 200, json(slotJson(key, s)));
  } else {
    respondError(sock, 404, QStringLiteral("not_found"));
  }
}

// ---------------------------------------------------------------- sessions (REST)

void FakeHub::handleSessions(QSslSocket* sock, const FakeRequest& req) {
  const auto json = [](const QJsonObject& o) { return QJsonDocument(o).toJson(QJsonDocument::Compact); };
  const QJsonObject body = QJsonDocument::fromJson(req.body).object();
  const QString path = req.path.mid(QStringLiteral("/api/v1").size());
  if (path == QLatin1String("/users") && req.method == "GET" && !fakeUsers.isEmpty()) {
    respond(sock, 200, json({{QStringLiteral("users"), fakeUsers}}));
    return;
  }
  if (path == QLatin1String("/users") && req.method == "GET") {
    respond(sock, 200, json({{QStringLiteral("users"), QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("u_test_1")},
                                                                               {QStringLiteral("display_name"), QStringLiteral("Tester")},
                                                                               {QStringLiteral("online"), true}}}}}));
    return;
  }
  if (path == QLatin1String("/sessions")) {
    if (req.method == "GET") {
      QJsonArray arr;
      for (const QJsonObject& s : std::as_const(sessions)) {
        arr.append(s);
      }
      respond(sock, 200, json({{QStringLiteral("sessions"), arr}}));
    } else if (req.method == "POST") {
      if (publishCapabilityMissing) {
        respondError(sock, 409, QStringLiteral("capability_missing"));
        return;
      }
      const QString id = QStringLiteral("00000000-0000-4000-8000-%1").arg(sessions.size() + 1, 12, 10, QLatin1Char('0'));
      QJsonObject s{{QStringLiteral("session_id"), id},
                    {QStringLiteral("game_id"), body.value(QStringLiteral("game_id"))},
                    {QStringLiteral("game_title"), QStringLiteral("Demo Homebrew")},
                    {QStringLiteral("owner"), QJsonObject{{QStringLiteral("user_id"), QStringLiteral("u_test_1")},
                                                          {QStringLiteral("display_name"), QStringLiteral("Tester")},
                                                          {QStringLiteral("device_name"), QStringLiteral("Test Device")}}},
                    {QStringLiteral("visibility"), body.value(QStringLiteral("visibility"))},
                    {QStringLiteral("created_at"), QStringLiteral("2026-01-01T12:00:00Z")},
                    {QStringLiteral("viewer_count"), 0},
                    {QStringLiteral("is_owner"), true},
                    {QStringLiteral("invited"), false}};
      sessions.insert(id, s);
      respond(sock, 201, json(s));
    } else {
      respondError(sock, 404, QStringLiteral("not_found"));
    }
    return;
  }
  const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);  // sessions, id, [sub, arg]
  if (parts.size() < 2 || parts.at(0) != QLatin1String("sessions")) {
    respondError(sock, 404, QStringLiteral("not_found"));
    return;
  }
  const QString id = parts.at(1);
  if (!sessions.contains(id)) {
    respondError(sock, 404, QStringLiteral("session_not_found"));
    return;
  }
  QJsonObject& s = sessions[id];
  if (parts.size() == 2) {
    if (req.method == "GET") {
      if (holdSessionGet) {
        heldGets_.append({sock, s});
      } else {
        respond(sock, 200, json(s));
      }
    } else if (req.method == "PATCH") {
      s.insert(QStringLiteral("visibility"), body.value(QStringLiteral("visibility")));
      respond(sock, 200, json(s));
    } else if (req.method == "DELETE") {
      sessions.remove(id);
      respond(sock, 204, {});
    }
  } else if (parts.at(2) == QLatin1String("join") && req.method == "POST") {
    if (joinFull) {
      respondError(sock, 409, QStringLiteral("session_full"));
      return;
    }
    QJsonObject joinResponse{{QStringLiteral("viewer_id"), QStringLiteral("11111111-1111-4111-8111-111111111111")},
                             {QStringLiteral("permissions"), QJsonObject{{QStringLiteral("view_video"), true},
                                                                         {QStringLiteral("hear_audio"), true},
                                                                         {QStringLiteral("send_input"), false}}},
                             {QStringLiteral("ice_servers"), QJsonArray{QStringLiteral("stun:stun.example.org:3478")}}};
    if (!joinTurnServers.isEmpty()) {
      joinResponse.insert(QStringLiteral("turn_servers"), joinTurnServers);
    }
    respond(sock, 201, json(joinResponse));
  } else if (parts.at(2) == QLatin1String("viewers") && req.method == "DELETE") {
    respond(sock, 204, {});
  } else if (parts.at(2) == QLatin1String("invites") && req.method == "PUT") {
    respond(sock, 200, json(s));
  } else if ((parts.at(2) == QLatin1String("invites") && req.method == "DELETE") ||
             (parts.at(2) == QLatin1String("decline") && req.method == "POST")) {
    respond(sock, 204, {});
  } else {
    respondError(sock, 404, QStringLiteral("not_found"));
  }
}

// ---------------------------------------------------------------- WSS (minimal RFC 6455 server side)

void FakeHub::upgradeWs(QSslSocket* sock, const FakeRequest& req) {
  lastWsAuthorization = req.headers.value(QStringLiteral("authorization"));
  const QByteArray key = req.headers.value(QStringLiteral("sec-websocket-key"));
  if (refuseWs || key.isEmpty()) {
    respondError(sock, 401, QStringLiteral("unauthorized"));
    return;
  }
  const QByteArray accept =
      QCryptographicHash::hash(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", QCryptographicHash::Sha1).toBase64();
  sock->write("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept +
              "\r\n\r\n");
  ++wsConnections;
  wsClients_.append(sock);
  wsBuffers_.insert(sock, {});
}

void FakeHub::wsWrite(QSslSocket* sock, quint8 opcode, const QByteArray& payload) {
  QByteArray f;
  f.append(static_cast<char>(0x80 | opcode));
  if (payload.size() < 126) {
    f.append(static_cast<char>(payload.size()));
  } else {
    f.append(static_cast<char>(126));
    f.append(static_cast<char>((payload.size() >> 8) & 0xFF));
    f.append(static_cast<char>(payload.size() & 0xFF));
  }
  sock->write(f + payload);
}

void FakeHub::onWsData(QSslSocket* sock) {
  QByteArray& buf = wsBuffers_[sock];
  while (buf.size() >= 2) {
    const quint8 b0 = static_cast<quint8>(buf[0]), b1 = static_cast<quint8>(buf[1]);
    qsizetype len = b1 & 0x7F, off = 2;
    if (len == 126) {
      if (buf.size() < 4) return;
      len = (static_cast<quint8>(buf[2]) << 8) | static_cast<quint8>(buf[3]);
      off = 4;
    }
    const bool masked = (b1 & 0x80) != 0;
    if (buf.size() < off + (masked ? 4 : 0) + len) return;
    QByteArray payload = buf.mid(off + (masked ? 4 : 0), len);
    if (masked) {
      for (qsizetype i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<char>(payload[i] ^ buf[off + (i % 4)]);
      }
    }
    buf.remove(0, off + (masked ? 4 : 0) + len);
    const quint8 op = b0 & 0x0F;
    if (op == 0x9) {
      wsWrite(sock, 0xA, payload);
    } else if (op == 0x8) {
      wsWrite(sock, 0x8, {});
      sock->disconnectFromHost();
      return;
    } else if (op == 0x1) {
      const QJsonObject env = QJsonDocument::fromJson(payload).object();
      wsReceived.append(env);
      if (env.value(QStringLiteral("type")).toString() == QLatin1String("hello")) {
        QJsonObject ackPayload{{QStringLiteral("protocol_version"), protocolVersion},
                               {QStringLiteral("hub_version"), QStringLiteral("0.1.0")},
                               {QStringLiteral("features"), wsFeatures()},
                               {QStringLiteral("ice_servers"), QJsonArray()}};
        if (!helloTurnServers.isEmpty()) {
          ackPayload.insert(QStringLiteral("turn_servers"), helloTurnServers);
        }
        const QJsonObject ack{{QStringLiteral("type"), QStringLiteral("hello_ack")}, {QStringLiteral("payload"), ackPayload}};
        wsWrite(sock, 0x1, QJsonDocument(ack).toJson(QJsonDocument::Compact));
        sendRunCatchUp(sock, env.value(QStringLiteral("payload")).toObject());
      }
    }
  }
}

void FakeHub::releaseHeldSessionGets(const QString& visibilityOverride) {
  const QList<HeldGet> held = heldGets_;
  heldGets_.clear();
  for (HeldGet h : held) {
    if (!h.sock) {
      continue;
    }
    if (!visibilityOverride.isEmpty()) {
      h.session.insert(QStringLiteral("visibility"), visibilityOverride);
    }
    respond(h.sock, 200, QJsonDocument(h.session).toJson(QJsonDocument::Compact));
  }
}

void FakeHub::sendWs(const QString& type, const QJsonObject& payload) {
  const QByteArray msg =
      QJsonDocument(QJsonObject{{QStringLiteral("type"), type}, {QStringLiteral("payload"), payload}}).toJson(QJsonDocument::Compact);
  for (QSslSocket* c : std::as_const(wsClients_)) {
    wsWrite(c, 0x1, msg);
  }
}

void FakeHub::closeWsClients() {
  const QList<QSslSocket*> list = wsClients_;
  for (QSslSocket* c : list) {
    c->abort();
  }
}

// ------------------------------------------------------------------ runs (runs_v1)

QString FakeHub::makeUuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

namespace {
QString nowIso() { return QStringLiteral("2026-10-12T10:00:00Z"); }
}  // namespace

qint64 FakeHub::addRunEvent(const QString& runId, const QString& type, const QString& text, const QString& userId, const QString& pluginId,
                            const QString& clientId, bool push) {
  QList<QJsonObject>& list = runEvents[runId];
  const qint64 seq = list.isEmpty() ? 1 : static_cast<qint64>(list.last().value(QStringLiteral("seq")).toDouble()) + 1;
  QString userName;
  QJsonObject runObj = runs.value(runId);
  for (const QJsonValue& v : runObj.value(QStringLiteral("participants")).toArray())
    if (v.toObject().value(QStringLiteral("user_id")).toString() == userId) userName = v.toObject().value(QStringLiteral("user_name")).toString();
  const auto nullable = [](const QString& s) { return s.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(s); };
  const QJsonObject e{{QStringLiteral("seq"), static_cast<double>(seq)},
                      {QStringLiteral("created_at"), nowIso()},
                      {QStringLiteral("user_id"), nullable(userId)},
                      {QStringLiteral("user_name"), userName},
                      {QStringLiteral("device_id"), userId.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(QStringLiteral("dev-1"))},
                      {QStringLiteral("plugin_id"), nullable(pluginId)},
                      {QStringLiteral("source"), userId.isEmpty() ? QStringLiteral("hub") : QStringLiteral("automatic")},
                      {QStringLiteral("type"), type},
                      {QStringLiteral("text"), text},
                      {QStringLiteral("payload"), QJsonObject()},
                      {QStringLiteral("client_id"), clientId.isEmpty() ? makeUuid() : clientId}};
  list.append(e);
  runObj.insert(QStringLiteral("last_seq"), static_cast<double>(seq));
  runs.insert(runId, runObj);
  if (push) sendWs(QStringLiteral("run_event"), {{QStringLiteral("run_id"), runId}, {QStringLiteral("event"), e}});
  return seq;
}

void FakeHub::pushRun(const QString& runId) {
  if (runs.contains(runId)) sendWs(QStringLiteral("run_updated"), {{QStringLiteral("run"), runs.value(runId)}});
}

QJsonObject FakeHub::makeRun(const QString& name, const QStringList& userIds, const QString& creator, const QJsonArray& plugins) {
  QJsonArray parts;
  int color = 0;
  for (const QString& u : userIds) {
    const bool joined = u == creator;
    parts.append(QJsonObject{{QStringLiteral("user_id"), u},
                             {QStringLiteral("user_name"), u == runsMe ? runsMeName : u},
                             {QStringLiteral("status"), joined ? QStringLiteral("joined") : QStringLiteral("invited")},
                             {QStringLiteral("game_id"), joined ? QJsonValue(QStringLiteral("00000000-0000-4000-8000-000000000001")) : QJsonValue(QJsonValue::Null)},
                             {QStringLiteral("color"), joined ? QJsonValue(color++) : QJsonValue(QJsonValue::Null)},
                             {QStringLiteral("states"), QJsonObject()}});
  }
  const QString id = makeUuid();
  const QJsonObject run{{QStringLiteral("run_id"), id},
                        {QStringLiteral("name"), name},
                        {QStringLiteral("status"), QStringLiteral("starting")},
                        {QStringLiteral("created_by"), creator},
                        {QStringLiteral("created_at"), nowIso()},
                        {QStringLiteral("updated_at"), nowIso()},
                        {QStringLiteral("status_by"), creator},
                        {QStringLiteral("save_slot"), QStringLiteral("run-") + QString(id).remove(QLatin1Char('-')).left(8)},  // runs_v2
                        {QStringLiteral("settings"), QJsonObject()},
                        {QStringLiteral("plugins"), plugins},
                        {QStringLiteral("participants"), parts},
                        {QStringLiteral("last_seq"), 0}};
  runs.insert(id, run);
  addRunEvent(id, QStringLiteral("run.created"), name, {}, {}, {}, false);
  return runs.value(id);
}

void FakeHub::sendRunCatchUp(QSslSocket* sock, const QJsonObject& hello) {
  if (!features.contains(QStringLiteral("runs_v1"))) return;
  const auto write = [&](const QString& type, const QJsonObject& payload) {
    wsWrite(sock, 0x1, QJsonDocument(QJsonObject{{QStringLiteral("type"), type}, {QStringLiteral("payload"), payload}}).toJson(QJsonDocument::Compact));
  };
  QMap<QString, qint64> listed;
  for (const QJsonValue& v : hello.value(QStringLiteral("runs")).toArray())
    listed.insert(v.toObject().value(QStringLiteral("run_id")).toString(), static_cast<qint64>(v.toObject().value(QStringLiteral("last_seq")).toDouble()));
  for (auto it = runs.begin(); it != runs.end(); ++it) write(QStringLiteral("run_updated"), {{QStringLiteral("run"), it.value()}});
  for (auto it = listed.begin(); it != listed.end(); ++it) {
    QList<QJsonObject> missing;
    for (const QJsonObject& e : runEvents.value(it.key()))
      if (static_cast<qint64>(e.value(QStringLiteral("seq")).toDouble()) > it.value()) missing.append(e);
    if (static_cast<int>(missing.size()) > runHelloCatchUpMax) continue;
    for (const QJsonObject& e : missing) write(QStringLiteral("run_event"), {{QStringLiteral("run_id"), it.key()}, {QStringLiteral("event"), e}});
  }
}

void FakeHub::handleRuns(QSslSocket* sock, const FakeRequest& req) {
  const auto json = [](const QJsonObject& o) { return QJsonDocument(o).toJson(QJsonDocument::Compact); };
  const QJsonObject body = QJsonDocument::fromJson(req.body).object();
  const QString full = req.path.mid(QStringLiteral("/api/v1/runs").size());
  const int q = full.indexOf(QLatin1Char('?'));
  const QUrlQuery query(q >= 0 ? full.mid(q + 1) : QString());
  const QStringList parts = (q >= 0 ? full.left(q) : full).split(QLatin1Char('/'), Qt::SkipEmptyParts);

  if (runFailNext > 0) {
    --runFailNext;
    respondError(sock, runFailStatus, QStringLiteral("internal"));
    return;
  }
  if (parts.isEmpty()) {
    if (req.method == "GET") {
      QJsonArray arr;
      for (const QJsonObject& r : std::as_const(runs)) arr.append(r);
      respond(sock, 200, json({{QStringLiteral("runs"), arr}}));
    } else if (req.method == "POST") {
      lastRunBody = body;
      QStringList users{runsMe};
      for (const QJsonValue& v : body.value(QStringLiteral("invite_user_ids")).toArray()) users << v.toString();
      QJsonArray plugins;
      for (const QJsonValue& v : body.value(QStringLiteral("plugins")).toArray()) {
        const QString pid = v.toObject().value(QStringLiteral("plugin_id")).toString();
        const QString ver = v.toObject().value(QStringLiteral("version")).toString();
        QString sha(64, QLatin1Char('0'));
        for (const QJsonValue& pv : std::as_const(pluginList))
          if (pv.toObject().value(QStringLiteral("plugin_id")).toString() == pid && pv.toObject().value(QStringLiteral("version")).toString() == ver)
            sha = pv.toObject().value(QStringLiteral("sha256")).toString();
        plugins.append(QJsonObject{{QStringLiteral("plugin_id"), pid}, {QStringLiteral("version"), ver}, {QStringLiteral("sha256"), sha}});
      }
      QJsonObject run = makeRun(body.value(QStringLiteral("name")).toString(), users, runsMe, plugins);
      run.insert(QStringLiteral("settings"), body.value(QStringLiteral("settings")).toObject());
      QJsonArray ps = run.value(QStringLiteral("participants")).toArray();
      QJsonObject me = ps.at(0).toObject();
      me.insert(QStringLiteral("game_id"), body.value(QStringLiteral("game_id")).toString());
      ps.replace(0, me);
      run.insert(QStringLiteral("participants"), ps);
      const QString id = run.value(QStringLiteral("run_id")).toString();
      runs.insert(id, run);
      pushRun(id);
      respond(sock, 201, json(runs.value(id)));
    } else {
      respondError(sock, 405, QStringLiteral("method_not_allowed"));
    }
    return;
  }
  const QString runId = parts.at(0);
  if (!runs.contains(runId)) {
    respondError(sock, 404, QStringLiteral("not_found"));
    return;
  }
  QJsonObject run = runs.value(runId);
  const QString sub = parts.value(1);
  const auto myIndex = [&]() {
    const QJsonArray ps = run.value(QStringLiteral("participants")).toArray();
    for (int i = 0; i < ps.size(); ++i)
      if (ps.at(i).toObject().value(QStringLiteral("user_id")).toString() == runsMe) return i;
    return -1;
  };
  const auto updateMe = [&](const std::function<void(QJsonObject&)>& f) {
    QJsonArray ps = run.value(QStringLiteral("participants")).toArray();
    const int i = myIndex();
    QJsonObject me = ps.at(i).toObject();
    f(me);
    ps.replace(i, me);
    run.insert(QStringLiteral("participants"), ps);
  };
  const QString status = run.value(QStringLiteral("status")).toString();
  const bool writable = status == QLatin1String("starting") || status == QLatin1String("active");
  if (sub.isEmpty() && req.method == "GET") {
    respond(sock, 200, json(run));
  } else if (sub == QLatin1String("join") && req.method == "POST") {
    lastRunBody = body;
    if (myIndex() < 0) {
      respondError(sock, 404, QStringLiteral("not_found"));
      return;
    }
    int color = 0;
    for (const QJsonValue& v : run.value(QStringLiteral("participants")).toArray())
      if (v.toObject().value(QStringLiteral("status")).toString() == QLatin1String("joined")) ++color;
    updateMe([&](QJsonObject& me) {
      me.insert(QStringLiteral("status"), QStringLiteral("joined"));
      me.insert(QStringLiteral("game_id"), body.value(QStringLiteral("game_id")).toString());
      me.insert(QStringLiteral("color"), color);
    });
    runs.insert(runId, run);
    addRunEvent(runId, QStringLiteral("run.joined"), runsMeName);
    pushRun(runId);
    respond(sock, 200, json(runs.value(runId)));
  } else if ((sub == QLatin1String("decline") || sub == QLatin1String("leave")) && req.method == "POST") {
    updateMe([&](QJsonObject& me) { me.insert(QStringLiteral("status"), sub == QLatin1String("decline") ? QStringLiteral("declined") : QStringLiteral("left")); });
    runs.insert(runId, run);
    addRunEvent(runId, sub == QLatin1String("decline") ? QStringLiteral("run.declined") : QStringLiteral("run.left"), runsMeName);
    pushRun(runId);
    respond(sock, 200, json(runs.value(runId)));
  } else if (sub == QLatin1String("invite") && req.method == "POST") {
    lastRunBody = body;
    QJsonArray ps = run.value(QStringLiteral("participants")).toArray();
    for (const QJsonValue& v : body.value(QStringLiteral("user_ids")).toArray())
      ps.append(QJsonObject{{QStringLiteral("user_id"), v.toString()}, {QStringLiteral("user_name"), v.toString()}, {QStringLiteral("status"), QStringLiteral("invited")},
                            {QStringLiteral("game_id"), QJsonValue(QJsonValue::Null)}, {QStringLiteral("color"), QJsonValue(QJsonValue::Null)},
                            {QStringLiteral("states"), QJsonObject()}});
    run.insert(QStringLiteral("participants"), ps);
    runs.insert(runId, run);
    pushRun(runId);
    respond(sock, 200, json(run));
  } else if (sub == QLatin1String("status") && req.method == "POST") {
    lastRunBody = body;
    const QString to = body.value(QStringLiteral("status")).toString();
    const bool ok = (to == QLatin1String("active") && (status == QLatin1String("starting") || status == QLatin1String("paused"))) ||
                    (to == QLatin1String("paused") && status == QLatin1String("active")) ||
                    ((to == QLatin1String("finished") || to == QLatin1String("abandoned")) &&
                     (status == QLatin1String("starting") || status == QLatin1String("active") || status == QLatin1String("paused")));
    if (!ok) {
      respondError(sock, 409, QStringLiteral("conflict"));
      return;
    }
    run.insert(QStringLiteral("status"), to);
    run.insert(QStringLiteral("status_by"), runsMe);
    runs.insert(runId, run);
    addRunEvent(runId, QStringLiteral("run.status"), to);
    pushRun(runId);
    respond(sock, 200, json(runs.value(runId)));
  } else if (sub == QLatin1String("state") && req.method == "PUT" && parts.size() == 3) {
    const QString pluginId = parts.value(2);
    lastRunBody = body;
    QJsonObject logged = body;
    logged.insert(QStringLiteral("plugin_id"), pluginId);
    runStatePuts.append(logged);
    if (!writable) {
      respondError(sock, 409, QStringLiteral("run_not_active"));
      return;
    }
    const auto stateOf = [&](const QJsonObject& me) { return me.value(QStringLiteral("states")).toObject().value(pluginId).toObject(); };
    if (runStateConflicts > 0) {
      --runStateConflicts;
      const QJsonObject me = run.value(QStringLiteral("participants")).toArray().at(myIndex()).toObject();
      respond(sock, 409, json({{QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), QStringLiteral("state_conflict")}, {QStringLiteral("message"), QStringLiteral("base_rev")}}},
                               {QStringLiteral("state_rev"), static_cast<double>(runConflictRev)},
                               {QStringLiteral("state"), stateOf(me).value(QStringLiteral("state")).toObject()}}));
      return;
    }
    qint64 rev = 0;
    updateMe([&](QJsonObject& me) {
      QJsonObject all = me.value(QStringLiteral("states")).toObject();
      const QJsonObject cur = all.value(pluginId).toObject();
      rev = (std::max)(static_cast<qint64>(cur.value(QStringLiteral("state_rev")).toDouble()), static_cast<qint64>(body.value(QStringLiteral("base_rev")).toDouble())) + 1;
      all.insert(pluginId, QJsonObject{{QStringLiteral("state"), body.value(QStringLiteral("state")).toObject()},
                                       {QStringLiteral("state_rev"), static_cast<double>(rev)},
                                       {QStringLiteral("updated_at"), QStringLiteral("2026-10-12T10:00:00Z")}});
      me.insert(QStringLiteral("states"), all);
    });
    runs.insert(runId, run);
    respond(sock, 200, json({{QStringLiteral("state_rev"), static_cast<double>(rev)}}));
  } else if (sub == QLatin1String("events") && req.method == "POST") {
    runEventPosts.append(body);
    if (!writable) {
      respondError(sock, 409, QStringLiteral("run_not_active"));
      return;
    }
    const QString clientId = body.value(QStringLiteral("client_id")).toString();
    for (const QJsonObject& e : runEvents.value(runId)) {
      if (e.value(QStringLiteral("client_id")).toString() == clientId) {
        respond(sock, 200, json({{QStringLiteral("seq"), e.value(QStringLiteral("seq"))}}));
        return;
      }
    }
    const qint64 seq = addRunEvent(runId, body.value(QStringLiteral("type")).toString(), body.value(QStringLiteral("text")).toString(), runsMe,
                                   body.value(QStringLiteral("plugin_id")).toString(), clientId, false);
    QJsonObject& stored = runEvents[runId].last();
    stored.insert(QStringLiteral("source"), body.value(QStringLiteral("source")).toString());
    stored.insert(QStringLiteral("payload"), body.value(QStringLiteral("payload")).toObject());
    // Push only after source and payload are patched in: a client that takes the WSS event must see the complete event.
    sendWs(QStringLiteral("run_event"), {{QStringLiteral("run_id"), runId}, {QStringLiteral("event"), stored}});
    if (runEventDropResponses > 0) {
      --runEventDropResponses;
      sock->abort();
      return;
    }
    respond(sock, 201, json({{QStringLiteral("seq"), static_cast<double>(seq)}}));
  } else if (sub == QLatin1String("events") && req.method == "GET") {
    const qint64 after = query.queryItemValue(QStringLiteral("after")).toLongLong();
    const int limit = query.hasQueryItem(QStringLiteral("limit")) ? query.queryItemValue(QStringLiteral("limit")).toInt() : 200;
    QJsonArray arr;
    for (const QJsonObject& e : runEvents.value(runId)) {
      if (static_cast<qint64>(e.value(QStringLiteral("seq")).toDouble()) <= after) continue;
      if (arr.size() >= limit) break;
      arr.append(e);
    }
    respond(sock, 200, json({{QStringLiteral("events"), arr}, {QStringLiteral("last_seq"), run.value(QStringLiteral("last_seq"))}}));
  } else if (sub == QLatin1String("boards") && req.method == "PUT" && parts.size() == 3) {
    ++runBoardPuts;
    if (!writable) {
      respondError(sock, 409, QStringLiteral("run_not_active"));
      return;
    }
    runBoards.insert(runId + QLatin1Char('/') + parts.at(2), body.value(QStringLiteral("tree")).toObject());
    respond(sock, 200, json({{QStringLiteral("rev"), runBoardPuts}}));
  } else {
    respondError(sock, 404, QStringLiteral("not_found"));
  }
}


// ---------------------------------------------------------------- Accounts (accounts_v1) and set-up with a code

void FakeHub::addAccount(const QString& username, const QString& password, const QString& role) {
  FakeAccount a;
  a.userId = QStringLiteral("u_") + username.toLower();
  a.username = username;
  a.displayName = username;
  a.role = role;
  a.password = password;
  a.passwordRequired = role == QLatin1String("admin");
  accountsByUser.insert(a.userId, a);
}

void FakeHub::respondRateLimited(QSslSocket* sock) {
  QJsonObject err{{QStringLiteral("code"), QStringLiteral("rate_limited")}, {QStringLiteral("message"), QStringLiteral("Too many attempts")}};
  if (rateLimitRetryAfter > 0) {
    err.insert(QStringLiteral("retry_after"), rateLimitRetryAfter);
  }
  respond(sock, 429, QJsonDocument(QJsonObject{{QStringLiteral("error"), err}}).toJson(QJsonDocument::Compact));
}

QJsonObject FakeHub::accountJson(const FakeAccount& a) const {
  return QJsonObject{{QStringLiteral("user_id"), a.userId},
                     {QStringLiteral("username"), a.username},
                     {QStringLiteral("display_name"), a.displayName},
                     {QStringLiteral("role"), a.role},
                     {QStringLiteral("has_password"), !a.password.isEmpty()},
                     {QStringLiteral("password_required"), a.passwordRequired || requirePasswords},
                     {QStringLiteral("must_change_password"), false},
                     {QStringLiteral("confirmed_until"), QJsonValue::Null},
                     {QStringLiteral("confirm_locked_until"), QJsonValue::Null}};
}

FakeHub::FakeAccount* FakeHub::callerAccount() {
  const auto du = deviceUsers.constFind(callerDeviceId);
  if (du != deviceUsers.constEnd() && accountsByUser.contains(*du)) {
    return &accountsByUser[*du];
  }
  return accountsByUser.isEmpty() ? nullptr : &*accountsByUser.begin();
}

// POST /auth/sign-in and POST /setup (no auth).
bool FakeHub::handleSignInAndSetup(QSslSocket* sock, const FakeRequest& req) {
  const bool signIn = req.method == "POST" && req.path == QLatin1String("/api/v1/auth/sign-in");
  const bool setup = req.method == "POST" && req.path == QLatin1String("/api/v1/setup");
  if (!signIn && !setup) {
    return false;
  }
  const QJsonObject body = QJsonDocument::fromJson(req.body).object();
  const auto json = [](const QJsonObject& o) { return QJsonDocument(o).toJson(QJsonDocument::Compact); };
  const auto approved = [&](const FakeAccount& a) {
    approvedDelivered_ = true;
    deviceUsers.insert(body.value(QStringLiteral("device_id")).toString(), a.userId);
    respond(sock, 200, json({{QStringLiteral("status"), QStringLiteral("approved")},
                             {QStringLiteral("hub_id"), hubId},
                             {QStringLiteral("user_id"), a.userId},
                             {QStringLiteral("device_credential"), QString::fromLatin1(kDeviceCredential)}}));
  };
  if (signIn) {
    signInBodies.append(body);
    if (holdSignIn) {
      return true;
    }
    if (signInUnsupported) {
      respondError(sock, 404, QStringLiteral("not_found"));
      return true;
    }
    if (rateLimitSignIn) {
      respondRateLimited(sock);
      return true;
    }
    const QString device = body.value(QStringLiteral("device_id")).toString();
    const QString user = body.value(QStringLiteral("username")).toString();
    const QString password = body.value(QStringLiteral("password")).toString();
    for (const FakeAccount& a : std::as_const(accountsByUser)) {
      if (a.username.compare(user, Qt::CaseInsensitive) != 0 || a.password.isEmpty() || a.password != password) {
        continue;
      }
      if (disabledUsers.contains(a.username, Qt::CaseInsensitive)) {
        respondError(sock, 401, QStringLiteral("user_disabled"));
      } else if (conflictDevices.contains(device)) {
        conflictDevices.removeAll(device);
        respondError(sock, 409, QStringLiteral("conflict"));
      } else {
        approved(a);
      }
      return true;
    }
    respondError(sock, 401, QStringLiteral("invalid_credentials"));
    return true;
  }
  setupBodies.append(body);
  if (holdSignIn) {
    return true;
  }
  if (rateLimitSetup) {
    respondRateLimited(sock);
    return true;
  }
  if (!setupRequired) {
    respondError(sock, 409, QStringLiteral("already_set_up"));
    return true;
  }
  const QString password = body.value(QStringLiteral("password")).toString();
  if (body.value(QStringLiteral("username")).toString().isEmpty() || password.size() < 12) {
    respondError(sock, 400, QStringLiteral("bad_request"));
    return true;
  }
  if (body.value(QStringLiteral("setup_code")).toString() != setupCode) {
    respondError(sock, 401, QStringLiteral("invalid_setup_code"));
    return true;
  }
  if (setupCodeExpired) {
    respondError(sock, 410, QStringLiteral("setup_code_expired"));
    return true;
  }
  addAccount(body.value(QStringLiteral("username")).toString(), password, QStringLiteral("admin"));
  setupRequired = false;
  approved(accountsByUser[QStringLiteral("u_") + body.value(QStringLiteral("username")).toString().toLower()]);
  return true;
}

// GET/PATCH /account, PUT/DELETE /account/password, /account/devices...
void FakeHub::handleAccount(QSslSocket* sock, const FakeRequest& req) {
  const auto json = [](const QJsonObject& o) { return QJsonDocument(o).toJson(QJsonDocument::Compact); };
  const QJsonObject body = QJsonDocument::fromJson(req.body).object();
  FakeAccount* me = callerAccount();
  if (me == nullptr) {
    respondError(sock, 404, QStringLiteral("not_found"));
    return;
  }
  if (req.path == QLatin1String("/api/v1/account")) {
    if (req.method == "GET") {
      respond(sock, 200, json(accountJson(*me)));
    } else if (req.method == "PATCH") {
      const QString name = body.value(QStringLiteral("display_name")).toString().trimmed();
      if (name.isEmpty() || name.size() > 32) {
        respondError(sock, 400, QStringLiteral("bad_request"));
        return;
      }
      for (const FakeAccount& a : std::as_const(accountsByUser)) {
        if (a.userId != me->userId && a.displayName.compare(name, Qt::CaseInsensitive) == 0) {
          respondError(sock, 409, QStringLiteral("display_name_taken"));
          return;
        }
      }
      me->displayName = name;
      respond(sock, 200, json(accountJson(*me)));
    } else {
      respondError(sock, 405, QStringLiteral("method_not_allowed"));
    }
    return;
  }
  if (req.path == QLatin1String("/api/v1/account/password")) {
    const QString current = body.value(QStringLiteral("current_password")).toString();
    if (req.method == "PUT") {
      const QString next = body.value(QStringLiteral("new_password")).toString();
      if (next.size() < 12) {
        respondError(sock, 400, QStringLiteral("bad_request"));
      } else if (!me->password.isEmpty() && current != me->password) {
        respondError(sock, 401, QStringLiteral("invalid_credentials"));
      } else {
        me->password = next;
        respond(sock, 204, {});
      }
    } else if (req.method == "DELETE") {
      if (me->passwordRequired || requirePasswords) {
        respondError(sock, 403, QStringLiteral("password_required"));
      } else if (!me->password.isEmpty() && current != me->password) {
        respondError(sock, 401, QStringLiteral("invalid_credentials"));
      } else if (me->password.isEmpty()) {
        respondError(sock, 409, QStringLiteral("password_not_set"));
      } else {
        me->password.clear();
        respond(sock, 204, {});
      }
    } else {
      respondError(sock, 405, QStringLiteral("method_not_allowed"));
    }
    return;
  }
  if (req.path == QLatin1String("/api/v1/account/devices") && req.method == "GET") {
    QJsonArray list;
    for (const QJsonObject& d : std::as_const(accountDevices)) list.append(d);
    respond(sock, 200, json({{QStringLiteral("devices"), list}}));
    return;
  }
  if (req.path == QLatin1String("/api/v1/account/devices/sign-out-all") && req.method == "POST") {
    const bool includeCurrent = body.value(QStringLiteral("include_current")).toBool(false);
    int n = 0;
    for (QJsonObject& d : accountDevices) {
      if (d.value(QStringLiteral("status")).toString() == QLatin1String("trusted") &&
          (includeCurrent || !d.value(QStringLiteral("current")).toBool())) {
        d.insert(QStringLiteral("status"), QStringLiteral("revoked"));
        ++n;
      }
    }
    respond(sock, 200, json({{QStringLiteral("signed_out"), n}}));
    return;
  }
  const QString prefix = QStringLiteral("/api/v1/account/devices/");
  if (req.path.startsWith(prefix) && req.method == "DELETE") {
    const QString id = req.path.mid(prefix.size());
    for (QJsonObject& d : accountDevices) {
      if (d.value(QStringLiteral("device_id")).toString() == id) {
        d.insert(QStringLiteral("status"), QStringLiteral("revoked"));
        respond(sock, 204, {});
        return;
      }
    }
    respondError(sock, 404, QStringLiteral("not_found"));
    return;
  }
  respondError(sock, 404, QStringLiteral("not_found"));
}

bool FakeHub::handleCollections(QSslSocket* sock, const FakeRequest& req) {
  static const QString kBase = QStringLiteral("/api/v1/collections");
  const bool isFavorite = req.path.startsWith(QLatin1String("/api/v1/games/")) && req.path.endsWith(QLatin1String("/favorite"));
  const bool isCollections = req.path == kBase || req.path.startsWith(kBase + QLatin1Char('/'));
  if (!isFavorite && !isCollections) {
    return false;
  }
  if (collectionsUnsupported) {
    respondError(sock, 404, QStringLiteral("not_found"));
    return true;
  }
  const auto json = [](const QJsonObject& o) { return QJsonDocument(o).toJson(QJsonDocument::Compact); };
  const bool change = req.method != "GET";
  if (change && collectionFailNext > 0) {
    --collectionFailNext;
    respondError(sock, 503, QStringLiteral("unavailable"));
    return true;
  }
  const QJsonObject body = QJsonDocument::fromJson(req.body).object();
  if (isFavorite) {
    const QString gameId = req.path.mid(QStringLiteral("/api/v1/games/").size()).chopped(QStringLiteral("/favorite").size());
    if (req.method == "PUT") {
      if (!favoriteGames.contains(gameId)) favoriteGames.append(gameId);
    } else if (req.method == "DELETE") {
      favoriteGames.removeAll(gameId);
    } else {
      respondError(sock, 405, QStringLiteral("method_not_allowed"));
      return true;
    }
    respond(sock, 204, {});
    return true;
  }
  const auto indexOf = [this](const QString& id) {
    for (int i = 0; i < collections.size(); ++i) {
      if (collections.at(i).toObject().value(QStringLiteral("id")).toString() == id) return i;
    }
    return -1;
  };
  const auto nameTaken = [this](const QString& name, const QString& exceptId) {
    for (const QJsonValue& v : std::as_const(collections)) {
      const QJsonObject o = v.toObject();
      if (o.value(QStringLiteral("id")).toString() != exceptId && o.value(QStringLiteral("name")).toString().compare(name, Qt::CaseInsensitive) == 0) return true;
    }
    return false;
  };
  const QStringList parts = req.path.mid(kBase.size()).split(QLatin1Char('/'), Qt::SkipEmptyParts);
  if (parts.isEmpty()) {
    if (req.method == "GET") {
      respond(sock, 200, json({{QStringLiteral("collections"), collections}}));
    } else if (req.method == "POST") {
      const QString name = body.value(QStringLiteral("name")).toString().trimmed();
      if (name.isEmpty() || name.size() > 40) {
        respondError(sock, 400, QStringLiteral("bad_request"));
      } else if (nameTaken(name, {})) {
        respondError(sock, 409, QStringLiteral("name_taken"));
      } else {
        QJsonObject c{{QStringLiteral("id"), QStringLiteral("c%1").arg(collections.size() + 1)},
                      {QStringLiteral("name"), name},
                      {QStringLiteral("game_ids"), body.value(QStringLiteral("game_ids")).toArray()},
                      {QStringLiteral("created_at"), QStringLiteral("2026-10-01T10:00:00Z")}};
        collections.append(c);
        respond(sock, 201, json(c));
      }
    } else {
      respondError(sock, 405, QStringLiteral("method_not_allowed"));
    }
    return true;
  }
  const int at = indexOf(parts.at(0));
  if (at < 0) {
    respondError(sock, 404, QStringLiteral("not_found"));
    return true;
  }
  QJsonObject c = collections.at(at).toObject();
  if (parts.size() == 1 && req.method == "PATCH") {
    const QString name = body.value(QStringLiteral("name")).toString().trimmed();
    if (name.isEmpty() || name.size() > 40) {
      respondError(sock, 400, QStringLiteral("bad_request"));
    } else if (nameTaken(name, parts.at(0))) {
      respondError(sock, 409, QStringLiteral("name_taken"));
    } else {
      c.insert(QStringLiteral("name"), name);
      collections.replace(at, c);
      respond(sock, 200, json(c));
    }
  } else if (parts.size() == 1 && req.method == "DELETE") {
    collections.removeAt(at);
    respond(sock, 204, {});
  } else if (parts.size() == 3 && parts.at(1) == QLatin1String("games") && (req.method == "PUT" || req.method == "DELETE")) {
    QStringList ids;
    for (const QJsonValue& v : c.value(QStringLiteral("game_ids")).toArray()) ids.append(v.toString());
    ids.removeAll(parts.at(2));
    if (req.method == "PUT") ids.append(parts.at(2));
    c.insert(QStringLiteral("game_ids"), QJsonArray::fromStringList(ids));
    collections.replace(at, c);
    respond(sock, 204, {});
  } else {
    respondError(sock, 405, QStringLiteral("method_not_allowed"));
  }
  return true;
}

// Screenshots (Uzume 0.19): POST/GET /games/{id}/screenshots, GET /runs/{id}/screenshots, GET /screenshots/{id}/content, DELETE
// /screenshots/{id}. The caller is the paired fake person (u_test_1, "Tester").
bool FakeHub::handleScreenshots(QSslSocket* sock, const FakeRequest& req) {
  static const QString kGames = QStringLiteral("/api/v1/games/");
  static const QString kRuns = QStringLiteral("/api/v1/runs/");
  static const QString kShots = QStringLiteral("/api/v1/screenshots/");
  const QString path = req.path.left(req.path.indexOf(QLatin1Char('?')) < 0 ? req.path.size() : req.path.indexOf(QLatin1Char('?')));
  const bool gameRoute = path.startsWith(kGames) && path.endsWith(QLatin1String("/screenshots"));
  const bool runRoute = path.startsWith(kRuns) && path.endsWith(QLatin1String("/screenshots"));
  const bool shotRoute = path.startsWith(kShots);
  if (!gameRoute && !runRoute && !shotRoute) {
    return false;
  }
  if (screenshotsUnsupported) {
    respondError(sock, 404, QStringLiteral("not_found"));
    return true;
  }
  const auto json = [](const QJsonObject& o) { return QJsonDocument(o).toJson(QJsonDocument::Compact); };
  const auto listOf = [&](const std::function<bool(const QJsonObject&)>& match) {
    QJsonArray arr;
    qint64 total = 0;
    for (qsizetype i = screenshots.size() - 1; i >= 0; --i) {  // newest first
      const QJsonObject m = screenshots.at(i).meta;
      if (!match(m)) continue;
      arr.append(m);
      total += screenshots.at(i).png.size();
    }
    return json({{QStringLiteral("screenshots"), arr}, {QStringLiteral("total_bytes"), double(total)}});
  };
  if (gameRoute) {
    const QString gameId = path.mid(kGames.size()).chopped(QStringLiteral("/screenshots").size());
    if (req.method == "GET") {
      respond(sock, 200, listOf([&](const QJsonObject& m) {
        return m.value(QStringLiteral("game_id")).toString() == gameId && m.value(QStringLiteral("user_id")).toString() == QLatin1String("u_test_1");
      }));
    } else if (req.method == "POST") {
      if (screenshotFailNext > 0) {
        --screenshotFailNext;
        respondError(sock, 503, QStringLiteral("unavailable"));
        return true;
      }
      if (req.body.size() > screenshotMaxBytes) {
        respondError(sock, 413, QStringLiteral("payload_too_large"));
        return true;
      }
      // PNG signature and the IHDR size (no image library in the network tests)
      const auto be32 = [&](int at) {
        return (quint32(quint8(req.body.at(at))) << 24) | (quint32(quint8(req.body.at(at + 1))) << 16) | (quint32(quint8(req.body.at(at + 2))) << 8) |
               quint32(quint8(req.body.at(at + 3)));
      };
      if (req.body.size() < 24 || !req.body.startsWith(QByteArray("\x89PNG\r\n\x1a\n", 8))) {
        respondError(sock, 400, QStringLiteral("invalid_image"));
        return true;
      }
      const QUrlQuery q(req.path.contains(QLatin1Char('?')) ? req.path.mid(req.path.indexOf(QLatin1Char('?')) + 1) : QString());
      const QString runId = q.queryItemValue(QStringLiteral("run_id"));
      FakeScreenshot s;
      s.png = req.body;
      s.meta = QJsonObject{{QStringLiteral("id"), QStringLiteral("s%1").arg(++screenshotSeq_)},
                           {QStringLiteral("game_id"), gameId},
                           {QStringLiteral("run_id"), runId.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(runId)},
                           {QStringLiteral("user_id"), QStringLiteral("u_test_1")},
                           {QStringLiteral("user_name"), QStringLiteral("Tester")},
                           {QStringLiteral("created_at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                           {QStringLiteral("width"), int(be32(16))},
                           {QStringLiteral("height"), int(be32(20))},
                           {QStringLiteral("size_bytes"), double(req.body.size())}};
      screenshots.append(s);
      respond(sock, 201, json(s.meta));
    } else {
      respondError(sock, 405, QStringLiteral("method_not_allowed"));
    }
    return true;
  }
  if (runRoute) {
    const QString runId = QUrl::fromPercentEncoding(path.mid(kRuns.size()).chopped(QStringLiteral("/screenshots").size()).toUtf8());
    respond(sock, 200, listOf([&](const QJsonObject& m) { return m.value(QStringLiteral("run_id")).toString() == runId; }));
    return true;
  }
  QString rest = path.mid(kShots.size());
  const bool content = rest.endsWith(QLatin1String("/content"));
  if (content) rest.chop(QStringLiteral("/content").size());
  qsizetype at = -1;
  for (qsizetype i = 0; i < screenshots.size(); ++i) {
    if (screenshots.at(i).meta.value(QStringLiteral("id")).toString() == rest) at = i;
  }
  if (at < 0) {
    respondError(sock, 404, QStringLiteral("not_found"));
  } else if (content && req.method == "GET") {
    respond(sock, 200, screenshots.at(at).png, "image/png");
  } else if (!content && req.method == "DELETE") {
    screenshots.removeAt(at);
    respond(sock, 204, {});
  } else {
    respondError(sock, 405, QStringLiteral("method_not_allowed"));
  }
  return true;
}
