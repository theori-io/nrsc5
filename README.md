### Dependencies

The following packages are required:

 * git
 * build-essential
 * cmake
 * autoconf
 * libtool
 * libao-dev
 * libfftw3-dev
 * librtlsdr-dev

### Build Instructions

     $ mkdir build && cd build
     $ cmake [options] ..
     $ make
     $ sudo make install

Available build options:

    -DUSE_COLOR=ON       Colorize log output. [default=OFF]
    -DUSE_NEON=ON        Use NEON instructions. [ARM, default=OFF]
    -DUSE_SSE=ON         Use SSSE3 instructions. [x86, default=OFF]
    -DUSE_FAST_MATH=ON   Use unsafe math optimizations. [default=OFF]
    -DUSE_THREADS=ON     Enable multithreading. [default=ON]
    -DUSE_FAAD2=ON       AAC decoding with FAAD2. [default=ON]

You can test the program using the included sample capture:

     $ xz -d < ../support/sample.xz | src/nrsc5 -r - 0

### Building with [Homebrew](https://brew.sh)

     $ brew install --HEAD https://raw.githubusercontent.com/theori-io/nrsc5/master/nrsc5.rb

## Usage

This was designed for use with an RTL-SDR dongle since that was our testing platform.

### Options:

       frequency                       rtl-sdr center frequency in MHz or Hz
                                         (do not provide frequency when reading from file)
       program                         audio program to decode
                                         (0, 1, 2, or 3)
       -d device-index                 rtl-sdr device
       -g gain                         rtl-sdr gain (0.1 dB)
                                         (automatic gain selection if not specified)
       -p ppm-error                    rtl-sdr ppm error
       -r samples-input                read samples from input file
       -w samples-output               write samples to output file
       -o audio-output                 write audio to output file
       -f adts|hdc|wav                 audio format: adts, hdc, or wav
                                         (hdc playback requires modified faad2)
       -q                              disable log output
       -l log-level                    set log level
                                         (1 = DEBUG, 2 = INFO, 3 = WARN)
       -v                              print the version number and exit

### Examples:

Tune to 107.1 MHz and play audio program 0:

     $ nrsc5 107.1 0

Tune to 107.1 MHz and play audio program 0. Manually set gain to 49.0 dB and save raw RF samples to a file:

     $ nrsc5 -g 490 -w samples1071 107.1 0

Read raw RF samples from a file and play back audio program 0:

     $ nrsc5 -r samples1071 0

Tune to 90.5 MHz and convert audio program 0 to ADTS format for playback in an external media player:

     $ nrsc5 -o - -f adts 90.5 0 | mplayer -
