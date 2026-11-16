#!/usr/bin/env python3
"""Local Hardware-in-the-Loop (HIL) smoke test for QHB Gateway."""

import argparse
import socket
import subprocess
import sys
import urllib.request


def check_tcp_port(host: str, port: int, timeout: float = 3.0) -> bool:
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False


def check_ssh(host: str, key: str | None) -> bool:
    cmd = ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5"]
    if key:
        cmd.extend(["-i", key])
    cmd.extend([f"root@{host}", "mountpoint -q /data && uname -a"])
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode == 0:
        print(f"[PASS] SSH + /data mount: {res.stdout.strip()}")
        return True
    print(f"[FAIL] SSH check failed: {res.stderr.strip()}")
    return False


def main() -> int:
    parser = argparse.ArgumentParser(description="QHB Gateway HIL smoke test")
    parser.add_argument("--host", default="192.168.0.5", help="Board IP/hostname")
    parser.add_argument("--ssh-key", default=None, help="Path to SSH private key")
    parser.add_argument("--power-webhook", default=None, help="Optional HA webhook URL to power-cycle")
    parser.add_argument("--ports", default="22,2002,3008,6053", help="Comma-separated TCP ports to probe")
    args = parser.parse_args()

    if args.power_webhook:
        print(f"[INFO] Triggering power-cycle webhook...")
        req = urllib.request.Request(args.power_webhook, method="POST")
        with urllib.request.urlopen(req, timeout=10):
            pass

    ok = True
    for raw_port in args.ports.split(","):
        port = int(raw_port.strip())
        if check_tcp_port(args.host, port):
            print(f"[PASS] TCP {args.host}:{port} reachable")
        else:
            print(f"[FAIL] TCP {args.host}:{port} unreachable")
            ok = False

    if not check_ssh(args.host, args.ssh_key):
        ok = False

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
