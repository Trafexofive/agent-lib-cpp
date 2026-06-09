# monolith.py — Python monolith needing decoupling
# Single file mixing: database, auth, business logic, HTTP routing, file I/O

import sqlite3
import hashlib
import os
import json
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse, parse_qs

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

def get_orders(user_id=None):
    db = get_db()
    if user_id:
        return [dict(r) for r in db.execute("SELECT * FROM orders WHERE user_id = ?", (user_id,)).fetchall()]
    return [dict(r) for r in db.execute("SELECT * FROM orders").fetchall()]

def create_order(user_id, product, quantity):
    db = get_db()
    db.execute("INSERT INTO orders (user_id, product, quantity, status) VALUES (?, ?, ?, 'pending')",
               (user_id, product, quantity))
    db.commit()

def generate_report(format="json"):
    orders = get_orders()
    if format == "json":
        return json.dumps(orders, indent=2)
    elif format == "csv":
        lines = ["id,user_id,product,quantity,status"]
        for o in orders:
            lines.append(f"{o['id']},{o['user_id']},{o['product']},{o['quantity']},{o['status']}")
        return "\n".join(lines)
    return "Unknown format"

def save_report_to_file(report, filename="report.txt"):
    with open(filename, "w") as f:
        f.write(report)
    return os.path.getsize(filename)

class APIHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path
        params = parse_qs(parsed.query)
        
        if path == "/health":
            self.json_response({"status": "ok"})
        elif path == "/orders":
            orders = get_orders()
            self.json_response(orders)
        elif path == "/report":
            fmt = params.get("format", ["json"])[0]
            report = generate_report(fmt)
            self.wfile.write(report.encode())
        elif path == "/users":
            users = [dict(r) for r in get_db().execute("SELECT id, username, role FROM users").fetchall()]
            self.json_response(users)
        else:
            self.send_error(404)
    
    def do_POST(self):
        content_len = int(self.headers.get("Content-Length", 0))
        body = json.loads(self.rfile.read(content_len)) if content_len else {}
        
        if self.path == "/login":
            user = authenticate(body.get("username"), body.get("password"))
            if user:
                self.json_response({"token": "mock_token_" + user["username"]})
            else:
                self.send_error(401)
        elif self.path == "/users":
            ok = create_user(body.get("username"), body.get("password"), body.get("role", "user"))
            self.json_response({"created": ok})
        elif self.path == "/orders":
            create_order(body["user_id"], body["product"], body["quantity"])
            self.json_response({"created": True})
        elif self.path == "/report/save":
            fmt = body.get("format", "json")
            report = generate_report(fmt)
            size = save_report_to_file(report, body.get("filename", "report.txt"))
            self.json_response({"saved": True, "size": size})
        else:
            self.send_error(404)
    
    def json_response(self, data):
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(json.dumps(data).encode())

if __name__ == "__main__":
    init_db()
    print("Starting monolith on :8080")
    HTTPServer(("0.0.0.0", 8080), APIHandler).serve_forever()
