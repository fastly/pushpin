<?php header('Content-Type:application/grip-instruct'); ?>
{
  "hold": {
    "mode": "stream",
    "channels": [
      {
        "name": "test"
      }
    ]
  },
  "response": {
    "headers": {
      "Content-Type": "text/plain"
    },
    "body": "[stream open]\n"
  }
}
