ndiflbridge
===========

`ndiflbridge` provides a bridge between a frame link capable application and
NDI. It supports sending to and receiving from NDI. It also has a helper for
finding available NDI sources on the LAN.

NDI, the Network Device Interface, is a protocol for transporting audio and
video data over a LAN segment, designed for low-latency use in live video
production. See http://www.newtek.com/ndi.html for more information.

To build `ndiflbridge` you need the NDI SDK from NewTek. Access to this can be
requested at http://pages.newtek.com/NDI-Developers.html .

Currently only Linux is "officially" supported, but OS X and Windows support was
a thought during development and it should be possible to make these work as
well.

`ndiflbridge` requires a C++11 capable compiler and Boost(headers only).

Building on Linux
-----------------

On Linux building is very simple. Set the `NDI_LIB` environment variable to the
path that contains the correct NDI SDK library(libndi.o) for your compiler. Set
`NDI_INCLUDE` to the include directory in the NDI SDK. Finally run
./build-linux.sh .

Building on Windows
-------------------

TODO

Building on Mac OS X
--------------------

TODO
