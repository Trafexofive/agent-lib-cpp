import sqlite3
import os

DB_PATH = "app.db"

def get_db():
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    return conn

def init_db():
    db = get_db()
    db.execute("CREATE TABLE IF NOT EXISTS users (id INTEGER PRIMARY KEY, username TEXT UNIQUE, password_hash TEXT, role TEXT)")
    db.execute("CREATE TABLE IF NOT EXISTS orders (id INTEGER PRIMARY KEY, user_id INTEGER, product TEXT, quantity INTEGER, status TEXT)")
    db.execute("INSERT OR IGNORE INTO users VALUES (1, 'admin', 'hash_admin_123', 'admin')")
    db.execute("INSERT OR IGNORE INTO orders VALUES (1, 1, 'Widget', 5, 'pending')")
    db.commit()
