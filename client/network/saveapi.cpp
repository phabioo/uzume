#include "saveapi.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>
#include <QUrl>

namespace uzume {

namespace {
QString str(const QJsonObject& o, const char* k) { return o.value(QLatin1String(k)).toString(); }
}  // namespace

std::optional<SaveCheckpoint> parseSaveCheckpoint(const QJsonObject& o) {
  SaveCheckpoint c;
  c.revision = o.value(QStringLiteral("revision")).toInt(0);
  c.sha256 = str(o, "sha256");
  c.size = o.value(QStringLiteral("size")).toVariant().toLongLong();
  c.deviceId = str(o, "device_id");
  c.deviceName = str(o, "device_name");
  c.createdAt = str(o, "created_at");
  c.reason = str(o, "reason");
  if (c.revision < 1 || c.sha256.size() != 64) {
    return std::nullopt;
  }
  return c;
}

std::optional<SaveConflictInfo> parseSaveConflict(const QJsonObject& o) {
  SaveConflictInfo c;
  c.id = str(o, "id");
  c.gameId = str(o, "game_id");
  c.slot = str(o, "slot");
  c.status = str(o, "status");
  c.createdAt = str(o, "created_at");
  const QJsonObject hub = o.value(QStringLiteral("hub")).toObject();
  c.hubRevision = hub.value(QStringLiteral("revision")).toInt(0);
  c.hubSha256 = str(hub, "sha256");
  c.hubDeviceId = str(hub, "device_id");
  c.hubDeviceName = str(hub, "device_name");
  c.hubCreatedAt = str(hub, "created_at");
  const QJsonObject sec = o.value(QStringLiteral("secured")).toObject();
  c.securedVersion = sec.value(QStringLiteral("version")).toInt(0);
  c.securedSha256 = str(sec, "sha256");
  c.securedBaseRevision = sec.value(QStringLiteral("base_revision")).toInt(0);
  c.securedDeviceId = str(sec, "device_id");
  c.securedDeviceName = str(sec, "device_name");
  c.securedCreatedAt = str(sec, "created_at");
  if (c.id.isEmpty() || c.hubRevision < 1) {
    return std::nullopt;
  }
  return c;
}

std::optional<SaveSlotInfo> parseSaveSlot(const QJsonObject& o) {
  SaveSlotInfo s;
  s.gameId = str(o, "game_id");
  s.slot = str(o, "slot");
  const auto cur = parseSaveCheckpoint(o.value(QStringLiteral("current")).toObject());
  if (!cur || s.gameId.isEmpty()) {
    return std::nullopt;
  }
  s.current = *cur;
  const QJsonArray arr = o.value(QStringLiteral("open_conflicts")).toArray();
  for (const QJsonValue& v : arr) {
    if (const auto c = parseSaveConflict(v.toObject())) {
      s.openConflicts.append(*c);
    }
  }
  s.openConflictCount = o.contains(QStringLiteral("open_conflict_count")) ? o.value(QStringLiteral("open_conflict_count")).toInt()
                                                                          : static_cast<int>(s.openConflicts.size());
  return s;
}

std::optional<SaveHistoryVersion> parseSaveHistoryVersion(const QJsonObject& o) {
  SaveHistoryVersion v;
  v.version = o.value(QStringLiteral("version")).toInt(0);
  v.revision = o.value(QStringLiteral("revision")).toInt(0);
  v.sha256 = str(o, "sha256");
  v.size = o.value(QStringLiteral("size")).toVariant().toLongLong();
  v.deviceId = str(o, "device_id");
  v.deviceName = str(o, "device_name");
  v.createdAt = str(o, "created_at");
  v.reason = str(o, "reason");
  v.label = str(o, "label");  // null -> empty
  if (v.version < 1 || v.sha256.size() != 64) {
    return std::nullopt;
  }
  return v;
}

std::optional<SaveUpdate> parseSaveUpdate(const QJsonObject& p) {
  SaveUpdate u;
  u.gameId = str(p, "game_id");
  u.slot = str(p, "slot");
  u.revision = p.value(QStringLiteral("revision")).toInt(0);
  u.sha256 = str(p, "sha256");
  u.deviceId = str(p, "device_id");
  u.deviceName = str(p, "device_name");
  u.reason = str(p, "reason");
  u.previousSlot = str(p, "previous_slot");
  if (u.gameId.isEmpty() || u.slot.isEmpty() || u.revision < 1) {
    return std::nullopt;
  }
  return u;
}

SaveApi::SaveApi(HubConnection* connection, QObject* parent) : QObject(parent), conn_(connection) {}

QString SaveApi::slotPath(const QString& gameId, const QString& slot) {
  return QStringLiteral("/games/%1/saves/%2")
      .arg(QString::fromLatin1(QUrl::toPercentEncoding(gameId)), QString::fromLatin1(QUrl::toPercentEncoding(slot)));
}

void SaveApi::immediate(SaveApiResult::Kind kind, const QString& code, Callback cb) {
  QTimer::singleShot(0, this, [kind, code, cb = std::move(cb)]() {
    SaveApiResult r;
    r.kind = kind;
    r.errorCode = code;
    cb(r);
  });
}

void SaveApi::run(QNetworkReply* reply, Expect expect, Callback cb) {
  connect(reply, &QNetworkReply::finished, this, [this, reply, expect, cb = std::move(cb)]() {
    SaveApiResult res;
    const QByteArray etag = reply->rawHeader("ETag");
    const int headerRev = reply->rawHeader("X-Uzume-Save-Revision").toInt();
    const HttpResult r = HubHttp::resultOf(reply);
    reply->deleteLater();
    res.status = r.status;
    res.errorCode = r.apiErrorCode;
    res.errorMessage = r.apiErrorMessage;
    using K = SaveApiResult::Kind;
    if (r.networkError || r.certMismatch) {
      res.kind = K::Offline;
      res.errorCode = QStringLiteral("unreachable");
      res.errorMessage = r.errorString;
    } else if (r.status == 401) {
      if (conn_) {
        conn_->noteUnauthorized();
      }
      res.kind = K::Offline;
    } else if (r.status >= 500 || r.status == 429) {
      res.kind = K::Offline;
    } else if (r.status == 404) {
      res.kind = K::NotFound;
    } else if (r.status == 409) {
      if (r.apiErrorCode == QLatin1String("save_conflict")) {
        res.conflict = parseSaveConflict(r.json().value(QStringLiteral("conflict")).toObject());
        res.kind = res.conflict ? K::Conflict : K::BadResponse;
      } else if (r.apiErrorCode.isEmpty() || r.apiErrorCode == QLatin1String("save_conflict_stale")) {
        res.kind = K::Stale;
      } else {
        res.kind = K::Rejected;  // save_slot_exists, save_slot_protected, session_running, conflict, ...
      }
    } else if (!r.ok()) {
      res.kind = K::Rejected;
    } else {
      res.kind = K::Ok;
      switch (expect) {
        case Expect::Slot:
        case Expect::Put:
        case Expect::Resolve:
          res.slot = parseSaveSlot(r.json());
          if (!res.slot) {
            res.kind = K::BadResponse;
          }
          break;
        case Expect::SlotList: {
          const QJsonArray arr = r.json().value(QStringLiteral("saves")).toArray();
          for (const QJsonValue& v : arr) {
            if (const auto s = parseSaveSlot(v.toObject())) {
              res.slotList.append(*s);
            }
          }
          break;
        }
        case Expect::History: {
          const QJsonArray arr = r.json().value(QStringLiteral("versions")).toArray();
          for (const QJsonValue& v : arr) {
            if (const auto h = parseSaveHistoryVersion(v.toObject())) {
              res.history.append(*h);
            }
          }
          break;
        }
        case Expect::None:
          break;
        case Expect::Snapshot:
          res.snapshot = parseSaveHistoryVersion(r.json());
          if (!res.snapshot) {
            res.kind = K::BadResponse;
          }
          break;
        case Expect::Content: {
          QByteArray tag = etag.trimmed();
          if (tag.startsWith('"') && tag.endsWith('"') && tag.size() >= 2) {
            tag = tag.mid(1, tag.size() - 2);
          }
          const QByteArray actual = QCryptographicHash::hash(r.body, QCryptographicHash::Sha256).toHex();
          if (tag.isEmpty() || tag.toLower() != actual || headerRev < 1) {
            res.kind = K::BadResponse;
            res.errorCode = QStringLiteral("verification_failed");
          } else {
            res.content = r.body;
            res.contentRevision = headerRev;
          }
          break;
        }
      }
    }
    cb(res);
  });
}

void SaveApi::listSlots(Callback cb) {
  QNetworkReply* reply = conn_ ? conn_->authorizedGet(QStringLiteral("/saves")) : nullptr;
  if (reply == nullptr) {
    immediate(SaveApiResult::Kind::Offline, QStringLiteral("not_connected"), std::move(cb));
    return;
  }
  run(reply, Expect::SlotList, std::move(cb));
}

void SaveApi::getSlot(const QString& gameId, const QString& slot, Callback cb) {
  QNetworkReply* reply = conn_ ? conn_->authorizedGet(slotPath(gameId, slot)) : nullptr;
  if (reply == nullptr) {
    immediate(SaveApiResult::Kind::Offline, QStringLiteral("not_connected"), std::move(cb));
    return;
  }
  run(reply, Expect::Slot, std::move(cb));
}

void SaveApi::putSave(const QString& gameId, const QString& slot, const QByteArray& data, int baseRevision,
                      const QString& reason, Callback cb) {
  if (data.size() > kMaxSaveBytes) {
    immediate(SaveApiResult::Kind::Rejected, QStringLiteral("payload_too_large"), std::move(cb));
    return;
  }
  const QByteArray sha = QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
  HttpHeaders headers{{"X-Uzume-Base-Revision", QByteArray::number(baseRevision)},
                      {"X-Uzume-Content-SHA256", sha},
                      {"X-Uzume-Sync-Reason", reason.toLatin1()}};
  if (cheatsUsed_) headers.append(qMakePair(QByteArray("X-Uzume-Cheats-Used"), QByteArray("true")));
  QNetworkReply* reply = conn_ ? conn_->authorizedSend("PUT", slotPath(gameId, slot), data, headers) : nullptr;
  if (reply == nullptr) {
    immediate(SaveApiResult::Kind::Offline, QStringLiteral("not_connected"), std::move(cb));
    return;
  }
  run(reply, Expect::Put, std::move(cb));
}

void SaveApi::getContent(const QString& gameId, const QString& slot, Callback cb) {
  QNetworkReply* reply = conn_ ? conn_->authorizedGet(slotPath(gameId, slot) + QStringLiteral("/content")) : nullptr;
  if (reply == nullptr) {
    immediate(SaveApiResult::Kind::Offline, QStringLiteral("not_connected"), std::move(cb));
    return;
  }
  run(reply, Expect::Content, std::move(cb));
}

void SaveApi::resolve(const QString& gameId, const QString& slot, const QString& conflictId, const QString& resolution,
                      int expectedRevision, Callback cb) {
  const QByteArray body = QJsonDocument(QJsonObject{{QStringLiteral("resolution"), resolution},
                                                    {QStringLiteral("expected_revision"), expectedRevision}})
                              .toJson(QJsonDocument::Compact);
  QNetworkReply* reply =
      conn_ ? conn_->authorizedSend("POST",
                                    slotPath(gameId, slot) + QStringLiteral("/conflicts/") +
                                        QString::fromLatin1(QUrl::toPercentEncoding(conflictId)) + QStringLiteral("/resolve"),
                                    body, {}, "application/json")
            : nullptr;
  if (reply == nullptr) {
    immediate(SaveApiResult::Kind::Offline, QStringLiteral("not_connected"), std::move(cb));
    return;
  }
  run(reply, Expect::Resolve, std::move(cb));
}

void SaveApi::listHistory(const QString& gameId, const QString& slot, Callback cb) {
  QNetworkReply* reply = conn_ ? conn_->authorizedGet(slotPath(gameId, slot) + QStringLiteral("/history")) : nullptr;
  if (reply == nullptr) {
    immediate(SaveApiResult::Kind::Offline, QStringLiteral("not_connected"), std::move(cb));
    return;
  }
  run(reply, Expect::History, std::move(cb));
}

void SaveApi::restore(const QString& gameId, const QString& slot, int version, int expectedRevision, Callback cb) {
  const QByteArray body =
      QJsonDocument(QJsonObject{{QStringLiteral("expected_revision"), expectedRevision}}).toJson(QJsonDocument::Compact);
  QNetworkReply* reply =
      conn_ ? conn_->authorizedSend("POST", slotPath(gameId, slot) + QStringLiteral("/history/%1/restore").arg(version), body, {},
                                    "application/json")
            : nullptr;
  if (reply == nullptr) {
    immediate(SaveApiResult::Kind::Offline, QStringLiteral("not_connected"), std::move(cb));
    return;
  }
  run(reply, Expect::Resolve, std::move(cb));  // answer: the SaveSlot after the restore
}

void SaveApi::uploadFile(const QString& gameId, const QString& slot, const QByteArray& data, int expectedRevision, Callback cb) {
  if (data.size() > kMaxSaveBytes) {
    immediate(SaveApiResult::Kind::Rejected, QStringLiteral("payload_too_large"), std::move(cb));
    return;
  }
  const QByteArray sha = QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
  QNetworkReply* reply = conn_ ? conn_->authorizedSend("POST", slotPath(gameId, slot) + QStringLiteral("/upload"), data,
                                                       {{"X-Uzume-Content-SHA256", sha},
                                                        {"X-Uzume-Expected-Revision", QByteArray::number(expectedRevision)}},
                                                       "application/octet-stream")
                               : nullptr;
  if (reply == nullptr) {
    immediate(SaveApiResult::Kind::Offline, QStringLiteral("not_connected"), std::move(cb));
    return;
  }
  run(reply, Expect::Resolve, std::move(cb));  // answer: the SaveSlot after the upload
}

void SaveApi::renameSlot(const QString& gameId, const QString& slot, const QString& newName, Callback cb) {
  const QByteArray body = QJsonDocument(QJsonObject{{QStringLiteral("name"), newName}}).toJson(QJsonDocument::Compact);
  QNetworkReply* reply =
      conn_ ? conn_->authorizedSend("POST", slotPath(gameId, slot) + QStringLiteral("/rename"), body, {}, "application/json") : nullptr;
  if (reply == nullptr) {
    immediate(SaveApiResult::Kind::Offline, QStringLiteral("not_connected"), std::move(cb));
    return;
  }
  run(reply, Expect::Slot, std::move(cb));
}

void SaveApi::deleteSlot(const QString& gameId, const QString& slot, int confirmRevision, Callback cb) {
  QNetworkReply* reply =
      conn_ ? conn_->authorizedSend("DELETE", slotPath(gameId, slot) + QStringLiteral("?confirm_revision=%1").arg(confirmRevision),
                                    QByteArray())
            : nullptr;
  if (reply == nullptr) {
    immediate(SaveApiResult::Kind::Offline, QStringLiteral("not_connected"), std::move(cb));
    return;
  }
  run(reply, Expect::None, std::move(cb));
}

void SaveApi::deleteSnapshot(const QString& gameId, const QString& slot, int version, Callback cb) {
  QNetworkReply* reply =
      conn_ ? conn_->authorizedSend("DELETE", slotPath(gameId, slot) + QStringLiteral("/history/%1").arg(version), QByteArray())
            : nullptr;
  if (reply == nullptr) {
    immediate(SaveApiResult::Kind::Offline, QStringLiteral("not_connected"), std::move(cb));
    return;
  }
  run(reply, Expect::None, std::move(cb));
}

void SaveApi::createSnapshot(const QString& gameId, const QString& slot, const QString& label, Callback cb) {
  QJsonObject o;
  if (!label.trimmed().isEmpty()) {
    o.insert(QStringLiteral("label"), label.trimmed());
  }
  QNetworkReply* reply = conn_ ? conn_->authorizedSend("POST", slotPath(gameId, slot) + QStringLiteral("/snapshots"),
                                                       QJsonDocument(o).toJson(QJsonDocument::Compact), {}, "application/json")
                               : nullptr;
  if (reply == nullptr) {
    immediate(SaveApiResult::Kind::Offline, QStringLiteral("not_connected"), std::move(cb));
    return;
  }
  run(reply, Expect::Snapshot, std::move(cb));
}

}  // namespace uzume
