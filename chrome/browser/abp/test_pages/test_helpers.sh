#!/bin/bash
# ABP Test Helpers - Shared utilities for ABP integration test suites
# Source this file from test scripts: source "$(dirname "$0")/test_helpers.sh"

# Don't use set -e as we want to continue after test failures

# ============================================
# Constants
# ============================================

ABP_URL="http://localhost:8222"
HTTP_URL="http://localhost:8081"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# Test counters
PASSED=0
FAILED=0

# Debug mode (set DEBUG=1 to see failure details)
DEBUG="${DEBUG:-0}"

# Debug helper — call at end of test before return
debug_info() {
    if [[ "$DEBUG" = "1" ]]; then
        echo "" >&2
        for msg in "$@"; do
            echo -e "    ${CYAN}DEBUG: $msg${NC}" >&2
        done
    fi
}

# ============================================
# Test Runner
# ============================================

run_test() {
    local test_name="$1"
    local test_func="$2"

    echo -n "  Testing $test_name... "

    if $test_func; then
        echo -e "${GREEN}PASSED${NC}"
        PASSED=$((PASSED + 1))
    else
        echo -e "${RED}FAILED${NC}"
        FAILED=$((FAILED + 1))
    fi
}

# ============================================
# Prerequisites
# ============================================

check_prerequisites() {
    echo "Checking prerequisites..."
    if ! curl -s "$ABP_URL/api/v1/tabs" > /dev/null 2>&1; then
        echo -e "${RED}ERROR: ABP server not running at $ABP_URL${NC}"
        exit 1
    fi

    if ! curl -s "$HTTP_URL/" > /dev/null 2>&1; then
        echo -e "${RED}ERROR: HTTP server not running at $HTTP_URL${NC}"
        exit 1
    fi

    echo -e "${GREEN}Prerequisites OK${NC}"
    echo ""
}

# ============================================
# Tab Helpers
# ============================================

get_tab_id() {
    curl -s "$ABP_URL/api/v1/tabs" | jq -r '.[0].id'
}

# ============================================
# API Helpers
# ============================================

abp_get() {
    local path="$1"
    curl -s --max-time 35 "$ABP_URL$path"
}

abp_post() {
    local path="$1"
    local data="$2"
    if [[ -n "$data" ]]; then
        curl -s --max-time 35 -X POST "$ABP_URL$path" \
            -H "Content-Type: application/json" \
            -d "$data"
    else
        curl -s --max-time 35 -X POST "$ABP_URL$path"
    fi
}

abp_delete() {
    local path="$1"
    curl -s -X DELETE "$ABP_URL$path"
}

# ============================================
# Execution Control Helpers
# ============================================

enable_execution_control() {
    local tab_id="$1"
    abp_post "/api/v1/tabs/$tab_id/execution" '{"paused": true}'
}

resume_execution() {
    local tab_id="$1"
    abp_post "/api/v1/tabs/$tab_id/execution" '{"paused": false}'
}

pause_execution() {
    local tab_id="$1"
    abp_post "/api/v1/tabs/$tab_id/execution" '{"paused": true}'
}

get_execution_state() {
    local tab_id="$1"
    abp_get "/api/v1/tabs/$tab_id/execution"
}

# ============================================
# JavaScript Execution Helper
# ============================================

execute_js() {
    local tab_id="$1"
    local script="$2"
    abp_post "/api/v1/tabs/$tab_id/execute" "{\"script\":$(echo "$script" | jq -Rs .)}" | jq -r 'if .result.value == null then empty else .result.value | tostring end'
}

# ============================================
# Assertion Helpers
# ============================================

assert_eq() {
    local label="$1"
    local expected="$2"
    local actual="$3"
    if [[ "$expected" != "$actual" ]]; then
        echo -e "${RED}  ASSERT_EQ failed ($label): expected '$expected', got '$actual'${NC}" >&2
        return 1
    fi
    return 0
}

assert_neq() {
    local label="$1"
    local val1="$2"
    local val2="$3"
    if [[ "$val1" = "$val2" ]]; then
        echo -e "${RED}  ASSERT_NEQ failed ($label): both values are '$val1'${NC}" >&2
        return 1
    fi
    return 0
}

assert_gt() {
    local label="$1"
    local val="$2"
    local threshold="$3"
    if [[ "$val" -le "$threshold" ]] 2>/dev/null; then
        echo -e "${RED}  ASSERT_GT failed ($label): $val not > $threshold${NC}" >&2
        return 1
    fi
    return 0
}

assert_lt() {
    local label="$1"
    local val="$2"
    local threshold="$3"
    if [[ "$val" -ge "$threshold" ]] 2>/dev/null; then
        echo -e "${RED}  ASSERT_LT failed ($label): $val not < $threshold${NC}" >&2
        return 1
    fi
    return 0
}

assert_nonempty() {
    local label="$1"
    local val="$2"
    if [[ -z "$val" ]]; then
        echo -e "${RED}  ASSERT_NONEMPTY failed ($label): value is empty${NC}" >&2
        return 1
    fi
    return 0
}

# ============================================
# Summary
# ============================================

print_summary() {
    local suite_name="${1:-Test}"
    echo ""
    echo "================================"
    echo "$suite_name Results"
    echo "================================"
    echo -e "Passed: ${GREEN}$PASSED${NC}"
    echo -e "Failed: ${RED}$FAILED${NC}"
    echo ""

    if [ $FAILED -eq 0 ]; then
        echo -e "${GREEN}All tests passed!${NC}"
        exit 0
    else
        echo -e "${RED}Some tests failed.${NC}"
        exit 1
    fi
}
