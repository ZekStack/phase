Phase::Phase() : _impl(Strata::makeUnique<PhaseImpl>(Strata::Placement::Internal)) {}

Phase::~Phase() {
	if (!_impl) return;
	bool calledFromPhaseTask = false;
	{
		PhaseLock lock(_impl->mutex);
		if (lock && _impl->initialized &&
		    _impl->task.handle() == xTaskGetCurrentTaskHandle()) {
			_impl->ending = true;
			_impl->stopRequested = true;
			_impl->startRequested = false;
			calledFromPhaseTask = true;
		}
	}
	if (calledFromPhaseTask) {
		// A Strata-owned static task must be reclaimed from another task context.
		// Destruction from a Phase callback is unsupported; release ownership here
		// to avoid freeing the stack while it is still executing.
		(void)_impl.release();
		return;
	}
	(void)end(0);
}

PhaseResult Phase::init(const PhaseConfig &config) {
	if (!_impl) return PhaseResult::failure(PhaseStatus::OutOfMemory, "phase allocation failed");
	if (!_impl->mutex) {
		return PhaseResult::failure(PhaseStatus::OutOfMemory, "phase mutex allocation failed");
	}
	if (!Strata::validMemoryPolicy(config.memory)) {
		return PhaseResult::failure(PhaseStatus::InvalidArgument, "invalid memory placement");
	}
	if (config.maxNodes == 0 || config.maxDependenciesPerNode == 0) {
		return PhaseResult::failure(PhaseStatus::InvalidArgument, "invalid phase limits");
	}
	if (!isValidStackSize(config.stackSizeBytes)) {
		return PhaseResult::failure(PhaseStatus::InvalidArgument, "invalid stack size");
	}

	TaskHandle_t taskHandle = nullptr;
	{
		PhaseLock lock(_impl->mutex);
		if (!lock) return PhaseResult::failure(PhaseStatus::InternalError, "lock failed");
		if (_impl->ending || _impl->currentState == PhaseState::Ended) {
			return PhaseResult::failure(PhaseStatus::Busy, "phase has ended");
		}
		if (_impl->initialized) {
			return PhaseResult::failure(PhaseStatus::AlreadyInitialized, "phase is already initialized");
		}

		_impl->config = config;
		_impl->initializeStorage(config.memory.allocation, config);
		_impl->registrationClosed = false;
		_impl->graphPrepared = false;
		_impl->startRequested = false;
		_impl->stopRequested = false;
		_impl->ending = false;
		_impl->taskRunning = false;
		_impl->taskExitReady = false;
		_impl->paused = false;
		_impl->pauseReason.fill('\0');
		_impl->stackHighWaterMarkBytes = 0;
		_impl->stackRegion = Strata::Region::Unknown;

		auto task = Strata::FreeRTOS::Task::create(
		    PhaseImpl::taskEntry,
		    _impl.get(),
		    Strata::FreeRTOS::TaskConfig{
		        .name = config.taskName,
		        .stackBytes = config.stackSizeBytes,
		        .stackPlacement = config.memory.taskStack,
		        .priority = config.priority,
		        .affinity = config.coreId,
		    }
		);
		if (!task) {
			return PhaseResult::failure(PhaseStatus::TaskCreateFailed, "phase task create failed");
		}
		_impl->task = std::move(task);
		_impl->stackRegion = _impl->task.stackRegion();
		_impl->initialized = true;
		_impl->currentState = PhaseState::Idle;
		taskHandle = _impl->task.handle();
	}

	if (taskHandle != nullptr) xTaskNotifyGive(taskHandle);
	const uint32_t startMs = millis();
	while (true) {
		{
			PhaseLock lock(_impl->mutex);
			if (lock && _impl->taskRunning) break;
		}
		if (millis() - startMs >= kTaskStartTimeoutMs) {
			(void)end(kTaskStartTimeoutMs);
			return PhaseResult::failure(PhaseStatus::Timeout, "phase task start timed out");
		}
		vTaskDelay(pdMS_TO_TICKS(1));
	}
	return PhaseResult::success("phase initialized");
}
