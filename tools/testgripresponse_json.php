<?php header('Content-Type: application/grip-instruct'); ?>
{
  "hold": {
    "mode": "response",
    "channels": [
      {
        "name": "test"
      }
    ],
    "timeout": 55
  },
  "response": {
    "headers": {
      "Content-Type": "text/plain"
    },
    "body": "nothing for now\n"
  }
}
