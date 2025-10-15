/*
 * Copyright (C) 2015-2022 Fanout, Inc.
 * Copyright (C) 2025 Fastly, Inc.
 *
 * This file is part of Pushpin.
 *
 * $FANOUT_BEGIN_LICENSE:APACHE2$
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * $FANOUT_END_LICENSE$
 */

#include "simplehttpserver.h"

#include <assert.h>
#include <QFile>
#include <QFileInfo>
#include "log.h"
#include "defercall.h"
#include "tcplistener.h"
#include "tcpstream.h"
#include "unixlistener.h"
#include "unixstream.h"
#include "httpheaders.h"

class SimpleHttpRequest::Private
{
public:
	enum State
	{
		ReadHeader,
		ReadBody,
		WriteBody,
		WaitForWritten,
		Closed,
		Finished,
	};

	SimpleHttpRequest *q;
	std::unique_ptr<ReadWrite> stream;
	State state;
	QByteArray inBuf;
	QByteArray outBuf;
	bool version1dot0;
	QString method;
	QByteArray uri;
	HttpHeaders reqHeaders;
	QByteArray reqBody;
	int contentLength;
	int headersSizeMax;
	int bodySizeMax;
	DeferCall deferCall;

	Private(SimpleHttpRequest *_q, int headersSizeMax, int bodySizeMax) :
		q(_q),
		state(ReadHeader),
		version1dot0(false),
		contentLength(0),
		headersSizeMax(headersSizeMax),
		bodySizeMax(bodySizeMax)
	{
	}

	~Private()
	{
		cleanup();
	}

	void cleanup()
	{
		stream.reset();
	}

	void start(std::unique_ptr<ReadWrite> _stream)
	{
		stream = std::move(_stream);

		stream->readReady.connect(boost::bind(&Private::stream_readReady, this));
		stream->writeReady.connect(boost::bind(&Private::stream_writeReady, this));

		deferCall.defer([&] { process(); });
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
			respData += (h.first + ": " + h.second + "\r\n").asQByteArray();
		respData += "\r\n";
		respData += body;

		state = WaitForWritten;

		outBuf += respData;

		deferCall.defer([&] { process(); });
	}

	void respond(int code, const QByteArray &reason, const QString &body)
	{
		HttpHeaders headers;
		headers += HttpHeader("Content-Type", "text/plain");

		respond(code, reason, headers, body.toUtf8());
	}

	Signal ready;

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

	void error(const QString &msg)
	{
		log_debug("httpserver error: %s", qPrintable(msg));

		doFinish();
	}

	void doFinish()
	{
		cleanup();
		state = Closed;
	}

	// return false if more I/O needed to make progress
	bool step()
	{
		if(state == ReadHeader)
		{
			QByteArray buf = stream->read(headersSizeMax - inBuf.size());

			if(buf.isNull())
			{
				int e = stream->errorCondition();
				if(e == EAGAIN)
					return false;

				error(QString("read error: %1").arg(e));
				return true;
			}

			if(buf.isEmpty())
			{
				error("client closed unexpectedly");
				return true;
			}

			inBuf += buf;

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
					return true;
				}

				bool methodAssumesBody = (method != "HEAD" && method != "GET" && method != "DELETE" && method != "OPTIONS");
				if(!reqHeaders.contains("Content-Length") && (reqHeaders.contains("Transfer-Encoding") || methodAssumesBody))
				{
					respondLengthRequired("Request requires Content-Length.");
					return true;
				}

				if(reqHeaders.contains("Content-Length"))
				{
					bool ok;
					contentLength = reqHeaders.get("Content-Length").asQByteArray().toInt(&ok);
					if(!ok)
					{
						respondBadRequest("Bad Content-Length.");
						return true;
					}

					if(contentLength > bodySizeMax)
					{
						respondBadRequest("Request body too large.");
						return true;
					}

					if(reqHeaders.get("Expect") == "100-continue")
					{
						QByteArray respData = "HTTP/";
						if(version1dot0)
							respData += "1.0 ";
						else
							respData += "1.1 ";
						respData += "100 Continue\r\n\r\n";

						outBuf += respData;
					}

					state = ReadBody;
					return true;
				}
				else
				{
					state = WriteBody;
					ready();
				}
			}
			else if(inBuf.size() >= headersSizeMax)
			{
				inBuf.clear();
				respondBadRequest("Request header too large.");
				return true;
			}
		}
		else if(state == ReadBody)
		{
			// write 100 continue
			if(!outBuf.isEmpty())
			{
				int ret = stream->write(outBuf);

				if(ret < 0)
				{
					int e = stream->errorCondition();
					if(e == EAGAIN)
						return false;

					error(QString("write error: %1").arg(e));
					return true;
				}

				outBuf = outBuf.mid(ret);
				return true;
			}

			if(reqBody.size() < contentLength)
			{
				QByteArray buf = stream->read(bodySizeMax - reqBody.size() + 1);

				if(buf.isNull())
				{
					int e = stream->errorCondition();
					if(e == EAGAIN)
						return false;

					error(QString("read error: %1").arg(e));
					return true;
				}

				if(buf.isEmpty())
				{
					error("client closed unexpectedly");
					return true;
				}

				reqBody += buf;
			}

			if(reqBody.size() > contentLength)
			{
				respondBadRequest("Request body exceeded Content-Length.");
				return true;
			}

			if(reqBody.size() == contentLength)
			{
				state = WriteBody;
				ready();
			}
		}
		else if(state == WaitForWritten)
		{
			if(outBuf.isEmpty())
			{
				doFinish();
				return true;
			}

			int ret = stream->write(outBuf);

			if(ret < 0)
			{
				int e = stream->errorCondition();
				if(e == EAGAIN)
					return false;

				error(QString("write error: %1").arg(e));
				return true;
			}

			outBuf = outBuf.mid(ret);
		}

		return true;
	}

	void process()
	{
		while((state == ReadHeader || state == ReadBody || state == WaitForWritten) && step()) {}

		if(state == Closed)
		{
			state = Finished;
			q->finished();
		}
	}

	void stream_readReady()
	{
		process();
	}

	void stream_writeReady()
	{
		process();
	}
};

SimpleHttpRequest::SimpleHttpRequest(int headersSizeMax, int bodySizeMax)
{
	d = new Private(this, headersSizeMax, bodySizeMax);
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

class SimpleHttpServerPrivate
{
public:
	SimpleHttpServer *q;
	void *listener;
	bool local;
	QSet<SimpleHttpRequest*> accepting;
	QList<SimpleHttpRequest*> pending;
	QSet<SimpleHttpRequest*> active;
	int connectionsMax;
	int headersSizeMax;
	int bodySizeMax;
	map<SimpleHttpRequest*, Connection> finishedConnections;
	map<SimpleHttpRequest*, Connection> readyConnections;
	DeferCall deferCall;

	SimpleHttpServerPrivate(int connectionsMax, int headersSizeMax, int bodySizeMax, SimpleHttpServer *_q) :
		q(_q),
		listener(nullptr),
		local(false),
		connectionsMax(connectionsMax),
		headersSizeMax(headersSizeMax),
		bodySizeMax(bodySizeMax)
	{
	}

	~SimpleHttpServerPrivate()
	{
		qDeleteAll(pending);
		qDeleteAll(accepting);

		if(listener)
		{
			if(local)
				delete ((UnixListener *)listener);
			else
				delete ((TcpListener *)listener);
		}
	}

	bool canAccept() const
	{
		return (accepting.count() + pending.count() + active.count() < connectionsMax);
	}

	bool listen(const QHostAddress &addr, int port)
	{
		assert(!listener);

		TcpListener *l = new TcpListener;
		l->streamsReady.connect(boost::bind(&SimpleHttpServerPrivate::listener_streamsReady, this));
		if(!l->bind(addr, port))
		{
			delete l;

			return false;
		}

		listener = l;
		local = false;

		deferCall.defer([&] { listener_streamsReady(); });

		return true;
	}

	bool listenLocal(const QString &name)
	{
		assert(!listener);

		QFileInfo fi(name);
		QString filePath = fi.absoluteFilePath();

		QFile::remove(filePath);

		UnixListener *l = new UnixListener;
		l->streamsReady.connect(boost::bind(&SimpleHttpServerPrivate::listener_streamsReady, this));
		if(!l->bind(name))
		{
			delete l;

			return false;
		}

		listener = l;
		local = true;

		deferCall.defer([&] { listener_streamsReady(); });

		return true;
	}

	void listener_streamsReady()
	{
		while(canAccept())
		{
			std::unique_ptr<ReadWrite> s;

			if(local)
			{
				UnixListener *l = (UnixListener *)listener;
				s = l->accept();
				if(!s && l->errorCondition() == EAGAIN)
					break;
			}
			else
			{
				TcpListener *l = (TcpListener *)listener;
				s = l->accept();
				if(!s && l->errorCondition() == EAGAIN)
					break;
			}

			if(s)
			{
				SimpleHttpRequest *req = new SimpleHttpRequest(headersSizeMax, bodySizeMax);
				readyConnections[req] = req->d->ready.connect(boost::bind(&SimpleHttpServerPrivate::req_ready, this, req->d->q));
				finishedConnections[req] = req->finished.connect(boost::bind(&SimpleHttpServerPrivate::req_finished, this, req));
				accepting += req;
				req->d->start(std::move(s));
			}
		}
	}

	void req_ready(SimpleHttpRequest *req)
	{
		accepting.remove(req);
		pending += req;

		q->requestReady();
	}

	void req_finished(SimpleHttpRequest *req)
	{
		bool del = false;

		if(active.contains(req))
		{
			active.remove(req);
		}
		else
		{
			pending.removeAll(req);
			accepting.remove(req);
			del = true;
		}

		readyConnections.erase(req);
		finishedConnections.erase(req);

		if(del)
			delete req;

		// try to accept more
		listener_streamsReady();
	}
};

SimpleHttpServer::SimpleHttpServer(int connectionsMax, int headersSizeMax, int bodySizeMax)
{
	d = new SimpleHttpServerPrivate(connectionsMax, headersSizeMax, bodySizeMax, this);
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
		d->active += req;

		return req;
	}
	else
		return nullptr;
}
