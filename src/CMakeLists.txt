set (CMAKE_C_FLAGS "--std=gnu11 -O3 -ffast-math")

if (CMAKE_SYSTEM_PROCESSOR MATCHES "armv[78].*")
    # XXX These match with the conditional and flags in liquid-dsp/configure.ac.
    #     If these are incorrect, you might want to change liquid-dsp too.
    set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mcpu=cortex-a7 -mfloat-abi=hard -mfpu=neon-vfpv4")
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
