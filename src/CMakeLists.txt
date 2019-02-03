add_compile_options (--std=gnu11 -O3 -Wall)

add_definitions (-D_GNU_SOURCE)
set (THREAD_LIBRARY pthread)
if (CMAKE_SYSTEM_NAME MATCHES Linux)
    check_library_exists (${THREAD_LIBRARY} pthread_setname_np "" HAVE_PTHREAD_SETNAME_NP)
endif()
set (CMAKE_MACOSX_RPATH ON)

configure_file (config.h.in config.h)
include_directories ("${CMAKE_CURRENT_BINARY_DIR}" "${CMAKE_SOURCE_DIR}/include")

add_library (
    nrsc5_object OBJECT
    acquire.c
    decode.c
    frame.c
    input.c
    nrsc5.c
    output.c
    pids.c
    sync.c

    firdecim_q15.c

    conv_dec.c

    reed-solomon.c
    galois.c

    log.c

    strndup.c
)
set_property(TARGET nrsc5_object PROPERTY POSITION_INDEPENDENT_CODE ON)
if (USE_FAAD2)
    add_dependencies(nrsc5_object faad2_external)
endif ()

set (
    LibraryDependencies
    ${FAAD2_LIBRARY}
    ${THREAD_LIBRARY}
    ${FFTW3F_LIBRARY}
    ${RTL_SDR_LIBRARY}
    m
)

add_library (
    nrsc5 SHARED
    $<TARGET_OBJECTS:nrsc5_object>
)
target_link_libraries (
    nrsc5
    ${LibraryDependencies}
)
set_target_properties(nrsc5 PROPERTIES PUBLIC_HEADER "../include/nrsc5.h")

add_library (
    nrsc5_static
    $<TARGET_OBJECTS:nrsc5_object>
)
target_link_libraries (
    nrsc5_static
    ${LibraryDependencies}
)

add_executable (
    app
    main.c
)
set_property (TARGET app PROPERTY OUTPUT_NAME nrsc5)
target_link_libraries (
    app
    nrsc5_static
    ${AO_LIBRARY}
    ${THREAD_LIBRARY}
)

install (
    TARGETS app nrsc5 nrsc5_static
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    PUBLIC_HEADER DESTINATION include
)
