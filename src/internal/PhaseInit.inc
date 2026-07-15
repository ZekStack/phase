Phase::Phase() : _impl(new (std::nothrow) PhaseImpl()) {}

Phase::~Phase() {
	if (!_impl) return;
	bool calledFromPhaseTask = false;
	{
		PhaseLock lock(_impl->mutex);
		if (lock && _impl->initialized && _impl->taskHandle == xTaskGetCurrentTaskHandle()) {
			_impl->ending = true;
			_impl->stopRequested = true;
			_impl->startRequested = false;
			_impl->deleteImplOnExit = true;
			calledFromPhaseTask = true;
		}
	}
	if (calledFromPhaseTask) {
		(void)_impl.release();
		return;
	}
	(void)end(0);
}

PhaseResult Phase::init(const PhaseConfig &config) {
	if (!_impl) return PhaseResult::failure(PhaseStatus::OutOfMemory, "phase allocation failed");
	SemaphoreHandle_t taskStarted = nullptr;
	{
		PhaseLock lock(_impl->mutex);
		if (!lock) return PhaseResult::failure(PhaseStatus::InternalError, "lock failed");
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
		if (_impl->taskStarted == nullptr) _impl->taskStarted = xSemaphoreCreateBinary();
		if (_impl->taskExited == nullptr) _impl->taskExited = xSemaphoreCreateBinary();
		if (_impl->taskStarted == nullptr || _impl->taskExited == nullptr) {
			return PhaseResult::failure(PhaseStatus::OutOfMemory, "phase semaphore allocation failed");
		}
		while (xSemaphoreTake(_impl->taskStarted, 0) == pdTRUE) {}
		while (xSemaphoreTake(_impl->taskExited, 0) == pdTRUE) {}
		_impl->config = config;
		_impl->nodes.clear();
		_impl->initOrder.clear();
		_impl->startOrder.clear();
		_impl->validationMarks.clear();
		_impl->nodes.reserve(config.maxNodes);
		_impl->initOrder.reserve(config.maxNodes);
		_impl->startOrder.reserve(config.maxNodes);
		_impl->validationMarks.resize(config.maxNodes, 0);
		_impl->actualStackType = PhaseStackType::Internal;
		_impl->registrationClosed = false;
		_impl->graphPrepared = false;
		_impl->startRequested = false;
		_impl->stopRequested = false;
		_impl->ending = false;
		_impl->taskRunning = false;
		_impl->taskExitComplete = false;
		_impl->paused = false;
		_impl->deleteImplOnExit = false;
		_impl->pauseReason.fill('\0');
		bool createdWithCaps = false;
		bool usePsram = config.stackType == PhaseStackType::Psram ||
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
		_impl->actualStackType = usePsram && createdWithCaps ? PhaseStackType::Psram : PhaseStackType::Internal;
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

