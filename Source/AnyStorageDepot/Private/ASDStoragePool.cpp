#include "ASDStoragePool.h"

#include "AnyStorageDepot.h"
#include "Buildables/FGBuildableStorage.h"
#include "Buildables/FGCentralStorageContainer.h"
#include "Engine/World.h"
#include "FGBuildableSubsystem.h"
#include "FGInventoryComponent.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "Resources/FGItemDescriptor.h"

AASDStoragePool::AASDStoragePool()
{
	// The pool is authoritative on the server and mirrored to clients for their local
	// affordability checks, so it has to be a replicated subsystem rather than a local one.
	// SML flips bReplicates itself for this policy (SubsystemActorManager.cpp:30).
	ReplicationPolicy = ESubsystemReplicationPolicy::SpawnOnServer_Replicate;

	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;
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

	// Counting only ever happens where the containers actually exist.
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
	// AFGCentralStorageContainer is the only subclass of AFGBuildableStorage in the game, and
	// it is the Depot's own upload buffer - counting it would double-count what the Depot
	// already reports through its own getter.
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

	AFGBuildableSubsystem* Buildables = AFGBuildableSubsystem::Get(GetWorld());
	if (!Buildables)
	{
		return;
	}

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
}

void AASDStoragePool::OnBuildableRemoved(AFGBuildable* Buildable)
{
	if (AFGBuildableStorage* Storage = Cast<AFGBuildableStorage>(Buildable))
	{
		ForgetContainer(Storage);
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

	// Apply the difference against what this container contributed last time, so the totals
	// stay right no matter how long ago that was.
	TMap<TSubclassOf<UFGItemDescriptor>, int32>& Cached = PerContainer.FindOrAdd(Container);
	for (const TPair<TSubclassOf<UFGItemDescriptor>, int32>& Old : Cached)
	{
		const int32 NewCount = Fresh.FindRef(Old.Key);
		if (const int32 Delta = NewCount - Old.Value; Delta != 0)
		{
			int32& Total = Totals.FindOrAdd(Old.Key);
			Total += Delta;
			if (Total <= 0)
			{
				Totals.Remove(Old.Key);
			}
		}
	}
	for (const TPair<TSubclassOf<UFGItemDescriptor>, int32>& New : Fresh)
	{
		if (!Cached.Contains(New.Key))
		{
			Totals.FindOrAdd(New.Key) += New.Value;
		}
	}

	Cached = MoveTemp(Fresh);
}

void AASDStoragePool::ForgetContainer(const TWeakObjectPtr<AFGBuildableStorage>& Container)
{
	TMap<TSubclassOf<UFGItemDescriptor>, int32> Cached;
	if (!PerContainer.RemoveAndCopyValue(Container, Cached))
	{
		return;
	}

	for (const TPair<TSubclassOf<UFGItemDescriptor>, int32>& Entry : Cached)
	{
		if (int32* Total = Totals.Find(Entry.Key))
		{
			*Total -= Entry.Value;
			if (*Total <= 0)
			{
				Totals.Remove(Entry.Key);
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

	if (Containers.Num() == 0)
	{
		return;
	}

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
}

int32 AASDStoragePool::GetPooled(TSubclassOf<UFGItemDescriptor> ItemClass) const
{
	return ItemClass ? Totals.FindRef(ItemClass) : 0;
}

void AASDStoragePool::DumpToLog() const
{
	UE_LOG(LogAnyStorageDepot, Display, TEXT("pool: %d container(s), %d item type(s), cursor %d"),
		Containers.Num(), Totals.Num(), SweepCursor);

	TArray<TPair<TSubclassOf<UFGItemDescriptor>, int32>> Sorted;
	Sorted.Reserve(Totals.Num());
	for (const TPair<TSubclassOf<UFGItemDescriptor>, int32>& Entry : Totals)
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
	TEXT("Logs what AnyStorageDepot currently counts in the world's storage containers."),
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
