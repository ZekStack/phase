#include "Phase.h"

#include "internal/PhaseMutex.h"
#include "internal/PhaseTaskSupport.h"

#include <algorithm>
#include <freertos/semphr.h>
#include <memory>
#include <new>
#include <vector>

constexpr uint32_t kWaitPollMs = 10;
constexpr uint32_t kTaskStartTimeoutMs = 1000;
constexpr uint8_t kInitTimeout = 0;
constexpr uint8_t kStartTimeout = 1;
constexpr uint8_t kStopTimeout = 2;
constexpr uint8_t kDeinitTimeout = 3;

enum class DependencyState : uint8_t {
	Ready,
	Waiting,
	FailedRequired,
	SkipOptional,
};

struct PhaseNode {
	PhaseNodeType type = PhaseNodeType::Step;
	std::string name;
	std::vector<std::string> dependencies;
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

struct PhaseNodeRuntimeSnapshot {
	size_t index = 0;
	PhaseNodeType type = PhaseNodeType::None;
	std::string name;
	std::vector<std::string> dependencies;
	bool optional = false;
	bool initialized = false;
	bool started = false;
	bool ready = false;
	bool failed = false;
	bool skipped = false;
	bool hasStartCallback = false;
};

struct PhaseStepCallbackSnapshot {
	std::string name;
	PhaseNodeType type = PhaseNodeType::Step;
	bool optional = false;
	PhaseCallback initCallback;
	PhaseCallback deinitCallback;
	PhaseCallback startCallback;
	PhaseCallback stopCallback;
	uint32_t initTimeoutMs = 0;
	uint32_t startTimeoutMs = 0;
	uint32_t stopTimeoutMs = 0;
	uint32_t deinitTimeoutMs = 0;
};

struct PhaseGroupCallbackSnapshot {
	std::string name;
	PhaseNodeType type = PhaseNodeType::Group;
	bool optional = false;
	PhaseConditionCallback conditionCallback;
	uint32_t timeoutMs = 0;
	uint32_t pollMs = 0;
};

struct PhaseImpl {
	PhaseConfig config{};
	PhaseMutex mutex;
	std::vector<PhaseNode> nodes;
	std::vector<size_t> initOrder;
	std::vector<size_t> startOrder;
	PhaseChangeCallback changeCallback;
	PhaseReadyCallback readyCallback;
	PhaseFailedCallback failedCallback;
	TaskHandle_t taskHandle = nullptr;
	SemaphoreHandle_t taskStarted = nullptr;
	bool createdWithCaps = false;
	bool initialized = false;
	bool registrationClosed = false;
	bool startRequested = false;
	bool stopRequested = false;
	bool ending = false;
	bool taskRunning = false;
	bool paused = false;
	std::string pauseReason;
	PhaseState currentState = PhaseState::Idle;
	PhaseStackType actualStackType = PhaseStackType::Internal;
	uint32_t bootCount = 0;
	uint32_t rollbackCount = 0;
	uint32_t changeCount = 0;
	size_t stackHighWaterMarkBytes = 0;

	~PhaseImpl() {
		if (taskStarted != nullptr) {
			vSemaphoreDelete(taskStarted);
		}
	}

	static void taskEntry(void *arg) {
		static_cast<PhaseImpl *>(arg)->taskLoop();
	}

	PhaseResult notifyTask() {
		if (taskHandle == nullptr) {
			return PhaseResult::failure(PhaseStatus::NotInitialized, "phase task is not available");
		}
		xTaskNotifyGive(taskHandle);
		return PhaseResult::success();
	}

	void setState(PhaseState state) {
		PhaseLock lock(mutex);
		if (!lock) {
			return;
		}
		currentState = state;
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

	const char *pauseReasonText() {
		return pauseReason.empty() ? nullptr : pauseReason.c_str();
	}

	void emitChange(
	    PhaseState state,
	    PhaseNodeType nodeType,
	    const char *nodeName,
	    const char *message,
	    PhaseResult result = PhaseResult::success(),
	    uint32_t durationMs = 0
	) {
		PhaseChangeCallback callback;
		PhaseChange change;
		{
			PhaseLock lock(mutex);
			if (!lock) {
				return;
			}
			currentState = state;
			changeCount++;
			callback = changeCallback;
			change.state = state;
			change.nodeType = nodeType;
			change.nodeName = nodeName;
			change.pauseReason = pauseReasonText();
			change.message = message != nullptr ? message : result.message;
			change.isBooting = state == PhaseState::Booting;
			change.isStarting = state == PhaseState::Starting;
			change.isPaused = paused || state == PhaseState::Paused;
			change.isStopping = state == PhaseState::Stopping;
			change.isDeinitializing = state == PhaseState::Deinitializing;
			change.isDone =
			    state == PhaseState::Ready || state == PhaseState::Stopped ||
			    state == PhaseState::Failed || state == PhaseState::Ended;
			change.hasError = !result;
			change.result = result;
			change.durationMs = durationMs;
		}
		if (callback) {
			callback(change);
		}
	}

	void emitReady() {
		PhaseReadyCallback callback;
		{
			PhaseLock lock(mutex);
			if (!lock) {
				return;
			}
			callback = readyCallback;
		}
		if (callback) {
			callback();
		}
	}

	void emitFailed(PhaseResult result) {
		PhaseFailedCallback callback;
		{
			PhaseLock lock(mutex);
			if (!lock) {
				return;
			}
			callback = failedCallback;
		}
		if (callback) {
			callback(result);
		}
	}

	size_t findNodeIndex(const std::string &name) const {
		for (size_t i = 0; i < nodes.size(); ++i) {
			if (nodes[i].name == name) {
				return i;
			}
		}
		return nodes.size();
	}

	PhaseNode *findNode(const std::string &name) {
		const size_t index = findNodeIndex(name);
		return index < nodes.size() ? &nodes[index] : nullptr;
	}

	PhaseResult validateRegistrationOpen() {
		if (!initialized) {
			return PhaseResult::failure(PhaseStatus::NotInitialized, "phase is not initialized");
		}
		if (registrationClosed) {
			return PhaseResult::failure(
			    PhaseStatus::RegistrationClosed,
			    "registration is closed after start"
			);
		}
		return PhaseResult::success();
	}

	PhaseResult validateGraph() {
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
			if (node.dependencies.size() > config.maxDependenciesPerNode) {
				return PhaseResult::failure(
				    PhaseStatus::TooManyDependencies,
				    "too many dependencies"
				);
			}
			for (size_t j = i + 1; j < nodes.size(); ++j) {
				if (node.name == nodes[j].name) {
					return PhaseResult::failure(PhaseStatus::DuplicateName, "duplicate node name");
				}
			}
			for (const std::string &dependency : node.dependencies) {
				if (dependency.empty()) {
					return PhaseResult::failure(
					    PhaseStatus::InvalidArgument,
					    "dependency name is required"
					);
				}
				if (findNodeIndex(dependency) >= nodes.size()) {
					return PhaseResult::failure(
					    PhaseStatus::MissingDependency,
					    "dependency was not registered"
					);
				}
			}
		}

		std::vector<uint8_t> marks(nodes.size(), 0);
		for (size_t i = 0; i < nodes.size(); ++i) {
			if (hasCycle(i, marks)) {
				return PhaseResult::failure(
				    PhaseStatus::CircularDependency,
				    "dependency cycle detected"
				);
			}
		}
		return PhaseResult::success();
	}

	bool hasCycle(size_t index, std::vector<uint8_t> &marks) {
		if (marks[index] == 1) {
			return true;
		}
		if (marks[index] == 2) {
			return false;
		}
		marks[index] = 1;
		for (const std::string &dependency : nodes[index].dependencies) {
			const size_t dependencyIndex = findNodeIndex(dependency);
			if (dependencyIndex < nodes.size() && hasCycle(dependencyIndex, marks)) {
				return true;
			}
		}
		marks[index] = 2;
		return false;
	}

	size_t nodeCount() {
		PhaseLock lock(mutex);
		if (!lock) {
			return 0;
		}
		return nodes.size();
	}

	bool getNodeRuntimeSnapshot(size_t index, PhaseNodeRuntimeSnapshot &out) {
		PhaseLock lock(mutex);
		if (!lock || index >= nodes.size()) {
			return false;
		}
		const PhaseNode &node = nodes[index];
		out.index = index;
		out.type = node.type;
		out.name = node.name;
		out.dependencies = node.dependencies;
		out.optional = node.optional;
		out.initialized = node.initialized;
		out.started = node.started;
		out.ready = node.ready;
		out.failed = node.failed;
		out.skipped = node.skipped;
		out.hasStartCallback = static_cast<bool>(node.startCallback);
		return true;
	}

	bool getNodeRuntimeSnapshot(const std::string &name, PhaseNodeRuntimeSnapshot &out) {
		PhaseLock lock(mutex);
		if (!lock) {
			return false;
		}
		for (size_t i = 0; i < nodes.size(); ++i) {
			const PhaseNode &node = nodes[i];
			if (node.name != name) {
				continue;
			}
			out.index = i;
			out.type = node.type;
			out.name = node.name;
			out.dependencies = node.dependencies;
			out.optional = node.optional;
			out.initialized = node.initialized;
			out.started = node.started;
			out.ready = node.ready;
			out.failed = node.failed;
			out.skipped = node.skipped;
			out.hasStartCallback = static_cast<bool>(node.startCallback);
			return true;
		}
		return false;
	}

	bool getStepCallbackSnapshot(size_t index, PhaseStepCallbackSnapshot &out) {
		PhaseLock lock(mutex);
		if (!lock || index >= nodes.size() || nodes[index].type != PhaseNodeType::Step) {
			return false;
		}
		const PhaseNode &node = nodes[index];
		out.name = node.name;
		out.type = node.type;
		out.optional = node.optional;
		out.initCallback = node.initCallback;
		out.deinitCallback = node.deinitCallback;
		out.startCallback = node.startCallback;
		out.stopCallback = node.stopCallback;
		out.initTimeoutMs = resolveTimeout(node, kInitTimeout);
		out.startTimeoutMs = resolveTimeout(node, kStartTimeout);
		out.stopTimeoutMs = resolveTimeout(node, kStopTimeout);
		out.deinitTimeoutMs = resolveTimeout(node, kDeinitTimeout);
		return true;
	}

	bool getGroupCallbackSnapshot(size_t index, PhaseGroupCallbackSnapshot &out) {
		PhaseLock lock(mutex);
		if (!lock || index >= nodes.size() || nodes[index].type != PhaseNodeType::Group) {
			return false;
		}
		const PhaseNode &node = nodes[index];
		out.name = node.name;
		out.type = node.type;
		out.optional = node.optional;
		out.conditionCallback = node.conditionCallback;
		out.timeoutMs = node.hasGroupTimeout ? node.groupTimeoutMs : config.defaultGroupTimeoutMs;
		out.pollMs = node.hasPollInterval ? node.pollIntervalMs : config.conditionPollIntervalMs;
		return true;
	}

	void resetRunState() {
		PhaseLock lock(mutex);
		if (!lock) {
			return;
		}
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

	DependencyState initDependencyState(const PhaseNodeRuntimeSnapshot &node) {
		for (const std::string &dependencyName : node.dependencies) {
			PhaseNodeRuntimeSnapshot dependency;
			if (!getNodeRuntimeSnapshot(dependencyName, dependency)) {
				return DependencyState::FailedRequired;
			}
			if (dependency.type == PhaseNodeType::Group) {
				continue;
			}
			if (dependency.failed || dependency.skipped) {
				return node.optional ? DependencyState::SkipOptional : DependencyState::FailedRequired;
			}
			if (!dependency.initialized) {
				return DependencyState::Waiting;
			}
		}
		return DependencyState::Ready;
	}

	DependencyState readinessDependencyState(const PhaseNodeRuntimeSnapshot &node) {
		for (const std::string &dependencyName : node.dependencies) {
			PhaseNodeRuntimeSnapshot dependency;
			if (!getNodeRuntimeSnapshot(dependencyName, dependency)) {
				return DependencyState::FailedRequired;
			}
			if (dependency.failed || dependency.skipped) {
				return node.optional ? DependencyState::SkipOptional : DependencyState::FailedRequired;
			}
			if (!dependency.ready) {
				return DependencyState::Waiting;
			}
		}
		return DependencyState::Ready;
	}

	bool allStepsInitialized() {
		PhaseLock lock(mutex);
		if (!lock) {
			return false;
		}
		for (const PhaseNode &node : nodes) {
			if (node.type == PhaseNodeType::Step && !node.initialized && !node.failed && !node.skipped) {
				return false;
			}
		}
		return true;
	}

	bool allNodesDone() {
		PhaseLock lock(mutex);
		if (!lock) {
			return false;
		}
		for (const PhaseNode &node : nodes) {
			if (!node.ready && !node.failed && !node.skipped) {
				return false;
			}
		}
		return true;
	}

	bool waitIfPaused(bool ignoreStop = false) {
		if (ignoreStop) {
			return true;
		}
		bool emitted = false;
		PhaseState resumeState = PhaseState::Booting;
		{
			PhaseLock lock(mutex);
			if (lock) {
				resumeState = currentState;
			}
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
		case kInitTimeout:
			return node.hasInitTimeout ? node.initTimeoutMs : config.defaultInitTimeoutMs;
		case kStartTimeout:
			return node.hasStartTimeout ? node.startTimeoutMs : config.defaultStartTimeoutMs;
		case kStopTimeout:
			return node.hasStopTimeout ? node.stopTimeoutMs : config.defaultStopTimeoutMs;
		case kDeinitTimeout:
			return node.hasDeinitTimeout ? node.deinitTimeoutMs : config.defaultDeinitTimeoutMs;
		default:
			return 0;
		}
	}

	PhaseResult runLifecycleCallback(
	    const std::string &name,
	    PhaseNodeType type,
	    uint32_t timeoutMs,
	    PhaseCallback callback,
	    PhaseState state,
	    const char *successMessage,
	    bool ignoreStop = false
	) {
		if (!callback) {
			return PhaseResult::success();
		}
		if (!waitIfPaused(ignoreStop)) {
			return PhaseResult::failure(PhaseStatus::Busy, "phase stopped");
		}
		const uint32_t startMs = millis();
		emitChange(state, type, name.c_str(), successMessage);
		PhaseResult result = callback();
		const uint32_t elapsedMs = millis() - startMs;
		if (result && timeoutMs > 0 && elapsedMs > timeoutMs) {
			result = PhaseResult::failure(PhaseStatus::Timeout, "callback timed out");
		}
		emitChange(state, type, name.c_str(), result.message, result, elapsedMs);
		return result;
	}

	PhaseResult waitForGroup(size_t index) {
		PhaseGroupCallbackSnapshot group;
		if (!getGroupCallbackSnapshot(index, group)) {
			return PhaseResult::failure(PhaseStatus::InvalidArgument, "invalid group");
		}
		if (!waitIfPaused()) {
			return PhaseResult::failure(PhaseStatus::Busy, "phase stopped");
		}
		uint32_t elapsedMs = 0;
		emitChange(PhaseState::Starting, group.type, group.name.c_str(), "waiting for group");
		while (!shouldStop() && !isEnding()) {
			if (!waitIfPaused()) {
				return PhaseResult::failure(PhaseStatus::Busy, "phase stopped");
			}
			if (!group.conditionCallback || group.conditionCallback()) {
				emitChange(PhaseState::Starting, group.type, group.name.c_str(), "group ready");
				return PhaseResult::success("group ready");
			}
			if (group.timeoutMs > 0 && elapsedMs >= group.timeoutMs) {
				const PhaseResult result =
				    PhaseResult::failure(PhaseStatus::Timeout, "group condition timed out");
				emitChange(PhaseState::Starting, group.type, group.name.c_str(), result.message, result);
				return result;
			}
			const uint32_t delayMs = group.pollMs == 0 ? 1 : group.pollMs;
			ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(delayMs));
			if (!isPausedFlag()) {
				elapsedMs += delayMs;
			}
		}
		return PhaseResult::failure(PhaseStatus::Busy, "phase stopped");
	}

	PhaseResult markOptionalFailure(
	    size_t index,
	    PhaseResult result,
	    PhaseState state = PhaseState::Booting
	) {
		std::string name;
		PhaseNodeType type = PhaseNodeType::None;
		{
			PhaseLock lock(mutex);
			if (!lock || index >= nodes.size()) {
				return PhaseResult::failure(PhaseStatus::InvalidArgument, "invalid node");
			}
			PhaseNode &node = nodes[index];
			node.failed = true;
			node.ready = false;
			name = node.name;
			type = node.type;
		}
		emitChange(state, type, name.c_str(), result.message, result);
		return PhaseResult::success("optional node failed");
	}

	PhaseResult runOneInitAction(bool &madeProgress) {
		madeProgress = false;
		const size_t count = nodeCount();
		for (size_t i = 0; i < count; ++i) {
			PhaseNodeRuntimeSnapshot node;
			if (!getNodeRuntimeSnapshot(i, node)) {
				continue;
			}
			if (node.type != PhaseNodeType::Step || node.initialized || node.failed || node.skipped) {
				continue;
			}
			const DependencyState deps = initDependencyState(node);
			if (deps == DependencyState::Waiting) {
				continue;
			}
			if (deps == DependencyState::SkipOptional) {
				{
					PhaseLock lock(mutex);
					if (lock) {
						nodes[i].skipped = true;
					}
				}
				madeProgress = true;
				emitChange(
				    PhaseState::Booting,
				    node.type,
				    node.name.c_str(),
				    "optional node skipped"
				);
				return PhaseResult::success();
			}
			if (deps == DependencyState::FailedRequired) {
				{
					PhaseLock lock(mutex);
					if (lock) {
						nodes[i].failed = true;
					}
				}
				madeProgress = true;
				return PhaseResult::failure(
				    PhaseStatus::DependencyFailed,
				    "required dependency failed"
				);
			}

			PhaseStepCallbackSnapshot step;
			if (!getStepCallbackSnapshot(i, step)) {
				continue;
			}
			PhaseResult result = runLifecycleCallback(
			    step.name,
			    step.type,
			    step.initTimeoutMs,
			    step.initCallback,
			    PhaseState::Booting,
			    "initializing step"
			);
			madeProgress = true;
			if (!result) {
				if (step.optional) {
					return markOptionalFailure(i, result);
				}
				PhaseLock lock(mutex);
				if (lock) {
					nodes[i].failed = true;
				}
				return result;
			}
			{
				PhaseLock lock(mutex);
				if (lock && i < nodes.size()) {
					nodes[i].initialized = true;
					initOrder.push_back(i);
				}
			}
			return result;
		}
		return PhaseResult::success();
	}

	PhaseResult runOneReadinessAction(bool &madeProgress) {
		madeProgress = false;
		const size_t count = nodeCount();
		for (size_t i = 0; i < count; ++i) {
			PhaseNodeRuntimeSnapshot node;
			if (!getNodeRuntimeSnapshot(i, node)) {
				continue;
			}
			if (node.ready || node.failed || node.skipped) {
				continue;
			}
			const DependencyState deps = readinessDependencyState(node);
			if (deps == DependencyState::Waiting) {
				continue;
			}
			if (deps == DependencyState::SkipOptional) {
				{
					PhaseLock lock(mutex);
					if (lock) {
						nodes[i].skipped = true;
					}
				}
				madeProgress = true;
				emitChange(
				    PhaseState::Starting,
				    node.type,
				    node.name.c_str(),
				    "optional node skipped"
				);
				return PhaseResult::success();
			}
			if (deps == DependencyState::FailedRequired) {
				PhaseLock lock(mutex);
				if (lock) {
					nodes[i].failed = true;
				}
				madeProgress = true;
				return PhaseResult::failure(
				    PhaseStatus::DependencyFailed,
				    "required dependency failed"
				);
			}

			if (node.type == PhaseNodeType::Group) {
				PhaseResult result = waitForGroup(i);
				madeProgress = true;
				if (result) {
					PhaseLock lock(mutex);
					if (lock) {
						nodes[i].ready = true;
					}
					return result;
				}
				if (node.optional) {
					return markOptionalFailure(i, result, PhaseState::Starting);
				}
				PhaseLock lock(mutex);
				if (lock) {
					nodes[i].failed = true;
				}
				return result;
			}

			if (!node.initialized) {
				continue;
			}
			if (!node.hasStartCallback) {
				PhaseLock lock(mutex);
				if (lock) {
					nodes[i].ready = true;
				}
				madeProgress = true;
				emitChange(PhaseState::Starting, node.type, node.name.c_str(), "step ready");
				return PhaseResult::success("step ready");
			}

			PhaseStepCallbackSnapshot step;
			if (!getStepCallbackSnapshot(i, step)) {
				continue;
			}
			PhaseResult result = runLifecycleCallback(
			    step.name,
			    step.type,
			    step.startTimeoutMs,
			    step.startCallback,
			    PhaseState::Starting,
			    "starting step"
			);
			madeProgress = true;
			if (!result) {
				if (step.optional) {
					if (step.deinitCallback) {
						(void)runLifecycleCallback(
						    step.name,
						    step.type,
						    step.deinitTimeoutMs,
						    step.deinitCallback,
						    PhaseState::Deinitializing,
						    "deinitializing optional step"
						);
					}
					{
						PhaseLock lock(mutex);
						if (lock) {
							nodes[i].initialized = false;
						}
					}
					return markOptionalFailure(i, result, PhaseState::Starting);
				}
				PhaseLock lock(mutex);
				if (lock) {
					nodes[i].failed = true;
				}
				return result;
			}
			{
				PhaseLock lock(mutex);
				if (lock) {
					nodes[i].started = true;
					nodes[i].ready = true;
					startOrder.push_back(i);
				}
			}
			return result;
		}
		return PhaseResult::success();
	}

	PhaseResult runBoot() {
		{
			PhaseLock lock(mutex);
			if (!lock) {
				return PhaseResult::failure(PhaseStatus::InternalError, "lock failed");
			}
			registrationClosed = true;
			stopRequested = false;
			bootCount++;
		}
		PhaseResult validation = validateGraph();
		if (!validation) {
			emitChange(PhaseState::Failed, PhaseNodeType::None, nullptr, validation.message, validation);
			emitFailed(validation);
			return validation;
		}
		resetRunState();

		emitChange(PhaseState::Booting, PhaseNodeType::None, nullptr, "phase boot started");
		while (!allStepsInitialized() && !shouldStop() && !isEnding()) {
			bool madeProgress = false;
			PhaseResult result = runOneInitAction(madeProgress);
			if (!result) {
				rollback();
				emitChange(PhaseState::Failed, PhaseNodeType::None, nullptr, result.message, result);
				emitFailed(result);
				return result;
			}
			if (!madeProgress) {
				PhaseResult stalled =
				    PhaseResult::failure(PhaseStatus::InternalError, "dependency graph stalled");
				rollback();
				emitChange(PhaseState::Failed, PhaseNodeType::None, nullptr, stalled.message, stalled);
				emitFailed(stalled);
				return stalled;
			}
		}

		if (shouldStop() || isEnding()) {
			return runShutdown();
		}

		emitChange(PhaseState::Starting, PhaseNodeType::None, nullptr, "phase start/readiness started");
		while (!allNodesDone() && !shouldStop() && !isEnding()) {
			bool madeProgress = false;
			PhaseResult result = runOneReadinessAction(madeProgress);
			if (!result) {
				rollback();
				emitChange(PhaseState::Failed, PhaseNodeType::None, nullptr, result.message, result);
				emitFailed(result);
				return result;
			}
			if (!madeProgress) {
				PhaseResult stalled =
				    PhaseResult::failure(PhaseStatus::InternalError, "dependency graph stalled");
				rollback();
				emitChange(PhaseState::Failed, PhaseNodeType::None, nullptr, stalled.message, stalled);
				emitFailed(stalled);
				return stalled;
			}
		}

		if (shouldStop() || isEnding()) {
			return runShutdown();
		}

		emitChange(PhaseState::Ready, PhaseNodeType::None, nullptr, "phase ready");
		emitReady();
		return PhaseResult::success("phase ready");
	}

	PhaseResult rollback() {
		{
			PhaseLock lock(mutex);
			if (lock) {
				rollbackCount++;
			}
		}
		stopStartedSteps();
		deinitInitializedSteps();
		resetGroups();
		return PhaseResult::success("rollback complete");
	}

	PhaseResult runShutdown() {
		emitChange(PhaseState::Stopping, PhaseNodeType::None, nullptr, "phase stopping");
		stopStartedSteps();
		deinitInitializedSteps();
		resetGroups();
		{
			PhaseLock lock(mutex);
			if (lock) {
				stopRequested = false;
			}
		}
		emitChange(PhaseState::Stopped, PhaseNodeType::None, nullptr, "phase stopped");
		return PhaseResult::success("phase stopped");
	}

	void stopStartedSteps() {
		std::vector<size_t> order;
		{
			PhaseLock lock(mutex);
			if (!lock) {
				return;
			}
			order = startOrder;
		}
		for (auto it = order.rbegin(); it != order.rend(); ++it) {
			PhaseNodeRuntimeSnapshot node;
			PhaseStepCallbackSnapshot step;
			if (!getNodeRuntimeSnapshot(*it, node) || !getStepCallbackSnapshot(*it, step)) {
				continue;
			}
			if (!node.started) {
				continue;
			}
			if (step.stopCallback) {
				(void)runLifecycleCallback(
				    step.name,
				    step.type,
				    step.stopTimeoutMs,
				    step.stopCallback,
				    PhaseState::Stopping,
				    "stopping step",
				    true
				);
			}
			{
				PhaseLock lock(mutex);
				if (lock && *it < nodes.size()) {
					nodes[*it].started = false;
					nodes[*it].ready = false;
				}
			}
		}
		PhaseLock lock(mutex);
		if (lock) {
			startOrder.clear();
		}
	}

	void deinitInitializedSteps() {
		std::vector<size_t> order;
		{
			PhaseLock lock(mutex);
			if (!lock) {
				return;
			}
			order = initOrder;
		}
		for (auto it = order.rbegin(); it != order.rend(); ++it) {
			PhaseNodeRuntimeSnapshot node;
			PhaseStepCallbackSnapshot step;
			if (!getNodeRuntimeSnapshot(*it, node) || !getStepCallbackSnapshot(*it, step)) {
				continue;
			}
			if (!node.initialized) {
				continue;
			}
			if (step.deinitCallback) {
				(void)runLifecycleCallback(
				    step.name,
				    step.type,
				    step.deinitTimeoutMs,
				    step.deinitCallback,
				    PhaseState::Deinitializing,
				    "deinitializing step",
				    true
				);
			}
			{
				PhaseLock lock(mutex);
				if (lock && *it < nodes.size()) {
					nodes[*it].initialized = false;
					nodes[*it].ready = false;
				}
			}
		}
		PhaseLock lock(mutex);
		if (lock) {
			initOrder.clear();
		}
	}

	void resetGroups() {
		PhaseLock lock(mutex);
		if (!lock) {
			return;
		}
		for (PhaseNode &node : nodes) {
			if (node.type == PhaseNodeType::Group) {
				node.ready = false;
			}
		}
	}

	void taskLoop() {
		{
			PhaseLock lock(mutex);
			if (lock) {
				taskRunning = true;
			}
		}
		if (taskStarted != nullptr) {
			xSemaphoreGive(taskStarted);
		}
		while (!isEnding()) {
			ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
			if (isEnding()) {
				break;
			}
			bool localStop = false;
			bool localStart = false;
			{
				PhaseLock lock(mutex);
				if (lock) {
					localStop = stopRequested;
					localStart = startRequested;
					startRequested = false;
				}
			}
			if (localStop) {
				(void)runShutdown();
			}
			if (localStart) {
				(void)runBoot();
			}
		}
		if (shouldStop()) {
			(void)runShutdown();
		}
		{
			PhaseLock lock(mutex);
			if (lock) {
				currentState = PhaseState::Ended;
				taskRunning = false;
				stackHighWaterMarkBytes = phase_task_support::currentStackHighWaterMarkBytes();
				taskHandle = nullptr;
			}
		}
		phase_task_support::deleteCurrentTask(createdWithCaps);
	}
};

PhaseResult PhaseResult::success(const char *message) {
	return PhaseResult{true, PhaseStatus::Ok, message != nullptr ? message : "ok"};
}

PhaseResult PhaseResult::failure(PhaseStatus status, const char *message) {
	return PhaseResult{false, status, message != nullptr ? message : "error"};
}

PhaseStepBuilder::PhaseStepBuilder(Phase *phase, size_t index, PhaseResult result)
    : _phase(phase), _index(index), _result(result) {
}

PhaseStepBuilder &PhaseStepBuilder::depends(const char *name) {
	if (_result && _phase != nullptr) {
		_result = _phase->addDependency(_index, name);
	}
	return *this;
}

PhaseStepBuilder &PhaseStepBuilder::depends(std::initializer_list<const char *> names) {
	for (const char *name : names) {
		depends(name);
		if (!_result) {
			break;
		}
	}
	return *this;
}

PhaseStepBuilder &PhaseStepBuilder::optional() {
	if (_result && _phase != nullptr) {
		_result = _phase->setOptional(_index);
	}
	return *this;
}

PhaseStepBuilder &PhaseStepBuilder::initTimeout(uint32_t timeoutMs) {
	if (_result && _phase != nullptr) {
		_result = _phase->setStepTimeout(_index, kInitTimeout, timeoutMs);
	}
	return *this;
}

PhaseStepBuilder &PhaseStepBuilder::startTimeout(uint32_t timeoutMs) {
	if (_result && _phase != nullptr) {
		_result = _phase->setStepTimeout(_index, kStartTimeout, timeoutMs);
	}
	return *this;
}

PhaseStepBuilder &PhaseStepBuilder::stopTimeout(uint32_t timeoutMs) {
	if (_result && _phase != nullptr) {
		_result = _phase->setStepTimeout(_index, kStopTimeout, timeoutMs);
	}
	return *this;
}

PhaseStepBuilder &PhaseStepBuilder::deinitTimeout(uint32_t timeoutMs) {
	if (_result && _phase != nullptr) {
		_result = _phase->setStepTimeout(_index, kDeinitTimeout, timeoutMs);
	}
	return *this;
}

PhaseResult PhaseStepBuilder::setStartCallbacks(
    PhaseCallback startCallback,
    PhaseCallback stopCallback
) {
	if (_phase == nullptr) {
		return PhaseResult::failure(PhaseStatus::InternalError, "builder is not attached");
	}
	return _phase->setStepStartCallbacks(_index, startCallback, stopCallback);
}

PhaseGroupBuilder::PhaseGroupBuilder(Phase *phase, size_t index, PhaseResult result)
    : _phase(phase), _index(index), _result(result) {
}

PhaseGroupBuilder &PhaseGroupBuilder::depends(const char *name) {
	if (_result && _phase != nullptr) {
		_result = _phase->addDependency(_index, name);
	}
	return *this;
}

PhaseGroupBuilder &PhaseGroupBuilder::depends(std::initializer_list<const char *> names) {
	for (const char *name : names) {
		depends(name);
		if (!_result) {
			break;
		}
	}
	return *this;
}

PhaseGroupBuilder &PhaseGroupBuilder::optional() {
	if (_result && _phase != nullptr) {
		_result = _phase->setOptional(_index);
	}
	return *this;
}

PhaseGroupBuilder &PhaseGroupBuilder::condition(PhaseConditionCallback callback) {
	return condition(callback, 0);
}

PhaseGroupBuilder &PhaseGroupBuilder::condition(
    PhaseConditionCallback callback,
    uint32_t timeoutMs
) {
	if (_result && _phase != nullptr) {
		_result = _phase->setGroupCondition(_index, callback, timeoutMs, timeoutMs > 0);
	}
	return *this;
}

PhaseGroupBuilder &PhaseGroupBuilder::conditionPollInterval(uint32_t intervalMs) {
	if (_result && _phase != nullptr) {
		_result = _phase->setGroupPollInterval(_index, intervalMs);
	}
	return *this;
}

Phase::Phase() : _impl(new (std::nothrow) PhaseImpl()) {
}

Phase::~Phase() {
	(void)end(0);
}

PhaseResult Phase::init(const PhaseConfig &config) {
	if (!_impl) {
		return PhaseResult::failure(PhaseStatus::OutOfMemory, "phase allocation failed");
	}
	SemaphoreHandle_t taskStarted = nullptr;
	{
		PhaseLock lock(_impl->mutex);
		if (!lock) {
			return PhaseResult::failure(PhaseStatus::InternalError, "lock failed");
		}
		if (_impl->ending || _impl->currentState == PhaseState::Ended) {
			return PhaseResult::failure(PhaseStatus::Busy, "phase has ended");
		}
		if (_impl->initialized) {
			return PhaseResult::failure(PhaseStatus::AlreadyInitialized, "phase is already initialized");
		}
		if (config.maxNodes == 0 || config.maxDependenciesPerNode == 0) {
			return PhaseResult::failure(PhaseStatus::InvalidArgument, "invalid phase limits");
		}
		if (!phase_task_support::isValidStackSize(config.stackSizeBytes)) {
			return PhaseResult::failure(PhaseStatus::InvalidArgument, "invalid stack size");
		}
		if (_impl->taskStarted == nullptr) {
			_impl->taskStarted = xSemaphoreCreateBinary();
			if (_impl->taskStarted == nullptr) {
				return PhaseResult::failure(PhaseStatus::OutOfMemory, "phase task semaphore allocation failed");
			}
		} else {
			while (xSemaphoreTake(_impl->taskStarted, 0) == pdTRUE) {
			}
		}

		_impl->config = config;
		_impl->actualStackType = PhaseStackType::Internal;
		bool createdWithCaps = false;
		bool usePsram =
		    config.stackType == PhaseStackType::Psram ||
		    (config.stackType == PhaseStackType::Auto && phase_task_support::hasExternalStackSupport());
		BaseType_t created = phase_task_support::createTask(
		    PhaseImpl::taskEntry,
		    config.taskName,
		    config.stackSizeBytes,
		    _impl.get(),
		    config.priority,
		    &_impl->taskHandle,
		    config.coreId,
		    usePsram,
		    createdWithCaps
		);
		if (created != pdPASS && config.stackType == PhaseStackType::Auto && usePsram) {
			usePsram = false;
			created = phase_task_support::createTask(
			    PhaseImpl::taskEntry,
			    config.taskName,
			    config.stackSizeBytes,
			    _impl.get(),
			    config.priority,
			    &_impl->taskHandle,
			    config.coreId,
			    false,
			    createdWithCaps
			);
		}
		if (created != pdPASS) {
			_impl->taskHandle = nullptr;
			return PhaseResult::failure(PhaseStatus::TaskCreateFailed, "phase task create failed");
		}
		_impl->createdWithCaps = createdWithCaps;
		_impl->actualStackType =
		    usePsram && createdWithCaps ? PhaseStackType::Psram : PhaseStackType::Internal;
		_impl->initialized = true;
		_impl->currentState = PhaseState::Idle;
		taskStarted = _impl->taskStarted;
	}
	if (xSemaphoreTake(taskStarted, pdMS_TO_TICKS(kTaskStartTimeoutMs)) != pdTRUE) {
		(void)end(kTaskStartTimeoutMs);
		return PhaseResult::failure(PhaseStatus::Timeout, "phase task start timed out");
	}
	return PhaseResult::success("phase initialized");
}

PhaseResult Phase::start() {
	if (!_impl) {
		return PhaseResult::failure(PhaseStatus::OutOfMemory, "phase allocation failed");
	}
	{
		PhaseLock lock(_impl->mutex);
		if (!lock) {
			return PhaseResult::failure(PhaseStatus::InternalError, "lock failed");
		}
		if (_impl->ending) {
			return PhaseResult::failure(PhaseStatus::Busy, "phase is ending");
		}
		if (_impl->currentState == PhaseState::Ended) {
			return PhaseResult::failure(PhaseStatus::Busy, "phase has ended");
		}
		if (!_impl->initialized) {
			return PhaseResult::failure(PhaseStatus::NotInitialized, "phase is not initialized");
		}
		if (_impl->currentState != PhaseState::Idle && _impl->currentState != PhaseState::Stopped) {
			if (_impl->currentState == PhaseState::Ready) {
				return PhaseResult::failure(PhaseStatus::Busy, "phase is already ready");
			}
			return PhaseResult::failure(PhaseStatus::Busy, "phase is busy");
		}
		_impl->registrationClosed = true;
		_impl->stopRequested = false;
		_impl->startRequested = true;
	}
	return _impl->notifyTask();
}

PhaseResult Phase::stop() {
	if (!_impl) {
		return PhaseResult::failure(PhaseStatus::OutOfMemory, "phase allocation failed");
	}
	{
		PhaseLock lock(_impl->mutex);
		if (!lock) {
			return PhaseResult::failure(PhaseStatus::InternalError, "lock failed");
		}
		if (_impl->ending) {
			return PhaseResult::failure(PhaseStatus::Busy, "phase is ending");
		}
		if (_impl->currentState == PhaseState::Ended) {
			return PhaseResult::failure(PhaseStatus::Busy, "phase has ended");
		}
		if (!_impl->initialized) {
			return PhaseResult::failure(PhaseStatus::NotInitialized, "phase is not initialized");
		}
		if (_impl->currentState == PhaseState::Idle ||
		    _impl->currentState == PhaseState::Stopped) {
			if (_impl->startRequested) {
				_impl->startRequested = false;
				return PhaseResult::success("phase start cancelled");
			}
			return PhaseResult::success("phase stopped");
		}
		if (_impl->currentState == PhaseState::Failed) {
			return PhaseResult::success("phase stopped");
		}
		if (_impl->currentState == PhaseState::Stopping ||
		    _impl->currentState == PhaseState::Deinitializing) {
			return PhaseResult::success("phase stopping");
		}
		_impl->stopRequested = true;
	}
	return _impl->notifyTask();
}

PhaseResult Phase::end(uint32_t timeoutMs) {
	if (!_impl) {
		return PhaseResult::success();
	}
	TaskHandle_t handle = nullptr;
	{
		PhaseLock lock(_impl->mutex);
		if (!lock) {
			return PhaseResult::failure(PhaseStatus::InternalError, "lock failed");
		}
		if (!_impl->initialized) {
			return PhaseResult::success("phase ended");
		}
		_impl->ending = true;
		_impl->stopRequested = true;
		handle = _impl->taskHandle;
	}
	if (handle != nullptr) {
		xTaskNotifyGive(handle);
	}
	const uint32_t startMs = millis();
	while (true) {
		{
			PhaseLock lock(_impl->mutex);
			if (lock && !_impl->taskRunning && _impl->taskHandle == nullptr) {
				_impl->initialized = false;
				_impl->currentState = PhaseState::Ended;
				return PhaseResult::success("phase ended");
			}
		}
		if (timeoutMs > 0 && millis() - startMs >= timeoutMs) {
			return PhaseResult::failure(PhaseStatus::Timeout, "phase end timed out");
		}
		vTaskDelay(pdMS_TO_TICKS(kWaitPollMs));
	}
}

PhaseResult Phase::pause(const char *reason) {
	if (!_impl) {
		return PhaseResult::failure(PhaseStatus::OutOfMemory, "phase allocation failed");
	}
	{
		PhaseLock lock(_impl->mutex);
		if (!lock) {
			return PhaseResult::failure(PhaseStatus::InternalError, "lock failed");
		}
		if (_impl->ending) {
			return PhaseResult::failure(PhaseStatus::Busy, "phase is ending");
		}
		if (_impl->currentState == PhaseState::Ended) {
			return PhaseResult::failure(PhaseStatus::Busy, "phase has ended");
		}
		if (!_impl->initialized) {
			return PhaseResult::failure(PhaseStatus::NotInitialized, "phase is not initialized");
		}
		_impl->paused = true;
		_impl->pauseReason = reason != nullptr ? reason : "";
	}
	return _impl->notifyTask();
}

PhaseResult Phase::resume() {
	if (!_impl) {
		return PhaseResult::failure(PhaseStatus::OutOfMemory, "phase allocation failed");
	}
	{
		PhaseLock lock(_impl->mutex);
		if (!lock) {
			return PhaseResult::failure(PhaseStatus::InternalError, "lock failed");
		}
		if (_impl->ending) {
			return PhaseResult::failure(PhaseStatus::Busy, "phase is ending");
		}
		if (_impl->currentState == PhaseState::Ended) {
			return PhaseResult::failure(PhaseStatus::Busy, "phase has ended");
		}
		if (!_impl->initialized) {
			return PhaseResult::failure(PhaseStatus::NotInitialized, "phase is not initialized");
		}
		_impl->paused = false;
		_impl->pauseReason.clear();
	}
	return _impl->notifyTask();
}

bool Phase::isPaused() {
	if (!_impl) {
		return false;
	}
	PhaseLock lock(_impl->mutex);
	return lock && _impl->paused;
}

PhaseState Phase::state() {
	if (!_impl) {
		return PhaseState::Ended;
	}
	PhaseLock lock(_impl->mutex);
	return lock ? _impl->currentState : PhaseState::Failed;
}

PhaseDiag Phase::getDiagnostics() {
	PhaseDiag diag;
	if (!_impl) {
		return diag;
	}
	PhaseLock lock(_impl->mutex);
	if (!lock) {
		return diag;
	}
	diag.nodeCount = _impl->nodes.size();
	for (const PhaseNode &node : _impl->nodes) {
		if (node.initialized) {
			diag.initializedCount++;
		}
		if (node.started) {
			diag.startedCount++;
		}
		if (node.ready) {
			diag.readyCount++;
		}
		if (node.failed) {
			diag.failedCount++;
		}
		if (node.skipped) {
			diag.skippedCount++;
		}
	}
	diag.bootCount = _impl->bootCount;
	diag.rollbackCount = _impl->rollbackCount;
	diag.changeCount = _impl->changeCount;
	diag.stackHighWaterMarkBytes = _impl->stackHighWaterMarkBytes;
	diag.state = _impl->currentState;
	diag.requestedStackType = _impl->config.stackType;
	diag.actualStackType = _impl->actualStackType;
	return diag;
}

void Phase::onChange(PhaseChangeCallback callback) {
	if (!_impl) {
		return;
	}
	PhaseLock lock(_impl->mutex);
	if (lock) {
		_impl->changeCallback = callback;
	}
}

void Phase::onReady(PhaseReadyCallback callback) {
	if (!_impl) {
		return;
	}
	PhaseLock lock(_impl->mutex);
	if (lock) {
		_impl->readyCallback = callback;
	}
}

void Phase::onFailed(PhaseFailedCallback callback) {
	if (!_impl) {
		return;
	}
	PhaseLock lock(_impl->mutex);
	if (lock) {
		_impl->failedCallback = callback;
	}
}

PhaseStepBuilder Phase::addStep(
    const char *name,
    PhaseCallback initCallback,
    PhaseCallback deinitCallback
) {
	if (!_impl) {
		return PhaseStepBuilder(
		    this,
		    0,
		    PhaseResult::failure(PhaseStatus::OutOfMemory, "phase allocation failed")
		);
	}
	PhaseLock lock(_impl->mutex);
	if (!lock) {
		return PhaseStepBuilder(
		    this,
		    0,
		    PhaseResult::failure(PhaseStatus::InternalError, "lock failed")
		);
	}
	PhaseResult open = _impl->validateRegistrationOpen();
	if (!open) {
		return PhaseStepBuilder(this, 0, open);
	}
	if (name == nullptr || name[0] == '\0') {
		return PhaseStepBuilder(
		    this,
		    0,
		    PhaseResult::failure(PhaseStatus::InvalidArgument, "node name is required")
		);
	}
	if (!initCallback) {
		return PhaseStepBuilder(
		    this,
		    0,
		    PhaseResult::failure(PhaseStatus::InvalidCallback, "init callback is required")
		);
	}
	if (_impl->nodes.size() >= _impl->config.maxNodes) {
		return PhaseStepBuilder(
		    this,
		    0,
		    PhaseResult::failure(PhaseStatus::TooManyNodes, "too many nodes")
		);
	}
	if (_impl->findNodeIndex(name) < _impl->nodes.size()) {
		return PhaseStepBuilder(
		    this,
		    0,
		    PhaseResult::failure(PhaseStatus::DuplicateName, "duplicate node name")
		);
	}
	PhaseNode node;
	node.type = PhaseNodeType::Step;
	node.name = name;
	node.initCallback = initCallback;
	node.deinitCallback = deinitCallback;
	_impl->nodes.push_back(node);
	return PhaseStepBuilder(this, _impl->nodes.size() - 1, PhaseResult::success("step added"));
}

PhaseGroupBuilder Phase::addGroup(const char *name) {
	if (!_impl) {
		return PhaseGroupBuilder(
		    this,
		    0,
		    PhaseResult::failure(PhaseStatus::OutOfMemory, "phase allocation failed")
		);
	}
	PhaseLock lock(_impl->mutex);
	if (!lock) {
		return PhaseGroupBuilder(
		    this,
		    0,
		    PhaseResult::failure(PhaseStatus::InternalError, "lock failed")
		);
	}
	PhaseResult open = _impl->validateRegistrationOpen();
	if (!open) {
		return PhaseGroupBuilder(this, 0, open);
	}
	if (name == nullptr || name[0] == '\0') {
		return PhaseGroupBuilder(
		    this,
		    0,
		    PhaseResult::failure(PhaseStatus::InvalidArgument, "node name is required")
		);
	}
	if (_impl->nodes.size() >= _impl->config.maxNodes) {
		return PhaseGroupBuilder(
		    this,
		    0,
		    PhaseResult::failure(PhaseStatus::TooManyNodes, "too many nodes")
		);
	}
	if (_impl->findNodeIndex(name) < _impl->nodes.size()) {
		return PhaseGroupBuilder(
		    this,
		    0,
		    PhaseResult::failure(PhaseStatus::DuplicateName, "duplicate node name")
		);
	}
	PhaseNode node;
	node.type = PhaseNodeType::Group;
	node.name = name;
	_impl->nodes.push_back(node);
	return PhaseGroupBuilder(this, _impl->nodes.size() - 1, PhaseResult::success("group added"));
}

PhaseResult Phase::addDependency(size_t index, const char *name) {
	if (!_impl) {
		return PhaseResult::failure(PhaseStatus::OutOfMemory, "phase allocation failed");
	}
	PhaseLock lock(_impl->mutex);
	if (!lock) {
		return PhaseResult::failure(PhaseStatus::InternalError, "lock failed");
	}
	PhaseResult open = _impl->validateRegistrationOpen();
	if (!open) {
		return open;
	}
	if (index >= _impl->nodes.size()) {
		return PhaseResult::failure(PhaseStatus::InvalidArgument, "invalid node");
	}
	if (name == nullptr || name[0] == '\0') {
		return PhaseResult::failure(PhaseStatus::InvalidArgument, "dependency name is required");
	}
	PhaseNode &node = _impl->nodes[index];
	if (node.dependencies.size() >= _impl->config.maxDependenciesPerNode) {
		return PhaseResult::failure(PhaseStatus::TooManyDependencies, "too many dependencies");
	}
	if (std::find(node.dependencies.begin(), node.dependencies.end(), name) == node.dependencies.end()) {
		node.dependencies.push_back(name);
	}
	return PhaseResult::success("dependency added");
}

PhaseResult Phase::setOptional(size_t index) {
	if (!_impl) {
		return PhaseResult::failure(PhaseStatus::OutOfMemory, "phase allocation failed");
	}
	PhaseLock lock(_impl->mutex);
	if (!lock) {
		return PhaseResult::failure(PhaseStatus::InternalError, "lock failed");
	}
	PhaseResult open = _impl->validateRegistrationOpen();
	if (!open) {
		return open;
	}
	if (index >= _impl->nodes.size()) {
		return PhaseResult::failure(PhaseStatus::InvalidArgument, "invalid node");
	}
	_impl->nodes[index].optional = true;
	return PhaseResult::success("node optional");
}

PhaseResult Phase::setStepStartCallbacks(
    size_t index,
    PhaseCallback startCallback,
    PhaseCallback stopCallback
) {
	if (!_impl) {
		return PhaseResult::failure(PhaseStatus::OutOfMemory, "phase allocation failed");
	}
	PhaseLock lock(_impl->mutex);
	if (!lock) {
		return PhaseResult::failure(PhaseStatus::InternalError, "lock failed");
	}
	PhaseResult open = _impl->validateRegistrationOpen();
	if (!open) {
		return open;
	}
	if (index >= _impl->nodes.size() || _impl->nodes[index].type != PhaseNodeType::Step) {
		return PhaseResult::failure(PhaseStatus::InvalidArgument, "invalid step");
	}
	if (!startCallback) {
		return PhaseResult::failure(PhaseStatus::InvalidCallback, "start callback is required");
	}
	_impl->nodes[index].startCallback = startCallback;
	_impl->nodes[index].stopCallback = stopCallback;
	return PhaseResult::success("start callback added");
}

PhaseResult Phase::setStepTimeout(size_t index, uint8_t timeoutKind, uint32_t timeoutMs) {
	if (!_impl) {
		return PhaseResult::failure(PhaseStatus::OutOfMemory, "phase allocation failed");
	}
	PhaseLock lock(_impl->mutex);
	if (!lock) {
		return PhaseResult::failure(PhaseStatus::InternalError, "lock failed");
	}
	PhaseResult open = _impl->validateRegistrationOpen();
	if (!open) {
		return open;
	}
	if (index >= _impl->nodes.size() || _impl->nodes[index].type != PhaseNodeType::Step) {
		return PhaseResult::failure(PhaseStatus::InvalidArgument, "invalid step");
	}
	PhaseNode &node = _impl->nodes[index];
	switch (timeoutKind) {
	case kInitTimeout:
		node.hasInitTimeout = true;
		node.initTimeoutMs = timeoutMs;
		break;
	case kStartTimeout:
		node.hasStartTimeout = true;
		node.startTimeoutMs = timeoutMs;
		break;
	case kStopTimeout:
		node.hasStopTimeout = true;
		node.stopTimeoutMs = timeoutMs;
		break;
	case kDeinitTimeout:
		node.hasDeinitTimeout = true;
		node.deinitTimeoutMs = timeoutMs;
		break;
	default:
		return PhaseResult::failure(PhaseStatus::InvalidArgument, "invalid timeout kind");
	}
	return PhaseResult::success("timeout set");
}

PhaseResult Phase::setGroupCondition(
    size_t index,
    PhaseConditionCallback callback,
    uint32_t timeoutMs,
    bool hasTimeoutOverride
) {
	if (!_impl) {
		return PhaseResult::failure(PhaseStatus::OutOfMemory, "phase allocation failed");
	}
	PhaseLock lock(_impl->mutex);
	if (!lock) {
		return PhaseResult::failure(PhaseStatus::InternalError, "lock failed");
	}
	PhaseResult open = _impl->validateRegistrationOpen();
	if (!open) {
		return open;
	}
	if (index >= _impl->nodes.size() || _impl->nodes[index].type != PhaseNodeType::Group) {
		return PhaseResult::failure(PhaseStatus::InvalidArgument, "invalid group");
	}
	if (!callback) {
		return PhaseResult::failure(PhaseStatus::InvalidCallback, "condition callback is required");
	}
	PhaseNode &node = _impl->nodes[index];
	node.conditionCallback = callback;
	node.hasGroupTimeout = hasTimeoutOverride;
	node.groupTimeoutMs = timeoutMs;
	return PhaseResult::success("condition set");
}

PhaseResult Phase::setGroupPollInterval(size_t index, uint32_t intervalMs) {
	if (!_impl) {
		return PhaseResult::failure(PhaseStatus::OutOfMemory, "phase allocation failed");
	}
	PhaseLock lock(_impl->mutex);
	if (!lock) {
		return PhaseResult::failure(PhaseStatus::InternalError, "lock failed");
	}
	PhaseResult open = _impl->validateRegistrationOpen();
	if (!open) {
		return open;
	}
	if (index >= _impl->nodes.size() || _impl->nodes[index].type != PhaseNodeType::Group) {
		return PhaseResult::failure(PhaseStatus::InvalidArgument, "invalid group");
	}
	_impl->nodes[index].hasPollInterval = true;
	_impl->nodes[index].pollIntervalMs = intervalMs;
	return PhaseResult::success("poll interval set");
}

const char *Phase::statusToString(PhaseStatus status) const {
	switch (status) {
	case PhaseStatus::Ok:
		return "Ok";
	case PhaseStatus::NotInitialized:
		return "NotInitialized";
	case PhaseStatus::AlreadyInitialized:
		return "AlreadyInitialized";
	case PhaseStatus::InvalidArgument:
		return "InvalidArgument";
	case PhaseStatus::OutOfMemory:
		return "OutOfMemory";
	case PhaseStatus::TaskCreateFailed:
		return "TaskCreateFailed";
	case PhaseStatus::TooManyNodes:
		return "TooManyNodes";
	case PhaseStatus::TooManyDependencies:
		return "TooManyDependencies";
	case PhaseStatus::DuplicateName:
		return "DuplicateName";
	case PhaseStatus::MissingDependency:
		return "MissingDependency";
	case PhaseStatus::CircularDependency:
		return "CircularDependency";
	case PhaseStatus::InvalidCallback:
		return "InvalidCallback";
	case PhaseStatus::RegistrationClosed:
		return "RegistrationClosed";
	case PhaseStatus::Busy:
		return "Busy";
	case PhaseStatus::Timeout:
		return "Timeout";
	case PhaseStatus::CallbackFailed:
		return "CallbackFailed";
	case PhaseStatus::DependencyFailed:
		return "DependencyFailed";
	case PhaseStatus::InternalError:
		return "InternalError";
	default:
		return "Unknown";
	}
}

const char *Phase::stateToString(PhaseState state) const {
	switch (state) {
	case PhaseState::Idle:
		return "Idle";
	case PhaseState::Booting:
		return "Booting";
	case PhaseState::Starting:
		return "Starting";
	case PhaseState::Ready:
		return "Ready";
	case PhaseState::Paused:
		return "Paused";
	case PhaseState::Stopping:
		return "Stopping";
	case PhaseState::Deinitializing:
		return "Deinitializing";
	case PhaseState::Stopped:
		return "Stopped";
	case PhaseState::Failed:
		return "Failed";
	case PhaseState::Ended:
		return "Ended";
	default:
		return "Unknown";
	}
}

const char *Phase::nodeTypeToString(PhaseNodeType type) const {
	switch (type) {
	case PhaseNodeType::Step:
		return "Step";
	case PhaseNodeType::Group:
		return "Group";
	case PhaseNodeType::None:
		return "None";
	default:
		return "Unknown";
	}
}
