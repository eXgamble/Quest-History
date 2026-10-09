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

	void DumpQuest(RE::TESQuest* a_quest)
	{
		logger::info("  quest {} [{:08X}] curStage={} curInstance={} instances={}", Edid(a_quest), a_quest->GetFormID(),
			a_quest->currentStage, a_quest->currentInstanceID, a_quest->instanceData.size());
		for (auto* inst : a_quest->instanceData) {
			if (inst) {
				logger::info("    instance id={} journalStage={} journalItem={} strings={} globals={}", inst->id, inst->journalStage,
					inst->journalStageItem, inst->stringData.size(), inst->valueData.size());
			}
		}
		if (a_quest->executedStages) {
			std::string s;
			for (auto& st : *a_quest->executedStages) {
				s += fmt::format("{}(f{:X}) ", st.data.index, st.data.flags.underlying());
			}
			logger::info("    executedStages (list order): {}", s);
		}
		std::string obj;
		for (auto* o : a_quest->objectives) {
			if (o) {
				obj += fmt::format("{}:{} ", o->index, static_cast<int>(o->state.get()));
			}
		}
		logger::info("    objectives index:state: {}", obj);
		std::string log;
		for (auto* item : RE::PlayerCharacter::GetSingleton()->GetQuestLog()) {
			if (item && item->owner == a_quest) {
				log += fmt::format("{}/{} ", item->owningStage ? item->owningStage->data.index : 0xFFFF, item->index);
			}
		}
		logger::info("    questLog items stage/item (list order): {}", log);
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
			logger::info("  no instance {} on quest", a_instance);
			return {};
		}
		const auto     oldStage = inst->journalStage;
		const auto     oldItem = inst->journalStageItem;
		RE::BSString   current, swapped;
		a_quest->GetJournalTextForInstance(current, a_instance);
		inst->journalStage = a_rec.stage;
		inst->journalStageItem = a_rec.item;
		a_quest->GetJournalTextForInstance(swapped, a_instance);
		inst->journalStage = oldStage;
		inst->journalStageItem = oldItem;
		for (auto* item : RE::PlayerCharacter::GetSingleton()->GetQuestLog()) {
			if (item && item->owner == a_quest && item->owningStage && item->owningStage->data.index == a_rec.stage && item->index == static_cast<std::uint8_t>(a_rec.item)) {
				logger::info("  raw GetLogEntry: {}", item->GetLogEntry(a_quest));
			}
		}
		logger::info("  current text : {}", current.c_str());
		logger::info("  swapped text : {}", swapped.c_str());
		return swapped.c_str();
	}

	void OnHighlight(RE::GFxMovie* a_movie, int a_index, int a_keyboardOrMouse)
	{
		RE::GFxValue page, titleList, sel;
		if (!a_movie->GetVariable(&page, kPage) || !page.GetMember("TitleList", &titleList) || !titleList.GetMember("selectedEntry", &sel) || !sel.IsObject()) {
			logger::info("highlight {}: no page/TitleList/selectedEntry", a_index);
			return;
		}
		RE::GFxValue formV, instV;
		sel.GetMember("formID", &formV);
		sel.GetMember("instance", &instV);
		const auto formID = formV.IsNumber() ? static_cast<RE::FormID>(formV.GetNumber()) : 0;
		const auto instance = instV.IsNumber() ? static_cast<std::uint32_t>(instV.GetNumber()) : 0;
		auto*      quest = formID ? RE::TESForm::LookupByID<RE::TESQuest>(formID) : nullptr;
		logger::info("highlight index={} input={} quest={} [{:08X}] instance={}", a_index, a_keyboardOrMouse, Edid(quest), formID, instance);
		if (!quest) {
			return;  // Miscellaneous page (formID 0)
		}

		// the latest description, cached by us once per menu open (the swf restores from selectedEntry.description)
		RE::GFxValue latest;
		sel.GetMember("qhLatest", &latest);
		if (latest.IsUndefined()) {
			sel.GetMember("description", &latest);
			sel.SetMember("qhLatest", latest);
			DumpQuest(quest);
			sel.VisitMembers([](const char* a_name, const RE::GFxValue& a_val) {
				logger::info("    selectedEntry.{} type={}", a_name, static_cast<int>(a_val.GetType()));
			});
		}

		std::string text;
		if (a_index >= 0) {
			RE::GFxValue objList, entries, entry, entryText;
			page.GetMember("ObjectiveList", &objList);
			objList.GetMember("entryList", &entries);
			if (entries.IsArray() && entries.GetElement(static_cast<std::uint32_t>(a_index), &entry)) {
				entry.GetMember("text", &entryText);
				entry.VisitMembers([](const char* a_name, const RE::GFxValue& a_val) {
					logger::info("    objectiveEntry.{} type={}", a_name, static_cast<int>(a_val.GetType()));
				});
			}
			const std::string shownText = entryText.IsString() ? entryText.GetString() : "";
			auto*             obj = MatchObjective(quest, shownText);
			logger::info("  entry text '{}' -> objective {}", shownText, obj ? static_cast<int>(obj->index) : -1);
			if (obj) {
				std::optional<Rec> rec;
				{
					std::scoped_lock l{ g_lock };
					if (auto it = g_recs.find({ formID, instance, obj->index }); it != g_recs.end()) {
						rec = it->second;
						logger::info("  recorded: stage {} item {}", rec->stage, rec->item);
					}
				}
				if (!rec) {
					// fallback guess: the highest logged stage at or below the objective index
					for (auto* item : RE::PlayerCharacter::GetSingleton()->GetQuestLog()) {
						if (item && item->owner == quest && item->owningStage) {
							const auto s = item->owningStage->data.index;
							if (s <= obj->index && (!rec || s > rec->stage)) {
								rec = Rec{ s, static_cast<std::int8_t>(item->index) };
							}
						}
					}
					logger::info("  guessed: stage {} item {}", rec ? rec->stage : 0xFFFF, rec ? rec->item : -2);
				}
				if (rec) {
					text = TextFor(quest, instance, *rec);
				}
			}
		}

		// let the swf lay itself out: show our text through its own SetDescriptionText, then put the cache back
		if (!text.empty()) {
			sel.SetMember("description", RE::GFxValue(text.c_str()));
		}
		page.Invoke("SetDescriptionText");
		sel.SetMember("description", latest);
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

	void AttachToJournal()
	{
		auto  ui = RE::UI::GetSingleton();
		auto  menu = ui ? ui->GetMenu(RE::JournalMenu::MENU_NAME) : nullptr;
		auto* movie = menu ? menu->uiMovie.get() : nullptr;
		if (!movie) {
			logger::info("journal: no movie");
			return;
		}
		RE::GFxValue list;
		if (!movie->GetVariable(&list, (std::string(kPage) + ".objectiveList").c_str())) {
			logger::info("journal: objectiveList not found");
			return;
		}
		static auto* handler = new HighlightHandler();  // lives for the session
		RE::GFxValue scope, fn;
		movie->CreateObject(&scope);
		movie->CreateFunction(&fn, handler);
		scope.SetMember("onSelect", fn);
		std::array<RE::GFxValue, 3> args{ RE::GFxValue("selectionChange"), scope, RE::GFxValue("onSelect") };
		const bool ok = list.Invoke("addEventListener", nullptr, args.data(), args.size());
		logger::info("journal: listener attached={} questLog size={}", ok,
			std::ranges::distance(RE::PlayerCharacter::GetSingleton()->GetQuestLog()));
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
