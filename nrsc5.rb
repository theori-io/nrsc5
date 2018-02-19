class Nrsc5 < Formula
  desc "NRSC-5 receiver"
  homepage "http://theori.io/research/nrsc-5-c"
  head "https://github.com/theori-io/nrsc5.git"

  depends_on "cmake" => :build
  depends_on "autoconf" => :build
  depends_on "automake" => :build
  depends_on "libtool" => :build
  depends_on "git" => :build
  depends_on "libao"
  depends_on "fftw"
  depends_on "soapysdr"

  def install
    mkdir "build" do
      system "cmake", "..", *std_cmake_args
      system "make", "install"
    end
  end
end
