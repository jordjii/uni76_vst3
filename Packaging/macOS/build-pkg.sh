#!/usr/bin/env bash
#
# UNI 76 - macOS .pkg build SKELETON.
#
# This script is NOT functional yet and must not be run expecting a signed,
# notarized installer to come out the other end - there is no Developer ID
# certificate or macOS build/signing machine available at this stage of the
# project (see README.md in this folder). It documents the exact sequence
# of steps the real packaging pipeline will need, so that work is a matter
# of filling in the marked TODOs rather than re-deriving the process.
#
# No certificates, passwords, Apple ID credentials, or app-specific
# passwords may ever be committed to this repository. Every credential
# referenced below must come from the environment (CI secrets) at run time.

set -euo pipefail

# --- Configuration (keep in sync with cmake/PluginIdentity.cmake) -----------
BUNDLE_ID="com.nostalgiaaudio.uni76"
PRODUCT_NAME="UNI 76"
VERSION="0.1.0"
INSTALL_LOCATION="/Library/Audio/Plug-Ins/VST3"

# TODO: set once a signing identity exists, e.g.
#   DEVELOPER_ID_APPLICATION="Developer ID Application: Nostalgia Audio (TEAMID)"
#   DEVELOPER_ID_INSTALLER="Developer ID Installer: Nostalgia Audio (TEAMID)"
#   NOTARIZE_KEYCHAIN_PROFILE="uni76-notarytool"   # set up via `xcrun notarytool store-credentials`
DEVELOPER_ID_APPLICATION="${DEVELOPER_ID_APPLICATION:-}"
DEVELOPER_ID_INSTALLER="${DEVELOPER_ID_INSTALLER:-}"
NOTARIZE_KEYCHAIN_PROFILE="${NOTARIZE_KEYCHAIN_PROFILE:-}"

VST3_PATH="${1:?usage: build-pkg.sh <path-to-built .vst3>}"

if [[ -z "${DEVELOPER_ID_APPLICATION}" || -z "${DEVELOPER_ID_INSTALLER}" || -z "${NOTARIZE_KEYCHAIN_PROFILE}" ]]; then
    echo "error: signing identities are not configured. This is expected at the" >&2
    echo "current project stage - see Packaging/macOS/README.md. Not proceeding." >&2
    exit 1
fi

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "${WORK_DIR}"' EXIT

# 1) Codesign the plugin bundle itself (Developer ID Application identity,
#    hardened runtime). TODO: pass a real entitlements file once the plugin
#    needs any (it needs none at this stage - no DSP, no networking, no
#    file access beyond WebView2-equivalent user-data folders on Windows;
#    on macOS WKWebView needs no equivalent entitlement for local content).
# codesign --force --deep --options runtime \
#     --sign "${DEVELOPER_ID_APPLICATION}" \
#     "${VST3_PATH}"

# 2) Stage the payload and build the component + product .pkg.
# STAGE_ROOT="${WORK_DIR}/root${INSTALL_LOCATION}"
# mkdir -p "${STAGE_ROOT}"
# cp -R "${VST3_PATH}" "${STAGE_ROOT}/"
#
# pkgbuild --root "${WORK_DIR}/root" \
#     --identifier "${BUNDLE_ID}.pkg" \
#     --version "${VERSION}" \
#     --install-location "/" \
#     --sign "${DEVELOPER_ID_INSTALLER}" \
#     "${WORK_DIR}/UNI76-component.pkg"
#
# productbuild --package "${WORK_DIR}/UNI76-component.pkg" \
#     --sign "${DEVELOPER_ID_INSTALLER}" \
#     "UNI76-${VERSION}.pkg"

# 3) Notarize and staple.
# xcrun notarytool submit "UNI76-${VERSION}.pkg" \
#     --keychain-profile "${NOTARIZE_KEYCHAIN_PROFILE}" \
#     --wait
#
# xcrun stapler staple "UNI76-${VERSION}.pkg"

echo "This script is a documented skeleton only - see the TODOs above." >&2
exit 1
