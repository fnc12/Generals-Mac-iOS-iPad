#!/usr/bin/env bash
# Play every test replay headless and report PASS/FAIL (macOS), like the CI
# replay-tests workflow but on your own machine with your own game data.
#
#   ./scripts/qa/replays/run-replay-tests-macos.sh [GeneralsXZH.app] [replay dir]
#
# Defaults: ~/GeneralsX/Apps/GeneralsXZH.app; the replays are macos_*.rep from
# GeneralsReplays/GeneralsXZH/Replays and tests/replays/macos (replays are
# deterministic per platform, so only macOS recordings). Replays in the old
# 4-byte wide char format are converted on the fly. With a replay dir given,
# only that dir is used. Maps come from GeneralsReplays/GeneralsXZH/Maps.
# Logs: logs/replay-tests/.
#
# The run uses a throwaway profile (HOME is redirected to a temp folder), so your
# own settings, maps and replays are not touched. Game data is taken from
# ~/GeneralsX/GeneralsZH and ~/GeneralsX/Generals (or CNC_GENERALS_ZH_PATH /
# CNC_GENERALS_PATH if set). Exit code: number of failed replays (0 = all passed).
set -uo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
APP="${1:-${HOME}/GeneralsX/Apps/GeneralsXZH.app}"
if [[ $# -ge 2 ]]; then
    REPLAY_DIRS=("$2")
else
    REPLAY_DIRS=("${PROJECT_ROOT}/GeneralsReplays/GeneralsXZH/Replays" "${PROJECT_ROOT}/tests/replays/macos")
fi
CONVERTER="${PROJECT_ROOT}/scripts/replays/convert_wchar32_replay.py"
LOG_DIR="${PROJECT_ROOT}/logs/replay-tests"
MAPS_DIR="${PROJECT_ROOT}/GeneralsReplays/GeneralsXZH/Maps"
TIMEOUT_SECONDS="${REPLAY_TIMEOUT:-600}"

ZH_DATA="$(cd "${CNC_GENERALS_ZH_PATH:-${HOME}/GeneralsX/GeneralsZH}" && pwd -P)" || { echo "Zero Hour data not found" >&2; exit 100; }
GEN_DATA="$(cd "${CNC_GENERALS_PATH:-${HOME}/GeneralsX/Generals}" && pwd -P)" || { echo "Generals data not found" >&2; exit 100; }
[[ -x "${APP}/Contents/MacOS/run.sh" ]] || { echo "not a GeneralsXZH app: ${APP}" >&2; exit 100; }

PROFILE="$(mktemp -d)"
trap 'rm -rf "${PROFILE}"' EXIT
USER_DATA="${PROFILE}/Library/Application Support/GeneralsX/GeneralsZH"
mkdir -p "${USER_DATA}/Maps" "${USER_DATA}/Replays" "${LOG_DIR}"
[[ -d "${MAPS_DIR}" ]] && cp -R "${MAPS_DIR}/." "${USER_DATA}/Maps/"

shopt -s nullglob
replays=()
for dir in "${REPLAY_DIRS[@]}"; do
    replays+=("${dir}"/macos_*.rep)
done
if (( ${#replays[@]} == 0 )); then
    echo "no macos_*.rep in ${REPLAY_DIRS[*]}" >&2
    exit 100
fi

pass=0 fail=0
for rep in "${replays[@]}"; do
    name="$(basename "${rep}")"
    # Old-format replays are converted; the converter refuses a replay that is already new.
    python3 -I "${CONVERTER}" "${rep}" "${USER_DATA}/Replays/${name}" > /dev/null 2>&1 \
        || cp "${rep}" "${USER_DATA}/Replays/${name}"
    log="${LOG_DIR}/${name%.rep}.log"
    start=$(date +%s)
    # macOS has no `timeout`; perl's alarm kills the run after TIMEOUT_SECONDS.
    ( cd "${APP}/Contents/MacOS" && \
      CNC_GENERALS_ZH_PATH="${ZH_DATA}" CNC_GENERALS_PATH="${GEN_DATA}" HOME="${PROFILE}" \
      perl -e "alarm ${TIMEOUT_SECONDS}; exec @ARGV" ./run.sh -headless -replay "${USER_DATA}/Replays/${name}" ) > "${log}" 2>&1
    code=$?
    secs=$(( $(date +%s) - start ))
    if (( code == 0 )); then
        pass=$((pass + 1))
        echo "PASS  ${name}  (${secs}s)"
    else
        fail=$((fail + 1))
        detail="$(grep -a -E "CRC Mismatch|Cannot open replay|Game Time" "${log}" | sort -u | tail -2 | tr '\n' ' ')"
        if [[ -z "${detail}" ]] && ! grep -a -q "Game Time" "${log}"; then
            detail="did not start simulating (bad header?)"
        fi
        echo "FAIL  ${name}  exit ${code} (${secs}s)  ${detail}  log: ${log}"
    fi
done
echo "Results: ${pass} passed, ${fail} failed"
exit "${fail}"
