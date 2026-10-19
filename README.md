# ishtaria-admin

Text-mode (Turbo Vision) administration tool for an Ishtaria world server.
It works directly on the server's PostgreSQL database, so it needs no admin
HTTP API. Run it on the server host as the service user:

```sh
sudo -u ishtaria ishtaria-admin            # default: postgresql:///ishtaria?host=/var/run/postgresql
ishtaria-admin --database-url=postgresql:///other?host=/var/run/postgresql
```

At start-up the tool shows the state of the `ishtaria-server` systemd unit
(running/stopped/failed), the address the server is configured to listen on,
the active map and the datadisks the world was generated with; when the world
is not initialised yet it says so, when no world map exists it offers
generating a first one (seed, face size, datadisks) and making it active right
away, and when the service is stopped it offers starting it.

| Menu (key) | Function |
| --- | --- |
| World maps (F2) | save the active map into the library, generate, load, rename, delete, export `.pgm`; when story datadisks are installed, *Generate map* offers a checkbox per disk to take it into account (stored with the map, applied on *Load*) |
| Players (F3) | rename, ban/unban (revokes sessions), delete (not possible with a permanent memorial) |
| Linked worlds (F4) | list portals; a link waiting for approval shows as `pending`: *Open* approves it, *Close* breaks it (the other world is told); create, disable, ban or delete portals |
| Server (F5) | shows the service state and the address the server listens on; start the service (needs root privileges or a polkit rule for the `ishtaria` user), schedule a shutdown with a delay and message, or cancel it |
| World status (F6) | compares the world's datadisks with the installed ones and re-runs the generator for the active seed; a newer datadisk or a changed generator is announced at start-up, and *Prepare update* saves a map regenerated with the installed versions (then optionally loads it) |

The generator has no recorded version: it counts as changed when the installed `ishtaria-worldgen`
produces a map with a different SHA-256 for the active seed and face size. Updating means a new
world (datadisks are pinned per world), so loading the prepared map replaces the active one.

Requires the server schema from migration `0013_admin.sql`, which `ishtaria-server`
applies at start-up. Loading a map replaces the active heightmap: restart the
server afterwards. Portals are only records for now; the server does not enforce
them until federation lands.

A scheduled shutdown is announced to clients as a non-dismissable `system`
message in `GET /world` (`messages[]`); the service then exits with status 0, so
systemd does not restart it.

## Build and test

```sh
cmake -S . -B build && cmake --build build
(cd build && ctest --output-on-failure)   # needs PostgreSQL and ../ishtaria-server/target/debug/ishtaria-server
dpkg-buildpackage -us -uc -b
```

English is the source language; Czech via gettext (`po/cs.po`). Error messages
produced by the database layer are English only.

Licence: AGPL-3.0-only.
