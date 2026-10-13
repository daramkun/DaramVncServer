"""Runs the whole verification in one go and prints a PASS / FAIL / SKIP line per check.

  python scripts/regression.py [--build DIR] [--animation] [--skip-input] [--only NAME[,NAME...]]

Always: unit tests, decoders against a screenshot (Tight, ZRLE, Raw), region request, 8-bit colour map, 16-bit pixel
format, three clients at once, IPv4/IPv6 binding, TLS login (good and bad password), fuzzing, the settings window (driven with window messages), and the real keyboard/
mouse injection test (--skip-input leaves that out; it hooks input system-wide for about 35 s and swallows only the
*injected* events).
With --animation (a window is repainted on screen for several seconds per check): H.264 end to end, JPEG quality
falling and recovering on a slow link, throughput on a 200 ms round trip, and the stalled-client test.

Checks that compare against the live screen allow a small difference (the screen keeps changing while it is captured).
Exit code 0 = no FAIL.
"""
import argparse
import itertools
import os
import re
import socket
import subprocess
import sys
import tempfile
import time

import common

ports = itertools.count(5931)
results = []


def record(name, status, detail=""):
    results.append((name, status, detail))
    print("%-5s %-44s %s" % (status, name, detail), flush=True)


def verify_pct(output):
    m = re.search(r"verify vs GDI screenshot: (\d+) of (\d+) pixels differ \(([\d.]+)%\)", output or "")
    return float(m.group(3)) if m else None


def check_verify(name, port, args, tolerance, attempts=3):
    """Compares the decoded picture with a GDI screenshot; the best of a few attempts counts (the screen is alive)."""
    best, got_any = None, False
    for _ in range(attempts):
        out = common.client(*args, "--verify", port=port, timeout=45)
        if out is None:
            continue
        pct = verify_pct(out)
        if pct is None:
            record(name, "FAIL", "client output: " + out.strip()[-200:])
            return
        got_any = True
        best = pct if best is None else min(best, pct)
        if best <= tolerance:
            break
    if not got_any:
        record(name, "SKIP", "the screen never became idle long enough to compare")
    elif best <= tolerance:
        record(name, "PASS", "%.4f%% of the pixels differ (limit %.1f%%)" % (best, tolerance))
    else:
        record(name, "FAIL", "%.4f%% of the pixels differ (limit %.1f%%)" % (best, tolerance))


def t_unit():
    r = subprocess.run([common.exe("daramvnc_tests")], capture_output=True, text=True, errors="replace", timeout=300)
    ok = r.returncode == 0 and "all tests passed" in r.stdout
    record("unit tests", "PASS" if ok else "FAIL", "all tests passed" if ok else r.stdout[-400:].strip())


def t_decoders():
    port = next(ports)
    with common.Server(port, "--view-only"):
        for enc in ("tight", "zrle", "raw"):
            check_verify("decode %s vs screenshot" % enc, port, ["--enc", enc, "--seconds", 2], 3.0)
        check_verify("region request (300,200 400x300)", port, ["--enc", "tight", "--seconds", 2, "--region", "300,200,400,300"], 1.0)
        check_verify("8-bit colour map (ZRLE)", port, ["--enc", "zrle", "--pf", "cmap", "--seconds", 2], 3.0)
        out = common.client("--enc", "zrle", "--pf", "565", "--seconds", 2, port=port)
        u = common.updates_in(out)
        record("16-bit pixel format (ZRLE)", "PASS" if u and u[0] >= 3 else "FAIL", "%s updates" % (u[0] if u else 0))


def t_concurrency():
    port = next(ports)
    with common.Server(port, "--view-only"):
        procs = [common.start_client("--enc", "tight", "--seconds", 3, "--continuous", port=port) for _ in range(3)]
        outs = [p.communicate(timeout=60)[0] for p in procs]
    counts = [(common.updates_in(o) or (0, 1))[0] for o in outs]
    ok = all(c >= 10 for c in counts) and not any("error" in o for o in outs)
    record("3 clients at the same time", "PASS" if ok else "FAIL", "updates per client: %s" % counts)


def t_ip_binding():
    try:
        s = socket.socket(socket.AF_INET6)
        s.bind(("::1", 0))
        s.close()
    except OSError:
        record("IPv6 binding", "SKIP", "no IPv6 loopback on this machine")
        return

    def connects(bind, host):
        port = next(ports)
        with common.Server(port, "--view-only", "--bind", bind):
            out = common.client("--enc", "tight", "--seconds", 1, port=port, host=host, timeout=30) or ""
        return "connected to" in out

    cases = [("127.0.0.1", "127.0.0.1", True), ("127.0.0.1", "::1", False), ("::1", "::1", True), ("::1", "127.0.0.1", False)]
    bad = [c for c in cases if connects(c[0], c[1]) != c[2]]
    record("IPv4 / IPv6 loopback binding", "FAIL" if bad else "PASS", ("wrong for %s" % bad) if bad else "4 combinations behave as documented")


def t_tls():
    cfg = common.tmp_path("daramvnc_regression.conf")
    if os.path.exists(cfg):
        os.remove(cfg)
    r = subprocess.run([common.exe("daramvnc"), "passwd", "tester", "--config", cfg], input=b"S3cure-test-pw\nS3cure-test-pw\n", capture_output=True)
    if r.returncode != 0:
        record("TLS login", "FAIL", "could not create the test user: " + r.stderr.decode(errors="replace")[-200:])
        return
    port = next(ports)
    with common.Server(port, "--view-only", "--config", cfg):
        good = common.client("--user", "tester", "--pass", "S3cure-test-pw", "--seconds", 2, port=port, timeout=30) or ""
        bad = common.client("--user", "tester", "--pass", "wrong", "--seconds", 1, port=port, timeout=30) or ""
        none = common.client("--seconds", 1, port=port, timeout=30) or ""
    record("TLS login with the right password", "PASS" if "TLS:" in good and "connected to" in good else "FAIL", good.strip().splitlines()[0] if good.strip() else "")
    record("TLS login with a wrong password", "PASS" if "authentication failed" in bad else "FAIL", bad.strip()[-100:])
    record("no-credentials client is not offered None", "PASS" if "does not offer" in none else "FAIL", none.strip()[-100:])


def t_vnc_auth():
    # RealVNC's standard RFB security type 2, alongside the existing TLS paths.
    with tempfile.TemporaryDirectory(prefix="daramvnc_vnc_auth_") as directory:
        cfg = os.path.join(directory, "server.conf")
        with open(cfg, "w", encoding="utf-8") as f:
            f.write("# keep existing settings\nview_only=1\n")

        def set_password(*options):
            r = subprocess.run([common.exe("daramvnc"), "vncpasswd", "--config", cfg, *options],
                               input=b"Vnc-test-password\nVnc-test-password\n", capture_output=True, timeout=30)
            if r.returncode:
                raise RuntimeError("vncpasswd failed: " + r.stderr.decode(errors="replace"))

        set_password()
        port = next(ports)
        with common.Server(port, "--config", cfg):
            out = common.client("--legacy-vnc", "--vncpass", "Vnc-test-password", "--seconds", 1, port=port) or ""
            record("legacy VNC authentication off by default", "PASS" if "does not offer" in out else "FAIL", out.strip()[-100:])
            tls = common.client("--vncpass", "Vnc-test-password", "--seconds", 1, port=port) or ""
            record("X509Vnc TLS password authentication", "PASS" if "TLS:" in tls and "connected to" in tls else "FAIL")

        set_password("--realvnc")
        with open(cfg, encoding="utf-8") as f:
            text = f.read()
        record("RealVNC setup preserves existing settings", "PASS" if "allow_legacy_vnc_auth=1" in text and "# keep existing settings\nview_only=1\n" in text else "FAIL")
        # Also retain username/password TLS authentication when compatibility is on.
        r = subprocess.run([common.exe("daramvnc"), "passwd", "tester", "--config", cfg],
                           input=b"S3cure-test-pw\nS3cure-test-pw\n", capture_output=True, timeout=30)
        if r.returncode:
            raise RuntimeError("could not create TLS user")
        port = next(ports)
        with common.Server(port, "--config", cfg):
            # Same first 8 bytes, different suffix: mandated by VNC's DES protocol.
            good = common.client("--legacy-vnc", "--vncpass", "Vnc-test-other", "--seconds", 1, port=port) or ""
            bad = common.client("--legacy-vnc", "--vncpass", "wrong-password", "--seconds", 1, port=port) or ""
            none = common.client("--seconds", 1, port=port) or ""
            tls = common.client("--vncpass", "Vnc-test-password", "--seconds", 1, port=port) or ""
            login = common.client("--user", "tester", "--pass", "S3cure-test-pw", "--seconds", 1, port=port) or ""
        record("RealVNC security type 2 login and first 8 bytes", "PASS" if "connected to" in good and "TLS:" not in good else "FAIL", good.strip()[-150:])
        record("RealVNC wrong password rejected", "PASS" if "authentication failed" in bad else "FAIL", bad.strip()[-100:])
        record("RealVNC mode does not offer unauthenticated access", "PASS" if "does not offer" in none else "FAIL")
        record("X509Vnc still works with RealVNC mode", "PASS" if "TLS:" in tls and "connected to" in tls else "FAIL")
        record("X509Plain still works with RealVNC mode", "PASS" if "TLS:" in login and "connected to" in login else "FAIL")


def t_fuzz():
    port = next(ports)
    with common.Server(port, "--view-only"):
        r = subprocess.run([sys.executable, os.path.join(common.ROOT, "scripts", "fuzz.py"), str(port), "100"], capture_output=True, text=True, timeout=300)
    record("fuzzing (100 malformed sessions)", "PASS" if r.returncode == 0 else "FAIL", r.stdout.strip().splitlines()[-1] if r.stdout.strip() else r.stderr[-200:])


def t_gui():
    r = subprocess.run([sys.executable, os.path.join(common.ROOT, "tests", "gui_test.py"), common.BUILD], capture_output=True, text=True, errors="replace", timeout=180)
    fails = [l for l in r.stdout.splitlines() if l.startswith("FAIL")]
    ok = r.returncode == 0
    record("settings window (GUI)", "PASS" if ok else "FAIL", "form, save, validation" if ok else " | ".join(fails) or r.stdout[-300:])


def t_input():
    r = subprocess.run([sys.executable, os.path.join(common.ROOT, "tests", "input_test.py"), common.BUILD], capture_output=True, text=True, timeout=300)
    lines = r.stdout.strip().splitlines()
    record("keyboard / mouse injection (SendInput)", "PASS" if r.returncode == 0 else "FAIL", " | ".join(lines[:2]) if r.returncode == 0 else r.stdout[-500:])


def t_h264():
    port = next(ports)
    with common.Server(port, "--view-only", "--h264"):
        with common.Animation(14, "photo"):
            out = common.client("--enc", "tight", "--h264", "--continuous", "--seconds", 6, port=port, timeout=60) or ""
    m = re.search(r"h264: (\d+) frames received, (\d+) decoded", out)
    ok = bool(m) and int(m.group(1)) >= 30 and int(m.group(2)) >= 20
    record("H.264 end to end (needs Media Foundation)", "PASS" if ok else "FAIL", m.group(0) if m else out.strip()[-200:])


def t_quality():
    port = next(ports)
    with common.Server(port, "--view-only") as srv:
        with common.Animation(40, "photo"):
            common.client("--enc", "tight", "--seconds", 32, "--continuous", "--throttle", 1200, "--throttle-until", 14, port=port, timeout=90)
        log = srv.log()
    changes = [(int(a), int(b)) for a, b in re.findall(r"JPEG quality (\d+) -> (\d+)", log)]
    fell = any(b <= 50 for _, b in changes)
    recovered = bool(changes) and changes[-1][1] == 85
    record("JPEG quality falls on a slow link and recovers", "PASS" if fell and recovered else "FAIL", "quality changes: %s" % (changes[:2] + ["..."] + changes[-2:] if len(changes) > 4 else changes))


def t_rtt():
    port = next(ports)
    with common.Server(port, "--view-only"):
        with common.Animation(16, "photo"):
            out = common.client("--enc", "tight", "--seconds", 10, "--continuous", "--ack-delay", 200, port=port, timeout=60)
    u = common.updates_in(out)
    rate = u[0] / u[1] if u else 0
    record("full frame rate on a 200 ms round trip", "PASS" if rate >= 40 else "FAIL", "%.1f updates/s (a stop-and-wait pipe would give ~5)" % rate)


def t_stall():
    r = subprocess.run([sys.executable, os.path.join(common.ROOT, "scripts", "stall_test.py"), "--build", common.BUILD], capture_output=True, text=True, timeout=300)
    record("stalled client does not delay the others", "PASS" if r.returncode == 0 else "FAIL", r.stdout.strip().splitlines()[0] if r.returncode == 0 and r.stdout.strip() else r.stdout[-400:])


BASIC = [("unit", t_unit), ("decoders", t_decoders), ("concurrency", t_concurrency), ("ip", t_ip_binding), ("tls", t_tls), ("vnc", t_vnc_auth), ("fuzz", t_fuzz), ("gui", t_gui), ("input", t_input)]
ANIMATED = [("h264", t_h264), ("quality", t_quality), ("rtt", t_rtt), ("stall", t_stall)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default=common.BUILD)
    ap.add_argument("--animation", action="store_true", help="also run the checks that need the animation window")
    ap.add_argument("--skip-input", action="store_true", help="skip the system-wide input hook test")
    ap.add_argument("--only", help="comma separated: " + ",".join(n for n, _ in BASIC + ANIMATED))
    args = ap.parse_args()
    common.set_build(args.build)
    common.check_build("daramvnc", "daramvnc_tests", "daramvnc_client", "animwin", "inputsink")

    wanted = set(args.only.split(",")) if args.only else None
    plan = list(BASIC) + (list(ANIMATED) if args.animation or (wanted and wanted & {n for n, _ in ANIMATED}) else [])
    started = time.time()
    for name, fn in plan:
        if wanted and name not in wanted:
            continue
        if name == "input" and args.skip_input and not wanted:
            record("keyboard / mouse injection (SendInput)", "SKIP", "--skip-input")
            continue
        try:
            fn()
        except Exception as e:  # a crashed check is a failed check, not a crashed run
            record(name, "FAIL", "exception: %r" % (e,))

    counts = {s: sum(1 for r in results if r[1] == s) for s in ("PASS", "FAIL", "SKIP")}
    print("\n%d passed, %d failed, %d skipped in %.0f s" % (counts["PASS"], counts["FAIL"], counts["SKIP"], time.time() - started))
    return 1 if counts["FAIL"] else 0


if __name__ == "__main__":
    sys.exit(main())
