// Phase is kept as one translation unit while the implementation is split into
// internal fragments to keep the lifecycle state machine reviewable.
#include "internal/PhaseRuntimeBase.inc"
#include "internal/PhaseRuntimeLifecycle.inc"
#include "internal/PhaseBuilders.inc"
#include "internal/PhaseInit.inc"
#include "internal/PhaseControl.inc"
#include "internal/PhaseRegistration.inc"
