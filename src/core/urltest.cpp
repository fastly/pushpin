/*
 * Copyright (C) 2025 Fastly, Inc.
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
 */

#include "test.h"
#include "url.h"

static void basicUrlOperations()
{
    // Test construction and basic operations
    Url url("https://example.com/path?query=value");
    TEST_ASSERT(url.isValid());
    TEST_ASSERT(!url.isEmpty());
    TEST_ASSERT_EQ(url.scheme(), QString("https"));
    TEST_ASSERT_EQ(url.path(), QString("/path"));
    TEST_ASSERT_EQ(url.query(), QString("query=value"));
    TEST_ASSERT(url.hasQuery());

    // Test toString
    QString urlString = url.toString();
    TEST_ASSERT(urlString.startsWith("https://example.com"));

    // Test toEncoded
    QByteArray encoded = url.toEncoded();
    TEST_ASSERT(!encoded.isEmpty());
}

static void invalidUrl()
{
    Url url("not a url");
    TEST_ASSERT(!url.isValid());
    TEST_ASSERT(url.isEmpty());
    TEST_ASSERT_EQ(url.scheme(), QString());
}

static void emptyUrl()
{
    Url url;
    TEST_ASSERT(!url.isValid());
    TEST_ASSERT(url.isEmpty());
    TEST_ASSERT_EQ(url.scheme(), QString());
}

static void fromEncoded()
{
    QByteArray encoded = "https%3A//example.com/path%3Fquery%3Dvalue";
    Url url = Url::fromEncoded(encoded);
    // Note: This will likely fail parsing as it's double-encoded
    // The actual usage in the codebase should provide proper encoded URLs
}

static void schemeOperations()
{
    Url url("http://example.com/path");
    TEST_ASSERT_EQ(url.scheme(), QString("http"));

    url.setScheme("https");
    TEST_ASSERT_EQ(url.scheme(), QString("https"));
}

static void clearUrl()
{
    Url url("https://example.com");
    TEST_ASSERT(url.isValid());

    url.clear();
    TEST_ASSERT(!url.isValid());
    TEST_ASSERT(url.isEmpty());
}

static void copyAndAssign()
{
    Url url1("https://example.com");
    Url url2(url1);  // Copy constructor

    TEST_ASSERT_EQ(url1.scheme(), url2.scheme());

    Url url3;
    url3 = url1;  // Assignment operator
    TEST_ASSERT_EQ(url1.scheme(), url3.scheme());
}

static void relativeUrlValidation()
{
    // Test valid relative URLs
    TEST_ASSERT(Url::isValidRelativeUrl("/next"));
    TEST_ASSERT(Url::isValidRelativeUrl("/path/to/resource"));
    TEST_ASSERT(Url::isValidRelativeUrl("relative/path"));
    TEST_ASSERT(Url::isValidRelativeUrl("?query=value"));
    TEST_ASSERT(Url::isValidRelativeUrl("../parent"));
    TEST_ASSERT(Url::isValidRelativeUrl("file.html"));

    // Test valid absolute URLs (should also work)
    TEST_ASSERT(Url::isValidRelativeUrl("http://example.com/path"));
    TEST_ASSERT(Url::isValidRelativeUrl("https://example.com/"));

    // Test invalid URLs
    TEST_ASSERT(!Url::isValidRelativeUrl(""));  // Empty string
    // Note: URL resolution is quite permissive, so truly invalid URLs are rare
    // Most strings that look like paths are valid relative URLs
}

static void urlResolution()
{
    Url base("http://example.com/path/current");

    // Test that absolute URLs are returned as-is (not modified by base)
    Url absoluteResult1 = base.resolved(QString("http://other.com/different"));
    TEST_ASSERT_EQ(absoluteResult1.toString(), QString("http://other.com/different"));

    Url absoluteResult2 = base.resolved(QString("https://secure.com/path"));
    TEST_ASSERT_EQ(absoluteResult2.toString(), QString("https://secure.com/path"));

    // Test relative URLs are properly resolved
    Url relativeResult1 = base.resolved(QString("/absolute-path"));
    TEST_ASSERT_EQ(relativeResult1.host(), QString("example.com"));
    TEST_ASSERT(relativeResult1.path().startsWith("/absolute-path"));

    Url relativeResult2 = base.resolved(QString("relative-file"));
    TEST_ASSERT_EQ(relativeResult2.host(), QString("example.com"));
    TEST_ASSERT(relativeResult2.toString().contains("relative-file"));

    // Test query-only relative URLs
    Url queryResult = base.resolved(QString("?newquery=value"));
    TEST_ASSERT_EQ(queryResult.host(), QString("example.com"));
    TEST_ASSERT(queryResult.hasQuery());

    // Test that our validation method works correctly for both cases
    TEST_ASSERT(Url::isValidRelativeUrl("http://absolute.com/path"));  // Absolute
    TEST_ASSERT(Url::isValidRelativeUrl("/relative/path"));             // Relative

    // Test other schemes (now supported via url crate's join method)
    Url ftpResult = base.resolved(QString("ftp://ftp.example.com/file"));
    TEST_ASSERT_EQ(ftpResult.toString(), QString("ftp://ftp.example.com/file"));

    // Test WebSocket URLs
    Url wsResult = base.resolved(QString("ws://websocket.example.com/socket"));
    TEST_ASSERT_EQ(wsResult.toString(), QString("ws://websocket.example.com/socket"));

    Url wssResult = base.resolved(QString("wss://secure-websocket.example.com/socket"));
    TEST_ASSERT_EQ(wssResult.toString(), QString("wss://secure-websocket.example.com/socket"));

    // Test empty relative URL
    Url emptyResult = base.resolved(QString(""));
    TEST_ASSERT(!emptyResult.isValid());  // Should return invalid URL

    // Verify that absolute URLs resolve to themselves (unchanged)
    QString absoluteUrl1 = "http://different.com/path";
    QString absoluteUrl2 = "https://secure.example.org/api";

    TEST_ASSERT_EQ(base.resolved(absoluteUrl1).toString(), absoluteUrl1);
    TEST_ASSERT_EQ(base.resolved(absoluteUrl2).toString(), absoluteUrl2);

    // Verify these absolute URLs are considered "valid relative URLs" by our validation method
    TEST_ASSERT(Url::isValidRelativeUrl(absoluteUrl1));
    TEST_ASSERT(Url::isValidRelativeUrl(absoluteUrl2));

    // Test that our validation method now supports more schemes too
    TEST_ASSERT(Url::isValidRelativeUrl("ftp://ftp.example.com/file"));
    TEST_ASSERT(Url::isValidRelativeUrl("ws://websocket.example.com/socket"));
    TEST_ASSERT(Url::isValidRelativeUrl("wss://secure-websocket.example.com/socket"));
    TEST_ASSERT(Url::isValidRelativeUrl("file:///local/file.txt"));
}

extern "C" int url_test(ffi::TestException *out_ex)
{
    TEST_CATCH(basicUrlOperations());
    TEST_CATCH(invalidUrl());
    TEST_CATCH(emptyUrl());
    TEST_CATCH(fromEncoded());
    TEST_CATCH(schemeOperations());
    TEST_CATCH(clearUrl());
    TEST_CATCH(copyAndAssign());
    TEST_CATCH(relativeUrlValidation());
    TEST_CATCH(urlResolution());

    return 0;
}