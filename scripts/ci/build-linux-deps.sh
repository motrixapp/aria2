#!/bin/sh
# Build the static C deps aria2 needs, cross for $1 (a musl triple), into $PREFIX.
set -eu
TRIPLE="$1"
j="$(nproc)"
mkdir -p /src && cd /src
fetch() { curl -fSL --retry 3 --retry-delay 2 --retry-connrefused -o "$2" "$1"; tar xf "$2"; }
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
# openssl 3.5 LTS. no-module compiles the legacy provider INTO libcrypto.a as a
# built-in, so aria2's OSSL_PROVIDER_load(NULL,"legacy") (RC4, for BitTorrent MSE)
# resolves it in-process instead of dlopen'ing legacy.so — which a fully-static
# binary cannot do. no-shared alone does NOT disable the dynamic provider module.
# Per-arch target: generic64 assumes a 64-bit word (breaks 32-bit armv7l).
# CC/AR/RANLIB come from the Dockerfile ENV (already ${TRIPLE}-*); do NOT also pass
# --cross-compile-prefix or openssl doubles it (…-musl-…-musl-gcc: not found).
fetch https://github.com/openssl/openssl/releases/download/openssl-3.5.7/openssl-3.5.7.tar.gz o.tgz
case "$TRIPLE" in
  x86_64-*)  ossl_target=linux-x86_64 ;;
  aarch64-*) ossl_target=linux-aarch64 ;;
  arm*)      ossl_target=linux-armv4 ;;
  *)         ossl_target=linux-generic64 ;;
esac
( cd openssl-3.5.7 && ./Configure no-shared no-module no-tests --prefix="$PREFIX" --libdir=lib "$ossl_target" && make -j$j && make install_sw )
# libssh2
fetch https://github.com/libssh2/libssh2/releases/download/libssh2-1.11.1/libssh2-1.11.1.tar.gz h.tgz
( cd libssh2-1.11.1 && ./configure --host="$TRIPLE" --enable-static --disable-shared --with-crypto=openssl --with-libssl-prefix="$PREFIX" --prefix="$PREFIX" && make -j$j && make install )
# gmp
fetch https://gmplib.org/download/gmp/gmp-6.3.0.tar.xz g.txz
( cd gmp-6.3.0 && ./configure --host="$TRIPLE" --enable-static --disable-shared --prefix="$PREFIX" && make -j$j && make install )
