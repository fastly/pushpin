// redisconnectionpool.h
#pragma once
#include "redisconnection.h"
#include <QQueue>
#include <QMutex>
#include <QSemaphore>

class RedisConnectionPool {
public:
	RedisConnectionPool(int size);
	~RedisConnectionPool();

	RedisConnection_* acquire();
	void release(RedisConnection_* conn);

private:
	QQueue<RedisConnection_*> pool;
	QMutex mutex;
	QSemaphore semaphore;
};
