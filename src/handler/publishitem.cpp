/*
 * Copyright (C) 2016 Fanout, Inc.
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

#include "publishitem.h"

#include "qtcompat.h"
#include "variant.h"
#include "variantutil.h"

using namespace VariantUtil;

PublishItem PublishItem::fromVariant(const Variant &vitem, const QString &channel, bool *ok,
                                     QString *errorMessage) {
    QString pn = "publish item object";

    if (!isKeyedObject(vitem)) {
        setError(ok, errorMessage, QString("%1 is not an object").arg(pn));
        return PublishItem();
    }

    PublishItem item;
    bool ok_;

    if (!channel.isEmpty()) {
        item.channel = channel;
    } else {
        item.channel = getString(vitem, pn, "channel", true, &ok_, errorMessage);
        if (!ok_) {
            if (ok)
                *ok = false;
            return PublishItem();
        }
    }

    item.id = getString(vitem, pn, "id", false, &ok_, errorMessage);
    if (!ok_) {
        if (ok)
            *ok = false;
        return PublishItem();
    }

    item.prevId = getString(vitem, pn, "prev-id", false, &ok_, errorMessage);
    if (!ok_) {
        if (ok)
            *ok = false;
        return PublishItem();
    }

    Variant vformats = getKeyedObject(vitem, pn, "formats", false, &ok_, errorMessage);
    if (!ok_) {
        if (ok)
            *ok = false;
        return PublishItem();
    }

    if (!vformats.isValid()) {
        vformats = createSameKeyedObject(vitem);

        Variant v = keyedObjectGetValue(vitem, "http-response");
        if (v.isValid())
            keyedObjectInsert(&vformats, "http-response", v);

        v = keyedObjectGetValue(vitem, "http-stream");
        if (v.isValid())
            keyedObjectInsert(&vformats, "http-stream", v);

        v = keyedObjectGetValue(vitem, "ws-message");
        if (v.isValid())
            keyedObjectInsert(&vformats, "ws-message", v);
    }

    if (keyedObjectIsEmpty(vformats)) {
        setError(ok, errorMessage, "no formats specified");
        return PublishItem();
    }

    if (keyedObjectContains(vformats, "http-response")) {
        PublishFormat f = PublishFormat::fromVariant(PublishFormat::HttpResponse,
                                                     keyedObjectGetValue(vformats, "http-response"),
                                                     &ok_, errorMessage);
        if (!ok_) {
            if (ok)
                *ok = false;
            return PublishItem();
        }

        item.formats.insert(f.type, f);
    }

    if (keyedObjectContains(vformats, "http-stream")) {
        PublishFormat f = PublishFormat::fromVariant(PublishFormat::HttpStream,
                                                     keyedObjectGetValue(vformats, "http-stream"),
                                                     &ok_, errorMessage);
        if (!ok_) {
            if (ok)
                *ok = false;
            return PublishItem();
        }

        item.formats.insert(f.type, f);
    }

    if (keyedObjectContains(vformats, "ws-message")) {
        PublishFormat f = PublishFormat::fromVariant(PublishFormat::WebSocketMessage,
                                                     keyedObjectGetValue(vformats, "ws-message"),
                                                     &ok_, errorMessage);
        if (!ok_) {
            if (ok)
                *ok = false;
            return PublishItem();
        }

        item.formats.insert(f.type, f);
    }

    Variant vmeta = getKeyedObject(vitem, pn, "meta", false, &ok_, errorMessage);
    if (!ok_) {
        if (ok)
            *ok = false;
        return PublishItem();
    }

    if (vmeta.isValid()) {
        if (typeId(vmeta) == VariantType::Hash) {
            VariantHash hmeta = vmeta.toHash();

            for (auto it = hmeta.constBegin(); it != hmeta.constEnd(); ++it) {
                const QString &key = it.key();
                const Variant &vval = it.value();

                QString val = getString(vval, &ok_);
                if (!ok_) {
                    setError(ok, errorMessage,
                             QString("'meta' contains '%1' with wrong type").arg(key));
                    return PublishItem();
                }

                item.meta[key] = val;
            }
        } else // Map
        {
            VariantMap mmeta = vmeta.toMap();

            for (auto it = mmeta.constBegin(); it != mmeta.constEnd(); ++it) {
                const QString &key = it.key();
                const Variant &vval = it.value();

                QString val = getString(vval, &ok_);
                if (!ok_) {
                    setError(ok, errorMessage,
                             QString("'meta' contains '%1' with wrong type").arg(key));
                    return PublishItem();
                }

                item.meta[key] = val;
            }
        }
    }

    if (keyedObjectContains(vitem, "size")) {
        Variant vsize = keyedObjectGetValue(vitem, "size");
        if (!canConvert(vsize, VariantType::Int)) {
            setError(ok, errorMessage, QString("%1 contains 'size' with wrong type").arg(pn));
            return PublishItem();
        }

        item.size = vsize.toInt();

        if (item.size < 0) {
            setError(ok, errorMessage, QString("%1 contains 'size' with invalid value").arg(pn));
            return PublishItem();
        }
    }

    if (keyedObjectContains(vitem, "no-seq")) {
        Variant vnoSeq = keyedObjectGetValue(vitem, "no-seq");
        if (typeId(vnoSeq) != VariantType::Bool) {
            setError(ok, errorMessage, QString("%1 contains 'no-seq' with wrong type").arg(pn));
            return PublishItem();
        }

        item.noSeq = vnoSeq.toBool();
    }

    setSuccess(ok, errorMessage);
    return item;
}
