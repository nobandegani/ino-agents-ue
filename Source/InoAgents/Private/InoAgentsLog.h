// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "Logging/LogMacros.h"

/**
 * Shared log category for the InoAgents runtime module.
 *
 * Declared EXTERN here so every TU in the module can log under the same
 * category name. The single definition lives in InoAgents.cpp via
 * DEFINE_LOG_CATEGORY(LogInoAgents).
 *
 * This header is Private because LogInoAgents is an implementation detail.
 * External modules should not tie themselves to our log category — they
 * should use their own categories or the global LogTemp.
 */
DECLARE_LOG_CATEGORY_EXTERN(LogInoAgents, Log, All);
