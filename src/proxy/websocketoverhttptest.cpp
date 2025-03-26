/*
 * Copyright (C) 2025 Fastly, Inc.
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

#include "test.h"
#include "websocketoverhttp.h"

static void convertFrames()
{
	QList<WebSocket::Frame> frames;
	frames += WebSocket::Frame(WebSocket::Frame::Text, "hello", true);
	frames += WebSocket::Frame(WebSocket::Frame::Continuation, " world", false);

	bool ok = false;
	int framesRepresented = 0;
	int contentRepresented = 0;
	QList<WebSocketOverHttp::Event> events = WebSocketOverHttp::framesToEvents(frames, -1, -1, &ok, &framesRepresented, &contentRepresented);
	TEST_ASSERT(ok);
	TEST_ASSERT_EQ(events.count(), 1);
	TEST_ASSERT_EQ(events[0].type, "TEXT");
	TEST_ASSERT_EQ(events[0].content, "hello world");

	int removed = WebSocketOverHttp::removeContentFromFrames(&frames, contentRepresented);
	TEST_ASSERT_EQ(removed, contentRepresented);
	TEST_ASSERT(frames.isEmpty());
}

static void removePartial()
{
	QList<WebSocket::Frame> frames;
	frames += WebSocket::Frame(WebSocket::Frame::Text, "hello", true);
	frames += WebSocket::Frame(WebSocket::Frame::Continuation, " world", false);
	frames += WebSocket::Frame(WebSocket::Frame::Text, "", false);
	frames += WebSocket::Frame(WebSocket::Frame::Text, "foo", false);

	int removed = WebSocketOverHttp::removeContentFromFrames(&frames, 3);
	TEST_ASSERT_EQ(removed, 3);
	TEST_ASSERT_EQ(frames.count(), 4);
	TEST_ASSERT_EQ(frames[0].type, WebSocket::Frame::Text);
	TEST_ASSERT_EQ(frames[0].data, "lo");

	removed = WebSocketOverHttp::removeContentFromFrames(&frames, 2);
	TEST_ASSERT_EQ(removed, 2);
	TEST_ASSERT_EQ(frames.count(), 3);
	TEST_ASSERT_EQ(frames[0].type, WebSocket::Frame::Text);
	TEST_ASSERT_EQ(frames[0].data, " world");

	removed = WebSocketOverHttp::removeContentFromFrames(&frames, 6);
	TEST_ASSERT_EQ(removed, 6);
	TEST_ASSERT_EQ(frames.count(), 1);
	TEST_ASSERT_EQ(frames[0].type, WebSocket::Frame::Text);
	TEST_ASSERT_EQ(frames[0].data, "foo");

	removed = WebSocketOverHttp::removeContentFromFrames(&frames, 10);
	TEST_ASSERT_EQ(removed, 3);
	TEST_ASSERT(frames.isEmpty());
}

extern "C" int websocketoverhttp_test(ffi::TestException *out_ex)
{
	TEST_CATCH(convertFrames());
	TEST_CATCH(removePartial());

	return 0;
}
