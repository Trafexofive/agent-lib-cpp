"""
Secret Store Relic — Encrypted key-value storage for sensitive data.

Standard library relic for Cortex-Prime MK2.
Secrets NEVER appear in logs, tool output, or history.
"""

import json
import os
import hashlib
import base64
import time
import uuid
from typing import Optional
from datetime import datetime

from fastapi import FastAPI, HTTPException, Header, Request
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel, Field

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
DB_PATH = os.getenv("SQLITE_DB_PATH", "/data/secrets.db")
MASTER_KEY_PATH = os.getenv("MASTER_KEY_PATH", "/run/secrets/master_key")

# ---------------------------------------------------------------------------
# Encryption (AES-256-GCM via cryptography lib)
# ---------------------------------------------------------------------------
def _get_master_key() -> bytes:
    """Load or generate the master encryption key."""
    if os.path.exists(MASTER_KEY_PATH):
        with open(MASTER_KEY_PATH, "rb") as f:
            key = f.read().strip()
            if key:
                return key

    # Fallback: generate from env or create a stable key
    # In production, MASTER_KEY_PATH must be set via Docker secrets
    key_env = os.getenv("SECRET_STORE_MASTER_KEY")
    if key_env:
        return key_env.encode().ljust(32, b'\0')[:32]

    # Generate deterministic key from hostname (NOT production-safe, but works for dev)
    import socket
    raw = f"cortex-secret-store-{socket.gethostname()}".encode()
    return hashlib.sha256(raw).digest()[:32]


def _encrypt(plaintext: str, key: bytes) -> str:
    """AES-256-GCM encrypt. Returns base64(nonce + tag + ciphertext)."""
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    nonce = os.urandom(12)
    aesgcm = AESGCM(key)
    ct = aesgcm.encrypt(nonce, plaintext.encode(), None)
    return base64.b64encode(nonce + ct).decode()


def _decrypt(encrypted: str, key: bytes) -> str:
    """AES-256-GCM decrypt. Input is base64(nonce + tag + ciphertext)."""
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    raw = base64.b64decode(encrypted)
    nonce = raw[:12]
    ct = raw[12:]
    aesgcm = AESGCM(key)
    return aesgcm.decrypt(nonce, ct, None).decode()


MASTER_KEY = _get_master_key()

# ---------------------------------------------------------------------------
# App
# ---------------------------------------------------------------------------
app = FastAPI(title="Secret Store Relic", version="1.0.0")

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
    with sqlite3.connect(DB_PATH) as conn:
        conn.execute("""
            CREATE TABLE IF NOT EXISTS secrets (
                id TEXT PRIMARY KEY,
                namespace TEXT NOT NULL,
                key TEXT NOT NULL,
                encrypted_value TEXT NOT NULL,
                description TEXT,
                created_at TEXT NOT NULL,
                updated_at TEXT NOT NULL,
                UNIQUE(namespace, key)
            )
        """)
        conn.execute("""
            CREATE TABLE IF NOT EXISTS grants (
                id TEXT PRIMARY KEY,
                namespace TEXT NOT NULL,
                key TEXT NOT NULL,
                target_namespace TEXT NOT NULL,
                token TEXT NOT NULL,
                expires_at TEXT,
                created_at TEXT NOT NULL,
                UNIQUE(namespace, key, target_namespace)
            )
        """)
        conn.execute("""
            CREATE TABLE IF NOT EXISTS audit_log (
                id TEXT PRIMARY KEY,
                namespace TEXT NOT NULL,
                key TEXT NOT NULL,
                action TEXT NOT NULL,
                caller TEXT,
                timestamp TEXT NOT NULL
            )
        """)
        conn.execute("""
            CREATE INDEX IF NOT EXISTS idx_audit_namespace
            ON audit_log(namespace, timestamp DESC)
        """)
        conn.commit()


def get_db():
    import sqlite3
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    return conn


def _log_audit(db, namespace: str, key: str, action: str, caller: str = ""):
    """Write-only audit log. Never logs values."""
    db.execute(
        "INSERT INTO audit_log (id, namespace, key, action, caller, timestamp) VALUES (?, ?, ?, ?, ?, ?)",
        (str(uuid.uuid4()), namespace, key, action, caller, datetime.utcnow().isoformat()),
    )
    db.commit()


# ---------------------------------------------------------------------------
# Models
# ---------------------------------------------------------------------------
class SecretSet(BaseModel):
    value: str = Field(..., description="Secret value — encrypted at rest, never logged")
    description: Optional[str] = Field(None, description="Human-readable note (not secret)")

class GrantCreate(BaseModel):
    target_namespace: str = Field(..., description="Namespace to grant access to")
    expires_seconds: Optional[int] = Field(0, description="Access TTL in seconds (0 = never)")


# ---------------------------------------------------------------------------
# Caller extraction (from header or default)
# ---------------------------------------------------------------------------
def _get_caller(request: Request) -> str:
    return request.headers.get("X-Caller-Agent", "unknown")


# ---------------------------------------------------------------------------
# API Endpoints
# ---------------------------------------------------------------------------
@app.on_event("startup")
def on_startup():
    init_db()


@app.get("/health")
def health_check():
    return {"status": "ok", "service": "secret_store", "version": "1.0.0"}


@app.post("/secrets/{namespace}/{key}")
def set_secret(namespace: str, key: str, body: SecretSet, request: Request):
    """Store an encrypted secret."""
    encrypted = _encrypt(body.value, MASTER_KEY)
    db = get_db()
    try:
        db.execute(
            """INSERT INTO secrets (id, namespace, key, encrypted_value, description, created_at, updated_at)
               VALUES (?, ?, ?, ?, ?, ?, ?)
               ON CONFLICT(namespace, key) DO UPDATE SET
                   encrypted_value=excluded.encrypted_value,
                   description=excluded.description,
                   updated_at=excluded.updated_at""",
            (
                str(uuid.uuid4()), namespace, key, encrypted, body.description,
                datetime.utcnow().isoformat(), datetime.utcnow().isoformat(),
            ),
        )
        db.commit()
        _log_audit(db, namespace, key, "set", _get_caller(request))
        return {"success": True, "namespace": namespace, "key": key, "description": body.description}
    finally:
        db.close()


@app.get("/secrets/{namespace}/{key}")
def get_secret(namespace: str, key: str, request: Request):
    """Retrieve a decrypted secret value."""
    db = get_db()
    try:
        # Check direct ownership
        row = db.execute(
            "SELECT encrypted_value, description FROM secrets WHERE namespace=? AND key=?",
            (namespace, key),
        ).fetchone()

        # Check grants if not owner
        caller = _get_caller(request)
        if not row and caller != namespace:
            grant = db.execute(
                """SELECT g.* FROM grants g
                   JOIN secrets s ON s.namespace = g.namespace AND s.key = g.key
                   WHERE g.target_namespace=? AND g.namespace=s.namespace AND s.key=?
                   AND (g.expires_at IS NULL OR g.expires_at > ?)""",
                (caller, key, datetime.utcnow().isoformat()),
            ).fetchone()
            if grant:
                row = db.execute(
                    "SELECT encrypted_value, description FROM secrets WHERE namespace=? AND key=?",
                    (grant["namespace"], key),
                ).fetchone()
                namespace = grant["namespace"]

        if not row:
            _log_audit(db, namespace, key, "get_miss", caller)
            raise HTTPException(status_code=404, detail="Secret not found")

        decrypted = _decrypt(row["encrypted_value"], MASTER_KEY)
        _log_audit(db, namespace, key, "get", caller)
        return {"success": True, "namespace": namespace, "key": key, "value": decrypted}
    finally:
        db.close()


@app.get("/secrets/{namespace}")
def list_secrets(namespace: str):
    """List secret keys (NOT values) in a namespace."""
    db = get_db()
    try:
        rows = db.execute(
            "SELECT key, description, created_at, updated_at FROM secrets WHERE namespace=?",
            (namespace,),
        ).fetchall()
        return {
            "success": True,
            "namespace": namespace,
            "count": len(rows),
            "secrets": [
                {"key": r["key"], "description": r["description"], "created_at": r["created_at"]}
                for r in rows
            ],
        }
    finally:
        db.close()


@app.delete("/secrets/{namespace}/{key}")
def delete_secret(namespace: str, key: str, request: Request):
    """Delete a secret."""
    db = get_db()
    try:
        cursor = db.execute(
            "DELETE FROM secrets WHERE namespace=? AND key=?",
            (namespace, key),
        )
        db.commit()
        if cursor.rowcount == 0:
            raise HTTPException(status_code=404, detail="Secret not found")
        _log_audit(db, namespace, key, "delete", _get_caller(request))
        # Revoke all grants for this secret
        db.execute(
            "DELETE FROM grants WHERE namespace=? AND key=?",
            (namespace, key),
        )
        db.commit()
        return {"success": True, "namespace": namespace, "key": key}
    finally:
        db.close()


@app.post("/secrets/{namespace}/{key}/grant")
def grant_access(namespace: str, key: str, body: GrantCreate, request: Request):
    """Grant another namespace access to a secret."""
    db = get_db()
    try:
        # Verify secret exists
        row = db.execute(
            "SELECT id FROM secrets WHERE namespace=? AND key=?",
            (namespace, key),
        ).fetchone()
        if not row:
            raise HTTPException(status_code=404, detail="Secret not found")

        # Generate access token
        token = base64.urlsafe_b64encode(os.urandom(24)).decode()

        expires_at = None
        if body.expires_seconds and body.expires_seconds > 0:
            expires_at = datetime.utcfromtimestamp(
                time.time() + body.expires_seconds
            ).isoformat()

        # Upsert grant
        db.execute(
            """INSERT INTO grants (id, namespace, key, target_namespace, token, expires_at, created_at)
               VALUES (?, ?, ?, ?, ?, ?, ?)
               ON CONFLICT(namespace, key, target_namespace) DO UPDATE SET
                   token=excluded.token,
                   expires_at=excluded.expires_at""",
            (
                str(uuid.uuid4()), namespace, key, body.target_namespace,
                token, expires_at, datetime.utcnow().isoformat(),
            ),
        )
        db.commit()
        _log_audit(db, namespace, key, f"grant:{body.target_namespace}", _get_caller(request))
        return {
            "success": True,
            "namespace": namespace,
            "key": key,
            "granted_to": body.target_namespace,
            "token": token,
            "expires_at": expires_at,
        }
    finally:
        db.close()


@app.get("/audit/{namespace}")
def get_audit_log(namespace: str, limit: int = 50):
    """View access audit log for a namespace. Write-only — never contains values."""
    db = get_db()
    try:
        rows = db.execute(
            "SELECT * FROM audit_log WHERE namespace=? ORDER BY timestamp DESC LIMIT ?",
            (namespace, limit),
        ).fetchall()
        return {
            "success": True,
            "namespace": namespace,
            "count": len(rows),
            "entries": [
                {
                    "id": r["id"],
                    "key": r["key"],
                    "action": r["action"],
                    "caller": r["caller"],
                    "timestamp": r["timestamp"],
                }
                for r in rows
            ],
        }
    finally:
        db.close()
