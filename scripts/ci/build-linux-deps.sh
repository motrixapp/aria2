#!/bin/sh
# Build the static C deps aria2 needs, cross for $1 (a musl triple), into $PREFIX.
set -eu
TRIPLE="$1"
j="$(nproc)"
# 32-bit arm has no native 8-byte atomics; openssl's threads_pthread.c references
# __atomic_*_8, which armv7 resolves via libatomic (linked by libssh2's examples,
# and later by aria2). 64-bit targets resolve them as intrinsics.
case "$TRIPLE" in
  arm*)
    ATOMIC_LIB=-latomic
    # GMP selects ARMv6 mpn asm (umaal) for armv7l, but this toolchain assembles
    # below armv6 by default; passing -march to fix that instead breaks GMP's
    # own compiler probe ("could not find a working compiler"). Disable GMP's
    # assembly and use the portable C mpn — fast enough for aria2's occasional
    # bignum use, and it sidesteps the whole arm-asm arch-mismatch class.
    GMP_EXTRA=--disable-assembly
    ;;
  *)
    ATOMIC_LIB=
    GMP_EXTRA=
    ;;
esac
# Shared dependency versions (see scripts/ci/deps.env). dirname "$0" is / in the
# Dockerfile.linux container, where deps.env is COPY'd to /deps.env.
. "$(dirname "$0")/deps.env"
mkdir -p /src && cd /src
# --speed-limit 1 --speed-time 30 aborts a transfer stalled below 1 B/s for 30s
# (e.g. gmplib.org accepting the connection then sending 0 bytes) so --retry can
# re-attempt instead of hanging until the job timeout.
fetch() { curl -fSL --retry 5 --retry-delay 3 --retry-connrefused --retry-all-errors --connect-timeout 30 --speed-limit 1 --speed-time 30 -o "$2" "$1"; tar xf "$2"; }
# zlib
fetch https://github.com/madler/zlib/releases/download/v$ZLIB_VERSION/zlib-$ZLIB_VERSION.tar.gz z.tgz
( cd zlib-$ZLIB_VERSION && CHOST="$TRIPLE" ./configure --static --prefix="$PREFIX" && make -j$j && make install )
# c-ares
fetch https://github.com/c-ares/c-ares/releases/download/v$CARES_VERSION/c-ares-$CARES_VERSION.tar.gz c.tgz
( cd c-ares-$CARES_VERSION && ./configure --host="$TRIPLE" --enable-static --disable-shared --prefix="$PREFIX" && make -j$j && make install )
# expat
fetch https://github.com/libexpat/libexpat/releases/download/R_2_7_5/expat-$EXPAT_VERSION.tar.bz2 e.tbz
( cd expat-$EXPAT_VERSION && ./configure --host="$TRIPLE" --enable-static --disable-shared --without-docbook --prefix="$PREFIX" && make -j$j && make install )
# sqlite3
fetch https://www.sqlite.org/$SQLITE_YEAR/sqlite-$SQLITE_VERSION.tar.gz s.tgz
( cd sqlite-$SQLITE_VERSION && ./configure --host="$TRIPLE" --enable-static --disable-shared --prefix="$PREFIX" && make -j$j && make install )
# openssl 3.5 LTS. no-module compiles the legacy provider INTO libcrypto.a as a
# built-in, so aria2's OSSL_PROVIDER_load(NULL,"legacy") (RC4, for BitTorrent MSE)
# resolves it in-process instead of dlopen'ing legacy.so — which a fully-static
# binary cannot do. no-shared alone does NOT disable the dynamic provider module.
# Per-arch target: generic64 assumes a 64-bit word (breaks 32-bit armv7l).
# CC/AR/RANLIB come from the Dockerfile ENV (already ${TRIPLE}-*); do NOT also pass
# --cross-compile-prefix or openssl doubles it (…-musl-…-musl-gcc: not found).
fetch https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL_VERSION/openssl-$OPENSSL_VERSION.tar.gz o.tgz
case "$TRIPLE" in
  x86_64-*)  ossl_target=linux-x86_64 ;;
  aarch64-*) ossl_target=linux-aarch64 ;;
  arm*)      ossl_target=linux-armv4 ;;
  *)         ossl_target=linux-generic64 ;;
esac
( cd openssl-$OPENSSL_VERSION && ./Configure no-shared no-module no-tests --prefix="$PREFIX" --libdir=lib "$ossl_target" && make -j$j && make install_sw )
# libssh2
fetch https://github.com/libssh2/libssh2/releases/download/libssh2-$LIBSSH2_VERSION/libssh2-$LIBSSH2_VERSION.tar.gz h.tgz
( cd libssh2-$LIBSSH2_VERSION && ./configure --host="$TRIPLE" --enable-static --disable-shared --with-crypto=openssl --with-libssl-prefix="$PREFIX" --prefix="$PREFIX" LIBS="$ATOMIC_LIB" && make -j$j && make install )
# gmp — from the GNU mirror; gmplib.org stalls (0 bytes) against CI/datacenter IPs
fetch https://ftp.gnu.org/gnu/gmp/gmp-$GMP_VERSION.tar.xz g.txz
( cd gmp-$GMP_VERSION && ./configure --host="$TRIPLE" --enable-static --disable-shared --prefix="$PREFIX" $GMP_EXTRA && make -j$j && make install )
