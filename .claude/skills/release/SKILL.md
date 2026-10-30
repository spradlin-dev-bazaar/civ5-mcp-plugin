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
- The Bazaar line names the oldest Bazaar release the plugin works with, and the SDK it is built against. Raise it whenever the plugin needs something newer, an API addition or a fix.

Fetch GitHub's tags first (`git fetch --tags --force`), then read `git log v<last release>..main` and propose the number to the user.

## 2. Prepare

On a branch named `release-<version>`:

- set the number in `VERSION`, in `MCPServer/src/McpVersion.h` and in `ModVersion` in `MCPServer/mod/MCPServer.civ5proj`;
- if the Bazaar line changed, change the last sentence of the civ5proj's description, and the "Requires Bazaar" line in `docs/steam-workshop-description.txt`, to match ("Requires Bazaar X.Y.Z or newer.");
- run `python tools/release.py check` and `python -m unittest discover -s tests`;
- commit, push, and open a pull request to main. The user reviews and merges it on GitHub; the skill never merges.

## 3. Build

After the user merges the pull request, switch to main and bring it up to date with GitHub (`git switch main`, then `git pull`). Then, with nothing uncommitted, run `python tools/release.py build`. It downloads the SDK of the Bazaar release in `VERSION`, builds from scratch, and writes the package and its debug symbols to `Build\release\<version>`, keeping any notes.md already there.

## 4. Check in the game

First run the scripted checks against the release's package, which plays the test save over MCP with nobody at the keyboard (`tests/ingame/README.md` says what it needs): `python tests\ingame\run.py Build\release\<version>\MCP-Server-for-Bazaar-<version>.civ5mod`. Every check must pass.

Then ask the user to install the package from `Build\release\<version>`, not from the build folders, with the Bazaar release it names, as a player would. Then check that:

- the Plugins screen shows "MCP Server for Bazaar <version>";
- a model connects at http://127.0.0.1:7676/mcp, plays a turn, and the AI Log shows it;
- a save made with the previous release still loads (not for the first release).

## 5. Notes

Draft the notes in `Build\release\<version>\notes.md` and show the user the exact text. They open with the Bazaar line, then say what changed for players since the last release:

    Requires Bazaar <X.Y.Z> or newer.

The script refuses notes without that sentence.

## 6. Publish

`publish` needs the GitHub CLI, `gh`, logged in to an account that can make releases here (`gh auth login`). With the user's OK, run `python tools/release.py publish --notes Build\release\<version>\notes.md`. It pushes the tag `v<version>` and creates a draft GitHub release, marked as a pre-release while MAJOR is 0. If it stops partway, it says how to finish: usually delete any half-made draft on GitHub, then run it again. The user publishes the draft on GitHub.

## 7. Steam Workshop

Uploading to the Workshop stays a manual step in ModBuddy, done by the user. The Workshop page's text is `docs/steam-workshop-description.txt`; paste it into ModBuddy's description.
