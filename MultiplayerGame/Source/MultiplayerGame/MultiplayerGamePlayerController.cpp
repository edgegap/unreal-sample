// Copyright Epic Games, Inc. All Rights Reserved.

#include "MultiplayerGamePlayerController.h"
#include "GameFramework/Pawn.h"
#include "Blueprint/AIBlueprintHelperLibrary.h"
#include "NiagaraSystem.h"
#include "NiagaraFunctionLibrary.h"
#include "MultiplayerGameCharacter.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "EdgegapMatchmakerSubsystem.h"
#include "Misc/CommandLine.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"

AMultiplayerGamePlayerController::AMultiplayerGamePlayerController()
{
	bShowMouseCursor = true;
	DefaultMouseCursor = EMouseCursor::Default;
}

void AMultiplayerGamePlayerController::BeginPlay()
{
	Super::BeginPlay();

	// The menu map runs standalone; once connected to a dedicated server the net mode is NM_Client.
	if (!IsLocalController() || GetNetMode() != NM_Standalone)
	{
		return;
	}

	UEdgegapMatchmakerSubsystem* Matchmaker = GetGameInstance()->GetSubsystem<UEdgegapMatchmakerSubsystem>();
	if (!Matchmaker)
	{
		return;
	}
	Matchmaker->NotifyBackOnMenu();
	ShowMatchmakingWidget();

	// -FindMatch on the command line queues once at startup, for unattended tests with several clients.
	static bool bAutoQueued = false;
	if (!bAutoQueued && FParse::Param(FCommandLine::Get(), TEXT("FindMatch")))
	{
		bAutoQueued = true;
		Matchmaker->FindMatch();
	}
}

void AMultiplayerGamePlayerController::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (MatchmakingWidget.IsValid() && GEngine && GEngine->GameViewport)
	{
		GEngine->GameViewport->RemoveViewportWidgetContent(MatchmakingWidget.ToSharedRef());
	}
	MatchmakingWidget.Reset();

	Super::EndPlay(EndPlayReason);
}

void AMultiplayerGamePlayerController::FindMatch()
{
	if (UEdgegapMatchmakerSubsystem* Matchmaker = GetGameInstance()->GetSubsystem<UEdgegapMatchmakerSubsystem>())
	{
		Matchmaker->FindMatch();
	}
	else
	{
		ClientMessage(TEXT("Edgegap matchmaking is disabled. Set bEnabled=True in DefaultGame.ini, see README."));
	}
}

void AMultiplayerGamePlayerController::CancelMatch()
{
	if (UEdgegapMatchmakerSubsystem* Matchmaker = GetGameInstance()->GetSubsystem<UEdgegapMatchmakerSubsystem>())
	{
		Matchmaker->CancelMatch();
	}
}

void AMultiplayerGamePlayerController::ShowMatchmakingWidget()
{
	if (!GEngine || !GEngine->GameViewport)
	{
		return;
	}

	TWeakObjectPtr<UEdgegapMatchmakerSubsystem> Matchmaker = GetGameInstance()->GetSubsystem<UEdgegapMatchmakerSubsystem>();
	auto IsSearching = [Matchmaker]() { return Matchmaker.IsValid() && Matchmaker->GetState() == EMatchmakingState::Searching; };

	// Plain Slate keeps the sample free of new .uasset files; a UMG widget calling the same subsystem works too.
	MatchmakingWidget =
		SNew(SBox)
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Top)
		.Padding(FMargin(0.f, 40.f))
		[
			SNew(SBorder)
			.Padding(FMargin(20.f, 12.f))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				.HAlign(HAlign_Center)
				[
					SNew(STextBlock)
					.Text_Lambda([Matchmaker]() { return FText::FromString(Matchmaker.IsValid() ? Matchmaker->GetStatusText() : FString()); })
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.HAlign(HAlign_Center)
				.Padding(FMargin(0.f, 10.f, 0.f, 0.f))
				[
					SNew(SButton)
					.Text_Lambda([IsSearching]() { return FText::FromString(IsSearching() ? TEXT("Cancel") : TEXT("Find Match")); })
					.OnClicked_Lambda([Matchmaker, IsSearching]()
					{
						if (Matchmaker.IsValid())
						{
							IsSearching() ? Matchmaker->CancelMatch() : Matchmaker->FindMatch();
						}
						return FReply::Handled();
					})
				]
			]
		];

	GEngine->GameViewport->AddViewportWidgetContent(MatchmakingWidget.ToSharedRef(), 10);
}

void AMultiplayerGamePlayerController::PlayerTick(float DeltaTime)
{
	Super::PlayerTick(DeltaTime);

	if(bInputPressed)
	{
		FollowTime += DeltaTime;

		// Look for the touch location
		FVector HitLocation = FVector::ZeroVector;
		FHitResult Hit;
		if(bIsTouch)
		{
			GetHitResultUnderFinger(ETouchIndex::Touch1, ECC_Visibility, true, Hit);
		}
		else
		{
			GetHitResultUnderCursor(ECC_Visibility, true, Hit);
		}
		HitLocation = Hit.Location;

		// Direct the Pawn towards that location
		APawn* const MyPawn = GetPawn();
		if(MyPawn)
		{
			FVector WorldDirection = (HitLocation - MyPawn->GetActorLocation()).GetSafeNormal();
			MyPawn->AddMovementInput(WorldDirection, 1.f, false);
		}
	}
	else
	{
		FollowTime = 0.f;
	}
}

void AMultiplayerGamePlayerController::SetupInputComponent()
{
	// set up gameplay key bindings
	Super::SetupInputComponent();

	InputComponent->BindAction("SetDestination", IE_Pressed, this, &AMultiplayerGamePlayerController::OnSetDestinationPressed);
	InputComponent->BindAction("SetDestination", IE_Released, this, &AMultiplayerGamePlayerController::OnSetDestinationReleased);

	// support touch devices 
	InputComponent->BindTouch(EInputEvent::IE_Pressed, this, &AMultiplayerGamePlayerController::OnTouchPressed);
	InputComponent->BindTouch(EInputEvent::IE_Released, this, &AMultiplayerGamePlayerController::OnTouchReleased);

}

void AMultiplayerGamePlayerController::OnSetDestinationPressed()
{
	// We flag that the input is being pressed
	bInputPressed = true;
	// Just in case the character was moving because of a previous short press we stop it
	StopMovement();
}

void AMultiplayerGamePlayerController::OnSetDestinationReleased()
{
	// Player is no longer pressing the input
	bInputPressed = false;

	// If it was a short press
	if(FollowTime <= ShortPressThreshold)
	{
		// We look for the location in the world where the player has pressed the input
		FVector HitLocation = FVector::ZeroVector;
		FHitResult Hit;
		GetHitResultUnderCursor(ECC_Visibility, true, Hit);
		HitLocation = Hit.Location;

		// We move there and spawn some particles
		UAIBlueprintHelperLibrary::SimpleMoveToLocation(this, HitLocation);
		UNiagaraFunctionLibrary::SpawnSystemAtLocation(this, FXCursor, HitLocation, FRotator::ZeroRotator, FVector(1.f, 1.f, 1.f), true, true, ENCPoolMethod::None, true);
	}
}

void AMultiplayerGamePlayerController::OnTouchPressed(const ETouchIndex::Type FingerIndex, const FVector Location)
{
	bIsTouch = true;
	OnSetDestinationPressed();
}

void AMultiplayerGamePlayerController::OnTouchReleased(const ETouchIndex::Type FingerIndex, const FVector Location)
{
	bIsTouch = false;
	OnSetDestinationReleased();
}
