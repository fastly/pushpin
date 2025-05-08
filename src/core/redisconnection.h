// redisconnection.h
#pragma once
#include <QObject>
#include <QMutex>
#include <QByteArray>
#include <hiredis/hiredis.h>

class RedisConnection_ : public QObject {
	Q_OBJECT
public:
	RedisConnection_(QObject *parent = nullptr);
	~RedisConnection_();

	bool connectToServer(const QString &host = "127.0.0.1", int port = 6379);
	bool isConnected() const;

	bool appendCommand(const QByteArray &cmd);
	QList<QByteArray> flushPipeline(int expectedReplies);

public:
	redisContext *ctx;
	QMutex mutex;
};
