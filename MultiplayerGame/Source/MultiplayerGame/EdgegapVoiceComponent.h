// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "EdgegapVoiceComponent.generated.h"

/**
 * Carries the optional EOS Voice handshake between a player and the dedicated server. The server adds
 * one to each PlayerController when the player logs in (UEdgegapVoiceServerSubsystem), so the game's own
 * classes need no changes. It replicates to the owning client only, which then sends its EOS identity
 * and receives its room token.
 */
UCLASS()
class UEdgegapVoiceComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UEdgegapVoiceComponent();

	/** The client proves its EOS identity; the server checks it and picks the room. */
	UFUNCTION(Server, Reliable)
	void ServerRegisterVoice(const FString& ProductUserId, const FString& IdToken);

	/** The server sends this player, and only this player, its room token. */
	UFUNCTION(Client, Reliable)
	void ClientJoinVoiceRoom(const FString& RoomName, const FString& ClientBaseUrl, const FString& ParticipantToken);

protected:
	// Begin UActorComponent interface
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	// End UActorComponent interface

private:
	bool IsLocalClient() const;
};
