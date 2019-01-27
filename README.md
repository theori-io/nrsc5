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
    -DUSE_FAAD2=ON       AAC decoding with FAAD2. [default=ON]

You can test the program using the included sample capture:

     $ xz -d < ../support/sample.xz | src/nrsc5 -r - 0

### Building on Debian / Ubuntu

     $ sudo apt install git build-essential cmake autoconf libtool libao-dev libfftw3-dev librtlsdr-dev

### Building with [Homebrew](https://brew.sh)

     $ brew install --HEAD https://raw.githubusercontent.com/theori-io/nrsc5/master/nrsc5.rb

## Usage

This was designed for use with an RTL-SDR dongle since that was our testing platform.

### Options:

       frequency                       center frequency in MHz or Hz
                                         (do not provide frequency when reading from file)
       program                         audio program to decode
                                         (0, 1, 2, or 3)
       -g gain                         gain
                                         (example: 49.6)
                                         (automatic gain selection if not specified)
       -d device-index                 rtl-sdr device
       -p ppm-error                    rtl-sdr ppm error
       -r iq-input                     read IQ samples from input file
       -w iq-output                    write IQ samples to output file
       -o audio-output                 write audio to output WAV file
       -q                              disable log output
       -l log-level                    set log level
                                         (1 = DEBUG, 2 = INFO, 3 = WARN)
       -v                              print the version number and exit
       --dump-aas-files dir-name       dump AAS files
                                         (WARNING: insecure)
       --dump-hdc file-name            dump HDC packets

### Examples:

Tune to 107.1 MHz and play audio program 0:

     $ nrsc5 107.1 0

Tune to 107.1 MHz and play audio program 0. Manually set gain to 49.0 dB and save raw IQ samples to a file:

     $ nrsc5 -g 49.0 -w samples1071 107.1 0

Read raw IQ samples from a file and play back audio program 0:

     $ nrsc5 -r samples1071 0

Tune to 90.5 MHz and convert audio program 0 to WAV format for playback in an external media player:

     $ nrsc5 -o - 90.5 0 | mplayer -

## Windows

The only build environment that has been tested on Windows is MSYS2 with MinGW. Unfortunately, some of the dependencies need to be compiled manually. The instructions below build and install fftw, libao, libusb, and rtl-sdr, as well as nrsc5.

### Building with [MSYS2](http://www.msys2.org)

Install MSYS2. Open a terminal using the "MSYS2 MinGW 32-bit" shortcut.

     $ pacman -Syu

If this is the first time running pacman, you will be told to close the terminal window. After doing so, reopen using the same shortcut as before.

     $ pacman -Su
     $ pacman -S autoconf automake git gzip make mingw-w64-i686-gcc mingw-w64-i686-cmake mingw-w64-i686-libtool patch tar xz

Download and install fftw:

     $ cd ~
     $ curl -L http://www.fftw.org/fftw-3.3.7.tar.gz | tar xvz
     $ cd fftw-3.3.7
     $ ./configure --enable-float --enable-sse2 --with-our-malloc && make && make install

Download and install libao:

     $ cd ~
     $ git clone https://git.xiph.org/libao.git
     $ cd libao
     $ ./autogen.sh
     $ LDFLAGS=-lksuser ./configure && make && make install

Download and install libusb:

     $ cd ~
     $ git clone https://github.com/libusb/libusb.git
     $ cd libusb
     $ ./autogen.sh
     $ make && make install

Download and install rtl-sdr:

     $ cd ~
     $ git clone git://git.osmocom.org/rtl-sdr.git
     $ mkdir rtl-sdr/build && cd rtl-sdr/build
     $ cmake -G "MSYS Makefiles" -D LIBUSB_FOUND=1 -D LIBUSB_INCLUDE_DIR=/mingw32/include/libusb-1.0 -D "LIBUSB_LIBRARIES=-L/mingw32/lib -lusb-1.0" -D THREADS_PTHREADS_WIN32_LIBRARY=/mingw32/i686-w64-mingw32/lib/libpthread.a -D THREADS_PTHREADS_INCLUDE_DIR=/mingw32/i686-w64-mingw32/include -D CMAKE_INSTALL_PREFIX=/mingw32 ..
     $ make && make install

Download and install nrsc5:

     $ cd ~
     $ git clone https://github.com/theori-io/nrsc5
     $ mkdir nrsc5/build && cd nrsc5/build
     $ cmake -G "MSYS Makefiles" -DUSE_COLOR=OFF -DUSE_SSE=ON -DCMAKE_INSTALL_PREFIX=/mingw32 ..
     $ make && make install

You can test your installation using the included sample file:

     $ cd ~/nrsc5/support
     $ xz -d sample.xz
     $ nrsc5.exe -r sample 0

If the sample file does not work, make sure you followed all of the instructions. If it still doesn't work, file an issue with the error message. Please put "[Windows]" in the title of the issue.

### Packaging

Once everything is built, you can run nrsc5 independently of MSYS2. Copy the following files from your MSYS2/mingw32 directory (e.g. C:\msys64\mingw32\bin):

 * libao-4.dll
 * libgcc\_s\_dw2-1.dll
 * librtlsdr.dll
 * libusb-1.0.dll
 * libwinpthread-1.dll
 * nrsc5.exe

### Running

See **Usage** section above.

If you get errors trying to access your RTL-SDR device, then you may need to use [Zadig](http://zadig.akeo.ie/) to change the USB driver. Once you download and run Zadig, select your RTL-SDR device and then click "Replace Driver". If your device is not listed, enable "Options" -> "List All Devices".
