#!/usr/bin/env python3
"""Runs the scripted echo_server session that scripts/trace.sh records by default."""

import socket
import sys
import time

PORT = 7000
STEP = 0.1
CONNECT_TIMEOUT = 5
START = time.monotonic()


def log(text):
    print(f"[{time.monotonic() - START:5.2f}s] {text}", flush=True)


def connect(tag):
    while time.monotonic() - START < CONNECT_TIMEOUT:
        try:
            client = socket.create_connection(("127.0.0.1", PORT), timeout=CONNECT_TIMEOUT)
        except OSError:
            time.sleep(STEP)
            continue
        log(f"{tag} connect")
        return client
    sys.exit(f"cannot connect to 127.0.0.1:{PORT} (is something else listening?)")


def say(client, tag, message):
    log(f'{tag} > "{message}"')
    client.sendall(message.encode())
    echo = client.recv(4096).decode()
    if echo != message:
        sys.exit(f"{tag}: echo mismatch, sent {message!r} got {echo!r}")
    time.sleep(STEP)


def main():
    c1 = connect("c1")
    say(c1, "c1", "Hello World")
    c2 = connect("c2")
    c3 = connect("c3")
    say(c2, "c2", "c2 #1")
    say(c3, "c3", "c3 #1")
    say(c2, "c2", "c2 #2")
    say(c3, "c3", "c3 #2")
    say(c1, "c1", "end")
    say(c2, "c2", "end")
    c1.close()
    c2.close()
    say(c3, "c3", "end")
    c3.close()
    print("3 connections, 8 messages, all echoed")


if __name__ == "__main__":
    main()
