#!/bin/sh
# Build the static C deps aria2 needs, cross for $1 (a musl triple), into $PREFIX.
set -eu
TRIPLE="$1"
j="$(nproc)"
mkdir -p /src && cd /src
fetch() { curl -L -o "$2" "$1"; tar xf "$2"; }
# zlib
fetch https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz z.tgz
( cd zlib-1.3.1 && CHOST="$TRIPLE" ./configure --static --prefix="$PREFIX" && make -j$j && make install )
# c-ares
fetch https://github.com/c-ares/c-ares/releases/download/v1.34.4/c-ares-1.34.4.tar.gz c.tgz
( cd c-ares-1.34.4 && ./configure --host="$TRIPLE" --enable-static --disable-shared --prefix="$PREFIX" && make -j$j && make install )
# expat
fetch https://github.com/libexpat/libexpat/releases/download/R_2_6_4/expat-2.6.4.tar.bz2 e.tbz
( cd expat-2.6.4 && ./configure --host="$TRIPLE" --enable-static --disable-shared --without-docbook --prefix="$PREFIX" && make -j$j && make install )
# sqlite3
fetch https://www.sqlite.org/2024/sqlite-autoconf-3470200.tar.gz s.tgz
( cd sqlite-autoconf-3470200 && ./configure --host="$TRIPLE" --enable-static --disable-shared --prefix="$PREFIX" && make -j$j && make install )
# openssl — pick the Configure target by arch/word size; linux-generic64 assumes
# a 64-bit word and miscompiles/fails on the 32-bit armv7l target.
fetch https://github.com/openssl/openssl/releases/download/openssl-3.4.0/openssl-3.4.0.tar.gz o.tgz
case "$TRIPLE" in
  x86_64-*)  ossl_target=linux-x86_64 ;;
  aarch64-*) ossl_target=linux-aarch64 ;;
  arm*)      ossl_target=linux-generic32 ;;
  *)         ossl_target=linux-generic64 ;;
esac
( cd openssl-3.4.0 && ./Configure no-shared no-tests --prefix="$PREFIX" --cross-compile-prefix="${TRIPLE}-" "$ossl_target" && make -j$j && make install_sw )
# libssh2
fetch https://github.com/libssh2/libssh2/releases/download/libssh2-1.11.1/libssh2-1.11.1.tar.gz h.tgz
( cd libssh2-1.11.1 && ./configure --host="$TRIPLE" --enable-static --disable-shared --with-crypto=openssl --with-libssl-prefix="$PREFIX" --prefix="$PREFIX" && make -j$j && make install )
# gmp
fetch https://gmplib.org/download/gmp/gmp-6.3.0.tar.xz g.txz
( cd gmp-6.3.0 && ./configure --host="$TRIPLE" --enable-static --disable-shared --prefix="$PREFIX" && make -j$j && make install )
