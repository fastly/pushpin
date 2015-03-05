#!/bin/sh

if test -d .git && git submodule status | grep '^-'; then
	git submodule init && git submodule update
fi
