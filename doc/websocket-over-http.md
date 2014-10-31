WebSocket-Over-HTTP Protocol
============================

The WebSocket-Over-HTTP protocol is a simple, text-based protocol for gatewaying between a WebSocket client and a conventional HTTP server.

Why??
-----

GRIP (Generic Realtime Intermediary Protocol, used by Pushpin and Fanout.io) enables out-of-band message injection into WebSocket connections. Normally, using GRIP with WebSockets requires a WebSocket connection on both sides of the proxy:

    Client <--WS--> GRIP Proxy <--WS--> Server

The GRIP Proxy is a publish/subscribe service. When the server has data to send spontaneously, it does not use its WebSocket connection to send the data. Rather, it uses an out-of-band publish command to the proxy (usually via HTTP POST). This means that the WebSocket connection between the proxy and the server is used almost exclusively for servicing incoming requests from the client.

If the communication path between the proxy and the server only needs to handle request/response interactions, then HTTP becomes a viable alternative to a WebSocket:

    Client <--WS--> GRIP Proxy <--HTTP--> Server

Using HTTP for communication between the proxy and server may be easier to maintain and scale since HTTP server tools are well established. Plus, if the server is merely doing stateless RPC processing, then HTTP is arguably a respectable choice for this tier in the service.

Of course, the usefulness of this gatewaying is entirely dependent on the server having a way to send data to clients out-of-band. As such, it is recommended that the WebSocket-Over-HTTP protocol be used in combination with GRIP. Note, however, that the WebSocket-Over-HTTP protocol does not explicitly depend on GRIP.

Protocol
--------

The gateway and server exchange WebSocket "events" via HTTP requests and responses. The following events are defined:

* `OPEN` - WebSocket negotiation request or acknowledgement
* `TEXT`, `BINARY` - Messages with content
* `PING`, `PONG` - Ping and pong messages
* `CLOSE` - Close message with 16-bit close code
* `DISCONNECT` - Indicates connection closed uncleanly or does not exist

Events are encoded in a format similar to HTTP chunked transfer encoding:

    TEXT B\r\n
    hello world\r\n

The format is the name of the event, a space, the hexidecimal encoding of the content size, a carriage return and newline, the content bytes, and finally another carriage return and newline.

For events with no content, the size and content section can be omitted:

    OPEN\r\n

Events with content are TEXT, BINARY, and CLOSE. Events without content are OPEN, PING, PONG, and DISCONNECT.

An event that should not contain content MAY be encoded with content. Receivers should ignore such content. For example, this is legal:

    OPEN 0\r\n
    \r\n

One or more encoded events are then concatenated and placed in the body of an HTTP request or response, with content type `application/websocket-events`.

Example
-------

Gateway opens connection:

    POST /target HTTP/1.1
    Connection-Id: b5ea0e11
    Content-Type: application/websocket-events
    [... any headers included by the client WebSocket handshake ...]

    OPEN\r\n

Server accepts connection:

    HTTP/1.1 200 OK
    Content-Type: application/websocket-events
    [... any headers to include in the WebSocket negotiation response ...]

    OPEN\r\n

Gateway relays message from client:

    POST /target HTTP/1.1
    Connection-Id: b5ea0e11
    Content-Type: application/websocket-events

    TEXT 5\r\n
    hello\r\n

Server responds with two messages:

    HTTP/1.1 200 OK
    Content-Type: application/websocket-events

    TEXT 5\r\n
    world\r\n
    TEXT 1B\r\n
    here is another nice message\r\n

Gateway relays a close message:

    POST /target HTTP/1.1
    Connection-Id: b5ea0e11
    Content-Type: application/websocket-events

    CLOSE 2\r\n
    [... binary status code ...]\r\n

Server sends a close message back:

    HTTP/1.1 200 OK
    Content-Type: application/websocket-events

    CLOSE 2\r\n
    [... binary status code ...]\r\n

State Management
----------------

Headers of the initial WebSocket negotiation request MUST be replayed with every request made by the gateway. This means that if the client uses cookies or other headers for authentication purposes, the server will receive this data with every message.

The gateway includes a `Connection-Id` header which uniquely identifies a particular client connection. Servers that need to track connections can use this. In most cases, though, servers should not have to care about connections.

It is possible to bind metadata to the connection via a `Set-Meta-*` header. This works similar to a cookie. The server can set a field that the gateway should echo back on all subsequent requests.

For example, a client supplies a cookie which the gateway relays across during connect:

    POST /target HTTP/1.1
    Connection-Id: b5ea0e11
    Content-Type: application/websocket-events
    Cookie: [... auth info ...]

    OPEN\r\n

The server accepts the connection and binds a User field based on the cookie:

    HTTP/1.1 200 OK
    Content-Type: application/websocket-events
    Set-Meta-User: alice

    OPEN\r\n

Now, any further requests from the gateway will include a Meta-User header:

    POST /target HTTP/1.1
    Connection-Id: b5ea0e11
    Meta-User: alice
    Content-Type: application/websocket-events

    TEXT 5\r\n
    hello\r\n

Security note: gateways MUST NOT relay any headers from the client that are prefixed with `Meta-`. This prevents the client from spoofing metadata bindings. Additionally, the server needs to ensure that an incoming request came from a gateway before trusting its `Meta-*` headers.

Notes
-----

* The first request MUST contain an OPEN event as the first event.
* The first response MUST contain an OPEN event as the first event.
* If the server tracks connections and no longer considers the connection to exist, it should respond with DISCONNECT. In most cases, servers will not track connections, though.
* Gateway should only have one outstanding request per client connection. This ensures in-order delivery.
* DISCONNECT event only sent if connection was not closed cleanly. With clean close, disconnect is implied.
* Within this protocol alone, the server has no way to talk to the client outside of responding to incoming requests.
* Gateway can send an empty request to keep-alive the current connection. The gateway shall consider an empty response to be a keep-alive from the server. The server enables keep-alives by providing a Keep-Alive-Interval response header.

