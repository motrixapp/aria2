#!/usr/bin/env bash

set -euo pipefail

usage() {
  cat <<'EOF'
Usage: rebuild-static-osx.sh [options]

Rebuild aria2 statically on macOS using makerelease-osx.mk.

Options:
  --clean              Remove the build directory before rebuilding.
  --package            Build release packages (.tar.bz2, .pkg, .dmg).
  --deps-only          Only build static dependencies.
  --no-deps            Skip the explicit "make deps" step.
  --build-dir PATH     Build directory to use. Default: ./build-release
  --jobs N             Override detected CPU count passed to the makefile.
  -h, --help           Show this help.

Environment:
  NON_RELEASE          Defaults to "force" for non-tag local builds.

Notes:
  If the source tree was previously configured in-place, this script runs
  "make distclean" in the source tree before the release build.
EOF
}

die() {
  echo "error: $*" >&2
  exit 1
}

require_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "missing required command: $1"
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build-release"
DO_CLEAN=0
DO_PACKAGE=0
DEPS_ONLY=0
SKIP_DEPS=0
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"

while (($#)); do
  case "$1" in
    --clean)
      DO_CLEAN=1
      ;;
    --package)
      DO_PACKAGE=1
      ;;
    --deps-only)
      DEPS_ONLY=1
      ;;
    --no-deps)
      SKIP_DEPS=1
      ;;
    --build-dir)
      shift
      (($#)) || die "--build-dir requires a value"
      BUILD_DIR="$1"
      ;;
    --jobs)
      shift
      (($#)) || die "--jobs requires a value"
      JOBS="$1"
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      die "unknown option: $1"
      ;;
  esac
  shift
done

[[ "$(uname -s)" == "Darwin" ]] || die "this script only supports macOS"
[[ "${DEPS_ONLY}" -eq 0 || "${DO_PACKAGE}" -eq 0 ]] || \
  die "--deps-only cannot be combined with --package"

require_cmd make
require_cmd ln
require_cmd python3
require_cmd sysctl

if [[ "${DO_PACKAGE}" -eq 1 ]]; then
  require_cmd sphinx-build
fi

: "${NON_RELEASE:=force}"
ARCH="$(uname -m)"
TARGET="aria2.${ARCH}.build"
MAKEFILE_PATH="${ROOT_DIR}/makerelease-osx.mk"
MAKEFILE_LINK="${BUILD_DIR}/Makefile"

if [[ "${DO_CLEAN}" -eq 1 ]]; then
  echo "==> Removing ${BUILD_DIR}"
  rm -rf "${BUILD_DIR}"
fi

mkdir -p "${BUILD_DIR}"
REL_MAKEFILE="$(python3 -c 'import os,sys; print(os.path.relpath(sys.argv[1], sys.argv[2]))' \
  "${MAKEFILE_PATH}" "${BUILD_DIR}")"
ln -sfn "${REL_MAKEFILE}" "${MAKEFILE_LINK}"

if [[ -f "${ROOT_DIR}/config.status" ]]; then
  echo "==> Source tree has in-tree configure state; running make distclean"
  (
    cd "${ROOT_DIR}"
    make distclean
  )
fi

run_make() {
  local target="${1:-}"
  if [[ -n "${target}" ]]; then
    echo "==> make ${target}"
    (
      cd "${BUILD_DIR}"
      env NON_RELEASE="${NON_RELEASE}" CPUS="${JOBS}" make "${target}"
    )
  else
    echo "==> make"
    (
      cd "${BUILD_DIR}"
      env NON_RELEASE="${NON_RELEASE}" CPUS="${JOBS}" make
    )
  fi
}

if [[ "${SKIP_DEPS}" -eq 0 ]]; then
  run_make deps
fi

if [[ "${DEPS_ONLY}" -eq 1 ]]; then
  echo "==> Dependencies finished"
  exit 0
fi

if [[ "${DO_PACKAGE}" -eq 1 ]]; then
  run_make
else
  run_make "${TARGET}"
fi

OUTPUT_BIN="${BUILD_DIR}/aria2.${ARCH}/aria2c"
if [[ -x "${OUTPUT_BIN}" ]]; then
  echo "==> Built binary: ${OUTPUT_BIN}"
  "${OUTPUT_BIN}" --version | sed -n '1,18p'
else
  die "build finished but ${OUTPUT_BIN} was not found"
fi
