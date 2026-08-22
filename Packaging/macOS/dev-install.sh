#!/usr/bin/env bash
#
# UNI 76 - macOS developer install.
#
# Copies a locally built .vst3 bundle into the current user's
# ~/Library/Audio/Plug-Ins/VST3 folder for development use. Not run
# automatically by the build (COPY_PLUGIN_AFTER_BUILD is off); the
# production installer will use the system-wide
# /Library/Audio/Plug-Ins/VST3 location instead (see README.md in this
# folder).
#
# Usage:
#   ./Packaging/macOS/dev-install.sh [Debug|Release]

set -euo pipefail

CONFIGURATION="${1:-Release}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build/macos-$(echo "${CONFIGURATION}" | tr '[:upper:]' '[:lower:]')"

if [[ ! -d "${BUILD_DIR}" ]]; then
    echo "error: build directory '${BUILD_DIR}' does not exist. Configure and build first (see docs/BUILD.md)." >&2
    exit 1
fi

VST3_PATH="$(find "${BUILD_DIR}" -type d -name "*.vst3" -path "*${CONFIGURATION}*" | head -n 1)"

if [[ -z "${VST3_PATH}" ]]; then
    VST3_PATH="$(find "${BUILD_DIR}" -type d -name "*.vst3" | head -n 1)"
fi

if [[ -z "${VST3_PATH}" ]]; then
    echo "error: no built .vst3 bundle found under '${BUILD_DIR}'." >&2
    exit 1
fi

TARGET_ROOT="${HOME}/Library/Audio/Plug-Ins/VST3"
mkdir -p "${TARGET_ROOT}"

DESTINATION="${TARGET_ROOT}/$(basename "${VST3_PATH}")"

echo "Installing:"
echo "  from: ${VST3_PATH}"
echo "  to:   ${DESTINATION}"

rm -rf "${DESTINATION}"
cp -R "${VST3_PATH}" "${DESTINATION}"

echo ""
echo "Done."
