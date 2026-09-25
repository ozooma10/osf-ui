#pragma once


#include <cstdint>
#include <vector>

namespace RE
{
	enum class BSEventNotifyControl
	{
		kContinue,
		kStop,
	};

	template <class T>
	class BSTEventSource;

	template <class T>
	class BSTEventSink
	{
	public:
		virtual ~BSTEventSink() = default;
		virtual BSEventNotifyControl ProcessEvent(const T& a_event, BSTEventSource<T>* a_source) = 0;
	};

	template <class T>
	class BSTEventSource
	{
	public:
		void RegisterSink(BSTEventSink<T>* a_sink) { sinks.push_back(a_sink); }

		void Notify(const T& a_event)
		{
			for (auto* sink : sinks) {
				sink->ProcessEvent(a_event, this);
			}
		}

		std::vector<BSTEventSink<T>*> sinks;
	};

	struct TESLoadGameEvent
	{
		static BSTEventSource<TESLoadGameEvent>* GetEventSource()
		{
			static BSTEventSource<TESLoadGameEvent> source;
			return &source;
		}
	};

	struct SaveLoadEvent
	{
		enum class OpType : std::uint8_t
		{
			kAutosave = 1,
			kLoadMostRecent = 2,
			kQuicksave = 3,
			kQuickload = 4,
			kManualSave = 5,
			kLoad = 6,
			kExitSaveToMainMenu = 7,
			kExitSaveToDesktop = 8,
			kLoadNamedFile = 0xB,
		};

		enum class Status : std::uint8_t
		{
			kBegin = 0,
			kLoadSucceeded = 1,
			kFailed = 3,
			kSaveCompleted = 4,
			kLoadDispatchRefused = 5,
		};

		static BSTEventSource<SaveLoadEvent>* GetEventSource()
		{
			static BSTEventSource<SaveLoadEvent> source;
			return &source;
		}

		OpType opType{ OpType::kLoad };
		Status status{ Status::kBegin };
	};
}
