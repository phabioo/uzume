# coturn on a VPS

At a glance: Set up a coturn TURN relay on a small VPS in six steps (install, secret, config, firewall, tell the server, check). Only needed for Sessions between apps that cannot connect directly; see [sessions-over-the-internet.md](Sessions-over-the-internet).

Run your own TURN relay on a small VPS and let the Uzume server hand it to apps. The VPS relays only the Session stream of apps that cannot connect directly; saves, ROMs and the server itself stay where they are, so the server needs no router port forward for the relay. Hostnames below are examples: `hub.example.org` is the server, `turn.example.org` the VPS.

## 1. Install

```
sudo apt install coturn
```

On Debian and Ubuntu enable the service in `/etc/default/coturn` (`TURNSERVER_ENABLED=1` on older releases), then `sudo systemctl enable --now coturn`.

## 2. Shared secret

Create a long random secret on the server host and copy it to the VPS (do not paste it into chat or a ticket):

```
openssl rand -base64 32 | sudo tee /etc/uzume/turn-vps.secret >/dev/null
sudo chmod 600 /etc/uzume/turn-vps.secret
```

## 3. Minimal `/etc/turnserver.conf`

```
listening-port=3478
# external IP of the VPS; add "/<private-ip>" when the VPS sits behind NAT (some clouds)
external-ip=203.0.113.10
realm=turn.example.org
fingerprint
lt-cred-mech
use-auth-secret
static-auth-secret=<the same secret as in turn-vps.secret>
# UDP relay range; open exactly this range in the firewall
min-port=49152
max-port=49252
# do not relay to private networks of the VPS
no-multicast-peers
denied-peer-ip=10.0.0.0-10.255.255.255
denied-peer-ip=172.16.0.0-172.31.255.255
denied-peer-ip=192.168.0.0-192.168.255.255
denied-peer-ip=127.0.0.0-127.255.255.255
no-cli
# optional: cap relayed bandwidth per allocation / in total (bytes per second)
# max-bps=500000
```

`use-auth-secret` is the TURN REST API mode: coturn accepts the time-limited usernames (`<expiry>:<device-id>`) the server issues and checks their HMAC-SHA1 password with the secret. `realm` must be set; the server does not need to know it.

Optional TLS (`turns:`, port 5349, helps on networks that only allow 443/5349): add `tls-listening-port=5349`, `cert=/etc/letsencrypt/live/turn.example.org/fullchain.pem` and `pkey=/etc/letsencrypt/live/turn.example.org/privkey.pem` (readable for the coturn user), and renew the certificate with your usual ACME client.

## 4. Firewall

Open on the VPS:

| Port | Protocol | Purpose |
| --- | --- | --- |
| 3478 | UDP and TCP | TURN |
| 5349 | TCP | `turns:` (only with TLS) |
| 49152-49252 | UDP | relay range (`min-port`/`max-port`) |

Example with ufw: `sudo ufw allow 3478 && sudo ufw allow 5349/tcp && sudo ufw allow 49152:49252/udp`. Cloud providers often have a second firewall in their console.

## 5. Tell the server

Easiest: in the app open Server › Settings › Network & sharing › External TURN servers › "+ Add server", enter a name, the URLs (one per line, for example `turn:turn.example.org:3478?transport=udp`), mode "Shared secret" and the `static-auth-secret` from `turnserver.conf`. It applies at once, no restart; the secret is write-only and never shown again. A list saved there replaces the flags below.

Alternatively on the server (`/etc/uzume/server.env` or the service environment), as the initial value:

```
UZUME_TURN_EXTERNAL="name=vps;urls=turn:turn.example.org:3478?transport=udp,turn:turn.example.org:3478?transport=tcp,turns:turn.example.org:5349?transport=tcp;secret-file=/etc/uzume/turn-vps.secret"
```

Leave out the `turns:` URL without TLS. Restart the server after editing `server.env` (not needed when you add it in the app). Server › Settings › Network & sharing lists the server under "External TURN servers". You can keep `-turn` on as well; apps then get both and ICE picks a working path. For a provider that only gives a fixed login use `mode=static;username=...;password-file=...` instead (see [server-configuration.md](Server-configuration)).

## 6. Check

Force the relay on an app (`UZUME_FORCE_RELAY=1`, see [sessions-over-the-internet.md](Sessions-over-the-internet)) and join a Session: the overlay should show "relay". If it does not, check on the VPS `journalctl -u coturn` (wrong secret shows as "Wrong credentials" / 401), the firewall and that the server clock and the VPS clock are within a few minutes (the username carries an expiry time).
