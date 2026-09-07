#pragma once

#include "CoreMinimal.h"
#include "Subsystem/ModSubsystem.h"
#include "ASDStoragePool.generated.h"

class AFGBuildable;
class AFGBuildableStorage;
class UFGInventoryComponent;
class UFGItemDescriptor;

/**
 * Server-side aggregate of everything sitting in ordinary storage containers.
 *
 * Clients cannot compute this themselves: UFGReplicationGraph never sends them buildables
 * outside their relevancy bubble, and that decision is not reachable from a mod. So the server
 * counts and (from stage 3 on) replicates the totals.
 *
 * The counting is a cursor sweep rather than inventory delegates. FOnItemAdded/FOnItemRemoved
 * look like the obvious choice, but they are dynamic multicasts - one ProcessEvent per item, and
 * a container on a belt takes items dozens of times a second - and, worse, the game can silence
 * them (UFGInventoryComponent::SetSuppressOnItemAddedDelegate, FGInventoryComponent.h:538), so an
 * incremental counter would drift out of sync without ever saying so. A sweep cannot drift: it
 * re-reads the truth, just not all of it in one frame.
 */
UCLASS()
class AASDStoragePool : public AModSubsystem
{
	GENERATED_BODY()
public:
	AASDStoragePool();

	/** The pool for this world, or null before it has been spawned. */
	static AASDStoragePool* Get(const UObject* WorldContext);

	/** How many of this item the containers hold, as of the last sweep. */
	int32 GetPooled(TSubclassOf<UFGItemDescriptor> ItemClass) const;

	/** Number of containers currently indexed. */
	int32 GetContainerCount() const { return Containers.Num(); }

	/** Writes the whole pool to the log, newest count first. Debug aid for stage 2. */
	void DumpToLog() const;

	/** Rebuilds the container index from scratch and recounts everything. */
	void RebuildIndex();

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

private:
	UFUNCTION()
	void OnBuildableAdded(AFGBuildable* Buildable);

	UFUNCTION()
	void OnBuildableRemoved(AFGBuildable* Buildable);

	/** True for a plain storage container - the Depot's own uploader is excluded. */
	static bool IsPoolableContainer(const AFGBuildable* Buildable);

	/** Re-reads one container and folds the difference into the totals. */
	void RecountContainer(AFGBuildableStorage* Container);

	/** Removes a container's contribution from the totals and forgets it. */
	void ForgetContainer(const TWeakObjectPtr<AFGBuildableStorage>& Container);

	/** Containers in index order; the sweep cursor walks this array. */
	TArray<TWeakObjectPtr<AFGBuildableStorage>> Containers;

	/** What each container contributed at its last count, so a recount can apply a delta. */
	TMap<TWeakObjectPtr<AFGBuildableStorage>, TMap<TSubclassOf<UFGItemDescriptor>, int32>> PerContainer;

	/** The aggregate. Entries are dropped when they reach zero. */
	TMap<TSubclassOf<UFGItemDescriptor>, int32> Totals;

	/** Position of the sweep in Containers. */
	int32 SweepCursor = 0;

	/** Containers recounted per tick. A full pass takes Containers.Num() / this ticks. */
	int32 ContainersPerTick = 128;
};
