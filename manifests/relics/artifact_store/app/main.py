"""
Artifact Store Relic — Persistent file/data storage with versioning and metadata.

Standard library relic for Cortex-Prime MK2.
Every agent produces artifacts — this is where they live.
"""

import json
import os
import hashlib
import time
import uuid
import mimetypes
from typing import Any, Optional
from datetime import datetime

from fastapi import FastAPI, HTTPException, UploadFile, File
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel, Field

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
DB_PATH = os.getenv("SQLITE_DB_PATH", "/data/artifacts.db")
STORAGE_PATH = os.getenv("STORAGE_PATH", "/data/files")
MAX_ARTIFACT_SIZE = int(os.getenv("MAX_ARTIFACT_SIZE", "52428800"))  # 50MB
MAX_VERSIONS = int(os.getenv("MAX_VERSIONS", "10"))
DEFAULT_TTL = int(os.getenv("DEFAULT_TTL", "0"))  # 0 = no expiration

# ---------------------------------------------------------------------------
# App
# ---------------------------------------------------------------------------
app = FastAPI(title="Artifact Store Relic", version="1.0.0")

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
    os.makedirs(os.path.dirname(DB_PATH), exist_ok=True)
    os.makedirs(STORAGE_PATH, exist_ok=True)
    import sqlite3
    with sqlite3.connect(DB_PATH) as conn:
        conn.execute("""
            CREATE TABLE IF NOT EXISTS artifacts (
                id TEXT PRIMARY KEY,
                namespace TEXT NOT NULL,
                key TEXT NOT NULL,
                version INTEGER NOT NULL DEFAULT 1,
                content_type TEXT NOT NULL DEFAULT 'application/json',
                size_bytes INTEGER NOT NULL DEFAULT 0,
                sha256 TEXT NOT NULL,
                tags TEXT NOT NULL DEFAULT '[]',
                storage_path TEXT NOT NULL,
                created_at TEXT NOT NULL,
                expires_at TEXT,
                is_latest INTEGER NOT NULL DEFAULT 1
            )
        """)
        conn.execute("""
            CREATE INDEX IF NOT EXISTS idx_namespace_key
            ON artifacts(namespace, key, version DESC)
        """)
        conn.execute("""
            CREATE INDEX IF NOT EXISTS idx_namespace_tags
            ON artifacts(namespace, tags)
        """)
        conn.execute("""
            CREATE INDEX IF NOT EXISTS idx_expires
            ON artifacts(expires_at) WHERE expires_at IS NOT NULL
        """)
        conn.commit()


def get_db():
    import sqlite3
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    return conn


# ---------------------------------------------------------------------------
# Models
# ---------------------------------------------------------------------------
class ArtifactPut(BaseModel):
    content: Any = Field(..., description="Artifact content (text, JSON, binary as base64)")
    content_type: Optional[str] = Field(None, description="MIME type (auto-detected if omitted)")
    tags: Optional[list[str]] = Field(default_factory=list, description="Metadata tags")
    ttl_seconds: Optional[int] = Field(None, description="Time-to-live in seconds (0 = no expiration)")


class SearchRequest(BaseModel):
    query: str = Field(..., description="Search term")
    namespace: Optional[str] = None
    content_type: Optional[str] = None
    tags: Optional[list[str]] = None
    limit: int = Field(50, ge=1, le=200)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
def _detect_content_type(content: Any, key: str) -> str:
    """Auto-detect content type from content or key extension."""
    if isinstance(content, dict) or isinstance(content, list):
        return "application/json"
    if isinstance(content, str):
        # Check if it looks like base64 binary
        if len(content) > 100 and content.endswith("=="):
            return "application/octet-stream"
        # Try extension-based detection
        guessed = mimetypes.guess_type(key)[0]
        return guessed or "text/plain"
    return "application/octet-stream"


def _store_content(namespace: str, key: str, version: int, content: Any) -> str:
    """Write content to filesystem, return storage path."""
    dir_path = os.path.join(STORAGE_PATH, namespace)
    os.makedirs(dir_path, exist_ok=True)
    filename = f"{key.replace('/', '_')}_v{version}"
    file_path = os.path.join(dir_path, filename)

    if isinstance(content, (dict, list)):
        data = json.dumps(content, indent=2).encode()
    elif isinstance(content, str):
        data = content.encode()
    elif isinstance(content, bytes):
        data = content
    else:
        data = str(content).encode()

    if len(data) > MAX_ARTIFACT_SIZE:
        raise ValueError(f"Artifact size {len(data)} exceeds limit {MAX_ARTIFACT_SIZE}")

    with open(file_path, "wb") as f:
        f.write(data)

    return file_path


def _read_content(storage_path: str) -> Any:
    """Read content from filesystem."""
    if not os.path.exists(storage_path):
        return None
    with open(storage_path, "rb") as f:
        data = f.read()
    # Try JSON first
    try:
        return json.loads(data.decode())
    except (json.JSONDecodeError, UnicodeDecodeError):
        pass
    # Try text
    try:
        return data.decode()
    except UnicodeDecodeError:
        pass
    # Return base64 for binary
    import base64
    return base64.b64encode(data).decode()


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


# ---------------------------------------------------------------------------
# API Endpoints
# ---------------------------------------------------------------------------
@app.on_event("startup")
def on_startup():
    init_db()


@app.get("/health")
def health_check():
    return {"status": "ok", "service": "artifact_store", "version": "1.0.0"}


@app.get("/system/capabilities")
def get_capabilities():
    return {
        "relic_name": "artifact_store",
        "version": "1.0.0",
        "operations": [
            {"name": "put", "description": "Store an artifact (versioned)"},
            {"name": "get", "description": "Retrieve artifact by key and optional version"},
            {"name": "list", "description": "List artifacts in a namespace"},
            {"name": "delete", "description": "Delete artifact(s)"},
            {"name": "search", "description": "Search by content, tags, or metadata"},
            {"name": "stats", "description": "Storage statistics"},
        ],
    }


@app.post("/artifacts/{namespace}/{key}")
def put_artifact(namespace: str, key: str, body: ArtifactPut):
    """Store an artifact. Creates a new version if key already exists."""
    content_type = body.content_type or _detect_content_type(body.content, key)

    # Get current version
    db = get_db()
    try:
        row = db.execute(
            "SELECT MAX(version) as v FROM artifacts WHERE namespace=? AND key=?",
            (namespace, key),
        ).fetchone()
        current_version = row["v"] or 0
        new_version = current_version + 1

        # Mark old latest as not latest
        db.execute(
            "UPDATE artifacts SET is_latest=0 WHERE namespace=? AND key=? AND is_latest=1",
            (namespace, key),
        )

        # Store content to filesystem
        storage_path = _store_content(namespace, key, new_version, body.content)

        # Compute hash
        with open(storage_path, "rb") as f:
            sha = _sha256(f.read())

        # Calculate size
        size = os.path.getsize(storage_path)

        # Calculate expiration
        expires_at = None
        ttl = body.ttl_seconds if body.ttl_seconds is not None else DEFAULT_TTL
        if ttl > 0:
            expires_at = datetime.utcfromtimestamp(time.time() + ttl).isoformat()

        # Insert metadata
        artifact_id = str(uuid.uuid4())
        db.execute(
            """INSERT INTO artifacts
               (id, namespace, key, version, content_type, size_bytes, sha256,
                tags, storage_path, created_at, expires_at, is_latest)
               VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 1)""",
            (
                artifact_id, namespace, key, new_version, content_type, size, sha,
                json.dumps(body.tags), storage_path,
                datetime.utcnow().isoformat(), expires_at,
            ),
        )
        db.commit()

        # Prune old versions beyond MAX_VERSIONS
        if MAX_VERSIONS > 0 and new_version > MAX_VERSIONS:
            old = db.execute(
                """SELECT id, storage_path FROM artifacts
                   WHERE namespace=? AND key=? AND version <= ?
                   ORDER BY version ASC""",
                (namespace, key, new_version - MAX_VERSIONS),
            ).fetchall()
            for r in old:
                if os.path.exists(r["storage_path"]):
                    os.remove(r["storage_path"])
                db.execute("DELETE FROM artifacts WHERE id=?", (r["id"],))
            db.commit()

        return {
            "success": True,
            "id": artifact_id,
            "namespace": namespace,
            "key": key,
            "version": new_version,
            "content_type": content_type,
            "size_bytes": size,
            "sha256": sha,
        }
    except ValueError as e:
        raise HTTPException(status_code=413, detail=str(e))
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))
    finally:
        db.close()


@app.get("/artifacts/{namespace}/{key}")
def get_artifact(namespace: str, key: str, version: Optional[int] = None):
    """Retrieve an artifact. Returns latest version if version omitted."""
    db = get_db()
    try:
        if version:
            row = db.execute(
                """SELECT * FROM artifacts
                   WHERE namespace=? AND key=? AND version=?""",
                (namespace, key, version),
            ).fetchone()
        else:
            row = db.execute(
                """SELECT * FROM artifacts
                   WHERE namespace=? AND key=? AND is_latest=1""",
                (namespace, key),
            ).fetchone()

        if not row:
            raise HTTPException(status_code=404, detail="Artifact not found")

        content = _read_content(row["storage_path"])
        return {
            "success": True,
            "id": row["id"],
            "namespace": row["namespace"],
            "key": row["key"],
            "version": row["version"],
            "content_type": row["content_type"],
            "size_bytes": row["size_bytes"],
            "sha256": row["sha256"],
            "tags": json.loads(row["tags"]),
            "created_at": row["created_at"],
            "content": content,
        }
    finally:
        db.close()


@app.get("/artifacts/{namespace}")
def list_artifacts(
    namespace: str,
    prefix: Optional[str] = None,
    tag: Optional[str] = None,
    limit: int = 50,
):
    """List artifacts in a namespace. Supports prefix and tag filtering."""
    db = get_db()
    try:
        query = "SELECT * FROM artifacts WHERE namespace=? AND is_latest=1"
        params = [namespace]

        if prefix:
            query += " AND key LIKE ?"
            params.append(f"{prefix}%")

        if tag:
            query += " AND tags LIKE ?"
            params.append(f'%"{tag}"%')

        query += " ORDER BY created_at DESC LIMIT ?"
        params.append(limit)

        rows = db.execute(query, params).fetchall()
        return {
            "success": True,
            "namespace": namespace,
            "count": len(rows),
            "artifacts": [
                {
                    "id": r["id"],
                    "key": r["key"],
                    "version": r["version"],
                    "content_type": r["content_type"],
                    "size_bytes": r["size_bytes"],
                    "tags": json.loads(r["tags"]),
                    "created_at": r["created_at"],
                }
                for r in rows
            ],
        }
    finally:
        db.close()


@app.delete("/artifacts/{namespace}/{key}")
def delete_artifact(namespace: str, key: str, version: Optional[int] = None):
    """Delete artifact(s). Deletes all versions if version omitted."""
    db = get_db()
    try:
        if version:
            row = db.execute(
                "SELECT storage_path FROM artifacts WHERE namespace=? AND key=? AND version=?",
                (namespace, key, version),
            ).fetchone()
            if not row:
                raise HTTPException(status_code=404, detail="Artifact version not found")
            if os.path.exists(row["storage_path"]):
                os.remove(row["storage_path"])
            db.execute(
                "DELETE FROM artifacts WHERE namespace=? AND key=? AND version=?",
                (namespace, key, version),
            )
        else:
            rows = db.execute(
                "SELECT storage_path FROM artifacts WHERE namespace=? AND key=?",
                (namespace, key),
            ).fetchall()
            for r in rows:
                if os.path.exists(r["storage_path"]):
                    os.remove(r["storage_path"])
            db.execute(
                "DELETE FROM artifacts WHERE namespace=? AND key=?",
                (namespace, key),
            )

        db.commit()
        return {"success": True, "namespace": namespace, "key": key, "version": version or "all"}
    finally:
        db.close()


@app.post("/search")
def search_artifacts(body: SearchRequest):
    """Search artifacts by content, tags, or metadata."""
    db = get_db()
    try:
        query = "SELECT * FROM artifacts WHERE is_latest=1"
        params = []

        if body.namespace:
            query += " AND namespace=?"
            params.append(body.namespace)

        if body.content_type:
            query += " AND content_type=?"
            params.append(body.content_type)

        if body.tags:
            for tag in body.tags:
                query += " AND tags LIKE ?"
                params.append(f'%"{tag}"%')

        if body.query:
            # Search in key names
            query += " AND (key LIKE ? OR tags LIKE ?)"
            params.extend([f"%{body.query}%", f'%"{body.query}"%'])

        query += " ORDER BY created_at DESC LIMIT ?"
        params.append(body.limit)

        rows = db.execute(query, params).fetchall()
        return {
            "success": True,
            "count": len(rows),
            "artifacts": [
                {
                    "id": r["id"],
                    "namespace": r["namespace"],
                    "key": r["key"],
                    "version": r["version"],
                    "content_type": r["content_type"],
                    "size_bytes": r["size_bytes"],
                    "tags": json.loads(r["tags"]),
                    "created_at": r["created_at"],
                }
                for r in rows
            ],
        }
    finally:
        db.close()


@app.get("/stats")
def storage_stats():
    """Storage statistics."""
    db = get_db()
    try:
        total = db.execute("SELECT COUNT(*) as c FROM artifacts").fetchone()["c"]
        latest = db.execute("SELECT COUNT(*) as c FROM artifacts WHERE is_latest=1").fetchone()["c"]
        size = db.execute("SELECT COALESCE(SUM(size_bytes), 0) as s FROM artifacts").fetchone()["s"]
        namespaces = db.execute(
            "SELECT namespace, COUNT(*) as c FROM artifacts WHERE is_latest=1 GROUP BY namespace"
        ).fetchall()

        return {
            "success": True,
            "total_versions": total,
            "latest_artifacts": latest,
            "total_bytes": size,
            "total_bytes_human": f"{size / 1024 / 1024:.1f} MB" if size > 1048576 else f"{size / 1024:.1f} KB",
            "namespaces": {r["namespace"]: r["c"] for r in namespaces},
        }
    finally:
        db.close()
