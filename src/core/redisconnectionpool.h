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

	RedisConnection* acquire();
	void release(RedisConnection* conn);

private:
	QQueue<RedisConnection*> pool;
	QMutex mutex;
	QSemaphore semaphore;
};
