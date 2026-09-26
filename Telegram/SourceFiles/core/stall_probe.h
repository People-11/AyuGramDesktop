/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QThread>

// Main thread stall attribution, configured entirely through the environment
// so one build can answer several questions. Off unless AYU_PROBE is set.
//
//   AYU_PROBE=1        enable. Unset or 0 disables every hook below.
//   AYU_PROBE_MS=8     log a scope as a stall once it takes this long.
//   AYU_PROBE_DUMP=10  seconds between per-scope tables. 0 disables them.
//   AYU_PROBE_EVENTS=1 time every event Sandbox::notify() delivers. This needs
//                      no instrumentation at the call site, so it attributes
//                      blocking anywhere in the app.
//   AYU_PROBE_KEY=class|type
//                      what AYU_PROBE_EVENTS groups by: the receiver's class
//                      (default) or the event type. The other one is printed
//                      on stall lines, so two runs give both views.
//   AYU_PROBE_ONLY=a,b only account scopes whose name contains one of these.
//                      Narrows a noisy run without touching the threshold.
//
// Scopes nest. Tables report inclusive and self time, and self time names the
// step that actually blocks: a large inclusive with a small self time means the
// scope is only waiting for something it called.
//
// Note that event/UpdateRequest covers the backing store flush, so its time is
// presentation as well as work.

namespace Core::Probe {
namespace details {

extern bool Enabled;
extern Qt::HANDLE MainThread;

} // namespace details

// Call once from the main thread, after the application object exists.
void Init();

[[nodiscard]] inline bool Active() {
	return details::Enabled
		&& (QThread::currentThreadId() == details::MainThread);
}

// AYU_PROBE_EVENTS, AYU_PROBE_KEY.
[[nodiscard]] bool Events();
[[nodiscard]] bool KeyByClass();

void Enter(const char *name);
void Leave();

// Extra text for stall lines only; never becomes a table key. Applies to the
// innermost open scope, so call it right after the scope is created.
void Detail(const char *text);

class Scope final {
public:
	explicit Scope(const char *name) : _on(Active()) {
		if (_on) {
			Enter(name);
		}
	}
	Scope(const Scope &other) = delete;
	Scope &operator=(const Scope &other) = delete;
	~Scope() {
		if (_on) {
			Leave();
		}
	}

private:
	const bool _on = false;

};

} // namespace Core::Probe

// Names must have static storage: the table keys on the pointer, not the text.
// String literals and typeid(x).name() both qualify.
#define PROBE_SCOPE(name) \
	const Core::Probe::Scope _probeScope(name)
