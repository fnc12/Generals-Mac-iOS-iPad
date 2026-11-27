#!/usr/bin/env bash
# Starts the game headless on maps saved by the END Editor, the way its Play button does (-testMap),
# and checks that each one loads, runs its frames and exits cleanly (macOS).
#
# Usage:
#   ./scripts/qa/editor/run-play-smoke-macos.sh [GeneralsXZH.app] [map path filter]
#
# For every map of the game's archives the editor's core (end-editor-cli --play-maps) saves two
# copies into a throwaway profile's user Maps folder: untouched (EdU<n>) and edited (EdE<n>: an
# object moved, one turned, three vehicles placed, a path of three waypoints, a road segment).
# Then each copy is started with
#   run.sh -headless -testMap <file> -testMapFrames $FRAMES -testMapSeed 1
# (default Play settings: FactionAmerica at start position 1 against one medium AI; a fixed seed,
# so the AI's random faction and start units are the same in both runs). A map passes when both
# runs exit 0 with a "TEST_MAP ok" line (the map started and ran every frame, and in a skirmish the
# local player owns objects) and the edited copy loads exactly as many more objects as vehicles
# were placed. Logs: logs/editor-play/. Exit code: number of failed maps.
#
# Environment: FRAMES (default 300), PLAY_TIMEOUT seconds per run (default 120),
# CLI (default build/editor-core/end-editor-cli, built with cmake -S Editor/core -B build/editor-core),
# CNC_GENERALS_ZH_PATH / CNC_GENERALS_PATH (default ~/GeneralsX/GeneralsZH and ~/GeneralsX/Generals).
# Game data is only read; nothing is written outside the temp profile and logs/.
set -uo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
APP="${1:-${HOME}/GeneralsX/Apps/GeneralsXZH.app}"
FILTER="${2:-}"
FRAMES="${FRAMES:-300}"
TIMEOUT_SECONDS="${PLAY_TIMEOUT:-120}"
CLI="${CLI:-${PROJECT_ROOT}/build/editor-core/end-editor-cli}"
LOG_DIR="${PROJECT_ROOT}/logs/editor-play"

ZH_DATA="$(cd "${CNC_GENERALS_ZH_PATH:-${HOME}/GeneralsX/GeneralsZH}" && pwd -P)" || { echo "Zero Hour data not found" >&2; exit 100; }
GEN_DATA="$(cd "${CNC_GENERALS_PATH:-${HOME}/GeneralsX/Generals}" && pwd -P)" || { echo "Generals data not found" >&2; exit 100; }
[[ -x "${APP}/Contents/MacOS/run.sh" ]] || { echo "not a GeneralsXZH app: ${APP}" >&2; exit 100; }
[[ -x "${CLI}" ]] || { echo "no editor core harness at ${CLI} (build Editor/core)" >&2; exit 100; }

PROFILE="$(mktemp -d)"
trap 'rm -rf "${PROFILE}"' EXIT
USER_MAPS="${PROFILE}/Library/Application Support/GeneralsX/GeneralsZH/Maps"
mkdir -p "${USER_MAPS}" "${LOG_DIR}"
rm -f "${LOG_DIR}"/*.log

args=("${ZH_DATA}" --base "${GEN_DATA}" --db "${PROFILE}/editor.sqlite" --play-maps "${USER_MAPS}")
[[ -n "${FILTER}" ]] && args+=(--only "${FILTER}")
"${CLI}" "${args[@]}" > "${LOG_DIR}/editor.out" 2> "${LOG_DIR}/editor.err"
manifest="$(grep -a "^PLAYMAP" "${LOG_DIR}/editor.out")"
[[ -n "${manifest}" ]] || { echo "the editor saved no maps (see ${LOG_DIR}/editor.err)" >&2; exit 100; }

# Runs the game on one map file; prints the TEST_MAP result line or the reason it failed.
play() {
    local file="$1" log="$2"
    ( cd "${APP}/Contents/MacOS" && \
      CNC_GENERALS_ZH_PATH="${ZH_DATA}" CNC_GENERALS_PATH="${GEN_DATA}" HOME="${PROFILE}" \
      perl -e "alarm ${TIMEOUT_SECONDS}; exec @ARGV" ./run.sh -headless -testMap "${file}" -testMapFrames "${FRAMES}" -testMapSeed 1 ) \
      > "${log}" 2>&1
    local code=$?
    local line
    line="$(grep -a -m1 "^TEST_MAP \(ok\|error\)" "${log}")"
    if (( code == 0 )) && [[ "${line}" == "TEST_MAP ok "* ]]; then
        echo "${line}"
        return 0
    fi
    echo "exit ${code}: ${line:-$(grep -a "TEST_MAP\|ERROR\|Assert\|crash" "${log}" | tail -1)}"
    return 1
}

loaded() { sed -E 's/.* loaded_objects=([0-9]+).*/\1/' <<< "$1"; }

pass=0 fail=0 total=0
while IFS=$'\t' read -r _ index original untouched edited placed; do
    total=$((total + 1))
    name="$(basename "${original}" .map)"
    if [[ "${untouched}" == "-" || "${edited}" == "-" ]]; then
        fail=$((fail + 1))
        echo "FAIL  ${original}  the editor could not open or save it"
        continue
    fi
    start=$(date +%s)
    u="$(play "${untouched}" "${LOG_DIR}/EdU${index}.log")"; uok=$?
    e="$(play "${edited}" "${LOG_DIR}/EdE${index}.log")"; eok=$?
    secs=$(( $(date +%s) - start ))
    if (( uok != 0 || eok != 0 )); then
        fail=$((fail + 1))
        echo "FAIL  ${original}  (${secs}s)  untouched: ${u}  edited: ${e}  logs: ${LOG_DIR}/EdU${index}.log EdE${index}.log"
        continue
    fi
    more=$(( $(loaded "${e}") - $(loaded "${u}") ))
    if (( more != placed )); then
        fail=$((fail + 1))
        echo "FAIL  ${original}  (${secs}s)  the edited copy loaded ${more} more objects, ${placed} were placed"
        continue
    fi
    pass=$((pass + 1))
    mode="$(sed -E 's/.* mode=([a-z_]+).*/\1/' <<< "${u}")"
    echo "PASS  ${original}  (${secs}s)  ${mode}, $(loaded "${u}") objects, edited +${placed}: ${e#TEST_MAP ok }"
done <<< "${manifest}"
echo "Results: ${pass} of ${total} maps passed, ${fail} failed (${FRAMES} frames each, untouched and edited)"
exit "${fail}"
