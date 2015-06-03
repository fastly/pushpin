Pushpin
=======

[![Join the chat at https://gitter.im/fanout/pushpin](https://badges.gitter.im/Join%20Chat.svg)](https://gitter.im/fanout/pushpin?utm_source=badge&utm_medium=badge&utm_campaign=pr-badge&utm_content=badge)

Author: Justin Karneges <justin@fanout.io>  
Mailing List: http://lists.fanout.io/listinfo.cgi/fanout-users-fanout.io

See: http://pushpin.org/

Pushpin is a reverse proxy server that makes it easy to implement WebSocket, HTTP streaming, and HTTP long-polling services. It communicates with backend web applications using regular, short-lived HTTP requests (GRIP protocol). This allows backend applications to be written in any language and use any webserver.

Additionally, Pushpin does all of this without exposing a proprietary protocol to clients. The HTTP/WebSocket content between the client and your server is whatever you want it to be. This makes it ideal for implementing APIs.

Pushpin is written in C++ and Python. The name means to "pin" (hold) connections open for "pushing".

License
-------

Pushpin is offered under the GNU AGPL. See the COPYING file.

Features
--------

  * Implement any realtime HTTP/WebSocket API using any webserver for the logic
  * Proxied requests are streamed, so non-realtime requests remain unhindered
  * Fault tolerant multiprocess design
  * Handle thousands of simultaneous connections

Install
-------

See [the Install guide](https://github.com/fanout/pushpin/wiki/Install), which covers how to install Pushpin and its dependencies. If you already have the dependencies installed, then below are brief instructions for Pushpin itself.

If accessing from Git, be sure to pull submodules:

    git submodule init && git submodule update

Build and run:

    make
    cp -r examples/config .
    ./pushpin

By default, Pushpin listens on port 7999 and forwards to localhost port 80. If you've got a webserver running on port 80, you can confirm that proxying works by browsing to http://localhost:7999/

Configuration
-------------

See [Configuration](https://github.com/fanout/pushpin/wiki/Configuration).
