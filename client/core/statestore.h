#pragma once
// StateStore: the local cache of save states (0.20 package 4). A save state is a core's serialization of the running game (not
// the game's save file); it belongs to one person on one server, one game, one save slot and a state number. Pure file logic.
//
// Numbers: 0 is the Auto state ("A": the app keeps the progress of the moment before a state is loaded, and a state saved by the
// auto key), 1 to 4 are the slots the person sees. The server allows 0 to 10; the app uses 0 to 4.
// Layout: <states dir>/<game id>/<save slot>/<n>.state (the blob), <n>.png (preview, optional), <n>.json (StateRecord).
// <states dir> is <data>/hubs/<hub id>/users/<user id>/states (SaveStore keeps the saves next to it), so states of two servers or
// two people never meet, even for the same ROM.
#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>
#include <optional>

namespace uzume {

class ProfileStore;

inline constexpr int kStateAuto = 0;
inline constexpr int kStateSlotCount = 4;
inline constexpr qint64 kMaxStateBytes = 64ll * 1024 * 1024;  // server limit per state blob
inline constexpr qint64 kMaxStatePreviewBytes = 512 * 1024;   // server limit per preview PNG

struct StateRecord {
  QString gameId;
  QString saveSlot = QStringLiteral("default");
  int n = 0;
  QString coreId;
  QString coreVersion;
  qint64 sizeBytes = 0;
  QString sha256;
  QString createdAt;   // ISO 8601, UTC
  QString deviceName;  // the device that saved it (server list); empty = this one
  bool hasPreview = false;
  bool onServer = false;       // the server holds exactly this state (same sha256)
  bool pendingUpload = false;  // saved here, not on the server yet (offline, or the upload failed)
  bool deletePending = false;  // deleted here, the server still has it (offline); the blob is gone
  QJsonObject toJson() const;
  static std::optional<StateRecord> fromJson(const QJsonObject& o);
};

class StateStore {
 public:
  explicit StateStore(QString root = QString()) : root_(std::move(root)) {}
  const QString& root() const { return root_; }
  bool valid() const { return !root_.isEmpty(); }

  static QString userStatesDir(const ProfileStore& profiles, const QString& hubId, const QString& userId);  // empty for unsafe ids
  static bool validNumber(int n) { return n >= 0 && n <= kStateSlotCount; }
  static QString label(int n);           // "A", "1" ... "4"
  static int nextSlot(int current);      // 1 -> 2 -> 3 -> 4 -> 1; Auto (and anything else) -> 1
  static int firstFreeSlot(const QList<StateRecord>& have);  // lowest of 1..4 without a state, 0 = all four taken

  // Stores a state: the blob, the optional preview PNG and the record (size and sha256 are computed here, createdAt is set when
  // empty). Replaces what was there. false: invalid ids, too large, or the disk refused.
  bool put(StateRecord rec, const QByteArray& state, const QByteArray& previewPng = QByteArray());
  // Updates the record of an existing state (flags, device name ...); the blob stays.
  bool updateRecord(const StateRecord& rec);
  // Creates or replaces just the record (a state of another device that is not downloaded yet); keeps a blob and preview that exist
  // when the sha256 is unchanged, else drops them.
  bool putRecord(StateRecord rec);
  bool putPreview(const QString& gameId, const QString& saveSlot, int n, const QByteArray& png);  // record must exist
  bool hasBlob(const QString& gameId, const QString& saveSlot, int n) const;
  QString previewPath(const QString& gameId, const QString& saveSlot, int n) const;  // empty without a preview file
  std::optional<StateRecord> record(const QString& gameId, const QString& saveSlot, int n) const;
  // All records of a game and save slot, ordered by number (including deletePending ones: callers filter).
  QList<StateRecord> list(const QString& gameId, const QString& saveSlot) const;
  QByteArray state(const QString& gameId, const QString& saveSlot, int n) const;
  QByteArray preview(const QString& gameId, const QString& saveSlot, int n) const;
  // Removes the state and its files. true if something was there.
  bool remove(const QString& gameId, const QString& saveSlot, int n);
  // Every record of every game with something to do on the server (pendingUpload or deletePending).
  QList<StateRecord> pending() const;

  qint64 bytes() const;  // everything under the root
  // States of games that are not in `libraryGameIds` (games deleted from the server's library).
  struct Orphans {
    qint64 bytes = 0;
    int games = 0;
    int states = 0;
  };
  Orphans orphans(const QSet<QString>& libraryGameIds) const;
  Orphans removeOrphans(const QSet<QString>& libraryGameIds);
  // Size of the states folders of every server and person under <data>/hubs.
  static qint64 measureAll(const QString& dataDir);

 private:
  QString dirFor(const QString& gameId, const QString& saveSlot) const;  // empty for unsafe names
  QString fileFor(const QString& gameId, const QString& saveSlot, int n, const char* ext) const;
  QString root_;
};

}  // namespace uzume
