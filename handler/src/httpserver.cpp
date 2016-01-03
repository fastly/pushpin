/*
 * Copyright (C) 2015 Fanout, Inc.
 *
 * This file is part of Pushpin.
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
 */

#include "httpserver.h"

#include <assert.h>
#include <QTcpSocket>
#include <QTcpServer>
#include "log.h"
#include "httpheaders.h"

#define MAX_HEADERS_SIZE 10000
#define MAX_BODY_SIZE 1000000

class HttpRequest::Private : public QObject
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

	HttpRequest *q;
	QTcpSocket *sock;
	State state;
	QByteArray inBuf;
	bool version1dot0;
	QString method;
	QByteArray uri;
	HttpHeaders reqHeaders;
	QByteArray reqBody;
	int contentLength;
	int pendingWritten;

	Private(HttpRequest *_q) :
		QObject(_q),
		q(_q),
		sock(0),
		state(ReadHeader),
		version1dot0(false),
		contentLength(0),
		pendingWritten(0)
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
		sock = _sock;
		sock->setParent(this);
		connect(sock, SIGNAL(readyRead()), SLOT(sock_readyRead()));
		connect(sock, SIGNAL(bytesWritten(qint64)), SLOT(sock_bytesWritten(qint64)));
		connect(sock, SIGNAL(disconnected()), SLOT(sock_disconnected()));
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
		emit q->finished();
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
		QByteArray buf = sock->readAll();

		if(state == ReadHeader)
		{
			if(inBuf.size() + buf.size() > MAX_HEADERS_SIZE)
			{
				inBuf.clear();
				respondBadRequest("Request header too large.");
				return;
			}

			inBuf += buf;

			// look for double newline
			int at = -1;
			int next;
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

				bool methodAssumesBody = (method != "HEAD" && method != "GET" && method != "DELETE");
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

					if(contentLength > MAX_BODY_SIZE)
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
		}
		else if(state == ReadBody)
		{
			reqBody += buf;

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

HttpRequest::HttpRequest(QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
}

HttpRequest::~HttpRequest()
{
	delete d;
}

QString HttpRequest::requestMethod() const
{
	return d->method;
}

QByteArray HttpRequest::requestUri() const
{
	return d->uri;
}

HttpHeaders HttpRequest::requestHeaders() const
{
	return d->reqHeaders;
}

QByteArray HttpRequest::requestBody() const
{
	return d->reqBody;
}

void HttpRequest::respond(int code, const QByteArray &reason, const HttpHeaders &headers, const QByteArray &body)
{
	d->respond(code, reason, headers, body);
}

void HttpRequest::respond(int code, const QByteArray &reason, const QString &body)
{
	d->respond(code, reason, body);
}

class HttpServerPrivate : public QObject
{
	Q_OBJECT

public:
	HttpServer *q;
	QTcpServer *server;
	QSet<HttpRequest*> accepting;
	QList<HttpRequest*> pending;

	HttpServerPrivate(HttpServer *_q) :
		QObject(_q),
		q(_q),
		server(0)
	{
	}

	~HttpServerPrivate()
	{
		qDeleteAll(pending);
		qDeleteAll(accepting);
	}

	bool listen(const QHostAddress &addr, int port)
	{
		server = new QTcpServer(this);
		connect(server, SIGNAL(newConnection()), SLOT(server_newConnection()));
		if(!server->listen(addr, port))
		{
			delete server;
			server = 0;
		}

		return true;
	}

private slots:
	void server_newConnection()
	{
		QTcpSocket *sock = server->nextPendingConnection();
		HttpRequest *req = new HttpRequest;
		connect(req->d, SIGNAL(ready()), SLOT(req_ready()));
		connect(req, SIGNAL(finished()), SLOT(req_finished()));
		accepting += req;
		req->d->start(sock);
	}

	void req_ready()
	{
		HttpRequest::Private *reqd = (HttpRequest::Private *)sender();
		HttpRequest *req = reqd->q;
		accepting.remove(req);
		pending += req;
		emit q->requestReady();
	}

	void req_finished()
	{
		HttpRequest *req = (HttpRequest *)sender();
		accepting.remove(req);
		pending.removeAll(req);
		delete req;
	}
};

HttpServer::HttpServer(QObject *parent) :
	QObject(parent)
{
	d = new HttpServerPrivate(this);
}

HttpServer::~HttpServer()
{
	delete d;
}

bool HttpServer::listen(const QHostAddress &addr, int port)
{
	return d->listen(addr, port);
}

HttpRequest *HttpServer::takeNext()
{
	if(!d->pending.isEmpty())
	{
		HttpRequest *req = d->pending.takeFirst();
		req->disconnect(d);
		return req;
	}
	else
		return 0;
}

#include "httpserver.moc"
