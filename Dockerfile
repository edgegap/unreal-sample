# Runtime image for the packaged Linux dedicated server.
# Package first (see README), then from the repo root:
#   docker build --platform linux/amd64 -t <image>:<tag> .
FROM ubuntu:22.04

ARG DEBIAN_FRONTEND=noninteractive
# ca-certificates: HTTPS for the server's self-stop call to Edgegap.
RUN apt-get update && \
    apt-get install -y --no-install-recommends ca-certificates && \
    apt-get clean && rm -rf /var/lib/apt/lists/*

# Unreal refuses to start as root.
RUN useradd -m -u 1000 server
WORKDIR /app
COPY --chown=server:server Packaged/LinuxServer /app/
RUN chmod +x /app/MultiplayerGame/Binaries/Linux/MultiplayerGameServer
USER server

# Documentation only: the port Edgegap exposes is the one on the app version.
EXPOSE 7777/udp

# Run the binary directly, not MultiplayerGameServer.sh: that script doesn't exec, so it would
# stay PID 1 and swallow the SIGTERM Edgegap sends when the deployment stops.
# -stdout -FullStdOutLogOutput: without them stdout is buffered and Edgegap logs stay empty.
CMD ["/app/MultiplayerGame/Binaries/Linux/MultiplayerGameServer", "MultiplayerGame", "-log", "-port=7777", "-stdout", "-FullStdOutLogOutput"]
