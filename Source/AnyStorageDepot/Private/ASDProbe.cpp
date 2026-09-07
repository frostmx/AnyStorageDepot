#include "ASDProbe.h"

#include "AnyStorageDepot.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

thread_local int32 FASDProbe::Depth = 0;

FString ASDDescribe(const UObject* Object)
{
	if (!Object)
	{
		return TEXT("<null>");
	}

	const UWorld* World = Object->GetWorld();
	const TCHAR* Side = TEXT("no-world");
	if (World)
	{
		switch (World->GetNetMode())
		{
		case NM_DedicatedServer: Side = TEXT("dedicated"); break;
		case NM_ListenServer:    Side = TEXT("listen");    break;
		case NM_Client:          Side = TEXT("client");    break;
		case NM_Standalone:      Side = TEXT("standalone");break;
		default:                 Side = TEXT("?");         break;
		}
	}
	return FString::Printf(TEXT("%s (%s)"), *Object->GetClass()->GetName(), Side);
}

FASDProbe::FASDProbe(const TCHAR* InName, const UObject* Context, const FString& Extra)
	: Name(InName)
{
	UE_LOG(LogAnyStorageDepot, Display, TEXT("%*s> %s  self=%s%s%s"),
		Depth * 2, TEXT(""), Name, *ASDDescribe(Context),
		Extra.IsEmpty() ? TEXT("") : TEXT("  "), *Extra);
	++Depth;
}

FASDProbe::~FASDProbe()
{
	--Depth;
	UE_LOG(LogAnyStorageDepot, Display, TEXT("%*s< %s"), Depth * 2, TEXT(""), Name);
}
