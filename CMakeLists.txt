cmake_minimum_required (VERSION 2.8)
include (ExternalProject)
project (nrsc5 C)

find_program (AUTOCONF autoconf)
if (NOT AUTOCONF)
    message (FATAL_ERROR "Missing autoconf. Install autoconf package and try again.")
endif ()

find_program (AUTOMAKE automake)
if (NOT AUTOMAKE)
    message (FATAL_ERROR "Missing automake. Install autoconf package and try again.")
endif ()

find_program (LIBTOOLIZE NAMES libtoolize glibtoolize)
if (NOT LIBTOOLIZE)
    message (FATAL_ERROR "Missing libtoolize. Install libtool package and try again.")
endif ()

find_library (AO_LIBRARY ao)
find_library (FFTW3F_LIBRARY fftw3f)
find_library (RTL_SDR_LIBRARY rtlsdr)
find_library (LIQUID_DSP_LIBRARY liquid)

set (FAAD2_PREFIX "${CMAKE_BINARY_DIR}/faad2-prefix")
ExternalProject_Add (
    faad2_external
    GIT_REPOSITORY "https://github.com/dsvensson/faad2.git"
    GIT_TAG b7aa099fd3220b71180ed2b0bc19dc6209a1b418
    PREFIX ${FAAD2_PREFIX}

    UPDATE_COMMAND ""
    PATCH_COMMAND patch -p1 -i "${CMAKE_SOURCE_DIR}/support/faad2-hdc-support.patch"
    COMMAND sh ./bootstrap

    CONFIGURE_COMMAND ${FAAD2_PREFIX}/src/faad2_external/configure --prefix=${FAAD2_PREFIX} --with-hdc "CFLAGS=${CMAKE_C_FLAGS}"

    BUILD_COMMAND make
)

add_library (faad2 STATIC IMPORTED)
set_property (TARGET faad2 PROPERTY IMPORTED_LOCATION "${FAAD2_PREFIX}/lib/libfaad.a")
add_dependencies (faad2 faad2_external)
include_directories ("${FAAD2_PREFIX}/include")

add_subdirectory (src)
