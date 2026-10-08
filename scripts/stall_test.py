"""A client that stops reading must not delay anybody else.

One client streams normally (B); another (A) stops reading for 12 s while the server pushes video-like traffic to it.
The server simulates a clipboard change every 9 s (--debug-clipboard-ping; the user's clipboard is not touched).
Before the fix, B was told about a change that happened during A's stall 9.7 s late.

Shows the animation window (animwin) for about 26 s.
usage: python scripts/stall_test.py [--build DIR]       exit code 0 = pass
"""
import argparse
import re
import sys
import time

import common


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default=common.BUILD)
    args = ap.parse_args(argv)
    common.set_build(args.build)
    common.check_build("daramvnc", "daramvnc_client", "animwin")

    port = 5951
    with common.Server(port, "--view-only", "--debug-clipboard-ping", "9") as srv:
        with common.Animation(28, "photo"):
            a = common.start_client("--enc", "tight", "--seconds", "24", "--continuous", "--stall-at", "3", "--stall-for", "12", port=port)
            time.sleep(0.5)
            b = common.start_client("--enc", "tight", "--seconds", "22", "--continuous", port=port)
            out_b = b.communicate(timeout=60)[0]
            out_a = a.communicate(timeout=60)[0]
        log = srv.log()

    pings = [float(x) for x in re.findall(r"simulated clipboard change at wall ([\d.]+)", log)]
    notes_b = [float(x) for x in re.findall(r"clipboard notify received at wall ([\d.]+)", out_b)]
    notes_a = [float(x) for x in re.findall(r"clipboard notify received at wall ([\d.]+)", out_a)]
    updates_b = common.updates_in(out_b)

    problems = []
    if "not reading" not in out_a:
        problems.append("client A never stalled; the experiment did not run")
    if len(pings) < 2:
        problems.append("fewer than 2 simulated clipboard changes were recorded: %s" % pings)
    delays = []
    for p in pings:
        later = [n - p for n in notes_b if n >= p - 5]
        # the first notification that arrives after (or just before) the ping belongs to it
        hit = min([d for d in later if d >= -5], default=None)
        delays.append(hit)
        if hit is None or hit > 1000:
            problems.append("healthy client B was told about the change at %.1f s only %s ms late" % ((p - pings[0]) / 1000, "never" if hit is None else int(hit)))
    if updates_b and updates_b[0] / updates_b[1] < 20:
        problems.append("healthy client B only got %.1f updates/s while A was stalled" % (updates_b[0] / updates_b[1]))

    print("simulated clipboard changes: %d; healthy client notified after %s ms; stalled client got %d notification(s)"
          % (len(pings), [None if d is None else int(d) for d in delays], len(notes_a)))
    if updates_b:
        print("healthy client: %.1f updates/s while the other one was stalled" % (updates_b[0] / updates_b[1]))
    if problems:
        print("FAILED")
        for p in problems:
            print(" -", p)
        return 1
    print("stalled-client test passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
