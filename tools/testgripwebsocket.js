const http = require('node:http');

const hostname = '127.0.0.1';
const port = 3000;

const server = http.createServer();

server.on('request', (req, res) => {
  console.log('Got request: ' + req.toString());
  var sub = false;
  var body = '';
  req.on('readable', () => {
    var bodyPart = req.read();
    if (bodyPart) {
      body += bodyPart;
    }
  }).on('end', () => {
    if (body && body.substring(0,6) === 'OPEN\r\n') {
      sub = true;
      res.setHeader('Sec-WebSocket-Extensions', 'grip; message-prefix=""');
    }

    res.statusCode = 200;
    res.setHeader('Content-Type', 'application/websocket-events');

    if (body !== null) {
      res.write(body);
    }

    if (sub) {
      var msg = 'c:{"type": "subscribe", "channel": "test"}';
      var out = 'TEXT ' + msg.length.toString(16) + '\r\n' + msg + '\r\n';
      res.write(out);
    }

    res.end();
  });
});

server.listen(port, hostname, () => {
  console.log(`Server running at http://${hostname}:${port}/`);
});
