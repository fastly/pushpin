prefix = /usr/local
varprefix = /var/local
configdir = $(prefix)/etc/pushpin
bindir = $(prefix)/bin
libdir = $(prefix)/lib/pushpin
rundir = $(varprefix)/run/pushpin
logdir = $(varprefix)/log/pushpin

all: make-m2adapter make-pushpin-proxy

clean:
	if [ -f m2adapter/Makefile ]; then cd m2adapter && make clean; fi
	if [ -f proxy/Makefile ]; then cd proxy && make clean; fi

distclean:
	if [ -f m2adapter/Makefile ]; then cd m2adapter && make distclean; fi
	rm -f m2adapter/conf.pri m2adapter/conf.log
	if [ -f proxy/Makefile ]; then cd proxy && make distclean; fi
	rm -f proxy/conf.pri proxy/conf.log

make-m2adapter: m2adapter/conf.pri
	cd m2adapter && make

make-pushpin-proxy: proxy/conf.pri
	cd proxy && make

m2adapter/conf.pri:
	cd m2adapter && ./configure

proxy/conf.pri:
	cd proxy && ./configure

install:
	mkdir -p $(bindir)
	mkdir -p $(configdir)
	mkdir -p $(configdir)/runner
	mkdir -p $(configdir)/runner/certs
	mkdir -p $(libdir)/handler
	mkdir -p $(libdir)/runner
	mkdir -p $(rundir)
	mkdir -p $(logdir)
	cp m2adapter/m2adapter $(bindir)
	cp proxy/pushpin-proxy $(bindir)
	cp handler/pushpin-handler $(bindir)
	cp handler/*.py $(libdir)/handler
	cp runner/*.py $(libdir)/runner
	cp runner/*.conf runner/*.template $(configdir)/runner
	sed -e "s,^default_config_dir =.*,default_config_dir = \"$(configdir)\",g" pushpin > $(bindir)/pushpin
	test -e $(configdir)/pushpin.conf || sed -e "s,libdir=.*,libdir=$(libdir),g" -e "s,configdir=.*,configdir=$(configdir)/runner,g" -e "s,rundir=.*,rundir=$(rundir),g" -e "s,logdir=.*,logdir=$(logdir),g" config/pushpin.conf.example > $(configdir)/pushpin.conf
	test -e $(configdir)/routes || cp config/routes.example $(configdir)/routes
