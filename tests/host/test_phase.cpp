#include <Phase.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <exception>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

std::atomic<bool> gTrackAllocations{false};
std::atomic<size_t> gTrackedAllocations{0};

void *operator new(std::size_t size) {
	if (gTrackAllocations.load(std::memory_order_relaxed)) gTrackedAllocations.fetch_add(1, std::memory_order_relaxed);
	if (void *memory = std::malloc(size)) return memory;
	std::terminate();
}

void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }

namespace {
void require(bool condition, const char *message) {
	if (!condition) {
		std::cerr << "FAIL: " << message << '\n';
		std::exit(1);
	}
}

bool waitForState(Phase &phase, PhaseState expected, uint32_t timeoutMs = 1000) {
	const uint32_t started = millis();
	while (millis() - started < timeoutMs) {
		if (phase.state() == expected) return true;
		std::this_thread::sleep_for(1ms);
	}
	return phase.state() == expected;
}

void testDependencyAndShutdownOrder() {
	Phase phase;
	require(static_cast<bool>(phase.init()), "init should succeed");
	std::mutex mutex;
	std::vector<std::string> calls;
	auto addCall = [&](const char *value) {
		std::lock_guard<std::mutex> lock(mutex);
		calls.emplace_back(value);
	};

	require(static_cast<bool>(phase.add("storage", [&] { addCall("init-storage"); }, [&] { addCall("deinit-storage"); })
	                              .start([&] { addCall("start-storage"); }, [&] { addCall("stop-storage"); })),
	        "storage registration should succeed");
	require(static_cast<bool>(phase.add("network", [&] { addCall("init-network"); }, [&] { addCall("deinit-network"); })
	                              .depends("storage")
	                              .start([&] { addCall("start-network"); }, [&] { addCall("stop-network"); })),
	        "network registration should succeed");
	require(static_cast<bool>(phase.start()), "start should succeed");
	require(waitForState(phase, PhaseState::Ready), "phase should become ready");
	require(static_cast<bool>(phase.stop()), "stop should succeed");
	require(waitForState(phase, PhaseState::Stopped), "phase should stop");

	const std::vector<std::string> expected = {
	    "init-storage", "init-network", "start-storage", "start-network",
	    "stop-network", "stop-storage", "deinit-network", "deinit-storage"
	};
	{
		std::lock_guard<std::mutex> lock(mutex);
		require(calls == expected, "lifecycle ordering should be dependency/reverse dependency ordered");
	}
	require(static_cast<bool>(phase.end()), "end should succeed");
	require(phase.getDiagnostics().stackHighWaterMarkBytes == 123, "stack high-water mark should remain byte-valued");
}

void testFailureRollback() {
	Phase phase;
	require(static_cast<bool>(phase.init()), "init should succeed");
	std::mutex mutex;
	std::vector<std::string> calls;
	auto addCall = [&](const char *value) {
		std::lock_guard<std::mutex> lock(mutex);
		calls.emplace_back(value);
	};
	phase.add("one", [&] { addCall("init-one"); }, [&] { addCall("deinit-one"); });
	phase.add("two", [&]() -> PhaseResult {
		addCall("init-two");
		return PhaseResult::failure(PhaseStatus::CallbackFailed, "boom");
	}, [&] { addCall("deinit-two"); }).depends("one");
	require(static_cast<bool>(phase.start()), "start request should succeed");
	require(waitForState(phase, PhaseState::Failed), "phase should fail");
	{
		std::lock_guard<std::mutex> lock(mutex);
		const std::vector<std::string> expected = {"init-one", "init-two", "deinit-one"};
		require(calls == expected, "rollback should deinitialize only initialized steps");
	}
	require(phase.getDiagnostics().rollbackCount == 1, "rollback should be counted");
	require(static_cast<bool>(phase.end()), "end should succeed after failure");
}

void testOptionalAndGroupPause() {
	Phase phase;
	require(static_cast<bool>(phase.init()), "init should succeed");
	phase.add("optional", [] { return false; }).optional();
	phase.add("dependent", [] {}).depends("optional").optional();
	std::atomic<bool> gate{false};
	phase.addGroup("gate").condition([&] { return gate.load(); }, 500).conditionPollInterval(2);
	require(static_cast<bool>(phase.pause("maintenance")), "pause should succeed");
	require(static_cast<bool>(phase.start()), "start should succeed");
	require(waitForState(phase, PhaseState::Paused), "phase should pause before lifecycle work");
	gate = true;
	require(static_cast<bool>(phase.resume()), "resume should succeed");
	require(waitForState(phase, PhaseState::Ready), "optional failures should not block ready");
	PhaseDiag diag = phase.getDiagnostics();
	require(diag.failedCount == 1, "optional failure should be recorded");
	require(diag.skippedCount == 1, "optional dependent should be skipped");
	require(static_cast<bool>(phase.end()), "end should succeed");
}

void testCallbackSafety() {
	Phase phase;
	require(static_cast<bool>(phase.init()), "init should succeed");
	phase.add("app", [] {});
	std::atomic<bool> pausedCallbackEntered{false};
	std::atomic<bool> pauseReasonStable{false};
	std::atomic<bool> endRejected{false};
	phase.onChange([&](PhaseChange change) {
		if (change.state == PhaseState::Paused && change.pauseReason != nullptr) {
			const std::string before = change.pauseReason;
			pausedCallbackEntered = true;
			std::this_thread::sleep_for(20ms);
			pauseReasonStable = before == change.pauseReason;
		}
	});
	phase.onReady([&] {
		PhaseResult result = phase.end(20);
		endRejected = !result && result.status == PhaseStatus::Busy;
	});
	require(static_cast<bool>(phase.pause("snapshot-reason")), "pause should succeed");
	require(static_cast<bool>(phase.start()), "start should succeed");
	const uint32_t started = millis();
	while (!pausedCallbackEntered && millis() - started < 500) std::this_thread::sleep_for(1ms);
	require(pausedCallbackEntered, "paused callback should run");
	require(static_cast<bool>(phase.resume()), "resume should succeed concurrently with callback");
	require(waitForState(phase, PhaseState::Ready), "phase should become ready");
	require(pauseReasonStable, "pause reason pointer should stay valid during callback");
	require(endRejected, "end should be rejected from the Phase task");
	require(static_cast<bool>(phase.end()), "external end should succeed");
}

void testLifecycleDoesNotAllocate() {
	Phase phase;
	require(static_cast<bool>(phase.init()), "init should succeed");
	phase.add("root", [] {}).start([] {}, [] {});
	phase.add("dependent", [] {}).depends("root").start([] {}, [] {});
	gTrackedAllocations = 0;
	gTrackAllocations = true;
	require(static_cast<bool>(phase.start()), "start should succeed");
	require(waitForState(phase, PhaseState::Ready), "phase should become ready");
	require(static_cast<bool>(phase.stop()), "stop should succeed");
	require(waitForState(phase, PhaseState::Stopped), "phase should stop");
	gTrackAllocations = false;
	require(gTrackedAllocations == 0, "lifecycle execution should not allocate");
	require(static_cast<bool>(phase.end()), "end should succeed");
}

void testPendingStartCancellationAndRestart() {
	Phase phase;
	require(static_cast<bool>(phase.init()), "init should succeed");
	std::atomic<int> initCount{0};
	phase.add("app", [&] { initCount++; });
	require(static_cast<bool>(phase.start()), "start should succeed");
	PhaseResult stopped = phase.stop();
	require(static_cast<bool>(stopped), "immediate stop should succeed");
	std::this_thread::sleep_for(20ms);
	if (phase.state() == PhaseState::Stopped) {
		require(static_cast<bool>(phase.start()), "restart should succeed");
		require(waitForState(phase, PhaseState::Ready), "restart should become ready");
	} else {
		require(phase.state() == PhaseState::Idle, "cancelled start should remain idle");
		require(initCount == 0, "cancelled pending start should not initialize");
		require(static_cast<bool>(phase.start()), "start after cancellation should succeed");
		require(waitForState(phase, PhaseState::Ready), "start after cancellation should become ready");
	}
	require(static_cast<bool>(phase.stop()), "stop should succeed");
	require(waitForState(phase, PhaseState::Stopped), "phase should stop");
	require(static_cast<bool>(phase.start()), "restart from stopped should succeed");
	require(waitForState(phase, PhaseState::Ready), "restart from stopped should become ready");
	require(static_cast<bool>(phase.end()), "end should succeed");
}
}

int main() {
	testDependencyAndShutdownOrder();
	testFailureRollback();
	testOptionalAndGroupPause();
	testCallbackSafety();
	testLifecycleDoesNotAllocate();
	testPendingStartCancellationAndRestart();
	std::cout << "Phase host tests passed\n";
	return 0;
}
