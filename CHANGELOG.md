Pushpin Changelog
=================

v. 1.8.0 (2016-02-22)

  * Fix issue proxying large responses.
  * Refactor README.
  * Port server code to Qt 5.
  * Rewrite pushpin-publish tool from Python to C++.
  * Move internal.conf into LIBDIR.

v. 1.7.0 (2016-01-10)

  * Rewrite pushpin-handler from Python to C++.
  * Initial support for subscription filters and skip-self filter.
  * Fix sending of large responses when flow control not used.
  * Speed up shutdown.
  * Pass WebSocket GRIP logic upstream if GRIP proxy detected.
  * Don't forward WebSocket-Over-HTTP requests unless client trusted.
  * WebSocket-Over-HTTP: strip private headers from responses.
  * Long-polling: finish support for JSON patch.
  * m2adapter: dynamically enable/disable control port as needed.
  * publish tool: add id, prev-id, patch, and sender options.
  * Add monitorsubsock tool for monitoring SUB socket.
  * Refactor docs/grip-protocol.md.

v. 1.6.0 (2015-09-24)

  * Fix rare assert when publishing to a WebSocket.
  * Remove libdir from pushpin.conf.
  * Mongrel2: use download flow control.
  * Mongrel2: enable relaxed parsing.
  * Auto Cross-Origin: include Access-Control-Max-Age.
  * Throw error if can't create runtime directories on startup.
  * Various cleanups.

v. 1.5.0 (2015-07-23)

  * replace_beg route parameter.
  * Fixed bug where non-persistent connections were closed before data sent.
  * Accept invalid characters in request URIs and URL-encode them.

v. 1.4.0 (2015-07-16)

  * Improved handling of streamed input while proxying.
  * WebSocket over_http mode: relay error responses rather than 502.
  * Various WebSocket bugfixes.
  * Prefer using sortedcontainers.SortedDict rather than blist.sorteddict.

v. 1.3.3 (2015-07-05)

  * Fix crash on conflict retry introduced in previous version.

v. 1.3.2 (2015-07-05)

  * Better handling of responses with no explicit body (HEAD, 204, 304).
  * Persistent connection fixes.
  * Proxy flow control fixes.
  * WebSocket over_http mode: buffer fragmented messages before sending.

v. 1.3.1 (2015-06-19)

  * Fix http-response conflict recovery.
  * Correctly proxy WebSocket ping and pong frames.
  * Fix WebSocket compatibility with latest Zurl.

v. 1.3.0 (2015-06-03)

  * Many fixes with subscription reporting via stats and SUB socket.
  * Tweaks to enable higher concurrent connection counts.
  * WebSocket over_http mode sends DISCONNECT events.

v. 1.2.0 (2015-05-09)

  * http-stream: close action, keep-alive.
  * Check for new pushpin versions.
  * ZeroMQ endpoint discovery via command socket.
  * pushpin-publish command line tool.

v. 1.1.1 (2015-04-17)

  * Fix auto-cross-origin feature.

v. 1.1.0 (2015-03-08)

  * SUB socket input. SockJS client support.

v. 1.0.0 (2014-09-16)

  * Stable version.
