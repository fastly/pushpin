// redisconnection.cpp
#include "redisconnection.h"

RedisConnection_::RedisConnection_(QObject *parent) : QObject(parent), ctx(nullptr) {}

RedisConnection_::~RedisConnection_() {
	if (ctx) redisFree(ctx);
}

bool RedisConnection_::connectToServer(const QString &host, int port) {
	QMutexLocker locker(&mutex);
	if (ctx) redisFree(ctx);

	struct timeval timeout = {1, 500000};  // 1.5 seconds
	ctx = redisConnectWithTimeout(host.toUtf8().constData(), port, timeout);

	return ctx && !ctx->err;
}

bool RedisConnection_::isConnected() const {
	return ctx && !ctx->err;
}

bool RedisConnection_::appendCommand(const QByteArray &cmd) {
	QMutexLocker locker(&mutex);
	if (!isConnected()) return false;
	return redisAppendCommand(ctx, cmd.constData()) == REDIS_OK;
}

QList<QByteArray> RedisConnection_::flushPipeline(int expectedReplies) {
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
