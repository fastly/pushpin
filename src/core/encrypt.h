#ifndef ENCRYPT_H
#define ENCRYPT_H

#include "rust/bindings.h"
#include <QByteArray>

class QString;
class QDir;

namespace Encrypt {

enum Error {
    InvalidInput = ffi::ENCRYPT_ERROR_INVALID_INPUT,
    UnsupportedAlgorithm = ffi::ENCRYPT_ERROR_UNSUPPORTED_ALGORITHM,
    BadFormat = ffi::ENCRYPT_ERROR_BAD_FORMAT,
    InvalidData = ffi::ENCRYPT_ERROR_INVALID_DATA,
};

QByteArray keyFromConfigString(const QString &s, const QDir &baseDir);

// returns decrypted data, null on error
QByteArray decryptMessage(const QByteArray &data, const QByteArray &key, Error *error = 0);

} // namespace Encrypt

#endif
