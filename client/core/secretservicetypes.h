#pragma once
// D-Bus wire types of the freedesktop Secret Service used by SecretServiceCredentialStore (Linux only). Header-only so
// that the test's fake service shares exactly the same registered types.

#include <QByteArray>
#include <QDBusArgument>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QList>
#include <QMap>
#include <QString>
#include <mutex>

namespace uzume::secretservice {

using StringMap = QMap<QString, QString>;

// "Secret" struct (oayays).
struct Secret {
  QDBusObjectPath session;
  QByteArray parameters;
  QByteArray value;
  QString contentType;
};

inline QDBusArgument& operator<<(QDBusArgument& arg, const Secret& s) {
  arg.beginStructure();
  arg << s.session << s.parameters << s.value << s.contentType;
  arg.endStructure();
  return arg;
}

inline const QDBusArgument& operator>>(const QDBusArgument& arg, Secret& s) {
  arg.beginStructure();
  arg >> s.session >> s.parameters >> s.value >> s.contentType;
  arg.endStructure();
  return arg;
}

inline void registerTypes() {
  static std::once_flag once;
  std::call_once(once, [] {
    qDBusRegisterMetaType<Secret>();
    qDBusRegisterMetaType<StringMap>();
    qDBusRegisterMetaType<QList<QDBusObjectPath>>();
  });
}

}  // namespace uzume::secretservice
