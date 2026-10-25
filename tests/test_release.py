"""Tests for tools/release.py's rules, without building anything.

python -m unittest discover -s tests
"""

import codecs
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))

import release  # noqa: E402

HEADER = '#pragma once\n#define MCP_SERVER_VERSION "0.2.3"\n'
SENTENCE = "Requires Bazaar 0.4.0 or newer."
PROJECT = (
    "<Name>MCP Server for Bazaar</Name>\n<ModVersion>3</ModVersion>\n<Description>Plays Civ V. %s</Description>"
    % (SENTENCE)
)
VERSION = release.Version("0.2.3", "0.4.0")
MODINFO = (
    """<Mod id="x" version="3">
  <Properties><Description>Plays Civ V. %s</Description><AffectsSavedGames>1</AffectsSavedGames></Properties>
  <Files><File md5="0" import="0">MCPServer.dll</File><File md5="0" import="0">LLMLog.lua</File></Files>
  <Actions><OnModActivated><UpdateDatabase>MCPServer.sql</UpdateDatabase></OnModActivated></Actions>
  <EntryPoints><EntryPoint type="InGameUIAddin" file="LLMLog.xml"><Name>AI Log</Name></EntryPoint></EntryPoints>
</Mod>
"""
    % SENTENCE
)
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

    def test_a_release_and_a_bazaar_version_are_read(self):
        self.assertEqual(self.read_file(b"release 0.1.1\nbazaar 0.4.0\n"), release.Version("0.1.1", "0.4.0"))

    def test_windows_line_ends_and_a_byte_order_mark_are_read(self):
        self.assertEqual(self.read_file(codecs.BOM_UTF8 + b"release 0.1.1\r\nbazaar 0.4.0\r\n").release, "0.1.1")

    def test_the_parts_of_the_numbers_are_named(self):
        self.assertEqual(VERSION.major, 0)
        self.assertEqual(VERSION.mod, 3)
        self.assertEqual(VERSION.api, (0, 4))

    def test_a_malformed_file_stops(self):
        for text in (
            "release 0.1\nbazaar 0.4.0\n",
            "release 0.1.1\n",
            "bazaar 0.4.0\nrelease 0.1.1\n",
            "release 01.1.1\nbazaar 0.4.0\n",
            "release 0.1.1\nbazaar 0.4.0\nmore\n",
        ):
            with self.assertRaises(release.ReleaseError):
                release.parse_version(text)

    def test_this_repository_agrees_with_itself(self):
        version = release.parse_version(release.read_text(release.VERSION_FILE))
        header, project = release.read_text(release.VERSION_HEADER), release.read_text(release.PROJECT)
        self.assertEqual(release.disagreements(version, header, project), [], "run python tools/release.py check")


class AgreementTest(unittest.TestCase):
    def assert_one_problem_naming(self, word, header=HEADER, project=PROJECT):
        problems = release.disagreements(VERSION, header, project)
        self.assertEqual(len(problems), 1, problems)
        self.assertIn(word, problems[0])

    def test_matching_files_agree(self):
        self.assertEqual(release.disagreements(VERSION, HEADER, PROJECT), [])

    def test_a_header_that_differs_is_named(self):
        self.assert_one_problem_naming("McpVersion.h", header=HEADER.replace("0.2.3", "0.2.2"))

    def test_a_header_with_no_version_is_named(self):
        self.assert_one_problem_naming("McpVersion.h", header="#pragma once\n")

    def test_a_mod_version_that_differs_is_named(self):
        self.assert_one_problem_naming("ModVersion", project=PROJECT.replace("<ModVersion>3", "<ModVersion>2"))

    def test_a_description_naming_another_bazaar_is_named(self):
        self.assert_one_problem_naming("description", project=PROJECT.replace("0.4", "0.3"))

    def test_a_description_with_half_the_sentence_is_named(self):
        self.assert_one_problem_naming("description", project=PROJECT.replace(" or newer", ""))

    def test_the_sentence_names_the_whole_bazaar_version(self):
        self.assertEqual(release.bazaar_sentence(release.Version("0.2.3", "0.4.2")), "Requires Bazaar 0.4.2 or newer.")


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
            return release.package_problems(folder, VERSION)

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
        self.assert_one_problem_naming("Bazaar", modinfo=MODINFO.replace("0.4", "0.3"))

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


GOOD_INFO = {"commit": "abc", "clean": True, "downloaded_sdk": True}
GOOD_STATE = release.PublishState(
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

    def test_the_release_is_a_draft_of_the_pushed_tag_in_this_repository(self):
        _, create = self.commands()
        self.assertEqual(create[:6], ["gh", "release", "create", "v0.2.3", "a.civ5mod", "a-symbols.zip"])
        for flag in ("--draft", "--verify-tag"):
            self.assertIn(flag, create)
        self.assertEqual(create[create.index("--repo") + 1], release.PLUGIN_REPO)
        self.assertEqual(create[create.index("--notes-file") + 1], "notes.md")
        self.assertEqual(create[create.index("--title") + 1], "Title")

    def test_it_is_a_pre_release_only_before_1_0(self):
        self.assertIn("--prerelease", self.commands()[1])
        self.assertNotIn("--prerelease", self.commands(version=release.Version("1.0.4", "0.4.0"))[1])


if __name__ == "__main__":
    unittest.main()
