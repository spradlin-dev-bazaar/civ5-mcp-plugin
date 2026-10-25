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
PROJECT = "<ModVersion>3</ModVersion>\n<Description>Plays Civ V. %s</Description>" % SENTENCE
VERSION = release.Version("0.2.3", "0.4.0")
MODINFO = (
    """<Mod id="x" version="3">
  <Properties><Description>Plays Civ V. %s</Description></Properties>
  <Files><File md5="0" import="0">MCPServer.dll</File><File md5="0" import="0">LLMLog.lua</File></Files>
  <Actions><OnModActivated><UpdateDatabase>MCPServer.sql</UpdateDatabase></OnModActivated></Actions>
  <EntryPoints><EntryPoint type="InGameUIAddin" file="LLMLog.xml"><Name>AI Log</Name></EntryPoint></EntryPoints>
</Mod>
"""
    % SENTENCE
)
PACKAGE_FILES = ("LICENSE.txt", "NOTICE.txt", "MCPServer.sql", "LLMLog.lua", "LLMLog.xml")


def fake_dll(exports):
    """A 32-bit DLL holding only an export table with exports, a {name: bytes} dict. Each name's ordinal is the
    reverse of its place in the sorted name table, so a reader that confuses the two reads the wrong export."""
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
    struct.pack_into("<H", headers, 0x58, 0x10B)
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

    def test_the_mod_and_api_versions_come_from_the_numbers(self):
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
        self.assertIn("already released", problems[0])

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

    def test_bytes_that_are_not_a_dll_give_none(self):
        self.assertIsNone(release.exported_api_version(b""))
        self.assertIsNone(release.exported_api_version(b"MZ" + bytes(200)))

    def test_the_built_plugin_declares_the_api_of_its_bazaar(self):
        dll = os.path.join(release.BUILD_OUT, "MCPServer.dll")
        if not os.path.isfile(dll):
            self.skipTest("MCPServer.dll is not built")
        with open(dll, "rb") as f:
            api = release.exported_api_version(f.read())
        self.assertEqual(api, release.parse_version(release.read_text(release.VERSION_FILE)).api)

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


class PackageTest(unittest.TestCase):
    def problems(self, modinfo=MODINFO, dll=PLUGIN_DLL, leave_out=()):
        with tempfile.TemporaryDirectory() as folder:
            with open(os.path.join(folder, "MCP Server for Bazaar (v 3).modinfo"), "w") as f:
                f.write(modinfo)
            for name in PACKAGE_FILES:
                if name not in leave_out:
                    open(os.path.join(folder, name), "w").close()
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
            self.assert_one_problem_naming(name, leave_out=(name,))
        self.assert_one_problem_naming("MCPServer.dll", dll=None)

    def test_another_mod_version_is_named(self):
        self.assert_one_problem_naming("mod version", modinfo=MODINFO.replace('version="3"', 'version="2"'))

    def test_a_description_naming_another_bazaar_is_named(self):
        self.assert_one_problem_naming("Bazaar", modinfo=MODINFO.replace("0.4", "0.3"))

    def test_a_dll_built_for_another_api_is_named(self):
        self.assert_one_problem_naming("API 0.3", dll=fake_dll({"CivPluginApiVersion": struct.pack("<ii", 0, 3)}))


class ReleaseFolderTest(unittest.TestCase):
    def test_a_rebuild_keeps_only_the_notes(self):
        with tempfile.TemporaryDirectory() as out:
            for name in ("notes.md", "build.json", "MCP-Server-for-Bazaar-0.2.3.civ5mod"):
                open(os.path.join(out, name), "w").close()
            os.makedirs(os.path.join(out, "unpacked", "inside"))
            release.clear_release_folder(out)
            self.assertEqual(os.listdir(out), ["notes.md"])

    def test_a_folder_that_is_not_there_is_fine(self):
        with tempfile.TemporaryDirectory() as root:
            release.clear_release_folder(os.path.join(root, "missing"))


GOOD_INFO = {"commit": "abc", "clean": True, "downloaded_sdk": True}
GOOD_STATE = release.PublishState(
    branch="main",
    clean=True,
    head="abc",
    origin_head="abc",
    remote_tag=None,
    release=None,
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
        self.assert_one_problem_naming("delete that tag", remote_tag="def")
        self.assert_one_problem_naming("draft release", release="draft", remote_tag="abc")
        self.assert_one_problem_naming("already released", release="published", remote_tag="abc")
        self.assert_one_problem_naming("notes", notes="First release.")


class GitHubTest(unittest.TestCase):
    def test_a_lightweight_tag_names_its_commit(self):
        self.assertEqual(release.remote_commit("abc\trefs/tags/v0.1.1", "v0.1.1"), "abc")

    def test_an_annotated_tag_names_the_commit_it_points_at(self):
        listing = "tagobject\trefs/tags/v0.1.1\nabc\trefs/tags/v0.1.1^{}"
        self.assertEqual(release.remote_commit(listing, "v0.1.1"), "abc")

    def test_a_missing_tag_names_nothing(self):
        self.assertIsNone(release.remote_commit("", "v0.1.1"))

    def test_a_release_is_a_draft_published_or_missing(self):
        releases = [{"tagName": "v0.1.1", "isDraft": False}, {"tagName": "v0.1.2", "isDraft": True}]
        self.assertEqual(release.release_state(releases, "v0.1.1"), "published")
        self.assertEqual(release.release_state(releases, "v0.1.2"), "draft")
        self.assertIsNone(release.release_state(releases, "v0.1.3"))


if __name__ == "__main__":
    unittest.main()
