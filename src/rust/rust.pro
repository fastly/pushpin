TEMPLATE = aux
CONFIG -= debug_and_release debug

root_dir = $$PWD/../..

CONFIG(debug, debug|release) {
	cargo_flags =
	target_dir = $$root_dir/target/debug
} else {
	cargo_flags = --release
	target_dir = $$root_dir/target/debug
}

check.commands = cd "$$root_dir" && cargo test --offline $$cargo_flags

rust_build.commands = cd "$$root_dir" && cargo build --offline $$cargo_flags

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
	publish_build

# make built-in clean depend on rust_clean
clean.depends = rust_clean
QMAKE_EXTRA_TARGETS += clean

PRE_TARGETDEPS += \
	$$target_dir/condure \
	$$target_dir/m2adapter \
	$$target_dir/pushpin-proxy \
	$$target_dir/pushpin-handler \
	$$target_dir/pushpin-legacy \
	$$target_dir/pushpin \
	$$target_dir/pushpin-publish
