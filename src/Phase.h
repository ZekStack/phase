#pragma once

#include <Arduino.h>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <type_traits>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

struct PhaseImpl;
class Phase;
class PhaseStepBuilder;
class PhaseGroupBuilder;

enum class PhaseStatus : uint8_t {
	Ok,
	NotInitialized,
	AlreadyInitialized,
	InvalidArgument,
	OutOfMemory,
	TaskCreateFailed,
	TooManyNodes,
	TooManyDependencies,
	DuplicateName,
	MissingDependency,
	CircularDependency,
	InvalidCallback,
	RegistrationClosed,
	Busy,
	Timeout,
	CallbackFailed,
	DependencyFailed,
	InternalError,
};

enum class PhaseState : uint8_t {
	Idle,
	Booting,
	Starting,
	Ready,
	Paused,
	Stopping,
	Deinitializing,
	Stopped,
	Failed,
	Ended,
};

enum class PhaseNodeType : uint8_t {
	Step,
	Group,
	None,
};

enum class PhaseStackType : uint8_t {
	Auto,
	Internal,
	Psram,
};

struct PhaseResult {
	bool result = false;
	PhaseStatus status = PhaseStatus::InternalError;
	const char *message = "internal error";

	explicit operator bool() const {
		return result;
	}

	static PhaseResult success(const char *message = "ok");
	static PhaseResult failure(PhaseStatus status, const char *message);
};

struct PhaseConfig {
	uint32_t stackSizeBytes = 4096;
	UBaseType_t priority = 1;
	BaseType_t coreId = tskNO_AFFINITY;
	const char *taskName = "phase-task";
	PhaseStackType stackType = PhaseStackType::Auto;
	size_t maxNodes = 32;
	size_t maxDependenciesPerNode = 8;
	uint32_t defaultInitTimeoutMs = 30000;
	uint32_t defaultStartTimeoutMs = 30000;
	uint32_t defaultStopTimeoutMs = 30000;
	uint32_t defaultDeinitTimeoutMs = 30000;
	uint32_t defaultGroupTimeoutMs = 30000;
	uint32_t conditionPollIntervalMs = 100;
};

struct PhaseChange {
	PhaseState state = PhaseState::Idle;
	PhaseNodeType nodeType = PhaseNodeType::None;
	const char *nodeName = nullptr;
	const char *pauseReason = nullptr;
	const char *message = "ok";
	bool isBooting = false;
	bool isStarting = false;
	bool isPaused = false;
	bool isStopping = false;
	bool isDeinitializing = false;
	bool isDone = false;
	bool hasError = false;
	PhaseResult result = PhaseResult::success();
	uint32_t durationMs = 0;
};

struct PhaseDiag {
	size_t nodeCount = 0;
	size_t initializedCount = 0;
	size_t startedCount = 0;
	size_t readyCount = 0;
	size_t failedCount = 0;
	size_t skippedCount = 0;
	uint32_t bootCount = 0;
	uint32_t rollbackCount = 0;
	uint32_t changeCount = 0;
	size_t stackHighWaterMarkBytes = 0;
	PhaseState state = PhaseState::Idle;
	PhaseStackType requestedStackType = PhaseStackType::Auto;
	PhaseStackType actualStackType = PhaseStackType::Internal;
};

using PhaseCallback = std::function<PhaseResult()>;
using PhaseConditionCallback = std::function<bool()>;
using PhaseChangeCallback = std::function<void(PhaseChange)>;
using PhaseReadyCallback = std::function<void()>;
using PhaseFailedCallback = std::function<void(PhaseResult)>;

namespace phase_detail {
template <typename Callable>
PhaseCallback makeLifecycleCallback(Callable callback) {
	return [callback]() mutable -> PhaseResult {
		if constexpr (std::is_void_v<std::invoke_result_t<Callable>>) {
			callback();
			return PhaseResult::success();
		} else if constexpr (std::is_same_v<
		                         std::remove_cv_t<std::remove_reference_t<std::invoke_result_t<Callable>>>,
		                         PhaseResult>) {
			return callback();
		} else if constexpr (std::is_same_v<
		                         std::remove_cv_t<std::remove_reference_t<std::invoke_result_t<Callable>>>,
		                         bool>) {
			return callback() ? PhaseResult::success()
			                  : PhaseResult::failure(
			                        PhaseStatus::CallbackFailed,
			                        "callback returned false"
			                    );
		} else {
			static_assert(
			    std::is_void_v<std::invoke_result_t<Callable>>,
			    "Phase callbacks must return void, bool, or PhaseResult"
			);
		}
	};
}
} // namespace phase_detail

class PhaseStepBuilder {
  public:
	PhaseStepBuilder() = default;

	explicit operator bool() const {
		return _result.result;
	}

	PhaseResult result() const {
		return _result;
	}

	PhaseStepBuilder &depends(const char *name);
	PhaseStepBuilder &depends(std::initializer_list<const char *> names);
	PhaseStepBuilder &optional();
	PhaseStepBuilder &initTimeout(uint32_t timeoutMs);
	PhaseStepBuilder &startTimeout(uint32_t timeoutMs);
	PhaseStepBuilder &stopTimeout(uint32_t timeoutMs);
	PhaseStepBuilder &deinitTimeout(uint32_t timeoutMs);

	template <typename Start>
	PhaseStepBuilder &start(Start startCallback) {
		return start(startCallback, nullptr);
	}

	template <typename Start, typename Stop>
	PhaseStepBuilder &start(Start startCallback, Stop stopCallback) {
		if (!_result) {
			return *this;
		}
		PhaseCallback startFn = phase_detail::makeLifecycleCallback(startCallback);
		PhaseCallback stopFn;
		if constexpr (!std::is_same_v<std::nullptr_t, std::remove_cv_t<Stop>>) {
			stopFn = phase_detail::makeLifecycleCallback(stopCallback);
		}
		_result = setStartCallbacks(startFn, stopFn);
		return *this;
	}

  private:
	friend class Phase;

	PhaseStepBuilder(Phase *phase, size_t index, PhaseResult result);

	PhaseResult setStartCallbacks(PhaseCallback startCallback, PhaseCallback stopCallback);

	Phase *_phase = nullptr;
	size_t _index = 0;
	PhaseResult _result = PhaseResult::failure(PhaseStatus::InternalError, "invalid builder");
};

class PhaseGroupBuilder {
  public:
	PhaseGroupBuilder() = default;

	explicit operator bool() const {
		return _result.result;
	}

	PhaseResult result() const {
		return _result;
	}

	PhaseGroupBuilder &depends(const char *name);
	PhaseGroupBuilder &depends(std::initializer_list<const char *> names);
	PhaseGroupBuilder &optional();
	PhaseGroupBuilder &condition(PhaseConditionCallback callback);
	PhaseGroupBuilder &condition(PhaseConditionCallback callback, uint32_t timeoutMs);
	PhaseGroupBuilder &conditionPollInterval(uint32_t intervalMs);

  private:
	friend class Phase;

	PhaseGroupBuilder(Phase *phase, size_t index, PhaseResult result);

	Phase *_phase = nullptr;
	size_t _index = 0;
	PhaseResult _result = PhaseResult::failure(PhaseStatus::InternalError, "invalid builder");
};

class Phase {
  public:
	Phase();
	~Phase();

	Phase(const Phase &) = delete;
	Phase &operator=(const Phase &) = delete;

	PhaseResult init(const PhaseConfig &config = PhaseConfig());
	PhaseResult start();
	PhaseResult stop();
	PhaseResult end(uint32_t timeoutMs = 5000);
	PhaseResult pause(const char *reason = nullptr);
	PhaseResult resume();

	bool isPaused();
	PhaseState state();
	PhaseDiag getDiagnostics();

	void onChange(PhaseChangeCallback callback);
	void onReady(PhaseReadyCallback callback);
	void onFailed(PhaseFailedCallback callback);

	template <typename Init>
	PhaseStepBuilder add(const char *name, Init initCallback) {
		return add(name, initCallback, nullptr);
	}

	template <typename Init, typename Deinit>
	PhaseStepBuilder add(const char *name, Init initCallback, Deinit deinitCallback) {
		PhaseCallback initFn = phase_detail::makeLifecycleCallback(initCallback);
		PhaseCallback deinitFn;
		if constexpr (!std::is_same_v<std::nullptr_t, std::remove_cv_t<Deinit>>) {
			deinitFn = phase_detail::makeLifecycleCallback(deinitCallback);
		}
		return addStep(name, initFn, deinitFn);
	}

	PhaseGroupBuilder addGroup(const char *name);

	const char *statusToString(PhaseStatus status) const;
	const char *stateToString(PhaseState state) const;
	const char *nodeTypeToString(PhaseNodeType type) const;

  private:
	friend class PhaseStepBuilder;
	friend class PhaseGroupBuilder;

	PhaseStepBuilder addStep(const char *name, PhaseCallback initCallback, PhaseCallback deinitCallback);
	PhaseResult addDependency(size_t index, const char *name);
	PhaseResult setOptional(size_t index);
	PhaseResult setStepStartCallbacks(
	    size_t index,
	    PhaseCallback startCallback,
	    PhaseCallback stopCallback
	);
	PhaseResult setStepTimeout(size_t index, uint8_t timeoutKind, uint32_t timeoutMs);
	PhaseResult setGroupCondition(
	    size_t index,
	    PhaseConditionCallback callback,
	    uint32_t timeoutMs,
	    bool hasTimeoutOverride
	);
	PhaseResult setGroupPollInterval(size_t index, uint32_t intervalMs);

	std::unique_ptr<PhaseImpl> _impl;
};
