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
WORKSHOP_TEXT = os.path.join(REPO, "docs", "steam-workshop-description.txt")
BUILD_CMD = os.path.join(REPO, "MCPServer", "build.cmd")
BUILD_OUT = os.path.join(REPO, "MCPServer", "src", "out")
PACKAGES = os.path.join(REPO, "MCPServer", "Packages")
OUT = os.path.join(REPO, "Build", "release")
PLUGIN_REPO = "spradlin-dev-bazaar/civ5-mcp-plugin"
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
    def major(self):
        return numbers(self.release)[0]

    @property
    def mod(self):
        return numbers(self.release)[2]

    @property
    def api(self):
        return numbers(self.bazaar)[:2]


PublishState = collections.namedtuple(
    "PublishState", "branch clean head origin_head remote_tag github_release tags missing"
)


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


def bazaar_sentence(version):
    return "Requires Bazaar %s or newer." % version.bazaar


def mod_name(project):
    name = re.search(r"<Name>([^<]*)</Name>", project)
    if not name:
        raise ReleaseError("MCPServer.civ5proj names no mod")
    return name.group(1)


def disagreements(version, header, project, workshop):
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
    if not description or bazaar_sentence(version) not in description.group(1):
        problems.append("MCPServer.civ5proj's description does not say %r" % bazaar_sentence(version))
    if bazaar_sentence(version) not in workshop:
        problems.append("docs\\steam-workshop-description.txt does not say %r" % bazaar_sentence(version))
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
        problems.append(
            "v%s is tagged as released; raise VERSION for the next release, or, if it was never published, "
            "delete its tag here (git tag -d v%s) and on GitHub if it is there" % (release, release)
        )
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
    modinfo = glob.glob(os.path.join(glob.escape(folder), "*.modinfo"))
    if len(modinfo) != 1:
        return ["the package has %d mod descriptions, not one" % len(modinfo)]
    text = read_text(modinfo[0])
    problems = []
    mod = re.search(r'<Mod [^>]*\bversion="(\d+)"', text)
    if not mod or int(mod.group(1)) != version.mod:
        problems.append("the package's mod version is %s, not %d" % (mod.group(1) if mod else "missing", version.mod))
    if bazaar_sentence(version) not in text:
        problems.append("the package's description does not say %r" % bazaar_sentence(version))
    if "<AffectsSavedGames>1</AffectsSavedGames>" not in text:
        problems.append("the package's mod does not affect saved games, so a loaded save would leave it off")
    updates = re.findall(r"<UpdateDatabase>([^<]+)</UpdateDatabase>", text)
    needed = {"LICENSE.txt", "NOTICE.txt", "MCPServer.dll"}
    needed.update(re.findall(r"<File [^>]*>([^<]+)</File>", text))
    needed.update(updates)
    needed.update(re.findall(r'<EntryPoint [^>]*\bfile="([^"]+)"', text))
    for name in sorted(needed):
        if not os.path.isfile(os.path.join(folder, name)):
            problems.append("the package has no %s" % name)
    sql = [read_text(os.path.join(folder, n)) for n in updates if os.path.isfile(os.path.join(folder, n))]
    if not any(
        re.search(r"INSERT\s+(?:OR\s+\w+\s+)?INTO\s+[\"`\[]?DllPlugins\b[^;]*'MCPServer\.dll'", s, re.I) for s in sql
    ):
        problems.append("no database update in the package adds MCPServer.dll to DllPlugins, so Bazaar would skip it")
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
    if state.github_release == "published":
        problems.append("%s is already released; raise VERSION for the next release" % tag)
    elif state.github_release == "draft" and state.remote_tag == state.head:
        problems.append(
            "%s has a draft release on GitHub; check its files, then publish it there, "
            "or delete it and run publish again" % tag
        )
    elif state.github_release == "draft" and state.remote_tag:
        problems.append(
            "%s has a draft release on GitHub from another commit; delete the draft and its tag there, "
            "delete the tag here with git tag -d %s, then run publish again" % (tag, tag)
        )
    elif state.github_release == "draft":
        problems.append(
            "%s has a draft release on GitHub with no tag; delete the draft there, then run publish again" % tag
        )
    elif state.remote_tag and state.remote_tag != state.head:
        problems.append(
            "%s is on GitHub at another commit; delete that tag there and here (git tag -d %s), or raise VERSION"
            % (tag, tag)
        )
    if bazaar_sentence(version) not in notes:
        problems.append("the notes do not say %r" % bazaar_sentence(version))
    return problems


def publish_commands(version, state, files, title, notes_file):
    tag = "v" + version.release
    push = None if state.remote_tag else ["git", "push", "origin", "%s:refs/tags/%s" % (state.head, tag)]
    create = ["gh", "release", "create", tag] + list(files) + ["--repo", PLUGIN_REPO, "--draft", "--verify-tag"]
    create += ["--title", title, "--notes-file", notes_file]
    if version.major == 0:
        create.append("--prerelease")
    return push, create


def refuse(problems):
    if problems:
        raise ReleaseError("\n".join(problems))


def run(command):
    try:
        done = subprocess.run(command, cwd=REPO, capture_output=True, text=True)
    except FileNotFoundError:
        if os.path.isabs(command[0]):
            raise ReleaseError("%s does not exist" % command[0])
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
    problems = disagreements(version, read_text(VERSION_HEADER), read_text(PROJECT), read_text(WORKSHOP_TEXT))
    return version, problems + order_problems(version.release, release_tags())


def release_folder(version):
    return os.path.join(OUT, version.release)


def release_files(version, project):
    # Not ModBuddy's file name: GitHub rewrites the spaces and brackets in it.
    base = os.path.join(release_folder(version), "%s-%s" % (mod_name(project).replace(" ", "-"), version.release))
    return base + ".civ5mod", base + "-symbols.zip"


def sdk_root(folder, what):
    pattern = os.path.join(glob.escape(folder), "**", "CivPlugin.vsprops")
    found = [os.path.dirname(p) for p in glob.glob(pattern, recursive=True)]
    if len(found) != 1:
        raise ReleaseError("%s in %s has %d CivPlugin.vsprops files, not one" % (what, folder, len(found)))
    return found[0]


def download_sdk(bazaar):
    folder = os.path.join(OUT, "sdk", bazaar)
    if not os.path.isdir(folder):
        os.makedirs(os.path.dirname(folder), exist_ok=True)
        with tempfile.TemporaryDirectory(dir=os.path.dirname(folder)) as work:
            gh("release", "download", "v" + bazaar, "--repo", BAZAAR_REPO, "--pattern", SDK_ASSET, "--dir", work)
            zips = glob.glob(os.path.join(glob.escape(work), "*.zip"))
            if len(zips) != 1:
                raise ReleaseError("Bazaar v%s has %d files matching %s, not one" % (bazaar, len(zips), SDK_ASSET))
            extracted = os.path.join(work, "sdk")
            try:
                with zipfile.ZipFile(zips[0]) as z:
                    z.extractall(extracted)
            except zipfile.BadZipFile:
                raise ReleaseError("the Bazaar v%s SDK download, %s, is not a zip file" % (bazaar, zips[0]))
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
    # vcbuild may not notice that BAZAAR_SDK now names another SDK, and would keep objects built against the old one.
    if os.path.isdir(BUILD_OUT):
        shutil.rmtree(BUILD_OUT)
    if os.path.exists(package):
        os.remove(package)
    if subprocess.run(["cmd", "/c", BUILD_CMD], env=dict(os.environ, BAZAAR_SDK=sdk)).returncode != 0:
        raise ReleaseError("build.cmd failed")
    if not os.path.isfile(package):
        raise ReleaseError("build.cmd made no %s" % package)


def clear_build_outputs(out, files):
    # Only what a build writes, so notes drafted in the same folder survive a rebuild.
    for path in [os.path.join(out, "unpacked"), os.path.join(out, "build.json")] + list(files):
        if os.path.isdir(path):
            shutil.rmtree(path)
        elif os.path.exists(path):
            os.remove(path)


def write_build_info(out, commit, clean, downloaded_sdk):
    with open(os.path.join(out, "build.json"), "w", encoding="utf-8") as f:
        json.dump({"commit": commit, "clean": clean, "downloaded_sdk": downloaded_sdk}, f, indent=2)


def read_build_info(out):
    path = os.path.join(out, "build.json")
    if not os.path.isfile(path):
        return None
    try:
        with open(path, encoding="utf-8") as f:
            info = json.load(f)
        return {key: info[key] for key in ("commit", "clean", "downloaded_sdk")}
    except (ValueError, KeyError, TypeError):
        raise ReleaseError("%s cannot be read; build again" % path)


def tree_state():
    return git("rev-parse", "HEAD"), git("status", "--porcelain") == ""


def write_release(package, version, project, commit, clean, downloaded_sdk):
    out = release_folder(version)
    civ5mod, symbols = release_files(version, project)
    clear_build_outputs(out, (civ5mod, symbols))
    unpacked = os.path.join(out, "unpacked")
    os.makedirs(unpacked)
    run([WINDOWS_TAR, "-xf", package, "-C", unpacked])
    refuse(package_problems(unpacked, version))
    shutil.copyfile(package, civ5mod)
    with zipfile.ZipFile(symbols, "w", zipfile.ZIP_DEFLATED) as z:
        for name in ("MCPServer.pdb", "MCPServer.map"):
            z.write(os.path.join(BUILD_OUT, name), name)
    write_build_info(out, commit, clean, downloaded_sdk)
    return out


def build(sdk_folder):
    version, problems = check()
    refuse(problems)
    trial = bool(sdk_folder)
    sdk = sdk_root(os.path.abspath(sdk_folder), "the SDK") if trial else download_sdk(version.bazaar)
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
    out = write_release(package, version, project, commit, clean, downloaded_sdk=not trial)
    print(
        "%s %s, built against Bazaar %s from %s%s with a %s SDK, in %s"
        % (
            mod_name(project),
            version.release,
            version.bazaar,
            commit[:7],
            "" if clean else " plus uncommitted changes",
            "local" if trial else "downloaded",
            out,
        )
    )


def publish(notes_file):
    if not os.path.isfile(notes_file):
        raise ReleaseError("there is no notes file %s" % notes_file)
    notes = read_text(notes_file)
    # GitHub's tags win over local ones, which are left at an old commit when a release's tag is remade on GitHub.
    git("fetch", "--quiet", "--tags", "--force", "origin", "main")
    version = parse_version(read_text(VERSION_FILE))
    project = read_text(PROJECT)
    tag = "v" + version.release
    head, clean = tree_state()
    files = release_files(version, project)
    releases = json.loads(gh("release", "list", "--repo", PLUGIN_REPO, "--json", "tagName,isDraft", "--limit", "1000"))
    state = PublishState(
        branch=git("rev-parse", "--abbrev-ref", "HEAD"),
        clean=clean,
        head=head,
        origin_head=git("rev-parse", "origin/main"),
        remote_tag=remote_commit(git("ls-remote", "--tags", "origin", tag, tag + "^{}"), tag),
        github_release=release_state(releases, tag),
        tags=release_tags(),
        missing=[os.path.basename(f) for f in files if not os.path.isfile(f)],
    )
    problems = disagreements(version, read_text(VERSION_HEADER), project, read_text(WORKSHOP_TEXT))
    refuse(problems + publish_problems(version, read_build_info(release_folder(version)), state, notes))

    title = "%s %s" % (mod_name(project), version.release)
    push, create = publish_commands(version, state, files, title, os.path.abspath(notes_file))
    if push:
        run(push)
    try:
        run(create)
    except ReleaseError as e:
        raise ReleaseError(
            "%s\n%s is on GitHub; if a draft release of it was made, delete the draft there, then run publish again"
            % (e, tag)
        )
    print("Draft release %s made; publish it on GitHub." % tag)
    try:
        git("fetch", "--quiet", "origin", "+refs/tags/%s:refs/tags/%s" % (tag, tag))
    except ReleaseError:
        print("Its tag is not here yet; fetch it with git fetch --tags --force.")


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
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
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as e:
        print("release: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
