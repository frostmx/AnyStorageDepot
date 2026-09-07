#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LogAnyStorageDepot, Log, All);

/**
 * Build and hand-craft out of any storage container in the world.
 *
 * The game already has exactly this mechanic for one building - the Dimensional Depot - and
 * the whole mod is an attempt to reuse that integration rather than re-implement it. Every
 * payment path in the game asks the central storage subsystem how much of an item is
 * available and later tells it to hand the items over; this module makes those two answers
 * also account for the contents of ordinary storage containers.
 *
 * Two things force the design:
 *
 *  - the pool has to be computed on the server. Clients never receive far away buildables at
 *    all (UFGReplicationGraph decides that, and it cannot be widened from a mod), so a client
 *    cannot count the chests itself. The server aggregates and replicates the totals.
 *
 *  - the numbers may only be inflated inside known payment paths. The same getter is used by
 *    the Depot's own upload logic, and an inflated answer there would break uploading. The
 *    hooks therefore open an explicit scope around the payment call and the effector only
 *    fires while that scope is open - a whitelist, because a blacklist of internal callers
 *    cannot be enumerated (the SDK ships no function bodies).
 */
class FAnyStorageDepotModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
