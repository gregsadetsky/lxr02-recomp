"""serve site/ with the cross origin isolation headers sharedarraybuffer needs.
usage: uv run --no-project python serve.py [port]"""
import http.server, sys, functools, os
class H(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()
port = int(sys.argv[1]) if len(sys.argv) > 1 else 8020
d = os.path.join(os.path.dirname(os.path.abspath(__file__)), "site")
http.server.ThreadingHTTPServer(("127.0.0.1", port), functools.partial(H, directory=d)).serve_forever()
