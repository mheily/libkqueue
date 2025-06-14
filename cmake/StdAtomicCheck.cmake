# Check if _Atomic needs -latomic

set(LIBATOMIC_STATIC_PATH "" CACHE PATH "Directory containing static libatomic.a")

include(CheckCSourceCompiles)

set(
  check_std_atomic_source_code
  [=[
  #include <stdatomic.h>
  _Atomic long long x = 0;
  atomic_uint y = 0;
  void test(_Atomic long long *x, long long v) {
      atomic_store(x, v);
      y = v + 1;
  }
  int main(int argc, char **argv) {
      test(&x, argc);
      return 0;
  }
  ]=])

check_c_source_compiles("${check_std_atomic_source_code}" std_atomic_without_libatomic)

if(NOT std_atomic_without_libatomic)
  set(CMAKE_REQUIRED_LIBRARIES atomic)
  check_c_source_compiles("${check_std_atomic_source_code}" std_atomic_with_libatomic)
  set(CMAKE_REQUIRED_LIBRARIES)
  if(NOT std_atomic_with_libatomic)
    message(FATAL_ERROR "Toolchain doesn't support C11 _Atomic with nor without -latomic")
  else()
    find_library(ATOMIC_STATIC NAMES libatomic.a PATHS /usr/lib /usr/local/lib ${LIBATOMIC_STATIC_PATH} NO_DEFAULT_PATH)
    if(ATOMIC_STATIC)
      get_filename_component(ATOMIC_STATIC_DIR "${ATOMIC_STATIC}" DIRECTORY)
      get_filename_component(ATOMIC_STATIC_NAME "${ATOMIC_STATIC}" NAME)
      message(STATUS "Linking static libatomic: -L${ATOMIC_STATIC_DIR} -l:${ATOMIC_STATIC_NAME}")
      set(EXTRA_PRIVATE_LIBS "-L${ATOMIC_STATIC_DIR} -l:${ATOMIC_STATIC_NAME}")
      if(ENABLE_SHARED)
        target_link_directories(kqueue PRIVATE "${ATOMIC_STATIC_DIR}")
        target_link_libraries(kqueue PRIVATE "-l:${ATOMIC_STATIC_NAME}")
      endif()
    else()
      message(WARNING "static libatomic not found; falling back to -latomic")
      set(EXTRA_PRIVATE_LIBS "-latomic")
      if(ENABLE_SHARED)
        target_link_libraries(kqueue PRIVATE atomic)
      endif()
    endif()
  endif()
endif()
