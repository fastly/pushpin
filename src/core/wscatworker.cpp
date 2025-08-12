// wscatworker.cpp
#include "wscatworker.h"
#include <QDebug>
#include <QThread>

#include "log.h"

WscatWorker::WscatWorker(QObject *parent) : QObject(parent), process(nullptr) {}

WscatWorker::~WscatWorker() {
	stopWscat();
}

void WscatWorker::startWscat(const QString &url, const QStringList &headers) {

	QThread::msleep(100);
	QMutexLocker locker(&mutex);
	if (process) return;

	process = new QProcess(this);

	QStringList args;
	for (const QString& h : headers) {
		args << "--header" << h;
	}
	args << "-c" << url;

	process->start("wscat", args);
	if (!process->waitForStarted()) {
		log_debug("[WS] Failed to start wscat");
		delete process;
		process = nullptr;
	}

	process->waitForFinished(-1); // Wait until wscat exits
}

void WscatWorker::stopWscat() {
	QMutexLocker locker(&mutex);
	if (process) {
		log_debug("[WS] killing wscat process");
		process->kill();     // or process->terminate() for graceful
		process->waitForFinished(3000);
		process->deleteLater();
		process = nullptr;
	}
}
