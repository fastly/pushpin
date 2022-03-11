Pushpin Changelog
=================

v. 1.35.0 (2022-03-11)

  * Add support for Prometheus metrics.
  * Ability to listen on a Unix socket for client connections.
  * New config options: prometheus_port, prometheus_prefix.
  * New config option: local_ports.
  * New config option: accept_pushpin_route.
  * New route condition option: no_grip.
  * Use the route of the initial request for retries and link requests.
  * pushpin-publish: fix sending hint action for http-response format.

v. 1.34.0 (2021-11-30)

  * New config option: message_wait.
  * Publish command for publishing via command socket.

v. 1.33.1 (2021-08-09)

  * Build system fixes.

v. 1.33.0 (2021-08-08)

  * Performance optimizations.
  * New config option: sig_iss.

v. 1.32.2 (2021-06-09)

  * Fix publishing to SockJS WebSocket connections.

v. 1.32.1 (2021-05-13)

  * Build system fixes.

v. 1.32.0 (2021-05-11)

  * pushpin-publish: support sending via HTTP, and do this by default.
  * pushpin-publish: support authentication.
  * pushpin-publish: use GRIP_URL environment variable if present.
  * Add Rust code to the build process.

v. 1.31.0 (2020-11-06)

  * Use Condure instead of Mongrel2, by default.
  * Ability to refresh WebSocket-over-HTTP sessions by channel.
  * Fix crash when sending delayed WebSocket messages.

v. 1.30.0 (2020-07-29)

  * Optional support for Condure instead of Mongrel2.
  * ZHTTP compatibility fixes.

v. 1.29.0 (2020-07-15)

  * Fix crash when parsing Accept header received on control port.
  * Fix crash when response hold times out while pausing.
  * Fix handling of hints in response mode.
  * Fix handling of ZeroMQ errors, including EINTR.
  * ZHTTP compatibility fixes.

v. 1.28.0 (2020-04-08)

  * New route target option: one_event.

v. 1.27.0 (2020-03-10)

  * WebSocket: ability to publish close reason.
  * WebSocket: proxy the content of ping and pong frames.

v. 1.26.0 (2019-12-11)

  * Respond with status 200 on HTTP control port root path.

v. 1.25.0 (2019-11-20)

  * Set the Mongrel2 log level and capture debug output.
  * Ability to set different log levels per subprocess.

v. 1.24.0 (2019-08-06)

  * runner: capture Mongrel2 logs when --merge-output is used.

v. 1.23.0 (2019-07-03)

  * Support log levels 0 and 1.
  * Don't write to Mongrel2 access log for log levels < 2.
  * Support JSON framing on the input PULL and SUB sockets.
  * New config option: push_in_sub_specs.
  * New config option: push_in_sub_connect.

v. 1.22.0 (2019-06-17)

  * New filter: var-subst.
  * Support content-filters field in ws-message format.

v. 1.21.0 (2019-05-01)

  * GRIP keep-alive modes: idle (default) and interval.
  * Don't put GRIP headers in Access-Control-Expose-Headers.

v. 1.20.3 (2019-04-08)

  * Fix Grip-Last values when route prefix is used.

v. 1.20.2 (2019-03-25)

  * WebSocket-Over-HTTP: fix mem leak when clients disconnect during close.

v. 1.20.1 (2019-02-20)

  * WebSocket-Over-HTTP: don't forward Content-Length header.

v. 1.20.0 (2019-02-19)

  * WebSocket-Over-HTTP: break up response messages to fit session buffers.
  * New config option: stats_format.
  * New config option: client_buffer_size.

v. 1.19.1 (2019-01-10)

  * WebSocket: fix crash when receiving frames after close frame.
  * WebSocket: include reason and headers in rejection responses.

v. 1.19.0 (2018-12-18)

  * WebSocket: support close reasons.

v. 1.18.0 (2018-08-20)

  * WebSocket-Over-HTTP: update headers (mainly Grip-Sig) for each request.
  * WebSocket-Over-HTTP: properly report errors and handle target failover.
  * WebSocket: support debug responses.
  * Option to not send non-standard X-Forwarded-Protocol header.
  * Increase default request buffer size to 8k.
  * Make http_port optional.
  * runner: remove mongrel2 pid file before starting.
  * runner: return non-zero status code if failing due to subprocess error.
  * runner: prevent SIGINT from being copied to subprocesses.

v. 1.17.2 (2018-01-11)

  * Fix close actions with HTTP streaming and WebSockets.

v. 1.17.1 (2017-12-12)

  * Fix compilation with Qt 5.10.

v. 1.17.0 (2017-11-06)

  * De-dup published messages based on recently seen IDs (default 60s).
  * Limit number of subscriptions per connection (default 20).
  * Ensure filters update after following next links.
  * Support content-filters field in http-stream and http-response formats.
  * Include subscribers field in subscription stats.
  * Include duration field in report stats.
  * New config options: connection_subscription_max, subscription_linger.
  * New config options: stats_connection_ttl, stats_subscription_ttl.
  * New config option: stats_report_interval.

v. 1.16.0 (2017-07-14)

  * Reliable streaming fixes.
  * SockJS: XHR transport fixes.
  * WebSocket-Over-HTTP: more fixes to ensure DISCONNECT events get sent.
  * Fix routes file change detection when file is replaced.
  * Set Grip-Last headers when retrying long-polling request.
  * Enable client-side TCP keep-alives.
  * Stats: report logical IP address rather than physical.
  * Published items can include no-seq flag to bypass sequencing buffer.
  * New config options: log_from, log_user_agent.
  * New filters: skip-users, build-id, require-sub.
  * Add randomness to stream keep alives.
  * pushpin-publish: --meta option.
  * pushpin-publish: --no-seq option.
  * Announce more features using Grip-Feature request header.
  * Fix GRIP session detection.
  * sub target parameter works for both HTTP and WebSocket, forbids unsub.
  * Packet logging uses new format that only trims content, not headers.

v. 1.15.0 (2017-01-22)

  * Publish hint action for triggering recovery requests.
  * Recover command for triggering recovery requests.
  * Refresh command for triggering WebSocket-Over-HTTP requests.
  * Improve reliability of long-polling when previous ID is used.
  * WebSocket-Over-HTTP: ensure DISCONNECT events get sent.
  * WebSocket: new control messages: send-delayed, flush-delayed.
  * WebSocket: break large published messages into frames.
  * Allow unknown previous ID for first message to channel.
  * Forget previous ID when channel has no subscribers.
  * Reduce timeout of out-of-order messages to 5 seconds.
  * pushpin-publish: --hint option
  * pushpin-publish: --no-eol option
  * pushpin-publish: ability to use file source (@filename)
  * New config option: message_block_size
  * Remove docs files from repository. Content moved to pushpin.org.

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
