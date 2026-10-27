#include "plugin_ui.h"

#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

#include "manifest.h"

namespace uzume::plugins {

namespace {

QString qs(const std::string& s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }

struct Ctx {
  QString err;
  int nodes = 0;
  QSet<QString> ids;
};

QString join(const QString& path, const QString& key) { return path.isEmpty() ? key : path + QLatin1Char('.') + key; }

bool bad(Ctx& c, const QString& path, const QString& msg) {
  c.err = path.isEmpty() ? msg : path + QStringLiteral(": ") + msg;
  return false;
}

using Vfn = std::function<bool(const Value&, const QString&, Ctx&, QVariant&)>;
struct Prop {
  Prop(const char* n, Vfn f, bool r = false) : name(n), fn(std::move(f)), required(r) {}
  const char* name;
  Vfn fn;
  bool required;
};
struct Entry {
  QString key;
  const Value* val;
};

bool isNum(const Value& v) { return v.type == Value::Type::Int || v.type == Value::Type::Float; }
double numOf(const Value& v) { return v.type == Value::Type::Int ? static_cast<double>(v.i) : v.d; }

// Array = table whose keys are exactly 1..n (an empty table is an empty array). Returns items in order.
bool arrayItems(const Value& v, std::vector<const Value*>& out) {
  if (v.type != Value::Type::Table) return false;
  std::vector<std::pair<int64_t, const Value*>> tmp;
  for (size_t i = 0; i < v.keys.size(); ++i) {
    if (v.keys[i].type != Value::Type::Int) return false;
    tmp.push_back({v.keys[i].i, &v.vals[i]});
  }
  std::sort(tmp.begin(), tmp.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  for (size_t i = 0; i < tmp.size(); ++i)
    if (tmp[i].first != static_cast<int64_t>(i) + 1) return false;
  for (const auto& p : tmp) out.push_back(p.second);
  return true;
}

bool objectEntries(const Value& v, const QString& path, Ctx& c, std::vector<Entry>& out) {
  if (v.type != Value::Type::Table) return bad(c, path, QStringLiteral("expected a table"));
  for (size_t i = 0; i < v.keys.size(); ++i) {
    if (v.keys[i].type != Value::Type::String) return bad(c, path, QStringLiteral("expected a table with string keys, got a list"));
    out.push_back({qs(v.keys[i].s), &v.vals[i]});
  }
  std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.key < b.key; });
  return true;
}

// ---- scalar validators

Vfn vStr(int minLen = 0, int maxLen = kUiMaxString) {
  return [=](const Value& v, const QString& p, Ctx& c, QVariant& out) {
    if (v.type != Value::Type::String) return bad(c, p, QStringLiteral("expected a string"));
    const QString s = qs(v.s);
    if (s.size() < minLen) return bad(c, p, minLen == 1 ? QStringLiteral("must not be empty") : QStringLiteral("must be at least %1 characters").arg(minLen));
    if (s.size() > maxLen) return bad(c, p, QStringLiteral("longer than %1 characters").arg(maxLen));
    out = s;
    return true;
  };
}

Vfn vBool() {
  return [](const Value& v, const QString& p, Ctx& c, QVariant& out) {
    if (v.type != Value::Type::Bool) return bad(c, p, QStringLiteral("expected true or false"));
    out = v.b;
    return true;
  };
}

Vfn vNum(double lo, double hi) {
  return [=](const Value& v, const QString& p, Ctx& c, QVariant& out) {
    if (!isNum(v)) return bad(c, p, QStringLiteral("expected a number"));
    const double d = numOf(v);
    if (!std::isfinite(d) || d < lo || d > hi) return bad(c, p, QStringLiteral("must be a number between %1 and %2").arg(lo).arg(hi));
    out = v.type == Value::Type::Int ? QVariant(static_cast<qlonglong>(v.i)) : QVariant(d);
    return true;
  };
}

Vfn vInt(int64_t lo, int64_t hi) {
  return [=](const Value& v, const QString& p, Ctx& c, QVariant& out) {
    int64_t n = 0;
    if (v.type == Value::Type::Int) n = v.i;
    else if (v.type == Value::Type::Float && std::isfinite(v.d) && v.d == std::floor(v.d) && std::fabs(v.d) < 1e15) n = static_cast<int64_t>(v.d);
    else return bad(c, p, QStringLiteral("expected an integer"));
    if (n < lo || n > hi) return bad(c, p, QStringLiteral("must be between %1 and %2").arg(lo).arg(hi));
    out = static_cast<qlonglong>(n);
    return true;
  };
}

QStringList sl(std::initializer_list<const char*> l) {
  QStringList out;
  for (const char* s : l) out << QLatin1String(s);
  return out;
}

Vfn vEnum(const QStringList& values) {
  return [=](const Value& v, const QString& p, Ctx& c, QVariant& out) {
    if (v.type != Value::Type::String) return bad(c, p, QStringLiteral("expected one of: %1").arg(values.join(QStringLiteral(", "))));
    const QString s = qs(v.s);
    if (!values.contains(s)) return bad(c, p, QStringLiteral("invalid value \"%1\" (use %2)").arg(s, values.join(QStringLiteral(", "))));
    out = s;
    return true;
  };
}

const QStringList kTones = {QStringLiteral("neutral"), QStringLiteral("info"), QStringLiteral("ok"), QStringLiteral("warning"), QStringLiteral("danger")};
Vfn vTone() { return vEnum(kTones); }

Vfn vGap() {
  return [](const Value& v, const QString& p, Ctx& c, QVariant& out) {
    if (!isNum(v) || !std::isfinite(numOf(v))) return bad(c, p, QStringLiteral("expected a number"));
    static const int allowed[] = {4, 8, 12, 16, 20};
    const double d = numOf(v);
    int g = allowed[0];
    for (int a : allowed)
      if (d >= a) g = a;
    out = g;
    return true;
  };
}

Vfn vId() {
  return [](const Value& v, const QString& p, Ctx& c, QVariant& out) {
    if (v.type != Value::Type::String || !validUiId(qs(v.s))) return bad(c, p, QStringLiteral("must be 1 to 64 characters of A-Z a-z 0-9 . _ -"));
    out = qs(v.s);
    return true;
  };
}

Vfn vIdOrInt() {
  return [](const Value& v, const QString& p, Ctx& c, QVariant& out) {
    if (v.type == Value::Type::Int && v.i >= 0) {
      out = static_cast<qlonglong>(v.i);
      return true;
    }
    if (v.type == Value::Type::String && validUiId(qs(v.s))) {
      out = qs(v.s);
      return true;
    }
    return bad(c, p, QStringLiteral("must be an id string (A-Z a-z 0-9 . _ -, up to 64) or a non-negative integer"));
  };
}

// `cache:icons/25.png` or `asset:icons/x.png`
Vfn vSrc() {
  return [](const Value& v, const QString& p, Ctx& c, QVariant& out) {
    if (v.type != Value::Type::String) return bad(c, p, QStringLiteral("expected a string like \"cache:icons/25.png\""));
    const QString s = qs(v.s);
    const bool cache = s.startsWith(QLatin1String("cache:"));
    if (!cache && !s.startsWith(QLatin1String("asset:"))) return bad(c, p, QStringLiteral("must start with \"cache:\" or \"asset:\""));
    const QString rel = s.mid(6);
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9._/-]{1,200}$"));
    if (!re.match(rel).hasMatch() || rel.contains(QLatin1String("..")) || rel.startsWith(QLatin1Char('/')) || rel.contains(QLatin1String("//")))
      return bad(c, p, QStringLiteral("invalid image path \"%1\"").arg(rel));
    out = s;
    return true;
  };
}

Vfn vAction() { return vStr(1, 64); }

Vfn vScalar() {
  return [](const Value& v, const QString& p, Ctx& c, QVariant& out) {
    switch (v.type) {
      case Value::Type::String:
        return vStr(0, kUiMaxString)(v, p, c, out);
      case Value::Type::Bool: out = v.b; return true;
      case Value::Type::Int: out = static_cast<qlonglong>(v.i); return true;
      case Value::Type::Float:
        if (!std::isfinite(v.d)) return bad(c, p, QStringLiteral("expected a finite number"));
        out = v.d;
        return true;
      default: return bad(c, p, QStringLiteral("expected a string, number or boolean"));
    }
  };
}

// Any plain data (action payloads): at most 8 KiB.
Vfn vAny() {
  return [](const Value& v, const QString& p, Ctx& c, QVariant& out) {
    out = variantFromValue(v);
    if (estimateBytes(out) > 8 * 1024) return bad(c, p, QStringLiteral("value is larger than 8 KiB"));
    return true;
  };
}

Vfn vArray(Vfn elem, int minN, int maxN) {
  return [=](const Value& v, const QString& p, Ctx& c, QVariant& out) {
    std::vector<const Value*> items;
    if (!arrayItems(v, items)) return bad(c, p, QStringLiteral("expected a list"));
    if (static_cast<int>(items.size()) < minN || static_cast<int>(items.size()) > maxN)
      return bad(c, p, minN == maxN ? QStringLiteral("needs exactly %1 entries").arg(minN) : QStringLiteral("needs %1 to %2 entries").arg(minN).arg(maxN));
    QVariantList list;
    for (size_t i = 0; i < items.size(); ++i) {
      QVariant e;
      if (!elem(*items[i], QStringLiteral("%1[%2]").arg(p).arg(i + 1), c, e)) return false;
      list.append(e);
    }
    out = list;
    return true;
  };
}

bool validateObject(const std::vector<Prop>& props, const std::vector<Entry>& entries, const QString& path, Ctx& c, QVariantMap& out,
                    bool checkRequired) {
  for (const Entry& e : entries) {
    const Prop* pr = nullptr;
    for (const Prop& cand : props)
      if (e.key == QLatin1String(cand.name)) pr = &cand;
    if (!pr) return bad(c, join(path, e.key), QStringLiteral("unknown property"));
    QVariant val;
    if (!pr->fn(*e.val, join(path, e.key), c, val)) return false;
    out.insert(e.key, val);
  }
  if (checkRequired)
    for (const Prop& pr : props)
      if (pr.required && !out.contains(QLatin1String(pr.name))) return bad(c, path, QStringLiteral("missing required property \"%1\"").arg(QLatin1String(pr.name)));
  return true;
}

Vfn vObject(std::vector<Prop> props) {
  return [props = std::move(props)](const Value& v, const QString& p, Ctx& c, QVariant& out) {
    std::vector<Entry> entries;
    if (!objectEntries(v, p, c, entries)) return false;
    QVariantMap m;
    if (!validateObject(props, entries, p, c, m, true)) return false;
    out = m;
    return true;
  };
}

Vfn vOptions(int minN, int maxN) {
  return vArray(vObject({{"value", vScalar(), true}, {"label", vStr(1, 100), true}}), minN, maxN);
}

// ---- block specs

struct TypeSpec {
  TypeSpec(bool c, std::vector<Prop> p) : container(c), props(std::move(p)) {}
  bool container;
  std::vector<Prop> props;
};

const QHash<QString, TypeSpec>& specs() {
  static const QHash<QString, TypeSpec> s = [] {
    QHash<QString, TypeSpec> m;
    const Vfn shortStr = vStr(0, 200);
    // `width` ("dialog") only matters on the root of a `screen` tree: the in-game sheet then keeps its content at dialog width (560 px).
    m.insert(QStringLiteral("column"), TypeSpec{true, {{"gap", vGap()}, {"align", vEnum({QStringLiteral("start"), QStringLiteral("center"), QStringLiteral("end"), QStringLiteral("stretch")})}, {"width", vEnum(sl({"dialog"}))}}});
    m.insert(QStringLiteral("row"), TypeSpec{true, {{"gap", vGap()}, {"align", vEnum({QStringLiteral("start"), QStringLiteral("center"), QStringLiteral("end"), QStringLiteral("stretch")})}}});
    m.insert(QStringLiteral("grid"), TypeSpec{true, {{"gap", vGap()}, {"columns", vInt(1, 6)}}});
    m.insert(QStringLiteral("tabs"), TypeSpec{true, {{"tabs", vArray(vStr(1, 60), 1, 12), true}, {"selected", vInt(1, 12)}, {"action", vAction()}}});
    m.insert(QStringLiteral("scroll"), TypeSpec{true, {{"max_height", vEnum(sl({"s", "m", "l"}))}}});
    m.insert(QStringLiteral("card"), TypeSpec{true, {{"title", shortStr}, {"eyebrow", shortStr}, {"link_label", vStr(0, 60)}, {"action", vAction()}, {"value", vAny()}}});
    m.insert(QStringLiteral("section"), TypeSpec{true, {{"title", shortStr, true}, {"link_label", vStr(0, 60)}, {"action", vAction()}}});
    m.insert(QStringLiteral("text"),
             TypeSpec{false, {{"text", vStr(), true}, {"style", vEnum(sl({"title", "heading", "body", "meta", "eyebrow", "value"}))}, {"tone", vTone()}, {"wrap", vBool()}}});
    m.insert(QStringLiteral("chip"), TypeSpec{false, {{"text", vStr(0, 100), true}, {"tone", vTone()}, {"action", vAction()}, {"selected", vBool()}}});
    m.insert(QStringLiteral("badge"), TypeSpec{false, {{"text", vStr(0, 100), true}, {"tone", vTone()}}});
    m.insert(QStringLiteral("avatar"), TypeSpec{false, {{"initial", vStr(1, 4), true}, {"color", vInt(1, 8)}, {"size", vEnum(sl({"s", "m", "l"}))}}});
    m.insert(QStringLiteral("image"), TypeSpec{false, {{"src", vSrc(), true}, {"size", vEnum(sl({"l", "m", "s"}))}, {"state", vEnum(sl({"alive", "dead", "missed", "empty"}))},
                                               {"owner", vInt(1, 8)}, {"label", shortStr}}});
    m.insert(QStringLiteral("bar"), TypeSpec{false, {{"value", vNum(0, 1), true}, {"tone", vTone()}, {"label", shortStr}}});
    m.insert(QStringLiteral("stat"), TypeSpec{false, {{"label", shortStr, true}, {"value", vScalar(), true}, {"action", vAction()}}});
    m.insert(QStringLiteral("list"),
             TypeSpec{false, {{"rows", vArray(vObject({{"id", vIdOrInt()}, {"title", shortStr, true}, {"sub", shortStr}, {"meta", shortStr}, {"image", vSrc()},
                                               {"tone", vTone()}, {"action", vAction()}, {"value", vAny()}}),
                                      0, 500),
                       true},
                      {"selected", vScalar()}}});
    m.insert(QStringLiteral("table"),
             TypeSpec{false, {{"columns", vArray(vStr(0, 60), 1, 12), true},
                      {"rows", vArray(vObject({{"cells", vArray(vScalar(), 0, 12), true}, {"action", vAction()}, {"value", vAny()}}), 0, 500), true}}});
    m.insert(QStringLiteral("button"),
             TypeSpec{false, {{"label", vStr(1, 100), true},
                      {"variant", vEnum(sl({"primary", "secondary", "quiet", "danger"}))},
                      {"action", vAction(), true},
                      {"disabled", vBool()},
                      {"value", vAny()},
                      {"confirm", vObject({{"title", vStr(1, 100), true}, {"text", vStr(1, 300), true}, {"confirm_label", vStr(1, 40)}, {"danger", vBool()}})}}});
    m.insert(QStringLiteral("toggle"), TypeSpec{false, {{"label", shortStr}, {"accessible", shortStr}, {"checked", vBool()}, {"action", vAction(), true}, {"disabled", vBool()}, {"value", vAny()}}});
    m.insert(QStringLiteral("select"), TypeSpec{false, {{"options", vOptions(1, 100), true}, {"selected", vScalar()}, {"action", vAction(), true}, {"disabled", vBool()}}});
    m.insert(QStringLiteral("segmented"), TypeSpec{false, {{"options", vOptions(2, 3), true}, {"selected", vScalar()}, {"action", vAction(), true}}});
    m.insert(QStringLiteral("textfield"), TypeSpec{false, {{"text", vStr()}, {"placeholder", shortStr}, {"error", shortStr}, {"action", vAction(), true}, {"live", vBool()}, {"disabled", vBool()}}});
    // textarea (0.20): multi-line input; "change" fires (default action name "change") with {id, value} once typing pauses.
    m.insert(QStringLiteral("textarea"),
             TypeSpec{false, {{"value", vStr(0, kUiMaxTextareaBytes)}, {"lines", vInt(1, 40)}, {"mono", vBool()}, {"placeholder", shortStr}, {"error", shortStr},
                              {"action", vAction()}, {"disabled", vBool()}}});  // action defaults to the block id; the value is the text
    m.insert(QStringLiteral("empty"), TypeSpec{false, {{"title", shortStr, true}, {"text", vStr(0, 500)}, {"action_label", vStr(0, 60)}, {"action", vAction()}, {"tone", vTone()}}});
    m.insert(QStringLiteral("location"),
             TypeSpec{true, {{"title", shortStr, true}, {"sub", shortStr}, {"status", vEnum(sl({"open", "encounter", "caught", "missed", "none"}))}}});
    const Vfn coord = vNum(-1e6, 1e6);
    m.insert(QStringLiteral("map"),
             {false,
              {{"image", vSrc()},
               {"image_w", vInt(1, 16384)},
               {"image_h", vInt(1, 16384)},
               {"players", vArray(vObject({{"id", vIdOrInt(), true}, {"initial", vStr(1, 4), true}, {"color", vInt(1, 8), true}, {"x", coord, true},
                                           {"y", coord, true}, {"label", shortStr}}),
                                  0, 16)},
               {"routes", vArray(vObject({{"id", vIdOrInt(), true}, {"x", coord, true}, {"y", coord, true}, {"area", vStr(0, 64)}, {"number", vInt(0, 9999)},
                                          {"done", vBool()},
                                          {"icons", vArray(vObject({{"src", vSrc(), true}, {"owner", vInt(1, 8)}}), 0, 8)}}),
                                 0, 1000)},
               {"filter", vEnum(sl({"all", "me", "open"}))},
               {"me", vScalar()},
               {"selected", vScalar()},
               {"legend", vBool()},
               {"action", vAction()},
               {"popover", vArray(vObject({{"owner", vInt(1, 8), true}, {"name", shortStr, true}, {"src", vSrc()}, {"status", vStr(0, 60), true}}), 0, 16)}}});
    return m;
  }();
  return s;
}

bool node(const Value& v, int depth, const QString& path, Ctx& c, QVariantMap& out) {
  std::vector<Entry> entries;
  if (!objectEntries(v, path, c, entries)) return false;
  const Value* typeV = nullptr;
  const Value* idV = nullptr;
  const Value* childrenV = nullptr;
  std::vector<Entry> propEntries;
  for (const Entry& e : entries) {
    if (e.key == QLatin1String("type")) typeV = e.val;
    else if (e.key == QLatin1String("id")) idV = e.val;
    else if (e.key == QLatin1String("children")) childrenV = e.val;
    else propEntries.push_back(e);
  }
  if (!typeV) return bad(c, path, QStringLiteral("missing \"type\""));
  if (typeV->type != Value::Type::String) return bad(c, join(path, QStringLiteral("type")), QStringLiteral("expected a string"));
  const QString type = qs(typeV->s);
  const auto spec = specs().constFind(type);
  if (spec == specs().constEnd()) return bad(c, join(path, QStringLiteral("type")), QStringLiteral("unknown block \"%1\"").arg(type));
  if (depth > kUiMaxDepth) return bad(c, path, QStringLiteral("nested deeper than %1 levels").arg(kUiMaxDepth));
  if (++c.nodes > kUiMaxNodes) return bad(c, path, QStringLiteral("more than %1 blocks in one slot").arg(kUiMaxNodes));

  QVariantMap m;
  m.insert(QStringLiteral("type"), type);
  if (idV) {
    QVariant id;
    if (!vId()(*idV, join(path, QStringLiteral("id")), c, id)) return false;
    if (c.ids.contains(id.toString())) return bad(c, join(path, QStringLiteral("id")), QStringLiteral("id \"%1\" is used twice in this slot").arg(id.toString()));
    c.ids.insert(id.toString());
    m.insert(QStringLiteral("id"), id);
  }
  if (!validateObject(spec->props, propEntries, path, c, m, true)) return false;
  if (spec->container) {
    QVariantList kids;
    if (childrenV) {
      std::vector<const Value*> items;
      if (!arrayItems(*childrenV, items)) return bad(c, join(path, QStringLiteral("children")), QStringLiteral("expected a list of blocks"));
      for (size_t i = 0; i < items.size(); ++i) {
        QVariantMap child;
        if (!node(*items[i], depth + 1, QStringLiteral("%1children[%2]").arg(path.isEmpty() ? QString() : path + QLatin1Char('.')).arg(i + 1), c, child)) return false;
        kids.append(child);
      }
    }
    m.insert(QStringLiteral("children"), kids);
  } else if (childrenV) {
    return bad(c, join(path, QStringLiteral("children")), QStringLiteral("block \"%1\" takes no children").arg(type));
  }
  out = m;
  return true;
}

bool patchRec(QVariantMap& n, const QString& id, const Value& props, Ctx& c, bool& found) {
  if (n.value(QStringLiteral("id")).toString() == id) {
    found = true;
    const auto spec = specs().constFind(n.value(QStringLiteral("type")).toString());
    std::vector<Entry> entries;
    if (!objectEntries(props, QString(), c, entries)) return false;
    for (const Entry& e : entries)
      if (e.key == QLatin1String("type") || e.key == QLatin1String("id") || e.key == QLatin1String("children"))
        return bad(c, e.key, QStringLiteral("cannot be changed with patch (use set)"));
    QVariantMap changes;
    if (!validateObject(spec->props, entries, QString(), c, changes, false)) return false;
    for (auto it = changes.begin(); it != changes.end(); ++it) n.insert(it.key(), it.value());
    return true;
  }
  QVariantList kids = n.value(QStringLiteral("children")).toList();
  for (int i = 0; i < kids.size(); ++i) {
    QVariantMap k = kids[i].toMap();
    if (!patchRec(k, id, props, c, found)) return false;
    if (found) {
      kids[i] = k;
      n.insert(QStringLiteral("children"), kids);
      return true;
    }
  }
  return true;
}

}  // namespace

// ---------------------------------------------------------------- public conversions

bool convertUiTree(const Value& tree, QVariantMap& out, QString& error) {
  Ctx c;
  QVariantMap m;
  if (tree.type != Value::Type::Table) {
    error = QStringLiteral("a tree must be a table with a \"type\"");
    return false;
  }
  if (!node(tree, 1, QString(), c, m)) {
    error = c.err;
    return false;
  }
  if (estimateBytes(m) > kUiMaxBytes) {
    error = QStringLiteral("tree is larger than %1 KiB").arg(kUiMaxBytes / 1024);
    return false;
  }
  out = m;
  return true;
}

bool patchUiTree(QVariantMap& tree, const QString& id, const Value& props, QString& error) {
  Ctx c;
  bool found = false;
  QVariantMap copy = tree;
  if (!patchRec(copy, id, props, c, found)) {
    error = c.err;
    return false;
  }
  if (!found) {
    error = QStringLiteral("no block with id \"%1\" in this slot").arg(id);
    return false;
  }
  if (estimateBytes(copy) > kUiMaxBytes) {
    error = QStringLiteral("tree is larger than %1 KiB").arg(kUiMaxBytes / 1024);
    return false;
  }
  tree = copy;
  return true;
}

bool convertToast(const Value& v, QVariantMap& out, QString& error) {
  Ctx c;
  QVariant r;
  const Vfn fn = vObject({{"tone", vEnum(sl({"info", "important", "warning"}))},
                          {"title", vStr(0, 80)},
                          {"text", vStr(1, kUiMaxToastText), true},
                          {"actions", vArray(vObject({{"label", vStr(1, 40), true}, {"action", vAction(), true}, {"value", vAny()}}), 0, 2)}});
  if (!fn(v, QString(), c, r)) {
    error = c.err;
    return false;
  }
  QVariantMap m = r.toMap();
  if (!m.contains(QStringLiteral("tone"))) m.insert(QStringLiteral("tone"), QStringLiteral("info"));
  if (!m.contains(QStringLiteral("actions"))) m.insert(QStringLiteral("actions"), QVariantList());
  out = m;
  return true;
}

QVariant variantFromValue(const Value& v) {
  switch (v.type) {
    case Value::Type::Nil: return {};
    case Value::Type::Bool: return v.b;
    case Value::Type::Int: return static_cast<qlonglong>(v.i);
    case Value::Type::Float: return v.d;
    case Value::Type::String: return qs(v.s);
    case Value::Type::Table: {
      std::vector<const Value*> items;
      if (arrayItems(v, items)) {
        QVariantList l;
        for (const Value* i : items) l.append(variantFromValue(*i));
        return l;
      }
      QVariantMap m;
      for (size_t i = 0; i < v.keys.size(); ++i) {
        const Value& k = v.keys[i];
        const QString key = k.type == Value::Type::String ? qs(k.s) : k.type == Value::Type::Int ? QString::number(k.i) : k.type == Value::Type::Bool ? (k.b ? QStringLiteral("true") : QStringLiteral("false")) : QString::number(k.d);
        m.insert(key, variantFromValue(v.vals[i]));
      }
      return m;
    }
  }
  return {};
}

Value valueFromVariant(const QVariant& var, int depth) {
  Value v;
  if (depth > 8 || !var.isValid() || var.isNull()) return v;
  switch (var.typeId()) {
    case QMetaType::Bool: v.type = Value::Type::Bool; v.b = var.toBool(); return v;
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
      v.type = Value::Type::Int;
      v.i = var.toLongLong();
      return v;
    case QMetaType::Double:
    case QMetaType::Float:
      v.type = Value::Type::Float;
      v.d = var.toDouble();
      return v;
    case QMetaType::QString:
      v.type = Value::Type::String;
      v.s = var.toString().toStdString();
      return v;
    case QMetaType::QVariantList: {
      v.type = Value::Type::Table;
      const QVariantList l = var.toList();
      for (int i = 0; i < l.size(); ++i) {
        Value k;
        k.type = Value::Type::Int;
        k.i = i + 1;
        v.keys.push_back(k);
        v.vals.push_back(valueFromVariant(l[i], depth + 1));
      }
      return v;
    }
    case QMetaType::QVariantMap: {
      v.type = Value::Type::Table;
      const QVariantMap m = var.toMap();
      for (auto it = m.begin(); it != m.end(); ++it) {
        Value k;
        k.type = Value::Type::String;
        k.s = it.key().toStdString();
        v.keys.push_back(k);
        v.vals.push_back(valueFromVariant(it.value(), depth + 1));
      }
      return v;
    }
    default: {
      // QML hands over QJSValue-converted variants as the types above; anything else is passed as text.
      v.type = Value::Type::String;
      v.s = var.toString().toStdString();
      return v;
    }
  }
}

size_t estimateBytes(const QVariant& v) {
  switch (v.typeId()) {
    case QMetaType::QString: return 16 + static_cast<size_t>(v.toString().toUtf8().size());
    case QMetaType::QVariantList: {
      size_t n = 16;
      for (const QVariant& e : v.toList()) n += estimateBytes(e);
      return n;
    }
    case QMetaType::QVariantMap: {
      size_t n = 16;
      const QVariantMap m = v.toMap();
      for (auto it = m.begin(); it != m.end(); ++it) n += 16 + static_cast<size_t>(it.key().size()) + estimateBytes(it.value());
      return n;
    }
    default: return 16;
  }
}

// ---------------------------------------------------------------- UiStore

void UiStore::reset(const QStringList& ids) {
  std::lock_guard<std::mutex> l(m_mutex);
  for (auto it = m_state.begin(); it != m_state.end(); ++it) {
    for (auto s = it->trees.begin(); s != it->trees.end(); ++s) dirty(it.key(), s.key());
    dirty(it.key(), QStringLiteral("@meta"));
  }
  m_state.clear();
  m_toasts.clear();
  for (const QString& id : ids) m_state.insert(id, UiPluginState());
}

void UiStore::setTree(const QString& id, const QString& slot, const QVariantMap& tree) {
  std::lock_guard<std::mutex> l(m_mutex);
  m_state[id].trees.insert(slot, tree);
  dirty(id, slot);
}

void UiStore::clearSlot(const QString& id, const QString& slot) {
  std::lock_guard<std::mutex> l(m_mutex);
  auto it = m_state.find(id);
  if (it == m_state.end() || !it->trees.remove(slot)) return;
  dirty(id, slot);
}

void UiStore::setTitle(const QString& id, const QString& slot, const QString& title) {
  std::lock_guard<std::mutex> l(m_mutex);
  UiPluginState& s = m_state[id];
  if (title.isEmpty()) s.titles.remove(slot);
  else s.titles.insert(slot, title);
  dirty(id, QStringLiteral("@meta"));
}

void UiStore::setBadge(const QString& id, int n) {
  std::lock_guard<std::mutex> l(m_mutex);
  UiPluginState& s = m_state[id];
  if (s.badge == n) return;
  s.badge = n;
  dirty(id, QStringLiteral("@meta"));
}

void UiStore::setDot(const QString& id, const QString& dot) {
  std::lock_guard<std::mutex> l(m_mutex);
  UiPluginState& s = m_state[id];
  if (s.dot == dot) return;
  s.dot = dot;
  dirty(id, QStringLiteral("@meta"));
}

void UiStore::markPaused(const QString& id) {
  std::lock_guard<std::mutex> l(m_mutex);
  UiPluginState& s = m_state[id];
  s.paused = true;
  s.badge = 0;
  s.dot.clear();
  dirty(id, QStringLiteral("@meta"));
  for (auto it = s.trees.begin(); it != s.trees.end(); ++it) dirty(id, it.key());
}

void UiStore::clearPlugin(const QString& id) {
  std::lock_guard<std::mutex> l(m_mutex);
  auto it = m_state.find(id);
  if (it == m_state.end()) return;
  for (auto s = it->trees.begin(); s != it->trees.end(); ++s) dirty(id, s.key());
  dirty(id, QStringLiteral("@meta"));
  *it = UiPluginState();
}

int UiStore::addToast(const QString& id, QVariantMap toast) {
  std::lock_guard<std::mutex> l(m_mutex);
  const int tid = m_nextToast++;
  toast.insert(QStringLiteral("toastId"), tid);
  toast.insert(QStringLiteral("pluginId"), id);
  m_toasts.append(toast);
  dirty(id, QStringLiteral("@toast"));
  return tid;
}

void UiStore::addOpenRequest(const QString& id, const QString& slot) {
  std::lock_guard<std::mutex> l(m_mutex);
  if (m_openRequests.size() >= 20) m_openRequests.removeFirst();
  m_openRequests.append({id, slot});
  dirty(id, QStringLiteral("@open"));
}

void UiStore::addCloseRequest(const QString& id, const QString& slot) {
  std::lock_guard<std::mutex> l(m_mutex);
  if (m_closeRequests.size() >= 20) m_closeRequests.removeFirst();
  m_closeRequests.append({id, slot});
  dirty(id, QStringLiteral("@close"));
}

QList<QPair<QString, QString>> UiStore::takeCloseRequests() {
  std::lock_guard<std::mutex> l(m_mutex);
  return std::exchange(m_closeRequests, {});
}

void UiStore::addPickRequest(const PickRequest& r) {
  std::lock_guard<std::mutex> l(m_mutex);
  if (m_pickRequests.size() >= 8) m_pickRequests.removeFirst();
  m_pickRequests.append(r);
  dirty(r.pluginId, QStringLiteral("@pick"));
}

QList<PickRequest> UiStore::takePickRequests() {
  std::lock_guard<std::mutex> l(m_mutex);
  return std::exchange(m_pickRequests, {});
}

QList<QPair<QString, QString>> UiStore::takeOpenRequests() {
  std::lock_guard<std::mutex> l(m_mutex);
  return std::exchange(m_openRequests, {});
}

UiPluginState UiStore::state(const QString& id) const {
  std::lock_guard<std::mutex> l(m_mutex);
  return m_state.value(id);
}

QVariantMap UiStore::tree(const QString& id, const QString& slot) const {
  std::lock_guard<std::mutex> l(m_mutex);
  auto it = m_state.constFind(id);
  return it == m_state.constEnd() ? QVariantMap() : it->trees.value(slot);
}

QList<QPair<QString, QString>> UiStore::takeDirty() {
  std::lock_guard<std::mutex> l(m_mutex);
  QList<QPair<QString, QString>> out(m_dirty.begin(), m_dirty.end());
  m_dirty.clear();
  return out;
}

QList<QVariantMap> UiStore::takeToasts() {
  std::lock_guard<std::mutex> l(m_mutex);
  return std::exchange(m_toasts, {});
}

}  // namespace uzume::plugins
