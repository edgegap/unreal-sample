// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "EdgegapVoiceSettings.generated.h"

/**
 * Optional EOS Voice chat, disabled by default. Settings live in Config/DefaultGame.ini under
 * [/Script/MultiplayerGame.EdgegapVoiceSettings]; the EOS product and game client IDs go in
 * Config/DefaultEngine.ini under [EOSVoiceChat]. See README "EOS Voice chat (optional)".
 *
 * The game server is the trusted server: players send it their EOS ID token, it checks the token,
 * picks their room (whole match, or their matchmaker team) and asks the EOS Voice Web API for a room
 * token that only that player receives.
 */
UCLASS(config=Game)
class UEdgegapVoiceSettings : public UObject
{
	GENERATED_BODY()

public:
	/** Turns voice on in game clients. Off by default: the sample then works exactly as before. */
	UPROPERTY(config)
	bool bEnabled = false;

	/**
	 * One room per matchmaker team instead of one per match. Players the matchmaker didn't place
	 * (servers deployed by hand, local tests) always share the match room.
	 */
	UPROPERTY(config)
	bool bTeamRooms = true;

	/** Name players get when logging in with an EOS device ID. EOS requires one; nobody else sees it. */
	UPROPERTY(config)
	FString DeviceIdDisplayName = TEXT("Player");

	/**
	 * Credentials of the EOS client the game server uses (Voice: createRoomToken, kick, mute). For local
	 * tests only, from the git-ignored Config/UserGame.ini. On Edgegap, set the environment variables
	 * EOS_VOICE_CLIENT_ID and EOS_VOICE_CLIENT_SECRET on the app version instead; they take precedence.
	 * Never ship these in a game client: they can create tokens for any room.
	 */
	UPROPERTY(config)
	FString ServerClientId;

	UPROPERTY(config)
	FString ServerClientSecret;

	static const UEdgegapVoiceSettings& Get() { return *GetDefault<UEdgegapVoiceSettings>(); }
};
