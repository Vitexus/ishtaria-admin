#pragma once

#include "ishtariaadmin/Db.h"

#include <functional>
#include <string>
#include <vector>

namespace ishtariaadmin {

// Called while ishtaria-worldgen runs: done face rows out of total (0..total).
// Empty / default means no UI feedback. Lines come from the generator's stderr.
using GeneratorProgress = std::function<void(unsigned done, unsigned total)>;


// A story datadisk installed on this host (see ishtaria-server --list-datadisks).
struct Datadisk {
    std::string id;
    std::string version;
    std::string name;
};

// Result of `ishtaria-server --list-datadisks`: disks that loaded, plus skip/load
// failures from stderr (e.g. unknown manifest field).
struct DatadiskListing {
    std::vector<Datadisk> disks;
    // id → server message after "skipping <path>: " (id is the directory basename).
    std::vector<std::pair<std::string, std::string>> failures;
};

// A datadisk of the active world compared with the copy installed on this host.
struct DiskStatus {
    std::string id;
    std::string name;
    std::string worldVersion;     // version the world was generated with
    std::string installedVersion; // empty when the disk is no longer installed / cannot load
    bool newer = false;           // the installed copy is newer than the world's
    // Why the installed copy could not be loaded (server skip line); empty when OK or absent.
    std::string loadError;
};

enum class GeneratorState {
    Current,     // regenerating the map reproduces it exactly
    Changed,     // the installed generator produces a different map for the same seed
    Unavailable, // not checked (no map, generator missing or failing)
};

// State of the ishtaria-server systemd unit on this host.
struct ServiceStatus {
    enum class State { Running, Stopped, Failed, Unknown };
    State state = State::Unknown;
    std::string detail; // why the state could not be told (Unknown only)
};

// Where the world's shared solar clock stands (worlds.solar_offset_seconds): the
// world runs this many seconds ahead of the real clock. The Day/Night spells jump
// it; the client's HUD shows it. Times are local solar time at the world's prime
// meridian (the centre of face 0, +X) — the same clock the HUD shows there.
struct SolarClock {
    long offsetSeconds = 0;
    std::string localTime; // "HH:MM:SS" on the world clock
    std::string realTime;  // "HH:MM:SS" on the real clock
    bool day = true;       // the sun is above the horizon (world clock)
};

struct WorldStatus {
    std::vector<DiskStatus> disks;
    GeneratorState generator = GeneratorState::Unavailable;
    std::string generatorDetail; // why the generator could not be checked
    // Skip failures for disks that are on disk but not used by this world (still useful to show).
    std::vector<std::pair<std::string, std::string>> otherDiskFailures;
    bool updateAvailable() const;
    bool hasDiskLoadErrors() const;
};

// Administrative operations on the ishtaria-server database. Every function
// throws DbError (database problem) or OpError (refused by a safety rule).
struct OpError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class Ops {
public:
    // `databaseUrl` is passed to ishtaria-server --import-only (as DATABASE_URL)
    // when bootstrapping a fresh world after a database reset.
    Ops(Db &db, std::string databaseUrl) : db_(db), databaseUrl_(std::move(databaseUrl)) {}

    // --- world maps -------------------------------------------------------
    // Rows: id, name, seed, face_size, sha256 prefix, created_at, datadisk ids (comma separated)
    Rows listMaps();
    // Row: seed, face_size, sha256 prefix (empty when no map is imported).
    Row activeMap();
    void saveActiveMap(const std::string &name);
    // Replaces the active map. Refused while living players exist unless force.
    void loadMap(long id, bool force);
    void renameMap(long id, const std::string &name);
    void deleteMap(long id);
    void exportMap(long id, const std::string &path);
    // Runs ishtaria-worldgen (or $ISHTARIA_WORLDGEN) and stores the result in the
    // library under `name`. The active map is not touched.
    // `datadisks` are ids of installed story datadisks the generated world takes into
    // account; they are checked to be combinable and stored with the map.
    // `progress` is invoked from the generator's PROGRESS stderr lines when set.
    void generateMap(const std::string &name, unsigned long long seed, int faceSize,
                     const std::vector<std::string> &datadisks = {},
                     GeneratorProgress progress = {});
    // Story datadisks installed on this host; empty when ishtaria-server is not installed.
    // Prefer listDatadisks() when load failures must be shown to the operator.
    std::vector<Datadisk> installedDatadisks();
    // Same listing plus stderr skip messages from a rejected/broken datadisk.yaml.
    DatadiskListing listDatadisks();

    // Compares the datadisks and the generator of the active world with what is installed.
    // Regenerating the map for the generator check takes a moment (skip with checkGenerator=false).
    WorldStatus worldStatus(bool checkGenerator = true, GeneratorProgress progress = {});
    // Generates a map with the active seed and face size and the world's datadisks at their
    // installed versions, saves it in the library and returns its name. Does not activate it.
    std::string prepareWorldUpdate(GeneratorProgress progress = {});

    // --- players ----------------------------------------------------------
    // Rows: id, username, state (alive/dead/banned), gold, created_at
    Rows listPlayers();
    void renamePlayer(long id, const std::string &name);
    void banPlayer(long id, const std::string &reason);
    void unbanPlayer(long id);
    void deletePlayer(long id);
    // Number of player rows (any state). 0 when the schema is not initialised yet.
    long playerCount();

    // --- linked worlds ------------------------------------------------------
    // Rows: id, name, peer, state, face, x, y
    Rows listPortals();
    void createPortal(const std::string &name, const std::string &peer, int face, int x, int y);
    void setPortalState(long id, const std::string &state);
    void deletePortal(long id);

    // --- server -------------------------------------------------------------
    // State of the ishtaria-server systemd unit. Does not throw: the state is
    // Unknown when systemd is not available on this host.
    ServiceStatus serviceStatus();
    // Asks systemd to start the ishtaria-server unit (via `sudo -n` when
    // /etc/sudoers.d/ishtaria-server is installed), then checks that the unit
    // is actually active (Restart=on-failure can make `systemctl start` succeed
    // while the process dies immediately). Throws OpError with systemctl output
    // and recent journal lines (e.g. "Address already in use") when it does not
    // stay up.
    void startService();
    // Asks systemd to stop the ishtaria-server unit. Succeeds when already stopped.
    // Throws OpError when the unit stays running or systemd refuses the request.
    void stopService();
    // The address the server is configured to listen on: the `listen` key of its
    // configuration (/etc/ishtaria/server.toml, $ISHTARIA_SERVER_CONFIG overrides),
    // the server's built-in default when the key is absent. Throws OpError when
    // the configuration cannot be read.
    std::string serverListen();
    // False while the server has not created the world yet, or the schema is missing.
    bool worldInitialised();
    // True when there is no world row or no active heightmap (first-time setup).
    bool needsSetup();
    // Drops every table in the public schema (accounts, maps, story state).
    // Call stopService first when the unit serves this database, then
    // bootstrapWorld. Equivalent to recreating the database from template0
    // without needing CREATEDB.
    void resetDatabase();
    // Runs migrations and imports a freshly generated map (same path as
    // ishtaria-server-init): worldgen + ishtaria-server --import-only. The
    // database must be empty (after resetDatabase or a brand-new install).
    void bootstrapWorld(unsigned long long seed, int faceSize,
                        const std::vector<std::string> &datadisks = {},
                        GeneratorProgress progress = {});
    // Row: seconds_left, message (empty when nothing is scheduled).
    Row scheduledShutdown();
    void scheduleShutdown(int delaySeconds, const std::string &message);
    void cancelShutdown();

    // --- world clock ----------------------------------------------------------
    // Where the shared solar clock stands right now (see SolarClock).
    SolarClock solarClock();
    // Jumps the shared solar clock forward to the next dawn (`day`) or dusk at the
    // world's prime meridian — the jump the Day/Night spells cast at the caster's
    // position. Everyone online is told through a `solar` event.
    void setSolarClock(bool day);
    // Jumps the shared solar clock forward to the next moment the local solar time
    // at the prime meridian is `hour`:`minute` (0-23, 0-59). Everyone online is
    // told through a `solar` event.
    void setSolarClockTime(int hour, int minute);

    static bool validUsername(const std::string &name);

private:
    long worldId();
    // Stores the new solar offset (the schema bounds it to one day) and tells
    // everyone online to look at the sky again, like a cast Day/Night spell.
    void applySolarOffset(double shiftSeconds);
    Db &db_;
    std::string databaseUrl_;
};

} // namespace ishtariaadmin
