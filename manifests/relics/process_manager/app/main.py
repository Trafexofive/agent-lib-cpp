"""
Process Manager Relic — Track and manage long-running subprocesses across agents.

Standard library relic for Cortex-Prime MK2.
Agents register or spawn processes, then track/kill/read logs.
"""

import json
import os
import signal
import subprocess
import threading
import time
import uuid
from typing import Optional
from datetime import datetime

from fastapi import FastAPI, HTTPException, Request
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel, Field

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
DB_PATH = os.getenv("SQLITE_DB_PATH", "/data/processes.db")
LOG_DIR = os.getenv("LOG_DIR", "/data/logs")
MAX_LOG_SIZE = int(os.getenv("MAX_LOG_SIZE", "10485760"))  # 10MB

# ---------------------------------------------------------------------------
# App
# ---------------------------------------------------------------------------
app = FastAPI(title="Process Manager Relic", version="1.0.0")

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)

# ---------------------------------------------------------------------------
# Database
# ---------------------------------------------------------------------------
def init_db():
    import sqlite3
    os.makedirs(os.path.dirname(DB_PATH), exist_ok=True)
    os.makedirs(LOG_DIR, exist_ok=True)
    with sqlite3.connect(DB_PATH) as conn:
        conn.execute("""
            CREATE TABLE IF NOT EXISTS processes (
                id TEXT PRIMARY KEY,
                namespace TEXT NOT NULL,
                pid INTEGER,
                label TEXT NOT NULL DEFAULT '',
                command TEXT NOT NULL DEFAULT '',
                working_dir TEXT NOT NULL DEFAULT '',
                status TEXT NOT NULL DEFAULT 'running',
                exit_code INTEGER,
                metadata TEXT NOT NULL DEFAULT '{}',
                stdout_path TEXT,
                stderr_path TEXT,
                created_at TEXT NOT NULL,
                updated_at TEXT NOT NULL
            )
        """)
        conn.execute("""
            CREATE INDEX IF NOT EXISTS idx_namespace
            ON processes(namespace, status)
        """)
        conn.commit()


def get_db():
    import sqlite3
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    return conn


# ---------------------------------------------------------------------------
# Process monitoring thread
# ---------------------------------------------------------------------------
_monitored = {}  # process_id -> subprocess.Popen

def _monitor_process(proc_id: str, proc: subprocess.Popen, db_path: str):
    """Background thread: wait for process exit, update status."""
    proc.wait()
    import sqlite3
    conn = sqlite3.connect(db_path)
    conn.execute(
        "UPDATE processes SET status=?, exit_code=?, updated_at=? WHERE id=?",
        ("exited", proc.returncode, datetime.utcnow().isoformat(), proc_id),
    )
    conn.commit()
    conn.close()


# ---------------------------------------------------------------------------
# Models
# ---------------------------------------------------------------------------
class RegisterRequest(BaseModel):
    pid: int = Field(..., description="Process ID to track")
    label: Optional[str] = ""
    working_dir: Optional[str] = ""
    metadata: Optional[dict] = {}

class SpawnRequest(BaseModel):
    command: str = Field(..., description="Shell command to spawn")
    working_dir: Optional[str] = ""
    env: Optional[dict] = None
    label: Optional[str] = ""


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
def _is_pid_running(pid: int) -> bool:
    """Check if a PID is still alive."""
    try:
        os.kill(pid, 0)
        return True
    except (OSError, ProcessLookupError):
        return False


def _read_log(path: str, tail: int = 100) -> str:
    """Read last N lines from a log file."""
    if not path or not os.path.exists(path):
        return ""
    with open(path, "r") as f:
        lines = f.readlines()
    return "".join(lines[-tail:])


# ---------------------------------------------------------------------------
# API
# ---------------------------------------------------------------------------
@app.on_event("startup")
def on_startup():
    init_db()


@app.get("/health")
def health_check():
    return {"status": "ok", "service": "process_manager", "version": "1.0.0"}


@app.post("/processes/{namespace}")
def register(namespace: str, body: RegisterRequest):
    """Register an existing PID for tracking."""
    proc_id = str(uuid.uuid4())[:8]
    stdout_path = os.path.join(LOG_DIR, f"{proc_id}.stdout")
    stderr_path = os.path.join(LOG_DIR, f"{proc_id}.stderr")

    db = get_db()
    try:
        status = "running" if _is_pid_running(body.pid) else "exited"
        db.execute(
            """INSERT INTO processes
               (id, namespace, pid, label, command, working_dir, status, metadata,
                stdout_path, stderr_path, created_at, updated_at)
               VALUES (?, ?, ?, ?, 'external', ?, ?, ?, ?, ?, ?, ?)""",
            (
                proc_id, namespace, body.pid, body.label, body.working_dir,
                status, json.dumps(body.metadata),
                stdout_path, stderr_path,
                datetime.utcnow().isoformat(), datetime.utcnow().isoformat(),
            ),
        )
        db.commit()
        return {"success": True, "process_id": proc_id, "pid": body.pid, "status": status}
    finally:
        db.close()


@app.post("/processes/{namespace}/spawn")
def spawn(namespace: str, body: SpawnRequest):
    """Spawn a new process and track it."""
    proc_id = str(uuid.uuid4())[:8]
    stdout_path = os.path.join(LOG_DIR, f"{proc_id}.stdout")
    stderr_path = os.path.join(LOG_DIR, f"{proc_id}.stderr")

    try:
        stdout_f = open(stdout_path, "w")
        stderr_f = open(stderr_path, "w")

        env = os.environ.copy()
        if body.env:
            env.update(body.env)

        proc = subprocess.Popen(
            body.command,
            shell=True,
            cwd=body.working_dir or None,
            stdout=stdout_f,
            stderr=stderr_f,
            env=env,
        )

        _monitored[proc_id] = proc

        # Start monitor thread
        t = threading.Thread(
            target=_monitor_process, args=(proc_id, proc, DB_PATH), daemon=True
        )
        t.start()

        db = get_db()
        db.execute(
            """INSERT INTO processes
               (id, namespace, pid, label, command, working_dir, status, metadata,
                stdout_path, stderr_path, created_at, updated_at)
               VALUES (?, ?, ?, ?, ?, ?, 'running', '{}', ?, ?, ?, ?)""",
            (
                proc_id, namespace, proc.pid, body.label,
                body.command, body.working_dir or "",
                stdout_path, stderr_path,
                datetime.utcnow().isoformat(), datetime.utcnow().isoformat(),
            ),
        )
        db.commit()
        db.close()

        return {"success": True, "process_id": proc_id, "pid": proc.pid, "status": "running"}
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))


@app.get("/processes/{namespace}")
def list_processes(namespace: str, status: Optional[str] = None):
    """List tracked processes in a namespace."""
    db = get_db()
    try:
        query = "SELECT * FROM processes WHERE namespace=?"
        params = [namespace]
        if status:
            query += " AND status=?"
            params.append(status)
        query += " ORDER BY created_at DESC"
        rows = db.execute(query, params).fetchall()
        return {
            "success": True,
            "namespace": namespace,
            "count": len(rows),
            "processes": [
                {
                    "id": r["id"],
                    "pid": r["pid"],
                    "label": r["label"],
                    "command": r["command"],
                    "status": r["status"],
                    "exit_code": r["exit_code"],
                    "created_at": r["created_at"],
                }
                for r in rows
            ],
        }
    finally:
        db.close()


@app.get("/processes/{namespace}/{process_id}")
def get_status(namespace: str, process_id: str):
    """Get process status."""
    db = get_db()
    try:
        row = db.execute(
            "SELECT * FROM processes WHERE id=? AND namespace=?",
            (process_id, namespace),
        ).fetchone()
        if not row:
            raise HTTPException(status_code=404, detail="Process not found")

        # Refresh status if we think it's running
        status = row["status"]
        exit_code = row["exit_code"]
        if status == "running" and row["pid"]:
            if not _is_pid_running(row["pid"]):
                # Process died without us noticing
                status = "exited"
                db.execute(
                    "UPDATE processes SET status='exited', updated_at=? WHERE id=?",
                    (datetime.utcnow().isoformat(), process_id),
                )
                db.commit()

        return {
            "success": True,
            "id": row["id"],
            "pid": row["pid"],
            "label": row["label"],
            "command": row["command"],
            "status": status,
            "exit_code": exit_code,
            "metadata": json.loads(row["metadata"]),
            "created_at": row["created_at"],
            "updated_at": row["updated_at"],
        }
    finally:
        db.close()


@app.get("/processes/{namespace}/{process_id}/logs")
def get_logs(namespace: str, process_id: str, stream: str = "both", tail: int = 100):
    """Get process logs."""
    db = get_db()
    try:
        row = db.execute(
            "SELECT stdout_path, stderr_path FROM processes WHERE id=? AND namespace=?",
            (process_id, namespace),
        ).fetchone()
        if not row:
            raise HTTPException(status_code=404, detail="Process not found")

        result = {"success": True, "process_id": process_id}
        if stream in ("stdout", "both"):
            result["stdout"] = _read_log(row["stdout_path"], tail)
        if stream in ("stderr", "both"):
            result["stderr"] = _read_log(row["stderr_path"], tail)
        return result
    finally:
        db.close()


@app.delete("/processes/{namespace}/{process_id}")
def kill(namespace: str, process_id: str, sig: str = "SIGTERM"):
    """Kill a tracked process."""
    db = get_db()
    try:
        row = db.execute(
            "SELECT pid, status FROM processes WHERE id=? AND namespace=?",
            (process_id, namespace),
        ).fetchone()
        if not row:
            raise HTTPException(status_code=404, detail="Process not found")
        if row["status"] != "running":
            return {"success": True, "status": "already_exited", "exit_code": None}

        pid = row["pid"]
        sig_num = signal.SIGKILL if sig == "SIGKILL" else signal.SIGTERM
        try:
            os.kill(pid, sig_num)
            status = "killed"
        except ProcessLookupError:
            status = "exited"
        except PermissionError:
            raise HTTPException(status_code=403, detail="Permission denied")

        db.execute(
            "UPDATE processes SET status=?, updated_at=? WHERE id=?",
            (status, datetime.utcnow().isoformat(), process_id),
        )
        db.commit()
        return {"success": True, "process_id": process_id, "status": status}
    finally:
        db.close()
