// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class MultiplayerGame : ModuleRules
{
	public MultiplayerGame(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "HeadMountedDisplay", "NavigationSystem", "AIModule", "Niagara", "HTTP", "Json", "Sockets", "Slate", "SlateCore" });

		// Optional EOS Voice (see README). The server side checks player ID tokens with OpenSSL and calls the
		// EOS Voice Web API over HTTP, so it builds for Linux. The client side uses the engine's EOSVoiceChat
		// plugin, which only exists for Windows, Mac and Android clients.
		PrivateDependencyModuleNames.Add("OpenSSL");

		bool bWithEosVoiceClient = Target.Type != TargetType.Server
			&& (Target.Platform == UnrealTargetPlatform.Win64 || Target.Platform == UnrealTargetPlatform.Mac);
		if (bWithEosVoiceClient)
		{
			PrivateDependencyModuleNames.AddRange(new string[] { "EOSSDK", "EOSShared", "VoiceChat", "EOSVoiceChat" });
		}
		PublicDefinitions.Add("WITH_EDGEGAP_EOS_VOICE_CLIENT=" + (bWithEosVoiceClient ? "1" : "0"));
    }
}
