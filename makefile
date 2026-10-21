#!/usr/bin/make -f
# Top-level convenience makefile (spec 1.2): builds the app and can produce a .deb.

.PHONY: all build deb clean

all: build

build:
mkdir -p build
cd build && cmake .. -DCMAKE_BUILD_TYPE=Release
cd build && make -j$$(nproc)

deb:
dpkg-buildpackage -us -uc -b

clean:
rm -rf build obj-*
-dh_clean
