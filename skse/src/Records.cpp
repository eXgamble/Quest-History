#include "Records.h"

namespace
{
	using State = RE::QUEST_OBJECTIVE_STATE;
	using Key = std::tuple<RE::FormID, std::uint32_t, std::uint16_t>;  // quest, instance, objective index

	constexpr std::uint32_t SERIALIZATION_ID = 'QHST';
	constexpr std::uint32_t RECORDS_TYPE = 'RECS';
	constexpr std::uint32_t RECORDS_VERSION = 1;

	std::mutex                    lock;  // objective events come from Papyrus threads, reads from the UI
	std::map<Key, Records::Record> records;
	std::uint32_t                 nextOrder = 1;
	std::atomic<bool>             loading{ false };

	RE::BGSQuestInstanceText* FindInstance(RE::TESQuest* a_quest, std::uint32_t a_id)
	{
		for (auto* inst : a_quest->instanceData) {
			if (inst && inst->id == a_id) {
				return inst;
			}
		}
		return nullptr;
	}

	// An objective appears: remember the journal entry the quest shows right now. The stage that
	// shows an objective has already put its log entry in the journal when its fragment runs.
	class ObjectiveSink : public RE::BSTEventSink<RE::ObjectiveState::Event>
	{
	public:
		static ObjectiveSink* GetSingleton()
		{
			static ObjectiveSink singleton;
			return &singleton;
		}

		RE::BSEventNotifyControl ProcessEvent(const RE::ObjectiveState::Event* a_event, RE::BSTEventSource<RE::ObjectiveState::Event>*) override
		{
			if (loading || !a_event || !a_event->objective || a_event->oldState != State::kDormant || a_event->newState != State::kDisplayed) {
				return RE::BSEventNotifyControl::kContinue;
			}
			const auto objective = a_event->objective;
			const auto quest = objective->ownerQuest;
			const auto instance = quest ? FindInstance(quest, quest->currentInstanceID) : nullptr;
			if (instance) {
				std::scoped_lock l{ lock };
				records.try_emplace({ quest->GetFormID(), instance->id, objective->index },
					Records::Record{ instance->journalStage, instance->journalStageItem, nextOrder++ });
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	// A quest (re)starts: its objectives begin again, so its old records no longer apply
	// (repeatable and radiant quests)
	class QuestStartSink : public RE::BSTEventSink<RE::TESQuestStartStopEvent>
	{
	public:
		static QuestStartSink* GetSingleton()
		{
			static QuestStartSink singleton;
			return &singleton;
		}

		RE::BSEventNotifyControl ProcessEvent(const RE::TESQuestStartStopEvent* a_event, RE::BSTEventSource<RE::TESQuestStartStopEvent>*) override
		{
			if (loading || !a_event || !a_event->started) {
				return RE::BSEventNotifyControl::kContinue;
			}
			std::scoped_lock l{ lock };
			std::erase_if(records, [&](const auto& a_entry) { return std::get<0>(a_entry.first) == a_event->formID; });
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	void OnSave(SKSE::SerializationInterface* a_intfc)
	{
		std::scoped_lock l{ lock };
		if (!a_intfc->OpenRecord(RECORDS_TYPE, RECORDS_VERSION)) {
			logger::error("Couldn't open the co-save record");
			return;
		}
		const auto count = static_cast<std::uint32_t>(records.size());
		a_intfc->WriteRecordData(nextOrder);
		a_intfc->WriteRecordData(count);
		for (const auto& [key, record] : records) {
			a_intfc->WriteRecordData(std::get<0>(key));
			a_intfc->WriteRecordData(std::get<1>(key));
			a_intfc->WriteRecordData(std::get<2>(key));
			a_intfc->WriteRecordData(record.stage);
			a_intfc->WriteRecordData(record.item);
			a_intfc->WriteRecordData(record.order);
		}
	}

	void OnLoad(SKSE::SerializationInterface* a_intfc)
	{
		std::scoped_lock l{ lock };
		records.clear();
		nextOrder = 1;
		std::uint32_t type, version, length;
		while (a_intfc->GetNextRecordInfo(type, version, length)) {
			if (type != RECORDS_TYPE || version != RECORDS_VERSION) {
				continue;
			}
			std::uint32_t count = 0;
			a_intfc->ReadRecordData(nextOrder);
			a_intfc->ReadRecordData(count);
			std::uint32_t dropped = 0;
			for (std::uint32_t i = 0; i < count; ++i) {
				RE::FormID      quest;
				std::uint32_t   instance;
				std::uint16_t   objective;
				Records::Record record;
				a_intfc->ReadRecordData(quest);
				a_intfc->ReadRecordData(instance);
				a_intfc->ReadRecordData(objective);
				a_intfc->ReadRecordData(record.stage);
				a_intfc->ReadRecordData(record.item);
				a_intfc->ReadRecordData(record.order);
				RE::FormID resolved;
				if (a_intfc->ResolveFormID(quest, resolved)) {  // the quest's plugin may have moved or gone
					records[{ resolved, instance, objective }] = record;
				} else {
					++dropped;
				}
			}
			logger::info("Loaded {} records ({} dropped: their plugin is gone)", records.size(), dropped);
		}
	}

	void OnRevert(SKSE::SerializationInterface*)
	{
		std::scoped_lock l{ lock };
		records.clear();
		nextOrder = 1;
	}
}

namespace Records
{
	void RegisterSinks()
	{
		RE::ObjectiveState::GetEventSource()->AddEventSink(ObjectiveSink::GetSingleton());
		RE::ScriptEventSourceHolder::GetSingleton()->AddEventSink<RE::TESQuestStartStopEvent>(QuestStartSink::GetSingleton());
	}

	void RegisterSerialization()
	{
		const auto serialization = SKSE::GetSerializationInterface();
		serialization->SetUniqueID(SERIALIZATION_ID);
		serialization->SetSaveCallback(OnSave);
		serialization->SetLoadCallback(OnLoad);
		serialization->SetRevertCallback(OnRevert);
	}

	void SetLoading(bool a_loading)
	{
		loading = a_loading;
	}

	std::optional<Record> Find(RE::FormID a_quest, std::uint32_t a_instance, std::uint16_t a_objective)
	{
		std::scoped_lock l{ lock };
		const auto it = records.find({ a_quest, a_instance, a_objective });
		return it != records.end() ? std::optional{ it->second } : std::nullopt;
	}
}
