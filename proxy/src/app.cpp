#include <QCoreApplication>
#include <QStringList>
#include <QFile>
#include <QSettings>
#include "qzmqsocket.h"
#include "packet/tnetstring.h"
#include "packet/m2requestpacket.h"
#include "packet/m2responsepacket.h"
#include "packet/inspectrequestpacket.h"
#include "packet/inspectresponsepacket.h"
#include "packet/zurlrequestpacket.h"
#include "packet/zurlresponsepacket.h"
#include "m2manager.h"
#include "m2request.h"
#include "m2response.h"
#include "zurlmanager.h"
#include "zurlrequest.h"
//#include "requestsession.h"
//#include "proxysession.h"

#include "app.h"

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
	QZmq::Socket *inspect_req_sock;
	QZmq::Socket *retry_in_sock;
	QZmq::Socket *accept_out_sock;
	int workers;
	int maxWorkers;

	Private(App *_q) :
		QObject(_q),
		q(_q),
		verbose(false),
		m2(0),
		zurl(0),
		inspect_req_sock(0),
		retry_in_sock(0),
		accept_out_sock(0),
		workers(0)
	{
	}

	void log(int level, const char *fmt, va_list ap) const
	{
		if(level <= 1 || verbose)
		{
			QString str;
			str.vsprintf(fmt, ap);

			const char *lstr;
			switch(level)
			{
				case 0: lstr = "ERR"; break;
				case 1: lstr = "WARN"; break;
				case 2:
				default:
					lstr = "INFO"; break;
			}

			fprintf(stderr, "[%s] %s\n", lstr, qPrintable(str));
		}
	}

	void log_info(const char *fmt, ...) const
	{
		va_list ap;
		va_start(ap, fmt);
		log(2, fmt, ap);
		va_end(ap);
	}

	void log_warning(const char *fmt, ...) const
	{
		va_list ap;
		va_start(ap, fmt);
		log(1, fmt, ap);
		va_end(ap);
	}

	void log_error(const char *fmt, ...) const
	{
		va_list ap;
		va_start(ap, fmt);
		log(0, fmt, ap);
		va_end(ap);
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
			verbose = true;

		QString configFile = options["config"];
		if(configFile.isEmpty())
			configFile = "/etc/pushpin.conf";

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
		QString inspect_req_spec = settings.value("proxy/inspect_req_spec").toString();
		QString retry_in_spec = settings.value("proxy/retry_in_spec").toString();
		QString accept_out_spec = settings.value("proxy/accept_out_spec").toString();
		maxWorkers = settings.value("proxy/max_open_requests", -1).toInt();

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

		if(!inspect_req_spec.isEmpty())
		{
			inspect_req_sock = new QZmq::Socket(QZmq::Socket::Dealer, this);
			connect(inspect_req_sock, SIGNAL(readyRead()), SLOT(inspect_req_readyRead()));
			connect(inspect_req_sock, SIGNAL(messagesWritten(int)), SLOT(inspect_req_messagesWritten(int)));
			inspect_req_sock->bind(inspect_req_spec);
		}

		if(!retry_in_spec.isEmpty())
		{
			retry_in_sock = new QZmq::Socket(QZmq::Socket::Pull, this);
			connect(retry_in_sock, SIGNAL(readyRead()), SLOT(retry_in_readyRead()));
			retry_in_sock->bind(retry_in_spec);
		}

		if(!accept_out_spec.isEmpty())
		{
			accept_out_sock = new QZmq::Socket(QZmq::Socket::Push, this);
			connect(accept_out_sock, SIGNAL(messagesWritten(int)), SLOT(accept_out_messagesWritten(int)));
			accept_out_sock->bind(accept_out_spec);
		}

		log_info("started");
	}

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

	M2Request *req;
	ZurlRequest *zr;
	bool firstRead;

private slots:
	void m2_requestReady()
	{
		//handleM2Request(m2_in_sock, false);
		req = m2->takeNext();
		connect(req, SIGNAL(readyRead()), SLOT(req_readyRead()));
		connect(req, SIGNAL(finished()), SLOT(req_finished()));

		zr = zurl->createRequest();
		connect(zr, SIGNAL(readyRead()), SLOT(zr_readyRead()));
		connect(zr, SIGNAL(bytesWritten(int)), SLOT(zr_bytesWritten(int)));
		connect(zr, SIGNAL(error()), SLOT(zr_error()));
		QUrl url(QByteArray("http://localhost:9000") + req->path());
		zr->start(req->method(), url, req->headers());
	}

	void req_readyRead()
	{
		//M2Request *req = (M2Request *)sender();
		QByteArray buf = req->read();
		printf("got chunk: %d\n", buf.size());
		zr->writeBody(buf);
	}

	void req_finished()
	{
		printf("finished\n");
		//M2Request *req = (M2Request *)sender();
		zr->endBody();
		firstRead = true;
	}

	void zr_readyRead()
	{
		printf("zr_readyRead\n");
		M2Response *resp = m2->createResponse(req->rid());
		if(firstRead)
		{
			//HttpHeaders headers;
			//QByteArray str = "body.size=" + QByteArray::number(req->actualContentLength()) + '\n';
			//delete req;
			//headers += HttpHeader("Content-Length", QByteArray::number(str.length()));
			//resp->write(200, "OK", headers, str);
			//delete resp;
			resp->write(zr->responseCode(), zr->responseStatus(), zr->responseHeaders(), zr->readResponseBody());
		}
		else
			resp->write(zr->readResponseBody());

		delete resp;

		if(zr->isFinished())
		{
			delete req;
			delete zr;
		}
	}

	void zr_bytesWritten(int count)
	{
		printf("zr_bytesWritten\n");
	}

	void zr_error()
	{
		printf("zr_error\n");
	}

	void zurl_out_messagesWritten(int count)
	{
		// TODO
		Q_UNUSED(count);
	}

	void zurl_in_readyRead()
	{
		// TODO
	}

	void inspect_req_readyRead()
	{
		// TODO
	}

	void inspect_req_messagesWritten(int count)
	{
		// TODO
		Q_UNUSED(count);
	}

	void retry_in_readyRead()
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

	void accept_out_messagesWritten(int count)
	{
		// TODO
		Q_UNUSED(count);
	}

	void rs_outgoingInspectRequest(const InspectRequestPacket &ireq)
	{
		// TODO
		Q_UNUSED(ireq);

		//RequestSession *rs = (RequestSession *)sender();
		//rs->inspectError();
	}

#if 0
	void rs_inspectFinished(const M2RequestPacket &hreq, bool doProxy, const QByteArray &sharingKey, const InspectResponsePacket *iresp)
	{
		// TODO
		Q_UNUSED(sharingKey);
		Q_UNUSED(iresp);

		if(doProxy)
		{
			ProxySession *ps = new ProxySession(this);
			connect(ps, SIGNAL(outgoingZurlRequest(const ZurlRequestPacket &)), SLOT(ps_outgoingZurlRequest(const ZurlRequestPacket &)));
			connect(ps, SIGNAL(outgoingHttpResponse(const M2ResponsePacket &)), SLOT(ps_outgoingHttpResponse(const M2ResponsePacket &)));
			connect(ps, SIGNAL(finishedForAccept(const InspectResponse &)), SLOT(ps_finishedForAccept(const InspectResponse &)));
			ps->start(hreq);
		}

		/*HttpResponsePacket resp;
		resp.sender = hreq.sender;
		resp.id = hreq.id;
		resp.data = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nok\n";

		m2_out_sock->write(QList<QByteArray>() << resp.toByteArray());
		resp.data = QByteArray();
		m2_out_sock->write(QList<QByteArray>() << resp.toByteArray());*/
	}
#endif

	void ps_outgoingZurlRequest(const ZurlRequestPacket &zreq)
	{
		// TODO
		//zurl_out_sock->write(QList<QByteArray>() << TnetString::fromVariant(zreq.toVariant()));
	}

	void ps_outgoingHttpResponse(const M2ResponsePacket &hresp)
	{
		// TODO
		Q_UNUSED(hresp);
	}

	/*void ps_finishedForAccept(const InspectResponse &iresp)
	{
		// TODO: if instructions were provided, then
		Q_UNUSED(iresp);
	}*/
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
