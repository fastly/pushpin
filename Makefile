ifdef RELEASE
cargo_flags = --offline --locked --release
else
cargo_flags =
endif

ifdef TOOLCHAIN
CARGO_TOOLCHAIN=+$(TOOLCHAIN)
else 
CARGO_TOOLCHAIN = 
endif

all: postbuild

build: FORCE
	cargo $(CARGO_TOOLCHAIN) build $(cargo_flags)

cargo-test: FORCE
	cargo $(CARGO_TOOLCHAIN) test $(cargo_flags) --all-features

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
