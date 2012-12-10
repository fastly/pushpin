#include "app.h"

#include <assert.h>
#include <QCoreApplication>
#include <QStringList>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QSettings>
#include "qzmqsocket.h"
#include "packet/tnetstring.h"
#include "packet/acceptresponsepacket.h"
#include "log.h"
#include "inspectdata.h"
#include "acceptdata.h"
#include "m2manager.h"
#include "m2request.h"
#include "m2response.h"
#include "zurlmanager.h"
#include "zurlrequest.h"
#include "inspectmanager.h"
#include "domainmap.h"
#include "requestsession.h"
#include "proxysession.h"

#define VERSION "1.0"

class App::Private : public QObject
{
	Q_OBJECT

public:
	App *q;
	bool verbose;
	QByteArray clientId;
	M2Manager *m2;
	ZurlManager *zurl;
	InspectManager *inspect;
	DomainMap *domainMap;
	QZmq::Socket *handler_retry_in_sock;
	QZmq::Socket *handler_accept_out_sock;
	int workers;
	int maxWorkers;

	Private(App *_q) :
		QObject(_q),
		q(_q),
		verbose(false),
		m2(0),
		zurl(0),
		inspect(0),
		domainMap(0),
		handler_retry_in_sock(0),
		handler_accept_out_sock(0),
		workers(0)
	{
	}

	void start()
	{
		QStringList args = QCoreApplication::instance()->arguments();
		args.removeFirst();

		// options
		QHash<QString, QString> options;
		for(int n = 0; n < args.count(); ++n)
		{
			if(args[n] == "--")
			{
				break;
			}
			else if(args[n].startsWith("--"))
			{
				QString opt = args[n].mid(2);
				QString var, val;

				int at = opt.indexOf("=");
				if(at != -1)
				{
					var = opt.mid(0, at);
					val = opt.mid(at + 1);
				}
				else
					var = opt;

				options[var] = val;

				args.removeAt(n);
				--n; // adjust position
			}
		}

		if(options.contains("version"))
		{
			printf("pushpin-proxy %s\n", VERSION);
			emit q->quit();
			return;
		}

		if(options.contains("verbose"))
			log_setOutputLevel(LOG_LEVEL_DEBUG);
		else
			log_setOutputLevel(LOG_LEVEL_WARNING);

		QString configFile = options["config"];
		if(configFile.isEmpty())
			configFile = "/etc/pushpin/pushpin.conf";

		// QSettings doesn't inform us if the config file doesn't exist, so do that ourselves
		{
			QFile file(configFile);
			if(!file.open(QIODevice::ReadOnly))
			{
				log_error("failed to open %s, and --config not passed", qPrintable(configFile));
				emit q->quit();
				return;
			}
		}

		QSettings settings(configFile, QSettings::IniFormat);

		QStringList m2_in_specs = settings.value("proxy/m2_in_specs").toStringList();
		QStringList m2_inhttps_specs = settings.value("proxy/m2_inhttps_specs").toStringList();
		QStringList m2_out_specs = settings.value("proxy/m2_out_specs").toStringList();
		QStringList zurl_out_specs = settings.value("proxy/zurl_out_specs").toStringList();
		QStringList zurl_out_stream_specs = settings.value("proxy/zurl_out_stream_specs").toStringList();
		QStringList zurl_in_specs = settings.value("proxy/zurl_in_specs").toStringList();
		QString handler_inspect_spec = settings.value("proxy/handler_inspect_spec").toString();
		QString handler_retry_in_spec = settings.value("proxy/handler_retry_in_spec").toString();
		QString handler_accept_out_spec = settings.value("proxy/handler_accept_out_spec").toString();
		//maxWorkers = settings.value("proxy/max_open_requests", -1).toInt();
		QString domainsfile = settings.value("proxy/domainsfile").toString();

		// if domainsfile is a relative path, then use it relative to the config file location
		QFileInfo fi(domainsfile);
		if(fi.isRelative())
		{
			QString fname = fi.fileName();
			domainsfile = QFileInfo(QDir(QFileInfo(configFile).absolutePath()), fname).filePath();
		}

		domainMap = new DomainMap(domainsfile);

		if(m2_in_specs.isEmpty() && m2_inhttps_specs.isEmpty())
		{
			log_error("must set at least one of m2_in_specs and m2_inhttps_specs");
			emit q->quit();
			return;
		}

		if(m2_out_specs.isEmpty() || zurl_out_specs.isEmpty() || zurl_out_stream_specs.isEmpty() || zurl_in_specs.isEmpty())
		{
			log_error("must set m2_out_specs, zurl_out_specs, zurl_out_stream_specs, and zurl_in_specs");
			emit q->quit();
			return;
		}

		m2 = new M2Manager(this);
		connect(m2, SIGNAL(requestReady()), SLOT(m2_requestReady()));

		if(!m2_in_specs.isEmpty())
			m2->setIncomingPlainSpecs(m2_in_specs);

		if(!m2_inhttps_specs.isEmpty())
			m2->setIncomingHttpsSpecs(m2_inhttps_specs);

		m2->setOutgoingSpecs(m2_out_specs);

		clientId = "pushpin-proxy_" + QByteArray::number(QCoreApplication::applicationPid());

		zurl = new ZurlManager(this);
		zurl->setClientId(clientId);

		zurl->setOutgoingSpecs(zurl_out_specs);
		zurl->setOutgoingStreamSpecs(zurl_out_stream_specs);
		zurl->setIncomingSpecs(zurl_in_specs);

		if(!handler_inspect_spec.isEmpty())
		{
			inspect = new InspectManager(this);
			if(!inspect->setSpec(handler_inspect_spec))
			{
				log_error("unable to bind to handler_inspect_spec: %s", qPrintable(handler_inspect_spec));
				emit q->quit();
				return;
			}
		}

		if(!handler_retry_in_spec.isEmpty())
		{
			handler_retry_in_sock = new QZmq::Socket(QZmq::Socket::Pull, this);
			connect(handler_retry_in_sock, SIGNAL(readyRead()), SLOT(handler_retry_in_readyRead()));
			if(!handler_retry_in_sock->bind(handler_retry_in_spec))
			{
				log_error("unable to bind to handler_retry_in_spec: %s", qPrintable(handler_retry_in_spec));
				emit q->quit();
				return;
			}
		}

		if(!handler_accept_out_spec.isEmpty())
		{
			handler_accept_out_sock = new QZmq::Socket(QZmq::Socket::Push, this);
			connect(handler_accept_out_sock, SIGNAL(messagesWritten(int)), SLOT(handler_accept_out_messagesWritten(int)));
			if(!handler_accept_out_sock->bind(handler_accept_out_spec))
			{
				log_error("unable to bind to handler_accept_out_spec: %s", qPrintable(handler_accept_out_spec));
				emit q->quit();
				return;
			}
		}

		log_info("started");
	}

private slots:
	void m2_requestReady()
	{
		M2Request *req = m2->takeNext();

		QByteArray host = req->headers().get("host");
		if(host.isEmpty())
		{
			// TODO: log warning, skip because of no host value
			return;
		}

		QString shost = QString::fromUtf8(host);

		QByteArray scheme;
		if(req->isHttps())
			scheme = "https";
		else
			scheme = "http";

		int port;
		int at = shost.lastIndexOf(':');
		if(at != -1)
		{
			QString sport = shost.mid(at + 1);
			bool ok;
			port = sport.toInt(&ok);
			if(!ok)
			{
				// TODO: log warning, invalid host
				return;
			}

			shost = shost.mid(0, at);
		}
		else
		{
			if(req->isHttps())
				port = 443;
			else
				port = 80;
		}

		QByteArray url = scheme + "://" + shost.toUtf8();
		if((req->isHttps() && port != 443) || (!req->isHttps() && port != 80))
			url += ':' + QByteArray::number(port);
		url += req->path();

		log_info("IN id=%d, %s %s", req->rid().second.data(), qPrintable(req->method()), qPrintable(url));

		RequestSession *rs = new RequestSession(inspect, this);
		connect(rs, SIGNAL(inspectFinished(const InspectData &)), SLOT(rs_inspectFinished(const InspectData &)));
		rs->start(req);
	}

	void rs_inspectFinished(const InspectData &idata)
	{
		RequestSession *rs = (RequestSession *)sender();

		if(idata.doProxy)
		{
			ProxySession *ps = new ProxySession(zurl, domainMap, this);
			connect(ps, SIGNAL(finishedByPassthrough()), SLOT(ps_finishedByPassthrough()));
			connect(ps, SIGNAL(finishedForAccept(const AcceptData &)), SLOT(ps_finishedForAccept(const AcceptData &)));
			ps->add(rs);
		}
		else
		{
			delete rs;
		}
	}

	void ps_finishedByPassthrough()
	{
		ProxySession *ps = (ProxySession *)sender();

		delete ps;
	}

	void ps_finishedForAccept(const AcceptData &adata)
	{
		ProxySession *ps = (ProxySession *)sender();

		AcceptResponsePacket p;
		foreach(const M2Request::Rid &rid, adata.rids)
			p.rids += AcceptResponsePacket::Rid(rid.first, rid.second);

		if(adata.haveInspectData)
		{
			// TODO
		}

		assert(adata.haveResponse);

		p.haveResponse = true; // Accept from ProxySession always has a response
		p.response = adata.response;

		delete ps;

		QList<QByteArray> msg;
		msg += TnetString::fromVariant(p.toVariant());
		handler_accept_out_sock->write(msg);
	}

	void handler_retry_in_readyRead()
	{
#if 0
		if(maxWorkers != -1 && workers >= maxWorkers)
			return;

		QList<QByteArray> msg = retry_in_sock->read();
		if(msg.count() != 1)
		{
			log_warning("received message with parts != 1, skipping");
			return;
		}

		bool ok;
		QVariant data = TnetString::toVariant(msg[0], 0, &ok);
		if(!ok)
		{
			log_warning("received message with invalid format (tnetstring parse failed), skipping");
			return;
		}

		log_info("IN retry %s", qPrintable(TnetString::variantToString(data)));

		// FIXME: we should use our own internal (non-m2) format here
		/*M2RequestPacket req;
		if(!req.fromVariant(data))
		{
			log_warning("received message with invalid format, skipping");
			return;
		}

		handleIncomingRequest(req);*/
#endif
	}

	void handler_accept_out_messagesWritten(int count)
	{
		// TODO
		Q_UNUSED(count);
	}
};

App::App(QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
}

App::~App()
{
	delete d;
}

void App::start()
{
	d->start();
}

#include "app.moc"
