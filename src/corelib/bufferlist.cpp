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

#include "bufferlist.h"

#include <assert.h>

BufferList::BufferList() :
	size_(0),
	offset_(0)
{
}

void BufferList::findPos(int pos, int *bufferIndex, int *offset) const
{
	assert(pos < size_);

	int at = 0;
	int curOffset = offset_;

	while(true)
	{
		const QByteArray &buf = bufs_[at];
		if(curOffset + pos < buf.size())
			break;

		++at;
		pos -= (buf.size() - curOffset);
		curOffset = 0;
	}

	*bufferIndex = at;
	*offset = curOffset + pos;
}

QByteArray BufferList::mid(int pos, int size) const
{
	assert(pos >= 0);

	if(size_ == 0 || size == 0 || pos >= size_)
		return QByteArray();

	int toRead;
	if(size > 0)
		toRead = qMin(size, size_ - pos);
	else
		toRead = size_ - pos;

	assert(!bufs_.isEmpty());

	int at;
	int offset;
	findPos(pos, &at, &offset);

	// if we're reading the exact size of the current buffer, cheaply
	//   return it
	if(offset == 0 && bufs_[at].size() == toRead)
		return bufs_[at];

	QByteArray out;
	out.resize(toRead);
	char *outp = out.data();

	while(toRead > 0)
	{
		const QByteArray &buf = bufs_[at];
		int bsize = qMin(buf.size() - offset, toRead);
		memcpy(outp, buf.data() + offset, bsize);

		if(offset + bsize >= buf.size())
		{
			++at;
			offset = 0;
		}

		toRead -= bsize;
		outp += bsize;
	}

	return out;
}

void BufferList::clear()
{
	bufs_.clear();
	size_ = 0;
	offset_ = 0;
}

void BufferList::append(const QByteArray &buf)
{
	if(buf.size() < 1)
		return;

	bufs_ += buf;
	size_ += buf.size();
}

QByteArray BufferList::take(int size)
{
	if(size_ == 0 || size == 0)
		return QByteArray();

	int toRead;
	if(size > 0)
		toRead = qMin(size, size_);
	else
		toRead = size_;

	assert(!bufs_.isEmpty());

	// if we're reading the exact size of the first buffer, cheaply
	//   return it
	if(offset_ == 0 && bufs_.first().size() == toRead)
	{
		size_ -= toRead;
		return bufs_.takeFirst();
	}

	QByteArray out;
	out.resize(toRead);
	char *outp = out.data();

	while(toRead > 0)
	{
		const QByteArray &buf = bufs_.first();
		int bsize = qMin(buf.size() - offset_, toRead);
		memcpy(outp, buf.data() + offset_, bsize);

		if(offset_ + bsize >= buf.size())
		{
			bufs_.removeFirst();
			offset_ = 0;
		}
		else
			offset_ += bsize;

		toRead -= bsize;
		size_ -= bsize;
		outp += bsize;
	}

	return out;
}

QByteArray BufferList::toByteArray()
{
	if(size_ == 0)
		return QByteArray();

	QByteArray out;
	while(!bufs_.isEmpty())
	{
		if(offset_ > 0)
		{
			out += bufs_.first().mid(offset_);
			offset_ = 0;
			bufs_.removeFirst();
		}
		else
			out += bufs_.takeFirst();
	}

	// keep the rewritten buffer as the only buffer
	bufs_ += out;

	return out;
}
