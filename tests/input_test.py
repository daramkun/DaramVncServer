"""Real input-injection test: scripted RFB key/pointer events -> server (InputInjector/SendInput) -> low-level hooks.

tools/inputsink.exe installs WH_KEYBOARD_LL / WH_MOUSE_LL hooks, writes every *injected* event to a log and swallows it,
so nothing reaches an application and the real cursor does not move (the user's own input is not touched).

usage: python tests/input_test.py <build dir>      (e.g. build)
Exit code 0 = all checks passed.
"""
import os
import re
import subprocess
import sys
import time

BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "build")
PORT = 5925
TMP = os.environ.get("TEMP", ".")


def parse(path):
    events = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            m = re.match(r"K (down|up)\s+unicode=U\+([0-9A-F]+)", line)
            if m:
                events.append({"k": "K", "state": m.group(1), "unicode": int(m.group(2), 16)})
                continue
            m = re.match(r"K (down|up)\s+vk=0x([0-9A-F]+) scan=0x([0-9A-F]+) ext=(\d)", line)
            if m:
                events.append({"k": "K", "state": m.group(1), "vk": int(m.group(2), 16), "scan": int(m.group(3), 16), "ext": int(m.group(4))})
                continue
            m = re.match(r"M (.+?)\s+x=(-?\d+) y=(-?\d+) delta=(-?\d+)", line)
            if m:
                events.append({"k": "M", "what": m.group(1).strip(), "x": int(m.group(2)), "y": int(m.group(3)), "delta": int(m.group(4))})
    return events


def key(state, **kw):
    return dict(k="K", state=state, **kw)


def vk(state, v, ext=None):
    d = key(state, vk=v)
    if ext is not None:
        d["ext"] = ext
    return d


def sc(state, scan, ext=0):
    return key(state, scan=scan, ext=ext)


def uni(state, cp):
    return key(state, unicode=cp)


def pair(v, ext=None):
    return [vk("down", v, ext), vk("up", v, ext)]


def mouse(what, x=None, y=None, delta=None):
    d = {"k": "M", "what": what}
    if x is not None:
        d["x"], d["y"] = x, y
    if delta is not None:
        d["delta"] = delta
    return d


def matches(got, want, tol):
    for k, v in want.items():
        if k in ("x", "y"):
            if abs(got.get(k, 10 ** 9) - v) > tol:
                return False
        elif got.get(k) != v:
            return False
    return True


# ------------------------------------------------------------------ expectations
STD = (
    pair(0x41)                                                        # a
    + [vk("down", 0xA0)] + pair(0x41) + [vk("up", 0xA0)]              # Shift + A
    + pair(0x31)                                                      # 1
    + [uni("down", 0xD55C), uni("up", 0xD55C)]                        # Hangul syllable: Unicode injection
    + [vk("down", 0xA2)] + pair(0x43) + [vk("up", 0xA2)]              # Ctrl + C by virtual key
    + pair(0x0D, 0) + pair(0x25, 1) + pair(0x2E, 1) + pair(0x74, 0) + pair(0x65, 0) + pair(0x24, 1) + pair(0x2D, 1)  # Enter Left Del F5 KP5 Home Ins
    + [vk("down", 0xA4)] + pair(0x09) + [vk("up", 0xA4)]              # Alt + Tab
    + [vk("down", 0xA0)] + pair(0x32) + [vk("up", 0xA0)]              # Shift + @  (= Shift + 2)
    + [vk("down", 0x58)] * 3 + [vk("up", 0x58)]                       # auto-repeat of x
    + [mouse("move", 100, 200), mouse("move", 500, 300)]
    + [mouse("left down"), mouse("left up"), mouse("right down"), mouse("right up"), mouse("middle down"), mouse("middle up")]
    + [mouse("wheel", delta=120), mouse("wheel", delta=-120), mouse("hwheel", delta=-120), mouse("hwheel", delta=120)]
    + [mouse("move", "SCREEN_RIGHT_BOTTOM", "SCREEN_RIGHT_BOTTOM")]    # 100000,100000 is clamped to the last pixel
    + [vk("down", 0xA2), vk("down", 0x51), mouse("move", 10, 10), mouse("left down")]   # held when the client disconnects
)
STD_RELEASED = [vk("up", 0xA2), vk("up", 0x51), mouse("left up")]  # in any order

EXT = (
    pair(0x41)                                                        # a (printable, no shortcut): virtual key
    + [sc("down", 0x1D)]                                              # Ctrl by scancode
    + [sc("down", 0x2E), sc("up", 0x2E)]                              # Ctrl + C by scancode
    + [sc("up", 0x1D)]
    + [sc("down", 0x4B, 1), sc("up", 0x4B, 1)]                        # Left arrow: E0 4B
    + [uni("down", 0xD55C), uni("up", 0xD55C)]                        # no scancode for the character: Unicode
    + [sc("down", 0x2A)] + pair(0x41) + [sc("up", 0x2A)]              # Shift + A: Shift by scancode, letter by virtual key
    + [sc("down", 0x1D), sc("down", 0x10)]                            # held when the client disconnects
)
EXT_RELEASED = [sc("up", 0x1D), sc("up", 0x10)]


def run_case(name, mode, expected, released):
    log = os.path.join(TMP, f"inputsink_{mode}.log")
    if os.path.exists(log):
        os.remove(log)
    sink = subprocess.Popen([os.path.join(BUILD, "inputsink.exe"), log, "16"])
    for _ in range(50):  # wait for READY
        if os.path.exists(log) and "READY" in open(log, errors="replace").read():
            break
        time.sleep(0.1)
    else:
        sink.kill()
        return [f"{name}: the input hooks did not start"]

    server = subprocess.Popen([os.path.join(BUILD, "daramvnc.exe"), "run", "--port", str(PORT), "--bind", "127.0.0.1"],
                              stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(2.5)
    args = [os.path.join(BUILD, "daramvnc_client.exe"), "127.0.0.1", str(PORT), "--enc", "raw", "--seconds", "12", "--input-test", mode]
    client = subprocess.run(args, capture_output=True, text=True, timeout=40)
    time.sleep(1.5)  # the server notices the disconnect and releases whatever is still held
    try:
        server.stdin.write(b"\n")
        server.stdin.flush()
        server.wait(timeout=10)
    except Exception:
        server.kill()
    sink.wait(timeout=30)

    got = parse(log)
    problems = []
    if "input sequence sent" not in client.stdout:
        problems.append(f"{name}: the client never sent the sequence\n{client.stdout[-300:]}{client.stderr[-300:]}")
        return problems

    # screen size = what the clamped pointer must end on
    size = re.search(r"connected to '.*?' (\d+)x(\d+)", client.stdout)
    sw, sh = (int(size.group(1)), int(size.group(2))) if size else (1920, 1200)
    exp = []
    for e in expected:
        e = dict(e)
        if e.get("x") == "SCREEN_RIGHT_BOTTOM":
            e["x"], e["y"] = sw - 1, sh - 1
        exp.append(e)

    main_part = got[:len(exp)]
    tail = got[len(exp):]
    for i, want in enumerate(exp):
        if i >= len(main_part):
            problems.append(f"{name}: event #{i + 1} missing, expected {want}")
            break
        if not matches(main_part[i], want, 1):
            problems.append(f"{name}: event #{i + 1}: expected {want}, got {main_part[i]}")
            break
    # whatever is still held must be released after the disconnect (order does not matter)
    remaining = list(tail)
    for want in released:
        hit = next((t for t in remaining if matches(t, want, 1)), None)
        if hit is None:
            problems.append(f"{name}: after the disconnect expected a release {want}; got {tail}")
        else:
            remaining.remove(hit)
    if remaining:
        problems.append(f"{name}: unexpected extra events after the sequence: {remaining}")
    print(f"{name}: {len(got)} injected events recorded, {len(problems)} problem(s)")
    return problems


def main():
    problems = []
    problems += run_case("standard KeyEvent", "std", STD, STD_RELEASED)
    problems += run_case("QEMU extended key event", "ext", EXT, EXT_RELEASED)
    if problems:
        print("\nFAILED")
        for p in problems:
            print(" -", p)
        return 1
    print("\nall input checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
