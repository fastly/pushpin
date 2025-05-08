#include <QtConcurrent>
#include "redisconnectionpool.h"

// Global or shared pool
RedisConnectionPool pool(4);

// Example function to run pipelined Redis commands asynchronously
void runRedisPipelineAsync() {
	QtConcurrent::run([=]() {
		RedisConnection_ *conn = pool.acquire();

		if (!conn->isConnected()) {
			qWarning() << "Redis not connected";
			pool.release(conn);
			return;
		}

		conn->appendCommand("SET async:key1 \"value1\"");
		conn->appendCommand("GET async:key1");
		conn->appendCommand("INCR async:counter");
		conn->appendCommand("GET async:counter");

		QList<QByteArray> replies = conn->flushPipeline(4);
		for (const QByteArray &r : replies)
			qDebug() << "[Async Reply]" << r;

		pool.release(conn);
	});
}
