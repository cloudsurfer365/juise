#
# Homebrew formula file for juise
# https://github.com/mxcl/homebrew
#

require 'formula'

class Juise < Formula
  homepage 'https://github.com/Juniper/juise'
  url 'https://github.com/Juniper/juise/releases/0.5.6/juise-0.5.6.tar.gz'
  sha1 '951a741f77ecb594ca745caedeed3abcfdc45a75'

  depends_on 'libtool' => :build
  depends_on 'libslax'
  depends_on 'libssh2'
  depends_on 'pcre'

  # Need newer versions of these libraries
  if MacOS.version <= :lion
    depends_on 'libxml2'
    depends_on 'libxslt'
    depends_on 'curl'
  end

  def install
    system "./configure", "--disable-dependency-tracking",
                          "--prefix=#{prefix}",
                          "--with-libssh2-prefix=#{HOMEBREW_PREFIX}"
    system "make install"
  end
end
