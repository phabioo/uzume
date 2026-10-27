#include "chatcontroller.h"

#include <QDate>
#include <QDateTime>
#include <QLocale>
#include <QPointer>
#include <QRegularExpression>
#include <QUuid>
#include <algorithm>

#include "onlineroster.h"

namespace uzume::ui {

namespace {
constexpr qint64 kGroupMs = 5 * 60 * 1000;  // messages of one person within 5 minutes share a head
constexpr int kMaxPages = 20;

struct Emote {
  const char* code;
  const char* glyph;
};
constexpr Emote kEmotes[] = {{":gg:", "\xF0\x9F\x8E\xAE"},    // game controller
                             {":wave:", "\xF0\x9F\x91\x8B"},  // waving hand
                             {":lol:", "\xF0\x9F\x98\x82"},   // face with tears of joy
                             {":hype:", "\xF0\x9F\x94\xA5"},  // fire
                             {":sad:", "\xF0\x9F\x98\xA2"},   // crying face
                             {":ok:", "\xF0\x9F\x91\x8D"}};   // thumbs up

qint64 parseTime(const QString& iso, qint64 fallback) {
  QDateTime t = QDateTime::fromString(iso, Qt::ISODateWithMs);
  if (!t.isValid()) t = QDateTime::fromString(iso, Qt::ISODate);
  return t.isValid() ? t.toMSecsSinceEpoch() : fallback;
}
}  // namespace

ChatController::ChatController(TogetherController* together, HubSocket* socket, QObject* parent) : QObject(parent), together_(together), socket_(socket) {
  connect(socket_, &HubSocket::chatReceived, this, [this](const ChatMessage& m) { onMessage(m, true); });
  connect(socket_, &HubSocket::stateChanged, this, &ChatController::onSocketState);
  connect(together_, &TogetherController::scopeChanged, this, &ChatController::refresh);
  timer_.setInterval(1000);
  connect(&timer_, &QTimer::timeout, this, &ChatController::tick);
  timer_.start();
}

qint64 ChatController::now() const { return clock_ ? clock_() : QDateTime::currentMSecsSinceEpoch(); }

QString ChatController::nameOf(const QString& userId) const {
  const QString n = nameFn_ ? nameFn_(userId) : QString();
  return n.isEmpty() ? tr("Someone") : n;
}

bool ChatController::available() const { return together_ && together_->available(); }

ChatController::Chat& ChatController::chatFor(const QString& kind, const QString& id) {
  const QString key = keyOf(kind, id);
  auto it = chats_.find(key);
  if (it == chats_.end()) {
    it = chats_.insert(key, Chat{});
    it->kind = kind;
    it->id = id;
    order_.append(key);
    while (order_.size() > kMaxScopes) {  // the oldest scope that is not the current one
      const int victim = order_.first() == keyOf(kind_, id_) ? 1 : 0;
      if (victim >= order_.size()) break;
      chats_.remove(order_.takeAt(victim));
    }
    it = chats_.find(key);
  }
  return *it;
}

ChatController::Chat* ChatController::current() {
  if (kind_.isEmpty()) return nullptr;
  const auto it = chats_.find(keyOf(kind_, id_));
  return it == chats_.end() ? nullptr : &*it;
}
const ChatController::Chat* ChatController::current() const {
  if (kind_.isEmpty()) return nullptr;
  const auto it = chats_.constFind(keyOf(kind_, id_));
  return it == chats_.constEnd() ? nullptr : &*it;
}

void ChatController::refresh() {
  const QString kind = together_ ? together_->scopeKind() : QString();
  const QString id = together_ ? together_->scopeId() : QString();
  const bool moved = kind != kind_ || id != id_;
  kind_ = kind;
  id_ = id;
  if (!kind_.isEmpty() && available()) {
    Chat& c = chatFor(kind_, id_);
    if (!c.loaded && !c.loading) load(kind_, id_, true);
    if (visible_) markSeen(c);
  }
  if (moved) {
    emit scopeChanged();
    emit unreadChanged();
    emit rowsChanged();
  } else {
    emit scopeChanged();  // `available` also follows the Hub's features
  }
}

void ChatController::reset() {
  chats_.clear();
  order_.clear();
  kind_.clear();
  id_.clear();
  emit scopeChanged();
  emit unreadChanged();
  emit rowsChanged();
}

void ChatController::onSocketState(HubSocket::State s) {
  if (s != HubSocket::State::Open) return;
  refresh();
  // Messages may have been missed while the link was down: fetch what follows the last one I know, for every scope I hold.
  for (auto it = chats_.begin(); it != chats_.end(); ++it) {
    if (it->loaded && !it->loading && available()) load(it->kind, it->id, false);
  }
}

void ChatController::setPanelVisible(bool on) {
  if (visible_ == on) return;
  visible_ = on;
  if (on) {
    if (Chat* c = current()) markSeen(*c);
  }
  emit visibleChanged();
}

void ChatController::markSeen(Chat& c) {
  if (c.unread == 0) return;
  c.unread = 0;
  emit unreadChanged();
}

int ChatController::unread() const {
  const Chat* c = current();
  return c ? c->unread : 0;
}

bool ChatController::loading() const {
  const Chat* c = current();
  return c && c->loading && c->msgs.isEmpty();
}

bool ChatController::canSend() const { return available() && current() != nullptr && (sender_ || (socket_ && socket_->isOpen())); }

QVariantList ChatController::quickReplies() const {
  return {QVariantMap{{QStringLiteral("label"), tr("Down!")}, {QStringLiteral("text"), tr("Down!")}, {QStringLiteral("key"), QStringLiteral("1")}},
          QVariantMap{{QStringLiteral("label"), tr("Gym next")}, {QStringLiteral("text"), tr("Gym next")}, {QStringLiteral("key"), QStringLiteral("2")}},
          QVariantMap{{QStringLiteral("label"), tr("Wait")}, {QStringLiteral("text"), tr("Wait")}, {QStringLiteral("key"), QStringLiteral("3")}}};
}

int ChatController::charCount(const QString& text) { return static_cast<int>(text.toUcs4().size()); }

// ---------------------------------------------------------------- sending

bool ChatController::send(const QString& text) {
  const QString t = text.trimmed();
  const int n = charCount(t);
  Chat* c = current();
  if (n < 1 || n > kMaxText || c == nullptr || !available()) return false;
  const QString cid = QUuid::createUuid().toString(QUuid::WithoutBraces);
  const bool sent = sender_ ? sender_(c->kind, c->id, cid, t) : (socket_ && socket_->sendChat(c->kind, c->id, cid, t));
  if (!sent) return false;
  Msg m;
  m.clientId = cid;
  m.userId = self();
  m.text = t;
  m.atMs = now();
  m.sentLocalMs = m.atMs;
  c->msgs.append(m);
  emitChanged(*c);
  return true;
}

bool ChatController::sendQuick(int index) {
  const QVariantList q = quickReplies();
  if (index < 0 || index >= q.size()) return false;
  return send(q.at(index).toMap().value(QStringLiteral("text")).toString());
}

void ChatController::retry(const QString& clientId) {
  Chat* c = current();
  if (c == nullptr) return;
  for (Msg& m : c->msgs) {
    if (m.seq != 0 || m.clientId != clientId || !m.failed) continue;
    const bool sent = sender_ ? sender_(c->kind, c->id, m.clientId, m.text) : (socket_ && socket_->sendChat(c->kind, c->id, m.clientId, m.text));
    if (!sent) return;
    m.failed = false;
    m.sentLocalMs = now();
    emitChanged(*c);
    return;
  }
}

void ChatController::tick() {
  bool changed = false;
  const qint64 t = now();
  for (auto it = chats_.begin(); it != chats_.end(); ++it) {
    bool here = false;
    for (Msg& m : it->msgs) {
      if (m.seq == 0 && !m.failed && t - m.sentLocalMs >= kPendingMs) {
        m.failed = true;
        here = true;
      }
    }
    changed = changed || (here && it->kind == kind_ && it->id == id_);
  }
  if (changed) emit rowsChanged();
}

bool ChatController::isPending(const QString& clientId) const {
  const Chat* c = current();
  if (c == nullptr) return false;
  for (const Msg& m : c->msgs)
    if (m.clientId == clientId) return m.seq == 0 && !m.failed;
  return false;
}

int ChatController::messageCount() const {
  const Chat* c = current();
  return c ? static_cast<int>(c->msgs.size()) : 0;
}

// ---------------------------------------------------------------- receiving

void ChatController::sortMsgs(Chat& c) {
  std::stable_sort(c.msgs.begin(), c.msgs.end(), [](const Msg& a, const Msg& b) {
    const qint64 ka = a.seq == 0 ? std::numeric_limits<qint64>::max() : a.seq;
    const qint64 kb = b.seq == 0 ? std::numeric_limits<qint64>::max() : b.seq;
    return ka < kb;
  });
}

void ChatController::trim(Chat& c) {
  qsizetype confirmed = 0;
  for (const Msg& m : c.msgs) confirmed += m.seq != 0 ? 1 : 0;
  while (confirmed > kMaxKept) {
    for (qsizetype i = 0; i < c.msgs.size(); ++i) {
      if (c.msgs[i].seq != 0) {
        c.msgs.removeAt(i);
        --confirmed;
        break;
      }
    }
  }
}

void ChatController::emitChanged(const Chat& c) {
  if (bulk_ > 0) return;
  if (c.kind == kind_ && c.id == id_) emit rowsChanged();
}

void ChatController::onMessage(const ChatMessage& m, bool live) {
  if (m.scope.isEmpty() || m.id.isEmpty() || m.seq <= 0) return;
  Chat& c = chatFor(m.scope, m.id);
  for (const Msg& e : c.msgs)
    if (e.seq == m.seq) return;  // known
  Msg* mine = nullptr;
  if (!m.clientId.isEmpty()) {
    for (Msg& e : c.msgs) {
      if (e.seq == 0 && e.clientId == m.clientId) {
        mine = &e;
        break;
      }
    }
  }
  const qint64 at = parseTime(m.sentAt, now());
  if (mine != nullptr) {  // the echo of a message I sent (pending or failed): the Hub's version is the truth
    mine->seq = m.seq;
    mine->userId = m.userId;
    mine->text = m.text;
    mine->atMs = at;
    mine->mentions = m.mentions;
    mine->failed = false;
    c.lastSeq = std::max(c.lastSeq, m.seq);
    sortMsgs(c);
    emitChanged(c);
    return;
  }
  Msg n;
  n.seq = m.seq;
  n.clientId = m.clientId;
  n.userId = m.userId;
  n.text = m.text;
  n.atMs = at;
  n.mentions = m.mentions;
  c.msgs.append(n);
  c.lastSeq = std::max(c.lastSeq, m.seq);
  sortMsgs(c);
  trim(c);
  const bool onScreen = visible_ && c.kind == kind_ && c.id == id_;
  if (live && m.userId != self() && !onScreen) {
    ++c.unread;
    if (c.kind == kind_ && c.id == id_) emit unreadChanged();
    if (!self().isEmpty() && m.mentions.contains(self())) emit mentioned(c.kind, c.id, nameOf(m.userId), m.text);
  }
  emitChanged(c);
}

void ChatController::load(const QString& kind, const QString& id, bool initial) {
  if (!fetch_) return;
  Chat& c = chatFor(kind, id);
  if (c.loading) return;
  c.loading = true;
  loadPage(keyOf(kind, id), kind == QLatin1String("run") && !initial ? c.lastSeq : 0, initial, 0);
  emitChanged(c);
}

void ChatController::loadPage(const QString& key, qint64 afterSeq, bool initial, int page) {
  const auto it = chats_.constFind(key);
  if (it == chats_.constEnd()) return;
  const QString kind = it->kind, id = it->id;
  QPointer<ChatController> self(this);
  fetch_(kind, id, afterSeq, kPageSize, [self, key, kind, id, initial, page](const ChatPage& p) {
    if (!self) return;
    auto cit = self->chats_.find(key);
    if (cit == self->chats_.end()) return;  // the scope was dropped meanwhile
    Chat& c = *cit;
    if (!p.ok) {
      c.loading = false;  // the next scope change or reconnect tries again
      self->emitChanged(c);
      return;
    }
    ++self->bulk_;
    for (ChatMessage m : p.messages) {
      m.scope = kind;
      m.id = id;
      self->onMessage(m, false);
    }
    --self->bulk_;
    if (kind == QLatin1String("run") && p.nextAfterSeq > 0 && page + 1 < kMaxPages) {
      self->loadPage(key, p.nextAfterSeq, initial, page + 1);
      return;
    }
    c.loading = false;
    c.loaded = true;
    if (kind == QLatin1String("run") && initial) {  // the last kHistoryRun of a long run
      qsizetype confirmed = 0;
      for (const Msg& m : c.msgs) confirmed += m.seq != 0 ? 1 : 0;
      for (qsizetype i = 0; i < c.msgs.size() && confirmed > kHistoryRun;) {
        if (c.msgs[i].seq != 0) {
          c.msgs.removeAt(i);
          --confirmed;
        } else {
          ++i;
        }
      }
    }
    self->emitChanged(c);
  });
}

void ChatController::noteRunEvent(const QString& runId) {
  if (kind_ == QLatin1String("run") && id_ == runId) emit rowsChanged();
}

// ---------------------------------------------------------------- display

QString ChatController::renderEmotes(const QString& text) {
  QString out = text;
  for (const Emote& e : kEmotes) out.replace(QLatin1String(e.code), QString::fromUtf8(e.glyph), Qt::CaseInsensitive);
  return out;
}

QString ChatController::renderHtml(const QString& text, const QString& myName, bool mentionsMe) {
  QString s = renderEmotes(text.toHtmlEscaped());
  const QString name = myName.trimmed();
  if (mentionsMe && !name.isEmpty()) {
    QString noSpaces = name;
    noSpaces.remove(QRegularExpression(QStringLiteral("\\s+")));
    QStringList alts{QRegularExpression::escape(name.toHtmlEscaped())};
    if (noSpaces != name) alts << QRegularExpression::escape(noSpaces.toHtmlEscaped());
    const QRegularExpression re(QStringLiteral("(@(?:%1))(?![\\p{L}\\p{N}_])").arg(alts.join(QLatin1Char('|'))),
                                QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption);
    s.replace(re, QStringLiteral("<b>\\1</b>"));
  }
  return s;
}

QString ChatController::header() const {
  const Chat* c = current();
  if (c == nullptr) return {};
  const int people = peopleFn_ ? static_cast<int>(peopleFn_(c->kind, c->id).size()) : 0;
  QStringList parts;
  if (c->kind == QLatin1String("run")) {
    parts << tr("RUN CHAT");
    const QString title = titleFn_ ? titleFn_(c->kind, c->id) : QString();
    if (!title.isEmpty()) parts << title.toUpper();
  } else {
    parts << tr("SESSION CHAT");
  }
  if (people > 0) parts << (people == 1 ? tr("1 PERSON") : tr("%1 PEOPLE").arg(people));
  return parts.join(QStringLiteral(" · "));
}

QVariantList ChatController::rows() const {
  const Chat* c = current();
  if (c == nullptr) return {};
  QList<EventLine> events;
  if (c->kind == QLatin1String("run") && runEventsFn_) events = runEventsFn_(c->id);
  std::stable_sort(events.begin(), events.end(), [](const EventLine& a, const EventLine& b) { return a.atMs < b.atMs; });
  struct Item {
    qint64 at;
    bool event;
    qsizetype idx;
  };
  QList<Item> items;
  qsizetype ei = 0;
  for (qsizetype mi = 0; mi < c->msgs.size(); ++mi) {
    while (ei < events.size() && events[ei].atMs <= c->msgs[mi].atMs) {
      items.append({events[ei].atMs, true, ei});
      ++ei;
    }
    items.append({c->msgs[mi].atMs, false, mi});
  }
  for (; ei < events.size(); ++ei) items.append({events[ei].atMs, true, ei});
  if (items.size() > kMaxRows) items = items.mid(items.size() - kMaxRows);

  const QString me = self();
  const QString myName = selfNameFn_ ? selfNameFn_() : QString();
  const QDate today = QDateTime::fromMSecsSinceEpoch(now()).date();
  QVariantList chrono;
  QDate lastDay;
  QString lastUser;
  qint64 lastAt = 0;
  bool lastWasMsg = false;
  for (const Item& it : items) {
    const QDateTime when = QDateTime::fromMSecsSinceEpoch(it.at);
    const QDate day = when.date();
    const bool newDay = day != lastDay;
    if (newDay) {
      QString label;
      if (day == today) label = tr("TODAY");
      else if (day == today.addDays(-1)) label = tr("YESTERDAY");
      else label = QLocale().toString(day, QStringLiteral("ddd d MMM")).toUpper();
      chrono.append(QVariantMap{{QStringLiteral("type"), QStringLiteral("day")}, {QStringLiteral("text"), label}, {QStringLiteral("key"), QStringLiteral("d") + day.toString(Qt::ISODate)}});
      lastDay = day;
    }
    const QString time = when.toString(QStringLiteral("HH:mm"));
    if (it.event) {
      const EventLine& e = events[it.idx];
      chrono.append(QVariantMap{{QStringLiteral("type"), QStringLiteral("event")},
                                {QStringLiteral("text"), e.text},
                                {QStringLiteral("time"), time},
                                {QStringLiteral("key"), QStringLiteral("e%1").arg(e.seq != 0 ? QString::number(e.seq) : QString::number(it.at))}});
      lastWasMsg = false;
      continue;
    }
    const Msg& m = c->msgs[it.idx];
    const bool mentionsMe = !me.isEmpty() && m.mentions.contains(me);
    const bool head = newDay || !lastWasMsg || lastUser != m.userId || it.at - lastAt > kGroupMs;
    chrono.append(QVariantMap{{QStringLiteral("type"), QStringLiteral("msg")},
                              {QStringLiteral("key"), m.clientId.isEmpty() ? QStringLiteral("s%1").arg(m.seq) : m.clientId},
                              {QStringLiteral("seq"), m.seq},
                              {QStringLiteral("clientId"), m.clientId},
                              {QStringLiteral("userId"), m.userId},
                              {QStringLiteral("name"), m.userId == me && !myName.isEmpty() ? myName : nameOf(m.userId)},
                              {QStringLiteral("color"), OnlineRoster::colorOf(m.userId)},
                              {QStringLiteral("time"), time},
                              {QStringLiteral("plain"), m.text},
                              {QStringLiteral("html"), renderHtml(m.text, myName, mentionsMe)},
                              {QStringLiteral("mine"), m.userId == me},
                              {QStringLiteral("mention"), mentionsMe},
                              {QStringLiteral("pending"), m.seq == 0 && !m.failed},
                              {QStringLiteral("failed"), m.seq == 0 && m.failed},
                              {QStringLiteral("head"), head}});
    lastWasMsg = true;
    lastUser = m.userId;
    lastAt = it.at;
  }
  QVariantList out;
  out.reserve(chrono.size());
  for (qsizetype i = chrono.size() - 1; i >= 0; --i) out.append(chrono.at(i));
  return out;
}

QVariantList ChatController::history(int limit) const {
  QVariantList out;
  const Chat* c = current();
  if (c == nullptr) return out;
  const qsizetype from = std::max<qsizetype>(0, c->msgs.size() - std::max(1, limit));
  for (qsizetype i = from; i < c->msgs.size(); ++i) {
    const Msg& m = c->msgs[i];
    out.append(QVariantMap{{QStringLiteral("seq"), m.seq},
                           {QStringLiteral("name"), nameOf(m.userId)},
                           {QStringLiteral("userId"), m.userId},
                           {QStringLiteral("text"), m.text},
                           {QStringLiteral("time"), QDateTime::fromMSecsSinceEpoch(m.atMs).toUTC().toString(Qt::ISODate)},
                           {QStringLiteral("pending"), m.seq == 0 && !m.failed},
                           {QStringLiteral("failed"), m.seq == 0 && m.failed}});
  }
  return out;
}

}  // namespace uzume::ui
