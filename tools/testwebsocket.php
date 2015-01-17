<?php

$body = @file_get_contents('php://input');

header('Content-Type: application/websocket-events');

print $body;

?>
