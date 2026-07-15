PhaseResult PhaseResult::success(const char *message) {
	return PhaseResult{true, PhaseStatus::Ok, message != nullptr ? message : "ok"};
}

PhaseResult PhaseResult::failure(PhaseStatus status, const char *message) {
	return PhaseResult{false, status, message != nullptr ? message : "error"};
}

PhaseStepBuilder::PhaseStepBuilder(Phase *phase, size_t index, PhaseResult result)
    : _phase(phase), _index(index), _result(result) {}

PhaseStepBuilder &PhaseStepBuilder::depends(const char *name) {
	if (_result && _phase != nullptr) _result = _phase->addDependency(_index, name);
	return *this;
}

PhaseStepBuilder &PhaseStepBuilder::depends(std::initializer_list<const char *> names) {
	for (const char *name : names) {
		depends(name);
		if (!_result) break;
	}
	return *this;
}

PhaseStepBuilder &PhaseStepBuilder::optional() {
	if (_result && _phase != nullptr) _result = _phase->setOptional(_index);
	return *this;
}

PhaseStepBuilder &PhaseStepBuilder::initTimeout(uint32_t timeoutMs) {
	if (_result && _phase != nullptr) _result = _phase->setStepTimeout(_index, kInitTimeout, timeoutMs);
	return *this;
}

PhaseStepBuilder &PhaseStepBuilder::startTimeout(uint32_t timeoutMs) {
	if (_result && _phase != nullptr) _result = _phase->setStepTimeout(_index, kStartTimeout, timeoutMs);
	return *this;
}

PhaseStepBuilder &PhaseStepBuilder::stopTimeout(uint32_t timeoutMs) {
	if (_result && _phase != nullptr) _result = _phase->setStepTimeout(_index, kStopTimeout, timeoutMs);
	return *this;
}

PhaseStepBuilder &PhaseStepBuilder::deinitTimeout(uint32_t timeoutMs) {
	if (_result && _phase != nullptr) _result = _phase->setStepTimeout(_index, kDeinitTimeout, timeoutMs);
	return *this;
}

PhaseResult PhaseStepBuilder::setStartCallbacks(PhaseCallback startCallback, PhaseCallback stopCallback) {
	if (_phase == nullptr) return PhaseResult::failure(PhaseStatus::InternalError, "builder is not attached");
	return _phase->setStepStartCallbacks(_index, std::move(startCallback), std::move(stopCallback));
}

PhaseGroupBuilder::PhaseGroupBuilder(Phase *phase, size_t index, PhaseResult result)
    : _phase(phase), _index(index), _result(result) {}

PhaseGroupBuilder &PhaseGroupBuilder::depends(const char *name) {
	if (_result && _phase != nullptr) _result = _phase->addDependency(_index, name);
	return *this;
}

PhaseGroupBuilder &PhaseGroupBuilder::depends(std::initializer_list<const char *> names) {
	for (const char *name : names) {
		depends(name);
		if (!_result) break;
	}
	return *this;
}

PhaseGroupBuilder &PhaseGroupBuilder::optional() {
	if (_result && _phase != nullptr) _result = _phase->setOptional(_index);
	return *this;
}

PhaseGroupBuilder &PhaseGroupBuilder::condition(PhaseConditionCallback callback) {
	return condition(std::move(callback), 0);
}

PhaseGroupBuilder &PhaseGroupBuilder::condition(PhaseConditionCallback callback, uint32_t timeoutMs) {
	if (_result && _phase != nullptr) {
		_result = _phase->setGroupCondition(_index, std::move(callback), timeoutMs, timeoutMs > 0);
	}
	return *this;
}

PhaseGroupBuilder &PhaseGroupBuilder::conditionPollInterval(uint32_t intervalMs) {
	if (_result && _phase != nullptr) _result = _phase->setGroupPollInterval(_index, intervalMs);
	return *this;
}

