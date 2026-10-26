#pragma once
// Minimal HTTP/1.1 client for the MCP tests: one request per call over a real QTcpSocket, answered while the server runs in
// the same event loop.
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QTcpSocket>
#include <QtTest>

namespace mcptest {

struct Response {
  int status = 0;
  QMap<QByteArray, QByteArray> headers;
  QByteArray body;
  bool closed = false;
  QJsonObject json() const { return QJsonDocument::fromJson(body).object(); }
};

// Reads one response from `s`; status 0 when nothing complete arrives in time.
inline Response readResponse(QTcpSocket& s, QByteArray& buf, int timeoutMs = 5000) {
  Response r;
  QElapsedTimer t;
  t.start();
  while (t.elapsed() < timeoutMs) {
    const qsizetype end = buf.indexOf("\r\n\r\n");
    if (end >= 0) {
      const QList<QByteArray> lines = buf.left(end).split('\n');
      const QList<QByteArray> st = lines.first().split(' ');
      qint64 len = 0;
      QMap<QByteArray, QByteArray> h;
      for (qsizetype i = 1; i < lines.size(); ++i) {
        const qsizetype c = lines[i].indexOf(':');
        if (c > 0) h.insert(lines[i].left(c).trimmed().toLower(), lines[i].mid(c + 1).trimmed());
      }
      len = h.value("content-length").toLongLong();
      if (buf.size() >= end + 4 + len) {
        r.status = st.value(1).toInt();
        r.headers = h;
        r.body = buf.mid(end + 4, len);
        buf.remove(0, end + 4 + len);
        return r;
      }
    }
    if (s.state() != QAbstractSocket::ConnectedState && !s.bytesAvailable()) {
      r.closed = true;
      break;
    }
    QTest::qWait(5);
    buf.append(s.readAll());
  }
  return r;
}

inline Response request(quint16 port, const QByteArray& method, const QByteArray& path, const QMap<QByteArray, QByteArray>& headers, const QByteArray& body,
                        bool addLength = true) {
  QTcpSocket s;
  s.connectToHost(QStringLiteral("127.0.0.1"), port);
  if (!s.waitForConnected(2000)) return {};
  QByteArray raw = method + " " + path + " HTTP/1.1\r\n";
  for (auto it = headers.begin(); it != headers.end(); ++it) raw += it.key() + ": " + it.value() + "\r\n";
  if (addLength) raw += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
  raw += "\r\n" + body;
  s.write(raw);
  QByteArray buf;
  return readResponse(s, buf);
}

inline QMap<QByteArray, QByteArray> goodHeaders(quint16 port, const QByteArray& token) {
  return {{"Host", "127.0.0.1:" + QByteArray::number(port)}, {"Authorization", "Bearer " + token}, {"Content-Type", "application/json"}};
}

inline QByteArray rpc(int id, const QString& method, const QJsonObject& params = {}) {
  return QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}}).toJson(QJsonDocument::Compact);
}

}  // namespace mcptest
