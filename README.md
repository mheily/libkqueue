libkqueue
=========

[![CI Linux](https://github.com/mheily/libkqueue/actions/workflows/ci-linux.yml/badge.svg)](https://github.com/mheily/libkqueue/actions/workflows/ci-linux.yml)
[![CI tests FreeBSD](https://github.com/mheily/libkqueue/actions/workflows/ci-freebsd.yml/badge.svg)](https://github.com/mheily/libkqueue/actions/workflows/ci-freebsd.yml)
[![CI tests macOS](https://github.com/mheily/libkqueue/actions/workflows/ci-macos.yml/badge.svg)](https://github.com/mheily/libkqueue/actions/workflows/ci-macos.yml)
[![CI tests Windows](https://github.com/mheily/libkqueue/actions/workflows/ci-windows.yml/badge.svg)](https://github.com/mheily/libkqueue/actions/workflows/ci-windows.yml)

A user space implementation of the kqueue(2) kernel event notification mechanism
libkqueue acts as a translator between the kevent structure and the native
kernel facilities on Linux, Android, Solaris, and Windows.

libkqueue is not perfect, and you may need to change the behaviour of your application
to work around limitations on a given platform. Please see [BUGS](BUGS.md) for known
behavioural differences between libkqueue and BSD kqueues.

Supported Event Types
---------------------

* vnode
* socket
* proc
* user
* timer

Installation - Linux, Solaris
-----------------------------

    cmake -G "Unix Makefiles" -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib <path to source>
    make
    make install

Installation - Red Hat
----------------------

    cmake3 -G "Unix Makefiles" -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib <path to source>
    make
    cpack3 -G RPM

Installation - Debian
---------------------

    cmake -G "Unix Makefiles" -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib <path to source>
    make
    cpack -G DEB

Installation - Android
----------------------

    cmake -G "Unix Makefiles" -DCMAKE_C_COMPILER=<path to NDK compiler> -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib <path to source>
    make

Windows (Visual Studio Project)
-------------------------------

    cmake -G "Visual Studio 14 2015" <path to source>
    cmake --build .

Windows (clang/C2) (Visual Studio Project)
------------------------------------------

    cmake -G "Visual Studio 14 2015" -T "LLVM-vs2014" <path to source>
    cmake --build .

Windows (cross-compiling on Ubuntu using MinGW)
-----------------------------------------------

    sudo apt-get install mingw-w64
    rm -rf CMakeCache.txt CMakeFiles
    mkdir build
    cd build
    cmake -DCMAKE_TOOLCHAIN_FILE=Toolchain-mingw32.cmake ..
    make

Xcode (project)
---------------

    cmake -G "Xcode" <path to source>

Running Unit Tests
------------------

    cmake -G "Unix Makefiles" -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib -DENABLE_TESTING=YES -DCMAKE_BUILD_TYPE=Debug <path to source>
    make
    make test

Build & Running only the test suite
-----------------------------------
Helpful to see the behavior of the tests on systems with native `kqueue`, e.g: macOS, FreeBSD

    cmake . test/CMakeLists.txt
    make -C test/
    ./test/libkqueue-test

To enable tests which expose bugs in native kqueue implementations pass `-DWITH_NATIVE_KQUEUE_BUGS=1` to cmake.
i.e. `cmake . test/CMakeLists.txt -DWITH_NATIVE_KQUEUE_BUGS=1`.

Debugging
---------

For best results add `-DCMAKE_BUILD_TYPE=Debug` to your cmake invocation, this will disable optimisation
and add debugging symbols to ensure your debugger produces usable output, it also enables asserts.

The environmental variable `KQUEUE_DEBUG` can then be set to enable debug output from libkqueue and the test utility.

    KQUEUE_DEBUG=1 <your application>

When building under clang and some later versions of GCC, you can add the following flags:

- `-DENABLE_ASAN=YES`, enables address sansitizer (detects use after free issues, and out of bounds accesses).
- `-DENABLE_LSAN=YES`, enables leak sanitizer (detects memory leaks).
- `-DENABLE_TSAN=YES`, enables thread sanitizer (detects races).
- `-DENABLE_UBSAN=YES`, enables undefined behaviour sanitizer (detects misaligned accesses, interger wrap, divide by zero etc...).

Building Applications
---------------------

    CFLAGS += -I/usr/include/kqueue
    LDFLAGS += -lkqueue

Tutorials & Examples
--------------------

[Kqueues for Fun and Profit](http://doc.geoffgarside.co.uk/kqueue)

[Handling TCP Connections with Kqueue Event Notification](http://eradman.com/posts//kqueue-tcp.html)

Releases History
----------------

2.0 add support for Android _2013-04-29_

1.0 stable relesae for Linux, Solaris, and Windows _2010-09-18_

[BuildStatus]: https://travis-ci.org/mheily/libkqueue.svg?branch=master "Travis CI status"
[BuildStatusLink]: https://travis-ci.org/mheily/libkqueue
