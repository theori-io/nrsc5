add_compile_options (--std=gnu11 -O3 -Wall)

if (USE_THREADS)
    add_definitions (-D_GNU_SOURCE)
    set (THREAD_LIBRARY pthread)
    if (CMAKE_SYSTEM_NAME MATCHES Linux)
        check_library_exists (${THREAD_LIBRARY} pthread_setname_np "" HAVE_PTHREAD_SETNAME_NP)
    endif()
endif()

if (USE_FAST_MATH)
    add_compile_options (-ffast-math)
endif()

configure_file (config.h.in config.h)
include_directories ("${CMAKE_CURRENT_BINARY_DIR}")

add_executable (
    nrsc5
    acquire.c
    decode.c
    frame.c
    hdc_to_aac.c
    input.c
    main.c
    output.c
    pids.c
    sync.c

    firdecim_q15.c
    firdes_kaiser.c
    resamp_q15.c
    math.c

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
    ${AO_LIBRARY}
    ${FFTW3F_LIBRARY}
    ${RTL_SDR_LIBRARY}
    m
)
install (
    TARGETS nrsc5
    RUNTIME DESTINATION bin
)
