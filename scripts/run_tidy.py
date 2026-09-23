#!/usr/bin/env python3
"""Run the exception-specification clang-tidy checks (see ../.clang-tidy).

    scripts/run_tidy.py [build-dir]        default: build/linux-debug

Environment:
    CLANG_TIDY   clang-tidy binary to use (default: clang-tidy-21..clang-tidy on PATH)
    TIDY_JOBS    parallel jobs (default: CPU count)

Exit status: 0 clean, 1 findings, 2 a translation unit failed to parse,
3 the built-in self-check did not fire, 4 setup problem.

Why this is a script and not a bare `clang-tidy -p build`:
  * bugprone-exception-escape turns itself off under -fno-exceptions, which is
    how the hot modules compile, so the analysis is run with -fexceptions.
  * CMake adds GCC-only C++-modules flags (-fdeps-format, -fmodule-mapper) to
    every compile command. Clang rejects them and skips the file, which looks
    exactly like "no findings". They are stripped from a private copy of the
    compilation database.
  * Because silence is ambiguous, a file that must produce findings is checked
    first; if tidy does not report on it, the run fails instead of passing.
"""
import concurrent.futures
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CONFIG = ROOT / ".clang-tidy"
GCC_ONLY = ("-fdeps-format", "-fmodule-mapper", "-fmodules-ts")
FIRST_PARTY = ("core", "apps", "handlers", "tests")
# clang-tidy 21 always treats main() as a function that must not throw and has
# no option to exempt it. Letting an exception reach main just terminates, which
# is acceptable for startup code and demos, so those findings are dropped here.
IGNORED = ("in function 'main' which should not throw",)
EXTRA_ARGS = ["-fexceptions", "-Wno-unknown-warning-option", "-Wno-unused-command-line-argument"]

# Must trigger exactly these two checks.
CONTROL = """\
void may_throw() { throw 1; }
void hot() noexcept { may_throw(); }
struct Movable { Movable(Movable&&) {} };
"""
CONTROL_EXPECTS = ("bugprone-exception-escape", "performance-noexcept-move-constructor")

DIAG = re.compile(r"^(?P<file>\S[^:\n]*):(?P<line>\d+):(?P<col>\d+): (?P<sev>warning|error): ", re.M)


def find_tidy() -> str:
    candidates = [os.environ.get("CLANG_TIDY", "")] + [f"clang-tidy-{v}" for v in (22, 21, 20, 19, 18)] + ["clang-tidy"]
    for c in candidates:
        if c and (Path(c).is_file() or shutil.which(c)):
            return c
    sys.exit("run_tidy: no clang-tidy found. Install one (e.g. `sudo apt install clang-tidy-21`) "
             "or set CLANG_TIDY=/path/to/clang-tidy.")


def sanitized_database(build_dir: Path, out_dir: Path) -> list[str]:
    db_path = build_dir / "compile_commands.json"
    if not db_path.is_file():
        sys.exit(f"run_tidy: {db_path} not found; configure the build first (cmake --preset ...).")
    entries, files = [], []
    for e in json.loads(db_path.read_text()):
        args = e["arguments"] if "arguments" in e else shlex.split(e["command"])
        args = [a for a in args if not a.startswith(GCC_ONLY)]
        e = {k: v for k, v in e.items() if k not in ("arguments", "command")}
        e["command"] = shlex.join(args)
        entries.append(e)
        f = Path(e["directory"], e["file"]).resolve() if not Path(e["file"]).is_absolute() else Path(e["file"])
        try:
            rel = f.relative_to(ROOT)
        except ValueError:
            continue
        if rel.parts[0] in FIRST_PARTY and str(f) not in files:
            files.append(str(f))
    (out_dir / "compile_commands.json").write_text(json.dumps(entries))
    return sorted(files)


def header_filter() -> str:
    # llvm::Regex is POSIX ERE: escape the specials that can appear in a path.
    return "^" + re.sub(r"([.^$*+?()\[\]{}|\\])", r"\\\1", str(ROOT / "include")) + "/.*"


def run_tidy(tidy: str, args: list[str]) -> str:
    cmd = [tidy, "--quiet", f"--config-file={CONFIG}", *args]
    p = subprocess.run(cmd, capture_output=True, text=True)
    return p.stdout + p.stderr


def blocks(text: str) -> list[tuple[str, str, str]]:
    """Split tidy output into (key, severity, full text) diagnostics."""
    text = re.sub(r"^\d+ warnings? (and \d+ errors? )?generated\.\n?", "", text, flags=re.M)
    starts = [m for m in DIAG.finditer(text)]
    out = []
    for i, m in enumerate(starts):
        end = starts[i + 1].start() if i + 1 < len(starts) else len(text)
        first = text[m.start():text.index("\n", m.start())] if "\n" in text[m.start():] else text[m.start():]
        out.append((first, m.group("sev"), text[m.start():end].rstrip()))
    return out


def main() -> int:
    tidy = find_tidy()
    build_arg = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("build/linux-debug")
    build_dir = build_arg if build_arg.is_absolute() else ROOT / build_arg
    jobs = int(os.environ.get("TIDY_JOBS", os.cpu_count() or 4))

    with tempfile.TemporaryDirectory(prefix="run_tidy_") as tmp:
        tmpdir = Path(tmp)
        files = sanitized_database(build_dir, tmpdir)
        if not files:
            sys.exit("run_tidy: no first-party translation units found in the compilation database.")

        control = tmpdir / "control.cpp"
        control.write_text(CONTROL)
        out = run_tidy(tidy, [str(control), "--", "-std=c++23", *EXTRA_ARGS])
        missing = [c for c in CONTROL_EXPECTS if f"[{c}]" not in out]
        if missing:
            print("run_tidy: SELF-CHECK FAILED. tidy did not report on a file that must trigger "
                  f"{', '.join(missing)}; a clean result would mean nothing.\n{out}", file=sys.stderr)
            return 3

        base = ["-p", str(tmpdir), f"--header-filter={header_filter()}",
                *[f"--extra-arg={a}" for a in EXTRA_ARGS]]
        with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
            outputs = list(pool.map(lambda f: run_tidy(tidy, [*base, f]), files))

    seen, findings, errors = set(), [], []
    for text in outputs:
        for key, sev, full in blocks(text):
            if key in seen or any(s in key for s in IGNORED):
                continue
            seen.add(key)
            (errors if sev == "error" else findings).append(full.replace(str(ROOT) + "/", ""))

    print(f"run_tidy: {len(files)} translation units, {tidy}")
    if errors:
        print(f"\n{len(errors)} translation unit error(s); the results below are incomplete:\n")
        print("\n\n".join(errors[:10]))
        return 2
    if findings:
        print(f"\n{len(findings)} finding(s):\n")
        print("\n\n".join(findings))
        return 1
    print("no findings")
    return 0


if __name__ == "__main__":
    sys.exit(main())
