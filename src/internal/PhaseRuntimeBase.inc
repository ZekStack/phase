#include "Phase.h"

#include <strata/freertos/Mutex.h>
#include <strata/freertos/Task.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <utility>

namespace {
constexpr uint32_t kWaitPollMs = 10;
constexpr uint32_t kTaskStartTimeoutMs = 1000;
constexpr size_t kEventTextBytes = 192;
constexpr uint8_t kInitTimeout = 0;
constexpr uint8_t kStartTimeout = 1;
constexpr uint8_t kStopTimeout = 2;
constexpr uint8_t kDeinitTimeout = 3;
constexpr size_t kMinStackSizeBytes = 1024;

void copyText(char *destination, size_t destinationSize, const char *source) {
	if (destination == nullptr || destinationSize == 0) return;
	if (source == nullptr) source = "";
	std::snprintf(destination, destinationSize, "%s", source);
}

bool isValidStackSize(size_t stackBytes) {
	return stackBytes >= kMinStackSizeBytes && (stackBytes % sizeof(StackType_t)) == 0;
}

template <typename T>
class PhasePlacedVector {
  public:
	PhasePlacedVector() noexcept
	    : _storage(std::in_place, Strata::Allocator<T>{Strata::Placement::Default}) {}

	void resetPlacement(Strata::Placement placement) {
		_storage.reset();
		_storage.emplace(Strata::Allocator<T>{placement});
	}

	bool empty() const { return _storage->empty(); }
	size_t size() const { return _storage->size(); }
	void clear() { _storage->clear(); }
	void reserve(size_t capacity) { _storage->reserve(capacity); }
	void resize(size_t size) { _storage->resize(size); }

	T &operator[](size_t index) { return (*_storage)[index]; }
	const T &operator[](size_t index) const { return (*_storage)[index]; }

	auto begin() { return _storage->begin(); }
	auto end() { return _storage->end(); }
	auto begin() const { return _storage->begin(); }
	auto end() const { return _storage->end(); }
	auto rbegin() { return _storage->rbegin(); }
	auto rend() { return _storage->rend(); }

	template <typename U>
	void push_back(U &&value) {
		_storage->push_back(std::forward<U>(value));
	}

  private:
	std::optional<Strata::Vector<T>> _storage;
};

enum class DependencyState : uint8_t {
	Ready,
	Waiting,
	FailedRequired,
	SkipOptional,
};

struct PhaseNodeState {
	PhaseNodeType type = PhaseNodeType::None;
	bool optional = false;
	bool initialized = false;
	bool started = false;
	bool ready = false;
	bool failed = false;
	bool skipped = false;
	bool hasStartCallback = false;
};

class PhaseLock {
  public:
	explicit PhaseLock(Strata::FreeRTOS::RecursiveMutex &mutex)
	    : _mutex(mutex), _locked(mutex.lock()) {}

	~PhaseLock() {
		if (_locked) _mutex.unlock();
	}

	PhaseLock(const PhaseLock &) = delete;
	PhaseLock &operator=(const PhaseLock &) = delete;

	explicit operator bool() const { return _locked; }

  private:
	Strata::FreeRTOS::RecursiveMutex &_mutex;
	bool _locked = false;
};
} // namespace

struct PhaseNode {
	explicit PhaseNode(Strata::Placement placement = Strata::Placement::Default)
	    : allocationPlacement(placement),
	      name(Strata::Allocator<char>{placement}),
	      dependencyNames(Strata::Allocator<Strata::String>{placement}),
	      dependencies(Strata::Allocator<size_t>{placement}) {}

	Strata::Placement allocationPlacement = Strata::Placement::Default;
	PhaseNodeType type = PhaseNodeType::Step;
	Strata::String name;
	Strata::Vector<Strata::String> dependencyNames;
	Strata::Vector<size_t> dependencies;
	PhaseCallback initCallback;
	PhaseCallback deinitCallback;
	PhaseCallback startCallback;
	PhaseCallback stopCallback;
	PhaseConditionCallback conditionCallback;
	bool optional = false;
	bool initialized = false;
	bool started = false;
	bool ready = false;
	bool failed = false;
	bool skipped = false;
	bool hasInitTimeout = false;
	bool hasStartTimeout = false;
	bool hasStopTimeout = false;
	bool hasDeinitTimeout = false;
	bool hasGroupTimeout = false;
	bool hasPollInterval = false;
	uint32_t initTimeoutMs = 0;
	uint32_t startTimeoutMs = 0;
	uint32_t stopTimeoutMs = 0;
	uint32_t deinitTimeoutMs = 0;
	uint32_t groupTimeoutMs = 0;
	uint32_t pollIntervalMs = 0;
};

struct PhaseImpl {
	PhaseImpl() noexcept : mutex(Strata::FreeRTOS::RecursiveMutex::create()) {}

	PhaseConfig config{};
	Strata::FreeRTOS::RecursiveMutex mutex;
	PhasePlacedVector<PhaseNode> nodes;
	PhasePlacedVector<size_t> initOrder;
	PhasePlacedVector<size_t> startOrder;
	PhasePlacedVector<uint8_t> validationMarks;
	std::shared_ptr<PhaseChangeCallback> changeCallback;
	std::shared_ptr<PhaseReadyCallback> readyCallback;
	std::shared_ptr<PhaseFailedCallback> failedCallback;
	Strata::FreeRTOS::Task task;
	bool initialized = false;
	bool registrationClosed = false;
	bool graphPrepared = false;
	bool startRequested = false;
	bool stopRequested = false;
	bool ending = false;
	bool taskRunning = false;
	bool taskExitReady = false;
	bool paused = false;
	std::array<char, kEventTextBytes> pauseReason{};
	PhaseState currentState = PhaseState::Idle;
	Strata::Region stackRegion = Strata::Region::Unknown;
	uint32_t bootCount = 0;
	uint32_t rollbackCount = 0;
	uint32_t changeCount = 0;
	size_t stackHighWaterMarkBytes = 0;

	void initializeStorage(Strata::Placement placement, const PhaseConfig &incomingConfig) {
		nodes.resetPlacement(placement);
		initOrder.resetPlacement(placement);
		startOrder.resetPlacement(placement);
		validationMarks.resetPlacement(placement);
		nodes.reserve(incomingConfig.maxNodes);
		initOrder.reserve(incomingConfig.maxNodes);
		startOrder.reserve(incomingConfig.maxNodes);
		validationMarks.resize(incomingConfig.maxNodes);
	}

	static void taskEntry(void *arg) {
		static_cast<PhaseImpl *>(arg)->taskLoop();
	}

	PhaseResult notifyTask() {
		TaskHandle_t handle = nullptr;
		{
			PhaseLock lock(mutex);
			if (!lock) return PhaseResult::failure(PhaseStatus::InternalError, "lock failed");
			handle = task.handle();
		}
		if (handle == nullptr) {
			return PhaseResult::failure(PhaseStatus::NotInitialized, "phase task is not available");
		}
		xTaskNotifyGive(handle);
		return PhaseResult::success();
	}

	bool isEnding() {
		PhaseLock lock(mutex);
		return lock && ending;
	}

	bool shouldStop() {
		PhaseLock lock(mutex);
		return lock && stopRequested;
	}

	bool isPausedFlag() {
		PhaseLock lock(mutex);
		return lock && paused;
	}

	void emitChange(
	    PhaseState state,
	    PhaseNodeType nodeType,
	    const char *nodeName,
	    const char *message,
	    PhaseResult result = PhaseResult::success(),
	    uint32_t durationMs = 0
	) {
		std::shared_ptr<PhaseChangeCallback> callback;
		PhaseChange change;
		std::array<char, kEventTextBytes> pauseReasonSnapshot{};
		std::array<char, kEventTextBytes> messageSnapshot{};
		{
			PhaseLock lock(mutex);
			if (!lock) return;
			currentState = state;
			changeCount++;
			callback = changeCallback;
			copyText(pauseReasonSnapshot.data(), pauseReasonSnapshot.size(), pauseReason.data());
			copyText(
			    messageSnapshot.data(),
			    messageSnapshot.size(),
			    message != nullptr ? message : result.message
			);
			change.state = state;
			change.nodeType = nodeType;
			change.nodeName = nodeName;
			change.pauseReason = pauseReasonSnapshot[0] == '\0' ? nullptr : pauseReasonSnapshot.data();
			change.message = messageSnapshot.data();
			change.isBooting = state == PhaseState::Booting;
			change.isStarting = state == PhaseState::Starting;
			change.isPaused = paused || state == PhaseState::Paused;
			change.isStopping = state == PhaseState::Stopping;
			change.isDeinitializing = state == PhaseState::Deinitializing;
			change.isDone = state == PhaseState::Ready || state == PhaseState::Stopped ||
			                state == PhaseState::Failed || state == PhaseState::Ended;
			change.hasError = !result;
			change.result = result;
			change.result.message = messageSnapshot.data();
			change.durationMs = durationMs;
		}
		if (callback && *callback) (*callback)(change);
	}

	void emitReady() {
		std::shared_ptr<PhaseReadyCallback> callback;
		{
			PhaseLock lock(mutex);
			if (!lock) return;
			callback = readyCallback;
		}
		if (callback && *callback) (*callback)();
	}

	void emitFailed(PhaseResult result) {
		std::shared_ptr<PhaseFailedCallback> callback;
		std::array<char, kEventTextBytes> messageSnapshot{};
		{
			PhaseLock lock(mutex);
			if (!lock) return;
			callback = failedCallback;
			copyText(messageSnapshot.data(), messageSnapshot.size(), result.message);
			result.message = messageSnapshot.data();
		}
		if (callback && *callback) (*callback)(result);
	}

	size_t findNodeIndex(const char *name) const {
		if (name == nullptr) return nodes.size();
		for (size_t i = 0; i < nodes.size(); ++i) {
			if (nodes[i].name == name) return i;
		}
		return nodes.size();
	}

	PhaseResult validateRegistrationOpen() const {
		if (!initialized) {
			return PhaseResult::failure(PhaseStatus::NotInitialized, "phase is not initialized");
		}
		if (registrationClosed) {
			return PhaseResult::failure(PhaseStatus::RegistrationClosed, "registration is closed after start");
		}
		return PhaseResult::success();
	}

	bool hasCycle(size_t index) {
		if (validationMarks[index] == 1) return true;
		if (validationMarks[index] == 2) return false;
		validationMarks[index] = 1;
		for (size_t dependencyIndex : nodes[index].dependencies) {
			if (hasCycle(dependencyIndex)) return true;
		}
		validationMarks[index] = 2;
		return false;
	}

	PhaseResult prepareGraphLocked() {
		if (nodes.empty()) {
			return PhaseResult::failure(PhaseStatus::InvalidArgument, "at least one node is required");
		}
		for (size_t i = 0; i < nodes.size(); ++i) {
			PhaseNode &node = nodes[i];
			if (node.name.empty()) {
				return PhaseResult::failure(PhaseStatus::InvalidArgument, "node name is required");
			}
			if (node.type == PhaseNodeType::Step && !node.initCallback) {
				return PhaseResult::failure(PhaseStatus::InvalidCallback, "init callback is required");
			}
			node.dependencies.clear();
			for (const Strata::String &dependencyName : node.dependencyNames) {
				const size_t dependencyIndex = findNodeIndex(dependencyName.c_str());
				if (dependencyIndex >= nodes.size()) {
					return PhaseResult::failure(PhaseStatus::MissingDependency, "dependency was not registered");
				}
				node.dependencies.push_back(dependencyIndex);
			}
		}
		std::fill(validationMarks.begin(), validationMarks.end(), 0);
		for (size_t i = 0; i < nodes.size(); ++i) {
			if (hasCycle(i)) {
				return PhaseResult::failure(PhaseStatus::CircularDependency, "dependency cycle detected");
			}
		}
		graphPrepared = true;
		return PhaseResult::success("dependency graph prepared");
	}

	bool getNodeState(size_t index, PhaseNodeState &out) {
		PhaseLock lock(mutex);
		if (!lock || index >= nodes.size()) return false;
		const PhaseNode &node = nodes[index];
		out.type = node.type;
		out.optional = node.optional;
		out.initialized = node.initialized;
		out.started = node.started;
		out.ready = node.ready;
		out.failed = node.failed;
		out.skipped = node.skipped;
		out.hasStartCallback = static_cast<bool>(node.startCallback);
		return true;
	}

	DependencyState dependencyState(size_t index, bool initWave) {
		PhaseLock lock(mutex);
		if (!lock || index >= nodes.size()) return DependencyState::FailedRequired;
		const PhaseNode &node = nodes[index];
		for (size_t dependencyIndex : node.dependencies) {
			if (dependencyIndex >= nodes.size()) return DependencyState::FailedRequired;
			const PhaseNode &dependency = nodes[dependencyIndex];
			if (initWave && dependency.type == PhaseNodeType::Group) continue;
			if (dependency.failed || dependency.skipped) {
				return node.optional ? DependencyState::SkipOptional : DependencyState::FailedRequired;
			}
			if (initWave ? !dependency.initialized : !dependency.ready) {
				return DependencyState::Waiting;
			}
		}
		return DependencyState::Ready;
	}

	bool allStepsInitialized() {
		PhaseLock lock(mutex);
		if (!lock) return false;
		for (const PhaseNode &node : nodes) {
			if (node.type == PhaseNodeType::Step && !node.initialized && !node.failed && !node.skipped) {
				return false;
			}
		}
		return true;
	}

	bool allNodesDone() {
		PhaseLock lock(mutex);
		if (!lock) return false;
		for (const PhaseNode &node : nodes) {
			if (!node.ready && !node.failed && !node.skipped) return false;
		}
		return true;
	}

	bool waitIfPaused(bool ignoreStop = false) {
		if (ignoreStop) return true;
		bool emitted = false;
		PhaseState resumeState = PhaseState::Booting;
		{
			PhaseLock lock(mutex);
			if (lock) resumeState = currentState;
		}
		while (isPausedFlag() && !shouldStop() && !isEnding()) {
			if (!emitted) {
				emitChange(PhaseState::Paused, PhaseNodeType::None, nullptr, "phase paused");
				emitted = true;
			}
			ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kWaitPollMs));
		}
		if (emitted && !shouldStop() && !isEnding()) {
			emitChange(resumeState, PhaseNodeType::None, nullptr, "phase resumed");
		}
		return !shouldStop() && !isEnding();
	}

	uint32_t resolveTimeout(const PhaseNode &node, uint8_t kind) const {
		switch (kind) {
		case kInitTimeout: return node.hasInitTimeout ? node.initTimeoutMs : config.defaultInitTimeoutMs;
		case kStartTimeout: return node.hasStartTimeout ? node.startTimeoutMs : config.defaultStartTimeoutMs;
		case kStopTimeout: return node.hasStopTimeout ? node.stopTimeoutMs : config.defaultStopTimeoutMs;
		case kDeinitTimeout: return node.hasDeinitTimeout ? node.deinitTimeoutMs : config.defaultDeinitTimeoutMs;
		default: return 0;
		}
	}

	PhaseResult runLifecycleCallback(
	    size_t index,
	    const PhaseCallback &callback,
	    uint32_t timeoutMs,
	    PhaseState state,
	    const char *startMessage,
	    bool ignoreStop = false
	) {
		if (!callback) return PhaseResult::success();
		if (!waitIfPaused(ignoreStop)) return PhaseResult::failure(PhaseStatus::Busy, "phase stopped");
		const char *name = nodes[index].name.c_str();
		const PhaseNodeType type = nodes[index].type;
		const uint32_t startMs = millis();
		emitChange(state, type, name, startMessage);
		PhaseResult result = callback();
		const uint32_t elapsedMs = millis() - startMs;
		if (result && timeoutMs > 0 && elapsedMs > timeoutMs) {
			result = PhaseResult::failure(PhaseStatus::Timeout, "callback timed out");
		}
		emitChange(state, type, name, result.message, result, elapsedMs);
		return result;
	}

	PhaseResult waitForGroup(size_t index) {
		const PhaseNode &group = nodes[index];
		const uint32_t timeoutMs = group.hasGroupTimeout ? group.groupTimeoutMs : config.defaultGroupTimeoutMs;
		const uint32_t pollMs = group.hasPollInterval ? group.pollIntervalMs : config.conditionPollIntervalMs;
		if (!waitIfPaused()) return PhaseResult::failure(PhaseStatus::Busy, "phase stopped");
		uint32_t elapsedMs = 0;
		emitChange(PhaseState::Starting, group.type, group.name.c_str(), "waiting for group");
		while (!shouldStop() && !isEnding()) {
			if (!waitIfPaused()) return PhaseResult::failure(PhaseStatus::Busy, "phase stopped");
			if (!group.conditionCallback || group.conditionCallback()) {
				emitChange(PhaseState::Starting, group.type, group.name.c_str(), "group ready");
				return PhaseResult::success("group ready");
			}
			if (timeoutMs > 0 && elapsedMs >= timeoutMs) {
				const PhaseResult result = PhaseResult::failure(PhaseStatus::Timeout, "group condition timed out");
				emitChange(PhaseState::Starting, group.type, group.name.c_str(), result.message, result);
				return result;
			}
			const uint32_t delayMs = pollMs == 0 ? 1 : pollMs;
			ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(delayMs));
			if (!isPausedFlag()) elapsedMs += delayMs;
		}
		return PhaseResult::failure(PhaseStatus::Busy, "phase stopped");
	}

	void resetRunState() {
		PhaseLock lock(mutex);
		if (!lock) return;
		initOrder.clear();
		startOrder.clear();
		for (PhaseNode &node : nodes) {
			node.initialized = false;
			node.started = false;
			node.ready = false;
			node.failed = false;
			node.skipped = false;
		}
	}

	PhaseResult markOptionalFailure(size_t index, PhaseResult result, PhaseState state) {
		{
			PhaseLock lock(mutex);
			if (!lock || index >= nodes.size()) {
				return PhaseResult::failure(PhaseStatus::InvalidArgument, "invalid node");
			}
			nodes[index].failed = true;
			nodes[index].ready = false;
		}
		emitChange(state, nodes[index].type, nodes[index].name.c_str(), result.message, result);
		return PhaseResult::success("optional node failed");
	}
