// Quest History: SKSE plugin.
// In the Quest Journal, highlighting an objective shows the quest description (journal entry) it was
// given with; without a highlight the journal shows the latest one, as vanilla.

#include "Journal.h"
#include "Records.h"

namespace
{
	// Own the log before SKSE::Init, and pass Init false so it doesn't truncate/replace it.
	void InitializeLog()
	{
		auto path = logger::log_directory();
		if (!path) {
			SKSE::stl::report_and_fail("Failed to find the SKSE log directory"sv);
		}
		*path /= "QuestHistory.log"sv;

		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
		auto log = std::make_shared<spdlog::logger>("global log"s, std::move(sink));
		log->set_level(spdlog::level::info);
		log->flush_on(spdlog::level::info);
		spdlog::set_default_logger(std::move(log));
		spdlog::set_pattern("[%H:%M:%S:%e] [%l] %v"s);
	}

	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kDataLoaded:
			Records::RegisterSinks();
			Journal::Register();
			break;
		case SKSE::MessagingInterface::kPreLoadGame:
			Records::SetLoading(true);
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
		case SKSE::MessagingInterface::kNewGame:
			Records::SetLoading(false);
			break;
		default:
			break;
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	InitializeLog();
	logger::info("QuestHistory loading on game runtime {}", a_skse->RuntimeVersion().string("."));

	SKSE::Init(a_skse, false);
	SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
	Records::RegisterSerialization();

	logger::info("QuestHistory loaded");
	return true;
}
