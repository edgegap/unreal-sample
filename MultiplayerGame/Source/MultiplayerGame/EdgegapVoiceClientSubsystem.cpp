// Copyright Edgegap. Licensed under the MIT License, see LICENSE at the repository root.

#include "EdgegapVoiceClientSubsystem.h"
#include "EdgegapVoiceSettings.h"
#include "Misc/CommandLine.h"
#include "HAL/IConsoleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogEdgegapVoice, Log, All);

#if WITH_EDGEGAP_EOS_VOICE_CLIENT
#include "VoiceChat.h"
#include "EOSVoiceChat.h"
#include "EOSVoiceChatTypes.h"
#include "EOSShared.h"
#include "IEOSSDKManager.h"
#include "eos_sdk.h"
#include "eos_connect.h"

namespace
{
	/** Runs a lambda as an EOS SDK callback. EOS calls back once per request, on the game thread, during the platform tick. */
	template<typename TCallbackInfo>
	struct TEosCallback
	{
		using FFunc = TFunction<void(const TCallbackInfo*)>;

		static void* Make(FFunc&& Func) { return new FFunc(MoveTemp(Func)); }

		static void EOS_CALL Run(const TCallbackInfo* Data)
		{
			FFunc* Func = static_cast<FFunc*>(Data->ClientData);
			(*Func)(Data);
			delete Func;
		}
	};
}
#endif

bool UEdgegapVoiceClientSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
#if WITH_EDGEGAP_EOS_VOICE_CLIENT
	// The game server never speaks; it only hands out room tokens (see UEdgegapVoiceServerSubsystem).
	return UEdgegapVoiceSettings::Get().bEnabled && !IsRunningDedicatedServer();
#else
	return false;
#endif
}

void UEdgegapVoiceClientSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
#if WITH_EDGEGAP_EOS_VOICE_CLIENT
	VoiceChat = IVoiceChat::Get();
	if (!VoiceChat)
	{
		UE_LOG(LogEdgegapVoice, Warning, TEXT("Voice: the EOSVoiceChat plugin is not loaded, voice chat is off"));
		return;
	}

	// These two events are how you can tell from the logs that audio really flows between players.
	PlayerAddedHandle = VoiceChat->OnVoiceChatPlayerAdded().AddWeakLambda(this, [](const FString& Room, const FString& Player)
	{
		UE_LOG(LogEdgegapVoice, Log, TEXT("Voice: %s joined room %s"), *Player, *Room);
	});
	TalkingHandle = VoiceChat->OnVoiceChatPlayerTalkingUpdated().AddWeakLambda(this, [](const FString& Room, const FString& Player, bool bTalking)
	{
		UE_LOG(LogEdgegapVoice, Log, TEXT("Voice: %s %s in room %s"), *Player, bTalking ? TEXT("started talking") : TEXT("stopped talking"), *Room);
	});

	MuteCommand = IConsoleManager::Get().RegisterConsoleCommand(TEXT("VoiceMute"), TEXT("Mute or unmute your microphone in voice chat"),
		FConsoleCommandDelegate::CreateWeakLambda(this, [this]() { ToggleMute(); }));

	// Log in right away so the identity is ready by the time the player reaches a server.
	Login();
#endif
}

void UEdgegapVoiceClientSubsystem::Deinitialize()
{
#if WITH_EDGEGAP_EOS_VOICE_CLIENT
	if (VoiceChat)
	{
		LeaveRooms();
		VoiceChat->OnVoiceChatPlayerAdded().Remove(PlayerAddedHandle);
		VoiceChat->OnVoiceChatPlayerTalkingUpdated().Remove(TalkingHandle);
	}
	if (MuteCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(MuteCommand);
		MuteCommand = nullptr;
	}
#endif
	Super::Deinitialize();
}

void UEdgegapVoiceClientSubsystem::RequestIdentity(FOnVoiceIdentityReady OnReady)
{
#if WITH_EDGEGAP_EOS_VOICE_CLIENT
	if (!VoiceChat)
	{
		return;
	}
	if (!LocalProductUserId.IsEmpty())
	{
		// Copied fresh every time: ID tokens expire with the EOS session.
		const FString IdToken = CopyIdToken();
		if (!IdToken.IsEmpty())
		{
			OnReady.ExecuteIfBound(LocalProductUserId, IdToken);
			return;
		}
	}
	PendingIdentityRequests.Add(MoveTemp(OnReady));
	Login();
#endif
}

void UEdgegapVoiceClientSubsystem::JoinRoom(const FString& RoomName, const FString& ClientBaseUrl, const FString& ParticipantToken)
{
#if WITH_EDGEGAP_EOS_VOICE_CLIENT
	if (!VoiceChat || !VoiceChat->IsLoggedIn())
	{
		UE_LOG(LogEdgegapVoice, Warning, TEXT("Voice: got a token for %s but the voice user is not logged in"), *RoomName);
		return;
	}

	FEOSVoiceChatChannelCredentials Credentials;
	Credentials.ClientBaseUrl = ClientBaseUrl;
	Credentials.ParticipantToken = ParticipantToken;

	VoiceChat->JoinChannel(RoomName, Credentials.ToJson(false), EVoiceChatChannelType::NonPositional,
		FOnVoiceChatChannelJoinCompleteDelegate::CreateWeakLambda(this, [this](const FString& Room, const FVoiceChatResult& Result)
		{
			if (Result.IsSuccess())
			{
				JoinedRooms.AddUnique(Room);
				UE_LOG(LogEdgegapVoice, Log, TEXT("Voice: joined room %s"), *Room);
			}
			else
			{
				UE_LOG(LogEdgegapVoice, Warning, TEXT("Voice: could not join room %s: %s"), *Room, *LexToString(Result));
			}
		}));
#endif
}

void UEdgegapVoiceClientSubsystem::LeaveRooms()
{
#if WITH_EDGEGAP_EOS_VOICE_CLIENT
	if (!VoiceChat)
	{
		return;
	}
	for (const FString& Room : JoinedRooms)
	{
		UE_LOG(LogEdgegapVoice, Log, TEXT("Voice: leaving room %s"), *Room);
		VoiceChat->LeaveChannel(Room, FOnVoiceChatChannelLeaveCompleteDelegate());
	}
	JoinedRooms.Reset();
#endif
}

bool UEdgegapVoiceClientSubsystem::ToggleMute()
{
#if WITH_EDGEGAP_EOS_VOICE_CLIENT
	if (VoiceChat)
	{
		bMuted = !bMuted;
		VoiceChat->SetAudioInputDeviceMuted(bMuted);
		UE_LOG(LogEdgegapVoice, Log, TEXT("Voice: microphone %s"), bMuted ? TEXT("muted") : TEXT("on"));
	}
#endif
	return bMuted;
}

#if WITH_EDGEGAP_EOS_VOICE_CLIENT

void* UEdgegapVoiceClientSubsystem::GetConnectInterface() const
{
	// EOSVoiceChat creates its own EOS platform from [EOSVoiceChat] in DefaultEngine.ini. Voice rooms are
	// joined as a Product User ID logged in on that same platform, so the Connect login happens there too.
	IEOSPlatformHandlePtr Platform = static_cast<FEOSVoiceChat*>(VoiceChat)->GetPlatformHandle();
	return Platform.IsValid() ? EOS_Platform_GetConnectInterface(*Platform) : nullptr;
}

void UEdgegapVoiceClientSubsystem::Login()
{
	if (bLoggingIn)
	{
		return;
	}
	bLoggingIn = true;

	VoiceChat->Initialize(FOnVoiceChatInitializeCompleteDelegate::CreateWeakLambda(this, [this](const FVoiceChatResult& InitResult)
	{
		if (!InitResult.IsSuccess())
		{
			FailLogin(FString::Printf(TEXT("EOS voice did not initialize (%s). Check [EOSVoiceChat] in DefaultEngine.ini"), *LexToString(InitResult)));
			return;
		}
		VoiceChat->Connect(FOnVoiceChatConnectCompleteDelegate::CreateWeakLambda(this, [this](const FVoiceChatResult& ConnectResult)
		{
			if (!ConnectResult.IsSuccess())
			{
				FailLogin(FString::Printf(TEXT("EOS voice did not connect (%s)"), *LexToString(ConnectResult)));
				return;
			}

			EOS_HConnect Connect = static_cast<EOS_HConnect>(GetConnectInterface());
			if (!Connect)
			{
				FailLogin(TEXT("no EOS platform"));
				return;
			}

			// A device ID is a pseudo-account stored for the current OS user, so players need no Epic account.
			// Two clients on one PC would share it: -EOSVoiceNewDeviceId makes a new one, for local tests only.
			TWeakObjectPtr<UEdgegapVoiceClientSubsystem> WeakThis(this);
			auto CreateDeviceId = [WeakThis, Connect]()
			{
				EOS_Connect_CreateDeviceIdOptions Options = {};
				Options.ApiVersion = EOS_CONNECT_CREATEDEVICEID_API_LATEST;
				Options.DeviceModel = "PC";
				EOS_Connect_CreateDeviceId(Connect, &Options,
					TEosCallback<EOS_Connect_CreateDeviceIdCallbackInfo>::Make([WeakThis](const EOS_Connect_CreateDeviceIdCallbackInfo* Data)
					{
						if (UEdgegapVoiceClientSubsystem* This = WeakThis.Get())
						{
							if (Data->ResultCode == EOS_EResult::EOS_Success || Data->ResultCode == EOS_EResult::EOS_DuplicateNotAllowed)
							{
								This->OnDeviceIdReady();
							}
							else
							{
								This->FailLogin(FString::Printf(TEXT("could not create a device ID (%s)"), *LexToString(Data->ResultCode)));
							}
						}
					}),
					&TEosCallback<EOS_Connect_CreateDeviceIdCallbackInfo>::Run);
			};

			if (FParse::Param(FCommandLine::Get(), TEXT("EOSVoiceNewDeviceId")) && LocalProductUserId.IsEmpty())
			{
				EOS_Connect_DeleteDeviceIdOptions Options = {};
				Options.ApiVersion = EOS_CONNECT_DELETEDEVICEID_API_LATEST;
				EOS_Connect_DeleteDeviceId(Connect, &Options,
					TEosCallback<EOS_Connect_DeleteDeviceIdCallbackInfo>::Make([CreateDeviceId](const EOS_Connect_DeleteDeviceIdCallbackInfo*) { CreateDeviceId(); }),
					&TEosCallback<EOS_Connect_DeleteDeviceIdCallbackInfo>::Run);
			}
			else
			{
				CreateDeviceId();
			}
		}));
	}));
}

void UEdgegapVoiceClientSubsystem::OnDeviceIdReady()
{
	EOS_HConnect Connect = static_cast<EOS_HConnect>(GetConnectInterface());
	const FTCHARToUTF8 DisplayName(*UEdgegapVoiceSettings::Get().DeviceIdDisplayName);

	EOS_Connect_Credentials Credentials = {};
	Credentials.ApiVersion = EOS_CONNECT_CREDENTIALS_API_LATEST;
	Credentials.Type = EOS_EExternalCredentialType::EOS_ECT_DEVICEID_ACCESS_TOKEN;
	Credentials.Token = nullptr; // The SDK reads the stored device ID itself.

	EOS_Connect_UserLoginInfo LoginInfo = {};
	LoginInfo.ApiVersion = EOS_CONNECT_USERLOGININFO_API_LATEST;
	LoginInfo.DisplayName = DisplayName.Get();

	EOS_Connect_LoginOptions Options = {};
	Options.ApiVersion = EOS_CONNECT_LOGIN_API_LATEST;
	Options.Credentials = &Credentials;
	Options.UserLoginInfo = &LoginInfo;

	TWeakObjectPtr<UEdgegapVoiceClientSubsystem> WeakThis(this);
	EOS_Connect_Login(Connect, &Options,
		TEosCallback<EOS_Connect_LoginCallbackInfo>::Make([WeakThis, Connect](const EOS_Connect_LoginCallbackInfo* Data)
		{
			UEdgegapVoiceClientSubsystem* This = WeakThis.Get();
			if (!This)
			{
				return;
			}
			if (Data->ResultCode == EOS_EResult::EOS_Success)
			{
				This->OnLoggedIn(LexToString(Data->LocalUserId));
				return;
			}
			if (Data->ResultCode != EOS_EResult::EOS_InvalidUser)
			{
				This->FailLogin(FString::Printf(TEXT("EOS Connect login failed (%s)"), *LexToString(Data->ResultCode)));
				return;
			}

			// First login with this device ID: create the Product User ID.
			EOS_Connect_CreateUserOptions CreateOptions = {};
			CreateOptions.ApiVersion = EOS_CONNECT_CREATEUSER_API_LATEST;
			CreateOptions.ContinuanceToken = Data->ContinuanceToken;
			EOS_Connect_CreateUser(Connect, &CreateOptions,
				TEosCallback<EOS_Connect_CreateUserCallbackInfo>::Make([WeakThis](const EOS_Connect_CreateUserCallbackInfo* CreateData)
				{
					if (UEdgegapVoiceClientSubsystem* This = WeakThis.Get())
					{
						if (CreateData->ResultCode == EOS_EResult::EOS_Success)
						{
							This->OnLoggedIn(LexToString(CreateData->LocalUserId));
						}
						else
						{
							This->FailLogin(FString::Printf(TEXT("could not create the EOS user (%s)"), *LexToString(CreateData->ResultCode)));
						}
					}
				}),
				&TEosCallback<EOS_Connect_CreateUserCallbackInfo>::Run);
		}),
		&TEosCallback<EOS_Connect_LoginCallbackInfo>::Run);
}

void UEdgegapVoiceClientSubsystem::OnLoggedIn(const FString& ProductUserId)
{
	UE_LOG(LogEdgegapVoice, Log, TEXT("Voice: logged into EOS as %s"), *ProductUserId);

	// The EOS session lasts about an hour; log in again with the same device ID when EOS says it is expiring.
	if (LocalProductUserId.IsEmpty())
	{
		EOS_HConnect Connect = static_cast<EOS_HConnect>(GetConnectInterface());
		EOS_Connect_AddNotifyAuthExpirationOptions Options = {};
		Options.ApiVersion = EOS_CONNECT_ADDNOTIFYAUTHEXPIRATION_API_LATEST;
		TWeakObjectPtr<UEdgegapVoiceClientSubsystem> WeakThis(this);
		EOS_Connect_AddNotifyAuthExpiration(Connect, &Options, new TWeakObjectPtr<UEdgegapVoiceClientSubsystem>(WeakThis),
			[](const EOS_Connect_AuthExpirationCallbackInfo* Data)
			{
				// Registered once for the process lifetime, so the weak pointer is never freed.
				if (UEdgegapVoiceClientSubsystem* This = static_cast<TWeakObjectPtr<UEdgegapVoiceClientSubsystem>*>(Data->ClientData)->Get())
				{
					UE_LOG(LogEdgegapVoice, Log, TEXT("Voice: EOS session expiring, logging in again"));
					This->bLoggingIn = true;
					This->OnDeviceIdReady();
				}
			});
	}

	LocalProductUserId = ProductUserId;
	VoiceChat->Login(FPlatformMisc::GetPlatformUserForUserIndex(0), ProductUserId, FString(),
		FOnVoiceChatLoginCompleteDelegate::CreateWeakLambda(this, [this](const FString& PlayerName, const FVoiceChatResult& Result)
		{
			bLoggingIn = false;
			if (!Result.IsSuccess())
			{
				FailLogin(FString::Printf(TEXT("voice login failed (%s)"), *LexToString(Result)));
				return;
			}

			TArray<FOnVoiceIdentityReady> Requests = MoveTemp(PendingIdentityRequests);
			const FString IdToken = CopyIdToken();
			for (FOnVoiceIdentityReady& Request : Requests)
			{
				Request.ExecuteIfBound(LocalProductUserId, IdToken);
			}
		}));
}

void UEdgegapVoiceClientSubsystem::FailLogin(const FString& Reason)
{
	bLoggingIn = false;
	PendingIdentityRequests.Reset();
	UE_LOG(LogEdgegapVoice, Warning, TEXT("Voice: %s. Playing without voice chat"), *Reason);
}

FString UEdgegapVoiceClientSubsystem::CopyIdToken() const
{
	EOS_HConnect Connect = static_cast<EOS_HConnect>(GetConnectInterface());
	if (!Connect || LocalProductUserId.IsEmpty())
	{
		return FString();
	}

	EOS_Connect_CopyIdTokenOptions Options = {};
	Options.ApiVersion = EOS_CONNECT_COPYIDTOKEN_API_LATEST;
	Options.LocalUserId = EOSProductUserIdFromString(*LocalProductUserId);

	FString Jwt;
	EOS_Connect_IdToken* IdToken = nullptr;
	if (EOS_Connect_CopyIdToken(Connect, &Options, &IdToken) == EOS_EResult::EOS_Success && IdToken)
	{
		Jwt = UTF8_TO_TCHAR(IdToken->JsonWebToken);
		EOS_Connect_IdToken_Release(IdToken);
	}
	return Jwt;
}

#else

void* UEdgegapVoiceClientSubsystem::GetConnectInterface() const { return nullptr; }
void UEdgegapVoiceClientSubsystem::Login() {}
void UEdgegapVoiceClientSubsystem::OnDeviceIdReady() {}
void UEdgegapVoiceClientSubsystem::OnLoggedIn(const FString&) {}
void UEdgegapVoiceClientSubsystem::FailLogin(const FString&) {}
FString UEdgegapVoiceClientSubsystem::CopyIdToken() const { return FString(); }

#endif
