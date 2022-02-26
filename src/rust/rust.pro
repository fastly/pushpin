TEMPLATE = aux
OBJECTS_DIR = ./
DESTDIR = ./
CONFIG -= debug_and_release debug

include($$OUT_PWD/../../conf.pri)

bin_dir = $$PWD/../../bin
root_dir = $$PWD/../..

CONFIG(debug, debug|release) {
	cargo_flags =
	target_dir = $$PWD/../../target/debug
} else {
	cargo_flags = --release
	target_dir = $$PWD/../../target/release
}

rust_build.commands = cd "$$root_dir" && cargo build --offline $$cargo_flags

publish_build.target = $$target_dir/pushpin-publish
publish_build.depends = rust_build
publish_build.commands = @:

publish_bin.target = $$bin_dir/pushpin-publish
publish_bin.depends = publish_build
publish_bin.commands = mkdir -p $$bin_dir && cp -a $$target_dir/pushpin-publish $$bin_dir/pushpin-publish

QMAKE_EXTRA_TARGETS += \
	rust_build \
	publish_build \
	publish_bin

PRE_TARGETDEPS += \
	$$bin_dir/pushpin-publish

unix:!isEmpty(BINDIR) {
	binfiles.path = $$BINDIR
	binfiles.files = \
		$$bin_dir/pushpin-publish
	binfiles.CONFIG += no_check_exist executable

	INSTALLS += binfiles
}
