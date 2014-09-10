WebSocket-Over-HTTP Protocol
============================

This is a simple text-based protocol for gatewaying between a WebSocket client and a conventional HTTP server.

Why??
-----

WebSocket applications designed around request/response and publish/subscribe messaging patterns tend to require very little session state. In many cases, a WebSocket's primary value is to act as an efficient downstream channel for publish/subscribe events.

GRIP (Generic Realtime Intermediary Protocol), used by Pushpin and Fanout.io, enables out-of-band message injection into WebSocket connections. Normally, using GRIP with WebSockets requires a WebSocket connection on both sides of the proxy: Client <--WS--> Grip Proxy <--WS--> Server. However, if published messages bypass the Proxy->Server connection, such that the Proxy->Server connection is only ever used for request/response interactions, then it becomes interesting to consider using HTTP for this hop instead.

Note that the WebSocket-Over-HTTP protocol does not explicitly depend on GRIP. However, you'll almost certainly want to pair this protocol with GRIP, or something like it. Without some kind of out-of-band delivery mechanism, the server will have no way to send data spontaneously to the client, which is the whole reason you'd use WebSockets in the first place.

Yes, there is overhead in using HTTP instead of WebSockets. This only replaces the Proxy->Server communication though, where latency and bandwidth are less of a concern. The last mile (Client->Proxy) continues to use a WebSocket.

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
    \r\n

Server accepts connection:

    HTTP/1.1 200 OK
    Content-Type: application/websocket-events
    [... any headers to include in the WebSocket negotiation response ...]

    OPEN\r\n
    \r\n

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

Headers of the initial WebSocket negotiation request MUST be replayed with every request made by the gateway. This means that if you use Cookies or other headers for authentication purposes, you'll receive this data with every message.

The gateway also includes a `Connection-Id` header which uniquely identifies a particular connection. Unless you're doing something fancy and tracking connections (presence?), though, you shouldn't need this.

It is possible to bind metadata to the connection via a `Meta-Set-*` header. This works similar to a cookie. The server can set a header name and value that the gateway should echo back on all subsequent requests.

For example, client supplies a cookie which the gateway relays across during connect:

    POST /target HTTP/1.1
    Connection-Id: b5ea0e11
    Content-Type: application/websocket-events
    Cookie: [... auth info ...]

    OPEN\r\n
    \r\n

Server accepts connection and binds a User header based on the cookie:

    HTTP/1.1 200 OK
    Content-Type: application/websocket-events
    Meta-Set-User: alice

    OPEN\r\n
    \r\n

Now, any further requests from the gateway will include this header:

    POST /target HTTP/1.1
    Connection-Id: b5ea0e11
    User: alice
    Content-Type: application/websocket-events

    TEXT 5\r\n
    hello\r\n

Security note: the server will need to ensure that the request originated from the gateway before trusting such headers.

Notes
-----

* The first request MUST contain an OPEN event as the first event.
* The first response MUST contain an OPEN event as the first event.
* If the server tracks connections and no longer considers the connection to exist, it should respond with DISCONNECT. In most cases, servers will not track connections, though.
* Gateway should only have one outstanding request per client connection. This ensures in-order delivery.
* DISCONNECT event only sent if connection was not closed cleanly. With clean close, disconnect is implied.
* Within this protocol alone, the server has no way to talk to the client outside of responding to incoming requests.
* Gateway can send an empty request to keep-alive the current connection. The gateway shall consider an empty response to be a keep-alive from the server. The server enables keep-alives by providing a Keep-Alive-Interval response header.

