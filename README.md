# Pushpin

Website: https://pushpin.org/  
Chat Room: [![Join the chat at https://gitter.im/fanout/pushpin](https://badges.gitter.im/Join%20Chat.svg)](https://gitter.im/fanout/pushpin?utm_source=badge&utm_medium=badge&utm_campaign=pr-badge&utm_content=badge)

Pushpin is a reverse proxy server written in C++ and Rust that makes it easy to implement WebSocket, HTTP streaming, and HTTP long-polling services. The project is unique among realtime push solutions in that it is designed to address the needs of API creators. Pushpin is transparent to clients and integrates easily into an API stack.

## How it works

Pushpin is placed in the network path between the backend and any clients:

<p align="center">
  <img src="https://pushpin.org/image/pushpin-abstract.png" alt="pushpin-abstract"/>
</p>

Pushpin communicates with backend web applications using regular, short-lived HTTP requests. This allows backend applications to be written in any language and use any webserver. There are two main integration points:

1. The backend must handle proxied requests. For HTTP, each incoming request is proxied to the backend. For WebSockets, the activity of each connection is translated into a series of HTTP requests<sup>[1](#proxy-modes)</sup> sent to the backend. Pushpin's behavior is determined by how the backend responds to these requests.
2. The backend must tell Pushpin to push data. Regardless of how clients are connected, data may be pushed to them by making an HTTP POST request to Pushpin's private control API (`http://localhost:5561/publish/` by default). Pushpin will inject this data into any client connections as necessary.

To assist with integration, there are [libraries](https://pushpin.org/docs/usage/#libraries) for many backend languages and frameworks. Pushpin has no libraries on the client side because it is transparent to clients.

## Example

To create an HTTP streaming connection, respond to a proxied request with special headers `Grip-Hold` and `Grip-Channel`<sup>[2](#grip)</sup>:

```http
HTTP/1.1 200 OK
Content-Type: text/plain
Content-Length: 22
Grip-Hold: stream
Grip-Channel: test

welcome to the stream
```

When Pushpin receives the above response from the backend, it will process it and send an initial response to the client that instead looks like this:

```http
HTTP/1.1 200 OK
Content-Type: text/plain
Transfer-Encoding: chunked
Connection: Transfer-Encoding

welcome to the stream
```

Pushpin eats the special headers and switches to chunked encoding (notice there's no `Content-Length`). The request between Pushpin and the backend is now complete, but the request between the client and Pushpin remains held open. The request is subscribed to a channel called `test`.

Data can then be pushed to the client by publishing data on the `test` channel:

```bash
curl -d '{ "items": [ { "channel": "test", "formats": { "http-stream": \
    { "content": "hello there\n" } } } ] }' \
    http://localhost:5561/publish
```

The client would then see the line "hello there" appended to the response stream. Ta-da, transparent realtime push!

For more details, see the [HTTP streaming](https://pushpin.org/docs/usage/#http-streaming) section of the documentation. Pushpin also supports [HTTP long-polling](https://pushpin.org/docs/usage/#http-long-polling) and [WebSockets](https://pushpin.org/docs/usage/#websockets).

## Example using a library

Using a library on the backend makes integration is even easier. Here's another HTTP streaming example, similar to the one shown above, except using Pushpin's [Django library](https://github.com/fanout/django-grip). Please note that Pushpin is not Python/Django-specific and there are backend libraries for [other languages/frameworks, too](https://pushpin.org/docs/usage/#libraries).

The Django library requires configuration in `settings.py`:
```python
MIDDLEWARE_CLASSES = (
    'django_grip.GripMiddleware',
    ...
)

GRIP_PROXIES = [{'control_uri': 'http://localhost:5561'}]
```

Here's a simple view:
```python
from django.http import HttpResponse
from django_grip import set_hold_stream

def myendpoint(request):
    if request.method == 'GET':
        # subscribe every incoming request to a channel in stream mode
        set_hold_stream(request, 'test')
        return HttpResponse('welcome to the stream\n', content_type='text/plain')
    ...
```

What happens here is the `set_hold_stream()` method flags the request as needing to turn into a stream, bound to channel `test`. The middleware will see this and add the necessary `Grip-Hold` and `Grip-Channel` headers to the response.

Publishing data is easy:
```python
from gripcontrol import HttpStreamFormat
from django_grip import publish

publish('test', HttpStreamFormat('hello there\n'))
```

## Example using WebSockets

Pushpin supports WebSockets by converting connection activity/messages into HTTP requests and sending them to the backend. For this example, we'll use Pushpin's [Express library](https://github.com/fanout/express-grip). As before, please note that Pushpin is not Node/Express-specific and there are backend libraries for [other languages/frameworks, too](https://pushpin.org/docs/usage/#libraries).

The Express library requires configuration and setting up middleware handlers before and after any endpoints:
```javascript
var express = require('express');
var grip = require('grip');
var expressGrip = require('express-grip');

expressGrip.configure({
    gripProxies: [{'control_uri': 'http://localhost:5561', 'key': 'changeme'}]
});

var app = express();

// Add the pre-handler middleware to the front of the stack
app.use(expressGrip.preHandlerGripMiddleware);

// put your normal endpoint handlers here, for example:
app.get('/hello', function(req, res, next) {
    res.send('hello world\n');

    // next() must be called for the post-handler middleware to execute
    next();
});

// Add the post-handler middleware to the back of the stack
app.use(expressGrip.postHandlerGripMiddleware);
```

Because of the post-handler middleware, it's important that you call `next()` at the end of your handlers.

With that structure in place, here's an example of a WebSocket endpoint:
```javascript
app.post('/websocket', function(req, res, next) {
    var ws = expressGrip.getWsContext(res);

    // If this is a new connection, accept it and subscribe it to a channel
    if (ws.isOpening()) {
        ws.accept();
        ws.subscribe('all');
    }

    while (ws.canRecv()) {
        var message = ws.recv();

        // If return value is null then connection is closed
        if (message == null) {
            ws.close();
            break;
        }

        // broadcast the message to everyone connected
        expressGrip.publish('all', new grip.WebSocketMessageFormat(message));
    }

    // next() must be called for the post-handler middleware to execute
    next();
});
```

The above code binds all incoming connections to a channel called `all`. Any received messages are published out to all connected clients.

What's particularly noteworthy is that the above endpoint is stateless. The app doesn't keep track of connections, and the handler code only runs whenever messages arrive. Restarting the app won't disconnect clients.

The `while` loop is deceptive. It looks like it's looping for the lifetime of the WebSocket connection, but what it's really doing is looping through a batch of WebSocket messages that was just received via HTTP. Often this will be one message, and so the loop performs one iteration and then exits. Similarly, the `ws` object only exists for the duration of the handler invocation, rather than for the lifetime of the connection as you might expect. It may look like socket code, but it's all an illusion. :tophat:

For details on the underlying protocol conversion, see the [WebSocket-Over-HTTP Protocol spec](https://pushpin.org/docs/protocols/websocket-over-http/).

## Example without a webserver

Pushpin can also connect to backend servers via ZeroMQ instead of HTTP. This may be preferred for writing lower-level services where a real webserver isn't needed. The messages exchanged over the ZeroMQ connection contain the same information as HTTP, encoded as TNetStrings.

To use a ZeroMQ backend, first make sure there's an appropriate route in Pushpin's `routes` file:
```
* zhttpreq/tcp://127.0.0.1:10000
```

The above line tells Pushpin to bind a REQ-compatible socket on port 10000 that handlers can connect to.

Activating an HTTP stream is as easy as responding on a REP socket:
```python
import zmq
import tnetstring

zmq_context = zmq.Context()
sock = zmq_context.socket(zmq.REP)
sock.connect('tcp://127.0.0.1:10000')

while True:
    req = tnetstring.loads(sock.recv()[1:])

    resp = {
        'id': req['id'],
        'code': 200,
        'reason': 'OK',
        'headers': [
            ['Grip-Hold', 'stream'],
            ['Grip-Channel', 'test'],
            ['Content-Type', 'text/plain']
        ],
        'body': 'welcome to the stream\n'
    }

    sock.send('T' + tnetstring.dumps(resp))
```

## Why another realtime solution?

Pushpin is an ambitious project with two primary goals:

* Make realtime API development easier. There are many other solutions out there that are excellent for building realtime apps, but few are useful within the context of *APIs*. For example, you can't use Socket.io to build Twitter's streaming API. A new kind of project is needed in this case.
* Make realtime push behavior delegable. The reason there isn't a realtime push CDN yet is because the standards and practices necessary for delegating to a third party in a transparent way are not yet established. Pushpin is more than just another realtime push solution; it represents the next logical step in the evolution of realtime web architectures.

To really understand Pushpin, you need to think of it as more like a gateway than a message queue. Pushpin does not persist data and it is agnostic to your application's data model. Your backend provides the mapping to whatever that data model is. Tools like Kafka and RabbitMQ are complementary. Pushpin is also agnostic to your API definition. Clients don't necessarily subscribe to "channels" or receive "messages". Clients make HTTP requests or send WebSocket frames, and your backend decides the meaning of those inputs. Pushpin could perhaps be awkwardly described as "a proxy server that enables web services to delegate the handling of realtime push primitives".

On a practical level, there are many benefits to Pushpin that you don't see anywhere else:

* The proxy design allows Pushpin to fit nicely within an API stack. This means it can inherit other facilities from your REST API, such as authentication, logging, throttling, etc. It can be combined with an API management system.
* As your API scales, a multi-tiered architecture will become inevitable. With Pushpin you can easily do this from the start.
* It works well with microservices. Each microservice can have its own Pushpin instance. No central bus needed.
* Hot reload. Restarting the backend doesn't disconnect clients.
* In the case of WebSocket messages being proxied out as HTTP requests, the messages may be handled statelessly by the backend. Messages from a single connection can even be load balanced across a set of backend instances.

## Install

Check out the [the Install guide](https://pushpin.org/docs/install/), which covers how to install and run. There are packages available for Linux (Debian, Ubuntu, CentOS, Red Hat), Mac (Homebrew), or you can build from source.

By default, Pushpin listens on port 7999 and requests are handled by its internal test handler. You can confirm the server is working by browsing to `http://localhost:7999/`. Next, you should modify the `routes` config file to route requests to your backend webserver. See [Configuration](https://pushpin.org/docs/configuration/).

## Scalability

Pushpin is horizontally scalable. Instances don’t talk to each other, and sticky routing is not needed. Backends must publish data to all instances to ensure clients connected to any instance will receive the data. Most of the backend libraries support configuring more than one Pushpin instance, so that a single publish call will send data to multiple instances at once.

Optionally, ZeroMQ PUB/SUB can be used to send data to Pushpin instead of using HTTP POST. When this method is used, subscription information is forwarded to each publisher, such that data will only be published to instances that have listeners.

As for vertical scalability, Pushpin has been tested with up to [1 million concurrent connections](https://github.com/fanout/pushpin-c1m) running on a single DigitalOcean droplet with 8 CPU cores. In practice, you may want to plan for fewer connections per instance, depending on your throughput. The new connection accept rate is about 800/sec (though this also depends on the speed of your backend), and the message throughput is about 8,000/sec. The important thing is that Pushpin is horizontally scalable which is effectively limitless.

## What does the name mean?

Pushpin means to "pin" connections open for "pushing".

## License

Pushpin is offered under the GNU AGPL. See the COPYING file.

## Footnotes

<a name="proxy-modes">1</a>: Pushpin can communicate WebSocket activity to the backend using either HTTP or WebSockets. Conversion to HTTP is generally recommended as it makes the backend easier to reason about.

<a name="grip">2</a>: GRIP (Generic Realtime Intermediary Protocol) is the name of Pushpin's backend protocol. More about that [here](https://pushpin.org/docs/protocols/grip/).
