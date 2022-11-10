/*
 * Copyright (C) 2013-2022 Fanout, Inc.
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

#include <QtTest/QtTest>
#include <QJsonDocument>
#include <QJsonObject>
#include "jwt.h"

static const char *test_ec_private_key_pem =
	"-----BEGIN PRIVATE KEY-----\n"
	"MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgFcZQVV16cpGC4QUQ\n"
	"8O8H85totFiAB54WBTxKQQElI7KhRANCAAQA3D4/QkBACQuC99MFqZllTOaamPAJ\n"
	"3+Z3JkPsrd/z651PYmlywcdEGVWRiD2PNhvdzM7Nckxx1ZofDLlkvoxH\n"
	"-----END PRIVATE KEY-----\n";

static const char *test_ec_public_key_pem =
	"-----BEGIN PUBLIC KEY-----\n"
	"MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEANw+P0JAQAkLgvfTBamZZUzmmpjw\n"
	"Cd/mdyZD7K3f8+udT2JpcsHHRBlVkYg9jzYb3czOzXJMcdWaHwy5ZL6MRw==\n"
	"-----END PUBLIC KEY-----\n";

static const char *test_rsa_private_key_pem =
	"-----BEGIN PRIVATE KEY-----\n"
	"MIIEvgIBADANBgkqhkiG9w0BAQEFAASCBKgwggSkAgEAAoIBAQDOgE+exziD5kFF\n"
	"4x3F76G64XAccp1KqWMgrTYM3c4C/7hxwuu7kMdGXlXL+xQOHe6vX6EM/H9tWaIf\n"
	"CyQ+KfdyBBDO05MXZcxEl3496ShN/UN1TghJk12gg3yPm3+V2mfh+NQi7jEFt1uv\n"
	"beco5T1ve5yhtu58PrCC87TuWINW+iFrUg41MEHcXWL/7COBR/azFOZqedZPCdnL\n"
	"5SoY1H5WAazZUftD6W7PvCYmQN+uCSr1SjbGg5g+9OQ6i7viHXRg0U9mIZVII56V\n"
	"g2sD0w6ClO4Tq+mQs94frKakD960drvg2QNCvW0cUiRBLkadOSqZkIp0It4r5ivi\n"
	"S2gJQO8XAgMBAAECggEBAJs4W6DwAw0yULIlq8WTALCmsEzR4mWyuW5ghJZbS3V5\n"
	"nrz0VZmhlAjS9A7l5gdOfJGagkZuraIWlARdrZqElRlA8Rlmc9RMkqSkcyI6Vi95\n"
	"RfGw/A3CFciHzWNs8RRFHX0AOwUeof63+tT8+ZsF5Y4dDnmINe9yd9+XLNNT+TWw\n"
	"aCFJ+RQ8j7xGtZb2N/AOI0prTCka/SNRYxNommdS1x9qCaTVKd1fXM/ZhRjIlsEo\n"
	"OzmcoG0Kdfq6pu2OgJ8DzSigXyWbCEy/amSWgPX80kubG1Xjc8MSFlQcg493Gve1\n"
	"JagUZEbKIQFNCxN42cAzuuf3hKV9vIT+L8yApuacwQECgYEA+Lgp8UtANVFOBSuE\n"
	"5HHP+dB3Ot8HdbK2FIQEQ+xwVUHgLnnWpQHhw8COpZgAoMGMPl37KGrTuPW/C/4o\n"
	"yGj/hK+df1ksLR8ViXFVpB5GbzfdsvMgPo1GCYVFVGJlVHO/oFxV6YQtydhiAMp+\n"
	"dcgQO3paKrzEoFSJdomNtoqMdUECgYEA1IvGiaiwk5yPafs2mbsoMM7K6NpzlO3x\n"
	"pPlTqgGgVgIM+Lg6FWEm3kWN6A/hELyfCIosHP5pdkPKxgkzs6OqVFxKa2anHSRT\n"
	"1lLUhU0kOrkYyfq1oMXumPb5Kc4zzbOnxScF7lCIzMo9y82OJSjHDbjAgmzNyJbm\n"
	"CEhOgf2RllcCgYAfyqKJ1j2R0x+u534oGSglXXEwFDwG3l4Jx0ooSHufWjlGl4pJ\n"
	"MzFhbSaOohxKcBL2Eds9slH3zWmrJcSewVUP58aw9XwBFH0TQWpZ/QixxKlQ62TO\n"
	"ug4ev2s6Ow2KuvTekY7lt2CG8WKtiTSa54SzpZMK7XAQsl2TykdT8ue7QQKBgGrG\n"
	"KR/gkYwmG1m3bK9/+OnECOU/UM8hVcJ1ylTeakiq0Q9lpTA2VQtWT7qjt4Hr78yf\n"
	"dRe/qwVRex1PZBy7fIbSskQQFqWqKT/C7qZkoW2qrMxS2UmCBaHseDFLOHT+6qo9\n"
	"N1qINKEEfFTU17LNMGoxROyAckRxoe/JOz9MPgYTAoGBAJKreX73d6s1s9oVB3u/\n"
	"DS1YXRmek+OkXQhFxekKXB3KxG8obx2uveeg18PtNf0RoYq9LF0hKcTqSCusfF9m\n"
	"lM+s5xc1mQfXI55AEOjT+8AssmhebHbFkpjr1/DSUUsCssO+1znkeZwAOApm/4kR\n"
	"pGokHI67k9CxNFZW3Z0U9EeW\n"
	"-----END PRIVATE KEY-----\n";

static const char *test_rsa_public_key_pem =
	"-----BEGIN PUBLIC KEY-----\n"
	"MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAzoBPnsc4g+ZBReMdxe+h\n"
	"uuFwHHKdSqljIK02DN3OAv+4ccLru5DHRl5Vy/sUDh3ur1+hDPx/bVmiHwskPin3\n"
	"cgQQztOTF2XMRJd+PekoTf1DdU4ISZNdoIN8j5t/ldpn4fjUIu4xBbdbr23nKOU9\n"
	"b3ucobbufD6wgvO07liDVvoha1IONTBB3F1i/+wjgUf2sxTmannWTwnZy+UqGNR+\n"
	"VgGs2VH7Q+luz7wmJkDfrgkq9Uo2xoOYPvTkOou74h10YNFPZiGVSCOelYNrA9MO\n"
	"gpTuE6vpkLPeH6ympA/etHa74NkDQr1tHFIkQS5GnTkqmZCKdCLeK+Yr4ktoCUDv\n"
	"FwIDAQAB\n"
	"-----END PUBLIC KEY-----\n";

class JwtTest : public QObject
{
	Q_OBJECT

private slots:
	void validToken()
	{
		QVariant vclaim = Jwt::decode("eyJhbGciOiAiSFMyNTYiLCAidHlwIjogIkpXVCJ9.eyJmb28iOiAiYmFyIn0.oBia0Fph39FwQWv0TS7Disg4qa0aFa8qpMaYDrIXZqs", Jwt::DecodingKey::fromSecret("secret"));
		QVERIFY(vclaim.type() == QVariant::Map);
		QVariantMap claim = vclaim.toMap();
		QVERIFY(claim.value("foo") == "bar");
	}

	void validTokenBinaryKey()
	{
		QByteArray key;
		key += 0x01;
		key += 0x61;
		key += 0x80;
		key += 0xfe;
		QVariant vclaim = Jwt::decode("eyJhbGciOiAiSFMyNTYiLCAidHlwIjogIkpXVCJ9.eyJmb28iOiAiYmFyIn0.-eLxyGEITnd6IP4WvGJx9CmIOt--Qcs3LB6wblJ7KXI", Jwt::DecodingKey::fromSecret(key));
		QVERIFY(vclaim.type() == QVariant::Map);
		QVariantMap claim = vclaim.toMap();
		QVERIFY(claim.value("foo") == "bar");
	}

	void invalidKey()
	{
		QVariant vclaim = Jwt::decode("eyJhbGciOiAiSFMyNTYiLCAidHlwIjogIkpXVCJ9.eyJmb28iOiAiYmFyIn0.oBia0Fph39FwQWv0TS7Disg4qa0aFa8qpMaYDrIXZqs", Jwt::DecodingKey::fromSecret("wrong"));
		QVERIFY(vclaim.isNull());
	}

	void es256EncodeDecode()
	{
		Jwt::EncodingKey privateKey = Jwt::EncodingKey::fromPem(QByteArray(test_ec_private_key_pem));
		QVERIFY(!privateKey.isNull());
		QCOMPARE(privateKey.type(), Jwt::KeyType::Ec);

		Jwt::DecodingKey publicKey = Jwt::DecodingKey::fromPem(QByteArray(test_ec_public_key_pem));
		QVERIFY(!publicKey.isNull());
		QCOMPARE(publicKey.type(), Jwt::KeyType::Ec);

		QVariantMap claim;
		claim["iss"] = "nobody";

		QByteArray claimJson = QJsonDocument(QJsonObject::fromVariantMap(claim)).toJson(QJsonDocument::Compact);
		QVERIFY(!claimJson.isNull());

		QByteArray token = Jwt::encodeWithAlgorithm(Jwt::ES256, claimJson, privateKey);
		QVERIFY(!token.isNull());

		QByteArray resultJson = Jwt::decodeWithAlgorithm(Jwt::ES256, token, publicKey);
		QVERIFY(!resultJson.isNull());

		QJsonParseError error;
		QJsonDocument doc = QJsonDocument::fromJson(resultJson, &error);
		QVERIFY(error.error == QJsonParseError::NoError);
		QVERIFY(doc.isObject());

		QVariantMap result = doc.object().toVariantMap();
		QCOMPARE(result["iss"], "nobody");
	}

	void rs256EncodeDecode()
	{
		Jwt::EncodingKey privateKey = Jwt::EncodingKey::fromPem(QByteArray(test_rsa_private_key_pem));
		QVERIFY(!privateKey.isNull());
		QCOMPARE(privateKey.type(), Jwt::KeyType::Rsa);

		Jwt::DecodingKey publicKey = Jwt::DecodingKey::fromPem(QByteArray(test_rsa_public_key_pem));
		QVERIFY(!publicKey.isNull());
		QCOMPARE(publicKey.type(), Jwt::KeyType::Rsa);

		QVariantMap claim;
		claim["iss"] = "nobody";

		QByteArray claimJson = QJsonDocument(QJsonObject::fromVariantMap(claim)).toJson(QJsonDocument::Compact);
		QVERIFY(!claimJson.isNull());

		QByteArray token = Jwt::encodeWithAlgorithm(Jwt::RS256, claimJson, privateKey);
		QVERIFY(!token.isNull());

		QByteArray resultJson = Jwt::decodeWithAlgorithm(Jwt::RS256, token, publicKey);
		QVERIFY(!resultJson.isNull());

		QJsonParseError error;
		QJsonDocument doc = QJsonDocument::fromJson(resultJson, &error);
		QVERIFY(error.error == QJsonParseError::NoError);
		QVERIFY(doc.isObject());

		QVariantMap result = doc.object().toVariantMap();
		QCOMPARE(result["iss"], "nobody");
	}
};

QTEST_MAIN(JwtTest)
#include "jwttest.moc"
