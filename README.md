# Edgegap Unreal Sample
Sample Unreal integration with Edgegap. Use with the Edgegap Plugin to get your game online quickly:

https://github.com/edgegap/edgegap-unreal-plugin

One the server has been deployed, you can connect game clients by opening the console by pressing the `` ` `` key and entering the following command:

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
