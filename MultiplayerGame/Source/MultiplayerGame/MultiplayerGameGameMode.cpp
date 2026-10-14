// Copyright Epic Games, Inc. All Rights Reserved.

#include "MultiplayerGameGameMode.h"
#include "MultiplayerGamePlayerController.h"
#include "MultiplayerGameCharacter.h"
#include "MultiplayerGame.h"
#include "UObject/ConstructorHelpers.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "HAL/PlatformMisc.h"
#include "Misc/App.h"
#include "TimerManager.h"

AMultiplayerGameGameMode::AMultiplayerGameGameMode()
{
	// use our custom PlayerController class
	PlayerControllerClass = AMultiplayerGamePlayerController::StaticClass();

	// set default pawn class to our Blueprinted character
	static ConstructorHelpers::FClassFinder<APawn> PlayerPawnBPClass(TEXT("/Game/TopDown/Blueprints/BP_TopDownCharacter"));
	if (PlayerPawnBPClass.Class != nullptr)
	{
		DefaultPawnClass = PlayerPawnBPClass.Class;
	}

	// set default controller to our Blueprinted controller
	static ConstructorHelpers::FClassFinder<APlayerController> PlayerControllerBPClass(TEXT("/Game/TopDown/Blueprints/BP_TopDownPlayerController"));
	if(PlayerControllerBPClass.Class != NULL)
	{
		PlayerControllerClass = PlayerControllerBPClass.Class;
	}
}

void AMultiplayerGameGameMode::BeginPlay()
{
	Super::BeginPlay();

	if (GetNetMode() == NM_DedicatedServer && IsMatchmakerDeployment())
	{
		UE_LOG(LogMultiplayerGame, Log, TEXT("Started by the Edgegap matchmaker: the server stops itself when the match is empty"));
		GetWorldTimerManager().SetTimer(StopTimer, FTimerDelegate::CreateWeakLambda(this, [this]() { StopDeployment(TEXT("no player joined")); }),
			NoPlayerTimeoutSeconds, false);
	}
}

void AMultiplayerGameGameMode::PostLogin(APlayerController* NewPlayer)
{
	Super::PostLogin(NewPlayer);
	GetWorldTimerManager().ClearTimer(StopTimer);
}

void AMultiplayerGameGameMode::Logout(AController* Exiting)
{
	Super::Logout(Exiting);

	// The leaving controller still counts here, so check once it is gone.
	if (GetNetMode() == NM_DedicatedServer && IsMatchmakerDeployment())
	{
		GetWorldTimerManager().SetTimer(StopTimer, this, &AMultiplayerGameGameMode::CheckEmptyServer, EmptyServerGraceSeconds, false);
	}
}

bool AMultiplayerGameGameMode::IsMatchmakerDeployment()
{
	// The matchmaker injects MM_* variables; servers deployed by hand or run locally don't have them,
	// so they keep running until stopped from the dashboard, like before.
	return !FPlatformMisc::GetEnvironmentVariable(TEXT("MM_MATCH_ID")).IsEmpty();
}

void AMultiplayerGameGameMode::CheckEmptyServer()
{
	if (GetNumPlayers() == 0)
	{
		StopDeployment(TEXT("all players left"));
	}
}

void AMultiplayerGameGameMode::StopDeployment(const FString& Reason)
{
	if (bStopRequested)
	{
		return;
	}

	// Edgegap injects these into every deployment; they are absent when the server runs locally.
	const FString DeleteUrl = FPlatformMisc::GetEnvironmentVariable(TEXT("ARBITRIUM_DELETE_URL"));
	const FString DeleteToken = FPlatformMisc::GetEnvironmentVariable(TEXT("ARBITRIUM_DELETE_TOKEN"));
	if (DeleteUrl.IsEmpty() || DeleteToken.IsEmpty())
	{
		UE_LOG(LogMultiplayerGame, Log, TEXT("Server would stop (%s), but it is not running on Edgegap"), *Reason);
		return;
	}

	bStopRequested = true;
	UE_LOG(LogMultiplayerGame, Log, TEXT("Stopping deployment: %s"), *Reason);

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(DeleteUrl);
	Request->SetVerb(TEXT("DELETE"));
	Request->SetHeader(TEXT("Authorization"), DeleteToken);
	Request->OnProcessRequestComplete().BindLambda([](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
	{
		const int32 Code = Response.IsValid() ? Response->GetResponseCode() : 0;
		UE_LOG(LogMultiplayerGame, Log, TEXT("Self-stop request returned %d"), Code);

		// On success Edgegap sends SIGTERM shortly; if the request failed, exit anyway so the match ends.
		if (!bConnected || Code < 200 || Code >= 300)
		{
			FPlatformMisc::RequestExit(false);
		}
	});
	Request->ProcessRequest();
}
