#!/usr/bin/env bash
# Install the two game apps plus one launcher per mod into ~/GeneralsX/Apps.
#
#   ./scripts/mods/install-macos-launchers.sh
#
# Inputs (already built):
#   GeneralsX-macos-arm64.zip     base game   (bundle-macos-generals.sh)
#   GeneralsXZH-macos-arm64.zip   Zero Hour   (bundle-macos-zh.sh)
#   build/mods/<Name>-generals.big, build/mods/<Name>-zh.big   (make_big.py)
#
# Result in ~/GeneralsX/Apps:
#   GeneralsX.app                 base game, no mod
#   GeneralsXZH.app               Zero Hour, no mod
#   GeneralsX + <Name>.app        base game with -mod ~/GeneralsX/Mods/<Name>-generals.big
#   GeneralsXZH + <Name>.app      Zero Hour with -mod ~/GeneralsX/Mods/<Name>-zh.big
#
# A mod launcher is a tiny app that starts the real app with -mod; the
# original .big archives are never touched. Delete a launcher (or the .big in
# ~/GeneralsX/Mods) to remove the mod.
#
# Game data is not installed here. Expected locations (same as the app defaults):
#   ~/GeneralsX/GeneralsZH            Zero Hour data
#   ~/GeneralsX/Generals              base game data (for the Steam layout this
#                                     can be a symlink to GeneralsZH/ZH_Generals)
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
APPS_DIR="${HOME}/GeneralsX/Apps"
MODS_DIR="${HOME}/GeneralsX/Mods"

mkdir -p "${APPS_DIR}" "${MODS_DIR}"

for zip in GeneralsX-macos-arm64.zip GeneralsXZH-macos-arm64.zip; do
    if [[ ! -f "${PROJECT_ROOT}/${zip}" ]]; then
        echo "ERROR: ${zip} not found in ${PROJECT_ROOT}; run the bundle script first." >&2
        exit 1
    fi
    tmp="$(mktemp -d)"
    unzip -q "${PROJECT_ROOT}/${zip}" -d "${tmp}"
    app="$(find "${tmp}" -maxdepth 1 -name '*.app' | head -1)"
    rm -rf "${APPS_DIR}/$(basename "${app}")"
    mv "${app}" "${APPS_DIR}/"
    rm -rf "${tmp}"
    echo "  + ${APPS_DIR}/$(basename "${app}")"
done

make_launcher() {
    local target_app="$1" mod_big="$2" name="$3"
    local bundle="${APPS_DIR}/${name}.app"
    rm -rf "${bundle}"
    mkdir -p "${bundle}/Contents/MacOS"
    cat > "${bundle}/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key><string>launch</string>
    <key>CFBundleIdentifier</key><string>local.generalsx.modlauncher.$(echo "${name}" | tr -cd 'A-Za-z0-9')</string>
    <key>CFBundleName</key><string>${name}</string>
    <key>CFBundlePackageType</key><string>APPL</string>
</dict>
</plist>
EOF
    cat > "${bundle}/Contents/MacOS/launch" <<EOF
#!/bin/bash
# Starts ${target_app} with the mod ${mod_big}.
exec "${APPS_DIR}/${target_app}/Contents/MacOS/run.sh" -mod "${mod_big}" "\$@"
EOF
    chmod +x "${bundle}/Contents/MacOS/launch"
    echo "  + ${bundle}"
}

shopt -s nullglob
for big in "${PROJECT_ROOT}"/build/mods/*-generals.big "${PROJECT_ROOT}"/build/mods/*-zh.big; do
    cp "${big}" "${MODS_DIR}/"
    file="$(basename "${big}" .big)"
    mod="${file%-*}"
    case "${file}" in
        *-generals) make_launcher GeneralsX.app "${MODS_DIR}/${file}.big" "GeneralsX + ${mod}" ;;
        *-zh)       make_launcher GeneralsXZH.app "${MODS_DIR}/${file}.big" "GeneralsXZH + ${mod}" ;;
    esac
done

echo "Done. Launch from Finder (${APPS_DIR}) or e.g.:"
echo "  open \"${APPS_DIR}/GeneralsXZH + HelloMod.app\""
