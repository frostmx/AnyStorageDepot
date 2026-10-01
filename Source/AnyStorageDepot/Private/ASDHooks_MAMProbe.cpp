#include "ASDProbe.h"

#include "AnyStorageDepot.h"
#include "Patching/NativeHookManager.h"

#include "FGCentralStorageSubsystem.h"
#include "FGInventoryComponent.h"
#include "FGPlayerController.h"
#include "FGResearchManager.h"
#include "FGResearchTree.h"
#include "FGSchematic.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Resources/FGItemDescriptor.h"
#include "UObject/Stack.h"

/*
 * MAM probe: answers one question before research payment is touched - what does the MAM window
 * read to draw its cost numbers and to enable its Research button?
 *
 * Disassembly already settled the server side: InitiateResearch -> PayForResearch ->
 * CanAffordResearch / UFGInventoryComponent::Remove, all on the pawn's own inventory, with no
 * Depot call anywhere. The window itself is Blueprint, and Blueprint cannot be read from here.
 *
 * So the probe hooks the exec thunks - the glue a Blueprint call goes through to reach a native
 * function. A thunk receives the CALLER's script frame, which names the widget class and the
 * Blueprint function asking. The native hooks below then log arguments and result under that
 * caller. Each distinct "function <- caller chain" is logged once when first seen, and
 * ASD.MAMProbeDump prints the whole table with call counts and sample arguments.
 *
 * Installed only when Saved/AnyStorageDepot.mamprobe exists (or -asdmamprobe is passed): the
 * client is relaunched by the Epic launcher, which drops our command line, so a file is the only
 * switch that reaches it. Even installed, it records nothing until ASD.MAMProbe 1.
 */

namespace
{
	int32 GMAMProbeEnabled = 0;
	FAutoConsoleVariableRef CVarMAMProbe(
		TEXT("ASD.MAMProbe"), GMAMProbeEnabled,
		TEXT("1 records which Blueprint callers ask about research cost and item counts (probe build only)."));

	/** The script frame of the Blueprint call currently inside one of the hooked thunks. */
	thread_local const FFrame* GBPFrame = nullptr;

	/** Points GBPFrame at a frame for the duration; null hides the caller from nested natives. */
	struct FBPFrameScope
	{
		const FFrame* Previous;
		explicit FBPFrameScope(const FFrame* Frame) : Previous(GBPFrame) { GBPFrame = Frame; }
		~FBPFrameScope() { GBPFrame = Previous; }
	};

	struct FCallerEntry
	{
		int64 Count = 0;
		TArray<FString> Samples;
	};

	FCriticalSection GTableLock;
	TMap<FString, FCallerEntry> GTable;

	constexpr int32 MaxSamplesPerKey = 24;
	constexpr int32 MaxChainDepth = 5;

	FString MamNameOf(const UClass* Class)
	{
		return Class ? Class->GetName() : FString(TEXT("<null>"));
	}

	FString OwnerOf(const UActorComponent* Component)
	{
		const AActor* Owner = Component ? Component->GetOwner() : nullptr;
		return Owner ? Owner->GetClass()->GetName() : FString(TEXT("<null>"));
	}

	/** "WidgetClass::Function < OuterWidget::Function < ..." for the Blueprint stack, or <native>. */
	FString CallerChain()
	{
		if (!GBPFrame)
		{
			return TEXT("<native>");
		}

		FString Chain;
		int32 Depth = 0;
		for (const FFrame* Frame = GBPFrame; Frame && Depth < MaxChainDepth; Frame = Frame->PreviousFrame, ++Depth)
		{
			if (!Chain.IsEmpty())
			{
				Chain += TEXT(" < ");
			}
			Chain += FString::Printf(TEXT("%s::%s"),
				Frame->Object ? *Frame->Object->GetClass()->GetName() : TEXT("<null>"),
				Frame->Node ? *Frame->Node->GetName() : TEXT("<null>"));
		}
		return Chain;
	}

	void Record(const TCHAR* Function, const FString& Chain, const FString& Sample)
	{
		const FString Key = FString::Printf(TEXT("%s <- %s"), Function, *Chain);

		FScopeLock Lock(&GTableLock);
		FCallerEntry& Entry = GTable.FindOrAdd(Key);
		if (Entry.Count++ == 0)
		{
			UE_LOG(LogAnyStorageDepot, Display, TEXT("[mam] new: %s  | %s"), *Key, *Sample);
		}
		if (Entry.Samples.Num() < MaxSamplesPerKey && !Entry.Samples.Contains(Sample))
		{
			Entry.Samples.Add(Sample);
		}
	}

	FString CostOf(TSubclassOf<UFGSchematic> Schematic)
	{
		const FBPFrameScope Hidden(nullptr);
		FString Text;
		for (const FItemAmount& Amount : UFGSchematic::GetCost(Schematic))
		{
			Text += FString::Printf(TEXT("%s%s x%d"), Text.IsEmpty() ? TEXT("") : TEXT(", "),
				*MamNameOf(Amount.ItemClass), Amount.Amount);
		}
		return Text;
	}

	void DumpTable()
	{
		FScopeLock Lock(&GTableLock);

		TArray<FString> Keys;
		GTable.GetKeys(Keys);
		Keys.Sort();

		UE_LOG(LogAnyStorageDepot, Display, TEXT("[mam] ---- %d callers ----"), Keys.Num());
		for (const FString& Key : Keys)
		{
			const FCallerEntry& Entry = GTable[Key];
			UE_LOG(LogAnyStorageDepot, Display, TEXT("[mam] %8lld  %s"), Entry.Count, *Key);
			for (const FString& Sample : Entry.Samples)
			{
				UE_LOG(LogAnyStorageDepot, Display, TEXT("[mam]             %s"), *Sample);
			}
		}
		UE_LOG(LogAnyStorageDepot, Display, TEXT("[mam] ---- end ----"));
	}

	FAutoConsoleCommand GMAMProbeDump(
		TEXT("ASD.MAMProbeDump"),
		TEXT("Logs every caller the MAM probe has seen, with call counts and sample arguments."),
		FConsoleCommandDelegate::CreateStatic(&DumpTable));

	FAutoConsoleCommand GMAMProbeReset(
		TEXT("ASD.MAMProbeReset"),
		TEXT("Forgets every caller the MAM probe has seen."),
		FConsoleCommandDelegate::CreateStatic([]()
		{
			FScopeLock Lock(&GTableLock);
			GTable.Reset();
			UE_LOG(LogAnyStorageDepot, Display, TEXT("[mam] table cleared"));
		}));

	void MamInstalling(const TCHAR* Target)
	{
		UE_LOG(LogAnyStorageDepot, Display, TEXT("installing MAM probe: %s"), Target);
	}
}

bool FASDMAMProbe::IsRequested()
{
	return FParse::Param(FCommandLine::Get(), TEXT("asdmamprobe"))
		|| FPaths::FileExists(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AnyStorageDepot.mamprobe")));
}

void FASDMAMProbe::Install()
{
	// ---------------------------------------------------------------------------------------
	// Exec thunks: remember who in Blueprint is asking. They change nothing else.
	// ---------------------------------------------------------------------------------------

#define ASD_THUNK(Thunk) \
	MamInstalling(TEXT(#Thunk)); \
	SUBSCRIBE_METHOD(Thunk, [](auto& Scope, UObject* Context, FFrame& Stack, void* const Result) \
	{ \
		const FBPFrameScope Frame(&Stack); \
		Scope(Context, Stack, Result); \
	})

	ASD_THUNK(UFGInventoryComponent::execGetNumItems);
	ASD_THUNK(UFGInventoryComponent::execHasItems);
	ASD_THUNK(AFGCentralStorageSubsystem::execGetNumItemsFromCentralStorage);
	ASD_THUNK(AFGResearchManager::execCanAffordResearch);
	ASD_THUNK(AFGResearchManager::execCanResearchBeInitiated);
	ASD_THUNK(AFGResearchManager::execInitiateResearch);
	ASD_THUNK(UFGSchematic::execGetCost);

#undef ASD_THUNK

	// ---------------------------------------------------------------------------------------
	// Natives: log arguments and result under the caller.
	//
	// Each one takes the caller chain first and then runs the original with the frame hidden:
	// CanAffordResearch calls GetCost and HasItems itself, and those nested calls must not be
	// booked under the Blueprint that asked for CanAffordResearch.
	//
	// Item counts are recorded only when a Blueprint asked. Native callers of GetNumItems include
	// every factory on its worker threads, and none of them is what the MAM window shows.
	// ---------------------------------------------------------------------------------------

	MamInstalling(TEXT("UFGInventoryComponent::GetNumItems"));
	SUBSCRIBE_METHOD(UFGInventoryComponent::GetNumItems,
		[](auto& Scope, const UFGInventoryComponent* Self, TSubclassOf<UFGItemDescriptor> ItemClass)
		{
			if (!GMAMProbeEnabled || !GBPFrame)
			{
				return;
			}
			const FString Chain = CallerChain();
			int32 Result;
			{
				const FBPFrameScope Hidden(nullptr);
				Result = Scope(Self, ItemClass);
			}
			Record(TEXT("Inventory::GetNumItems"), Chain, FString::Printf(TEXT("owner=%s item=%s -> %d"),
				*OwnerOf(Self), *MamNameOf(ItemClass), Result));
		});

	MamInstalling(TEXT("UFGInventoryComponent::HasItems"));
	SUBSCRIBE_METHOD(UFGInventoryComponent::HasItems,
		[](auto& Scope, const UFGInventoryComponent* Self, TSubclassOf<UFGItemDescriptor> ItemClass, int32 Num)
		{
			if (!GMAMProbeEnabled || !GBPFrame)
			{
				return;
			}
			const FString Chain = CallerChain();
			bool Result;
			{
				const FBPFrameScope Hidden(nullptr);
				Result = Scope(Self, ItemClass, Num);
			}
			Record(TEXT("Inventory::HasItems"), Chain, FString::Printf(TEXT("owner=%s item=%s num=%d -> %d"),
				*OwnerOf(Self), *MamNameOf(ItemClass), Num, Result ? 1 : 0));
		});

	MamInstalling(TEXT("AFGCentralStorageSubsystem::GetNumItemsFromCentralStorage"));
	SUBSCRIBE_METHOD(AFGCentralStorageSubsystem::GetNumItemsFromCentralStorage,
		[](auto& Scope, const AFGCentralStorageSubsystem* Self, TSubclassOf<UFGItemDescriptor> ItemClass)
		{
			if (!GMAMProbeEnabled || !GBPFrame)
			{
				return;
			}
			const FString Chain = CallerChain();
			int32 Result;
			{
				const FBPFrameScope Hidden(nullptr);
				Result = Scope(Self, ItemClass);
			}
			Record(TEXT("CentralStorage::GetNumItems"), Chain, FString::Printf(TEXT("item=%s -> %d"),
				*MamNameOf(ItemClass), Result));
		});

	// Research itself is recorded from native callers too: that is how PayForResearch shows up
	// calling CanAffordResearch, and how a C++ caller nobody expected would show up.
	MamInstalling(TEXT("AFGResearchManager::CanAffordResearch"));
	SUBSCRIBE_METHOD(AFGResearchManager::CanAffordResearch,
		[](auto& Scope, const AFGResearchManager* Self, UFGInventoryComponent* Inventory, TSubclassOf<UFGSchematic> Schematic)
		{
			if (!GMAMProbeEnabled)
			{
				return;
			}
			const FString Chain = CallerChain();
			bool Result;
			{
				const FBPFrameScope Hidden(nullptr);
				Result = Scope(Self, Inventory, Schematic);
			}
			Record(TEXT("Research::CanAffordResearch"), Chain, FString::Printf(TEXT("%s inv=%s schematic=%s cost=[%s] -> %d"),
				*ASDDescribe(Self), *OwnerOf(Inventory), *MamNameOf(Schematic), *CostOf(Schematic), Result ? 1 : 0));
		});

	MamInstalling(TEXT("AFGResearchManager::CanResearchBeInitiated"));
	SUBSCRIBE_METHOD(AFGResearchManager::CanResearchBeInitiated,
		[](auto& Scope, const AFGResearchManager* Self, TSubclassOf<UFGSchematic> Schematic)
		{
			if (!GMAMProbeEnabled)
			{
				return;
			}
			const FString Chain = CallerChain();
			bool Result;
			{
				const FBPFrameScope Hidden(nullptr);
				Result = Scope(Self, Schematic);
			}
			Record(TEXT("Research::CanResearchBeInitiated"), Chain, FString::Printf(TEXT("%s schematic=%s -> %d"),
				*ASDDescribe(Self), *MamNameOf(Schematic), Result ? 1 : 0));
		});

	MamInstalling(TEXT("AFGResearchManager::InitiateResearch"));
	SUBSCRIBE_METHOD(AFGResearchManager::InitiateResearch,
		[](auto& Scope, AFGResearchManager* Self, AFGPlayerController* Controller,
		   TSubclassOf<UFGSchematic> Schematic, TSubclassOf<UFGResearchTree> Tree)
		{
			if (!GMAMProbeEnabled)
			{
				return;
			}
			Record(TEXT("Research::InitiateResearch"), CallerChain(), FString::Printf(TEXT("%s schematic=%s tree=%s cost=[%s]"),
				*ASDDescribe(Self), *MamNameOf(Schematic), *MamNameOf(Tree), *CostOf(Schematic)));
			const FBPFrameScope Hidden(nullptr);
			Scope(Self, Controller, Schematic, Tree);
		});

	// Blueprint callers only: the cost list of every schematic is read natively all the time
	// (sorting, unlock checks), and that is not what the window draws.
	MamInstalling(TEXT("UFGSchematic::GetCost"));
	SUBSCRIBE_METHOD(UFGSchematic::GetCost,
		[](auto& Scope, TSubclassOf<UFGSchematic> Schematic)
		{
			if (!GMAMProbeEnabled || !GBPFrame)
			{
				return;
			}
			Record(TEXT("Schematic::GetCost"), CallerChain(), FString::Printf(TEXT("schematic=%s type=%d"),
				*MamNameOf(Schematic), static_cast<int32>(UFGSchematic::GetType(Schematic))));
			const FBPFrameScope Hidden(nullptr);
			Scope(Schematic);
		});

	UE_LOG(LogAnyStorageDepot, Display, TEXT("MAM probe installed; ASD.MAMProbe 1 to record"));
}
