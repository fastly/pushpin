ifdef RELEASE
cargo_flags = --offline --release
else
cargo_flags =
endif

all: postbuild

build:
	cargo build $(cargo_flags)

cargo-test:
	cargo test $(cargo_flags)

cargo-clean:
	cargo clean

postbuild: build FORCE
	cd postbuild && $(MAKE) -f Makefile

postbuild-install:
	cd postbuild && $(MAKE) -f Makefile install

postbuild-clean:
	cd postbuild && $(MAKE) -f Makefile clean

postbuild-distclean:
	cd postbuild && $(MAKE) -f Makefile distclean

check: cargo-test

install: postbuild-install

clean: cargo-clean postbuild-clean

distclean: cargo-clean postbuild-distclean

FORCE:
