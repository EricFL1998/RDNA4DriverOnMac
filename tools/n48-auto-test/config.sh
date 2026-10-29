# Central connection settings; no credentials are stored here.
N48_TARGET=${N48_TARGET:-louxuanchen@louxuanchendeMac-Pro.local}
N48_SSH_KEY=${N48_SSH_KEY:-ssh-keys/n48-debug}
N48_KNOWN_HOSTS=${N48_KNOWN_HOSTS:-/tmp/n48_known_hosts_v6}
N48_SETTLE_SECONDS=${N48_SETTLE_SECONDS:-30}
N48_DIAGNOSTIC_DIR=${N48_DIAGNOSTIC_DIR:-diagnostics/n48-auto-test}
N48_IP_FAMILY=${N48_IP_FAMILY:--6}

n48_ssh() {
    ssh "$N48_IP_FAMILY" -i "$N48_SSH_KEY" -o UserKnownHostsFile="$N48_KNOWN_HOSTS" \
        -o ConnectTimeout=5 -o ServerAliveInterval=10 -o ServerAliveCountMax=6 \
        "$N48_TARGET" "$@"
}

n48_scp() {
    scp "$N48_IP_FAMILY" -i "$N48_SSH_KEY" -o UserKnownHostsFile="$N48_KNOWN_HOSTS" \
        -o ConnectTimeout=5 "$@"
}
