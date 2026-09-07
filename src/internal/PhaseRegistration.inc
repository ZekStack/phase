PhaseStepBuilder Phase::addStep(const char *name, PhaseCallback initCallback, PhaseCallback deinitCallback) {
	if (!_impl) {
		return PhaseStepBuilder(this, 0, PhaseResult::failure(PhaseStatus::OutOfMemory, "phase allocation failed"));
	}
	PhaseLock lock(_impl->mutex);
	if (!lock) return PhaseStepBuilder(this, 0, PhaseResult::failure(PhaseStatus::InternalError, "lock failed"));
	PhaseResult open = _impl->validateRegistrationOpen();
	if (!open) return PhaseStepBuilder(this, 0, open);
	if (name == nullptr || name[0] == '\0') {
		return PhaseStepBuilder(this, 0, PhaseResult::failure(PhaseStatus::InvalidArgument, "node name is required"));
	}
	if (!initCallback) {
		return PhaseStepBuilder(this, 0, PhaseResult::failure(PhaseStatus::InvalidCallback, "init callback is required"));
	}
	if (_impl->nodes.size() >= _impl->config.maxNodes) {
		return PhaseStepBuilder(this, 0, PhaseResult::failure(PhaseStatus::TooManyNodes, "too many nodes"));
	}
	if (_impl->findNodeIndex(name) < _impl->nodes.size()) {
		return PhaseStepBuilder(this, 0, PhaseResult::failure(PhaseStatus::DuplicateName, "duplicate node name"));
	}
	PhaseNode node(_impl->config.memory.allocation);
	node.type = PhaseNodeType::Step;
	node.name = name;
	node.dependencyNames.reserve(_impl->config.maxDependenciesPerNode);
	node.dependencies.reserve(_impl->config.maxDependenciesPerNode);
	node.initCallback = std::move(initCallback);
	node.deinitCallback = std::move(deinitCallback);
	_impl->nodes.push_back(std::move(node));
	_impl->graphPrepared = false;
	return PhaseStepBuilder(this, _impl->nodes.size() - 1, PhaseResult::success("step added"));
}

PhaseGroupBuilder Phase::addGroup(const char *name) {
	if (!_impl) {
		return PhaseGroupBuilder(this, 0, PhaseResult::failure(PhaseStatus::OutOfMemory, "phase allocation failed"));
	}
	PhaseLock lock(_impl->mutex);
	if (!lock) return PhaseGroupBuilder(this, 0, PhaseResult::failure(PhaseStatus::InternalError, "lock failed"));
	PhaseResult open = _impl->validateRegistrationOpen();
	if (!open) return PhaseGroupBuilder(this, 0, open);
	if (name == nullptr || name[0] == '\0') {
		return PhaseGroupBuilder(this, 0, PhaseResult::failure(PhaseStatus::InvalidArgument, "node name is required"));
	}
	if (_impl->nodes.size() >= _impl->config.maxNodes) {
		return PhaseGroupBuilder(this, 0, PhaseResult::failure(PhaseStatus::TooManyNodes, "too many nodes"));
	}
	if (_impl->findNodeIndex(name) < _impl->nodes.size()) {
		return PhaseGroupBuilder(this, 0, PhaseResult::failure(PhaseStatus::DuplicateName, "duplicate node name"));
	}
	PhaseNode node(_impl->config.memory.allocation);
	node.type = PhaseNodeType::Group;
	node.name = name;
	node.dependencyNames.reserve(_impl->config.maxDependenciesPerNode);
	node.dependencies.reserve(_impl->config.maxDependenciesPerNode);
	_impl->nodes.push_back(std::move(node));
	_impl->graphPrepared = false;
	return PhaseGroupBuilder(this, _impl->nodes.size() - 1, PhaseResult::success("group added"));
}

PhaseResult Phase::addDependency(size_t index, const char *name) {
	if (!_impl) return PhaseResult::failure(PhaseStatus::OutOfMemory, "phase allocation failed");
	PhaseLock lock(_impl->mutex);
	if (!lock) return PhaseResult::failure(PhaseStatus::InternalError, "lock failed");
	PhaseResult open = _impl->validateRegistrationOpen();
	if (!open) return open;
	if (index >= _impl->nodes.size()) return PhaseResult::failure(PhaseStatus::InvalidArgument, "invalid node");
	if (name == nullptr || name[0] == '\0') {
		return PhaseResult::failure(PhaseStatus::InvalidArgument, "dependency name is required");
	}
	PhaseNode &node = _impl->nodes[index];
	if (node.dependencyNames.size() >= _impl->config.maxDependenciesPerNode) {
		return PhaseResult::failure(PhaseStatus::TooManyDependencies, "too many dependencies");
	}
	if (std::find(node.dependencyNames.begin(), node.dependencyNames.end(), name) == node.dependencyNames.end()) {
		Strata::String dependency{Strata::Allocator<char>{node.allocationPlacement}};
		dependency = name;
		node.dependencyNames.push_back(std::move(dependency));
		_impl->graphPrepared = false;
	}
	return PhaseResult::success("dependency added");
}

PhaseResult Phase::setOptional(size_t index) {
	if (!_impl) return PhaseResult::failure(PhaseStatus::OutOfMemory, "phase allocation failed");
	PhaseLock lock(_impl->mutex);
	if (!lock) return PhaseResult::failure(PhaseStatus::InternalError, "lock failed");
	PhaseResult open = _impl->validateRegistrationOpen();
	if (!open) return open;
	if (index >= _impl->nodes.size()) return PhaseResult::failure(PhaseStatus::InvalidArgument, "invalid node");
	_impl->nodes[index].optional = true;
	return PhaseResult::success("node optional");
}

PhaseResult Phase::setStepStartCallbacks(size_t index, PhaseCallback startCallback, PhaseCallback stopCallback) {
	if (!_impl) return PhaseResult::failure(PhaseStatus::OutOfMemory, "phase allocation failed");
	PhaseLock lock(_impl->mutex);
	if (!lock) return PhaseResult::failure(PhaseStatus::InternalError, "lock failed");
	PhaseResult open = _impl->validateRegistrationOpen();
	if (!open) return open;
	if (index >= _impl->nodes.size() || _impl->nodes[index].type != PhaseNodeType::Step) {
		return PhaseResult::failure(PhaseStatus::InvalidArgument, "invalid step");
	}
	if (!startCallback) return PhaseResult::failure(PhaseStatus::InvalidCallback, "start callback is required");
	_impl->nodes[index].startCallback = std::move(startCallback);
	_impl->nodes[index].stopCallback = std::move(stopCallback);
	return PhaseResult::success("start callback added");
}

PhaseResult Phase::setStepTimeout(size_t index, uint8_t timeoutKind, uint32_t timeoutMs) {
	if (!_impl) return PhaseResult::failure(PhaseStatus::OutOfMemory, "phase allocation failed");
	PhaseLock lock(_impl->mutex);
	if (!lock) return PhaseResult::failure(PhaseStatus::InternalError, "lock failed");
	PhaseResult open = _impl->validateRegistrationOpen();
	if (!open) return open;
	if (index >= _impl->nodes.size() || _impl->nodes[index].type != PhaseNodeType::Step) {
		return PhaseResult::failure(PhaseStatus::InvalidArgument, "invalid step");
	}
	PhaseNode &node = _impl->nodes[index];
	switch (timeoutKind) {
	case kInitTimeout: node.hasInitTimeout = true; node.initTimeoutMs = timeoutMs; break;
	case kStartTimeout: node.hasStartTimeout = true; node.startTimeoutMs = timeoutMs; break;
	case kStopTimeout: node.hasStopTimeout = true; node.stopTimeoutMs = timeoutMs; break;
	case kDeinitTimeout: node.hasDeinitTimeout = true; node.deinitTimeoutMs = timeoutMs; break;
	default: return PhaseResult::failure(PhaseStatus::InvalidArgument, "invalid timeout kind");
	}
	return PhaseResult::success("timeout set");
}

PhaseResult Phase::setGroupCondition(
    size_t index,
    PhaseConditionCallback callback,
    uint32_t timeoutMs,
    bool hasTimeoutOverride
) {
	if (!_impl) return PhaseResult::failure(PhaseStatus::OutOfMemory, "phase allocation failed");
	PhaseLock lock(_impl->mutex);
	if (!lock) return PhaseResult::failure(PhaseStatus::InternalError, "lock failed");
	PhaseResult open = _impl->validateRegistrationOpen();
	if (!open) return open;
	if (index >= _impl->nodes.size() || _impl->nodes[index].type != PhaseNodeType::Group) {
		return PhaseResult::failure(PhaseStatus::InvalidArgument, "invalid group");
	}
	if (!callback) return PhaseResult::failure(PhaseStatus::InvalidCallback, "condition callback is required");
	PhaseNode &node = _impl->nodes[index];
	node.conditionCallback = std::move(callback);
	node.hasGroupTimeout = hasTimeoutOverride;
	node.groupTimeoutMs = timeoutMs;
	return PhaseResult::success("condition set");
}

PhaseResult Phase::setGroupPollInterval(size_t index, uint32_t intervalMs) {
	if (!_impl) return PhaseResult::failure(PhaseStatus::OutOfMemory, "phase allocation failed");
	PhaseLock lock(_impl->mutex);
	if (!lock) return PhaseResult::failure(PhaseStatus::InternalError, "lock failed");
	PhaseResult open = _impl->validateRegistrationOpen();
	if (!open) return open;
	if (index >= _impl->nodes.size() || _impl->nodes[index].type != PhaseNodeType::Group) {
		return PhaseResult::failure(PhaseStatus::InvalidArgument, "invalid group");
	}
	_impl->nodes[index].hasPollInterval = true;
	_impl->nodes[index].pollIntervalMs = intervalMs;
	return PhaseResult::success("poll interval set");
}

const char *Phase::statusToString(PhaseStatus status) const {
	switch (status) {
	case PhaseStatus::Ok: return "Ok";
	case PhaseStatus::NotInitialized: return "NotInitialized";
	case PhaseStatus::AlreadyInitialized: return "AlreadyInitialized";
	case PhaseStatus::InvalidArgument: return "InvalidArgument";
	case PhaseStatus::OutOfMemory: return "OutOfMemory";
	case PhaseStatus::TaskCreateFailed: return "TaskCreateFailed";
	case PhaseStatus::TooManyNodes: return "TooManyNodes";
	case PhaseStatus::TooManyDependencies: return "TooManyDependencies";
	case PhaseStatus::DuplicateName: return "DuplicateName";
	case PhaseStatus::MissingDependency: return "MissingDependency";
	case PhaseStatus::CircularDependency: return "CircularDependency";
	case PhaseStatus::InvalidCallback: return "InvalidCallback";
	case PhaseStatus::RegistrationClosed: return "RegistrationClosed";
	case PhaseStatus::Busy: return "Busy";
	case PhaseStatus::Timeout: return "Timeout";
	case PhaseStatus::CallbackFailed: return "CallbackFailed";
	case PhaseStatus::DependencyFailed: return "DependencyFailed";
	case PhaseStatus::InternalError: return "InternalError";
	default: return "Unknown";
	}
}

const char *Phase::stateToString(PhaseState state) const {
	switch (state) {
	case PhaseState::Idle: return "Idle";
	case PhaseState::Booting: return "Booting";
	case PhaseState::Starting: return "Starting";
	case PhaseState::Ready: return "Ready";
	case PhaseState::Paused: return "Paused";
	case PhaseState::Stopping: return "Stopping";
	case PhaseState::Deinitializing: return "Deinitializing";
	case PhaseState::Stopped: return "Stopped";
	case PhaseState::Failed: return "Failed";
	case PhaseState::Ended: return "Ended";
	default: return "Unknown";
	}
}

const char *Phase::nodeTypeToString(PhaseNodeType type) const {
	switch (type) {
	case PhaseNodeType::Step: return "Step";
	case PhaseNodeType::Group: return "Group";
	case PhaseNodeType::None: return "None";
	default: return "Unknown";
	}
}
