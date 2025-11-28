// Copyright Splash Damage, Ltd. All Rights Reserved.


namespace UnrealBuildTool.Rules
{
    public class SDCollisionVis : ModuleRules
    {
        public SDCollisionVis(ReadOnlyTargetRules Target) : base(Target)
        {

            PrivateIncludePaths.AddRange(
                new[]
                {
                    System.IO.Path.Combine(GetModuleDirectory("Renderer"), "Private"),
                    System.IO.Path.Combine(GetModuleDirectory("Engine"), "Private")
                });

            PrivateDependencyModuleNames.AddRange(
                new string[]
                {
                    "Core",
                    "CoreUObject",
                    "PhysicsCore",
					"Chaos",
                    "Engine",
                    "ImageCore",
                    "Projects",
                    "RenderCore",
                    "Renderer",
                    "RHI",
                }
                );

				if (Target.bBuildEditor == true)
				{
					PrivateDependencyModuleNames.AddRange(
						new string[] {
						"UnrealEd",
						});
				}
		}
    }
}
