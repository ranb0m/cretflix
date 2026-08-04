#!/bin/bash
# tests/run.sh
# ----------------------------------------------------------------------------
# Discover and run tests. Pass a category to run only that category.
#
# Usage:
#   tests/run.sh              # run all
#   tests/run.sh smoke        # run only smoke tests
#   tests/run.sh smoke proto  # multiple categories
#
# Logs go to logs/<timestamp>/. Each test gets its own server log, client
# log, and assertion log. The runner prints a summary at the end.
# ----------------------------------------------------------------------------

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Pick a single timestamped log directory for this whole run; all tests
# share it so `lnav logs/<ts>/` shows a unified timeline.
export LOG_DIR="$REPO_ROOT/logs/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$LOG_DIR"

if [ -t 1 ]; then
    GREEN='\033[0;32m'; RED='\033[0;31m'; CYAN='\033[0;36m'; BOLD='\033[1m'; NC='\033[0m'
else
    GREEN=''; RED=''; CYAN=''; BOLD=''; NC=''
fi

# Sanity: server must be built
if [ ! -x "$REPO_ROOT/server" ]; then
    echo "Server binary not found at $REPO_ROOT/server. Run 'make' first." >&2
    exit 1
fi

# Categories to run (default = all)
if [ "$#" -eq 0 ]; then
    categories=(smoke protocol inotify concurrency)
else
    categories=("$@")
fi

# Collect tests
tests=()
for cat in "${categories[@]}"; do
    if [ ! -d "$SCRIPT_DIR/$cat" ]; then
        echo "Warning: no such category: $cat" >&2
        continue
    fi
    for t in "$SCRIPT_DIR/$cat"/*.sh; do
        [ -f "$t" ] && tests+=("$t")
    done
done

total=${#tests[@]}
if [ "$total" -eq 0 ]; then
    echo "No tests found." >&2
    exit 1
fi

passed=0
failed=0
failures=()

echo -e "${CYAN}${BOLD}== Running $total tests ==${NC}"
echo -e "Logs: $LOG_DIR"
echo ""

for t in "${tests[@]}"; do
    name="$(basename "$t" .sh)"
    cat="$(basename "$(dirname "$t")")"
    printf "  [%s] %-40s " "$cat" "$name"

    if bash "$t" > "$LOG_DIR/${cat}_${name}.runner.log" 2>&1; then
        echo -e "${GREEN}OK${NC}"
        passed=$((passed + 1))
    else
        echo -e "${RED}FAIL${NC}"
        failed=$((failed + 1))
        failures+=("${cat}/${name}")
    fi
done

echo ""
echo -e "${BOLD}== Results ==${NC}"
echo "  Passed: $passed/$total"
if [ "$failed" -gt 0 ]; then
    echo -e "  ${RED}Failed: $failed${NC}"
    for f in "${failures[@]}"; do
        echo -e "    - ${RED}$f${NC}"
        echo "      see: $LOG_DIR/${f//\//_}.runner.log"
    done
    exit 1
fi
echo -e "  ${GREEN}All passed.${NC}"
