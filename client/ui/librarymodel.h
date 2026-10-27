#pragma once
// LibraryModel: games of the active hub with ROM status, filterable by title and "Ready".

#include <QAbstractListModel>
#include <QCollator>
#include <QDate>
#include <QSortFilterProxyModel>
#include <QList>
#include <QHash>
#include <QSet>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>
#include <functional>
#include <optional>

#include "hubprotocol.h"
#include "personalapi.h"
#include "romdownloader.h"

namespace uzume::ui {

class LibraryModel : public QAbstractListModel {
  Q_OBJECT
  QML_ELEMENT
  QML_UNCREATABLE("Provided by the PlayerController")
  Q_PROPERTY(QString filterText READ filterText WRITE setFilterText NOTIFY filterChanged)
  Q_PROPERTY(bool readyOnly READ readyOnly WRITE setReadyOnly NOTIFY filterChanged)
  // Filter chips of the Library (3c): "all" | "ready" | "attention" | "download". Combined with filterText.
  Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)
  // Counts of the chips: {all, ready, attention, download}. Every game is in exactly one of the last three
  // (attention wins, then ready), so they add up to all.
  Q_PROPERTY(QVariantMap counts READ counts NOTIFY countChanged)
  Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
  Q_PROPERTY(int totalCount READ totalCount NOTIFY countChanged)
  Q_PROPERTY(int readyCount READ readyCount NOTIFY countChanged)
  Q_PROPERTY(int conflictCount READ conflictCount NOTIFY countChanged)  // games whose save is in conflict (admin attention list)
  Q_PROPERTY(int systemCount READ systemCount NOTIFY countChanged)  // distinct systems among all games ("ON THIS PC · 214 GAMES · 5 SYSTEMS")
  // Collection chips (3c-0, Uzume 0.19): which games belong to the shown list. "all" | "favorites" | a collection id. Combined with
  // the status chip and the search; an unknown or deleted id falls back to "all".
  Q_PROPERTY(QString collectionFilter READ collectionFilter WRITE setCollectionFilter NOTIFY filterChanged)
  // True when the server keeps favourites and collections per person (any game came with `personal`, or GET /collections worked).
  Q_PROPERTY(bool personalSupported READ personalSupported NOTIFY collectionsChanged)
  // Chips of the collection row: [{value, label, count, kind: "all" | "favorites" | "collection"}], without "+ Collection".
  Q_PROPERTY(QVariantList collectionChips READ collectionChips NOTIFY collectionsChanged)
  // System filter of the "Systems…" menu: "" (all systems) or a system id (case-insensitive). Combined with the collection, the
  // status filter and the search. systemChips: [{value, label, count}] per system among all games, sorted by label.
  Q_PROPERTY(QString systemFilter READ systemFilter WRITE setSystemFilter NOTIFY filterChanged)
  Q_PROPERTY(QVariantList systemChips READ systemChips NOTIFY countChanged)
  // Sort (3c-2): "name_asc" | "name_desc" | "added_desc" | "added_asc" | "size_desc" | "size_asc" | "system" | "played"
  // (recently played, the default; server last played or this device's, the newer one) | "playtime" (most play time first).
  // Persisted per device by the PlayerController (PlayerSettings).
  Q_PROPERTY(QString sortKey READ sortKey WRITE setSortKey NOTIFY sortChanged)
  // "Ready first": ready games on top, a divider, then the rest; same sort inside both groups. Not applied (and the toggle
  // disabled) while the "Ready" chip is active: every result is ready then.
  Q_PROPERTY(bool readyFirst READ readyFirst WRITE setReadyFirst NOTIFY sortChanged)
  Q_PROPERTY(bool readyFirstApplicable READ readyFirstApplicable NOTIFY filterChanged)
  // Visible games in the "ready" group / the rest when grouping is on (otherwise all visible games count as ready group).
  Q_PROPERTY(bool grouped READ grouped NOTIFY countChanged)
  Q_PROPERTY(int readyGroupCount READ readyGroupCount NOTIFY countChanged)
  Q_PROPERTY(int notReadyGroupCount READ notReadyGroupCount NOTIFY countChanged)
  // Toolbar count (decision ac): "42 games · Nintendo DS", "37 of 42 games", "0 of 37 ready games".
  Q_PROPERTY(QString countText READ countText NOTIFY countChanged)
  // Empty result: {count, title, reason} of the games that match the search but not the chip; {} otherwise.
  Q_PROPERTY(QVariantMap outsideFilter READ outsideFilter NOTIFY countChanged)
  // "Continue" block (3c-0): [{gameId, title, system, meta, dateText}], the last played game first, then up to two
  // "RECENT" ones. Only games still in the library; empty while nothing was played (3c-4 state C).
  Q_PROPERTY(QVariantList continueEntries READ continueEntries NOTIFY continueChanged)
 public:
  enum Roles {
    GameIdRole = Qt::UserRole + 1,
    TitleRole,
    SystemRole,       // e.g. "NDS"
    MonogramRole,     // up to two initials
    RomShaRole,
    RomSizeRole,
    StateKindRole,    // ready | download | validating | downloading | mismatch | failed | missing | unknown
    StatusTextRole,   // e.g. "Download needed · 128 MB"
    StatusToneRole,   // ok | neutral | warn | error
    ProgressRole,     // 0..1, only meaningful for downloading
    SyncKindRole,     // none | synced | pending | conflict (save sync)
    SyncTextRole,     // "Synced" | "Sync pending" | "Conflict" | ""
    TileTextRole,     // status line of the tile per 3c, with leading symbol ("● Ready", "▲ Save conflict", ...)
    TileToneRole,     // ok | neutral | warn | error
    NeedsAttentionRole,  // bool, see attentionOf()
    ReadyGroupRole,      // bool: "ready" for Ready first (ROM ready, no attention, save sync not pending)
    AddedTextRole,       // "DD.MM.YYYY" (Hub added_at, local time); empty if unknown
    OnHubRole,           // bool: a file of the game is stored on the Hub (false = local-only game, "Local" pill)
    LocalFileRole,       // bool: the game starts from a local file of a library folder (with OnHub: "Local · on Hub")
    PlaySecondsRole,     // qint64: play time of the signed-in person (0 = never or an older server)
    PlayTimeTextRole,    // "12 h", "45 min", "–" when never
    SessionsRole,        // int
    LastPlayedMsRole,    // qint64 Unix ms, max(server last played, this device's); 0 = never
    LastPlayedTextRole,  // "Today", "Yesterday", "3 days ago", "DD.MM.YYYY", "–" when never
    FavoriteRole,        // bool
    CollectionIdsRole,   // QStringList
    StatusWordRole,      // the status line without its symbol ("Ready", "Core missing", "Not downloaded · 1 MB")
    StatusWarnRole       // bool: the row needs attention (warning square before the word)
  };
  Q_ENUM(Roles)

  explicit LibraryModel(QObject* parent = nullptr);

  int rowCount(const QModelIndex& parent = {}) const override;
  QVariant data(const QModelIndex& index, int role) const override;
  QHash<int, QByteArray> roleNames() const override;

  QString filterText() const { return filterText_; }
  void setFilterText(const QString& t);
  bool readyOnly() const { return filter_ == QLatin1String("ready"); }
  void setReadyOnly(bool on);
  int conflictCount() const;
  int totalCount() const { return static_cast<int>(items_.size()); }
  int readyCount() const;
  int systemCount() const {
    QSet<QString> s;
    for (const Item& it : items_) s.insert(it.game.system);
    return static_cast<int>(s.size());
  }
  QString systemFilter() const { return system_; }
  void setSystemFilter(const QString& system);
  QVariantList systemChips() const;
  int gamesForSystem(const QString& system) const;
  QString filter() const { return filter_; }
  void setFilter(const QString& filter);
  // Chip counts; they follow the search text (not the chip filter), see counts(false) for the totals.
  QVariantMap counts() const { return countsFor(true); }
  QString sortKey() const { return sortKey_; }
  void setSortKey(const QString& key);  // unknown keys are ignored
  static QStringList sortKeys();
  bool readyFirst() const { return readyFirst_; }
  void setReadyFirst(bool on);
  bool readyFirstApplicable() const { return filter_ != QLatin1String("ready"); }
  bool grouped() const { return readyFirst_ && readyFirstApplicable() && readyGroupCount_ > 0 && notReadyGroupCount_ > 0; }
  int readyGroupCount() const { return readyGroupCount_; }
  int notReadyGroupCount() const { return notReadyGroupCount_; }
  QString countText() const;
  QVariantMap outsideFilter() const;
  // --- per person (Uzume 0.19) ---
  QString collectionFilter() const { return collection_; }
  void setCollectionFilter(const QString& value);
  bool personalSupported() const { return personalSupported_; }
  void setPersonalSupported(bool on);
  QVariantList collectionChips() const;
  // The collections of the person (order as served); membership of the games follows each collection's game ids. An open
  // collection filter whose collection is gone falls back to "all".
  void setCollections(const QList<CollectionInfo>& collections);
  QList<CollectionInfo> collections() const { return collections_; }
  // Local changes ahead of the server's answer (the caller reverts them when the request fails).
  void setFavorite(const QString& gameId, bool favorite);
  void setCollectionMember(const QString& gameId, const QString& collectionId, bool member);
  // {supported, playSeconds, playTimeText, sessions, lastPlayedMs, lastPlayedText, favorite, collectionIds, collectionNames};
  // empty for an unknown game.
  Q_INVOKABLE QVariantMap personalFor(const QString& gameId) const;
  Q_INVOKABLE static QString formatPlayTime(qint64 seconds);                     // "–" for 0
  static QString formatLastPlayed(qint64 msecs, qint64 nowMsecs = 0);            // "–" for 0; nowMsecs 0 = now
  // Last played on this device (Unix ms per game id of the active Hub); never played = missing.
  void setLastPlayed(const QHash<QString, qint64>& byGameId);
  void noteLastPlayed(const QString& gameId, qint64 msecs);
  // Variants of the games (0.21 Randomizer, 10g-1): gameId -> {count, buildPercent (-1 none), buildName, lastId, lastName, lastPlayedMs}.
  // The status of a ready game shows the count; a running build shows "Building n %"; Continue resumes the variant played last.
  void setVariantBadges(const QVariantMap& byGameId);
  QVariantList continueEntries() const { return continueEntriesAt(QDate::currentDate()); }
  QVariantList continueEntriesAt(const QDate& today) const;
  // "TODAY" | "YESTERDAY" | "16 OCT" (uppercase, for the Mono meta line).
  static QString playedDateText(qint64 msecs, const QDate& today);
  // Label for the toolbar count ("Nintendo DS"): set by the PlayerController, empty = none or several systems.
  void setSystemLabel(const QString& label);
  Q_INVOKABLE bool isReady(const QString& gameId) const;
  // "Needs attention" (D14): save conflict, hash mismatch, core missing/incompatible, firmware missing.
  // Core and firmware problems come from the PlayerController (game id -> short text); sync pending is not attention.
  void setAttention(const QHash<QString, QString>& byGameId);
  QString attentionText(const QString& gameId) const;

  void setGames(const QList<GameEntry>& games, const std::function<RomStatus(const GameEntry&)>& statusOf);
  void clear();
  void setStatus(const QString& romSha256, const RomStatus& status);
  void setSyncKind(const QString& gameId, const QString& kind);
  // Applies many kinds (game id -> kind) with one rebuild.
  void setSyncKinds(const QHash<QString, QString>& kinds);
  QString syncKind(const QString& gameId) const;
  static QString syncText(const QString& kind);
  QVariantMap variantBadge(const QString& gameId) const;
  static QString variantCountText(int count);

  Q_INVOKABLE int rowOfGame(const QString& gameId) const;
  std::optional<GameEntry> game(const QString& gameId) const;
  std::optional<RomStatus> status(const QString& gameId) const;

  static QString formatSize(qint64 bytes);
  static QString monogram(const QString& title);
  static QString stateKind(RomState s);

 signals:
  void filterChanged();
  void countChanged();
  void sortChanged();
  void continueChanged();
  void collectionsChanged();

 private:
  struct Item {
    GameEntry game;
    RomStatus status;
    QString sync = QStringLiteral("none");
    QString attention;  // core / firmware problem
    qint64 addedMs = 0;
  };
  enum class Category { Ready, Attention, Download };
  static Category categoryOf(const Item& it);
  static QString attentionOf(const Item& it);  // short reason, empty = none
  bool matches(const Item& it) const;
  bool matchesText(const Item& it) const;
  bool matchesCollection(const Item& it) const;
  qint64 lastPlayedOf(const Item& it) const;
  void afterPersonalChange();
  bool inReadyGroup(const Item& it) const;
  bool lessThan(const Item& a, const Item& b) const;
  int compareTitles(const QString& a, const QString& b) const;
  QVariantMap countsFor(bool applyText) const;
  static qint64 parseAdded(const QString& iso);
  void rebuild();
  void sortVisible(QList<int>& list) const;

  QList<Item> items_;
  QList<int> visible_;  // indices into items_
  QString filterText_;
  QString filter_ = QStringLiteral("all");
  QString collection_ = QStringLiteral("all");
  QString system_;
  QList<CollectionInfo> collections_;
  bool personalSupported_ = false;
  QString sortKey_ = QStringLiteral("played");
  bool readyFirst_ = false;
  QHash<QString, qint64> lastPlayed_;
  QVariantMap variantBadges_;
  QString systemLabel_;
  QCollator collator_;
  int readyGroupCount_ = 0;
  int notReadyGroupCount_ = 0;
};

// Filters the LibraryModel to the ready group (group = 0), the not-ready group (group = 1) or everything (group = -1),
// so the Library can draw the two groups as separate grids with a divider between them.
class LibraryGroupModel : public QSortFilterProxyModel {
  Q_OBJECT
  QML_ELEMENT
  Q_PROPERTY(QObject* library READ library WRITE setLibrary NOTIFY libraryChanged)
  Q_PROPERTY(int group READ group WRITE setGroup NOTIFY groupChanged)
 public:
  explicit LibraryGroupModel(QObject* parent = nullptr) : QSortFilterProxyModel(parent) {}
  QObject* library() const { return library_; }
  void setLibrary(QObject* library);
  int group() const { return group_; }
  void setGroup(int group);
 signals:
  void libraryChanged();
  void groupChanged();
 protected:
  bool filterAcceptsRow(int row, const QModelIndex& parent) const override;
 private:
  QObject* library_ = nullptr;
  int group_ = -1;
};

}  // namespace uzume::ui
