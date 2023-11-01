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

condure_build.target = $$target_dir/condure
condure_build.depends = rust_build
condure_build.commands = @:

pushpin_build.target = $$target_dir/pushpin
pushpin_build.depends = rust_build
pushpin_build.commands = @:

publish_build.target = $$target_dir/pushpin-publish
publish_build.depends = rust_build
publish_build.commands = @:

condure_bin.target = $$bin_dir/condure
condure_bin.depends = condure_build
condure_bin.commands = mkdir -p $$bin_dir && cp -a $$target_dir/condure $$bin_dir/condure

pushpin_bin.target = $$root_dir/pushpin
pushpin_bin.depends = pushpin_build
pushpin_bin.commands = cp -a $$target_dir/pushpin $$root_dir/pushpin

publish_bin.target = $$bin_dir/pushpin-publish
publish_bin.depends = publish_build
publish_bin.commands = mkdir -p $$bin_dir && cp -a $$target_dir/pushpin-publish $$bin_dir/pushpin-publish


QMAKE_EXTRA_TARGETS += \
	rust_build \
	condure_build \
	pushpin_build \
	publish_build \
	condure_bin \
	pushpin_bin \
	publish_bin

PRE_TARGETDEPS += \
	$$bin_dir/condure \
	$$root_dir/pushpin \
	$$bin_dir/pushpin-publish

unix:!isEmpty(BINDIR) {
	binfiles.path = $$BINDIR
	binfiles.files = \
		$$bin_dir/condure \
		$$root_dir/pushpin \
		$$bin_dir/pushpin-publish
	binfiles.CONFIG += no_check_exist executable

	INSTALLS += binfiles
}
