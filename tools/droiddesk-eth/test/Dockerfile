# Hosts for the droiddesk-eth test: everything the namespaces run, plus the tools that check
# the daemon's output (curl, socat, tcpdump, proxychains4).
FROM debian:trixie
RUN apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
        iproute2 dnsmasq-base busybox python3 curl socat tcpdump proxychains4 util-linux procps \
    && rm -rf /var/lib/apt/lists/*
