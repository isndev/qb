#!/usr/bin/env python3
"""tcp_close_witness.py -- what the PEER of a TCP connection sees when WE end it, measured on this host's TCP stack.

Standard library only; runs unchanged on Linux, Windows and macOS:  python3 tcp_close_witness.py [--bound 1.0]

Two sockets over 127.0.0.1 in one process.  C is "us" (the side that ends the connection), P is the peer.
LAST is our last message (17 octets, the size of an HTTP/2 GOAWAY with no debug data).
Every case prints one line:  CASE  what P read of LAST  /  what P's next recv said  /  what P's sends said.
Nothing here is timed for performance; the sleeps only let the loopback deliver a FIN or a RST before the next step.
"""
import argparse
import errno
import platform
import socket
import struct
import sys
import threading
import time

LAST = b"LAST-BYTES-GOAWAY"  # 17 octets
SETTLE = 0.30  # seconds: a segment sent on loopback has arrived and been processed


def pair():
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.bind(("127.0.0.1", 0))
    srv.listen(1)
    p = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    p.connect(srv.getsockname())
    c, _ = srv.accept()
    srv.close()
    for s in (c, p):
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    return c, p


def err_name(e):
    n = getattr(e, "winerror", None) or e.errno
    return errno.errorcode.get(e.errno, str(n)) + ("/WSA%d" % e.winerror if getattr(e, "winerror", None) else "")


def recv_report(p, want=len(LAST), timeout=2.0):
    """One recv on P: returns a short description."""
    p.settimeout(timeout)
    try:
        d = p.recv(65536)
    except socket.timeout:
        return "TIMEOUT(no data, no EOF, no error)"
    except OSError as e:
        return "ERROR " + err_name(e)
    if d == b"":
        return "EOF"
    if d == LAST:
        return "LAST intact (%d)" % len(d)
    return "%d octets%s" % (len(d), " (LAST truncated!)" if d != LAST and LAST.startswith(d) else "")


def send_report(p, n=100):
    p.settimeout(2.0)
    try:
        p.sendall(b"x" * n)
        return "ok"
    except OSError as e:
        return "ERROR " + err_name(e)


def fill(p, seconds, chunk=65536):
    """P writes non-blocking chunks for `seconds`; returns (octets accepted, first error or None, time of first error,
    time of the first would-block, whether it was blocked at the end)."""
    p.setblocking(False)
    buf = b"y" * chunk
    total, first_err, t_err, t_block, blocked = 0, None, None, None, False
    t0 = time.monotonic()
    while time.monotonic() - t0 < seconds:
        try:
            total += p.send(buf)
            blocked = False
        except (BlockingIOError, InterruptedError):
            if t_block is None:
                t_block = time.monotonic() - t0
            blocked = True
            time.sleep(0.01)
        except OSError as e:
            first_err, t_err = err_name(e), time.monotonic() - t0
            break
    p.setblocking(True)
    return total, first_err, t_err, t_block, blocked


def line(case, *cols):
    print("%-34s | %s" % (case, " | ".join(cols)))
    sys.stdout.flush()


def k1_close_unread(variant):
    """C holds unread input and closes; P has LAST in its receive queue and reads it AFTER the RST arrived."""
    c, p = pair()
    p.sendall(b"u" * 4096)  # C never reads it
    time.sleep(SETTLE)
    c.sendall(LAST)
    time.sleep(SETTLE)  # LAST is in P's receive queue
    if variant == "close":
        c.close()
    elif variant == "shut_both+close":  # qb::io::socket::close(QB_SD_BOTH)
        c.shutdown(socket.SHUT_RDWR)
        c.close()
    elif variant == "shut_wr,wait,close":  # today: end_transport half-closes, the destructor closes later
        c.shutdown(socket.SHUT_WR)
        time.sleep(SETTLE)
        c.close()
    elif variant == "linger{1,0}+close":
        c.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("hh" if platform.system() == "Windows" else "ii", 1, 0))
        c.close()
    time.sleep(SETTLE)  # the RST (if any) has reached P
    r1 = recv_report(p)
    r2 = recv_report(p)
    s1 = send_report(p)
    time.sleep(SETTLE)
    s2 = send_report(p)
    line("K1 unread input, " + variant, "P recv#1: " + r1, "recv#2: " + r2, "send#1: " + s1, "send#2: " + s2)
    p.close()


def k1e_read_before_rst():
    """Same as K1/close, but P reads LAST BEFORE we close: the control (nothing can be lost)."""
    c, p = pair()
    p.sendall(b"u" * 4096)
    time.sleep(SETTLE)
    c.sendall(LAST)
    r1 = recv_report(p)
    c.close()
    time.sleep(SETTLE)
    r2 = recv_report(p)
    line("K1 control: P read LAST first", "P recv#1: " + r1, "recv#2: " + r2)
    p.close()


def k2_clean_close_then_peer_sends():
    """C has read everything and closes (FIN). P sends afterwards (C's stack answers RST), THEN reads."""
    c, p = pair()
    c.sendall(LAST)
    time.sleep(SETTLE)
    c.close()  # nothing unread: FIN
    time.sleep(SETTLE)
    s1 = send_report(p)  # data on a closed socket: RST comes back
    time.sleep(SETTLE)
    r1 = recv_report(p)
    r2 = recv_report(p)
    s2 = send_report(p)
    line("K2 clean close, peer sends, reads", "P send#1: " + s1, "recv#1: " + r1, "recv#2: " + r2, "send#2: " + s2)
    p.close()


def k3_halfclose_never_read(seconds):
    """Today's standalone dispose: shutdown(SEND) and nothing reads the socket again."""
    c, p = pair()
    c.sendall(LAST)
    c.shutdown(socket.SHUT_WR)
    r1 = recv_report(p)
    r2 = recv_report(p)
    total, e, t_err, t_block, blocked = fill(p, seconds)
    line("K3 half-close, never read (%.0fs)" % seconds, "P recv: " + r1 + ", " + r2,
         "P wrote %d octets, first would-block at %s" % (total, "%.3fs" % t_block if t_block is not None else "never"),
         "error: %s" % (e or "none"), "still blocked at the end: %s" % blocked)
    c.close()
    p.close()


def drainer(c, bound, out):
    """Candidate A/B's linger: read and drop until EOF, an error or the bound; then close."""
    t0 = time.monotonic()
    dropped, why = 0, "bound"
    c.settimeout(0.05)
    while time.monotonic() - t0 < bound:
        try:
            d = c.recv(65536)
        except socket.timeout:
            continue
        except OSError as e:
            why = "error " + err_name(e)
            break
        if d == b"":
            why = "EOF"
            break
        dropped += len(d)
    c.close()
    out.update(dropped=dropped, why=why, t=time.monotonic() - t0)


def k4_linger_peer_closes(bound):
    """Linger: shutdown(SEND), read and drop, close on EOF. The peer reads LAST, writes 1 MiB more, then closes."""
    c, p = pair()
    p.sendall(b"u" * 4096)
    c.sendall(LAST)
    c.shutdown(socket.SHUT_WR)
    out = {}
    th = threading.Thread(target=drainer, args=(c, bound, out))
    th.start()
    r1 = recv_report(p)
    r2 = recv_report(p)
    s = "ok"
    try:
        p.settimeout(2.0)
        p.sendall(b"m" * (1 << 20))
    except OSError as e:
        s = "ERROR " + err_name(e)
    t_close = time.monotonic()
    p.close()
    th.join()
    line("K4 linger, peer closes", "P recv: " + r1 + ", " + r2, "P 1 MiB send: " + s,
         "C ended by %s after %.3fs, dropped %d" % (out["why"], out["t"], out["dropped"]))


def k5_linger_bound(bound):
    """Linger against a peer that never closes and keeps writing: the bound ends it."""
    c, p = pair()
    c.sendall(LAST)
    c.shutdown(socket.SHUT_WR)
    out = {}
    th = threading.Thread(target=drainer, args=(c, bound, out))
    th.start()
    r1 = recv_report(p)
    r2 = recv_report(p)
    total, e, t_err, t_block, blocked = fill(p, bound + 3.0)
    th.join()
    line("K5 linger, peer keeps writing", "P recv: " + r1 + ", " + r2,
         "C ended by %s after %.3fs, dropped %d" % (out["why"], out["t"], out["dropped"]),
         "P wrote %d, first error %s at %s" % (total, e, "%.3fs" % t_err if t_err is not None else "never"),
         "would-block seen: %s" % ("%.3fs" % t_block if t_block is not None else "never"))
    p.close()


def k6_solinger_n(seconds):
    """SO_LINGER {1, N}: close() on a NON-BLOCKING socket, unread input present, and with none."""
    for unread in (True, False):
        c, p = pair()
        if unread:
            p.sendall(b"u" * 4096)
            time.sleep(SETTLE)
        c.sendall(LAST)
        time.sleep(SETTLE)
        c.setblocking(False)
        c.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("hh" if platform.system() == "Windows" else "ii", 1, seconds))
        t0 = time.monotonic()
        try:
            c.close()
            res = "returned"
        except OSError as e:
            res = "ERROR " + err_name(e)
        dt = time.monotonic() - t0
        time.sleep(SETTLE)
        r1 = recv_report(p)
        r2 = recv_report(p)
        line("K6 SO_LINGER{1,%d} %s" % (seconds, "unread input" if unread else "no unread input"),
             "close() %s in %.3fs" % (res, dt), "P recv#1: " + r1, "recv#2: " + r2)
        p.close()


def k7_shut_rd(how):
    """shutdown(RD) / shutdown(RDWR) without close, then the peer sends."""
    c, p = pair()
    c.sendall(LAST)
    time.sleep(SETTLE)
    c.shutdown(how)
    time.sleep(SETTLE)
    s1 = send_report(p)
    time.sleep(SETTLE)
    r1 = recv_report(p, timeout=1.0)
    r2 = recv_report(p, timeout=1.0)
    s2 = send_report(p)
    line("K7 shutdown(%s), peer sends" % ("RD" if how == socket.SHUT_RD else "RDWR"), "P send#1: " + s1, "recv#1: " + r1,
         "recv#2: " + r2, "send#2: " + s2)
    c.close()
    p.close()


def k8_drain_now_then_close():
    """Candidate D: read what is readable now, then close (no timer). The peer sends again afterwards."""
    c, p = pair()
    p.sendall(b"u" * 4096)
    time.sleep(SETTLE)
    c.sendall(LAST)
    c.setblocking(False)
    dropped = 0
    while True:
        try:
            d = c.recv(65536)
        except (BlockingIOError, InterruptedError):
            break
        if d == b"":
            break
        dropped += len(d)
    c.close()  # FIN: nothing unread
    time.sleep(SETTLE)
    s1 = send_report(p)  # late data: RST
    time.sleep(SETTLE)
    r1 = recv_report(p)
    r2 = recv_report(p)
    line("K8 drain now (%d), close, peer sends" % dropped, "P send#1: " + s1, "recv#1: " + r1, "recv#2: " + r2)
    p.close()


def k9_peer_reads_after_fin_then_rst():
    """Linger ended by the bound while the peer has LAST unread and data in flight: the worst case of A/B."""
    c, p = pair()
    c.sendall(LAST)
    c.shutdown(socket.SHUT_WR)
    time.sleep(SETTLE)
    p.sendall(b"u" * 4096)  # arrives after the FIN; C leaves it unread
    time.sleep(SETTLE)
    c.close()  # the bound: close with unread input -> RST
    time.sleep(SETTLE)
    r1 = recv_report(p)
    r2 = recv_report(p)
    line("K9 bound hits, input unread, P late", "P recv#1: " + r1, "recv#2: " + r2)
    p.close()


def k10_fin_then_rst_peer_sends_first():
    """Today's destructor path seen from a peer that WRITES before it reads: FIN (half-close), then the reset of a
    close with unread input, then the peer sends, then reads."""
    c, p = pair()
    p.sendall(b"u" * 4096)  # C never reads it
    time.sleep(SETTLE)
    c.sendall(LAST)
    c.shutdown(socket.SHUT_WR)
    time.sleep(SETTLE)
    c.close()  # unread input: RST, after the FIN
    time.sleep(SETTLE)
    s1 = send_report(p)
    time.sleep(SETTLE)
    r1 = recv_report(p)
    r2 = recv_report(p)
    line("K10 FIN, RST, peer sends, reads", "P send#1: " + s1, "recv#1: " + r1, "recv#2: " + r2)
    p.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bound", type=float, default=1.0)
    ap.add_argument("--hold", type=float, default=6.0, help="seconds K3 keeps writing")
    a = ap.parse_args()
    print("# host: %s %s %s, python %s" % (platform.system(), platform.release(), platform.version(), platform.python_version()))
    k1e_read_before_rst()
    for v in ("close", "shut_both+close", "shut_wr,wait,close", "linger{1,0}+close"):
        k1_close_unread(v)
    k2_clean_close_then_peer_sends()
    k8_drain_now_then_close()
    k9_peer_reads_after_fin_then_rst()
    k10_fin_then_rst_peer_sends_first()
    k7_shut_rd(socket.SHUT_RD)
    k7_shut_rd(socket.SHUT_RDWR)
    k6_solinger_n(2)
    k4_linger_peer_closes(a.bound)
    k5_linger_bound(a.bound)
    k3_halfclose_never_read(a.hold)


if __name__ == "__main__":
    main()
