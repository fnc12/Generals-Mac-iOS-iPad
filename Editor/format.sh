#!/bin/sh
# Formats the editor's C and C++ sources with Editor/.clang-format (sqlite_orm's style).
#   Editor/format.sh          format the files in place
#   Editor/format.sh --check  change nothing; exit with an error when a file is not formatted (CI)
# CLANG_FORMAT picks the binary; by default Homebrew's LLVM one, else clang-format from PATH.
# Different clang-format versions format a few things differently: use one version everywhere.
set -eu
cd "$(dirname "$0")"

if [ -z "${CLANG_FORMAT:-}" ]; then
    if [ -x /opt/homebrew/opt/llvm/bin/clang-format ]; then
        CLANG_FORMAT=/opt/homebrew/opt/llvm/bin/clang-format
    else
        CLANG_FORMAT=clang-format
    fi
fi

case "${1:-}" in
    "") mode="-i" ;;
    --check) mode="--dry-run --Werror" ;;
    *) echo "usage: $0 [--check]" >&2; exit 2 ;;
esac

# shellcheck disable=SC2086
find core/src core/include core/cli core/tests macos \
    \( -name '*.c' -o -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) -print0 |
    xargs -0 "$CLANG_FORMAT" $mode
