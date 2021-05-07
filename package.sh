#!/bin/sh
set -e

VERSION=`grep "^version = " Cargo.toml | cut -d ' ' -f 3 | cut -d '"' -f 2`

DESTDIR=build/pushpin-$VERSION

mkdir -p $DESTDIR

cp -a .gitignore build.rs Cargo.lock Cargo.toml CHANGELOG.md configure COPYING examples pushpin.pro pushpin.qc qcm README.md src tools $DESTDIR
rm -rf $DESTDIR/src/corelib/qzmq/.git $DESTDIR/src/corelib/common/.git

cd $DESTDIR
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
