#include "encrypt.h"

namespace Encrypt {

QByteArray decryptMessage(const QByteArray &data, const QByteArray &key, Error *error)
{
    if(key.size() != ENCRYPT_KEY_SIZE)
    {
        if(error)
            *error = InvalidInput;
        return QByteArray();
    }

    EncryptBuffer buf;
    int ret = encrypt_decrypt_message((const quint8 *)data.constData(), data.size(), (const quint8 *)key.constData(), &buf);

    if(ret != 0)
    {
        if(error)
        {
            Error e = InvalidInput;

            switch(ret) {
                case 2: e = UnsupportedAlgorithm; break;
                case 3: e = BadFormat; break;
                case 4: e = InvalidData; break;
            }

            *error = e;
        }

        return QByteArray();
    }

    QByteArray out((const char *)buf.data, buf.len);
    encrypt_buffer_deinit(&buf);

    return out;
}

}
