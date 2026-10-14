// Copyright Epic Games, Inc. All Rights Reserved.

#include "EdgegapMatchmakerSubsystem.h"
#include "MultiplayerGame.h"
#include "Engine/GameInstance.h"
#include "GameFramework/PlayerController.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Async/Async.h"
#include "Async/ParallelFor.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "TimerManager.h"

namespace
{
	/** Round trip of a TCP handshake to the beacon, in ms, or a negative value if it didn't answer in time. */
	float MeasureTcpPingMs(const FString& Ip, int32 Port, float TimeoutSeconds)
	{
		ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		TSharedRef<FInternetAddr> Address = Sockets->CreateInternetAddr();
		bool bValidIp = false;
		Address->SetIp(*Ip, bValidIp);
		Address->SetPort(Port);
		if (!bValidIp)
		{
			return -1.f;
		}

		FSocket* Socket = Sockets->CreateSocket(NAME_Stream, TEXT("EdgegapBeaconPing"), Address->GetProtocolType());
		if (!Socket)
		{
			return -1.f;
		}

		float PingMs = -1.f;
		Socket->SetNonBlocking(true);
		const double Start = FPlatformTime::Seconds();
		Socket->Connect(*Address);
		if (Socket->Wait(ESocketWaitConditions::WaitForWrite, FTimespan::FromSeconds(TimeoutSeconds))
			&& Socket->GetConnectionState() == SCS_Connected)
		{
			PingMs = static_cast<float>((FPlatformTime::Seconds() - Start) * 1000.0);
		}
		Socket->Close();
		Sockets->DestroySocket(Socket);
		return PingMs;
	}

	struct FBeacon
	{
		FString City;
		FString Ip;
		int32 TcpPort = 0;
		float PingMs = -1.f;
	};
}

bool UEdgegapMatchmakerSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	// Only game clients look for matches; the dedicated server is what they get matched into.
	return bEnabled && !IsRunningDedicatedServer();
}

void UEdgegapMatchmakerSubsystem::Deinitialize()
{
	// Don't leave a ticket behind when the game closes mid-search, or it can still start a server.
	if (State == EMatchmakingState::Searching)
	{
		DeleteTicket();
	}
	StopPolling();
	Super::Deinitialize();
}

void UEdgegapMatchmakerSubsystem::FindMatch()
{
	if (State == EMatchmakingState::Searching)
	{
		return;
	}
	if (MatchmakerUrl.IsEmpty() || AuthToken.IsEmpty() || MatchmakerUrl.StartsWith(TEXT("YOUR_")) || AuthToken.StartsWith(TEXT("YOUR_")))
	{
		Fail(TEXT("Set MatchmakerUrl and AuthToken in DefaultGame.ini"));
		return;
	}

	TicketId.Reset();
	++SearchId;

	if (!bSendBeaconLatencies)
	{
		CreateTicket({});
		return;
	}

	// Beacons are rescaled in real time, so fetch the list again for every search.
	SetState(EMatchmakingState::Searching, TEXT("Measuring latency..."));
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = MakeRequest(TEXT("GET"), TEXT("/locations/beacons"));
	Request->OnProcessRequestComplete().BindUObject(this, &UEdgegapMatchmakerSubsystem::OnBeaconsReceived, SearchId);
	Request->ProcessRequest();
}

void UEdgegapMatchmakerSubsystem::OnBeaconsReceived(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnected, int32 ForSearch)
{
	if (State != EMatchmakingState::Searching || ForSearch != SearchId)
	{
		return;
	}
	if (!bConnected || !Response.IsValid() || Response->GetResponseCode() != 200)
	{
		Fail(TEXT("Could not get the beacon list from the matchmaker"));
		return;
	}

	TSharedPtr<FJsonObject> Json;
	const TArray<TSharedPtr<FJsonValue>>* BeaconValues = nullptr;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Response->GetContentAsString()), Json) || !Json.IsValid()
		|| !Json->TryGetArrayField(TEXT("beacons"), BeaconValues))
	{
		Fail(TEXT("Unexpected beacon list"));
		return;
	}

	TArray<FBeacon> Beacons;
	for (const TSharedPtr<FJsonValue>& Value : *BeaconValues)
	{
		const TSharedPtr<FJsonObject> Object = Value->AsObject();
		const TSharedPtr<FJsonObject>* Location = nullptr;
		FBeacon Beacon;
		if (Object.IsValid() && Object->TryGetObjectField(TEXT("location"), Location)
			&& (*Location)->TryGetStringField(TEXT("city"), Beacon.City)
			&& Object->TryGetStringField(TEXT("public_ip"), Beacon.Ip)
			&& Object->TryGetNumberField(TEXT("tcp_port"), Beacon.TcpPort))
		{
			Beacons.Add(Beacon);
		}
	}
	if (Beacons.IsEmpty())
	{
		Fail(TEXT("No beacons available"));
		return;
	}

	// Ping every beacon in parallel off the game thread, keeping the best of three handshakes each.
	const float Timeout = BeaconPingTimeoutSeconds;
	TWeakObjectPtr<UEdgegapMatchmakerSubsystem> WeakThis(this);
	Async(EAsyncExecution::ThreadPool, [WeakThis, Beacons = MoveTemp(Beacons), Timeout, ForSearch]() mutable
	{
		ParallelFor(Beacons.Num(), [&Beacons, Timeout](int32 Index)
		{
			FBeacon& Beacon = Beacons[Index];
			for (int32 Sample = 0; Sample < 3; ++Sample)
			{
				const float PingMs = MeasureTcpPingMs(Beacon.Ip, Beacon.TcpPort, Timeout);
				if (PingMs >= 0.f && (Beacon.PingMs < 0.f || PingMs < Beacon.PingMs))
				{
					Beacon.PingMs = PingMs;
				}
			}
		});

		TMap<FString, float> Latencies;
		for (const FBeacon& Beacon : Beacons)
		{
			if (Beacon.PingMs >= 0.f)
			{
				Latencies.Add(Beacon.City, Beacon.PingMs);
			}
		}

		AsyncTask(ENamedThreads::GameThread, [WeakThis, Latencies = MoveTemp(Latencies), ForSearch]()
		{
			UEdgegapMatchmakerSubsystem* This = WeakThis.Get();
			if (!This || This->State != EMatchmakingState::Searching || ForSearch != This->SearchId)
			{
				return;
			}
			if (Latencies.IsEmpty())
			{
				This->Fail(TEXT("Could not reach any Edgegap beacon"));
				return;
			}
			This->CreateTicket(Latencies);
		});
	});
}

void UEdgegapMatchmakerSubsystem::CreateTicket(const TMap<FString, float>& BeaconLatencies)
{
	SetState(EMatchmakingState::Searching, TEXT("Creating ticket..."));

	TSharedRef<FJsonObject> Attributes = MakeShared<FJsonObject>();
	if (bSendBeaconLatencies)
	{
		TSharedRef<FJsonObject> Beacons = MakeShared<FJsonObject>();
		for (const TPair<FString, float>& Latency : BeaconLatencies)
		{
			Beacons->SetNumberField(Latency.Key, FMath::RoundToFloat(Latency.Value * 10.f) / 10.f);
			UE_LOG(LogMultiplayerGame, Log, TEXT("Beacon %s: %.1f ms"), *Latency.Key, Latency.Value);
		}
		Attributes->SetObjectField(TEXT("beacons"), Beacons);
	}

	// player_ip is left out so the matchmaker uses the address the request comes from.
	TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("profile"), Profile);
	Body->SetObjectField(TEXT("attributes"), Attributes);
	FString BodyText;
	FJsonSerializer::Serialize(Body, TJsonWriterFactory<>::Create(&BodyText));

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = MakeRequest(TEXT("POST"), TEXT("/tickets"));
	Request->SetContentAsString(BodyText);
	Request->OnProcessRequestComplete().BindUObject(this, &UEdgegapMatchmakerSubsystem::OnTicketCreated);
	Request->ProcessRequest();
}

void UEdgegapMatchmakerSubsystem::CancelMatch()
{
	if (State != EMatchmakingState::Searching)
	{
		return;
	}
	++SearchId;
	DeleteTicket();
	StopPolling();
	SetState(EMatchmakingState::Idle, TEXT("Cancelled"));
}

void UEdgegapMatchmakerSubsystem::NotifyBackOnMenu()
{
	if (State == EMatchmakingState::Connecting)
	{
		SetState(EMatchmakingState::Idle, TEXT("Left the match. Press Find Match to play again"));
	}
}

TSharedRef<IHttpRequest, ESPMode::ThreadSafe> UEdgegapMatchmakerSubsystem::MakeRequest(const FString& Verb, const FString& Path) const
{
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	FString Url = MatchmakerUrl;
	Url.RemoveFromEnd(TEXT("/"));
	Request->SetURL(Url + Path);
	Request->SetVerb(Verb);
	Request->SetHeader(TEXT("Authorization"), AuthToken);
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	return Request;
}

void UEdgegapMatchmakerSubsystem::OnTicketCreated(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnected)
{
	if (State != EMatchmakingState::Searching)
	{
		return;
	}
	if (!bConnected || !Response.IsValid())
	{
		Fail(TEXT("Could not reach the matchmaker"));
		return;
	}
	if (Response->GetResponseCode() != 201)
	{
		Fail(FString::Printf(TEXT("Ticket refused (%d): %s"), Response->GetResponseCode(), *Response->GetContentAsString()));
		return;
	}

	TSharedPtr<FJsonObject> Ticket;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Response->GetContentAsString()), Ticket) || !Ticket.IsValid()
		|| !Ticket->TryGetStringField(TEXT("id"), TicketId))
	{
		Fail(TEXT("Unexpected ticket response"));
		return;
	}

	UE_LOG(LogMultiplayerGame, Log, TEXT("Matchmaking ticket %s created"), *TicketId);
	SetState(EMatchmakingState::Searching, TEXT("Searching for players..."));
	GetGameInstance()->GetTimerManager().SetTimer(PollTimer, this, &UEdgegapMatchmakerSubsystem::PollTicket, PollIntervalSeconds, true);
}

void UEdgegapMatchmakerSubsystem::PollTicket()
{
	if (bPollInFlight || TicketId.IsEmpty())
	{
		return;
	}
	bPollInFlight = true;
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = MakeRequest(TEXT("GET"), TEXT("/tickets/") + TicketId);
	Request->OnProcessRequestComplete().BindUObject(this, &UEdgegapMatchmakerSubsystem::OnTicketPolled);
	Request->ProcessRequest();
}

void UEdgegapMatchmakerSubsystem::OnTicketPolled(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnected)
{
	bPollInFlight = false;
	if (State != EMatchmakingState::Searching)
	{
		return;
	}
	if (!bConnected || !Response.IsValid())
	{
		// Network blip: the timer polls again.
		return;
	}

	const int32 Code = Response->GetResponseCode();
	if (Code == 404)
	{
		Fail(TEXT("Ticket expired, try again"));
		return;
	}
	if (Code != 200)
	{
		// 429 and 5xx are retried on the next tick.
		UE_LOG(LogMultiplayerGame, Warning, TEXT("Ticket poll returned %d: %s"), Code, *Response->GetContentAsString());
		return;
	}

	TSharedPtr<FJsonObject> Ticket;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Response->GetContentAsString()), Ticket) || !Ticket.IsValid())
	{
		return;
	}

	const FString Status = Ticket->GetStringField(TEXT("status"));
	if (Status == TEXT("CANCELLED"))
	{
		Fail(TEXT("Ticket expired, try again"));
		return;
	}
	if (Status == TEXT("MATCH_FOUND"))
	{
		SetState(EMatchmakingState::Searching, TEXT("Match found, starting server..."));
		return;
	}
	if (Status != TEXT("HOST_ASSIGNED"))
	{
		return;
	}

	const TSharedPtr<FJsonObject>* Assignment = nullptr;
	const TSharedPtr<FJsonObject>* Ports = nullptr;
	if (!Ticket->TryGetObjectField(TEXT("assignment"), Assignment) || !(*Assignment)->TryGetObjectField(TEXT("ports"), Ports))
	{
		return;
	}

	// Use the configured port name, or the only port if the version has just one.
	TSharedPtr<FJsonObject> Port;
	const TSharedPtr<FJsonObject>* Named = nullptr;
	if ((*Ports)->TryGetObjectField(PortName, Named))
	{
		Port = *Named;
	}
	else if ((*Ports)->Values.Num() == 1)
	{
		Port = (*Ports)->Values.CreateConstIterator().Value()->AsObject();
	}
	if (!Port.IsValid())
	{
		Fail(FString::Printf(TEXT("No port named '%s' on the assigned server"), *PortName));
		return;
	}

	const FString Address = FString::Printf(TEXT("%s:%d"),
		*(*Assignment)->GetStringField(TEXT("public_ip")), Port->GetIntegerField(TEXT("external")));

	StopPolling();
	SetState(EMatchmakingState::Connecting, FString::Printf(TEXT("Connecting to %s..."), *Address));
	UE_LOG(LogMultiplayerGame, Log, TEXT("Ticket %s assigned to %s"), *TicketId, *Address);

	if (APlayerController* PC = GetGameInstance()->GetFirstLocalPlayerController())
	{
		PC->ClientTravel(Address, TRAVEL_Absolute);
	}
	else
	{
		Fail(TEXT("No local player to travel"));
	}
}

void UEdgegapMatchmakerSubsystem::DeleteTicket()
{
	if (TicketId.IsEmpty())
	{
		return;
	}
	// Fire and forget: the ticket also expires on its own.
	MakeRequest(TEXT("DELETE"), TEXT("/tickets/") + TicketId)->ProcessRequest();
	TicketId.Reset();
}

void UEdgegapMatchmakerSubsystem::StopPolling()
{
	if (UGameInstance* GameInstance = GetGameInstance())
	{
		GameInstance->GetTimerManager().ClearTimer(PollTimer);
	}
	bPollInFlight = false;
}

void UEdgegapMatchmakerSubsystem::Fail(const FString& Reason)
{
	UE_LOG(LogMultiplayerGame, Warning, TEXT("Matchmaking failed: %s"), *Reason);
	StopPolling();
	TicketId.Reset();
	SetState(EMatchmakingState::Failed, Reason);
}

void UEdgegapMatchmakerSubsystem::SetState(EMatchmakingState NewState, const FString& NewStatusText)
{
	State = NewState;
	StatusText = NewStatusText;
}
