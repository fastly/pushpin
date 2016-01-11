#!/bin/sh
set -e

if [ $# -lt 1 ]; then
	echo "usage: $0 [version]"
	exit 1
fi

VERSION=$1

mkdir -p build/pushpin-$VERSION
cp -a .gitignore CHANGELOG.md COPYING corelib doc examples handler init.sh m2adapter Makefile proxy pushpin README.md runner tools build/pushpin-$VERSION
rm -rf build/pushpin-$VERSION/qzmq/.git build/pushpin-$VERSION/common/.git
echo $VERSION > build/pushpin-$VERSION/version
cd build
tar jcvf pushpin-$VERSION.tar.bz2 pushpin-$VERSION
