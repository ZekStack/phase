#include <Arduino.h>
#include <Phase.h>

Phase phase;

void setup() {
	Serial.begin(115200);

	PhaseConfig config;
	config.memory.allocation = Strata::Placement::PreferExternal;
	config.memory.taskStack = Strata::Placement::PreferExternal;

	PhaseResult result = phase.init(config);
	if (!result) {
		Serial.println(result.message);
		return;
	}

	phase.add("app", []() {
		Serial.println("app init");
	});

	PhaseDiag diag = phase.getDiagnostics();
	Serial.printf(
	    "allocation=%s stack-requested=%s stack-region=%s\n",
	    Strata::toString(diag.allocationPlacement),
	    Strata::toString(diag.requestedStackPlacement),
	    Strata::toString(diag.stackRegion)
	);

	phase.start();
}

void loop() {
	delay(1000);
}
