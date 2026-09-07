#include "AnyStorageDepot.h"

#include "ASDProbe.h"

DEFINE_LOG_CATEGORY(LogAnyStorageDepot);

IMPLEMENT_MODULE(FAnyStorageDepotModule, AnyStorageDepot);

void FAnyStorageDepotModule::StartupModule()
{
#if WITH_EDITOR
	// Never hook in an editor or commandlet process. FactoryGame is built there from the SDK
	// stubs, whose bodies are empty, and funchook cannot place a trampoline in a function that
	// has no instructions - it aborts the process with "Too short instructions". The cook runs
	// in UnrealEditor-Cmd.exe, so without this guard packaging the mod kills the cooker.
	UE_LOG(LogAnyStorageDepot, Display, TEXT("editor build: hooks not installed"));
#else
	// Stage 1: instrumentation only, no behaviour change. The payment call chain cannot be read
	// out of the SDK (it ships empty function bodies), so it gets measured in the real game.
	FASDProbe::Install();
#endif
}

void FAnyStorageDepotModule::ShutdownModule()
{
}
