// Copyright Splash Damage, Ltd. All Rights Reserved.


#include "SDCollisionVisModule.h"
#include "SDCollisionVisRenderer.h"

#include <Interfaces/IPluginManager.h>
#include <Modules/ModuleManager.h>
#include <ShaderCore.h>


#define LOCTEXT_NAMESPACE "SDCollisionVis"

DEFINE_LOG_CATEGORY(LogSDCollisionVis);

IMPLEMENT_MODULE( FSDCollisionVisModule, SDCollisionVis )


void FSDCollisionVisModule::StartupModule()
{
	FString PluginShaderDir = FPaths::Combine(IPluginManager::Get().FindPlugin(TEXT("SDCollisionVis"))->GetBaseDir(), TEXT("Shaders"));
	AddShaderSourceDirectoryMapping(TEXT("/Plugin/SDCollisionVis"), PluginShaderDir);

	FCoreDelegates::OnPostEngineInit.AddRaw(this, &FSDCollisionVisModule::OnPostEngineInit);
	FCoreDelegates::OnEnginePreExit.AddRaw(this, &FSDCollisionVisModule::OnEnginePreExit);

}

void FSDCollisionVisModule::ShutdownModule()
{
	FCoreDelegates::OnPostEngineInit.RemoveAll(this);
	FCoreDelegates::OnEnginePreExit.RemoveAll(this);
}

void FSDCollisionVisModule::OnPostEngineInit()
{
	ViewExtension = FSceneViewExtensions::NewExtension<SDCollisionVis::FSDCollisionVisRealtimeViewExtension>();

	// Internally, we keep an association of ViewKey => Data, if the data hasn't been accessed for a while free the
	// memory backing it.
	if (FApp::CanEverRender())
	{
		PruneUnusedViewFamilies = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([this](float)
		{
			constexpr uint64 KeepAliveFrames = 30;
			for (auto It = ViewFamilyData.CreateIterator(); It; ++It)
			{
				if ((GFrameCounter - It->Value->LastAccessed) > KeepAliveFrames)
				{
					It.RemoveCurrent();
				}
			}
			return true;
		}));
	}
}

void FSDCollisionVisModule::OnEnginePreExit()
{
	FTSTicker::GetCoreTicker().RemoveTicker(PruneUnusedViewFamilies);
	PruneUnusedViewFamilies.Reset();
	ViewFamilyData.Empty();
	ViewExtension.Reset();
}


TSharedPtr<SDCollisionVis::FSDCollisionVisRealtimeViewData> FSDCollisionVisModule::GetRealtimeViewFamilyData(FSceneViewFamily& ViewFamily)
{
	check(IsInGameThread());

	if (ViewFamily.Views.IsEmpty())
	{
		return {};
	}

	uint32 ViewKey = ViewFamily.Views[0]->GetViewKey();
	TSharedPtr<SDCollisionVis::FSDCollisionVisRealtimeViewData> Data;
	if (TSharedPtr<SDCollisionVis::FSDCollisionVisRealtimeViewData>* Found = ViewFamilyData.Find(ViewKey))
	{
		Data = *Found;
	}
	else
	{
		Data = MakeShared<SDCollisionVis::FSDCollisionVisRealtimeViewData>();
		ViewFamilyData.Add(ViewKey, Data);
	}

	Data->LastAccessed = GFrameCounter;
	return Data;
}


#undef LOCTEXT_NAMESPACE
