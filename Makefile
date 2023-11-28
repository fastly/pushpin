all: postbuild

src: FORCE
	cd src && $(MAKE) -f Makefile

postbuild: src FORCE
	cd postbuild && $(MAKE) -f Makefile

src-check:
	cd src && $(MAKE) -f Makefile check

postbuild-install:
	cd postbuild && $(MAKE) -f Makefile install

src-clean:
	cd src && $(MAKE) -f Makefile clean

postbuild-clean:
	cd postbuild && $(MAKE) -f Makefile clean

postbuild-distclean:
	cd postbuild && $(MAKE) -f Makefile distclean

check: src-check

install: postbuild-install

clean: src-clean postbuild-clean

distclean: src-clean postbuild-distclean

FORCE:
