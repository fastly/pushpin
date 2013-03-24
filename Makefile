all: pushpin

clean:
	if [ -f proxy/Makefile ]; then cd proxy && make clean; fi

distclean:
	if [ -f proxy/Makefile ]; then cd proxy && make distclean; fi
	rm -f proxy/conf.pri

pushpin: proxy/pushpin-proxy

proxy/pushpin-proxy: proxy/conf.pri
	cd proxy && make

proxy/conf.pri:
	cd proxy && ./configure
