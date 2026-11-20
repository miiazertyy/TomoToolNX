#!/usr/bin/env python3
"""Serve data/webui.html on your computer, to work on the WebUI without
rebuilding the .nro after every change.

    python tools/webui_dev_server.py                      # mock data
    python tools/webui_dev_server.py --console 192.168.1.20:8080
    python tools/webui_dev_server.py --locale ja          # preview a translation

The page is read from disk on every request, so saving the file and reloading
the browser is enough.

The page follows the console's language (it asks /api/config every few
seconds), so picking a language in the browser does not stick. --locale makes
/api/config report that language instead, in both modes.

Mock mode answers the read-only endpoints from the seed saves in data/ (an
island with a player, Miis and a map), so every tab has something to show.
Anything that would change a save is refused. The texture import sheet works
too: /api/importsource keeps the picture, /api/importpreview turns and fits it
with Pillow (when installed; the console makes the exact texture).

With --console, every /api request is passed through to TomoToolNX running on
a Switch (or in an emulator), so the page edits the real save. The console
must be on the editor screen for its server to be running.
"""
import argparse
import email.parser
import email.policy
import http.server
import io
import json
import os
import re
import sys
import urllib.error
import urllib.parse
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PAGE = os.path.join(ROOT, "data", "webui.html")
SEEDS = {"player": "seed_Player.bin", "mii": "seed_Mii.bin", "map": "seed_Map.bin"}


def app_version():
    try:
        with open(os.path.join(ROOT, "source", "updater.h"), encoding="utf-8") as f:
            m = re.search(r'#define\s+APP_VERSION\s+"([^"]+)"', f.read())
            return m.group(1) if m else "dev"
    except OSError:
        return "dev"


class Handler(http.server.BaseHTTPRequestHandler):
    console = None  # "host:port" when proxying
    locale = None   # language /api/config reports (None: the console's own, or "en" when mocking)

    def log_message(self, fmt, *args):
        sys.stderr.write("  %s %s\n" % (self.command, self.path))

    # ── helpers ──────────────────────────────────────────────────────────────
    def send(self, code, body, ctype="application/json"):
        if isinstance(body, (dict, list)):
            body = json.dumps(body).encode()
        elif isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n) if n > 0 else b""

    # ── routes ───────────────────────────────────────────────────────────────
    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        if url.path in ("/", "/index.html"):
            with open(PAGE, "rb") as f:
                return self.send(200, f.read(), "text/html; charset=utf-8")
        if url.path.startswith("/gfx/"):
            # The game's art, from this checkout (the console serves its own copy).
            name = url.path[5:]
            if re.fullmatch(r"[a-z0-9_]+\.(png|jpg)", name):
                path = os.path.join(ROOT, "romfs", "gfx", name)
                if os.path.isfile(path):
                    with open(path, "rb") as f:
                        return self.send(200, f.read(), "image/png" if name.endswith(".png") else "image/jpeg")
            return self.send(404, {"error": "not found"})
        if url.path.startswith("/mapart/") and not self.console:
            return self.mock_map_art(url.path[8:])
        if url.path.startswith("/api/") or (url.path.startswith("/mapart/") and self.console):
            return self.proxy() if self.console else self.mock_get(url)
        self.send(404, {"error": "not found"})

    def do_POST(self):
        if self.path.startswith("/api/") and self.console:
            return self.proxy()
        if urllib.parse.urlparse(self.path).path == "/api/importsource":
            return self.mock_import_source()
        self.body()
        self.send(403, {"ok": False, "error": "mock server: saves are read-only here (use --console to edit a real one)"})

    def mock_get(self, url):
        q = urllib.parse.parse_qs(url.query)
        if url.path == "/api/config":
            return self.send(200, {"saveWarnAcked": True, "locale": self.locale or "en", "theme": "classic",
                                   "version": app_version()})
        if url.path == "/api/list":
            return self.send(200, {"entries": []})
        if url.path == "/api/mii/list":
            return self.send(200, {"miis": []})
        if url.path == "/api/save/download":
            which = (q.get("file") or [""])[0]
            name = SEEDS.get(which)
            if not name:
                return self.send(400, {"error": "unknown file"})
            with open(os.path.join(ROOT, "data", name), "rb") as f:
                return self.send(200, f.read(), "application/octet-stream")
        if url.path == "/api/importpreview":
            return self.mock_import_preview(q)
        if url.path in ("/api/imgsearch", "/api/fetchimg"):
            return self.mock_web_picture(url.path, q)
        if url.path == "/api/preview":
            path = os.path.join(ROOT, "romfs", "gfx", "art_atelier.png")
            with open(path, "rb") as f:
                return self.send(200, f.read(), "image/png")
        self.send(404, {"error": "mock server: %s is not mocked" % url.path})

    # ── pictures from the web, fetched the way the console does ──
    def mock_web_picture(self, path, q):
        get = lambda k, d="": (q.get(k) or [d])[0]
        try:
            if path == "/api/imgsearch":
                term = get("q").strip()
                if not term:
                    return self.send(500, {"ok": False, "error": "Type something to look for"})
                page = max(1, min(50, int(get("page", "1") or 1)))
                target = ("https://api.openverse.org/v1/images/?q=%s&page=%d&page_size=20&mature=false"
                          % (urllib.parse.quote(term), page))
            else:
                target = get("url")
                host = urllib.parse.urlparse(target).hostname or ""
                if not target.startswith("https://") or not host or re.fullmatch(r"[0-9.]+", host) or host == "localhost":
                    return self.send(500, {"ok": False, "error": "Only https pictures"})
            req = urllib.request.Request(target, headers={"User-Agent": "TomoToolNX-dev"})
            with urllib.request.urlopen(req, timeout=20) as r:
                body = r.read(16 * 1024 * 1024 + 1)
            if path == "/api/imgsearch":
                return self.send(200, body, "application/json")
            kinds = {bytes([0x89, 0x50]): "image/png", bytes([0xFF, 0xD8]): "image/jpeg"}
            ct = kinds.get(body[:2]) or ("image/webp" if body[:4] == b"RIFF" and body[8:12] == b"WEBP" else None)
            if not ct or len(body) > 16 * 1024 * 1024:
                return self.send(500, {"ok": False, "error": "That is not a picture"})
            return self.send(200, body, ct)
        except Exception as e:  # network trouble shows in the page
            return self.send(500, {"ok": False, "error": "Network error: %s" % e})

    # ── map pictures, as the console serves them (romfs/items) ──
    map_art = None   # actor key -> picture name

    def mock_map_art(self, name):
        if Handler.map_art is None:
            keys = dict(re.findall(r'\{ 0x([0-9a-f]+)u, "([^"]+)",', open(os.path.join(ROOT, "source", "map_data.cpp"), encoding="utf-8").read()))
            art = dict(re.findall(r'\{ 0x([0-9a-f]+)u, "([^"]+)", ', open(os.path.join(ROOT, "include", "map_art_data.h"), encoding="utf-8").read()))
            Handler.map_art = {key: art[h] for h, key in keys.items() if h in art}
        key = name[:-4] if name.endswith(".png") else name
        asset = key if key.startswith(("Floor_", "Top_")) else Handler.map_art.get(key)
        path = os.path.join(ROOT, "romfs", "items", (asset or "-") + ".png")
        if asset and re.fullmatch(r"[A-Za-z0-9_]+", asset) and os.path.isfile(path):
            with open(path, "rb") as f:
                return self.send(200, f.read(), "image/png")
        self.send(404, {"error": "no picture"})

    # ── the import sheet, roughly (the console's preview is the exact one) ──
    picture = None   # (bytes) the last picture sent to /api/importsource

    def mock_import_source(self):
        ctype = self.headers.get("Content-Type", "")
        head = ("Content-Type: " + ctype).encode() + bytes([13, 10, 13, 10])
        msg = email.parser.BytesParser(policy=email.policy.default).parsebytes(head + self.body())
        for part in msg.iter_parts() if msg.is_multipart() else []:
            if part.get_param("name", header="content-disposition") == "file":
                Handler.picture = part.get_payload(decode=True)
        if not Handler.picture:
            return self.send(500, {"ok": False, "error": "no file"})
        try:
            from PIL import Image
            w, h = Image.open(io.BytesIO(Handler.picture)).size
        except Exception:  # Pillow missing or not a picture
            w = h = 0
        self.send(200, {"ok": True, "w": w, "h": h, "lw": 512, "lh": 512, "fmt": "BC1"})

    def mock_import_preview(self, q):
        get = lambda k, d="": (q.get(k) or [d])[0]
        try:
            from PIL import Image
        except ImportError:
            return self.send(500, {"ok": False, "error": "install Pillow for the mock preview"})
        if not Handler.picture:
            return self.send(500, {"ok": False, "error": "No picture"})
        img = Image.open(io.BytesIO(Handler.picture)).convert("RGBA")
        img = img.rotate(-90 * (int(get("rotate", "0")) % 4), expand=True)
        if get("flipH") == "1":
            img = img.transpose(Image.FLIP_LEFT_RIGHT)
        S = 512
        iw, ih = S, S
        if get("shape") == "book":
            iw = (S * 200 // 256) & ~3
        elif get("shape") == "tv":
            ih = (S * 144 // 256) & ~3
        fit = get("fitMode", "cover") if (iw, ih) == (S, S) else "fill"
        matte = get("matte")
        bg = (0, 0, 0, 0)
        if fit == "contain" and len(matte) == 7:
            bg = tuple(int(matte[i:i + 2], 16) for i in (1, 3, 5)) + (255,)
        inner = Image.new("RGBA", (iw, ih), bg)
        if fit == "fill":
            inner = img.resize((iw, ih), Image.LANCZOS)
        elif fit == "contain":
            k = min(iw / img.width, ih / img.height)
            r = img.resize((max(1, round(img.width * k)), max(1, round(img.height * k))), Image.LANCZOS)
            inner.alpha_composite(r, ((iw - r.width) // 2, (ih - r.height) // 2))
        else:
            k = max(iw / img.width, ih / img.height)
            r = img.resize((max(1, round(img.width * k)), max(1, round(img.height * k))), Image.LANCZOS)
            inner = r.crop(((r.width - iw) // 2, (r.height - ih) // 2, (r.width - iw) // 2 + iw, (r.height - ih) // 2 + ih))
        out = Image.new("RGBA", (S, S), (0, 0, 0, 255) if (iw, ih) != (S, S) else (0, 0, 0, 0))
        out.alpha_composite(inner, ((S - iw) // 2, (S - ih) // 2))
        buf = io.BytesIO()
        out.save(buf, "PNG")
        self.send(200, buf.getvalue(), "image/png")

    def proxy(self):
        data = self.body() if self.command == "POST" else None
        req = urllib.request.Request("http://%s%s" % (self.console, self.path), data=data, method=self.command)
        for h in ("Content-Type", "X-TomoToolNX"):
            if self.headers.get(h):
                req.add_header(h, self.headers[h])
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                body = r.read()
                if self.locale and urllib.parse.urlparse(self.path).path == "/api/config":
                    try:
                        cfg = json.loads(body.decode("utf-8"))
                        cfg["locale"] = self.locale
                        body = json.dumps(cfg).encode()
                    except ValueError:
                        pass
                self.send(r.status, body, r.headers.get("Content-Type", "application/octet-stream"))
        except urllib.error.HTTPError as e:
            self.send(e.code, e.read(), e.headers.get("Content-Type", "application/json"))
        except (urllib.error.URLError, OSError) as e:
            self.send(502, {"ok": False, "error": "console not reachable: %s" % e})


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--port", type=int, default=8000, help="local port (default 8000)")
    ap.add_argument("--console", help="pass /api through to TomoToolNX at host:port")
    ap.add_argument("--locale", help="language the page is shown in: en, fr, es, de, ru, zh or ja")
    a = ap.parse_args()
    if a.console:
        Handler.console = a.console.replace("http://", "").rstrip("/")
    Handler.locale = a.locale
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", a.port), Handler)
    mode = "proxying /api to " + Handler.console if Handler.console else "mock data from data/seed_*.bin"
    if a.locale:
        mode += ", language " + a.locale
    print("TomoToolNX WebUI on http://127.0.0.1:%d  (%s)" % (a.port, mode))
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
