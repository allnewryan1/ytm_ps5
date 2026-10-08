#!/usr/bin/env bash
# Install the PS5 Payload SDK during `docker build`.
# Same release zip EVO Player pins (ps5-payload-dev/sdk).
set -euo pipefail

PS5_SDK_VERSION="${PS5_SDK_VERSION:-v0.42}"
PS5_PAYLOAD_SDK="${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}"
SDK_REPO="https://github.com/ps5-payload-dev/sdk"

die() { echo "ERROR [install-sdk-image]: $*" >&2; exit 1; }
log() { echo "==> [sdk] $*"; }

command -v llvm-config >/dev/null 2>&1 || die "llvm-config not on PATH"
command -v clang >/dev/null 2>&1 || die "clang not on PATH"
command -v ld.lld >/dev/null 2>&1 || die "ld.lld not on PATH"
log "host llvm-config: $(llvm-config --version) at $(llvm-config --bindir)"

tmp="$(mktemp -d)"
url="${SDK_REPO}/releases/download/${PS5_SDK_VERSION}/ps5-payload-sdk.zip"
log "installing SDK release ${PS5_SDK_VERSION}"
wget -q -O "${tmp}/sdk.zip" "${url}" || die "download failed: ${url}"
unzip -q "${tmp}/sdk.zip" -d "${tmp}/x" || die "unzip failed"
mkdir -p "${PS5_PAYLOAD_SDK}"
if [[ -d "${tmp}/x/ps5-payload-sdk" ]]; then
  cp -a "${tmp}/x/ps5-payload-sdk/." "${PS5_PAYLOAD_SDK}/"
else
  cp -a "${tmp}/x/." "${PS5_PAYLOAD_SDK}/"
fi
rm -rf "${tmp}"

cat > "${PS5_PAYLOAD_SDK}/YTM_SDK_VERSION" <<EOF
ps5_payload_sdk_version=${PS5_SDK_VERSION}
installed_from=release-zip
host_llvm_version=$(llvm-config --version)
host_clang=$(clang --version | head -1)
installed_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)
EOF

for f in toolchain/prospero.sh toolchain/prospero.mk bin/prospero-clang bin/prospero-lld bin/prospero-deploy; do
  [[ -e "${PS5_PAYLOAD_SDK}/${f}" ]] || die "SDK install incomplete: missing ${f}"
done
chmod -R a+rX "${PS5_PAYLOAD_SDK}"
find "${PS5_PAYLOAD_SDK}/bin" -type f -exec chmod a+rx {} +
log "installed to ${PS5_PAYLOAD_SDK}"
cat "${PS5_PAYLOAD_SDK}/YTM_SDK_VERSION"
