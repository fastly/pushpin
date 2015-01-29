<?php

$body = @file_get_contents('php://input');

header('Content-Type: application/websocket-events');

if (substr($body, 0, 6) === "OPEN\r\n") {
    header('Sec-WebSocket-Extensions: grip; message-prefix=""');
    $sub = True;
} else {
    $sub = False;
}

print $body;

if ($sub) {
    $msg = 'c:{"type": "subscribe", "channel": "test"}';
    print 'TEXT ' . dechex(strlen($msg)) . "\r\n" . $msg . "\r\n";
}

?>
