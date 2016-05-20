/*
 * Copyright (C) 2016 Fanout, Inc.
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

#include <stdio.h>
#include <assert.h>
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QJsonDocument>
#include <QJsonArray>
#include <QVariantList>
#include "zmq.h"
#include "tnetstring.h"
#include "config.h"

// return true if item modified
static bool convertFromJsonStyleInPlace(QVariant *in)
{
	// Map -> Hash
	// String -> ByteArray (UTF-8)

	bool changed = false;

	int type = in->type();
	if(type == QVariant::Map)
	{
		QVariantHash vhash;
		QVariantMap vmap = in->toMap();
		QMapIterator<QString, QVariant> it(vmap);
		while(it.hasNext())
		{
			it.next();
			QVariant i = it.value();
			convertFromJsonStyleInPlace(&i);
			vhash[it.key()] = i;
		}

		*in = vhash;
		changed = true;
	}
	else if(type == QVariant::List)
	{
		QVariantList vlist = in->toList();
		for(int n = 0; n < vlist.count(); ++n)
		{
			QVariant i = vlist.at(n);
			convertFromJsonStyleInPlace(&i);
			vlist[n] = i;
		}

		*in = vlist;
		changed = true;
	}
	else if(type == QVariant::String)
	{
		*in = QVariant(in->toString().toUtf8());
		changed = true;
	}
	else if(type != QVariant::Bool && type != QVariant::Double && in->canConvert(QVariant::Int))
	{
		*in = in->toInt();
		changed = true;
	}

	return changed;
}

static QVariant convertFromJsonStyle(const QVariant &in)
{
	QVariant v = in;
	convertFromJsonStyleInPlace(&v);
	return v;
}

enum CommandLineParseResult
{
	CommandLineOk,
	CommandLineError,
	CommandLineVersionRequested,
	CommandLineHelpRequested
};

class ArgsData
{
public:
	typedef QPair<QByteArray, QByteArray> Header;

	QString id;
	QString prevId;
	QString sender;
	int code;
	QList<Header> headers;
	bool close;
	bool patch;
	QVariantList bodyPatch;
	QString spec;
	QString channel;
	QString content;

	ArgsData() :
		code(-1),
		close(false),
		patch(false)
	{
	}
};

static CommandLineParseResult parseCommandLine(QCommandLineParser *parser, ArgsData *args, QString *errorMessage)
{
	parser->setSingleDashWordOptionMode(QCommandLineParser::ParseAsLongOptions);
	const QCommandLineOption idOption("id", "Payload ID.", "id");
	parser->addOption(idOption);
	const QCommandLineOption prevIdOption("prev-id", "Previous payload ID.", "id");
	parser->addOption(prevIdOption);
	const QCommandLineOption senderOption("sender", "Sender meta value.", "sender");
	parser->addOption(senderOption);
	const QCommandLineOption codeOption("code", "HTTP response code to use (default: 200).", "code", "200");
	parser->addOption(codeOption);
	const QCommandLineOption headerOption(QStringList() << "H" << "header", "Add HTTP response header.", "\"K: V\"");
	parser->addOption(headerOption);
	const QCommandLineOption closeOption("close", "Close streaming and WebSocket connections.");
	parser->addOption(closeOption);
	const QCommandLineOption patchOption("patch", "Content is JSON patch.");
	parser->addOption(patchOption);
	const QCommandLineOption specOption("spec", "ZeroMQ PUSH spec (default: tcp://localhost:5560).", "spec", "tcp://localhost:5560");
	parser->addOption(specOption);
	parser->addPositionalArgument("channel", "Channel to send to.");
	parser->addPositionalArgument("content", "Content to use for HTTP body and WebSocket message.");
	const QCommandLineOption helpOption = parser->addHelpOption();
	const QCommandLineOption versionOption = parser->addVersionOption();

	if(!parser->parse(QCoreApplication::arguments()))
	{
		*errorMessage = parser->errorText();
		return CommandLineError;
	}

	if(parser->isSet(versionOption))
		return CommandLineVersionRequested;

	if(parser->isSet(helpOption))
		return CommandLineHelpRequested;

	if(parser->isSet(idOption))
		args->id = parser->value(idOption);

	if(parser->isSet(prevIdOption))
		args->prevId = parser->value(prevIdOption);

	if(parser->isSet(senderOption))
		args->sender = parser->value(senderOption);

	{
		bool ok;
		int x = parser->value(codeOption).toInt(&ok);
		if(!ok || x < 0 || x > 999)
		{
			*errorMessage = "error: code must be an integer between 0 and 999.";
			return CommandLineError;
		}

		args->code = x;
	}

	if(parser->isSet(headerOption))
	{
		foreach(const QString &h, parser->values(headerOption))
		{
			int at = h.indexOf(':');
			if(at < 1)
			{
				*errorMessage = "error: header must be in the form \"name: value\".";
				return CommandLineError;
			}

			QByteArray name = h.mid(0, at).toUtf8();
			QByteArray val = h.mid(at + 1).trimmed().toUtf8();
			args->headers += ArgsData::Header(name, val);
		}
	}

	if(parser->isSet(closeOption))
		args->close = true;

	const QStringList positionalArguments = parser->positionalArguments();

	if(parser->isSet(patchOption) && positionalArguments.count() >= 2)
	{
		QJsonParseError e;
		QJsonDocument doc = QJsonDocument::fromJson(parser->positionalArguments()[1].toUtf8(), &e);
		if(e.error != QJsonParseError::NoError || !doc.isArray())
		{
			*errorMessage = "error: failed to parse content as JSON patch";
			return CommandLineError;
		}

		args->patch = true;
		args->bodyPatch = convertFromJsonStyle(doc.array().toVariantList()).toList();
	}

	args->spec = parser->value(specOption);

	if(positionalArguments.isEmpty())
	{
		*errorMessage = "error: must specify channel";
		return CommandLineError;
	}

	args->channel = positionalArguments[0];

	if(positionalArguments.count() >= 2)
		args->content = positionalArguments[1];

	if(!args->close && positionalArguments.count() < 2)
	{
		*errorMessage = "error: must specify content";
		return CommandLineError;
	}

	return CommandLineOk;
}

int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);

	QCoreApplication::setApplicationName("pushpin-publish");
	QCoreApplication::setApplicationVersion(VERSION);

	QCommandLineParser parser;
	parser.setApplicationDescription("Publish messages to Pushpin.");

	ArgsData args;
	QString errorMessage;
	switch(parseCommandLine(&parser, &args, &errorMessage))
	{
		case CommandLineOk:
			break;
		case CommandLineError:
			fprintf(stderr, "%s\n\n%s", qPrintable(errorMessage), qPrintable(parser.helpText()));
			return 1;
		case CommandLineVersionRequested:
			printf("%s %s\n", qPrintable(QCoreApplication::applicationName()),
				qPrintable(QCoreApplication::applicationVersion()));
			return 0;
		case CommandLineHelpRequested:
			parser.showHelp();
			Q_UNREACHABLE();
	}

	QVariantHash formats;

	if(!args.close)
	{
		QVariantHash httpResponse;

		if(args.patch)
			httpResponse["body-patch"] = args.bodyPatch;
		else
			httpResponse["body"] = args.content.toUtf8() + "\n";

		if(args.code != -1)
			httpResponse["code"] = args.code;

		if(!args.headers.isEmpty())
		{
			QVariantList vheaders;
			foreach(const ArgsData::Header &header, args.headers)
				vheaders += QVariant(QVariantList() << header.first << header.second);

			httpResponse["headers"] = vheaders;
		}

		formats["http-response"] = httpResponse;
	}

	if(!args.patch)
	{
		QVariantHash httpStream;
		if(args.close)
			httpStream["action"] = QByteArray("close");
		else
			httpStream["content"] = args.content.toUtf8() + "\n";

		formats["http-stream"] = httpStream;

		QVariantHash wsMessage;
		if(args.close)
			wsMessage["action"] = QByteArray("close");
		else
			wsMessage["content"] = args.content.toUtf8();

		formats["ws-message"] = wsMessage;
	}

	QVariantHash meta;

	if(!args.sender.isEmpty())
		meta["sender"] = args.sender.toUtf8();

	QVariantHash item;

	item["channel"] = args.channel.toUtf8();

	if(!args.id.isEmpty())
		item["id"] = args.id.toUtf8();

	if(!args.prevId.isEmpty())
		item["prev-id"] = args.prevId.toUtf8();

	item["formats"] = formats;

	if(!meta.isEmpty())
		item["meta"] = meta;

	QByteArray message = TnetString::fromVariant(item);

	void *context = zmq_ctx_new();
	void *sock = zmq_socket(context, ZMQ_PUSH);
	int rc = zmq_connect(sock, args.spec.toUtf8());
	assert(rc == 0);

	zmq_send(sock, message.data(), message.size(), 0);

	zmq_close(sock);
	zmq_ctx_destroy(context);

	printf("Published\n");
	return 0;
}
