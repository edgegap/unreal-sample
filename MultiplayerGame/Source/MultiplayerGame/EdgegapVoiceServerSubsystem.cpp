// Copyright Epic Games, Inc. All Rights Reserved.

#include "EdgegapVoiceServerSubsystem.h"
#include "EdgegapVoiceSettings.h"
#include "EdgegapVoiceComponent.h"
#include "GameFramework/PlayerController.h"
#include "Engine/NetConnection.h"
#include "GameFramework/GameModeBase.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/Base64.h"
#include "Misc/ConfigCacheIni.h"
#include "HAL/PlatformMisc.h"
#include "GenericPlatform/GenericPlatformHttp.h"

DEFINE_LOG_CATEGORY_STATIC(LogEdgegapVoice, Log, All);

#define UI UI_ST
THIRD_PARTY_INCLUDES_START
#include <openssl/bn.h>
#include <openssl/objects.h>
#include <openssl/rsa.h>
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END
#undef UI

namespace
{
	const TCHAR* const EosApi = TEXT("https://api.epicgames.dev");

	FString GetEnv(const TCHAR* Name)
	{
		return FPlatformMisc::GetEnvironmentVariable(Name);
	}

	bool Base64UrlDecode(FString Text, TArray<uint8>& Out)
	{
		while (Text.Len() % 4 != 0)
		{
			Text.AppendChar(TEXT('='));
		}
		return FBase64::Decode(Text, Out, EBase64Mode::UrlSafe);
	}

	TSharedPtr<FJsonObject> ParseJson(const FString& Text)
	{
		TSharedPtr<FJsonObject> Object;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object);
		return Object;
	}

	TSharedPtr<FJsonObject> DecodeJwtPart(const FString& Part)
	{
		TArray<uint8> Bytes;
		if (!Base64UrlDecode(Part, Bytes))
		{
			return nullptr;
		}
		Bytes.Add(0);
		return ParseJson(UTF8_TO_TCHAR(reinterpret_cast<const ANSICHAR*>(Bytes.GetData())));
	}

	/** RS256: RSASSA-PKCS1-v1_5 with SHA-256, the algorithm EOS signs ID tokens with. */
	bool VerifyRs256(const FString& SigningInput, const TArray<uint8>& Signature, const TArray<uint8>& Modulus, const TArray<uint8>& Exponent)
	{
		const FTCHARToUTF8 Input(*SigningInput);
		uint8 Digest[SHA256_DIGEST_LENGTH];
		SHA256(reinterpret_cast<const unsigned char*>(Input.Get()), Input.Length(), Digest);

		RSA* Rsa = RSA_new();
		BIGNUM* N = BN_bin2bn(Modulus.GetData(), Modulus.Num(), nullptr);
		BIGNUM* E = BN_bin2bn(Exponent.GetData(), Exponent.Num(), nullptr);
		RSA_set0_key(Rsa, N, E, nullptr); // Rsa owns N and E from here on.
		const bool bValid = RSA_verify(NID_sha256, Digest, SHA256_DIGEST_LENGTH, Signature.GetData(), Signature.Num(), Rsa) == 1;
		RSA_free(Rsa);
		return bValid;
	}

	/** Room names end up in URLs; keep them to letters, digits, '-' and '_'. */
	FString SanitizeRoomName(const FString& Name)
	{
		FString Out;
		for (TCHAR Char : Name)
		{
			Out.AppendChar(FChar::IsAlnum(Char) || Char == TEXT('-') || Char == TEXT('_') ? Char : TEXT('-'));
		}
		return Out.Left(64);
	}
}

bool UEdgegapVoiceServerSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	// Off unless turned on in config, or the deployment was given the server's EOS credentials.
	return IsRunningDedicatedServer() && (UEdgegapVoiceSettings::Get().bEnabled || !GetEnv(TEXT("EOS_VOICE_CLIENT_ID")).IsEmpty());
}

void UEdgegapVoiceServerSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	const UEdgegapVoiceSettings& Settings = UEdgegapVoiceSettings::Get();
	ClientId = GetEnv(TEXT("EOS_VOICE_CLIENT_ID"));
	ClientSecret = GetEnv(TEXT("EOS_VOICE_CLIENT_SECRET"));
	if (ClientId.IsEmpty())
	{
		ClientId = Settings.ServerClientId;
		ClientSecret = Settings.ServerClientSecret;
	}
	DeploymentId = GetEnv(TEXT("EOS_VOICE_DEPLOYMENT_ID"));
	if (DeploymentId.IsEmpty())
	{
		GConfig->GetString(TEXT("EOSVoiceChat"), TEXT("DeploymentId"), DeploymentId, GEngineIni);
	}
	GameClientId = GetEnv(TEXT("EOS_VOICE_GAME_CLIENT_ID"));
	if (GameClientId.IsEmpty())
	{
		GConfig->GetString(TEXT("EOSVoiceChat"), TEXT("ClientId"), GameClientId, GEngineIni);
	}

	// One room per match. The matchmaker's match ID is unique; a server deployed by hand uses its deployment ID.
	FString MatchId = GetEnv(TEXT("MM_MATCH_ID"));
	if (MatchId.IsEmpty())
	{
		MatchId = GetEnv(TEXT("ARBITRIUM_REQUEST_ID"));
	}
	if (MatchId.IsEmpty())
	{
		MatchId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	}
	MatchRoom = SanitizeRoomName(MatchId);

	// Every player gets the component that carries the voice RPCs, and leaving the server leaves voice,
	// without any change to the game mode or player controller.
	FGameModeEvents::OnGameModePostLoginEvent().AddWeakLambda(this, [](AGameModeBase*, APlayerController* NewPlayer)
	{
		if (NewPlayer && !NewPlayer->FindComponentByClass<UEdgegapVoiceComponent>())
		{
			UEdgegapVoiceComponent* Component = NewObject<UEdgegapVoiceComponent>(NewPlayer, TEXT("EdgegapVoice"));
			Component->RegisterComponent();
		}
	});
	FGameModeEvents::OnGameModeLogoutEvent().AddWeakLambda(this, [this](AGameModeBase*, AController* Exiting)
	{
		RemovePlayer(Cast<APlayerController>(Exiting));
	});

	if (ClientId.IsEmpty() || ClientSecret.IsEmpty() || DeploymentId.IsEmpty() || DeploymentId.StartsWith(TEXT("YOUR_")))
	{
		UE_LOG(LogEdgegapVoice, Warning, TEXT("Voice: missing EOS server credentials or deployment ID, voice chat is off. Set the EOS_VOICE_* variables"));
		ClientId.Reset();
		return;
	}
	UE_LOG(LogEdgegapVoice, Log, TEXT("Voice: trusted voice server ready, match room %s"), *MatchRoom);
}

void UEdgegapVoiceServerSubsystem::RegisterPlayer(APlayerController* Player, const FString& ProductUserId, const FString& IdToken)
{
	if (ClientId.IsEmpty() || !Player)
	{
		return;
	}

	FString KeyId;
	TArray<FString> Parts;
	IdToken.ParseIntoArray(Parts, TEXT("."), false);
	const TSharedPtr<FJsonObject> Header = Parts.Num() == 3 ? DecodeJwtPart(Parts[0]) : nullptr;
	if (!Header.IsValid() || !Header->TryGetStringField(TEXT("kid"), KeyId))
	{
		UE_LOG(LogEdgegapVoice, Warning, TEXT("Voice: %s sent an invalid ID token"), *ProductUserId);
		return;
	}

	TWeakObjectPtr<APlayerController> WeakPlayer(Player);
	WithSigningKeys(KeyId, [this, WeakPlayer, ProductUserId, IdToken](bool bHaveKeys)
	{
		APlayerController* PlayerPtr = WeakPlayer.Get();
		if (!PlayerPtr || !bHaveKeys)
		{
			return;
		}

		// Never trust the Product User ID a client claims: only a token signed by Epic proves it.
		FString Error;
		if (!VerifyIdToken(IdToken, ProductUserId, Error))
		{
			UE_LOG(LogEdgegapVoice, Warning, TEXT("Voice: rejected %s: %s"), *ProductUserId, *Error);
			return;
		}

		FString ClientIp;
		const FString Room = PickRoom(PlayerPtr, ClientIp);
		Players.Add(WeakPlayer, { ProductUserId, Room });
		RequestRoomToken(WeakPlayer, ProductUserId, Room, ClientIp);
	});
}

bool UEdgegapVoiceServerSubsystem::VerifyIdToken(const FString& IdToken, const FString& ExpectedProductUserId, FString& OutError) const
{
	TArray<FString> Parts;
	IdToken.ParseIntoArray(Parts, TEXT("."), false);
	const TSharedPtr<FJsonObject> Header = Parts.Num() == 3 ? DecodeJwtPart(Parts[0]) : nullptr;
	const TSharedPtr<FJsonObject> Claims = Parts.Num() == 3 ? DecodeJwtPart(Parts[1]) : nullptr;
	if (!Header.IsValid() || !Claims.IsValid())
	{
		OutError = TEXT("malformed token");
		return false;
	}

	const TPair<FString, FString>* Key = SigningKeys.Find(Header->GetStringField(TEXT("kid")));
	TArray<uint8> Signature, Modulus, Exponent;
	if (Header->GetStringField(TEXT("alg")) != TEXT("RS256") || !Key
		|| !Base64UrlDecode(Parts[2], Signature) || !Base64UrlDecode(Key->Key, Modulus) || !Base64UrlDecode(Key->Value, Exponent)
		|| !VerifyRs256(Parts[0] + TEXT(".") + Parts[1], Signature, Modulus, Exponent))
	{
		OutError = TEXT("bad signature");
		return false;
	}

	// Claims documented in Epic's Connect Interface reference, "ID Tokens".
	int64 Expires = 0;
	if (!Claims->TryGetNumberField(TEXT("exp"), Expires) || Expires < FDateTime::UtcNow().ToUnixTimestamp())
	{
		OutError = TEXT("token expired");
		return false;
	}
	if (!Claims->GetStringField(TEXT("iss")).StartsWith(EosApi))
	{
		OutError = TEXT("wrong issuer");
		return false;
	}
	if (Claims->GetStringField(TEXT("sub")) != ExpectedProductUserId)
	{
		OutError = TEXT("token belongs to another player");
		return false;
	}
	if (Claims->GetStringField(TEXT("pfdid")) != DeploymentId)
	{
		OutError = TEXT("token is for another EOS deployment");
		return false;
	}
	if (!GameClientId.IsEmpty() && !GameClientId.StartsWith(TEXT("YOUR_")) && Claims->GetStringField(TEXT("aud")) != GameClientId)
	{
		OutError = TEXT("token was issued to another EOS client");
		return false;
	}
	return true;
}

FString UEdgegapVoiceServerSubsystem::PickRoom(APlayerController* Player, FString& OutClientIp) const
{
	// The address the player connects from. EOS uses it to pick a voice server close to the player.
	UNetConnection* Connection = Player->GetNetConnection();
	OutClientIp = Connection ? Connection->LowLevelGetRemoteAddress(false) : FString();
	if (!UEdgegapVoiceSettings::Get().bTeamRooms || OutClientIp.IsEmpty())
	{
		return MatchRoom;
	}

	// The matchmaker injects MM_TICKET_IDS and one MM_TICKET_<id> per ticket, with the IP the player
	// queued from and their team. Find the player's team from their IP rather than trusting the client.
	TArray<TSharedPtr<FJsonValue>> TicketIds;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(GetEnv(TEXT("MM_TICKET_IDS"))), TicketIds);
	TSet<FString> Teams;
	for (const TSharedPtr<FJsonValue>& Id : TicketIds)
	{
		const TSharedPtr<FJsonObject> Ticket = ParseJson(GetEnv(*(TEXT("MM_TICKET_") + Id->AsString())));
		FString TicketIp, TeamId;
		if (Ticket.IsValid() && Ticket->TryGetStringField(TEXT("player_ip"), TicketIp) && TicketIp == OutClientIp
			&& Ticket->TryGetStringField(TEXT("team_id"), TeamId))
		{
			Teams.Add(TeamId);
		}
	}

	// No ticket from this IP (connected by IP, or a different address), or players on different teams
	// behind one IP: keep them in the match room rather than guess.
	if (Teams.Num() != 1)
	{
		return MatchRoom;
	}
	return SanitizeRoomName(MatchRoom + TEXT("-team-") + *Teams.CreateConstIterator());
}

void UEdgegapVoiceServerSubsystem::RequestRoomToken(TWeakObjectPtr<APlayerController> Player, const FString& ProductUserId, const FString& Room, const FString& ClientIp)
{
	// POST /rtc/v1/{DeploymentId}/room/{RoomId} creates the room if needed and returns one token per participant.
	TSharedRef<FJsonObject> Participant = MakeShared<FJsonObject>();
	Participant->SetStringField(TEXT("puid"), ProductUserId);
	if (!ClientIp.IsEmpty())
	{
		Participant->SetStringField(TEXT("clientIp"), ClientIp);
	}
	Participant->SetBoolField(TEXT("hardMuted"), false);
	TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetArrayField(TEXT("participants"), { MakeShared<FJsonValueObject>(Participant) });
	FString BodyText;
	FJsonSerializer::Serialize(Body, TJsonWriterFactory<>::Create(&BodyText));

	CallRoomApi(TEXT("POST"), FString::Printf(TEXT("/room/%s"), *Room), BodyText, [Player, ProductUserId, Room](int32 Code, const FString& Response)
	{
		const TSharedPtr<FJsonObject> Json = Code == 200 ? ParseJson(Response) : nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Participants = nullptr;
		FString ClientBaseUrl;
		if (!Json.IsValid() || !Json->TryGetStringField(TEXT("clientBaseUrl"), ClientBaseUrl) || !Json->TryGetArrayField(TEXT("participants"), Participants))
		{
			UE_LOG(LogEdgegapVoice, Warning, TEXT("Voice: room token request for %s failed (%d): %s"), *ProductUserId, Code, *Response);
			return;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Participants)
		{
			const TSharedPtr<FJsonObject> Entry = Value->AsObject();
			if (Entry.IsValid() && Entry->GetStringField(TEXT("puid")) == ProductUserId)
			{
				// Each token goes to its owner only: anyone holding it can join as that player.
				UEdgegapVoiceComponent* Component = Player.IsValid() ? Player->FindComponentByClass<UEdgegapVoiceComponent>() : nullptr;
				if (Component)
				{
					UE_LOG(LogEdgegapVoice, Log, TEXT("Voice: %s gets room %s"), *ProductUserId, *Room);
					Component->ClientJoinVoiceRoom(Room, ClientBaseUrl, Entry->GetStringField(TEXT("token")));
				}
				return;
			}
		}
	});
}

void UEdgegapVoiceServerSubsystem::RemovePlayer(APlayerController* Player)
{
	FVoicePlayer VoicePlayer;
	if (!Players.RemoveAndCopyValue(Player, VoicePlayer))
	{
		return;
	}
	// DELETE also revokes the player's token, so a kicked player can't rejoin with it.
	CallRoomApi(TEXT("DELETE"), FString::Printf(TEXT("/room/%s/participants/%s"), *VoicePlayer.Room, *VoicePlayer.ProductUserId), FString(),
		[VoicePlayer](int32 Code, const FString& Response)
		{
			UE_LOG(LogEdgegapVoice, Log, TEXT("Voice: removed %s from %s (%d)"), *VoicePlayer.ProductUserId, *VoicePlayer.Room, Code);
		});
}

void UEdgegapVoiceServerSubsystem::SetHardMuted(APlayerController* Player, bool bMuted)
{
	const FVoicePlayer* VoicePlayer = Players.Find(Player);
	if (!VoicePlayer)
	{
		return;
	}
	CallRoomApi(TEXT("POST"), FString::Printf(TEXT("/room/%s/participants/%s"), *VoicePlayer->Room, *VoicePlayer->ProductUserId),
		bMuted ? TEXT("{\"hardMuted\":true}") : TEXT("{\"hardMuted\":false}"),
		[Puid = VoicePlayer->ProductUserId, bMuted](int32 Code, const FString& Response)
		{
			UE_LOG(LogEdgegapVoice, Log, TEXT("Voice: %s %s for everyone (%d)"), *Puid, bMuted ? TEXT("muted") : TEXT("unmuted"), Code);
		});
}

void UEdgegapVoiceServerSubsystem::CallRoomApi(const FString& Verb, const FString& Path, const FString& Body, TFunction<void(int32, const FString&)> OnDone)
{
	WithAccessToken([this, Verb, Path, Body, OnDone = MoveTemp(OnDone)](const FString& Token)
	{
		if (Token.IsEmpty())
		{
			OnDone(0, TEXT("no EOS access token"));
			return;
		}
		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
		Request->SetURL(FString::Printf(TEXT("%s/rtc/v1/%s%s"), EosApi, *DeploymentId, *Path));
		Request->SetVerb(Verb);
		Request->SetHeader(TEXT("Authorization"), TEXT("Bearer ") + Token);
		Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
		if (!Body.IsEmpty())
		{
			Request->SetContentAsString(Body);
		}
		Request->OnProcessRequestComplete().BindLambda([OnDone](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
		{
			OnDone(bConnected && Response.IsValid() ? Response->GetResponseCode() : 0, Response.IsValid() ? Response->GetContentAsString() : FString());
		});
		Request->ProcessRequest();
	});
}

void UEdgegapVoiceServerSubsystem::WithAccessToken(FOnAccessToken OnToken)
{
	// One client access token serves every request until shortly before it expires.
	if (!AccessToken.IsEmpty() && FPlatformTime::Seconds() < AccessTokenExpiresAt)
	{
		OnToken(AccessToken);
		return;
	}
	PendingAccessToken.Add(MoveTemp(OnToken));
	if (PendingAccessToken.Num() > 1)
	{
		return;
	}

	// Connect Web API, client credentials grant: POST /auth/v1/oauth/token with Basic client ID:secret.
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(FString(EosApi) + TEXT("/auth/v1/oauth/token"));
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Authorization"), TEXT("Basic ") + FBase64::Encode(ClientId + TEXT(":") + ClientSecret));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/x-www-form-urlencoded"));
	Request->SetContentAsString(TEXT("grant_type=client_credentials&deployment_id=") + FGenericPlatformHttp::UrlEncode(DeploymentId));
	Request->OnProcessRequestComplete().BindWeakLambda(this, [this](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
	{
		const TSharedPtr<FJsonObject> Json = bConnected && Response.IsValid() && Response->GetResponseCode() == 200 ? ParseJson(Response->GetContentAsString()) : nullptr;
		double ExpiresIn = 0.0;
		if (Json.IsValid() && Json->TryGetStringField(TEXT("access_token"), AccessToken) && Json->TryGetNumberField(TEXT("expires_in"), ExpiresIn))
		{
			AccessTokenExpiresAt = FPlatformTime::Seconds() + ExpiresIn - 60.0;
		}
		else
		{
			AccessToken.Reset();
			UE_LOG(LogEdgegapVoice, Warning, TEXT("Voice: EOS refused the server credentials (%d): %s"),
				Response.IsValid() ? Response->GetResponseCode() : 0, Response.IsValid() ? *Response->GetContentAsString() : TEXT(""));
		}
		TArray<FOnAccessToken> Callbacks = MoveTemp(PendingAccessToken);
		for (FOnAccessToken& Callback : Callbacks)
		{
			Callback(AccessToken);
		}
	});
	Request->ProcessRequest();
}

void UEdgegapVoiceServerSubsystem::WithSigningKeys(const FString& KeyId, FOnKeys OnKeys)
{
	if (SigningKeys.Contains(KeyId))
	{
		OnKeys(true);
		return;
	}
	// Unknown key ID: Epic rotated its keys or this is the first player, so fetch the key set again.
	PendingKeys.Add(MoveTemp(OnKeys));
	if (PendingKeys.Num() > 1)
	{
		return;
	}

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(FString(EosApi) + TEXT("/auth/v1/oauth/jwks"));
	Request->SetVerb(TEXT("GET"));
	Request->OnProcessRequestComplete().BindWeakLambda(this, [this](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
	{
		const TSharedPtr<FJsonObject> Json = bConnected && Response.IsValid() && Response->GetResponseCode() == 200 ? ParseJson(Response->GetContentAsString()) : nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
		if (Json.IsValid() && Json->TryGetArrayField(TEXT("keys"), Keys))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Keys)
			{
				const TSharedPtr<FJsonObject> Key = Value->AsObject();
				FString Kid, N, E;
				if (Key.IsValid() && Key->TryGetStringField(TEXT("kid"), Kid) && Key->TryGetStringField(TEXT("n"), N) && Key->TryGetStringField(TEXT("e"), E))
				{
					SigningKeys.Add(Kid, { N, E });
				}
			}
		}
		else
		{
			UE_LOG(LogEdgegapVoice, Warning, TEXT("Voice: could not get Epic's ID token keys"));
		}
		TArray<FOnKeys> Callbacks = MoveTemp(PendingKeys);
		for (FOnKeys& Callback : Callbacks)
		{
			Callback(SigningKeys.Num() > 0);
		}
	});
	Request->ProcessRequest();
}
