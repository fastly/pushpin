#!/bin/sh
set -e

if [ $# -lt 1 ]; then
	echo "usage: $0 [dest dir]"
	exit 1
fi

if [ ! -d $1 ]; then
	echo "$1 is not a directory"
	exit 1
fi

dest="$1"

mkdir -p \
	"$dest/usr/share/doc/pushpin" \
	"$dest/usr/share/man/man1"

gzip -c README.md >"$dest/usr/share/doc/pushpin/README.md.gz"
gzip -c packaging/NEWS >"$dest/usr/share/doc/pushpin/NEWS.Debian.gz"
gzip -c packaging/changelog >"$dest/usr/share/doc/pushpin/changelog.Debian.gz"

gzip -c packaging/pushpin.1 >"$dest/usr/share/man/man1/pushpin.1.gz"
gzip -c packaging/pushpin-connmgr.1 >"$dest/usr/share/man/man1/pushpin-connmgr.1.gz"
gzip -c packaging/pushpin-proxy.1 >"$dest/usr/share/man/man1/pushpin-proxy.1.gz"
gzip -c packaging/pushpin-handler.1 >"$dest/usr/share/man/man1/pushpin-handler.1.gz"
gzip -c packaging/m2adapter.1 >"$dest/usr/share/man/man1/m2adapter.1.gz"
gzip -c packaging/pushpin-publish.1 >"$dest/usr/share/man/man1/pushpin-publish.1.gz"
