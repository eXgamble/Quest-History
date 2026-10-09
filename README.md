# Quest History

An SKSE plugin for Skyrim Anniversary Edition (1.6.x and 1.7.x). In the Quest Journal, each quest
only shows its latest description; every new stage overwrites the last one. With Quest History,
**highlighting an objective shows the description the quest had when that objective was given**.
Move off the objectives and the latest description is back, as in vanilla.

- Works with mouse, keyboard and controller: highlighting is the journal's own (the same highlight
  vanilla uses for Show on Map).
- Names in old descriptions are filled in the way the game fills the current one.
- The objectives list never jumps while you hover: the page reserves room for the tallest
  description of the quest.
- No swf replaced, no plugin (esp), no MCM. Works with journal skins that keep the vanilla Quests
  page (tested with Oathvein UI).

**Requirements:** [SKSE64](https://skse.silverlock.org/),
[Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444).

## What to expect on an existing save

Quest History remembers each description when an objective appears, from the moment it's installed
(saved with your game in the SKSE co-save). Objectives you got before installing it have no record
and keep showing the latest description. Restarting a repeatable quest starts its history fresh.

## How it works

1. When an objective is first shown (`RE::ObjectiveState` event), the quest's current journal entry
   (`BGSQuestInstanceText` stage and log item) is recorded per quest instance and objective.
2. When the Journal opens, a native listener is added to the Quests page's objective list
   (`selectionChange`, fired for every highlight), and the page's `SetDescriptionText` is wrapped to
   lay the page out for the tallest description.
3. On a highlight, the old entry's text is built with `TESQuest::GetJournalTextForInstance` while the
   instance briefly points at the old entry (so tokens are filled), and shown in the description.

The journal's objective entries carry only their text, so entries are matched to objectives by text;
several objectives with the same text are told apart by the order they were shown (the journal lists
the newest first). See [docs/SPIKE.md](docs/SPIKE.md) for the investigation behind this.

## Building

```
skse\build.bat
```

Needs Visual Studio 2026 Build Tools with vcpkg. CommonLibSSE-NG is a git submodule
(`skse/lib/commonlibsse-ng`, pinned to v10.0.0): clone with `--recurse-submodules`. The DLL is
copied to `mod/SKSE/Plugins` (the MO2 mod folder "Quest History" is a junction to `mod/`).

## License

GPL-3.0-or-later (the plugin statically links CommonLibSSE-NG).
