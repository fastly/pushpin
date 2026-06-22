# Pushpin for Debian

Run from the root of the pushpin repo.

```
# install tools
apt install dpkg dpkg-dev devscripts vim rustc cargo
cargo install --locked cargo-deb@2.12.1  # compatible with older rustc in distro

# install pushpin dependencies
apt install zstd git pkg-config curl make g++ libssl-dev libzmq3-dev qt6-base-dev libboost-dev

# if new upstream version, update version in Cargo.toml. leave alone if only bumping package revision
vim Cargo.toml

# if new upstream version, prepend a new entry to the changelog
dch -c packaging/debian/changelog -v 1.41.0-1 "New upstream version"

# build and install pushpin into ./dist from the given git tag
./packaging/debian/prepare-dist.sh v1.41.0

# create deb, specifying distro revision. will output to ./target/debian
# repeat this step for each target distro (e.g. 1~noble1, 1~jammy1)
cargo deb --no-build --deb-revision 1~noble1

# commit the changelog update
git commit packaging/debian/changelog
```
