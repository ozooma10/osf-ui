"""Small real-compiler regressions for the incremental runner (no game/browser)."""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

RUNNER = Path(__file__).with_name("build.py").resolve()


class IncrementalBuildTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if os.environ.get("CXX"):
            cls.compiler = ["--cxx", os.environ["CXX"]]
        elif os.name == "nt":
            vswhere = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
            if not vswhere.exists():
                raise unittest.SkipTest("MSVC is not installed")
            root = subprocess.check_output([str(vswhere), "-latest", "-property", "installationPath"], text=True).strip()
            cls.compiler = ["--msvc", root]
        else:
            compiler = shutil.which("clang++") or shutil.which("g++")
            if not compiler:
                raise unittest.SkipTest("No C++ compiler")
            cls.compiler = ["--cxx", compiler]

    def test_incremental_dependencies_and_failure_recovery(self):
        with tempfile.TemporaryDirectory(prefix="native runner ") as temporary:
            root = Path(temporary) / "tests/native"
            root.mkdir(parents=True)
            (root / "stubs").mkdir()
            (root / "stubs/pch.h").write_text("// forced include\n")
            (root / "leaf header.h").write_text("#define VALUE 1\n")
            (root / "nested.h").write_text('#include "leaf header.h"\n')
            (root / "shared.cpp").write_text('#include "nested.h"\nint value() { return VALUE; }\n')
            main = "int value(); int main() { return value(); }\n"
            (root / "a.cpp").write_text(main)
            (root / "b.cpp").write_text("int main() { return 0; }\n")
            suites = "a_tests a.cpp shared.cpp\nb_tests b.cpp\n"

            def run(compiled, linked, *, manifest=None, extra=(), succeeds=True, environment=None):
                result = subprocess.run([sys.executable, str(RUNNER), *self.compiler, *extra],
                                        input=manifest or suites, cwd=root, text=True, env=environment,
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                self.assertEqual(result.returncode == 0, succeeds, result.stdout)
                self.assertIn(f"== compiling {compiled}/", result.stdout)
                if linked is not None:
                    self.assertIn(f"== linking {linked}/", result.stdout)
                return result

            run(3, 2)
            run(0, 0)
            suffix = ".exe" if os.name == "nt" else ""
            self.assertEqual(subprocess.run([str(root / (".build/a_tests" + suffix))]).returncode, 1)
            # Hashes avoid recompiling a header merely because its timestamp changed.
            os.utime(root / "leaf header.h", None)
            run(0, 0)
            (root / "leaf header.h").write_text("#define VALUE 2\n")
            run(1, 1)
            self.assertEqual(subprocess.run([str(root / (".build/a_tests" + suffix))]).returncode, 2)
            (root / "a.cpp").write_text(main + "// changed source\n")
            run(1, 1)
            # A missing header must fail rather than silently running the cached binary.
            (root / "leaf header.h").unlink()
            run(1, None, succeeds=False)
            (root / "leaf header.h").write_text("#define VALUE 2\n")
            run(1, 1)
            (root / "a.cpp").write_text("int absent(); int main() { return absent(); }\n")
            run(1, 1, succeeds=False)
            # The successful object compile can be reused, but a failed link is retried.
            run(0, 1, succeeds=False)
            (root / "a.cpp").write_text(main)
            run(1, 1)
            # Ordered suite inputs are part of the link receipt, even when removed.
            (root / "extra.cpp").write_text("int extra() { return 0; }\n")
            run(1, 1, manifest=suites.replace("a.cpp shared.cpp", "a.cpp shared.cpp extra.cpp"))
            run(0, 1)
            (root / (".build/b_tests" + suffix)).unlink()
            run(0, 1)
            next((root / ".build/obj").iterdir()).unlink()
            run(1, 1)
            (root / "stubs/pch.h").write_text("// forced include changed\n")
            run(3, 2)
            changed = dict(os.environ)
            variable = "CL" if self.compiler[0] == "--msvc" else "CPATH"
            changed[variable] = "/D RUNNER_FLAG_CHECK=1" if variable == "CL" else str(root)
            run(3, 2, environment=changed)
            run(3, 2)  # Restoring compiler settings must also invalidate the cache.
            run(3, 2, extra=["--clean"])
            run(0, 0)
            run(0, 0, manifest="a_tests a.cpp shared.cpp\n")
            self.assertFalse((root / (".build/b_tests" + suffix)).exists())


if __name__ == "__main__":
    unittest.main()
