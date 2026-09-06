#pragma once

#include <Strata.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <utility>

namespace Strata::FreeRTOS {

struct TaskConfig {
	const char *name{"strata"};
	std::size_t stackBytes{0};
	Placement stackPlacement{Placement::Internal};
	UBaseType_t priority{1};
	BaseType_t affinity{tskNO_AFFINITY};
};

class Task {
  public:
	Task() noexcept = default;
	~Task() noexcept { reset(); }
	Task(const Task &) = delete;
	Task &operator=(const Task &) = delete;
	Task(Task &&other) noexcept { moveFrom(other); }
	Task &operator=(Task &&other) noexcept {
		if (this != &other) {
			reset();
			moveFrom(other);
		}
		return *this;
	}

	static Task create(TaskFunction_t entry, void *context, const TaskConfig &config) noexcept {
		Task task;
		TaskHandle_t handle = nullptr;
		const BaseType_t created = config.affinity == tskNO_AFFINITY
		    ? xTaskCreate(entry, config.name, static_cast<uint32_t>(config.stackBytes), context, config.priority, &handle)
		    : xTaskCreatePinnedToCore(entry, config.name, static_cast<uint32_t>(config.stackBytes), context, config.priority, &handle, config.affinity);
		if (created == pdPASS) {
			task._handle = handle;
			task._stackBytes = config.stackBytes;
			task._placement = config.stackPlacement;
			task._region = config.stackPlacement == Placement::Internal ? Region::Internal : Region::External;
		}
		return task;
	}

	void reset() noexcept {
		if (_handle != nullptr) {
			vTaskDelete(_handle);
			_handle = nullptr;
		}
	}

	TaskHandle_t handle() const noexcept { return _handle; }
	explicit operator bool() const noexcept { return _handle != nullptr; }
	std::size_t stackSizeBytes() const noexcept { return _stackBytes; }
	Placement stackPlacement() const noexcept { return _placement; }
	Region stackRegion() const noexcept { return _region; }
	std::size_t stackHighWaterMarkBytes() const noexcept {
		return _handle != nullptr ? static_cast<std::size_t>(uxTaskGetStackHighWaterMark(_handle)) : 0;
	}

  private:
	void moveFrom(Task &other) noexcept {
		_handle = std::exchange(other._handle, nullptr);
		_stackBytes = other._stackBytes;
		_placement = other._placement;
		_region = other._region;
	}

	TaskHandle_t _handle = nullptr;
	std::size_t _stackBytes = 0;
	Placement _placement = Placement::Internal;
	Region _region = Region::Unknown;
};

} // namespace Strata::FreeRTOS
