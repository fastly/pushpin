ifdef RELEASE
cargo_flags = --offline --release
else
cargo_flags =
endif

all: build

build:
	cd .. && cargo build $(cargo_flags)

clean:
	cd .. && cargo clean

distclean: clean

check:
	cd .. && cargo test $(cargo_flags)
