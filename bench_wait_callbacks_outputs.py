#!/usr/bin/env python3
import argparse
import concurrent.futures
import json
import os
import random
import threading
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, HTTPServer
from socketserver import ThreadingMixIn

SM4_FIELDS = ['user_id', 'serial_no', 'user_code', 'business_key', 'device_id', 'trans_id', 'secret_code']
MASK_FIELDS = ['id_card', 'phone', 'name', 'email']


class ThreadingHTTPServer(ThreadingMixIn, HTTPServer):
    daemon_threads = True


class CallbackState:
    def __init__(self, prefix):
        self.prefix = prefix
        self.request_ids = set()
        self.condition = threading.Condition()

    def add(self, request_id):
        if not request_id.startswith(self.prefix):
            return
        with self.condition:
            self.request_ids.add(request_id)
            self.condition.notify_all()

    def count(self):
        with self.condition:
            return len(self.request_ids)


def make_handler(state):
    class CallbackHandler(BaseHTTPRequestHandler):
        def do_POST(self):
            length = int(self.headers.get('Content-Length', '0'))
            raw = self.rfile.read(length)
            try:
                body = json.loads(raw.decode('utf-8'))
                state.add(body.get('requestId', ''))
            except Exception:
                pass
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.end_headers()
            self.wfile.write(b'{"ok":true}')

        def log_message(self, fmt, *args):
            return

    return CallbackHandler


def choose_fields():
    n = random.randint(3, 7)
    sm4_n = min(len(SM4_FIELDS), max(1, int(round(n * 0.7))))
    mask_n = n - sm4_n
    if mask_n > len(MASK_FIELDS):
        mask_n = len(MASK_FIELDS)
        sm4_n = n - mask_n
    fields = random.sample(SM4_FIELDS, sm4_n) + random.sample(MASK_FIELDS, mask_n)
    random.shuffle(fields)
    return fields


def post(i, url, prefix, fields):
    req_id = '%s_%06d' % (prefix, i)
    body = json.dumps({
        'requestId': req_id,
        'sm4Key': '%016d' % (2123433411630000 + i),
        'ip': '127.0.0.1',
        'fieldsToEncrypt': fields,
    }).encode('utf-8')
    req = urllib.request.Request(url, data=body, headers={'Content-Type': 'application/json'}, method='POST')
    start = time.time()
    with urllib.request.urlopen(req, timeout=60) as resp:
        resp.read()
    return req_id, time.time() - start


def wait_outputs(req_ids, output_dir, start, timeout):
    deadline = time.time() + timeout
    last = -1
    while time.time() < deadline:
        done = sum(os.path.exists(os.path.join(output_dir, rid + '.csv')) for rid in req_ids)
        if done != last:
            print('outputs %d/%d elapsed %.3fs' % (done, len(req_ids), time.time() - start), flush=True)
            last = done
        if done == len(req_ids):
            return time.time() - start
        time.sleep(1)
    raise SystemExit('timeout waiting outputs')


def wait_callbacks(state, total, start, timeout):
    deadline = time.time() + timeout
    last = -1
    while time.time() < deadline:
        done = state.count()
        if done != last:
            print('callbacks %d/%d elapsed %.3fs' % (done, total, time.time() - start), flush=True)
            last = done
        if done == total:
            return time.time() - start
        with state.condition:
            state.condition.wait(timeout=0.25)
    raise SystemExit('timeout waiting callbacks')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--url', default='http://127.0.0.1:8080/encrypt')
    ap.add_argument('--callback-host', default='127.0.0.1')
    ap.add_argument('--callback-port', type=int, default=18081)
    ap.add_argument('--output-dir', default='/opt/app/dcc/team106/output')
    ap.add_argument('--requests', type=int, default=100)
    ap.add_argument('--concurrency', type=int, default=100)
    ap.add_argument('--timeout', type=int, default=7200)
    ap.add_argument('--seed', type=int, default=20260524)
    args = ap.parse_args()

    random.seed(args.seed)
    prefix = 'REQ_CB_%d' % int(time.time())
    state = CallbackState(prefix)
    server = ThreadingHTTPServer((args.callback_host, args.callback_port), make_handler(state))
    server_thread = threading.Thread(target=server.serve_forever, daemon=True)
    server_thread.start()
    print('callback_url=http://%s:%d/callback prefix=%s' % (args.callback_host, args.callback_port, prefix), flush=True)

    request_fields = [choose_fields() for _ in range(args.requests)]
    sm4_total = sum(sum(f in SM4_FIELDS for f in fields) for fields in request_fields)
    field_total = sum(len(fields) for fields in request_fields)
    print('field_total=%d sm4_total=%d sm4_ratio=%.3f seed=%d' %
          (field_total, sm4_total, float(sm4_total) / field_total, args.seed), flush=True)

    start = time.time()
    req_ids = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.concurrency) as pool:
        futs = [pool.submit(post, i, args.url, prefix, request_fields[i]) for i in range(args.requests)]
        for fut in concurrent.futures.as_completed(futs):
            req_id, elapsed = fut.result()
            req_ids.append(req_id)
            print('accepted %s %.3fs' % (req_id, elapsed), flush=True)
    accept_done = time.time()
    print('all accepted in %.3fs; waiting callbacks and outputs...' % (accept_done - start), flush=True)

    callback_elapsed = wait_callbacks(state, len(req_ids), start, args.timeout)
    output_elapsed = wait_outputs(req_ids, args.output_dir, start, args.timeout)
    print('completed callbacks %.3fs outputs %.3fs after_accept %.3fs' %
          (callback_elapsed, output_elapsed, time.time() - accept_done), flush=True)

    server.shutdown()


if __name__ == '__main__':
    main()
