# Rejects malformed gallery command lines before any window is created.
# The executable path is supplied by CMake add_test.
import subprocess
import sys

exe = sys.argv[1]


def code(*args):
    return subprocess.run([exe, *args], check=False, capture_output=True,
                          timeout=60).returncode


assert code("--measure", "ignored", "--workload", "unknown") == 2
assert code("--workload", "controls100") == 2
assert code("--validate", "ignored", "--workload", "gallery") == 2
assert code("--measure", "ignored", "--workload") == 2
assert code("--measure", "ignored", "--workload", "gallery", "--workload", "gallery") == 2
assert code("--help") == 0
