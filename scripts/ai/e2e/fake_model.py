#!/usr/bin/env python3
"""A scripted stand-in for an AI provider, for OpenCAD's end-to-end tests (no keys, no network).

  fake_model_e2e.py PORT SCRIPT.json LOG.jsonl

SCRIPT is a list of replies, used in order, one per chat request: {"text": "..."} or
{"tool_calls": [{"name": "run_command", "arguments": {...}}], "text": "optional"}; {"echo": true} answers with a summary
of what the request held (images, PDFs, last message); when the list is used up it answers "Done.". A reply with
{"when": "regex"} is used only for a request whose last user/tool message matches (others are taken in order).
Every request body is appended to LOG. Speech: POST .../audio/transcriptions answers {"text": FAKE_TRANSCRIPT};
a Gemini generateContent call with audio answers the same transcript as JSON.
"""
import json
import os
import re
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT, SCRIPT, LOG = int(sys.argv[1]), sys.argv[2], sys.argv[3]
replies = json.load(open(SCRIPT)) if os.path.exists(SCRIPT) else []
lock = threading.Lock()
TRANSCRIPT = os.environ.get("FAKE_TRANSCRIPT", "draw a circle with radius 25 at 100,100")


def last_text(body):
    msgs = body.get("messages") or body.get("contents") or []
    for m in reversed(msgs):
        c = m.get("content", m.get("parts"))
        if isinstance(c, str) and c:
            return c
        if isinstance(c, list):
            t = " ".join(str(p.get("text", "")) for p in c if isinstance(p, dict))
            if t.strip():
                return t
    return ""


def summary(body):
    s = json.dumps(body)
    images = s.count('"image_url"') + s.count('"type": "image"') + len(re.findall(r'"mime_?[tT]ype": "image/', s))
    pdfs = s.count("application/pdf") + s.count('"type": "file"')
    return "Seen: images=%d pdfs=%d. Last message: %s" % (images, pdfs, last_text(body)[:400])


def next_reply(body):
    text = last_text(body)
    with lock:
        for i, r in enumerate(replies):
            if "when" in r and not re.search(r["when"], text, re.S):
                continue
            return replies.pop(i)
    return {"text": "Done."}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def send_json(self, obj, code=200):
        data = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        self.send_json({"data": [{"id": "m"}, {"id": "whisper-1"}]})

    def do_POST(self):
        raw = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        ctype = self.headers.get("Content-Type", "")
        try:
            body = json.loads(raw) if "json" in ctype else {"_multipart_bytes": len(raw), "_has_wav": b"RIFF" in raw}
        except Exception:
            body = {"_raw": len(raw)}
        with lock:
            with open(LOG, "a") as f:
                f.write(json.dumps({"path": self.path, "body": body}) + "\n")
        if "audio/transcriptions" in self.path:
            return self.send_json({"text": TRANSCRIPT})
        dumped = json.dumps(body)
        if "generateContent" in self.path and "audio/" in dumped:
            return self.send_json({"candidates": [{"content": {"parts": [{"text": json.dumps({"transcript": TRANSCRIPT})}]}}]})
        r = next_reply(body)
        text = summary(body) if r.get("echo") else r.get("text", "")
        calls = r.get("tool_calls", [])
        if not body.get("stream"):
            msg = {"role": "assistant", "content": text or None}
            if calls:
                msg["tool_calls"] = [{"id": "call_%d" % i, "type": "function",
                                      "function": {"name": c["name"], "arguments": json.dumps(c.get("arguments", {}))}}
                                     for i, c in enumerate(calls)]
            return self.send_json({"choices": [{"index": 0, "message": msg, "finish_reason": "tool_calls" if calls else "stop"}]})
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()

        def chunk(delta, finish=None):
            self.wfile.write(("data: " + json.dumps({"choices": [{"index": 0, "delta": delta, "finish_reason": finish}]}) + "\n\n").encode())
            self.wfile.flush()

        for i in range(0, len(text), 24):
            chunk({"content": text[i:i + 24]})
        for i, c in enumerate(calls):
            chunk({"tool_calls": [{"index": i, "id": "call_%s_%d" % (os.urandom(3).hex(), i), "type": "function",
                                   "function": {"name": c["name"], "arguments": json.dumps(c.get("arguments", {}))}}]})
        chunk({}, "tool_calls" if calls else "stop")
        self.wfile.write(b"data: [DONE]\n\n")
        self.wfile.flush()


ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
