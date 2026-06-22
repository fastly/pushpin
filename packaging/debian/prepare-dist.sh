#!/bin/sh
set -e

if [ $# -lt 1 ]; then
	echo "usage: $0 [git ref]"
	exit 1
fi

ref="$1"
src="$PWD/source"
dest="$PWD/dist"

if [ ! -f "packaging/debian/changelog" ]; then
	echo "must be run from the root of the pushpin repo"
	exit 1
fi

if [ -d "$dest" ]; then
	echo "$dest already exists"
	exit 1
fi

git worktree add "$src" "$ref"


export RELEASE=1
export PREFIX=/usr
export CONFIGDIR=/etc
export INSTALL_ROOT="$dest"

(cd "$src" && cargo fetch && make && make install)

mkdir -p \
	"$dest/usr/share/doc/pushpin" \
	"$dest/usr/share/man/man1"

gzip -c "$src/README.md" >"$dest/usr/share/doc/pushpin/README.md.gz"
gzip -c packaging/debian/NEWS >"$dest/usr/share/doc/pushpin/NEWS.Debian.gz"
gzip -c packaging/debian/changelog >"$dest/usr/share/doc/pushpin/changelog.Debian.gz"

gzip -c packaging/debian/pushpin.1 >"$dest/usr/share/man/man1/pushpin.1.gz"
gzip -c packaging/debian/pushpin-connmgr.1 >"$dest/usr/share/man/man1/pushpin-connmgr.1.gz"
gzip -c packaging/debian/pushpin-proxy.1 >"$dest/usr/share/man/man1/pushpin-proxy.1.gz"
gzip -c packaging/debian/pushpin-handler.1 >"$dest/usr/share/man/man1/pushpin-handler.1.gz"
gzip -c packaging/debian/m2adapter.1 >"$dest/usr/share/man/man1/m2adapter.1.gz"
gzip -c packaging/debian/pushpin-publish.1 >"$dest/usr/share/man/man1/pushpin-publish.1.gz"

git worktree remove "$src"

echo "$dest prepared"
