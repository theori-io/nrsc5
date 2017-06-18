### Dependencies

The following packages are required:

 * libao-dev
 * libfftw3-dev
 * rtl-sdr

### Build Instructions

     $ mkdir build && cd build
     $ cmake [options] ..
     $ make

Available build options:

    -DUSE_NEON=ON        Use NEON instructions. [ARM, default=OFF]
    -DUSE_SSE=ON         Use SSSE3 instructions. [x86, default=OFF]

You can test the program using the included sample capture:

     $ xz -d < ../support/sample.xz | src/nrsc5 -r - 0

### Building with [Homebrew](https://brew.sh)

     $ brew install --HEAD https://raw.githubusercontent.com/theori-io/nrsc5/master/nrsc5.rb

## Usage

This was designed for use with an RTL-SDR dongle since that was our testing platform.

Options:

       frequency                       rtl-sdr center frequency
                                         (do not provide frequency when reading from file)
       program                         audio program to decode
                                         (0, 1, 2, or 3)
       -d device-index                 rtl-sdr device
       -g gain                         rtl-sdr gain (0.1 dB)
       -p ppm-error                    rtl-sdr ppm error
       -r samples-input                read samples from input file
       -w samples-output               write samples to output file
       -o audio-output                 write audio to output file
       -f adts|wav                     audio format: adts or wav
                                         (adts playback requires modified faad2)

Examples:

     $ nrsc5 -p 63 -g 490 -w samples1071 107100000 0

     $ nrsc5 -r samples1071 0
