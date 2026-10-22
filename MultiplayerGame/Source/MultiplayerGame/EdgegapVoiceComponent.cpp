// Copyright Edgegap. Licensed under the MIT License, see LICENSE at the repository root.

#include "EdgegapVoiceComponent.h"
#include "EdgegapVoiceClientSubsystem.h"
#include "EdgegapVoiceServerSubsystem.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "TimerManager.h"

UEdgegapVoiceComponent::UEdgegapVoiceComponent()
{
	SetIsReplicatedByDefault(true);
}

bool UEdgegapVoiceComponent::IsLocalClient() const
{
	const APlayerController* PC = Cast<APlayerController>(GetOwner());
	return PC && PC->IsLocalController() && GetNetMode() == NM_Client;
}

void UEdgegapVoiceComponent::BeginPlay()
{
	Super::BeginPlay();

	// The component reaches the owning client once the server added it: ask for a voice room.
	if (!IsLocalClient())
	{
		return;
	}
	if (UEdgegapVoiceClientSubsystem* Voice = GetWorld()->GetGameInstance()->GetSubsystem<UEdgegapVoiceClientSubsystem>())
	{
		Voice->RequestIdentity(UEdgegapVoiceClientSubsystem::FOnVoiceIdentityReady::CreateWeakLambda(this,
			[this](const FString& ProductUserId, const FString& IdToken)
			{
				// Never in the frame the component arrived: the server can't resolve it yet and drops the
				// connection ("Unable to resolve default guid"). That happens when the EOS login finished first.
				GetWorld()->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(this, [this, ProductUserId, IdToken]()
				{
					ServerRegisterVoice(ProductUserId, IdToken);
				}));
			}));
	}
}

void UEdgegapVoiceComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (IsLocalClient())
	{
		if (UEdgegapVoiceClientSubsystem* Voice = GetWorld()->GetGameInstance()->GetSubsystem<UEdgegapVoiceClientSubsystem>())
		{
			Voice->LeaveRooms();
		}
	}
	Super::EndPlay(EndPlayReason);
}

void UEdgegapVoiceComponent::ServerRegisterVoice_Implementation(const FString& ProductUserId, const FString& IdToken)
{
	if (UEdgegapVoiceServerSubsystem* Voice = GetWorld()->GetGameInstance()->GetSubsystem<UEdgegapVoiceServerSubsystem>())
	{
		Voice->RegisterPlayer(Cast<APlayerController>(GetOwner()), ProductUserId, IdToken);
	}
}

void UEdgegapVoiceComponent::ClientJoinVoiceRoom_Implementation(const FString& RoomName, const FString& ClientBaseUrl, const FString& ParticipantToken)
{
	if (UEdgegapVoiceClientSubsystem* Voice = GetWorld()->GetGameInstance()->GetSubsystem<UEdgegapVoiceClientSubsystem>())
	{
		Voice->JoinRoom(RoomName, ClientBaseUrl, ParticipantToken);
	}
}
