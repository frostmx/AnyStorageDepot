#pragma once

#include "CoreMinimal.h"
#include "ItemAmount.h"
#include "Subsystem/ModSubsystem.h"
#include "ASDStoragePool.generated.h"

class AFGBuildable;
class AFGBuildableStorage;
class UFGInventoryComponent;
class UFGItemDescriptor;

/**
 * What every ordinary storage container in the world holds, counted on the server and mirrored
 * to clients.
 *
 * Clients cannot count it themselves: UFGReplicationGraph never sends them buildables outside
 * their relevancy bubble, and that is not reachable from a mod. But a client still has to answer
 * "can I afford this" locally every frame while a hologram is up, so the totals are replicated.
 * The client copy is a prediction for the UI - the server arbitrates when items actually move.
 *
 * The counting is a cursor sweep, not inventory delegates. FOnItemAdded/FOnItemRemoved are
 * dynamic multicasts, so a container on a belt would cost a ProcessEvent per item, and the game
 * can silence them outright (SetSuppressOnItemAddedDelegate, FGInventoryComponent.h:538) - an
 * incremental counter would drift and never say so. A sweep re-reads the truth; it just does not
 * read all of it in one frame.
 */
UCLASS()
class AASDStoragePool : public AModSubsystem
{
	GENERATED_BODY()
public:
	AASDStoragePool();

	/** The pool for this world, or null before it has been spawned. */
	static AASDStoragePool* Get(const UObject* WorldContext);

	/** How many of this item the containers hold. Server: live. Client: last replicated. */
	int32 GetPooled(TSubclassOf<UFGItemDescriptor> ItemClass) const;

	/**
	 * Takes items out of the containers, server only.
	 *
	 * All or nothing: if the containers together cannot cover Num, nothing is removed and 0 is
	 * returned. A partial withdrawal would leave the caller having paid for something it does
	 * not get, and there is no path in the game to hand the difference back.
	 */
	int32 TakeFromPool(TSubclassOf<UFGItemDescriptor> ItemClass, int32 Num);

	int32 GetContainerCount() const { return Containers.Num(); }

	/** Writes the whole pool to the log, largest count first. */
	void DumpToLog() const;

	/** Rebuilds the container index from scratch and recounts everything. */
	void RebuildIndex();

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

private:
	UFUNCTION()
	void OnBuildableAdded(AFGBuildable* Buildable);

	UFUNCTION()
	void OnBuildableRemoved(AFGBuildable* Buildable);

	UFUNCTION()
	void OnRep_Pooled();

	/** True for a plain storage container - the Depot's own uploader is excluded. */
	static bool IsPoolableContainer(const AFGBuildable* Buildable);

	/** Re-reads one container and folds the difference into the totals. */
	void RecountContainer(AFGBuildableStorage* Container);

	/** Removes a container's contribution from the totals and forgets it. */
	void ForgetContainer(const TWeakObjectPtr<AFGBuildableStorage>& Container);

	/** Copies the totals into the replicated array. */
	void PublishTotals();

	/** Replicated view of Totals. Same shape the Depot uses for its own contents. */
	UPROPERTY(ReplicatedUsing = OnRep_Pooled)
	TArray<FItemAmount> mPooled;

	/** Containers in index order; the sweep cursor walks this array. */
	TArray<TWeakObjectPtr<AFGBuildableStorage>> Containers;

	/** What each container contributed at its last count, so a recount can apply a delta. */
	TMap<TWeakObjectPtr<AFGBuildableStorage>, TMap<TSubclassOf<UFGItemDescriptor>, int32>> PerContainer;

	/** The aggregate, server side. Entries are dropped when they reach zero. */
	TMap<TSubclassOf<UFGItemDescriptor>, int32> Totals;

	/** The aggregate as last received, client side. */
	TMap<TSubclassOf<UFGItemDescriptor>, int32> ReplicatedTotals;

	int32 SweepCursor = 0;

	/** Containers recounted per tick. A full pass takes Containers.Num() / this ticks. */
	int32 ContainersPerTick = 128;

	/** Totals changed since the last publish. */
	bool bTotalsDirty = false;

	/** Seconds since the replicated array was last refreshed. */
	float SincePublish = 0.f;

	/** Building and crafting do not need the pool at frame rate. */
	static constexpr float PublishIntervalSeconds = 0.5f;
};
