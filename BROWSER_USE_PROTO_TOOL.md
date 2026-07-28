Going with Python stdlib only, talking raw CDP over a hand-rolled WebSocket client — no Playwright/Puppeteer/Selenium. Those pull in a whole npm or pip dependency tree plus their own bundled browser download; you already have `chromium` from the Arch repos, and CDP over `ws://127.0.0.1` needs nothing beyond sockets and JSON. One file, zero `pip install`, auditable start to finish. Interpreter startup cost doesn't matter here — every action is dominated by network round-trips and page loads, not the ~30ms CPython spin-up.

Lifecycle model matters more than the code: each tool call is a fresh process (per the `popen` contract in the schema), so the browser itself has to be a detached, self-managing background process — launched lazily on first call, left running, found again via a port-liveness check on every subsequent call. Current tab state persists across calls in a small state file. No systemd unit needed; the tool manages its own dependency.

## tool.yml

```yaml
kind: Tool
name: browser
version: "1.0"
summary: "Lightweight headless browser control via raw Chrome DevTools Protocol"

description: |
  Drives a persistent Chromium instance (launched lazily, kept running across
  calls) through a hand-rolled CDP client over a raw WebSocket — no
  Playwright/Puppeteer/Selenium, no npm/pip tree, no bundled browser download.
  Requires only a system chromium binary and Python3 stdlib. Current tab
  persists across invocations via ~/.cache/cortex-browser/state.json since
  each call is a fresh process.

category: network
tags: [browser, automation, cdp]
state: beta

implementation:
  runtime: python3
  entrypoint: main.py
  timeout_secs: 45
  input_type: json

input_schema:
  type: object
  required: [action]
  properties:
    action:
      type: string
      enum: [navigate, click, type, get_text, get_html, screenshot, evaluate, wait_for, new_tab, close_tab]
    url:
      type: string
      description: "Required for 'navigate' and 'new_tab'"
    selector:
      type: string
      description: "CSS selector — required for click/type/wait_for, optional for get_text/get_html"
    text:
      type: string
      description: "Required for 'type'"
    clear:
      type: boolean
      default: true
      description: "For 'type': select-all + delete existing field content first"
    js:
      type: string
      description: "Required for 'evaluate'"
    timeout:
      type: number
      default: 15
    path:
      type: string
      description: "For 'screenshot': PNG file path. Omit for inline base64 (small viewports only)."
    headless:
      type: boolean
      default: true
      description: "Only applies the first time the browser is launched"
    port:
      type: integer
      default: 9222

output_schema:
  type: object
  required: [success]
  properties:
    success: { type: boolean }
    error: { type: string }
    data: { type: object, additionalProperties: true }

examples:
  - description: "Open a page"
    params: { action: navigate, url: "https://example.com" }
  - description: "Fill a search box"
    params: { action: type, selector: "input[name=q]", text: "cortex prime mk3" }
  - description: "Click a button"
    params: { action: click, selector: "button.submit" }
  - description: "Grab visible text"
    params: { action: get_text, selector: "article" }
  - description: "Save a screenshot"
    params: { action: screenshot, path: "/tmp/page.png" }
  - description: "Run arbitrary JS"
    params: { action: evaluate, js: "document.title" }
```

## main.py

```python
#!/usr/bin/env python3
"""
tools/browser/main.py — CDP-native browser control, zero external deps.

Talks directly to Chrome's remote-debugging HTTP endpoint (/json/*) and a
hand-rolled WebSocket client for the CDP session itself. No websockets/
websocket-client pip package, no Playwright/Puppeteer. If you'd rather not
carry the ~150 lines of WS framing below, `websocket-client` (pure Python,
one pip install) is the standard drop-in replacement for MiniWebSocket.

Manual test: echo '{"action":"navigate","url":"https://example.com"}' | python3 main.py
"""

import sys, os, json, time, base64, hashlib, struct, socket, secrets
import shutil, subprocess, urllib.request, urllib.parse

# ── paths / constants ────────────────────────────────────────────────────────

STATE_DIR = os.path.expanduser("~/.cache/cortex-browser")
STATE_FILE = os.path.join(STATE_DIR, "state.json")
PROFILE_DIR = os.path.join(STATE_DIR, "profile")
DEFAULT_PORT = 9222

# never trust an inherited HTTP_PROXY/NO_PROXY env for localhost CDP traffic
_opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))


class WebSocketError(Exception):
    pass


class CDPTimeout(Exception):
    pass


# ── minimal RFC6455 client (text frames only, handles continuation + ping) ──

class MiniWebSocket:
    def __init__(self, url, timeout=10):
        assert url.startswith("ws://"), "CDP is localhost — no TLS needed, ws:// only"
        rest = url[len("ws://"):]
        host_port, _, path = rest.partition("/")
        path = "/" + path
        host, _, port = host_port.partition(":")
        port = int(port) if port else 80
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)
        self._leftover = b""
        self._handshake(host, port, path)

    def _handshake(self, host, port, path):
        key = base64.b64encode(secrets.token_bytes(16)).decode()
        req = (
            f"GET {path} HTTP/1.1\r\n"
            f"Host: {host}:{port}\r\n"
            f"Upgrade: websocket\r\n"
            f"Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            f"Sec-WebSocket-Version: 13\r\n\r\n"
        )
        self.sock.sendall(req.encode())
        resp = b""
        while b"\r\n\r\n" not in resp:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise WebSocketError("handshake closed early")
            resp += chunk
        header, _, rest = resp.partition(b"\r\n\r\n")
        self._leftover = rest
        if b"101" not in header.split(b"\r\n", 1)[0]:
            raise WebSocketError(f"handshake failed: {header[:200]!r}")
        expected = base64.b64encode(
            hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()
        ).decode()
        if expected.encode() not in header:
            raise WebSocketError("Sec-WebSocket-Accept mismatch")

    def send(self, data: str):
        payload = data.encode()
        mask = secrets.token_bytes(4)
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        length = len(payload)
        header = bytearray([0x81])  # FIN + text opcode
        if length < 126:
            header.append(0x80 | length)
        elif length < 65536:
            header.append(0x80 | 126)
            header += struct.pack(">H", length)
        else:
            header.append(0x80 | 127)
            header += struct.pack(">Q", length)
        self.sock.sendall(bytes(header) + mask + masked)

    def _recv_exact(self, n):
        buf = b""
        while len(buf) < n:
            if self._leftover:
                take = self._leftover[: n - len(buf)]
                self._leftover = self._leftover[len(take):]
                buf += take
                continue
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise WebSocketError("connection closed")
            buf += chunk
        return buf

    def recv(self) -> str:
        message = b""
        while True:
            hdr = self._recv_exact(2)
            b0, b1 = hdr[0], hdr[1]
            fin, opcode = b0 & 0x80, b0 & 0x0F
            masked, length = b1 & 0x80, b1 & 0x7F
            if length == 126:
                length = struct.unpack(">H", self._recv_exact(2))[0]
            elif length == 127:
                length = struct.unpack(">Q", self._recv_exact(8))[0]
            mask_key = self._recv_exact(4) if masked else None
            payload = self._recv_exact(length)
            if mask_key:
                payload = bytes(b ^ mask_key[i % 4] for i, b in enumerate(payload))
            if opcode == 0x8:
                raise WebSocketError("server closed connection")
            if opcode == 0x9:  # ping -> pong
                self._send_control(0xA, payload)
                continue
            if opcode in (0x0, 0x1, 0x2):
                message += payload
                if fin:
                    return message.decode(errors="replace")
                continue

    def _send_control(self, opcode, payload=b""):
        mask = secrets.token_bytes(4)
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        self.sock.sendall(bytes([0x80 | opcode, 0x80 | len(payload)]) + mask + masked)

    def close(self):
        try:
            self._send_control(0x8)
        except Exception:
            pass
        self.sock.close()


# ── CDP session: request/response by id, plus raw event waiting ─────────────

class CDPSession:
    def __init__(self, ws_url, timeout=15):
        self.ws = MiniWebSocket(ws_url, timeout=timeout)
        self._id = 0

    def call(self, method, params=None, timeout=15):
        self._id += 1
        msg_id = self._id
        self.ws.send(json.dumps({"id": msg_id, "method": method, "params": params or {}}))
        self.ws.sock.settimeout(timeout)
        try:
            while True:
                msg = json.loads(self.ws.recv())
                if msg.get("id") == msg_id:
                    if "error" in msg:
                        raise RuntimeError(f"CDP error on {method}: {msg['error']}")
                    return msg.get("result", {})
        except socket.timeout:
            raise CDPTimeout(f"timed out waiting for response to {method}")

    def wait_for_event(self, method, timeout=15):
        self.ws.sock.settimeout(timeout)
        try:
            while True:
                msg = json.loads(self.ws.recv())
                if msg.get("method") == method:
                    return msg.get("params", {})
        except socket.timeout:
            raise CDPTimeout(f"timed out waiting for event {method}")

    def close(self):
        self.ws.close()


# ── browser process lifecycle ────────────────────────────────────────────────

def _http(method, url, timeout=5):
    req = urllib.request.Request(url, method=method)
    with _opener.open(req, timeout=timeout) as r:
        return r.read()


def port_alive(port):
    try:
        _http("GET", f"http://127.0.0.1:{port}/json/version", timeout=0.5)
        return True
    except Exception:
        return False


def find_chromium():
    for name in ("chromium", "chromium-browser", "google-chrome-stable", "google-chrome"):
        path = shutil.which(name)
        if path:
            return path
    raise RuntimeError("no chromium/chrome binary on PATH — pacman -S chromium")


def ensure_browser(port, headless):
    if port_alive(port):
        return port
    os.makedirs(PROFILE_DIR, exist_ok=True)
    args = [
        find_chromium(),
        f"--remote-debugging-port={port}",   # binds 127.0.0.1 only by default — never pass
                                              # --remote-debugging-address=0.0.0.0 on a networked box,
                                              # that port is unauthenticated full browser control
        f"--user-data-dir={PROFILE_DIR}",    # persistent profile — cookies/logins survive across
                                              # agent sessions on purpose; rm -rf to reset
        "--disable-extensions",
        "--disable-background-networking",
        "--disable-sync",
        "--no-first-run",
        "--disable-translate",
        "--disable-default-apps",
    ]
    if headless:
        args.append("--headless=new")
    subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                      stdin=subprocess.DEVNULL, start_new_session=True)
    deadline = time.time() + 8
    while time.time() < deadline:
        if port_alive(port):
            return port
        time.sleep(0.15)
    raise RuntimeError(f"chromium did not come up on port {port} in time")


# ── tab state, persisted across invocations ─────────────────────────────────

def load_state():
    try:
        with open(STATE_FILE) as f:
            return json.load(f)
    except Exception:
        return {}


def save_state(state):
    os.makedirs(STATE_DIR, exist_ok=True)
    with open(STATE_FILE, "w") as f:
        json.dump(state, f)


def create_tab(port, url="about:blank"):
    resp = _http("PUT", f"http://127.0.0.1:{port}/json/new?{urllib.parse.quote(url, safe='')}")
    return json.loads(resp)["id"]


def close_tab(port, target_id):
    _http("PUT", f"http://127.0.0.1:{port}/json/close/{target_id}")


def target_exists(port, target_id):
    try:
        targets = json.loads(_http("GET", f"http://127.0.0.1:{port}/json/list", timeout=2))
        return any(t.get("id") == target_id for t in targets)
    except Exception:
        return False


def get_or_create_tab(port):
    state = load_state()
    target_id = state.get("target_id")
    if target_id and state.get("port") == port and target_exists(port, target_id):
        return target_id
    target_id = create_tab(port)
    save_state({"target_id": target_id, "port": port})
    return target_id


# ── actions ──────────────────────────────────────────────────────────────────

def act_navigate(cdp, p):
    url = p.get("url")
    if not url:
        raise ValueError("url is required for navigate")
    timeout = float(p.get("timeout", 15))
    cdp.call("Page.enable")
    cdp.call("Page.navigate", {"url": url})
    try:
        cdp.wait_for_event("Page.loadEventFired", timeout=timeout)
    except CDPTimeout:
        pass  # pages with long-poll/websocket connections may never fire a clean load event —
              # page is very likely usable already; this is a coarser heuristic than Playwright's
              # networkidle, good enough for most sites
    title = cdp.call("Runtime.evaluate", {"expression": "document.title", "returnByValue": True})
    url_now = cdp.call("Runtime.evaluate", {"expression": "location.href", "returnByValue": True})
    return {"url": url_now.get("result", {}).get("value"), "title": title.get("result", {}).get("value")}


def act_click(cdp, p):
    selector = p.get("selector")
    if not selector:
        raise ValueError("selector is required for click")
    cdp.call("DOM.enable")
    root = cdp.call("DOM.getDocument", {"depth": 0})["root"]["nodeId"]
    found = cdp.call("DOM.querySelector", {"nodeId": root, "selector": selector})
    node_id = found.get("nodeId", 0)
    if not node_id:
        raise ValueError(f"no element matches selector: {selector}")
    cdp.call("DOM.scrollIntoViewIfNeeded", {"nodeId": node_id})
    box = cdp.call("DOM.getBoxModel", {"nodeId": node_id})
    quad = box["model"]["content"]  # [x1,y1,x2,y2,x3,y3,x4,y4]
    cx, cy = sum(quad[0::2]) / 4, sum(quad[1::2]) / 4
    # real Input-domain dispatch, not element.click() via JS — this is what makes the click
    # trusted from the page's perspective (isTrusted: true), same reason Playwright/Puppeteer
    # do it this way instead of a script-invoked .click()
    cdp.call("Input.dispatchMouseEvent", {"type": "mouseMoved", "x": cx, "y": cy})
    cdp.call("Input.dispatchMouseEvent", {"type": "mousePressed", "x": cx, "y": cy, "button": "left", "clickCount": 1})
    cdp.call("Input.dispatchMouseEvent", {"type": "mouseReleased", "x": cx, "y": cy, "button": "left", "clickCount": 1})
    return {"clicked": selector, "x": cx, "y": cy}


def act_type(cdp, p):
    selector, text = p.get("selector"), p.get("text")
    if not selector or text is None:
        raise ValueError("selector and text are required for type")
    act_click(cdp, {"selector": selector})  # focus via the same real-click path
    if p.get("clear", True):
        # Ctrl+A then Backspace — modifiers bitmask: Alt=1, Ctrl=2, Meta=4, Shift=8
        cdp.call("Input.dispatchKeyEvent", {"type": "keyDown", "modifiers": 2, "key": "a", "code": "KeyA", "windowsVirtualKeyCode": 65})
        cdp.call("Input.dispatchKeyEvent", {"type": "keyUp", "modifiers": 2, "key": "a", "code": "KeyA", "windowsVirtualKeyCode": 65})
        cdp.call("Input.dispatchKeyEvent", {"type": "keyDown", "key": "Backspace", "code": "Backspace", "windowsVirtualKeyCode": 8})
        cdp.call("Input.dispatchKeyEvent", {"type": "keyUp", "key": "Backspace", "code": "Backspace", "windowsVirtualKeyCode": 8})
    # Input.insertText — lighter than per-character keyDown/char/keyUp sequences, fires real
    # input events (works fine with React/Vue controlled inputs). Tradeoff: no per-character
    # keydown, so a page with custom keyboard-shortcut handling on the field won't see it.
    cdp.call("Input.insertText", {"text": text})
    return {"typed_into": selector, "chars": len(text)}


def act_get_text(cdp, p):
    selector = p.get("selector")
    expr = f"document.querySelector({json.dumps(selector)})?.innerText" if selector else "document.body.innerText"
    value = cdp.call("Runtime.evaluate", {"expression": expr, "returnByValue": True}).get("result", {}).get("value")
    if value is None:
        raise ValueError(f"no element matches selector: {selector}" if selector else "no body text")
    return {"text": value}


def act_get_html(cdp, p):
    selector = p.get("selector")
    expr = f"document.querySelector({json.dumps(selector)})?.outerHTML" if selector else "document.documentElement.outerHTML"
    value = cdp.call("Runtime.evaluate", {"expression": expr, "returnByValue": True}).get("result", {}).get("value")
    if value is None:
        raise ValueError(f"no element matches selector: {selector}" if selector else "no html")
    return {"html": value}


def act_screenshot(cdp, p):
    data_b64 = cdp.call("Page.captureScreenshot", {"format": "png"})["data"]
    path = p.get("path")
    if path:
        with open(path, "wb") as f:
            f.write(base64.b64decode(data_b64))
        return {"path": path, "bytes": len(data_b64) * 3 // 4}
    return {"base64_png": data_b64}


def act_evaluate(cdp, p):
    js = p.get("js")
    if not js:
        raise ValueError("js is required for evaluate")
    res = cdp.call("Runtime.evaluate", {"expression": js, "returnByValue": True, "awaitPromise": True})
    if "exceptionDetails" in res:
        raise RuntimeError(res["exceptionDetails"].get("text", "JS exception"))
    return {"result": res.get("result", {}).get("value")}


def act_wait_for(cdp, p):
    selector = p.get("selector")
    if not selector:
        raise ValueError("selector is required for wait_for")
    timeout = float(p.get("timeout", 15))
    expr = f"!!document.querySelector({json.dumps(selector)})"
    deadline = time.time() + timeout
    while time.time() < deadline:
        if cdp.call("Runtime.evaluate", {"expression": expr, "returnByValue": True}).get("result", {}).get("value"):
            return {"found": selector}
        time.sleep(0.25)
    raise TimeoutError(f"selector not found within {timeout}s: {selector}")


ACTIONS = {
    "navigate": act_navigate,
    "click": act_click,
    "type": act_type,
    "get_text": act_get_text,
    "get_html": act_get_html,
    "screenshot": act_screenshot,
    "evaluate": act_evaluate,
    "wait_for": act_wait_for,
}


# ── entrypoint ───────────────────────────────────────────────────────────────

def main():
    try:
        raw = sys.stdin.read()
        p = json.loads(raw) if raw.strip() else {}
    except json.JSONDecodeError as e:
        print(json.dumps({"success": False, "error": f"invalid JSON on stdin: {e}"}))
        return

    action = p.get("action")
    if not action:
        print(json.dumps({"success": False, "error": "action is required"}))
        return

    port = int(p.get("port", DEFAULT_PORT))

    try:
        port = ensure_browser(port, p.get("headless", True))

        if action == "new_tab":
            target_id = create_tab(port, p.get("url", "about:blank"))
            save_state({"target_id": target_id, "port": port})
            print(json.dumps({"success": True, "data": {"target_id": target_id}}))
            return

        if action == "close_tab":
            state = load_state()
            target_id = state.get("target_id")
            if target_id:
                close_tab(port, target_id)
                save_state({})
            print(json.dumps({"success": True, "data": {"closed": target_id}}))
            return

        handler = ACTIONS.get(action)
        if not handler:
            raise ValueError(f"unknown action: {action}")

        target_id = get_or_create_tab(port)
        cdp = CDPSession(f"ws://127.0.0.1:{port}/devtools/page/{target_id}", timeout=float(p.get("timeout", 15)))
        try:
            print(json.dumps({"success": True, "data": handler(cdp, p)}))
        finally:
            cdp.close()

    except Exception as e:
        print(json.dumps({"success": False, "error": str(e)}))


if __name__ == "__main__":
    main()
```

Known limitations worth knowing before it bites you: navigation readiness is `Page.loadEventFired`, not Playwright-style network-idle — heavy SPAs may report "loaded" before their content actually renders, so a `wait_for` on a real content selector right after `navigate` is the correct pattern, not an assumption that navigate alone means the DOM is ready. `type` uses `Input.insertText` rather than full per-character key events, which is lighter and works fine for standard form fields but won't trigger custom keydown-bound shortcuts on a page. Single active tab per `state.json` by default — `new_tab`/`close_tab` exist but there's no explicit `target_id` override param yet if you want to juggle multiple tabs concurrently from one agent; easy to add if you need it (pass `target_id` through `params`, skip `get_or_create_tab`).
