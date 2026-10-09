"""Shared helpers for the verification scripts: start/stop the server, run the test client, show the animation window.

Run the scripts as `python scripts/<name>.py` (not with -I): they import this module from their own folder.
"""
import ctypes
import os
import re
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")


def set_build(path):
    global BUILD
    BUILD = os.path.abspath(path)


def exe(name):
    return os.path.join(BUILD, name + ".exe")


def check_build(*names):
    missing = [n for n in names if not os.path.exists(exe(n))]
    if missing:
        sys.exit("missing in %s: %s  (run scripts\\build.bat first)" % (BUILD, ", ".join(n + ".exe" for n in missing)))


def tmp_path(name):
    return os.path.join(tempfile.gettempdir(), name)


class Server:
    """daramvnc.exe run --port N ...; stdout/stderr go to a log file that can be read while it runs."""

    def __init__(self, port, *args):
        self.port = port
        self.args = list(args)
        self.log_path = tmp_path("daramvnc_script_%d.log" % port)
        self.proc = None

    def __enter__(self):
        self.start()
        return self

    def __exit__(self, *exc):
        self.stop()

    def start(self):
        logf = open(self.log_path, "wb")
        self.proc = subprocess.Popen([exe("daramvnc"), "run", "--port", str(self.port), *(() if "--bind" in self.args else ("--bind", "127.0.0.1")), *self.args],
                                     stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=logf)
        logf.close()
        deadline = time.time() + 15
        while time.time() < deadline:
            text = self.log()
            if "listening on" in text:
                return
            if "cannot start" in text or self.proc.poll() is not None:
                raise RuntimeError("server did not start: " + text.strip()[-300:])
            time.sleep(0.1)
        raise RuntimeError("server did not report that it is listening")

    def log(self):
        try:
            with open(self.log_path, encoding="utf-8", errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    def stop(self):
        if not self.proc:
            return
        try:
            self.proc.stdin.write(b"\n")  # the foreground server stops on a line of input
            self.proc.stdin.flush()
            self.proc.wait(timeout=10)
        except Exception:
            self.proc.kill()
        self.proc = None


def client(*args, port=None, host="127.0.0.1", timeout=60):
    """Runs daramvnc_client.exe; returns its combined output, or None if it did not finish in time."""
    cmd = [exe("daramvnc_client"), host, str(port)] if port else [exe("daramvnc_client")]
    try:
        r = subprocess.run(cmd + [str(a) for a in args], capture_output=True, text=True, timeout=timeout, errors="replace")
    except subprocess.TimeoutExpired:
        return None
    return r.stdout + r.stderr


def start_client(*args, port, host="127.0.0.1"):
    return subprocess.Popen([exe("daramvnc_client"), host, str(port), *[str(a) for a in args]],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")


def updates_in(output):
    """(updates, seconds) from the client's summary line, or None."""
    m = re.search(r"(\d+) updates in ([\d.]+)s", output or "")
    return (int(m.group(1)), float(m.group(2))) if m else None


class Animation:
    """animwin.exe: a topmost window that repaints at ~60 Hz for a while (it IS visible on the screen)."""

    def __init__(self, seconds, mode="photo"):
        self.proc = subprocess.Popen([exe("animwin"), str(seconds), mode])

    def __enter__(self):
        time.sleep(1.2)
        return self

    def __exit__(self, *exc):
        self.stop()

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.kill()


def cpu_seconds(pid):
    """User + kernel CPU time of a process (seconds)."""
    k32 = ctypes.windll.kernel32
    k32.OpenProcess.restype = ctypes.c_void_p
    h = k32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
    if not h:
        return 0.0
    ft = [ctypes.c_ulonglong() for _ in range(4)]
    k32.GetProcessTimes(ctypes.c_void_p(h), *[ctypes.byref(x) for x in ft])
    k32.CloseHandle(ctypes.c_void_p(h))
    return (ft[2].value + ft[3].value) / 1e7
