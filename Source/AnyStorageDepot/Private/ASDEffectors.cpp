#include "ASDEffectors.h"

#include "ASDScope.h"
#include "ASDStoragePool.h"
#include "AnyStorageDepot.h"
#include "Patching/NativeHookManager.h"

#include "FGBlueprintFunctionLibrary.h"
#include "FGCentralStorageSubsystem.h"
#include "FGCharacterPlayer.h"
#include "FGInventoryComponent.h"
#include "FGInventoryLibrary.h"
#include "FGRecipe.h"
#include "FGRecipeManager.h"
#include "FGWorkBench.h"
#include "Equipment/FGBuildGunBuild.h"
#include "Hologram/FGHologram.h"
#include "Resources/FGItemDescriptor.h"

namespace
{
	void Installing(const TCHAR* Target)
	{
		// SML turns a funchook refusal into a fatal error, which during StartupModule takes the
		// whole process down. Naming each target first turns that from a stack trace into a log
		// line saying which function was refused.
		UE_LOG(LogAnyStorageDepot, Display, TEXT("installing: %s"), Target);
	}
}

void FASDEffectors::Install()
{
	// ---------------------------------------------------------------------------------------
	// Scope openers. These change nothing by themselves; they mark the stretches of execution
	// during which the effectors below are allowed to answer differently. See ASDScope.h for why
	// this is a whitelist and not a blacklist.
	// ---------------------------------------------------------------------------------------

	// Building: the cost check. Non-virtual, and measured to be the single entry point for every
	// hologram class (FGFactoryHologram, FGBuildableHologram, FGStackableStorageHologram,
	// Holo_BuildingGradualBase_C, Holo_TradingPost_C all came through it), recursing into child
	// holograms with the same inventory.
	Installing(TEXT("AFGHologram::ValidatePlacementAndCost"));
	SUBSCRIBE_METHOD(AFGHologram::ValidatePlacementAndCost,
		[](auto& Scope, AFGHologram* Self, UFGInventoryComponent* Inventory)
		{
			const FASDPaymentScope Payment;
			Scope(Self, Inventory);
		});

	// Building: the server actually constructing and paying.
	Installing(TEXT("UFGBuildGunStateBuild::InternalConstructHologram"));
	SUBSCRIBE_METHOD(UFGBuildGunStateBuild::InternalConstructHologram,
		[](auto& Scope, UFGBuildGunStateBuild* Self, FNetConstructionID ConstructionID)
		{
			const FASDPaymentScope Payment;
			Scope(Self, ConstructionID);
		});

	// Hand crafting: the gate, and the payment.
	Installing(TEXT("UFGWorkBench::CanProduce"));
	SUBSCRIBE_METHOD(UFGWorkBench::CanProduce,
		[](auto& Scope, const UFGWorkBench* Self, TSubclassOf<UFGRecipe> Recipe, UFGInventoryComponent* Inventory)
		{
			const FASDPaymentScope Payment;
			Scope(Self, Recipe, Inventory);
		});

	Installing(TEXT("UFGWorkBench::RemoveIngredientsAndAwardRewards"));
	SUBSCRIBE_METHOD(UFGWorkBench::RemoveIngredientsAndAwardRewards,
		[](auto& Scope, UFGWorkBench* Self, UFGInventoryComponent* Inventory, TSubclassOf<UFGRecipe> Recipe)
		{
			const FASDPaymentScope Payment;
			Scope(Self, Inventory, Recipe);
		});

	// The craft menu: which recipes look affordable, and which categories are shown at all.
	// Without these the mod would work but the UI would still grey everything out.
	Installing(TEXT("UFGRecipe::IsRecipeAffordable"));
	SUBSCRIBE_METHOD(UFGRecipe::IsRecipeAffordable,
		[](auto& Scope, AFGCharacterPlayer* Player, TSubclassOf<UFGRecipe> Recipe)
		{
			const FASDPaymentScope Payment;
			Scope(Player, Recipe);
		});

	Installing(TEXT("AFGRecipeManager::GetAffordableRecipesForProducer"));
	SUBSCRIBE_METHOD(AFGRecipeManager::GetAffordableRecipesForProducer,
		[](auto& Scope, AFGRecipeManager* Self, AFGCharacterPlayer* Player,
		   TSubclassOf<UObject> ForProducer, TArray<TSubclassOf<UFGRecipe>>& OutRecipes)
		{
			const FASDPaymentScope Payment;
			Scope(Self, Player, ForProducer, OutRecipes);
		});

	Installing(TEXT("UFGBlueprintFunctionLibrary::GetCategoriesWithAffordableRecipes"));
	SUBSCRIBE_METHOD(UFGBlueprintFunctionLibrary::GetCategoriesWithAffordableRecipes,
		[](auto& Scope, AFGCharacterPlayer* Player, TSubclassOf<UObject> ForProducer)
		{
			const FASDPaymentScope Payment;
			Scope(Player, ForProducer);
		});

	// The one function that spends "inventory + Depot" in a single call. Both payment chains end
	// here, so the scope is opened again in case a future path reaches it another way.
	Installing(TEXT("UFGInventoryLibrary::GrabItemsFromInventoryAndCentralStorage"));
	SUBSCRIBE_METHOD(UFGInventoryLibrary::GrabItemsFromInventoryAndCentralStorage,
		[](auto& Scope, UFGInventoryComponent* Inventory, AFGCentralStorageSubsystem* CentralStorage,
		   bool bTakeFromInventoryFirst, TSubclassOf<UFGItemDescriptor> ItemClass, int32 NumToRemove)
		{
			const FASDPaymentScope Payment;
			Scope(Inventory, CentralStorage, bTakeFromInventoryFirst, ItemClass, NumToRemove);
		});

	// ---------------------------------------------------------------------------------------
	// Effectors.
	// ---------------------------------------------------------------------------------------

	// Read: how much is available. Adding the containers here is what makes a hologram go green
	// and a recipe stop being greyed out - the game's own cost checks and its cost widgets all
	// read this number, so the UI comes along for free.
	Installing(TEXT("AFGCentralStorageSubsystem::GetNumItemsFromCentralStorage"));
	SUBSCRIBE_METHOD(AFGCentralStorageSubsystem::GetNumItemsFromCentralStorage,
		[](auto& Scope, const AFGCentralStorageSubsystem* Self, TSubclassOf<UFGItemDescriptor> ItemClass)
		{
			const int32 InDepot = Scope(Self, ItemClass);
			if (!FASDPaymentScope::IsOpen())
			{
				return;
			}

			const AASDStoragePool* Pool = AASDStoragePool::Get(Self);
			if (!Pool)
			{
				return;
			}

			const int32 InContainers = Pool->GetPooled(ItemClass);
			if (InContainers > 0)
			{
				Scope.Override(InDepot + InContainers);
			}
		});

	// Write: hand the items over. The vanilla call runs first, so the real Depot is still spent
	// before any chest is touched; only the shortfall comes out of the containers.
	//
	// Reporting the total back as "removed from central storage" is what makes the rest of the
	// accounting work: GrabItemsFromInventoryAndCentralStorage subtracts this return value from
	// what it then takes out of the player's own inventory, so the items the containers supplied
	// are exactly the ones the player does not pay twice for.
	Installing(TEXT("AFGCentralStorageSubsystem::TryRemoveItemsFromCentralStorage"));
	SUBSCRIBE_METHOD(AFGCentralStorageSubsystem::TryRemoveItemsFromCentralStorage,
		[](auto& Scope, AFGCentralStorageSubsystem* Self, TSubclassOf<UFGItemDescriptor> ItemClass, const int32 NumToRemove)
		{
			const int32 FromDepot = Scope(Self, ItemClass, NumToRemove);
			if (!FASDPaymentScope::IsOpen())
			{
				return;
			}

			const int32 Shortfall = NumToRemove - FromDepot;
			if (Shortfall <= 0)
			{
				return;
			}

			AASDStoragePool* Pool = AASDStoragePool::Get(Self);
			if (!Pool)
			{
				return;
			}

			// All or nothing, and server only - TakeFromPool enforces both.
			const int32 FromContainers = Pool->TakeFromPool(ItemClass, Shortfall);
			if (FromContainers > 0)
			{
				Scope.Override(FromDepot + FromContainers);
			}
		});

	UE_LOG(LogAnyStorageDepot, Display, TEXT("effectors installed"));
}
