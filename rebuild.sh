#!/bin/sh

#rm -rf ./build
make clean
# git pull
cmake -DCMAKE_PREFIX_PATH=/usr -S . -B build
#cmake --build build
make
# ./build/audioscout
