// Copyright Splash Damage, Ltd. All Rights Reserved.

#include "SDCollisionVisRenderer.h"
#include "SDCollisionVisSettings.h"

#include <GlobalShader.h>
#include <RenderGraphResources.h>
#include <RHICommandList.h>
#include <Shader.h>
#include <ScreenPass.h>
#include <ShaderParameterStruct.h>
#include <Async/ParallelFor.h>
#include <HAL/ConsoleManager.h>
#include <Misc/FileHelper.h>
#include <ImageUtils.h>
#include <DDSFile.h>
#include <GameFramework/Pawn.h>
#include <Engine/Level.h>
#include <Containers/ResourceArray.h>
#include <Misc/EngineVersionComparison.h>


#if WITH_EDITOR
#include <LevelEditorViewport.h>
#endif // WITH_EDITOR


#define LOCTEXT_NAMESPACE "SDCollisionVis"

namespace SDCollisionVis
{

////////////////////////////////////
//////          Realtime          //
////////////////////////////////////

static TAutoConsoleVariable<int32> CVarSettingsUseWorldServer(
	TEXT("r.SDCollisionVis.Settings.UseServerWorld"),
	0,
	TEXT("Attempt to use the World from a locally running server on the same process."),
	ECVF_Default);

namespace
{

static TCustomShowFlag<EShowFlagShippingValue::ForceDisabled> ShowSDCollisionVis (TEXT("SDCollisionVis"), false, SFG_Visualize, LOCTEXT("SDVisCollision", "[SDCollisionVis] Visualize Collisions"));

} // unnamed namespace


class FDrawTracedTexturePS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FDrawTracedTexturePS);
	SHADER_USE_PARAMETER_STRUCT(FDrawTracedTexturePS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SourceTexture)
		SHADER_PARAMETER(FVector2f, InvViewport)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		 return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
	
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	}
};

IMPLEMENT_GLOBAL_SHADER(FDrawTracedTexturePS, "/Plugin/SDCollisionVis/DrawTracedTexture.usf", "DrawTracedTexturePS", SF_Pixel);


void FSDCollisionVisRealtimeViewExtension::BeginRenderViewFamily(FSceneViewFamily& ViewFamily)
{

	if (ViewFamily.Views.IsEmpty() || !ViewFamily.bIsMainViewFamily || !ViewFamily.Scene || !ViewFamily.Scene->GetWorld())
	{
		return;
	}

	UWorld* World = ViewFamily.Scene->GetWorld();
	const FSceneView& MainView = *ViewFamily.Views[0];
	bool bEnabled = ShowSDCollisionVis.IsEnabled(MainView.Family->EngineShowFlags)
					&& (MainView.UnscaledViewRect.Area() > 0)
					;
	
	if (bEnabled)
	{
		TSharedPtr<FSDCollisionVisRealtimeViewData> RenderData = FModuleManager::LoadModuleChecked<FSDCollisionVisModule>("SDCollisionVis").GetRealtimeViewFamilyData(ViewFamily);
		if (!RenderData)
		{
			return;
		}

		if (CVarSettingsUseWorldServer.GetValueOnGameThread())
		{
			for (UWorld* It : TObjectRange<UWorld>(RF_ClassDefaultObject | RF_ArchetypeObject, true, EInternalObjectFlags::Garbage))
			{
				if (It->GetNetMode() == NM_DedicatedServer)
				{
					World = It;
					break;
				}
			}
		}

		FSDCollisionSettings Settings;

		FIntPoint ViewRectSize = MainView.UnscaledViewRect.Size();
		float Scale = FMath::Max(Settings.Scale, 1.0f / float(FMath::Min(ViewRectSize.X, ViewRectSize.Y)));
		FIntPoint RenderTargetSize((int32)(ViewRectSize.X * Scale + 0.5f),
									(int32)(ViewRectSize.Y * Scale + 0.5f));

		bool bKeepFrameBuffer = RenderData->FramebufferGameThread.IsValid()
								&& RenderData->FramebufferGameThread->Dimensions == RenderTargetSize;

		if (!bKeepFrameBuffer)
		{
			RenderData->FramebufferGameThread = MakeShared<FRenderBuffer>();
			RenderData->FramebufferGameThread->Init(RenderTargetSize);
		}

		FPerspectiveRenderer PerspectiveRenderer(	World,
													*RenderData->FramebufferGameThread,
													Settings,
													(FVector)MainView.ViewLocation,
													MainView.ViewMatrices);

		TFunction<void()> TraceFunc = [	PerspectiveRenderer = MoveTemp(PerspectiveRenderer),
										KeepAlive=RenderData->FramebufferGameThread]()
		{
			const auto& Settings = PerspectiveRenderer.Settings;
			const int32 NumTileY = (PerspectiveRenderer.RenderTargetSize.Y + Settings.TileSize - 1) / Settings.TileSize;

			auto Kernel = [&](auto DispatchParameters)
			{
				const static ESamplingPattern SamplingPattern = decltype(DispatchParameters)::SamplingPattern;
				const static EVisualisationType VisType = decltype(DispatchParameters)::VisType;
				
				ParallelFor(NumTileY, [&, Settings=Settings](int32 TileIdY)
				{
					int32 TileY = Settings.TileSize * TileIdY;
					for (int32 TileX = 0; TileX < PerspectiveRenderer.RenderTargetSize.X; TileX += Settings.TileSize)
					{
						PerspectiveRenderer.RenderPerspectiveTilePixel<SamplingPattern, VisType>(FIntPoint(TileX, TileY));
					}
				});
			};

			FKernelExecutor Executor
			{
				.VisType = Settings.VisType,
				.SamplingPattern = Settings.SamplingPattern
			};

			Executor.Dispatch<	TKernelDispatchParameters<>,
								(EKD_VisType | EKD_SamplingPattern)>(Kernel);
		};

		FRenderState& RenderState = *ViewFamily.GetOrCreateExtentionData<FRenderState>();
		RenderState.ViewFamilyData = RenderData;
		RenderState.TraceTask = FFunctionGraphTask::CreateAndDispatchWhenReady(MoveTemp(TraceFunc), TStatId(), nullptr);
		RenderState.FramebufferRenderThreadQueued = RenderData->FramebufferGameThread;
	}
}

void FSDCollisionVisRealtimeViewExtension::PostRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& ViewFamily)
{
	FRenderState* RenderStatePtr = ViewFamily.GetExtentionData<FRenderState>();
	if (!RenderStatePtr)
	{
		return;
	}

	FRenderState& RenderState = *RenderStatePtr;
	RenderState.ViewFamilyData->FramebufferRenderThread = RenderState.FramebufferRenderThreadQueued;
	if (RenderState.TraceTask)
	{
		FTaskGraphInterface::Get().WaitUntilTaskCompletes(RenderState.TraceTask);
	}

	FRDGTextureRef ViewFamilyTexture = TryCreateViewFamilyTexture(GraphBuilder, ViewFamily);
	if (!ViewFamilyTexture)
	{
		return;
	}

	struct FTraceTextureUploadData : FResourceBulkDataInterface
	{
		const void* GetResourceBulkData() const override final { return RenderBuffer->PixelData.GetData(); }
		uint32 GetResourceBulkDataSize() const override final { return RenderBuffer->PixelData.Num() * RenderBuffer->PixelData.GetTypeSize(); }
		void Discard() override final{ }
		FRenderBuffer* RenderBuffer;
	};
	
	FRenderBuffer& RenderBuffer = *RenderState.FramebufferRenderThreadQueued.Get();

	FTraceTextureUploadData Loader;
	Loader.RenderBuffer = &RenderBuffer;

	FTextureRHIRef TraceTexture = RHICreateTexture(
		FRHITextureCreateDesc::Create2D(TEXT("SDCollisionVis.TracedTexture"), RenderBuffer.Dimensions.X, RenderBuffer.Dimensions.Y, PF_B8G8R8A8)
			.SetFlags(TexCreate_ShaderResource)
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
			.SetInitActionBulkData(&Loader))
#else	// UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
			.SetBulkData(&Loader))
#endif // UE_VERSION
		;

	{
		FIntVector DestSize = ViewFamilyTexture->Desc.GetSize();

		FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(ViewFamily.GetFeatureLevel());
		FDrawTracedTexturePS::FParameters* PassParameters = GraphBuilder.AllocParameters<FDrawTracedTexturePS::FParameters>();
		PassParameters->SourceTexture = TryRegisterExternalTexture(GraphBuilder, CreateRenderTarget(TraceTexture, TEXT("SDVisCollision.TracedTextureSrc")));
		PassParameters->InvViewport = FVector2f(1.0f / DestSize.X, 1.0f / DestSize.Y);
		PassParameters->View = ViewFamily.Views[0]->ViewUniformBuffer;
		PassParameters->RenderTargets[0] = FRenderTargetBinding(ViewFamilyTexture, ERenderTargetLoadAction::ENoAction);
		
		FDrawTracedTexturePS::FPermutationDomain PermutationVector;
		TShaderMapRef<FDrawTracedTexturePS> PixelShader(GlobalShaderMap, PermutationVector);

		FPixelShaderUtils::AddFullscreenPass(
			GraphBuilder,
			GlobalShaderMap,
			RDG_EVENT_NAME("SDCollisionVis::UpdateFromRenderThread"),
			PixelShader,
			PassParameters,
			FIntRect(0, 0, DestSize.X, DestSize.Y)
		);
	}

}


////////////////////////////////////
//////          Offline           //
////////////////////////////////////

static void LogInfoMessageKey(uint64 Key, const FString& Payload, const float TimeOnScreen=1.0f)
{
	if (GAreScreenMessagesEnabled && GEngine)
	{
		GEngine->AddOnScreenDebugMessage(Key, TimeOnScreen, FColor::Magenta, FString::Printf(TEXT("SDCollisionVis - %s"), *Payload));
	}
	UE_LOG(LogSDCollisionVis, Display, TEXT("%s"), *Payload);
}


static void RenderOfflineCollision(FSDOfflineCollisionSettings Settings)
{
	auto CreateViewMatrices = [](FVector RayOrigin, FRotator RayRotator, int32 Resolution, FMatrix CubemapRotation)
	{
		FViewMatrices::FMinimalInitializer ViewMatricesInit;
		ViewMatricesInit.ViewOrigin = RayOrigin;
		ViewMatricesInit.ViewRotationMatrix = FInverseRotationMatrix(RayRotator);

		// Random 90deg rotation that seems to be the done thing.
		ViewMatricesInit.ViewRotationMatrix = ViewMatricesInit.ViewRotationMatrix * FMatrix(
				FPlane(0, 0, 1, 0),
				FPlane(1, 0, 0, 0),
				FPlane(0, 1, 0, 0),
				FPlane(0, 0, 0, 1));

		ViewMatricesInit.ViewRotationMatrix = ViewMatricesInit.ViewRotationMatrix * CubemapRotation;

		ViewMatricesInit.ProjectionMatrix = FReversedZPerspectiveMatrix(
			UE_PI * 0.25,      //< 90 degrees FOV
			(float)Resolution,
			(float)Resolution,
			4.0f              //< MinZ
		);
		ViewMatricesInit.ConstrainedViewRect = FIntRect(0, 0, Resolution, Resolution);
		
		return FViewMatrices(ViewMatricesInit);
	};
	
	TArray<TSharedPtr<FRenderBuffer>> RenderBuffers;
	TArray<TSharedPtr<FPerspectiveRenderer>> PerspectiveRenderers;

	if (Settings.bCubeMap)
	{
		Settings.MaxRaysPerFrame = FMath::Max(1, Settings.MaxRaysPerFrame / 6);
	
		// Dealing with unreals man lying down cubemaps is rather confusing and painful
		// (https://dev.epicgames.com/documentation/en-us/unreal-engine/creating-cubemaps)
		// Mercifully, UMoviePipelineImagePassBase::CalcCubeFaceTransform provides a good
		// reference for how things should end up.
		auto MakeCubemapBasis = [](FVector Dir, FVector Up)
		{
			// Hand-wavey matrix to make the center of the cubemap point directly forward when previewing.
			const FMatrix BasisCorrectionMatrix(FPlane( 1,  0,  0,  0),
												FPlane( 0,  0,  1,  0),
												FPlane( 0, -1,  0,  0),
												FPlane( 0,  0,  0,  1));
			FVector Right = Up ^ Dir;
			return BasisCorrectionMatrix * FBasisVectorMatrix(Right, Up, Dir, FVector::Zero());
		};
		
		const FMatrix BasisRotations[6]
		{
			MakeCubemapBasis( FVector::XAxisVector,  FVector::YAxisVector),	// +X
			MakeCubemapBasis(-FVector::XAxisVector,  FVector::YAxisVector),	// -X
			MakeCubemapBasis( FVector::YAxisVector, -FVector::ZAxisVector),	// +Y
			MakeCubemapBasis(-FVector::YAxisVector,  FVector::ZAxisVector),	// -Y
			MakeCubemapBasis( FVector::ZAxisVector,  FVector::YAxisVector),	// +Z
			MakeCubemapBasis(-FVector::ZAxisVector,  FVector::YAxisVector),	// -Z
		};

		for (int32 i = 0; i < 6; ++i)
		{
			FViewMatrices ViewMatrices = CreateViewMatrices(Settings.RayOrigin, Settings.RayRotator, Settings.Resolution, BasisRotations[i]);
			TSharedPtr<FRenderBuffer> Buffer = MakeShared<FRenderBuffer>();
			Buffer->Init({ Settings.Resolution, Settings.Resolution });
			TSharedPtr<FPerspectiveRenderer> PerspectiveRenderer = MakeShared<FPerspectiveRenderer>(Settings.World, *Buffer, Settings, Settings.RayOrigin, ViewMatrices);

			RenderBuffers.Add(Buffer);
			PerspectiveRenderers.Add(PerspectiveRenderer);
		}

		// Use a consistent forward vector, so things don't look super weird between slices
		for (int32 i = 1; i < 6; ++i)
		{
			PerspectiveRenderers[i]->RevViewForward = PerspectiveRenderers[0]->RevViewForward;
		}
	}
	else
	{
		FViewMatrices ViewMatrices = CreateViewMatrices(Settings.RayOrigin, Settings.RayRotator, Settings.Resolution, FMatrix::Identity);

		TSharedPtr<FRenderBuffer> Buffer = MakeShared<FRenderBuffer>();
		Buffer->Init({ Settings.Resolution, Settings.Resolution });
		TSharedPtr<FPerspectiveRenderer> PerspectiveRenderer = MakeShared<FPerspectiveRenderer>(Settings.World, *Buffer, Settings, Settings.RayOrigin, ViewMatrices);

		RenderBuffers.Add(Buffer);
		PerspectiveRenderers.Add(PerspectiveRenderer);
	}

	FKernelExecutor Executor
	{
		.VisType = Settings.VisType
	};

	uint64 MaxIterations = FMath::DivideAndRoundUp(	uint64(Settings.Resolution) * uint64(Settings.Resolution),
													uint64(Settings.MaxRaysPerFrame));

	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
	[
		Executor=FKernelExecutor{ .VisType = Settings.VisType },
		RenderBuffers=MoveTemp(RenderBuffers),
		PerspectiveRenderers=MoveTemp(PerspectiveRenderers),
		Iteration=uint64(0),
		MaxIterations=MaxIterations,
		Settings=Settings,
		TraceTask=FGraphEventRef(),
		LogKey = uint64(FMath::Rand())
	]
	(float) mutable
	{
		if (TraceTask)
		{
			FTaskGraphInterface::Get().WaitUntilTaskCompletes(TraceTask);
		}

		if (!IsValid(Settings.World))
		{
			LogInfoMessageKey(LogKey, TEXT("World has gone out of scope! Bailing!"));
			return false;
		}

		LogInfoMessageKey(	LogKey,
							FString::Printf(TEXT("%02.02f%% [%d / %d]"),
											(100.0f * Iteration) / MaxIterations,
											Iteration,
											MaxIterations));

		// Hooray we're done
		if (Iteration == MaxIterations)
		{
			FString OutDir = FPaths::Combine(FPaths::ProjectDir(), TEXT("Saved"), TEXT("SDCollisionVis"));
			if (!IFileManager::Get().DirectoryExists(*OutDir))
			{
				IFileManager::Get().MakeDirectory(*OutDir, true);
			}

			FString MapName;
			if (ULevel* Level = Settings.World->GetCurrentLevel())
			{
				MapName = Level->GetOutermost()->GetName();
				if (MapName.Contains(TEXT("/")))
				{
					MapName.Split(TEXT("/"), nullptr, &MapName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
				}
			}
			if (MapName.IsEmpty())
			{
				MapName = TEXT("UnknownMap");
			}

			bool bFileWritten = false;
			FString OutFile;
			
			if (!Settings.bCubeMap)
			{
				FFileHelper::GenerateDateTimeBasedBitmapFilename(OutDir / MapName, TEXT("png"), OutFile);

				const FRenderBuffer& Buffer = *RenderBuffers[0];
				FImageView Data(Buffer.PixelData.GetData(), Settings.Resolution, Settings.Resolution);
				FImageUtils::SaveImageByExtension(*OutFile, Data);
				bFileWritten = true;
			}
			else
			{
				FFileHelper::GenerateDateTimeBasedBitmapFilename(OutDir / (MapName + TEXT("_cubemap")), TEXT("dds"), OutFile);

				UE::DDS::EDDSError Error;
				UE::DDS::FDDSFile* DDS = UE::DDS::FDDSFile::CreateEmpty(/* Dimensions*/     2,
																		/* InWidth */       Settings.Resolution,
																		/* InHeight */      Settings.Resolution,
																		/* InDepth */       1,
																		/* InMipCount */    1,
																		/* ArraySize */     6,
																		/* InFormat */      UE::DDS::EDXGIFormat::B8G8R8A8_UNORM_SRGB,
																		/* InCreateFlags */ UE::DDS::FDDSFile::CREATE_FLAG_CUBEMAP,
																		/* OutError */      &Error);
				if ( DDS == nullptr || Error != UE::DDS::EDDSError::OK )
				{
					LogInfoMessageKey(	LogKey,
										FString::Printf(TEXT("Failed to save cubemap! FDDSFile::CreateEmpty (Error=%d)"),
														(int)Error));
				}
				else
				{
					TUniquePtr<UE::DDS::FDDSFile> DeleteOnExit(DDS);
					for(int32 Face = 0; Face < 6; Face++)
					{
						FImageView Data(	RenderBuffers[Face]->PixelData.GetData(),
											Settings.Resolution,
											Settings.Resolution);
						DDS->FillMip( Data, Face );
					}

					TArray64<uint8> BytesToWrite;
					check(DDS->WriteDDS(BytesToWrite) == UE::DDS::EDDSError::OK);

					if (FArchive* FileHandle = IFileManager::Get().CreateFileWriter(*OutFile))
					{
						FileHandle->Serialize(BytesToWrite.GetData(), BytesToWrite.Num());
						FileHandle->Close();
						delete FileHandle;
					}
					bFileWritten = true;
				}
			}

			if (bFileWritten)
			{
				LogInfoMessageKey(	LogKey,
									FString::Printf(TEXT("Written to: %s"),
									*FPaths::ConvertRelativePathToFull(OutFile)));
			}

			return false;
		}

		TFunction<void()> TraceFunc = [	&PerspectiveRenderers,
										Executor,
										MaxRaysPerFrame=Settings.MaxRaysPerFrame,
										Resolution=Settings.Resolution,
										Iteration=Iteration++]
		{
			// NB: We don't care about sampling pattern, since we just stride stuff out
			Executor.Dispatch<	TKernelDispatchParameters<>,
								EKD_VisType>([&](auto DispatchParameters)
			{
				const static EVisualisationType VisType = decltype(DispatchParameters)::VisType;

				ParallelFor(MaxRaysPerFrame, [&](int32 Offset)
				{
					// TODO: Fully linear tiling is a bit crap, since whats on screen can change
					//       (e.g, the bottom half of the screen would change as a player moves)
					//       I'm sure there's some sort of stochastically stable way to dither it.
					//       Could do something like:
					//          PixelOffset = (PixelOffset * p) % NumPixels;
					//       Where p is a large prime number, although that would create a white
					//       noise pattern.
					uint64 PixelOffset = MaxRaysPerFrame * Iteration + Offset;
					FIntPoint PixelPos = FIntPoint(PixelOffset % Resolution, PixelOffset / Resolution);
					for (const auto& PerspectiveRenderer : PerspectiveRenderers)
					{
						PerspectiveRenderer->RenderPerspectivePixel<VisType>(PixelPos);
					}
				});
			});
		};

		TraceTask = FFunctionGraphTask::CreateAndDispatchWhenReady(MoveTemp(TraceFunc), TStatId(), nullptr);
		return true;
	}));
}

static void DeriveTransformFromWorld(FVector& RayOrigin, FRotator& RayRotator, UWorld* World, int32 PlayerControllerIndex, TArray<FString> Messages)
{
	if (PlayerControllerIndex < 0)
	{
		return;
	}

	// Use the LevelEditingViewport instead of the player controller.
#if WITH_EDITOR
	if (World->WorldType == EWorldType::Editor)
	{
		if (GCurrentLevelEditingViewportClient)
		{
			RayOrigin = GCurrentLevelEditingViewportClient->GetViewLocation();
			RayRotator = GCurrentLevelEditingViewportClient->GetViewRotation();
		}
		else
		{
			Messages.Add(TEXT("| - ERR: Unable to resolve current level editing viewport!"));
		}
		return;
	}
#endif // WITH_EDITOR

	if (PlayerControllerIndex > World->GetNumPlayerControllers())
	{
		PlayerControllerIndex = World->GetNumPlayerControllers() - 1;
		Messages.Add(FString::Printf(TEXT("| - ERR: Unable to resolve input PlayerControllerIndex, trying to use %d"), PlayerControllerIndex));
	}

	APlayerController* FoundPlayerController = nullptr;
	int32 I = 0;
	for (FConstPlayerControllerIterator Iterator = World->GetPlayerControllerIterator(); Iterator; ++Iterator, ++I)
	{
		if (I == PlayerControllerIndex)
		{
			FoundPlayerController = Iterator->Get();
			break;
		}
	}

	if (!FoundPlayerController)
	{
		Messages.Add(FString::Printf(TEXT("| - ERR: Couldn't resolve, trying to use first player controller!")));
		FoundPlayerController = World->GetFirstPlayerController();
	}

	if (!FoundPlayerController)
	{
		Messages.Add(FString::Printf(TEXT("| - ERR: No PlayerControllerIndex was resolved!")));
		return;
	}

	if (APlayerCameraManager* CameraManager = FoundPlayerController->PlayerCameraManager)
	{
		RayOrigin = CameraManager->GetCameraLocation();
		RayRotator = CameraManager->GetCameraRotation();
	}
	else
	{
		if (APawn* Pawn = FoundPlayerController->GetPawn())
		{
			Messages.Add(FString::Printf(TEXT("| - ERR: Player controller didn't have a camera manager, falling back to pawn!")));
			RayOrigin = Pawn->GetActorLocation();
			RayRotator = Pawn->GetActorRotation();
		}
		else
		{
			Messages.Add(FString::Printf(TEXT("| - ERR: Player controller pawn couldn't be resolved!")));
		}
	}
}

static FAutoConsoleCommandWithWorldAndArgs ConsoleCommandOfflineRender(
	TEXT("r.SDCollisionVis.OfflineRender()"),
	TEXT("Render the phys scene and save the result")
	TEXT("Args:\n")
	TEXT("    -resolution         : Resolution to use. (Default: 512)\n")
	TEXT("    -max-rays-per-frame : Number of rays to dispatch per frame. (Default: 1024)\n")
	TEXT("    -cubemap            : Render as a CubeMap. (Default: false)\n")
	TEXT("    -player-controller  : Player controller for fetching transform info. (Default: 0)\n")
	,
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
	{
		check(World);

		FSDOfflineCollisionSettings Settings;
		Settings.World = World;

		FString Params = FString::Join(Args, TEXT(" "));
		Settings.Resolution = 512;             FParse::Value(*Params, TEXT("resolution="), Settings.Resolution);
		Settings.MaxRaysPerFrame = 1024;       FParse::Value(*Params, TEXT("max-rays-per-frame="), Settings.MaxRaysPerFrame);
		Settings.bCubeMap	=                  FParse::Param(*Params, TEXT("cubemap"));
		
		int32 PlayerControllerIndex = 0;    FParse::Value(*Params, TEXT("player-controller="), PlayerControllerIndex);
		
		Settings.Resolution = FMath::Clamp(Settings.Resolution, 32, 8192);
		Settings.MaxRaysPerFrame = FMath::Clamp(Settings.MaxRaysPerFrame, 4, (WITH_EDITOR) ? (1 << 16) : 4096);

		uint64 NumRays = (uint64)Settings.MaxRaysPerFrame * (uint64)Settings.MaxRaysPerFrame;
		if (Settings.bCubeMap)
		{
			NumRays *= 6llu;
		}

		TArray<FString> Messages;
		Messages.Add(FString::Printf(TEXT("WorldNetMode = %s"), *ToString(World->GetNetMode())));
		Messages.Add(FString::Printf(TEXT("Resolution = %d"), Settings.Resolution));
		Messages.Add(FString::Printf(TEXT("MaxRaysPerFrame = %d"), Settings.MaxRaysPerFrame));
		Messages.Add(FString::Printf(TEXT("|- NumRays = %llu"), NumRays));
		Messages.Add(FString::Printf(TEXT("bCubeMap = %d"), (int32)Settings.bCubeMap));
		Messages.Add(FString::Printf(TEXT("PlayerControllerIndex = %d"), PlayerControllerIndex));

		Settings.RayOrigin = FVector::Zero();
		Settings.RayRotator = FRotator::ZeroRotator;
		DeriveTransformFromWorld(Settings.RayOrigin, Settings.RayRotator, World, PlayerControllerIndex, Messages);

		Messages.Add(FString::Printf(TEXT("Location = %s"), *Settings.RayOrigin.ToString()));
		Messages.Add(FString::Printf(TEXT("Rotation = %s"), *Settings.RayRotator.ToString()));

		for (const FString& Message : Messages)
		{
			LogInfoMessageKey(INDEX_NONE, Message, 7.0f);
		}

		RenderOfflineCollision(Settings);
	}));

} // namespace SDCollisionVis

#undef LOCTEXT_NAMESPACE 
