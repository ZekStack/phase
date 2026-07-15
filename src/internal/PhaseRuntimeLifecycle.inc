	PhaseResult runOneInitAction(bool &madeProgress) {
		madeProgress = false;
		for (size_t i = 0; i < nodes.size(); ++i) {
			PhaseNodeState state;
			if (!getNodeState(i, state)) continue;
			if (state.type != PhaseNodeType::Step || state.initialized || state.failed || state.skipped) continue;
			const DependencyState dependencies = dependencyState(i, true);
			if (dependencies == DependencyState::Waiting) continue;
			if (dependencies == DependencyState::SkipOptional) {
				{
					PhaseLock lock(mutex);
					if (lock) nodes[i].skipped = true;
				}
				madeProgress = true;
				emitChange(PhaseState::Booting, nodes[i].type, nodes[i].name.c_str(), "optional node skipped");
				return PhaseResult::success();
			}
			if (dependencies == DependencyState::FailedRequired) {
				{
					PhaseLock lock(mutex);
					if (lock) nodes[i].failed = true;
				}
				madeProgress = true;
				return PhaseResult::failure(PhaseStatus::DependencyFailed, "required dependency failed");
			}
			PhaseNode &node = nodes[i];
			PhaseResult result = runLifecycleCallback(
			    i,
			    node.initCallback,
			    resolveTimeout(node, kInitTimeout),
			    PhaseState::Booting,
			    "initializing step"
			);
			madeProgress = true;
			if (!result) {
				if (state.optional) return markOptionalFailure(i, result, PhaseState::Booting);
				PhaseLock lock(mutex);
				if (lock) nodes[i].failed = true;
				return result;
			}
			{
				PhaseLock lock(mutex);
				if (lock) {
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
		for (size_t i = 0; i < nodes.size(); ++i) {
			PhaseNodeState state;
			if (!getNodeState(i, state)) continue;
			if (state.ready || state.failed || state.skipped) continue;
			const DependencyState dependencies = dependencyState(i, false);
			if (dependencies == DependencyState::Waiting) continue;
			if (dependencies == DependencyState::SkipOptional) {
				{
					PhaseLock lock(mutex);
					if (lock) nodes[i].skipped = true;
				}
				madeProgress = true;
				emitChange(PhaseState::Starting, nodes[i].type, nodes[i].name.c_str(), "optional node skipped");
				return PhaseResult::success();
			}
			if (dependencies == DependencyState::FailedRequired) {
				{
					PhaseLock lock(mutex);
					if (lock) nodes[i].failed = true;
				}
				madeProgress = true;
				return PhaseResult::failure(PhaseStatus::DependencyFailed, "required dependency failed");
			}
			PhaseNode &node = nodes[i];
			if (state.type == PhaseNodeType::Group) {
				PhaseResult result = waitForGroup(i);
				madeProgress = true;
				if (result) {
					PhaseLock lock(mutex);
					if (lock) nodes[i].ready = true;
					return result;
				}
				if (state.optional) return markOptionalFailure(i, result, PhaseState::Starting);
				PhaseLock lock(mutex);
				if (lock) nodes[i].failed = true;
				return result;
			}
			if (!state.initialized) continue;
			if (!state.hasStartCallback) {
				{
					PhaseLock lock(mutex);
					if (lock) nodes[i].ready = true;
				}
				madeProgress = true;
				emitChange(PhaseState::Starting, node.type, node.name.c_str(), "step ready");
				return PhaseResult::success("step ready");
			}
			PhaseResult result = runLifecycleCallback(
			    i,
			    node.startCallback,
			    resolveTimeout(node, kStartTimeout),
			    PhaseState::Starting,
			    "starting step"
			);
			madeProgress = true;
			if (!result) {
				if (state.optional) {
					if (node.deinitCallback) {
						(void)runLifecycleCallback(
						    i,
						    node.deinitCallback,
						    resolveTimeout(node, kDeinitTimeout),
						    PhaseState::Deinitializing,
						    "deinitializing optional step",
						    true
						);
					}
					{
						PhaseLock lock(mutex);
						if (lock) nodes[i].initialized = false;
					}
					return markOptionalFailure(i, result, PhaseState::Starting);
				}
				PhaseLock lock(mutex);
				if (lock) nodes[i].failed = true;
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

	void stopStartedSteps() {
		for (auto it = startOrder.rbegin(); it != startOrder.rend(); ++it) {
			const size_t index = *it;
			PhaseNodeState state;
			if (!getNodeState(index, state) || !state.started) continue;
			PhaseNode &node = nodes[index];
			if (node.stopCallback) {
				(void)runLifecycleCallback(
				    index,
				    node.stopCallback,
				    resolveTimeout(node, kStopTimeout),
				    PhaseState::Stopping,
				    "stopping step",
				    true
				);
			}
			PhaseLock lock(mutex);
			if (lock) {
				nodes[index].started = false;
				nodes[index].ready = false;
			}
		}
		startOrder.clear();
	}

	void deinitInitializedSteps() {
		for (auto it = initOrder.rbegin(); it != initOrder.rend(); ++it) {
			const size_t index = *it;
			PhaseNodeState state;
			if (!getNodeState(index, state) || !state.initialized) continue;
			PhaseNode &node = nodes[index];
			if (node.deinitCallback) {
				(void)runLifecycleCallback(
				    index,
				    node.deinitCallback,
				    resolveTimeout(node, kDeinitTimeout),
				    PhaseState::Deinitializing,
				    "deinitializing step",
				    true
				);
			}
			PhaseLock lock(mutex);
			if (lock) {
				nodes[index].initialized = false;
				nodes[index].ready = false;
			}
		}
		initOrder.clear();
	}

	void resetGroups() {
		PhaseLock lock(mutex);
		if (!lock) return;
		for (PhaseNode &node : nodes) {
			if (node.type == PhaseNodeType::Group) node.ready = false;
		}
	}

	PhaseResult rollback() {
		{
			PhaseLock lock(mutex);
			if (lock) rollbackCount++;
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
			if (lock) stopRequested = false;
		}
		emitChange(PhaseState::Stopped, PhaseNodeType::None, nullptr, "phase stopped");
		return PhaseResult::success("phase stopped");
	}

	PhaseResult runBoot() {
		{
			PhaseLock lock(mutex);
			if (!lock) return PhaseResult::failure(PhaseStatus::InternalError, "lock failed");
			bootCount++;
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
				PhaseResult stalled = PhaseResult::failure(PhaseStatus::InternalError, "dependency graph stalled");
				rollback();
				emitChange(PhaseState::Failed, PhaseNodeType::None, nullptr, stalled.message, stalled);
				emitFailed(stalled);
				return stalled;
			}
		}
		if (shouldStop() || isEnding()) return runShutdown();
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
				PhaseResult stalled = PhaseResult::failure(PhaseStatus::InternalError, "dependency graph stalled");
				rollback();
				emitChange(PhaseState::Failed, PhaseNodeType::None, nullptr, stalled.message, stalled);
				emitFailed(stalled);
				return stalled;
			}
		}
		if (shouldStop() || isEnding()) return runShutdown();
		emitChange(PhaseState::Ready, PhaseNodeType::None, nullptr, "phase ready");
		emitReady();
		return PhaseResult::success("phase ready");
	}

	void taskLoop() {
		{
			PhaseLock lock(mutex);
			if (lock) taskRunning = true;
		}
		if (taskStarted != nullptr) xSemaphoreGive(taskStarted);
		while (!isEnding()) {
			ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
			if (isEnding()) break;
			bool localStop = false;
			bool localStart = false;
			{
				PhaseLock lock(mutex);
				if (lock) {
					localStop = stopRequested;
					localStart = startRequested;
					startRequested = false;
					if (localStart && !localStop) currentState = PhaseState::Booting;
				}
			}
			if (localStop) (void)runShutdown();
			if (localStart && !localStop) (void)runBoot();
		}
		if (shouldStop()) (void)runShutdown();
		bool localCreatedWithCaps = false;
		bool localDeleteImpl = false;
		SemaphoreHandle_t localTaskExited = nullptr;
		{
			PhaseLock lock(mutex);
			if (lock) {
				currentState = PhaseState::Ended;
				taskRunning = false;
				stackHighWaterMarkBytes = phase_task_support::currentStackHighWaterMarkBytes();
				localCreatedWithCaps = createdWithCaps;
				localDeleteImpl = deleteImplOnExit;
				localTaskExited = taskExited;
			}
		}
		if (localDeleteImpl) {
			PhaseImpl *self = this;
			delete self;
		} else {
			if (localTaskExited != nullptr) xSemaphoreGive(localTaskExited);
			{
				PhaseLock lock(mutex);
				if (lock) {
					taskExitComplete = true;
					taskHandle = nullptr;
				}
			}
		}
		phase_task_support::deleteCurrentTask(localCreatedWithCaps);
	}
};

