using UnrealBuildTool;

public class AnyStorageDepot : ModuleRules
{
	public AnyStorageDepot(ReadOnlyTargetRules Target) : base(Target)
	{
		CppStandard = CppStandardVersion.Cpp20;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		bLegacyPublicIncludePaths = false;

		PublicDependencyModuleNames.AddRange(new[] {
			"Core", "CoreUObject", "Engine",
			// NetCore: FFastArraySerializer, which is how the item pool reaches clients
			// without resending the whole table on every change.
			"NetCore",
		});

		// FactoryGame gives the buildables, inventories and the central storage subsystem
		// this mod piggybacks on; SML gives the native hooks and the mod subsystem base.
		PublicDependencyModuleNames.AddRange(new[] { "FactoryGame", "SML" });
	}
}
