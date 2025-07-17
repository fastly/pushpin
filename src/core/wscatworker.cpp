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
	connect(process, &QProcess::readyReadStandardOutput, [=]() {
		//qDebug() << "[wscat out]" << process->readAllStandardOutput();
	});
	connect(process, &QProcess::readyReadStandardError, [=]() {
		//qDebug() << "[wscat err]" << process->readAllStandardError();
	});
	connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
			this, [=](int exitCode, QProcess::ExitStatus status) {
		//qDebug() << "[wscat finished]" << exitCode << status;
		emit finished();
	});

	process->start("wscat", args);
	if (!process->waitForStarted()) {
		log_debug("[WS] Failed to start wscat");
		delete process;
		process = nullptr;
	}
}

void WscatWorker::stopWscat() {
	QMutexLocker locker(&mutex);
	if (process) {
		process->kill();     // or process->terminate() for graceful
		process->waitForFinished(3000);
		process->deleteLater();
		process = nullptr;
	}
}
