# civ5-mcp-plugin
A DLL Bazaar Plugin for Civilization 5 that allows an LLM with MCP functionality to play the game with you as a teammate, against you as an opponent, or play your turns for you.

## What you need

- Civilization V with Brave New World, on Windows.
- Bazaar, the multi-DLL mod loader, with its two mods: Bazaar Multi-DLL Mod Loader and Bazaar - VMC Bootstrap (required). Each release of this plugin says which Bazaar version it requires.
- An MCP client on the same PC, such as Claude Code.

## Installing

1. Install Bazaar's two mods.
2. Download the `.civ5mod` file from this plugin's release on GitHub, and put it in `Documents\My Games\Sid Meier's Civilization 5\MODS`.
3. In the game's Mods menu, turn on Bazaar - VMC Bootstrap (required), Bazaar Multi-DLL Mod Loader and MCP Server for Bazaar.
4. Start or load a single-player game. While it runs, the server is at `http://127.0.0.1:7676/mcp`.
5. Connect your MCP client to that address. For Claude Code: `claude mcp add --transport http civ5 http://127.0.0.1:7676/mcp`

This plugin replaces the Local MCP Server DLL mod, so turn that one off.

## Making a release

Run `python tools/release.py check`, then `build`, then `publish`. The steps around them are in `.claude/skills/release/SKILL.md`.
