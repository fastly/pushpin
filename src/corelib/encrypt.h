#ifndef ENCRYPT_H
#define ENCRYPT_H

#include <QByteArray>
#include "rust/encrypt.h"

class QString;
class QDir;

namespace Encrypt {

enum Error {
    InvalidInput = ENCRYPT_ERROR_INVALID_INPUT,
    UnsupportedAlgorithm = ENCRYPT_ERROR_UNSUPPORTED_ALGORITHM,
    BadFormat = ENCRYPT_ERROR_BAD_FORMAT,
    InvalidData = ENCRYPT_ERROR_INVALID_DATA,
};

QByteArray keyFromConfigString(const QString &s, const QDir &baseDir);

// returns decrypted data, null on error
QByteArray decryptMessage(const QByteArray &data, const QByteArray &key, Error *error = 0);

}

#endif
