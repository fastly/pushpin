#pragma once

#include <QObject>
#include <QWebSocket>
#include <QWebSocketHandshakeOptions>
#include <QUrl>
#include <QMap>

class WebSocketWorker : public QObject {
    Q_OBJECT
public:
    WebSocketWorker(const QUrl& url, const QMap<QString, QString>& headers, QObject* parent = nullptr);

public slots:
    void start();
    void stop();
    void sendMessage(const QString& message);

signals:
    void messageReceived(const QString& message);
    void connected();
    void disconnected();
    void finished();

private slots:
    void onConnected();
    void onDisconnected();
    void onTextMessageReceived(const QString& message);

private:
    QWebSocket m_socket;
    QUrl m_url;
    QMap<QString, QString> m_headers;
};
