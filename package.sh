#!/bin/sh
set -e

if [ $# -lt 1 ]; then
	echo "usage: $0 [version]"
	exit 1
fi

VERSION=$1

mkdir -p build/pushpin-$VERSION
cp -a .gitignore Cargo.lock Cargo.toml CHANGELOG.md configure COPYING examples pushpin.pro pushpin.qc qcm README.md src tools build/pushpin-$VERSION
rm -rf build/pushpin-$VERSION/src/corelib/qzmq/.git build/pushpin-$VERSION/src/corelib/common/.git
echo $VERSION > build/pushpin-$VERSION/version
cd build/pushpin-$VERSION
mkdir -p .cargo
cat >.cargo/config.toml <<EOF
[source.crates-io]
replace-with = "vendored-sources"

[source.vendored-sources]
directory = "vendor"
EOF
cargo vendor
cd ..
tar jcvf pushpin-$VERSION.tar.bz2 pushpin-$VERSION
