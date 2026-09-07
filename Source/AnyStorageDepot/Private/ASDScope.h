#pragma once

#include "CoreMinimal.h"

/**
 * Marks a stretch of execution as "the game is paying for something".
 *
 * The mod works by answering the Depot's own two questions differently - how much of an item is
 * available, and hand that much over. Measured in game (stage 1), every payment path funnels
 * through them:
 *
 *     InternalConstructHologram / RemoveIngredientsAndAwardRewards
 *       GrabItemsFromInventoryAndCentralStorage
 *         AFGCentralStorageSubsystem::TryRemoveItemsFromCentralStorage
 *     ValidatePlacementAndCost / CanProduce
 *         AFGCentralStorageSubsystem::GetNumItemsFromCentralStorage
 *
 * But those two are also used by the Depot's own machinery - upload limits, the Depot window,
 * stack limits - and inflating the answers there would break uploading and let players drain
 * other people's chests through the Depot UI. Which callers are internal cannot be enumerated:
 * the SDK ships no function bodies, and a game patch would add more.
 *
 * So the effectors are off by default and switched on only inside this scope, opened by the
 * hooks on the payment entry points. A whitelist is sound where a blacklist cannot be written:
 * anything the mod has not explicitly recognised as paying sees stock behaviour.
 *
 * The counter is thread_local because the scope must not leak across the worker threads the
 * engine may run alongside the game thread.
 */
class FASDPaymentScope
{
public:
	FASDPaymentScope() { ++Depth; }
	~FASDPaymentScope() { --Depth; }

	FASDPaymentScope(const FASDPaymentScope&) = delete;
	FASDPaymentScope& operator=(const FASDPaymentScope&) = delete;

	/** True while a payment path is on the stack of this thread. */
	static bool IsOpen() { return Depth > 0; }

private:
	static thread_local int32 Depth;
};
