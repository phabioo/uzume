#include "onlineroster.h"

#include <QCoreApplication>
#include <QVariantMap>
#include <algorithm>

namespace uzume::ui {

void OnlineRoster::apply(const PresenceUpdate& u) {
  if (u.userId.isEmpty()) return;
  const QList<Person> before = people();
  if (u.state == QLatin1String("offline")) {
    const auto it = users_.find(u.userId);
    if (it != users_.end()) {
      it->devices.remove(u.deviceId);
      if (it->devices.isEmpty()) users_.erase(it);
    }
  } else if (u.state == QLatin1String("online") || u.state == QLatin1String("in_game")) {
    User& user = users_[u.userId];
    if (!u.displayName.isEmpty()) user.name = u.displayName;
    user.devices.insert(u.deviceId, {u.state, u.gameTitle, u.system, u.sessionId});
  } else {
    return;  // an unknown state changes nothing
  }
  if (people() != before) emit changed();
}

void OnlineRoster::clear() {
  if (users_.isEmpty()) return;
  users_.clear();
  emit changed();
}

QList<OnlineRoster::Person> OnlineRoster::people(const QString& excludeUserId) const {
  QList<Person> out;
  for (auto it = users_.constBegin(); it != users_.constEnd(); ++it) {
    if (it.key() == excludeUserId) continue;
    const User& user = it.value();
    Person p;
    p.userId = it.key();
    p.name = user.name.isEmpty() ? it.key() : user.name;
    p.state = QStringLiteral("online");
    p.devices = static_cast<int>(user.devices.size());
    // The device that shows: in game before online, one with a visible Session before one without (ties: smaller device id, so the pick is stable).
    const Device* best = nullptr;
    QString bestId;
    const auto rank = [](const Device& d) { return (d.state == QLatin1String("in_game") ? 2 : 0) + (d.sessionId.isEmpty() ? 0 : 1); };
    for (auto d = user.devices.constBegin(); d != user.devices.constEnd(); ++d) {
      if (best == nullptr || rank(*d) > rank(*best) || (rank(*d) == rank(*best) && d.key() < bestId)) {
        best = &*d;
        bestId = d.key();
      }
    }
    if (best != nullptr) {
      p.state = best->state;
      p.gameTitle = best->gameTitle;
      p.system = best->system;
      p.sessionId = best->sessionId;
    }
    for (const Device& d : user.devices) {  // a Session on a device that is not the one shown still can be watched
      if (p.sessionId.isEmpty() && !d.sessionId.isEmpty()) p.sessionId = d.sessionId;
    }
    out.append(p);
  }
  std::sort(out.begin(), out.end(), [](const Person& a, const Person& b) {
    if ((a.state == QLatin1String("in_game")) != (b.state == QLatin1String("in_game"))) return a.state == QLatin1String("in_game");
    const int c = a.name.compare(b.name, Qt::CaseInsensitive);
    return c != 0 ? c < 0 : a.userId < b.userId;
  });
  return out;
}

int OnlineRoster::colorOf(const QString& userId) {
  quint32 h = 0;
  for (const QChar c : userId) h = h * 31 + c.unicode();
  return static_cast<int>(h % 8) + 1;
}

QVariantList OnlineRoster::peopleVariant(const QString& excludeUserId) const {
  QVariantList out;
  for (const Person& p : people(excludeUserId)) {
    QString sub = QCoreApplication::translate("OnlineRoster", "Online");
    if (p.state == QLatin1String("in_game"))
      sub = p.gameTitle.isEmpty() ? QCoreApplication::translate("OnlineRoster", "Playing")
                                  : QCoreApplication::translate("OnlineRoster", "Playing %1").arg(p.gameTitle);
    out.append(QVariantMap{{QStringLiteral("userId"), p.userId},
                           {QStringLiteral("name"), p.name},
                           {QStringLiteral("state"), p.state},
                           {QStringLiteral("gameTitle"), p.gameTitle},
                           {QStringLiteral("system"), p.system},
                           {QStringLiteral("sessionId"), p.sessionId},
                           {QStringLiteral("sub"), sub},
                           {QStringLiteral("color"), colorOf(p.userId)}});
  }
  return out;
}

}  // namespace uzume::ui
