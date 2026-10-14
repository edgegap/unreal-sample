// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Templates/SubclassOf.h"
#include "GameFramework/PlayerController.h"
#include "MultiplayerGamePlayerController.generated.h"

/** Forward declaration to improve compiling times */
class UNiagaraSystem;
class SWidget;

UCLASS()
class AMultiplayerGamePlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	AMultiplayerGamePlayerController();

	/** Time Threshold to know if it was a short press */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = Input)
	float ShortPressThreshold;

	/** FX Class that we will spawn when clicking */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = Input)
	UNiagaraSystem* FXCursor;

	/** Console command: queue for a match on Edgegap. */
	UFUNCTION(Exec)
	void FindMatch();

	/** Console command: leave the matchmaking queue. */
	UFUNCTION(Exec)
	void CancelMatch();

protected:
	/** True if the controlled character should navigate to the mouse cursor. */
	uint32 bMoveToMouseCursor : 1;

	// Begin PlayerController interface
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void PlayerTick(float DeltaTime) override;
	virtual void SetupInputComponent() override;
	// End PlayerController interface

	/** Input handlers for SetDestination action. */
	void OnSetDestinationPressed();
	void OnSetDestinationReleased();
	void OnTouchPressed(const ETouchIndex::Type FingerIndex, const FVector Location);
	void OnTouchReleased(const ETouchIndex::Type FingerIndex, const FVector Location);

private:
	/** Adds the Find Match / Cancel overlay on the menu map (standalone only, never on a server). */
	void ShowMatchmakingWidget();

	TSharedPtr<SWidget> MatchmakingWidget;

	bool bInputPressed; // Input is bring pressed
	bool bIsTouch; // Is it a touch device
	float FollowTime; // For how long it has been pressed
};


