import json
from urllib.parse import urlparse, parse_qs
from http.server import BaseHTTPRequestHandler
from auth import authenticate, create_user
from models import get_orders, create_order, update_order_status, delete_order, get_user

def handle_request(handler):
    parsed = urlparse(handler.path)
    path = parsed.path
    method = handler.command
    params = parse_qs(parsed.query)
    content_length = int(handler.headers.get('Content-Length', 0))
    body = json.loads(handler.rfile.read(content_length).decode()) if content_length else {}
    
    if path == "/login" and method == "POST":
        user = authenticate(body.get("username", ""), body.get("password", ""))
        if user:
            send_json(handler, {"ok": True, "user": user["username"], "role": user["role"]})
        else:
            send_json(handler, {"ok": False, "error": "Invalid credentials"}, 401)
    elif path == "/register" and method == "POST":
        ok = create_user(body.get("username", ""), body.get("password", ""), body.get("role", "user"))
        if ok:
            send_json(handler, {"ok": True})
        else:
            send_json(handler, {"ok": False, "error": "Username taken"}, 409)
    elif path == "/orders" and method == "GET":
        user_id = params.get("user_id", [None])[0]
        orders = get_orders(int(user_id) if user_id else None)
        send_json(handler, {"ok": True, "orders": orders})
    elif path == "/orders" and method == "POST":
        order_id = create_order(body["user_id"], body["product"], body["quantity"])
        send_json(handler, {"ok": True, "order_id": order_id}, 201)
    elif path.startswith("/orders/") and method == "PATCH":
        order_id = int(path.split("/")[2])
        ok = update_order_status(order_id, body["status"])
        send_json(handler, {"ok": ok})
    elif path.startswith("/orders/") and method == "DELETE":
        order_id = int(path.split("/")[2])
        ok = delete_order(order_id)
        send_json(handler, {"ok": ok})
    elif path == "/users" and method == "GET":
        user_id = params.get("id", [None])[0]
        user = get_user(int(user_id)) if user_id else None
        send_json(handler, {"ok": True, "user": user} if user else {"ok": False, "error": "Not found"}, 200 if user else 404)
    else:
        send_json(handler, {"ok": False, "error": "Not found"}, 404)

def send_json(handler, data, status=200):
    handler.send_response(status)
    handler.send_header("Content-Type", "application/json")
    handler.end_headers()
    handler.wfile.write(json.dumps(data).encode())

class AppHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        handle_request(self)
    def do_POST(self):
        handle_request(self)
    def do_PATCH(self):
        handle_request(self)
    def do_DELETE(self):
        handle_request(self)
    def log_message(self, format, *args):
        pass
