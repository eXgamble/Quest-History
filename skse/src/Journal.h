#pragma once

// The Quest Journal's Quests page: highlighting an objective shows the journal entry the quest had
// when that objective was given; no highlight shows the latest, as vanilla. Works on the journal swf
// at runtime (no swf replaced), so any journal that keeps vanilla's names works.
namespace Journal
{
	// Watch for the Journal Menu opening (kDataLoaded)
	void Register();
}
