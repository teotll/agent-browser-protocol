#!/bin/bash
# Virtual Cursor Grid Test
# Tests virtual cursor positioning, scaling, and ratio on a CSS grid with 100px offset

set -e

ABP_URL="http://localhost:8222"
HTTP_URL="http://localhost:8081"
OUTPUT_DIR="${1:-/tmp/cursor-grid-test-$(date +%Y%m%d_%H%M%S)}"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

PASSED=0
FAILED=0

echo "================================"
echo "Virtual Cursor Grid Test Suite"
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
    -d '{"url":"'"$HTTP_URL"'/virtual-cursor-grid-test.html"}' > /dev/null

sleep 2
echo -e "${GREEN}Navigated to grid test page${NC}"

# Test function: Move cursor and take screenshot
# Args: test_name, x, y, expected_row, expected_col
test_cursor_position() {
    local test_name="$1"
    local x="$2"
    local y="$3"
    local expected_row="$4"
    local expected_col="$5"

    echo -n "  $test_name... "

    # Move cursor to position
    local move_result=$(curl -s -X POST "$ABP_URL/api/v1/tabs/$TAB_ID/move" \
        -H "Content-Type: application/json" \
        -d '{"x":'"$x"',"y":'"$y"'}')

    local move_status=$(echo "$move_result" | jq -r '.result.status // empty')
    if [[ "$move_status" != "moved" ]]; then
        echo -e "${RED}FAILED (move failed)${NC}"
        echo "  Move result: $move_result"
        FAILED=$((FAILED + 1))
        return 1
    fi

    sleep 0.3

    # Take screenshot with cursor visible
    local screenshot_file="$OUTPUT_DIR/${test_name// /_}.webp"
    local screenshot_result=$(curl -s -X POST "$ABP_URL/api/v1/tabs/$TAB_ID/screenshot" \
        -H "Content-Type: application/json" \
        -d '{"screenshot":{"format":"webp"}}')

    local screenshot_data=$(echo "$screenshot_result" | jq -r '.data // empty')
    if [[ -z "$screenshot_data" ]]; then
        echo -e "${RED}FAILED (screenshot failed)${NC}"
        FAILED=$((FAILED + 1))
        return 1
    fi

    # Save screenshot
    echo "$screenshot_data" | base64 -d > "$screenshot_file"

    # Verify cursor is in correct cell via JavaScript
    local cell_check=$(curl -s -X POST "$ABP_URL/api/v1/tabs/$TAB_ID/execute" \
        -H "Content-Type: application/json" \
        -d '{"script":"JSON.stringify(window.gridTest.getCellAtPoint('"$x"', '"$y"'))"}' | jq -r '.result.value // "null"')

    if [[ "$cell_check" == "null" ]]; then
        echo -e "${RED}FAILED (cursor outside grid)${NC}"
        FAILED=$((FAILED + 1))
        return 1
    fi

    local actual_row=$(echo "$cell_check" | jq -r '.row')
    local actual_col=$(echo "$cell_check" | jq -r '.col')

    if [[ "$actual_row" == "$expected_row" && "$actual_col" == "$expected_col" ]]; then
        echo -e "${GREEN}PASSED${NC} -> $screenshot_file"
        PASSED=$((PASSED + 1))
        return 0
    else
        echo -e "${RED}FAILED (expected cell ($expected_row,$expected_col), got ($actual_row,$actual_col))${NC}"
        FAILED=$((FAILED + 1))
        return 1
    fi
}

# Test function: Verify cursor moved and returned position
test_cursor_move_response() {
    local test_name="$1"
    local x="$2"
    local y="$3"

    echo -n "  $test_name... "

    local move_result=$(curl -s -X POST "$ABP_URL/api/v1/tabs/$TAB_ID/move" \
        -H "Content-Type: application/json" \
        -d '{"x":'"$x"',"y":'"$y"'}')

    local status=$(echo "$move_result" | jq -r '.result.status // empty')
    local returned_x=$(echo "$move_result" | jq -r '.result.x // empty')
    local returned_y=$(echo "$move_result" | jq -r '.result.y // empty')

    # Handle float comparison - compare as strings after normalizing
    # Remove trailing .0 from both if present
    local expect_x=$(echo "$x" | sed 's/\.0$//')
    local expect_y=$(echo "$y" | sed 's/\.0$//')
    local got_x=$(echo "$returned_x" | sed 's/\.0$//')
    local got_y=$(echo "$returned_y" | sed 's/\.0$//')

    if [[ "$status" == "moved" && "$got_x" == "$expect_x" && "$got_y" == "$expect_y" ]]; then
        echo -e "${GREEN}PASSED${NC}"
        PASSED=$((PASSED + 1))
        return 0
    else
        echo -e "${RED}FAILED${NC}"
        echo "    Expected: status=moved, x=$x, y=$y"
        echo "    Got: status=$status, x=$returned_x, y=$returned_y"
        FAILED=$((FAILED + 1))
        return 1
    fi
}

# Run tests
echo ""
echo -e "${CYAN}--- Move Response Tests ---${NC}"
echo "Testing cursor move endpoint returns correct positions..."

test_cursor_move_response "Move to origin" 0 0
test_cursor_move_response "Move to cell (0,0) center" 150 150
test_cursor_move_response "Move to cell (2,2) center" 350 350
test_cursor_move_response "Move to cell (4,4) center" 550 550
test_cursor_move_response "Move to fractional coords" 150.5 150.5

echo ""
echo -e "${CYAN}--- Grid Cell Tests ---${NC}"
echo "Testing cursor position maps to correct grid cells..."

# Grid offset is 100px, cell size is 100px
# Cell (0,0) center: (150, 150)
# Cell (2,2) center: (350, 350)
# Cell (4,4) center: (550, 550)
# Cell (0,4) center: (550, 150) - top-right
# Cell (4,0) center: (150, 550) - bottom-left

test_cursor_position "Cell (0,0) top-left" 150 150 0 0
test_cursor_position "Cell (2,2) center" 350 350 2 2
test_cursor_position "Cell (4,4) bottom-right" 550 550 4 4
test_cursor_position "Cell (0,4) top-right" 550 150 0 4
test_cursor_position "Cell (4,0) bottom-left" 150 550 4 0

echo ""
echo -e "${CYAN}--- Edge Case Tests ---${NC}"
echo "Testing cursor at cell boundaries..."

# Just inside first cell
test_cursor_position "Just inside cell (0,0)" 101 101 0 0

# Last pixel of grid
test_cursor_position "Last pixel of cell (4,4)" 599 599 4 4

# Cell boundary - right edge of (1,1) should be (1,1), not (1,2)
test_cursor_position "Cell (1,1) near right edge" 299 250 1 1

# Cell boundary - bottom edge
test_cursor_position "Cell (1,1) near bottom edge" 250 299 1 1

echo ""
echo -e "${CYAN}--- Offset Verification Tests ---${NC}"
echo "Testing that 100px offset is correctly applied..."

# Position at exactly (100, 100) should be cell (0,0)
test_cursor_position "Grid origin (100,100)" 100 100 0 0

# Position at (199, 199) should still be cell (0,0)
test_cursor_position "Cell (0,0) far corner" 199 199 0 0

# Position at (200, 200) should be cell (1,1)
test_cursor_position "Cell (1,1) origin" 200 200 1 1

echo ""
echo -e "${CYAN}--- Screenshot with Cursor Tests ---${NC}"
echo "Taking screenshots at each test target..."

# Move to each test target and capture screenshot
for target in "0 0 150 150" "2 2 350 350" "4 4 550 550" "0 4 550 150" "4 0 150 550"; do
    read -r row col x y <<< "$target"
    test_name="Screenshot target ($row,$col)"
    echo -n "  $test_name... "

    # Move cursor
    curl -s -X POST "$ABP_URL/api/v1/tabs/$TAB_ID/move" \
        -H "Content-Type: application/json" \
        -d '{"x":'"$x"',"y":'"$y"'}' > /dev/null

    sleep 0.3

    # Take screenshot
    screenshot_file="$OUTPUT_DIR/target_${row}_${col}.webp"
    screenshot_result=$(curl -s -X POST "$ABP_URL/api/v1/tabs/$TAB_ID/screenshot" \
        -H "Content-Type: application/json" \
        -d '{"screenshot":{"format":"webp"}}')

    screenshot_data=$(echo "$screenshot_result" | jq -r '.data // empty')
    if [[ -n "$screenshot_data" ]]; then
        echo "$screenshot_data" | base64 -d > "$screenshot_file"
        echo -e "${GREEN}SAVED${NC} -> $screenshot_file"
        PASSED=$((PASSED + 1))
    else
        echo -e "${RED}FAILED${NC}"
        FAILED=$((FAILED + 1))
    fi
done

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
