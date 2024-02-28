#ifndef RUST_ENCRYPT_H
#define RUST_ENCRYPT_H

#include <QtGlobal>

// NOTE: must match values on the rust side
#define ENCRYPT_ERROR_INVALID_INPUT 1
#define ENCRYPT_ERROR_UNSUPPORTED_ALGORITHM 2
#define ENCRYPT_ERROR_BAD_FORMAT 3
#define ENCRYPT_ERROR_INVALID_DATA 4

#define ENCRYPT_KEY_SIZE 16

extern "C"
{
	struct EncryptBuffer
	{
		quint8 *data;
		size_t len;
	};

	// key is expected to point to a 16-byte array.
	// out_plain is expected to point to an uninitialized EncryptBuffer, allocated by the caller.
	// if zero is returned, EncryptBuffer will be initialized with the decrypted data
	int encrypt_decrypt_message(const quint8 *data, size_t len, const quint8 *key, EncryptBuffer *out_plain);

	// buf is expected to point to an initialized EncryptBuffer, allocated by the caller
	void encrypt_buffer_deinit(EncryptBuffer *buf);
}

#endif
