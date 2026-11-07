#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Checks chromiumPlus in a built browser, using only the Python standard library.

Run from the source root:
  python chrome/test/chromium_plus_smoke_test.py --chrome-binary out/Debug/chrome.exe
"""

import argparse
import functools
import http.server
import json
import os
import pathlib
import queue
import subprocess
import tempfile
import threading
import time
import urllib.parse


class TestRequestHandler(http.server.SimpleHTTPRequestHandler):

  def __init__(self, *args, results, verbose, **kwargs):
    self.results = results
    self.verbose = verbose
    super().__init__(*args, **kwargs)

  def end_headers(self):
    self.send_header('Cache-Control', 'no-store')
    if 'csp' in urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query):
      self.send_header('Content-Security-Policy', "script-src 'self'")
    super().end_headers()

  def do_POST(self):
    if self.path not in ('/results', '/progress'):
      self.send_error(404)
      return
    size = int(self.headers.get('Content-Length', '0'))
    if not 0 < size <= 1024 * 1024:
      self.send_error(400)
      return
    results = json.loads(self.rfile.read(size))
    self.send_response(200)
    self.send_header('Content-Length', '2')
    self.end_headers()
    self.wfile.write(b'OK')
    self.wfile.flush()
    self.results.put(results)

  def log_message(self, format_string, *args):
    if self.verbose:
      print(format_string % args, flush=True)


def main():
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument('--chrome-binary', required=True, type=pathlib.Path)
  parser.add_argument('--timeout', type=float, default=120)
  parser.add_argument('--output', type=pathlib.Path)
  parser.add_argument('--verbose', action='store_true')
  parser.add_argument('--chrome-arg', action='append', default=[])
  args = parser.parse_args()
  binary = args.chrome_binary.resolve(strict=True)
  fixtures = pathlib.Path(__file__).parent / 'data' / 'chromium_plus'
  results_queue = queue.Queue()
  handler = functools.partial(TestRequestHandler,
                              directory=str(fixtures),
                              results=results_queue,
                              verbose=args.verbose)
  with http.server.ThreadingHTTPServer(('127.0.0.1', 0), handler) as server:
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    with tempfile.TemporaryDirectory(prefix='chromium-plus-smoke-',
                                     ignore_cleanup_errors=True) as temp_dir:
      profile = pathlib.Path(temp_dir) / 'profile'
      log_path = pathlib.Path(temp_dir) / 'chrome.log'
      command = [
          str(binary), '--headless', '--disable-gpu', '--no-first-run',
          '--no-default-browser-check', '--disable-background-networking',
          '--disable-popup-blocking', '--no-proxy-server', '--site-per-process',
          '--disable-background-timer-throttling',
          '--disable-backgrounding-occluded-windows',
          '--disable-renderer-backgrounding', '--remote-debugging-port=0',
          f'--user-data-dir={profile}',
          *args.chrome_arg,
          f'http://127.0.0.1:{server.server_port}/index.html'
      ]
      results = None
      with log_path.open('w', encoding='utf-8') as log:
        process = subprocess.Popen(
            command,
            stdout=log,
            stderr=subprocess.STDOUT,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
        try:
          deadline = time.monotonic() + args.timeout
          while time.monotonic() < deadline:
            try:
              report = results_queue.get(timeout=1)
              if isinstance(report, dict):
                if args.verbose:
                  status = 'PASS' if report.get('passed') else 'FAIL'
                  print(f"{status}: {report['name']} "
                        f"{report.get('error', '')}".rstrip(), flush=True)
                continue
              results = report
              break
            except queue.Empty:
              if process.poll() is not None:
                break
        finally:
          if process.poll() is None:
            if os.name == 'nt':
              subprocess.run(['taskkill', '/PID', str(process.pid), '/T', '/F'],
                             stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL,
                             check=False)
            else:
              process.terminate()
            process.wait(timeout=15)
          server.shutdown()
          thread.join(timeout=5)
      if results is None:
        print(log_path.read_text(encoding='utf-8', errors='replace')[-6000:])
        raise RuntimeError('Browser exited or timed out before reporting results')
  if args.output:
    args.output.write_text(json.dumps(results, indent=2) + '\n', encoding='utf-8')
  for result in results:
    status = 'PASS' if result.get('passed') else 'FAIL'
    print(f"{status}: {result['name']} {result.get('error', '')}".rstrip())
  passed = sum(result.get('passed', False) for result in results)
  print(f'{passed}/{len(results)} checks passed')
  return 0 if results and passed == len(results) else 1


if __name__ == '__main__':
  raise SystemExit(main())
