#include "encrypt.h"

#include <QString>
#include <QDir>
#include "rust/bindings.h"

namespace Encrypt {

QByteArray keyFromConfigString(const QString &s, const QDir &baseDir)
{
    if(s.startsWith("file:"))
    {
        QString keyFile = s.mid(5);
        QFileInfo fi(keyFile);
        if(fi.isRelative())
            keyFile = QFileInfo(baseDir, keyFile).filePath();

        QFile f(keyFile);
        if(!f.open(QFile::ReadOnly))
            return QByteArray();

        QByteArray data = f.readAll().trimmed();

        return QByteArray::fromHex(data);
    }
    else
    {
        return QByteArray::fromHex(s.toUtf8());
    }
}

QByteArray decryptMessage(const QByteArray &data, const QByteArray &key, Error *error)
{
    if(key.size() != ENCRYPT_KEY_SIZE)
    {
        if(error)
            *error = InvalidInput;
        return QByteArray();
    }

    ffi::EncryptBuffer buf;
    int ret = ffi::encrypt_decrypt_message((const quint8 *)data.constData(), data.size(), (const quint8 *)key.constData(), &buf);

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
    ffi::encrypt_buffer_deinit(&buf);

    return out;
}

}
