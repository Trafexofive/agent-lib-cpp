"""Build artifact cache — stores compiled artifacts keyed by source hash."""
import os
import hashlib
import sqlite3
from fastapi import FastAPI, HTTPException
from pydantic import BaseModel
from pathlib import Path

app = FastAPI(title="Build Cache")
DB_PATH = os.environ.get("SQLITE_DB_PATH", "/tmp/build_cache.db")
ARTIFACT_DIR = Path(os.environ.get("ARTIFACT_DIR", "/tmp/build_artifacts"))
ARTIFACT_DIR.mkdir(parents=True, exist_ok=True)


def get_db():
    conn = sqlite3.connect(DB_PATH)
    conn.execute("""
        CREATE TABLE IF NOT EXISTS cache (
            key TEXT PRIMARY KEY,
            artifact_path TEXT NOT NULL,
            source_hash TEXT NOT NULL,
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        )
    """)
    conn.commit()
    return conn


class PutRequest(BaseModel):
    key: str
    artifact_path: str


@app.get("/health")
async def health():
    return {"status": "ok"}


@app.post("/put")
async def put_artifact(req: PutRequest):
    if not os.path.exists(req.artifact_path):
        raise HTTPException(status_code=404, detail="artifact not found")

    # Hash the artifact file
    h = hashlib.sha256()
    with open(req.artifact_path, "rb") as f:
        for chunk in iter(lambda: f.read(8192), b""):
            h.update(chunk)

    source_hash = h.hexdigest()
    # Copy artifact to cache dir
    cached_path = ARTIFACT_DIR / f"{req.key}_{source_hash[:8]}"
    with open(req.artifact_path, "rb") as src, open(cached_path, "wb") as dst:
        dst.write(src.read())

    conn = get_db()
    conn.execute(
        "INSERT OR REPLACE INTO cache (key, artifact_path, source_hash) VALUES (?, ?, ?)",
        (req.key, str(cached_path), source_hash),
    )
    conn.commit()
    conn.close()
    return {"key": req.key, "hash": source_hash[:16], "cached": str(cached_path)}


@app.get("/get/{key}")
async def get_artifact(key: str):
    conn = get_db()
    row = conn.execute("SELECT artifact_path, source_hash FROM cache WHERE key = ?", (key,)).fetchone()
    conn.close()
    if not row:
        raise HTTPException(status_code=404, detail="not cached")
    return {"key": key, "artifact_path": row[0], "hash": row[1]}


@app.delete("/invalidate/{key}")
async def invalidate(key: str):
    conn = get_db()
    row = conn.execute("SELECT artifact_path FROM cache WHERE key = ?", (key,)).fetchone()
    if row and os.path.exists(row[0]):
        os.remove(row[0])
    conn.execute("DELETE FROM cache WHERE key = ?", (key,))
    conn.commit()
    conn.close()
    return {"invalidated": key}
