#include "AnyStorageDepot.h"

#include "ASDEffectors.h"
#include "ASDProbe.h"
#include "ASDResearch.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

DEFINE_LOG_CATEGORY(LogAnyStorageDepot);

IMPLEMENT_MODULE(FAnyStorageDepotModule, AnyStorageDepot);

void FAnyStorageDepotModule::StartupModule()
{
#if WITH_EDITOR
	// Never hook in an editor or commandlet process. FactoryGame is built there from the SDK
	// stubs, whose bodies are empty, and funchook cannot place a trampoline in a function with no
	// instructions - it aborts the process with "Too short instructions". The cook runs in
	// UnrealEditor-Cmd.exe, so without this guard packaging the mod kills the cooker.
	UE_LOG(LogAnyStorageDepot, Display, TEXT("editor build: hooks not installed"));
#else
	FASDEffectors::Install();

	// MAM research: its own payment path, which never asks the Depot. Relies on the effectors
	// above for the widened Depot reads, so it goes second.
	FASDResearch::Install();

	// The stage-1 probes stay in the build but off: they log every cost query, which runs to tens
	// of thousands of lines in a few minutes of play. Launch with -asdprobe to get the call tree
	// back when something needs explaining.
	if (FParse::Param(FCommandLine::Get(), TEXT("asdprobe")))
	{
		FASDProbe::Install();
	}

	// The MAM probe is switched by a file as well, because the client is relaunched by the Epic
	// launcher and loses the command line. Installed only at startup: placing a hook while a
	// factory worker may be inside GetNumItems is a race this probe does not need to take.
	if (FASDMAMProbe::IsRequested())
	{
		FASDMAMProbe::Install();
	}
#endif
}

void FAnyStorageDepotModule::ShutdownModule()
{
}
