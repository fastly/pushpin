# controlled leading whitespace, per the GNU make manual
nullstring :=
space := $(nullstring) # end of the line

ifdef RELEASE
cargo_flags = $(space)--offline --locked --release
endif

ifdef TOOLCHAIN
cargo_toolchain = $(space)+$(TOOLCHAIN)
endif

all: postbuild

build: FORCE
	cargo$(cargo_toolchain) build$(cargo_flags)

cargo-test: FORCE
	cargo$(cargo_toolchain) test$(cargo_flags) --all-features -- --test-threads=1

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
