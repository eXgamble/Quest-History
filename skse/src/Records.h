#pragma once

// Which journal entry each objective came with. When an objective is first shown, the quest's
// journal entry at that moment (its stage and log entry item) is recorded, per quest instance.
// Saved in the SKSE co-save. Objectives shown before the mod was installed have no record.
namespace Records
{
	struct Record
	{
		std::uint16_t stage = 0;  // the stage whose log entry was the quest's description
		std::int8_t   item = 0;   // which of that stage's log entries
		std::uint32_t order = 0;  // when it was shown: higher = later
	};

	// Event sinks and co-save callbacks. Call at kDataLoaded / plugin load respectively.
	void RegisterSinks();
	void RegisterSerialization();

	// Objective events while a save loads are the game restoring state, not new objectives
	void SetLoading(bool a_loading);

	std::optional<Record> Find(RE::FormID a_quest, std::uint32_t a_instance, std::uint16_t a_objective);
}
