#include "Journal.h"

#include "Records.h"

namespace
{
	using State = RE::QUEST_OBJECTIVE_STATE;
	constexpr auto PAGE = "_root.QuestJournalFader.Menu_mc.QuestsFader.Page_mc";
	constexpr auto ORIGINAL_LAYOUT = "qhOrigSetDescriptionText";

	RE::BGSQuestInstanceText* FindInstance(RE::TESQuest* a_quest, std::uint32_t a_id)
	{
		for (auto* inst : a_quest->instanceData) {
			if (inst && inst->id == a_id) {
				return inst;
			}
		}
		return nullptr;
	}

	// The journal entry a record points to, with its <Alias=...> and <Global=...> filled in the way
	// the game fills the current one: the quest instance briefly points at the old entry.
	std::string EntryText(RE::TESQuest* a_quest, std::uint32_t a_instance, const Records::Record& a_record)
	{
		const auto inst = FindInstance(a_quest, a_instance);
		if (!inst) {
			return {};
		}
		const auto   stage = inst->journalStage;
		const auto   item = inst->journalStageItem;
		RE::BSString text;
		inst->journalStage = a_record.stage;
		inst->journalStageItem = a_record.item;
		a_quest->GetJournalTextForInstance(text, a_instance);
		inst->journalStage = stage;
		inst->journalStageItem = item;
		return text.c_str() ? text.c_str() : "";
	}

	// Does the journal's objective text belong to this objective? The journal shows the text with
	// tokens filled in, so a text with tokens is compared up to its first token.
	bool TextMatches(const RE::BGSQuestObjective* a_objective, std::string_view a_shown, bool a_exact)
	{
		const std::string_view raw{ a_objective->displayText.c_str() ? a_objective->displayText.c_str() : "" };
		if (a_exact) {
			return raw == a_shown;
		}
		const auto cut = raw.find('<');
		return cut != std::string_view::npos && cut >= 4 && a_shown.starts_with(raw.substr(0, cut));
	}

	// What the page shows right now: the selected quest and its objective entries
	struct Page
	{
		RE::GFxValue  page, titleList, sel, objList, entries, descText;
		RE::TESQuest* quest = nullptr;
		std::uint32_t instance = 0;

		explicit Page(const RE::GFxValue& a_page) :
			page(a_page)
		{
			page.GetMember("TitleList", &titleList);
			titleList.GetMember("selectedEntry", &sel);
			page.GetMember("ObjectiveList", &objList);
			objList.GetMember("entryList", &entries);
			page.GetMember("DescriptionText", &descText);
			if (sel.IsObject()) {
				RE::GFxValue formID, inst;
				sel.GetMember("formID", &formID);
				sel.GetMember("instance", &inst);
				const auto id = formID.IsNumber() ? static_cast<RE::FormID>(formID.GetNumber()) : 0;
				instance = inst.IsNumber() ? static_cast<std::uint32_t>(inst.GetNumber()) : 0;
				quest = id ? RE::TESForm::LookupByID<RE::TESQuest>(id) : nullptr;  // none on the Miscellaneous page
			}
		}

		std::string EntryTextOf(std::uint32_t a_index) const
		{
			RE::GFxValue entry, text;
			if (!entries.IsArray() || !entries.GetElement(a_index, &entry) || !entry.GetMember("text", &text) || !text.IsString()) {
				return {};
			}
			return text.GetString();
		}

		// The objective behind the list entry at a_index. Several objectives can show the same text
		// (e.g. "Talk to <Alias=Jarl>" three times): the journal lists the newest first, so the k-th
		// entry with that text is the k-th most recently shown of them.
		std::optional<Records::Record> RecordAt(std::uint32_t a_index) const
		{
			const auto shown = EntryTextOf(a_index);
			if (!quest || shown.empty()) {
				return std::nullopt;
			}
			std::vector<std::pair<RE::BGSQuestObjective*, std::optional<Records::Record>>> candidates;
			for (const bool exact : { true, false }) {
				for (auto* objective : quest->objectives) {
					if (objective && objective->state.get() != State::kDormant && TextMatches(objective, shown, exact)) {
						candidates.emplace_back(objective, Records::Find(quest->GetFormID(), instance, objective->index));
					}
				}
				if (!candidates.empty()) {
					break;
				}
			}
			if (candidates.empty()) {
				return std::nullopt;
			}
			std::uint32_t sameTextAbove = 0;
			for (std::uint32_t i = 0; i < a_index; ++i) {
				sameTextAbove += EntryTextOf(i) == shown;
			}
			std::ranges::stable_sort(candidates, std::greater{}, [](const auto& a_candidate) { return a_candidate.second ? a_candidate.second->order : 0u; });
			return sameTextAbove < candidates.size() ? candidates[sameTextAbove].second : std::nullopt;
		}

		// The entry the objective at a_index came with; empty when it has none (given before the mod
		// was installed) or it is the latest one anyway
		std::string OldTextAt(std::uint32_t a_index) const
		{
			const auto record = RecordAt(a_index);
			return record ? EntryText(quest, instance, *record) : std::string{};
		}

		std::string Latest() const
		{
			RE::GFxValue description;
			sel.GetMember("description", &description);
			return description.IsString() ? description.GetString() : "";
		}

		double ListY() const
		{
			RE::GFxValue y;
			objList.GetMember("_y", &y);
			return y.IsNumber() ? y.GetNumber() : 0.0;
		}

		void ShowText(const std::string& a_text)
		{
			RE::GFxValue arg(a_text.c_str());
			descText.Invoke("SetText", nullptr, &arg, 1);
		}
	};

	// The page lays the objectives out under the description. If a highlight changed the description's
	// height, the list would move under the mouse and the mouse would highlight another objective.
	// So the page's own SetDescriptionText is wrapped: it lays out once for the TALLEST text this quest
	// can show, then shows the latest. A highlight only swaps the text, so the list never moves.
	void LayOut(const RE::GFxValue& a_page)
	{
		Page       page{ a_page };
		const auto layout = [&] { page.page.Invoke(ORIGINAL_LAYOUT); };
		const auto latest = page.Latest();
		std::vector<std::string> olds;
		for (std::uint32_t i = 0; page.quest && page.entries.IsArray() && i < page.entries.GetArraySize(); ++i) {
			if (auto text = page.OldTextAt(i); !text.empty() && text != latest && std::ranges::find(olds, text) == olds.end()) {
				olds.push_back(std::move(text));
			}
		}
		layout();
		if (olds.empty()) {
			return;  // vanilla layout
		}
		double      tallest = page.ListY();
		std::string best;
		for (const auto& text : olds) {
			page.sel.SetMember("description", RE::GFxValue(text.c_str()));
			layout();
			if (const auto y = page.ListY(); y > tallest) {
				tallest = y;
				best = text;
			}
		}
		if (!best.empty()) {
			page.sel.SetMember("description", RE::GFxValue(best.c_str()));
			layout();
		}
		page.sel.SetMember("description", RE::GFxValue(latest.c_str()));  // the page restores from this
		page.ShowText(latest);
	}

	void OnHighlight(RE::GFxMovie* a_movie, int a_index)
	{
		RE::GFxValue value;
		if (!a_movie->GetVariable(&value, PAGE)) {
			return;
		}
		Page page{ value };
		if (!page.quest) {
			return;
		}
		const auto text = a_index >= 0 ? page.OldTextAt(static_cast<std::uint32_t>(a_index)) : std::string{};
		page.ShowText(text.empty() ? page.Latest() : text);
	}

	class HighlightHandler : public RE::GFxFunctionHandler
	{
	public:
		void Call(Params& a_params) override
		{
			if (a_params.argCount < 1) {
				return;
			}
			RE::GFxValue index;
			a_params.args[0].GetMember("index", &index);
			OnHighlight(a_params.movie, index.IsNumber() ? static_cast<int>(index.GetNumber()) : -1);
		}
	};

	class LayOutHandler : public RE::GFxFunctionHandler
	{
	public:
		void Call(Params& a_params) override
		{
			if (a_params.thisPtr) {
				LayOut(*a_params.thisPtr);
			}
		}
	};

	void Attach()
	{
		const auto   ui = RE::UI::GetSingleton();
		const auto   menu = ui ? ui->GetMenu(RE::JournalMenu::MENU_NAME) : nullptr;
		const auto   movie = menu ? menu->uiMovie.get() : nullptr;
		RE::GFxValue page, list, original;
		if (!movie || !movie->GetVariable(&page, PAGE) || !page.GetMember("ObjectiveList", &list) || !page.GetMember("SetDescriptionText", &original)) {
			static bool warned = false;
			if (!std::exchange(warned, true)) {
				logger::warn("The journal's Quests page isn't the vanilla layout; Quest History can't show past entries");
			}
			return;
		}
		static auto* highlight = new HighlightHandler();  // live for the session
		static auto* layOut = new LayOutHandler();

		// the objective list already fires selectionChange on every highlight (mouse, keyboard, gamepad)
		RE::GFxValue scope, function;
		movie->CreateObject(&scope);
		movie->CreateFunction(&function, highlight);
		scope.SetMember("onSelect", function);
		std::array<RE::GFxValue, 3> args{ RE::GFxValue("selectionChange"), scope, RE::GFxValue("onSelect") };
		list.Invoke("addEventListener", nullptr, args.data(), args.size());

		// shadow the page's SetDescriptionText (the original stays reachable for LayOut)
		RE::GFxValue wrapped;
		page.GetMember(ORIGINAL_LAYOUT, &wrapped);
		if (wrapped.IsUndefined()) {
			page.SetMember(ORIGINAL_LAYOUT, original);
			RE::GFxValue wrapper;
			movie->CreateFunction(&wrapper, layOut);
			page.SetMember("SetDescriptionText", wrapper);
		}
		page.Invoke("SetDescriptionText");  // the quest shown on open
	}

	class MenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		static MenuSink* GetSingleton()
		{
			static MenuSink singleton;
			return &singleton;
		}

		RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
		{
			if (a_event && a_event->opening && a_event->menuName == RE::JournalMenu::MENU_NAME) {
				SKSE::GetTaskInterface()->AddUITask(Attach);
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};
}

namespace Journal
{
	void Register()
	{
		RE::UI::GetSingleton()->AddEventSink<RE::MenuOpenCloseEvent>(MenuSink::GetSingleton());
	}
}
