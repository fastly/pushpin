/*
 * Copyright (C) 2016-2020 Fanout, Inc.
 * Copyright (C) 2024 Fastly, Inc.
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

// NOTE: this is a basic jinja-like template engine. its abilities are
//   minimal, because at the time of this writing our templates aren't very
//   complex. if someday we need more template functionality, we should
//   consider throwing this code away and using a real template library.

#include "template.h"

#include <QVariantMap>
#include <QFile>
#include "qtcompat.h"
#include "log.h"

namespace Template {

class TemplateItem
{
public:
	enum Type
	{
		Root,
		Content,
		Expression,
		If,
		For
	};

	Type type;
	QString data;
	QList<TemplateItem> children;

	TemplateItem() :
		type((Type)-1)
	{
	}

	TemplateItem(Type _type, const QString &_data) :
		type(_type),
		data(_data)
	{
	}
};

enum ControlType
{
	ControlNone,
	ControlIf,
	ControlFor
};

static QList<TemplateItem> parseContent(const QString &content, int *pos, ControlType ctype, QString *error)
{
	QList<TemplateItem> out;
	QString curContent;
	bool closed = false;

	for(int n = *pos; n < content.length(); ++n)
	{
		QChar c = content[n];

		if(n + 1 < content.length() && c == '{' && (content[n + 1] == '{' || content[n + 1] == '%'))
		{
			if(!curContent.isEmpty())
			{
				out += TemplateItem(TemplateItem::Content, curContent);
				curContent.clear();
			}

			++n;

			if(content[n] == '{')
			{
				if(n + 1 >= content.length())
				{
					*error = "EOF reached while parsing directive";
					return QList<TemplateItem>();
				}

				++n;

				int end = content.indexOf("}}", n);
				if(end == -1)
				{
					*error = "no matching }}";
					return QList<TemplateItem>();
				}

				QString s = content.mid(n, end - n).simplified();
				n = end + 2;

				out += TemplateItem(TemplateItem::Expression, s);
			}
			else if(content[n] == '%')
			{
				if(n + 1 >= content.length())
				{
					*error = "EOF reached while parsing directive";
					return QList<TemplateItem>();
				}

				++n;

				int end = content.indexOf("%}", n);
				if(end == -1)
				{
					*error = "no matching %}";
					return QList<TemplateItem>();
				}

				QString s = content.mid(n, end - n).simplified();
				n = end + 2;

				QString stype;
				int at = s.indexOf(' ');
				if(at != -1)
				{
					stype = s.mid(0, at);
					s = s.mid(at + 1);
				}
				else
				{
					stype = s;
					s.clear();
				}

				if(stype == "if" || stype == "for")
				{
					TemplateItem t;
					ControlType ct;
					t.data = s;

					if(stype == "if")
					{
						t.type = TemplateItem::If;
						ct = ControlIf;
					}
					else // for
					{
						t.type = TemplateItem::For;
						ct = ControlFor;
					}

					*pos = n;
					QString error_;
					t.children = parseContent(content, pos, ct, &error_);
					if(!error_.isEmpty())
					{
						*error = error_;
						return QList<TemplateItem>();
					}

					out += t;
					n = *pos;
				}
				else if(stype == "endif" || stype == "endfor")
				{
					ControlType endType;
					if(stype == "endif")
						endType = ControlIf;
					else // for
						endType = ControlFor;

					if(endType != ctype)
					{
						QString expected;
						if(ctype == ControlIf)
							expected = "endif";
						else
							expected = "endfor";

						*error = QString("encountered \"%1\" while expecting \"%2\"").arg(stype, expected);
						return QList<TemplateItem>();
					}

					*pos = n;
					closed = true;
					break;
				}
				else
				{
					// unknown control type
					*error = QString("unknown control directive \"%1\"").arg(stype);
					return QList<TemplateItem>();
				}
			}

			--n; // adjust position
		}
		else
		{
			curContent += c;
		}

		*pos = n;
	}

	if(ctype != ControlNone && !closed)
	{
		QString ctypeStr;
		if(ctype == ControlIf)
			ctypeStr = "if";
		else // ControlFor
			ctypeStr = "for";

		*error = "directive \"%1\" not closed";
		return QList<TemplateItem>();
	}

	if(!curContent.isEmpty())
	{
		out += TemplateItem(TemplateItem::Content, curContent);
		curContent.clear();
	}

	error->clear();
	return out;
}

// handles lookup by exact name or dot-notation for children
static QVariant getVar(const QString &s, const QVariantMap &context)
{
	int at = s.indexOf('.');
	if(at != -1)
	{
		QString parent = s.mid(0, at);
		QString member = s.mid(at + 1);
		if(parent.isEmpty() || !context.contains(parent))
			return QVariant();

		QVariant subContext = context[parent];
		if(typeId(subContext) != QMetaType::QVariantMap)
			return QVariant();

		return getVar(member, subContext.toMap());
	}
	else
	{
		if(!context.contains(s))
			return QVariant();

		return context[s];
	}
}

static QString renderExpression(const QString &exp, const QVariantMap &context)
{
	// for now all we support is variable lookups. no fancy expressions

	QVariant val = getVar(exp, context);
	if(!val.isValid())
		return QString();

	return val.toString();
}

static bool evalCondition(const QString &s, const QVariantMap &context)
{
	// for now all we support is variable test with optional negation

	if(s.startsWith("not "))
	{
		return !evalCondition(s.mid(4), context);
	}
	else
	{
		QVariant val = getVar(s, context);
		if(typeId(val) == QMetaType::QString)
			return !val.toString().isEmpty();
		else if(typeId(val) == QMetaType::Bool)
			return val.toBool();
		else if(canConvert(val, QMetaType::Int))
			return (val.toInt() != 0);
		else
			return false;
	}
}

static QVariantList parseFor(const QString &s, QString *iterVarName, const QVariantMap &context, QString *error)
{
	// for now all we support is "varname in map"

	int at = s.indexOf(" in ");
	if(at == -1)
	{
		*error = "\"for\" directive must be of the form: \"for variable in container\"";
		return QVariantList();
	}

	*iterVarName = s.mid(0, at);
	QString containerName = s.mid(at + 4);

	QVariant container = getVar(containerName, context);
	if(typeId(container) != QMetaType::QVariantList)
	{
		*error = "\"for\" container must be a list";
		return QVariantList();
	}

	return container.toList();
}

static QString renderInternal(const TemplateItem &item, const QVariantMap &context, QString *error)
{
	QString out;

	if(item.type == TemplateItem::Root)
	{
		foreach(const TemplateItem &i, item.children)
		{
			out += renderInternal(i, context, error);
			if(!error->isEmpty())
				return QString();
		}
	}
	else if(item.type == TemplateItem::Content)
	{
		out += item.data;
	}
	else if(item.type == TemplateItem::Expression)
	{
		out += renderExpression(item.data, context);
	}
	else if(item.type == TemplateItem::If)
	{
		if(evalCondition(item.data, context))
		{
			foreach(const TemplateItem &i, item.children)
			{
				out += renderInternal(i, context, error);
				if(!error->isEmpty())
					return QString();
			}
		}
	}
	else if(item.type == TemplateItem::For)
	{
		QString iterVarName;
		QVariantList forItems = parseFor(item.data, &iterVarName, context, error);
		if(!error->isEmpty())
			return QString();

		for(int n = 0; n < forItems.count(); ++n)
		{
			const QVariant &forItem = forItems[n];

			QVariantMap loop;
			loop["first"] = (n == 0);
			loop["last"] = (n == forItems.count() - 1);

			QVariantMap tmp = context;
			tmp[iterVarName] = forItem;
			tmp["loop"] = loop;

			foreach(const TemplateItem &i, item.children)
			{
				out += renderInternal(i, tmp, error);
				if(!error->isEmpty())
					return QString();
			}
		}
	}

	return out;
}

static void dumpItem(const TemplateItem &item, int depth = 0)
{
	for(int n = 0; n < depth; ++n)
		printf(" ");

	if(item.type == TemplateItem::Root)
	{
		printf("root\n");
		foreach(const TemplateItem &i, item.children)
			dumpItem(i, depth + 2);
	}
	else if(item.type == TemplateItem::Content)
	{
		printf("content: [%s]\n", qPrintable(item.data));
	}
	else if(item.type == TemplateItem::Expression)
	{
		printf("expression: [%s]\n", qPrintable(item.data));
	}
	else if(item.type == TemplateItem::If)
	{
		printf("if: [%s]\n", qPrintable(item.data));
		foreach(const TemplateItem &i, item.children)
			dumpItem(i, depth + 2);
	}
	else if(item.type == TemplateItem::For)
	{
		printf("for: [%s]\n", qPrintable(item.data));
		foreach(const TemplateItem &i, item.children)
			dumpItem(i, depth + 2);
	}
}

QString render(const QString &content, const QVariantMap &context, QString *error)
{
	TemplateItem root;
	root.type = TemplateItem::Root;
	int pos = 0;
	QString error_;
	root.children = parseContent(content, &pos, ControlNone, &error_);
	if(!error_.isEmpty())
	{
		if(error)
			*error = error_;
		return QString();
	}

	QString result = renderInternal(root, context, &error_);
	if(!error_.isEmpty())
	{
		if(error)
			*error = error_;
		return QString();
	}

	return result;
}

bool renderFile(const QString &inFile, const QString &outFile, const QVariantMap &context, QString *error)
{
	QFile in(inFile);
	if(!in.open(QFile::ReadOnly | QFile::Text))
	{
		if(error)
			*error = QString("error reading file \"%1\"").arg(inFile);
		return false;
	}

	QString inFileData = QString::fromLocal8Bit(in.readAll());
	in.close();

	QString error_;
	QString outFileData = render(inFileData, context, &error_);
	if(outFileData.isNull())
	{
		if(error)
			*error = QString("error rendering template: %1").arg(error_);
		return false;
	}

	QFile out(outFile);
	if(!out.open(QFile::WriteOnly | QFile::Truncate))
	{
		if(error)
			*error = QString("error writing file \"%1\"").arg(outFile);
		return false;
	}

	int ret = out.write(outFileData.toLocal8Bit());
	if(ret == -1)
	{
		if(error)
			*error = QString("error writing file \"%1\"").arg(outFile);
		return false;
	}

	return true;
}

void dumpTemplate(const QString &content)
{
	TemplateItem root;
	root.type = TemplateItem::Root;
	int pos = 0;
	QString error;
	root.children = parseContent(content, &pos, ControlNone, &error);
	if(!error.isEmpty())
	{
		printf("error parsing template: %s\n", qPrintable(error));
		return;
	}

	dumpItem(root);
}

}
