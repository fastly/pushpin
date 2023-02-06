Condure Changelog
=================

v. 1.9.2 (2023-02-06)

  * Fix WebSocket compression with fragmented messages.

v. 1.9.1 (2023-01-19)

  * Fix crash in stream connection handler.

v. 1.9.0 (2022-12-05)

  * Support permessage-deflate WebSocket compression.

v. 1.8.0 (2022-11-03)

  * Compatibility with httparse 1.8.
  * Add more benchmarks.

v. 1.7.0 (2022-08-18)

  * Fix worker thread hang when backend buffer is full.

v. 1.6.0 (2022-04-15)

  * Significantly reduce connection memory usage.
  * Allow up to 64 headers in requests and responses.
  * Allow WebSocket requests that include a Content-Length of 0.

v. 1.5.0 (2022-03-11)

  * Ability to listen on a Unix socket for client connections.

v. 1.4.1 (2021-10-24)

  * Fix crash when sending too fast to clients.

v. 1.4.0 (2021-10-22)

  * Port connection handler to use async functions.

v. 1.3.1 (2021-08-11)

  * Fixes for high load.

v. 1.3.0 (2021-07-29)

  * Port to async/await.

v. 1.2.0 (2021-05-04)

  * Send PING/PONG frame data to clients.
  * Port to mio 0.7.

v. 1.1.0 (2020-11-02)

  * TLS support.
  * Don't preallocate connection buffers.
  * Start using async/await in some places.

v. 1.0.1 (2020-07-24)

  * Remove some unsafe usage.

v. 1.0.0 (2020-07-21)

  * Stable version.
