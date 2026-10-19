#pragma once

#include "ishtariaadmin/Db.h"

#include <string>
#include <vector>

namespace ishtariaadmin {

// A story datadisk installed on this host (see ishtaria-server --list-datadisks).
struct Datadisk {
    std::string id;
    std::string version;
    std::string name;
};

// A datadisk of the active world compared with the copy installed on this host.
struct DiskStatus {
    std::string id;
    std::string name;
    std::string worldVersion;     // version the world was generated with
    std::string installedVersion; // empty when the disk is no longer installed
    bool newer = false;           // the installed copy is newer than the world's
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

struct WorldStatus {
    std::vector<DiskStatus> disks;
    GeneratorState generator = GeneratorState::Unavailable;
    std::string generatorDetail; // why the generator could not be checked
    bool updateAvailable() const;
};

// Administrative operations on the ishtaria-server database. Every function
// throws DbError (database problem) or OpError (refused by a safety rule).
struct OpError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class Ops {
public:
    explicit Ops(Db &db) : db_(db) {}

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
    void generateMap(const std::string &name, unsigned long long seed, int faceSize,
                     const std::vector<std::string> &datadisks = {});
    // Story datadisks installed on this host; empty when ishtaria-server is not installed.
    std::vector<Datadisk> installedDatadisks();

    // Compares the datadisks and the generator of the active world with what is installed.
    // Regenerating the map for the generator check takes a moment (skip with checkGenerator=false).
    WorldStatus worldStatus(bool checkGenerator = true);
    // Generates a map with the active seed and face size and the world's datadisks at their
    // installed versions, saves it in the library and returns its name. Does not activate it.
    std::string prepareWorldUpdate();

    // --- players ----------------------------------------------------------
    // Rows: id, username, state (alive/dead/banned), gold, created_at
    Rows listPlayers();
    void renamePlayer(long id, const std::string &name);
    void banPlayer(long id, const std::string &reason);
    void unbanPlayer(long id);
    void deletePlayer(long id);

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
    // Asks systemd to start the ishtaria-server unit. Throws OpError when it
    // refuses: starting a unit needs root privileges or a polkit rule for the
    // ishtaria user.
    void startService();
    // The address the server is configured to listen on: the `listen` key of its
    // configuration (/etc/ishtaria/server.toml, $ISHTARIA_SERVER_CONFIG overrides),
    // the server's built-in default when the key is absent. Throws OpError when
    // the configuration cannot be read.
    std::string serverListen();
    // False while the server has not created the world yet (run it once first).
    bool worldInitialised();
    // Row: seconds_left, message (empty when nothing is scheduled).
    Row scheduledShutdown();
    void scheduleShutdown(int delaySeconds, const std::string &message);
    void cancelShutdown();

    static bool validUsername(const std::string &name);

private:
    long worldId();
    Db &db_;
};

} // namespace ishtariaadmin
