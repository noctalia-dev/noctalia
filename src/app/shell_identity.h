#pragma once

#include "core/application_identity.h"

// The shell's library identity: ~/.config/noctalia, noctalia.log, NOCTALIA_* environment variables.
inline ApplicationIdentity shellApplicationIdentity() { return {.name = "noctalia", .envPrefix = "NOCTALIA"}; }
