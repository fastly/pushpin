Pushpin Changelog
=================

v. 1.14.0 (2016-11-15)

  * Reliable HTTP streaming (stream hold + "GRIP Next").
  * Process messages in order if received out of order.

v. 1.13.1 (2016-10-27)

  * Fix crash when publishing to a long-polling client that is closing.
  * More conservative message_rate default.

v. 1.13.0 (2016-10-22)

  * Optimizations for higher concurrent connections.
  * New config options: message_rate and message_hwm.
  * New stats message: report.
  * Handle next links internally if relative.
  * Log accepted requests as "accept", not "hold".
  * Log handler-initiated requests in handler, not proxy.
  * Fix memory leaks.
  * Send anonymous usage statistics to Fanout.

v. 1.12.0 (2016-09-03)

  * "GRIP Next" feature for streaming many responses as a single response.
  * header route parameter for sending custom headers when proxying.
  * trust_connect_host target parameter for trusting cert of connect host.
  * SockJS: fix bug with not receiving messages from client.
  * More correct handling of Host header.
  * Set X-Forwarded-Proto in addition to X-Forwarded-Protocol.
  * Various bugfixes.

v. 1.11.0 (2016-07-11)

  * Debug mode, to get more information about errors while proxying.
  * Command line option to log subprocess output: --merge-output.
  * Command line option to log merged output to file: --logfile.
  * Command line options for quick config: --port, --route.
  * Command line option to easily run multiple instances: --id.
  * Rewrite runner from Python to C++.
  * Don't relay Content-Encoding (fixes compressed long-polling timeouts).
  * Fixes to log output.

v. 1.10.1 (2016-05-30)

  * Fix SockJS crash.
  * Fix bug that logged successful requests as errors.

v. 1.10.0 (2016-05-25)

  * Streaming: initial response now has no size limit.
  * WebSocket-Over-HTTP: retry requests to the origin server.
  * WebSocket: ability to disconnect clients by publishing a close action.
  * WebSocket: ability to publish ping/pong frames.
  * WebSocket: keep-alives.
  * New route target "test", for testing without an origin server.
  * Fix publishing of large payloads through HTTP control port.
  * New config option: log_level.
  * Ability to set bind interface in config (use addr:port form).
  * Grip-Status header, for setting alternate response code and reason.

v. 1.9.0 (2016-04-14)

  * More practical logging. Non-verbose output more informative.
  * New config option: accept_x_forwarded_protocol.
  * Support JSON responses in HTTP control endpoint.
  * More accurate WebSocket activity counting.

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
