#include "ishtariaadmin/Ops.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <poll.h>
#include <thread>
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

namespace {

// --- solar clock ---------------------------------------------------------------
// The same sun the server announces in GET /world (atmosphere::Solar::at) and the
// client draws (planet_sky.gd solar_direction): the admin must agree with it to the
// second, or the hour it sets is not the hour the players see.

constexpr double kPi = 3.14159265358979323846;

// v mod m, always in [0, m) (Rust's rem_euclid).
double remEuclid(double v, double m) {
    const double r = std::fmod(v, m);
    return r < 0.0 ? r + m : r;
}

// Unit vector towards the sun at `unixSeconds` (atmosphere::Solar::at).
struct Sun {
    double x, y, z;
};

Sun sunAt(double unixSeconds) {
    const double deg = kPi / 180.0;
    const double days = unixSeconds / 86400.0 + 2440587.5 - 2451545.0;
    const double anomaly = remEuclid(357.528 + 0.9856003 * days, 360.0) * deg;
    const double longitude = remEuclid(280.460 + 0.9856474 * days + 1.915 * std::sin(anomaly) +
                                           0.020 * std::sin(2.0 * anomaly),
                                       360.0) * deg;
    const double obliquity = (23.439 - 0.0000004 * days) * deg;
    const double ascension =
        std::atan2(std::cos(obliquity) * std::sin(longitude), std::cos(longitude));
    const double declination = std::asin(std::sin(obliquity) * std::sin(longitude));
    const double sidereal = remEuclid(280.46061837 + 360.98564736629 * days, 360.0) * deg;
    const double localLongitude = ascension - sidereal;
    return {std::cos(declination) * std::cos(localLongitude), std::sin(declination),
            -std::cos(declination) * std::sin(localLongitude)};
}

// The world's prime meridian: the centre of face 0 (+X), on the equator.
constexpr double kPrimeMeridian[3] = {1.0, 0.0, 0.0};

// The sun's centre sits this many degrees below the horizon when the day begins and
// ends (atmosphere::HORIZON_ELEVATION_DEGREES).
constexpr double kHorizonElevationDegrees = -0.833;

// Local solar time at the prime meridian, seconds since local midnight (0..86400):
// the clock the client's HUD shows (planet_sky.gd clock_state).
double primeMeridianSeconds(double unixSeconds) {
    const Sun sun = sunAt(unixSeconds);
    const double hourAngle =
        std::atan2(kPrimeMeridian[2], kPrimeMeridian[0]) - std::atan2(sun.z, sun.x);
    const double local = 43200.0 + hourAngle * 86400.0 / (2.0 * kPi);
    return local - 86400.0 * std::floor(local / 86400.0);
}

bool sunAboveHorizon(double unixSeconds) {
    const Sun sun = sunAt(unixSeconds);
    const double dot = kPrimeMeridian[0] * sun.x + kPrimeMeridian[1] * sun.y +
                       kPrimeMeridian[2] * sun.z;
    return dot >= std::sin(kHorizonElevationDegrees * kPi / 180.0);
}

// The moment the sun crosses the horizon at the prime meridian next after `after`
// (atmosphere::next_horizon_crossing): `rising` picks dawn over dusk. Falls back to
// `after` when no crossing fits one solar day.
double nextHorizonCrossing(double after, bool rising) {
    const double step = 300.0;
    double beginning = after;
    for (int index = 1; index < 289; ++index) {
        const double end = after + index * step;
        if (sunAboveHorizon(end) == sunAboveHorizon(beginning)) {
            beginning = end;
            continue;
        }
        if (sunAboveHorizon(beginning) == rising) {
            beginning = end;
            continue;
        }
        double low = beginning;
        double high = end;
        for (int i = 0; i < 20; ++i) {
            const double middle = (low + high) * 0.5;
            if (sunAboveHorizon(middle) == sunAboveHorizon(low)) {
                low = middle;
            } else {
                high = middle;
            }
        }
        return high;
    }
    return after;
}

// The next moment after `after` at which the local solar time at the prime meridian
// is `targetSeconds` (0..86400). The clock runs at nearly one second per second, so
// a quarter-hour scan finds the crossing and a bisection pins it down. Falls back to
// `after` when no crossing fits one solar day.
double nextLocalTime(double targetSeconds, double after) {
    const double step = 300.0;
    double beginning = after;
    double previous = primeMeridianSeconds(beginning);
    for (int index = 1; index < 289; ++index) {
        const double end = after + index * step;
        const double current = primeMeridianSeconds(end);
        const double advance = remEuclid(current - previous, 86400.0);
        const double wanted = remEuclid(targetSeconds - previous, 86400.0);
        if (wanted > 0.0 && wanted <= advance) {
            // Signed distance to the target: negative before the crossing.
            const auto distance = [&](double t) {
                return remEuclid(primeMeridianSeconds(t) - targetSeconds + 43200.0, 86400.0) -
                       43200.0;
            };
            double low = beginning;
            double high = end;
            for (int i = 0; i < 20; ++i) {
                const double middle = (low + high) * 0.5;
                if (distance(middle) < 0.0) {
                    low = middle;
                } else {
                    high = middle;
                }
            }
            return high;
        }
        beginning = end;
        previous = current;
    }
    return after;
}

std::string solarTimeOfDay(double unixSeconds) {
    const double seconds = primeMeridianSeconds(unixSeconds);
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d",
                  static_cast<int>(seconds / 3600.0) % 24,
                  static_cast<int>(seconds / 60.0) % 60, static_cast<int>(seconds) % 60);
    return buffer;
}

} // namespace

bool WorldStatus::updateAvailable() const {
    return generator == GeneratorState::Changed ||
           std::any_of(disks.begin(), disks.end(), [](const DiskStatus &d) { return d.newer; });
}

bool WorldStatus::hasDiskLoadErrors() const {
    return std::any_of(disks.begin(), disks.end(),
                       [](const DiskStatus &d) { return !d.loadError.empty(); }) ||
           !otherDiskFailures.empty();
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

// Parses `PROGRESS <done>/<total>` lines from the generator's stderr into `progress`.
void feedGeneratorProgress(std::string &pending, const char *data, std::size_t n,
                           const GeneratorProgress &progress) {
    if (!progress) {
        return;
    }
    pending.append(data, n);
    for (;;) {
        const std::size_t end = pending.find('\n');
        if (end == std::string::npos) {
            break;
        }
        const std::string line = pending.substr(0, end);
        pending.erase(0, end + 1);
        unsigned done = 0, total = 0;
        if (std::sscanf(line.c_str(), "PROGRESS %u/%u", &done, &total) == 2 && total > 0) {
            progress(done, total);
        }
    }
}

// Runs the generator without a shell and collects its standard output (the PGM).
// Progress lines on stderr are forwarded to `progress` when set.
std::string runGenerator(const std::string &program, const std::vector<std::string> &args,
                         const GeneratorProgress &progress = {}) {
    int outfds[2];
    int errfds[2];
    if (::pipe(outfds) != 0 || ::pipe(errfds) != 0) {
        throw OpError("Cannot create a pipe.");
    }
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(outfds[0]);
        ::close(outfds[1]);
        ::close(errfds[0]);
        ::close(errfds[1]);
        throw OpError("Cannot start the generator.");
    }
    if (pid == 0) {
        ::dup2(outfds[1], STDOUT_FILENO);
        ::dup2(errfds[1], STDERR_FILENO);
        ::close(outfds[0]);
        ::close(outfds[1]);
        ::close(errfds[0]);
        ::close(errfds[1]);
        std::vector<char *> argv;
        argv.push_back(const_cast<char *>(program.c_str()));
        for (const auto &a : args) {
            argv.push_back(const_cast<char *>(a.c_str()));
        }
        argv.push_back(nullptr);
        ::execvp(program.c_str(), argv.data());
        ::_exit(127);
    }
    ::close(outfds[1]);
    ::close(errfds[1]);
    // Non-blocking reads so poll can interleave PGM bytes and progress lines.
    ::fcntl(outfds[0], F_SETFL, O_NONBLOCK);
    ::fcntl(errfds[0], F_SETFL, O_NONBLOCK);

    std::string out;
    std::string errPending;
    char buffer[65536];
    bool tooBig = false;
    bool outOpen = true;
    bool errOpen = true;
    while (outOpen || errOpen) {
        pollfd fds[2] = {};
        nfds_t nfd = 0;
        if (outOpen) {
            fds[nfd++] = {outfds[0], POLLIN, 0};
        }
        if (errOpen) {
            fds[nfd++] = {errfds[0], POLLIN, 0};
        }
        if (::poll(fds, nfd, -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        for (nfds_t i = 0; i < nfd; ++i) {
            if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) {
                continue;
            }
            const ssize_t n = ::read(fds[i].fd, buffer, sizeof(buffer));
            if (n > 0) {
                if (fds[i].fd == outfds[0]) {
                    if (out.size() + static_cast<std::size_t>(n) > kMaxMapBytes) {
                        tooBig = true;
                        outOpen = false;
                        errOpen = false;
                        break;
                    }
                    out.append(buffer, static_cast<std::size_t>(n));
                } else {
                    feedGeneratorProgress(errPending, buffer, static_cast<std::size_t>(n), progress);
                }
            } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                if (fds[i].fd == outfds[0]) {
                    outOpen = false;
                } else {
                    errOpen = false;
                }
            }
        }
    }
    ::close(outfds[0]);
    ::close(errfds[0]);
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

namespace {

// Basename of a path without trailing slash (datadisk id when listing skips).
std::string pathBaseName(const std::string &path) {
    std::size_t end = path.size();
    while (end > 0 && path[end - 1] == '/') {
        --end;
    }
    const std::size_t slash = path.rfind('/', end == 0 ? 0 : end - 1);
    return slash == std::string::npos ? path.substr(0, end) : path.substr(slash + 1, end - slash - 1);
}

// Parses "skipping <path>: <message>" from ishtaria-server --list-datadisks stderr.
bool parseSkipLine(const std::string &line, std::string &id, std::string &message) {
    static const char kPrefix[] = "skipping ";
    if (line.compare(0, sizeof(kPrefix) - 1, kPrefix) != 0) {
        return false;
    }
    const std::size_t colon = line.find(": ", sizeof(kPrefix) - 1);
    if (colon == std::string::npos) {
        return false;
    }
    id = pathBaseName(line.substr(sizeof(kPrefix) - 1, colon - (sizeof(kPrefix) - 1)));
    message = line.substr(colon + 2);
    return !id.empty() && !message.empty();
}

} // namespace

DatadiskListing Ops::listDatadisks() {
    DatadiskListing listing;
    int status = 0;
    std::string output;
    try {
        output = captureOutput(serverProgram(), {"--list-datadisks"}, status);
    } catch (const OpError &) {
        return listing;
    }
    if (status != 0) {
        return listing;
    }
    std::size_t start = 0;
    while (start < output.size()) {
        std::size_t end = output.find('\n', start);
        if (end == std::string::npos) {
            end = output.size();
        }
        const std::string line = output.substr(start, end - start);
        start = end + 1;
        if (line.empty()) {
            continue;
        }
        const auto first = line.find('\t');
        const auto second = first == std::string::npos ? first : line.find('\t', first + 1);
        if (second != std::string::npos) {
            listing.disks.push_back({line.substr(0, first), line.substr(first + 1, second - first - 1),
                                     line.substr(second + 1)});
            continue;
        }
        std::string id, message;
        if (parseSkipLine(line, id, message)) {
            listing.failures.emplace_back(std::move(id), std::move(message));
        }
    }
    return listing;
}

std::vector<Datadisk> Ops::installedDatadisks() { return listDatadisks().disks; }

void Ops::generateMap(const std::string &name, unsigned long long seed, int faceSize,
                      const std::vector<std::string> &datadisks, GeneratorProgress progress) {
    if (name.empty() || name.size() > 64) {
        throw OpError("The map name must have 1-64 characters.");
    }
    if (faceSize < 16 || faceSize > 1024) {
        throw OpError("The face size must be 16-1024.");
    }
    std::string diskJson = "[]";
    if (!datadisks.empty()) {
        const auto listing = listDatadisks();
        std::string ids;
        diskJson = "[";
        for (const auto &id : datadisks) {
            const auto found = std::find_if(listing.disks.begin(), listing.disks.end(),
                                            [&](const Datadisk &d) { return d.id == id; });
            if (found == listing.disks.end()) {
                const auto failed = std::find_if(listing.failures.begin(), listing.failures.end(),
                                                 [&](const auto &f) { return f.first == id; });
                if (failed != listing.failures.end()) {
                    throw OpError("Datadisk " + id + " failed to load: " + failed->second);
                }
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
        override_path != nullptr ? override_path : "ishtaria-worldgen", generatorArgs, progress);
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

WorldStatus Ops::worldStatus(bool checkGenerator, GeneratorProgress progress) {
    WorldStatus status;
    const auto listing = listDatadisks();
    for (const auto &row : db_.exec("SELECT disk_id, version FROM world_datadisks WHERE world_id = $1 ORDER BY position",
                                    {std::to_string(worldId())})) {
        DiskStatus disk{row[0], row[0], row[1], "", false, {}};
        const auto found = std::find_if(listing.disks.begin(), listing.disks.end(),
                                        [&](const Datadisk &d) { return d.id == row[0]; });
        if (found != listing.disks.end()) {
            disk.name = found->name;
            disk.installedVersion = found->version;
            disk.newer = compareVersions(found->version, row[1]) > 0;
        } else {
            const auto failed = std::find_if(listing.failures.begin(), listing.failures.end(),
                                             [&](const auto &f) { return f.first == row[0]; });
            if (failed != listing.failures.end()) {
                disk.loadError = failed->second;
            }
        }
        status.disks.push_back(disk);
    }
    for (const auto &failure : listing.failures) {
        const bool used = std::any_of(status.disks.begin(), status.disks.end(),
                                      [&](const DiskStatus &d) { return d.id == failure.first; });
        if (!used) {
            status.otherDiskFailures.push_back(failure);
        }
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
                                             generatorArgs, progress);
        const auto hash = db_.exec("SELECT encode(sha256(decode($1, 'hex')), 'hex')", {toHex(pgm)});
        status.generator = hash.at(0).at(0) == map[0][2] ? GeneratorState::Current : GeneratorState::Changed;
    } catch (const OpError &e) {
        status.generatorDetail = e.what();
    }
    return status;
}

std::string Ops::prepareWorldUpdate(GeneratorProgress progress) {
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
    generateMap(name, seed, std::atoi(map[0][1].c_str()), ids, std::move(progress));
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

long Ops::playerCount() {
    try {
        const auto rows = db_.exec("SELECT count(*)::text FROM players");
        return rows.empty() ? 0 : std::stol(rows[0][0]);
    } catch (const DbError &) {
        return 0;
    }
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

namespace {

// Privileged systemctl/journalctl via `sudo -n` when /etc/sudoers.d/ishtaria-server
// is installed; falls back to a direct call (works when already root).
std::string capturePrivileged(const std::string &program, const std::vector<std::string> &args,
                              int &status) {
    std::vector<std::string> sudoArgs = {"-n", program};
    sudoArgs.insert(sudoArgs.end(), args.begin(), args.end());
    try {
        std::string out = captureOutput("sudo", sudoArgs, status);
        const bool needsPassword =
            status == 1 && (out.find("password") != std::string::npos ||
                            out.find("a terminal is required") != std::string::npos ||
                            out.find("Sorry") != std::string::npos);
        if (status == 127 || needsPassword) {
            return captureOutput(program, args, status);
        }
        return out;
    } catch (const OpError &) {
        return captureOutput(program, args, status);
    }
}

std::vector<std::string> splitLines(const std::string &text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto end = text.find('\n', start);
        lines.push_back(text.substr(start, end == std::string::npos ? end : end - start));
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return lines;
}

std::string trimDetail(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    if (text.size() > 1200) {
        text.resize(1200);
        text += "…";
    }
    return text;
}

// Port number from a listen address like "0.0.0.0:7400" or "[::]:7400".
std::string listenPort(const std::string &listen) {
    const auto colon = listen.rfind(':');
    if (colon == std::string::npos || colon + 1 >= listen.size()) {
        return {};
    }
    const std::string port = listen.substr(colon + 1);
    if (port.empty() || port.find_first_not_of("0123456789") != std::string::npos) {
        return {};
    }
    return port;
}

// Why ExecStart failed: journal (when readable), else a listen-port conflict, else a short
// binary probe. The ishtaria user often cannot read the system journal.
std::string describeServiceFailure(const std::string &listen) {
    int status = 0;
    std::string log;
    try {
        log = capturePrivileged("journalctl",
                                {"-u", "ishtaria-server", "-n", "60", "--no-pager", "-o", "cat"}, status);
    } catch (const OpError &) {
        log.clear();
    }
    if (log.find("insufficient permissions") == std::string::npos &&
        log.find("No journal files were opened") == std::string::npos && !log.empty()) {
        const auto lines = splitLines(log);
        std::size_t from = lines.size();
        for (std::size_t i = 0; i < lines.size(); ++i) {
            const std::string &line = lines[i];
            if (line.rfind("Error:", 0) == 0 || line.find("Failed to start") != std::string::npos ||
                line.find("start request repeated too quickly") != std::string::npos ||
                line.find("Start request repeated too quickly") != std::string::npos) {
                from = i;
            }
        }
        if (from == lines.size()) {
            from = lines.size() > 12 ? lines.size() - 12 : 0;
        }
        std::string out;
        for (std::size_t i = from; i < lines.size(); ++i) {
            if (lines[i].empty()) {
                continue;
            }
            if (!out.empty()) {
                out += '\n';
            }
            out += lines[i];
        }
        if (!out.empty()) {
            return trimDetail(std::move(out));
        }
    }

    std::string detail;
    const std::string port = listenPort(listen);
    if (!port.empty()) {
        int ssStatus = 0;
        std::string ssOut;
        try {
            ssOut = captureOutput("ss", {"-tlnH", "sport = :" + port}, ssStatus);
        } catch (const OpError &) {
            ssOut.clear();
        }
        while (!ssOut.empty() && (ssOut.back() == '\n' || ssOut.back() == '\r')) {
            ssOut.pop_back();
        }
        if (!ssOut.empty()) {
            detail = "Error: cannot bind HTTP listener\nCaused by:\n    Address already in use (os error 98)\n"
                     "Something is already listening on " +
                     listen + ":\n" + ssOut;
            return trimDetail(std::move(detail));
        }
    }

    // Port free but unit failed: run the binary briefly to capture its Error: line.
    int probeStatus = 0;
    std::string probeOut;
    try {
        probeOut = captureOutput("timeout",
                                 {"--signal=KILL", "1s", serverProgram(), serverConfigPath()},
                                 probeStatus);
    } catch (const OpError &) {
        probeOut.clear();
    }
    std::string errors;
    for (const auto &line : splitLines(probeOut)) {
        if (line.rfind("Error:", 0) == 0 || line.rfind("Caused by:", 0) == 0 ||
            (!line.empty() && line[0] == ' ' && !errors.empty())) {
            if (!errors.empty()) {
                errors += '\n';
            }
            errors += line;
        }
    }
    if (!errors.empty()) {
        return trimDetail(std::move(errors));
    }

    int showStatus = 0;
    std::string show;
    try {
        show = captureOutput("systemctl",
                             {"show", "ishtaria-server", "-p", "Result", "-p", "ExecMainStatus",
                              "-p", "ActiveState", "--no-pager"},
                             showStatus);
    } catch (const OpError &) {
        show.clear();
    }
    if (!show.empty()) {
        return trimDetail(std::move(show));
    }
    return {};
}

} // namespace

void Ops::startService() {
    // Clear a start-limit failure so this attempt can run again and surface the real error.
    int ignored = 0;
    try {
        capturePrivileged("systemctl", {"reset-failed", "ishtaria-server"}, ignored);
    } catch (const OpError &) {
    }

    int status = 0;
    std::string output;
    try {
        output = capturePrivileged("systemctl", {"start", "ishtaria-server"}, status);
    } catch (const OpError &e) {
        throw OpError(std::string("Cannot start the ishtaria-server service: ") + e.what());
    }
    if (status == 127) {
        throw OpError("systemctl is not installed.");
    }

    auto failureMessage = [&](const std::string &head) {
        std::string message = head;
        if (output.find("uthenticat") != std::string::npos ||
            output.find("ccess denied") != std::string::npos ||
            output.find("password") != std::string::npos) {
            message += "\nStarting a unit needs /etc/sudoers.d/ishtaria-server (package ishtaria-server) "
                       "or root privileges.";
        }
        std::string listen;
        try {
            listen = serverListen();
        } catch (const OpError &) {
        }
        const std::string detail = describeServiceFailure(listen);
        if (!detail.empty()) {
            message += "\n\n" + detail;
        } else if (!output.empty()) {
            message += "\n" + output;
        }
        return message;
    };

    if (status != 0) {
        throw OpError(failureMessage("Cannot start the ishtaria-server service."));
    }

    // Restart=on-failure: systemctl start may return 0 while the process dies right after.
    // Wait until the unit is active and still active after a short settle, or has failed.
    for (int attempt = 0; attempt < 40; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        const ServiceStatus now = serviceStatus();
        if (now.state == ServiceStatus::State::Running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            if (serviceStatus().state == ServiceStatus::State::Running) {
                return;
            }
            continue;
        }
        if (now.state == ServiceStatus::State::Failed || now.state == ServiceStatus::State::Stopped) {
            break;
        }
    }

    if (serviceStatus().state == ServiceStatus::State::Running) {
        return;
    }

    throw OpError(failureMessage("The ishtaria-server service did not stay running after start."));
}

void Ops::stopService() {
    if (serviceStatus().state == ServiceStatus::State::Stopped) {
        return;
    }
    int status = 0;
    std::string output;
    try {
        output = capturePrivileged("systemctl", {"stop", "ishtaria-server"}, status);
    } catch (const OpError &e) {
        throw OpError(std::string("Cannot stop the ishtaria-server service: ") + e.what());
    }
    if (status == 127) {
        throw OpError("systemctl is not installed.");
    }
    if (status != 0) {
        std::string message = "Cannot stop the ishtaria-server service.";
        if (output.find("uthenticat") != std::string::npos ||
            output.find("ccess denied") != std::string::npos ||
            output.find("password") != std::string::npos) {
            message += "\nStopping a unit needs /etc/sudoers.d/ishtaria-server (package ishtaria-server) "
                       "or root privileges.";
        }
        if (!output.empty()) {
            message += "\n" + output;
        }
        throw OpError(message);
    }
    for (int attempt = 0; attempt < 40; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        const ServiceStatus now = serviceStatus();
        if (now.state == ServiceStatus::State::Stopped || now.state == ServiceStatus::State::Failed) {
            return;
        }
        if (now.state == ServiceStatus::State::Unknown) {
            // No systemd on this host (tests): treat a successful stop as done.
            return;
        }
    }
    if (serviceStatus().state == ServiceStatus::State::Running) {
        throw OpError("The ishtaria-server service is still running after stop.");
    }
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
    try {
        return !db_.exec("SELECT 1 FROM worlds LIMIT 1").empty();
    } catch (const DbError &) {
        return false;
    }
}

bool Ops::needsSetup() {
    try {
        if (!worldInitialised()) {
            return true;
        }
        return activeMap()[0].empty();
    } catch (const DbError &) {
        return true;
    }
}

void Ops::resetDatabase() {
    // Same effect as dropdb + createdb -T template0 -O ishtaria, without CREATEDB.
    // The role owns the database (package postinst) so it may recreate public.
    // The setup wizard stops the systemd unit first so the server is not writing.
    try {
        db_.exec("DROP SCHEMA public CASCADE");
        db_.exec("CREATE SCHEMA public");
        db_.exec("GRANT ALL ON SCHEMA public TO CURRENT_USER");
        db_.exec("GRANT ALL ON SCHEMA public TO public");
    } catch (const DbError &e) {
        throw OpError(std::string("Cannot reset the database: ") + e.what());
    }
}

void Ops::bootstrapWorld(unsigned long long seed, int faceSize,
                         const std::vector<std::string> &datadisks, GeneratorProgress progress) {
    if (faceSize < 16 || faceSize > 1024) {
        throw OpError("The face size must be 16-1024.");
    }
    if (worldInitialised() && !activeMap()[0].empty()) {
        throw OpError("A world map is already imported; reset the database first.");
    }
    if (!datadisks.empty()) {
        const auto listing = listDatadisks();
        std::string ids;
        for (const auto &id : datadisks) {
            const auto found = std::find_if(listing.disks.begin(), listing.disks.end(),
                                            [&](const Datadisk &d) { return d.id == id; });
            if (found == listing.disks.end()) {
                const auto failed = std::find_if(listing.failures.begin(), listing.failures.end(),
                                                 [&](const auto &f) { return f.first == id; });
                if (failed != listing.failures.end()) {
                    throw OpError("Datadisk " + id + " failed to load: " + failed->second);
                }
                throw OpError("Datadisk " + id + " is not installed.");
            }
            ids += (ids.empty() ? "" : ",") + id;
        }
        int status = 0;
        const std::string output = captureOutput(serverProgram(), {"--check-datadisks", ids}, status);
        if (status != 0) {
            throw OpError("These datadisks cannot be combined: " + output);
        }
    }

    std::vector<std::string> generatorArgs = worldTerrainArgs(datadisks);
    if (generatorArgs.empty()) {
        generatorArgs = {std::to_string(seed), std::to_string(faceSize)};
    } else {
        generatorArgs.push_back(std::to_string(faceSize));
    }
    const char *override_path = std::getenv("ISHTARIA_WORLDGEN");
    const std::string pgm = runGenerator(
        override_path != nullptr ? override_path : "ishtaria-worldgen", generatorArgs, progress);

    char pathTemplate[] = "/tmp/ishtaria-admin-bootstrap-XXXXXX";
    const int fd = ::mkstemp(pathTemplate);
    if (fd < 0) {
        throw OpError(std::string("Cannot create a temporary map file: ") + std::strerror(errno));
    }
    const std::string path = pathTemplate;
    std::size_t written = 0;
    while (written < pgm.size()) {
        const ssize_t n = ::write(fd, pgm.data() + written, pgm.size() - written);
        if (n < 0) {
            const int err = errno;
            ::close(fd);
            ::unlink(path.c_str());
            throw OpError(std::string("Cannot write the temporary map: ") + std::strerror(err));
        }
        written += static_cast<std::size_t>(n);
    }
    if (::close(fd) != 0) {
        ::unlink(path.c_str());
        throw OpError("Cannot write the temporary map.");
    }

    std::vector<std::string> args = {serverConfigPath(), "--import", path, "--seed",
                                     std::to_string(seed), "--import-only"};
    for (const auto &id : datadisks) {
        args.push_back("--datadisk");
        args.push_back(id);
    }

    // The child inherits DATABASE_URL so --import-only targets the same database
    // as this admin session (including --database-url / test URLs).
    const char *previous = std::getenv("DATABASE_URL");
    const std::string previousCopy = previous != nullptr ? previous : "";
    if (::setenv("DATABASE_URL", databaseUrl_.c_str(), 1) != 0) {
        ::unlink(path.c_str());
        throw OpError("Cannot set DATABASE_URL for the import.");
    }
    int status = 0;
    std::string output;
    try {
        output = captureOutput(serverProgram(), args, status);
    } catch (...) {
        if (previous != nullptr) {
            ::setenv("DATABASE_URL", previousCopy.c_str(), 1);
        } else {
            ::unsetenv("DATABASE_URL");
        }
        ::unlink(path.c_str());
        throw;
    }
    if (previous != nullptr) {
        ::setenv("DATABASE_URL", previousCopy.c_str(), 1);
    } else {
        ::unsetenv("DATABASE_URL");
    }
    ::unlink(path.c_str());

    if (status == 127) {
        throw OpError("ishtaria-server is not installed.");
    }
    if (status != 0) {
        throw OpError(std::string("World import failed: ") +
                      (output.empty() ? "ishtaria-server exited with an error" : output));
    }
    if (!worldInitialised() || activeMap()[0].empty()) {
        throw OpError("World import reported success but no map is present.");
    }
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

// --- world clock ---------------------------------------------------------------

SolarClock Ops::solarClock() {
    const auto rows = db_.exec("SELECT solar_offset_seconds::text, "
                               "EXTRACT(EPOCH FROM clock_timestamp())::float8 FROM worlds WHERE id = $1",
                               {str(worldId())});
    SolarClock clock;
    clock.offsetSeconds = std::stol(rows[0][0]);
    // The server adds the stored offset to its own clock (GET /world); read the
    // database clock so the jump lands where the server will put the sun.
    const double now = std::stod(rows[0][1]);
    clock.localTime = solarTimeOfDay(now + static_cast<double>(clock.offsetSeconds));
    clock.realTime = solarTimeOfDay(now);
    clock.day = sunAboveHorizon(now + static_cast<double>(clock.offsetSeconds));
    return clock;
}

void Ops::setSolarClock(bool day) {
    const double now =
        std::stod(db_.exec("SELECT EXTRACT(EPOCH FROM clock_timestamp())::float8")[0][0]);
    applySolarOffset(nextHorizonCrossing(now, day) - now);
}

void Ops::setSolarClockTime(int hour, int minute) {
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        throw OpError("The hour must be 0-23 and the minute 0-59.");
    }
    const double now =
        std::stod(db_.exec("SELECT EXTRACT(EPOCH FROM clock_timestamp())::float8")[0][0]);
    applySolarOffset(nextLocalTime(hour * 3600.0 + minute * 60.0, now) - now);
}

void Ops::applySolarOffset(double shiftSeconds) {
    long shift = std::lround(shiftSeconds);
    if (shift > 86400) {
        shift = 86400;
    }
    if (shift < -86400) {
        shift = -86400;
    }
    Transaction tx(db_);
    db_.exec("UPDATE worlds SET solar_offset_seconds = $2 WHERE id = $1",
             {str(worldId()), str(shift)});
    // Everyone online is told to look at the sky again (the client re-reads /world).
    db_.exec("INSERT INTO player_events (recipient_id, kind, subject) "
             "SELECT id, 'solar', $2 FROM players WHERE world_id = $1 AND died_at IS NULL "
             "AND banned_at IS NULL AND coalesce(greatest(last_seen_at, last_active_at) > "
             "clock_timestamp() - make_interval(secs => 30), false)",
             {str(worldId()), "ishtaria-admin"});
    tx.commit();
}

} // namespace ishtariaadmin
