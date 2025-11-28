// Copyright Splash Damage, Ltd. All Rights Reserved.

#pragma once


#include <CoreMinimal.h>
#include <UObject/ObjectMacros.h>
#include <SceneView.h>
#include <SceneViewExtension.h>
#include <RenderGraph.h>
#include <Async/TaskGraphInterfaces.h>
#include <PostProcess/PostProcessing.h>
#include <PostProcess/PostProcessMaterial.h>

#include "SDCollisionVisSettings.h"


namespace SDCollisionVis
{

struct FRenderBuffer
{
	FIntPoint       Dimensions;
	TArray<FColor>  PixelData;
	void Init(FIntPoint InDimensions)
	{
		Dimensions = InDimensions;
		PixelData.SetNumZeroed(Dimensions.X * Dimensions.Y);
	}
};


struct FSDCollisionVisRealtimeViewData
{
	uint64 LastAccessed = 0;
	TSharedPtr<FRenderBuffer> FramebufferGameThread;	//< Framebuffer held onto by the GameThread
	TSharedPtr<FRenderBuffer> FramebufferRenderThread;	//< Framebuffer held onto by the RenderThread
};

// Realtime renderer, rays are dispatched on the gamethread and then joined
// just before presenting the final image, whereby we overwrite whatever is there.
class FSDCollisionVisRealtimeViewExtension final : public FSceneViewExtensionBase
{
public:
	FSDCollisionVisRealtimeViewExtension(const FAutoRegister& AutoRegister)
		: FSceneViewExtensionBase(AutoRegister)
	{}
	
	struct FRenderState final : ISceneViewFamilyExtentionData
	{
		/** ISceneViewFamilyExtentionData implementation */
		const static inline TCHAR* GSubclassIdentifier = TEXT("FSDCollisionVisRealtimeViewExtension::FRenderState");
		const TCHAR* GetSubclassIdentifier() const { return GSubclassIdentifier; }

		FGraphEventRef TraceTask;					//< Raytracing task which is dispatched by the GameThread, but waited on by the RenderThread.
		TSharedPtr<FRenderBuffer>					FramebufferRenderThreadQueued;
		TSharedPtr<FSDCollisionVisRealtimeViewData> ViewFamilyData;
	};

	/** ISceneViewExtension implementation */
	void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override;
	void PostRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily) override;
};


class FPerspectiveRenderer
{
public:
	FPerspectiveRenderer(	UWorld* InWorld,
							FRenderBuffer& InRenderBuffer,
							const FSDCollisionSettings& InSettings,
							const FVector& InOrigin,
							const FViewMatrices& InViewMatrices)
		: World(InWorld)
		, RenderTargetSize(InRenderBuffer.Dimensions)
		, PixelData(InRenderBuffer.PixelData)
		, Settings(InSettings)
		, Origin(InOrigin)
		, ViewMatrices(InViewMatrices)
		, PointToUV(FVector2D::One() / (FVector2D)RenderTargetSize)
		, RevViewForward(-ViewMatrices.GetOverriddenTranslatedViewMatrix().GetColumn(2))
	{
		check((RenderTargetSize.X * RenderTargetSize.Y) == InRenderBuffer.PixelData.Num());
	}

	FPerspectiveRenderer(const FPerspectiveRenderer& Other) = default;
	FPerspectiveRenderer& operator= (const FPerspectiveRenderer& Other) = default;

	template<EVisualisationType VisType>
	void RenderPerspectivePixel(FIntPoint PixelPos) const
	{
		if (PixelPos.X < RenderTargetSize.X && PixelPos.Y < RenderTargetSize.Y)
		{
			FVector2D UV = PointToUV * ((FVector2D)PixelPos + 0.5);
			FVector2D NDC  = UV * FVector2D(2.0, -2.0) + FVector2D(-1.0, 1.0);
			FVector4 Screen = FVector4(NDC.X, NDC.Y, 0.5, 1.0);

			FVector4 WorldPointHomogenous = ViewMatrices.GetInvViewProjectionMatrix().TransformFVector4(Screen);
			FVector TraceWorldPos (	WorldPointHomogenous.X / WorldPointHomogenous.W,
									WorldPointHomogenous.Y / WorldPointHomogenous.W,
									WorldPointHomogenous.Z / WorldPointHomogenous.W);
			FVector TraceNormal = (TraceWorldPos - Origin).GetUnsafeNormal();

			FHitResult HitResult;

			constexpr bool bUseTimer = (VisType == EVisualisationType::RayTime)
										|| (VisType == EVisualisationType::RayTimeEvenMiss)
										;							
			FTimer Timer;
			if constexpr (bUseTimer)
			{
				Timer.MinTime = Settings.RaytraceTimeMinTime;
				Timer.MaxTime = Settings.RaytraceTimeMaxTime;
				Timer.Start();
			}

			bool bHit = World->LineTraceSingleByObjectType(	HitResult,
															Origin + TraceNormal * Settings.MinDistance,
															Origin + TraceNormal * HALF_WORLD_MAX,
															Settings.CollisionObjectQueryParams,
															Settings.CollisionQueryParams);
							
			if constexpr (bUseTimer)
			{
				Timer.End();
			}

			FColor WritebackColour = CalculateVisualisationColour<VisType>(	bHit,
																			Origin,
																			HitResult,
																			TraceNormal,
																			RevViewForward,
																			Timer,
																			Settings.TriangleDensityMinArea2,
																			Settings.TriangleDensityMul);

			PixelData[PixelPos.Y * RenderTargetSize.X + PixelPos.X] = WritebackColour;
		}
	}

	template<ESamplingPattern SamplingPattern, EVisualisationType VisType>
	void RenderPerspectiveTilePixel(FIntPoint Tile) const
	{
		FIntPoint PixelPos = NextTileSamplePosition<SamplingPattern>(	Tile,
																		Settings.TileSize,
																		Settings.FrameId);
		RenderPerspectivePixel<VisType>(PixelPos);
	}

	UWorld* World;

	// Localised version of FRenderBuffer
	FIntPoint RenderTargetSize;
	TArrayView<FColor> PixelData;

	FSDCollisionSettings Settings;

	// Stuff needed to figure out ray direction and what have you.
	FVector       Origin;
	FViewMatrices ViewMatrices;
	FVector2D     PointToUV;
	FVector       RevViewForward;
};




} // namespace SDCollisionVis
