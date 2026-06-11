#include <Arduino.h>
#include <Phase.h>

Phase phase;
uint32_t bootStartedAtMs = 0;

bool networkReady() {
	return millis() - bootStartedAtMs > 500;
}

bool internetReady() {
	return millis() - bootStartedAtMs > 1000;
}

void setup() {
	Serial.begin(115200);
	bootStartedAtMs = millis();

	PhaseConfig config;
	config.conditionPollIntervalMs = 100;

	PhaseResult result = phase.init(config);
	if (!result) {
		Serial.println(result.message);
		return;
	}

	phase.add("network", []() {
		Serial.println("network init");
		return true;
	}).start([]() {
		Serial.println("network start");
		return true;
	});

	phase.addGroup("link-up")
	    .depends("network")
	    .condition(networkReady, 3000)
	    .conditionPollInterval(100);

	phase.addGroup("internet")
	    .depends("link-up")
	    .condition(internetReady, 5000)
	    .conditionPollInterval(100);

	phase.add("cloud", []() {
		Serial.println("cloud init after internet");
		return true;
	}).depends("internet");

	phase.onReady([]() {
		Serial.println("groups ready");
	});

	phase.start();
}

void loop() {
	delay(1000);
}
