#!/bin/sh
set -e

if [ $# -lt 2 ]; then
	echo "usage: $0 [channel] [content]"
	exit 1
fi

channel=$1
content=$2

curl -d "{ \"items\": [ { \"channel\": \"$channel\", \"formats\": { \"http-response\": { \"body\": \"$content\\n\" }, \"http-stream\": {\"content\": \"$content\\n\" }, \"ws-message\": {\"content\": \"$content\" } } } ] }" http://localhost:5561/publish/
