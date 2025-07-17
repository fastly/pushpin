// wscatworker.cpp
#include "wscatworker.h"
#include <QDebug>
#include <QThread>

#include "log.h"

WscatWorker::WscatWorker(QObject *parent) : QObject(parent), process(nullptr) {}

WscatWorker::~WscatWorker() {
	stopWscat();
}

void WscatWorker::startWscat(const QStringList &args) {

	QThread::msleep(100);
	log_debug("1");
	QMutexLocker locker(&mutex);
	log_debug("2");
	if (process) return;
	log_debug("3");

	process = new QProcess(this);

	log_debug("4");

	process->start("wscat", args);
	log_debug("5");
	if (!process->waitForStarted()) {
		log_debug("[WS] Failed to start wscat");
		delete process;
		process = nullptr;
	}
	log_debug("6");

	process->waitForFinished(-1); // Wait until wscat exits
	log_debug("7");
}

void WscatWorker::stopWscat() {
	QMutexLocker locker(&mutex);
	if (process) {
		log_debug("8");
		process->kill();     // or process->terminate() for graceful
		process->waitForFinished(3000);
		process->deleteLater();
		process = nullptr;
	}
}
