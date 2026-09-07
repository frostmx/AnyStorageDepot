#pragma once

/**
 * The mod proper: the hooks that let building and hand crafting spend out of storage containers.
 *
 * Everything hangs off the two questions the game already asks the Dimensional Depot, so no
 * payment path had to be reimplemented. Stage 1 measured the chains in the running game and both
 * end in the same pair:
 *
 *     InternalConstructHologram / RemoveIngredientsAndAwardRewards
 *       GrabItemsFromInventoryAndCentralStorage
 *         TryRemoveItemsFromCentralStorage        <- write effector
 *
 *     ValidatePlacementAndCost / CanProduce
 *         GetNumItemsFromCentralStorage           <- read effector
 *
 * The same measurement settled the one thing the design could have died on: the world under test
 * had no Depot at all, and the getter was still called hundreds of times, always returning zero.
 * So the payment paths ask unconditionally - there is no IsCentralStorageBuilt() gate to defeat,
 * which is fortunate, because that function is inline and cannot be hooked.
 */
class FASDEffectors
{
public:
	static void Install();
};
