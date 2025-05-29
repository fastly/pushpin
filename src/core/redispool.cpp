// RedisPool.cpp
#include "redispool.h"
#include "log.h"

RedisPool* RedisPool::instance() {
	static RedisPool pool;
	return &pool;
}

RedisPool::RedisPool() {}

RedisPool::~RedisPool() {
	QMutexLocker locker(&m_mutex);
	while (!m_pool.isEmpty()) {
		redisFree(m_pool.dequeue());
	}
}

redisContext* RedisPool::createConnection() {
	return redisConnect("127.0.0.1", 6379); // Update if needed
}

QSharedPointer<redisContext> RedisPool::acquire() {
	QMutexLocker locker(&m_mutex);
	while (m_pool.isEmpty() && m_activeConnections >= m_maxConnections) {
		m_cond.wait(&m_mutex);
	}

	redisContext* conn = nullptr;
	if (!m_pool.isEmpty()) {
		conn = m_pool.dequeue();
	} else {
		conn = createConnection();
		++m_activeConnections;
	}

	log_debug("[TTT] active connections = %d", m_activeConnections);

	return QSharedPointer<redisContext>(conn, [](redisContext* c) {
		RedisPool::instance()->release(c);
	});
}

void RedisPool::release(redisContext* conn) {
	QMutexLocker locker(&m_mutex);
	m_pool.enqueue(conn);
	m_cond.wakeOne();
}
