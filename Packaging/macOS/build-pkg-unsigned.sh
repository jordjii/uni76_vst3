#!/usr/bin/env bash
#
# UNI 76 - macOS .pkg build, UNSIGNED / UNNOTARIZED variant.
#
# This is a local-use counterpart to build-pkg.sh (the signed/notarized
# production pipeline, which refuses to run without real Developer ID
# credentials - see README.md in this folder). This script produces a real,
# installable .pkg with no certificate involved at all: no codesign, no
# pkgbuild/productbuild --sign, no notarytool submission. It exists so the
# plugin can be packaged and installed on a Mac before Developer ID
# credentials are obtained.
#
# Consequence: Gatekeeper will not let a double-clicked .pkg run without an
# explicit user override (System Settings > Privacy & Security > "Open
# Anyway", or right-click > Open, on the .pkg itself) on a fresh download.
# This is expected and is not a bug in this script - do not try to work
# around it here (e.g. by disabling Gatekeeper system-wide); that decision
# belongs to whoever installs the plugin, not to this build script.
#
# No certificates, private keys, Apple ID credentials, or app-specific
# passwords are used or required by this script.

set -euo pipefail

# --- Configuration (keep in sync with cmake/PluginIdentity.cmake) -----------
BUNDLE_ID="com.nostalgiaaudio.uni76"
PRODUCT_NAME="UNI 76"
VERSION="0.1.0"
VST3_INSTALL_LOCATION="/Library/Audio/Plug-Ins/VST3"
AU_INSTALL_LOCATION="/Library/Audio/Plug-Ins/Components"

VST3_PATH="${1:?usage: build-pkg-unsigned.sh <path-to-built .vst3> <path-to-built .component> [output-dir]}"
AU_PATH="${2:?usage: build-pkg-unsigned.sh <path-to-built .vst3> <path-to-built .component> [output-dir]}"
OUTPUT_DIR="${3:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/Output}"

if [[ ! -d "${VST3_PATH}" ]]; then
    echo "error: '${VST3_PATH}' does not exist or is not a directory (expected a built .vst3 bundle)." >&2
    exit 1
fi
if [[ ! -d "${AU_PATH}" ]]; then
    echo "error: '${AU_PATH}' does not exist or is not a directory (expected a built .component bundle)." >&2
    exit 1
fi

mkdir -p "${OUTPUT_DIR}"

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "${WORK_DIR}"' EXIT

# 1) Stage both payloads at the exact install locations the component
#    package will reproduce on the target system.
VST3_STAGE_ROOT="${WORK_DIR}/root${VST3_INSTALL_LOCATION}"
AU_STAGE_ROOT="${WORK_DIR}/root${AU_INSTALL_LOCATION}"
mkdir -p "${VST3_STAGE_ROOT}" "${AU_STAGE_ROOT}"
cp -R "${VST3_PATH}" "${VST3_STAGE_ROOT}/"
cp -R "${AU_PATH}" "${AU_STAGE_ROOT}/"

# 2) Build the component package (unsigned) - one package installs both
#    formats, since they share the same install root.
COMPONENT_PKG="${WORK_DIR}/UNI76-component.pkg"
pkgbuild --root "${WORK_DIR}/root" \
    --identifier "${BUNDLE_ID}.pkg" \
    --version "${VERSION}" \
    --install-location "/" \
    "${COMPONENT_PKG}"

# 3) Build the product archive (unsigned) - this is the file a user
#    double-clicks to install.
OUTPUT_PKG="${OUTPUT_DIR}/UNI76-${VERSION}-macOS-unsigned.pkg"
productbuild --package "${COMPONENT_PKG}" \
    "${OUTPUT_PKG}"

echo ""
echo "Built (UNSIGNED, UNNOTARIZED): ${OUTPUT_PKG}"
echo "Installs '${PRODUCT_NAME}.vst3' to ${VST3_INSTALL_LOCATION}"
echo "     and '${PRODUCT_NAME}.component' (AU) to ${AU_INSTALL_LOCATION}"
echo "(system-wide, all users)."
echo ""
echo "Because this .pkg is not signed with a Developer ID Installer certificate"
echo "or notarized, Gatekeeper will block it on first run. To install:"
echo "  right-click the .pkg -> Open -> Open (or System Settings > Privacy &"
echo "  Security > 'Open Anyway' after the first blocked attempt)."
