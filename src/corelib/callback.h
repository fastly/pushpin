/*
 * Copyright (C) 2023 Fanout, Inc.
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

#ifndef CALLBACK_H
#define CALLBACK_H

#include <assert.h>
#include <QList>

template <typename T> class Callback
{
public:
    typedef void (*CallbackFunc)(void *data, T);

    Callback() :
        activeCalls_(0),
        destroyed_(0)
    {
    }

    ~Callback()
    {
        if(destroyed_)
            *destroyed_ = true;
    }

    void add(CallbackFunc cb, void *data)
    {
        targets_ += Target(cb, data);
    }

    void remove(void *data)
    {
        // mark for removal, but don't actually remove
        for(int n = 0; n < targets_.count(); ++n)
        {
            Target &t = targets_[n];

            if(t.second == data)
            {
                t.second = 0;
            }
        }

        // only actually remove if not in the middle of a call
        if(activeCalls_ == 0)
        {
            removeMarked();
        }
    }

    void call(T value)
    {
        activeCalls_ += 1;

        for(int n = 0; n < targets_.count(); ++n)
        {
            const Target &t = targets_[n];

            // skip if marked for removal
            if(!t.second)
            {
                continue;
            }

            CallbackFunc f = t.first;
            void *data = t.second;

            assert(!destroyed_);

            bool destroyed = false;
            destroyed_ = &destroyed;

            f(data, value);

            if(destroyed)
                return;

            destroyed_ = 0;
        }

        assert(activeCalls_ >= 1);
        activeCalls_ -= 1;

        if(activeCalls_ == 0)
        {
            removeMarked();
        }
    }

private:
    typedef QPair<CallbackFunc, void *> Target;
    QList<Target> targets_;
    bool activeCalls_;
    bool *destroyed_;

    void removeMarked()
    {
        assert(activeCalls_ == 0);

        for(int n = 0; n < targets_.count(); ++n)
        {
            if(!targets_[n].second)
            {
                targets_.removeAt(n);
                --n; // adjust position
            }
        }
    }
};

#endif
