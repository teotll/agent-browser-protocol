#!/bin/bash
# Cursor Style Test
# Tests that virtual cursor renders correctly with different cursor styles
# Uses a 4x4 grid with predictable spacing

set -e

ABP_URL="http://localhost:8222"
HTTP_URL="http://localhost:8081"
OUTPUT_DIR="${1:-/tmp/cursor-style-test-$(date +%Y%m%d_%H%M%S)}"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

PASSED=0
FAILED=0

# Grid configuration (must match HTML)
GRID_OFFSET_X=100
GRID_OFFSET_Y=100
CELL_WIDTH=150
CELL_HEIGHT=100

# Calculate cell center
get_cell_center_x() {
    local col=$1
    echo $((GRID_OFFSET_X + col * CELL_WIDTH + CELL_WIDTH / 2))
}

get_cell_center_y() {
    local row=$1
    echo $((GRID_OFFSET_Y + row * CELL_HEIGHT + CELL_HEIGHT / 2))
}

echo "================================"
echo "Cursor Style Test Suite"
echo "================================"
echo ""
echo "Output directory: $OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

# Check prerequisites
echo ""
echo "Checking prerequisites..."
if ! curl -s "$ABP_URL/api/v1/tabs" > /dev/null 2>&1; then
    echo -e "${RED}ERROR: ABP server not running at $ABP_URL${NC}"
    exit 1
fi

if ! curl -s "$HTTP_URL/" > /dev/null 2>&1; then
    echo -e "${RED}ERROR: HTTP server not running at $HTTP_URL${NC}"
    echo -e "${YELLOW}Start with: cd test_pages && python3 -m http.server 8081${NC}"
    exit 1
fi

echo -e "${GREEN}Prerequisites OK${NC}"

# Get tab ID
get_tab_id() {
    curl -s "$ABP_URL/api/v1/tabs" | jq -r '.[0].id'
}

# Navigate to test page
echo ""
echo "Setting up test page..."
TAB_ID=$(get_tab_id)
curl -s -X POST "$ABP_URL/api/v1/tabs/$TAB_ID/navigate" \
    -H "Content-Type: application/json" \
    -d '{"url":"'"$HTTP_URL"'/cursor-style-test.html"}' > /dev/null

sleep 2
echo -e "${GREEN}Navigated to cursor style test page${NC}"

# Test function: Move cursor to cell center and verify cursor type
# Args: row, col, expected_cursor
test_cursor_at_cell() {
    local row="$1"
    local col="$2"
    local expected_cursor="$3"
    local x=$(get_cell_center_x $col)
    local y=$(get_cell_center_y $row)
    local test_name="[$row,$col] $expected_cursor"

    echo -n "  $test_name at ($x,$y)... "

    # Move cursor to position
    local move_result=$(curl -s -X POST "$ABP_URL/api/v1/tabs/$TAB_ID/move" \
        -H "Content-Type: application/json" \
        -d '{"x":'"$x"',"y":'"$y"'}')

    local move_status=$(echo "$move_result" | jq -r '.result.status // empty')
    if [[ "$move_status" != "moved" ]]; then
        echo -e "${RED}FAILED (move failed)${NC}"
        FAILED=$((FAILED + 1))
        return 1
    fi

    sleep 0.2

    # Get the computed cursor at this position
    local cursor_check=$(curl -s -X POST "$ABP_URL/api/v1/tabs/$TAB_ID/execute" \
        -H "Content-Type: application/json" \
        -d '{"script":"window.cursorStyleTest.getCursorAt('"$x"', '"$y"')"}' | jq -r '.result.value // "null"')

    # Take screenshot
    local screenshot_file="$OUTPUT_DIR/cursor_${row}_${col}_${expected_cursor}.webp"
    local screenshot_result=$(curl -s -X POST "$ABP_URL/api/v1/tabs/$TAB_ID/screenshot" \
        -H "Content-Type: application/json" \
        -d '{"screenshot":{"format":"webp"}}')

    local screenshot_data=$(echo "$screenshot_result" | jq -r '.data // empty')
    if [[ -n "$screenshot_data" ]]; then
        echo "$screenshot_data" | base64 -d > "$screenshot_file"
    fi

    # Check if cursor matches expected
    if [[ "$cursor_check" == "$expected_cursor" ]]; then
        echo -e "${GREEN}PASSED${NC} -> $screenshot_file"
        PASSED=$((PASSED + 1))
        return 0
    else
        echo -e "${RED}FAILED${NC} (expected: $expected_cursor, got: $cursor_check)"
        FAILED=$((FAILED + 1))
        return 1
    fi
}

# Run tests
echo ""
echo -e "${CYAN}--- Row 0: Basic Cursors ---${NC}"
test_cursor_at_cell 0 0 "default"
test_cursor_at_cell 0 1 "pointer"
test_cursor_at_cell 0 2 "text"
test_cursor_at_cell 0 3 "crosshair"

echo ""
echo -e "${CYAN}--- Row 1: Interaction Cursors ---${NC}"
test_cursor_at_cell 1 0 "move"
test_cursor_at_cell 1 1 "not-allowed"
test_cursor_at_cell 1 2 "grab"
test_cursor_at_cell 1 3 "grabbing"

echo ""
echo -e "${CYAN}--- Row 2: Status Cursors ---${NC}"
test_cursor_at_cell 2 0 "wait"
test_cursor_at_cell 2 1 "progress"
test_cursor_at_cell 2 2 "help"
test_cursor_at_cell 2 3 "cell"

echo ""
echo -e "${CYAN}--- Row 3: Resize/Zoom Cursors ---${NC}"
test_cursor_at_cell 3 0 "col-resize"
test_cursor_at_cell 3 1 "row-resize"
test_cursor_at_cell 3 2 "zoom-in"
test_cursor_at_cell 3 3 "zoom-out"

# Summary
echo ""
echo "================================"
echo "Test Results"
echo "================================"
echo -e "Passed: ${GREEN}$PASSED${NC}"
echo -e "Failed: ${RED}$FAILED${NC}"
echo ""
echo "Screenshots saved to: $OUTPUT_DIR"
echo ""

if [ $FAILED -eq 0 ]; then
    echo -e "${GREEN}All tests passed!${NC}"
    exit 0
else
    echo -e "${RED}Some tests failed.${NC}"
    exit 1
fi
