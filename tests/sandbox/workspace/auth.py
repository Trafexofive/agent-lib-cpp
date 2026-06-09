import hashlib
from db import get_db

def hash_password(password):
    return hashlib.sha256(password.encode()).hexdigest()

def verify_password(password, hash_val):
    return hashlib.sha256(password.encode()).hexdigest() == hash_val

def authenticate(username, password):
    db = get_db()
    user = db.execute("SELECT * FROM users WHERE username = ?", (username,)).fetchone()
    if user and verify_password(password, user["password_hash"]):
        return dict(user)
    return None

def create_user(username, password, role="user"):
    db = get_db()
    try:
        db.execute("INSERT INTO users (username, password_hash, role) VALUES (?, ?, ?)",
                   (username, hash_password(password), role))
        db.commit()
        return True
    except sqlite3.IntegrityError:
        return False
