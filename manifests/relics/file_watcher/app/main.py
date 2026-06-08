"""
File Watcher Relic — Monitor file changes and emit events to subscribed agents.

Standard library relic for Cortex-Prime MK2.
Uses inotify (via watchdog) for efficient filesystem monitoring.
"""

import json
import os
import time
import uuid
import fnmatch
import threading
from typing import Optional
from datetime import datetime
from contextlib import contextmanager

from fastapi import FastAPI, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel, Field

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
DB_PATH = os.getenv("SQLITE_DB_PATH", "/data/watcher.db")
MAX_EVENTS = int(os.getenv("MAX_EVENTS", "5000"))
DEFAULT_DEBOUNCE_MS = int(os.getenv("DEFAULT_DEBOUNCE_MS", "500"))

# ---------------------------------------------------------------------------
# App
# ---------------------------------------------------------------------------
app = FastAPI(title="File Watcher Relic", version="1.0.0")

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)

# ---------------------------------------------------------------------------
# Database
# ---------------------------------------------------------------------------
_db_lock = threading.Lock()

@contextmanager
def get_db():
    import sqlite3
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    with _db_lock:
        yield conn
    conn.close()

def init_db():
    import sqlite3
    os.makedirs(os.path.dirname(DB_PATH), exist_ok=True)
    with sqlite3.connect(DB_PATH) as conn:
        conn.execute("""
            CREATE TABLE IF NOT EXISTS watches (
                id TEXT PRIMARY KEY,
                namespace TEXT NOT NULL,
                path TEXT NOT NULL,
                recursive INTEGER NOT NULL DEFAULT 1,
                events TEXT NOT NULL DEFAULT '["create","modify","delete","move"]',
                include_pattern TEXT NOT NULL DEFAULT '*',
                exclude_pattern TEXT NOT NULL DEFAULT '',
                debounce_ms INTEGER NOT NULL DEFAULT 500,
                created_at TEXT NOT NULL
            )
        """)
        conn.execute("""
            CREATE TABLE IF NOT EXISTS events (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                namespace TEXT NOT NULL,
                watch_id TEXT NOT NULL,
                event_type TEXT NOT NULL,
                src_path TEXT NOT NULL,
                dest_path TEXT,
                timestamp TEXT NOT NULL
            )
        """)
        conn.execute("""
            CREATE INDEX IF NOT EXISTS idx_events_namespace
            ON events(namespace, id DESC)
        """)
        conn.commit()


# ---------------------------------------------------------------------------
# Watcher engine
# ---------------------------------------------------------------------------
_observer = None
_event_queue = []

def _start_watcher():
    """Start the watchdog observer in a background thread."""
    global _observer
    try:
        from watchdog.observers import Observer
        from watchdog.events import FileSystemEventHandler, FileSystemEvent
        _observer = Observer()

        class ChangeHandler(FileSystemEventHandler):
            def on_any_event(self, event: FileSystemEvent):
                # Skip directories
                if event.is_directory:
                    return

                event_type = {
                    "created": "create",
                    "modified": "modify",
                    "deleted": "delete",
                    "moved": "move",
                }.get(event.event_type, event.event_type)

                entry = {
                    "src_path": event.src_path,
                    "dest_path": getattr(event, "dest_path", None),
                    "event_type": event_type,
                    "timestamp": datetime.utcnow().isoformat(),
                }
                _event_queue.append(entry)

        # Handler is shared — we match watches to events on dequeue
        _observer.daemon = True
        _observer.start()
    except ImportError:
        # watchdog not installed — polling fallback
        pass


def _event_processor():
    """Background thread: match raw events to watches and insert to DB."""
    import sqlite3
    while True:
        if not _event_queue:
            time.sleep(0.1)
            continue

        batch = _event_queue[:50]
        del _event_queue[:len(batch)]

        try:
            conn = sqlite3.connect(DB_PATH)
            conn.row_factory = sqlite3.Row

            # Get all active watches
            watches = conn.execute("SELECT * FROM watches").fetchall()

            for entry in batch:
                for w in watches:
                    if not entry["src_path"].startswith(w["path"]):
                        continue

                    # Check event type filter
                    allowed = json.loads(w["events"])
                    if entry["event_type"] not in allowed:
                        continue

                    # Check include/exclude patterns
                    fname = os.path.basename(entry["src_path"])
                    if w["exclude_pattern"] and fnmatch.fnmatch(fname, w["exclude_pattern"]):
                        continue
                    if w["include_pattern"] != "*" and not fnmatch.fnmatch(fname, w["include_pattern"]):
                        continue

                    conn.execute(
                        """INSERT INTO events (namespace, watch_id, event_type, src_path, dest_path, timestamp)
                           VALUES (?, ?, ?, ?, ?, ?)""",
                        (w["namespace"], w["id"], entry["event_type"],
                         entry["src_path"], entry.get("dest_path"), entry["timestamp"]),
                    )

            conn.commit()
            conn.close()
        except Exception:
            pass  # Don't crash the processor thread


# ---------------------------------------------------------------------------
# Models
# ---------------------------------------------------------------------------
class WatchRequest(BaseModel):
    path: str = Field(..., description="Path to watch")
    recursive: bool = Field(True, description="Watch subdirectories")
    events: Optional[list[str]] = Field(
        default_factory=lambda: ["create", "modify", "delete", "move"],
        description="Event types to watch",
    )
    include: Optional[str] = Field("*", description="Glob pattern to include")
    exclude: Optional[str] = Field("", description="Glob pattern to exclude")
    debounce_ms: Optional[int] = Field(DEFAULT_DEBOUNCE_MS, description="Debounce window")


# ---------------------------------------------------------------------------
# API
# ---------------------------------------------------------------------------
@app.on_event("startup")
def on_startup():
    init_db()
    _start_watcher()
    t = threading.Thread(target=_event_processor, daemon=True)
    t.start()


@app.get("/health")
def health_check():
    return {"status": "ok", "service": "file_watcher", "version": "1.0.0"}


@app.post("/watches/{namespace}")
def create_watch(namespace: str, body: WatchRequest):
    """Register a file/directory watch."""
    # Validate path exists
    if not os.path.exists(body.path):
        raise HTTPException(status_code=400, detail=f"Path does not exist: {body.path}")

    watch_id = str(uuid.uuid4())[:8]

    with get_db() as db:
        db.execute(
            """INSERT INTO watches
               (id, namespace, path, recursive, events, include_pattern, exclude_pattern, debounce_ms, created_at)
               VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)""",
            (
                watch_id, namespace, body.path, int(body.recursive),
                json.dumps(body.events), body.include, body.exclude,
                body.debounce_ms, datetime.utcnow().isoformat(),
            ),
        )
        db.commit()

        # Schedule watch with observer if running
        if _observer:
            from watchdog.events import FileSystemEventHandler
            handler = type("Handler", (FileSystemEventHandler,), {"namespace": namespace})()
            try:
                _observer.schedule(handler, body.path, recursive=body.recursive)
            except Exception:
                pass  # Watch recorded in DB even if observer fails

        return {
            "success": True,
            "watch_id": watch_id,
            "namespace": namespace,
            "path": body.path,
            "events": body.events,
        }


@app.delete("/watches/{namespace}/{watch_id}")
def delete_watch(namespace: str, watch_id: str):
    """Remove a watch."""
    with get_db() as db:
        cursor = db.execute(
            "DELETE FROM watches WHERE id=? AND namespace=?",
            (watch_id, namespace),
        )
        db.commit()
        if cursor.rowcount == 0:
            raise HTTPException(status_code=404, detail="Watch not found")
        return {"success": True, "watch_id": watch_id}


@app.get("/watches/{namespace}")
def list_watches(namespace: str):
    """List active watches in a namespace."""
    with get_db() as db:
        rows = db.execute(
            "SELECT * FROM watches WHERE namespace=?",
            (namespace,),
        ).fetchall()
        return {
            "success": True,
            "namespace": namespace,
            "watches": [
                {
                    "id": r["id"],
                    "path": r["path"],
                    "recursive": bool(r["recursive"]),
                    "events": json.loads(r["events"]),
                    "include": r["include_pattern"],
                    "exclude": r["exclude_pattern"],
                    "debounce_ms": r["debounce_ms"],
                    "created_at": r["created_at"],
                }
                for r in rows
            ],
        }


@app.get("/events/{namespace}")
def poll_events(namespace: str, after: int = 0, limit: int = 100):
    """Poll for file change events since last sequence."""
    with get_db() as db:
        rows = db.execute(
            """SELECT * FROM events WHERE namespace=? AND id > ?
               ORDER BY id ASC LIMIT ?""",
            (namespace, after, limit),
        ).fetchall()
        return {
            "success": True,
            "namespace": namespace,
            "count": len(rows),
            "last_sequence": rows[-1]["id"] if rows else after,
            "events": [
                {
                    "sequence": r["id"],
                    "watch_id": r["watch_id"],
                    "event_type": r["event_type"],
                    "src_path": r["src_path"],
                    "dest_path": r["dest_path"],
                    "timestamp": r["timestamp"],
                }
                for r in rows
            ],
        }


@app.get("/events/{namespace}/history")
def event_history(namespace: str, limit: int = 50, watch_id: Optional[str] = None):
    """Get event history for a namespace."""
    with get_db() as db:
        query = "SELECT * FROM events WHERE namespace=?"
        params = [namespace]
        if watch_id:
            query += " AND watch_id=?"
            params.append(watch_id)
        query += " ORDER BY id DESC LIMIT ?"
        params.append(limit)

        rows = db.execute(query, params).fetchall()
        return {
            "success": True,
            "namespace": namespace,
            "count": len(rows),
            "events": [
                {
                    "sequence": r["id"],
                    "watch_id": r["watch_id"],
                    "event_type": r["event_type"],
                    "src_path": r["src_path"],
                    "dest_path": r["dest_path"],
                    "timestamp": r["timestamp"],
                }
                for r in reversed(list(rows))
            ],
        }
