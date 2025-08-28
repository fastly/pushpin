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
#include "cowstring.h"

static void basicUsage()
{
	CowString s("hello");
	CowByteArray a = s.toUtf8();
	TEST_ASSERT_EQ(s.asQString().toUtf8(), a.asQByteArray());
}

extern "C" int cowstring_test(ffi::TestException *out_ex)
{
	TEST_CATCH(basicUsage());

	return 0;
}
