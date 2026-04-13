#!/usr/bin/env python3
"""
frank-netcard firmware test suite.

Usage:
    ./test.sh                              # basic tests only
    ./test.sh --wifi SSID,password         # include WiFi + network tests
    ./test.sh --wifi SSID,password --full  # full test including TLS + HTTP
"""

import argparse
import serial
import sys
import time

# ── Helpers ───────────────────────────────────────────────────────

class Modem:
    def __init__(self, port, baud):
        self.ser = serial.Serial(port, baud, timeout=2)
        time.sleep(0.1)
        self.ser.reset_input_buffer()

    def close(self):
        self.ser.close()

    def send(self, cmd):
        self.ser.reset_input_buffer()
        self.ser.write((cmd + "\r\n").encode())

    def readline(self, timeout=5):
        """Read one \\r\\n-terminated line. Returns stripped string or None on timeout."""
        self.ser.timeout = timeout
        line = self.ser.readline()
        if not line:
            return None
        return line.decode("ascii", errors="replace").strip()

    def read_raw(self, n, timeout=5):
        """Read exactly n raw bytes."""
        self.ser.timeout = timeout
        data = b""
        deadline = time.time() + timeout
        while len(data) < n and time.time() < deadline:
            chunk = self.ser.read(n - len(data))
            if chunk:
                data += chunk
        return data

    def command(self, cmd, timeout=20):
        """Send a command, collect response lines until OK or ERROR. Returns (ok, lines)."""
        self.send(cmd)
        lines = []
        while True:
            line = self.readline(timeout=timeout)
            if line is None:
                return False, lines + ["<TIMEOUT>"]
            if line == "OK":
                return True, lines
            if line.startswith("ERROR:"):
                return False, lines + [line]
            if line:
                lines.append(line)

    def send_data(self, sock_id, data):
        """Execute AT+SSEND flow. Returns True on SEND OK."""
        self.send(f"AT+SSEND={sock_id},{len(data)}")
        prompt = self.readline(timeout=5)
        if prompt != ">":
            return False
        self.ser.write(data if isinstance(data, bytes) else data.encode())
        result = self.readline(timeout=10)
        return result == "SEND OK"

    def recv_all(self, timeout=5):
        """Read all pending +SRECV events. Returns list of (id, bytes)."""
        results = []
        closed = False
        deadline = time.time() + timeout
        self.ser.timeout = 0.5
        while time.time() < deadline:
            line = self.ser.readline()
            if not line:
                # No data yet — keep waiting unless we already got some and
                # had a full idle interval (server done sending)
                if results:
                    break
                continue
            text = line.decode("ascii", errors="replace").strip()
            if text.startswith("+SRECV:"):
                parts = text[7:].split(",")
                sock_id = int(parts[0])
                length = int(parts[1])
                raw = self.read_raw(length, timeout=3)
                results.append((sock_id, raw))
            elif text.startswith("+SCLOSED:"):
                closed = True
                break
        return results, closed


# ── Test runner ───────────────────────────────────────────────────

passed = 0
failed = 0
skipped = 0

def test(name, ok, detail=""):
    global passed, failed
    if ok:
        passed += 1
        print(f"  PASS  {name}")
    else:
        failed += 1
        print(f"  FAIL  {name}  {detail}")

def skip(name, reason=""):
    global skipped
    skipped += 1
    print(f"  SKIP  {name}  {reason}")


# ── Tests ─────────────────────────────────────────────────────────

def test_basic(m):
    print("\n--- Basic Commands ---")

    ok, _ = m.command("AT")
    test("AT", ok)

    ok, lines = m.command("AT+VER")
    ver_ok = ok and any("+VER:frank-netcard," in l for l in lines)
    ver = lines[0] if lines else ""
    test("AT+VER", ver_ok, ver)

    ok, lines = m.command("AT+HEAP")
    heap = 0
    if ok and lines and lines[0].startswith("+HEAP:"):
        heap = int(lines[0].split(":")[1])
    test("AT+HEAP", ok and heap > 10000, f"{heap} bytes free")

    ok, lines = m.command("GARBAGE")
    test("Reject bad command", not ok)

    ok, lines = m.command("AT+NONEXIST")
    test("Reject unknown AT+ cmd", not ok)


def test_wifi_scan(m):
    print("\n--- WiFi Scan ---")

    ok, lines = m.command("AT+WSCAN", timeout=15)
    networks = [l for l in lines if l.startswith("+WSCAN:")]
    test("AT+WSCAN", ok, f"{len(networks)} networks found")
    for net in networks[:5]:
        fields = net[7:].split(",")
        print(f"         {fields[0]:30s}  rssi={fields[1]}  ch={fields[3]}")
    if len(networks) > 5:
        print(f"         ... and {len(networks) - 5} more")


def test_wifi_connect(m, ssid, password):
    print("\n--- WiFi Connect ---")

    ok, lines = m.command(f"AT+WJOIN={ssid},{password}", timeout=20)
    ip = ""
    if ok and lines:
        ip = lines[0]
    test("AT+WJOIN", ok, ip)
    if not ok:
        return False

    ok, lines = m.command("AT+WSTAT")
    connected = ok and any("CONNECTED" in l for l in lines)
    detail = lines[0] if lines else ""
    test("AT+WSTAT", connected, detail)

    return connected


def test_network(m):
    print("\n--- Network ---")

    ok, lines = m.command("AT+RESOLVE=example.com", timeout=10)
    ip = ""
    if ok and lines and lines[0].startswith("+RESOLVE:"):
        ip = lines[0].split(":")[1]
    test("AT+RESOLVE", ok, ip)

    # Resolve failure
    ok, _ = m.command("AT+RESOLVE=this.host.does.not.exist.invalid", timeout=10)
    test("AT+RESOLVE bad host", not ok)


def test_tcp(m):
    print("\n--- TCP Socket ---")

    ok, _ = m.command("AT+SOPEN=0,TCP,httpbin.org,80", timeout=15)
    test("AT+SOPEN TCP", ok)
    if not ok:
        return

    ok, lines = m.command("AT+SSTAT")
    has_sock = any("+SSTAT:0,TCP,CONNECTED" in l for l in lines)
    test("AT+SSTAT shows connected", ok and has_sock)

    request = "GET /get HTTP/1.1\r\nHost: httpbin.org\r\nConnection: close\r\n\r\n"
    sent = m.send_data(0, request)
    test("AT+SSEND", sent)

    if sent:
        chunks, closed = m.recv_all(timeout=10)
        total = sum(len(d) for _, d in chunks)
        test("Receive HTTP response", total > 0, f"{total} bytes in {len(chunks)} chunks")
        if chunks:
            first_bytes = chunks[0][1][:80].decode("ascii", errors="replace")
            got_200 = "200" in first_bytes
            test("HTTP 200 OK", got_200, first_bytes.split("\r\n")[0])

        # Wait for +SCLOSED if we didn't get it during recv
        if not closed:
            time.sleep(2)
            m.ser.reset_input_buffer()

    # Verify socket is gone
    ok, lines = m.command("AT+SSTAT")
    sock_gone = not any("+SSTAT:0," in l for l in lines)
    test("Socket closed by remote", ok and sock_gone)

    # Clean up in case socket is still lingering
    m.command("AT+SCLOSE=0", timeout=2)


def test_tls(m, tls_host=None, tls_port=443):
    print("\n--- TLS Socket ---")

    host = tls_host or "httpbin.org"
    port = tls_port

    ok, lines = m.command("AT+HEAP")
    heap = 0
    if ok and lines and lines[0].startswith("+HEAP:"):
        heap = int(lines[0].split(":")[1])
    print(f"         Heap: {heap} bytes free")
    if heap < 25000:
        skip("AT+SOPEN TLS", f"low memory ({heap} bytes, need 25000+)")
        return

    # DNS pre-check — helps diagnose DNS vs TLS issues
    ok, lines = m.command(f"AT+RESOLVE={host}", timeout=10)
    if ok and lines:
        for l in lines:
            if l.startswith("+RESOLVE:"):
                print(f"         DNS: {host} -> {l.split(':')[1]}")
    test(f"DNS resolve {host}", ok, " ".join(lines) if not ok else "")
    if not ok:
        return

    ok, lines = m.command(f"AT+SOPEN=1,TLS,{host},{port}", timeout=30)
    detail = " ".join(lines) if not ok else ""
    test("AT+SOPEN TLS", ok, detail)
    if not ok:
        return

    path = "/get" if host == "httpbin.org" else "/"
    request = f"GET {path} HTTP/1.1\r\nHost: {host}\r\nConnection: close\r\n\r\n"
    sent = m.send_data(1, request)
    test("AT+SSEND over TLS", sent)

    if sent:
        chunks, closed = m.recv_all(timeout=10)
        total = sum(len(d) for _, d in chunks)
        test("Receive HTTPS response", total > 0, f"{total} bytes in {len(chunks)} chunks")
        if chunks:
            first_bytes = chunks[0][1][:80].decode("ascii", errors="replace")
            got_http = "HTTP/" in first_bytes
            test("HTTPS response valid", got_http, first_bytes.split("\r\n")[0])

    time.sleep(1)
    m.command("AT+SCLOSE=1", timeout=3)


def test_udp(m):
    print("\n--- UDP Socket ---")

    ok, _ = m.command("AT+SOPEN=2,UDP,8.8.8.8,53", timeout=5)
    test("AT+SOPEN UDP", ok)
    if not ok:
        return

    # Send a minimal DNS query for example.com (A record)
    # Header: ID=0xABCD, flags=0x0100 (standard query), 1 question
    # Question: example.com, type A, class IN
    dns_query = (
        b"\xab\xcd"  # ID
        b"\x01\x00"  # Flags: standard query, recursion desired
        b"\x00\x01"  # 1 question
        b"\x00\x00\x00\x00\x00\x00"  # 0 answers, authority, additional
        b"\x07example\x03com\x00"  # QNAME
        b"\x00\x01"  # QTYPE: A
        b"\x00\x01"  # QCLASS: IN
    )
    sent = m.send_data(2, dns_query)
    test("AT+SSEND DNS query", sent)

    if sent:
        chunks, _ = m.recv_all(timeout=5)
        total = sum(len(d) for _, d in chunks)
        dns_ok = total > 12  # minimal DNS response is >12 bytes
        test("Receive DNS response", dns_ok, f"{total} bytes")
        if chunks and len(chunks[0][1]) >= 12:
            resp = chunks[0][1]
            resp_id = (resp[0] << 8) | resp[1]
            test("DNS response ID matches", resp_id == 0xABCD, f"0x{resp_id:04x}")

    m.command("AT+SCLOSE=2", timeout=3)


def test_error_cases(m):
    print("\n--- Error Handling ---")

    # Ensure all sockets are closed first
    for i in range(4):
        m.command(f"AT+SCLOSE={i}", timeout=2)
    m.ser.reset_input_buffer()

    ok, _ = m.command("AT+SOPEN=9,TCP,example.com,80")
    test("Reject invalid socket ID", not ok)

    ok, _ = m.command("AT+SCLOSE=0")
    test("Reject close on unused socket", not ok)

    ok, _ = m.command("AT+SSEND=0,10")
    test("Reject send on closed socket", not ok)

    ok, _ = m.command("AT+SOPEN=0,QUIC,example.com,443")
    test("Reject unknown socket type", not ok)


def test_wifi_disconnect(m):
    print("\n--- WiFi Disconnect ---")

    ok, _ = m.command("AT+WQUIT")
    test("AT+WQUIT", ok)

    ok, lines = m.command("AT+WSTAT")
    disconnected = ok and any("DISCONNECTED" in l for l in lines)
    test("AT+WSTAT shows disconnected", disconnected)

    ok, _ = m.command("AT+RESOLVE=example.com", timeout=5)
    test("Reject resolve without WiFi", not ok)


# ── Main ──────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="Test frank-netcard firmware")
    parser.add_argument("--port", required=True, help="Serial port")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--wifi", metavar="SSID,PASS", help="WiFi credentials for network tests")
    parser.add_argument("--full", action="store_true", help="Run TLS and UDP tests too")
    parser.add_argument("--tls-host", metavar="HOST[:PORT]", help="Custom TLS host (default: httpbin.org:443)")
    args = parser.parse_args()

    m = Modem(args.port, args.baud)

    print(f"Testing on {args.port} @ {args.baud}")

    try:
        test_basic(m)
        test_wifi_scan(m)

        if args.wifi:
            sep = args.wifi.index(",")
            ssid = args.wifi[:sep]
            pw = args.wifi[sep + 1:]

            connected = test_wifi_connect(m, ssid, pw)
            if connected:
                test_network(m)
                test_tcp(m)

                if args.full:
                    tls_host = None
                    tls_port = 443
                    if args.tls_host:
                        parts = args.tls_host.split(":")
                        tls_host = parts[0]
                        if len(parts) > 1:
                            tls_port = int(parts[1])
                    test_tls(m, tls_host=tls_host, tls_port=tls_port)
                    test_udp(m)

                test_error_cases(m)
                test_wifi_disconnect(m)
        else:
            skip("WiFi + network tests", "pass --wifi SSID,password to enable")
    finally:
        m.close()

    print(f"\n{'='*40}")
    print(f"  {passed} passed, {failed} failed, {skipped} skipped")
    print(f"{'='*40}")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
