"""Make a release of the MCP server plugin.

    python tools/release.py check
    python tools/release.py build [--sdk FOLDER]
    python tools/release.py publish --notes FILE

check: every copy of the version agrees with VERSION, and the release number follows the last release tagged here.
build: builds the DLL and its .civ5mod from scratch against the SDK of the Bazaar release named in VERSION, downloaded
from GitHub, or against a local SDK folder for a trial build, then checks what went into the package. Output goes to
Build\\release.
publish: fetches GitHub's tags, pushes the release's tag and creates a draft GitHub release with the package, its debug
symbols and the given notes. It refuses a build made with --sdk or from anything but a clean, pushed main. If it stops
after pushing the tag, it says how to finish: delete any half-made draft on GitHub, then run it again.

The steps around it are in .claude/skills/release. Building needs Visual C++ 2008, so a release is made on the
developer's machine.
"""

import argparse
import collections
import glob
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VERSION_FILE = os.path.join(REPO, "VERSION")
VERSION_HEADER = os.path.join(REPO, "MCPServer", "src", "McpVersion.h")
PROJECT = os.path.join(REPO, "MCPServer", "mod", "MCPServer.civ5proj")
BUILD_CMD = os.path.join(REPO, "MCPServer", "build.cmd")
BUILD_OUT = os.path.join(REPO, "MCPServer", "src", "out")
PACKAGES = os.path.join(REPO, "MCPServer", "Packages")
STAGING = os.path.join(REPO, "MCPServer", "Build")
OUT = os.path.join(REPO, "Build", "release")
BAZAAR_REPO = "spradlin-dev-bazaar/civ5-bazaar"
SDK_ASSET = "*SDK*.zip"
# libarchive, which reads the 7z format ModBuddy's packages use; Git's tar does not.
WINDOWS_TAR = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", "tar.exe")

NUMBER = "(?:0|[1-9][0-9]*)"
TRIPLE = r"(%s\.%s\.%s)" % (NUMBER, NUMBER, NUMBER)
FORMAT = re.compile(r"release %s\nbazaar %s\n?" % (TRIPLE, TRIPLE))
TAG = re.compile(r"v%s" % TRIPLE)


class ReleaseError(Exception):
    pass


def numbers(text):
    return tuple(int(n) for n in text.split("."))


class Version(collections.namedtuple("Version", "release bazaar")):
    @property
    def mod(self):
        return numbers(self.release)[2]

    @property
    def api(self):
        return numbers(self.bazaar)[:2]


PublishState = collections.namedtuple("PublishState", "branch clean head origin_head remote_tag release tags missing")


def read_text(path):
    with open(path, encoding="utf-8-sig") as f:
        return f.read()


def parse_version(text):
    found = FORMAT.fullmatch(text)
    if not found:
        raise ReleaseError(
            "VERSION should be 'release <major.minor.mod>' and 'bazaar <major.minor.patch>' on two lines; it reads %r"
            % text
        )
    return Version(found.group(1), found.group(2))


def bazaar_sentence(bazaar):
    return "Built against Bazaar %s. Needs Bazaar %s or newer." % (bazaar, bazaar)


def mod_name(project):
    name = re.search(r"<Name>([^<]*)</Name>", project)
    if not name:
        raise ReleaseError("MCPServer.civ5proj names no mod")
    return name.group(1)


def disagreements(version, header, project):
    problems = []
    defined = re.search(r'#define MCP_SERVER_VERSION "([^"]*)"', header)
    if not defined or defined.group(1) != version.release:
        problems.append(
            "McpVersion.h has %s, not %s" % (defined.group(1) if defined else "no MCP_SERVER_VERSION", version.release)
        )
    mod = re.search(r"<ModVersion>([^<]*)</ModVersion>", project)
    if not mod or mod.group(1).strip() != str(version.mod):
        problems.append(
            "MCPServer.civ5proj's ModVersion is %s, not %d" % (mod.group(1) if mod else "missing", version.mod)
        )
    description = re.search(r"<Description>(.*?)</Description>", project, re.S)
    if not description or bazaar_sentence(version.bazaar) not in description.group(1):
        problems.append("MCPServer.civ5proj's description does not say %r" % bazaar_sentence(version.bazaar))
    return problems


def order_problems(release, tags):
    # The last number is also the game's mod version, which must never go down, so it never resets.
    released = sorted(numbers(m.group(1)) for m in (TAG.fullmatch(t) for t in tags) if m)
    new = numbers(release)
    if not released:
        return [] if new[2] == 1 else ["the first release's last number, its mod version, must be 1, not %d" % new[2]]
    last = released[-1]
    problems = []
    if new == last:
        problems.append("v%s is already released; raise VERSION for the next release" % release)
    elif new[2] != last[2] + 1:
        problems.append(
            "the last number, the mod version, must be %d after v%s, not %d"
            % (last[2] + 1, ".".join(map(str, last)), new[2])
        )
    if new[:2] < last[:2]:
        problems.append("%d.%d goes below the last release's %d.%d" % (new[0], new[1], last[0], last[1]))
    return problems


def exported_api_version(data):
    # CivPluginApiVersion is data, not code: its address holds CivApiVersion, two ints (see CivPluginApi.h). PE32 only.
    try:
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        if data[pe : pe + 4] != b"PE\0\0":
            return None
        section_count = struct.unpack_from("<H", data, pe + 6)[0]
        optional_size = struct.unpack_from("<H", data, pe + 20)[0]
        optional = pe + 24
        if struct.unpack_from("<H", data, optional)[0] != 0x10B:
            return None
        export_rva = struct.unpack_from("<I", data, optional + 96)[0]
        sections = [
            struct.unpack_from("<8xIIII", data, optional + optional_size + 40 * i) for i in range(section_count)
        ]

        def at(rva):
            for virtual_size, address, raw_size, raw in sections:
                if address <= rva < address + max(virtual_size, raw_size):
                    return raw + rva - address
            raise ValueError("no section holds RVA %#x" % rva)

        name_count, functions, names, ordinals = struct.unpack_from("<IIII", data, at(export_rva) + 24)
        for i in range(name_count):
            start = at(struct.unpack_from("<I", data, at(names) + 4 * i)[0])
            if data[start : data.index(b"\0", start)] == b"CivPluginApiVersion":
                ordinal = struct.unpack_from("<H", data, at(ordinals) + 2 * i)[0]
                return struct.unpack_from(
                    "<ii", data, at(struct.unpack_from("<I", data, at(functions) + 4 * ordinal)[0])
                )
    except (struct.error, ValueError):
        return None
    return None


def package_problems(folder, version):
    modinfo = glob.glob(os.path.join(folder, "*.modinfo"))
    if len(modinfo) != 1:
        return ["the package has %d mod descriptions, not one" % len(modinfo)]
    text = read_text(modinfo[0])
    problems = []
    mod = re.search(r'<Mod [^>]*\bversion="(\d+)"', text)
    if not mod or int(mod.group(1)) != version.mod:
        problems.append("the package's mod version is %s, not %d" % (mod.group(1) if mod else "missing", version.mod))
    if bazaar_sentence(version.bazaar) not in text:
        problems.append("the package's description does not name Bazaar %s" % version.bazaar)
    needed = {"LICENSE.txt", "NOTICE.txt", "MCPServer.dll"}
    needed.update(re.findall(r"<File [^>]*>([^<]+)</File>", text))
    needed.update(re.findall(r"<UpdateDatabase>([^<]+)</UpdateDatabase>", text))
    needed.update(re.findall(r'<EntryPoint [^>]*\bfile="([^"]+)"', text))
    for name in sorted(needed):
        if not os.path.isfile(os.path.join(folder, name)):
            problems.append("the package has no %s" % name)
    dll = os.path.join(folder, "MCPServer.dll")
    if os.path.isfile(dll):
        with open(dll, "rb") as f:
            api = exported_api_version(f.read())
        if api != version.api:
            problems.append(
                "the package's DLL declares API %s, not %d.%d"
                % ("%d.%d" % api if api else "nothing", version.api[0], version.api[1])
            )
    return problems


def remote_commit(listing, tag):
    refs = {ref: sha for sha, ref in (line.split("\t") for line in listing.splitlines())}
    # An annotated tag's own line names the tag object; its ^{} line names the commit.
    return refs.get("refs/tags/%s^{}" % tag) or refs.get("refs/tags/" + tag)


def release_state(releases, tag):
    for found in releases:
        if found["tagName"] == tag:
            return "draft" if found["isDraft"] else "published"
    return None


def publish_problems(version, info, state, notes):
    tag = "v" + version.release
    # This release's own tag is judged by where it sits on GitHub, so a half-made release can be finished.
    problems = order_problems(version.release, [t for t in state.tags if t != tag])
    if state.branch != "main":
        problems.append("this is not main")
    if not state.clean:
        problems.append("there are uncommitted changes")
    if state.head != state.origin_head:
        problems.append("main is not the same as GitHub's main")
    if info is None:
        problems.append("there is no build of %s; run build first" % version.release)
    else:
        if info["commit"] != state.head or not info["clean"]:
            problems.append("the build is not of this commit with nothing uncommitted; build again")
        if not info["downloaded_sdk"]:
            problems.append("the build used a local SDK; build again without --sdk")
    for name in state.missing:
        problems.append("the build has no %s; build again" % name)
    if state.remote_tag and state.remote_tag != state.head:
        problems.append("%s is on GitHub at another commit; delete that tag on GitHub, or raise VERSION" % tag)
    if state.release == "draft":
        problems.append("%s has a draft release on GitHub; publish it there, or delete it and run publish again" % tag)
    elif state.release == "published":
        problems.append("%s is already released; raise VERSION for the next release" % tag)
    if bazaar_sentence(version.bazaar) not in notes:
        problems.append("the notes do not say %r" % bazaar_sentence(version.bazaar))
    return problems


def refuse(problems):
    if problems:
        raise ReleaseError("\n".join(problems))


def run(command):
    try:
        done = subprocess.run(command, cwd=REPO, capture_output=True, text=True)
    except FileNotFoundError:
        raise ReleaseError("%s is not installed or not on PATH" % command[0])
    if done.returncode:
        raise ReleaseError("%s failed: %s" % (" ".join(command), done.stderr.strip()))
    return done.stdout


def git(*args):
    return run(["git"] + list(args)).strip()


def gh(*args):
    return run(["gh"] + list(args))


def release_tags():
    return git("tag", "--list", "v*").split()


def check():
    version = parse_version(read_text(VERSION_FILE))
    problems = disagreements(version, read_text(VERSION_HEADER), read_text(PROJECT))
    return version, problems + order_problems(version.release, release_tags())


def release_files(version, project):
    # Not ModBuddy's file name: GitHub rewrites the spaces and brackets in it.
    base = os.path.join(OUT, version.release, "%s-%s" % (mod_name(project).replace(" ", "-"), version.release))
    return base + ".civ5mod", base + "-symbols.zip"


def sdk_root(folder, what):
    found = [os.path.dirname(p) for p in glob.glob(os.path.join(folder, "**", "CivPlugin.vsprops"), recursive=True)]
    if len(found) != 1:
        raise ReleaseError("%s in %s has %d CivPlugin.vsprops files, not one" % (what, folder, len(found)))
    return found[0]


def download_sdk(bazaar):
    folder = os.path.join(OUT, "sdk", bazaar)
    if not os.path.isdir(folder):
        os.makedirs(os.path.dirname(folder), exist_ok=True)
        with tempfile.TemporaryDirectory(dir=os.path.dirname(folder)) as work:
            gh("release", "download", "v" + bazaar, "--repo", BAZAAR_REPO, "--pattern", SDK_ASSET, "--dir", work)
            zips = glob.glob(os.path.join(work, "*.zip"))
            if len(zips) != 1:
                raise ReleaseError("Bazaar v%s has %d files matching %s, not one" % (bazaar, len(zips), SDK_ASSET))
            extracted = os.path.join(work, "sdk")
            with zipfile.ZipFile(zips[0]) as z:
                z.extractall(extracted)
            # Moved into place only once complete and checked, so a broken download is never taken for an SDK.
            sdk_root(extracted, "the Bazaar v%s SDK" % bazaar)
            os.replace(extracted, folder)
    return sdk_root(folder, "the Bazaar v%s SDK" % bazaar)


def sdk_api_version(sdk):
    path = os.path.join(sdk, "include", "CivPluginApi.h")
    if not os.path.isfile(path):
        raise ReleaseError("%s is not a Bazaar SDK: it has no include\\CivPluginApi.h" % sdk)
    header = read_text(path)
    major = re.search(r"#define CIV_HOST_API_MAJOR (\d+)", header)
    minor = re.search(r"#define CIV_HOST_API_MINOR (\d+)", header)
    if not major or not minor:
        raise ReleaseError("%s names no API version" % path)
    return int(major.group(1)), int(minor.group(1))


def run_build_cmd(sdk, package):
    # vcbuild may not notice that BAZAAR_SDK now names another SDK and would keep objects built against the old one,
    # and ModBuddy's staging folder keeps files the project no longer lists.
    for folder in (BUILD_OUT, STAGING):
        if os.path.isdir(folder):
            shutil.rmtree(folder)
    if os.path.exists(package):
        os.remove(package)
    if subprocess.run(["cmd", "/c", BUILD_CMD], env=dict(os.environ, BAZAAR_SDK=sdk)).returncode != 0:
        raise ReleaseError("build.cmd failed")
    if not os.path.isfile(package):
        raise ReleaseError("build.cmd made no %s" % package)


def tree_state():
    return git("rev-parse", "HEAD"), git("status", "--porcelain") == ""


def build(sdk_folder):
    version, problems = check()
    refuse(problems)
    sdk = sdk_root(os.path.abspath(sdk_folder), "the SDK") if sdk_folder else download_sdk(version.bazaar)
    api = sdk_api_version(sdk)
    if api != version.api:
        raise ReleaseError(
            "the SDK in %s is API %d.%d; VERSION names Bazaar %s" % (sdk, api[0], api[1], version.bazaar)
        )
    project = read_text(PROJECT)
    package = os.path.join(PACKAGES, "%s (v %d).civ5mod" % (mod_name(project), version.mod))
    commit, clean = tree_state()
    run_build_cmd(sdk, package)
    if tree_state() != (commit, clean):
        raise ReleaseError("the tree changed while building; build again")

    out = os.path.join(OUT, version.release)
    if os.path.isdir(out):
        shutil.rmtree(out)
    unpacked = os.path.join(out, "unpacked")
    os.makedirs(unpacked)
    subprocess.run([WINDOWS_TAR, "-xf", package, "-C", unpacked], check=True)
    refuse(package_problems(unpacked, version))

    civ5mod, symbols = release_files(version, project)
    shutil.copyfile(package, civ5mod)
    with zipfile.ZipFile(symbols, "w", zipfile.ZIP_DEFLATED) as z:
        for name in ("MCPServer.pdb", "MCPServer.map"):
            z.write(os.path.join(BUILD_OUT, name), name)
    info = {"commit": commit, "clean": clean, "downloaded_sdk": not sdk_folder}
    with open(os.path.join(out, "build.json"), "w", encoding="utf-8") as f:
        json.dump(info, f, indent=2)
    print(
        "%s %s, built against Bazaar %s from %s%s with a %s SDK, in %s"
        % (
            mod_name(project),
            version.release,
            version.bazaar,
            commit[:7],
            "" if clean else " plus uncommitted changes",
            "downloaded" if not sdk_folder else "local",
            out,
        )
    )


def build_info(version):
    path = os.path.join(OUT, version.release, "build.json")
    if not os.path.isfile(path):
        return None
    try:
        with open(path, encoding="utf-8") as f:
            info = json.load(f)
        return {key: info[key] for key in ("commit", "clean", "downloaded_sdk")}
    except (ValueError, KeyError, TypeError):
        raise ReleaseError("%s cannot be read; build again" % path)


def publish(notes_file):
    if not os.path.isfile(notes_file):
        raise ReleaseError("there is no notes file %s" % notes_file)
    notes = read_text(notes_file)
    git("fetch", "--quiet", "--tags", "origin", "main")
    version = parse_version(read_text(VERSION_FILE))
    project = read_text(PROJECT)
    tag = "v" + version.release
    head, clean = tree_state()
    files = release_files(version, project)
    state = PublishState(
        branch=git("rev-parse", "--abbrev-ref", "HEAD"),
        clean=clean,
        head=head,
        origin_head=git("rev-parse", "origin/main"),
        remote_tag=remote_commit(git("ls-remote", "--tags", "origin", tag, tag + "^{}"), tag),
        release=release_state(json.loads(gh("release", "list", "--json", "tagName,isDraft", "--limit", "1000")), tag),
        tags=release_tags(),
        missing=[os.path.basename(f) for f in files if not os.path.isfile(f)],
    )
    problems = disagreements(version, read_text(VERSION_HEADER), project)
    refuse(problems + publish_problems(version, build_info(version), state, notes))

    if state.remote_tag is None:
        git("push", "origin", "%s:refs/tags/%s" % (head, tag))
    title = "%s %s" % (mod_name(project), version.release)
    command = ["release", "create", tag] + list(files)
    command += ["--draft", "--verify-tag", "--title", title, "--notes-file", os.path.abspath(notes_file)]
    if numbers(version.release)[0] == 0:
        command.append("--prerelease")
    try:
        gh(*command)
    except ReleaseError as e:
        raise ReleaseError(
            "%s\n%s is on GitHub; if a draft release of it was made, delete the draft there, then run publish again"
            % (e, tag)
        )
    print("Draft release %s made; publish it on GitHub." % tag)
    try:
        git("fetch", "--quiet", "origin", "refs/tags/%s:refs/tags/%s" % (tag, tag))
    except ReleaseError:
        print("Its tag is not here yet; fetch it with git fetch --tags.")


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    steps = parser.add_subparsers(dest="step", required=True)
    steps.add_parser("check")
    build_step = steps.add_parser("build")
    build_step.add_argument("--sdk", help="a local Bazaar SDK folder, for a trial build that cannot be published")
    publish_step = steps.add_parser("publish")
    publish_step.add_argument("--notes", required=True, help="the release notes, a Markdown file")
    args = parser.parse_args(argv)
    try:
        if args.step == "check":
            version, problems = check()
            refuse(problems)
            print(
                "%s %s (mod version %d), for Bazaar %s"
                % (mod_name(read_text(PROJECT)), version.release, version.mod, version.bazaar)
            )
        elif args.step == "build":
            build(args.sdk)
        else:
            publish(args.notes)
    except ReleaseError as e:
        for line in str(e).splitlines():
            print("release: " + line, file=sys.stderr)
        return 1
    except (subprocess.CalledProcessError, OSError, ValueError) as e:
        print("release: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
