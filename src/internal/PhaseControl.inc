PhaseResult Phase::start() {
	if (!_impl) return PhaseResult::failure(PhaseStatus::OutOfMemory, "phase allocation failed");
	{
		PhaseLock lock(_impl->mutex);
		if (!lock) return PhaseResult::failure(PhaseStatus::InternalError, "lock failed");
		if (_impl->ending) return PhaseResult::failure(PhaseStatus::Busy, "phase is ending");
		if (!_impl->initialized) return PhaseResult::failure(PhaseStatus::NotInitialized, "phase is not initialized");
		if (_impl->currentState == PhaseState::Ended) return PhaseResult::failure(PhaseStatus::Busy, "phase has ended");
		if (_impl->currentState != PhaseState::Idle && _impl->currentState != PhaseState::Stopped) {
			return PhaseResult::failure(PhaseStatus::Busy, _impl->currentState == PhaseState::Ready ? "phase is already ready" : "phase is busy");
		}
		if (!_impl->graphPrepared) {
			PhaseResult graph = _impl->prepareGraphLocked();
			if (!graph) return graph;
		}
		_impl->registrationClosed = true;
		_impl->stopRequested = false;
		_impl->startRequested = true;
	}
	return _impl->notifyTask();
}

PhaseResult Phase::stop() {
	if (!_impl) return PhaseResult::failure(PhaseStatus::OutOfMemory, "phase allocation failed");
	{
		PhaseLock lock(_impl->mutex);
		if (!lock) return PhaseResult::failure(PhaseStatus::InternalError, "lock failed");
		if (_impl->ending) return PhaseResult::failure(PhaseStatus::Busy, "phase is ending");
		if (!_impl->initialized) return PhaseResult::failure(PhaseStatus::NotInitialized, "phase is not initialized");
		if (_impl->currentState == PhaseState::Ended) return PhaseResult::failure(PhaseStatus::Busy, "phase has ended");
		if (_impl->currentState == PhaseState::Idle || _impl->currentState == PhaseState::Stopped) {
			if (_impl->startRequested) {
				_impl->startRequested = false;
				return PhaseResult::success("phase start cancelled");
			}
			return PhaseResult::success("phase stopped");
		}
		if (_impl->currentState == PhaseState::Failed) return PhaseResult::success("phase stopped");
		if (_impl->currentState == PhaseState::Stopping || _impl->currentState == PhaseState::Deinitializing) {
			return PhaseResult::success("phase stopping");
		}
		_impl->stopRequested = true;
	}
	return _impl->notifyTask();
}

PhaseResult Phase::end(uint32_t timeoutMs) {
	if (!_impl) return PhaseResult::success("phase ended");
	TaskHandle_t handle = nullptr;
	SemaphoreHandle_t taskExited = nullptr;
	{
		PhaseLock lock(_impl->mutex);
		if (!lock) return PhaseResult::failure(PhaseStatus::InternalError, "lock failed");
		if (!_impl->initialized) return PhaseResult::success("phase ended");
		if (_impl->taskHandle != nullptr && _impl->taskHandle == xTaskGetCurrentTaskHandle()) {
			return PhaseResult::failure(PhaseStatus::Busy, "end cannot be called from the Phase task");
		}
		_impl->ending = true;
		_impl->stopRequested = true;
		_impl->startRequested = false;
		handle = _impl->taskHandle;
		taskExited = _impl->taskExited;
	}
	if (handle != nullptr) xTaskNotifyGive(handle);
	const uint32_t startMs = millis();
	while (true) {
		{
			PhaseLock lock(_impl->mutex);
			if (lock && _impl->taskExitComplete) {
				_impl->initialized = false;
				_impl->currentState = PhaseState::Ended;
				return PhaseResult::success("phase ended");
			}
		}
		if (timeoutMs > 0 && millis() - startMs >= timeoutMs) {
			return PhaseResult::failure(PhaseStatus::Timeout, "phase end timed out");
		}
		const TickType_t waitTicks = timeoutMs == 0 ? pdMS_TO_TICKS(kWaitPollMs) :
		                             pdMS_TO_TICKS(std::min<uint32_t>(kWaitPollMs, timeoutMs));
		if (taskExited != nullptr) (void)xSemaphoreTake(taskExited, waitTicks);
		else vTaskDelay(waitTicks);
	}
}

PhaseResult Phase::pause(const char *reason) {
	if (!_impl) return PhaseResult::failure(PhaseStatus::OutOfMemory, "phase allocation failed");
	{
		PhaseLock lock(_impl->mutex);
		if (!lock) return PhaseResult::failure(PhaseStatus::InternalError, "lock failed");
		if (_impl->ending) return PhaseResult::failure(PhaseStatus::Busy, "phase is ending");
		if (!_impl->initialized) return PhaseResult::failure(PhaseStatus::NotInitialized, "phase is not initialized");
		if (_impl->currentState == PhaseState::Ended) return PhaseResult::failure(PhaseStatus::Busy, "phase has ended");
		_impl->paused = true;
		copyText(_impl->pauseReason.data(), _impl->pauseReason.size(), reason);
	}
	return _impl->notifyTask();
}

PhaseResult Phase::resume() {
	if (!_impl) return PhaseResult::failure(PhaseStatus::OutOfMemory, "phase allocation failed");
	{
		PhaseLock lock(_impl->mutex);
		if (!lock) return PhaseResult::failure(PhaseStatus::InternalError, "lock failed");
		if (_impl->ending) return PhaseResult::failure(PhaseStatus::Busy, "phase is ending");
		if (!_impl->initialized) return PhaseResult::failure(PhaseStatus::NotInitialized, "phase is not initialized");
		if (_impl->currentState == PhaseState::Ended) return PhaseResult::failure(PhaseStatus::Busy, "phase has ended");
		_impl->paused = false;
		_impl->pauseReason.fill('\0');
	}
	return _impl->notifyTask();
}

bool Phase::isPaused() {
	if (!_impl) return false;
	PhaseLock lock(_impl->mutex);
	return lock && _impl->paused;
}

PhaseState Phase::state() {
	if (!_impl) return PhaseState::Ended;
	PhaseLock lock(_impl->mutex);
	return lock ? _impl->currentState : PhaseState::Failed;
}

PhaseDiag Phase::getDiagnostics() {
	PhaseDiag diag;
	if (!_impl) return diag;
	PhaseLock lock(_impl->mutex);
	if (!lock) return diag;
	diag.nodeCount = _impl->nodes.size();
	for (const PhaseNode &node : _impl->nodes) {
		if (node.initialized) diag.initializedCount++;
		if (node.started) diag.startedCount++;
		if (node.ready) diag.readyCount++;
		if (node.failed) diag.failedCount++;
		if (node.skipped) diag.skippedCount++;
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
	if (!_impl) return;
	std::shared_ptr<PhaseChangeCallback> replacement;
	if (callback) replacement = std::make_shared<PhaseChangeCallback>(std::move(callback));
	PhaseLock lock(_impl->mutex);
	if (lock) _impl->changeCallback = std::move(replacement);
}

void Phase::onReady(PhaseReadyCallback callback) {
	if (!_impl) return;
	std::shared_ptr<PhaseReadyCallback> replacement;
	if (callback) replacement = std::make_shared<PhaseReadyCallback>(std::move(callback));
	PhaseLock lock(_impl->mutex);
	if (lock) _impl->readyCallback = std::move(replacement);
}

void Phase::onFailed(PhaseFailedCallback callback) {
	if (!_impl) return;
	std::shared_ptr<PhaseFailedCallback> replacement;
	if (callback) replacement = std::make_shared<PhaseFailedCallback>(std::move(callback));
	PhaseLock lock(_impl->mutex);
	if (lock) _impl->failedCallback = std::move(replacement);
}

