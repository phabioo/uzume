#pragma once
// McpController: the opt-in of the debug MCP server (ADR 0032 D2) for the Settings > Diagnostics section and the band
// "Claude can control this app". Owns the tool registry and the server (client/mcp), the setting (off by default, port),
// and the token in the credential store (target "Uzume/mcp"). The token is never a property, never logged and only leaves
// through copySetupCommand().
#include <QObject>
#include <QString>
#include <QtQml/qqmlregistration.h>
#include <functional>
#include <memory>

#include "mcp_registry.h"
#include "mcp_server.h"

namespace uzume {
class CredentialStore;
class PlayerSettings;
}

namespace uzume::ui {

class McpController : public QObject {
  Q_OBJECT
  QML_ELEMENT
  QML_UNCREATABLE("Provided by the PlayerController")
  // The switch "Allow Claude to control this app" (also on for the rest of the run after --mcp).
  Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY changed)
  Q_PROPERTY(int port READ port WRITE setPort NOTIFY changed)
  // The server accepts connections now (the band is shown).
  Q_PROPERTY(bool listening READ listening NOTIFY changed)
  // Why the server does not listen although it is switched on ("" = fine), e.g. the port is taken.
  Q_PROPERTY(QString error READ error NOTIFY changed)
 public:
  static constexpr const char* kTokenTarget = "Uzume/mcp";

  McpController(PlayerSettings* settings, std::shared_ptr<CredentialStore> credentials, QObject* parent = nullptr);
  ~McpController() override;

  bool enabled() const;
  void setEnabled(bool on);
  int port() const { return port_; }
  void setPort(int port);
  bool listening() const { return server_.isListening(); }
  QString error() const { return error_; }

  // The app starts: listens when the setting is on (or --mcp). Not called in the smoke test.
  void start();
  // `--mcp`: on for this run only, the setting stays as it is.
  void enableForThisStart();
  // `--mcp-port <n>`: the port for this run only (not stored). False when out of range 1024..65535 (nothing changes).
  bool setPortForThisStart(int port);
  // `UZUME_MCP_TOKEN`: the token for this run only; never stored in the credential store, never logged. Empty or too short
  // (< 32 chars, or characters outside base64url) is refused and the stored token stays in use.
  bool setTokenForThisStart(const QByteArray& token);
  // The value of UZUME_MCP_TOKEN, but only together with `--mcp` (`mcpFlag`); empty otherwise.
  static QByteArray tokenFromEnvironment(bool mcpFlag);

  // Installs the tools (the app calls it once; tests install their own).
  void setToolInstaller(const std::function<void(mcp::McpToolRegistry&)>& installer);
  mcp::McpToolRegistry* registry() { return &registry_; }
  mcp::McpServer* server() { return &server_; }

  // `claude mcp add --transport http uzume http://127.0.0.1:<port>/mcp --header "Authorization: Bearer <token>"`; empty
  // without a token. Contains the token: never log it.
  QString setupCommand() const;
  Q_INVOKABLE void copySetupCommand();
  // A new token; connections with the old one stop working at once.
  Q_INVOKABLE void renewToken();
  // The band's "Stop": switches the server off (and the setting).
  Q_INVOKABLE void stop();

 signals:
  void changed();

 private:
  void apply();
  bool ensureToken();

  PlayerSettings* settings_;
  std::shared_ptr<CredentialStore> credentials_;
  mcp::McpToolRegistry registry_;
  mcp::McpServer server_;
  QByteArray token_;
  int port_;
  bool forced_ = false;
  bool tokenFromEnv_ = false;  // token_ came from UZUME_MCP_TOKEN: renewToken() keeps it out of the credential store
  QString error_;
};

}  // namespace uzume::ui
