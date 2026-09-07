#include "ASDGameWorldModule.h"

#include "ASDStoragePool.h"

UASDGameWorldModule::UASDGameWorldModule()
{
	bRootModule = true;
	ModSubsystems.Add(AASDStoragePool::StaticClass());
}
