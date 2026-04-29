#
# Copyright (c) 2011 Marius Zwicker <marius@mlba-team.de>
#
# Permission to use, copy, modify, and distribute this software for any
# purpose with or without fee is hereby granted, provided that the above
# copyright notice and this permission notice appear in all copies.
#
# THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
# WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
# MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
# ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
# WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
# ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
# OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
#
cmake_minimum_required(VERSION 3.7.2)

project(libkqueue-test LANGUAGES C)

#
# Per-filter test sources are only compiled when the matching
# filter is enabled in the build (LIBKQUEUE_HAVE_FILT_* is set in
# the parent CMakeLists).  When this file is invoked standalone
# (cd test && cmake .), the gates are unset; default them to ON
# so the standalone build (which targets the host's native kqueue)
# still pulls in every filter test source.
#
foreach(_filt READ WRITE VNODE PROC TIMER SIGNAL USER LIBKQUEUE)
  if(NOT DEFINED LIBKQUEUE_HAVE_FILT_${_filt})
    set(LIBKQUEUE_HAVE_FILT_${_filt} 1)
  endif()
endforeach()

set(LIBKQUEUE_TEST_SOURCES
    common.c
    kqueue.c
    main.c
    test.c
    threading.c)
if(LIBKQUEUE_HAVE_FILT_READ)
  list(APPEND LIBKQUEUE_TEST_SOURCES read.c)
endif()
if(LIBKQUEUE_HAVE_FILT_WRITE)
  list(APPEND LIBKQUEUE_TEST_SOURCES write.c)
endif()
if(LIBKQUEUE_HAVE_FILT_TIMER)
  list(APPEND LIBKQUEUE_TEST_SOURCES timer.c)
endif()
if(LIBKQUEUE_HAVE_FILT_USER)
  list(APPEND LIBKQUEUE_TEST_SOURCES user.c)
endif()
if(LIBKQUEUE_HAVE_FILT_VNODE)
  list(APPEND LIBKQUEUE_TEST_SOURCES vnode.c)
endif()
if(LIBKQUEUE_HAVE_FILT_PROC AND UNIX)
  list(APPEND LIBKQUEUE_TEST_SOURCES proc.c)
endif()
if(LIBKQUEUE_HAVE_FILT_SIGNAL AND UNIX)
  list(APPEND LIBKQUEUE_TEST_SOURCES signal.c)
endif()
if(LIBKQUEUE_HAVE_FILT_LIBKQUEUE)
  list(APPEND LIBKQUEUE_TEST_SOURCES libkqueue.c)
endif()

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/config.h.in
               ${CMAKE_CURRENT_BINARY_DIR}/config.h)

add_executable(libkqueue-test ${LIBKQUEUE_TEST_SOURCES})
target_include_directories(libkqueue-test
                           PRIVATE
                             "${CMAKE_SOURCE_DIR}"
                             "${CMAKE_BINARY_DIR}/include")
if(NOT "${CMAKE_SYSTEM_NAME}" MATCHES "(Solaris|SunOS)")
  # Backends that drop the kq mutex across kevent_wait can be exercised
  # by the cross-thread EVFILT_USER trigger test.  Solaris doesn't
  # currently opt into KEVENT_WAIT_DROP_LOCK (would need a UAF-safe
  # portev_user wrapper or non-coalescing dispatcher signal), so skip.
  target_compile_definitions(libkqueue-test PRIVATE TEST_DROP_LOCK_WAKE)
endif()

if("${CMAKE_SYSTEM_NAME}" MATCHES "(Solaris|SunOS)")
  # Solaris/illumos ships sigwait() with both the historical 1-arg
  # signature (default) and the 2-arg POSIX one (gated behind
  # _POSIX_PTHREAD_SEMANTICS or __EXTENSIONS__).  Same applies to
  # several other thread/signal calls.  Enable the POSIX namespace
  # so the tests get the standard signatures.
  target_compile_definitions(libkqueue-test
                             PRIVATE
                               __EXTENSIONS__
                               _POSIX_PTHREAD_SEMANTICS)
  # BSD socket symbols (socket, bind, recv, send, accept, listen,
  # connect, shutdown, getsockname, setsockopt, socketpair) live in
  # libsocket and libnsl on Solaris/illumos, not libc.
  target_link_libraries(libkqueue-test PRIVATE socket nsl)
endif()

if(WIN32)
  target_compile_definitions(libkqueue-test
                             PRIVATE
                               _CRT_SECURE_NO_WARNINGS
                               _CRT_NONSTDC_NO_WARNINGS
                               _WINSOCK_DEPRECATED_NO_WARNINGS)
  if(CMAKE_C_COMPILER_ID MATCHES Clang)
    target_compile_options(libkqueue-test PRIVATE -Wno-unused-variable)
  endif()
elseif(UNIX)
  target_link_libraries(libkqueue-test PRIVATE -rdynamic)
endif()

#
# Don't link against -lkqueue on OSX and FreeBSD (native kqueue).  On
# FreeBSD pthread is a separate library, so the threading tests still
# need an explicit link there; macOS pulls pthread in via libSystem.
#
#
# Default: don't link -lkqueue on macOS/FreeBSD (they ship native
# kqueue).  When we explicitly built the libkqueue POSIX backend on
# those hosts, the tests must exercise our library, not the native
# one.  ${LIBKQUEUE_BACKEND_RESOLVED} is inherited from the parent
# CMakeLists.
#
set(_test_use_libkqueue ON)
if("${CMAKE_SYSTEM_NAME}" MATCHES "(Darwin|FreeBSD)" AND
   NOT "${LIBKQUEUE_BACKEND_RESOLVED}" STREQUAL "posix")
  set(_test_use_libkqueue OFF)
endif()

if(_test_use_libkqueue)
  target_link_libraries(libkqueue-test
                        PRIVATE
                        kqueue
                        Threads::Threads)
elseif("${CMAKE_SYSTEM_NAME}" STREQUAL "FreeBSD")
  set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
  set(THREADS_PREFER_PTHREAD_FLAG TRUE)
  find_package(Threads REQUIRED)
  target_link_libraries(libkqueue-test PRIVATE Threads::Threads)
endif()

if(WIN32)
  target_link_libraries(libkqueue-test
                        PRIVATE
                          ws2_32)
endif()

add_test(NAME libkqueue-test
         COMMAND libkqueue-test)

