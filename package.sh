#!/bin/sh
set -e

if [ $# -lt 1 ]; then
	echo "usage: $0 [version]"
	exit 1
fi

VERSION=$1

mkdir -p build/pushpin-$VERSION
cp -a .gitignore CHANGELOG.md configure COPYING corelib doc examples handler m2adapter Makefile proxy pushpin pushpin.pro pushpin.qc README.md runner tools build/pushpin-$VERSION
rm -rf build/pushpin-$VERSION/corelib/qzmq/.git build/pushpin-$VERSION/corelib/common/.git
echo $VERSION > build/pushpin-$VERSION/version
cd build
tar jcvf pushpin-$VERSION.tar.bz2 pushpin-$VERSION
