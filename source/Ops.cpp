#include "ishtariaadmin/Ops.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <unistd.h>
#include <libpq-fe.h>
#include <regex>
#include <sys/wait.h>

namespace ishtariaadmin {

namespace {
std::string str(long v) { return std::to_string(v); }
std::string toHex(const std::string &bytes) {
    static const char *digits = "0123456789abcdef";
    std::string hex;
    hex.reserve(bytes.size() * 2);
    for (unsigned char c : bytes) {
        hex.push_back(digits[c >> 4]);
        hex.push_back(digits[c & 15]);
    }
    return hex;
}

// Compares "major.minor.patch" versions numerically; unparsable parts count as 0.
int compareVersions(const std::string &a, const std::string &b) {
    auto parse = [](const std::string &v) {
        std::vector<long> parts;
        std::size_t start = 0;
        while (start <= v.size()) {
            const std::size_t dot = v.find('.', start);
            parts.push_back(std::atol(v.substr(start, dot == std::string::npos ? std::string::npos : dot - start).c_str()));
            if (dot == std::string::npos) break;
            start = dot + 1;
        }
        parts.resize(3, 0);
        return parts;
    };
    const auto x = parse(a), y = parse(b);
    return x < y ? -1 : (x == y ? 0 : 1);
}

} // namespace

bool WorldStatus::updateAvailable() const {
    return generator == GeneratorState::Changed ||
           std::any_of(disks.begin(), disks.end(), [](const DiskStatus &d) { return d.newer; });
}

long Ops::worldId() {
    const auto rows = db_.exec("SELECT id FROM worlds ORDER BY id LIMIT 1");
    if (rows.empty()) {
        throw OpError("The world is not initialised yet (run ishtaria-server once).");
    }
    return std::stol(rows[0][0]);
}

bool Ops::validUsername(const std::string &name) {
    static const std::regex re("^[a-zA-Z0-9_-]{3,32}$");
    return std::regex_match(name, re);
}

// --- maps -------------------------------------------------------------------

Rows Ops::listMaps() {
    return db_.exec("SELECT id::text, name, seed, face_size::text, left(sha256, 12), "
                    "to_char(created_at, 'YYYY-MM-DD HH24:MI'), "
                    "coalesce((SELECT string_agg(e->>'id', ',') FROM jsonb_array_elements(datadisks) e), '') "
                    "FROM world_maps ORDER BY name");
}

Row Ops::activeMap() {
    const auto rows = db_.exec("SELECT seed, face_size::text, left(sha256, 12) FROM heightmaps LIMIT 1");
    return rows.empty() ? Row{"", "", ""} : rows[0];
}

void Ops::saveActiveMap(const std::string &name) {
    if (name.empty() || name.size() > 64) {
        throw OpError("The map name must have 1-64 characters.");
    }
    db_.exec("INSERT INTO world_maps (world_id, name, seed, face_size, sha256, pgm, pixels, datadisks) "
             "SELECT world_id, $1, seed, face_size, sha256, pgm, pixels, "
             "coalesce((SELECT jsonb_agg(jsonb_build_object('id', disk_id, 'version', version) ORDER BY position) "
             "FROM world_datadisks d WHERE d.world_id = heightmaps.world_id), '[]'::jsonb) FROM heightmaps",
             {name});
    if (db_.affected() == 0) {
        throw OpError("No map is imported in the world yet.");
    }
}

void Ops::loadMap(long id, bool force) {
    Transaction tx(db_);
    db_.exec("SELECT id FROM worlds WHERE id = $1 FOR UPDATE", {str(worldId())});
    const auto alive = db_.exec("SELECT count(*) FROM players WHERE died_at IS NULL");
    if (!force && alive[0][0] != "0") {
        throw OpError("The world has " + alive[0][0] + " living players; loading a map needs confirmation.");
    }
    db_.exec("UPDATE heightmaps SET seed = m.seed, face_size = m.face_size, sha256 = m.sha256, "
             "pgm = m.pgm, pixels = m.pixels, imported_at = now() "
             "FROM world_maps m WHERE m.id = $1 AND heightmaps.world_id = m.world_id",
             {str(id)});
    if (db_.affected() == 0) {
        // No active map yet: take it from the library.
        db_.exec("INSERT INTO heightmaps (world_id, seed, face_size, sha256, pgm, pixels) "
                 "SELECT world_id, seed, face_size, sha256, pgm, pixels FROM world_maps WHERE id = $1",
                 {str(id)});
        if (db_.affected() == 0) {
            throw OpError("The map no longer exists.");
        }
    }
    // The places of the previous map's story no longer fit the new terrain: the server
    // places them again on its next start, using the disks stored with the loaded map.
    db_.exec("DELETE FROM story_anchors WHERE world_id = (SELECT world_id FROM world_maps WHERE id = $1)", {str(id)});
    db_.exec("DELETE FROM world_datadisks WHERE world_id = (SELECT world_id FROM world_maps WHERE id = $1)", {str(id)});
    db_.exec("INSERT INTO world_datadisks (world_id, disk_id, version, position) "
             "SELECT m.world_id, e.value->>'id', e.value->>'version', (e.ordinality - 1)::int "
             "FROM world_maps m, jsonb_array_elements(m.datadisks) WITH ORDINALITY e "
             "WHERE m.id = $1",
             {str(id)});
    tx.commit();
}

void Ops::renameMap(long id, const std::string &name) {
    if (name.empty() || name.size() > 64) {
        throw OpError("The map name must have 1-64 characters.");
    }
    db_.exec("UPDATE world_maps SET name = $2 WHERE id = $1", {str(id), name});
    if (db_.affected() == 0) {
        throw OpError("The map no longer exists.");
    }
}

void Ops::deleteMap(long id) {
    db_.exec("DELETE FROM world_maps WHERE id = $1", {str(id)});
    if (db_.affected() == 0) {
        throw OpError("The map no longer exists.");
    }
}

void Ops::exportMap(long id, const std::string &path) {
    // bytea comes back hex-encoded in text format; decode it ourselves.
    const auto rows = db_.exec("SELECT encode(pgm, 'hex') FROM world_maps WHERE id = $1", {str(id)});
    if (rows.empty()) {
        throw OpError("The map no longer exists.");
    }
    // Create the file exclusively and never follow a symlink, so an export
    // cannot be redirected onto another file (the tool runs as the service user).
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0640);
    if (fd < 0) {
        throw OpError("Cannot create " + path + ": " + std::strerror(errno) +
                      " (an existing file is never overwritten)");
    }
    const std::string &hex = rows[0][0];
    std::string bytes;
    bytes.reserve(hex.size() / 2);
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        bytes.push_back(static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    }
    std::size_t written = 0;
    while (written < bytes.size()) {
        const ssize_t n = ::write(fd, bytes.data() + written, bytes.size() - written);
        if (n < 0) {
            const int err = errno;
            ::close(fd);
            ::unlink(path.c_str());
            throw OpError(std::string("Cannot write ") + path + ": " + std::strerror(err));
        }
        written += static_cast<std::size_t>(n);
    }
    if (::close(fd) != 0) {
        throw OpError("Cannot write " + path);
    }
}

namespace {

constexpr std::size_t kMaxMapBytes = 16 * 1024 * 1024;

// Runs the generator without a shell and collects its standard output.
std::string runGenerator(const std::string &program, const std::vector<std::string> &args) {
    int fds[2];
    if (::pipe(fds) != 0) {
        throw OpError("Cannot create a pipe.");
    }
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(fds[0]);
        ::close(fds[1]);
        throw OpError("Cannot start the generator.");
    }
    if (pid == 0) {
        ::dup2(fds[1], STDOUT_FILENO);
        ::close(fds[0]);
        ::close(fds[1]);
        std::vector<char *> argv;
        argv.push_back(const_cast<char *>(program.c_str()));
        for (const auto &a : args) {
            argv.push_back(const_cast<char *>(a.c_str()));
        }
        argv.push_back(nullptr);
        ::execvp(program.c_str(), argv.data());
        ::_exit(127);
    }
    ::close(fds[1]);
    std::string out;
    char buffer[65536];
    bool tooBig = false;
    for (;;) {
        const ssize_t n = ::read(fds[0], buffer, sizeof(buffer));
        if (n <= 0) {
            break;
        }
        if (out.size() + static_cast<std::size_t>(n) > kMaxMapBytes) {
            tooBig = true;
            break;
        }
        out.append(buffer, static_cast<std::size_t>(n));
    }
    ::close(fds[0]);
    if (tooBig) {
        ::kill(pid, SIGKILL);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (tooBig) {
        throw OpError("The generated map is larger than 16 MiB.");
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        throw OpError(WIFEXITED(status) && WEXITSTATUS(status) == 127
                          ? program + " is not installed (package ishtaria-worldgen)."
                          : "The generator failed.");
    }
    return out;
}


std::string serverProgram() {
    const char *path = std::getenv("ISHTARIA_SERVER");
    return path != nullptr ? path : "ishtaria-server";
}

// The server configuration path: the one the systemd unit starts it with,
// unless an environment override says otherwise.
std::string serverConfigPath() {
    const char *path = std::getenv("ISHTARIA_SERVER_CONFIG");
    return path != nullptr ? path : "/etc/ishtaria/server.toml";
}

// Runs a program without a shell; returns standard output and error together.
// `status` is the exit code (127 when the program cannot be started, -1 on a signal).
std::string captureOutput(const std::string &program, const std::vector<std::string> &args, int &status) {
    int fds[2];
    if (::pipe(fds) != 0) {
        throw OpError("Cannot create a pipe.");
    }
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(fds[0]);
        ::close(fds[1]);
        throw OpError("Cannot start " + program + ".");
    }
    if (pid == 0) {
        ::dup2(fds[1], STDOUT_FILENO);
        ::dup2(fds[1], STDERR_FILENO);
        ::close(fds[0]);
        ::close(fds[1]);
        std::vector<char *> argv;
        argv.push_back(const_cast<char *>(program.c_str()));
        for (const auto &a : args) {
            argv.push_back(const_cast<char *>(a.c_str()));
        }
        argv.push_back(nullptr);
        ::execvp(program.c_str(), argv.data());
        ::_exit(127);
    }
    ::close(fds[1]);
    std::string out;
    char buffer[4096];
    for (;;) {
        const ssize_t n = ::read(fds[0], buffer, sizeof(buffer));
        if (n <= 0) {
            break;
        }
        if (out.size() < 65536) {
            out.append(buffer, static_cast<std::size_t>(n));
        }
    }
    ::close(fds[0]);
    int raw = 0;
    ::waitpid(pid, &raw, 0);
    status = WIFEXITED(raw) ? WEXITSTATUS(raw) : -1;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
        out.pop_back();
    }
    return out;
}

// The `ishtaria-worldgen --earth` arguments of the world terrain one of the datadisks
// defines (its real planet), empty for a procedural world: `ishtaria-server
// --print-world-terrain` prints the heightmap and water mask paths.
std::vector<std::string> worldTerrainArgs(const std::vector<std::string> &datadisks) {
    if (datadisks.empty()) {
        return {};
    }
    std::string ids;
    for (const auto &id : datadisks) {
        ids += (ids.empty() ? "" : ",") + id;
    }
    int status = 0;
    std::string output;
    try {
        output = captureOutput(serverProgram(), {"--print-world-terrain", ids}, status);
    } catch (const OpError &) {
        return {};
    }
    if (status != 0) {
        return {};
    }
    std::string::size_type start = 0;
    while (start <= output.size()) {
        const auto end = output.find('\n', start);
        const std::string line = output.substr(start, end == std::string::npos ? end : end - start);
        const auto tab = line.find('\t');
        if (tab != std::string::npos && tab > 0 && tab + 1 < line.size() &&
            line.size() - tab > 5 && tab > 5) {
            return {"--earth", line.substr(0, tab), "--water", line.substr(tab + 1)};
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return {};
}

} // namespace

std::vector<Datadisk> Ops::installedDatadisks() {
    int status = 0;
    std::string output;
    try {
        output = captureOutput(serverProgram(), {"--list-datadisks"}, status);
    } catch (const OpError &) {
        return {};
    }
    std::vector<Datadisk> disks;
    if (status != 0) {
        return disks;
    }
    std::size_t start = 0;
    while (start < output.size()) {
        std::size_t end = output.find('\n', start);
        if (end == std::string::npos) {
            end = output.size();
        }
        const std::string line = output.substr(start, end - start);
        start = end + 1;
        const auto first = line.find('\t');
        const auto second = first == std::string::npos ? first : line.find('\t', first + 1);
        if (second == std::string::npos) {
            continue; // a diagnostic line, not a disk
        }
        disks.push_back({line.substr(0, first), line.substr(first + 1, second - first - 1), line.substr(second + 1)});
    }
    return disks;
}

void Ops::generateMap(const std::string &name, unsigned long long seed, int faceSize,
                      const std::vector<std::string> &datadisks) {
    if (name.empty() || name.size() > 64) {
        throw OpError("The map name must have 1-64 characters.");
    }
    if (faceSize < 16 || faceSize > 1024) {
        throw OpError("The face size must be 16-1024.");
    }
    std::string diskJson = "[]";
    if (!datadisks.empty()) {
        const auto installed = installedDatadisks();
        std::string ids;
        diskJson = "[";
        for (const auto &id : datadisks) {
            const auto found = std::find_if(installed.begin(), installed.end(),
                                            [&](const Datadisk &d) { return d.id == id; });
            if (found == installed.end()) {
                throw OpError("Datadisk " + id + " is not installed.");
            }
            ids += (ids.empty() ? "" : ",") + id;
            diskJson += (diskJson.size() > 1 ? "," : "") + std::string("{\"id\":\"") + found->id +
                        "\",\"version\":\"" + found->version + "\"}";
        }
        diskJson += "]";
        int status = 0;
        const std::string output = captureOutput(serverProgram(), {"--check-datadisks", ids}, status);
        if (status != 0) {
            throw OpError("These datadisks cannot be combined: " + output);
        }
    }
    const char *override_path = std::getenv("ISHTARIA_WORLDGEN");
    std::vector<std::string> generatorArgs = worldTerrainArgs(datadisks);
    if (generatorArgs.empty()) {
        generatorArgs = {std::to_string(seed), std::to_string(faceSize)};
    } else {
        // A datadisk defines the world's real planet: the seed does not shape it.
        generatorArgs.push_back(std::to_string(faceSize));
    }
    const std::string pgm = runGenerator(
        override_path != nullptr ? override_path : "ishtaria-worldgen", generatorArgs);
    // Validate the P5 header exactly as the server expects it before storing anything.
    std::size_t pos = 0;
    auto token = [&]() {
        while (pos < pgm.size() && std::isspace(static_cast<unsigned char>(pgm[pos]))) {
            ++pos;
        }
        const std::size_t start = pos;
        while (pos < pgm.size() && !std::isspace(static_cast<unsigned char>(pgm[pos]))) {
            ++pos;
        }
        return pgm.substr(start, pos - start);
    };
    if (token() != "P5") {
        throw OpError("The generator did not produce a binary PGM.");
    }
    const long w = std::atol(token().c_str());
    const long h = std::atol(token().c_str());
    const std::string maxval = token();
    ++pos; // single whitespace after maxval
    if (w != 6L * faceSize || h != faceSize || maxval != "255" ||
        pgm.size() - std::min(pos, pgm.size()) != static_cast<std::size_t>(w * h)) {
        throw OpError("The generated map has an unexpected format.");
    }
    const std::string hex = toHex(pgm);
    db_.exec("INSERT INTO world_maps (world_id, name, seed, face_size, sha256, pgm, pixels, datadisks) "
             "SELECT $1::bigint, $2, $3, $4::int, encode(sha256(decode($5, 'hex')), 'hex'), "
             "decode($5, 'hex'), substring(decode($5, 'hex') FROM $6::int + 1), $7::jsonb",
             {std::to_string(worldId()), name, std::to_string(seed), std::to_string(faceSize), hex,
              std::to_string(pos), diskJson});
}

WorldStatus Ops::worldStatus(bool checkGenerator) {
    WorldStatus status;
    const auto installed = installedDatadisks();
    for (const auto &row : db_.exec("SELECT disk_id, version FROM world_datadisks WHERE world_id = $1 ORDER BY position",
                                    {std::to_string(worldId())})) {
        DiskStatus disk{row[0], row[0], row[1], "", false};
        const auto found = std::find_if(installed.begin(), installed.end(), [&](const Datadisk &d) { return d.id == row[0]; });
        if (found != installed.end()) {
            disk.name = found->name;
            disk.installedVersion = found->version;
            disk.newer = compareVersions(found->version, row[1]) > 0;
        }
        status.disks.push_back(disk);
    }
    if (!checkGenerator) {
        status.generatorDetail = "not checked";
        return status;
    }
    const auto map = db_.exec("SELECT seed, face_size::text, sha256 FROM heightmaps LIMIT 1");
    if (map.empty()) {
        status.generatorDetail = "no map is imported";
        return status;
    }
    const bool numericSeed = !map[0][0].empty() && map[0][0].find_first_not_of("0123456789") == std::string::npos;
    if (!numericSeed) {
        status.generatorDetail = "the map was not generated from a numeric seed";
        return status;
    }
    try {
        const char *override_path = std::getenv("ISHTARIA_WORLDGEN");
        std::vector<std::string> diskIds;
        for (const auto &disk : status.disks) {
            diskIds.push_back(disk.id);
        }
        std::vector<std::string> generatorArgs = worldTerrainArgs(diskIds);
        if (generatorArgs.empty()) {
            generatorArgs = {map[0][0], map[0][1]};
        } else {
            generatorArgs.push_back(map[0][1]);
        }
        const std::string pgm = runGenerator(override_path != nullptr ? override_path : "ishtaria-worldgen",
                                             generatorArgs);
        const auto hash = db_.exec("SELECT encode(sha256(decode($1, 'hex')), 'hex')", {toHex(pgm)});
        status.generator = hash.at(0).at(0) == map[0][2] ? GeneratorState::Current : GeneratorState::Changed;
    } catch (const OpError &e) {
        status.generatorDetail = e.what();
    }
    return status;
}

std::string Ops::prepareWorldUpdate() {
    const auto map = db_.exec("SELECT seed, face_size::text FROM heightmaps LIMIT 1");
    if (map.empty()) {
        throw OpError("No map is imported.");
    }
    char *end = nullptr;
    const unsigned long long seed = std::strtoull(map[0][0].c_str(), &end, 10);
    if (map[0][0].empty() || *end != '\0') {
        throw OpError("The active map was not generated from a numeric seed.");
    }
    std::vector<std::string> ids;
    for (const auto &disk : worldStatus(false).disks) {
        ids.push_back(disk.id);
    }
    const auto stamp = db_.exec("SELECT to_char(now(), 'YYYYMMDD-HH24MI')");
    const std::string name = "update-" + map[0][0] + "-" + stamp.at(0).at(0);
    generateMap(name, seed, std::atoi(map[0][1].c_str()), ids);
    return name;
}

// --- players ------------------------------------------------------------------

Rows Ops::listPlayers() {
    return db_.exec("SELECT id::text, username, "
                    "CASE WHEN banned_at IS NOT NULL THEN 'banned' WHEN died_at IS NOT NULL THEN 'dead' ELSE 'alive' END, "
                    "coalesce((SELECT quantity FROM player_inventory WHERE player_id = players.id AND item_id = 'gold'), 0)::text, "
                    "to_char(created_at, 'YYYY-MM-DD') FROM players ORDER BY lower(username), id");
}

void Ops::renamePlayer(long id, const std::string &name) {
    if (!validUsername(name)) {
        throw OpError("Names use 3-32 characters: letters, digits, '_' and '-'.");
    }
    db_.exec("UPDATE players SET username = $2 WHERE id = $1", {str(id), name});
    if (db_.affected() == 0) {
        throw OpError("The player no longer exists.");
    }
}

void Ops::banPlayer(long id, const std::string &reason) {
    Transaction tx(db_);
    db_.exec("UPDATE players SET banned_at = now(), ban_reason = $2 WHERE id = $1",
             {str(id), reason.substr(0, 500)});
    if (db_.affected() == 0) {
        throw OpError("The player no longer exists.");
    }
    db_.exec("DELETE FROM player_sessions WHERE player_id = $1", {str(id)});
    tx.commit();
}

void Ops::unbanPlayer(long id) {
    db_.exec("UPDATE players SET banned_at = NULL, ban_reason = NULL WHERE id = $1", {str(id)});
}

void Ops::deletePlayer(long id) {
    const auto grave = db_.exec("SELECT 1 FROM graves WHERE player_id = $1", {str(id)});
    if (!grave.empty()) {
        throw OpError("The player has a permanent memorial and cannot be deleted. Ban the account instead.");
    }
    Transaction tx(db_);
    db_.exec("DELETE FROM player_friendships WHERE player_id = $1 OR friend_id = $1", {str(id)});
    db_.exec("DELETE FROM players WHERE id = $1", {str(id)});
    if (db_.affected() == 0) {
        throw OpError("The player no longer exists.");
    }
    tx.commit();
}

// --- portals ------------------------------------------------------------------

Rows Ops::listPortals() {
    // A portal whose link waits for the operator is shown as "pending".
    return db_.exec("SELECT id::text, name, coalesce(peer, ''), "
                    "CASE WHEN EXISTS (SELECT 1 FROM portal_pacts p WHERE p.portal_id = portals.id AND p.state = 'pending') "
                    "THEN 'pending' ELSE state END, face::text, x::text, y::text "
                    "FROM portals ORDER BY name");
}

void Ops::createPortal(const std::string &name, const std::string &peer, int face, int x, int y) {
    static const std::regex nameRe("^[a-z0-9-]{1,64}$");
    static const std::regex peerRe("^[a-z0-9.-]{1,253}$");
    if (!std::regex_match(name, nameRe)) {
        throw OpError("Portal name: 1-64 characters a-z, 0-9 and '-'.");
    }
    if (!peer.empty() && !std::regex_match(peer, peerRe)) {
        throw OpError("The peer must be a lowercase DNS name.");
    }
    if (face < 0 || face > 5 || x < 0 || y < 0) {
        throw OpError("Face is 0-5, coordinates must not be negative.");
    }
    db_.exec("INSERT INTO portals (world_id, name, peer, face, x, y) VALUES ($1, $2, $3, $4, $5, $6)",
             {str(worldId()), name, peer.empty() ? Param{} : Param{peer}, str(face), str(x), str(y)});
}

void Ops::setPortalState(long id, const std::string &state) {
    Transaction tx(db_);
    const auto pact = db_.exec("SELECT id::text, state, peer_host, peer_portal_id::text FROM portal_pacts "
                               "WHERE portal_id = $1 AND role = 'builder' FOR UPDATE",
                               {str(id)});
    if (!pact.empty()) {
        const std::string &pactState = pact[0][1];
        if (state == "open") {
            // Approving: only a link that was made and waits for the operator may open.
            if (pactState != "pending" && pactState != "open") {
                throw OpError("Only a linked portal waiting for approval can be opened.");
            }
            db_.exec("UPDATE portal_pacts SET state = 'open', updated_at = now() WHERE id = $1::uuid", {pact[0][0]});
        } else if (state == "closed" && (pactState == "pending" || pactState == "open")) {
            // Closing a linked portal breaks the link: the other world is told by the server.
            db_.exec("INSERT INTO portal_unlinks (world_id, own_portal_id, peer_host, peer_portal_id) "
                     "SELECT world_id, id, peer_host, peer_portal_id FROM portal_pacts WHERE id = $1::uuid",
                     {pact[0][0]});
            db_.exec("UPDATE portal_pacts SET state = 'closed', peer_host = NULL, peer_portal_id = NULL, "
                     "peer_portal_name = NULL, updated_at = now() WHERE id = $1::uuid",
                     {pact[0][0]});
        } else if (state == "closed") {
            db_.exec("UPDATE portal_pacts SET state = 'closed', updated_at = now() WHERE id = $1::uuid", {pact[0][0]});
        }
    }
    db_.exec("UPDATE portals SET state = $2, updated_at = now() WHERE id = $1", {str(id), state});
    if (db_.affected() == 0) {
        throw OpError("The portal no longer exists.");
    }
    tx.commit();
}

void Ops::deletePortal(long id) {
    db_.exec("DELETE FROM portals WHERE id = $1", {str(id)});
    if (db_.affected() == 0) {
        throw OpError("The portal no longer exists.");
    }
}

// --- server -------------------------------------------------------------------

ServiceStatus Ops::serviceStatus() {
    ServiceStatus status;
    int exitCode = 0;
    std::string output;
    try {
        output = captureOutput("systemctl", {"is-active", "ishtaria-server"}, exitCode);
    } catch (const OpError &e) {
        status.detail = e.what();
        return status;
    }
    if (exitCode == 127) {
        status.detail = "systemctl is not installed";
        return status;
    }
    // The first line of stdout is the state; systemctl writes diagnostics after it.
    const auto lineEnd = output.find('\n');
    const std::string state = output.substr(0, lineEnd == std::string::npos ? output.size() : lineEnd);
    if (state == "active") {
        status.state = ServiceStatus::State::Running;
    } else if (state == "inactive") {
        status.state = ServiceStatus::State::Stopped;
    } else if (state == "failed") {
        status.state = ServiceStatus::State::Failed;
    } else {
        status.detail = state.empty() ? "systemctl did not report a state" : state;
    }
    return status;
}

void Ops::startService() {
    int status = 0;
    std::string output;
    try {
        output = captureOutput("systemctl", {"start", "ishtaria-server"}, status);
    } catch (const OpError &e) {
        throw OpError(std::string("Cannot start the ishtaria-server service: ") + e.what());
    }
    if (status == 0) {
        return;
    }
    std::string message = "Cannot start the ishtaria-server service" +
                          (output.empty() ? "." : ": " + output);
    if (output.find("uthenticat") != std::string::npos ||
        output.find("ccess denied") != std::string::npos) {
        message += "\nStarting a unit needs root privileges or a polkit rule for the ishtaria user.";
    }
    throw OpError(message);
}

std::string Ops::serverListen() {
    const std::string path = serverConfigPath();
    std::ifstream config(path);
    if (!config) {
        throw OpError("Cannot read the server configuration " + path + ".");
    }
    // Top-level TOML only: no key can follow a table header, commented lines do not count.
    static const std::regex listenRe("^\\s*listen\\s*=\\s*\"([^\"]*)\"");
    std::string line;
    while (std::getline(config, line)) {
        const auto first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos || line[first] == '#') {
            continue;
        }
        if (line[first] == '[') {
            break;
        }
        std::smatch match;
        if (std::regex_search(line, match, listenRe)) {
            return match[1].str();
        }
    }
    return "0.0.0.0:7400"; // the server's built-in default (default_listen in main.rs)
}

bool Ops::worldInitialised() {
    return !db_.exec("SELECT 1 FROM worlds LIMIT 1").empty();
}

Row Ops::scheduledShutdown() {
    const auto rows = db_.exec("SELECT GREATEST(0, CEIL(EXTRACT(EPOCH FROM shutdown_at - now())))::bigint::text, "
                               "coalesce(message, '') FROM server_shutdown");
    return rows.empty() ? Row{} : rows[0];
}

void Ops::scheduleShutdown(int delaySeconds, const std::string &message) {
    if (delaySeconds < 0 || delaySeconds > 86400) {
        throw OpError("The delay must be 0-86400 seconds.");
    }
    if (message.size() > 200) {
        throw OpError("The message may have at most 200 bytes.");
    }
    db_.exec("INSERT INTO server_shutdown (world_id, shutdown_at, message) "
             "VALUES ($1, now() + make_interval(secs => $2::int), $3) "
             "ON CONFLICT (world_id) DO UPDATE SET requested_at = now(), "
             "shutdown_at = EXCLUDED.shutdown_at, message = EXCLUDED.message, requested_by = current_user",
             {str(worldId()), str(delaySeconds), message.empty() ? Param{} : Param{message}});
}

void Ops::cancelShutdown() { db_.exec("DELETE FROM server_shutdown"); }

} // namespace ishtariaadmin
