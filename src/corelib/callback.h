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

#include <QList>

template <typename T> class Callback
{
public:
    typedef void (*CallbackFunc)(void *data, T);

    Callback()
    {
    }

    void add(CallbackFunc cb, void *data)
    {
        targets_ += QPair<CallbackFunc, void *>(cb, data);
    }

    void removeAll(void *data)
    {
        for(int n = 0; n < targets_.count(); ++n)
        {
            if(targets_[n].second == data)
            {
                targets_.removeAt(n);
                --n; // adjust position
            }
        }
    }

    void call(T value)
    {
        for(int n = 0; n < targets_.count(); ++n)
        {
            targets_[n].first(targets_[n].second, value);
        }
    }

private:
    QList<QPair<CallbackFunc, void *>> targets_;
};

#endif
