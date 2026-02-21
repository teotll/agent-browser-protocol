# Training Data Guide

ABP records every browser action as a structured training sample, making it possible to turn successful agent sessions into fine-tuning datasets for vision-language models (VLMs).

## How It Works

Every action dispatched through ABP follows a consistent lifecycle:

1. **Capture the "before" state** -- screenshot of the page before any change
2. **Execute the action** -- click, type, navigate, scroll, etc.
3. **Capture the "after" state** -- screenshot of the page after the action completes

This produces a sequence of `(observation, action, outcome)` triples:

```
Action 1: navigate to "https://example.com"
  screenshot_before  ->  { action_type: "navigate", params: { url: "https://example.com" } }  ->  screenshot_after

Action 2: click at (340, 215)
  screenshot_before  ->  { action_type: "click", params: { x: 340, y: 215 } }  ->  screenshot_after

Action 3: type "hello world"
  screenshot_before  ->  { action_type: "type", params: { text: "hello world" } }  ->  screenshot_after
```

Each triple is stored with full metadata (timing, success/failure, result data, associated browser events) in a SQLite database alongside the screenshot files on disk.

The vision: run an agent session, review it for correctness, and feed the successful trajectories directly into VLM fine-tuning pipelines. No separate recording tool, no replay infrastructure, no post-hoc screenshot alignment -- every action is a training sample by default.


## Session Directory

### The `--abp-session-dir` Flag

The `--abp-session-dir` command-line flag controls where session data is stored:

```bash
# Explicit session directory
./out/Default/abp --abp-session-dir=./datasets/session-001

# Default: /tmp/abp-<UUID>/
./out/Default/abp
```

When no flag is provided, ABP generates a random UUID-based directory under the system temp directory (e.g., `/tmp/abp-3a7c1f2e-...`).

### Directory Layout

```
session-001/
├── history.db                          # SQLite database (sessions, actions, events)
└── screenshots/
    ├── 1706123456789_TABID1_before.webp
    ├── 1706123456789_TABID1_after.webp
    ├── 1706123457123_TABID1_before.webp
    ├── 1706123457123_TABID1_after.webp
    └── ...
```

Screenshot filenames follow the pattern `{timestamp_ms}_{tab_id}_{before|after}.webp`. The timestamp is the Unix epoch in milliseconds when the action started, matching the `timestamp` column in the `actions` table.


## SQLite Schema

The `history.db` file contains three tables. Open it with any SQLite client:

```bash
sqlite3 session-001/history.db
```

### `sessions` Table

Each ABP launch creates one session record.

| Column | Type | Description |
|--------|------|-------------|
| `id` | TEXT PRIMARY KEY | UUID v4 (e.g., `"3a7c1f2e-8b4d-4e6a-9f2c-1d3e5f7a8b9c"`) |
| `start_time` | INTEGER NOT NULL | Session start time, Unix epoch in milliseconds |
| `end_time` | INTEGER | Session end time, Unix epoch in milliseconds. NULL if session is still active |
| `browser_version` | TEXT | Chromium version string (e.g., `"131.0.6778.0"`) |
| `user_agent` | TEXT | Browser user-agent string |

### `actions` Table

Every API call that modifies browser state is recorded as an action row.

| Column | Type | Description |
|--------|------|-------------|
| `id` | INTEGER PRIMARY KEY AUTOINCREMENT | Monotonically increasing action ID |
| `session_id` | TEXT NOT NULL (FK -> sessions.id) | Which session this action belongs to |
| `tab_id` | TEXT | DevTools agent host ID of the target tab |
| `action_type` | TEXT NOT NULL | Action name: `navigate`, `click`, `type`, `scroll`, `keyboard_press`, `execute`, `reload`, etc. |
| `timestamp` | INTEGER NOT NULL | Action start time, Unix epoch in milliseconds |
| `duration_ms` | INTEGER | Wall-clock duration of the action in milliseconds |
| `params` | TEXT | JSON-encoded action parameters (e.g., `{"url":"https://example.com"}` or `{"x":340,"y":215}`) |
| `result` | TEXT | JSON-encoded action result data |
| `success` | INTEGER NOT NULL | `1` for success, `0` for failure |
| `error_code` | TEXT | Error code string if `success=0` (e.g., `"NAVIGATION_FAILED"`) |
| `error_message` | TEXT | Human-readable error message if `success=0` |
| `screenshot_before_path` | TEXT | Absolute path to the before-screenshot WebP file |
| `screenshot_after_path` | TEXT | Absolute path to the after-screenshot WebP file |

### `events` Table

Browser events (page loads, navigations, console messages, network activity) that occur during and between actions.

| Column | Type | Description |
|--------|------|-------------|
| `id` | INTEGER PRIMARY KEY AUTOINCREMENT | Monotonically increasing event ID |
| `session_id` | TEXT NOT NULL (FK -> sessions.id) | Which session this event belongs to |
| `tab_id` | TEXT | Tab that emitted the event |
| `event_type` | TEXT NOT NULL | CDP event name (e.g., `Page.loadEventFired`, `Network.requestWillBeSent`) |
| `timestamp` | INTEGER NOT NULL | Event time, Unix epoch in milliseconds |
| `data` | TEXT | JSON-encoded event payload |

### Indexes

The database creates indexes on commonly queried columns for fast lookups:

- `idx_actions_session` -- actions by session_id
- `idx_actions_tab` -- actions by tab_id
- `idx_actions_type` -- actions by action_type
- `idx_actions_timestamp` -- actions by timestamp
- `idx_actions_session_timestamp` -- actions by (session_id, timestamp)
- `idx_actions_session_id` -- actions by (session_id, id)
- `idx_events_session` -- events by session_id
- `idx_events_tab` -- events by tab_id
- `idx_events_type` -- events by event_type
- `idx_events_timestamp` -- events by timestamp
- `idx_events_session_timestamp` -- events by (session_id, timestamp)
- `idx_events_session_id` -- events by (session_id, id)


## Accessing Session Data

### Via REST API

Query the running ABP instance over HTTP for live session data.

**Get current session info:**

```bash
curl http://localhost:8222/api/v1/history/sessions/current
```

**List all actions in the current session:**

```bash
curl http://localhost:8222/api/v1/history/actions
```

**Filter to successful actions only:**

```bash
curl "http://localhost:8222/api/v1/history/actions?success=true"
```

**Get a specific action by ID:**

```bash
curl http://localhost:8222/api/v1/history/actions/42
```

**Get the after-screenshot for an action (returns image/webp):**

```bash
curl http://localhost:8222/api/v1/history/actions/42/screenshot -o action_42.webp

# Get the before-screenshot instead:
curl "http://localhost:8222/api/v1/history/actions/42/screenshot?type=before" -o action_42_before.webp
```

**Export an entire session with base64-encoded screenshots:**

```bash
curl "http://localhost:8222/api/v1/history/sessions/current/export?include_screenshots=true"
```

The export endpoint returns paginated results. Use `next_actions_cursor` and `next_events_cursor` from the response to fetch subsequent pages:

```bash
curl "http://localhost:8222/api/v1/history/sessions/{id}/export?include_screenshots=true&actions_cursor=50&chunk_size=100"
```

### Direct SQLite Access

After a session ends (or while it is running, since WAL mode is enabled with shared locking), query the database directly.

**List all actions chronologically:**

```sql
SELECT id, action_type, timestamp, duration_ms, success, params
FROM actions
ORDER BY id ASC;
```

**Get successful actions only (training samples):**

```sql
SELECT id, action_type, timestamp, duration_ms, params, result,
       screenshot_before_path, screenshot_after_path
FROM actions
WHERE success = 1
ORDER BY id ASC;
```

**Count actions by type with average duration:**

```sql
SELECT action_type,
       COUNT(*) AS count,
       AVG(duration_ms) AS avg_duration_ms,
       SUM(CASE WHEN success = 1 THEN 1 ELSE 0 END) AS success_count
FROM actions
GROUP BY action_type
ORDER BY count DESC;
```

**Join actions with events that occurred during each action:**

```sql
SELECT a.id AS action_id,
       a.action_type,
       a.timestamp AS action_start,
       a.timestamp + a.duration_ms AS action_end,
       e.event_type,
       e.timestamp AS event_time,
       e.data
FROM actions a
LEFT JOIN events e
  ON e.session_id = a.session_id
  AND e.timestamp >= a.timestamp
  AND e.timestamp <= a.timestamp + a.duration_ms
ORDER BY a.id, e.timestamp;
```


## Using abp-debug

`abp-debug` is a companion debug UI that connects to a running ABP instance. It provides a browser-based interface for driving actions and inspecting session history with before/after screenshots side by side.

### Launch

Open two terminals. Start ABP in one and `abp-debug` in the other, both pointing at the same session directory:

**Terminal 1 -- ABP Browser:**

```bash
./out/Default/abp --abp-session-dir=./datasets/session-001
```

**Terminal 2 -- Debug Server:**

```bash
npx abp-debug --session-dir=./datasets/session-001
```

Then open http://localhost:8223 in any browser.

### Architecture

```
ABP Browser (port 8222)               Debug Server (port 8223)
┌────────────────────────────┐         ┌──────────────────────────────┐
│ REST API (/api/v1/*)       │◄────────│ Proxies action requests      │
│ SQLite history DB          │         │ Reads SQLite (read-only)     │
│ Screenshot files on disk   │         │ Serves screenshot files      │
└────────────────────────────┘         │ Serves web UI                │
                                       │ SSE for real-time updates    │
                                       └──────────────────────────────┘
```

- **Action requests** from the debug UI are proxied through to ABP's REST API on port 8222.
- **History queries** read the SQLite database directly in read-only mode (safe because ABP uses WAL mode with shared locking).
- **Screenshot files** are served from the session directory.
- **Real-time updates** use Server-Sent Events (SSE) so the history panel refreshes as new actions are recorded.

### The Two-Panel UI

- **Action Panel (left):** Drive the ABP browser -- navigate to URLs, click coordinates, type text, press keys, scroll. Each action you dispatch is recorded as a training sample.

- **History Panel (right):** Browse the action history for the session. Each action shows its before and after screenshots side by side, the action type and parameters, timing information, and success/failure status. Click any action to inspect its full details.


## Training Data Pipeline

### Training Sample Structure

Each successful action maps to a training sample with three components:

| Component | Source | Description |
|-----------|--------|-------------|
| **Observation** | `screenshot_before_path` | What the page looked like before the action (WebP image) |
| **Action** | `action_type` + `params` | What action was taken and with what parameters |
| **Outcome** | `screenshot_after_path` | What the page looked like after the action |

### Export Script

The following Python script reads a session database and writes a JSON Lines file suitable for VLM fine-tuning:

```python
#!/usr/bin/env python3
"""Export ABP session data to JSON Lines format for VLM fine-tuning."""

import base64
import json
import sqlite3
import sys
from pathlib import Path


def export_session(session_dir: str, output_path: str):
    db_path = Path(session_dir) / "history.db"
    if not db_path.exists():
        print(f"Database not found: {db_path}", file=sys.stderr)
        sys.exit(1)

    conn = sqlite3.connect(str(db_path))
    conn.row_factory = sqlite3.Row

    # Query successful actions with screenshots
    cursor = conn.execute("""
        SELECT id, action_type, params, result, timestamp, duration_ms,
               screenshot_before_path, screenshot_after_path
        FROM actions
        WHERE success = 1
          AND screenshot_before_path IS NOT NULL
          AND screenshot_before_path != ''
          AND screenshot_after_path IS NOT NULL
          AND screenshot_after_path != ''
        ORDER BY id ASC
    """)

    count = 0
    with open(output_path, "w") as f:
        for row in cursor:
            # Load screenshots as base64
            before_path = Path(row["screenshot_before_path"])
            after_path = Path(row["screenshot_after_path"])

            if not before_path.exists() or not after_path.exists():
                continue

            before_b64 = base64.b64encode(before_path.read_bytes()).decode()
            after_b64 = base64.b64encode(after_path.read_bytes()).decode()

            # Parse JSON fields
            params = json.loads(row["params"]) if row["params"] else {}
            result = json.loads(row["result"]) if row["result"] else {}

            sample = {
                "id": row["id"],
                "observation": {
                    "screenshot": before_b64,
                    "format": "webp",
                },
                "action": {
                    "type": row["action_type"],
                    "params": params,
                },
                "outcome": {
                    "screenshot": after_b64,
                    "format": "webp",
                    "result": result,
                },
                "metadata": {
                    "timestamp": row["timestamp"],
                    "duration_ms": row["duration_ms"],
                },
            }

            f.write(json.dumps(sample) + "\n")
            count += 1

    conn.close()
    print(f"Exported {count} training samples to {output_path}")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("Usage: python export_training_data.py <session_dir> <output.jsonl>")
        sys.exit(1)

    export_session(sys.argv[1], sys.argv[2])
```

Usage:

```bash
python export_training_data.py ./datasets/session-001 training_data.jsonl
```

### Multi-Step Trajectories

For tasks that require a sequence of actions (e.g., "fill out a form and submit it"), you can group consecutive actions into trajectories:

```python
# Extract trajectories grouped by tab
cursor = conn.execute("""
    SELECT id, tab_id, action_type, params, timestamp,
           screenshot_before_path, screenshot_after_path
    FROM actions
    WHERE success = 1
      AND session_id = ?
    ORDER BY id ASC
""", (session_id,))

trajectory = []
for row in cursor:
    trajectory.append({
        "step": len(trajectory),
        "action_type": row["action_type"],
        "params": json.loads(row["params"]) if row["params"] else {},
        "screenshot_before": row["screenshot_before_path"],
        "screenshot_after": row["screenshot_after_path"],
    })
```

### Filtering Notes

Not all recorded actions are equally useful for training. Consider these filters:

- **`success = 1`** -- Only successful actions. Failed actions are useful for debugging but produce noisy training signal.
- **`duration_ms`** -- Exclude abnormally slow actions (e.g., `duration_ms > 10000`) which may indicate timeouts or network issues.
- **`action_type`** -- Focus on interaction-heavy action types (`click`, `type`, `scroll`, `keyboard_press`, `navigate`) and exclude passive queries (`text`, `execute`).
- **Screenshot existence** -- Some actions may not have screenshots if screenshot capture failed or was disabled. Filter to rows where both `screenshot_before_path` and `screenshot_after_path` are non-empty.
- **Consecutive pairs** -- For trajectory-based training, ensure actions are contiguous within the same tab to avoid context switches.
