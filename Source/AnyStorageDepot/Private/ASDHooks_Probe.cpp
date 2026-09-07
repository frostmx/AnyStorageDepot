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

	/**
	 * SML escalates a funchook failure to a fatal error, which on a dedicated server means the
	 * process dies inside StartupModule. Naming each target before installing it turns that from
	 * a stack trace into a log line saying which function was refused.
	 */
	void ASDInstalling(const TCHAR* Target)
	{
		UE_LOG(LogAnyStorageDepot, Display, TEXT("installing probe: %s"), Target);
	}
}

void FASDProbe::Install()
{
	// The single entry point every hologram class goes through when the build gun validates
	// placement and cost. Deliberately not CheckCanAfford: that one is virtual, and SML refuses
	// to hook a virtual override without a sample instance to resolve the implementation from
	// (NativeHookManager.cpp:103) - at StartupModule no hologram exists yet, and the assert
	// takes the whole process down. ValidatePlacementAndCost is non-virtual and, by its own
	// comment, recurses into child holograms, so logging the concrete class here answers the
	// coverage question anyway.
	ASDInstalling(TEXT("AFGHologram::ValidatePlacementAndCost"));
	SUBSCRIBE_METHOD(AFGHologram::ValidatePlacementAndCost,
		[](auto& Scope, AFGHologram* Self, UFGInventoryComponent* Inventory)
		{
			const FASDProbe Probe(TEXT("Hologram::ValidatePlacementAndCost"), Self,
				FString::Printf(TEXT("inv=%s"), *ASDDescribe(Inventory)));
			Scope(Self, Inventory);
		});

	// The server side of building. An authoritative re-check would have to live here.
	ASDInstalling(TEXT("UFGBuildGunStateBuild::InternalConstructHologram"));
	SUBSCRIBE_METHOD(UFGBuildGunStateBuild::InternalConstructHologram,
		[](auto& Scope, UFGBuildGunStateBuild* Self, FNetConstructionID ConstructionID)
		{
			const FASDProbe Probe(TEXT("BuildGunStateBuild::InternalConstructHologram"), Self);
			Scope(Self, ConstructionID);
		});

	// The one function in the whole SDK that spends "inventory + Depot" in a single call.
	// Whether building and crafting really funnel through it is the central question.
	ASDInstalling(TEXT("UFGInventoryLibrary::GrabItemsFromInventoryAndCentralStorage"));
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
	ASDInstalling(TEXT("AFGCentralStorageSubsystem::GetNumItemsFromCentralStorage"));
	SUBSCRIBE_METHOD(AFGCentralStorageSubsystem::GetNumItemsFromCentralStorage,
		[](auto& Scope, const AFGCentralStorageSubsystem* Self, TSubclassOf<UFGItemDescriptor> ItemClass)
		{
			const int32 Result = Scope(Self, ItemClass);
			UE_LOG(LogAnyStorageDepot, Display, TEXT("  . CentralStorage::GetNumItems %s -> %d"),
				*NameOf(ItemClass), Result);
		});

	ASDInstalling(TEXT("AFGCentralStorageSubsystem::TryRemoveItemsFromCentralStorage"));
	SUBSCRIBE_METHOD(AFGCentralStorageSubsystem::TryRemoveItemsFromCentralStorage,
		[](auto& Scope, AFGCentralStorageSubsystem* Self, TSubclassOf<UFGItemDescriptor> ItemClass, const int32 NumToRemove)
		{
			const FASDProbe Probe(TEXT("CentralStorage::TryRemoveItems"), Self,
				FString::Printf(TEXT("item=%s num=%d"), *NameOf(ItemClass), NumToRemove));
			Scope(Self, ItemClass, NumToRemove);
		});

	// Does the Depot's own upload logic read the same getter? If it does, inflating that getter
	// globally would break uploading - which is why the effector is scoped.
	ASDInstalling(TEXT("AFGCentralStorageSubsystem::CanUploadInventoryItemToCentralStorage"));
	SUBSCRIBE_METHOD(AFGCentralStorageSubsystem::CanUploadInventoryItemToCentralStorage,
		[](auto& Scope, const AFGCentralStorageSubsystem* Self, const FInventoryItem& Item)
		{
			const FASDProbe Probe(TEXT("CentralStorage::CanUploadInventoryItem"), Self);
			Scope(Self, Item);
		});

	// Hand crafting.
	ASDInstalling(TEXT("UFGWorkBench::CanProduce"));
	SUBSCRIBE_METHOD(UFGWorkBench::CanProduce,
		[](auto& Scope, const UFGWorkBench* Self, TSubclassOf<UFGRecipe> Recipe, UFGInventoryComponent* Inventory)
		{
			const FASDProbe Probe(TEXT("WorkBench::CanProduce"), Self,
				FString::Printf(TEXT("recipe=%s inv=%s"), *NameOf(Recipe), *ASDDescribe(Inventory)));
			Scope(Self, Recipe, Inventory);
		});

	ASDInstalling(TEXT("UFGWorkBench::RemoveIngredientsAndAwardRewards"));
	SUBSCRIBE_METHOD(UFGWorkBench::RemoveIngredientsAndAwardRewards,
		[](auto& Scope, UFGWorkBench* Self, UFGInventoryComponent* Inventory, TSubclassOf<UFGRecipe> Recipe)
		{
			const FASDProbe Probe(TEXT("WorkBench::RemoveIngredientsAndAwardRewards"), Self,
				FString::Printf(TEXT("recipe=%s inv=%s"), *NameOf(Recipe), *ASDDescribe(Inventory)));
			Scope(Self, Inventory, Recipe);
		});

	ASDInstalling(TEXT("UFGRecipe::IsRecipeAffordable"));
	SUBSCRIBE_METHOD(UFGRecipe::IsRecipeAffordable,
		[](auto& Scope, AFGCharacterPlayer* Player, TSubclassOf<UFGRecipe> Recipe)
		{
			const bool Result = Scope(Player, Recipe);
			UE_LOG(LogAnyStorageDepot, Display, TEXT("  . Recipe::IsRecipeAffordable %s -> %d"),
				*NameOf(Recipe), Result ? 1 : 0);
		});

	ASDInstalling(TEXT("AFGRecipeManager::GetAffordableRecipesForProducer"));
	SUBSCRIBE_METHOD(AFGRecipeManager::GetAffordableRecipesForProducer,
		[](auto& Scope, AFGRecipeManager* Self, AFGCharacterPlayer* Player,
		   TSubclassOf<UObject> ForProducer, TArray<TSubclassOf<UFGRecipe>>& OutRecipes)
		{
			const FASDProbe Probe(TEXT("RecipeManager::GetAffordableRecipesForProducer"), Self,
				FString::Printf(TEXT("producer=%s"), *NameOf(ForProducer)));
			Scope(Self, Player, ForProducer, OutRecipes);
		});

	ASDInstalling(TEXT("UFGBlueprintFunctionLibrary::GetCategoriesWithAffordableRecipes"));
	SUBSCRIBE_METHOD(UFGBlueprintFunctionLibrary::GetCategoriesWithAffordableRecipes,
		[](auto& Scope, AFGCharacterPlayer* Player, TSubclassOf<UObject> ForProducer)
		{
			const FASDProbe Probe(TEXT("BPLibrary::GetCategoriesWithAffordableRecipes"), Player,
				FString::Printf(TEXT("producer=%s"), *NameOf(ForProducer)));
			Scope(Player, ForProducer);
		});

	UE_LOG(LogAnyStorageDepot, Display, TEXT("stage-1 probes installed"));
}
