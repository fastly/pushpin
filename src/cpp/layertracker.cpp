/*
 * Copyright (C) 2014 Fanout, Inc.
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

#include "layertracker.h"

#include <assert.h>

LayerTracker::LayerTracker() :
	plain_(0)
{
}

void LayerTracker::reset()
{
	plain_ = 0;
	items_.clear();
}

void LayerTracker::addPlain(int plain)
{
	plain_ += plain;
}

void LayerTracker::specifyEncoded(int encoded, int plain)
{
	// can't specify more bytes than we have
	assert(plain <= plain_);

	plain_ -= plain;
	Item i;
	i.plain = plain;
	i.encoded = encoded;
	items_ += i;
}

int LayerTracker::finished(int encoded)
{
	int plain = 0;

	for(QList<Item>::Iterator it = items_.begin(); it != items_.end();)
	{
		Item &i = *it;

		// not enough?
		if(encoded < i.encoded)
		{
			i.encoded -= encoded;
			break;
		}

		encoded -= i.encoded;
		plain += i.plain;
		it = items_.erase(it);
	}

	return plain;
}
