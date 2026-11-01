"""Exercise the native HTTP/state paths against isolated HTTPS and registry fixtures.

The proxy terminates TLS for api.noctalia.dev locally; its CA is trusted only by the
child process. The client keeps its production URL, and no public vote is submitted.
Run: python3 tests/plugin_recommendations_http_test.py build-debug/plugin_recommendations_test
"""

import hashlib
import http.server
import json
import os
from pathlib import Path
import re
import socket
import sqlite3
import ssl
import subprocess
import sys
import tempfile
import threading


def run(binary):
    with tempfile.TemporaryDirectory(prefix="noctalia-recommendations-") as directory:
        root = Path(directory)
        api = int(subprocess.check_output([str(Path(binary).resolve()), "--api"]))
        names = ["CasePlugin", "Hidden", "Lost", "Conflict", "Limited", "Unavailable", "Proxy", "Uninstalled", "Deprecated", "Delisted"]
        state_root = root / "state/noctalia"
        config_dir = root / "config/noctalia"
        config_dir.mkdir(parents=True)
        (config_dir / "config.toml").write_text("[shell]\ntelemetry_enabled = false\nsetup_wizard_enabled = false\n")
        for source in ["official", "community"]:
            repo = state_root / "plugins/sources" / source / "repo"
            repo.mkdir(parents=True)
            catalog = ""
            for name in names if source == "official" else []:
                row = f'id = "Fixture/{name}"\nname = "{name}"\nversion = "1.0.0"\nplugin_api = {api}\n'
                if name == "Deprecated":
                    row += "deprecated = true\n"
                catalog += "[[plugin]]\n" + row + "\n"
                if name != "Uninstalled":
                    plugin = state_root / "plugins/materialized" / source / name
                    plugin.mkdir(parents=True)
                    (plugin / "plugin.toml").write_text(row)
            (repo / "catalog.toml").write_text(catalog)
            for args in [["init", "-q"], ["add", "catalog.toml"], ["-c", "commit.gpgsign=false", "-c", "core.hooksPath=/dev/null", "-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid", "commit", "-qm", "fixture"]]:
                subprocess.run(["git", "-C", str(repo), *args], check=True, capture_output=True)

        key = root / "key.pem"
        cert = root / "cert.pem"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", str(key), "-out", str(cert), "-days", "1", "-subj", "/CN=api.noctalia.dev", "-addext", "subjectAltName=DNS:api.noctalia.dev", "-addext", "basicConstraints=critical,CA:TRUE"], check=True, capture_output=True)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        database = sqlite3.connect(":memory:", check_same_thread=False)
        database.execute("CREATE TABLE recommendation (key TEXT, token TEXT, desired INTEGER, PRIMARY KEY(key, token))")
        lock = threading.Lock()
        calls = []
        lost_once = set()
        violations = []
        delisted = False

        class Origin(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *_):
                pass

            def reply(self, status, doc=None, **headers):
                body = b"" if doc is None else json.dumps(doc, separators=(",", ":")).encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Connection", "close")
                self.close_connection = True
                for name, value in headers.items():
                    self.send_header(name.replace("_", "-"), str(value))
                self.end_headers()
                self.wfile.write(body)

            def do_GET(self):
                if self.path != "/v1/plugin-metrics":
                    self.reply(404, {"error": "invalid_path"})
                    return
                with lock:
                    entries = []
                    for name in names:
                        if name == "Hidden" or (name == "Delisted" and delisted):
                            continue
                        key = "official:Fixture/" + name
                        count = database.execute("SELECT COUNT(*) FROM recommendation WHERE key=? AND desired=1", (key,)).fetchone()[0]
                        entries.append({"key": key, "recommendations": count, "trending_score": float(count)})
                    document = {"schema": 1, "generated_at": "2026-10-09T12:00:00Z", "plugins": sorted(entries, key=lambda x: x["key"])}
                    etag = '"' + hashlib.sha256(json.dumps(document).encode()).hexdigest() + '"'
                    status = 304 if self.headers.get("If-None-Match") == etag else 200
                    calls.append(("GET", status))
                self.reply(status, document if status == 200 else None, ETag=etag, Cache_Control="public, max-age=0")

            def do_PUT(self):
                nonlocal delisted
                match = re.fullmatch(r"/v1/plugins/(official|community)/(Fixture)/([A-Za-z]+)/recommendation", self.path)
                body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
                doc = json.loads(body)
                valid = match and len(body) <= 1024 and set(doc) == {"voter_token", "recommended"} and type(doc["recommended"]) is bool and re.fullmatch(r"[A-Za-z0-9_-]{43}", doc["voter_token"]) and self.headers.get("Content-Type") == "application/json" and not self.headers.get("X-Forwarded-Proto") and not self.headers.get("X-Real-IP")
                if not valid:
                    violations.append("invalid native request")
                    self.reply(400, {"error": "invalid_request"})
                    return
                source, author, name = match.groups()
                key = f"{source}:{author}/{name}"
                with lock:
                    calls.append(("PUT", name, doc["recommended"]))
                    if name in ["Conflict", "Limited", "Unavailable", "Proxy"]:
                        status, code = {"Conflict": (409, "plugin_not_recommendable"), "Limited": (429, "rate_limited"), "Unavailable": (503, "recommendations_unavailable"), "Proxy": (502, "proxy")}[name]
                        if name == "Proxy":
                            self.send_response(status)
                            self.send_header("Content-Length", "16")
                            self.end_headers()
                            self.wfile.write(b"<html>bad</html>")
                        else:
                            self.reply(status, {"error": code}, Retry_After=1, Cache_Control="no-store")
                        return
                    database.execute("INSERT INTO recommendation VALUES (?, ?, ?) ON CONFLICT(key, token) DO UPDATE SET desired=excluded.desired", (key, doc["voter_token"], doc["recommended"]))
                    count = database.execute("SELECT COUNT(*) FROM recommendation WHERE key=? AND desired=1", (key,)).fetchone()[0]
                    if name == "Delisted" and not delisted:
                        delisted = True
                        repo = state_root / "plugins/sources/official/repo"
                        catalog_file = repo / "catalog.toml"
                        catalog_file.write_text(re.sub(r'\[\[plugin\]\]\nid = "Fixture/Delisted"\n.*?(?=\n\[\[plugin\]\]|\Z)', '', catalog_file.read_text(), flags=re.DOTALL))
                        subprocess.run(["git", "-C", str(repo), "add", "catalog.toml"], check=True, capture_output=True)
                        subprocess.run(["git", "-C", str(repo), "-c", "commit.gpgsign=false", "-c", "core.hooksPath=/dev/null", "-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid", "commit", "-qm", "delist"], check=True, capture_output=True)
                    if name == "Lost" and name not in lost_once:
                        lost_once.add(name)
                        self.close_connection = True
                        self.connection.shutdown(socket.SHUT_RDWR)
                        return
                self.reply(200, {"recommended": doc["recommended"], "recommendations": None if name == "Hidden" else count}, Cache_Control="no-store")

        class Proxy(http.server.BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass

            def do_CONNECT(self):
                if self.path != "api.noctalia.dev:443":
                    violations.append("unexpected proxy destination")
                    self.send_error(403)
                    return
                self.send_response(200)
                self.end_headers()
                self.close_connection = True
                try:
                    connection = context.wrap_socket(self.connection, server_side=True)
                    Origin(connection, self.client_address, self.server)
                except (ssl.SSLError, ConnectionError):
                    pass

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Proxy)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        env = dict(os.environ)
        for name in ["HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "http_proxy", "https_proxy", "all_proxy", "NO_PROXY", "no_proxy"]:
            env.pop(name, None)
        env.update(NOCTALIA_RECOMMENDATION_FIXTURE="1", NOCTALIA_CONFIG_HOME=str(root / "config"), NOCTALIA_STATE_HOME=str(root / "state"), NOCTALIA_DATA_HOME=str(root / "data"), HTTPS_PROXY=f"http://127.0.0.1:{server.server_port}", NOCTALIA_ASSETS_DIR=str(Path("assets").resolve()))
        try:
            subprocess.run([str(Path(binary).resolve()), "--http", str(cert)], env=env, check=True, timeout=60)
            if violations:
                raise AssertionError(violations)
            assert not any(call[0] == "PUT" and call[1] in ["Deprecated", "Uninstalled"] for call in calls)
            assert sum(call[0] == "PUT" and call[1] == "Limited" for call in calls) == 1
            assert sum(call[0] == "PUT" and call[1] == "Conflict" for call in calls) == 1
            assert sum(call[0] == "PUT" and call[1] == "Lost" and call[2] for call in calls) == 2
            assert any(call == ("GET", 304) for call in calls)
            assert database.execute("SELECT COUNT(*) FROM recommendation WHERE key='official:Fixture/Lost'").fetchone()[0] == 1
            assert database.execute("SELECT SUM(desired) FROM recommendation WHERE key='official:Fixture/CasePlugin'").fetchone()[0] == 0
            print("Native HTTPS/state fixture passed; public API received no requests.")
        finally:
            server.shutdown()
            server.server_close()
            database.close()


if __name__ == "__main__":
    run(sys.argv[1])
