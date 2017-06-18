add_compile_options (--std=gnu11 -O3 -ffast-math)

if (CMAKE_SYSTEM_PROCESSOR MATCHES "arm.*")
    if (USE_NEON)
        add_compile_options (-mcpu=cortex-a7 -mfloat-abi=hard -mfpu=neon-vfpv4)
        add_definitions (-DHAVE_NEON)
    endif()
endif()
if (CMAKE_SYSTEM_PROCESSOR MATCHES "x86.*")
    if (USE_SSE)
        add_definitions (-DHAVE_SSE2 -DHAVE_SSE3)
    endif()
endif()

add_executable (
    nrsc5
    acquire.c
    decode.c
    frame.c
    input.c
    main.c
    output.c
    sync.c

    firdecim_q15.c
    resamp_q15.c

    conv_dec.c

    reed-solomon.c
    galois.c
)
target_link_libraries (
    nrsc5
    faad2
    liquid-dsp
    m
    pthread
    ${AO_LIBRARY}
    ${FFTW3F_LIBRARY}
    ${RTL_SDR_LIBRARY}
)
install (
    TARGETS nrsc5
    RUNTIME DESTINATION bin
)
