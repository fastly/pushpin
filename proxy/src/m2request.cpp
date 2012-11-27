#include "m2request.h"

#include <assert.h>
#include <QPointer>
#include <QFile>
#include <QFileSystemWatcher>
#include "packet/m2requestpacket.h"
#include "m2manager.h"

#define BUFFER_SIZE (1024 * 64)

	/*void handleM2Request(QZmq::Socket *sock, bool https)
	{
		if(maxWorkers != -1 && workers >= maxWorkers)
			return;

		QList<QByteArray> msg = sock->read();
		if(msg.count() != 1)
		{
			log_warning("received message with parts != 1, skipping");
			return;
		}

		log_info("IN m2 %s", qPrintable(TnetString::byteArrayToEscapedString(msg[0])));

		M2RequestPacket req;
		if(!req.fromByteArray(msg[0]))
		{
			log_warning("received message with invalid format, skipping");
			return;
		}

		req.isHttps = https;

		handleIncomingRequest(req);
	}

	void handleIncomingRequest(const M2RequestPacket &req)
	{
		printf("id=[%s] method=[%s], path=[%s]\n", req.id.data(), qPrintable(req.method), req.path.data());
		printf("headers:\n");
		foreach(const HttpHeader &header, req.headers)
			printf("  [%s] = [%s]\n", header.first.data(), header.second.data());
		printf("body: [%s]\n", req.body.data());

		RequestSession *rs = new RequestSession(this);
		connect(rs, SIGNAL(outgoingInspectRequest(const InspectRequestPacket &)), SLOT(rs_outgoingInspectRequest(const InspectRequestPacket &)));
		connect(rs, SIGNAL(inspectFinished(const M2RequestPacket &, bool, const QByteArray &, const InspectResponsePacket *)), SLOT(rs_inspectFinished(const M2RequestPacket &, bool, const QByteArray &, const InspectResponsePacket *)));
		rs->start(req);
	}*/

class M2Request::Private : public QObject
{
	Q_OBJECT

public:
	M2Request *q;
	M2Manager *manager;
	bool active;
	M2RequestPacket p;
	bool isHttps;
	bool finished;
	QFile *file;
	QFileSystemWatcher *watcher;
	bool fileComplete;
	QByteArray in;
	int rcount;

	Private(M2Request *_q) :
		QObject(_q),
		q(_q),
		manager(0),
		active(false),
		isHttps(false),
		finished(false),
		file(0),
		watcher(0),
		fileComplete(false),
		rcount(0)
	{
	}

	~Private()
	{
		if(manager)
			manager->unlink(q);
	}

	bool tryReadFile()
	{
		int avail = BUFFER_SIZE - in.size();
		if(avail <= 0)
			return true;

		assert(file);

		QByteArray buf(avail, 0);
		qint64 pos = file->pos();
		qint64 ret = file->read(buf.data(), avail);
		if(ret < 0)
			return false;

		buf.resize(ret);

		if(file->atEnd())
		{
			// take a step back from EOF
			if(!file->seek(pos + ret))
				return false;
		}

		if(!buf.isEmpty())
		{
			in += buf;
			rcount += buf.size();

			if(fileComplete && rcount >= file->size())
			{
				delete watcher;
				watcher = 0;
				delete file;
				file = 0;
			}

			if(active)
			{
				QPointer<QObject> self = this;
				emit q->readyRead();
				if(!self)
					return true;

				tryFinish();
			}
		}

		return true;
	}

	void tryFinish()
	{
		// file is null once everything is in a local buffer, or if a file wasn't used at all
		if(active && !file)
		{
			manager->unlink(q);
			//manager = 0;
			finished = true;
			emit q->finished();
		}
	}

public slots:
	void watcher_fileChanged(const QString &path)
	{
		Q_UNUSED(path);

		if(in.size() < BUFFER_SIZE)
			doRead();
	}

	void doActivate()
	{
		QPointer<QObject> self = this;

		if(!in.isEmpty())
		{
			emit q->readyRead();
			if(!self)
				return;
		}

		tryFinish();
	}

	void doRead()
	{
		if(!tryReadFile())
		{
			// TODO: log error
		}
	}
};

M2Request::M2Request(QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
}

M2Request::~M2Request()
{
	delete d;
}

M2Request::Rid M2Request::rid() const
{
	return Rid(d->p.sender, d->p.id);
}

bool M2Request::isHttps() const
{
	return d->isHttps;
}

bool M2Request::isFinished() const
{
	return d->finished;
}

QString M2Request::method() const
{
	return d->p.method;
}

QByteArray M2Request::path() const
{
	return d->p.path;
}

const HttpHeaders & M2Request::headers() const
{
	return d->p.headers;
}

QByteArray M2Request::read()
{
	QByteArray out = d->in;
	d->in.clear();
	if(d->file)
		QMetaObject::invokeMethod(d, "doRead", Qt::QueuedConnection);
	return out;
}

int M2Request::actualContentLength() const
{
	return d->rcount;
}

M2Response *M2Request::createResponse()
{
	return d->manager->createResponse(Rid(d->p.sender, d->p.id));
}

bool M2Request::handle(M2Manager *manager, const M2RequestPacket &packet, bool https)
{
	d->manager = manager;
	d->p = packet;
	d->isHttps = https;

	if(!d->p.uploadFile.isEmpty())
	{
		// if uploadFile was specified then start monitoring
		d->file = new QFile(d->p.uploadFile, d);
		if(!d->file->open(QFile::ReadOnly))
		{
			// TODO: log warning
			return false;
		}

		d->watcher = new QFileSystemWatcher(d);
		connect(d->watcher, SIGNAL(fileChanged(const QString &)), d, SLOT(watcher_fileChanged(const QString &)));
		d->watcher->addPath(d->p.uploadFile);

		if(!d->tryReadFile())
		{
			// TODO: log error
			return false;
		}
	}
	else
	{
		// if no uploadFile was specified then we have the body in the packet and we're done
		d->in = d->p.body;
		d->rcount = d->p.body.size();
		d->p.body.clear();
		d->finished = true;
	}

	return true;
}

void M2Request::activate()
{
	d->active = true;
	QMetaObject::invokeMethod(d, "doActivate", Qt::QueuedConnection);
}

void M2Request::uploadDone()
{
	d->fileComplete = true;

	if(d->rcount >= d->file->size())
	{
		delete d->watcher;
		d->watcher = 0;
		delete d->file;
		d->file = 0;

		d->tryFinish();
	}
}

#include "m2request.moc"
