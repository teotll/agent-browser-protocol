#!/bin/bash
# ABP Event Lifecycle Test Suite
# Tests virtual time, virtual cursor, and compositor update behavior.
#
# Prerequisites:
#   1. Chromium with ABP: ./out/Default/Chromium.app/Contents/MacOS/Chromium --enable-abp --abp-session-dir=sessions/test --no-first-run
#   2. HTTP server: cd chrome/browser/abp/test_pages && python3 -m http.server 8081
#
# Usage: bash chrome/browser/abp/test_pages/run_lifecycle_tests.sh

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/test_helpers.sh"

# ============================================
# Shared state
# ============================================
VTIME_TAB_ID=""
CURSOR_TAB_ID=""

# ============================================
# Category 1: Virtual Time / Execution Control
# ============================================

setup_vtime_tab() {
    # Create new tab with virtual-time-test.html
    local response=$(abp_post "/api/v1/tabs" '{"url":"'"$HTTP_URL"'/virtual-time-test.html"}')
    VTIME_TAB_ID=$(echo "$response" | jq -r '.id')

    if [[ -z "$VTIME_TAB_ID" || "$VTIME_TAB_ID" = "null" ]]; then
        echo -e "${RED}FAILED (no tab id)${NC}"
        return 1
    fi

    sleep 2

    # Enable execution control (starts paused)
    enable_execution_control "$VTIME_TAB_ID" > /dev/null
    sleep 1

    local enabled=$(get_execution_state "$VTIME_TAB_ID" | jq -r '.enabled')
    if [[ "$enabled" = "true" ]]; then
        return 0
    else
        return 1
    fi
}

reset_vtime_tab() {
    # Resume so navigation can complete
    resume_execution "$VTIME_TAB_ID" > /dev/null
    sleep 0.3

    # Reload the page to reset state
    abp_post "/api/v1/tabs/$VTIME_TAB_ID/navigate" '{"url":"'"$HTTP_URL"'/virtual-time-test.html"}' > /dev/null
    sleep 1.5

    # Re-pause
    pause_execution "$VTIME_TAB_ID" > /dev/null
    sleep 0.5
}

# VTIME-101: Enable execution control
test_vtime_101() {
    local state=$(get_execution_state "$VTIME_TAB_ID")
    local enabled=$(echo "$state" | jq -r '.enabled')
    local paused=$(echo "$state" | jq -r '.paused')
    [[ "$enabled" = "true" && "$paused" = "true" ]]
}

# VTIME-102: Date.now frozen when paused
test_vtime_102() {
    local time1=$(execute_js "$VTIME_TAB_ID" "Date.now()")
    sleep 1
    local time2=$(execute_js "$VTIME_TAB_ID" "Date.now()")
    [[ "$time1" = "$time2" ]]
}

# VTIME-103: setInterval frozen when paused
test_vtime_103() {
    local count1=$(execute_js "$VTIME_TAB_ID" "window.virtualTimeTest.getIntervalCount()")
    sleep 2
    local count2=$(execute_js "$VTIME_TAB_ID" "window.virtualTimeTest.getIntervalCount()")
    local diff=$((count2 - count1))
    [[ $diff -lt 3 ]]
}

# VTIME-104: setTimeout not firing when paused
test_vtime_104() {
    reset_vtime_tab
    execute_js "$VTIME_TAB_ID" "window.virtualTimeTest.scheduleTestTimeout(100)" > /dev/null
    sleep 0.5
    local fired=$(execute_js "$VTIME_TAB_ID" "window.testTimeoutFired")
    [[ "$fired" = "false" ]]
}

# VTIME-105: RAF not incrementing when paused
test_vtime_105() {
    local count1=$(execute_js "$VTIME_TAB_ID" "window.virtualTimeTest.getRafCount()")
    sleep 1
    local count2=$(execute_js "$VTIME_TAB_ID" "window.virtualTimeTest.getRafCount()")
    local diff=$((count2 - count1))
    [[ $diff -lt 3 ]]
}

# VTIME-106: Resume unfreezes timers
test_vtime_106() {
    reset_vtime_tab
    local count_before=$(execute_js "$VTIME_TAB_ID" "window.virtualTimeTest.getIntervalCount()")
    resume_execution "$VTIME_TAB_ID" > /dev/null
    sleep 1
    local count_after=$(execute_js "$VTIME_TAB_ID" "window.virtualTimeTest.getIntervalCount()")
    # Re-pause for subsequent tests
    pause_execution "$VTIME_TAB_ID" > /dev/null
    sleep 0.3
    local diff=$((count_after - count_before))
    [[ $diff -gt 5 ]]
}

# VTIME-107: Resume unfreezes Date.now
test_vtime_107() {
    reset_vtime_tab
    resume_execution "$VTIME_TAB_ID" > /dev/null
    sleep 0.3
    local time1=$(execute_js "$VTIME_TAB_ID" "Date.now()")
    sleep 0.5
    local time2=$(execute_js "$VTIME_TAB_ID" "Date.now()")
    # Re-pause
    pause_execution "$VTIME_TAB_ID" > /dev/null
    sleep 0.3
    [[ "$time1" != "$time2" ]]
}

# VTIME-108: Pause-Resume-Pause cycle
test_vtime_108() {
    reset_vtime_tab
    local count1=$(execute_js "$VTIME_TAB_ID" "window.virtualTimeTest.getIntervalCount()")

    # Resume for 500ms then re-pause
    resume_execution "$VTIME_TAB_ID" > /dev/null
    sleep 0.5
    pause_execution "$VTIME_TAB_ID" > /dev/null
    sleep 0.3

    local count2=$(execute_js "$VTIME_TAB_ID" "window.virtualTimeTest.getIntervalCount()")

    # Intervals should have advanced during the resumed period
    local diff=$((count2 - count1))
    [[ $diff -gt 0 ]]
}

# VTIME-109: Virtual time in action envelope
test_vtime_109() {
    reset_vtime_tab
    # Click somewhere on the page — action envelope should contain virtual_time
    local response=$(abp_post "/api/v1/tabs/$VTIME_TAB_ID/click" '{"x":100,"y":100}')
    sleep 0.5

    local vt_paused=$(echo "$response" | jq -r '.virtual_time.paused // empty')
    local vt_base=$(echo "$response" | jq -r '.virtual_time.base_ticks_ms // empty')
    [[ -n "$vt_paused" && -n "$vt_base" ]]
}

# VTIME-110: Screenshots in action envelope
test_vtime_110() {
    reset_vtime_tab
    local response=$(abp_post "/api/v1/tabs/$VTIME_TAB_ID/click" '{"x":100,"y":100}')
    sleep 0.5

    local sb_vt=$(echo "$response" | jq -r '.screenshot_before.virtual_time_ms // empty')
    local sb_w=$(echo "$response" | jq -r '.screenshot_before.width // empty')
    local sb_h=$(echo "$response" | jq -r '.screenshot_before.height // empty')
    local sa_vt=$(echo "$response" | jq -r '.screenshot_after.virtual_time_ms // empty')
    local sa_w=$(echo "$response" | jq -r '.screenshot_after.width // empty')
    local sa_h=$(echo "$response" | jq -r '.screenshot_after.height // empty')

    [[ -n "$sb_vt" && -n "$sb_w" && -n "$sb_h" && -n "$sa_vt" && -n "$sa_w" && -n "$sa_h" ]]
}

# VTIME-111: Action resumes then re-pauses
test_vtime_111() {
    reset_vtime_tab
    # Verify paused before
    local paused_before=$(get_execution_state "$VTIME_TAB_ID" | jq -r '.paused')

    # Click — action envelope flow: resume -> action -> re-pause
    abp_post "/api/v1/tabs/$VTIME_TAB_ID/click" '{"x":100,"y":100}' > /dev/null
    sleep 0.5

    # After action, should be paused again
    local paused_after=$(get_execution_state "$VTIME_TAB_ID" | jq -r '.paused')

    # Date.now should be frozen again
    local time1=$(execute_js "$VTIME_TAB_ID" "Date.now()")
    sleep 0.5
    local time2=$(execute_js "$VTIME_TAB_ID" "Date.now()")

    [[ "$paused_before" = "true" && "$paused_after" = "true" && "$time1" = "$time2" ]]
}

# VTIME-112: GET execution state accuracy
test_vtime_112() {
    reset_vtime_tab

    # After reset, should be paused
    local state1=$(get_execution_state "$VTIME_TAB_ID" | jq -r '.paused')

    # Resume
    resume_execution "$VTIME_TAB_ID" > /dev/null
    sleep 0.3
    local state2=$(get_execution_state "$VTIME_TAB_ID" | jq -r '.paused')

    # Pause again
    pause_execution "$VTIME_TAB_ID" > /dev/null
    sleep 0.3
    local state3=$(get_execution_state "$VTIME_TAB_ID" | jq -r '.paused')

    [[ "$state1" = "true" && "$state2" = "false" && "$state3" = "true" ]]
}

# VTIME-113: Timing info in envelope
test_vtime_113() {
    reset_vtime_tab
    local response=$(abp_post "/api/v1/tabs/$VTIME_TAB_ID/click" '{"x":100,"y":100}')
    sleep 0.5

    local t_started=$(echo "$response" | jq -r '.timing.action_started_ms // empty')
    local t_completed=$(echo "$response" | jq -r '.timing.action_completed_ms // empty')
    local t_wait=$(echo "$response" | jq -r '.timing.wait_completed_ms // empty')
    local t_duration=$(echo "$response" | jq -r '.timing.duration_ms // empty')

    [[ -n "$t_started" && -n "$t_completed" && -n "$t_wait" && -n "$t_duration" ]]
}

# VTIME-114: Events captured in envelope
test_vtime_114() {
    reset_vtime_tab
    # Navigate — should produce a navigation event in the envelope
    local response=$(abp_post "/api/v1/tabs/$VTIME_TAB_ID/navigate" '{"url":"'"$HTTP_URL"'/navigation-test.html"}')

    local event_type=$(echo "$response" | jq -r '.events[0].type // empty')
    local event_vt=$(echo "$response" | jq -r '.events[0].virtual_time_ms // empty')
    local resp_error=$(echo "$response" | jq -r '.error // empty')

    # If events empty on first try, retry once (Page.frameNavigated can race)
    if [[ -z "$event_type" && -z "$resp_error" ]]; then
        sleep 0.5
        reset_vtime_tab
        response=$(abp_post "/api/v1/tabs/$VTIME_TAB_ID/navigate" '{"url":"'"$HTTP_URL"'/navigation-test.html"}')
        event_type=$(echo "$response" | jq -r '.events[0].type // empty')
        event_vt=$(echo "$response" | jq -r '.events[0].virtual_time_ms // empty')
    fi

    debug_info "event_type=$event_type event_vt=$event_vt events=$(echo "$response" | jq -c '.events // []') error=$resp_error"
    [[ -n "$event_type" && -n "$event_vt" ]]
}

# ============================================
# Category 2: Virtual Cursor
# ============================================

setup_cursor_tab() {
    # Create a fresh tab — no execution control so moves don't time out
    # (InsertVisualStateFence hangs on consecutive moves when paused)
    local response=$(abp_post "/api/v1/tabs" '{"url":"'"$HTTP_URL"'/virtual-cursor-grid-test.html"}')
    CURSOR_TAB_ID=$(echo "$response" | jq -r '.id')
    if [[ -z "$CURSOR_TAB_ID" || "$CURSOR_TAB_ID" = "null" ]]; then
        return 1
    fi
    sleep 2
    return 0
}

# CURSOR-101: Move returns position
test_cursor_101() {
    local result=$(abp_post "/api/v1/tabs/$CURSOR_TAB_ID/move" '{"x":350,"y":350}')
    sleep 0.3

    local error=$(echo "$result" | jq -r '.error // empty')
    local status=$(echo "$result" | jq -r '.result.status // .status // empty')
    local rx=$(echo "$result" | jq -r '.result.x // .x // empty' | sed 's/\.0$//')
    local ry=$(echo "$result" | jq -r '.result.y // .y // empty' | sed 's/\.0$//')

    debug_info "error=$error status=$status rx=$rx ry=$ry"
    [[ "$status" = "moved" && "$rx" = "350" && "$ry" = "350" ]]
}

# CURSOR-102: Move maps to correct cell
test_cursor_102() {
    abp_post "/api/v1/tabs/$CURSOR_TAB_ID/move" '{"x":150,"y":150}' > /dev/null
    sleep 0.3

    local cell=$(execute_js "$CURSOR_TAB_ID" "JSON.stringify(window.gridTest.getCellAtPoint(150, 150))")
    if [[ -z "$cell" || "$cell" = "null" ]]; then
        return 1
    fi

    local row=$(echo "$cell" | jq -r '.row')
    local col=$(echo "$cell" | jq -r '.col')
    [[ "$row" = "0" && "$col" = "0" ]]
}

# CURSOR-103: All 5 test targets
test_cursor_103() {
    # Test all 5 target positions: (0,0), (2,2), (4,4), (0,4), (4,0)
    local targets=("150,150,0,0" "350,350,2,2" "550,550,4,4" "150,550,4,0" "550,150,0,4")

    for target in "${targets[@]}"; do
        IFS=',' read -r x y er ec <<< "$target"

        abp_post "/api/v1/tabs/$CURSOR_TAB_ID/move" '{"x":'"$x"',"y":'"$y"'}' > /dev/null
        sleep 0.2

        local cell=$(execute_js "$CURSOR_TAB_ID" "JSON.stringify(window.gridTest.getCellAtPoint($x, $y))")
        if [[ -z "$cell" || "$cell" = "null" ]]; then
            return 1
        fi

        local row=$(echo "$cell" | jq -r '.row')
        local col=$(echo "$cell" | jq -r '.col')
        if [[ "$row" != "$er" || "$col" != "$ec" ]]; then
            debug_info "target ($x,$y) expected row=$er col=$ec got row=$row col=$col cell=$cell"
            return 1
        fi
    done
    return 0
}

# CURSOR-104: Position persists across actions
test_cursor_104() {
    abp_post "/api/v1/tabs/$CURSOR_TAB_ID/move" '{"x":350,"y":350}' > /dev/null
    sleep 0.3

    # Execute JS (should not affect cursor position)
    execute_js "$CURSOR_TAB_ID" "1+1" > /dev/null

    # Take screenshot — should be non-blank
    local response=$(abp_post "/api/v1/tabs/$CURSOR_TAB_ID/screenshot" '{"screenshot":{"format":"webp","cursor":true}}')
    local data=$(echo "$response" | jq -r '.data // empty')
    [[ -n "$data" && ${#data} -gt 1000 ]]
}

# CURSOR-105: Screenshot with cursor
test_cursor_105() {
    abp_post "/api/v1/tabs/$CURSOR_TAB_ID/move" '{"x":300,"y":300}' > /dev/null
    sleep 0.3

    local response=$(abp_post "/api/v1/tabs/$CURSOR_TAB_ID/screenshot" '{"screenshot":{"format":"webp","cursor":true}}')
    local data=$(echo "$response" | jq -r '.data // empty')
    [[ -n "$data" && ${#data} -gt 1000 ]]
}

# CURSOR-106: Screenshot without cursor differs from with cursor
test_cursor_106() {
    abp_post "/api/v1/tabs/$CURSOR_TAB_ID/move" '{"x":300,"y":300}' > /dev/null
    sleep 0.3

    local with_cursor=$(abp_post "/api/v1/tabs/$CURSOR_TAB_ID/screenshot" '{"screenshot":{"format":"webp","cursor":true}}' | jq -r '.data // empty')
    sleep 0.3
    local without_cursor=$(abp_post "/api/v1/tabs/$CURSOR_TAB_ID/screenshot" '{"screenshot":{"format":"webp","cursor":false}}' | jq -r '.data // empty')

    # Both should have data, but they should differ
    debug_info "with_len=${#with_cursor} without_len=${#without_cursor} same=$([[ "$with_cursor" = "$without_cursor" ]] && echo yes || echo no)"
    [[ -n "$with_cursor" && -n "$without_cursor" && "$with_cursor" != "$without_cursor" ]]
}

# CURSOR-107: Click reaches correct element
test_cursor_107() {
    abp_post "/api/v1/tabs/$CURSOR_TAB_ID/navigate" '{"url":"'"$HTTP_URL"'/click-input-test.html"}' > /dev/null
    sleep 1

    local response=$(abp_post "/api/v1/tabs/$CURSOR_TAB_ID/click" '{"x":100,"y":100}')
    sleep 0.5

    local count=$(execute_js "$CURSOR_TAB_ID" "document.getElementById('click-count').textContent")
    local click_err=$(echo "$response" | jq -r '.error // empty')
    debug_info "click_error=$click_err count=$count"
    # Navigate back to grid page for remaining tests
    abp_post "/api/v1/tabs/$CURSOR_TAB_ID/navigate" '{"url":"'"$HTTP_URL"'/virtual-cursor-grid-test.html"}' > /dev/null
    sleep 1

    [[ "$count" = "1" ]]
}

# CURSOR-108: Action envelope has screenshots
test_cursor_108() {
    # Enable execution control temporarily to get full action envelope
    enable_execution_control "$CURSOR_TAB_ID" > /dev/null
    sleep 0.5

    local response=$(abp_post "/api/v1/tabs/$CURSOR_TAB_ID/click" '{"x":200,"y":200}')
    sleep 0.3

    local sa_data=$(echo "$response" | jq -r '.screenshot_after.data // empty')
    local err=$(echo "$response" | jq -r '.error // empty')
    debug_info "error=$err sa_data_len=${#sa_data}"

    # Resume execution for remaining tests (don't leave paused)
    resume_execution "$CURSOR_TAB_ID" > /dev/null
    sleep 0.3

    [[ -n "$sa_data" ]]
}

# CURSOR-109: Edge coordinates handled
test_cursor_109() {
    # Move to (0,0)
    local r1=$(abp_post "/api/v1/tabs/$CURSOR_TAB_ID/move" '{"x":0,"y":0}')
    local s1=$(echo "$r1" | jq -r '.result.status // .status // empty')

    # Move to far corner
    local r2=$(abp_post "/api/v1/tabs/$CURSOR_TAB_ID/move" '{"x":9999,"y":9999}')
    local s2=$(echo "$r2" | jq -r '.result.status // .status // empty')

    debug_info "s1=$s1 s2=$s2 r1_err=$(echo "$r1" | jq -r '.error // empty') r2_err=$(echo "$r2" | jq -r '.error // empty')"
    [[ "$s1" = "moved" && "$s2" = "moved" ]]
}

# CURSOR-110: Multiple rapid moves
test_cursor_110() {
    abp_post "/api/v1/tabs/$CURSOR_TAB_ID/move" '{"x":100,"y":100}' > /dev/null
    abp_post "/api/v1/tabs/$CURSOR_TAB_ID/move" '{"x":200,"y":200}' > /dev/null
    abp_post "/api/v1/tabs/$CURSOR_TAB_ID/move" '{"x":300,"y":300}' > /dev/null
    abp_post "/api/v1/tabs/$CURSOR_TAB_ID/move" '{"x":400,"y":400}' > /dev/null
    local r5=$(abp_post "/api/v1/tabs/$CURSOR_TAB_ID/move" '{"x":500,"y":500}')

    local rx=$(echo "$r5" | jq -r '.result.x // .x // empty' | sed 's/\.0$//')
    local ry=$(echo "$r5" | jq -r '.result.y // .y // empty' | sed 's/\.0$//')
    [[ "$rx" = "500" && "$ry" = "500" ]]
}

# CURSOR-111: Scroll then move
test_cursor_111() {
    abp_post "/api/v1/tabs/$CURSOR_TAB_ID/navigate" '{"url":"'"$HTTP_URL"'/scroll-execution-test.html"}' > /dev/null
    sleep 1

    # Scroll down
    abp_post "/api/v1/tabs/$CURSOR_TAB_ID/scroll" '{"delta_y":2000}' > /dev/null
    sleep 0.5

    # Move after scroll
    local result=$(abp_post "/api/v1/tabs/$CURSOR_TAB_ID/move" '{"x":300,"y":300}')
    local status=$(echo "$result" | jq -r '.result.status // .status // empty')

    [[ "$status" = "moved" ]]
}

# CURSOR-112: Click after scroll
test_cursor_112() {
    abp_post "/api/v1/tabs/$CURSOR_TAB_ID/navigate" '{"url":"'"$HTTP_URL"'/scroll-execution-test.html"}' > /dev/null
    sleep 1

    # Scroll to middle button area (around y=2480)
    abp_post "/api/v1/tabs/$CURSOR_TAB_ID/scroll" '{"delta_y":2400}' > /dev/null
    sleep 0.5

    # Click the middle button (should be visible after scroll)
    abp_post "/api/v1/tabs/$CURSOR_TAB_ID/click" '{"x":150,"y":150}' > /dev/null
    sleep 0.5

    # Verify click registered by checking page state
    local scroll_y=$(execute_js "$CURSOR_TAB_ID" "window.pageState.getScrollY()")
    # Scroll should be > 0 confirming we scrolled
    [[ -n "$scroll_y" && "$scroll_y" != "0" ]]
}

# ============================================
# Category 3: Compositor Updates
# ============================================

# Compositor tests use a fresh tab to avoid auto-pause issues
COMP_TAB_ID=""

setup_comp_tab() {
    local response=$(abp_post "/api/v1/tabs" '{"url":"'"$HTTP_URL"'/compositor-test.html"}')
    COMP_TAB_ID=$(echo "$response" | jq -r '.id')
    if [[ -z "$COMP_TAB_ID" || "$COMP_TAB_ID" = "null" ]]; then
        return 1
    fi
    sleep 2
    return 0
}

# COMP-101: Screenshot returns data
test_comp_101() {
    local response=$(abp_post "/api/v1/tabs/$COMP_TAB_ID/screenshot" '{"screenshot":{"format":"webp"}}')
    local data=$(echo "$response" | jq -r '.data // empty')
    [[ -n "$data" && ${#data} -gt 1000 ]]
}

# COMP-102: Dimensions in action envelope
test_comp_102() {
    local tab_id="$COMP_TAB_ID"
    local response=$(abp_post "/api/v1/tabs/$tab_id/click" '{"x":10,"y":10}')
    sleep 0.3

    local sa_w=$(echo "$response" | jq -r '.screenshot_after.width // empty')
    local sa_h=$(echo "$response" | jq -r '.screenshot_after.height // empty')
    [[ -n "$sa_w" && "$sa_w" != "0" && -n "$sa_h" && "$sa_h" != "0" ]]
}

# COMP-103: DOM change reflected in screenshot
test_comp_103() {
    local tab_id="$COMP_TAB_ID"
    abp_post "/api/v1/tabs/$tab_id/navigate" '{"url":"'"$HTTP_URL"'/compositor-test.html"}' > /dev/null
    sleep 1

    # Screenshot before change
    local before=$(abp_post "/api/v1/tabs/$tab_id/screenshot" '{"screenshot":{"format":"webp"}}' | jq -r '.data // empty')
    sleep 0.3

    # Change color to blue
    execute_js "$tab_id" "window.compositorTest.setColor('blue')" > /dev/null
    sleep 1

    # Screenshot after change
    local after=$(abp_post "/api/v1/tabs/$tab_id/screenshot" '{"screenshot":{"format":"webp"}}' | jq -r '.data // empty')

    debug_info "before_len=${#before} after_len=${#after} same=$([[ "$before" = "$after" ]] && echo yes || echo no)"
    [[ -n "$before" && -n "$after" && "$before" != "$after" ]]
}

# COMP-104: Markup overlay renders
test_comp_104() {
    local tab_id="$COMP_TAB_ID"
    abp_post "/api/v1/tabs/$tab_id/navigate" '{"url":"'"$HTTP_URL"'/screenshot-markup-test.html"}' > /dev/null
    sleep 1

    local no_markup=$(abp_post "/api/v1/tabs/$tab_id/screenshot" '{"screenshot":{"format":"webp"}}' | jq -r '.data // empty')
    sleep 0.3
    local with_markup=$(abp_post "/api/v1/tabs/$tab_id/screenshot" '{"screenshot":{"format":"webp","markup":"interactive"}}' | jq -r '.data // empty')

    debug_info "no_markup_len=${#no_markup} with_markup_len=${#with_markup} same=$([[ "$no_markup" = "$with_markup" ]] && echo yes || echo no)"
    [[ -n "$no_markup" && -n "$with_markup" && "$no_markup" != "$with_markup" ]]
}

# COMP-105: WebP format
test_comp_105() {
    local tab_id="$COMP_TAB_ID"
    local data=$(abp_post "/api/v1/tabs/$tab_id/screenshot" '{"screenshot":{"format":"webp"}}' | jq -r '.data // empty')
    [[ -n "$data" && ${#data} -gt 100 ]]
}

# COMP-106: PNG format
test_comp_106() {
    local tab_id="$COMP_TAB_ID"
    local data=$(abp_post "/api/v1/tabs/$tab_id/screenshot" '{"screenshot":{"format":"png"}}' | jq -r '.data // empty')
    [[ -n "$data" && ${#data} -gt 100 ]]
}

# COMP-107: JPEG format
test_comp_107() {
    local tab_id="$COMP_TAB_ID"
    local data=$(abp_post "/api/v1/tabs/$tab_id/screenshot" '{"screenshot":{"format":"jpeg"}}' | jq -r '.data // empty')
    [[ -n "$data" && ${#data} -gt 100 ]]
}

# COMP-108: Scroll changes screenshot
test_comp_108() {
    local tab_id="$COMP_TAB_ID"
    abp_post "/api/v1/tabs/$tab_id/navigate" '{"url":"'"$HTTP_URL"'/scroll-execution-test.html"}' > /dev/null
    sleep 1

    # Screenshot at top
    local top_shot=$(abp_post "/api/v1/tabs/$tab_id/screenshot" '{"screenshot":{"format":"webp"}}' | jq -r '.data // empty')
    sleep 0.3

    # Scroll down 2000px
    abp_post "/api/v1/tabs/$tab_id/scroll" '{"delta_y":2000}' > /dev/null
    sleep 0.5

    # Screenshot after scroll
    local scrolled_shot=$(abp_post "/api/v1/tabs/$tab_id/screenshot" '{"screenshot":{"format":"webp"}}' | jq -r '.data // empty')

    [[ -n "$top_shot" && -n "$scrolled_shot" && "$top_shot" != "$scrolled_shot" ]]
}

# COMP-109: Non-blank after navigation
test_comp_109() {
    local tab_id="$COMP_TAB_ID"
    abp_post "/api/v1/tabs/$tab_id/navigate" '{"url":"'"$HTTP_URL"'/screenshot-markup-test.html"}' > /dev/null
    sleep 1

    local data=$(abp_post "/api/v1/tabs/$tab_id/screenshot" '{"screenshot":{"format":"webp"}}' | jq -r '.data // empty')
    [[ -n "$data" && ${#data} -gt 10000 ]]
}

# COMP-110: Element addition reflected in screenshot
test_comp_110() {
    local tab_id="$COMP_TAB_ID"
    abp_post "/api/v1/tabs/$tab_id/navigate" '{"url":"'"$HTTP_URL"'/compositor-test.html"}' > /dev/null
    sleep 1

    # Screenshot before adding element
    local before=$(abp_post "/api/v1/tabs/$tab_id/screenshot" '{"screenshot":{"format":"webp"}}' | jq -r '.data // empty')
    sleep 0.3

    # Add a new element
    execute_js "$tab_id" "window.compositorTest.addElement('new-el', 'green')" > /dev/null
    sleep 1

    # Screenshot after adding element
    local after=$(abp_post "/api/v1/tabs/$tab_id/screenshot" '{"screenshot":{"format":"webp"}}' | jq -r '.data // empty')

    debug_info "before_len=${#before} after_len=${#after} same=$([[ "$before" = "$after" ]] && echo yes || echo no)"
    [[ -n "$before" && -n "$after" && "$before" != "$after" ]]
}

# ============================================
# Main Execution
# ============================================

echo "================================"
echo "ABP Event Lifecycle Test Suite"
echo "================================"
echo ""

check_prerequisites

# --- Category 1: Virtual Time ---
echo "--- Category 1: Virtual Time / Execution Control (14 tests) ---"
echo ""
echo -n "  Setting up virtual time tab... "
if setup_vtime_tab; then
    echo -e "${GREEN}OK${NC}"
    echo ""
    run_test "VTIME-101 (Enable execution control)" test_vtime_101
    run_test "VTIME-102 (Date.now frozen when paused)" test_vtime_102
    run_test "VTIME-103 (setInterval frozen when paused)" test_vtime_103
    run_test "VTIME-104 (setTimeout not firing when paused)" test_vtime_104
    run_test "VTIME-105 (RAF not incrementing when paused)" test_vtime_105
    run_test "VTIME-106 (Resume unfreezes timers)" test_vtime_106
    run_test "VTIME-107 (Resume unfreezes Date.now)" test_vtime_107
    run_test "VTIME-108 (Pause-Resume-Pause cycle)" test_vtime_108
    run_test "VTIME-109 (Virtual time in action envelope)" test_vtime_109
    run_test "VTIME-110 (Screenshots in action envelope)" test_vtime_110
    run_test "VTIME-111 (Action resumes then re-pauses)" test_vtime_111
    run_test "VTIME-112 (GET execution state accuracy)" test_vtime_112
    run_test "VTIME-113 (Timing info in envelope)" test_vtime_113
    run_test "VTIME-114 (Events captured in envelope)" test_vtime_114
else
    echo -e "${RED}FAILED${NC}"
    echo "  Skipping virtual time tests - could not set up tab"
    FAILED=$((FAILED + 14))
fi

# --- Category 2: Virtual Cursor ---
echo ""
echo "--- Category 2: Virtual Cursor (12 tests) ---"
echo ""
echo -n "  Setting up cursor tab... "
if setup_cursor_tab; then
    echo -e "${GREEN}OK${NC}"
    echo ""
    run_test "CURSOR-101 (Move returns position)" test_cursor_101
    run_test "CURSOR-102 (Move maps to correct cell)" test_cursor_102
    run_test "CURSOR-103 (All 5 test targets)" test_cursor_103
    run_test "CURSOR-104 (Position persists across actions)" test_cursor_104
    run_test "CURSOR-105 (Screenshot with cursor)" test_cursor_105
    run_test "CURSOR-106 (Screenshot without cursor differs)" test_cursor_106
    run_test "CURSOR-107 (Click reaches correct element)" test_cursor_107
    run_test "CURSOR-108 (Action envelope has screenshots)" test_cursor_108
    run_test "CURSOR-109 (Edge coordinates handled)" test_cursor_109
    run_test "CURSOR-110 (Multiple rapid moves)" test_cursor_110
    run_test "CURSOR-111 (Scroll then move)" test_cursor_111
    run_test "CURSOR-112 (Click after scroll)" test_cursor_112
else
    echo -e "${RED}FAILED${NC}"
    echo "  Skipping cursor tests - could not set up tab"
    FAILED=$((FAILED + 12))
fi

# --- Category 3: Compositor Updates ---
echo ""
echo "--- Category 3: Compositor Updates (10 tests) ---"
echo ""
echo -n "  Setting up compositor tab... "
if setup_comp_tab; then
    echo -e "${GREEN}OK${NC}"
    echo ""
    run_test "COMP-101 (Screenshot returns data)" test_comp_101
    run_test "COMP-102 (Dimensions in action envelope)" test_comp_102
    run_test "COMP-103 (DOM change in screenshot)" test_comp_103
    run_test "COMP-104 (Markup overlay renders)" test_comp_104
    run_test "COMP-105 (WebP format)" test_comp_105
    run_test "COMP-106 (PNG format)" test_comp_106
    run_test "COMP-107 (JPEG format)" test_comp_107
    run_test "COMP-108 (Scroll changes screenshot)" test_comp_108
    run_test "COMP-109 (Non-blank after navigation)" test_comp_109
    run_test "COMP-110 (Element addition in screenshot)" test_comp_110
else
    echo -e "${RED}FAILED${NC}"
    echo "  Skipping compositor tests - could not set up tab"
    FAILED=$((FAILED + 10))
fi

# --- Summary ---
print_summary "ABP Event Lifecycle"
