// Copyright Splash Damage, Ltd. All Rights Reserved.

#pragma once

#include "SDCollisionVisModule.h"

#include <CoreMinimal.h>
#include <DataDrivenShaderPlatformInfo.h>
#include <PixelShaderUtils.h>
#include <Engine/HitResult.h>
#include <Components/PrimitiveComponent.h>
#include <Chaos/ChaosEngineInterface.h>
#include <Chaos/Transform.h>
#include <Chaos/TriangleMeshImplicitObject.h>
#include <Math/Color.h>
#include <Physics/Experimental/PhysScene_Chaos.h>
#include <PhysicsEngine/PhysicsObjectExternalInterface.h>
#include <PhysicalMaterials/PhysicalMaterial.h>

namespace SDCollisionVis
{

enum class EVisualisationType
{
	Default,
	Primitive,
	Triangles,
	Material,
	RayTime,
	RayTimeEvenMiss,
	TriangleDensity,
};


enum class ESamplingPattern
{
	Linear,
	R2
};


FORCEINLINE float RandomBounded(uint32 Seed)
{
    Seed = 0x3f800000u + (Seed & 0x7fffffu);
    return *((float*)&Seed) - 1.0f;
}

FORCEINLINE uint32 SimpleHash32(FUintVector Seed)
{
    uint32 ha = 0xb543c3a6u ^ Seed.X;
    uint32 hb = 0x526f94e2u ^ Seed.Y;
    uint32 hab = ha * hb;
    uint32 hz0 = 0x53c5ca59u ^ (hab >> 5u);
    uint32 hz1 = 0x74743c1bu ^ Seed.Z;
    uint32 h = hz0 * hz1;
    return h;
}


FORCEINLINE FColor RandomColour(FUintVector Seed, float Dampening=1.0f)
{
    float Hue = RandomBounded(SimpleHash32(Seed));
    // HUEtoRGB
    float R = abs(Hue * 6 - 3) - 1;
    float G = 2 - abs(Hue * 6 - 2);
    float B = 2 - abs(Hue * 6 - 4);
	R = FMath::Clamp<float>(R, 0.0f, 1.0f);
	G = FMath::Clamp<float>(G, 0.0f, 1.0f);
	B = FMath::Clamp<float>(B, 0.0f, 1.0f);
	uint8 Cr = (uint8)(R * Dampening * 255.0f + 0.5f);
	uint8 Cg = (uint8)(G * Dampening * 255.0f + 0.5f);
	uint8 Cb = (uint8)(B * Dampening * 255.0f + 0.5f);
	return FColor(Cr, Cg, Cb, 255);
}


FORCEINLINE FColor Heatmap(float Intensity, float Dampening = 1.0)
{
	// Green = Low intensity.
	// Yellow = Medium intensity.
	// Red = High intensity
	float Hue = (1.0f - Intensity) * (1.0f / 3.0f);
	float R = abs(Hue * 6 - 3) - 1;
	float G = 2 - abs(Hue * 6 - 2);
	float B = 2 - abs(Hue * 6 - 4);
	R = FMath::Clamp<float>(R, 0.0f, 1.0f);
	G = FMath::Clamp<float>(G, 0.0f, 1.0f);
	B = FMath::Clamp<float>(B, 0.0f, 1.0f);
	uint8 Cr = (uint8)(R * Dampening * 255.0f + 0.5f);
	uint8 Cg = (uint8)(G * Dampening * 255.0f + 0.5f);
	uint8 Cb = (uint8)(B * Dampening * 255.0f + 0.5f);
	return FColor(Cr, Cg, Cb, 255);
}


template<ESamplingPattern SamplingPattern>
FORCEINLINE FIntPoint NextTileSamplePosition(FIntPoint TileStartOffset, uint32 TileSize, uint32 FrameId)
{
	if constexpr(SamplingPattern == ESamplingPattern::Linear)
	{
		return FIntPoint(TileStartOffset.X + (int32)(FrameId % TileSize),
							TileStartOffset.Y + (int32)((FrameId / TileSize) % TileSize));
	}
	else if constexpr(SamplingPattern == ESamplingPattern::R2)
	{
		constexpr double G1 = 0.7548776662466927600495088963585286918946;
		constexpr double G2 = 0.5698402909980532659113999581195686488398;
		uint32 H = SimpleHash32(FUintVector((uint32)TileStartOffset.X, (uint32)TileStartOffset.Y, 0u));
		int32 X = (int32)(FMath::Frac(G1 * (double)(FrameId + H)) * (double)TileSize);
		int32 Y = (int32)(FMath::Frac(G2 * (double)(FrameId + H)) * (double)TileSize);
		return TileStartOffset + FIntPoint(X, Y);
	}
	else
	{
		// Unhandled sampling mode
		check(false);
		return TileStartOffset;
	}
}


struct FTimer
{
	void Start()
	{
		CyclesStart = FPlatformTime::Cycles64();
	}

	void End()
	{
		float Ms = (float)FPlatformTime::ToMilliseconds64(FPlatformTime::Cycles64() - CyclesStart);
		ClippedTime = FMath::Clamp<float>((Ms - MinTime) / (MaxTime - MinTime), 0.0f, 1.0f);
	}

	float Get() const
	{
		return ClippedTime;
	}

	uint64 CyclesStart{};
	float MinTime{};
	float MaxTime{};
	float ClippedTime{};
};

template<EVisualisationType VisType>
FORCEINLINE FColor CalculateVisualisationColour(bool bHit,
												const FVector Origin,
												FHitResult& HitResult,
												const FVector& TraceNormal,
												const FVector& RevViewForward,
												const FTimer& Timer,
												const float TriangleDensityMinArea2,
												const float TriangleDensityMul)
{

	if constexpr (VisType == EVisualisationType::RayTimeEvenMiss)
	{
		return Heatmap(Timer.Get());
	}

	if (!bHit)
	{
		return FColor::Black;
	}

	float FacingRatio = FMath::Clamp(-(float)TraceNormal.Dot(HitResult.Normal), 0.0f, 1.0f);

	if constexpr (VisType == EVisualisationType::Default)
	{
		float Fr = FacingRatio;
		float Fg = FMath::Clamp((float)RevViewForward.Dot(HitResult.Normal), 0.0f, 1.0f);
		float Fb = FMath::Min(FMath::Sqrt(Fr * Fr + Fg * Fg), 1.0f);
		uint8 Cr = (uint8)(Fr * 255.0f + 0.5f);
		uint8 Cg = (uint8)(Fg * 255.0f + 0.5f);
		uint8 Cb = (uint8)(Fb * 255.0f + 0.5f);
		return FColor(Cr, Cg, Cb, 255);
	}
	else if constexpr (VisType == EVisualisationType::Primitive)
	{
		uint32 PrimIndex = HitResult.ElementIndex;
		return RandomColour(FUintVector(PrimIndex, 0, 0), FacingRatio);
		
	}
	else if constexpr (VisType == EVisualisationType::Triangles)
	{
		uint32 PrimIndex = HitResult.ElementIndex;
		uint32 FaceIndex = HitResult.FaceIndex;
		return RandomColour(FUintVector(FaceIndex, PrimIndex, 0));
	}
	else if constexpr (VisType == EVisualisationType::Material)
	{
		uint32 MaterialId = 0;
		if (UPhysicalMaterial* Material = HitResult.PhysMaterial.Get())
		{
			MaterialId = Material->GetUniqueID();
		}
		return RandomColour(FUintVector(MaterialId, 0, 0), FacingRatio);
	}
	else if constexpr (VisType == EVisualisationType::RayTime)
	{
		return Heatmap(Timer.Get());
	}
	else if constexpr (VisType == EVisualisationType::TriangleDensity)
	{		
		FColor Result { (uint8)(127.0 * FacingRatio), 0, 0 };
		Result.G = Result.R;
		Result.B = Result.R;

		UPrimitiveComponent* Component = HitResult.GetComponent();

		// TODO:
		// Things like instanced static meshes don't write back the phys object, but
		// it can be fetched from the component directly.
		// Sadly, it looks like the RayCast we do later always fails, which probably means
		// there is some level of transform we need to do to account for this properly.
#if 0
		if (!HitResult.PhysicsObject)
		{
			if (IPhysicsComponent* PhysComp = Cast<IPhysicsComponent>(Component))
			{
				HitResult.PhysicsObject = PhysComp->GetPhysicsObjectById(0); // Get the root physics object
			}
		}
#endif

		if (HitResult.PhysicsObject)
		{
			if (FChaosScene* Scene = static_cast<FChaosScene*>(FPhysicsObjectExternalInterface::GetScene(HitResult.PhysicsObject)))
			{
				FLockedReadPhysicsObjectExternalInterface Interface = FPhysicsObjectExternalInterface::LockRead(Scene);
				if (Chaos::FImplicitObjectRef Ref = Interface->GetGeometry(HitResult.PhysicsObject))
				{
					if (Ref->IsUnderlyingMesh() || Ref->IsUnderlyingUnion())
					{
						FTransform RootTransform = Interface->GetTransform(HitResult.PhysicsObject);

						// Unhandled mesh type
						Result = FColor(0, 0, FacingRatio * 255.0f + 0.5f, 255);

						Ref->VisitLeafObjects(
							[&](const Chaos::FImplicitObject* Implicit, const Chaos::FRigidTransform3& RelativeTransform, const int32 RootObjectIndex, const int32 ObjectIndex, const int32 LeafObjectIndex)
							{
								Chaos::FRigidTransform3 Transform = RelativeTransform;
								const Chaos::FTriangleMeshImplicitObject* TriangleMesh = Implicit->template GetObject<Chaos::FTriangleMeshImplicitObject>();

								// Fetch mesh from nested type
								if (!TriangleMesh)
								{
									// Scaled mesh
									if (const Chaos::TImplicitObjectScaled<Chaos::FTriangleMeshImplicitObject>* ScaledTriangleMesh = Implicit->template GetObject<const Chaos::TImplicitObjectScaled<Chaos::FTriangleMeshImplicitObject>>())
									{
										Transform = Chaos::FRigidTransform3::Identity;
										Transform.SetScale3D(ScaledTriangleMesh->GetScale());
										Transform = RelativeTransform * Transform;
										TriangleMesh = ScaledTriangleMesh->GetUnscaledObject();
									}

									// Instanced mesh
									else if (const Chaos::TImplicitObjectInstanced<Chaos::FTriangleMeshImplicitObject>* InstancedTriangleMesh = Implicit->template GetObject<const Chaos::TImplicitObjectInstanced<Chaos::FTriangleMeshImplicitObject>>())
									{
										TriangleMesh = InstancedTriangleMesh->GetInstancedObject();
									}
								}

								if (TriangleMesh)
								{
									FVector RayStart = HitResult.ImpactPoint - TraceNormal;

									Chaos::FRigidTransform3 NodeTransform = Transform * RootTransform;
									Chaos::FReal Time;
									Chaos::FVec3 Pos;
									Chaos::FVec3 N;
									int32 ContactFaceIndex = INDEX_NONE;
									if (TriangleMesh->Raycast(	NodeTransform.InverseTransformPosition(RayStart),
																NodeTransform.InverseTransformVector(TraceNormal),
																10.0, 0.0, Time, Pos, N, ContactFaceIndex))
									{
										Chaos::FVec3 pA{};
										Chaos::FVec3 pB{};
										Chaos::FVec3 pC{};
										
										const Chaos::FTrimeshIndexBuffer& Elements = TriangleMesh->Elements();
										if (Elements.RequiresLargeIndices())
										{
											auto I = Elements.GetLargeIndexBuffer()[ContactFaceIndex];
											pA = TriangleMesh->Particles().GetX(I[0]);
											pB = TriangleMesh->Particles().GetX(I[1]);
											pC = TriangleMesh->Particles().GetX(I[2]);
										}
										else
										{
											auto I = Elements.GetSmallIndexBuffer()[ContactFaceIndex];
											pA = TriangleMesh->Particles().GetX(I[0]);
											pB = TriangleMesh->Particles().GetX(I[1]);
											pC = TriangleMesh->Particles().GetX(I[2]);
										}

										pA = NodeTransform.TransformPosition(pA);
										pB = NodeTransform.TransformPosition(pB);
										pC = NodeTransform.TransformPosition(pC);

										pA -= pC;
										pB -= pC;
										float Area2 = pA.Cross(pB).Length();
										Area2 = FMath::Clamp(1.0 - (Area2 - TriangleDensityMinArea2) * TriangleDensityMul, 0.0, 1.0);
										Result = Heatmap(Area2, FacingRatio);
									}
								}

							});
					}
				}
			}
		}

		return Result;
	}
	else
	{
		// Unhandled visualisation mode
		check(false);
		return FColor::Black;
	}
}


template< EVisualisationType InVisType = EVisualisationType::Default
		, ESamplingPattern InSamplingPattern = ESamplingPattern::Linear>
struct TKernelDispatchParameters
{
	const static EVisualisationType VisType = InVisType;
	const static ESamplingPattern SamplingPattern = InSamplingPattern;

	template<EVisualisationType NewVisType>
	static TKernelDispatchParameters<NewVisType, InSamplingPattern> SetVisType() { return {}; };

	template<ESamplingPattern NewSamplingPattern>
	static TKernelDispatchParameters<InVisType, NewSamplingPattern> SetSamplingPattern() { return {}; };
};

// Mask used for subscribing to dynamic dispatch parameters
// e.g: If we know we want to force the SamplingPattern to linear,
//      we don't need have dynamic branching for anything else.
using EKernelDispatchMaskType = uint32;

enum EKernelDispatchMask : EKernelDispatchMaskType
{
	EKD_VisType			= (1 << 0),
	EKD_SamplingPattern = (1 << 1)
};

struct FKernelExecutor
{
	EVisualisationType VisType = EVisualisationType::Default;
	ESamplingPattern SamplingPattern = ESamplingPattern::Linear;

	// Resolves the settings into TKernelDispatchParameters, which has constexpr friendly attributes, then
	// calls the provided kernel with it (which is expected to be a lambda with auto as the parameter).
	// Internally, this should create a giant switch-jump at the assembly level.
	template<	typename BaseDispatcher = TKernelDispatchParameters<>,                          //< Base dispatcher (with type overrides if wanted)
				EKernelDispatchMaskType DispatchTypes = (EKD_VisType | EKD_SamplingPattern),    //< Mask for which settings are dynamic
				typename F                                                                      //< Kernel function
	>
	void Dispatch(F&& Kernel) const
	{
			auto DispatchKernelVisType = [&](auto Settings, auto Next, auto... Others)
			{
				if constexpr ((DispatchTypes & EKD_VisType) != EKD_VisType)
				{
					return Next(Settings, Others...);
				}
				else
				{
					switch (VisType)
					{
					case EVisualisationType::Default:           { Next(Settings.template SetVisType<EVisualisationType::Default>(), Others...); break; }
					case EVisualisationType::Primitive:         { Next(Settings.template SetVisType<EVisualisationType::Primitive>(), Others...); break; }
					case EVisualisationType::Triangles:         { Next(Settings.template SetVisType<EVisualisationType::Triangles>(), Others...); break; }
					case EVisualisationType::Material:          { Next(Settings.template SetVisType<EVisualisationType::Material>(), Others...); break; }
					case EVisualisationType::RayTime:           { Next(Settings.template SetVisType<EVisualisationType::RayTime>(), Others...); break; }
					case EVisualisationType::RayTimeEvenMiss:   { Next(Settings.template SetVisType<EVisualisationType::RayTimeEvenMiss>(), Others...); break; }
					case EVisualisationType::TriangleDensity:   { Next(Settings.template SetVisType<EVisualisationType::TriangleDensity>(), Others...); break; }
					}
				}
			};

			auto DispatchSamplingPattern = [&](auto Settings, auto Next, auto... Others)
			{
				if constexpr ((DispatchTypes & EKD_SamplingPattern) != EKD_SamplingPattern)
				{
					return Next(Settings, Others...);
				}
				else
				{
					switch (SamplingPattern)
					{
					case ESamplingPattern::Linear:  { Next(Settings.template SetSamplingPattern<ESamplingPattern::Linear>(), Others...); break; }
					case ESamplingPattern::R2:      { Next(Settings.template SetSamplingPattern<ESamplingPattern::R2>(), Others...); break; }
					}
				}
			};

			auto DispatchChain = [&](auto&& First, auto&&... Others)
			{
				First(BaseDispatcher(), Others...);
			};

			DispatchChain(DispatchSamplingPattern, DispatchKernelVisType, Kernel);
	}
};


struct FSDCollisionSettings
{
	FSDCollisionSettings();
	// Update parameters which are dependant on other parameters, which may have changed.
	void UpdateSettings();

	EVisualisationType VisType = EVisualisationType::Default;
	ESamplingPattern SamplingPattern = ESamplingPattern::Linear;

	FCollisionObjectQueryParams CollisionObjectQueryParams;
	FCollisionQueryParams CollisionQueryParams;

	uint32 TileSize = 8u;
	float Scale = 0.5f;
	double MinDistance = 0.0;
	uint32 FrameId = 0u;
	float RaytraceTimeMinTime = 0.0f;
	float RaytraceTimeMaxTime = 0.0f;
	float TriangleDensityMinArea2 = 0.0f;
	float TriangleDensityMaxArea2 = 0.0f;
	float TriangleDensityMul = 0.0f;
};


struct FSDOfflineCollisionSettings : FSDCollisionSettings
{
	using FSDCollisionSettings::FSDCollisionSettings;
	
	FVector RayOrigin;
	FRotator RayRotator;

	int32 Resolution = 512;
	int32 MaxRaysPerFrame = 1024;
	bool bCubeMap = false;
	UWorld* World = nullptr;
};

} // namespace SDCollisionVis
