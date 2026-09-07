#include "ASDProbe.h"

#include "AnyStorageDepot.h"
#include "Patching/NativeHookManager.h"

#include "FGBlueprintFunctionLibrary.h"
#include "FGCharacterPlayer.h"
#include "FGCentralStorageSubsystem.h"
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
	FString NameOf(const UClass* Class)
	{
		return Class ? Class->GetName() : FString(TEXT("<null>"));
	}
}

/**
 * Declared a friend of AFGHologram through Config/AccessTransformers.ini, which is the only
 * way to take a pointer to its protected CheckCanAfford.
 */
class FASDHookAccess
{
public:
	static void InstallHologramProbes()
	{
		// Non-virtual, and the single entry point every hologram class goes through.
		SUBSCRIBE_METHOD(AFGHologram::ValidatePlacementAndCost,
			[](auto& Scope, AFGHologram* Self, UFGInventoryComponent* Inventory)
			{
				const FASDProbe Probe(TEXT("Hologram::ValidatePlacementAndCost"), Self,
					FString::Printf(TEXT("inv=%s"), *ASDDescribe(Inventory)));
				Scope(Self, Inventory);
			});

		// Virtual and overridden in subclasses. If this never fires for a conveyor or a
		// blueprint hologram, the overrides do not call Super and the base address is not a
		// usable seam - exactly what stage 1 is here to find out.
		SUBSCRIBE_METHOD(AFGHologram::CheckCanAfford,
			[](auto& Scope, AFGHologram* Self, UFGInventoryComponent* Inventory)
			{
				const FASDProbe Probe(TEXT("Hologram::CheckCanAfford"), Self,
					FString::Printf(TEXT("inv=%s"), *ASDDescribe(Inventory)));
				Scope(Self, Inventory);
			});
	}
};

void FASDProbe::Install()
{
	FASDHookAccess::InstallHologramProbes();

	// The server side of building. An authoritative re-check would have to live here.
	SUBSCRIBE_METHOD(UFGBuildGunStateBuild::InternalConstructHologram,
		[](auto& Scope, UFGBuildGunStateBuild* Self, FNetConstructionID ConstructionID)
		{
			const FASDProbe Probe(TEXT("BuildGunStateBuild::InternalConstructHologram"), Self);
			Scope(Self, ConstructionID);
		});

	// The one function in the whole SDK that spends "inventory + Depot" in a single call.
	// Whether building and crafting really funnel through it is the central question.
	SUBSCRIBE_METHOD(UFGInventoryLibrary::GrabItemsFromInventoryAndCentralStorage,
		[](auto& Scope, UFGInventoryComponent* Inventory, AFGCentralStorageSubsystem* CentralStorage,
		   bool bTakeFromInventoryFirst, TSubclassOf<UFGItemDescriptor> ItemClass, int32 NumToRemove)
		{
			const FASDProbe Probe(TEXT("InventoryLibrary::GrabItemsFromInventoryAndCentralStorage"), Inventory,
				FString::Printf(TEXT("item=%s num=%d invFirst=%d depot=%s"),
					*NameOf(ItemClass), NumToRemove, bTakeFromInventoryFirst ? 1 : 0,
					CentralStorage ? TEXT("yes") : TEXT("null")));
			Scope(Inventory, CentralStorage, bTakeFromInventoryFirst, ItemClass, NumToRemove);
		});

	// Reading the Depot. If this never fires while no Depot is built, the payment paths gate on
	// the inlined IsCentralStorageBuilt() and the mod needs its fallback mode.
	SUBSCRIBE_METHOD(AFGCentralStorageSubsystem::GetNumItemsFromCentralStorage,
		[](auto& Scope, const AFGCentralStorageSubsystem* Self, TSubclassOf<UFGItemDescriptor> ItemClass)
		{
			const int32 Result = Scope(Self, ItemClass);
			UE_LOG(LogAnyStorageDepot, Display, TEXT("  . CentralStorage::GetNumItems %s -> %d"),
				*NameOf(ItemClass), Result);
		});

	SUBSCRIBE_METHOD(AFGCentralStorageSubsystem::TryRemoveItemsFromCentralStorage,
		[](auto& Scope, AFGCentralStorageSubsystem* Self, TSubclassOf<UFGItemDescriptor> ItemClass, const int32 NumToRemove)
		{
			const FASDProbe Probe(TEXT("CentralStorage::TryRemoveItems"), Self,
				FString::Printf(TEXT("item=%s num=%d"), *NameOf(ItemClass), NumToRemove));
			Scope(Self, ItemClass, NumToRemove);
		});

	// Does the Depot's own upload logic read the same getter? If it does, inflating that getter
	// globally would break uploading - which is why the effector is scoped.
	SUBSCRIBE_METHOD(AFGCentralStorageSubsystem::CanUploadInventoryItemToCentralStorage,
		[](auto& Scope, const AFGCentralStorageSubsystem* Self, const FInventoryItem& Item)
		{
			const FASDProbe Probe(TEXT("CentralStorage::CanUploadInventoryItem"), Self);
			Scope(Self, Item);
		});

	// Hand crafting.
	SUBSCRIBE_METHOD(UFGWorkBench::CanProduce,
		[](auto& Scope, const UFGWorkBench* Self, TSubclassOf<UFGRecipe> Recipe, UFGInventoryComponent* Inventory)
		{
			const FASDProbe Probe(TEXT("WorkBench::CanProduce"), Self,
				FString::Printf(TEXT("recipe=%s inv=%s"), *NameOf(Recipe), *ASDDescribe(Inventory)));
			Scope(Self, Recipe, Inventory);
		});

	SUBSCRIBE_METHOD(UFGWorkBench::RemoveIngredientsAndAwardRewards,
		[](auto& Scope, UFGWorkBench* Self, UFGInventoryComponent* Inventory, TSubclassOf<UFGRecipe> Recipe)
		{
			const FASDProbe Probe(TEXT("WorkBench::RemoveIngredientsAndAwardRewards"), Self,
				FString::Printf(TEXT("recipe=%s inv=%s"), *NameOf(Recipe), *ASDDescribe(Inventory)));
			Scope(Self, Inventory, Recipe);
		});

	SUBSCRIBE_METHOD(UFGRecipe::IsRecipeAffordable,
		[](auto& Scope, AFGCharacterPlayer* Player, TSubclassOf<UFGRecipe> Recipe)
		{
			const bool Result = Scope(Player, Recipe);
			UE_LOG(LogAnyStorageDepot, Display, TEXT("  . Recipe::IsRecipeAffordable %s -> %d"),
				*NameOf(Recipe), Result ? 1 : 0);
		});

	SUBSCRIBE_METHOD(AFGRecipeManager::GetAffordableRecipesForProducer,
		[](auto& Scope, AFGRecipeManager* Self, AFGCharacterPlayer* Player,
		   TSubclassOf<UObject> ForProducer, TArray<TSubclassOf<UFGRecipe>>& OutRecipes)
		{
			const FASDProbe Probe(TEXT("RecipeManager::GetAffordableRecipesForProducer"), Self,
				FString::Printf(TEXT("producer=%s"), *NameOf(ForProducer)));
			Scope(Self, Player, ForProducer, OutRecipes);
		});

	SUBSCRIBE_METHOD(UFGBlueprintFunctionLibrary::GetCategoriesWithAffordableRecipes,
		[](auto& Scope, AFGCharacterPlayer* Player, TSubclassOf<UObject> ForProducer)
		{
			const FASDProbe Probe(TEXT("BPLibrary::GetCategoriesWithAffordableRecipes"), Player,
				FString::Printf(TEXT("producer=%s"), *NameOf(ForProducer)));
			Scope(Player, ForProducer);
		});

	UE_LOG(LogAnyStorageDepot, Display, TEXT("stage-1 probes installed"));
}
