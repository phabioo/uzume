#include "discordpresence.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QLoggingCategory>
#include <QTimer>
#include <QUuid>
#include <optional>

namespace uzume {

Q_LOGGING_CATEGORY(lcDiscord, "uzume.discord")

namespace {
constexpr int kMaxFrame = 1 << 20;
constexpr int kMaxText = 128;

quint32 readLe(const char* p) {
  const auto* u = reinterpret_cast<const uchar*>(p);
  return quint32(u[0]) | quint32(u[1]) << 8 | quint32(u[2]) << 16 | quint32(u[3]) << 24;
}
void writeLe(QByteArray& out, quint32 v) {
  for (int i = 0; i < 4; ++i) out.append(char((v >> (8 * i)) & 0xff));
}
// Discord wants 2 to 128 characters per text field.
QString clip(const QString& s) { return s.left(kMaxText); }
}  // namespace

QStringList DiscordPresence::defaultSocketNames() {
  QStringList out;
#ifdef Q_OS_WIN
  for (int i = 0; i < 10; ++i) out << QStringLiteral("discord-ipc-%1").arg(i);  // QLocalSocket adds \\.\pipe\ .
#else
  QStringList dirs;
  for (const char* var : {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"}) {
    const QString v = qEnvironmentVariable(var);
    if (!v.isEmpty()) dirs << v;
  }
  dirs << QStringLiteral("/tmp");
  dirs.removeDuplicates();
  const QString xdg = qEnvironmentVariable("XDG_RUNTIME_DIR");
  QStringList bases;
  for (const QString& d : std::as_const(dirs)) bases << d;
  if (!xdg.isEmpty()) bases << xdg + QStringLiteral("/app/com.discordapp.Discord") << xdg + QStringLiteral("/snap.discord");
  for (const QString& b : std::as_const(bases)) {
    for (int i = 0; i < 10; ++i) out << QDir(b).filePath(QStringLiteral("discord-ipc-%1").arg(i));
  }
#endif
  return out;
}

QByteArray DiscordPresence::encodeFrame(quint32 opcode, const QByteArray& json) {
  QByteArray f;
  f.reserve(8 + json.size());
  writeLe(f, opcode);
  writeLe(f, quint32(json.size()));
  f.append(json);
  return f;
}

DiscordPresence::DiscordPresence(QObject* parent) : QObject(parent), socketNames_(defaultSocketNames()) {
  flushTimer_.setSingleShot(true);
  reconnectTimer_.setSingleShot(true);
  connect(&flushTimer_, &QTimer::timeout, this, &DiscordPresence::flush);
  connect(&reconnectTimer_, &QTimer::timeout, this, &DiscordPresence::tryConnect);
}

DiscordPresence::~DiscordPresence() { closeSocket(); }

QJsonObject DiscordPresence::stateJson() const {
  return QJsonObject{{QStringLiteral("enabled"), enabled_},
                     {QStringLiteral("connected"), ready_},
                     {QStringLiteral("details"), lastDetails_},
                     {QStringLiteral("state"), lastState_}};
}

void DiscordPresence::setEnabled(bool on) {
  if (enabled_ == on) return;
  enabled_ = on;
  if (!on) {
    flushTimer_.stop();
    if (ready_ && sent_) sendActivity(std::nullopt);
    dirty_ = false;
    closeSocket();
  }
  wantConnectionChanged();
}

void DiscordPresence::setActivity(const std::optional<DiscordActivity>& activity) {
  if (desired_ == activity) return;
  desired_ = activity;
  dirty_ = true;
  if (!enabled_) return;
  if (!desired_) {  // game ended: clear at once, not throttled
    flushTimer_.stop();
    if (ready_) flush();
    else dirty_ = false;
    reconnectTimer_.stop();
    return;
  }
  wantConnectionChanged();
  if (ready_) flush();
}

void DiscordPresence::wantConnectionChanged() {
  if (!wantsConnection()) {
    reconnectTimer_.stop();
    return;
  }
  if (!socket_ && !reconnectTimer_.isActive()) tryConnect();
}

void DiscordPresence::tryConnect() {
  if (!wantsConnection() || socket_) return;
  nameIndex_ = 0;
  tryNextName();
}

void DiscordPresence::tryNextName() {
  if (nameIndex_ >= socketNames_.size()) {
    if (!loggedDown_) {
      qCInfo(lcDiscord) << "Discord not reachable";
      loggedDown_ = true;
    }
    closeSocket();
    if (wantsConnection()) reconnectTimer_.start(reconnectMs_);
    return;
  }
  const QString name = socketNames_.at(nameIndex_++);
  if (socket_) {
    socket_->disconnect(this);
    socket_->abort();
    socket_->deleteLater();
  }
  socket_ = new QLocalSocket(this);
  connecting_ = true;
  connect(socket_, &QLocalSocket::connected, this, &DiscordPresence::onConnected);
  connect(socket_, &QLocalSocket::readyRead, this, &DiscordPresence::onReadyRead);
  connect(socket_, &QLocalSocket::disconnected, this, &DiscordPresence::onDisconnected);
  connect(socket_, &QLocalSocket::errorOccurred, this, [this](QLocalSocket::LocalSocketError) {
    if (connecting_) tryNextName();
    else onDisconnected();
  });
  buffer_.clear();
  socket_->connectToServer(name);
}

void DiscordPresence::onConnected() {
  connecting_ = false;
  const QJsonObject hs{{QStringLiteral("v"), 1}, {QStringLiteral("client_id"), QString::fromLatin1(kApplicationId)}};
  sendFrame(Handshake, QJsonDocument(hs).toJson(QJsonDocument::Compact));
}

void DiscordPresence::onReadyRead() {
  buffer_.append(socket_->readAll());
  while (buffer_.size() >= 8) {
    const quint32 op = readLe(buffer_.constData());
    const quint32 len = readLe(buffer_.constData() + 4);
    if (len > quint32(kMaxFrame)) {
      closeSocket();
      return;
    }
    if (buffer_.size() < qsizetype(8 + len)) return;
    const QByteArray payload = buffer_.mid(8, len);
    buffer_.remove(0, int(8 + len));
    handleFrame(op, payload);
    if (!socket_) return;
  }
}

void DiscordPresence::handleFrame(quint32 opcode, const QByteArray& payload) {
  switch (opcode) {
    case Ping: sendFrame(Pong, payload); break;
    case Close: closeSocket(); if (wantsConnection()) reconnectTimer_.start(reconnectMs_); break;
    case Frame: {
      const QJsonObject o = QJsonDocument::fromJson(payload).object();
      if (!ready_ && o.value(QStringLiteral("evt")).toString() == QLatin1String("READY")) {
        setReady(true);
        dirty_ = true;
        flush();
      }
      break;
    }
    default: break;
  }
}

void DiscordPresence::onDisconnected() {
  if (!socket_) return;
  closeSocket();
  if (wantsConnection()) reconnectTimer_.start(reconnectMs_);
}

void DiscordPresence::flush() {
  if (!ready_ || !dirty_ || !enabled_) return;
  if (!desired_) {  // clear
    if (sent_) sendActivity(std::nullopt);
    dirty_ = false;
    return;
  }
  if (sinceSend_.isValid() && sinceSend_.elapsed() < throttleMs_) {
    flushTimer_.start(int(throttleMs_ - sinceSend_.elapsed()));
    return;
  }
  sendActivity(desired_);
  dirty_ = false;
}

void DiscordPresence::sendActivity(const std::optional<DiscordActivity>& a) {
  QJsonValue activity = QJsonValue::Null;
  if (a) {
    QJsonObject act{{QStringLiteral("details"), clip(a->details.isEmpty() ? QStringLiteral("Playing") : a->details)},
                    {QStringLiteral("timestamps"), QJsonObject{{QStringLiteral("start"), a->startMs / 1000}}}};
    if (a->state.size() >= 2) act.insert(QStringLiteral("state"), clip(a->state));
    activity = act;
  }
  const QJsonObject msg{{QStringLiteral("cmd"), QStringLiteral("SET_ACTIVITY")},
                        {QStringLiteral("args"), QJsonObject{{QStringLiteral("pid"), qint64(QCoreApplication::applicationPid())},
                                                             {QStringLiteral("activity"), activity}}},
                        {QStringLiteral("nonce"), QUuid::createUuid().toString(QUuid::WithoutBraces)}};
  sendFrame(Frame, QJsonDocument(msg).toJson(QJsonDocument::Compact));
  sinceSend_.restart();
  sent_ = a;
  lastDetails_ = a ? clip(a->details) : QString();
  lastState_ = a ? clip(a->state) : QString();
}

void DiscordPresence::sendFrame(quint32 opcode, const QByteArray& json) {
  if (socket_ && socket_->state() == QLocalSocket::ConnectedState) socket_->write(encodeFrame(opcode, json));
}

void DiscordPresence::closeSocket() {
  connecting_ = false;
  if (socket_) {
    QLocalSocket* s = std::exchange(socket_, nullptr);
    s->disconnect(this);
    // A clear written just before must still reach Discord: deleting the socket right away dropped it. It goes once closed.
    s->flush();
    if (s->state() == QLocalSocket::UnconnectedState) {
      s->deleteLater();
    } else {
      connect(s, &QLocalSocket::disconnected, s, &QObject::deleteLater);
      QTimer::singleShot(2000, s, &QObject::deleteLater);
      s->disconnectFromServer();
    }
  }
  buffer_.clear();
  sent_.reset();
  setReady(false);
}

void DiscordPresence::setReady(bool ready) {
  if (ready_ == ready) return;
  ready_ = ready;
  if (ready) {
    loggedDown_ = false;
    qCInfo(lcDiscord) << "connected to Discord";
  } else {
    qCInfo(lcDiscord) << "disconnected from Discord";
    loggedDown_ = true;  // do not log the next failed retry again
  }
  emit connectedChanged();
}

}  // namespace uzume
