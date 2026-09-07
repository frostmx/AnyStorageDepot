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

/**
 * Marks a stretch of execution as "the Depot is doing its own bookkeeping".
 *
 * The read effector is on by default rather than confined to the payment scope, and this is what
 * keeps that safe. The reason for the asymmetry is measured, not assumed: in a real client
 * session the Depot getter was called 3855 times inside a payment path and 17409 times outside
 * one, across 708 frames, for exactly the item classes the craft menu lists. The cost widgets ask
 * the Depot themselves, and there is nothing to hang a scope on at their end - the whole cost and
 * crafting UI is Blueprint, with no C++ class to hook. On the server the same log shows zero
 * out-of-scope reads, so widening the read changes nothing where items actually move.
 *
 * So the read is wide and the write stays narrow: TryRemoveItemsFromCentralStorage is still gated
 * on the payment scope. Even if some caller we never anticipated sees an inflated number, the
 * worst it can do is show a wrong figure or misjudge an upload - it cannot move items out of
 * anyone's containers.
 *
 * What must not see the inflated number is the Depot's own machinery: upload limits, stack limits
 * and its window would all misread chests as Depot contents. Those callers, unlike the UI, are a
 * short list that can be named straight out of FGCentralStorageSubsystem.h, and each one gets a
 * hook that opens this scope for the duration.
 */
class FASDDepotInternalScope
{
public:
	FASDDepotInternalScope() { ++Depth; }
	~FASDDepotInternalScope() { --Depth; }

	FASDDepotInternalScope(const FASDDepotInternalScope&) = delete;
	FASDDepotInternalScope& operator=(const FASDDepotInternalScope&) = delete;

	/** True while one of the Depot's own operations is on the stack of this thread. */
	static bool IsOpen() { return Depth > 0; }

private:
	static thread_local int32 Depth;
};
