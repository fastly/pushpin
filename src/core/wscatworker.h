// wscatworker.h
#pragma once

#include <QObject>
#include <QProcess>
#include <QMutex>

class WscatWorker : public QObject {
	Q_OBJECT
public:
	explicit WscatWorker(QObject *parent = nullptr);
	~WscatWorker();

public slots:
	void startWscat(const QString &url, const QStringList &args);
	void stopWscat();

signals:
	void finished();

private:
	QProcess *process;
	QMutex mutex;
};
