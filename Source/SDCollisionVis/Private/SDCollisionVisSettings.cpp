// Copyright Splash Damage, Ltd. All Rights Reserved.

#include "SDCollisionVisSettings.h"

#include <DataDrivenShaderPlatformInfo.h>
#include <HAL/ConsoleManager.h>
#include <GlobalShader.h>
#include <PixelShaderUtils.h>
#include <RenderGraphResources.h>
#include <RHICommandList.h>
#include <Shader.h>
#include <ShaderParameterStruct.h>
#include <Async/ParallelFor.h>
#include <Containers/ResourceArray.h>
#include <Engine/HitResult.h>
#include <Components/PrimitiveComponent.h>
#include <Chaos/ChaosEngineInterface.h>
#include <Chaos/Transform.h>
#include <Chaos/TriangleMeshImplicitObject.h>
#include <Physics/Experimental/PhysScene_Chaos.h>
#include <PhysicsEngine/PhysicsObjectExternalInterface.h>
#include <PhysicalMaterials/PhysicalMaterial.h>

#define LOCTEXT_NAMESPACE "SDCollisionVis"

namespace SDCollisionVis
{

// Top level settings
static TAutoConsoleVariable<int32> CVarSettingsTileSize(
	TEXT("r.SDCollisionVis.Settings.TileSize"),
	8,
	TEXT("Tile size to split up the screen to use when tracing (1px per tile is updated per frame)."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarSettingsScale(
	TEXT("r.SDCollisionVis.Settings.Scale"),
	0.5f,
	TEXT("How much to downscale the render buffer."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarSettingsSamplingPattern(
	TEXT("r.SDCollisionVis.Settings.SamplingPattern"),
	1,
	TEXT("Sampling pattern to use:\n")
	TEXT("0 = Linear\n")
	TEXT("1 = R2"),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarSettingsVisType(
	TEXT("r.SDCollisionVis.Settings.VisType"),
	0,
	TEXT("Visualisation type:\n")
	TEXT("0 = Default (Facing ratio based thing)\n")
	TEXT("1 = Primitive Id\n")
	TEXT("2 = Triangle Id\n")
	TEXT("3 = Material Id\n")
	TEXT("4 = Raytrace Time\n")
	TEXT("5 = Triangle Density\n"),
	ECVF_Default);


static TAutoConsoleVariable<int32> CVarSettingsRaytraceTimeIncludeMisses(
	TEXT("r.SDCollisionVis.Settings.RaytraceTime.IncludeMisses"),
	0,
	TEXT("Include rays which didn't hit any surface."),
	ECVF_Default);


static TAutoConsoleVariable<float> CVarSettingsRaytraceTimeMinTime(
	TEXT("r.SDCollisionVis.Settings.RaytraceTime.MinTime"),
	0.001f,
	TEXT("Minumum representable time (ms)."),
	ECVF_Default);


static TAutoConsoleVariable<float> CVarSettingsRaytraceTimeMaxTime(
	TEXT("r.SDCollisionVis.Settings.RaytraceTime.MaxTime"),
	0.02f,
	TEXT("Maximum representable time (ms)."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarSettingsTriangleDensityMinArea(
	TEXT("r.SDCollisionVis.Settings.TriangleDensity.MinArea"),
	1.0f,
	TEXT("Minimum area (high density)."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarSettingsTriangleDensityMaxArea(
	TEXT("r.SDCollisionVis.Settings.TriangleDensity.MaxArea"),
	10000.0f,
	TEXT("Maximum area (low density)."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarSettingsMinDistance(
	TEXT("r.SDCollisionVis.Settings.MinDistance"),
	100.0f,
	TEXT("Minimum distance to travel. (Default 100)"),
	ECVF_Default);

// Inputs to `FCollisionObjectQueryParams`
static TAutoConsoleVariable<int32> CVarCollisionObjectQueryAllObjects(
	TEXT("r.SDCollisionVis.CollisionObjectQuery.AllObjects"),
	0,
	TEXT("Enable AllObjects\n"),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarCollisionObjectQueryAllStaticObjects(
	TEXT("r.SDCollisionVis.CollisionObjectQuery.AllStaticObjects"),
	0,
	TEXT("Enable AllStaticObjects\n"),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarCollisionObjectQueryAllDynamicObjects(
	TEXT("r.SDCollisionVis.CollisionObjectQuery.AllDynamicObjects"),
	0,
	TEXT("Enable AllDynamicObjects\n"),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarCollisionObjectQueryWorldStatic(
	TEXT("r.SDCollisionVis.CollisionObjectQuery.WorldStatic"),
	0,
	TEXT("Enable ECC_WorldStatic\n"),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarCollisionObjectQueryWorldDynamic(
	TEXT("r.SDCollisionVis.CollisionObjectQuery.WorldDynamic"),
	0,
	TEXT("Enable ECC_WorldDynamic\n"),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarCollisionObjectQueryPawn(
	TEXT("r.SDCollisionVis.CollisionObjectQuery.Pawn"),
	0,
	TEXT("Enable ECC_Pawn\n"),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarCollisionObjectQueryVisibility(
	TEXT("r.SDCollisionVis.CollisionObjectQuery.Visibility"),
	0,
	TEXT("Enable ECC_Visibility\n"),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarCollisionObjectQueryCamera(
	TEXT("r.SDCollisionVis.CollisionObjectQuery.Camera"),
	0,
	TEXT("Enable ECC_Camera\n"),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarCollisionObjectQueryPhysicsBody(
	TEXT("r.SDCollisionVis.CollisionObjectQuery.PhysicsBody"),
	0,
	TEXT("Enable ECC_PhysicsBody\n"),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarCollisionObjectQueryVehicle(
	TEXT("r.SDCollisionVis.CollisionObjectQuery.Vehicle"),
	0,
	TEXT("Enable ECC_Vehicle\n"),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarCollisionObjectQueryDestructible(
	TEXT("r.SDCollisionVis.CollisionObjectQuery.Destructible"),
	0,
	TEXT("Enable ECC_Destructible\n"),
	ECVF_Default);


// Inputs to `FCollisionQueryParams`
static TAutoConsoleVariable<FString> CVarCollisionQueryTraceTag(
	TEXT("r.SDCollisionVis.CollisionQuery.TraceTag"),
	TEXT(""),
	TEXT("Trace tag to use when traversing (e.g Landscape or NavigationFilterOverlapTest)"),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarCollisionQueryTraceComplex(
	TEXT("r.SDCollisionVis.CollisionQuery.TraceComplex"),
	1,
	TEXT("Enable bTraceComplex\n"),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarCollisionQueryIgnoreBlocks(
	TEXT("r.SDCollisionVis.CollisionQuery.IgnoreBlocks"),
	0,
	TEXT("Enable bIgnoreBlocks\n"),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarCollisionQueryIgnoreTouches(
	TEXT("r.SDCollisionVis.CollisionQuery.IgnoreTouches"),
	0,
	TEXT("Enable bIgnoreTouches\n"),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarCollisionQueryMobilityType(
	TEXT("r.SDCollisionVis.CollisionQuery.MobilityType"),
	0,
	TEXT("Mobility type to use\n")
	TEXT("0 = Any\n")
	TEXT("1 = Static\n")
	TEXT("2 = Dynamic\n"),
	ECVF_Default);


// Presets
static FAutoConsoleCommand CVarPresetDefault(
	TEXT("r.SDCollisionVis.Preset.Default()"),
	TEXT("Changes the settings to use default settings."),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		CVarCollisionObjectQueryAllObjects.AsVariable()->Set(0, ECVF_SetByConsole);
		CVarCollisionObjectQueryAllStaticObjects.AsVariable()->Set(0, ECVF_SetByConsole);
		CVarCollisionObjectQueryAllDynamicObjects.AsVariable()->Set(0, ECVF_SetByConsole);
		CVarCollisionObjectQueryWorldStatic.AsVariable()->Set(0, ECVF_SetByConsole);
		CVarCollisionObjectQueryWorldDynamic.AsVariable()->Set(0, ECVF_SetByConsole);
		CVarCollisionObjectQueryPawn.AsVariable()->Set(0, ECVF_SetByConsole);
		CVarCollisionObjectQueryVisibility.AsVariable()->Set(0, ECVF_SetByConsole);
		CVarCollisionObjectQueryCamera.AsVariable()->Set(0, ECVF_SetByConsole);
		CVarCollisionObjectQueryPhysicsBody.AsVariable()->Set(0, ECVF_SetByConsole);
		CVarCollisionObjectQueryVehicle.AsVariable()->Set(0, ECVF_SetByConsole);
		CVarCollisionObjectQueryDestructible.AsVariable()->Set(0, ECVF_SetByConsole);

		CVarCollisionQueryTraceTag.AsVariable()->Set(TEXT(""), ECVF_SetByConsole);
		CVarCollisionQueryTraceComplex.AsVariable()->Set(1, ECVF_SetByConsole);
		CVarCollisionQueryIgnoreBlocks.AsVariable()->Set(0, ECVF_SetByConsole);
		CVarCollisionQueryIgnoreTouches.AsVariable()->Set(0, ECVF_SetByConsole);
		CVarCollisionQueryMobilityType.AsVariable()->Set(0, ECVF_SetByConsole);
	}));

static FAutoConsoleCommand CVarPresetLandscapeEditor(
	TEXT("r.SDCollisionVis.Preset.LandscapeEditor()"),
	TEXT("Changes the settings to match those used when evaluating the landscape sculpting tools."),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		CVarCollisionObjectQueryAllObjects.AsVariable()->Set(0, ECVF_SetByConsole);
		CVarCollisionObjectQueryAllStaticObjects.AsVariable()->Set(0, ECVF_SetByConsole);
		CVarCollisionObjectQueryAllDynamicObjects.AsVariable()->Set(0, ECVF_SetByConsole);
		CVarCollisionObjectQueryWorldStatic.AsVariable()->Set(0, ECVF_SetByConsole);
		CVarCollisionObjectQueryWorldDynamic.AsVariable()->Set(0, ECVF_SetByConsole);
		CVarCollisionObjectQueryPawn.AsVariable()->Set(0, ECVF_SetByConsole);
		CVarCollisionObjectQueryVisibility.AsVariable()->Set(1, ECVF_SetByConsole);
		CVarCollisionObjectQueryCamera.AsVariable()->Set(0, ECVF_SetByConsole);
		CVarCollisionObjectQueryPhysicsBody.AsVariable()->Set(0, ECVF_SetByConsole);
		CVarCollisionObjectQueryVehicle.AsVariable()->Set(0, ECVF_SetByConsole);
		CVarCollisionObjectQueryDestructible.AsVariable()->Set(0, ECVF_SetByConsole);

		CVarCollisionQueryTraceTag.AsVariable()->Set(TEXT("Landscape"), ECVF_SetByConsole);
		CVarCollisionQueryTraceComplex.AsVariable()->Set(1, ECVF_SetByConsole);
		CVarCollisionQueryIgnoreBlocks.AsVariable()->Set(0, ECVF_SetByConsole);
		CVarCollisionQueryIgnoreTouches.AsVariable()->Set(0, ECVF_SetByConsole);
		CVarCollisionQueryMobilityType.AsVariable()->Set(0, ECVF_SetByConsole);
	}));


FSDCollisionSettings::FSDCollisionSettings()
{
	switch (CVarSettingsVisType.GetValueOnGameThread())
	{
	case 0: { VisType = EVisualisationType::Default; break; }
	case 1: { VisType = EVisualisationType::Primitive; break; }
	case 2: { VisType = EVisualisationType::Triangles; break; }
	case 3: { VisType = EVisualisationType::Material; break; }
	case 4: { VisType = (CVarSettingsRaytraceTimeIncludeMisses.GetValueOnGameThread() != 0) ? EVisualisationType::RayTimeEvenMiss : EVisualisationType::RayTime; break; }
	case 5: { VisType = EVisualisationType::TriangleDensity; break; }
	}

	switch (CVarSettingsSamplingPattern.GetValueOnGameThread())
	{
	case 0: { SamplingPattern = ESamplingPattern::Linear; break; }
	case 1: { SamplingPattern = ESamplingPattern::R2; break; }
	}

	int32 ObjectQueryMask = 0;
	if(CVarCollisionObjectQueryAllObjects.GetValueOnGameThread() != 0)          { ObjectQueryMask |= FCollisionObjectQueryParams(FCollisionObjectQueryParams::AllObjects).ObjectTypesToQuery; }
	if(CVarCollisionObjectQueryAllStaticObjects.GetValueOnGameThread() != 0)    { ObjectQueryMask |= FCollisionObjectQueryParams(FCollisionObjectQueryParams::AllStaticObjects).ObjectTypesToQuery; }
	if(CVarCollisionObjectQueryAllDynamicObjects.GetValueOnGameThread() != 0)   { ObjectQueryMask |= FCollisionObjectQueryParams(FCollisionObjectQueryParams::AllDynamicObjects).ObjectTypesToQuery; }
	if(CVarCollisionObjectQueryWorldStatic.GetValueOnGameThread() != 0)         { ObjectQueryMask |= ECC_TO_BITFIELD(ECC_WorldStatic); }
	if(CVarCollisionObjectQueryWorldDynamic.GetValueOnGameThread() != 0)        { ObjectQueryMask |= ECC_TO_BITFIELD(ECC_WorldDynamic); }
	if(CVarCollisionObjectQueryPawn.GetValueOnGameThread() != 0)                { ObjectQueryMask |= ECC_TO_BITFIELD(ECC_Pawn); }
	if(CVarCollisionObjectQueryVisibility.GetValueOnGameThread() != 0)          { ObjectQueryMask |= ECC_TO_BITFIELD(ECC_Visibility); }
	if(CVarCollisionObjectQueryCamera.GetValueOnGameThread() != 0)              { ObjectQueryMask |= ECC_TO_BITFIELD(ECC_Camera); }
	if(CVarCollisionObjectQueryPhysicsBody.GetValueOnGameThread() != 0)         { ObjectQueryMask |= ECC_TO_BITFIELD(ECC_PhysicsBody); }
	if(CVarCollisionObjectQueryVehicle.GetValueOnGameThread() != 0)             { ObjectQueryMask |= ECC_TO_BITFIELD(ECC_Vehicle); }
	if(CVarCollisionObjectQueryDestructible.GetValueOnGameThread() != 0)        { ObjectQueryMask |= ECC_TO_BITFIELD(ECC_Destructible); }

	CollisionObjectQueryParams = FCollisionObjectQueryParams(ObjectQueryMask);

	const FString& TraceTag = CVarCollisionQueryTraceTag.GetValueOnGameThread();
	if (!TraceTag.IsEmpty())
	{
		CollisionQueryParams.TraceTag = FName(TraceTag, FNAME_Find);
	}

	CollisionQueryParams.bTraceComplex = CVarCollisionQueryTraceComplex.GetValueOnGameThread() != 0;
	CollisionQueryParams.bIgnoreBlocks = CVarCollisionQueryIgnoreBlocks.GetValueOnGameThread() != 0;
	CollisionQueryParams.bIgnoreTouches = CVarCollisionQueryIgnoreTouches.GetValueOnGameThread() != 0;
	CollisionQueryParams.bReturnPhysicalMaterial = VisType == EVisualisationType::Material;
	switch (CVarCollisionQueryMobilityType.GetValueOnGameThread())
	{
	case 0: { CollisionQueryParams.MobilityType = EQueryMobilityType::Any; break; }
	case 1: { CollisionQueryParams.MobilityType = EQueryMobilityType::Static; break; }
	case 2: { CollisionQueryParams.MobilityType = EQueryMobilityType::Dynamic; break; }
	}

	MinDistance = (double)CVarSettingsMinDistance.GetValueOnGameThread();
	TileSize = CVarSettingsTileSize.GetValueOnGameThread();
	Scale = CVarSettingsScale.GetValueOnGameThread();
	RaytraceTimeMinTime = CVarSettingsRaytraceTimeMinTime.GetValueOnGameThread();
	RaytraceTimeMaxTime = CVarSettingsRaytraceTimeMaxTime.GetValueOnGameThread();
	TriangleDensityMinArea2 = CVarSettingsTriangleDensityMinArea.GetValueOnGameThread() * 2.0;
	TriangleDensityMaxArea2 = CVarSettingsTriangleDensityMaxArea.GetValueOnGameThread() * 2.0;

	UpdateSettings();
}

void FSDCollisionSettings::UpdateSettings()
{
	CollisionQueryParams.bReturnFaceIndex = (VisType == EVisualisationType::Triangles) || (VisType == EVisualisationType::TriangleDensity);
	TileSize = FMath::Clamp<uint32>(TileSize, 2u, 128u);
	Scale = FMath::Clamp<float>(Scale, 0.0f, 1.0f);
	FrameId = SamplingPattern == ESamplingPattern::R2 ?
								((uint32)(GFrameCounter % (TileSize * TileSize * TileSize * TileSize)))
								: ((uint32)(GFrameCounter & 0xffffffffu))
								;
	TriangleDensityMul = 1.0 / (TriangleDensityMaxArea2 - TriangleDensityMinArea2);
}

} // namespace SDCollisionVis

#undef LOCTEXT_NAMESPACE 
