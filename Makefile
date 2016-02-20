prefix = /usr/local
varprefix = /var
configdir = $(prefix)/etc/pushpin
bindir = $(prefix)/bin
libdir = $(prefix)/lib/pushpin
rundir = $(varprefix)/run/pushpin
logdir = $(varprefix)/log/pushpin
version = $(shell sed -e "s/@@DATE@@/`date +%Y%m%d`/g" version)

CHK_DIR_EXISTS  = test -d
COPY            = cp -f
COPY_DIR        = $(COPY) -r
INSTALL_FILE    = install -m 644 -p
INSTALL_DIR     = $(COPY_DIR)
INSTALL_PROGRAM = install -m 755 -p
MKDIR           = mkdir -p
STRIP           = strip

ifdef $($(shell sh ./init.sh))
endif

all: make-m2adapter make-pushpin-proxy make-pushpin-handler pushpin.inst make-pushpin-publish

clean:
	if [ -f m2adapter/Makefile ]; then cd m2adapter && make clean; fi
	rm -f m2adapter/version.h
	if [ -f proxy/Makefile ]; then cd proxy && make clean; fi
	rm -f proxy/version.h
	if [ -f handler/Makefile ]; then cd handler && make clean; fi
	rm -f handler/version.h
	if [ -f tools/publish/Makefile ]; then cd tools/publish && make clean; fi
	rm -f tools/publish/version.h

distclean:
	if [ -f m2adapter/Makefile ]; then cd m2adapter && make distclean; fi
	rm -f m2adapter/conf.pri m2adapter/conf.log
	rm -f m2adapter/version.h
	if [ -f proxy/Makefile ]; then cd proxy && make distclean; fi
	rm -f proxy/conf.pri proxy/conf.log
	rm -f proxy/version.h
	if [ -f handler/Makefile ]; then cd handler && make distclean; fi
	rm -f handler/conf.pri handler/conf.log
	rm -f handler/version.h
	rm -f pushpin.inst
	if [ -f tools/publish/Makefile ]; then cd tools/publish && make distclean; fi
	rm -f tools/publish/version.h

make-m2adapter: m2adapter/version.h m2adapter/Makefile
	cd m2adapter && make

make-pushpin-proxy: proxy/version.h proxy/Makefile
	cd proxy && make

make-pushpin-handler: handler/version.h handler/Makefile
	cd handler && make

make-pushpin-publish: tools/publish/version.h tools/publish/Makefile
	cd tools/publish && make

m2adapter/version.h: version
	echo "#define VERSION \"$(version)\"" > m2adapter/version.h

m2adapter/Makefile:
	cd m2adapter && ./configure

proxy/version.h: version
	echo "#define VERSION \"$(version)\"" > proxy/version.h

proxy/Makefile:
	cd proxy && ./configure

handler/version.h: version
	echo "#define VERSION \"$(version)\"" > handler/version.h

handler/Makefile:
	cd handler && ./configure

tools/publish/version.h: version
	echo "#define VERSION \"$(version)\"" > tools/publish/version.h

tools/publish/Makefile:
	cd tools/publish && ./configure

pushpin.inst: pushpin version
	sed -e "s,^default_libdir = .*,default_libdir = \'$(libdir)\',g" pushpin | sed -e "s,^default_configdir =.*,default_configdir = \"$(configdir)\",g" | sed -e "s,^version =.*,version = \"$(version)\",g" > pushpin.inst && chmod 755 pushpin.inst

check:
	cd proxy && make check && cd ..
	cd handler && make check

install:
	@$(CHK_DIR_EXISTS) $(INSTALL_ROOT)$(bindir) || $(MKDIR) $(INSTALL_ROOT)$(bindir)
	@$(CHK_DIR_EXISTS) $(INSTALL_ROOT)$(configdir) || $(MKDIR) $(INSTALL_ROOT)$(configdir)
	@$(CHK_DIR_EXISTS) $(INSTALL_ROOT)$(configdir)/runner || $(MKDIR) $(INSTALL_ROOT)$(configdir)/runner
	@$(CHK_DIR_EXISTS) $(INSTALL_ROOT)$(configdir)/runner/certs || $(MKDIR) $(INSTALL_ROOT)$(configdir)/runner/certs
	@$(CHK_DIR_EXISTS) $(INSTALL_ROOT)$(libdir)/runner || $(MKDIR) $(INSTALL_ROOT)$(libdir)/runner
	-$(INSTALL_PROGRAM) m2adapter/m2adapter "$(INSTALL_ROOT)$(bindir)/m2adapter"
	-$(STRIP) "$(INSTALL_ROOT)$(bindir)/m2adapter"
	-$(INSTALL_PROGRAM) proxy/pushpin-proxy "$(INSTALL_ROOT)$(bindir)/pushpin-proxy"
	-$(STRIP) "$(INSTALL_ROOT)$(bindir)/pushpin-proxy"
	-$(INSTALL_PROGRAM) handler/pushpin-handler "$(INSTALL_ROOT)$(bindir)/pushpin-handler"
	-$(STRIP) "$(INSTALL_ROOT)$(bindir)/pushpin-handler"
	-$(INSTALL_PROGRAM) pushpin.inst $(INSTALL_ROOT)$(bindir)/pushpin
	-$(INSTALL_PROGRAM) tools/publish/pushpin-publish "$(INSTALL_ROOT)$(bindir)/pushpin-publish"
	-$(STRIP) "$(INSTALL_ROOT)$(bindir)/pushpin-publish"
	$(COPY) runner/*.py $(INSTALL_ROOT)$(libdir)/runner
	$(COPY) runner/*.template $(INSTALL_ROOT)$(configdir)/runner
	sed -e "s,libdir=.*,libdir=$(libdir),g" -e "s,configdir=.*,configdir=$(configdir)/runner,g" -e "s,rundir=.*,rundir=$(rundir),g" -e "s,logdir=.*,logdir=$(logdir),g" examples/config/internal.conf > $(INSTALL_ROOT)$(configdir)/internal.conf
	test -e $(INSTALL_ROOT)$(configdir)/pushpin.conf || sed -e "s,libdir=.*,libdir=$(libdir),g" -e "s,configdir=.*,configdir=$(configdir)/runner,g" -e "s,rundir=.*,rundir=$(rundir),g" -e "s,logdir=.*,logdir=$(logdir),g" examples/config/pushpin.conf > $(INSTALL_ROOT)$(configdir)/pushpin.conf
	test -e $(INSTALL_ROOT)$(configdir)/routes || cp examples/config/routes $(INSTALL_ROOT)$(configdir)/routes
