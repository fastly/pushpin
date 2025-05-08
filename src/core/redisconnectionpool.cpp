// redisconnectionpool.cpp
#include "redisconnectionpool.h"

RedisConnectionPool::RedisConnectionPool(int size) : semaphore(size) {
	for (int i = 0; i < size; ++i) {
		auto *conn = new RedisConnection_;
		if (conn->connectToServer())
			pool.enqueue(conn);
		else
			delete conn;
	}
}

RedisConnectionPool::~RedisConnectionPool() {
	while (!pool.isEmpty())
		delete pool.dequeue();
}

RedisConnection_* RedisConnectionPool::acquire() {
	semaphore.acquire();
	QMutexLocker locker(&mutex);
	return pool.dequeue();
}

void RedisConnectionPool::release(RedisConnection_* conn) {
	QMutexLocker locker(&mutex);
	pool.enqueue(conn);
	semaphore.release();
}
