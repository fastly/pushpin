// RedisPool.h
#pragma once
#include <QMutex>
#include <QQueue>
#include <QScopedPointer>
#include <QSharedPointer>
#include <QWaitCondition>
#include <hiredis.h>

class RedisPool {
public:
	static RedisPool* instance();

	QSharedPointer<redisContext> acquire();
	void release(redisContext* conn);

private:
	RedisPool();
	~RedisPool();

	redisContext* createConnection();

	QMutex m_mutex;
	QWaitCondition m_cond;
	QQueue<redisContext*> m_pool;
	const int m_maxConnections = 10;
	int m_activeConnections = 0;
};
