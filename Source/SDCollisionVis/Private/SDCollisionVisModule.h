// Copyright Splash Damage, Ltd. All Rights Reserved.

#pragma once


#include <CoreMinimal.h>
#include <Modules/ModuleManager.h>
#include <Modules/ModuleInterface.h>
#include <Interfaces/IPluginManager.h>
#include <Containers/Ticker.h>


class FSceneViewFamily;

namespace SDCollisionVis
{

class FSDCollisionVisRealtimeViewExtension;
struct FSDCollisionVisRealtimeViewData;

} // SDCollisionVis

class FSDCollisionVisModule : public IModuleInterface
{
public:
	/** IModuleInterface implementation */
	virtual void StartupModule() override final;
	virtual void ShutdownModule() override final;

	TSharedPtr<SDCollisionVis::FSDCollisionVisRealtimeViewData> GetRealtimeViewFamilyData(FSceneViewFamily& ViewFamily);

private:
	void OnPostEngineInit();
	void OnEnginePreExit();

	// Stuff for realtime renderer
	FTSTicker::FDelegateHandle																PruneUnusedViewFamilies;
	TSharedPtr<SDCollisionVis::FSDCollisionVisRealtimeViewExtension, ESPMode::ThreadSafe>	ViewExtension;
	TMap<uint32, TSharedPtr<SDCollisionVis::FSDCollisionVisRealtimeViewData>>				ViewFamilyData;
};


DECLARE_LOG_CATEGORY_EXTERN(LogSDCollisionVis, Log, All);
