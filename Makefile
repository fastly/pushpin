ifdef RELEASE
cargo_flags = --offline --release
else
cargo_flags =
endif

all: postbuild

build: FORCE
	cargo build $(cargo_flags)

cargo-test: FORCE
	cargo test $(cargo_flags)

cargo-clean: FORCE
	cargo clean

postbuild: build FORCE
	cd postbuild && $(MAKE) -f Makefile

postbuild-install: FORCE
	cd postbuild && $(MAKE) -f Makefile install

postbuild-clean: FORCE
	cd postbuild && $(MAKE) -f Makefile clean

postbuild-distclean: FORCE
	cd postbuild && $(MAKE) -f Makefile distclean

check: cargo-test

install: postbuild-install

clean: cargo-clean postbuild-clean

distclean: cargo-clean postbuild-distclean

FORCE:
