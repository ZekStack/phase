#include <Arduino.h>
#include <Phase.h>

Phase phase;

void setup() {
	Serial.begin(115200);

	PhaseResult result = phase.init();
	if (!result) {
		Serial.println(result.message);
		return;
	}

	phase.add("network", []() {
		Serial.println("network init");
		return true;
	});

	phase.addGroup("internet")
	    .depends("network")
	    .condition([]() {
		    return false;
	    }, 500)
	    .optional();

	phase.add("telemetry", []() {
		Serial.println("telemetry should be skipped");
		return true;
	}).depends("internet")
	    .optional();

	phase.add("local-ui", []() {
		Serial.println("local ui still starts");
		return true;
	});

	phase.onChange([](PhaseChange change) {
		if (change.hasError) {
			Serial.printf("optional error: %s\n", change.message);
		}
	});

	phase.onReady([]() {
		Serial.println("ready with optional internet unavailable");
	});

	phase.start();
}

void loop() {
	delay(1000);
}
