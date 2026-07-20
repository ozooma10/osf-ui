#pragma once

// Desktop-test stand-in for CommonLibSF's event plumbing: the minimal
// sink/source pair api/PapyrusApi.cpp needs (TESLoadGameEvent teardown).
// Tests fire the event with GetEventSource()->Notify({}).

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
}
