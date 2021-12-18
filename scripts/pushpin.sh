#!/bin/sh

unshare -m --mount-proc --propagation=slave /opt/fst-pushpin/bin/pushpin-starter.sh
