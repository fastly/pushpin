#!/bin/sh
set -e

if [ $# -lt 1 ]; then
	echo "usage: $0 [version]"
	exit 1
fi

VERSION=$1

DESTDIR=build/pushpin-$VERSION

mkdir -p $DESTDIR

cp -a .gitignore benches build.rs Cargo.lock Cargo.toml cbindgen.toml CHANGELOG.md examples LICENSE Makefile postbuild SECURITY.md README.md src tools $DESTDIR

sed -i.orig -e "s/^version = .*/version = \"$VERSION\"/g" $DESTDIR/Cargo.toml
rm $DESTDIR/Cargo.toml.orig

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
