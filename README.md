# nrsc5

This program receives NRSC-5 digital radio stations using an RTL-SDR dongle. It offers a command-line interface as well as an API upon which other applications can be built. Before using it, you'll first need to compile the program using the build instructions below.

## Building on Ubuntu, Debian or Raspbian

    $ sudo apt install git build-essential cmake autoconf libtool libao-dev libfftw3-dev librtlsdr-dev
    $ git clone https://github.com/theori-io/nrsc5.git
    $ cd nrsc5
    $ mkdir build
    $ cd build
    $ cmake [options] ..
    $ make
    $ sudo make install
    $ sudo ldconfig

Available build options:

    -DUSE_NEON=ON            Use NEON instructions. [ARM, default=OFF]
    -DUSE_SSE=ON             Use SSSE3 instructions. [x86, default=OFF]
    -DUSE_FAAD2=ON           AAC decoding with FAAD2. [default=ON]
    -DLIBRARY_DEBUG_LEVEL=1  Debug logging level for libnrsc5. [default=5]

You can test the program using the included sample capture:

    $ xz -d < ../support/sample.xz | src/nrsc5 -r - 0

## Building on Fedora

Follow the Ubuntu instructions above, but replace the first command with the following:

    $ sudo dnf install make patch cmake autoconf libtool libao-devel fftw-devel rtl-sdr-devel libusb-devel

## Building on macOS using [Homebrew](https://brew.sh)

    $ curl https://raw.githubusercontent.com/theori-io/nrsc5/master/nrsc5.rb > /tmp/nrsc5
    $ brew install --HEAD -s /tmp/nrsc5

## Building for Windows

To build the program for Windows, you can either use [MSYS2](http://www.msys2.org) on Windows, or else use a cross-compiler on an Ubuntu, Debian or macOS machine. Scripts are provided to help with both cases.

### Building on Windows with MSYS2

Install [MSYS2](http://www.msys2.org). Open a terminal using the "MSYS2 MinGW 32-bit" shortcut. (Or use the 64-bit shortcut if you prefer a 64-bit build.)

    $ pacman -Syu

If this is the first time running pacman, you will be told to close the terminal window. After doing so, reopen using the same shortcut as before.

    $ pacman -Su
    $ pacman -S git
    $ git clone https://github.com/theori-io/nrsc5.git
    $ nrsc5/support/msys2-build

You can test your installation using the included sample file:

    $ cd ~/nrsc5/support
    $ xz -d sample.xz
    $ nrsc5.exe -r sample 0

If the sample file does not work, make sure you followed all of the instructions. If it still doesn't work, file an issue with the error message. Please put "[Windows]" in the title of the issue.

Once everything is built, you can run nrsc5 independently of MSYS2. Copy the following files from your MSYS2/mingw32 directory (e.g. C:\\msys64\\mingw32\\bin):

* libnrsc5.dll
* nrsc5.exe

### Cross-compiling for Windows from Ubuntu / Debian

    $ sudo apt install mingw-w64
    $ support/win-cross-compile 32

Replace `32` with `64` if you want a 64-bit build. Once the build is complete, copy `*.dll` and `nrsc5.exe` from the `build-win32/bin` (or `build-win64/bin`) folder to your Windows machine.

### Cross-compiling for Windows from macOS

    $ brew install mingw-w64
    $ support/win-cross-compile 32

Replace `32` with `64` if you want a 64-bit build. Once the build is complete, copy `*.dll` and `nrsc5.exe` from the `build-win32/bin` (or `build-win64/bin`) folder to your Windows machine.

## Usage

### Command-line options:

    frequency                       center frequency in MHz or Hz
                                      (do not provide frequency when reading from file)
    program                         audio program to decode
                                      (0, 1, 2, or 3)
    -g gain                         gain
                                      (example: 49.6)
                                      (automatic gain selection if not specified)
    -d device-index                 rtl-sdr device
    -p ppm-error                    rtl-sdr ppm error
    -H rtltcp-host                  rtl_tcp host with optional port
                                      (example: localhost:1234)
    -r iq-input                     read IQ samples from input file
    -w iq-output                    write IQ samples to output file
    -o audio-output                 write audio to output WAV file
    -q                              disable log output
    -l log-level                    set log level
                                      (1 = DEBUG, 2 = INFO, 3 = WARN)
    -v                              print the version number and exit
    --am                            receive AM signals
                                      (default is FM)
    -T                              enable bias-T
    -D direct-sampling-mode         enable direct sampling
                                      (1 = I-ADC input, 2 = Q-ADC input)
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

### Keyboard commands:

To switch between audio programs at runtime, press <kbd>0</kbd>, <kbd>1</kbd>, <kbd>2</kbd>, or <kbd>3</kbd>.

To quit, press <kbd>Q</kbd>.

### RTL-SDR drivers on Windows

If you get errors trying to access your RTL-SDR device, then you may need to use [Zadig](http://zadig.akeo.ie/) to change the USB driver. Once you download and run Zadig, select your RTL-SDR device, ensure the driver is set to WinUSB, and then click "Replace Driver". If your device is not listed, enable "Options" -> "List All Devices".
