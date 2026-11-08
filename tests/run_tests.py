#!/usr/bin/env python3
"""Rift compiler test runner.

Two kinds of tests are discovered and executed:

  * Driver tests — fixed CLI-level smoke checks against the riftc binary.

  * Golden tests — `tests/rfts/*.rfts` files, driven by header metadata.

Golden test metadata (leading `//` comment block):

    // EXIT: <int>                      expected process exit code (default 0)
    // EXPECT_STDOUT: <line>            expected stdout line (one directive per line)
    // EXPECT_STDERR_CONTAINS: <text>   expected stderr substring
    // EXPECT_ERROR: <text>             shorthand for nonzero exit + stderr substring
    // STDIN: <line>                    stdin line (one directive per line)
    // SKIP: <reason>                   skip this test
    // XFAIL: <reason>                  the test is expected to fail

The runner tries `riftc compile` first. If the compiler reports "not
implemented" (exit code 2), it falls back to `riftc lex` so lexer-only
tests are still validated during M1. Once M4 lands codegen, `compile`
succeeds and the fallback never fires.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parent
TESTS_DIR = ROOT / "rfts"

NOT_IMPLEMENTED = 2


@dataclass
class TestResult:
    name: str
    passed: bool
    skipped: bool = False
    xfailed: bool = False
    message: str = ""


def run(cmd, *, stdin=None, cwd=None):
    return subprocess.run(
        cmd, input=stdin, cwd=cwd, text=True, capture_output=True, check=False,
    )


# ── Driver tests ────────────────────────────────────────────────

def driver_tests(riftc: str):
    results = []

    def check(name, cond, detail=""):
        results.append(TestResult(name=name, passed=cond, message=detail))

    r = run([riftc, "--version"])
    check("driver: --version exits 0", r.returncode == 0, f"exit={r.returncode}")
    check("driver: --version prints semver",
          bool(re.match(r"^riftc \d+\.\d+\.\d+", r.stdout)), f"stdout={r.stdout!r}")
    check("driver: --version mentions M1", "(M1)" in r.stdout,
          f"stdout={r.stdout!r}")

    r = run([riftc, "version"])
    check("driver: 'version' subcommand exits 0", r.returncode == 0)

    r = run([riftc, "--help"])
    check("driver: --help exits 0", r.returncode == 0)
    check("driver: --help mentions 'compile'", "compile" in r.stdout)

    r = run([riftc])
    check("driver: no args exits nonzero", r.returncode != 0)
    check("driver: no args prints usage", "Usage:" in r.stderr)

    r = run([riftc, "frobnicate"])
    check("driver: unknown command exits nonzero", r.returncode != 0)
    check("driver: unknown command names the word", "frobnicate" in r.stderr)

    r = run([riftc, "compile"])
    check("driver: compile with no input exits nonzero", r.returncode != 0)

    r = run([riftc, "compile", "definitely-not-a-file.rfts", "-o", "/tmp/x"])
    check("driver: compile missing file exits nonzero", r.returncode != 0)
    check("driver: compile missing file reports 'not found'",
          "not found" in r.stderr.lower())

    # lex subcommand
    r = run([riftc, "lex"])
    check("driver: lex with no input exits nonzero", r.returncode != 0)

    with tempfile.NamedTemporaryFile("w", suffix=".rfts", delete=False) as f:
        f.write("u32 main() { return 0u32; }\n")
        tmp = f.name
    try:
        r = run([riftc, "lex", tmp])
        check("driver: lex on simple file exits 0", r.returncode == 0,
              f"stderr={r.stderr!r}")
        # The kind column is tab-delimited; look for "\tu32\t" rather than "prim_u32" because token_kind_name()
        # returns the pretty name ("u32", "identifier", ...), not the enum identifier
        check("driver: lex dumps tokens to stdout",
              "\tu32\t" in r.stdout and "\tidentifier\t" in r.stdout,
              f"stdout={r.stdout!r}")
        check("driver: lex output ends with eof",
              "eof" in r.stdout.splitlines()[-1] if r.stdout else False)
    finally:
        os.unlink(tmp)

    return results


# ── Golden tests ───────────────────────────────────────────────

@dataclass
class GoldenMetadata:
    exit_code: int = 0
    expected_stdout: list = field(default_factory=list)
    expected_stderr_contains: list = field(default_factory=list)
    stdin: list = field(default_factory=list)
    skip: str | None = None
    xfail: str | None = None
    expect_error: bool = False


_DIRECTIVE_RE = re.compile(
    r"^(EXIT|EXPECT_STDOUT|EXPECT_STDERR_CONTAINS|STDIN|SKIP|XFAIL|EXPECT_ERROR):\s*(.*)$"
)


def parse_metadata(path: Path) -> GoldenMetadata:
    meta = GoldenMetadata()
    for line in path.read_text().splitlines():
        stripped = line.strip()
        if not stripped:
            continue
        if not stripped.startswith("//"):
            break
        m = _DIRECTIVE_RE.match(stripped[2:].strip())
        if not m:
            continue
        key, value = m.group(1), m.group(2)
        if key == "EXIT":
            meta.exit_code = int(value)
            if meta.exit_code != 0:
                meta.expect_error = True
        elif key == "EXPECT_STDOUT":
            meta.expected_stdout.append(value)
        elif key == "EXPECT_STDERR_CONTAINS":
            meta.expected_stderr_contains.append(value)
        elif key == "STDIN":
            meta.stdin.append(value)
        elif key == "SKIP":
            meta.skip = value or "skipped"
        elif key == "XFAIL":
            meta.xfail = value or "expected to fail"
        elif key == "EXPECT_ERROR":
            meta.expect_error = True
            if value:
                meta.expected_stderr_contains.append(value)
    return meta


def run_golden(path: Path, riftc: str) -> TestResult:
    name = f"golden: {path.name}"
    try:
        meta = parse_metadata(path)
    except Exception as exc:
        return TestResult(name=name, passed=False,
                          message=f"failed to parse metadata: {exc}")

    if meta.skip is not None:
        return TestResult(name=name, passed=True, skipped=True, message=meta.skip)

    stdin_text = "\n".join(meta.stdin) + ("\n" if meta.stdin else "")
    result = TestResult(name=name, passed=True)

    # Try `compile` first.
    with tempfile.TemporaryDirectory() as tmp:
        out_path = Path(tmp) / (path.stem + ".out")
        proc = run([riftc, "compile", str(path), "-o", str(out_path)],
                   stdin=stdin_text)

        # If the compiler isn't at codegen yet, fall back to `lex`.
        if proc.returncode == NOT_IMPLEMENTED:
            proc = run([riftc, "lex", str(path)], stdin=stdin_text)
            out_path = None  # no binary expected

        if meta.expect_error:
            exit_ok = (
                proc.returncode == meta.exit_code
                if meta.exit_code != 0
                else proc.returncode != 0
            )
            stderr_ok = all(
                s in proc.stderr for s in meta.expected_stderr_contains
            )
            ok = exit_ok and stderr_ok
            if meta.xfail is not None:
                ok = not ok
            return TestResult(
                name=name, passed=ok,
                message="" if ok else (
                    f"expected error exit={meta.exit_code} "
                    f"containing {meta.expected_stderr_contains!r}, "
                    f"got exit={proc.returncode} stderr={proc.stderr!r}"),
            )

        if proc.returncode != 0:
            msg = f"compiler failed (exit={proc.returncode}): {proc.stderr}"
            if meta.xfail is not None:
                return TestResult(name=name, passed=True, xfailed=True, message=msg)
            return TestResult(name=name, passed=False, message=msg)

        if out_path is not None and not out_path.exists():
            msg = f"compiler succeeded but produced no output at {out_path}"
            if meta.xfail is not None:
                return TestResult(name=name, passed=True, xfailed=True, message=msg)
            return TestResult(name=name, passed=False, message=msg)

        if out_path is not None:
            exec_proc = run([str(out_path)], stdin=stdin_text)
            if exec_proc.returncode != meta.exit_code:
                return TestResult(
                    name=name, passed=False,
                    message=f"expected exit {meta.exit_code}, got {exec_proc.returncode}",
                )
            if meta.expected_stdout:
                want = "\n".join(meta.expected_stdout)
                got = exec_proc.stdout.rstrip("\n")
                if got != want:
                    return TestResult(
                        name=name, passed=False,
                        message=f"stdout mismatch: want {want!r}, got {got!r}",
                    )

        return result


# ── Entry point ────────────────────────────────────────────────

def main() -> int:
    ap = argparse.ArgumentParser(description="Rift compiler test runner")
    ap.add_argument("--riftc", required=True)
    ap.add_argument("--filter", default=None)
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    if not os.path.isfile(args.riftc):
        print(f"error: riftc binary not found: {args.riftc}", file=sys.stderr)
        return 2

    results = driver_tests(args.riftc)
    if TESTS_DIR.is_dir():
        for path in sorted(TESTS_DIR.glob("*.rfts")):
            results.append(run_golden(path, args.riftc))

    if args.filter:
        rx = re.compile(args.filter)
        results = [r for r in results if rx.search(r.name)]

    passed = sum(1 for r in results if r.passed and not r.skipped and not r.xfailed)
    skipped = sum(1 for r in results if r.skipped)
    xfailed = sum(1 for r in results if r.xfailed)
    failed = [r for r in results if not r.passed]

    for r in results:
        marker = ("PASS" if r.passed and not r.skipped and not r.xfailed
                  else "SKIP" if r.skipped
                  else "XFAIL" if r.xfailed
                  else "FAIL")
        if args.verbose or not r.passed:
            print(f"[{marker}] {r.name}")
            if r.message:
                print(f"       {r.message}")

    print()
    print(f"{len(results)} tests: {passed} passed, {len(failed)} failed, "
          f"{skipped} skipped, {xfailed} xfailed")
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())
