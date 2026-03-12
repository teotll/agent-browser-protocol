#!/bin/bash
# Test script for ABP console capture (WebContentsObserver-based)
# Usage: ./tools/abp-tests/test_console_capture.sh
# Requires: ABP running on localhost:8222
#
# Deterministic strategy:
#   - Never rely on DELETE + immediate query (stale renderer IPCs can arrive after clear)
#   - Use after_id pagination to isolate each test's entries
#   - After async operations (fetch, setTimeout), do a follow-up execute to flush callbacks
#   - poll_console() retries queries to handle IPC arrival jitter

set -e

BASE="http://localhost:8222/api/v1"
PASS=0
FAIL=0

green() { printf "\033[32m%s\033[0m\n" "$1"; }
red()   { printf "\033[31m%s\033[0m\n" "$1"; }
bold()  { printf "\033[1m%s\033[0m\n" "$1"; }

# check_result <description> <python_output>
# Expects python to print lines, last line being "PASS" or "FAIL:reason".
# All lines before the verdict are printed as-is (indented details).
check_result() {
  local desc="$1" output="$2"
  local verdict
  # Print detail lines (everything except last line)
  local total_lines
  total_lines=$(echo "$output" | wc -l | tr -d ' ')
  if [ "$total_lines" -gt 1 ]; then
    echo "$output" | sed '$d'
  fi
  # Last line is the verdict
  verdict=$(echo "$output" | tail -1)
  case "$verdict" in
    PASS)
      green "  PASS: $desc"
      PASS=$((PASS + 1))
      ;;
    FAIL:*)
      red "  FAIL: $desc (${verdict#FAIL:})"
      FAIL=$((FAIL + 1))
      ;;
    *)
      red "  FAIL: $desc (unexpected output: $verdict)"
      FAIL=$((FAIL + 1))
      ;;
  esac
}

# Return the highest entry id currently in the buffer (0 if empty).
max_id() {
  curl -s "$BASE/console?limit=1000" | python3 -c "
import sys, json
d = json.load(sys.stdin)
print(max((int(e['id']) for e in d['entries']), default=0))
"
}

# Poll console entries with after_id until at least $min_count new entries appear
# or $max_wait half-second ticks elapse.  Prints the JSON response.
# Usage: poll_console <after_id> <min_count> <max_wait> [extra_query_params]
poll_console() {
  local after_id="$1" min_count="$2" max_wait="$3" extra="${4:-}"
  local elapsed=0 result="" count=0
  local url="$BASE/console?after_id=${after_id}${extra:+&$extra}"
  while [ $elapsed -lt "$max_wait" ]; do
    result=$(curl -s "$url")
    count=$(echo "$result" | python3 -c "import sys,json; print(len(json.load(sys.stdin)['entries']))" 2>/dev/null || echo "0")
    if [ "$count" -ge "$min_count" ]; then
      echo "$result"
      return 0
    fi
    sleep 0.5
    elapsed=$((elapsed + 1))
  done
  echo "$result"
  return 0
}

# ─── Wait for browser ────────────────────────────────────────────────
bold "=== Waiting for ABP browser ==="
for i in $(seq 1 30); do
  if curl -s "$BASE/browser/status" | grep -q "ready"; then
    green "Browser ready"
    break
  fi
  if [ "$i" -eq 30 ]; then
    red "Browser not ready after 30s"
    exit 1
  fi
  sleep 1
done

# Get initial tab
TAB1=$(curl -s "$BASE/tabs" | python3 -c "import sys,json; print(json.load(sys.stdin)[0]['id'])")
echo "Tab1: $TAB1"

# Close any extra tabs from prior runs
for EXTRA in $(curl -s "$BASE/tabs" | python3 -c "
import sys,json
for t in json.load(sys.stdin):
    if t['id'] != '$TAB1': print(t['id'])
"); do
  curl -s -X DELETE "$BASE/tabs/$EXTRA" > /dev/null
  sleep 0.5
done

# Drain stale entries: navigate to blank, wait, clear.
# The navigate creates the console observer for this tab (via FindWebContents).
curl -s -X POST "$BASE/tabs/$TAB1/navigate" \
  -H "Content-Type: application/json" -d '{"url":"about:blank"}' > /dev/null
sleep 3
curl -s -X DELETE "$BASE/console" > /dev/null
sleep 2

# ─── Test 1: console.log / warn / error / info ───────────────────────
bold ""
bold "=== Test 1: console.log / warn / error / info ==="
AFTER=$(max_id)
curl -s -X POST "$BASE/tabs/$TAB1/navigate" \
  -H "Content-Type: application/json" \
  -d '{"url":"data:text/html,<html><body><script>console.log(\"t1-log\");console.warn(\"t1-warn\");console.error(\"t1-error\");console.info(\"t1-info\");</script></body></html>"}' > /dev/null
# Flush: resume renderer so pending console IPCs are delivered
curl -s -X POST "$BASE/tabs/$TAB1/execute" \
  -H "Content-Type: application/json" -d '{"script":"\"flush\""}' > /dev/null

RESULT=$(poll_console "$AFTER" 3 20 "pattern=t1-")
OUTPUT=$(echo "$RESULT" | python3 -c "
import sys, json
d = json.load(sys.stdin)
t1 = [e for e in d['entries'] if e['message'].startswith('t1-')]
levels = {e['level'] for e in t1}
for e in t1:
    print(f'  [{e[\"level\"]}] {e[\"message\"]}')
expected = {'info', 'warning', 'error'}
print('PASS' if levels == expected else f'FAIL:expected {expected}, got {levels}')
")
check_result "all four log levels captured" "$OUTPUT"

# ─── Test 2: Uncaught exception ──────────────────────────────────────
bold ""
bold "=== Test 2: Uncaught exception ==="
AFTER=$(max_id)
curl -s -X POST "$BASE/tabs/$TAB1/execute" \
  -H "Content-Type: application/json" \
  -d '{"script":"setTimeout(()=>{throw new Error(\"t2-uncaught\")},0);\"ok\""}' > /dev/null
# Flush: second execute lets the setTimeout callback run during resume
curl -s -X POST "$BASE/tabs/$TAB1/execute" \
  -H "Content-Type: application/json" -d '{"script":"\"flush\""}' > /dev/null

RESULT=$(poll_console "$AFTER" 1 20 "pattern=t2-uncaught")
OUTPUT=$(echo "$RESULT" | python3 -c "
import sys, json
d = json.load(sys.stdin)
hits = [e for e in d['entries'] if 't2-uncaught' in e['message']]
for e in hits: print(f'  [{e[\"level\"]}] {e[\"message\"]}')
print('PASS' if hits else 'FAIL:no uncaught exception found')
")
check_result "uncaught exception captured" "$OUTPUT"

# ─── Test 3: CORS error ─────────────────────────────────────────────
bold ""
bold "=== Test 3: CORS error ==="
AFTER=$(max_id)
curl -s -X POST "$BASE/tabs/$TAB1/navigate" \
  -H "Content-Type: application/json" \
  -d '{"url":"data:text/html,<html><body><script>fetch(\"http://example.com/api\").catch(e=>console.error(\"t3-cors:\"+e));</script></body></html>"}' > /dev/null
# Flush: execute to let the fetch rejection callback fire
curl -s -X POST "$BASE/tabs/$TAB1/execute" \
  -H "Content-Type: application/json" -d '{"script":"\"flush\""}' > /dev/null

RESULT=$(poll_console "$AFTER" 1 20 "pattern=t3-cors")
OUTPUT=$(echo "$RESULT" | python3 -c "
import sys, json
d = json.load(sys.stdin)
hits = [e for e in d['entries'] if 't3-cors' in e['message']]
for e in hits: print(f'  [{e[\"level\"]}] {e[\"message\"][:100]}')
print('PASS' if hits else 'FAIL:no CORS error found')
")
check_result "CORS error captured" "$OUTPUT"

# ─── Test 4: CSP violation ──────────────────────────────────────────
bold ""
bold "=== Test 4: CSP violation ==="
AFTER=$(max_id)
curl -s -X POST "$BASE/tabs/$TAB1/navigate" \
  -H "Content-Type: application/json" \
  -d '{"url":"data:text/html,<html><head><meta http-equiv=\"Content-Security-Policy\" content=\"script-src none\"></head><body><script>console.log(\"t4\");</script></body></html>"}' > /dev/null
# Flush: resume renderer so pending console IPCs (CSP violation) are delivered
curl -s -X POST "$BASE/tabs/$TAB1/execute" \
  -H "Content-Type: application/json" -d '{"script":"\"flush\""}' > /dev/null

RESULT=$(poll_console "$AFTER" 1 20 "pattern=Content.Security")
OUTPUT=$(echo "$RESULT" | python3 -c "
import sys, json
d = json.load(sys.stdin)
hits = [e for e in d['entries'] if 'content security policy' in e['message'].lower()]
for e in hits: print(f'  [{e[\"level\"]}] {e[\"message\"][:100]}')
print('PASS' if hits else 'FAIL:no CSP violation found')
")
check_result "CSP violation captured" "$OUTPUT"

# ─── Test 5: Level filter ───────────────────────────────────────────
bold ""
bold "=== Test 5: Level filter (error only) ==="
OUTPUT=$(curl -s "$BASE/console?level=error" | python3 -c "
import sys, json
d = json.load(sys.stdin)
entries = d['entries']
ok = len(entries) > 0 and all(e['level'] == 'error' for e in entries)
print(f'  error entries: {len(entries)}')
print('PASS' if ok else 'FAIL:non-error entries returned or empty')
")
check_result "level filter returns only errors" "$OUTPUT"

# ─── Test 6: Pattern filter ─────────────────────────────────────────
bold ""
bold "=== Test 6: Pattern regex filter ==="
OUTPUT=$(curl -s "$BASE/console?pattern=t1-warn" | python3 -c "
import sys, json
d = json.load(sys.stdin)
ok = len(d['entries']) == 1 and 't1-warn' in d['entries'][0]['message']
print(f'  matches: {len(d[\"entries\"])}')
print('PASS' if ok else f'FAIL:expected 1 match, got {len(d[\"entries\"])}')
")
check_result "pattern filter matches exactly" "$OUTPUT"

# ─── Test 7: after_id pagination ────────────────────────────────────
bold ""
bold "=== Test 7: after_id pagination ==="
FIRST_ID=$(curl -s "$BASE/console?limit=1" | python3 -c "import sys,json; print(int(json.load(sys.stdin)['entries'][0]['id']))")
SECOND_ID=$(curl -s "$BASE/console?after_id=$FIRST_ID&limit=1" | python3 -c "import sys,json; print(int(json.load(sys.stdin)['entries'][0]['id']))")
if [ "$SECOND_ID" -gt "$FIRST_ID" ]; then
  green "  PASS: after_id returns subsequent entry ($FIRST_ID -> $SECOND_ID)"
  PASS=$((PASS + 1))
else
  red "  FAIL: after_id pagination (second id $SECOND_ID not > first id $FIRST_ID)"
  FAIL=$((FAIL + 1))
fi

# ─── Test 8: limit ──────────────────────────────────────────────────
bold ""
bold "=== Test 8: limit ==="
COUNT=$(curl -s "$BASE/console?limit=2" | python3 -c "import sys,json; print(len(json.load(sys.stdin)['entries']))")
if [ "$COUNT" -eq 2 ]; then
  green "  PASS: limit=2 returns exactly 2 entries"
  PASS=$((PASS + 1))
else
  red "  FAIL: limit (expected 2, got $COUNT)"
  FAIL=$((FAIL + 1))
fi

# ─── Test 9: Cross-tab capture + tab_id filter ──────────────────────
bold ""
bold "=== Test 9: Cross-tab capture ==="
AFTER=$(max_id)
curl -s -X POST "$BASE/tabs" \
  -H "Content-Type: application/json" \
  -d '{"url":"data:text/html,<html><body><script>console.log(\"t9-tab2\");</script></body></html>"}' > /dev/null
sleep 1

TAB2=$(curl -s "$BASE/tabs" | python3 -c "
import sys,json
tabs = json.load(sys.stdin)
others = [t['id'] for t in tabs if t['id'] != '$TAB1']
print(others[0] if others else '')
")
echo "  Tab2: $TAB2"

if [ -n "$TAB2" ]; then
  # Flush: resume renderer on tab2 so console IPCs are delivered
  curl -s -X POST "$BASE/tabs/$TAB2/execute" \
    -H "Content-Type: application/json" -d '{"script":"\"flush\""}' > /dev/null
  RESULT=$(poll_console "$AFTER" 1 20 "tab_id=$TAB2")
  OUTPUT=$(echo "$RESULT" | python3 -c "
import sys, json
d = json.load(sys.stdin)
hits = [e for e in d['entries'] if 't9-tab2' in e['message']]
ok = len(hits) > 0 and all(e['tab_id'] == '$TAB2' for e in hits)
for e in hits: print(f'  [{e[\"level\"]}] tab={e[\"tab_id\"][:8]}... {e[\"message\"]}')
print('PASS' if ok else f'FAIL:t9 hits={len(hits)}')
")
  check_result "cross-tab capture with tab_id filter" "$OUTPUT"
else
  red "  FAIL: cross-tab capture (could not determine tab2 id)"
  FAIL=$((FAIL + 1))
fi

# ─── Test 10: DELETE clear (all) ────────────────────────────────────
bold ""
bold "=== Test 10: DELETE clear ==="
BEFORE=$(curl -s "$BASE/console" | python3 -c "import sys,json; print(json.load(sys.stdin)['total_buffered'])")
CLEARED=$(curl -s -X DELETE "$BASE/console" | python3 -c "import sys,json; print(json.load(sys.stdin)['cleared'])")
sleep 1
AFTER_DEL=$(curl -s "$BASE/console" | python3 -c "import sys,json; print(json.load(sys.stdin)['total_buffered'])")
echo "  before=$BEFORE cleared=$CLEARED after=$AFTER_DEL"
if [ "$AFTER_DEL" -eq 0 ] && [ "$CLEARED" -gt 0 ]; then
  green "  PASS: DELETE clears all entries"
  PASS=$((PASS + 1))
else
  red "  FAIL: DELETE clear (after=$AFTER_DEL cleared=$CLEARED)"
  FAIL=$((FAIL + 1))
fi

# ─── Test 11: DELETE clear per-tab ──────────────────────────────────
bold ""
bold "=== Test 11: DELETE clear per-tab ==="
if [ -n "$TAB2" ]; then
  # Generate entries on both tabs
  curl -s -X POST "$BASE/tabs/$TAB1/execute" \
    -H "Content-Type: application/json" -d '{"script":"console.log(\"t11-tab1\");\"ok\""}' > /dev/null
  curl -s -X POST "$BASE/tabs/$TAB2/execute" \
    -H "Content-Type: application/json" -d '{"script":"console.log(\"t11-tab2\");\"ok\""}' > /dev/null
  # Wait for both entries to arrive
  poll_console 0 2 20 > /dev/null

  # Clear only tab2
  CLEARED=$(curl -s -X DELETE "$BASE/console?tab_id=$TAB2" | python3 -c "import sys,json; print(json.load(sys.stdin)['cleared'])")
  sleep 1
  OUTPUT=$(curl -s "$BASE/console" | python3 -c "
import sys,json
d = json.load(sys.stdin)
tab2 = [e for e in d['entries'] if e['tab_id']=='$TAB2']
tab1 = [e for e in d['entries'] if e['tab_id']=='$TAB1']
print(f'  tab1_remaining={len(tab1)} tab2_remaining={len(tab2)} cleared=$CLEARED')
print('PASS' if len(tab2) == 0 and len(tab1) > 0 else f'FAIL:tab1={len(tab1)} tab2={len(tab2)}')
")
  check_result "per-tab DELETE clears only target tab" "$OUTPUT"
else
  red "  FAIL: per-tab DELETE (no tab2 available)"
  FAIL=$((FAIL + 1))
fi

# ─── Test 12: MCP browser_console tool ──────────────────────────────
bold ""
bold "=== Test 12: MCP browser_console tool ==="
curl -s -X DELETE "$BASE/console" > /dev/null
sleep 1
curl -s -X POST "$BASE/tabs/$TAB1/execute" \
  -H "Content-Type: application/json" -d '{"script":"console.log(\"t12-mcp\");\"ok\""}' > /dev/null
poll_console 0 1 10 "pattern=t12-mcp" > /dev/null

# Initialize MCP session
curl -s -X POST http://localhost:8222/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","clientInfo":{"name":"test","version":"1.0"},"capabilities":{}}}' > /dev/null

OUTPUT=$(curl -s -X POST http://localhost:8222/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"browser_console","arguments":{}}}' | python3 -c "
import sys, json
d = json.load(sys.stdin)
text = d['result']['content'][0]['text']
data = json.loads(text)
hits = [e for e in data['entries'] if 't12-mcp' in e['message']]
print(f'  MCP entries: {len(data[\"entries\"])}, t12 matches: {len(hits)}')
print('PASS' if hits else 'FAIL:t12-mcp not in MCP response')
")
check_result "MCP browser_console returns entries" "$OUTPUT"

# ─── Test 13: MCP browser_console clear ─────────────────────────────
bold ""
bold "=== Test 13: MCP browser_console clear ==="
OUTPUT=$(curl -s -X POST http://localhost:8222/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"browser_console","arguments":{"clear":true}}}' | python3 -c "
import sys, json
d = json.load(sys.stdin)
text = d['result']['content'][0]['text']
data = json.loads(text)
cleared = data.get('cleared', 0)
print(f'  MCP cleared: {cleared}')
print('PASS' if cleared > 0 else 'FAIL:nothing cleared')
")
check_result "MCP clear works" "$OUTPUT"

# Verify empty after MCP clear
sleep 1
REMAINING=$(curl -s "$BASE/console" | python3 -c "import sys,json; print(json.load(sys.stdin)['total_buffered'])")
if [ "$REMAINING" -eq 0 ]; then
  green "  PASS: MCP clear empties buffer"
  PASS=$((PASS + 1))
else
  red "  FAIL: MCP clear verify (remaining=$REMAINING)"
  FAIL=$((FAIL + 1))
fi

# ─── Clean up: close tab2 ───────────────────────────────────────────
if [ -n "$TAB2" ]; then
  curl -s -X DELETE "$BASE/tabs/$TAB2" > /dev/null
fi

# ─── Summary ────────────────────────────────────────────────────────
echo ""
bold "========================================="
bold "  Console Capture Test Results"
bold "========================================="
green "  Passed: $PASS"
if [ $FAIL -gt 0 ]; then
  red "  Failed: $FAIL"
else
  echo "  Failed: 0"
fi
bold "========================================="

exit $FAIL
