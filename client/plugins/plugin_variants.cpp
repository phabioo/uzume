#include "plugin_variants.h"

#include <QMutexLocker>

namespace uzume::plugins {

void VariantRegistry::setOnChange(std::function<void()> cb) {
  QMutexLocker l(&mutex_);
  onChange_ = std::move(cb);
}

void VariantRegistry::changed() {
  std::function<void()> cb;
  {
    QMutexLocker l(&mutex_);
    cb = onChange_;
  }
  if (cb) cb();
}

void VariantRegistry::set(const VariantRegistration& reg) {
  {
    QMutexLocker l(&mutex_);
    byPlugin_.insert(reg.pluginId, reg);
  }
  changed();
}

void VariantRegistry::remove(const QString& pluginId) {
  bool had;
  {
    QMutexLocker l(&mutex_);
    had = byPlugin_.remove(pluginId) > 0;
  }
  if (had) changed();
}

void VariantRegistry::clear() {
  bool had;
  {
    QMutexLocker l(&mutex_);
    had = !byPlugin_.isEmpty();
    byPlugin_.clear();
  }
  if (had) changed();
}

void VariantRegistry::keepOnly(const QSet<QString>& pluginIds) {
  bool any = false;
  {
    QMutexLocker l(&mutex_);
    for (auto it = byPlugin_.begin(); it != byPlugin_.end();) {
      if (pluginIds.contains(it.key())) {
        ++it;
      } else {
        it = byPlugin_.erase(it);
        any = true;
      }
    }
  }
  if (any) changed();
}

QList<VariantRegistration> VariantRegistry::all() const {
  QMutexLocker l(&mutex_);
  return byPlugin_.values();
}

bool VariantRegistry::hasEngine(const QString& engine) const {
  QMutexLocker l(&mutex_);
  for (const VariantRegistration& r : byPlugin_)
    if (r.engine == engine) return true;
  return false;
}

namespace {
bool matches(const VariantRegistration& r, const QString& system, const QString& code) {
  for (const VariantGameSupport& g : r.games) {
    if (g.system != system) continue;
    if (g.codes.isEmpty()) return true;
    for (const QString& c : g.codes)
      if (c.compare(code, Qt::CaseInsensitive) == 0) return true;
  }
  return false;
}
}  // namespace

bool VariantRegistry::supported(const QString& system, const QString& code) const {
  QMutexLocker l(&mutex_);
  for (const VariantRegistration& r : byPlugin_)
    if (matches(r, system, code)) return true;
  return false;
}

QList<VariantPreset> VariantRegistry::presetsFor(const QString& system, const QString& code) const {
  QMutexLocker l(&mutex_);
  QList<VariantPreset> out;
  for (const VariantRegistration& r : byPlugin_)
    if (matches(r, system, code)) out += r.presets;
  return out;
}

VariantPreset VariantRegistry::preset(const QString& id) const {
  QMutexLocker l(&mutex_);
  for (const VariantRegistration& r : byPlugin_)
    for (const VariantPreset& p : r.presets)
      if (p.id == id) return p;
  return {};
}

}  // namespace uzume::plugins
