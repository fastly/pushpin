/*
 * Copyright (C) 2023-2024 Fastly, Inc.
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

#ifndef RUST_WZMQ_H
#define RUST_WZMQ_H

#include <stddef.h>

// zmq crate version 0.10 bundles zeromq 4.3
#define WZMQ_VERSION_MAJOR 4
#define WZMQ_VERSION_MINOR 3

// NOTE: must match values in ffi.rs
#define WZMQ_PAIR 0
#define WZMQ_PUB 1
#define WZMQ_SUB 2
#define WZMQ_REQ 3
#define WZMQ_REP 4
#define WZMQ_DEALER 5
#define WZMQ_ROUTER 6
#define WZMQ_PULL 7
#define WZMQ_PUSH 8
#define WZMQ_XPUB 9
#define WZMQ_XSUB 10
#define WZMQ_STREAM 11

// NOTE: must match values in ffi.rs
#define WZMQ_FD 0
#define WZMQ_SUBSCRIBE 1
#define WZMQ_UNSUBSCRIBE 2
#define WZMQ_LINGER 3
#define WZMQ_IDENTITY 4
#define WZMQ_IMMEDIATE 5
#define WZMQ_RCVMORE 6
#define WZMQ_EVENTS 7
#define WZMQ_SNDHWM 8
#define WZMQ_RCVHWM 9
#define WZMQ_TCP_KEEPALIVE 10
#define WZMQ_TCP_KEEPALIVE_IDLE 11
#define WZMQ_TCP_KEEPALIVE_CNT 12
#define WZMQ_TCP_KEEPALIVE_INTVL 13
#define WZMQ_ROUTER_MANDATORY 14

// NOTE: must match values in ffi.rs
#define WZMQ_DONTWAIT 0x01
#define WZMQ_SNDMORE 0x02

// NOTE: must match values in ffi.rs
#define WZMQ_POLLIN 0x01
#define WZMQ_POLLOUT 0x02

extern "C" {
	typedef struct wzmq_msg {
		void *data;
	} wzmq_msg_t;

	void *wzmq_init(int io_threads);
	int wzmq_term(void *context);
	void *wzmq_socket(void *context, int type);
	int wzmq_close(void *socket);
	int wzmq_getsockopt(void *socket, int option_name, void *option_value, size_t *option_len);
	int wzmq_setsockopt(void *socket, int option_name, const void *option_value, size_t option_len);
	int wzmq_connect(void *socket, const char *endpoint);
	int wzmq_bind(void *socket, const char *endpoint);
	int wzmq_send(void *socket, void *buf, size_t len, int flags);
	int wzmq_recv(void *socket, void *buf, size_t len, int flags);
	int wzmq_msg_init(wzmq_msg_t *msg);
	int wzmq_msg_init_size(wzmq_msg_t *msg, size_t size);
	void *wzmq_msg_data(wzmq_msg_t *msg);
	size_t wzmq_msg_size(wzmq_msg_t *msg);
	int wzmq_msg_close(wzmq_msg_t *msg);
	int wzmq_msg_send(wzmq_msg_t *msg, void *socket, int flags);
	int wzmq_msg_recv(wzmq_msg_t *msg, void *socket, int flags);
}

#endif
