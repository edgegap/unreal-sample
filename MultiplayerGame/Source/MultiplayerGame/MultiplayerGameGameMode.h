// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "MultiplayerGameGameMode.generated.h"

UCLASS(minimalapi)
class AMultiplayerGameGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AMultiplayerGameGameMode();

	// Begin AGameModeBase interface
	virtual void BeginPlay() override;
	virtual void PostLogin(APlayerController* NewPlayer) override;
	virtual void Logout(AController* Exiting) override;
	// End AGameModeBase interface

private:
	/** Seconds a fresh server waits for its first player before stopping itself. */
	static constexpr float NoPlayerTimeoutSeconds = 300.f;

	/** Seconds the server stays up once empty, so a player who is still connecting can join. */
	static constexpr float EmptyServerGraceSeconds = 15.f;

	/** True when the Edgegap matchmaker started this server; only then does the server stop itself. */
	static bool IsMatchmakerDeployment();

	void CheckEmptyServer();

	/** Asks Edgegap to stop this deployment. An idle deployment keeps billing until its max duration. */
	void StopDeployment(const FString& Reason);

	FTimerHandle StopTimer;
	bool bStopRequested = false;
};



