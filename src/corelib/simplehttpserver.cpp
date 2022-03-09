/*
 * Copyright (C) 2015-2022 Fanout, Inc.
 *
 * This file is part of Pushpin.
 *
 * $FANOUT_BEGIN_LICENSE:AGPL$
 *
 * Pushpin is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * Pushpin is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 * Alternatively, Pushpin may be used under the terms of a commercial license,
 * where the commercial license agreement is provided with the software or
 * contained in a written agreement between you and Fanout. For further
 * information use the contact form at <https://fanout.io/enterprise/>.
 *
 * $FANOUT_END_LICENSE$
 */

#include "simplehttpserver.h"

#include <assert.h>
#include <QFile>
#include <QFileInfo>
#include <QTcpSocket>
#include <QTcpServer>
#include <QLocalSocket>
#include <QLocalServer>
#include "log.h"
#include "httpheaders.h"

class SimpleHttpRequest::Private : public QObject
{
	Q_OBJECT

public:
	enum State
	{
		ReadHeader,
		ReadBody,
		WriteBody,
		WaitForWritten,
		Closing
	};

	SimpleHttpRequest *q;
	QIODevice *sock;
	State state;
	QByteArray inBuf;
	bool version1dot0;
	QString method;
	QByteArray uri;
	HttpHeaders reqHeaders;
	QByteArray reqBody;
	int contentLength;
	int pendingWritten;
	int maxHeadersSize;
	int maxBodySize;

	Private(SimpleHttpRequest *_q, int maxHeadersSize, int maxBodySize) :
		QObject(_q),
		q(_q),
		sock(0),
		state(ReadHeader),
		version1dot0(false),
		contentLength(0),
		pendingWritten(0),
		maxHeadersSize(maxHeadersSize),
		maxBodySize(maxBodySize)
	{
	}

	~Private()
	{
		cleanup();
	}

	void cleanup()
	{
		if(sock)
		{
			sock->disconnect(this);
			sock->setParent(0);
			sock->deleteLater();
			sock = 0;
		}
	}

	void start(QTcpSocket *_sock)
	{
		connect(_sock, &QTcpSocket::readyRead, this, &Private::sock_readyRead);
		connect(_sock, &QTcpSocket::bytesWritten, this, &Private::sock_bytesWritten);
		connect(_sock, &QTcpSocket::disconnected, this, &Private::sock_disconnected);

		sock = _sock;
		sock->setParent(this);

		processIn();
	}

	void start(QLocalSocket *_sock)
	{
		connect(_sock, &QLocalSocket::readyRead, this, &Private::sock_readyRead);
		connect(_sock, &QLocalSocket::bytesWritten, this, &Private::sock_bytesWritten);
		connect(_sock, &QLocalSocket::disconnected, this, &Private::sock_disconnected);

		sock = _sock;
		sock->setParent(this);

		processIn();
	}

	void respond(int code, const QByteArray &reason, const HttpHeaders &headers, const QByteArray &body)
	{
		if(state != WriteBody)
			return;

		HttpHeaders outHeaders = headers;
		outHeaders.removeAll("Connection");
		outHeaders.removeAll("Transfer-Encoding");
		outHeaders.removeAll("Content-Length");

		outHeaders += HttpHeader("Connection", "close");
		outHeaders += HttpHeader("Content-Length", QByteArray::number(body.size()));

		QByteArray respData = "HTTP/";
		if(version1dot0)
			respData += "1.0 ";
		else
			respData += "1.1 ";
		respData += QByteArray::number(code) + " " + reason + "\r\n";
		foreach(const HttpHeader &h, outHeaders)
			respData += h.first + ": " + h.second + "\r\n";
		respData += "\r\n";
		respData += body;

		state = WaitForWritten;
		pendingWritten += respData.size();
		sock->write(respData);
	}

	void respond(int code, const QByteArray &reason, const QString &body)
	{
		HttpHeaders headers;
		headers += HttpHeader("Content-Type", "text/plain");

		respond(code, reason, headers, body.toUtf8());
	}

signals:
	void ready();

private:
	void respondError(int code, const QByteArray &reason, const QString &body)
	{
		state = WriteBody;
		respond(code, reason, body + '\n');
	}

	void respondBadRequest(const QString &body)
	{
		respondError(400, "Bad Request", body);
	}

	void respondLengthRequired(const QString &body)
	{
		respondError(411, "Length Required", body);
	}

	bool processHeaderData(const QByteArray &headerData)
	{
		QList<QByteArray> lines;
		int at = 0;
		while(at < headerData.size())
		{
			int end = headerData.indexOf("\n", at);
			assert(end != -1);

			if(end > at && headerData[end - 1] == '\r')
				lines += headerData.mid(at, end - at - 1);
			else
				lines += headerData.mid(at, end - at);
			at = end + 1;
		}

		if(lines.isEmpty())
			return false;

		QByteArray requestLine = lines[0];

		at = requestLine.indexOf(' ');
		if(at == -1)
			return false;

		method = QString::fromLatin1(requestLine.mid(0, at));
		if(method.isEmpty())
			return false;

		++at;
		int end = requestLine.indexOf(' ', at);
		if(end == -1)
			return false;

		uri = requestLine.mid(at, end - at);

		QByteArray versionStr = requestLine.mid(end + 1);
		if(versionStr == "HTTP/1.0")
			version1dot0 = true;

		for(int n = 1; n < lines.count(); ++n)
		{
			const QByteArray &line = lines[n];
			end = line.indexOf(':');
			if(end == -1)
				continue;

			// skip first space
			at = end + 1;
			if(at < line.length() && line[at] == ' ')
				++at;

			QByteArray name = line.mid(0, end);
			QByteArray val = line.mid(at);

			reqHeaders += HttpHeader(name, val);
		}

		//log_debug("httpserver: IN method=[%s] uri=[%s] 1.1=%s", qPrintable(method), uri.data(), version1dot0 ? "no" : "yes");
		//foreach(const HttpHeader &h, reqHeaders)
		//	log_debug("httpserver:   [%s] [%s]", h.first.data(), h.second.data());
		log_debug("httpserver: IN %s %s", qPrintable(method), uri.data());

		return true;
	}

	void processIn()
	{
		if(state == ReadHeader)
		{
			inBuf += sock->read(maxHeadersSize - inBuf.size());

			// look for double newline
			int at = -1;
			int next = 0;
			for(int n = 0; n < inBuf.size(); ++n)
			{
				if(n + 1 < inBuf.size() && qstrncmp(inBuf.data() + n, "\n\n", 2) == 0)
				{
					at = n + 1;
					next = n + 2;
					break;
				}
				else if(n + 2 < inBuf.size() && qstrncmp(inBuf.data() + n, "\n\r\n", 3) == 0)
				{
					at = n + 1;
					next = n + 3;
					break;
				}
			}

			if(at != -1)
			{
				QByteArray headerData = inBuf.mid(0, at);
				reqBody = inBuf.mid(next);
				inBuf.clear();

				if(!processHeaderData(headerData))
				{
					respondBadRequest("Failed to parse request header.");
					return;
				}

				bool methodAssumesBody = (method != "HEAD" && method != "GET" && method != "DELETE" && method != "OPTIONS");
				if(!reqHeaders.contains("Content-Length") && (reqHeaders.contains("Transfer-Encoding") || methodAssumesBody))
				{
					respondLengthRequired("Request requires Content-Length.");
					return;
				}

				if(reqHeaders.contains("Content-Length"))
				{
					bool ok;
					contentLength = reqHeaders.get("Content-Length").toInt(&ok);
					if(!ok)
					{
						respondBadRequest("Bad Content-Length.");
						return;
					}

					if(contentLength > maxBodySize)
					{
						respondBadRequest("Request body too large.");
						return;
					}

					if(reqHeaders.get("Expect") == "100-continue")
					{
						QByteArray respData = "HTTP/";
						if(version1dot0)
							respData += "1.0 ";
						else
							respData += "1.1 ";
						respData += "100 Continue\r\n\r\n";

						pendingWritten += respData.size();
						sock->write(respData);
					}

					state = ReadBody;
					processIn();
				}
				else
				{
					state = WriteBody;
					emit ready();
				}
			}
			else if(inBuf.size() >= maxHeadersSize)
			{
				inBuf.clear();
				respondBadRequest("Request header too large.");
				return;
			}
		}
		else if(state == ReadBody)
		{
			reqBody += sock->read(maxBodySize - reqBody.size() + 1);

			if(reqBody.size() > contentLength)
			{
				respondBadRequest("Request body exceeded Content-Length.");
				return;
			}

			if(reqBody.size() == contentLength)
			{
				state = WriteBody;
				emit ready();
			}
		}
	}

private slots:
	void sock_readyRead()
	{
		if(state == ReadHeader || state == ReadBody)
			processIn();
	}

	void sock_bytesWritten(qint64 bytes)
	{
		pendingWritten -= (int)bytes;
		assert(pendingWritten >= 0);

		if(state != WaitForWritten)
			return;

		if(pendingWritten == 0)
		{
			state = Closing;
			sock->close();
		}
	}

	void sock_disconnected()
	{
		cleanup();

		emit q->finished();
	}
};

SimpleHttpRequest::SimpleHttpRequest(int maxHeadersSize, int maxBodySize,QObject *parent) :
	QObject(parent)
{
	d = new Private(this, maxHeadersSize, maxBodySize);
}

SimpleHttpRequest::~SimpleHttpRequest()
{
	delete d;
}

QString SimpleHttpRequest::requestMethod() const
{
	return d->method;
}

QByteArray SimpleHttpRequest::requestUri() const
{
	return d->uri;
}

HttpHeaders SimpleHttpRequest::requestHeaders() const
{
	return d->reqHeaders;
}

QByteArray SimpleHttpRequest::requestBody() const
{
	return d->reqBody;
}

void SimpleHttpRequest::respond(int code, const QByteArray &reason, const HttpHeaders &headers, const QByteArray &body)
{
	d->respond(code, reason, headers, body);
}

void SimpleHttpRequest::respond(int code, const QByteArray &reason, const QString &body)
{
	d->respond(code, reason, body);
}

class SimpleHttpServerPrivate : public QObject
{
	Q_OBJECT

public:
	SimpleHttpServer *q;
	void *server;
	bool local;
	QSet<SimpleHttpRequest*> accepting;
	QList<SimpleHttpRequest*> pending;
	int maxHeadersSize;
	int maxBodySize;

	SimpleHttpServerPrivate(int maxHeadersSize, int maxBodySize, SimpleHttpServer *_q) :
		QObject(_q),
		q(_q),
		server(0),
		local(false),
		maxHeadersSize(maxHeadersSize),
		maxBodySize(maxBodySize)
	{
	}

	~SimpleHttpServerPrivate()
	{
		qDeleteAll(pending);
		qDeleteAll(accepting);
	}

	bool listen(const QHostAddress &addr, int port)
	{
		assert(!server);

		QTcpServer *s = new QTcpServer(this);
		connect(s, &QTcpServer::newConnection, this, &SimpleHttpServerPrivate::server_newConnection);
		if(!s->listen(addr, port))
		{
			delete s;

			return false;
		}

		server = s;
		local = false;

		return true;
	}

	bool listenLocal(const QString &name)
	{
		assert(!server);

		QFileInfo fi(name);
		QString filePath = fi.absoluteFilePath();

		QFile::remove(filePath);

		QLocalServer *s = new QLocalServer(this);
		connect(s, &QLocalServer::newConnection, this, &SimpleHttpServerPrivate::server_newConnection);
		if(!s->listen(filePath))
		{
			delete s;

			return false;
		}

		server = s;
		local = true;

		return true;
	}

private slots:
	void server_newConnection()
	{
		if(local)
		{
			QLocalSocket *sock = ((QLocalServer *)server)->nextPendingConnection();
			SimpleHttpRequest *req = new SimpleHttpRequest(maxHeadersSize, maxBodySize);
			connect(req->d, &SimpleHttpRequest::Private::ready, this, &SimpleHttpServerPrivate::req_ready);
			connect(req, &SimpleHttpRequest::finished, this, &SimpleHttpServerPrivate::req_finished);
			accepting += req;
			req->d->start(sock);
		}
		else
		{
			QTcpSocket *sock = ((QTcpServer *)server)->nextPendingConnection();
			SimpleHttpRequest *req = new SimpleHttpRequest(maxHeadersSize, maxBodySize);
			connect(req->d, &SimpleHttpRequest::Private::ready, this, &SimpleHttpServerPrivate::req_ready);
			connect(req, &SimpleHttpRequest::finished, this, &SimpleHttpServerPrivate::req_finished);
			accepting += req;
			req->d->start(sock);
		}
	}

	void req_ready()
	{
		SimpleHttpRequest::Private *reqd = (SimpleHttpRequest::Private *)sender();
		SimpleHttpRequest *req = reqd->q;
		accepting.remove(req);
		pending += req;
		emit q->requestReady();
	}

	void req_finished()
	{
		SimpleHttpRequest *req = (SimpleHttpRequest *)sender();
		accepting.remove(req);
		pending.removeAll(req);
		delete req;
	}
};

SimpleHttpServer::SimpleHttpServer(int maxHeadersSize, int maxBodySize, QObject *parent) :
	QObject(parent)
{
	d = new SimpleHttpServerPrivate(maxHeadersSize, maxBodySize, this);
}

SimpleHttpServer::~SimpleHttpServer()
{
	delete d;
}

bool SimpleHttpServer::listen(const QHostAddress &addr, int port)
{
	return d->listen(addr, port);
}

bool SimpleHttpServer::listenLocal(const QString &name)
{
	return d->listenLocal(name);
}

SimpleHttpRequest *SimpleHttpServer::takeNext()
{
	if(!d->pending.isEmpty())
	{
		SimpleHttpRequest *req = d->pending.takeFirst();
		req->disconnect(d);
		return req;
	}
	else
		return 0;
}

#include "simplehttpserver.moc"
