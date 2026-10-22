// Copyright Edgegap. Licensed under the MIT License, see LICENSE at the repository root.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "EdgegapVoiceClientSubsystem.generated.h"

class IVoiceChat;
class IConsoleObject;

/**
 * Client side of the optional EOS Voice chat. Logs the player into EOS with a device ID (no Epic account
 * needed), hands the player's ID token to the game server, and joins the room the server returns.
 * Only created in game clients, on platforms with the EOSVoiceChat plugin, when
 * UEdgegapVoiceSettings::bEnabled is set.
 */
UCLASS()
class UEdgegapVoiceClientSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	DECLARE_DELEGATE_TwoParams(FOnVoiceIdentityReady, const FString& /* ProductUserId */, const FString& /* IdToken */);

	// Begin USubsystem interface
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	// End USubsystem interface

	/** Logs into EOS if needed, then calls back with a fresh ID token for the game server. */
	void RequestIdentity(FOnVoiceIdentityReady OnReady);

	/** Joins the room with the token the game server got for this player. */
	void JoinRoom(const FString& RoomName, const FString& ClientBaseUrl, const FString& ParticipantToken);

	/** Leaves every room, e.g. when the player leaves the server. */
	void LeaveRooms();

	/** Mutes or unmutes the microphone. Returns the new mute state. */
	bool ToggleMute();

private:
	void Login();
	void OnDeviceIdReady();
	void OnLoggedIn(const FString& ProductUserId);
	void FailLogin(const FString& Reason);
	FString CopyIdToken() const;
	void* GetConnectInterface() const;

	IVoiceChat* VoiceChat = nullptr;
	FString LocalProductUserId;
	TArray<FOnVoiceIdentityReady> PendingIdentityRequests;
	TArray<FString> JoinedRooms;
	bool bLoggingIn = false;
	bool bMuted = false;
	FDelegateHandle TalkingHandle;
	FDelegateHandle PlayerAddedHandle;
	IConsoleObject* MuteCommand = nullptr;
};
