"""Tests for tools/release.py's rules, without building anything.

python -m unittest discover -s tests
"""

import codecs
import contextlib
import io
import os
import struct
import sys
import tempfile
import types
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))

import release  # noqa: E402

HEADER = '#pragma once\n#define MCP_SERVER_VERSION "0.2.3"\n'
SENTENCE = "Requires Bazaar 0.4.0 or newer."
RELEASE_SENTENCE = "This is MCP Server for Bazaar 0.2.3, built with the Bazaar 0.4.1 SDK."
PROJECT = (
    "<Name>MCP Server for Bazaar</Name>\n<ModVersion>3</ModVersion>\n<Description>Plays Civ V. %s %s</Description>"
    % (SENTENCE, RELEASE_SENTENCE)
)
WORKSHOP = "[h1]MCP Server for Bazaar[/h1]\nPlays Civ V.\n\n%s\n" % SENTENCE
VERSION = release.Version("0.2.3", "0.4.0", "0.4.1")
MODINFO = """<Mod id="x" version="3">
  <Properties><Description>Plays Civ V. %s %s</Description><AffectsSavedGames>1</AffectsSavedGames></Properties>
  <Files><File md5="0" import="0">MCPServer.dll</File><File md5="0" import="0">LLMLog.lua</File></Files>
  <Actions><OnModActivated><UpdateDatabase>MCPServer.sql</UpdateDatabase></OnModActivated></Actions>
  <EntryPoints><EntryPoint type="InGameUIAddin" file="LLMLog.xml"><Name>AI Log</Name></EntryPoint></EntryPoints>
</Mod>
""" % (SENTENCE, RELEASE_SENTENCE)
CREATE_TABLE = "CREATE TABLE IF NOT EXISTS DllPlugins (File TEXT NOT NULL);\n"
SQL = CREATE_TABLE + "INSERT INTO DllPlugins (File) VALUES ('MCPServer.dll');\n"
PACKAGE_FILES = ("LICENSE.txt", "NOTICE.txt", "MCPServer.sql", "LLMLog.lua", "LLMLog.xml")


def fake_dll(exports, magic=0x10B):
    """A DLL holding only an export table with exports, a {name: bytes} dict. Each name's ordinal is the reverse of
    its place in the sorted name table, so a reader that confuses the two reads the wrong export."""
    base = 0x1000
    names = sorted(exports)
    count = len(names)
    functions_at, names_at, ordinals_at = 40, 40 + 4 * count, 40 + 8 * count
    body = bytearray(ordinals_at + 2 * count)
    struct.pack_into("<IIIII", body, 20, count, count, base + functions_at, base + names_at, base + ordinals_at)
    for i, name in enumerate(names):
        ordinal = count - 1 - i
        struct.pack_into("<I", body, names_at + 4 * i, base + len(body))
        body += name.encode() + bytes(1)
        struct.pack_into("<H", body, ordinals_at + 2 * i, ordinal)
        struct.pack_into("<I", body, functions_at + 4 * ordinal, base + len(body))
        body += exports[name]
    headers = bytearray(0x200)
    struct.pack_into("<I", headers, 0x3C, 0x40)
    headers[0x40:0x44] = b"PE" + bytes(2)
    struct.pack_into("<HH", headers, 0x44, 0x14C, 1)
    struct.pack_into("<H", headers, 0x54, 224)
    struct.pack_into("<H", headers, 0x58, magic)
    struct.pack_into("<II", headers, 0x58 + 96, base, len(body))
    struct.pack_into("<IIII", headers, 0x58 + 224 + 8, len(body), base, len(body), 0x200)
    return bytes(headers + body)


PLUGIN_DLL = fake_dll({"CivPluginApiVersion": struct.pack("<ii", 0, 4), "CivPluginEntry": b"code"})


class VersionFileTest(unittest.TestCase):
    def read_file(self, data):
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "VERSION")
            with open(path, "wb") as f:
                f.write(data)
            return release.parse_version(release.read_text(path))

    def test_a_release_a_bazaar_and_an_sdk_version_are_read(self):
        self.assertEqual(
            self.read_file(b"release 0.1.1\nbazaar 0.4.0\nsdk 0.4.2\n"), release.Version("0.1.1", "0.4.0", "0.4.2")
        )

    def test_windows_line_ends_and_a_byte_order_mark_are_read(self):
        data = codecs.BOM_UTF8 + b"release 0.1.1\r\nbazaar 0.4.0\r\nsdk 0.4.0\r\n"
        self.assertEqual(self.read_file(data).release, "0.1.1")

    def test_the_parts_of_the_numbers_are_named(self):
        self.assertEqual(VERSION.mod, 3)
        self.assertEqual(VERSION.api, (0, 4))

    def test_a_malformed_file_stops(self):
        for text in (
            "release 0.1\nbazaar 0.4.0\nsdk 0.4.0\n",
            "release 0.1.1\nbazaar 0.4.0\n",
            "bazaar 0.4.0\nrelease 0.1.1\nsdk 0.4.0\n",
            "release 0.1.1\nsdk 0.4.0\nbazaar 0.4.0\n",
            "release 01.1.1\nbazaar 0.4.0\nsdk 0.4.0\n",
            "release 0.1.1\nbazaar 0.4.0\nsdk 0.4.0\nmore\n",
        ):
            with self.assertRaises(release.ReleaseError):
                release.parse_version(text)

    def test_an_sdk_older_than_the_bazaar_it_needs_stops(self):
        with self.assertRaisesRegex(
            release.ReleaseError, "sdk 0.4.1 is older than its bazaar 0.4.2; raise sdk to 0.4.2"
        ):
            release.parse_version("release 0.1.1\nbazaar 0.4.2\nsdk 0.4.1\n")

    def test_an_sdk_for_a_newer_bazaar_api_stops(self):
        with self.assertRaisesRegex(
            release.ReleaseError,
            "sdk 0.5.0 is for Bazaar 0.5, which Bazaar 0.4.2 does not .*use an sdk from Bazaar 0.4",
        ):
            release.parse_version("release 0.1.1\nbazaar 0.4.2\nsdk 0.5.0\n")

    def test_this_repository_agrees_with_itself(self):
        version = release.parse_version(release.read_text(release.VERSION_FILE))
        texts = [release.read_text(p) for p in (release.VERSION_HEADER, release.PROJECT, release.WORKSHOP_TEXT)]
        self.assertEqual(release.disagreements(version, *texts), [], "run python tools/release.py check")


class AgreementTest(unittest.TestCase):
    def assert_one_problem_naming(self, word, header=HEADER, project=PROJECT, workshop=WORKSHOP):
        problems = release.disagreements(VERSION, header, project, workshop)
        self.assertEqual(len(problems), 1, problems)
        self.assertIn(word, problems[0])

    def test_matching_files_agree(self):
        self.assertEqual(release.disagreements(VERSION, HEADER, PROJECT, WORKSHOP), [])

    def test_a_workshop_text_naming_another_bazaar_is_named(self):
        self.assert_one_problem_naming("steam-workshop-description.txt", workshop=WORKSHOP.replace("0.4", "0.3"))

    def test_a_header_that_differs_is_named(self):
        self.assert_one_problem_naming("McpVersion.h", header=HEADER.replace("0.2.3", "0.2.2"))

    def test_a_header_with_no_version_is_named(self):
        self.assert_one_problem_naming("McpVersion.h", header="#pragma once\n")

    def test_a_mod_version_that_differs_is_named(self):
        self.assert_one_problem_naming("ModVersion", project=PROJECT.replace("<ModVersion>3", "<ModVersion>2"))

    def test_a_description_naming_another_bazaar_is_named(self):
        self.assert_one_problem_naming("Requires Bazaar 0.4.0", project=PROJECT.replace("0.4.0", "0.3.0"))

    def test_a_description_with_half_the_sentence_is_named(self):
        self.assert_one_problem_naming("Requires Bazaar 0.4.0", project=PROJECT.replace(" or newer", ""))

    def test_a_description_naming_another_release_or_sdk_is_named(self):
        for old, new in (("Bazaar 0.2.3,", "Bazaar 0.2.2,"), ("0.4.1 SDK", "0.4.0 SDK"), (" " + RELEASE_SENTENCE, "")):
            self.assert_one_problem_naming(RELEASE_SENTENCE, project=PROJECT.replace(old, new))

    def test_the_sentences_name_the_whole_versions(self):
        version = release.Version("0.2.3", "0.4.2", "0.4.5")
        self.assertEqual(release.bazaar_sentence(version), "Requires Bazaar 0.4.2 or newer.")
        self.assertEqual(
            release.release_sentence(version, "MCP Server for Bazaar"),
            "This is MCP Server for Bazaar 0.2.3, built with the Bazaar 0.4.5 SDK.",
        )


class OrderTest(unittest.TestCase):
    def test_the_first_release_has_mod_version_1(self):
        self.assertEqual(release.order_problems("0.1.1", []), [])
        self.assertEqual(len(release.order_problems("0.1.2", [])), 1)

    def test_each_release_raises_the_mod_version_by_one(self):
        self.assertEqual(release.order_problems("0.1.2", ["v0.1.1"]), [])
        self.assertEqual(release.order_problems("0.2.2", ["v0.1.1"]), [])
        self.assertEqual(release.order_problems("1.0.2", ["v0.1.1"]), [])
        self.assertEqual(len(release.order_problems("0.1.3", ["v0.1.1"])), 1)

    def test_the_newest_release_is_the_one_followed(self):
        self.assertEqual(release.order_problems("0.2.4", ["v0.1.1", "v0.2.3", "v0.1.2", "not-a-release"]), [])
        self.assertEqual(release.order_problems("0.1.11", ["v0.1.9", "v0.1.10"]), [])

    def test_tags_that_only_start_like_a_release_are_ignored(self):
        self.assertEqual(release.order_problems("0.1.2", ["v0.1.1", "v0.1.5-rc1", "xv0.1.7"]), [])

    def test_a_released_version_must_be_raised(self):
        problems = release.order_problems("0.1.1", ["v0.1.1"])
        self.assertEqual(len(problems), 1)
        self.assertIn("tagged as released", problems[0])
        self.assertIn("if it was never published, delete its tag here (git tag -d v0.1.1)", problems[0])

    def test_the_first_two_numbers_never_go_down(self):
        self.assertEqual(len(release.order_problems("0.1.4", ["v0.2.3"])), 1)
        self.assertEqual(len(release.order_problems("0.9.4", ["v1.0.3"])), 1)


class ApiVersionTest(unittest.TestCase):
    def test_the_version_a_dll_exports_is_read(self):
        self.assertEqual(release.exported_api_version(PLUGIN_DLL), (0, 4))
        dll = fake_dll({"CivPluginApiVersion": struct.pack("<ii", 2, 7), "AnotherExport": b"data"})
        self.assertEqual(release.exported_api_version(dll), (2, 7))

    def test_a_dll_without_the_export_gives_none(self):
        self.assertIsNone(release.exported_api_version(fake_dll({"CivPluginEntry": b"code"})))

    def test_a_64_bit_dll_gives_none(self):
        dll = fake_dll({"CivPluginApiVersion": struct.pack("<ii", 0, 4)}, magic=0x20B)
        self.assertIsNone(release.exported_api_version(dll))

    def test_bytes_that_are_not_a_dll_give_none(self):
        self.assertIsNone(release.exported_api_version(b""))
        self.assertIsNone(release.exported_api_version(b"MZ" + bytes(200)))

    def test_a_dll_the_linker_made_can_be_read(self):
        dll = os.path.join(release.BUILD_OUT, "MCPServer.dll")
        if not os.path.isfile(dll):
            self.skipTest("MCPServer.dll is not built")
        with open(dll, "rb") as f:
            self.assertIsNotNone(release.exported_api_version(f.read()))

    def test_an_sdk_header_gives_its_api_version(self):
        with tempfile.TemporaryDirectory() as sdk:
            os.makedirs(os.path.join(sdk, "include"))
            with open(os.path.join(sdk, "include", "CivPluginApi.h"), "w") as f:
                f.write("#define CIV_HOST_API_MAJOR 1\n#define CIV_HOST_API_MINOR 12\n")
            self.assertEqual(release.sdk_api_version(sdk), (1, 12))

    def test_a_folder_that_is_not_an_sdk_stops(self):
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaises(release.ReleaseError):
                release.sdk_api_version(folder)


class SdkFolderTest(unittest.TestCase):
    def test_the_sdk_is_the_one_folder_with_the_property_sheet(self):
        with tempfile.TemporaryDirectory() as root:
            folder = os.path.join(root, "trial [0.4.0]")
            os.makedirs(os.path.join(folder, "Bazaar SDK"))
            sheet = os.path.join(folder, "Bazaar SDK", "CivPlugin.vsprops")
            open(sheet, "w").close()
            self.assertEqual(release.sdk_root(folder, "the SDK"), os.path.dirname(sheet))

    def test_no_property_sheet_or_two_stop(self):
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaises(release.ReleaseError):
                release.sdk_root(folder, "the SDK")
            for name in ("one", "two"):
                os.makedirs(os.path.join(folder, name))
                open(os.path.join(folder, name, "CivPlugin.vsprops"), "w").close()
            with self.assertRaises(release.ReleaseError):
                release.sdk_root(folder, "the SDK")


class PackageTest(unittest.TestCase):
    def problems(self, modinfo=MODINFO, dll=PLUGIN_DLL, sql=SQL, leave_out=(), modinfo_count=1):
        with tempfile.TemporaryDirectory() as root:
            folder = os.path.join(root, "unpacked [v 3]")
            os.makedirs(folder)
            for i in range(modinfo_count):
                with open(os.path.join(folder, "MCP Server for Bazaar (v %d).modinfo" % (3 + i)), "w") as f:
                    f.write(modinfo)
            for name in PACKAGE_FILES:
                if name not in leave_out:
                    with open(os.path.join(folder, name), "w") as f:
                        f.write(sql if name == "MCPServer.sql" else "")
            if dll is not None:
                with open(os.path.join(folder, "MCPServer.dll"), "wb") as f:
                    f.write(dll)
            return release.package_problems(folder, VERSION, "MCP Server for Bazaar")

    def assert_one_problem_naming(self, word, **kwargs):
        problems = self.problems(**kwargs)
        self.assertEqual(len(problems), 1, problems)
        self.assertIn(word, problems[0])

    def test_a_good_package_passes(self):
        self.assertEqual(self.problems(), [])

    def test_each_file_the_package_needs_is_named_when_missing(self):
        for name in PACKAGE_FILES:
            if name != "MCPServer.sql":
                self.assert_one_problem_naming(name, leave_out=(name,))
        self.assert_one_problem_naming("MCPServer.dll", dll=None)
        self.assertIn("the package has no MCPServer.sql", self.problems(leave_out=("MCPServer.sql",)))

    def test_two_mod_descriptions_stop(self):
        self.assert_one_problem_naming("2 mod descriptions", modinfo_count=2)

    def test_another_mod_version_is_named(self):
        self.assert_one_problem_naming("mod version", modinfo=MODINFO.replace('version="3"', 'version="2"'))

    def test_a_description_naming_another_bazaar_is_named(self):
        self.assert_one_problem_naming("Requires Bazaar 0.4.0", modinfo=MODINFO.replace("0.4.0", "0.3.0"))

    def test_a_description_naming_another_release_or_sdk_is_named(self):
        for old, new in (("Bazaar 0.2.3,", "Bazaar 0.2.2,"), ("0.4.1 SDK", "0.4.0 SDK"), (" " + RELEASE_SENTENCE, "")):
            self.assert_one_problem_naming(RELEASE_SENTENCE, modinfo=MODINFO.replace(old, new))

    def test_a_mod_that_does_not_affect_saved_games_is_named(self):
        modinfo = MODINFO.replace("<AffectsSavedGames>1", "<AffectsSavedGames>0")
        self.assert_one_problem_naming("saved games", modinfo=modinfo)

    def test_a_package_that_does_not_add_the_plugin_is_named(self):
        self.assert_one_problem_naming("DllPlugins", sql=CREATE_TABLE)
        self.assert_one_problem_naming(
            "DllPlugins", sql=CREATE_TABLE + "DELETE FROM DllPlugins WHERE File = 'MCPServer.dll';\n"
        )

    def test_other_ways_of_adding_the_plugin_count(self):
        for insert in ("INSERT OR IGNORE INTO DllPlugins", 'INSERT INTO "DllPlugins"', "insert into DllPlugins"):
            self.assertEqual(self.problems(sql=CREATE_TABLE + insert + " (File) VALUES ('MCPServer.dll');\n"), [])

    def test_a_dll_built_for_another_api_is_named(self):
        self.assert_one_problem_naming("API 0.3", dll=fake_dll({"CivPluginApiVersion": struct.pack("<ii", 0, 3)}))


class ReleaseFolderTest(unittest.TestCase):
    def test_release_files_have_no_spaces_in_their_names(self):
        civ5mod, symbols = release.release_files(VERSION, PROJECT)
        self.assertEqual(os.path.basename(civ5mod), "MCP-Server-for-Bazaar-0.2.3.civ5mod")
        self.assertEqual(os.path.basename(symbols), "MCP-Server-for-Bazaar-0.2.3-symbols.zip")

    def test_a_rebuild_removes_only_what_a_build_writes(self):
        with tempfile.TemporaryDirectory() as out:
            files = [os.path.join(out, "MCP-Server-for-Bazaar-0.2.3.civ5mod"), os.path.join(out, "symbols.zip")]
            for path in files + [os.path.join(out, n) for n in ("notes.md", "my notes.txt", "build.json")]:
                open(path, "w").close()
            os.makedirs(os.path.join(out, "unpacked", "inside"))
            release.clear_build_outputs(out, files)
            self.assertEqual(sorted(os.listdir(out)), ["my notes.txt", "notes.md"])

    def test_a_folder_that_is_not_there_is_fine(self):
        with tempfile.TemporaryDirectory() as root:
            release.clear_build_outputs(os.path.join(root, "missing"), [os.path.join(root, "missing", "a.civ5mod")])

    def test_a_build_record_reads_back_as_written(self):
        with tempfile.TemporaryDirectory() as out:
            self.assertIsNone(release.read_build_info(out))
            release.write_build_info(out, "abc", True, False)
            self.assertEqual(release.read_build_info(out), {"commit": "abc", "clean": True, "downloaded_sdk": False})

    def test_a_damaged_build_record_stops(self):
        for text in ("{", "null", "[]", '{"commit": "abc", "clean": true}'):
            with tempfile.TemporaryDirectory() as out:
                with open(os.path.join(out, "build.json"), "w") as f:
                    f.write(text)
                with self.assertRaises(release.ReleaseError):
                    release.read_build_info(out)


class BuildTest(unittest.TestCase):
    def build(self, sdk_api=(0, 4)):
        """What build stops with, or None; self.downloads holds the SDK releases it downloaded and self.built the SDK
        folders build.cmd was given."""
        self.downloads = []
        self.built = []

        def download_sdk(bazaar):
            self.downloads.append(bazaar)
            return "sdk folder"

        with (
            mock.patch.object(release, "check", return_value=(VERSION, [])),
            mock.patch.object(release, "download_sdk", download_sdk),
            mock.patch.object(release, "sdk_api_version", return_value=sdk_api),
            mock.patch.object(release, "read_text", return_value=PROJECT),
            mock.patch.object(release, "tree_state", return_value=("abc", True)),
            mock.patch.object(release, "run_build_cmd", lambda sdk, package: self.built.append(sdk)),
            mock.patch.object(release, "write_release", return_value="out"),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            try:
                release.build(None)
                return None
            except release.ReleaseError as e:
                return str(e)

    def test_it_builds_with_the_sdk_on_the_sdk_line(self):
        self.assertIsNone(self.build())
        self.assertEqual((self.downloads, self.built), (["0.4.1"], ["sdk folder"]))

    def test_an_sdk_for_another_api_stops_before_building(self):
        self.assertIn("is API 0.5, not the 0.4 of VERSION's sdk 0.4.1", self.build(sdk_api=(0, 5)))
        self.assertEqual(self.built, [])


GOOD_INFO = {"commit": "abc", "clean": True, "downloaded_sdk": True}
GOOD_STATE = release.ReleaseState(
    branch="main",
    clean=True,
    head="abc",
    origin_head="abc",
    remote_tag=None,
    github_release=None,
    tags=["v0.1.1", "v0.2.2"],
    missing=[],
)
NOTES = "%s\n\nFirst release." % SENTENCE


class PublishTest(unittest.TestCase):
    def problems(self, info=GOOD_INFO, notes=NOTES, **state):
        return release.publish_problems(VERSION, info, GOOD_STATE._replace(**state), notes)

    def assert_one_problem_naming(self, words, **kwargs):
        problems = self.problems(**kwargs)
        self.assertEqual(len(problems), 1, problems)
        self.assertIn(words, problems[0])

    def test_a_clean_pushed_build_of_main_passes(self):
        self.assertEqual(self.problems(), [])

    def test_a_tag_left_on_this_commit_by_a_stopped_publish_passes(self):
        self.assertEqual(self.problems(remote_tag="abc", tags=GOOD_STATE.tags + ["v0.2.3"]), [])

    def test_the_release_must_follow_the_other_tags(self):
        self.assert_one_problem_naming("must be 2", tags=["v0.2.1"])

    def test_each_refusal_is_named(self):
        self.assert_one_problem_naming("not main", branch="release-0.2.3")
        self.assert_one_problem_naming("uncommitted", clean=False)
        self.assert_one_problem_naming("GitHub's main", origin_head="def")
        self.assert_one_problem_naming("no build", info=None)
        self.assert_one_problem_naming("not of this commit", info=dict(GOOD_INFO, commit="def"))
        self.assert_one_problem_naming("not of this commit", info=dict(GOOD_INFO, clean=False))
        self.assert_one_problem_naming("local SDK", info=dict(GOOD_INFO, downloaded_sdk=False))
        self.assert_one_problem_naming("symbols.zip", missing=["MCP-Server-for-Bazaar-0.2.3-symbols.zip"])
        self.assert_one_problem_naming("git tag -d v0.2.3", remote_tag="def")
        self.assert_one_problem_naming("notes", notes="First release.")

    def test_a_release_already_on_github_gets_one_piece_of_advice(self):
        self.assert_one_problem_naming("already released", github_release="published", remote_tag="abc")
        self.assert_one_problem_naming("already released", github_release="published", remote_tag="def")
        self.assert_one_problem_naming("check its files", github_release="draft", remote_tag="abc")
        self.assert_one_problem_naming("git tag -d v0.2.3", github_release="draft", remote_tag="def")
        self.assert_one_problem_naming("with no tag", github_release="draft", remote_tag=None)


class PublishCommandTest(unittest.TestCase):
    def commands(self, version=VERSION, **state):
        files = ("a.civ5mod", "a-symbols.zip")
        return release.publish_commands(version, GOOD_STATE._replace(**state), files, "Title", "notes.md")

    def test_the_tag_is_pushed_from_this_commit(self):
        push, _ = self.commands(origin_head="def")
        self.assertEqual(push, ["git", "push", "origin", "abc:refs/tags/v0.2.3"])

    def test_a_tag_already_on_github_is_not_pushed_again(self):
        push, _ = self.commands(remote_tag="abc")
        self.assertIsNone(push)

    def test_the_release_is_of_the_pushed_tag_in_this_repository(self):
        _, create = self.commands()
        self.assertEqual(create[:6], ["gh", "release", "create", "v0.2.3", "a.civ5mod", "a-symbols.zip"])
        self.assertIn("--verify-tag", create)
        self.assertEqual(create[create.index("--repo") + 1], release.PLUGIN_REPO)
        self.assertEqual(create[create.index("--notes-file") + 1], "notes.md")
        self.assertEqual(create[create.index("--title") + 1], "Title")

    def test_it_is_published_as_a_full_release(self):
        for version in (VERSION, release.Version("1.0.4", "0.4.0", "0.4.0")):
            _, create = self.commands(version=version)
            self.assertNotIn("--draft", create)
            self.assertNotIn("--prerelease", create)


CHANGES = """[b]0.2.4[/b]
Fixes one problem.

Fixed:
[list]
[*]A fix.
[/list]

[b]0.2.3[/b]
Shows settlers [b]good[/b] sites.

[b]0.1.2[/b]
Older.
"""
NOTE_0_2_4 = "[b]0.2.4[/b]\nFixes one problem.\n\nFixed:\n[list]\n[*]A fix.\n[/list]"


class ChangeNoteTest(unittest.TestCase):
    def test_the_newest_release_gets_only_its_own_section(self):
        self.assertEqual(release.change_note(CHANGES, "0.2.4"), NOTE_0_2_4)

    def test_an_older_release_ends_at_the_next_heading_not_at_bold_text(self):
        self.assertEqual(release.change_note(CHANGES, "0.2.3"), "[b]0.2.3[/b]\nShows settlers [b]good[/b] sites.")

    def test_the_last_section_runs_to_the_end(self):
        self.assertEqual(release.change_note(CHANGES, "0.1.2"), "[b]0.1.2[/b]\nOlder.")

    def test_a_release_without_a_section_stops(self):
        with self.assertRaisesRegex(release.ReleaseError, r"no \[b\]0\.2\.5\[/b\] section"):
            release.change_note(CHANGES, "0.2.5")


class SteamTextTest(unittest.TestCase):
    def test_a_text_at_steam_s_limit_fits(self):
        self.assertEqual(release.steam_text_problems([("note", "x" * 7999)]), [])

    def test_the_limit_counts_bytes_not_characters(self):
        (problem,) = release.steam_text_problems([("note", "x"), ("description", "é" * 4000)])
        self.assertIn("description is 8000 bytes, more than the 7999 Steam allows", problem)


PUBLISHED = GOOD_STATE._replace(github_release="published")


class WorkshopProblemsTest(unittest.TestCase):
    def problems(self, **state):
        return release.workshop_problems(VERSION, PUBLISHED._replace(**state))

    def assert_one_problem_naming(self, words, **kwargs):
        problems = self.problems(**kwargs)
        self.assertEqual(len(problems), 1, problems)
        self.assertIn(words, problems[0])

    def test_a_published_release_on_github_s_main_passes(self):
        self.assertEqual(self.problems(), [])

    def test_main_may_have_moved_on_since_the_release(self):
        self.assertEqual(self.problems(head="def", origin_head="def"), [])

    def test_each_refusal_is_named(self):
        self.assert_one_problem_naming("not main", branch="release-0.2.3")
        self.assert_one_problem_naming("uncommitted", clean=False)
        self.assert_one_problem_naming("GitHub's main", origin_head="def")

    def test_a_release_not_published_stops(self):
        for github_release in (None, "draft"):
            self.assert_one_problem_naming("v0.2.3 is not published", github_release=github_release)


class UploadTest(unittest.TestCase):
    BEFORE = {"result": 1, "consumer_app_id": release.GAME_APP, "file_size": "10", "time_updated": 100}
    AFTER = {"result": 1, "consumer_app_id": release.GAME_APP, "file_size": "3", "time_updated": 200}

    def upload(self, answers=(BEFORE, AFTER), uploader_code=0):
        """What upload stops with, or None. Steam's Web API gives the answers in turn, then the last one again, and
        raises an answer that is an exception. self.builds counts the uploader's builds, self.uploads holds its
        command lines and self.texts what its change note and description files held."""
        answers = list(answers)
        self.checks = 0
        self.builds = 0
        self.uploads = []
        self.texts = []

        def details():
            self.checks += 1
            answer = answers.pop(0) if len(answers) > 1 else answers[0]
            if isinstance(answer, Exception):
                raise answer
            return answer

        def build():
            self.builds += 1
            return "upload.exe"

        def run(command, **kwargs):
            self.uploads.append(command)
            for path in command[4:]:
                with open(path, encoding="utf-8") as f:
                    self.texts.append(f.read())
            return types.SimpleNamespace(returncode=uploader_code)

        with tempfile.TemporaryDirectory() as folder:
            package = os.path.join(folder, "MCP-Server-for-Bazaar-0.2.3.civ5mod")
            with open(package, "wb") as f:
                f.write(b"abc")
            with (
                mock.patch.object(release, "UPLOADER_OUT", folder),
                mock.patch.object(release, "steam_api", return_value="steam_api.dll"),
                mock.patch.object(release, "item_details", details),
                mock.patch.object(release, "build_uploader", build),
                mock.patch.object(release.subprocess, "run", run),
                mock.patch.object(release.time, "sleep"),
                contextlib.redirect_stdout(io.StringIO()),
            ):
                try:
                    release.upload(package, "The note.", "The description.")
                    return None
                except release.ReleaseError as e:
                    return str(e)

    def test_the_uploader_gets_the_item_the_package_the_note_and_the_description(self):
        self.assertIsNone(self.upload())
        (command,) = self.uploads
        self.assertEqual(command[:3], ["upload.exe", "steam_api.dll", "3827230313"])
        self.assertTrue(command[3].endswith("MCP-Server-for-Bazaar-0.2.3.civ5mod"))
        self.assertEqual(self.texts, ["The note.", "The description."])

    def test_an_item_that_is_hidden_or_gone_stops_before_building(self):
        self.assertIn("is not a public Civilization V item", self.upload(answers=(dict(self.BEFORE, result=9),)))
        self.assertEqual(self.builds, 0)

    def test_an_item_already_holding_the_file_stops_before_building(self):
        holding = dict(self.BEFORE, filename="MCP-Server-for-Bazaar-0.2.3.civ5mod", file_size="3")
        self.assertIn("already holds MCP-Server-for-Bazaar-0.2.3.civ5mod", self.upload(answers=(holding,)))
        self.assertEqual(self.builds, 0)

    def test_an_item_holding_another_file_of_that_name_is_updated(self):
        same_name = dict(self.BEFORE, filename="MCP-Server-for-Bazaar-0.2.3.civ5mod")
        self.assertIsNone(self.upload(answers=(same_name, self.AFTER)))
        self.assertEqual(self.builds, 1)

    def test_an_item_of_another_game_stops_before_building(self):
        message = self.upload(answers=(dict(self.BEFORE, consumer_app_id=289070),))
        self.assertIn("is not a public Civilization V item", message)
        self.assertEqual(self.builds, 0)

    def test_no_answer_from_steam_before_the_upload_stops(self):
        message = self.upload(answers=(OSError("timed out"),))
        self.assertIn("did not answer about item 3827230313: timed out", message)
        self.assertEqual(self.builds, 0)

    def test_a_failed_upload_stops(self):
        self.assertIn("the uploader stopped", self.upload(uploader_code=1))

    def test_a_new_file_that_shows_late_is_waited_for(self):
        self.assertIsNone(self.upload(answers=(self.BEFORE, self.BEFORE, self.BEFORE, self.AFTER)))
        self.assertEqual(self.checks, 4)

    def test_no_answer_after_the_upload_counts_as_not_yet(self):
        self.assertIsNone(self.upload(answers=(self.BEFORE, OSError("timed out"), ValueError("not JSON"), self.AFTER)))

    def test_an_item_still_showing_the_old_file_stops(self):
        self.assertIn("does not show the new file", self.upload(answers=(self.BEFORE,)))

    def test_an_item_updated_with_a_file_of_another_size_stops(self):
        message = self.upload(answers=(self.BEFORE, dict(self.BEFORE, time_updated=200)))
        self.assertIn("does not show the new file", message)


class BuildUploaderTest(unittest.TestCase):
    def test_without_visual_c_plus_plus_it_stops(self):
        with mock.patch.dict(os.environ, {"VS90COMNTOOLS": ""}):
            with self.assertRaisesRegex(release.ReleaseError, "Visual C\\+\\+ 2008 was not found"):
                release.build_uploader()

    def test_a_failed_build_stops_with_the_compiler_s_errors(self):
        with tempfile.TemporaryDirectory() as folder:
            open(os.path.join(folder, "vsvars32.bat"), "w").close()
            failed = types.SimpleNamespace(returncode=2, stdout="upload.cpp(1) : error C2143\n", stderr="")
            with (
                mock.patch.dict(os.environ, {"VS90COMNTOOLS": folder}),
                mock.patch.object(release, "UPLOADER_OUT", os.path.join(folder, "out")),
                mock.patch.object(release.subprocess, "run", return_value=failed),
            ):
                with self.assertRaisesRegex(release.ReleaseError, "did not build:\nupload.cpp\\(1\\) : error C2143"):
                    release.build_uploader()

    def test_a_failure_before_the_compiler_stops_with_cmd_s_error(self):
        with tempfile.TemporaryDirectory() as folder:
            open(os.path.join(folder, "vsvars32.bat"), "w").close()
            failed = types.SimpleNamespace(returncode=1, stdout="", stderr="'cl' is not recognized\n")
            with (
                mock.patch.dict(os.environ, {"VS90COMNTOOLS": folder}),
                mock.patch.object(release, "UPLOADER_OUT", os.path.join(folder, "out")),
                mock.patch.object(release.subprocess, "run", return_value=failed),
            ):
                with self.assertRaisesRegex(release.ReleaseError, "did not build:\n'cl' is not recognized"):
                    release.build_uploader()


class WorkshopTest(unittest.TestCase):
    def workshop(self, state=PUBLISHED, changes=CHANGES, description=WORKSHOP, download_fails=False):
        """What workshop stops with, or None. self.downloads holds gh's arguments and self.sent what workshop handed to
        upload, with the package's bytes in place of its path, which is gone once workshop returns."""
        self.downloads = []
        self.sent = []
        files = {
            release.VERSION_FILE: "release 0.2.4\nbazaar 0.4.0\nsdk 0.4.1\n",
            release.VERSION_HEADER: HEADER.replace("0.2.3", "0.2.4"),
            release.PROJECT: PROJECT.replace("<ModVersion>3", "<ModVersion>4").replace(
                "Bazaar 0.2.3,", "Bazaar 0.2.4,"
            ),
            release.WORKSHOP_TEXT: description,
            release.WORKSHOP_CHANGES: changes,
        }

        def gh(*args):
            self.downloads.append(args)
            if download_fails:
                raise release.ReleaseError("gh release download failed: no assets match the file pattern")
            folder = args[args.index("--dir") + 1]
            with open(os.path.join(folder, args[args.index("--pattern") + 1]), "wb") as f:
                f.write(b"published")

        def upload(package, note, description):
            with open(package, "rb") as f:
                self.sent.append((os.path.basename(package), f.read(), note, description))

        with (
            mock.patch.object(release, "read_text", files.__getitem__),
            mock.patch.object(release, "current_state", return_value=state),
            mock.patch.object(release, "gh", gh),
            mock.patch.object(release, "upload", upload),
        ):
            try:
                release.workshop()
                return None
            except release.ReleaseError as e:
                return str(e)

    def test_a_published_release_sends_github_s_package_its_note_and_the_description(self):
        self.assertIsNone(self.workshop())
        (download,) = self.downloads
        self.assertEqual(download[:3], ("release", "download", "v0.2.4"))
        self.assertEqual(download[download.index("--repo") + 1], release.PLUGIN_REPO)
        self.assertEqual(self.sent, [("MCP-Server-for-Bazaar-0.2.4.civ5mod", b"published", NOTE_0_2_4, WORKSHOP)])

    def test_a_failed_download_sends_nothing(self):
        self.assertIn("no assets match", self.workshop(download_fails=True))
        self.assertEqual(self.sent, [])

    def test_a_refused_state_sends_nothing(self):
        self.assertIn("not published", self.workshop(state=PUBLISHED._replace(github_release="draft")))
        self.assertEqual(self.sent, [])

    def test_a_release_without_a_change_note_sends_nothing(self):
        self.assertIn("no [b]0.2.4[/b] section", self.workshop(changes=CHANGES.replace("0.2.4", "0.2.5")))
        self.assertEqual(self.sent, [])

    def test_a_description_without_the_bazaar_line_sends_nothing(self):
        self.assertIn("steam-workshop-description.txt does not say", self.workshop(description="[h1]MCP[/h1]\n"))
        self.assertEqual(self.sent, [])

    def test_a_description_over_steam_s_limit_sends_nothing(self):
        self.assertIn("8000 bytes", self.workshop(description=WORKSHOP + "x" * (8000 - len(WORKSHOP))))
        self.assertEqual(self.sent, [])


if __name__ == "__main__":
    unittest.main()
