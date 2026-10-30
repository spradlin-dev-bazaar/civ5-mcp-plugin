# In-game checks

`run.py` checks a build of the plugin in Civilization V the way a model uses it, but with a script in the model's place, so every run makes the same calls and nobody needs to be at the keyboard.

```
python tests\ingame\run.py [package.civ5mod or mod folder]
```

Without an argument it checks the mod folder `MCPServer\build.cmd` made, and refuses if a source file changed after that build. It installs the build as a test build, with the mod version 900 above the one in the `.civ5proj`, so a copy of the released mod you have installed, such as Steam's, is never replaced. Given a package or a mod folder, it checks that one as it is: use this for a release's package, and know that it takes the place of an installed copy of the same mod version for the run. Either way the plugin must report the release in `VERSION`.

A run takes about five minutes and prints PASS or FAIL for each check, then a one-line summary.

If `run.py` itself is killed part way, the checker carries on without it. While the game is handed over, the checker waits for it: run `python -m bazaar_check --hand-back` in `tests\ingame` and it closes the game and puts your files back. Ctrl+C in `run.py`'s window reaches the checker too, which then stops as `bazaar_check\README.md` describes.

## What a run does

1. Bazaar's checker, the `bazaar_check` folder, starts Civilization V through Steam with the build and the mods it needs, and loads `saves\mcp_test.Civ5Save`.
2. It hands the game over, and `mcp_checks.py` calls the plugin's tools over MCP in a fixed order, checking each answer.
3. Before handing the game back, `run.py` saves a picture of the game window to `Build\ingame\game.png`: if a check failed, it shows what was on screen.
4. The checker takes the game back, closes it, puts back everything it changed in your Civilization V folder, and checks that no plugin crashed and that `Lua.log` has no Lua errors.

## Before the first run

It needs what `bazaar_check\README.md` lists: Python 3.11 or newer, `EnableTuner = 1` and `LoggingEnabled = 1` in the game's `config.ini`, DirectX 11 chosen in Steam, and Bazaar with its VMC Bootstrap installed. Close Civilization V first.

The checker loads saves only from your saves folder, so `run.py` copies the test save there as `MCP Server checks` for the run and deletes it afterwards. A copy that was there already is left in place, and a different save of that name stops the run.

## The test save

`mcp_test.Civ5Save` is a small Continents Plus map at quick pace with six civilizations in three teams of two (Spain and Songhai, Denmark and India, Russia and Poland) and six city-states. The game's AI played every civilization for 150 turns; the save starts on turn 150, in the Renaissance, with you as Denmark. It needs only Bazaar, its VMC Bootstrap and this plugin.

## Why the checks can be exact

The game's AI makes the same moves when it gets the same orders, so a run that sends the same calls in the same order plays the same game, down to which leader makes which offer. Changing the calls or their order changes everything after them. A check that then fails says what the game gave instead: first decide whether that answer is right, and only then put it in place of the old one.

## Adding a check

`mcp_checks.py` holds the scenario: the steps listed in `SCENARIO`, each a function that plays part of the game. In a step:

- `c.answers(label, tool, **arguments)` calls a tool, checks that it answered, and returns the answer;
- `c.equal(label, actual, expected)` checks a value against the one this save gives;
- `c.refuses(label, tool, pattern, **arguments)` checks that a tool refuses, with a reason matching the pattern.

```python
spies = c.answers("spies", "spies")
c.equal("spies names the spy in Gao", [s["city"] for s in spies["spies"]], ["Gao"])
c.refuses("congress before the World Congress is founded", "congress", "has not been founded")
```

Put a new check at the end of the step it belongs to, or in a new step at the end of `SCENARIO`, so that it does not change what the game does before the checks already there. Ids of cities and units come from the save. A new tool in the plugin also goes in `TOOLS`, which the first step checks against the plugin's list.

A refused call, an answer a step cannot use, or a call `c.refuses` expected to be refused but that went through, ends that step. When the step only looks at the game (those in `READ_ONLY`), the steps after it still run; otherwise the scenario stops there and says which steps it did not run.

To try the checks against a game you have open, copy the test save into your saves folder, load it with this plugin switched on, and run `python tests\ingame\mcp_checks.py`.

## The checker

`bazaar_check` is a copy of Bazaar's checker, taken before Bazaar released its `hand_over` step. When a Bazaar release ships it, replace this copy with the one in that release's SDK.
