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

check.commands = cd "$$root_dir" && cargo test $$cargo_flags

rust_build.commands = cd "$$root_dir" && cargo build $$cargo_flags

rust_clean.commands = cd "$$root_dir" && cargo clean

condure_build.target = $$target_dir/condure
condure_build.depends = rust_build
condure_build.commands = @:

m2adapter_build.target = $$target_dir/m2adapter
m2adapter_build.depends = rust_build
m2adapter_build.commands = @:

proxy_build.target = $$target_dir/pushpin-proxy
proxy_build.depends = rust_build
proxy_build.commands = @:

handler_build.target = $$target_dir/pushpin-handler
handler_build.depends = rust_build
handler_build.commands = @:

runner_legacy_build.target = $$target_dir/pushpin-legacy
runner_legacy_build.depends = rust_build
runner_legacy_build.commands = @:

runner_build.target = $$target_dir/pushpin
runner_build.depends = rust_build
runner_build.commands = @:

publish_build.target = $$target_dir/pushpin-publish
publish_build.depends = rust_build
publish_build.commands = @:

stats_emitter_build.target = $$target_dir/pushpin-stats-emitter
stats_emitter_build.depends = rust_build
stats_emitter_build.commands = @:

condure_bin.target = $$bin_dir/condure
condure_bin.depends = condure_build
condure_bin.commands = mkdir -p $$bin_dir && cp -a $$target_dir/condure $$bin_dir/condure

m2adapter_bin.target = $$bin_dir/m2adapter
m2adapter_bin.depends = m2adapter_build
m2adapter_bin.commands = mkdir -p $$bin_dir && cp -a $$target_dir/m2adapter $$bin_dir/m2adapter

proxy_bin.target = $$bin_dir/pushpin-proxy
proxy_bin.depends = proxy_build
proxy_bin.commands = mkdir -p $$bin_dir && cp -a $$target_dir/pushpin-proxy $$bin_dir/pushpin-proxy

handler_bin.target = $$bin_dir/pushpin-handler
handler_bin.depends = handler_build
handler_bin.commands = mkdir -p $$bin_dir && cp -a $$target_dir/pushpin-handler $$bin_dir/pushpin-handler

runner_legacy_bin.target = $$root_dir/pushpin-legacy
runner_legacy_bin.depends = runner_legacy_build
runner_legacy_bin.commands = cp -a $$target_dir/pushpin-legacy $$root_dir/pushpin-legacy

runner_bin.target = $$root_dir/pushpin
runner_bin.depends = runner_build
runner_bin.commands = cp -a $$target_dir/pushpin $$root_dir/pushpin

publish_bin.target = $$bin_dir/pushpin-publish
publish_bin.depends = publish_build
publish_bin.commands = mkdir -p $$bin_dir && cp -a $$target_dir/pushpin-publish $$bin_dir/pushpin-publish

stats_emitter_bin.target = $$bin_dir/pushpin-stats-emitter
stats_emitter_bin.depends = stats_emitter_build
stats_emitter_bin.commands = mkdir -p $$bin_dir && cp -a $$target_dir/pushpin-stats-emitter $$bin_dir/pushpin-stats-emitter

QMAKE_EXTRA_TARGETS += \
	check \
	rust_build \
	rust_clean \
	condure_build \
	m2adapter_build \
	proxy_build \
	handler_build \
	runner_legacy_build \
	runner_build \
	publish_build \
	stats_emitter_build \
	condure_bin \
	m2adapter_bin \
	proxy_bin \
	handler_bin \
	runner_legacy_bin \
	runner_bin \
	publish_bin \
	stats_emitter_bin

# make built-in clean depend on rust_clean
clean.depends = rust_clean
QMAKE_EXTRA_TARGETS += clean

PRE_TARGETDEPS += \
	$$bin_dir/condure \
	$$bin_dir/m2adapter \
	$$bin_dir/pushpin-proxy \
	$$bin_dir/pushpin-handler \
	$$root_dir/pushpin-legacy \
	$$root_dir/pushpin \
	$$bin_dir/pushpin-publish \
	$$bin_dir/pushpin-stats-emitter

unix:!isEmpty(BINDIR) {
	binfiles.path = $$BINDIR
	binfiles.files = \
		$$bin_dir/condure \
		$$bin_dir/m2adapter \
		$$bin_dir/pushpin-proxy \
		$$bin_dir/pushpin-handler \
		$$root_dir/pushpin-legacy \
		$$root_dir/pushpin \
		$$bin_dir/pushpin-publish \
		$$bin_dir/pushpin-stats-emitter
	binfiles.CONFIG += no_check_exist executable

	INSTALLS += binfiles
}
