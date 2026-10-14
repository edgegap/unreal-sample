// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Interfaces/IHttpRequest.h"
#include "EdgegapMatchmakerSubsystem.generated.h"

enum class EMatchmakingState : uint8
{
	Idle,
	Searching,
	Connecting,
	Failed
};

/**
 * Optional client side of Edgegap matchmaking: measures latency to Edgegap's ping beacons, creates a
 * ticket with those latencies, polls it until a server is assigned, then travels to that server. Disabled by default; settings live in Config/DefaultGame.ini under
 * [/Script/MultiplayerGame.EdgegapMatchmakerSubsystem]. When disabled the subsystem is never created.
 */
UCLASS(config=Game)
class UEdgegapMatchmakerSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	// Begin USubsystem interface
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Deinitialize() override;
	// End USubsystem interface

	/** Creates a ticket and starts polling. Does nothing if already searching. */
	void FindMatch();

	/** Deletes the current ticket and stops polling. */
	void CancelMatch();

	/** Called when the client is back on the menu map, e.g. after the server closed or the connection failed. */
	void NotifyBackOnMenu();

	EMatchmakingState GetState() const { return State; }
	const FString& GetStatusText() const { return StatusText; }

	/** Turns matchmaking on. Off by default: the sample then works as before, connecting with "open <ip:port>". */
	UPROPERTY(config)
	bool bEnabled = false;

	/** Matchmaker API URL from the dashboard, e.g. https://om-xxxx.edgegap.net */
	UPROPERTY(config)
	FString MatchmakerUrl;

	/** Matchmaker auth token from the dashboard. Safe to ship in clients: it grants no Edgegap API access. */
	UPROPERTY(config)
	FString AuthToken;

	/** Profile name from the matchmaker config. */
	UPROPERTY(config)
	FString Profile = TEXT("casual");

	/** Port name from the app version on Edgegap. */
	UPROPERTY(config)
	FString PortName = TEXT("gameport");

	UPROPERTY(config)
	float PollIntervalSeconds = 2.f;

	/**
	 * Sends the player's latency to each beacon with the ticket, for a "latencies" rule in the matchmaker
	 * profile. Turn off if the profile has no such rule: the matchmaker rejects attributes it doesn't expect.
	 */
	UPROPERTY(config)
	bool bSendBeaconLatencies = true;

	/** Timeout for one ping to one beacon. Unreachable beacons are left out of the ticket. */
	UPROPERTY(config)
	float BeaconPingTimeoutSeconds = 1.f;

private:
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> MakeRequest(const FString& Verb, const FString& Path) const;
	void OnBeaconsReceived(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnected, int32 ForSearch);
	void CreateTicket(const TMap<FString, float>& BeaconLatencies);
	void OnTicketCreated(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnected);
	void PollTicket();
	void OnTicketPolled(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnected);
	void DeleteTicket();
	void StopPolling();
	void Fail(const FString& Reason);
	void SetState(EMatchmakingState NewState, const FString& NewStatusText);

	EMatchmakingState State = EMatchmakingState::Idle;
	FString StatusText = TEXT("Press Find Match");
	FString TicketId;
	/** Increments on every FindMatch and CancelMatch, so late beacon results from an old search are ignored. */
	int32 SearchId = 0;
	FTimerHandle PollTimer;
	bool bPollInFlight = false;
};
