#include "ASDStoragePool.h"

#include "AnyStorageDepot.h"
#include "Buildables/FGBuildableStorage.h"
#include "Buildables/FGCentralStorageContainer.h"
#include "Engine/World.h"
#include "FGBuildableSubsystem.h"
#include "FGInventoryComponent.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "Net/UnrealNetwork.h"
#include "Resources/FGItemDescriptor.h"

AASDStoragePool::AASDStoragePool()
{
	// SML flips bReplicates itself for this policy (SubsystemActorManager.cpp:30).
	ReplicationPolicy = ESubsystemReplicationPolicy::SpawnOnServer_Replicate;

	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;
}

void AASDStoragePool::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AASDStoragePool, mPooled);
}

AASDStoragePool* AASDStoragePool::Get(const UObject* WorldContext)
{
	const UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContext, EGetWorldErrorMode::ReturnNull) : nullptr;
	if (!World)
	{
		return nullptr;
	}
	return Cast<AASDStoragePool>(UGameplayStatics::GetActorOfClass(const_cast<UWorld*>(World), AASDStoragePool::StaticClass()));
}

void AASDStoragePool::BeginPlay()
{
	Super::BeginPlay();

	UE_LOG(LogAnyStorageDepot, Display, TEXT("storage pool ready (authority=%d, netmode=%d)"),
		HasAuthority() ? 1 : 0, static_cast<int32>(GetNetMode()));

	// Counting only happens where the containers actually exist.
	if (!HasAuthority())
	{
		return;
	}

	if (AFGBuildableSubsystem* Buildables = AFGBuildableSubsystem::Get(GetWorld()))
	{
		Buildables->mBuildableAddedDelegate.AddDynamic(this, &AASDStoragePool::OnBuildableAdded);
		Buildables->mBuildableRemovedDelegate.AddDynamic(this, &AASDStoragePool::OnBuildableRemoved);
	}

	RebuildIndex();
	SetActorTickEnabled(true);
}

bool AASDStoragePool::IsPoolableContainer(const AFGBuildable* Buildable)
{
	// AFGCentralStorageContainer is the only subclass of AFGBuildableStorage in the game, and it
	// is the Depot's own upload buffer - counting it would double what the Depot already reports.
	return Buildable
		&& Buildable->IsA<AFGBuildableStorage>()
		&& !Buildable->IsA<AFGCentralStorageContainer>();
}

void AASDStoragePool::RebuildIndex()
{
	Containers.Reset();
	PerContainer.Reset();
	Totals.Reset();
	SweepCursor = 0;

	if (AFGBuildableSubsystem* Buildables = AFGBuildableSubsystem::Get(GetWorld()))
	{
		TArray<AFGBuildableStorage*> Found;
		Buildables->GetTypedBuildable<AFGBuildableStorage>(Found);
		for (AFGBuildableStorage* Storage : Found)
		{
			if (IsPoolableContainer(Storage))
			{
				Containers.Add(Storage);
				RecountContainer(Storage);
			}
		}
	}

	bTotalsDirty = true;
	UE_LOG(LogAnyStorageDepot, Display, TEXT("indexed %d storage container(s), %d item type(s) pooled"),
		Containers.Num(), Totals.Num());
}

void AASDStoragePool::OnBuildableAdded(AFGBuildable* Buildable)
{
	if (!IsPoolableContainer(Buildable))
	{
		return;
	}

	AFGBuildableStorage* Storage = Cast<AFGBuildableStorage>(Buildable);
	Containers.AddUnique(Storage);
	RecountContainer(Storage);

	UE_LOG(LogAnyStorageDepot, Display, TEXT("container added: %s (%d indexed)"),
		*Storage->GetName(), Containers.Num());
}

void AASDStoragePool::OnBuildableRemoved(AFGBuildable* Buildable)
{
	AFGBuildableStorage* Storage = Cast<AFGBuildableStorage>(Buildable);
	if (Storage && PerContainer.Contains(Storage))
	{
		ForgetContainer(Storage);
		UE_LOG(LogAnyStorageDepot, Display, TEXT("container removed: %s (%d indexed)"),
			*Storage->GetName(), Containers.Num());
	}
}

void AASDStoragePool::RecountContainer(AFGBuildableStorage* Container)
{
	if (!IsValid(Container))
	{
		return;
	}

	TMap<TSubclassOf<UFGItemDescriptor>, int32> Fresh;
	if (const UFGInventoryComponent* Inventory = Container->GetStorageInventory())
	{
		const int32 Slots = Inventory->GetSizeLinear();
		for (int32 Index = 0; Index < Slots; ++Index)
		{
			FInventoryStack Stack;
			if (Inventory->GetStackFromIndex(Index, Stack) && Stack.HasItems())
			{
				Fresh.FindOrAdd(Stack.Item.GetItemClass()) += Stack.NumItems;
			}
		}
	}

	// Apply the difference against what this container contributed last time, so the totals stay
	// right no matter how long ago that was.
	TMap<TSubclassOf<UFGItemDescriptor>, int32>& Cached = PerContainer.FindOrAdd(Container);
	for (const TPair<TSubclassOf<UFGItemDescriptor>, int32>& Old : Cached)
	{
		const int32 Delta = Fresh.FindRef(Old.Key) - Old.Value;
		if (Delta != 0)
		{
			int32& Total = Totals.FindOrAdd(Old.Key);
			Total += Delta;
			if (Total <= 0)
			{
				Totals.Remove(Old.Key);
			}
			bTotalsDirty = true;
		}
	}
	for (const TPair<TSubclassOf<UFGItemDescriptor>, int32>& New : Fresh)
	{
		if (!Cached.Contains(New.Key))
		{
			Totals.FindOrAdd(New.Key) += New.Value;
			bTotalsDirty = true;
		}
	}

	Cached = MoveTemp(Fresh);
}

void AASDStoragePool::ForgetContainer(const TWeakObjectPtr<AFGBuildableStorage>& Container)
{
	TMap<TSubclassOf<UFGItemDescriptor>, int32> Cached;
	if (PerContainer.RemoveAndCopyValue(Container, Cached))
	{
		for (const TPair<TSubclassOf<UFGItemDescriptor>, int32>& Entry : Cached)
		{
			if (int32* Total = Totals.Find(Entry.Key))
			{
				*Total -= Entry.Value;
				if (*Total <= 0)
				{
					Totals.Remove(Entry.Key);
				}
				bTotalsDirty = true;
			}
		}
	}

	Containers.RemoveAllSwap([&Container](const TWeakObjectPtr<AFGBuildableStorage>& Weak)
	{
		return Weak == Container || !Weak.IsValid();
	});
}

void AASDStoragePool::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	const int32 Steps = FMath::Min(ContainersPerTick, Containers.Num());
	for (int32 Step = 0; Step < Steps; ++Step)
	{
		if (SweepCursor >= Containers.Num())
		{
			SweepCursor = 0;
		}

		const TWeakObjectPtr<AFGBuildableStorage> Weak = Containers[SweepCursor];
		if (AFGBuildableStorage* Storage = Weak.Get())
		{
			RecountContainer(Storage);
			++SweepCursor;
		}
		else
		{
			// Dismantled without the delegate reaching us, or garbage collected.
			ForgetContainer(Weak);
		}
	}

	SincePublish += DeltaSeconds;
	if (bTotalsDirty && SincePublish >= PublishIntervalSeconds)
	{
		PublishTotals();
		SincePublish = 0.f;
		bTotalsDirty = false;
	}
}

void AASDStoragePool::PublishTotals()
{
	mPooled.Reset();
	mPooled.Reserve(Totals.Num());
	for (const TPair<TSubclassOf<UFGItemDescriptor>, int32>& Entry : Totals)
	{
		mPooled.Emplace(Entry.Key, Entry.Value);
	}
}

void AASDStoragePool::OnRep_Pooled()
{
	ReplicatedTotals.Reset();
	for (const FItemAmount& Entry : mPooled)
	{
		if (Entry.ItemClass)
		{
			ReplicatedTotals.Add(Entry.ItemClass, Entry.Amount);
		}
	}
}

int32 AASDStoragePool::GetPooled(TSubclassOf<UFGItemDescriptor> ItemClass) const
{
	if (!ItemClass)
	{
		return 0;
	}
	return HasAuthority() ? Totals.FindRef(ItemClass) : ReplicatedTotals.FindRef(ItemClass);
}

int32 AASDStoragePool::TakeFromPool(TSubclassOf<UFGItemDescriptor> ItemClass, int32 Num)
{
	if (!HasAuthority() || !ItemClass || Num <= 0)
	{
		return 0;
	}

	// Plan first, apply second. Both halves run in the same frame on the game thread, so nothing
	// can change in between - and if the plan does not cover Num, nothing is touched at all. A
	// partial withdrawal would leave the caller having paid for something it does not get, and
	// there is no path in the game to hand the difference back.
	struct FCandidate
	{
		UFGInventoryComponent* Inventory;
		int32 Available;
	};

	TArray<FCandidate> Candidates;
	for (const TWeakObjectPtr<AFGBuildableStorage>& Weak : Containers)
	{
		AFGBuildableStorage* Storage = Weak.Get();
		if (!IsValid(Storage))
		{
			continue;
		}
		if (UFGInventoryComponent* Inventory = Storage->GetStorageInventory())
		{
			const int32 Available = Inventory->GetNumItems(ItemClass);
			if (Available > 0)
			{
				Candidates.Add({Inventory, Available});
			}
		}
	}

	// Emptiest container first, so small leftovers get cleared out whole and the big stores that
	// feed a factory are the last thing touched.
	Candidates.Sort([](const FCandidate& A, const FCandidate& B) { return A.Available < B.Available; });

	TArray<TPair<UFGInventoryComponent*, int32>> Plan;
	int32 Remaining = Num;
	for (const FCandidate& Candidate : Candidates)
	{
		if (Remaining <= 0)
		{
			break;
		}
		const int32 Take = FMath::Min(Candidate.Available, Remaining);
		Plan.Emplace(Candidate.Inventory, Take);
		Remaining -= Take;
	}

	if (Remaining > 0)
	{
		return 0;
	}

	for (const TPair<UFGInventoryComponent*, int32>& Step : Plan)
	{
		Step.Key->Remove(ItemClass, Step.Value);
	}

	// The sweep would catch up on its own, but not before the caller asks again.
	for (const TWeakObjectPtr<AFGBuildableStorage>& Weak : Containers)
	{
		AFGBuildableStorage* Storage = Weak.Get();
		if (!IsValid(Storage))
		{
			continue;
		}
		const UFGInventoryComponent* Inventory = Storage->GetStorageInventory();
		const bool bTouched = Plan.ContainsByPredicate(
			[Inventory](const TPair<UFGInventoryComponent*, int32>& Step) { return Step.Key == Inventory; });
		if (bTouched)
		{
			RecountContainer(Storage);
		}
	}

	UE_LOG(LogAnyStorageDepot, Verbose, TEXT("took %d x %s from %d container(s)"),
		Num, *ItemClass->GetName(), Plan.Num());
	return Num;
}

void AASDStoragePool::DumpToLog() const
{
	const TMap<TSubclassOf<UFGItemDescriptor>, int32>& Source = HasAuthority() ? Totals : ReplicatedTotals;

	UE_LOG(LogAnyStorageDepot, Display, TEXT("pool: %d container(s), %d item type(s), authority=%d"),
		Containers.Num(), Source.Num(), HasAuthority() ? 1 : 0);

	TArray<TPair<TSubclassOf<UFGItemDescriptor>, int32>> Sorted;
	Sorted.Reserve(Source.Num());
	for (const TPair<TSubclassOf<UFGItemDescriptor>, int32>& Entry : Source)
	{
		Sorted.Add(Entry);
	}
	Sorted.Sort([](const TPair<TSubclassOf<UFGItemDescriptor>, int32>& A,
	               const TPair<TSubclassOf<UFGItemDescriptor>, int32>& B)
	{
		return A.Value > B.Value;
	});

	for (const TPair<TSubclassOf<UFGItemDescriptor>, int32>& Entry : Sorted)
	{
		UE_LOG(LogAnyStorageDepot, Display, TEXT("  %6d  %s"),
			Entry.Value, Entry.Key ? *Entry.Key->GetName() : TEXT("<null>"));
	}
}

static FAutoConsoleCommandWithWorld GDumpPoolCommand(
	TEXT("ASD.DumpPool"),
	TEXT("Logs what AnyStorageDepot counts in the world's storage containers."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		if (const AASDStoragePool* Pool = AASDStoragePool::Get(World))
		{
			Pool->DumpToLog();
		}
		else
		{
			UE_LOG(LogAnyStorageDepot, Warning, TEXT("no storage pool in this world"));
		}
	}));

static FAutoConsoleCommandWithWorld GRebuildPoolCommand(
	TEXT("ASD.RebuildPool"),
	TEXT("Rebuilds the AnyStorageDepot container index from scratch."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		if (AASDStoragePool* Pool = AASDStoragePool::Get(World))
		{
			Pool->RebuildIndex();
		}
	}));
