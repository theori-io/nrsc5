cmake_minimum_required (VERSION 3.5)
include (CheckLibraryExists)
include (CheckSymbolExists)
include (ExternalProject)
include (FindPkgConfig)
project (nrsc5 C)

execute_process (COMMAND ${CMAKE_C_COMPILER} -dumpmachine OUTPUT_VARIABLE HOST_TRIPLE_DEFAULT OUTPUT_STRIP_TRAILING_WHITESPACE)

option (USE_NEON "Use NEON instructions")
option (USE_SSE "Use SSE3 instructions")
option (USE_FAAD2 "AAC decoding with FAAD2" ON)
option (USE_STATIC "Link with static libraries")
option (USE_SYSTEM_FFTW "Use system provided fftw" ON)
option (USE_SYSTEM_RTLSDR "Use system provided rtl-sdr" ON)
option (USE_SYSTEM_LIBUSB "Use system provided libusb" ON)
option (USE_SYSTEM_LIBAO "Use system provided libao" ON)
option (BUILD_DOC "Build API documentation" OFF)

set (FAAD2_CMAKE_ARGS "" CACHE STRING "Extra arguments for FAAD2 cmake command")
set (LIBRARY_DEBUG_LEVEL "5" CACHE STRING "Debug logging level for libnrsc5: 1=debug, 2=info, 3=warn, 4=error, 5=none")
set (HOST_TRIPLE "${HOST_TRIPLE_DEFAULT}" CACHE STRING "Override default host triple")
if (HOST_TRIPLE)
    set (HOST_TRIPLE_ARG "--host=${HOST_TRIPLE}")
endif()

message (STATUS "Building for ${HOST_TRIPLE}")

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

find_program (PATCH patch)
if (NOT PATCH)
    message (FATAL_ERROR "Missing patch. Install patch package and try again.")
endif()

if (USE_SYSTEM_FFTW)
    find_library (FFTW3F_LIBRARIES fftw3f)
    pkg_search_module (FFTW3F fftw3f)
    if (NOT FFTW3F_LIBRARIES)
        message (WARNING "libfftw3f not found. Building from source.")
        set (USE_SYSTEM_FFTW OFF)
    endif ()
endif ()
if (USE_SYSTEM_RTLSDR)
    find_library (RTL_SDR_LIBRARIES rtlsdr)
    pkg_search_module (RTL_SDR librtlsdr)
    if (NOT RTL_SDR_LIBRARIES)
        message (WARNING "librtlsdr not found. Building from source.")
        set (USE_SYSTEM_RTLSDR OFF)
    endif ()
endif ()
if (USE_SYSTEM_LIBUSB)
    find_library (LIBUSB_LIBRARIES usb-1.0)
    pkg_search_module (LIBUSB libusb-1.0)
    if (NOT LIBUSB_LIBRARIES)
        message (WARNING "libusb-1.0 not found. Building from source.")
        set (USE_SYSTEM_LIBUSB OFF)
    endif ()
endif ()
if (USE_SYSTEM_LIBAO)
    find_library (AO_LIBRARIES ao)
    pkg_search_module(AO ao)
    if (NOT AO_LIBRARIES)
        message (WARNING "libao not found. Building from source.")
        set (USE_SYSTEM_LIBAO OFF)
    endif ()
endif ()

if (CMAKE_SYSTEM_PROCESSOR MATCHES "arm.*")
    if (USE_NEON)
        set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mcpu=cortex-a7 -mfloat-abi=hard -mfpu=neon-vfpv4")
        add_definitions (-DHAVE_NEON)
    endif()
endif()

if (CMAKE_SYSTEM_PROCESSOR MATCHES "(i[456]|x)86.*")
    if (USE_SSE)
        set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -msse2 -msse3 -mssse3")
        add_definitions (-DHAVE_SSE2 -DHAVE_SSE3)
    endif()
endif()

set (CMAKE_REQUIRED_FLAGS --std=gnu11)
check_symbol_exists (strndup string.h HAVE_STRNDUP)
check_symbol_exists (CMPLXF complex.h HAVE_CMPLXF)
check_symbol_exists (_Imaginary_I complex.h HAVE_IMAGINARY_I)
check_symbol_exists (_Complex_I complex.h HAVE_COMPLEX_I)

if (NOT USE_SYSTEM_FFTW)
    set (FFTW_PREFIX "${CMAKE_BINARY_DIR}/fftw-prefix")
    ExternalProject_Add (
        fftw_external
        URL "https://www.fftw.org/fftw-3.3.10.tar.gz"
        URL_HASH SHA256=56c932549852cddcfafdab3820b0200c7742675be92179e59e6215b340e26467
        PREFIX ${FFTW_PREFIX}

        CONFIGURE_COMMAND ${FFTW_PREFIX}/src/fftw_external/configure ${HOST_TRIPLE_ARG} --prefix=${FFTW_PREFIX} --enable-float --enable-static --disable-shared --enable-sse2 --enable-avx --enable-avx2 --with-our-malloc
    )

    add_library (fftw3f STATIC IMPORTED)
    set_property (TARGET fftw3f PROPERTY IMPORTED_LOCATION "${FFTW_PREFIX}/lib/libfftw3f.a")
    add_dependencies (fftw3f fftw_external)

    set (FFTW3F_INCLUDE_DIRS "${FFTW_PREFIX}/include")
    set (FFTW3F_LIBRARIES fftw3f)
    set (FFTW3F_BUILTIN fftw3f)
endif ()

if (NOT USE_SYSTEM_LIBUSB)
    set (LIBUSB_PREFIX "${CMAKE_BINARY_DIR}/libusb-prefix")
    ExternalProject_Add (
        libusb_external
        TIMEOUT 120
        GIT_REPOSITORY "https://github.com/libusb/libusb.git"
        GIT_TAG v1.0.27
        PREFIX ${LIBUSB_PREFIX}

        UPDATE_COMMAND ""
        CONFIGURE_COMMAND ${LIBUSB_PREFIX}/src/libusb_external/configure ${HOST_TRIPLE_ARG} --prefix=${LIBUSB_PREFIX}
    )
    ExternalProject_Add_Step (
        libusb_external
        bootstrap
        COMMAND sh ./bootstrap.sh
        DEPENDEES patch
        DEPENDERS configure
        WORKING_DIRECTORY <SOURCE_DIR>
    )
    add_library (libusb STATIC IMPORTED)
    set_property (TARGET libusb PROPERTY IMPORTED_LOCATION "${LIBUSB_PREFIX}/lib/libusb-1.0.a")

    set (LIBUSB_BUILTIN libusb_external)
    set (LIBUSB_LIBRARIES libusb)
    set (RTLSDR_CMAKE_ARGS
        -DCMAKE_PREFIX_PATH:STRING=${LIBUSB_PREFIX}
    )
endif ()

if (NOT USE_SYSTEM_RTLSDR)
    set (RTLSDR_PREFIX "${CMAKE_BINARY_DIR}/rtlsdr-prefix")
    ExternalProject_Add (
        rtlsdr_external
        TIMEOUT 120
        GIT_REPOSITORY "https://gitea.osmocom.org/sdr/rtl-sdr.git"
        GIT_TAG b85f037d85fa5382c3e88463184b77b273cd1091
        PREFIX ${RTLSDR_PREFIX}

        UPDATE_COMMAND ""
        CMAKE_ARGS
            -DCMAKE_INSTALL_PREFIX:STRING=${RTLSDR_PREFIX}
            -DCMAKE_C_COMPILER:STRING=${CMAKE_C_COMPILER}
            -DCMAKE_SYSTEM_NAME:STRING=${CMAKE_SYSTEM_NAME}
            ${RTLSDR_CMAKE_ARGS}
    )

    if (LIBUSB_BUILTIN)
        add_dependencies (rtlsdr_external ${LIBUSB_BUILTIN})
    endif ()

    add_library (rtlsdr STATIC IMPORTED)
    set_property (TARGET rtlsdr PROPERTY IMPORTED_LOCATION "${RTLSDR_PREFIX}/lib/librtlsdr_static.a")
    add_dependencies (rtlsdr rtlsdr_external)

    set (RTL_SDR_INCLUDE_DIRS "${RTLSDR_PREFIX}/include")
    set (RTL_SDR_LIBRARIES rtlsdr ${LIBUSB_LIBRARIES})
    set (RTL_SDR_BUILTIN rtlsdr)
endif ()

if (NOT USE_SYSTEM_LIBAO)
    if (NOT WIN32)
        message (FATAL_ERROR "Built-in libao only supported on Windows.")
    endif ()

    set (LIBAO_PREFIX "${CMAKE_BINARY_DIR}/libao-prefix")
    ExternalProject_Add (
        libao_external
        TIMEOUT 120
        GIT_REPOSITORY "https://gitlab.xiph.org/xiph/libao.git"
        GIT_TAG cafce902a73c1050474a62feff83e428bbbee5f4
        PREFIX ${LIBAO_PREFIX}

        UPDATE_COMMAND ""
        CONFIGURE_COMMAND ${LIBAO_PREFIX}/src/libao_external/configure ${HOST_TRIPLE_ARG} --prefix=${LIBAO_PREFIX} --enable-static --disable-shared --disable-pulse
    )
    ExternalProject_Add_Step (
        libao_external
        bootstrap
        COMMAND sh ./autogen.sh
        DEPENDEES patch
        DEPENDERS configure
        WORKING_DIRECTORY <SOURCE_DIR>
    )

    add_library (libao STATIC IMPORTED)
    set_property (TARGET libao PROPERTY IMPORTED_LOCATION "${LIBAO_PREFIX}/lib/libao.a")
    add_dependencies (libao libao_external)

    set (AO_INCLUDE_DIRS "${LIBAO_PREFIX}/include")
    set (AO_LIBRARIES libao ksuser winmm)
    set (AO_BUILTIN libao)
endif ()

if (USE_FAAD2)
    set (FAAD2_PREFIX "${CMAKE_BINARY_DIR}/faad2-prefix")
    ExternalProject_Add (
        faad2_external
        TIMEOUT 120
        GIT_REPOSITORY "https://github.com/knik0/faad2.git"
        GIT_TAG 66cfdb9059f6e1ee4a41adcf2008b29fc6267561
        PREFIX ${FAAD2_PREFIX}

        UPDATE_COMMAND ""
        PATCH_COMMAND patch -p1 -Ni "${CMAKE_SOURCE_DIR}/support/faad2-hdc-support.patch" || exit 0

        CMAKE_ARGS
        -DBUILD_SHARED_LIBS:BOOL=OFF
        -DCMAKE_INSTALL_PREFIX:STRING=${FAAD2_PREFIX}
        -DCMAKE_C_COMPILER:STRING=${CMAKE_C_COMPILER}
        -DCMAKE_SYSTEM_NAME:STRING=${CMAKE_SYSTEM_NAME}
        ${FAAD2_CMAKE_ARGS}
    )

    add_library (faad2 STATIC IMPORTED)
    set_property (TARGET faad2 PROPERTY IMPORTED_LOCATION "${FAAD2_PREFIX}/lib/libfaad_hdc.a")
    add_dependencies (faad2 faad2_external)

    set (FAAD2_INCLUDE_DIRS "${FAAD2_PREFIX}/include")
    set (FAAD2_LIBRARIES faad2)
    add_definitions (-DHAVE_FAAD2)
    set (FAAD2_BUILTIN faad2)
else ()
    # we only use libao with faad2
    unset (AO_INCLUDE_DIRS)
    unset (AO_LIBRARIES)
    unset (AO_BUILTIN)
endif()

set (BUILTIN_LIBRARIES ${FFTW3F_BUILTIN} ${RTL_SDR_BUILTIN} ${FAAD2_BUILTIN} ${AO_BUILTIN})

if(EXISTS "${CMAKE_SOURCE_DIR}/.git")
  execute_process(
    COMMAND git log -1 --format=%h
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_COMMIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
else()
  set(GIT_COMMIT_HASH "unknown")
endif()
add_definitions("-DGIT_COMMIT_HASH=\"${GIT_COMMIT_HASH}\"")

add_subdirectory (src)

# optionally generate documentation via Doxygen
if (BUILD_DOC)
  find_package(Doxygen)
  if (NOT DOXYGEN_FOUND)
    message("** Install the Doxygen application to generate API documentation")
  else ()
    message("-- Build of Doxygen API documentation enabled")
    set(DOXYGEN_IN ${CMAKE_CURRENT_BINARY_DIR}/../doc/Doxyfile.in)
    set(DOXYGEN_OUT ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile)
    configure_file(${DOXYGEN_IN} ${DOXYGEN_OUT} @ONLY)
    # 'ALL' below enables building docs together with the application
    add_custom_target( doc_doxygen ALL
      COMMAND ${DOXYGEN_EXECUTABLE} ${DOXYGEN_OUT}
      WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
      COMMENT "Generating API documentation with Doxygen"
      VERBATIM )
  endif ()
else ()
    message("-- Build of Doxygen API documentation disabled; use -DBUILD_DOC=ON to enable")
endif ()
