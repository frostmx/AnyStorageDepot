#include "AnyStorageDepot.h"

#include "ASDProbe.h"

DEFINE_LOG_CATEGORY(LogAnyStorageDepot);

IMPLEMENT_MODULE(FAnyStorageDepotModule, AnyStorageDepot);

void FAnyStorageDepotModule::StartupModule()
{
	// Stage 1: instrumentation only, no behaviour change. The payment call chain cannot be
	// read out of the SDK (it ships empty function bodies), so it gets measured instead.
	FASDProbe::Install();
}

void FAnyStorageDepotModule::ShutdownModule()
{
}
