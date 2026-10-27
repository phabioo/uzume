#pragma once

#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <functional>

#include "hubconnection.h"

namespace uzume {

// A game variant as the server stores it (feature variants_v1, ADR 0031 D1): a library game plus a recipe. Seed and settings come
// only for the owner's own variants. Seeds are int63 and are kept as decimal text (they do not fit a JSON double).
struct VariantInfo {
  QString id;
  QString gameId;
  QString name;
  QString slot;  // "var-<8 hex>"
  QString toolId;
  QString toolVersion;
  QString settings;
  QString seed;
  QString baseSha256;
  QString titleId;
  QString updateVersion;
  bool spoilerLog = false;
  QString runId;      // set for the variant of a run
  QString createdAt;
  QString deletedAt;  // set when the recipe stayed after "keep saves"
  bool valid() const { return !id.isEmpty(); }
};

// GET /runs/{id}/variant: what a participant builds for a run.
struct RunVariantRecipe {
  QString toolId;
  QString toolVersion;
  QString settings;
  QString seed;
  QString baseSha256;
  QString titleId;
  QString updateVersion;
  QString slot;
  bool valid() const { return !settings.isEmpty() && !seed.isEmpty(); }
};

struct VariantCall {
  bool ok = false;
  int status = 0;
  QString error;  // the API error code ("name_taken", ...), or a short network text; empty when ok
};

struct NewVariant {
  QString name;
  QString toolId = QStringLiteral("randomizer");
  QString toolVersion;
  QString settings;
  QString seed;  // decimal integer text
  QString baseSha256;
  QString titleId;
  QString updateVersion;
  bool spoilerLog = false;
};

// Parses JSON where the numbers of the keys `seed` and `run_seed` stay exact (quoted before parsing).
QJsonObject parseVariantJson(const QByteArray& body);
VariantInfo parseVariant(const QJsonObject& o);
RunVariantRecipe parseRunVariant(const QJsonObject& o);

class VariantsClient : public QObject {
  Q_OBJECT
 public:
  explicit VariantsClient(HubConnection* connection, QObject* parent = nullptr) : QObject(parent), conn_(connection) {}

  bool available() const;  // connected and variants_v1
  using ListCb = std::function<void(const QList<VariantInfo>&, const VariantCall&)>;
  using OneCb = std::function<void(const VariantInfo&, const VariantCall&)>;
  using DoneCb = std::function<void(const VariantCall&)>;
  using RunCb = std::function<void(const RunVariantRecipe&, const VariantCall&)>;

  void list(const QString& gameId, ListCb cb);  // gameId empty: GET /variants (all games)
  void create(const QString& gameId, const NewVariant& v, OneCb cb);
  void rename(const QString& id, const QString& name, OneCb cb);
  void remove(const QString& id, bool keepSaves, DoneCb cb);
  void runVariant(const QString& runId, RunCb cb);
  // state: not_built | building | ready
  void setRunVariantState(const QString& runId, const QString& state, DoneCb cb);

 private:
  void send(const QByteArray& method, const QString& path, const QByteArray& body, std::function<void(const HttpResult&)> cb);
  static VariantCall callOf(const HttpResult& r);

  QPointer<HubConnection> conn_;
};

}  // namespace uzume

Q_DECLARE_METATYPE(uzume::VariantInfo)
