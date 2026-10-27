#pragma once
// OnlineRoster (ADR 0033 D1): the people who are online on the server, built from the per-receiver `presence_update` messages.
// People, not devices: a person is online while any of their devices is, "in game" wins over "online", and the visible Session id of
// any device is offered for "Watch". In memory only; cleared when the WSS link goes away (the server sends a snapshot after the next
// hello_ack).

#include <QHash>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QList>

#include "sessiontypes.h"

namespace uzume::ui {

class OnlineRoster : public QObject {
  Q_OBJECT
 public:
  struct Person {
    QString userId;
    QString name;
    QString state;  // online | in_game
    QString gameTitle;
    QString system;
    QString sessionId;  // empty = nothing to watch
    int devices = 0;
    bool operator==(const Person& o) const {
      return userId == o.userId && name == o.name && state == o.state && gameTitle == o.gameTitle && system == o.system && sessionId == o.sessionId &&
             devices == o.devices;
    }
  };

  explicit OnlineRoster(QObject* parent = nullptr) : QObject(parent) {}

  void apply(const PresenceUpdate& u);
  void clear();
  // In game first, then by name; `excludeUserId` (me) is left out.
  QList<Person> people(const QString& excludeUserId = {}) const;
  // [{userId, name, state, gameTitle, system, sessionId, sub, color}]; `sub` is the second line ("Playing <game>" or "Online"), `color` the
  // stable person colour 1..8.
  QVariantList peopleVariant(const QString& excludeUserId = {}) const;
  bool isEmpty() const { return users_.isEmpty(); }
  static int colorOf(const QString& userId);

 signals:
  void changed();

 private:
  struct Device {
    QString state;
    QString gameTitle;
    QString system;
    QString sessionId;
  };
  struct User {
    QString name;
    QHash<QString, Device> devices;
  };
  QHash<QString, User> users_;
};

}  // namespace uzume::ui
