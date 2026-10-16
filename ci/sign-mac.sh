#!/usr/bin/env bash
# CI helper (identical copy in each plugin repo): Developer ID sign + notarize + staple every bundle in a folder.
#   usage: ci/sign-mac.sh <dist dir> <zip name>
# Needs secrets MACOS_CERT_P12 (base64 .p12), MACOS_CERT_PASSWORD, APPLE_ID, APPLE_TEAM_ID, APPLE_APP_PASSWORD.
# With no certificate configured it just zips (ad-hoc signed build), so tags still produce artifacts.
set -euo pipefail
dist="$1"; zipname="$2"
if [ -z "${MACOS_CERT_P12:-}" ]; then
  echo "::warning::No MACOS_CERT_P12 secret - shipping unsigned (not notarized) build"
  (cd "$dist" && zip -ry "../$zipname" .)
  exit 0
fi
keychain="$RUNNER_TEMP/signing.keychain-db"
kcpass="$(uuidgen)"
echo "$MACOS_CERT_P12" | base64 --decode > "$RUNNER_TEMP/cert.p12"
security create-keychain -p "$kcpass" "$keychain"
security set-keychain-settings -lut 21600 "$keychain"
security unlock-keychain -p "$kcpass" "$keychain"
security import "$RUNNER_TEMP/cert.p12" -P "$MACOS_CERT_PASSWORD" -A -t cert -f pkcs12 -k "$keychain"
security set-key-partition-list -S apple-tool:,apple:,codesign: -s -k "$kcpass" "$keychain" >/dev/null
security list-keychain -d user -s "$keychain" login.keychain-db
identity=$(security find-identity -v -p codesigning "$keychain" | grep "Developer ID Application" | head -1 | sed -E 's/.*"(.*)".*/\1/')
[ -n "$identity" ] || { echo "No Developer ID Application identity in the p12"; exit 1; }
echo "Signing with: $identity"
sign() { codesign --force --timestamp --options runtime --sign "$identity" "$@"; }
for b in "$dist"/*.vst3 "$dist"/*.component "$dist"/*.app; do
  [ -e "$b" ] || continue
  # nested dylibs / frameworks first (XTRACT ships onnxruntime in Contents/Frameworks)
  find "$b" -type f \( -name "*.dylib" -o -perm -u+x \) -path "*/Contents/*" ! -path "*/Contents/MacOS/*" -print0 | xargs -0 -I{} codesign --force --timestamp --options runtime --sign "$identity" "{}" || true
  sign --deep "$b"
  codesign --verify --deep --strict "$b"
done
(cd "$dist" && zip -ry "../notarize-$zipname" .)
xcrun notarytool submit "notarize-$zipname" --apple-id "$APPLE_ID" --team-id "$APPLE_TEAM_ID" --password "$APPLE_APP_PASSWORD" --wait --timeout 30m
for b in "$dist"/*.vst3 "$dist"/*.component "$dist"/*.app; do [ -e "$b" ] && xcrun stapler staple "$b"; done
rm -f "notarize-$zipname"
(cd "$dist" && zip -ry "../$zipname" .)
spctl -a -vv -t install "$dist"/*.component 2>&1 | tail -2 || true
security delete-keychain "$keychain"
