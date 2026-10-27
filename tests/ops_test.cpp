// Exercises Ops against a throw-away database prepared by run_ops_test.sh.
#include "ishtariaadmin/Ops.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unistd.h>

using namespace ishtariaadmin;

static int failures = 0;
#define CHECK(cond)                                                                      \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            std::cerr << "FAILED " << __LINE__ << ": " #cond "\n";                       \
            ++failures;                                                                  \
        }                                                                                \
    } while (0)

template <typename F> static bool throwsOp(F f) {
    try {
        f();
    } catch (const OpError &) {
        return true;
    }
    return false;
}

int main() {
    const char *url = std::getenv("DATABASE_URL");
    if (url == nullptr) {
        std::cerr << "DATABASE_URL is required\n";
        return 77;
    }
    Db db(url);
    Ops ops(db, url);

    // maps
    CHECK(!ops.activeMap()[0].empty());
    ops.saveActiveMap("first");
    CHECK(ops.listMaps().size() == 1);
    CHECK(throwsOp([&] { ops.saveActiveMap(""); }));
    bool duplicate = false;
    try {
        ops.saveActiveMap("first");
    } catch (const DbError &) {
        duplicate = true;
    }
    CHECK(duplicate);
    const long mapId = std::stol(ops.listMaps()[0][0]);
    ops.renameMap(mapId, "renamed");
    CHECK(ops.listMaps()[0][1] == "renamed");
    std::remove("ops_test_export.pgm");
    ops.exportMap(mapId, "ops_test_export.pgm");
    CHECK(throwsOp([&] { ops.exportMap(mapId, "ops_test_export.pgm"); })); // never overwrites
    CHECK(symlink("ops_test_export.pgm", "ops_test_link.pgm") == 0);
    CHECK(throwsOp([&] { ops.exportMap(mapId, "ops_test_link.pgm"); })); // never follows a symlink
    std::remove("ops_test_link.pgm");
    std::ifstream pgm("ops_test_export.pgm", std::ios::binary);
    std::string magic(2, '\0');
    pgm.read(magic.data(), 2);
    CHECK(magic == "P5");
    ops.loadMap(mapId, false); // no living players yet
    std::remove("ops_test_export.pgm");

    // players: created through SQL, as the server does on registration
    db.exec("INSERT INTO players (world_id, username, password_hash) SELECT id, 'admin-test', 'x' FROM worlds");
    db.exec("INSERT INTO players (world_id, username, password_hash) SELECT id, 'second-test', 'x' FROM worlds");
    CHECK(throwsOp([&] { ops.loadMap(mapId, false); })); // living players now
    ops.loadMap(mapId, true);
    const long pid = std::stol(db.exec("SELECT id FROM players WHERE username = 'admin-test'")[0][0]);
    CHECK(throwsOp([&] { ops.renamePlayer(pid, "no"); }));
    ops.renamePlayer(pid, "admin-renamed");
    db.exec("INSERT INTO player_sessions (token_hash, player_id) VALUES (decode(repeat('ab', 32), 'hex'), $1)", {std::to_string(pid)});
    ops.banPlayer(pid, "test");
    CHECK(db.exec("SELECT count(*) FROM player_sessions WHERE player_id = $1", {std::to_string(pid)})[0][0] == "0");
    CHECK(ops.listPlayers()[0][2] == "banned");
    ops.unbanPlayer(pid);
    ops.deletePlayer(pid);
    CHECK(ops.listPlayers().size() == 1);

    // portals
    ops.createPortal("gate-one", "other.example.org", 1, 10, 20);
    CHECK(throwsOp([&] { ops.createPortal("Bad Name", "", 0, 0, 0); }));
    const long portal = std::stol(ops.listPortals()[0][0]);
    CHECK(ops.listPortals()[0][3] == "building");
    for (const char *state : {"open", "closed", "disabled", "banned"}) {
        ops.setPortalState(portal, state);
        CHECK(ops.listPortals()[0][3] == state);
    }
    ops.deletePortal(portal);

    // A link waiting for the operator is shown as pending; Open approves it, Close breaks it
    // and leaves a message for the other world.
    ops.createPortal("linked-gate", "other.example.org", 1, 5, 5);
    const long linked = std::stol(ops.listPortals()[0][0]);
    db.exec("INSERT INTO portal_pacts (world_id, role, player_id, portal_name, state, portal_id, site_x, site_y, site_z, peer_host, peer_portal_id) "
            "SELECT p.world_id, 'builder', pl.id, 'linked-gate', 'pending', $1::bigint, 1, 2, 3, 'other.example.org', "
            "'0192f3a1-5b1e-7c3a-9d4e-1a2b3c4d5e6f'::uuid FROM portals p, players pl WHERE p.id = $1 LIMIT 1",
            {std::to_string(linked)});
    CHECK(ops.listPortals()[0][3] == "pending");
    ops.setPortalState(linked, "open");
    CHECK(db.exec("SELECT state FROM portal_pacts WHERE portal_id = $1", {std::to_string(linked)})[0][0] == "open");
    ops.setPortalState(linked, "closed");
    CHECK(db.exec("SELECT state || coalesce(peer_host, '-') FROM portal_pacts WHERE portal_id = $1", {std::to_string(linked)})[0][0] == "closed-");
    CHECK(db.exec("SELECT count(*) FROM portal_unlinks")[0][0] == "1");
    // An unlinked finished portal cannot be opened by the operator.
    ops.createPortal("lonely-gate", "", 1, 9, 9);
    const long lonely = std::stol(db.exec("SELECT id FROM portals WHERE name = 'lonely-gate'")[0][0]);
    db.exec("INSERT INTO portal_pacts (world_id, role, player_id, portal_name, state, portal_id, site_x, site_y, site_z) "
            "SELECT p.world_id, 'builder', pl.id, 'lonely-gate', 'built', $1::bigint, 4, 5, 6 FROM portals p, players pl WHERE p.id = $1 LIMIT 1",
            {std::to_string(lonely)});
    CHECK(throwsOp([&] { ops.setPortalState(lonely, "open"); }));
    db.exec("DELETE FROM portal_pacts");
    ops.deletePortal(linked);
    ops.deletePortal(lonely);
    CHECK(ops.listPortals().empty());

    // shutdown
    CHECK(ops.scheduledShutdown().empty());
    ops.scheduleShutdown(60, "Maintenance");
    CHECK(ops.scheduledShutdown()[1] == "Maintenance");
    ops.cancelShutdown();
    CHECK(ops.scheduledShutdown().empty());
    CHECK(throwsOp([&] { ops.scheduleShutdown(-1, ""); }));

    // startup status: the service state never throws (Unknown without systemd)
    // and the world exists after the server has run once
    const auto service = ops.serviceStatus();
    if (service.state != ServiceStatus::State::Unknown) {
        CHECK(service.detail.empty());
    }
    CHECK(ops.worldInitialised());
    CHECK(!ops.needsSetup());
    CHECK(ops.playerCount() == 1);

    // the listen address comes from the server configuration ($ISHTARIA_SERVER_CONFIG):
    // the top-level `listen` key, table keys and comments do not count, the default applies
    const char *savedConfig = std::getenv("ISHTARIA_SERVER_CONFIG");
    const std::string savedConfigCopy = savedConfig != nullptr ? savedConfig : "";
    CHECK(ops.serverListen() == "0.0.0.0:7400");
    {
        std::ofstream out("ops_test_listen.toml");
        out << "[limits]\nlisten = \"0.0.0.0:9999\"\n";
    }
    CHECK(setenv("ISHTARIA_SERVER_CONFIG", "ops_test_listen.toml", 1) == 0);
    CHECK(ops.serverListen() == "0.0.0.0:7400"); // a table key is not the top-level key
    {
        std::ofstream out("ops_test_listen.toml", std::ios::trunc);
        out << "# listen = \"commented out\"\nlisten = \"127.0.0.1:7500\"\n";
    }
    CHECK(ops.serverListen() == "127.0.0.1:7500");
    CHECK(std::remove("ops_test_listen.toml") == 0);
    // Restore the test harness config; bootstrapWorld needs a readable server.toml.
    if (savedConfig != nullptr) {
        setenv("ISHTARIA_SERVER_CONFIG", savedConfigCopy.c_str(), 1);
    } else {
        unsetenv("ISHTARIA_SERVER_CONFIG");
    }

    ops.generateMap("generated", 99, 16);
    CHECK(ops.listMaps().size() == 2);
    CHECK(db.exec("SELECT seed || face_size::text FROM world_maps WHERE name = 'generated'")[0][0] == "9916");
    CHECK(db.exec("SELECT octet_length(pixels) FROM world_maps WHERE name = 'generated'")[0][0] == std::to_string(96 * 16 * 2));
    CHECK(throwsOp([&] { ops.generateMap("tiny", 1, 4); }));

    // story datadisks: offered when installed, checked, stored with the map and applied on load
    const auto disks = ops.installedDatadisks();
    CHECK(disks.size() == 1 && disks[0].id == "testdisk" && disks[0].version == "1.0.0");
    CHECK(throwsOp([&] { ops.generateMap("ghost", 5, 16, {"nosuchdisk"}); }));
    ops.generateMap("with-story", 7, 16, {"testdisk"});
    const auto maps = ops.listMaps();
    const auto story = std::find_if(maps.begin(), maps.end(), [](const Row &r) { return r[1] == "with-story"; });
    CHECK(story != maps.end() && (*story)[6] == "testdisk");
    CHECK(db.exec("SELECT count(*) FROM world_datadisks")[0][0] == "0");
    db.exec("INSERT INTO story_anchors (world_id, anchor_id, heightmap_sha256, direction_x, direction_y, direction_z, height_m) "
            "SELECT id, 'testdisk:old', repeat('a', 64), 1, 0, 0, 10 FROM worlds");
    ops.loadMap(std::stol((*story)[0]), true);
    CHECK(db.exec("SELECT disk_id || ' ' || version FROM world_datadisks")[0][0] == "testdisk 1.0.0");
    CHECK(db.exec("SELECT count(*) FROM story_anchors")[0][0] == "0"); // placed again on the new terrain
    ops.saveActiveMap("saved-with-story");
    CHECK(db.exec("SELECT datadisks->0->>'id' FROM world_maps WHERE name = 'saved-with-story'")[0][0] == "testdisk");

    // world status: datadisk and generator updates are announced
    auto status = ops.worldStatus();
    CHECK(status.disks.size() == 1 && status.disks[0].installedVersion == "1.0.0" && !status.disks[0].newer);
    CHECK(status.generator == GeneratorState::Current && !status.updateAvailable());
    db.exec("UPDATE world_datadisks SET version = '0.9.0'");
    status = ops.worldStatus(false);
    CHECK(status.disks[0].newer && status.updateAvailable());
    db.exec("UPDATE world_datadisks SET version = '1.0.0'");
    db.exec("UPDATE heightmaps SET sha256 = repeat('b', 64)");
    status = ops.worldStatus();
    CHECK(status.generator == GeneratorState::Changed && status.updateAvailable());
    const std::string updateName = ops.prepareWorldUpdate();
    const auto updated = ops.listMaps();
    const auto update = std::find_if(updated.begin(), updated.end(), [&](const Row &r) { return r[1] == updateName; });
    CHECK(update != updated.end() && (*update)[2] == "7" && (*update)[3] == "16" && (*update)[6] == "testdisk");
    if (update != updated.end()) {
        ops.deleteMap(std::stol((*update)[0]));
    }

    ops.loadMap(mapId, true);
    CHECK(db.exec("SELECT count(*) FROM world_datadisks")[0][0] == "0");
    CHECK(ops.worldStatus(false).disks.empty());

    ops.deleteMap(mapId);
    CHECK(ops.listMaps().size() == 3);
    for (const auto &row : ops.listMaps()) {
        if (row[1] != "generated") {
            ops.deleteMap(std::stol(row[0]));
        }
    }
    CHECK(ops.listMaps().size() == 1);
    ops.deleteMap(std::stol(ops.listMaps()[0][0]));
    CHECK(ops.listMaps().empty());

    // First-time setup: wipe the schema and import a fresh world (no systemd unit here;
    // serviceStatus is Unknown, so reset is allowed).
    ops.resetDatabase();
    CHECK(!ops.worldInitialised());
    CHECK(ops.needsSetup());
    CHECK(ops.playerCount() == 0);
    ops.bootstrapWorld(12345, 16, {"testdisk"});
    CHECK(ops.worldInitialised());
    CHECK(!ops.needsSetup());
    CHECK(ops.activeMap()[0] == "12345");
    CHECK(ops.activeMap()[1] == "16");
    CHECK(db.exec("SELECT disk_id FROM world_datadisks")[0][0] == "testdisk");
    CHECK(throwsOp([&] { ops.bootstrapWorld(1, 16); })); // already imported

    if (failures == 0) {
        std::cout << "all ops tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
