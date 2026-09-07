"""Regression probe for the MAX_OPEN_SOCKETS bug fixed in c684840.

The defect: ws_clients() sized its enumeration array by MAX_WS_CLIENTS, but
httpd_get_client_list() fails outright when that array cannot hold EVERY open socket the
server is tracking - which includes plain HTTP requests, not just websockets. So a plain GET
to /id, arriving while MAX_WS_CLIENTS websockets were already open, pushed the open-socket
count one past what ws_clients() had room for, enumeration failed, /id reported clients:0,
and sink_send() silently dropped everything as "no client" - a transport failure invisible on
the serial console, and the wrong kind of failure to leave undetected: SPEC 12.2 makes
telemetry best-effort, but a socket accounting bug is not the frame drop that policy permits.

This holds the server at that same shape and asks the same two questions the original ad hoc
probe answered before the fix, at whatever MAX_WS_CLIENTS/MAX_OPEN_SOCKETS the current build
compiles - printed below so a change to either constant in net_transport.c is visible here
too, not just in the diff.

    python tools/probe/socket_pressure.py [--host IP] [--clients N]

--clients defaults to 4 (MAX_WS_CLIENTS in src/net_transport.c as of this writing). Pass the
current value explicitly if that constant has changed and this script hasn't caught up.
"""
import argparse
import json
import os
import sys
import threading
import time
import urllib.error
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ws_client import WS  # noqa: E402


def get_id(host, timeout=8.0):
    with urllib.request.urlopen("http://%s/id" % host, timeout=timeout) as r:
        return json.loads(r.read().decode())


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default="192.168.4.1")
    p.add_argument("--clients", type=int, default=4, help="MAX_WS_CLIENTS at build time (default 4)")
    p.add_argument("--extra-sockets", type=int, default=3,
                   help="plain HTTP requests to hold open concurrently with the websockets "
                        "(default 3 = the MAX_OPEN_SOCKETS headroom over MAX_WS_CLIENTS)")
    args = p.parse_args()
    n = args.clients

    print("opening %d websocket(s) against %s ..." % (n, args.host))
    socks = []
    try:
        for _ in range(n):
            socks.append(WS(args.host, 80, "/ws"))
    except OSError as e:
        for s in socks:
            s.close()
        raise SystemExit("could not reach %s:80 (%s). Is this machine joined to the board's AP?" % (args.host, e))
    print("websockets open        : %d of %d" % (len(socks), n))

    # Put the server at its socket limit BEFORE asking the websockets to speak - that ordering
    # is what the original bug needed: enumeration has to fail while a reply is due, not before
    # or after. `--extra-sockets` concurrent plain requests plus the sockets above approximate
    # a client at MAX_OPEN_SOCKETS.
    results = {}

    def hold_and_get(i):
        try:
            results[i] = get_id(args.host)
        except (urllib.error.URLError, OSError) as e:
            results[i] = {"error": str(e)}

    threads = [threading.Thread(target=hold_and_get, args=(i,)) for i in range(args.extra_sockets)]
    for t in threads:
        t.start()

    replies = {}

    def collect(i, sock):
        sock.send_text({"cmd": "GET_CURRENT_STATE", "seq": 200 + i})
        deadline = time.time() + 5.0
        while time.time() < deadline:
            frame = sock.recv()
            if frame is None:
                continue
            try:
                f = json.loads(frame)
            except ValueError:
                continue
            if f.get("t") == "state":
                replies[i] = True
                return

    collectors = [threading.Thread(target=collect, args=(i, s)) for i, s in enumerate(socks)]
    for c in collectors:
        c.start()
    for c in collectors:
        c.join()
    for t in threads:
        t.join()

    reported = results.get(0, {})
    clients_seen = reported.get("clients", "?")
    print("/id clients (concurrent request) : %s (error: %s)" % (clients_seen, reported.get("error")))
    print("replies while at socket limit     : %d of %d" % (len(replies), n))

    for s in socks:
        s.close()

    pass_ = len(replies) >= n and clients_seen == n
    print("")
    if pass_:
        print("PASS  clients stayed visible and answerable at the server's socket limit")
    else:
        print("FAIL  clients went invisible or unanswerable under socket pressure "
              "(this is the c684840 regression if it reappears)")
    return 0 if pass_ else 1


if __name__ == "__main__":
    sys.exit(main())
