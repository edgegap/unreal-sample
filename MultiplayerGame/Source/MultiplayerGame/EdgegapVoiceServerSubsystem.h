// Copyright Edgegap. Licensed under the MIT License, see LICENSE at the repository root.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Interfaces/IHttpRequest.h"
#include "EdgegapVoiceServerSubsystem.generated.h"

class APlayerController;
class FJsonObject;

/**
 * Server side of the optional EOS Voice chat: the dedicated server is the trusted server that decides who
 * talks to whom. It checks each player's EOS ID token, puts the player in a room (the match, or their
 * matchmaker team), asks the EOS Voice Web API for that player's room token and sends it only to them.
 * It also removes players from voice when they leave and can mute them for everyone.
 *
 * Uses the Web API over HTTPS rather than the EOS SDK because EOS Voice doesn't support Linux, which is
 * what the server runs on. Created on dedicated servers when UEdgegapVoiceSettings::bEnabled is set or
 * the EOS_VOICE_CLIENT_ID environment variable is present.
 */
UCLASS()
class UEdgegapVoiceServerSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	// Begin USubsystem interface
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	// End USubsystem interface

	/** A player asks for voice, through their UEdgegapVoiceComponent. */
	void RegisterPlayer(APlayerController* Player, const FString& ProductUserId, const FString& IdToken);

	/** Removes the player from their room and revokes their token, so it can't be used to rejoin. Called on logout. */
	void RemovePlayer(APlayerController* Player);

	/** Mutes or unmutes the player for everyone in their room, whatever their own mute setting is. */
	void SetHardMuted(APlayerController* Player, bool bMuted);

private:
	struct FVoicePlayer
	{
		FString ProductUserId;
		FString Room;
	};

	using FOnAccessToken = TFunction<void(const FString& /* AccessToken, empty on failure */)>;
	using FOnKeys = TFunction<void(bool /* bSuccess */)>;

	bool VerifyIdToken(const FString& IdToken, const FString& ExpectedProductUserId, FString& OutError) const;
	FString PickRoom(APlayerController* Player, FString& OutClientIp) const;
	void WithAccessToken(FOnAccessToken OnToken);
	void WithSigningKeys(const FString& KeyId, FOnKeys OnKeys);
	void RequestRoomToken(TWeakObjectPtr<APlayerController> Player, const FString& ProductUserId, const FString& Room, const FString& ClientIp);
	void CallRoomApi(const FString& Verb, const FString& Path, const FString& Body, TFunction<void(int32, const FString&)> OnDone);

	FString ClientId;
	FString ClientSecret;
	FString DeploymentId;
	/** [EOSVoiceChat] ClientId: the game client that ID tokens must have been issued to. */
	FString GameClientId;
	FString MatchRoom;

	FString AccessToken;
	double AccessTokenExpiresAt = 0.0;
	TArray<FOnAccessToken> PendingAccessToken;

	/** Epic's ID token signing keys by key ID: modulus and exponent, base64url. */
	TMap<FString, TPair<FString, FString>> SigningKeys;
	TArray<FOnKeys> PendingKeys;

	TMap<TWeakObjectPtr<APlayerController>, FVoicePlayer> Players;
};
