// UI-control tools of the debug MCP server (ADR 0032 D3 "UI control", D4 brakes): read the QML item tree, click, type, press
// keys, scroll, navigate, start and stop a game, take a window screenshot. Input is synthesized as real Qt events on the
// QQuickWindow (mouse, key, wheel; QtTest is not used) so the app behaves as it does for a person. Destructive controls are
// never operated: ui_click and ui_key/ui_type (Return, Enter, Space) on them answer isError "needs_confirmation_in_app".
#include <QAccessible>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QHash>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMetaMethod>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QTimer>
#include <QWheelEvent>
#include <QtGui/qtguiglobal.h>
#include <algorithm>
#include <functional>
#include <memory>

#include "mcp_tools.h"
#include "playercontroller.h"
#include "savehistorycontroller.h"
#include "tools_util.h"

// Exported by QtGui (the entry point QtTest uses too): runs the shortcut map for a key press so Shortcut items fire.
Q_GUI_EXPORT bool qt_sendShortcutOverrideEvent(QObject* o, ulong timestamp, int k, Qt::KeyboardModifiers mods, const QString& text,
                                               bool autorep, ushort count);

namespace uzume::mcp {

using namespace tools;

namespace {

constexpr int kMaxTreeBytes = 200 * 1024;  // ui_tree output cap
constexpr int kDefaultDepth = 12;
constexpr int kMaxText = 200;
constexpr int kSettleMs = 80;        // after an input event: let the UI react before the answer
constexpr int kMaxVisited = 400000;  // safety net for the item walks

QString q(const char* s) { return QString::fromLatin1(s); }

// ------------------------------------------------------------------------------------------------ windows and paths

QList<QQuickWindow*> appWindows() {
  QList<QQuickWindow*> out;
  const QWindowList all = QGuiApplication::allWindows();
  for (QWindow* w : all)
    if (auto* qw = qobject_cast<QQuickWindow*>(w); qw != nullptr && qw->isVisible()) out.append(qw);
  return out;
}

// `spec`: empty = the active window (else the first); an index; an objectName; or a part of the title.
QQuickWindow* pickWindow(const QString& spec, QString* error) {
  const QList<QQuickWindow*> ws = appWindows();
  if (ws.isEmpty()) {
    if (error) *error = QStringLiteral("no_window");
    return nullptr;
  }
  if (spec.isEmpty()) {
    for (QQuickWindow* w : ws)
      if (w == QGuiApplication::focusWindow()) return w;
    for (QQuickWindow* w : ws)
      if (w->isActive()) return w;
    return ws.first();
  }
  bool ok = false;
  const int idx = spec.toInt(&ok);
  if (ok && idx >= 0 && idx < ws.size()) return ws.at(idx);
  for (QQuickWindow* w : ws)
    if (w->objectName() == spec) return w;
  for (QQuickWindow* w : ws)
    if (w->title().contains(spec, Qt::CaseInsensitive)) return w;
  if (error) *error = QStringLiteral("unknown_window: %1 (0..%2, objectName or title)").arg(spec).arg(ws.size() - 1);
  return nullptr;
}

int windowIndex(const QQuickWindow* w) { return static_cast<int>(appWindows().indexOf(const_cast<QQuickWindow*>(w))); }

// "<window>/<child>/<child>..." : window index among the visible windows, then child indices (QQuickItem::childItems order).
QString pathOf(const QQuickItem* it) {
  QStringList parts;
  const QQuickItem* cur = it;
  while (cur->parentItem() != nullptr) {
    const QQuickItem* p = cur->parentItem();
    parts.prepend(QString::number(p->childItems().indexOf(const_cast<QQuickItem*>(cur))));
    cur = p;
  }
  parts.prepend(QString::number(cur->window() != nullptr ? windowIndex(cur->window()) : -1));
  return parts.join(QLatin1Char('/'));
}

QQuickItem* resolvePath(const QString& path) {
  static const QRegularExpression re(QStringLiteral("^\\d+(/\\d+)*$"));
  if (!re.match(path).hasMatch()) return nullptr;
  const QStringList parts = path.split(QLatin1Char('/'));
  const QList<QQuickWindow*> ws = appWindows();
  const int wi = parts.first().toInt();
  if (wi < 0 || wi >= ws.size()) return nullptr;
  QQuickItem* cur = ws.at(wi)->contentItem();
  for (int i = 1; i < parts.size() && cur != nullptr; ++i) {
    const QList<QQuickItem*> kids = cur->childItems();
    const int ci = parts.at(i).toInt();
    cur = ci >= 0 && ci < kids.size() ? kids.at(ci) : nullptr;
  }
  return cur;
}

// ------------------------------------------------------------------------------------------------ item properties

QString typeName(const QObject* o) {
  QString n = QString::fromLatin1(o->metaObject()->className());
  for (const char* marker : {"_QMLTYPE_", "_QML_"}) {
    const qsizetype i = n.indexOf(QLatin1String(marker));
    if (i > 0) n.truncate(i);
  }
  if (n.startsWith(QLatin1String("QQuick")) && n.size() > 6) n = n.mid(6);
  return n;
}

bool hasProp(const QObject* o, const char* name) { return o->metaObject()->indexOfProperty(name) >= 0; }

bool stringProp(const QObject* o, const char* name, QString* out) {
  if (!hasProp(o, name)) return false;
  const QVariant v = o->property(name);
  if (v.metaType().id() != QMetaType::QString) return false;
  *out = v.toString();
  return true;
}

bool boolProp(const QObject* o, const char* name, bool* out = nullptr) {
  if (!hasProp(o, name)) return false;
  const QVariant v = o->property(name);
  if (v.metaType().id() != QMetaType::Bool) return false;
  if (out) *out = v.toBool();
  return true;
}

bool isTrue(const QObject* o, const char* name) {
  bool b = false;
  return boolProp(o, name, &b) && b;
}

// A password field, or a field of a component that declares itself `secret`: its content is never read out.
bool isSecret(const QQuickItem* it) {
  int hops = 0;
  for (const QQuickItem* cur = it; cur != nullptr && hops < 4; cur = cur->parentItem(), ++hops) {
    if (hasProp(cur, "echoMode")) {
      const QVariant e = cur->property("echoMode");
      if (e.isValid() && e.toInt() != 0) return true;  // TextInput.Normal == 0
    }
    if (isTrue(cur, "secret")) return true;
  }
  return false;
}

QString clip(QString s) {
  s = s.simplified();
  if (s.size() > kMaxText) s = s.left(kMaxText) + QStringLiteral("…");
  return s;
}

QObject* accessibleOf(const QQuickItem* it) {
  for (QObject* c : it->children())
    if (qstrcmp(c->metaObject()->className(), "QQuickAccessibleAttached") == 0) return c;
  return nullptr;
}

QString roleName(int role) {
  switch (role) {
    case QAccessible::Button: return QStringLiteral("button");
    case QAccessible::CheckBox: return QStringLiteral("checkbox");
    case QAccessible::RadioButton: return QStringLiteral("radiobutton");
    case QAccessible::ComboBox: return QStringLiteral("combobox");
    case QAccessible::EditableText: return QStringLiteral("textfield");
    case QAccessible::StaticText: return QStringLiteral("text");
    case QAccessible::Dialog: return QStringLiteral("dialog");
    case QAccessible::PageTab: return QStringLiteral("tab");
    case QAccessible::PageTabList: return QStringLiteral("tablist");
    case QAccessible::Pane: return QStringLiteral("pane");
    case QAccessible::List: return QStringLiteral("list");
    case QAccessible::ListItem: return QStringLiteral("listitem");
    case QAccessible::Slider: return QStringLiteral("slider");
    case QAccessible::SpinBox: return QStringLiteral("spinbox");
    case QAccessible::ProgressBar: return QStringLiteral("progressbar");
    case QAccessible::MenuItem: return QStringLiteral("menuitem");
    case QAccessible::PopupMenu: return QStringLiteral("menu");
    case QAccessible::Heading: return QStringLiteral("heading");
    case QAccessible::Link: return QStringLiteral("link");
    case QAccessible::Graphic: return QStringLiteral("image");
    case QAccessible::Grouping: return QStringLiteral("group");
    case QAccessible::Separator: return QStringLiteral("separator");
    case QAccessible::ToolBar: return QStringLiteral("toolbar");
    default: return QString::number(role);
  }
}

// Has a clicked() signal: buttons, switches, check boxes, mouse areas.
bool isClickable(const QObject* o) {
  static QHash<const QMetaObject*, bool> cache;
  const QMetaObject* mo = o->metaObject();
  if (const auto it = cache.constFind(mo); it != cache.constEnd()) return it.value();
  bool found = false;
  for (int i = mo->methodCount() - 1; i >= 0 && !found; --i) {
    const QMetaMethod m = mo->method(i);
    found = m.methodType() == QMetaMethod::Signal && m.name() == "clicked";
  }
  cache.insert(mo, found);
  return found;
}

struct Labels {
  QString text, label, placeholder, title, accName;
  bool secret = false;
};

Labels labelsOf(const QQuickItem* it) {
  Labels l;
  l.secret = isSecret(it);
  QString s;
  if (stringProp(it, "text", &s)) l.text = l.secret ? (s.isEmpty() ? QString() : QStringLiteral("***")) : s;
  if (l.text.isEmpty() && !l.secret && stringProp(it, "displayText", &s)) l.text = s;
  if (stringProp(it, "label", &s)) l.label = s;
  if (stringProp(it, "placeholderText", &s)) l.placeholder = s;
  if (stringProp(it, "title", &s)) l.title = s;
  if (QObject* a = accessibleOf(it)) {
    if (stringProp(a, "name", &s)) l.accName = s;
  }
  return l;
}

bool isInput(const QQuickItem* it) { return hasProp(it, "echoMode") || (hasProp(it, "text") && hasProp(it, "cursorPosition")); }

bool interesting(const QQuickItem* it, const Labels& l) {
  return !it->objectName().isEmpty() || !l.text.isEmpty() || !l.label.isEmpty() || !l.placeholder.isEmpty() || !l.title.isEmpty() ||
         !l.accName.isEmpty() || isClickable(it) || hasProp(it, "checked") || isInput(it) || it->hasActiveFocus();
}

// ------------------------------------------------------------------------------------------------ the destructive brake

// Controls that only the person may operate in the app. A UzButton has `destructive` (true for kind "danger"); the name and label
// rules below cover everything else that deletes, removes, overwrites, shares, installs or updates.
struct Destructive {
  bool hit = false;
  QString who;  // objectName or type, for the log and the answer
  QString reason;
};

// The label rule is for push buttons only: a switch such as "Install updates automatically" is a setting, not an action.
// `selfOnly`: look at the item alone (the tree flags); otherwise also at its ancestors (a label inside a button).
Destructive destructiveOf(const QQuickItem* start, bool selfOnly = false) {
  static const QRegularExpression nameRe(
      QStringLiteral("(delete|remove|erase|wipe|uninstall|revoke|forget|discard|overwrite|rollback|restore|stopsharing|signoutall|"
                     "signouteverywhere|installupdate|updatenow|restartinstall)"),
      QRegularExpression::CaseInsensitiveOption);
  static const QRegularExpression nameSafeRe(QStringLiteral("(cancel|close|back|dismiss|keep|no)$"), QRegularExpression::CaseInsensitiveOption);
  static const QRegularExpression textRe(
      QStringLiteral("^\\W*(delete|remove|erase|wipe|uninstall|revoke|forget|discard|overwrite|reset|restore|roll ?back|stop sharing|"
                     "sign out everywhere|share\\b|play and share|install\\b|update\\b|upgrade\\b|restart and (install|update)|"
                     "download and install)"),
      QRegularExpression::CaseInsensitiveOption);
  int hops = 0;
  for (const QQuickItem* it = start; it != nullptr && hops < 16; it = it->parentItem(), ++hops) {
    if (hops > 0 && selfOnly) break;
    const QString who = it->objectName().isEmpty() ? typeName(it) : it->objectName();
    if (isTrue(it, "destructive")) return {true, who, QStringLiteral("marked destructive")};
    QString kind;
    if (stringProp(it, "kind", &kind) && kind == QLatin1String("danger")) return {true, who, QStringLiteral("danger button")};
    if (!isClickable(it)) continue;
    const QString name = it->objectName();
    if (!name.isEmpty() && nameRe.match(name).hasMatch() && !nameSafeRe.match(name).hasMatch())
      return {true, who, QStringLiteral("name suggests delete, remove, restore or install")};
    QString text;
    if (!isTrue(it, "checkable") && stringProp(it, "text", &text) && textRe.match(text.trimmed()).hasMatch())
      return {true, who, QStringLiteral("label suggests delete, remove, share, install or update")};
  }
  return {};
}

ToolResult refuse(const QString& tool, const Destructive& d) {
  // Logged by object name or type only: never texts.
  qWarning().noquote() << "mcp: refused" << tool << "needs_confirmation_in_app" << d.who;
  ToolResult r = ToolResult::error(QStringLiteral("needs_confirmation_in_app"));
  r.addText(QStringLiteral("'%1' is a destructive control (%2). Ask the person to operate it in the app themselves.").arg(d.who, d.reason));
  return r;
}

// ------------------------------------------------------------------------------------------------ describing items

QJsonObject describe(QQuickItem* it, const QString& path) {
  QJsonObject o;
  o.insert(QStringLiteral("type"), typeName(it));
  if (!it->objectName().isEmpty()) o.insert(QStringLiteral("objectName"), it->objectName());
  o.insert(QStringLiteral("path"), path);
  const Labels l = labelsOf(it);
  if (!l.text.isEmpty()) o.insert(QStringLiteral("text"), clip(l.text));
  if (!l.label.isEmpty()) o.insert(QStringLiteral("label"), clip(l.label));
  if (!l.placeholder.isEmpty()) o.insert(QStringLiteral("placeholder"), clip(l.placeholder));
  if (!l.title.isEmpty() && l.title != l.label) o.insert(QStringLiteral("title"), clip(l.title));
  if (l.secret) o.insert(QStringLiteral("secret"), true);
  bool checked = false;
  if (boolProp(it, "checked", &checked)) o.insert(QStringLiteral("checked"), checked);
  o.insert(QStringLiteral("enabled"), it->isEnabled());
  o.insert(QStringLiteral("visible"), it->isVisible());
  if (it->hasActiveFocus()) o.insert(QStringLiteral("focus"), true);
  if (isClickable(it)) o.insert(QStringLiteral("clickable"), true);
  if (hasProp(it, "value") && !l.secret) {
    const QVariant v = it->property("value");
    const int id = v.metaType().id();
    if (id == QMetaType::Int || id == QMetaType::Double || id == QMetaType::Float || id == QMetaType::LongLong) o.insert(QStringLiteral("value"), v.toDouble());
  }
  if (QObject* a = accessibleOf(it)) {
    const QVariant role = a->property("role");
    if (role.isValid() && role.toInt() != 0) o.insert(QStringLiteral("role"), roleName(role.toInt()));
    if (!l.accName.isEmpty()) o.insert(QStringLiteral("accessibleName"), clip(l.accName));
  }
  if (destructiveOf(it, true).hit) o.insert(QStringLiteral("destructive"), true);
  const QRectF r = it->mapRectToScene(QRectF(0, 0, it->width(), it->height()));
  o.insert(QStringLiteral("rect"), QJsonArray{qRound(r.x()), qRound(r.y()), qRound(r.width()), qRound(r.height())});
  return o;
}

struct TreeBudget {
  qsizetype used = 0;
  bool truncated = false;
  bool depthCut = false;
  int visited = 0;
};

void buildTree(QQuickItem* it, const QString& path, int depth, int maxDepth, bool visibleOnly, TreeBudget& b, QJsonArray& out) {
  if (b.truncated) return;
  if (++b.visited > kMaxVisited) {
    b.truncated = true;
    return;
  }
  if (visibleOnly && !it->isVisible()) return;
  const QList<QQuickItem*> kids = it->childItems();
  const Labels l = labelsOf(it);
  if (!interesting(it, l)) {  // structural items without identity are left out; their children move up one level
    for (int i = 0; i < kids.size() && !b.truncated; ++i) buildTree(kids.at(i), path + QLatin1Char('/') + QString::number(i), depth, maxDepth, visibleOnly, b, out);
    return;
  }
  QJsonObject o = describe(it, path);
  const qsizetype size = QJsonDocument(o).toJson(QJsonDocument::Compact).size() + 2;
  if (b.used + size > kMaxTreeBytes) {
    b.truncated = true;
    return;
  }
  b.used += size;
  if (depth >= maxDepth) {
    if (!kids.isEmpty()) {
      o.insert(QStringLiteral("more"), true);  // children not shown (depth limit)
      b.depthCut = true;
    }
    out.append(o);
    return;
  }
  QJsonArray children;
  for (int i = 0; i < kids.size() && !b.truncated; ++i) buildTree(kids.at(i), path + QLatin1Char('/') + QString::number(i), depth + 1, maxDepth, visibleOnly, b, children);
  if (!children.isEmpty()) o.insert(QStringLiteral("children"), children);
  out.append(o);
}

// ------------------------------------------------------------------------------------------------ finding targets

template <class F>
void walkItems(QQuickItem* root, F&& f, int* visited) {
  if (root == nullptr || ++*visited > kMaxVisited) return;
  f(root);
  for (QQuickItem* c : root->childItems()) walkItems(c, f, visited);
}

struct Found {
  QQuickItem* item = nullptr;
  int matches = 0;
  QString error;
};

// A path id, an objectName or a visible text (text, label, title, placeholder or accessible name; exact before partial).
Found findTarget(const QString& target, QQuickWindow* only) {
  Found f;
  if (target.isEmpty()) {
    f.error = QStringLiteral("empty_target");
    return f;
  }
  if (QQuickItem* byPath = resolvePath(target)) {
    f.item = byPath;
    f.matches = 1;
    return f;
  }
  const QList<QQuickWindow*> ws = only != nullptr ? QList<QQuickWindow*>{only} : appWindows();
  struct Cand {
    QQuickItem* item;
    int score;
  };
  QList<Cand> cands;
  int visited = 0;
  const auto rank = [](QQuickItem* it, bool exact, bool byName) {
    int s = exact ? 0 : 10;
    if (!it->isVisible()) s += 100;
    if (!it->isEnabled()) s += 50;
    if (!isClickable(it)) s += byName ? 0 : 5;
    return s;
  };
  for (QQuickWindow* w : ws)
    walkItems(w->contentItem(), [&](QQuickItem* it) {
      if (it->objectName() == target) cands.append({it, rank(it, true, true)});
    }, &visited);
  if (cands.isEmpty()) {
    for (QQuickWindow* w : ws) {
      walkItems(w->contentItem(), [&](QQuickItem* it) {
        const Labels l = labelsOf(it);
        if (l.secret) return;
        bool exact = false, partial = false;
        for (const QString& t : {l.text, l.label, l.title, l.placeholder, l.accName}) {
          if (t.isEmpty()) continue;
          if (t.simplified().compare(target, Qt::CaseInsensitive) == 0) exact = true;
          else if (t.contains(target, Qt::CaseInsensitive)) partial = true;
        }
        if (exact || partial) cands.append({it, rank(it, exact, false)});
      }, &visited);
    }
  }
  if (cands.isEmpty()) {
    f.error = QStringLiteral("target_not_found: %1").arg(target);
    return f;
  }
  const auto best = std::min_element(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.score < b.score; });
  f.item = best->item;
  f.matches = static_cast<int>(cands.size());
  return f;
}

// Visits the items under a window point from the top of the paint order down (children before their parent).
bool hitWalk(QQuickItem* it, const QPointF& scene, const std::function<bool(QQuickItem*)>& visitor, int* visited) {
  if (it == nullptr || !it->isVisible() || ++*visited > kMaxVisited) return false;
  const QPointF local = it->mapFromScene(scene);
  if (it->clip() && !it->contains(local)) return false;
  QList<QQuickItem*> kids = it->childItems();
  std::stable_sort(kids.begin(), kids.end(), [](QQuickItem* a, QQuickItem* b) { return a->z() < b->z(); });
  for (auto k = kids.crbegin(); k != kids.crend(); ++k)
    if (hitWalk(*k, scene, visitor, visited)) return true;
  return it->contains(local) && visitor(it);
}

// The topmost item at a point and the item that would receive a mouse click there (first enabled item that takes mouse buttons).
struct Hit {
  QQuickItem* top = nullptr;
  QQuickItem* receiver = nullptr;
};

Hit hitAt(QQuickWindow* w, const QPointF& scene) {
  Hit h;
  int visited = 0;
  hitWalk(w->contentItem(), scene, [&](QQuickItem* it) {
    if (h.top == nullptr) h.top = it;
    if (it->isEnabled() && it->acceptedMouseButtons() != Qt::NoButton) {
      h.receiver = it;
      return true;
    }
    return false;
  }, &visited);
  return h;
}

// Scrolls every Flickable ancestor so that the item is inside its viewport, as a person would before clicking.
void scrollIntoView(QQuickItem* it) {
  for (QQuickItem* a = it->parentItem(); a != nullptr; a = a->parentItem()) {
    const QVariant ch = a->property("contentHeight"), cy = a->property("contentY");
    if (!ch.isValid() || !cy.isValid()) continue;
    QQuickItem* content = a->property("contentItem").value<QQuickItem*>();
    if (content == nullptr) continue;
    const QPointF inContent = it->mapToItem(content, QPointF(0, 0));
    // Only the part of the view inside the window counts: a view taller than the window would otherwise leave the
    // target below the window edge.
    QRectF view = a->mapRectToScene(QRectF(0, 0, a->width(), a->height()));
    const qreal viewTop0 = view.top();
    if (QQuickWindow* w = a->window()) view = view.intersected(QRectF(0, 0, w->width(), w->height()));
    if (view.height() <= 0) continue;
    const qreal top = view.top() - viewTop0;
    const qreal viewH = view.height();
    qreal y = cy.toReal();
    if (inContent.y() < y + top) y = inContent.y() - top - 8;
    else if (inContent.y() + it->height() > y + top + viewH) y = inContent.y() + it->height() - top - viewH + 8;
    const qreal maxY = (std::max)(qreal(0), ch.toReal() - a->height());
    a->setProperty("contentY", std::clamp<qreal>(y, 0, maxY));
  }
}

// The point to click on an item: the middle of the part that is inside the window and every clipping ancestor.
bool clickPoint(QQuickItem* it, QPointF* out) {
  QRectF r = it->mapRectToScene(QRectF(0, 0, it->width(), it->height()));
  for (QQuickItem* a = it->parentItem(); a != nullptr; a = a->parentItem())
    if (a->clip()) r = r.intersected(a->mapRectToScene(QRectF(0, 0, a->width(), a->height())));
  if (QQuickWindow* w = it->window()) r = r.intersected(QRectF(0, 0, w->width(), w->height()));
  if (r.width() <= 0 || r.height() <= 0) return false;
  *out = r.center();
  return true;
}

// ------------------------------------------------------------------------------------------------ synthesized input

void sendMouse(QQuickWindow* w, QEvent::Type type, const QPointF& pos, Qt::MouseButton button, Qt::MouseButtons buttons) {
  QMouseEvent ev(type, pos, pos, w->mapToGlobal(pos), button, buttons, Qt::NoModifier);
  QCoreApplication::sendEvent(w, &ev);
}

Qt::MouseButton buttonFrom(const QString& s, bool* ok) {
  *ok = true;
  if (s.isEmpty() || s == QLatin1String("left")) return Qt::LeftButton;
  if (s == QLatin1String("right")) return Qt::RightButton;
  if (s == QLatin1String("middle")) return Qt::MiddleButton;
  *ok = false;
  return Qt::LeftButton;
}

void pressKey(QQuickWindow* w, int key, Qt::KeyboardModifiers mods, const QString& text) {
  w->requestActivate();
  // A shortcut (Shortcut items, standard keys) takes the press, exactly like a key from the keyboard.
  if (qt_sendShortcutOverrideEvent(w, 0, key, mods, text, false, 1)) return;
  QKeyEvent press(QEvent::KeyPress, key, mods, text);
  QCoreApplication::sendEvent(w, &press);
  QKeyEvent release(QEvent::KeyRelease, key, mods, text);
  QCoreApplication::sendEvent(w, &release);
}

QString textForKey(int key, Qt::KeyboardModifiers mods) {
  if (mods & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) return {};
  switch (key) {
    case Qt::Key_Space: return QStringLiteral(" ");
    case Qt::Key_Return:
    case Qt::Key_Enter: return QStringLiteral("\r");
    case Qt::Key_Tab: return QStringLiteral("\t");
    case Qt::Key_Escape: return QStringLiteral("\x1b");
    case Qt::Key_Backspace: return QStringLiteral("\b");
    default: break;
  }
  if (key >= Qt::Key_A && key <= Qt::Key_Z) {
    const QChar c(static_cast<char16_t>(key));
    return QString((mods & Qt::ShiftModifier) ? c : c.toLower());
  }
  if (key >= Qt::Key_0 && key <= Qt::Key_9 && !(mods & Qt::ShiftModifier)) return QString(QChar(static_cast<char16_t>(key)));
  return {};
}

bool parseKey(const QString& spec, int* key, Qt::KeyboardModifiers* mods) {
  const QString s = spec.trimmed();
  if (s.isEmpty()) return false;
  const qsizetype plus = s.size() >= 2 ? s.lastIndexOf(QLatin1Char('+'), s.size() - 2) : -1;  // a trailing '+' is the key itself
  const QString head = plus >= 0 ? s.left(plus + 1) : QString();
  QString last = plus >= 0 ? s.mid(plus + 1) : s;
  static const QHash<QString, QString> alias{{QStringLiteral("escape"), QStringLiteral("Esc")},   {QStringLiteral("delete"), QStringLiteral("Del")},
                                             {QStringLiteral("pageup"), QStringLiteral("PgUp")}, {QStringLiteral("pagedown"), QStringLiteral("PgDown")},
                                             {QStringLiteral("insert"), QStringLiteral("Ins")}};
  if (const auto it = alias.constFind(last.toLower()); it != alias.constEnd()) last = it.value();
  QString full = head + last;
  full.replace(QLatin1String("Control+"), QLatin1String("Ctrl+"), Qt::CaseInsensitive);
  const QKeySequence seq = QKeySequence::fromString(full, QKeySequence::PortableText);
  if (seq.isEmpty()) return false;
  const QKeyCombination kc = seq[0];
  if (kc.key() == Qt::Key_unknown) return false;
  *key = kc.key();
  *mods = kc.keyboardModifiers();
  return true;
}

bool isActivationKey(int key, Qt::KeyboardModifiers mods) {
  if (mods & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) return false;
  return key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Space;
}

void settle(std::function<void()> then) { QTimer::singleShot(kSettleMs, qApp, std::move(then)); }

// Calls `done(ready)` as soon as `pred()` holds or after `ms`, polling on the GUI thread.
void pollUntil(std::function<bool()> pred, int ms, std::function<void(bool)> done) {
  auto elapsed = std::make_shared<int>(0);
  auto tick = std::make_shared<std::function<void()>>();
  *tick = [pred, ms, done, elapsed, tick]() {
    if (pred()) return done(true);
    if (*elapsed >= ms) return done(false);
    *elapsed += 40;
    QTimer::singleShot(40, qApp, *tick);
  };
  (*tick)();
}

QJsonObject brief(QQuickItem* it) { return describe(it, pathOf(it)); }

// ------------------------------------------------------------------------------------------------ navigation

QJsonObject navState(ui::PlayerController& pc) {
  return QJsonObject{{QStringLiteral("screen"), pc.screen()},
                     {QStringLiteral("serverPage"), pc.serverPage()},
                     {QStringLiteral("selectedGameId"), pc.selectedGameId()},
                     {QStringLiteral("runningGameId"), pc.runningGameId()},
                     {QStringLiteral("startConfirm"), QJsonObject::fromVariantMap(pc.startConfirm())}};
}

// Selects a library game; an unknown id leaves the selection as it was.
bool selectKnownGame(ui::PlayerController& pc, const QString& gameId) {
  const QString before = pc.selectedGameId();
  pc.selectGame(gameId);
  if (pc.selectedGame().value(QStringLiteral("id")).toString() == gameId) return true;
  pc.selectGame(before);
  return false;
}

QQuickWindow* optionalWindow(const QJsonObject& a, QString* err) {
  const QString spec = a.value(QStringLiteral("window")).toString();
  return spec.isEmpty() ? nullptr : pickWindow(spec, err);
}

}  // namespace

void installUiTools(McpToolRegistry& registry, ui::PlayerController& player) {
  QPointer<ui::PlayerController> pc(&player);

  // ----- ui_tree
  {
    ToolDef t;
    t.name = QStringLiteral("ui_tree");
    t.description = QStringLiteral(
        "The QML item tree of the visible app windows, popups and dialogs included. Per item: type, objectName, text/label/placeholder "
        "(never the content of password fields: \"***\"), checked/enabled/visible/focus, accessible role and name, rect [x,y,w,h] in window "
        "coordinates, a path id usable as target, \"destructive\" for controls that only the person may operate. Items without a name, text "
        "or role are folded into their parent. Output is capped at about 200 KB (\"truncated\": true).");
    t.inputSchema = schema({{QStringLiteral("root"), typeStr(QStringLiteral("objectName, path id or text of the item to start at (default: whole windows)."))},
                            {QStringLiteral("depth"), typeInt(QStringLiteral("Levels of named items to show, default 12."))},
                            {QStringLiteral("visibleOnly"), typeBool(QStringLiteral("Skip invisible items, default true."))},
                            {QStringLiteral("window"), typeStr(QStringLiteral("Only this window: index, objectName or title part."))}});
    t.targetArg = QStringLiteral("root");
    t.handler = [](const QJsonObject& a, ToolReply reply) {
      const int depth = std::clamp(a.contains(QStringLiteral("depth")) ? a.value(QStringLiteral("depth")).toInt(kDefaultDepth) : kDefaultDepth, 0, 64);
      const bool visibleOnly = a.value(QStringLiteral("visibleOnly")).toBool(true);
      TreeBudget budget;
      const QString rootSpec = a.value(QStringLiteral("root")).toString();
      QString err;
      QQuickWindow* only = optionalWindow(a, &err);
      if (!err.isEmpty()) return reply(ToolResult::error(err));
      const QString hint = QStringLiteral("items with \"more\": true have children below the depth limit");
      if (!rootSpec.isEmpty()) {
        const Found f = findTarget(rootSpec, only);
        if (f.item == nullptr) return reply(ToolResult::error(f.error));
        QJsonArray tree;
        buildTree(f.item, pathOf(f.item), 0, depth, visibleOnly, budget, tree);
        QJsonObject out{{QStringLiteral("root"), brief(f.item)}, {QStringLiteral("tree"), tree}, {QStringLiteral("truncated"), budget.truncated}};
        if (budget.depthCut) out.insert(QStringLiteral("depthLimit"), hint);
        return reply(ToolResult::json(out));
      }
      QJsonArray windows;
      const QList<QQuickWindow*> all = appWindows();
      for (int i = 0; i < all.size(); ++i) {
        QQuickWindow* w = all.at(i);
        if (only != nullptr && w != only) continue;
        QJsonArray tree;
        buildTree(w->contentItem(), QString::number(i), 0, depth, visibleOnly, budget, tree);
        windows.append(QJsonObject{{QStringLiteral("index"), i},
                                   {QStringLiteral("title"), w->title()},
                                   {QStringLiteral("objectName"), w->objectName()},
                                   {QStringLiteral("size"), QJsonArray{w->width(), w->height()}},
                                   {QStringLiteral("active"), w->isActive()},
                                   {QStringLiteral("tree"), tree}});
      }
      QJsonObject out{{QStringLiteral("windows"), windows}, {QStringLiteral("truncated"), budget.truncated}};
      if (budget.truncated) out.insert(QStringLiteral("note"), QStringLiteral("output capped at 200 KB: use root, depth or window to look at a part"));
      if (budget.depthCut) out.insert(QStringLiteral("depthLimit"), hint);
      reply(ToolResult::json(out));
    };
    registry.add(std::move(t));
  }

  // ----- ui_click
  {
    ToolDef t;
    t.name = QStringLiteral("ui_click");
    t.description = QStringLiteral(
        "Clicks with real mouse events, like a person: move, press, release (a double click with double: true). target is an objectName, a "
        "path id from ui_tree or a visible text; or x,y in window coordinates (with a target, x,y is the offset inside it). The target is "
        "scrolled into view first. Destructive controls (delete, remove, overwrite, share with others, installs and updates) are refused "
        "with isError \"needs_confirmation_in_app\".");
    t.inputSchema = schema({{QStringLiteral("target"), typeStr(QStringLiteral("objectName, path id or text."))},
                            {QStringLiteral("x"), typeAny(QStringLiteral("Window x (no target) or x inside the target."))},
                            {QStringLiteral("y"), typeAny(QStringLiteral("Window y (no target) or y inside the target."))},
                            {QStringLiteral("button"), typeStr(QStringLiteral("left (default) | right | middle."))},
                            {QStringLiteral("double"), typeBool(QStringLiteral("Double click."))},
                            {QStringLiteral("window"), typeStr(QStringLiteral("Window for x,y: index, objectName or title part."))}});
    t.targetArg = QStringLiteral("target");
    t.handler = [pc](const QJsonObject& a, ToolReply reply) {
      bool okButton = true;
      const Qt::MouseButton button = buttonFrom(a.value(QStringLiteral("button")).toString(), &okButton);
      if (!okButton) return reply(ToolResult::error(QStringLiteral("button must be left, right or middle")));
      const QString target = a.value(QStringLiteral("target")).toString();
      const bool hasXY = a.value(QStringLiteral("x")).isDouble() && a.value(QStringLiteral("y")).isDouble();
      if (target.isEmpty() && !hasXY) return reply(ToolResult::error(QStringLiteral("target or x,y required")));
      QPointer<QQuickItem> item;
      int matches = 0;
      QString err;
      QQuickWindow* only = optionalWindow(a, &err);
      if (!err.isEmpty()) return reply(ToolResult::error(err));
      QQuickWindow* w = only;
      if (!target.isEmpty()) {
        const Found f = findTarget(target, only);
        if (f.item == nullptr) return reply(ToolResult::error(f.error));
        item = f.item;
        matches = f.matches;
        if (!item->isVisible()) return reply(ToolResult::error(QStringLiteral("target_not_visible")));
        if (!item->isEnabled()) return reply(ToolResult::error(QStringLiteral("target_disabled")));
        w = item->window();
        scrollIntoView(item);
        QCoreApplication::processEvents();  // the Flickable moved: positions are final after this
      } else if (w == nullptr) {
        w = pickWindow(QString(), &err);
      }
      if (w == nullptr) return reply(ToolResult::error(err.isEmpty() ? QStringLiteral("no_window") : err));
      QPointF pos;
      if (item != nullptr) {
        if (hasXY) pos = item->mapToScene(QPointF(a.value(QStringLiteral("x")).toDouble(), a.value(QStringLiteral("y")).toDouble()));
        else if (!clickPoint(item, &pos)) return reply(ToolResult::error(QStringLiteral("target_outside_window")));
      } else {
        pos = QPointF(a.value(QStringLiteral("x")).toDouble(), a.value(QStringLiteral("y")).toDouble());
      }
      if (pos.x() < 0 || pos.y() < 0 || pos.x() >= w->width() || pos.y() >= w->height())
        return reply(ToolResult::error(QStringLiteral("point_outside_window: %1,%2 in %3x%4").arg(pos.x()).arg(pos.y()).arg(w->width()).arg(w->height())));
      // Brake: the target, the topmost item and the item that takes the click, each with its ancestors.
      const Hit hit = hitAt(w, pos);
      for (QQuickItem* c : {item.data(), hit.top, hit.receiver}) {
        if (c == nullptr) continue;
        const Destructive d = destructiveOf(c);
        if (d.hit) return reply(refuse(QStringLiteral("ui_click"), d));
      }
      const bool dbl = a.value(QStringLiteral("double")).toBool(false);
      w->requestActivate();
      sendMouse(w, QEvent::MouseMove, pos, Qt::NoButton, Qt::NoButton);
      sendMouse(w, QEvent::MouseButtonPress, pos, button, button);
      sendMouse(w, QEvent::MouseButtonRelease, pos, button, Qt::NoButton);
      if (dbl) {
        sendMouse(w, QEvent::MouseButtonDblClick, pos, button, button);
        sendMouse(w, QEvent::MouseButtonRelease, pos, button, Qt::NoButton);
      }
      const bool hadTarget = !target.isEmpty();
      QPointer<QQuickItem> recv(hit.receiver);
      settle([reply, item, recv, pos, matches, hadTarget, pc]() {
        QJsonObject o{{QStringLiteral("clicked"), QJsonArray{qRound(pos.x()), qRound(pos.y())}}};
        if (recv != nullptr) o.insert(QStringLiteral("hit"), QJsonObject{{QStringLiteral("type"), typeName(recv)}, {QStringLiteral("objectName"), recv->objectName()}});
        if (matches > 1) o.insert(QStringLiteral("matches"), matches);
        if (item != nullptr) o.insert(QStringLiteral("target"), brief(item));
        else if (hadTarget) o.insert(QStringLiteral("targetGone"), true);
        if (pc) o.insert(QStringLiteral("screen"), pc->screen());
        reply(ToolResult::json(o));
      });
    };
    registry.add(std::move(t));
  }

  // ----- ui_type
  {
    ToolDef t;
    t.name = QStringLiteral("ui_type");
    t.description = QStringLiteral(
        "Types text into the focused item or, with target, into that item (it gets the focus first) as real key events. clear: true replaces "
        "the content; submit: true presses Return afterwards. Typing into password fields is allowed; their content is never returned.");
    t.inputSchema = schema({{QStringLiteral("target"), typeStr(QStringLiteral("objectName, path id or text of the field; default: the focused item."))},
                            {QStringLiteral("text"), typeStr(QStringLiteral("The text; a newline presses Return."))},
                            {QStringLiteral("clear"), typeBool(QStringLiteral("Select the old content first so it is replaced."))},
                            {QStringLiteral("submit"), typeBool(QStringLiteral("Press Return after the text."))},
                            {QStringLiteral("window"), typeStr(QStringLiteral("Window: index, objectName or title part."))}},
                           {QStringLiteral("text")});
    t.targetArg = QStringLiteral("target");
    t.handler = [pc](const QJsonObject& a, ToolReply reply) {
      if (!a.value(QStringLiteral("text")).isString()) return reply(ToolResult::error(QStringLiteral("text required")));
      const QString text = a.value(QStringLiteral("text")).toString();
      QString err;
      QQuickWindow* only = optionalWindow(a, &err);
      if (!err.isEmpty()) return reply(ToolResult::error(err));
      QPointer<QQuickItem> item;
      const QString target = a.value(QStringLiteral("target")).toString();
      QQuickWindow* w = nullptr;
      if (!target.isEmpty()) {
        const Found f = findTarget(target, only);
        if (f.item == nullptr) return reply(ToolResult::error(f.error));
        item = f.item;
        if (!item->isVisible() || !item->isEnabled()) return reply(ToolResult::error(QStringLiteral("target_not_usable")));
        w = item->window();
      } else {
        w = only != nullptr ? only : pickWindow(QString(), &err);
        if (w == nullptr) return reply(ToolResult::error(err.isEmpty() ? QStringLiteral("no_window") : err));
        item = w->activeFocusItem();
        if (item == nullptr) return reply(ToolResult::error(QStringLiteral("no_focus: pass a target")));
      }
      if (w == nullptr) return reply(ToolResult::error(QStringLiteral("no_window")));
      const bool submit = a.value(QStringLiteral("submit")).toBool(false);
      if (submit || text.contains(QLatin1Char('\n')) || text.contains(QLatin1Char('\r'))) {  // Return on a destructive control
        const Destructive d = destructiveOf(item);
        if (d.hit) return reply(refuse(QStringLiteral("ui_type"), d));
      }
      w->requestActivate();
      if (!target.isEmpty()) {
        scrollIntoView(item);
        item->forceActiveFocus(Qt::OtherFocusReason);
      }
      if (a.value(QStringLiteral("clear")).toBool(false)) {
        if (!QMetaObject::invokeMethod(item, "selectAll")) pressKey(w, Qt::Key_A, Qt::ControlModifier, QString());
        if (labelsOf(item).text.isEmpty()) pressKey(w, Qt::Key_Backspace, Qt::NoModifier, QStringLiteral("\b"));
      }
      for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (c == QLatin1Char('\n') || c == QLatin1Char('\r')) {
          pressKey(w, Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"));
          continue;
        }
        if (c == QLatin1Char('\t')) {
          pressKey(w, Qt::Key_Tab, Qt::NoModifier, QStringLiteral("\t"));
          continue;
        }
        QString s(c);
        if (c.isHighSurrogate() && i + 1 < text.size()) s.append(text.at(++i));
        int key = Qt::Key_unknown;
        const QChar u = c.toUpper();
        if ((u >= QLatin1Char('A') && u <= QLatin1Char('Z')) || (c >= QLatin1Char('0') && c <= QLatin1Char('9')) || c == QLatin1Char(' ')) key = u.unicode();
        // Plain characters go straight to the focus item (no shortcut lookup), so typing "q" never fires a Shortcut.
        const Qt::KeyboardModifiers mods = c.isUpper() ? Qt::ShiftModifier : Qt::NoModifier;
        QKeyEvent press(QEvent::KeyPress, key, mods, s);
        QCoreApplication::sendEvent(w, &press);
        QKeyEvent release(QEvent::KeyRelease, key, mods, s);
        QCoreApplication::sendEvent(w, &release);
      }
      if (submit) pressKey(w, Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"));
      settle([reply, item, text, pc]() {
        QJsonObject o{{QStringLiteral("typed"), static_cast<int>(text.size())}};
        if (item != nullptr) {
          const Labels l = labelsOf(item);
          o.insert(QStringLiteral("secret"), l.secret);
          if (!l.secret) o.insert(QStringLiteral("value"), clip(l.text));
          o.insert(QStringLiteral("target"), brief(item));
        }
        if (pc) o.insert(QStringLiteral("screen"), pc->screen());
        reply(ToolResult::json(o));
      });
    };
    registry.add(std::move(t));
  }

  // ----- ui_key
  {
    ToolDef t;
    t.name = QStringLiteral("ui_key");
    t.description = QStringLiteral(
        "Presses one key or key combination as a real key event, for example Return, Escape, Space, Tab, Left, \"Ctrl+Shift+R\", \"Ctrl+,\". "
        "Shortcuts of the app fire as from the keyboard. With target, that item gets the focus first. Return, Enter and Space on a destructive "
        "control are refused with isError \"needs_confirmation_in_app\".");
    t.inputSchema = schema({{QStringLiteral("key"), typeStr(QStringLiteral("Key name with optional Ctrl+, Shift+, Alt+, Meta+."))},
                            {QStringLiteral("target"), typeStr(QStringLiteral("objectName, path id or text of the item to focus first."))},
                            {QStringLiteral("window"), typeStr(QStringLiteral("Window: index, objectName or title part."))}},
                           {QStringLiteral("key")});
    t.targetArg = QStringLiteral("key");
    t.handler = [pc](const QJsonObject& a, ToolReply reply) {
      int key = 0;
      Qt::KeyboardModifiers mods;
      if (!parseKey(a.value(QStringLiteral("key")).toString(), &key, &mods))
        return reply(ToolResult::error(QStringLiteral("unknown_key: %1").arg(a.value(QStringLiteral("key")).toString())));
      QString err;
      QQuickWindow* only = optionalWindow(a, &err);
      if (!err.isEmpty()) return reply(ToolResult::error(err));
      QPointer<QQuickItem> item;
      QQuickWindow* w = only;
      const QString target = a.value(QStringLiteral("target")).toString();
      if (!target.isEmpty()) {
        const Found f = findTarget(target, only);
        if (f.item == nullptr) return reply(ToolResult::error(f.error));
        item = f.item;
        w = item->window();
      } else {
        if (w == nullptr) w = pickWindow(QString(), &err);
        if (w == nullptr) return reply(ToolResult::error(err));
        item = w->activeFocusItem();
      }
      if (w == nullptr) return reply(ToolResult::error(QStringLiteral("no_window")));
      if (isActivationKey(key, mods) && item != nullptr) {
        const Destructive d = destructiveOf(item);
        if (d.hit) return reply(refuse(QStringLiteral("ui_key"), d));
      }
      if (!target.isEmpty()) {
        scrollIntoView(item);
        item->forceActiveFocus(Qt::OtherFocusReason);
      }
      pressKey(w, key, mods, textForKey(key, mods));
      QPointer<QQuickWindow> wp(w);
      settle([reply, wp, pc]() {
        QJsonObject o{{QStringLiteral("sent"), true}};
        if (wp != nullptr && wp->activeFocusItem() != nullptr) o.insert(QStringLiteral("focus"), brief(wp->activeFocusItem()));
        if (pc) o.insert(QStringLiteral("screen"), pc->screen());
        reply(ToolResult::json(o));
      });
    };
    registry.add(std::move(t));
  }

  // ----- ui_scroll
  {
    ToolDef t;
    t.name = QStringLiteral("ui_scroll");
    t.description = QStringLiteral(
        "Scrolls with a real mouse wheel event over the target (default: the middle of the window). dy > 0 scrolls down, dy < 0 up, dx > 0 "
        "right; the unit is one wheel notch.");
    t.inputSchema = schema({{QStringLiteral("target"), typeStr(QStringLiteral("objectName, path id or text to scroll over."))},
                            {QStringLiteral("dx"), typeAny(QStringLiteral("Notches to the right, default 0."))},
                            {QStringLiteral("dy"), typeAny(QStringLiteral("Notches down, default 0."))},
                            {QStringLiteral("window"), typeStr(QStringLiteral("Window: index, objectName or title part."))}});
    t.targetArg = QStringLiteral("target");
    t.handler = [](const QJsonObject& a, ToolReply reply) {
      const double dx = a.value(QStringLiteral("dx")).toDouble(0), dy = a.value(QStringLiteral("dy")).toDouble(0);
      if (dx == 0 && dy == 0) return reply(ToolResult::error(QStringLiteral("dx or dy required")));
      QString err;
      QQuickWindow* only = optionalWindow(a, &err);
      if (!err.isEmpty()) return reply(ToolResult::error(err));
      QPointer<QQuickItem> item;
      QQuickWindow* w = only;
      QPointF pos;
      const QString target = a.value(QStringLiteral("target")).toString();
      if (!target.isEmpty()) {
        const Found f = findTarget(target, only);
        if (f.item == nullptr) return reply(ToolResult::error(f.error));
        item = f.item;
        w = item->window();
        if (!clickPoint(item, &pos)) return reply(ToolResult::error(QStringLiteral("target_outside_window")));
      } else {
        if (w == nullptr) w = pickWindow(QString(), &err);
        if (w == nullptr) return reply(ToolResult::error(err));
        pos = QPointF(w->width() / 2.0, w->height() / 2.0);
      }
      if (w == nullptr) return reply(ToolResult::error(QStringLiteral("no_window")));
      const QPoint angle(qRound(dx * 120), qRound(-dy * 120));  // a wheel turned away from the person (positive) scrolls up
      w->requestActivate();
      sendMouse(w, QEvent::MouseMove, pos, Qt::NoButton, Qt::NoButton);
      QWheelEvent ev(pos, w->mapToGlobal(pos), QPoint(), angle, Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
      QCoreApplication::sendEvent(w, &ev);
      settle([reply, item, pos]() {
        QJsonObject o{{QStringLiteral("scrolledAt"), QJsonArray{qRound(pos.x()), qRound(pos.y())}}};
        if (item != nullptr) o.insert(QStringLiteral("target"), brief(item));
        reply(ToolResult::json(o));
      });
    };
    registry.add(std::move(t));
  }

  // ----- navigate
  {
    ToolDef t;
    t.name = QStringLiteral("navigate");
    t.description = QStringLiteral(
        "Opens a screen through the controller calls the sidebar uses (no clicks). to: library | game (library with gameId selected) | settings "
        "(page: account|updates|hubs|appearance|audio|library|storage|plugins|diagnostics) | server (page: status|users|systems|plugins|reports) "
        "| emulation | controllers | runs | plugin (page = plugin id) | plugin_settings (page = plugin id) | setup. Returns the screen reached; "
        "screens that need a connected server are refused while none is, and while a game is in front.");
    t.inputSchema = schema({{QStringLiteral("to"), typeStr(QStringLiteral("Target screen, see description."))},
                            {QStringLiteral("page"), typeStr(QStringLiteral("Settings section, server page or plugin id."))},
                            {QStringLiteral("gameId"), typeStr(QStringLiteral("Library game id to select."))}},
                           {QStringLiteral("to")});
    t.targetArg = QStringLiteral("to");
    t.handler = [pc](const QJsonObject& a, ToolReply reply) {
      if (!pc) return reply(ToolResult::error(QStringLiteral("app_closing")));
      const QString to = a.value(QStringLiteral("to")).toString();
      const QString page = a.value(QStringLiteral("page")).toString();
      const QString gameId = a.value(QStringLiteral("gameId")).toString();
      static const QStringList settingsPages{q("account"), q("updates"), q("hubs"),    q("appearance"), q("audio"),
                                             q("library"), q("storage"), q("plugins"), q("diagnostics")};
      static const QStringList serverPages{q("status"), q("users"), q("systems"), q("plugins"), q("reports")};
      if (!gameId.isEmpty()) {
        if (!selectKnownGame(*pc, gameId)) return reply(ToolResult::error(QStringLiteral("unknown_game: %1").arg(gameId)));
      }
      QString expect;  // the screen that must result (empty: not checked)
      if (to == QLatin1String("library") || to == QLatin1String("game")) {
        if (to == QLatin1String("game") && gameId.isEmpty()) return reply(ToolResult::error(QStringLiteral("gameId required for game")));
        pc->showLibrary();
        expect = QStringLiteral("library");
      } else if (to == QLatin1String("settings")) {
        if (!page.isEmpty() && !settingsPages.contains(page))
          return reply(ToolResult::error(QStringLiteral("unknown settings page: %1 (%2)").arg(page, settingsPages.join(QLatin1Char('|')))));
        if (page.isEmpty()) pc->showSettings();
        else pc->showSettingsSection(page);
        expect = QStringLiteral("settings");
      } else if (to == QLatin1String("server")) {
        if (!page.isEmpty() && !serverPages.contains(page))
          return reply(ToolResult::error(QStringLiteral("unknown server page: %1 (%2)").arg(page, serverPages.join(QLatin1Char('|')))));
        pc->showServerPage(page);
        expect = QStringLiteral("server");
      } else if (to == QLatin1String("emulation")) {
        pc->showEmulation();
        expect = QStringLiteral("emulation");
      } else if (to == QLatin1String("controllers")) {
        pc->showControllers();
        expect = QStringLiteral("controllers");
      } else if (to == QLatin1String("runs")) {
        pc->showRuns();
        expect = QStringLiteral("runs");
      } else if (to == QLatin1String("plugin")) {
        if (page.isEmpty()) return reply(ToolResult::error(QStringLiteral("page (plugin id) required")));
        pc->openPluginScreen(page);
      } else if (to == QLatin1String("plugin_settings")) {
        if (page.isEmpty()) return reply(ToolResult::error(QStringLiteral("page (plugin id) required")));
        pc->showPluginSettings(page);
        expect = QStringLiteral("settings");
      } else if (to == QLatin1String("setup")) {
        pc->showSetupAssistant();
      } else {
        return reply(ToolResult::error(QStringLiteral("unknown_target: %1 (library|game|settings|server|emulation|controllers|runs|plugin|plugin_settings|setup)").arg(to)));
      }
      settle([reply, pc, expect]() {
        if (!pc) return reply(ToolResult::error(QStringLiteral("app_closing")));
        QJsonObject o = navState(*pc);
        const bool reached = expect.isEmpty() || pc->screen() == expect;
        o.insert(QStringLiteral("reached"), reached);
        if (!reached) {
          o.insert(QStringLiteral("why"), QStringLiteral("not reachable now: no connected server, a game is in front, or the screen is not available"));
          ToolResult r = ToolResult::error(QStringLiteral("not_reachable"));
          r.addText(QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
          return reply(r);
        }
        reply(ToolResult::json(o));
      });
    };
    registry.add(std::move(t));
  }

  // ----- game_start
  {
    ToolDef t;
    t.name = QStringLiteral("game_start");
    t.description = QStringLiteral(
        "Starts a library game exactly like the Play button (selects it, optionally picks a save slot, then playSelected). If another game "
        "runs in the background, the app asks to quit it: the answer is {status: \"confirm_pending\", startConfirm} and the person decides "
        "in the app. Otherwise {status: \"started\" | \"starting\" | \"resumed\"}.");
    t.inputSchema = schema({{QStringLiteral("gameId"), typeStr(QStringLiteral("Library game id."))},
                            {QStringLiteral("slot"), typeStr(QStringLiteral("Save slot name (an existing slot of the game)."))}},
                           {QStringLiteral("gameId")});
    t.targetArg = QStringLiteral("gameId");
    t.timeoutMs = 20000;
    t.handler = [pc](const QJsonObject& a, ToolReply reply) {
      if (!pc) return reply(ToolResult::error(QStringLiteral("app_closing")));
      const QString gameId = a.value(QStringLiteral("gameId")).toString();
      const QString slot = a.value(QStringLiteral("slot")).toString();
      if (gameId.isEmpty()) return reply(ToolResult::error(QStringLiteral("gameId required")));
      if (!pc->runningGameId().isEmpty() && !pc->gameInBackground()) return reply(ToolResult::error(QStringLiteral("game_running: stop it first (game_stop)")));
      if (!selectKnownGame(*pc, gameId)) return reply(ToolResult::error(QStringLiteral("unknown_game: %1").arg(gameId)));
      if (!slot.isEmpty()) {
        ui::SaveHistoryController* h = pc->saveHistory();
        if (h->slot() != slot) h->selectSlot(slot);
        if (h->slot() != slot) return reply(ToolResult::error(QStringLiteral("slot_unavailable: %1").arg(slot)));
      }
      const QVariantMap sel = pc->selectedGame();
      const bool running = sel.value(QStringLiteral("running")).toBool();
      if (!running && !sel.value(QStringLiteral("canPlay")).toBool()) {
        ToolResult r = ToolResult::error(QStringLiteral("cannot_play"));
        r.addText(sel.value(QStringLiteral("playDisabledReason")).toString());
        return reply(r);
      }
      const bool wasBackground = pc->gameInBackground();
      pc->playSelected();
      const auto state = [pc, gameId]() -> QString {
        if (!pc) return QStringLiteral("closing");
        if (pc->startConfirm().value(QStringLiteral("active")).toBool()) return QStringLiteral("confirm_pending");
        if (pc->runningGameId() == gameId && pc->screen() == QLatin1String("game")) return QStringLiteral("started");
        return QString();
      };
      pollUntil([state]() { return !state().isEmpty(); }, 2500, [reply, pc, state, running, wasBackground](bool) {
        if (!pc) return reply(ToolResult::error(QStringLiteral("app_closing")));
        QString st = state();
        if (st.isEmpty()) st = QStringLiteral("starting");
        else if (st == QLatin1String("started") && running && wasBackground) st = QStringLiteral("resumed");
        QJsonObject o = navState(*pc);
        o.insert(QStringLiteral("status"), st);
        if (st == QLatin1String("confirm_pending")) o.insert(QStringLiteral("hint"), QStringLiteral("The app asks whether to quit the running game; the person answers in the app."));
        reply(ToolResult::json(o));
      });
    };
    registry.add(std::move(t));
  }

  // ----- game_stop
  {
    ToolDef t;
    t.name = QStringLiteral("game_stop");
    t.description = QStringLiteral("Quits the running game exactly like the Quit game button (the save is flushed and synced first). Answers when the game is gone.");
    t.inputSchema = schema();
    t.timeoutMs = 30000;
    t.handler = [pc](const QJsonObject&, ToolReply reply) {
      if (!pc) return reply(ToolResult::error(QStringLiteral("app_closing")));
      if (pc->runningGameId().isEmpty()) return reply(ToolResult::error(QStringLiteral("no_game")));
      pc->requestQuit();
      pollUntil([pc]() { return !pc || (!pc->quitting() && pc->runningGameId().isEmpty()); }, 20000, [reply, pc](bool ok) {
        if (!pc) return reply(ToolResult::error(QStringLiteral("app_closing")));
        if (!ok) return reply(ToolResult::error(QStringLiteral("timeout: the game is still quitting")));
        QJsonObject o = navState(*pc);
        o.insert(QStringLiteral("status"), QStringLiteral("stopped"));
        reply(ToolResult::json(o));
      });
    };
    registry.add(std::move(t));
  }

  // ----- app_screenshot
  {
    ToolDef t;
    t.name = QStringLiteral("app_screenshot");
    t.description = QStringLiteral(
        "A PNG of an app window as it is drawn (QQuickWindow::grabWindow), dialogs and popups included. Password fields show dots; a revealed "
        "password would be visible, so do not reveal one while looking.");
    t.inputSchema = schema({{QStringLiteral("window"), typeStr(QStringLiteral("Window: index, objectName or title part; default the active one."))}});
    t.timeoutMs = 15000;
    t.handler = [](const QJsonObject& a, ToolReply reply) {
      QString err;
      QQuickWindow* w = pickWindow(a.value(QStringLiteral("window")).toString(), &err);
      if (w == nullptr) return reply(ToolResult::error(err));
      QPointer<QQuickWindow> wp(w);
      settle([reply, wp]() {
        if (wp == nullptr) return reply(ToolResult::error(QStringLiteral("window_closed")));
        const QImage img = wp->grabWindow();
        const QByteArray png = pngOf(img);
        if (png.isEmpty()) return reply(ToolResult::error(QStringLiteral("no_image")));
        ToolResult res = ToolResult::json(QJsonObject{{QStringLiteral("width"), img.width()}, {QStringLiteral("height"), img.height()}, {QStringLiteral("window"), windowIndex(wp)}});
        res.addImage(png);
        reply(res);
      });
    };
    registry.add(std::move(t));
  }
}

}  // namespace uzume::mcp
