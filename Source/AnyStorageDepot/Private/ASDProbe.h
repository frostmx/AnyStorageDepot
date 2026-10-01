#pragma once

#include "CoreMinimal.h"

/**
 * Stage 1 instrumentation: proves who calls whom.
 *
 * The SDK in this dev environment ships headers with empty function bodies - the real code is
 * linked from the game binary - so every claim about the payment call chain ("building goes
 * through GrabItemsFromInventoryAndCentralStorage", "the craft menu asks the Depot even when
 * none is built") is an inference from signatures, not something a grep can confirm. These
 * probes change no behaviour; they log entry and exit with a nesting depth so the chain shows
 * up in the log as an indented tree.
 *
 * The whole file is expected to be deleted once the questions it answers are answered.
 */
class FASDProbe
{
public:
	/** Installs every probe. Safe to call once, from StartupModule. */
	static void Install();

	/** Logs entry on construction and exit on destruction, indented by nesting depth. */
	explicit FASDProbe(const TCHAR* InName, const UObject* Context = nullptr, const FString& Extra = FString());
	~FASDProbe();

private:
	const TCHAR* Name;
	static thread_local int32 Depth;
};

/**
 * Who the MAM window asks before research payment is changed - see ASDHooks_MAMProbe.cpp.
 * Separate from FASDProbe so it can be switched on without drowning in build-cost traffic.
 */
class FASDMAMProbe
{
public:
	/** True when -asdmamprobe was passed or Saved/AnyStorageDepot.mamprobe exists. */
	static bool IsRequested();

	/** Installs the MAM probe hooks. Safe to call once, from StartupModule. */
	static void Install();
};

/** Describes an object as "class name (server|client|no-world)" for the probe log. */
FString ASDDescribe(const UObject* Object);
