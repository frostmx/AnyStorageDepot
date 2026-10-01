#include "ASDResearch.h"

#include "ASDScope.h"
#include "ASDStoragePool.h"
#include "AnyStorageDepot.h"
#include "Patching/NativeHookManager.h"

#include "FGCentralStorageSubsystem.h"
#include "FGInventoryComponent.h"
#include "FGPlayerState.h"
#include "FGResearchManager.h"
#include "FGSchematic.h"
#include "GameFramework/Pawn.h"
#include "Resources/FGItemDescriptor.h"
#include "UObject/Stack.h"

// Named, not anonymous: the module is built as a unity file, and the probes' anonymous helpers
// (CostOf, NameOf) would collide with these.
namespace ASDResearch
{
	// -------------------------------------------------------------------------------------------
	// PayForResearch is private. An explicit instantiation may name a private member - the one
	// place the language lets it - and the friend it defines carries the pointer out. This is
	// what an AccessTransformers friend would give, without the eight-minute FactoryGame rebuild
	// every change to that file costs.
	// -------------------------------------------------------------------------------------------

	struct FPayForResearchTag
	{
		using Type = bool (AFGResearchManager::*)(UFGInventoryComponent*, TSubclassOf<UFGSchematic>) const;
		friend constexpr Type ASDPrivateMember(FPayForResearchTag);
	};

	template <typename Tag, typename Tag::Type Member>
	struct TASDPrivateMember
	{
		friend constexpr typename Tag::Type ASDPrivateMember(Tag) { return Member; }
	};

	template struct TASDPrivateMember<FPayForResearchTag, &AFGResearchManager::PayForResearch>;

	using FPayForResearchHook = HookInvoker<FPayForResearchTag::Type, ASDPrivateMember(FPayForResearchTag{})>;

	/** The cost of a schematic with repeated entries folded together. */
	TMap<TSubclassOf<UFGItemDescriptor>, int32> CostOf(TSubclassOf<UFGSchematic> Schematic)
	{
		TMap<TSubclassOf<UFGItemDescriptor>, int32> Cost;
		for (const FItemAmount& Amount : UFGSchematic::GetCost(Schematic))
		{
			if (Amount.ItemClass && Amount.Amount > 0)
			{
				Cost.FindOrAdd(Amount.ItemClass) += Amount.Amount;
			}
		}
		return Cost;
	}

	/**
	 * Depot plus containers, as a payment sees them: the read effector widens the Depot getter
	 * inside the payment scope. Server: the sweep totals. Client: the replicated ones.
	 */
	int32 BeyondInventory(const UObject* WorldContext, TSubclassOf<UFGItemDescriptor> ItemClass)
	{
		const FASDPaymentScope Payment;
		if (const AFGCentralStorageSubsystem* Depot = AFGCentralStorageSubsystem::Get(const_cast<UObject*>(WorldContext)))
		{
			return Depot->GetNumItemsFromCentralStorage(ItemClass);
		}
		const AASDStoragePool* Pool = AASDStoragePool::Get(WorldContext);
		return Pool ? Pool->GetPooled(ItemClass) : 0;
	}

	/** What the Depot really holds, with every widening switched off. */
	int32 InDepot(const AFGCentralStorageSubsystem* Depot, TSubclassOf<UFGItemDescriptor> ItemClass)
	{
		const FASDDepotInternalScope Internal;
		return Depot ? Depot->GetNumItemsFromCentralStorage(ItemClass) : 0;
	}

	/** The player's own "take from inventory before the Depot" setting; vanilla defaults to yes. */
	bool TakeFromInventoryFirst(const UFGInventoryComponent* Inventory)
	{
		const APawn* Pawn = Inventory ? Cast<APawn>(Inventory->GetOwner()) : nullptr;
		const AFGPlayerState* PlayerState = Pawn ? Pawn->GetPlayerState<AFGPlayerState>() : nullptr;
		return PlayerState ? PlayerState->GetTakeFromInventoryBeforeCentralStorage() : true;
	}

	/**
	 * The replacement for PayForResearch. Same contract: true means the cost has been taken and
	 * research may start, false means nothing was touched.
	 */
	bool PayForResearch(const AFGResearchManager* Manager, UFGInventoryComponent* Inventory, TSubclassOf<UFGSchematic> Schematic)
	{
		if (!Manager || !Inventory || !Schematic)
		{
			return false;
		}

		AFGCentralStorageSubsystem* Depot = AFGCentralStorageSubsystem::Get(const_cast<AFGResearchManager*>(Manager));
		AASDStoragePool* Pool = AASDStoragePool::Get(Manager);
		const TMap<TSubclassOf<UFGItemDescriptor>, int32> Cost = CostOf(Schematic);

		// Check every item before taking any. TakeFromPool is all or nothing per item, not per
		// research: without this, a three-item cost could spend the first two and then find the
		// third short. The live count is used rather than the sweep totals, and everything below
		// runs in one frame on the game thread, so nothing can change between check and payment.
		for (const TPair<TSubclassOf<UFGItemDescriptor>, int32>& Entry : Cost)
		{
			const int32 Available = Inventory->GetNumItems(Entry.Key) + InDepot(Depot, Entry.Key)
				+ (Pool ? Pool->CountLive(Entry.Key) : 0);
			if (Available < Entry.Value)
			{
				return false;
			}
		}

		const bool bInventoryFirst = TakeFromInventoryFirst(Inventory);
		for (const TPair<TSubclassOf<UFGItemDescriptor>, int32>& Entry : Cost)
		{
			int32 Remaining = Entry.Value;

			const auto FromInventory = [&]()
			{
				const int32 Take = FMath::Min(Remaining, Inventory->GetNumItems(Entry.Key));
				if (Take > 0)
				{
					Inventory->Remove(Entry.Key, Take);
					Remaining -= Take;
				}
			};
			const auto FromDepot = [&]()
			{
				// Outside the payment scope the write effector stays out of it, so this is the
				// Depot alone - the containers come last, below, and only for what is missing.
				const int32 Take = FMath::Min(Remaining, InDepot(Depot, Entry.Key));
				if (Take > 0)
				{
					Remaining -= Depot->TryRemoveItemsFromCentralStorage(Entry.Key, Take);
				}
			};

			if (bInventoryFirst)
			{
				FromInventory();
				FromDepot();
			}
			else
			{
				FromDepot();
				FromInventory();
			}

			if (Remaining > 0)
			{
				// Covered by the check above; failing here means a container changed under us
				// within the frame, which the check exists to rule out.
				if (!Pool || Pool->TakeFromPool(Entry.Key, Remaining) != Remaining)
				{
					UE_LOG(LogAnyStorageDepot, Error, TEXT("research %s: %d x %s could not be taken from containers after the check passed"),
						*Schematic->GetName(), Remaining, *Entry.Key->GetName());
				}
			}
		}

		UE_LOG(LogAnyStorageDepot, Verbose, TEXT("paid for research %s"), *Schematic->GetName());
		return true;
	}

	/** Same question as vanilla CanAffordResearch, with the Depot and the containers counted. */
	bool CanAffordWithContainers(const AFGResearchManager* Manager, UFGInventoryComponent* Inventory, TSubclassOf<UFGSchematic> Schematic)
	{
		for (const TPair<TSubclassOf<UFGItemDescriptor>, int32>& Entry : CostOf(Schematic))
		{
			const int32 InInventory = Inventory->GetNumItems(Entry.Key);
			if (InInventory < Entry.Value && InInventory + BeyondInventory(Manager, Entry.Key) < Entry.Value)
			{
				return false;
			}
		}
		return true;
	}

	/** The two MAM widgets whose item counts should include the containers. */
	bool IsMAMCostWidget(const UObject* Object)
	{
		static const FName NodeInfo(TEXT("Widget_MAMTree_NodeInfo_C"));
		static const FName HardDriveScanner(TEXT("BPW_MAM_HardDriveScanner_C"));

		for (const UClass* Class = Object ? Object->GetClass() : nullptr; Class; Class = Class->GetSuperClass())
		{
			const FName Name = Class->GetFName();
			if (Name == NodeInfo || Name == HardDriveScanner)
			{
				return true;
			}
		}
		return false;
	}

	/**
	 * True when this Blueprint call to GetNumItems comes from a MAM cost widget. The frame the
	 * thunk receives is the caller's: HUDHelpers_C::GetNumItemsFromPlayerInventory, and above it
	 * the widget function that asked. Three frames is enough to see it and keeps the walk short
	 * for every other Blueprint caller.
	 */
	bool CalledFromMAMCostWidget(const FFrame& Stack)
	{
		int32 Depth = 0;
		for (const FFrame* Frame = &Stack; Frame && Depth < 3; Frame = Frame->PreviousFrame, ++Depth)
		{
			if (IsMAMCostWidget(Frame->Object))
			{
				return true;
			}
		}
		return false;
	}
}

void FASDResearch::Install()
{
	// Server: the payment. Vanilla PayForResearch never runs - it would call Remove for items the
	// inventory does not have as soon as CanAffordResearch below says yes.
	UE_LOG(LogAnyStorageDepot, Display, TEXT("installing: AFGResearchManager::PayForResearch"));
	ASDResearch::FPayForResearchHook::InstallHook(TEXT("AFGResearchManager::PayForResearch"));
	ASDResearch::FPayForResearchHook::AddHandlerBefore(
		[](auto& Scope, const AFGResearchManager* Self, UFGInventoryComponent* Inventory, TSubclassOf<UFGSchematic> Schematic)
		{
			Scope.Override(ASDResearch::PayForResearch(Self, Inventory, Schematic));
		});

	// Both sides: the Research button. The MAM window asks this through
	// Widget_MAMTree_NodeInfo_C::Can Afford Research; on the server only the replaced
	// PayForResearch called it, so after the hook above nothing there does.
	UE_LOG(LogAnyStorageDepot, Display, TEXT("installing: AFGResearchManager::CanAffordResearch"));
	SUBSCRIBE_METHOD(AFGResearchManager::CanAffordResearch,
		[](auto& Scope, const AFGResearchManager* Self, UFGInventoryComponent* Inventory, TSubclassOf<UFGSchematic> Schematic)
		{
			const bool bVanilla = Scope(Self, Inventory, Schematic);
			if (!bVanilla && Self && Inventory && Schematic)
			{
				Scope.Override(ASDResearch::CanAffordWithContainers(Self, Inventory, Schematic));
			}
		});

	// Client: the numbers. For a MAM cost widget the thunk body is run here - it is the three
	// lines UHT generates - with the Depot and the containers added. Every other caller goes
	// through untouched. Hooking the native GetNumItems instead would put a hook on a function
	// every factory calls from its worker threads, for the sake of a window.
	UE_LOG(LogAnyStorageDepot, Display, TEXT("installing: UFGInventoryComponent::execGetNumItems"));
	SUBSCRIBE_METHOD(UFGInventoryComponent::execGetNumItems,
		[](auto& Scope, UObject* Context, FFrame& Stack, void* const Z_Param__Result)
		{
			if (!ASDResearch::CalledFromMAMCostWidget(Stack))
			{
				return;
			}

			P_GET_OBJECT(UClass, Z_Param_itemClass);
			P_FINISH;
			P_NATIVE_BEGIN;
			const UFGInventoryComponent* Inventory = CastChecked<UFGInventoryComponent>(Context);
			const TSubclassOf<UFGItemDescriptor> ItemClass = Z_Param_itemClass;
			*(int32*)Z_Param__Result = Inventory->GetNumItems(ItemClass) + ASDResearch::BeyondInventory(Inventory, ItemClass);
			P_NATIVE_END;

			Scope.Cancel();
		});

	UE_LOG(LogAnyStorageDepot, Display, TEXT("research hooks installed"));
}
