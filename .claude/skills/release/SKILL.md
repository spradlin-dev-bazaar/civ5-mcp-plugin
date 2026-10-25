---
name: release
description: Make a release of the MCP server plugin - choose the number, prepare it on a branch, build and check the package, draft the notes, and publish a draft GitHub release. Use when the user wants to release, publish or ship a new version of this plugin.
---

# Releasing the MCP server plugin

`tools/release.py` does the repeatable work; this skill covers the steps around it that need judgment. Building needs Visual C++ 2008, so a release is made on the developer's machine. Ask the user before every step that changes something outside this machine: pushing, the pull request, publishing.

## 1. Choose the number

`VERSION` holds the release number, MAJOR.MINOR.MOD, and the Bazaar release to build against.

- MOD, the last number, is the game's mod version. It goes up by one every release and never resets.
- MINOR goes up when the release adds something, such as a tool or a feature.
- MAJOR goes up when the release breaks something, such as a tool removed or renamed or an argument changed, so prompts written for the old one stop working. MINOR then goes back to 0.
- The Bazaar line names the Bazaar release whose SDK the plugin is built against. Raise it only when the plugin needs something newer.

Read `git log v<last release>..main`, then propose the number to the user.

## 2. Prepare

On a branch named `release-<version>`:

- set the number in `VERSION`, in `MCPServer/src/McpVersion.h` and in `ModVersion` in `MCPServer/mod/MCPServer.civ5proj`;
- if the Bazaar line changed, change the last two sentences of the civ5proj's description to match ("Built against Bazaar X. Needs Bazaar X or newer.");
- run `python tools/release.py check` and `python -m unittest discover -s tests`;
- commit, push, and open a pull request to main.

## 3. Build

After the pull request merges, on main with nothing uncommitted, run `python tools/release.py build`. It downloads the SDK of the Bazaar release in `VERSION`, builds from scratch, and writes the package and its debug symbols to `Build\release\<version>`.

## 4. Check in the game

Ask the user to install the package from `Build\release\<version>`, not from the build folders, with the Bazaar release it names, as a player would. Then check that:

- the Plugins screen shows "MCP Server for Bazaar <version>";
- a model connects at http://127.0.0.1:7676/mcp, plays a turn, and the AI Log shows it;
- a save made with the previous release still loads.

## 5. Notes

Draft the notes in `Build\release\<version>\notes.md` and show the user the exact text. They open with the Bazaar line, then say what changed for players since the last release:

    Built against Bazaar <X>. Needs Bazaar <X> or newer.

The script refuses notes without that sentence.

## 6. Publish

With the user's OK, run `python tools/release.py publish --notes Build\release\<version>\notes.md`. It pushes the tag `v<version>` and creates a draft GitHub release, marked as a pre-release while MAJOR is 0. If it stops partway, it says how to finish: usually delete any half-made draft on GitHub, then run it again. The user publishes the draft on GitHub.

## 7. Steam Workshop

Uploading to the Workshop stays a manual step in ModBuddy, done by the user.
