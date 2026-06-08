"""
Event Bus Relic — Pub/sub messaging for async agent-to-agent communication.

Standard library relic for Cortex-Prime MK2.
Decouples agents via channels with topic routing and message history.
"""

import json
import os
import time
import uuid
import threading
from typing import Optional, Any
from datetime import datetime
from contextlib import contextmanager

from fastapi import FastAPI, HTTPException, Request
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel, Field

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
DB_PATH = os.getenv("SQLITE_DB_PATH", "/data/eventbus.db")
MAX_HISTORY = int(os.getenv("MAX_HISTORY", "1000"))
DEFAULT_POLL_TIMEOUT = int(os.getenv("DEFAULT_POLL_TIMEOUT", "30"))

# ---------------------------------------------------------------------------
# App
# ---------------------------------------------------------------------------
app = FastAPI(title="Event Bus Relic", version="1.0.0")

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
            CREATE TABLE IF NOT EXISTS channels (
                namespace TEXT NOT NULL,
                name TEXT NOT NULL,
                created_at TEXT NOT NULL,
                PRIMARY KEY (namespace, name)
            )
        """)
        conn.execute("""
            CREATE TABLE IF NOT EXISTS messages (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                namespace TEXT NOT NULL,
                channel TEXT NOT NULL,
                topic TEXT NOT NULL DEFAULT '',
                publisher TEXT NOT NULL,
                payload TEXT NOT NULL,
                timestamp TEXT NOT NULL
            )
        """)
        conn.execute("""
            CREATE INDEX IF NOT EXISTS idx_messages_channel
            ON messages(namespace, channel, id DESC)
        """)
        conn.execute("""
            CREATE TABLE IF NOT EXISTS subscriptions (
                namespace TEXT NOT NULL,
                channel TEXT NOT NULL,
                subscriber TEXT NOT NULL,
                topic_filter TEXT NOT NULL DEFAULT '*',
                last_sequence INTEGER NOT NULL DEFAULT 0,
                created_at TEXT NOT NULL,
                PRIMARY KEY (namespace, channel, subscriber)
            )
        """)
        conn.commit()


# ---------------------------------------------------------------------------
# Models
# ---------------------------------------------------------------------------
class PublishRequest(BaseModel):
    message: Any = Field(..., description="Message payload")
    topic: Optional[str] = Field("", description="Topic tag for routing")

class SubscribeRequest(BaseModel):
    subscriber: str = Field(..., description="Agent name subscribing")
    topic_filter: Optional[str] = Field("*", description="Topic glob filter")
    from_sequence: Optional[int] = Field(0, description="Resume from sequence ID")


# ---------------------------------------------------------------------------
# API
# ---------------------------------------------------------------------------
@app.on_event("startup")
def on_startup():
    init_db()


@app.get("/health")
def health_check():
    return {"status": "ok", "service": "event_bus", "version": "1.0.0"}


@app.post("/channels/{namespace}/{channel}")
def publish(namespace: str, channel: str, body: PublishRequest, request: Request):
    """Publish a message to a channel."""
    publisher = request.headers.get("X-Caller-Agent", "unknown")
    with get_db() as db:
        # Ensure channel exists
        db.execute(
            "INSERT OR IGNORE INTO channels (namespace, name, created_at) VALUES (?, ?, ?)",
            (namespace, channel, datetime.utcnow().isoformat()),
        )
        cursor = db.execute(
            """INSERT INTO messages (namespace, channel, topic, publisher, payload, timestamp)
               VALUES (?, ?, ?, ?, ?, ?)""",
            (namespace, channel, body.topic, publisher,
             json.dumps(body.message), datetime.utcnow().isoformat()),
        )
        seq = cursor.lastrowid
        db.commit()

        # Prune old messages beyond MAX_HISTORY per channel
        count = db.execute(
            "SELECT COUNT(*) as c FROM messages WHERE namespace=? AND channel=?",
            (namespace, channel),
        ).fetchone()["c"]
        if count > MAX_HISTORY:
            db.execute(
                """DELETE FROM messages WHERE namespace=? AND channel=?
                   AND id NOT IN (
                       SELECT id FROM messages WHERE namespace=? AND channel=?
                       ORDER BY id DESC LIMIT ?
                   )""",
                (namespace, channel, namespace, channel, MAX_HISTORY),
            )
            db.commit()

        return {"success": True, "sequence": seq, "channel": channel, "topic": body.topic}


@app.post("/channels/{namespace}/{channel}/subscribe")
def subscribe(namespace: str, channel: str, body: SubscribeRequest):
    """Subscribe to a channel. Returns message history from from_sequence."""
    with get_db() as db:
        # Upsert subscription
        db.execute(
            """INSERT INTO subscriptions (namespace, channel, subscriber, topic_filter, last_sequence, created_at)
               VALUES (?, ?, ?, ?, ?, ?)
               ON CONFLICT(namespace, channel, subscriber) DO UPDATE SET
                   topic_filter=excluded.topic_filter,
                   last_sequence=excluded.last_sequence""",
            (namespace, channel, body.subscriber, body.topic_filter,
             body.from_sequence, datetime.utcnow().isoformat()),
        )
        db.commit()

        # Return history from requested sequence
        rows = db.execute(
            """SELECT id, topic, publisher, payload, timestamp FROM messages
               WHERE namespace=? AND channel=? AND id > ?
               ORDER BY id ASC LIMIT 100""",
            (namespace, channel, body.from_sequence),
        ).fetchall()

        return {
            "success": True,
            "channel": channel,
            "subscriber": body.subscriber,
            "messages": [
                {
                    "sequence": r["id"],
                    "topic": r["topic"],
                    "publisher": r["publisher"],
                    "payload": json.loads(r["payload"]),
                    "timestamp": r["timestamp"],
                }
                for r in rows
            ],
        }


@app.get("/channels/{namespace}/{channel}/poll")
def poll(namespace: str, channel: str, subscriber: str, timeout: int = DEFAULT_POLL_TIMEOUT):
    """Long-poll for new messages since subscriber's last_sequence."""
    with get_db() as db:
        sub = db.execute(
            "SELECT last_sequence FROM subscriptions WHERE namespace=? AND channel=? AND subscriber=?",
            (namespace, channel, subscriber),
        ).fetchone()
        if not sub:
            raise HTTPException(status_code=404, detail="Not subscribed")

        last_seq = sub["last_sequence"]

        # Check immediately
        rows = db.execute(
            """SELECT id, topic, publisher, payload, timestamp FROM messages
               WHERE namespace=? AND channel=? AND id > ?
               ORDER BY id ASC LIMIT 100""",
            (namespace, channel, last_seq),
        ).fetchall()

        if rows:
            # Update last_sequence
            max_seq = max(r["id"] for r in rows)
            db.execute(
                "UPDATE subscriptions SET last_sequence=? WHERE namespace=? AND channel=? AND subscriber=?",
                (max_seq, namespace, channel, subscriber),
            )
            db.commit()
            return {
                "success": True,
                "messages": [
                    {
                        "sequence": r["id"],
                        "topic": r["topic"],
                        "publisher": r["publisher"],
                        "payload": json.loads(r["payload"]),
                        "timestamp": r["timestamp"],
                    }
                    for r in rows
                ],
            }

        # No messages — short sleep loop (not true long-poll, but good enough for SQLite)
        deadline = time.time() + min(timeout, 30)
        while time.time() < deadline:
            time.sleep(0.5)
            rows = db.execute(
                """SELECT id, topic, publisher, payload, timestamp FROM messages
                   WHERE namespace=? AND channel=? AND id > ?
                   ORDER BY id ASC LIMIT 100""",
                (namespace, channel, last_seq),
            ).fetchall()
            if rows:
                max_seq = max(r["id"] for r in rows)
                db.execute(
                    "UPDATE subscriptions SET last_sequence=? WHERE namespace=? AND channel=? AND subscriber=?",
                    (max_seq, namespace, channel, subscriber),
                )
                db.commit()
                return {
                    "success": True,
                    "messages": [
                        {
                            "sequence": r["id"],
                            "topic": r["topic"],
                            "publisher": r["publisher"],
                            "payload": json.loads(r["payload"]),
                            "timestamp": r["timestamp"],
                        }
                        for r in rows
                    ],
                }

        return {"success": True, "messages": []}


@app.get("/channels/{namespace}/{channel}/history")
def history(namespace: str, channel: str, limit: int = 50, from_sequence: int = 0):
    """Get message history for a channel."""
    with get_db() as db:
        rows = db.execute(
            """SELECT id, topic, publisher, payload, timestamp FROM messages
               WHERE namespace=? AND channel=? AND id > ?
               ORDER BY id DESC LIMIT ?""",
            (namespace, channel, from_sequence, limit),
        ).fetchall()
        return {
            "success": True,
            "channel": channel,
            "count": len(rows),
            "messages": [
                {
                    "sequence": r["id"],
                    "topic": r["topic"],
                    "publisher": r["publisher"],
                    "payload": json.loads(r["payload"]),
                    "timestamp": r["timestamp"],
                }
                for r in reversed(rows)
            ],
        }


@app.get("/channels/{namespace}")
def list_channels(namespace: str):
    """List channels in a namespace."""
    with get_db() as db:
        rows = db.execute(
            "SELECT name, created_at FROM channels WHERE namespace=?",
            (namespace,),
        ).fetchall()
        return {
            "success": True,
            "namespace": namespace,
            "channels": [{"name": r["name"], "created_at": r["created_at"]} for r in rows],
        }


@app.delete("/channels/{namespace}/{channel}/subscribe")
def unsubscribe(namespace: str, channel: str, subscriber: str):
    """Unsubscribe from a channel."""
    with get_db() as db:
        cursor = db.execute(
            "DELETE FROM subscriptions WHERE namespace=? AND channel=? AND subscriber=?",
            (namespace, channel, subscriber),
        )
        db.commit()
        return {"success": True, "unsubscribed": cursor.rowcount > 0}
