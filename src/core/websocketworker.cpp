#include "websocketworker.h"
#include <QDebug>

WebSocketWorker::WebSocketWorker(const QUrl& url, const QMap<QString, QString>& headers, QObject* parent)
    : QObject(parent), m_url(url), m_headers(headers) {

    connect(&m_socket, &QWebSocket::connected, this, &WebSocketWorker::onConnected);
    connect(&m_socket, &QWebSocket::disconnected, this, &WebSocketWorker::onDisconnected);
    connect(&m_socket, &QWebSocket::textMessageReceived, this, &WebSocketWorker::onTextMessageReceived);
}

void WebSocketWorker::start() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QWebSocketHandshakeOptions options;
    QList<QPair<QByteArray, QByteArray>> headersList;
    for (auto it = m_headers.begin(); it != m_headers.end(); ++it) {
        headersList.append({it.key().toUtf8(), it.value().toUtf8()});
    }
    options.setRequestHeaders(headersList);
    m_socket.open(m_url, options);
#else
    // Qt 5.x workaround: cannot set headers directly.
    qWarning() << "Custom headers in WebSocket only supported in Qt 6+.";
    m_socket.open(m_url);
#endif
}

void WebSocketWorker::stop() {
    m_socket.close();
}

void WebSocketWorker::sendMessage(const QString& message) {
    m_socket.sendTextMessage(message);
}

void WebSocketWorker::onConnected() {
    qDebug() << "WebSocket connected.";
    emit connected();
}

void WebSocketWorker::onDisconnected() {
    qDebug() << "WebSocket disconnected.";
    emit disconnected();
    emit finished();
}

void WebSocketWorker::onTextMessageReceived(const QString& message) {
    emit messageReceived(message);
}
