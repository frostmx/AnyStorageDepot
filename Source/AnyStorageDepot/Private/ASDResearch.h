#pragma once

/**
 * MAM research paid out of storage containers.
 *
 * Research never goes near the Depot, so none of the build and craft effectors reach it. Read
 * off the disassembly of both game binaries:
 *
 *     Server_InitiateResearch -> AFGResearchManager::InitiateResearch
 *       PayForResearch(pawn inventory)              private
 *         CanAffordResearch -> HasItems per cost entry
 *         UFGInventoryComponent::Remove per cost entry
 *
 * And measured with the MAM probe (ASDHooks_MAMProbe.cpp) in the running client, the window:
 *
 *     Widget_MAMTree_NodeInfo_C::UpdateState
 *       Can Research -> Can Afford Research -> AFGResearchManager::CanAffordResearch   (button)
 *       HUDHelpers_C::GetNumItemsFromPlayerInventory -> GetNumItems                    (numbers)
 *     BPW_MAM_HardDriveScanner_C::UpdateCostIcon
 *       HUDHelpers_C::GetNumItemsFromPlayerInventory -> GetNumItems                    (numbers)
 *
 * So three hooks: CanAffordResearch counts the containers too, PayForResearch is replaced with
 * a payment that can spend them, and the count the MAM widgets draw is widened - for those two
 * widgets only, because the same helper feeds the workbench recipe buttons, which already add
 * the Depot themselves and would count every chest twice.
 *
 * Spending order is the one building uses: the player's inventory and the Depot in the order
 * the player chose, the containers last and only for what is still missing.
 */
class FASDResearch
{
public:
	static void Install();
};
