/*
 * Copyright (C) 2013 Fanout, Inc.
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

#ifndef BUFFERLIST_H
#define BUFFERLIST_H

#include <QList>
#include <QByteArray>

class BufferList
{
public:
	BufferList();

	int size() const { return size_; }
	bool isEmpty() const { return size_ == 0; }

	QByteArray mid(int pos, int size = -1) const;

	void clear();
	void append(const QByteArray &buf);
	QByteArray take(int size = -1);

	QByteArray toByteArray(); // non-const because we rewrite the list

	BufferList & operator+=(const QByteArray &buf)
	{
		append(buf);
		return *this;
	}

private:
	QList<QByteArray> bufs_;
	int size_;
	int offset_;

	void findPos(int pos, int *bufferIndex, int *offset) const;
};

#endif
