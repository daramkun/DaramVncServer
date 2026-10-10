"""Drives the real settings window (`daramvnc gui --no-elevate`) with window messages and checks the config file.

usage: python tests/gui_test.py [build-dir]      exit code 0 = pass

Uses a config file in the temp folder; the service is never touched (the buttons that need administrator
rights are not pressed). The window is started with a hidden STARTUPINFO, like the elevated relaunch does,
which also checks that the window still becomes visible.
"""
import ctypes
import os
import subprocess
import sys
import tempfile
import time
from ctypes import wintypes

BUILD = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "build")
EXE = os.path.join(BUILD, "daramvnc.exe")
CONF = os.path.join(tempfile.gettempdir(), "daramvnc_gui_test.conf")

u = ctypes.WinDLL("user32", use_last_error=True)
u.FindWindowW.restype = wintypes.HWND
u.FindWindowW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR]
u.GetDlgItem.restype = wintypes.HWND
u.GetDlgItem.argtypes = [wintypes.HWND, ctypes.c_int]
u.SendMessageW.restype = ctypes.c_ssize_t
u.SendMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
u.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
u.IsWindowVisible.argtypes = [wintypes.HWND]
u.FindWindowExW.restype = wintypes.HWND
u.FindWindowExW.argtypes = [wintypes.HWND, wintypes.HWND, wintypes.LPCWSTR, wintypes.LPCWSTR]
u.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]

WM_GETTEXT = 0x000D
WM_SETTEXT, WM_CLOSE, BM_GETCHECK, BM_SETCHECK, BM_CLICK = 0x000C, 0x0010, 0x00F0, 0x00F1, 0x00F5
CLASS = "DaramVncSettingsWindow"
ID = dict(status=100, firewall=101, bind=120, port=121, view_only=122, h264=123, legacy=124, jpeg_max=130, jpeg_min=131,
          user=140, account=143, log=150, log_mb=151, log_keep=152, save=160, close=162, message=163)

failed = []


def check(name, ok, detail=""):
    print(("PASS  " if ok else "FAIL  ") + name + (("  " + detail) if detail and not ok else ""), flush=True)
    if not ok:
        failed.append(name)


def text(win, key):
    # GetWindowText does not return the text of another process's edit control; WM_GETTEXT does.
    buf = ctypes.create_unicode_buffer(1024)
    u.SendMessageW(u.GetDlgItem(win, ID[key]), WM_GETTEXT, 1024, ctypes.addressof(buf))
    return buf.value


def set_text(win, key, value):
    buf = ctypes.create_unicode_buffer(value)  # must stay alive until SendMessage returns
    u.SendMessageW(u.GetDlgItem(win, ID[key]), WM_SETTEXT, 0, ctypes.addressof(buf))


def checked(win, key):
    return u.SendMessageW(u.GetDlgItem(win, ID[key]), BM_GETCHECK, 0, 0) == 1


def set_checked(win, key, on):
    u.SendMessageW(u.GetDlgItem(win, ID[key]), BM_SETCHECK, 1 if on else 0, 0)


def read_conf():
    out = {}
    try:
        for line in open(CONF, encoding="utf-8"):
            if "=" in line:
                k, v = line.rstrip("\r\n").split("=", 1)
                out[k.strip()] = v.strip()
    except FileNotFoundError:
        pass
    return out


def start_gui():
    si = subprocess.STARTUPINFO()
    si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    si.wShowWindow = 0  # SW_HIDE
    proc = subprocess.Popen([EXE, "gui", "--no-elevate", "--config", CONF], startupinfo=si)
    deadline = time.time() + 15
    while time.time() < deadline:
        win = u.FindWindowW(CLASS, None)
        if win:
            return proc, win
        time.sleep(0.1)
    proc.kill()
    raise RuntimeError("the settings window did not appear")


def stop_gui(proc, win):
    u.PostMessageW(win, WM_CLOSE, 0, 0)
    try:
        proc.wait(10)
    except subprocess.TimeoutExpired:
        proc.kill()


def our_dialog(pid):
    """A message box (#32770) that belongs to the process with this id."""
    prev = None
    while True:
        dlg = u.FindWindowExW(None, prev, "#32770", None)
        if not dlg:
            return None
        owner = wintypes.DWORD()
        u.GetWindowThreadProcessId(dlg, ctypes.byref(owner))
        if owner.value == pid:
            return dlg
        prev = dlg


def click_save_expecting_dialog(win, pid):
    """Save with invalid settings opens a message box; it blocks the click, so post it and close the box."""
    u.PostMessageW(u.GetDlgItem(win, ID["save"]), BM_CLICK, 0, 0)
    deadline = time.time() + 10
    while time.time() < deadline:
        dlg = our_dialog(pid)
        if dlg:
            u.PostMessageW(dlg, WM_CLOSE, 0, 0)
            for _ in range(50):  # wait until the box is really gone and the handler returned
                if not our_dialog(pid):
                    break
                time.sleep(0.1)
            time.sleep(0.2)
            return True
        time.sleep(0.1)
    return False


def main():
    if os.path.exists(CONF):
        os.remove(CONF)
    with open(CONF, "w", encoding="utf-8") as f:
        f.write("# my comment\nuser=me\npassword_hash=pbkdf2-sha256$1$aa$bb\nport=5950\njpeg_quality_max=80\ncert=C:/keep/this.crt\n")

    proc, win = start_gui()
    try:
        time.sleep(0.5)
        check("window is visible although started hidden", bool(u.IsWindowVisible(win)))
        check("form shows the file's port", text(win, "port") == "5950", text(win, "port"))
        check("form shows the file's JPEG max", text(win, "jpeg_max") == "80", text(win, "jpeg_max"))
        check("form shows the default JPEG min", text(win, "jpeg_min") == "40", text(win, "jpeg_min"))
        check("form shows the default bind", text(win, "bind") == "0.0.0.0", text(win, "bind"))
        check("form shows the user", text(win, "user") == "me", text(win, "user"))
        check("account line reports the password as set", "설정됨" in text(win, "account").split("|")[0], text(win, "account"))
        check("service status is filled in", text(win, "status") != "", text(win, "status"))

        set_text(win, "bind", "::")
        set_text(win, "port", "5961")
        set_checked(win, "view_only", True)
        set_checked(win, "h264", True)
        set_text(win, "jpeg_max", "70")
        set_text(win, "jpeg_min", "30")
        set_text(win, "log", "D:/logs/x.log")
        set_text(win, "log_mb", "7")
        set_text(win, "log_keep", "2")
        u.SendMessageW(u.GetDlgItem(win, ID["save"]), BM_CLICK, 0, 0)
        time.sleep(0.3)
        conf = read_conf()
        check("save writes the edited keys",
              conf.get("bind") == "::" and conf.get("port") == "5961" and conf.get("view_only") == "1" and conf.get("h264") == "1"
              and conf.get("jpeg_quality_max") == "70" and conf.get("jpeg_quality_min") == "30" and conf.get("log") == "D:/logs/x.log"
              and conf.get("log_max_mb") == "7" and conf.get("log_keep") == "2", str(conf))
        check("save keeps credentials, certificate and comments",
              conf.get("user") == "me" and conf.get("password_hash") == "pbkdf2-sha256$1$aa$bb" and conf.get("cert") == "C:/keep/this.crt"
              and "# my comment" in open(CONF, encoding="utf-8").read(), str(conf))
        check("message line confirms the save", text(win, "message").startswith("저장했습니다"), text(win, "message"))

        before = open(CONF, encoding="utf-8").read()
        set_text(win, "port", "99999")
        check("a port out of range is refused with a message box", click_save_expecting_dialog(win, proc.pid))
        check("the file is untouched after a refused save", open(CONF, encoding="utf-8").read() == before)
        set_text(win, "port", "abc")
        check("a non-numeric port is refused too", click_save_expecting_dialog(win, proc.pid))
        set_text(win, "port", "5961")
        set_text(win, "jpeg_min", "90")
        check("JPEG min above max is refused", click_save_expecting_dialog(win, proc.pid))
        check("the file is still untouched", open(CONF, encoding="utf-8").read() == before)
    finally:
        stop_gui(proc, win)

    # Without credentials the form may not save a non-loopback address.
    os.remove(CONF)
    proc, win = start_gui()
    try:
        time.sleep(0.5)
        check("empty config: default bind is 0.0.0.0", text(win, "bind") == "0.0.0.0", text(win, "bind"))
        check("empty config: account line says no password", "설정 안 됨" in text(win, "account"), text(win, "account"))
        check("saving 0.0.0.0 without credentials is refused", click_save_expecting_dialog(win, proc.pid))
        check("nothing was written", not os.path.exists(CONF))
        set_text(win, "bind", "127.0.0.1")
        u.SendMessageW(u.GetDlgItem(win, ID["save"]), BM_CLICK, 0, 0)
        time.sleep(0.3)
        check("loopback without credentials saves", read_conf().get("bind") == "127.0.0.1", str(read_conf()))
    finally:
        stop_gui(proc, win)
        if os.path.exists(CONF):
            os.remove(CONF)

    print("\n%s" % ("GUI test FAILED: " + ", ".join(failed) if failed else "GUI test passed"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
