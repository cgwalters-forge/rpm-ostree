# SPDX-License-Identifier: Apache-2.0 OR MIT
"""Exercise the compose assertions without downloading or composing an OS."""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest


TESTS = Path(__file__).resolve().parents[1]


def section(filename, start, end):
    # Run the actual assertion blocks, not copies which could drift from them.
    text = (TESTS / "compose" / filename).read_text()
    return start + text.split(start, 1)[1].split(end, 1)[0]


class ComposeAssertions(unittest.TestCase):
    def run_shell(self, script, cwd, shell="bash", **env):
        return subprocess.run(
            [shell, "-euc", script], cwd=cwd,
            env={**os.environ, **env}, capture_output=True, text=True,
        )

    def test_rpm_databases(self):
        script = section(
            "libbasic-test.sh", "for path in /usr/share/rpm ",
            "ostree --repo=${repo} ls ${treeref} /usr/lib/sysimage/rpm >",
        )
        # Only the ostree listing is mocked; use the real assertion helpers.
        script = (
            f"source {shlex.quote(str(TESTS / 'common/libtest-core.sh'))}\n"
            'ostree() { cat "${PWD}${@: -1}/listing"; }\n' + script
        )
        paths = ("usr/share/rpm", "usr/lib/sysimage/rpm-ostree-base-db")
        with tempfile.TemporaryDirectory() as tmp:
            for path in paths:
                (Path(tmp) / path).mkdir(parents=True)
            for shm in (False, True):
                for missing in (None, *paths):
                    with self.subTest(shm=shm, missing=missing):
                        for path in paths:
                            names = [] if path == missing else ["rpmdb.sqlite"]
                            if shm:
                                names.append("rpmdb.sqlite-shm")
                            (Path(tmp) / path / "listing").write_text(
                                "".join(f"-00644 0 0 0 /{path}/{n}\n" for n in names)
                            )
                        result = self.run_shell(script, tmp, repo="repo", treeref="ref")
                        self.assertEqual(result.returncode == 0, missing is None,
                                         result.stderr)

    def test_modules_boot(self):
        script = section(
            "test-installroot.sh", "# with treefile, no kernels in /boot",
            '\n"\necho "ok postprocess with treefile"',
        )
        for state in ("absent", "empty", "kernel", "listing-error"):
            with self.subTest(state=state), tempfile.TemporaryDirectory() as tmp:
                boot = Path(tmp) / "rootfs-postprocess-treefile/boot"
                boot.parent.mkdir()
                if state != "absent":
                    boot.mkdir()
                if state == "kernel":
                    (boot / "vmlinuz-test").touch()
                # Deterministic even as root; a failed listing must not be masked.
                prefix = "ls() { return 42; }\n" if state == "listing-error" else ""
                result = self.run_shell(prefix + script, tmp, shell="sh", instroot="rootfs")
                self.assertEqual(result.returncode == 0, state in ("absent", "empty"),
                                 result.stderr)
                if state == "listing-error":
                    self.assertEqual(result.returncode, 42)


if __name__ == "__main__":
    unittest.main()
