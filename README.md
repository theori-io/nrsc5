### Dependencies

The following packages are required:

 * git
 * build-essential
 * cmake
 * autoconf
 * libtool
 * libao-dev
 * libfftw3-dev
 * libsoapysdr-dev

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

     $ xz -d < ../support/sample.xz | src/nrsc5 -r - 0 0

### Building on Debian / Ubuntu

Debian 9+, Ubuntu 17.04+

     $ sudo apt install git build-essential cmake autoconf libtool libao-dev libfftw3-dev libsoapysdr-dev soapysdr-module-all

Ubuntu 16.04:

     $ sudo add-apt-repository -y ppa:myriadrf/drivers
     $ sudo apt update
     $ sudo apt install git build-essential cmake autoconf libtool libao-dev libfftw3-dev libsoapysdr-dev soapysdr-modules-all

### Building with [Homebrew](https://brew.sh)

Install SoapySDR. [Additional packages](https://github.com/pothosware/homebrew-pothos/wiki) are available to support more hardware devices.

     $ brew tap pothosware/homebrew-pothos
     $ brew update
     $ brew install soapysdr soapyrtlsdr

Compile and install nrsc5:

     $ brew install --HEAD https://raw.githubusercontent.com/theori-io/nrsc5/master/nrsc5.rb

## Usage

We use the SoapySDR platform to support multiple types of hardware devices. You will likely need to specify which hardware driver to use.
Refer to the list of modules on the [SoapySDR wiki](https://github.com/pothosware/SoapySDR/wiki) for the list of drivers and optional
arguments.

RTL-SDR:

     $ nrsc5 -d driver=rtlsdr FREQUENCY PROGRAM

HackRF:

     $ nrsc5 -d driver=hackrf FREQUENCY PROGRAM

### Options:

       frequency                       center frequency in MHz or Hz
                                         (ignored when using IQ input file)
       program                         audio program to decode
                                         (0, 1, 2, or 3)
       -d device-args                  device arguments for SoapySDR
                                         (example: driver=rtlsdr)
       -g gain                         gain
                                         (example: 49.6)
                                         (automatic gain selection if not specified)
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

     $ nrsc5 107.1e6 0

Tune to 107.1 MHz and play audio program 0. Manually set gain to 49.0 dB and save raw RF samples to a file:

     $ nrsc5 -g 49.0 -w samples1071 107.1e6 0

Read raw RF samples from a file and play back audio program 0:

     $ nrsc5 -r samples1071 0 0

Tune to 90.5 MHz and convert audio program 0 to WAV format for playback in an external media player:

     $ nrsc5 -o - 90.5e6 0 | mplayer -

## Windows

The only build environment that has been tested on Windows is MSYS2 with MinGW. Unfortunately, some of the dependencies need to be compiled manually. The instructions below build and install fftw, libao, libusb, and rtl-sdr, as well as nrsc5.

Before continuing, you must have the PothosSDR development environment. An installer for Windows 64-bit is available at: [http://downloads.myriadrf.org/builds/PothosSDR/](http://downloads.myriadrf.org/builds/PothosSDR/?C=M;O=D). During the installation, the "Application runtime" and "Development" components must be enabled.

### Building with [MSYS2](http://www.msys2.org)

Install MSYS2. You must use the x86\_64 installer. Open a terminal using the "MSYS2 MinGW 64-bit" shortcut.

     $ pacman -Syu

If this is the first time running pacman, you will be told to close the terminal window. After doing so, reopen using the same shortcut as before.

     $ pacman -Su
     $ pacman -S autoconf automake git gzip make mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-libtool patch tar xz

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

Download and build nrsc5 (change the path to PothosSDR if appropriate):

     $ cd ~
     $ git clone https://github.com/theori-io/nrsc5
     $ mkdir nrsc5/build && cd nrsc5/build
     $ cmake -G "MSYS Makefiles" -DUSE_COLOR=OFF -DUSE_SSE=ON -DCMAKE_INSTALL_PREFIX=/mingw64 -DSoapySDR_DIR="C:/Program Files/PothosSDR/cmake" ..
     $ make && make install

Before you can run nrsc5.exe, you must have the PothosSDR directory in your path:

     $ export PATH="$PATH:/c/Program Files/PothosSDR/bin"

You can test your installation using the included sample file:

     $ cd ~/nrsc5/support
     $ xz -d sample.xz
     $ nrsc5.exe -r sample 0 0

If the sample file does not work, make sure you followed all of the instructions. If it still doesn't work, file an issue with the error message. Please put "[Windows]" in the title of the issue.

If you want to run your build on a different computer, install PothosSDR and  copy the following files to the PothosSDR bin directory (e.g. C:\Program Files\PothosSDR\bin):

 * C:\msys64\home\%USERNAME%\nrsc5\build\src\nrsc5.exe
 * C:\msys64\mingw64\bin\libao-4.dll
 * C:\msys64\mingw64\bin\libwinpthread-1.dll

### Running

See **Usage** section above.

You must specify the proper device arguments otherwise the wrong driver may be loaded. For example, if you use a RTL-SDR:

     $ nrsc5.exe -d driver=rtlsdr FREQUENCY PROGRAM

If you get errors trying to access your RTL-SDR device, then you may need to use [Zadig](http://zadig.akeo.ie/) to change the USB driver. Once you download and run Zadig, select your RTL-SDR device and then click "Replace Driver". If your device is not listed, enable "Options" -> "List All Devices".
