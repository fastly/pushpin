#!/bin/sh

exec /usr/bin/unshare -m -f -p --mount-proc --propagation=slave /opt/fst-pushpin/bin/pushpin-starter.sh
