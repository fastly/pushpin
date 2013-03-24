all: make-pushpin-proxy

clean:
	if [ -f proxy/Makefile ]; then cd proxy && make clean; fi

distclean:
	if [ -f proxy/Makefile ]; then cd proxy && make distclean; fi
	rm -f proxy/conf.pri

make-pushpin-proxy: proxy/conf.pri
	cd proxy && make

proxy/conf.pri:
	cd proxy && ./configure
