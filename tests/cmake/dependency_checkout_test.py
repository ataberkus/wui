"""Locked dependency checks accept generated EOLs, not source modifications."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("prepare_dependencies", ROOT / "tools/prepare_dependencies.py")
prepare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare)


class CheckoutVerification(unittest.TestCase):
    def test_generated_line_endings_and_real_edits(self):
        with tempfile.TemporaryDirectory(prefix="wui-checkout-") as directory:
            repository = Path(directory)

            def git(*arguments):
                return subprocess.check_output(
                    ["git", "-C", str(repository), *arguments], stderr=subprocess.STDOUT, text=True
                ).strip()

            git("init", "-q")
            git("config", "core.autocrlf", "true")
            git("config", "commit.gpgsign", "false")
            source = repository / "source.txt"
            source.write_bytes(b"locked source\r\n")
            git("add", "source.txt")
            git("-c", "user.name=WuiTest", "-c", "user.email=test@example.invalid",
                "commit", "-qm", "fixture")
            record = {"destination": str(repository), "revision": git("rev-parse", "HEAD")}
            source.write_bytes(b"locked source\n")
            prepare.ensure_checkout(record, False)
            source.write_bytes(b"changed source\n")
            with self.assertRaises(RuntimeError):
                prepare.ensure_checkout(record, False)
            git("add", "source.txt")
            with self.assertRaises(RuntimeError):
                prepare.ensure_checkout(record, False)
            git("reset", "--hard", "HEAD")
            (repository / "untracked.txt").write_text("unexpected", encoding="utf-8")
            with self.assertRaises(RuntimeError):
                prepare.ensure_checkout(record, False)


if __name__ == "__main__":
    unittest.main()
