# Edgegap Unreal Sample
Sample Unreal integration with Edgegap. Use with one of the Edgegap plugins for Unreal Engine (the Edgegap Integration Kit, or the legacy plugin) to get your game online quickly:

https://docs.edgegap.com/learn/unreal-engine-games/developer-tools

**Tested with:** Unreal Engine 5.5.4, built from source. The Edgegap Matchmaker end-to-end test passed on 2026-10-14 (two clients matched into one deployment, and the server stopped itself afterwards). The EOS Voice end-to-end test passed on 2026-10-20 (two matched clients in the same voice room heard each other).

Once the server has been deployed, you can connect game clients by opening the console by pressing the `` ` `` key and entering the following command:

```shell
open <YOUR_DEPLOYMENT_IP:PORT>
```

You can follow this guide to help you with the different steps involved in building and deploying your Unreal authoritative server:

https://docs.edgegap.com/learn/unreal-engine-games/getting-started-with-servers

## Edgegap Matchmaker (optional)

The sample can also match players automatically with the [Edgegap Matchmaker](https://docs.edgegap.com/learn/matchmaking). It is **disabled by default**: until you turn it on, the game works exactly as above.

When enabled:

- The menu map shows a **Find Match** button. It measures the player's ping to Edgegap's ping beacons, creates a matchmaking ticket with those latencies, waits for the matchmaker to start a server near the players, then connects to it. `FindMatch` and `CancelMatch` also work as console commands, and launching with `-FindMatch` queues automatically, which helps when testing with several clients.
- A server started by the matchmaker stops its own deployment 15 seconds after the last player leaves, or after 5 minutes if nobody joins, so idle servers don't keep billing. Servers you deploy by hand, or run locally, are never stopped.

### Setup

1. Package the Linux dedicated server into `Packaged/` (engine built from source, with the Linux cross-compile toolchain):

   ```shell
   <UE_ROOT>/Engine/Build/BatchFiles/RunUAT.bat BuildCookRun -project=<PATH>/MultiplayerGame/MultiplayerGame.uproject -noP4 -server -noclient -serverplatform=Linux -serverconfig=Development -build -cook -stage -pak -archive -archivedirectory=<PATH>/Packaged
   ```

2. Build the image from the repository root with the included `Dockerfile`, push it to your registry, and create an app version with port `7777`, protocol `UDP`, named `gameport`:

   ```shell
   docker build --platform linux/amd64 -t <REGISTRY>/<IMAGE>:<TAG> .
   docker push <REGISTRY>/<IMAGE>:<TAG>
   ```

3. In the Edgegap dashboard, create a matchmaker from `matchmaker-config.json`. Change `application.name` and `application.version` to your app and version first. The profile `casual`:

   - matches 2 to 4 players whose ping to a common beacon is under 150 ms and within 100 ms of each other;
   - after 10 seconds of waiting, also accepts a single player and pings up to 250 ms;
   - cancels a ticket after 20 seconds without a match; press **Find Match** again.

   **Playing alone:** with this profile, a player who queues alone is matched alone after the 10-second expansion (`min_team_size` drops to 1) and gets a server of their own, provided their ping to at least one beacon is under 250 ms. Otherwise the ticket expires after 20 seconds.

   The client sends latencies because the profile has a `latencies` rule. If you remove that rule, set `bSendBeaconLatencies=False` in `DefaultGame.ini`, or the matchmaker rejects the tickets.

4. Turn matchmaking on and point the game at your matchmaker in `MultiplayerGame/Config/DefaultGame.ini`. Replace `YOUR_MATCHMAKER_URL` and `YOUR_MATCHMAKER_AUTH_TOKEN` with the URL and auth token shown on the matchmaker's dashboard page:

   ```ini
   [/Script/MultiplayerGame.EdgegapMatchmakerSubsystem]
   bEnabled=True
   MatchmakerUrl="YOUR_MATCHMAKER_URL"
   AuthToken="YOUR_MATCHMAKER_AUTH_TOKEN"
   ```

   Keep the URL in quotes: unquoted, Unreal's ini parser cuts it at `//`. To keep your values out of version control, put the same section in `MultiplayerGame/Config/UserGame.ini` instead, which is git-ignored and overrides `DefaultGame.ini`.

   The matchmaker auth token only lets clients create and read tickets; it gives no access to your Edgegap account, so it is safe to ship in game clients. Never put your Edgegap API token in the game.

5. Launch two clients and press **Find Match** in both within 10 seconds. They join the same server.

## EOS Voice chat (optional)

The sample can add in-game voice chat with [EOS Voice](https://dev.epicgames.com/docs/epic-online-services/multiplayer/voice-and-rtc-interface/voice-interface/voice-overview) from Epic Online Services. It is **disabled by default**: until you turn it on, the game works exactly as above.

The dedicated server is the [trusted server](https://dev.epicgames.com/docs/epic-online-services/multiplayer/voice-and-rtc-interface/voice-interface/voice-overview#voice-with-a-trusted-server-application) that decides who talks to whom:

1. The game client logs the player into EOS with a device ID, so players need no Epic account, and sends the server its Product User ID with an EOS ID token.
2. The server checks the ID token against Epic's public keys, so a player can't claim someone else's ID. Then it picks the player's room. In code, rooms are per matchmaker team (`<match>-team-<team id>`, the team found by matching the player's IP to the matchmaker tickets), or one per match for players who connected by IP. The shipped `matchmaker-config.json` has a single team (`team_count: 1`), so with it everyone in a match shares one room.
3. The server asks the [EOS Voice Web API](https://dev.epicgames.com/docs/web-api-ref/voice-web-api) for a room token for that player, passing the player's IP so EOS uses a voice server near them, and sends the token to that player only.
4. The client joins the room with the engine's EOSVoiceChat plugin. When a player leaves, the server removes them from the room, which also revokes their token. Game code can also mute a player for everyone with `UEdgegapVoiceServerSubsystem::SetHardMuted`.

The server uses the Web API over HTTPS rather than the EOS SDK because the EOS SDK doesn't support voice on Linux. The client side builds for Windows and Mac, where the EOSVoiceChat plugin exists. The game mode and player controller don't change: the server adds a small replicated component to each player when they log in, and that component carries the voice messages. Type `VoiceMute` in the console to mute or unmute your microphone.

| File | What it does |
|---|---|
| `EdgegapVoiceSettings.h` | The on/off switch and options, from `DefaultGame.ini` |
| `EdgegapVoiceClientSubsystem` | EOS device ID login, ID token, joining and leaving rooms |
| `EdgegapVoiceComponent` | Added to every player by the server; carries the `ServerRegisterVoice` and `ClientJoinVoiceRoom` RPCs |
| `EdgegapVoiceServerSubsystem` | ID token check, room choice, EOS Voice Web API calls, removal on logout |

### Setup

1. In the [Epic Developer Portal](https://dev.epicgames.com/portal), open your product, then **Product Settings**:
   - On the **SDK Download & Credentials** tab, note the Product ID, and the Sandbox ID and Deployment ID you'll use.
   - On the **Clients** tab, under **Client policies**, add two policies:
     - **Game client**: type `GameClient`, **User required** checked. It needs no Voice permission.
     - **Trusted server**: **Create custom policy**, **User required** unchecked, and the Voice actions `createRoomToken`, `kick` and `mute`.
   - Add one client for each policy, and note each Client ID and Client Secret.

   The trusted server's client can create a token for any room in your deployment. Never put it in a game build.

2. Fill in `[EOSVoiceChat]` in `MultiplayerGame/Config/DefaultEngine.ini` with the product, sandbox and deployment IDs and the **game client's** ID and secret. To keep them out of version control, put the same section in `MultiplayerGame/Config/UserEngine.ini` instead (git-ignored).

3. Turn voice on in `MultiplayerGame/Config/DefaultGame.ini`, or in the git-ignored `UserGame.ini`:

   ```ini
   [/Script/MultiplayerGame.EdgegapVoiceSettings]
   bEnabled=True
   ```

4. Give the server the trusted server's credentials. On Edgegap, add these environment variables to your app version, and mark the secret as hidden:

   | Variable | Value |
   |---|---|
   | `EOS_VOICE_CLIENT_ID` | Trusted server Client ID |
   | `EOS_VOICE_CLIENT_SECRET` | Trusted server Client Secret |
   | `EOS_VOICE_DEPLOYMENT_ID` | EOS Deployment ID (optional if it's already in `DefaultEngine.ini`) |
   | `EOS_VOICE_GAME_CLIENT_ID` | Game client's Client ID, optional: the server then only accepts ID tokens issued to your game client |

   Voice turns on in the server when these are set, so you don't need to rebuild the server image to enable it. For a server running locally, put `ServerClientId` and `ServerClientSecret` in the `EdgegapVoiceSettings` section of `UserGame.ini` instead.

5. Package the server and the client again, deploy, and play. With the matchmaker enabled and `bTeamRooms=True` (the default), each matchmaker team gets its own room. The shipped `matchmaker-config.json` has one team, so everyone in the match ends up in the same room; separate team rooms (a profile with `team_count` above 1) have not been tested. Set `bTeamRooms=False` to always put the whole match in one room.

### Testing with two clients on one PC

A device ID belongs to the Windows user, so two clients on the same PC would log in as the same EOS player. Add `-EOSVoiceNewDeviceId` to the second client's command line to give it a new identity; it creates a new EOS user on every launch, so use it for tests only. Start the second client only after the first logs `Voice: logged into EOS`: replacing the device ID while the first client is still logging in gives both clients the same identity.

To check that audio flows without talking into a microphone, start a client with `-ExecCmds="EOSVoiceChat.FakeAudioInputEnabled 1"` (Development builds). It sends a test signal, and the other client logs `Voice: <id> started talking in room <room>`.

With the matchmaker, both clients must queue within 10 seconds of each other or the `casual` profile starts a solo match. Clients start at different speeds, so launch both with the same `-FindMatchAt=<unix time>` (for example two minutes ahead) instead of `-FindMatch`: each queues at that moment.

Log lines to look for (`LogEdgegapVoice`):

- Server: `Voice: trusted voice server ready`, then `Voice: <id> gets room <room>` for each player, and `Voice: removed <id>` when a player leaves.
- Client: `Voice: logged into EOS as <id>`, `Voice: joined room <room>`, and `Voice: <id> started talking`.

## Security

**Known limitation:** the server does not check that a connecting player holds a matchmaker ticket for this match, so anyone who learns the server address can join. With voice on, such a player also gets a token for the match's voice room. Deployments started by the matchmaker stop themselves when no one joins or everyone leaves, which limits cost. Before production, validate each player against the match's tickets (`MM_TICKET_IDS` / the matchmaker API) or an auth token, for example in `AMultiplayerGameGameMode::PreLogin`.

## License

Edgegap's code in this repository is available under the [MIT License](LICENSE). The Unreal Engine content it builds on (everything in `MultiplayerGame/Content/` and the Top Down template source files marked `Copyright Epic Games, Inc.`) is not MIT: it remains under the [Unreal Engine EULA](https://www.unrealengine.com/eula/unreal). See [LICENSE](LICENSE) for which files are which.
