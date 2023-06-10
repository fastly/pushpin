/*
 * Copyright (C) 2022-2023 Fanout, Inc.
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

#ifndef STATS_H
#define STATS_H

#include <QtGlobal>
#include <assert.h>
#include <string.h>

#define STATS_COUNTERS_MAX 12

namespace Stats {

// keep in sync with STATS_COUNTERS_MAX above
enum Counter {
    ClientHeaderBytesReceived  =  0,
    ClientHeaderBytesSent      =  1,
    ClientContentBytesReceived =  2,
    ClientContentBytesSent     =  3,
    ClientMessagesReceived     =  4,
    ClientMessagesSent         =  5,
    ServerHeaderBytesReceived  =  6,
    ServerHeaderBytesSent      =  7,
    ServerContentBytesReceived =  8,
    ServerContentBytesSent     =  9,
    ServerMessagesReceived     = 10,
    ServerMessagesSent         = 11,
};

class Counters
{
public:
    Counters()
    {
        reset();
    }

    bool isEmpty() const
    {
        return _empty;
    }

    void reset()
    {
        for(int n = 0; n < STATS_COUNTERS_MAX; ++n)
            _values[n] = 0;

        _empty = true;
    }

    quint32 get(Counter c)
    {
        int index = (int)c;

        assert(index >= 0 && index < STATS_COUNTERS_MAX);

        return _values[index];
    }

    void inc(Counter c, quint32 count = 1)
    {
        int index = (int)c;

        assert(index >= 0 && index < STATS_COUNTERS_MAX);

        _values[index] += count;

        if(count > 0)
            _empty = false;
    }

    void add(const Counters &other);

private:
    quint32 _values[STATS_COUNTERS_MAX];
    bool _empty;
};

}

#endif
