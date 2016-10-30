#!/bin/sh
g++ --std=c++11 -O2 -o ndiflbridge \
  -pthread -lrt \
  `pkg-config --libs --cflags avahi-client` \
  -I $NDI_INCLUDE \
  ndiflbridge.cpp $NDI_LIB/libndi.a
