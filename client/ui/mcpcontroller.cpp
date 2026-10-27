#include "mcpcontroller.h"

#include <QClipboard>
#include <QGuiApplication>

#include "credentialstore.h"
#include "envvar.h"
#include "playersettings.h"
#include "version.h"

namespace uzume::ui {

McpController::McpController(PlayerSettings* settings, std::shared_ptr<CredentialStore> credentials, QObject* parent)
    : QObject(parent),
      settings_(settings),
      credentials_(std::move(credentials)),
      registry_(this),
      server_(&registry_, this),
      port_(settings->mcpPort()) {
  const std::string_view v = playerVersion();
  server_.setServerInfo(QStringLiteral("uzume"), QString::fromUtf8(v.data(), static_cast<qsizetype>(v.size())));
  connect(&server_, &mcp::McpServer::listeningChanged, this, &McpController::changed);
}

McpController::~McpController() { server_.stop(); }

bool McpController::enabled() const { return forced_ || settings_->mcpEnabled(); }

void McpController::setEnabled(bool on) {
  if (on == enabled() && (on == settings_->mcpEnabled())) return;
  forced_ = false;
  settings_->setMcpEnabled(on);
  apply();
}

void McpController::setPort(int port) {
  if (port == port_ && error_.isEmpty()) return;
  if (port < 1024 || port > 65535) {
    error_ = tr("The port must be between 1024 and 65535.");
    emit changed();
    return;
  }
  port_ = port;
  settings_->setMcpPort(port);
  apply();
  emit changed();
}

void McpController::start() { apply(); }

void McpController::enableForThisStart() {
  forced_ = true;
  apply();
}

bool McpController::setPortForThisStart(int port) {
  if (port < 1024 || port > 65535) return false;
  port_ = port;  // not written to the settings
  return true;
}

QByteArray McpController::tokenFromEnvironment(bool mcpFlag) {
  if (!mcpFlag) return {};
  return env::bytes("UZUME_MCP_TOKEN").trimmed();
}

bool McpController::setTokenForThisStart(const QByteArray& token) {
  if (token.size() < 32 || token.size() > 256) return false;
  for (const char c : token) {
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    if (!ok) return false;
  }
  token_ = token;
  tokenFromEnv_ = true;
  return true;
}

void McpController::setToolInstaller(const std::function<void(mcp::McpToolRegistry&)>& installer) {
  if (installer) installer(registry_);
}

bool McpController::ensureToken() {
  if (!token_.isEmpty()) return true;
  if (!credentials_) return false;
  const std::optional<QString> stored = credentials_->read(QString::fromLatin1(kTokenTarget));
  if (stored && stored->size() >= 32) {
    token_ = stored->toLatin1();
    return true;
  }
  const QByteArray fresh = mcp::generateToken();
  if (!credentials_->write(QString::fromLatin1(kTokenTarget), QString::fromLatin1(fresh))) return false;
  token_ = fresh;
  return true;
}

void McpController::apply() {
  if (!enabled()) {
    server_.stop();
    error_.clear();
    emit changed();
    return;
  }
  if (server_.isListening() && server_.port() == port_) {
    emit changed();
    return;
  }
  if (!ensureToken()) {
    error_ = tr("The token could not be stored in the credential store.");
    emit changed();
    return;
  }
  QString err;
  if (server_.listen(static_cast<quint16>(port_), token_, &err)) {
    error_.clear();
  } else {
    error_ = tr("Port %1 is not available: %2").arg(port_).arg(err);
  }
  emit changed();
}

QString McpController::setupCommand() const {
  if (token_.isEmpty()) return {};
  return QStringLiteral("claude mcp add --transport http uzume http://127.0.0.1:%1/mcp --header \"Authorization: Bearer %2\"")
      .arg(port_)
      .arg(QString::fromLatin1(token_));
}

void McpController::copySetupCommand() {
  const QString cmd = setupCommand();
  if (cmd.isEmpty()) return;
  if (QClipboard* cb = QGuiApplication::clipboard()) cb->setText(cmd);
}

void McpController::renewToken() {
  if (!credentials_) return;
  const QByteArray fresh = mcp::generateToken();
  if (tokenFromEnv_) {  // the run's token came from the environment: the new one is kept in memory only
    token_ = fresh;
    if (server_.isListening()) {
      server_.stop();
      apply();
    } else {
      emit changed();
    }
    return;
  }
  if (!credentials_->write(QString::fromLatin1(kTokenTarget), QString::fromLatin1(fresh))) {
    error_ = tr("The token could not be stored in the credential store.");
    emit changed();
    return;
  }
  token_ = fresh;
  if (server_.isListening()) {
    server_.stop();  // open connections end; the new token applies to every new request
    apply();
  } else {
    emit changed();
  }
}

void McpController::stop() {
  forced_ = false;
  if (settings_->mcpEnabled()) settings_->setMcpEnabled(false);
  apply();
}

}  // namespace uzume::ui
