/*
 * Copyright (C) 2022 Fanout, Inc.
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

#ifndef RUST_JWT_H
#define RUST_JWT_H

#include <QtGlobal>

// NOTE: must match values on the rust side
#define JWT_KEYTYPE_SECRET 0
#define JWT_KEYTYPE_EC 1
#define JWT_KEYTYPE_RSA 2
#define JWT_ALGORITHM_HS256 0
#define JWT_ALGORITHM_ES256 1
#define JWT_ALGORITHM_RS256 2

extern "C"
{
	struct JwtEncodingKey
	{
		int type;
		void *key;
	};

	struct JwtDecodingKey
	{
		int type;
		void *key;
	};

	struct JwtBuffer
	{
		quint8 *data;
		size_t len;
	};

	JwtEncodingKey jwt_encoding_key_from_secret(const quint8 *data, size_t len);
	JwtEncodingKey jwt_encoding_key_from_pem(const quint8 *data, size_t len);
	void jwt_encoding_key_destroy(void *key);

	JwtDecodingKey jwt_decoding_key_from_secret(const quint8 *data, size_t len);
	JwtDecodingKey jwt_decoding_key_from_pem(const quint8 *data, size_t len);
	void jwt_decoding_key_destroy(void *key);

	void jwt_str_destroy(char *s);

	int jwt_encode(int alg, const char *claim, const void *key, char **out_token);
	int jwt_decode(int alg, const char *token, const void *key, char **out_claim);
}

#endif
