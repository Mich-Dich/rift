#!/usr/bin/env python3
"""Rift compiler test runner.

Three suites:

  unit     C++ unit tests of compiler internals (the rift_unit_tests binary).
  feature  Language-feature tests: tests/feature/**/*.rifts with inline expectations.
           The file is checked with `riftc check --diagnostic-format=short` and every
           error/warning must match an annotation, and vice versa:

               u8 y = 0x;        //~ error[E1009]
               u8 z = 0b2;       //~ error[E1010]: invalid digit '2'
               //~^ error                      (`^` = one line up, `^^` = two lines up)
               //~v warning[W1001]             (`v` = one line down)

           A file without error annotations must compile (exit 0).
  action   End-to-end runs of riftc: tests/action/**/*.rifts whose first lines hold
           `// RUN: riftc <args>` (%s = this file, relative to the repo root).
           stdout, stderr and the exit code are compared with the golden file
           <name>.expected next to the test. `--update` (re)writes the golden files.

Any test file may contain `// XFAIL: <reason>` - it is expected to fail (a feature that is
specified but not implemented yet). If it passes, that is reported as XPASS and counts as a
failure, so the marker gets removed.

Exit status: 0 if everything passed, 1 otherwise.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import difflib
import os
import re
import shlex
import signal
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
TESTS = REPO / "tests"
TIMEOUT_SECONDS = 20

SHORT_DIAG = re.compile(
    r"^(?P<path>.+?):(?P<line>\d+):(?P<col>\d+): (?P<sev>error|warning)\[(?P<code>[EW]\d{4})\]: (?P<msg>.*)$"
)
ANNOTATION = re.compile(
    r"//~(?P<dir>\^+|v+)?\s*(?P<sev>error|warning)(?:\[(?P<code>[EW]\d{4})\])?(?::\s*(?P<msg>.*?))?\s*$"
)
RUN_LINE = re.compile(r"^\s*//\s*RUN:\s*(?P<cmd>.+?)\s*$")
XFAIL_LINE = re.compile(r"^\s*//\s*XFAIL:\s*(?P<reason>.*)$", re.MULTILINE)
STOP_AFTER_LINE = re.compile(r"^\s*//\s*STOP-AFTER:\s*(?P<phase>lex|parse|resolve|types)\b", re.MULTILINE)


# --- output helpers -----------------------------------------------------------------------

class Style:
    def __init__(self, enabled: bool) -> None:
        self.enabled = enabled

    def _wrap(self, code: str, text: str) -> str:
        return f"\x1b[{code}m{text}\x1b[0m" if self.enabled else text

    def green(self, t: str) -> str: return self._wrap("1;32", t)
    def red(self, t: str) -> str: return self._wrap("1;31", t)
    def yellow(self, t: str) -> str: return self._wrap("1;33", t)
    def dim(self, t: str) -> str: return self._wrap("2", t)


@dataclass
class Result:
    suite: str
    name: str
    status: str                  # PASS FAIL XFAIL XPASS NEW UPDATED
    details: list[str] = field(default_factory=list)

    @property
    def ok(self) -> bool:
        return self.status in ("PASS", "XFAIL", "NEW", "UPDATED")


def rel(path: Path) -> str:
    return path.resolve().relative_to(REPO).as_posix()


def run(cmd: list[str]) -> subprocess.CompletedProcess[str]:
    env = dict(os.environ, NO_COLOR="1", LC_ALL="C.UTF-8")
    try:
        return subprocess.run(cmd, cwd=REPO, capture_output=True, text=True,
                              timeout=TIMEOUT_SECONDS, env=env, errors="replace")
    except subprocess.TimeoutExpired:
        return subprocess.CompletedProcess(cmd, -999, "", f"<timed out after {TIMEOUT_SECONDS}s>")
    except OSError as e:
        return subprocess.CompletedProcess(cmd, -998, "", f"<cannot run {cmd[0]}: {e}>")


def apply_xfail(result: Result, text: str) -> Result:
    m = XFAIL_LINE.search(text)
    if not m:
        return result
    if result.ok:
        return Result(result.suite, result.name, "XPASS",
                      [f"marked XFAIL ({m.group('reason').strip()}) but passed; remove the XFAIL line"])
    return Result(result.suite, result.name, "XFAIL", result.details)


def exit_code_note(code: int) -> str:
    names = {0: "success", 1: "compile errors", 2: "usage error", 3: "internal compiler error",
             -999: "timeout", -998: "could not start"}
    return f"{code} ({names.get(code, 'crash/signal' if code < 0 else 'unknown')})"


# --- unit suite ---------------------------------------------------------------------------

def run_unit(binary: Path, filters: list[str]) -> list[Result]:
    if not binary.exists():
        return [Result("unit", rel_or_abs(binary), "FAIL", ["unit test binary not found; build first"])]
    proc = run([str(binary), "--verbose"] + filters)
    results: list[Result] = []
    current: Result | None = None
    for line in proc.stdout.splitlines():
        if line.startswith("PASS  "):
            results.append(Result("unit", line[6:], "PASS"))
            current = None
        elif line.startswith("FAIL  "):
            name, _, where = line[6:].partition("  (")
            current = Result("unit", name, "FAIL", [f"at {where.rstrip(')')}"])
            results.append(current)
        elif line.startswith("summary:"):
            current = None
        elif current is not None:
            current.details.append(line)
    if proc.returncode not in (0, 1) or (proc.returncode == 1 and not any(not r.ok for r in results)):
        results.append(Result("unit", "<unit test binary>", "FAIL",
                              [f"exit code {exit_code_note(proc.returncode)}", proc.stderr.strip()]))
    return results


def rel_or_abs(p: Path) -> str:
    try:
        return rel(p)
    except ValueError:
        return str(p)


# --- feature suite ------------------------------------------------------------------------

@dataclass(frozen=True)
class Expected:
    path: str
    line: int
    sev: str
    code: str | None
    msg: str | None

    def describe(self, show_path: bool) -> str:
        code = f"[{self.code}]" if self.code else ""
        msg = f": {self.msg}" if self.msg else ""
        where = f"{Path(self.path).name}:" if show_path else "line "
        return f"{where}{self.line}: {self.sev}{code}{msg}"


def parse_annotations(path: Path, text: str) -> list[Expected]:
    expected = []
    for index, line in enumerate(text.splitlines(), start=1):
        m = ANNOTATION.search(line)
        if not m:
            continue
        direction = m.group("dir") or ""
        target = index - len(direction) if direction.startswith("^") else index + len(direction)
        expected.append(Expected(rel(path), target, m.group("sev"), m.group("code"), m.group("msg") or None))
    return expected


def matches(exp: Expected, diag: re.Match[str]) -> bool:
    return (Path(diag.group("path")).as_posix() == exp.path
            and int(diag.group("line")) == exp.line
            and diag.group("sev") == exp.sev
            and (exp.code is None or diag.group("code") == exp.code)
            and (exp.msg is None or exp.msg in diag.group("msg")))


def is_project_dir(path: Path) -> bool:
    return path.is_dir() and path.name.endswith("_project")


def inside_project(path: Path) -> bool:
    return any(p.name.endswith("_project") for p in path.parents)


def run_feature(path: Path, riftc: Path) -> Result:
    """`path` is a single test file, or a `*_project` directory whose entry is main.rifts."""
    name = rel(path)
    if is_project_dir(path):
        files = sorted(path.rglob("*.rift[sh]"))
        entry = path / "main.rifts"
        extra = [f"--root={name}"]
        if not entry.exists():
            return Result("feature", name, "FAIL", ["a *_project directory needs a main.rifts entry file"])
    else:
        files, entry, extra = [path], path, []
    texts = {f: f.read_text(encoding="utf-8", errors="replace") for f in files}
    all_text = "\n".join(texts.values())
    expected = [e for f in files for e in parse_annotations(f, texts[f])]
    stop = STOP_AFTER_LINE.search(all_text)
    if stop:
        extra.append(f"--stop-after={stop.group('phase')}")
    proc = run([str(riftc), "check", "--diagnostic-format=short", "--color=never", *extra, rel(entry)])

    details: list[str] = []
    actual = []
    for line in proc.stderr.splitlines():
        m = SHORT_DIAG.match(line)
        if m:
            actual.append(m)

    multi = len(files) > 1
    unmatched = list(actual)
    for exp in expected:
        hit = next((d for d in unmatched if matches(exp, d)), None)
        if hit is None:
            details.append(f"missing    {exp.describe(multi)}")
        else:
            unmatched.remove(hit)
    for d in unmatched:
        where = f"{Path(d.group('path')).name}:" if multi else "line "
        details.append(f"unexpected {where}{d.group('line')}:{d.group('col')}: "
                       f"{d.group('sev')}[{d.group('code')}]: {d.group('msg')}")

    want_exit = 1 if any(e.sev == "error" for e in expected) else 0
    if proc.returncode != want_exit:
        details.append(f"exit code {exit_code_note(proc.returncode)}, expected {exit_code_note(want_exit)}")
        if proc.returncode not in (0, 1) or not actual:
            details.extend("  " + l for l in proc.stderr.splitlines()[-10:])

    result = Result("feature", name, "FAIL" if details else "PASS", details)
    return apply_xfail(result, all_text)


# --- action suite -------------------------------------------------------------------------

def golden_path(test: Path) -> Path:
    return test.with_suffix(".expected")


def format_golden(exit_code: int, stdout: str, stderr: str) -> str:
    def section(title: str, body: str) -> str:
        if body and not body.endswith("\n"):
            body += "\n<no newline at end>\n"
        return f"--- {title}\n{body}"
    return f"exit: {exit_code}\n" + section("stdout", stdout) + section("stderr", stderr)


def run_action(path: Path, riftc: Path, update: bool) -> Result:
    name = rel(path)
    text = path.read_text(encoding="utf-8", errors="replace")
    run_lines = [m.group("cmd") for line in text.splitlines()[:20] if (m := RUN_LINE.match(line))]
    if len(run_lines) != 1:
        return Result("action", name, "FAIL", ["need exactly one '// RUN: riftc ...' line in the first 20 lines"])

    argv = shlex.split(run_lines[0].replace("%s", name).replace("%S", rel(path.parent)))
    if not argv or argv[0] != "riftc":
        return Result("action", name, "FAIL", ["RUN line must start with 'riftc'"])
    proc = run([str(riftc)] + argv[1:])
    actual = format_golden(proc.returncode, proc.stdout, proc.stderr)

    golden = golden_path(path)
    if update:
        if not golden.exists() or golden.read_text(encoding="utf-8") != actual:
            status = "NEW" if not golden.exists() else "UPDATED"
            golden.write_text(actual, encoding="utf-8")
            return Result("action", name, status if not XFAIL_LINE.search(text) else "XFAIL",
                          [f"wrote {rel(golden)}"])
        return apply_xfail(Result("action", name, "PASS"), text)

    if not golden.exists():
        return apply_xfail(Result("action", name, "FAIL",
                                  [f"golden file {rel(golden)} missing; run with --update to create it",
                                   *("  " + l for l in actual.splitlines())]), text)
    want = golden.read_text(encoding="utf-8")
    if want == actual:
        return apply_xfail(Result("action", name, "PASS"), text)
    diff = difflib.unified_diff(want.splitlines(), actual.splitlines(),
                                fromfile=f"{rel(golden)} (expected)", tofile="actual", lineterm="")
    return apply_xfail(Result("action", name, "FAIL", list(diff)), text)


# --- main ---------------------------------------------------------------------------------

def discover(suite_dir: Path, filters: list[str], projects: bool) -> list[Path]:
    """Test files of a suite. Files inside `*_project` directories are fixtures; with
    `projects=True` each such directory is itself one test."""
    files = sorted(suite_dir.rglob("*.rifts")) + sorted(suite_dir.rglob("*.rifth"))
    files = [f for f in files if not inside_project(f)]
    if projects:
        files += sorted(d for d in suite_dir.rglob("*_project") if is_project_dir(d) and not inside_project(d))
    if filters:
        files = [f for f in files if any(flt in rel(f) for flt in filters)]
    return files


def main() -> int:
    parser = argparse.ArgumentParser(description="Run the Rift compiler test suites.")
    parser.add_argument("--build-dir", default="build", help="CMake build directory (default: build)")
    parser.add_argument("--riftc", help="path to riftc (default: <build-dir>/compiler/riftc)")
    parser.add_argument("--unit-bin", help="path to the unit test binary "
                        "(default: <build-dir>/tests/rift_unit_tests)")
    parser.add_argument("--suite", action="append", choices=["unit", "feature", "action"],
                        help="run only this suite (repeatable; default: all)")
    parser.add_argument("--update", action="store_true", help="rewrite action-test golden files")
    parser.add_argument("-v", "--verbose", action="store_true", help="list every test, not only failures")
    parser.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--color", choices=["auto", "always", "never"], default="auto")
    parser.add_argument("filters", nargs="*", help="only run tests whose path/name contains one of these")
    args = parser.parse_args()

    build = (REPO / args.build_dir) if not Path(args.build_dir).is_absolute() else Path(args.build_dir)
    riftc = Path(args.riftc).resolve() if args.riftc else build / "compiler" / "riftc"
    unit_bin = Path(args.unit_bin).resolve() if args.unit_bin else build / "tests" / "rift_unit_tests"
    suites = args.suite or ["unit", "feature", "action"]
    style = Style(args.color == "always" or (args.color == "auto" and sys.stdout.isatty()))

    if any(s in suites for s in ("feature", "action")) and not riftc.exists():
        print(style.red(f"riftc not found at {riftc}; build first (cmake --build {args.build_dir})"))
        return 1

    results: list[Result] = []
    if "unit" in suites:
        results += run_unit(unit_bin, args.filters)
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        jobs = []
        if "feature" in suites:
            jobs += [pool.submit(run_feature, f, riftc) for f in discover(TESTS / "feature", args.filters, projects=True)]
        if "action" in suites:
            jobs += [pool.submit(run_action, f, riftc, args.update) for f in discover(TESTS / "action", args.filters, projects=False)]
        results += [j.result() for j in jobs]

    colors = {"PASS": style.green, "XFAIL": style.yellow, "NEW": style.yellow, "UPDATED": style.yellow,
              "FAIL": style.red, "XPASS": style.red}
    for r in results:
        if r.ok and not args.verbose and r.status not in ("NEW", "UPDATED"):
            continue
        print(f"{colors[r.status](f'{r.status:<7}')} {r.suite:<8} {r.name}")
        if not r.ok or r.status in ("NEW", "UPDATED") or (args.verbose and r.details):
            for line in r.details:
                print(style.dim("      " + line) if r.ok else "      " + line)

    print()
    any_failed = False
    for suite in ("unit", "feature", "action"):
        if suite not in suites:
            continue
        mine = [r for r in results if r.suite == suite]
        counts = {s: sum(1 for r in mine if r.status == s) for s in ("PASS", "FAIL", "XFAIL", "XPASS", "NEW", "UPDATED")}
        failed = counts["FAIL"] + counts["XPASS"]
        any_failed |= failed > 0
        extra = "".join(f", {counts[s]} {s.lower()}" for s in ("XFAIL", "XPASS", "NEW", "UPDATED") if counts[s])
        line = f"{suite:<8} {counts['PASS'] + counts['XFAIL'] + counts['NEW'] + counts['UPDATED']}/{len(mine)} ok{extra}"
        print(style.red(line) if failed else style.green(line))
    print(style.red("FAILED") if any_failed else style.green("ALL TESTS PASSED"))
    return 1 if any_failed else 0


if __name__ == "__main__":
    if hasattr(signal, "SIGPIPE"):
        signal.signal(signal.SIGPIPE, signal.SIG_DFL)   # quiet exit when piped into `head`
    sys.exit(main())
