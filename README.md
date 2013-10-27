Pushpin
-------
Date: October 27th, 2013

Author: Justin Karneges <justin@fanout.io>

Mailing List: http://lists.fanout.io/listinfo.cgi/fanout-users-fanout.io

Read:
  * http://blog.fanout.io/2013/02/10/http-grip-proxy-hold-technique/
  * http://blog.fanout.io/2013/04/09/an-http-reverse-proxy-for-realtime/

Pushpin is an HTTP reverse proxy server that makes it easy to implement streaming and long-polling services. It communicates with backend web applications using regular, short-lived HTTP requests (GRIP protocol). This allows the backend applications to be written in any language and use any webserver.

Additionally, Pushpin does all of this without exposing a proprietary protocol to clients. The HTTP content between the client and your server is whatever you want it to be. This makes it ideal for implementing APIs.

License
-------

Pushpin is offered under the GNU AGPL. See the COPYING file.

Features
--------

  * Implement any realtime HTTP API using any webserver for the logic
  * Proxied requests are streamed, so non-realtime requests remain unhindered
  * Fault tolerant multiprocess design reduces risk if things go wrong
  * Handle thousands of simultaneous connections

Requirements
------------

  * qt >= 4.7
  * qca >= 2.0 (and an hmac(sha256)-supporting plugin, like qca-ossl)
  * libzmq >= 2.0
  * qjson
  * mongrel2 (git release/1.9.0 or develop branch)
  * zurl (git v1.0.0 tag)
  * python
  * python setproctitle
  * python tnetstring
  * python zmq
  * python jinja2

Install guide
-------------

https://github.com/fanout/pushpin/wiki/Install

If accessing from Git, be sure to pull submodules:

    git submodule init
    git submodule update

Build and run:

    make
    cp config/pushpin.conf.example pushpin.conf
    cp config/routes.example routes
    ./pushpin

By default, Pushpin listens on port 7999 and forwards to localhost port 80. If you've got a webserver running on port 80, you can confirm that proxying works by browsing to http://localhost:7999/

Multiprocess design
-------------------

Pushpin consists of five processes: mongrel2, zurl, pushpin-proxy, pushpin-handler, and pushpin (the "runner"). In a basic setup you don't really need to think about this. Just run pushpin to start everything up, and terminate the process (or ctrl-c) to shut everything down.

If you'd prefer to individually manage any of these processes yourself, then adjust the "services" field in pushpin.conf. You can even choose to not use the runner at all. In that case, Pushpin's own processes can be launched as follows:

Proxy process:

    pushpin-proxy --config=/path/to/pushpin.conf

Handler process:

    pushpin-handler --config=/path/to/pushpin.conf
