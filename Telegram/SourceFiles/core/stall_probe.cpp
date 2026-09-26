/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/stall_probe.h"

#include "base/debug_log.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>

namespace Core::Probe {
namespace details {

bool Enabled/* = false*/;
Qt::HANDLE MainThread/* = nullptr*/;

} // namespace details
namespace {

// Main thread only, so none of this needs locking.
constexpr auto kMaxDepth = 24;
constexpr auto kMaxNames = 256;
constexpr auto kMaxOnly = 8;

struct Entry;

struct Frame {
	Entry *entry = nullptr;
	const char *name = nullptr;
	const char *detail = nullptr;
	crl::profile_time started = 0;
	crl::profile_time children = 0;
};

struct Entry {
	const char *name = nullptr;
	int64 calls = 0;
	crl::profile_time inclusive = 0;
	crl::profile_time self = 0;
	crl::profile_time worst = 0;
	bool wanted = false;
};

Frame Stack[kMaxDepth];
Entry Names[kMaxNames];
int Depth/* = 0*/;
int Overflowed/* = 0*/;
int NameCount/* = 0*/;
int64 Stalls/* = 0*/;

bool EventsWanted/* = false*/;
bool ClassKey/* = false*/;
crl::profile_time StallLimit/* = 0*/; // microseconds
crl::time DumpEvery/* = 0*/; // milliseconds, 0 disables
crl::time DumpStarted/* = 0*/;

// Owned copies, so the needles outlive the environment block.
std::array<QByteArray, kMaxOnly> Only;
int OnlyCount/* = 0*/;

[[nodiscard]] bool EnvSet(const char *name) {
	const auto value = std::getenv(name);
	return value && *value && std::strcmp(value, "0");
}

[[nodiscard]] int EnvInt(const char *name, int fallback) {
	const auto value = std::getenv(name);
	if (!value || !*value) {
		return fallback;
	}
	const auto parsed = std::atoi(value);
	return (parsed >= 0) ? parsed : fallback;
}

[[nodiscard]] QString Ms(crl::profile_time us) {
	return QString::number(us / 1000., 'f', 1);
}

// Decided once per unique name, so the scan never repeats.
[[nodiscard]] bool Wanted(const char *name) {
	if (!OnlyCount) {
		return true;
	}
	for (auto i = 0; i != OnlyCount; ++i) {
		if (std::strstr(name, Only[i].constData())) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] Entry *Lookup(const char *name) {
	for (auto i = 0; i != NameCount; ++i) {
		if (Names[i].name == name) {
			return &Names[i];
		}
	}
	if (NameCount == kMaxNames) {
		return nullptr;
	}
	auto &entry = Names[NameCount++];
	entry.name = name;
	entry.wanted = Wanted(name);
	return &entry;
}

void ReportStall(const Frame &frame, crl::profile_time spent) {
	auto path = QString::fromUtf8(frame.name);
	if (frame.detail) {
		path += u" ["_q + QString::fromUtf8(frame.detail) + u"]"_q;
	}
	for (auto i = Depth - 1; i >= 0; --i) {
		path += u" < "_q + QString::fromUtf8(Stack[i].name);
	}
	++Stalls;
	LOG(("Probe stall: %1ms (self %2ms) %3")
		.arg(Ms(spent))
		.arg(Ms(spent - frame.children))
		.arg(path));
}

void Dump(crl::time now) {
	const auto window = now - DumpStarted;
	DumpStarted = now;

	auto order = std::array<int, kMaxNames>();
	auto count = 0;
	for (auto i = 0; i != NameCount; ++i) {
		if (Names[i].calls) {
			order[count++] = i;
		}
	}
	std::sort(order.begin(), order.begin() + count, [](int a, int b) {
		return Names[a].self > Names[b].self;
	});

	auto text = u"Probe dump over %1ms, %2 stalls over %3ms"_q
		.arg(window)
		.arg(Stalls)
		.arg(Ms(StallLimit));
	if (Overflowed) {
		text += u", %1 scopes past depth %2"_q
			.arg(Overflowed)
			.arg(kMaxDepth);
	}
	if (NameCount == kMaxNames) {
		text += u", name table full"_q;
	}
	for (auto i = 0; i != count; ++i) {
		const auto &entry = Names[order[i]];
		text += u"\n  %1 calls %2  incl %3ms  self %4ms  worst %5ms"_q
			.arg(QString::fromUtf8(entry.name), -52)
			.arg(entry.calls, 6)
			.arg(Ms(entry.inclusive), 9)
			.arg(Ms(entry.self), 9)
			.arg(Ms(entry.worst), 8);
	}
	LOG(("%1").arg(text));

	for (auto i = 0; i != NameCount; ++i) {
		auto &entry = Names[i];
		entry.calls = 0;
		entry.inclusive = 0;
		entry.self = 0;
		entry.worst = 0;
	}
	Stalls = 0;
	Overflowed = 0;
}

} // namespace

void Init() {
	Expects(!details::Enabled);

	if (!EnvSet("AYU_PROBE")) {
		return;
	}
	StallLimit = crl::profile_time(EnvInt("AYU_PROBE_MS", 8)) * 1000;
	DumpEvery = crl::time(EnvInt("AYU_PROBE_DUMP", 10)) * 1000;
	EventsWanted = EnvSet("AYU_PROBE_EVENTS");

	const auto key = std::getenv("AYU_PROBE_KEY");
	ClassKey = !key || !*key || !std::strcmp(key, "class");

	if (const auto only = std::getenv("AYU_PROBE_ONLY")) {
		for (const auto &part : QByteArray(only).split(',')) {
			const auto trimmed = part.trimmed();
			if (!trimmed.isEmpty() && OnlyCount != kMaxOnly) {
				Only[OnlyCount++] = trimmed;
			}
		}
	}

	DumpStarted = crl::now();
	details::MainThread = QThread::currentThreadId();
	details::Enabled = true;

	auto only = QString(u"all"_q);
	if (OnlyCount) {
		only = QString();
		for (auto i = 0; i != OnlyCount; ++i) {
			only += (i ? u","_q : QString()) + QString::fromUtf8(Only[i]);
		}
	}
	LOG(("Probe: on, stalls over %1ms, dump every %2ms, events %3, "
		"key %4, only %5.")
		.arg(Ms(StallLimit))
		.arg(DumpEvery)
		.arg(EventsWanted ? "on" : "off")
		.arg(ClassKey ? "class" : "type")
		.arg(only));
}

bool Events() {
	return EventsWanted;
}

bool KeyByClass() {
	return ClassKey;
}

void Enter(const char *name) {
	if (Depth >= kMaxDepth) {
		if (Depth == kMaxDepth) {
			++Overflowed;
		}
		++Depth;
		return;
	}
	auto &frame = Stack[Depth++];
	frame.entry = Lookup(name);
	frame.name = name;
	frame.detail = nullptr;
	frame.children = 0;
	frame.started = crl::profile();
}

void Leave() {
	if (Depth > kMaxDepth) {
		--Depth;
		return;
	}
	const auto &frame = Stack[--Depth];
	const auto spent = crl::profile() - frame.started;
	const auto counted = !frame.entry || frame.entry->wanted;

	if (frame.entry && frame.entry->wanted) {
		auto &entry = *frame.entry;
		++entry.calls;
		entry.inclusive += spent;
		entry.self += spent - frame.children;
		entry.worst = std::max(entry.worst, spent);
	}
	if (counted && spent >= StallLimit) {
		ReportStall(frame, spent);
	}
	if (Depth > 0) {
		Stack[Depth - 1].children += spent;
	} else if (DumpEvery > 0) {
		const auto now = crl::now();
		if (now - DumpStarted >= DumpEvery) {
			Dump(now);
		}
	}
}

void Detail(const char *text) {
	if (Depth > 0 && Depth <= kMaxDepth) {
		Stack[Depth - 1].detail = text;
	}
}

} // namespace Core::Probe
