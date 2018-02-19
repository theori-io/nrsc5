add_compile_options (--std=gnu11 -O3 -Wall)

add_definitions (-D_GNU_SOURCE)
set (THREAD_LIBRARY pthread)
if (CMAKE_SYSTEM_NAME MATCHES Linux)
    check_library_exists (${THREAD_LIBRARY} pthread_setname_np "" HAVE_PTHREAD_SETNAME_NP)
endif()

configure_file (config.h.in config.h)
include_directories ("${CMAKE_CURRENT_BINARY_DIR}" "${CMAKE_SOURCE_DIR}/include" "${SOAPY_SDR_INCLUDE_DIR}")

add_library (
    nrsc5
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
target_link_libraries (
    nrsc5
    ${FAAD2_LIBRARY}
    ${THREAD_LIBRARY}
    ${FFTW3F_LIBRARY}
    ${SOAPY_SDR_LIBRARY}
    m
)

add_executable (
    app
    main.c
)
set_property (TARGET app PROPERTY OUTPUT_NAME nrsc5)
target_link_libraries (
    app
    nrsc5
    ${AO_LIBRARY}
    ${THREAD_LIBRARY}
)

install (
    TARGETS app
    RUNTIME DESTINATION bin
)
