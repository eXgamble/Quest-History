# Quest History: spike (2026-10-08)

**Goal:** in the Quest Journal, highlighting one of a quest's objectives shows the quest description (journal entry) that was current when that objective was given. Today the journal only ever shows the latest entry; older ones are overwritten.

**Verdict: feasible as a pure SKSE plugin, no swf edit.** A throwaway prototype (`spike/`) did the full loop in the real game with Oathvein UI's journal: record, highlight, old entry shown, latest entry restored. The journal's names and events are identical in vanilla, SkyUI and Oathvein, so one DLL should work with all three.

## How the prototype works

1. **Record:** the game fires `RE::ObjectiveState` when an objective changes state. On Dormant -> Displayed, store the quest's current journal entry (`BGSQuestInstanceText::journalStage` / `journalStageItem`) keyed by (quest, instance, objective index). First value wins.
2. **Listen:** on Journal Menu open, add a native GFx function as a `selectionChange` listener on `Page_mc.objectiveList`. That event already fires on mouse hover, keyboard and gamepad highlight (vanilla uses it for the Show on Map button).
3. **Show:** on highlight, look up the stored entry and build its text with `TESQuest::GetJournalTextForInstance` while `journalStage`/`journalStageItem` are briefly swapped (then restored), and put it in the description field. Index -1 (deselect, quest change, focus back to the quest list) restores the latest text. The layout is reserved for the tallest text up front (see 4 below), so swapping never moves the objectives list.

## Proven in game (UE_Test_AfterBear, Laid to Rest stages 20 -> 30 -> 70)

| Highlighted objective | Recorded entry | Shown |
|---|---|---|
| Investigate Alva's house | stage 70 | latest text (correct) |
| Investigate the burned house | stage 30 | the stage-30 text ("...many in the town think he did it.") |
| Talk to the Jarl | stage 20 | the stage-20 text ("...The Jarl might pay me to investigate it."), checked on screen |
| (none, index -1) | | latest text restored, checked on screen |

Log: `spike/run-2026-10-08.log`.

## Engine facts learned

- `ObjectiveState` events arrive on Papyrus VM threads (several thread IDs), so the store needs a lock. None fired during a save load.
- At event time `journalStage` was always the stage whose entry belongs to the objective (fragments run after the stage's entry is logged).
- `PlayerCharacter::GetQuestLog()` = the player's logged entries, **newest first**, each with quest, stage and item index. `TESQuest::executedStages` held nonsense (`0(f3) 51392(f30)`); don't use it.
- The swf's objective entries carry only `text, completed, failed, clipIndex, questTargetID`: **no objective index**. The prototype matches by text (exact, or the part before the first `<Alias=...>` token).
- Synthetic keys from DevBench don't reach the journal's lists; the prototype has a self-test (`SKSE/QuestHistorySpike.selftest` file with a quest editor ID) that drives the highlight from code.

## Not solved yet (the real mod's work)

1. **Objectives shown before install** (old saves): no record. The "highest logged stage at or below the objective index" guess found nothing for Unbound (logged stages 150/180/950, objectives 30-100). Vanilla data: only 43% of objectives share their index with a log-entry stage. Proposal: no guessing; those objectives keep the latest text.
2. **Matching by text breaks on duplicates** (Blood on the Ice has "Talk to <Alias=AuthorityFigure>" three times). Better: tag each objective entry with its index when the game builds the array (wrap the `RequestObjectivesData` callback that `Journal_QuestsTab::Accept` registers), or rebuild the native order.
3. **Co-save:** the prototype keeps records in memory only. Real mod: SKSE serialization of (quest, instance, objective) -> (stage, item); drop records when a quest restarts/resets (radiant quests).
4. ~~**Mouse hover flicker**~~ **FIXED (2026-10-08):** user test confirmed the loop (a different-height description moved the list under the cursor, hovering the other objective: a swap every frame). Fix: the plugin shadows the page's `SetDescriptionText` with a native wrapper. The wrapper runs the original once per candidate text (latest + every recorded old entry of the listed objectives), keeps the layout of the tallest, then shows the latest. A highlight only calls `DescriptionText.SetText`, so the list never moves (log: list y 233.15 for both texts). Cost: a gap under a short latest text when an older entry is longer. Works for every journal skin that keeps vanilla's `SetDescriptionText`. Fallback if ever needed (user idea): no swap on mouse input (`keyboardOrMouse == 0`).
5. **Same-frame stages:** if a quest sets two stages before the first stage's fragment shows its objective, the objective would be paired with the later entry. Not seen in the test.
6. Not yet checked: an old entry with `<Alias=...>` tokens after the alias was cleared, the Miscellaneous page (skipped: formID 0), gamepad and keyboard by hand, SkyUI / vanilla journals in game.
