"""Checks a build of the plugin in Civilization V: Bazaar's checker starts the game with the build, loads
saves/mcp_test.Civ5Save and hands the game over; mcp_checks.py then calls the plugin over MCP, and the checker takes the
game back, closes it, puts the player's files back and checks that nothing crashed.

    python tests/ingame/run.py [package.civ5mod or mod folder]

Without an argument it checks the mod folder build.cmd made, as a test build with mod version 900 above the .civ5proj's,
so it never replaces an installed copy of the released mod. A run takes about five minutes and needs what
bazaar_check/README.md lists, with Bazaar and its VMC Bootstrap installed.
"""

import contextlib
import filecmp
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
MOD_SOURCE = os.path.join(REPO, "MCPServer", "mod")
CODE_SOURCE = os.path.join(REPO, "MCPServer", "src")
PROJECT = os.path.join(MOD_SOURCE, "MCPServer.civ5proj")
BUILT = os.path.join(REPO, "MCPServer", "Build")
SAVE = os.path.join(HERE, "saves", "mcp_test.Civ5Save")
SCREENSHOT = os.path.join(REPO, "Build", "ingame", "game.png")
# The checker counts mod versions from 900 up as test builds.
TEST_VERSION_OFFSET = 900
# The checker loads saves only from the player's saves folder, so the save goes there for the run under its own name.
STAGED = "MCP Server checks"
HAND_OVER = "mcp_checks.py is calling the plugin over MCP."
SCREENSHOT_SECONDS = 60

sys.path.insert(0, HERE)
import mcp_checks  # noqa: E402
from bazaar_check import files  # noqa: E402

CHECKS_TOML = """mods = [%(mod)s]

[[steps]]
plugin = "MCPServer.dll"
status = "Loaded"

[[steps]]
load = %(save)s

[[steps]]
hand_over = %(hand_over)s
"""


def stop(message):
    raise SystemExit("run: " + message)


def say(message):
    print("run: " + message, flush=True)


def toml_text(value):
    return '"%s"' % value.replace("\\", "\\\\").replace('"', '\\"')


def project_field(name):
    with open(PROJECT, encoding="utf-8-sig") as f:
        return re.search(r"<%s>([^<]+)</%s>" % (name, name), f.read()).group(1).strip()


def newest_source():
    paths = [
        p for p in glob.glob(os.path.join(CODE_SOURCE, "*")) if os.path.splitext(p)[1] in (".cpp", ".h", ".vcproj")
    ]
    paths += [
        p
        for p in glob.glob(os.path.join(MOD_SOURCE, "*"))
        if os.path.splitext(p)[1] in (".lua", ".xml", ".sql", ".civ5proj")
    ]
    return max(paths, key=os.path.getmtime)


def test_build(folder):
    """The mod folder build.cmd made, copied into folder as a test build, or a reason to stop."""
    built = os.path.join(BUILT, project_field("Name"))
    modinfo = glob.glob(os.path.join(built, "*.modinfo"))
    if not modinfo:
        stop("%s holds no built mod; build the plugin with MCPServer\\build.cmd first" % built)
    source = newest_source()
    if os.path.getmtime(source) > os.path.getmtime(modinfo[0]):
        stop("%s changed after the last build; build the plugin with MCPServer\\build.cmd first" % source)
    version = TEST_VERSION_OFFSET + int(project_field("ModVersion"))
    staged = os.path.join(folder, "%s (v %d)" % (project_field("Name"), version))
    shutil.copytree(built, staged)
    path = os.path.join(staged, os.path.basename(modinfo[0]))
    with open(path, "rb") as f:
        text = f.read()
    marked, count = re.subn(rb'(<Mod\b[^>]*\bversion=")\d+(")', rb"\g<1>%d\g<2>" % version, text, count=1)
    if count != 1:
        stop("%s has no mod version to change" % path)
    with open(path, "wb") as f:
        f.write(marked)
    say("checking the build as test mod version %d" % version)
    return staged


@contextlib.contextmanager
def staged_save():
    """The test save in the player's saves folder for the run. A copy already there is left in place afterwards: it may
    be another run's."""
    folder = os.path.join(files.user_folder(), files.SAVES)
    os.makedirs(folder, exist_ok=True)
    staged = os.path.join(folder, STAGED + ".Civ5Save")
    ours = not os.path.exists(staged)
    if not ours and not filecmp.cmp(staged, SAVE, shallow=False):
        stop("%s is not this repository's test save; move it away, then run this again" % staged)
    if ours:
        shutil.copyfile(SAVE, staged)
    try:
        yield
    finally:
        if ours:
            with contextlib.suppress(FileNotFoundError):
                os.remove(staged)


def screenshot():
    """A picture of the game window as the checks left it, in Build\\ingame: what was on screen if a check failed."""
    script = os.path.join(HERE, "screenshot.ps1")
    try:
        with contextlib.suppress(FileNotFoundError):
            os.remove(SCREENSHOT)
        os.makedirs(os.path.dirname(SCREENSHOT), exist_ok=True)
        done = subprocess.run(
            ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", script, "-Path", SCREENSHOT],
            capture_output=True,
            text=True,
            errors="replace",
            timeout=SCREENSHOT_SECONDS,
        )
    except (OSError, subprocess.SubprocessError) as e:
        say("no picture of the game window: %s" % e)
        return
    if done.returncode == 0:
        say("the game window is in %s" % SCREENSHOT)
    else:
        say("no picture of the game window: %s" % done.stderr.strip()[:200])


def check_while_handed_over():
    try:
        return mcp_checks.run()
    finally:
        try:
            screenshot()
        finally:
            if not files.hand_back():
                say("the checker was not waiting for the game back")


def follow(log, checker):
    """The lines the checker writes to its log, as it writes them, until it ends. log is read as bytes, so a character
    split across two reads is decoded only once its line is whole."""
    pending = b""
    while True:
        ended = checker.poll() is not None
        data = log.read()
        if data:
            pending += data
            *lines, pending = pending.split(b"\n")
            for line in lines:
                yield line.rstrip(b"\r").decode("utf-8", "replace")
        elif ended:
            if pending:
                yield pending.rstrip(b"\r").decode("utf-8", "replace")
            return
        else:
            time.sleep(0.5)


def run_checker(checks_file, folder):
    """Plays the checks file, running the MCP checks at its hand-over. Returns their results, or None if the checker ended
    before it handed the game over, and the checker's exit code. The checker writes to a log rather than to this script,
    so it outlives this script if this script is killed: it still puts the player's files back, once given the game back
    with python -m bazaar_check --hand-back. Ctrl+C reaches both, and the checker then stops by its own rules."""
    log_path = os.path.join(folder, "checker.log")
    with open(log_path, "w", encoding="utf-8") as out:
        checker = subprocess.Popen(
            [sys.executable, "-B", "-m", "bazaar_check", checks_file],
            cwd=HERE,
            stdin=subprocess.DEVNULL,
            stdout=out,
            stderr=subprocess.STDOUT,
            env=dict(os.environ, PYTHONIOENCODING="utf-8"),
        )
    results = None
    with open(log_path, "rb") as log:
        try:
            for line in follow(log, checker):
                print(line, flush=True)
                if results is None and HAND_OVER in line:
                    results = check_while_handed_over()
        finally:
            for line in follow(log, checker):
                print(line, flush=True)
    return results, checker.wait()


def main(argv):
    folder = tempfile.mkdtemp(prefix="mcp checks ")
    try:
        if argv:
            mod = os.path.abspath(argv[0])
            if not os.path.exists(mod):
                stop("%s does not exist" % mod)
        else:
            mod = test_build(folder)
        checks_file = os.path.join(folder, "checks.toml")
        with open(checks_file, "w", encoding="utf-8") as f:
            f.write(CHECKS_TOML % {"mod": toml_text(mod), "save": toml_text(STAGED), "hand_over": toml_text(HAND_OVER)})
        with staged_save():
            results, code = run_checker(checks_file, folder)
    finally:
        shutil.rmtree(folder, ignore_errors=True)
    if results is None:
        stop("the checker ended before it handed the game over, so the MCP checks did not run")
    failed = [r for r in results if not r.passed]
    print(
        "MCP checks: %d, failed %d. Bazaar's checks: %s." % (len(results), len(failed), "failed" if code else "passed")
    )
    return 1 if failed or code else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
