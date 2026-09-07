#pragma once

#include "CoreMinimal.h"
#include "Module/GameWorldModule.h"
#include "ASDGameWorldModule.generated.h"

/**
 * Registers the mod's subsystem with SML.
 *
 * SML discovers native root modules by class (FPluginModuleLoader::FindRootModulesOfType via
 * FindNativeClassesByType), so a code-only mod needs no content asset for this - only
 * bRootModule on the CDO.
 */
UCLASS()
class UASDGameWorldModule : public UGameWorldModule
{
	GENERATED_BODY()
public:
	UASDGameWorldModule();
};
