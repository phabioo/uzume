#pragma once
// LocalStorageController (Uzume 0.19, design 6l-1 "Settings > This PC > Storage"). Measures what the app keeps on this PC in a
// worker thread (never on the UI thread) and runs the clean-up suggestions after the person confirmed them in QML:
//   old_games     downloaded games not played for 60 days (never the running game or a download in progress; they download again)
//   unused_cores  cached core versions that are not the newest of their core and not the running game's core
//   deleted_game_states  the local copies of save states of games that are no longer in the library (0.20; the saves are never touched)
// Saves are never measured as clean-up and never removed.
#include <QFutureWatcher>
#include <QObject>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>
#include <functional>

#include "localstorage.h"

namespace uzume::ui {

class LocalStorageController : public QObject {
  Q_OBJECT
  QML_ELEMENT
  QML_UNCREATABLE("Provided by the PlayerController")
  Q_PROPERTY(bool measuring READ measuring NOTIFY measuringChanged)
  // Empty until the first measurement ended. Otherwise: {dataPath, measuredAt (ms), measuredText ("hh:mm"), totalBytes, totalText, freeBytes,
  // freeText, usedBytes, usedText, kinds: [{id: games|saves|states|screenshots|cores, bytes, text, fraction (of the volume, 0..1)}],
  // cleanup: [{id: old_games|deleted_game_states|unused_cores, bytes, text, count}] (only suggestions with something to clean)}.
  Q_PROPERTY(QVariantMap report READ report NOTIFY reportChanged)
 public:
  // Called on the UI thread for every measurement and clean-up: the folders, the protected hashes and the running core of that moment.
  using InputSource = std::function<LocalStorageInput()>;
  explicit LocalStorageController(InputSource source, QObject* parent = nullptr);

  bool measuring() const { return measuring_; }
  QVariantMap report() const { return report_; }

  Q_INVOKABLE void measure();
  // Runs one suggestion now (the confirmation happened in the UI). Returns {ok, freedBytes, freedText, removed}; ok false for an unknown
  // id. Measures again afterwards.
  Q_INVOKABLE QVariantMap cleanUp(const QString& id);
  static QString formatBytes(qint64 bytes);

 signals:
  void measuringChanged();
  void reportChanged();
  void gamesRemoved();  // the ROM cache changed: the game page and the ROM cache numbers must refresh

 private:
  static QVariantMap toMap(const LocalStorageReport& r);
  InputSource source_;
  bool measuring_ = false;
  bool again_ = false;  // a measurement was requested while one ran
  QVariantMap report_;
  QFutureWatcher<LocalStorageReport> watcher_;
};

}  // namespace uzume::ui
