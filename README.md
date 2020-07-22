# Condure

Condure is a service that manages network connections on behalf of server applications, in order to allow controlling the connections from multiple processes. Applications communicate with Condure over [ZeroMQ](https://zeromq.org/).

Condure can only manage connections for protocols it knows about. Currently this is HTTP/1 and WebSockets. See [Supported protocols](#supported-protocols).

The project was inspired by [Mongrel2](https://mongrel2.org/).

## Use cases

* Pass connection ownership from one process to another.
* Restart an application without its connections getting disconnected.
* Balance connection ownership among multiple processes.

## Basic usage

Start the server:

```
$ condure --listen 8000 --zclient-stream ipc://client
```

Connect a handler to it, such as this simple Python program:

```py
# this handler responds to every request with "hello world"

import os
import time
import tnetstring
import zmq

instance_id = 'basichandler.{}'.format(os.getpid()).encode()

ctx = zmq.Context()
in_sock = ctx.socket(zmq.PULL)
in_sock.connect('ipc://client-out')
out_sock = ctx.socket(zmq.PUB)
out_sock.connect('ipc://client-in')

# await subscription
time.sleep(0.01)

while True:
    m_raw = in_sock.recv()
    req = tnetstring.loads(m_raw[1:])
    print('IN {}'.format(req))

    resp = {}
    resp[b'from'] = instance_id
    resp[b'id'] = req[b'id']
    resp[b'code'] = 200
    resp[b'reason'] = b'OK'
    resp[b'headers'] = [[b'Content-Type', b'text/plain']]
    resp[b'body'] = b'hello world\n'

    print('OUT {}'.format(resp))
    out_sock.send(req[b'from'] + b' T' + tnetstring.dumps(resp))
```

A client request:

```
$ curl -i http://localhost:8000
HTTP/1.1 200 OK
Content-Type: text/plain
Content-Length: 12

hello world
```

The process that receives the request doesn't need to be the same one that responds! For example, here's a program that outputs request IDs to stdout:

```py
# this handler just outputs the request ID

import tnetstring
import zmq

ctx = zmq.Context()
sock = ctx.socket(zmq.PULL)
sock.connect('ipc://client-out')

while True:
    m = sock.recv_multipart()
    req = tnetstring.loads(m[0][1:])
    print('{} {}'.format(req[b'from'].decode(), req[b'id'].decode()))
```

We can see request ID information when a client request is made:

```
$ python examples/printreq.py
condure 0-0-0
```

From another shell we can respond using a program like this:

```py
# this program sends a response to a certain request ID

import sys
import time
import tnetstring
import zmq

body = sys.argv[1]
addr = sys.argv[2].encode()
rid = sys.argv[3].encode()

ctx = zmq.Context()
sock = ctx.socket(zmq.PUB)
sock.connect('ipc://client-in')

# await subscription
time.sleep(0.01)

resp = {}
resp[b'from'] = b'sendresp'
resp[b'id'] = rid
resp[b'code'] = 200
resp[b'reason'] = b'OK'
resp[b'headers'] = [[b'Content-Type', b'text/plain']]
resp[b'body'] = '{}\n'.format(body).encode()

m = [addr + b' T' + tnetstring.dumps(resp)]

sock.send_multipart(m)
```

For example:

```
$ python examples/sendresp.py "responding from another process" condure 0-0-0
```

The client sees:

```
$ curl -i http://localhost:8000
HTTP/1.1 200 OK
Content-Type: text/plain
Content-Length: 32

responding from another process
```

For easy testing, the programs can be piped together:

```
$ python -u examples/printreq.py | xargs -n 2 python examples/sendresp.py "responding from another process"
```

## Suspending and resuming connections

When passing control of a connection from one process to another, it is important to suspend the connection first. This is done by sending a `handoff-start` message and waiting for a `handoff-proceed` message. At that point, the connection information can be given to another process, and the connection can be resumed by sending any message (such as `keep-alive`). See the [ZHTTP spec](https://rfc.zeromq.org/spec/33/).

## REQ mode

In addition to the stream mode which uses PUSH/ROUTER/SUB sockets, there is a "REQ" mode available which uses a DEALER socket. To enable it, set `req` as the mode on a listen port. This mode can be handy for implementing simple request/response servers using ZeroMQ.

## Supported protocols

Condure supports HTTP/1 and WebSockets.

Condure manages connections at layer 7 and only supports protocols it knows about. This is to simplify its usage. Handling arbitrary protocols would require applications to build protocol stacks capable of suspending/resuming sessions at arbitrary byte positions in TCP streams, making Condure usage prohibitive. Instead, Condure is protocol-aware, and provides parsed frames to applications, so that applications are only required to support suspending/resuming sessions at frame boundaries.

## Performance

Condure was built for high performance. It uses numerous optimization techniques, including minimal heap allocations, ring buffers, vectored I/O, hierarchical timing wheels, and fast data structures (e.g. slabs). Over 1M concurrent connections have been tested on a single instance using just 2 workers (4 threads total). More detailed benchmarks to come.

## Comparison to Mongrel2

* Condure supports multiple cores.
* Condure supports listening on multiple ports without requiring multiple processes.
* Condure does not support multiple routes and is not intended to be a shared server. Each application that wants to keep connections in a separate process should spawn its own Condure instance.
* Condure has no config file. Configuration is supplied using command line arguments.
* Condure uses a different ZeroMQ-based protocol, [ZHTTP](https://rfc.zeromq.org/spec/33/), which is easier to use than Mongrel2's protocol and more reliable.

## Future plans

* TLS
* HTTP/2
* HTTP/3
