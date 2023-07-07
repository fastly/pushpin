CONFIG(debug, debug|release) {
	RUST_BUILD_DIR = $$PWD/../../target/debug
} else {
	RUST_BUILD_DIR = $$PWD/../../target/release
}

LIBS += -L$$RUST_BUILD_DIR -lpushpin -lssl -lcrypto

unix:!mac: LIBS += -ldl -lrt
mac:LIBS += -framework CoreFoundation -framework Security

PRE_TARGETDEPS += $$RUST_BUILD_DIR/libpushpin.a
