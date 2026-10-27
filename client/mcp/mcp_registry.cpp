#include "mcp_registry.h"

#include <QDebug>
#include <QJsonDocument>
#include <QMetaObject>
#include <QPointer>
#include <QRegularExpression>
#include <QThread>
#include <QTimer>
#include <memory>

namespace uzume::mcp {

ToolResult ToolResult::text(const QString& text) {
  ToolResult r;
  r.content.append({ToolContent::Kind::Text, text, {}});
  return r;
}

ToolResult ToolResult::json(const QJsonValue& value) {
  QJsonDocument doc;
  if (value.isArray()) doc = QJsonDocument(value.toArray());
  else if (value.isObject()) doc = QJsonDocument(value.toObject());
  else doc = QJsonDocument(QJsonArray{value});
  QString text = QString::fromUtf8(doc.toJson(QJsonDocument::Compact));
  if (!value.isArray() && !value.isObject()) text = text.mid(1, text.size() - 2);  // a scalar: the value itself
  return ToolResult::text(text);
}

ToolResult ToolResult::error(const QString& message) {
  ToolResult r = ToolResult::text(message);
  r.isError = true;
  return r;
}

ToolResult& ToolResult::addImage(const QByteArray& png) {
  content.append({ToolContent::Kind::Image, {}, png});
  return *this;
}

ToolResult& ToolResult::addText(const QString& text) {
  content.append({ToolContent::Kind::Text, text, {}});
  return *this;
}

QJsonObject ToolResult::toJson() const {
  QJsonArray items;
  for (const ToolContent& c : content) {
    if (c.kind == ToolContent::Kind::Image) {
      items.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("image")},
                               {QStringLiteral("data"), QString::fromLatin1(c.png.toBase64())},
                               {QStringLiteral("mimeType"), QStringLiteral("image/png")}});
    } else {
      items.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), c.text}});
    }
  }
  return QJsonObject{{QStringLiteral("content"), items}, {QStringLiteral("isError"), isError}};
}

McpToolRegistry::McpToolRegistry(QObject* parent) : QObject(parent) {}

void McpToolRegistry::add(ToolDef def) {
  for (ToolDef& t : m_tools) {
    if (t.name == def.name) {
      t = std::move(def);
      return;
    }
  }
  m_tools.append(std::move(def));
}

const ToolDef* McpToolRegistry::find(const QString& name) const {
  for (const ToolDef& t : m_tools)
    if (t.name == name) return &t;
  return nullptr;
}

QJsonArray McpToolRegistry::listJson() const {
  QJsonArray out;
  for (const ToolDef& t : m_tools) {
    out.append(QJsonObject{{QStringLiteral("name"), t.name},
                           {QStringLiteral("description"), t.description},
                           {QStringLiteral("inputSchema"), t.inputSchema}});
  }
  return out;
}

QString McpToolRegistry::logTarget(const ToolDef& def, const QJsonObject& args) {
  if (def.targetArg.isEmpty()) return {};
  const QString v = args.value(def.targetArg).toString();
  static const QRegularExpression ok(QStringLiteral("^[A-Za-z0-9._:-]{1,64}$"));
  return ok.match(v).hasMatch() ? v : QString();
}

namespace {
struct CallState {
  bool done = false;
  QTimer* timer = nullptr;
  ToolReply cb;
};
}  // namespace

void McpToolRegistry::call(const QString& name, const QJsonObject& argsIn, ToolReply done) {
  const ToolDef* def = find(name);
  if (def == nullptr || !def->handler) {
    done(ToolResult::error(QStringLiteral("unknown_tool")));
    return;
  }
  // A plugin is named `id` by most tools and `plugin_id` by others: either spelling works for every tool that takes one.
  QJsonObject args = argsIn;
  const QJsonObject props = def->inputSchema.value(QStringLiteral("properties")).toObject();
  const QString idKey = QStringLiteral("id"), pluginKey = QStringLiteral("plugin_id");
  if (props.contains(idKey) && !args.contains(idKey) && args.contains(pluginKey)) args.insert(idKey, args.value(pluginKey));
  if (props.contains(pluginKey) && !args.contains(pluginKey) && args.contains(idKey)) args.insert(pluginKey, args.value(idKey));
  const QString target = logTarget(*def, args);
  qInfo().noquote() << "mcp:" << def->name << target;

  auto state = std::make_shared<CallState>();
  state->cb = std::move(done);
  QPointer<McpToolRegistry> self(this);
  auto finish = [state](ToolResult r) {
    if (state->done) return;
    state->done = true;
    if (state->timer != nullptr) {
      state->timer->stop();
      state->timer->deleteLater();
      state->timer = nullptr;
    }
    ToolReply cb = std::move(state->cb);
    if (cb) cb(std::move(r));
  };
  state->timer = new QTimer(this);
  state->timer->setSingleShot(true);
  connect(state->timer, &QTimer::timeout, this, [finish]() { finish(ToolResult::error(QStringLiteral("timeout"))); });
  state->timer->start(def->timeoutMs > 0 ? def->timeoutMs : kDefaultToolTimeoutMs);

  // The reply may come from any thread: it hops to the GUI thread (this object's thread).
  ToolReply reply = [self, finish](ToolResult r) {
    if (self.isNull()) return;
    if (QThread::currentThread() == self->thread()) {
      finish(std::move(r));
    } else {
      QMetaObject::invokeMethod(self.data(), [finish, r = std::move(r)]() mutable { finish(std::move(r)); }, Qt::QueuedConnection);
    }
  };
  try {
    def->handler(args, reply);
  } catch (const std::exception& e) {
    finish(ToolResult::error(QString::fromUtf8(e.what())));
  } catch (...) {
    finish(ToolResult::error(QStringLiteral("internal error")));
  }
}

}  // namespace uzume::mcp
