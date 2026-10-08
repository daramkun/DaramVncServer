"""Server CPU use under a fixed load, from the server's own process times.

  python scripts/bench_cpu.py [--mode photo|text|both|desktop] [--seconds 8] [--build DIR]

photo/text/both: animwin repaints a window (shown on screen) while a continuous-updates client receives it.
desktop: no animation, whatever the screen is doing right now.
Prints CPU as a percentage of one core, per update and per MB sent. Live screens and window animations vary by
roughly +-20 % between runs, so compare several runs. The server log also has a per-thread CPU breakdown
(run longer than 20 s and look for "cpu over 10 s").
"""
import argparse
import re
import time

import common


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", default="both", choices=["photo", "text", "both", "desktop"])
    ap.add_argument("--seconds", type=int, default=8)
    ap.add_argument("--build", default=common.BUILD)
    args = ap.parse_args()
    common.set_build(args.build)
    common.check_build("daramvnc", "daramvnc_client", "animwin")

    port = 5961
    with common.Server(port, "--view-only") as srv:
        anim = None if args.mode == "desktop" else common.Animation(args.seconds + 6, args.mode)
        try:
            if anim:
                time.sleep(1.2)
            cpu0 = common.cpu_seconds(srv.proc.pid)
            t0 = time.time()
            out = common.client("--enc", "tight", "--seconds", args.seconds, "--continuous", port=port, timeout=args.seconds + 40)
            wall = time.time() - t0
            cpu = common.cpu_seconds(srv.proc.pid) - cpu0
        finally:
            if anim:
                anim.stop()
    u = common.updates_in(out)
    mb = re.search(r"([\d.]+) MB received", out or "")
    if not u or not mb:
        print("the client did not finish:", (out or "")[-300:])
        return 1
    mb = float(mb.group(1))
    print("mode %s: %d updates in %.1f s (%.1f/s), %.1f MB sent" % (args.mode, u[0], u[1], u[0] / u[1], mb))
    print("server CPU %.2f s over %.1f s = %.1f%% of one core; %.2f ms per update, %.1f ms per MB" % (
        cpu, wall, cpu / wall * 100, cpu / u[0] * 1000, cpu / mb * 1000))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
