#pragma once

#include "CoreMinimal.h"
#include "Subsystem/ModSubsystem.h"
#include "ASDStoragePool.generated.h"

class UFGItemDescriptor;

/**
 * Server-side aggregate of everything sitting in ordinary storage containers, replicated to
 * clients so their build gun and craft menu can answer "can I afford this" locally.
 *
 * Clients cannot compute this themselves: UFGReplicationGraph never sends them buildables
 * outside their relevancy bubble, and that decision is not reachable from a mod.
 *
 * Stage 0 is deliberately empty - it only proves the subsystem is registered and spawned on
 * both sides before any behaviour is attached to it.
 */
UCLASS()
class AASDStoragePool : public AModSubsystem
{
	GENERATED_BODY()
public:
	AASDStoragePool();

	/** The pool for this world, or null before it has been spawned. */
	static AASDStoragePool* Get(const UObject* WorldContext);

protected:
	virtual void BeginPlay() override;
};
