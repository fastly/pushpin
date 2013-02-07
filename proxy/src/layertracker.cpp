/*
 * Copyright (C) 2013 Fan Out Networks, Inc.
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
