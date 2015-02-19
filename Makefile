prefix = /usr/local
varprefix = /var
configdir = $(prefix)/etc/pushpin
bindir = $(prefix)/bin
libdir = $(prefix)/lib/pushpin
rundir = $(varprefix)/run/pushpin
logdir = $(varprefix)/log/pushpin

CHK_DIR_EXISTS  = test -d
COPY            = cp -f
COPY_DIR        = $(COPY) -r
INSTALL_FILE    = install -m 644 -p
INSTALL_DIR     = $(COPY_DIR)
INSTALL_PROGRAM = install -m 755 -p
MKDIR           = mkdir -p
STRIP           = strip

all: make-m2adapter make-pushpin-proxy pushpin.inst

clean:
	if [ -f m2adapter/Makefile ]; then cd m2adapter && make clean; fi
	if [ -f proxy/Makefile ]; then cd proxy && make clean; fi

distclean:
	if [ -f m2adapter/Makefile ]; then cd m2adapter && make distclean; fi
	rm -f m2adapter/conf.pri m2adapter/conf.log
	if [ -f proxy/Makefile ]; then cd proxy && make distclean; fi
	rm -f proxy/conf.pri proxy/conf.log
	rm -f pushpin.inst

make-m2adapter: m2adapter/Makefile
	cd m2adapter && make

make-pushpin-proxy: proxy/Makefile
	cd proxy && make

m2adapter/Makefile:
	cd m2adapter && ./configure

proxy/Makefile:
	cd proxy && ./configure

pushpin.inst: pushpin
	sed -e "s,^default_config_dir =.*,default_config_dir = \"$(configdir)\",g" pushpin > pushpin.inst && chmod 755 pushpin.inst

check:
	cd proxy && make check

install:
	@$(CHK_DIR_EXISTS) $(INSTALL_ROOT)$(bindir) || $(MKDIR) $(INSTALL_ROOT)$(bindir)
	@$(CHK_DIR_EXISTS) $(INSTALL_ROOT)$(configdir) || $(MKDIR) $(INSTALL_ROOT)$(configdir)
	@$(CHK_DIR_EXISTS) $(INSTALL_ROOT)$(configdir)/runner || $(MKDIR) $(INSTALL_ROOT)$(configdir)/runner
	@$(CHK_DIR_EXISTS) $(INSTALL_ROOT)$(configdir)/runner/certs || $(MKDIR) $(INSTALL_ROOT)$(configdir)/runner/certs
	@$(CHK_DIR_EXISTS) $(INSTALL_ROOT)$(libdir)/handler || $(MKDIR) $(INSTALL_ROOT)$(libdir)/handler
	@$(CHK_DIR_EXISTS) $(INSTALL_ROOT)$(libdir)/runner || $(MKDIR) $(INSTALL_ROOT)$(libdir)/runner
	-$(INSTALL_PROGRAM) m2adapter/m2adapter "$(INSTALL_ROOT)$(bindir)/m2adapter"
	-$(STRIP) "$(INSTALL_ROOT)$(bindir)/m2adapter"
	-$(INSTALL_PROGRAM) proxy/pushpin-proxy "$(INSTALL_ROOT)$(bindir)/pushpin-proxy"
	-$(STRIP) "$(INSTALL_ROOT)$(bindir)/pushpin-proxy"
	-$(INSTALL_PROGRAM) handler/pushpin-handler "$(INSTALL_ROOT)$(bindir)/pushpin-handler"
	-$(INSTALL_PROGRAM) pushpin.inst $(INSTALL_ROOT)$(bindir)/pushpin
	$(COPY) handler/*.py $(INSTALL_ROOT)$(libdir)/handler
	$(COPY) runner/*.py $(INSTALL_ROOT)$(libdir)/runner
	$(COPY) runner/*.template $(INSTALL_ROOT)$(configdir)/runner
	sed -e "s,libdir=.*,libdir=$(libdir),g" -e "s,configdir=.*,configdir=$(configdir)/runner,g" -e "s,rundir=.*,rundir=$(rundir),g" -e "s,logdir=.*,logdir=$(logdir),g" config/internal.conf > $(INSTALL_ROOT)$(configdir)/internal.conf
	test -e $(INSTALL_ROOT)$(configdir)/pushpin.conf || sed -e "s,libdir=.*,libdir=$(libdir),g" -e "s,configdir=.*,configdir=$(configdir)/runner,g" -e "s,rundir=.*,rundir=$(rundir),g" -e "s,logdir=.*,logdir=$(logdir),g" config/pushpin.conf > $(INSTALL_ROOT)$(configdir)/pushpin.conf
	test -e $(INSTALL_ROOT)$(configdir)/routes || cp config/routes $(INSTALL_ROOT)$(configdir)/routes
