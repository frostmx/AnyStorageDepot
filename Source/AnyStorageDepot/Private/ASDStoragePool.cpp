#include "ASDStoragePool.h"

#include "AnyStorageDepot.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"

AASDStoragePool::AASDStoragePool()
{
	// The pool is authoritative on the server and mirrored to clients for their local
	// affordability checks, so it has to be a replicated subsystem rather than a local one.
	// SML flips bReplicates itself for this policy (SubsystemActorManager.cpp:30).
	ReplicationPolicy = ESubsystemReplicationPolicy::SpawnOnServer_Replicate;
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
}
