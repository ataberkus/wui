"""Run programmer-contract assertions outside the normal doctest process."""

import argparse
import os
import re
import signal
import subprocess
import sys


def is_contract_assertion(result, mode):
    if os.name == "nt":
        # CRT abort, fast-fail, and fatal application exit. A crash alone is not
        # proof: also require the armed operation, WUI report, and CRT assertion.
        aborted = (result.returncode & 0xFFFFFFFF) in {3, 0xC0000409, 0x40000015}
    else:
        aborted = result.returncode == -signal.SIGABRT
    return (
        aborted
        and f"WUI_ASSERT_ARMED:{mode}" in result.stdout.splitlines()
        and "wui contract:" in result.stderr
        and re.search(r"\bassertion\b[^\r\n]*\bfailed\b", result.stderr, re.IGNORECASE)
        is not None
    )


def run_probe(probe, mode):
    # The executable disables CRT/Windows Error Reporting dialogs itself.
    # Captured stderr provides proof that termination was an assertion rather
    # than a loader error, uncaught exception, access violation, or plain abort.
    return subprocess.run(
        [probe, mode],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=20,
        check=False,
    )


def failure(mode, result):
    return (
        f"{mode}: unexpected exit {result.returncode}\n"
        f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", required=True)
    args = parser.parse_args()

    control = run_probe(args.probe, "control")
    if control.returncode != 0 or "WUI_CONTROL_OK" not in control.stdout.splitlines():
        raise RuntimeError(failure("control", control))

    ordinary = run_probe(args.probe, "ordinary-error")
    if ordinary.returncode != 23 or is_contract_assertion(ordinary, "ordinary-error"):
        raise RuntimeError(failure("ordinary-error", ordinary))
    aborted = run_probe(args.probe, "abort-control")
    if aborted.returncode == 0 or is_contract_assertion(aborted, "abort-control"):
        raise RuntimeError(failure("abort-control", aborted))

    for mode in ("duplicate", "out-of-order", "open-scopes", "row-overlap",
                 "spacer-negative", "custom-height", "custom-moved", "style-overlap",
                 "style-color", "icon-label", "second-frame"):
        result = run_probe(args.probe, mode)
        if not is_contract_assertion(result, mode):
            raise RuntimeError(failure(mode, result))
        print(f"{mode}: observed isolated contract assertion")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
