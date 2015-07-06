Pushpin Changelog
=================

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
