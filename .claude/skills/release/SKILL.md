---
name: release
description: Make a release of the MCP server plugin - choose the number, prepare it on a branch with its notes, build and check the package, publish the GitHub release and upload it to the Steam Workshop. Use when the user wants to release, publish or ship a new version of this plugin.
---

# Releasing the MCP server plugin

`tools/release.py` does the repeatable work; this skill covers the steps around it that need judgment. Building needs Visual C++ 2008, so a release is made on the developer's machine. The user approves the number and, all at once, every text others will read; with those approved, merging and publishing need no further OK. The Workshop upload needs the user's go-ahead just before it runs (step 6).

## 1. Choose the number

`VERSION` holds the release number, MAJOR.MINOR.MOD, the oldest Bazaar release the plugin works with, and the Bazaar release whose SDK it is built with.

- MOD, the last number, is the game's mod version. It goes up by one every release and never resets.
- MINOR goes up when the release adds something, such as a tool or a feature.
- MAJOR goes up when the release breaks something, such as a tool removed or renamed or an argument changed, so prompts written for the old one stop working. MINOR then goes back to 0.
- The bazaar line names the oldest Bazaar release the plugin works with. Raise it whenever the plugin needs something newer, an API addition or a fix.
- The sdk line names the Bazaar release whose SDK the plugin is built with, usually the newest Bazaar release with the same first two numbers as the bazaar line (`gh release list --repo spradlin-dev-bazaar/civ5-bazaar` lists them). The script refuses an SDK older than the bazaar line or with other first two numbers.

Fetch GitHub's tags first (`git fetch --tags --force`), then read `git log v<last release>..main` and propose the number to the user.

## 2. Prepare

On a branch named `release-<version>`:

- set the number in `VERSION`, in `MCPServer/src/McpVersion.h`, in `ModVersion` in `MCPServer/mod/MCPServer.civ5proj`, and in the sentence that ends the civ5proj's description, which also names the sdk line: "This is MCP Server for Bazaar <version>, built with the Bazaar <sdk> SDK.";
- if the bazaar line changed, change the "Requires Bazaar" sentence in the civ5proj's description, and the "Requires Bazaar" line in `docs/steam-workshop-description.txt`, to match ("Requires Bazaar X.Y.Z or newer.");
- draft the release notes in `Build\release\<version>\notes.md`. They open with the Bazaar line, `Requires Bazaar <X.Y.Z> or newer.`, which `publish` requires, say what changed for players since the last release, and end with how to install it, as the last release's notes do;
- add the same changes at the top of `docs/steam-workshop-changes.txt` in BBCode, under a `[b]<version>[/b]` line, without the install steps or the file names. `workshop` uploads that section as the Workshop change note;
- run `python tools/release.py check` and `python -m unittest discover -s tests`;
- put the commit message and the pull request's title and description in review files in `Build\drafts\`, open them, the notes and the change-note file in CLion for the user to edit, and get them all approved together; use the text as the user saved it, then delete the review files;
- commit, push, and open a pull request to main. Merge it and delete the branch, on GitHub and here: `gh pr merge --merge --delete-branch`.

## 3. Build

After the merge, switch to main and bring it up to date with GitHub (`git switch main`, then `git pull`). Then, with nothing uncommitted, run `python tools/release.py build`. It downloads the SDK of the Bazaar release on `VERSION`'s sdk line, builds from scratch, and writes the package and its debug symbols to `Build\release\<version>`, keeping any notes.md already there.

## 4. Check in the game

Run the scripted checks against the release's package. They play the test save over MCP with nobody at the keyboard (`tests/ingame/README.md` says what they need): `python tests\ingame\run.py Build\release\<version>\MCP-Server-for-Bazaar-<version>.civ5mod`. Every check must pass. These checks are the game check: don't ask the user to install and play the release.

If a check fails, fix it through a pull request, then build and check again. If the fix changes what the notes or the change note say, get the new text approved.

## 5. Publish

`publish` needs the GitHub CLI, `gh`, logged in to an account that can make releases here (`gh auth login`). Run `python tools/release.py publish --notes Build\release\<version>\notes.md`. It pushes the tag `v<version>` and publishes the GitHub release with the package, its debug symbols and the notes. If it stops partway, it says how to finish: usually delete any half-made draft on GitHub, then run it again.

## 6. Steam Workshop

Ask the user for the go-ahead to deploy: the upload reaches every subscriber at once, and Claude Code's auto mode refuses it without the user's approval of that upload. Then run `python tools/release.py workshop`. It needs Steam running and signed in to the account that owns the plugin's Workshop item, and the Sid Meier's Civilization V SDK installed from Steam; ModBuddy itself need not be open. It downloads the package published on GitHub and uploads it to the item, with the release's section of `docs/steam-workshop-changes.txt` as the change note and `docs/steam-workshop-description.txt` as the page's description, then checks that Steam shows the new file. It refuses when the item already holds that file.

`tools/workshop/upload.cpp` is a copy of Bazaar's uploader, in its repository's `tools/workshop`. When a Bazaar SDK ships the uploader, use that one instead.
