/*
 * Copyright (C) 2013 Fanout, Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <QtTest/QtTest>
#include "jwt.h"

class JwtTest : public QObject
{
	Q_OBJECT

private slots:
	void validToken()
	{
		QVariant vclaim = Jwt::decode("eyJhbGciOiAiSFMyNTYiLCAidHlwIjogIkpXVCJ9.eyJmb28iOiAiYmFyIn0.oBia0Fph39FwQWv0TS7Disg4qa0aFa8qpMaYDrIXZqs", "secret");
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
		QVariant vclaim = Jwt::decode("eyJhbGciOiAiSFMyNTYiLCAidHlwIjogIkpXVCJ9.eyJmb28iOiAiYmFyIn0.-eLxyGEITnd6IP4WvGJx9CmIOt--Qcs3LB6wblJ7KXI", key);
		QVERIFY(vclaim.type() == QVariant::Map);
		QVariantMap claim = vclaim.toMap();
		QVERIFY(claim.value("foo") == "bar");
	}

	void invalidKey()
	{
		QVariant vclaim = Jwt::decode("eyJhbGciOiAiSFMyNTYiLCAidHlwIjogIkpXVCJ9.eyJmb28iOiAiYmFyIn0.oBia0Fph39FwQWv0TS7Disg4qa0aFa8qpMaYDrIXZqs", "wrong");
		QVERIFY(vclaim.isNull());
	}
};

QTEST_MAIN(JwtTest)
#include "jwttest.moc"
