// redisconnection.cpp
#include "redisconnection.h"

RedisConnection::RedisConnection(QObject *parent) : QObject(parent), ctx(nullptr) {}

RedisConnection::~RedisConnection() {
	if (ctx) redisFree(ctx);
}

bool RedisConnection::connectToServer(const QString &host, int port) {
	QMutexLocker locker(&mutex);
	if (ctx) redisFree(ctx);

	struct timeval timeout = {1, 500000};  // 1.5 seconds
	ctx = redisConnectWithTimeout(host.toUtf8().constData(), port, timeout);

	return ctx && !ctx->err;
}

bool RedisConnection::isConnected() const {
	return ctx && !ctx->err;
}

bool RedisConnection::appendCommand(const QByteArray &cmd) {
	QMutexLocker locker(&mutex);
	if (!isConnected()) return false;
	return redisAppendCommand(ctx, cmd.constData()) == REDIS_OK;
}

QList<QByteArray> RedisConnection::flushPipeline(int expectedReplies) {
	QList<QByteArray> replies;
	redisReply *reply = nullptr;
	for (int i = 0; i < expectedReplies; ++i) {
		if (redisGetReply(ctx, (void**)&reply) == REDIS_OK && reply) {
			if (reply->type == REDIS_REPLY_STRING || reply->type == REDIS_REPLY_STATUS)
				replies << QByteArray(reply->str, reply->len);
			else
				replies << "(non-string)";
			freeReplyObject(reply);
		} else {
			replies << "(error)";
		}
	}
	return replies;
}
