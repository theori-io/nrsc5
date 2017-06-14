set (CMAKE_C_FLAGS "--std=gnu11 -O3 -ffast-math")

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
