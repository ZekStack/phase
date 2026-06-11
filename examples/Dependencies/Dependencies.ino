#include <Arduino.h>
#include <Phase.h>

Phase phase;

bool initStep(const char *name) {
	Serial.printf("%s init\n", name);
	return true;
}

bool deinitStep(const char *name) {
	Serial.printf("%s deinit\n", name);
	return true;
}

bool startStep(const char *name) {
	Serial.printf("%s start\n", name);
	return true;
}

bool stopStep(const char *name) {
	Serial.printf("%s stop\n", name);
	return true;
}

void setup() {
	Serial.begin(115200);

	PhaseConfig config;
	config.taskName = "phase-deps";

	PhaseResult result = phase.init(config);
	if (!result) {
		Serial.println(result.message);
		return;
	}

	phase.add("storage", []() {
		return initStep("storage");
	}, []() {
		return deinitStep("storage");
	});

	phase.add("network", []() {
		return initStep("network");
	}, []() {
		return deinitStep("network");
	}).start([]() {
		return startStep("network");
	}, []() {
		return stopStep("network");
	});

	phase.add("time", []() {
		return initStep("time");
	}, []() {
		return deinitStep("time");
	}).depends("network");

	phase.add("database", []() {
		return initStep("database");
	}, []() {
		return deinitStep("database");
	}).depends({ "storage", "time" });

	phase.onChange([](PhaseChange change) {
		if (change.nodeName != nullptr) {
			Serial.printf("%s: %s\n", change.nodeName, change.message);
		}
	});

	phase.start();
}

void loop() {
	delay(1000);
}
