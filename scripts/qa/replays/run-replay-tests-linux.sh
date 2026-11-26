#!/usr/bin/env bash
# Play every test replay headless and report PASS/FAIL (Linux), the counterpart of
# run-replay-tests-macos.sh and run-replay-tests-windows.ps1. The replays are the macOS
# recordings, so a pass here means the Linux build simulates exactly like the macOS one
# (cross-platform determinism).
#
#   ./scripts/qa/replays/run-replay-tests-linux.sh <GeneralsXZH binary> [replay dir]
#
# The binary is e.g. build/linux64-deploy/GeneralsMD/GeneralsXZH (its shared libraries are
# found in the build tree) or a deployed GeneralsXZH with the libraries next to it.
# Defaults: replays are macos_*.rep from GeneralsReplays/GeneralsXZH/Replays and
# Tests/replays/macos (a later dir replaces a replay with the same name); with a replay dir
# given, only that dir is used. Old 4-byte wide char replays are converted with
# convert_wchar32_replay.py. Maps come from GeneralsReplays/GeneralsXZH/Maps (or MAPS_DIR).
# Game data: CNC_GENERALS_ZH_PATH / CNC_GENERALS_PATH, else ~/GeneralsX/GeneralsZH and
# ~/GeneralsX/Generals.
#
# The run uses a throwaway user data folder (GENERALSX_USER_DATA_DIR under WORK_DIR, default
# TMPDIR or /tmp), so the real profile, maps and replays are not touched. The game runs with
# -headless (no window, no GPU, no SDL video). Its working directory is a staged folder holding
# the three loose stock files (Data/Scripts/SkirmishScripts.scb, MultiplayerScripts.scb,
# Scripts.ini): on Windows they are only looked up in the working directory, and without
# SkirmishScripts.scb the skirmish AI gets no teams and the replay desyncs at once. (On Linux
# they are also found through the asset root, CNC_GENERALS_ZH_PATH; staging makes it certain.)
# Logs: logs/replay-tests/ (<name>.log, and <name>.crc with the compared CRCs as
# "frame crc" lines, for diffing against another platform).
#
# Options (environment): EXTRA_ARGS=... extra game arguments (e.g. -ignoreReplaySyncErrors),
# NO_CONVERT=1 copy replays as they are, KEEP_DEBUG=<dir> keep the game's Debug folder (deep CRC
# dumps written on a mismatch), REPLAY_TIMEOUT=<seconds> (default 900), WORK_DIR=<dir>.
#
# A replay passes only if the game exits 0 AND compared at least one CRC (REPLAY_CRC_COMPARE
# lines) AND none of them mismatched. Exit code: number of failed replays (0 = all passed).
set -uo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
[[ $# -ge 1 ]] || { echo "usage: $0 <GeneralsXZH binary> [replay dir]" >&2; exit 100; }
GAME="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
[[ -x "${GAME}" ]] || { echo "not an executable: $1" >&2; exit 100; }
if [[ $# -ge 2 ]]; then
    REPLAY_DIRS=("$2")
else
    REPLAY_DIRS=("${PROJECT_ROOT}/GeneralsReplays/GeneralsXZH/Replays" "${PROJECT_ROOT}/Tests/replays/macos")
fi
CONVERTER="${PROJECT_ROOT}/scripts/replays/convert_wchar32_replay.py"
LOG_DIR="${PROJECT_ROOT}/logs/replay-tests"
MAPS_DIR="${MAPS_DIR:-${PROJECT_ROOT}/GeneralsReplays/GeneralsXZH/Maps}"
TIMEOUT_SECONDS="${REPLAY_TIMEOUT:-900}"

ZH_DATA="$(cd "${CNC_GENERALS_ZH_PATH:-${HOME}/GeneralsX/GeneralsZH}" && pwd -P)" || { echo "Zero Hour data not found" >&2; exit 100; }
GEN_DATA="$(cd "${CNC_GENERALS_PATH:-${HOME}/GeneralsX/Generals}" && pwd -P)" || { echo "Generals data not found" >&2; exit 100; }

# REPLAY_DOCKER_IMAGE=<image>: run this script inside that image (e.g. generalsx/linux-builder:latest,
# which scripts/build/linux/docker-build-linux-zh.sh builds with). Needed when the binary was built
# in the container against a newer glibc/libstdc++ than the host has. Every path is mounted at the
# same place, so logs land in this checkout as usual.
if [[ -n "${REPLAY_DOCKER_IMAGE:-}" && -z "${REPLAY_IN_DOCKER:-}" ]]; then
    work_root="${WORK_DIR:-${TMPDIR:-/tmp}}"
    mounts=()
    [[ -n "${KEEP_DEBUG:-}" ]] && mkdir -p "${KEEP_DEBUG}" && KEEP_DEBUG="$(cd "${KEEP_DEBUG}" && pwd)"
    for d in "${KEEP_DEBUG:-}" "${PROJECT_ROOT}" "${ZH_DATA}" "${GEN_DATA}" "$(dirname "${GAME}")" "${work_root}" ${2:+"$(cd "$2" && pwd)"} "${MAPS_DIR}"; do
        [[ -d "${d}" ]] && mounts+=(-v "${d}:${d}")
    done
    exec docker run --rm --user "$(id -u):$(id -g)" --network none "${mounts[@]}" \
        -e REPLAY_IN_DOCKER=1 -e HOME=/tmp -e WORK_DIR="${work_root}" \
        -e CNC_GENERALS_ZH_PATH="${ZH_DATA}" -e CNC_GENERALS_PATH="${GEN_DATA}" -e MAPS_DIR="${MAPS_DIR}" \
        -e EXTRA_ARGS="${EXTRA_ARGS:-}" -e NO_CONVERT="${NO_CONVERT:-}" -e KEEP_DEBUG="${KEEP_DEBUG:-}" \
        -e REPLAY_TIMEOUT="${TIMEOUT_SECONDS}" -w "${PROJECT_ROOT}" \
        "${REPLAY_DOCKER_IMAGE}" "${PROJECT_ROOT}/scripts/qa/replays/run-replay-tests-linux.sh" "${GAME}" ${2:+"$(cd "$2" && pwd)"}
fi

WORK="$(mktemp -d "${WORK_DIR:-${TMPDIR:-/tmp}}/replay-profile-XXXXXXXX")"
trap '[ -n "${KEEP_DEBUG:-}" ] && [ -d "${WORK}/user/Debug" ] && mkdir -p "${KEEP_DEBUG}" && cp -R "${WORK}/user/Debug/." "${KEEP_DEBUG}/"; rm -rf "${WORK}"' EXIT
USER_DATA="${WORK}/user"
GAME_DIR="${WORK}/game"
mkdir -p "${USER_DATA}/Maps" "${USER_DATA}/Replays" "${GAME_DIR}/Data/Scripts" "${LOG_DIR}"
[[ -d "${MAPS_DIR}" ]] && cp -R "${MAPS_DIR}/." "${USER_DATA}/Maps/"
for loose in SkirmishScripts.scb MultiplayerScripts.scb Scripts.ini; do
    [[ -f "${ZH_DATA}/Data/Scripts/${loose}" ]] && cp "${ZH_DATA}/Data/Scripts/${loose}" "${GAME_DIR}/Data/Scripts/"
done

shopt -s nullglob
replays=()
for dir in "${REPLAY_DIRS[@]}"; do
    for rep in "${dir}"/macos_*.rep; do
        kept=()
        for r in ${replays[@]+"${replays[@]}"}; do
            [[ "$(basename "${r}")" == "$(basename "${rep}")" ]] || kept+=("${r}")
        done
        replays=(${kept[@]+"${kept[@]}"} "${rep}")
    done
done
if (( ${#replays[@]} == 0 )); then
    echo "no macos_*.rep in ${REPLAY_DIRS[*]}" >&2
    exit 100
fi

# Shared libraries: next to the binary (a deployed folder), or, for a binary straight from a
# CMake build tree (build/<preset>/GeneralsMD/GeneralsXZH), the build root (libgamespy.so) and
# the fetched SDL3 / SDL3_image / DXVK libraries.
GAME_LIBS="$(dirname "${GAME}")"
BUILD_ROOT="$(dirname "$(dirname "${GAME}")")"
if [[ -f "${BUILD_ROOT}/CMakeCache.txt" ]]; then
    for d in "${BUILD_ROOT}" "${BUILD_ROOT}/_deps/sdl3-build" "${BUILD_ROOT}/_deps/sdl3_image-build" "${BUILD_ROOT}/_deps/dxvk-src/lib"; do
        [[ -d "${d}" ]] && GAME_LIBS="${GAME_LIBS}:${d}"
    done
fi

pass=0 fail=0
for rep in "${replays[@]}"; do
    name="$(basename "${rep}")"
    # Old-format replays are converted; the converter refuses a replay that is already new.
    if [[ -n "${NO_CONVERT:-}" ]]; then cp "${rep}" "${USER_DATA}/Replays/${name}"; else
    python3 -I "${CONVERTER}" "${rep}" "${USER_DATA}/Replays/${name}" > /dev/null 2>&1 \
        || cp "${rep}" "${USER_DATA}/Replays/${name}"; fi
    log="${LOG_DIR}/${name%.rep}.log"
    start=$(date +%s)
    ( cd "${GAME_DIR}" && \
      GENERALSX_USER_DATA_DIR="${USER_DATA}" CNC_GENERALS_ZH_PATH="${ZH_DATA}" CNC_GENERALS_PATH="${GEN_DATA}" \
      LD_LIBRARY_PATH="${GAME_LIBS}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}" \
      timeout -k 10 "${TIMEOUT_SECONDS}" "${GAME}" -headless ${EXTRA_ARGS:-} -replay "${USER_DATA}/Replays/${name}" ) > "${log}" 2>&1
    code=$?
    (( code == 124 )) && code=timeout
    secs=$(( $(date +%s) - start ))
    grep -a -o "REPLAY_CRC_COMPARE frame=[0-9]* inGame=0x[0-9A-F]*" "${log}" \
        | sed -E 's/.*frame=([0-9]+) inGame=0x([0-9A-F]+)/\1 \2/' > "${log%.log}.crc"
    compares=$(grep -a -c "REPLAY_CRC_COMPARE" "${log}")
    mismatches=$(grep -a -c "REPLAY_CRC_COMPARE.* MISMATCH" "${log}")
    if [[ "${code}" == 0 ]] && (( compares == 0 )); then
        # Exit 0 without a single compared CRC (map did not load, CRC sent by another slot,
        # replay too short) means nothing, so it does not count as a pass.
        fail=$((fail + 1))
        echo "FAIL  ${name}  (${secs}s)  CRC was never compared  log: ${log}"
    elif [[ "${code}" == 0 ]] && (( mismatches == 0 )); then
        pass=$((pass + 1))
        echo "PASS  ${name}  (${secs}s)  ${compares} CRC compares, all equal"
    else
        fail=$((fail + 1))
        detail="$(grep -a -E "CRC Mismatch|Cannot open replay|Game Time" "${log}" | sort -u | tail -2 | tr '\n' ' ')"
        echo "FAIL  ${name}  exit ${code} (${secs}s)  ${compares} compares, ${mismatches} mismatched  ${detail}  log: ${log}"
    fi
done
echo "Results: ${pass} passed, ${fail} failed"
exit "${fail}"
