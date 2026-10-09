// Quest History SPIKE (throwaway): answers the open questions before the real mod is designed.
// 1. Does RE::ObjectiveState fire when an objective is shown, and what is the quest's current
//    journal entry (BGSQuestInstanceText::journalStage/Item) at that moment?
// 2. What does PlayerCharacter::questLog / TESQuest::executedStages hold, in what order?
// 3. Can a native listener on the journal's objectiveList (no swf edit) swap the description,
//    and does GetJournalTextForInstance with a swapped journalStage give the old entry with tokens filled?
// No co-save: records live only until the next load.

#include <fstream>
#include <thread>

namespace
{
	using State = RE::QUEST_OBJECTIVE_STATE;
	constexpr auto kPage = "_root.QuestJournalFader.Menu_mc.QuestsFader.Page_mc";

	struct Rec
	{
		std::uint16_t stage;
		std::int8_t   item;
	};
	std::mutex                                                       g_lock;
	std::map<std::tuple<RE::FormID, std::uint32_t, std::uint16_t>, Rec> g_recs;  // quest, instance, objective -> entry shown then
	std::atomic_bool                                                 g_loading{ false };

	void InitializeLog()
	{
		auto path = logger::log_directory();
		if (!path) {
			SKSE::stl::report_and_fail("Failed to find the SKSE log directory"sv);
		}
		*path /= "QuestHistorySpike.log"sv;
		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
		auto log = std::make_shared<spdlog::logger>("global log"s, std::move(sink));
		log->set_level(spdlog::level::info);
		log->flush_on(spdlog::level::info);
		spdlog::set_default_logger(std::move(log));
		spdlog::set_pattern("[%H:%M:%S:%e] %v"s);
	}

	const char* Edid(const RE::TESForm* a_form) { return a_form ? a_form->GetFormEditorID() : ""; }

	RE::BGSQuestInstanceText* FindInstance(RE::TESQuest* a_quest, std::uint32_t a_id)
	{
		for (auto* inst : a_quest->instanceData) {
			if (inst && inst->id == a_id) {
				return inst;
			}
		}
		return nullptr;
	}

	// ---------- 1. objective events ----------
	class ObjectiveSink : public RE::BSTEventSink<RE::ObjectiveState::Event>
	{
	public:
		static ObjectiveSink* Get()
		{
			static ObjectiveSink sink;
			return &sink;
		}

		RE::BSEventNotifyControl ProcessEvent(const RE::ObjectiveState::Event* a_event, RE::BSTEventSource<RE::ObjectiveState::Event>*) override
		{
			if (!a_event || !a_event->objective) {
				return RE::BSEventNotifyControl::kContinue;
			}
			auto* obj = a_event->objective;
			auto* quest = obj->ownerQuest;
			auto* inst = quest ? FindInstance(quest, quest->currentInstanceID) : nullptr;
			logger::info("ObjectiveState {} obj {} '{}' {}->{} loading={} thread={} curStage={} inst={} journalStage={} journalItem={}",
				Edid(quest), obj->index, obj->displayText.c_str(), static_cast<int>(a_event->oldState), static_cast<int>(a_event->newState),
				g_loading.load(), GetCurrentThreadId(), quest ? quest->currentStage : 0, quest ? quest->currentInstanceID : 0,
				inst ? inst->journalStage : 0xFFFF, inst ? inst->journalStageItem : -2);
			if (!g_loading && quest && inst && a_event->newState == State::kDisplayed && a_event->oldState == State::kDormant) {
				std::scoped_lock l{ g_lock };
				g_recs.try_emplace({ quest->GetFormID(), inst->id, obj->index }, Rec{ inst->journalStage, inst->journalStageItem });
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	// ---------- 3. journal highlight ----------
	RE::BGSQuestObjective* MatchObjective(RE::TESQuest* a_quest, const std::string& a_text)
	{
		std::vector<RE::BGSQuestObjective*> shown;
		for (auto* o : a_quest->objectives) {
			if (o && o->state.get() != State::kDormant) {
				shown.push_back(o);
			}
		}
		for (auto* o : shown) {
			if (a_text == o->displayText.c_str()) {
				return o;
			}
		}
		// <Alias=...>/<Global=...> tokens: compare the text before the first token
		RE::BGSQuestObjective* found = nullptr;
		for (auto* o : shown) {
			std::string raw = o->displayText.c_str();
			auto        cut = raw.find('<');
			if (cut != std::string::npos && cut >= 4 && a_text.compare(0, cut, raw, 0, cut) == 0) {
				if (found) {
					return nullptr;  // ambiguous
				}
				found = o;
			}
		}
		return found;
	}

	std::string TextFor(RE::TESQuest* a_quest, std::uint32_t a_instance, Rec a_rec)
	{
		auto* inst = FindInstance(a_quest, a_instance);
		if (!inst) {
			return {};
		}
		const auto   oldStage = inst->journalStage;
		const auto   oldItem = inst->journalStageItem;
		RE::BSString text;
		inst->journalStage = a_rec.stage;
		inst->journalStageItem = a_rec.item;
		a_quest->GetJournalTextForInstance(text, a_instance);
		inst->journalStage = oldStage;
		inst->journalStageItem = oldItem;
		return text.c_str();
	}

	// What the page shows right now: the selected quest and its objective entries.
	struct PageState
	{
		RE::GFxValue  page, titleList, sel, objList, entries, descText;
		RE::TESQuest* quest{ nullptr };
		std::uint32_t instance{ 0 };

		explicit PageState(const RE::GFxValue& a_page) :
			page(a_page)
		{
			page.GetMember("TitleList", &titleList);
			titleList.GetMember("selectedEntry", &sel);
			page.GetMember("ObjectiveList", &objList);
			objList.GetMember("entryList", &entries);
			page.GetMember("DescriptionText", &descText);
			if (sel.IsObject()) {
				RE::GFxValue formV, instV;
				sel.GetMember("formID", &formV);
				sel.GetMember("instance", &instV);
				const auto formID = formV.IsNumber() ? static_cast<RE::FormID>(formV.GetNumber()) : 0;
				instance = instV.IsNumber() ? static_cast<std::uint32_t>(instV.GetNumber()) : 0;
				quest = formID ? RE::TESForm::LookupByID<RE::TESQuest>(formID) : nullptr;  // null on the Miscellaneous page
			}
		}

		// the old entry recorded for the objective at this list index, empty if none
		std::string OldTextAt(std::uint32_t a_index) const
		{
			RE::GFxValue entry, entryText;
			if (!quest || !entries.IsArray() || !entries.GetElement(a_index, &entry) || !entry.GetMember("text", &entryText) || !entryText.IsString()) {
				return {};
			}
			auto* obj = MatchObjective(quest, entryText.GetString());
			if (!obj) {
				return {};
			}
			std::optional<Rec> rec;
			{
				std::scoped_lock l{ g_lock };
				if (auto it = g_recs.find({ quest->GetFormID(), instance, obj->index }); it != g_recs.end()) {
					rec = it->second;
				}
			}
			return rec ? TextFor(quest, instance, *rec) : std::string{};  // nothing recorded (e.g. given before install): keep the latest
		}

		std::string Latest() const
		{
			RE::GFxValue d;
			sel.GetMember("description", &d);
			return d.IsString() ? d.GetString() : "";
		}

		double ListY() const
		{
			RE::GFxValue y;
			objList.GetMember("_y", &y);
			return y.IsNumber() ? y.GetNumber() : 0.0;
		}

		void ShowText(const std::string& a_text)
		{
			std::array<RE::GFxValue, 1> args{ RE::GFxValue(a_text.c_str()) };
			descText.Invoke("SetText", nullptr, args.data(), args.size());
		}
	};

	// Mouse fix: the page lays the objectives out under the description, so a description of another
	// height moved the list under the cursor and hovered the next objective (a swap every frame).
	// We wrap the page's own SetDescriptionText: lay out once for the TALLEST text this quest can show,
	// then show the latest. A highlight only swaps the text, so the list never moves while hovering.
	void LayOut(const RE::GFxValue& a_page)
	{
		PageState s{ a_page };
		auto      orig = [&] { s.page.Invoke("qhOrigSetDescriptionText"); };
		const auto latest = s.Latest();
		std::vector<std::string> olds;
		for (std::uint32_t i = 0; s.quest && s.entries.IsArray() && i < s.entries.GetArraySize(); ++i) {
			if (auto t = s.OldTextAt(i); !t.empty() && t != latest && std::ranges::find(olds, t) == olds.end()) {
				olds.push_back(std::move(t));
			}
		}
		orig();
		if (olds.empty()) {
			return;  // vanilla layout
		}
		double      bestY = s.ListY();
		std::string best;
		for (const auto& t : olds) {
			s.sel.SetMember("description", RE::GFxValue(t.c_str()));
			orig();
			if (const auto y = s.ListY(); y > bestY) {
				bestY = y;
				best = t;
			}
		}
		if (!best.empty()) {
			s.sel.SetMember("description", RE::GFxValue(best.c_str()));
			orig();
		}
		s.sel.SetMember("description", RE::GFxValue(latest.c_str()));
		s.ShowText(latest);
		logger::info("layout {}: {} old entries, list y reserved {}", Edid(s.quest), olds.size(), bestY);
	}

	void OnHighlight(RE::GFxMovie* a_movie, int a_index, int a_keyboardOrMouse)
	{
		RE::GFxValue page;
		if (!a_movie->GetVariable(&page, kPage)) {
			return;
		}
		PageState s{ page };
		if (!s.quest) {
			return;
		}
		const double before = s.ListY();
		auto         text = a_index >= 0 ? s.OldTextAt(static_cast<std::uint32_t>(a_index)) : std::string{};
		s.ShowText(text.empty() ? s.Latest() : text);
		logger::info("highlight {} index={} input={} old={} listY {} -> {}", Edid(s.quest), a_index, a_keyboardOrMouse, !text.empty(), before, s.ListY());
	}

	class HighlightHandler : public RE::GFxFunctionHandler
	{
	public:
		void Call(Params& a_params) override
		{
			if (a_params.argCount < 1) {
				return;
			}
			RE::GFxValue index, input;
			a_params.args[0].GetMember("index", &index);
			a_params.args[0].GetMember("keyboardOrMouse", &input);
			OnHighlight(a_params.movie, index.IsNumber() ? static_cast<int>(index.GetNumber()) : -1,
				input.IsNumber() ? static_cast<int>(input.GetNumber()) : -1);
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

	void AttachToJournal()
	{
		auto  ui = RE::UI::GetSingleton();
		auto  menu = ui ? ui->GetMenu(RE::JournalMenu::MENU_NAME) : nullptr;
		auto* movie = menu ? menu->uiMovie.get() : nullptr;
		RE::GFxValue page, list, orig;
		if (!movie || !movie->GetVariable(&page, kPage) || !page.GetMember("ObjectiveList", &list) || !page.GetMember("SetDescriptionText", &orig)) {
			logger::info("journal: quests page not found");
			return;
		}
		static auto* highlight = new HighlightHandler();  // live for the session
		static auto* layOut = new LayOutHandler();

		RE::GFxValue scope, fn;
		movie->CreateObject(&scope);
		movie->CreateFunction(&fn, highlight);
		scope.SetMember("onSelect", fn);
		std::array<RE::GFxValue, 3> args{ RE::GFxValue("selectionChange"), scope, RE::GFxValue("onSelect") };
		const bool ok = list.Invoke("addEventListener", nullptr, args.data(), args.size());

		// shadow the class method on this page instance; the original stays reachable for LayOut
		RE::GFxValue already;
		page.GetMember("qhOrigSetDescriptionText", &already);
		if (already.IsUndefined()) {
			page.SetMember("qhOrigSetDescriptionText", orig);
			RE::GFxValue wrapper;
			movie->CreateFunction(&wrapper, layOut);
			page.SetMember("SetDescriptionText", wrapper);
		}
		page.Invoke("SetDescriptionText");  // the quest shown on open
		logger::info("journal: listener attached={}, SetDescriptionText wrapped", ok);
	}

	// Self-test (synthetic keys don't reach the journal lists): if SKSE/QuestHistorySpike.selftest exists,
	// select the quest named in it (editor ID) and walk the objective highlight 0,1,2,..,-1, 4 s per step.
	void SelfTestStep(int a_step)
	{
		auto  ui = RE::UI::GetSingleton();
		auto  menu = ui ? ui->GetMenu(RE::JournalMenu::MENU_NAME) : nullptr;
		auto* movie = menu ? menu->uiMovie.get() : nullptr;
		if (!movie) {
			return;
		}
		RE::GFxValue page, titleList, objList;
		movie->GetVariable(&page, kPage);
		page.GetMember("TitleList", &titleList);
		page.GetMember("ObjectiveList", &objList);
		if (a_step == 0) {
			auto path = logger::log_directory();
			std::ifstream f(*path / "QuestHistorySpike.selftest");
			std::string   edid;
			std::getline(f, edid);
			auto* quest = RE::TESForm::LookupByEditorID<RE::TESQuest>(edid);
			RE::GFxValue entries;
			titleList.GetMember("entryList", &entries);
			for (std::uint32_t i = 0; quest && i < entries.GetArraySize(); ++i) {
				RE::GFxValue e, id;
				entries.GetElement(i, &e);
				e.GetMember("formID", &id);
				if (id.IsNumber() && static_cast<RE::FormID>(id.GetNumber()) == quest->GetFormID()) {
					titleList.SetMember("selectedIndex", RE::GFxValue(static_cast<double>(i)));
					page.Invoke("onQuestHighlight");
					logger::info("selftest: selected {} at title index {}", edid, i);
				}
			}
			return;
		}
		RE::GFxValue entries;
		objList.GetMember("entryList", &entries);
		const int count = static_cast<int>(entries.GetArraySize());
		const int index = a_step - 1 < count ? a_step - 1 : -1;
		logger::info("selftest: objective selectedIndex = {} (of {})", index, count);
		objList.SetMember("selectedIndex", RE::GFxValue(static_cast<double>(index)));
	}

	void StartSelfTest()
	{
		auto path = logger::log_directory();
		if (!path || !std::filesystem::exists(*path / "QuestHistorySpike.selftest")) {
			return;
		}
		std::thread([] {
			for (int step = 0; step < 8; ++step) {
				std::this_thread::sleep_for(4s);
				SKSE::GetTaskInterface()->AddUITask([step] { SelfTestStep(step); });
			}
		}).detach();
	}

	class MenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		static MenuSink* Get()
		{
			static MenuSink sink;
			return &sink;
		}

		RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
		{
			if (a_event && a_event->opening && a_event->menuName == RE::JournalMenu::MENU_NAME) {
				SKSE::GetTaskInterface()->AddUITask(AttachToJournal);
				StartSelfTest();
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kDataLoaded:
			RE::ObjectiveState::GetEventSource()->AddEventSink(ObjectiveSink::Get());
			RE::UI::GetSingleton()->AddEventSink<RE::MenuOpenCloseEvent>(MenuSink::Get());
			logger::info("sinks registered");
			break;
		case SKSE::MessagingInterface::kPreLoadGame:
			g_loading = true;
			{
				std::scoped_lock l{ g_lock };
				g_recs.clear();
			}
			logger::info("--- pre load ---");
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
		case SKSE::MessagingInterface::kNewGame:
			g_loading = false;
			logger::info("--- post load / new game ---");
			break;
		default:
			break;
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	InitializeLog();
	logger::info("QuestHistorySpike on runtime {}", a_skse->RuntimeVersion().string("."));
	SKSE::Init(a_skse, false);
	SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
	return true;
}
