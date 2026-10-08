#!/usr/bin/env bash
# Install the pacbrew homebrew prefix (SDL2 + FFmpeg) without replacing the pinned SDK.
# Same tarball and surgical copy EVO Player uses.
set -euo pipefail

PACBREW_VERSION="${PACBREW_VERSION:-v0.39}"
INSTALL_PACBREW="${INSTALL_PACBREW:-1}"
PS5_PAYLOAD_SDK="${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}"
REPO="https://github.com/ps5-payload-dev/pacbrew-repo"

die() { echo "ERROR [install-pacbrew]: $*" >&2; exit 1; }
log() { echo "==> [pacbrew] $*"; }

if [[ "${INSTALL_PACBREW}" != "1" ]]; then
  log "INSTALL_PACBREW=0 - skipping SDL2 and FFmpeg"
  exit 0
fi

[[ -d "${PS5_PAYLOAD_SDK}" ]] || die "SDK not installed yet at ${PS5_PAYLOAD_SDK}"

tmp="$(mktemp -d)"
trap 'rm -rf -- "${tmp}"' EXIT
url="${REPO}/releases/download/${PACBREW_VERSION}/ps5-payload-dev.tar.gz"
log "downloading ${url}"
wget -q -O "${tmp}/pacbrew.tar.gz" "${url}" || die "download failed: ${url}"
mkdir -p "${tmp}/x"
tar -xzf "${tmp}/pacbrew.tar.gz" -C "${tmp}/x" || die "extract failed"

src="${tmp}/x/opt/ps5-payload-sdk"
hb="${src}/target/user/homebrew"
[[ -d "${hb}" ]] || die "tarball has no target/user/homebrew"

log "installing ports prefix"
mkdir -p "${PS5_PAYLOAD_SDK}/target/user/homebrew"
cp -a "${hb}/." "${PS5_PAYLOAD_SDK}/target/user/homebrew/"

libdir="${PS5_PAYLOAD_SDK}/target/user/homebrew/lib"
missing=()
for l in libSDL2.a libavformat.a libavcodec.a libswresample.a libavutil.a; do
  [[ -f "${libdir}/${l}" ]] || missing+=("${l}")
done
if (( ${#missing[@]} )); then
  die "pacbrew ${PACBREW_VERSION} did not provide: ${missing[*]}"
fi

ffver="unknown"
vhdr="${PS5_PAYLOAD_SDK}/target/user/homebrew/include/libavutil/ffversion.h"
if [[ -f "${vhdr}" ]]; then
  ffver="$(sed -n 's/.*FFMPEG_VERSION *"\([^"]*\)".*/\1/p' "${vhdr}")"
fi
cat > "${PS5_PAYLOAD_SDK}/YTM_PACBREW_VERSION" <<EOF
pacbrew_repo_version=${PACBREW_VERSION}
ffmpeg_version=${ffver}
installed_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)
EOF
log "ok - FFmpeg ${ffver}, SDL2 installed"
cat "${PS5_PAYLOAD_SDK}/YTM_PACBREW_VERSION"
