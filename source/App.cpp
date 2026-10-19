#define Uses_TApplication
#define Uses_TDeskTop
#define Uses_TDialog
#define Uses_TButton
#define Uses_TStaticText
#define Uses_TCheckBoxes
#define Uses_TSItem
#define Uses_TScrollBar
#define Uses_TListViewer
#define Uses_TRect
#define Uses_TEvent
#define Uses_TKeys
#define Uses_TMenuBar
#define Uses_TSubMenu
#define Uses_TMenuItem
#define Uses_TStatusLine
#define Uses_TStatusItem
#define Uses_TStatusDef
#define Uses_MsgBox
#define Uses_TProgram
#include <tvision/tv.h>

#include "ishtariaadmin/App.h"
#include "ishtariaadmin/Ops.h"
#include "ishtariaadmin/i18n.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ishtariaadmin {

namespace {

enum : ushort {
    cmMaps = 1001,
    cmUsers,
    cmPortals,
    cmServer,
    cmWorldStatus,
    cmAction = 1100, // + action index
};

void showError(const std::string &text) { messageBox(text.c_str(), mfError | mfOKButton); }

bool confirm(const std::string &text) {
    return messageBox(text.c_str(), mfConfirmation | mfYesButton | mfNoButton) == cmYes;
}

std::optional<std::string> ask(const std::string &title, const std::string &label, const std::string &initial = "") {
    char buffer[256] = {};
    initial.copy(buffer, sizeof(buffer) - 1);
    if (inputBox(title.c_str(), label.c_str(), buffer, sizeof(buffer) - 1) != cmOK) {
        return std::nullopt;
    }
    return std::string(buffer);
}

// Lets the operator choose which installed datadisks the generated world takes into
// account. Nothing is shown (and nothing selected) when no datadisk is installed.
// Returns nullopt when the dialog was cancelled.
std::optional<std::vector<std::string>> pickDatadisks(const std::vector<Datadisk> &disks) {
    if (disks.empty()) {
        return std::vector<std::string>{};
    }
    const int count = static_cast<int>(std::min<std::size_t>(disks.size(), 16));
    auto *dialog = new TDialog(TRect(0, 0, 66, count + 9), _("Story datadisks"));
    dialog->options |= ofCentered;
    TSItem *items = nullptr;
    for (int i = count - 1; i >= 0; --i) {
        const std::string label = disks[i].name + " (" + disks[i].id + " " + disks[i].version + ")";
        items = new TSItem(label.substr(0, 56).c_str(), items);
    }
    dialog->insert(new TStaticText(TRect(3, 2, 63, 4), _("Take these datadisks into account when generating the world:")));
    auto *boxes = new TCheckBoxes(TRect(3, 4, 63, 4 + count), items);
    dialog->insert(boxes);
    dialog->insert(new TButton(TRect(14, count + 6, 28, count + 8), _("~O~K"), cmOK, bfDefault));
    dialog->insert(new TButton(TRect(34, count + 6, 50, count + 8), _("Cancel"), cmCancel, bfNormal));
    boxes->select();
    const ushort result = TProgram::deskTop->execView(dialog);
    std::vector<std::string> chosen;
    if (result == cmOK) {
        for (int i = 0; i < count; ++i) {
            if (boxes->mark(i)) {
                chosen.push_back(disks[i].id);
            }
        }
    }
    TObject::destroy(dialog);
    if (result != cmOK) {
        return std::nullopt;
    }
    return chosen;
}

// Map names are free text: keep only characters that are safe in a file name.
std::string safeFileName(std::string name) {
    for (char &c : name) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) {
            c = '_';
        }
    }
    return name.empty() ? "map" : name;
}

std::string pad(const std::string &text, std::size_t width) {
    return text.size() >= width ? text.substr(0, width) : text + std::string(width - text.size(), ' ');
}

// List viewer that renders externally owned text rows.
class RowList : public TListViewer {
public:
    RowList(const TRect &bounds, TScrollBar *bar) : TListViewer(bounds, 1, nullptr, bar) {}
    void setRows(std::vector<std::string> rows) {
        rows_ = std::move(rows);
        setRange(static_cast<short>(rows_.size()));
        if (focused >= range) {
            focusItem(range > 0 ? range - 1 : 0);
        }
        drawView();
    }
    void getText(char *dest, short item, short maxLen) override {
        std::string text = item >= 0 && item < static_cast<short>(rows_.size()) ? rows_[item] : "";
        std::snprintf(dest, maxLen, "%s", text.c_str());
    }

private:
    std::vector<std::string> rows_;
};

struct Action {
    std::string label;
    std::function<void(long selected)> run; // selected = row index or -1
    bool needsSelection = true;
};

// One line about the state of the ishtaria-server systemd unit.
std::string serviceLine(const ServiceStatus &service) {
    std::string text = _("Server service: ");
    switch (service.state) {
    case ServiceStatus::State::Running: text += _("running"); break;
    case ServiceStatus::State::Stopped: text += _("stopped"); break;
    case ServiceStatus::State::Failed: text += _("failed"); break;
    case ServiceStatus::State::Unknown:
        text += service.detail.empty() ? _("unknown")
                                       : std::string(_("unknown (")) + service.detail + ")";
        break;
    }
    return text;
}

// Modal dialog: header, list of rows and a row of action buttons.
class ListDialog : public TDialog {
public:
    ListDialog(const std::string &title, std::function<std::string()> header,
               std::function<std::vector<std::string>()> rows, std::vector<Action> actions,
               int headerLines = 2)
        : TDialog(TRect(0, 0, 78, 22), title.c_str()), TWindowInit(&TDialog::initFrame),
          header_(std::move(header)), rows_(std::move(rows)), actions_(std::move(actions)),
          headerLines_(headerLines) {
        options |= ofCentered;
        auto *bar = new TScrollBar(TRect(75, 2 + headerLines_, 76, 16));
        insert(bar);
        list_ = new RowList(TRect(2, 2 + headerLines_, 75, 16), bar);
        insert(list_);
        int index = 0;
        for (const auto &action : actions_) {
            const int col = index % 4, row = index / 4;
            insert(new TButton(TRect(2 + col * 18, 17 + row * 2, 19 + col * 18, 19 + row * 2),
                               action.label.c_str(), static_cast<ushort>(cmAction + index), bfNormal));
            ++index;
        }
        insert(new TButton(TRect(2 + 3 * 18, 19, 19 + 3 * 18, 21), _("~C~lose"), cmCancel, bfNormal));
        reload();
    }

    void handleEvent(TEvent &event) override {
        TDialog::handleEvent(event);
        if (event.what == evCommand && event.message.command >= cmAction &&
            event.message.command < cmAction + actions_.size()) {
            const auto &action = actions_[event.message.command - cmAction];
            clearEvent(event);
            const long selected = rowCount() > 0 ? list_->focused : -1;
            if (action.needsSelection && selected < 0) {
                showError(_("Select an item first."));
                return;
            }
            try {
                action.run(selected);
            } catch (const std::exception &e) {
                showError(e.what());
            }
            reload();
        }
    }

    void reload() {
        try {
            if (headerText_ != nullptr) {
                destroy(headerText_);
            }
            headerText_ = new TStaticText(TRect(2, 2, 76, 2 + headerLines_), header_().c_str());
            insert(headerText_);
            const auto rows = rows_();
            list_->setRows(rows);
        } catch (const std::exception &e) {
            showError(e.what());
        }
    }

private:
    int rowCount() const { return list_->range; }
    TStaticText *headerText_ = nullptr;
    RowList *list_;
    std::function<std::string()> header_;
    std::function<std::vector<std::string>()> rows_;
    std::vector<Action> actions_;
    int headerLines_;
};

} // namespace

class AdminApp : public TApplication {
public:
    explicit AdminApp(Ops &ops) : TProgInit(&AdminApp::initStatusLine, &AdminApp::initMenuBar, &TApplication::initDeskTop), ops_(ops) {}

    void handleEvent(TEvent &event) override {
        TApplication::handleEvent(event);
        if (event.what != evCommand) {
            return;
        }
        switch (event.message.command) {
        case cmMaps: mapsDialog(); break;
        case cmUsers: usersDialog(); break;
        case cmPortals: portalsDialog(); break;
        case cmServer: serverDialog(); break;
        case cmWorldStatus: worldStatusDialog(); break;
        default: return;
        }
        clearEvent(event);
    }

    // Shown once at start-up: the systemd service state, the port the server
    // listens on, the datadisks the world was generated with, an offer to generate
    // a first world when none exists yet and to start a stopped service.
    void showStartupStatus() {
        ServiceStatus service = ops_.serviceStatus();
        std::string text = serviceLine(service);
        try {
            if (!ops_.worldInitialised()) {
                messageBox((text + "\n" +
                            _("The world is not initialised yet. Start the server once to create it."))
                               .c_str(),
                           mfInformation | mfOKButton);
                return;
            }
            const Row map = ops_.activeMap();
            if (map[0].empty()) {
                if (confirm(text + "\n" + _("No world map is generated yet. Generate a new world now?"))) {
                    const auto generated = generateMapFlow("world");
                    if (generated && loadMapFlow(*generated)) {
                        messageBox(_("The new world map is now active. Start (or restart) the server service to serve it."),
                                   mfInformation | mfOKButton);
                    }
                }
                return;
            }
            std::string worldText = "\n" + listenText();
            worldText += "\n" + std::string(_("Active map: seed ")) + map[0] + ", " + map[1] + " px";
            const WorldStatus status = ops_.worldStatus(false);
            if (status.disks.empty()) {
                worldText += "\n" + std::string(_("The world uses no datadisks."));
            } else {
                worldText += "\n" + std::string(_("Datadisks the world was generated with: "));
                for (std::size_t i = 0; i < status.disks.size(); ++i) {
                    if (i > 0) {
                        worldText += ", ";
                    }
                    worldText += status.disks[i].id + " " + status.disks[i].worldVersion;
                }
            }
            if (service.state == ServiceStatus::State::Stopped ||
                service.state == ServiceStatus::State::Failed) {
                if (confirm(text + worldText + "\n\n" +
                            _("The server service is not running. Start it now?"))) {
                    ops_.startService();
                    service = ops_.serviceStatus();
                    text = serviceLine(service);
                }
            }
            text += worldText;
        } catch (const std::exception &e) {
            text += "\n";
            text += e.what();
        }
        messageBox(text.c_str(), mfInformation | mfOKButton);
    }

    // Tells the operator at start-up when the data served to players can be refreshed.
    void announceUpdates() {
        try {
            const WorldStatus status = ops_.worldStatus();
            if (status.updateAvailable() &&
                confirm(std::string(_("An update of the world data is available.")) + "\n" + describeUpdates(status) + "\n" +
                        _("Open the world status now?"))) {
                worldStatusDialog();
            }
        } catch (const std::exception &) {
            // The status is informational; the dialog reports problems when opened explicitly.
        }
    }

    static TMenuBar *initMenuBar(TRect r) {
        r.b.y = r.a.y + 1;
        return new TMenuBar(r,
            *new TSubMenu(_("~A~dministration"), kbAltA) +
                *new TMenuItem(_("World ~m~aps..."), cmMaps, kbF2, hcNoContext, "F2") +
                *new TMenuItem(_("~P~layers..."), cmUsers, kbF3, hcNoContext, "F3") +
                *new TMenuItem(_("~L~inked worlds..."), cmPortals, kbF4, hcNoContext, "F4") +
                *new TMenuItem(_("~S~erver..."), cmServer, kbF5, hcNoContext, "F5") +
                *new TMenuItem(_("World s~t~atus..."), cmWorldStatus, kbF6, hcNoContext, "F6") +
                newLine() +
                *new TMenuItem(_("E~x~it"), cmQuit, kbAltX, hcNoContext, "Alt-X"));
    }

    static TStatusLine *initStatusLine(TRect r) {
        r.a.y = r.b.y - 1;
        return new TStatusLine(r,
            *new TStatusDef(0, 0xFFFF) +
                *new TStatusItem(_("~F2~ Maps"), kbF2, cmMaps) +
                *new TStatusItem(_("~F3~ Players"), kbF3, cmUsers) +
                *new TStatusItem(_("~F4~ Linked worlds"), kbF4, cmPortals) +
                *new TStatusItem(_("~F5~ Server"), kbF5, cmServer) +
                *new TStatusItem(_("~F6~ Status"), kbF6, cmWorldStatus) +
                *new TStatusItem(_("~Alt-X~ Exit"), kbAltX, cmQuit) +
                *new TStatusItem("v" ISHTARIA_ADMIN_VERSION, kbNoKey, 0));
    }

private:
    static long idOf(const Rows &rows, long index) { return std::stol(rows.at(static_cast<std::size_t>(index))[0]); }

    void showDialog(ListDialog *dialog) { deskTop->execView(dialog); destroy(dialog); }

    // "Listening on: <address>" from the server configuration; the reason when unknown.
    std::string listenText() const {
        try {
            return std::string(_("Listening on: ")) + ops_.serverListen();
        } catch (const std::exception &e) {
            std::string detail = e.what();
            const auto lineEnd = detail.find('\n');
            detail.erase(lineEnd == std::string::npos ? detail.size() : lineEnd);
            if (detail.size() > 48) {
                detail.erase(48);
            }
            return std::string(_("Listening on: unknown (")) + detail + ")";
        }
    }

    // Asks for the name, seed, face size and datadisks, generates the map into the
    // library and returns its id (nullopt when the operator cancelled).
    std::optional<long> generateMapFlow(const std::string &defaultName) {
        auto name = ask(_("Generate map"), _("Name in the library:"), defaultName);
        if (!name) return std::nullopt;
        auto seed = ask(_("Generate map"), _("Seed (number):"), std::to_string(std::time(nullptr)));
        if (!seed) return std::nullopt;
        auto size = ask(_("Generate map"), _("Face size in pixels (16-1024):"), "256");
        if (!size) return std::nullopt;
        char *end = nullptr;
        const unsigned long long seedValue = std::strtoull(seed->c_str(), &end, 10);
        if (seed->empty() || *end != '\0' || seed->find('-') != std::string::npos) {
            throw OpError(_("The seed must be a non-negative whole number."));
        }
        const auto chosen = pickDatadisks(ops_.installedDatadisks());
        if (!chosen) return std::nullopt;
        ops_.generateMap(*name, seedValue, std::atoi(size->c_str()), *chosen);
        const auto maps = ops_.listMaps();
        const auto found = std::find_if(maps.begin(), maps.end(), [&](const Row &r) { return r[1] == *name; });
        return found == maps.end() ? std::nullopt : std::optional<long>{std::stol((*found)[0])};
    }

    // Makes a library map the active one; asks for confirmation when it was refused.
    bool loadMapFlow(long id) {
        try {
            ops_.loadMap(id, false);
            return true;
        } catch (const OpError &e) {
            if (!confirm(std::string(e.what()) + "\n" + _("Load anyway?"))) {
                return false;
            }
            ops_.loadMap(id, true);
            return true;
        }
    }

    void mapsDialog() {
        auto cache = std::make_shared<Rows>();
        showDialog(new ListDialog(
            _("World maps"),
            [this] {
                const Row a = ops_.activeMap();
                return a[0].empty() ? std::string(_("Active map: none imported"))
                                    : std::string(_("Active map: seed ")) + a[0] + ", " + a[1] + " px, sha256 " + a[2];
            },
            [this, cache] {
                *cache = ops_.listMaps();
                std::vector<std::string> out;
                for (const auto &r : *cache) {
                    out.push_back(pad(r[1], 20) + pad(r[2], 11) + pad(r[3], 5) + pad(r[4], 9) + pad(r[5], 17) + r[6]);
                }
                return out;
            },
            {
                {_("~S~ave active"), [this](long) {
                     if (auto name = ask(_("Save map"), _("Name in the library:"))) {
                         ops_.saveActiveMap(*name);
                     }
                 }, false},
                {_("~G~enerate map"), [this](long) {
                     if (generateMapFlow("")) {
                         messageBox(_("The map was generated and saved. Use Load to make it active."),
                                    mfInformation | mfOKButton);
                     }
                 }, false},
                {_("~L~oad"), [this, cache](long i) {
                     if (!confirm(_("Replace the active world map? Restart the server afterwards."))) {
                         return;
                     }
                     loadMapFlow(idOf(*cache, i));
                 }},
                {_("~R~ename"), [this, cache](long i) {
                     if (auto name = ask(_("Rename map"), _("New name:"), (*cache).at(i)[1])) {
                         ops_.renameMap(idOf(*cache, i), *name);
                     }
                 }},
                {_("~D~elete"), [this, cache](long i) {
                     if (confirm(_("Delete this saved map?"))) {
                         ops_.deleteMap(idOf(*cache, i));
                     }
                 }},
                {_("~E~xport .pgm"), [this, cache](long i) {
                     if (auto path = ask(_("Export map"), _("New target file (never overwritten):"), safeFileName((*cache).at(i)[1]) + ".pgm")) {
                         ops_.exportMap(idOf(*cache, i), *path);
                     }
                 }},
            }));
    }

    void usersDialog() {
        auto cache = std::make_shared<Rows>();
        showDialog(new ListDialog(
            _("Players"), [] { return std::string(_("States: alive, dead, banned")); },
            [this, cache] {
                *cache = ops_.listPlayers();
                std::vector<std::string> out;
                for (const auto &r : *cache) {
                    out.push_back(pad(r[1], 34) + pad(r[2], 9) + pad(r[3], 12) + r[4]);
                }
                return out;
            },
            {
                {_("~R~ename"), [this, cache](long i) {
                     if (auto name = ask(_("Rename player"), _("New name:"), (*cache).at(i)[1])) {
                         ops_.renamePlayer(idOf(*cache, i), *name);
                     }
                 }},
                {_("~B~an"), [this, cache](long i) {
                     if (auto reason = ask(_("Ban player"), _("Reason (optional):"))) {
                         ops_.banPlayer(idOf(*cache, i), *reason);
                     }
                 }},
                {_("~U~nban"), [this, cache](long i) { ops_.unbanPlayer(idOf(*cache, i)); }},
                {_("~D~elete"), [this, cache](long i) {
                     if (confirm(std::string(_("Delete player ")) + (*cache).at(i)[1] + "?")) {
                         ops_.deletePlayer(idOf(*cache, i));
                     }
                 }},
            }));
    }

    void portalsDialog() {
        auto cache = std::make_shared<Rows>();
        auto setState = [this, cache](const char *state) {
            return [this, cache, state](long i) { ops_.setPortalState(idOf(*cache, i), state); };
        };
        showDialog(new ListDialog(
            _("Linked worlds - portals"),
            [] { return std::string(_("Portals built by players and linked by share links; Open approves a pending link, Close breaks it.")); },
            [this, cache] {
                *cache = ops_.listPortals();
                std::vector<std::string> out;
                for (const auto &r : *cache) {
                    out.push_back(pad(r[1], 24) + pad(r[2], 26) + pad(r[3], 10) + r[4] + "/" + r[5] + "," + r[6]);
                }
                return out;
            },
            {
                {_("~N~ew"), [this](long) {
                     auto name = ask(_("New portal"), _("Name (a-z, 0-9, -):"));
                     if (!name) return;
                     auto peer = ask(_("New portal"), _("Peer world DNS name (optional):"));
                     if (!peer) return;
                     auto pos = ask(_("New portal"), _("Position face,x,y:"), "0,0,0");
                     if (!pos) return;
                     int face = 0, x = 0, y = 0;
                     if (std::sscanf(pos->c_str(), "%d,%d,%d", &face, &x, &y) != 3) {
                         throw OpError(_("Use the format face,x,y."));
                     }
                     ops_.createPortal(*name, *peer, face, x, y);
                 }, false},
                {_("~O~pen"), setState("open")},
                {_("C~l~ose"), setState("closed")},
                {_("D~i~sable"), setState("disabled")},
                {_("~B~an"), [this, cache](long i) {
                     if (confirm(_("Ban this portal (and its peer)?"))) {
                         ops_.setPortalState(idOf(*cache, i), "banned");
                     }
                 }},
                {_("~D~elete"), [this, cache](long i) {
                     if (confirm(_("Cancel this portal?"))) {
                         ops_.deletePortal(idOf(*cache, i));
                     }
                 }},
            }));
    }

    static std::string describeUpdates(const WorldStatus &status) {
        std::string text;
        for (const auto &d : status.disks) {
            if (d.newer) {
                text += std::string(_("Datadisk ")) + d.id + ": " + d.worldVersion + " -> " + d.installedVersion + "\n";
            }
        }
        if (status.generator == GeneratorState::Changed) {
            text += std::string(_("The installed generator produces a different map for this seed.")) + "\n";
        }
        return text;
    }

    void worldStatusDialog() {
        auto status = std::make_shared<WorldStatus>();
        showDialog(new ListDialog(
            _("World status"),
            [this, status] {
                *status = ops_.worldStatus();
                std::string generator;
                switch (status->generator) {
                case GeneratorState::Current: generator = _("Generator: map is reproduced exactly (up to date)"); break;
                case GeneratorState::Changed: generator = _("Generator: a newer generator gives a different map"); break;
                case GeneratorState::Unavailable: generator = std::string(_("Generator: not checked (")) + status->generatorDetail + ")"; break;
                }
                return generator + "\n" + (status->updateAvailable() ? _("Update available: prepare it below.") : _("Nothing to update."));
            },
            [status] {
                std::vector<std::string> out;
                for (const auto &d : status->disks) {
                    const std::string state = d.installedVersion.empty() ? _("not installed")
                                              : d.newer                  ? std::string(_("NEWER: ")) + d.installedVersion
                                                                         : _("up to date");
                    out.push_back(pad(d.id, 18) + pad(d.worldVersion, 10) + state);
                }
                if (out.empty()) {
                    out.push_back(_("The world uses no datadisks."));
                }
                return out;
            },
            {
                {_("~P~repare update"), [this, status](long) {
                     if (!status->updateAvailable()) {
                         throw OpError(_("Nothing to update."));
                     }
                     if (!confirm(_("Generate an updated map with the same seed and the installed datadisks and save it in the library?"))) {
                         return;
                     }
                     const std::string name = ops_.prepareWorldUpdate();
                     const auto maps = ops_.listMaps();
                     const auto found = std::find_if(maps.begin(), maps.end(), [&](const Row &r) { return r[1] == name; });
                     if (found == maps.end() ||
                         !confirm(std::string(_("Saved as ")) + name + ".\n" + _("Make it the active world now? Players' progress in the current world is lost. Restart the server afterwards."))) {
                         return;
                     }
                     const long id = std::stol((*found)[0]);
                     loadMapFlow(id);
                 }, false},
            }));
    }

    void serverDialog() {
        showDialog(new ListDialog(
            _("Server"),
            [this] {
                std::string header = listenText() + "\n" + serviceLine(ops_.serviceStatus());
                const Row s = ops_.scheduledShutdown();
                header += "\n" + (s.empty() ? std::string(_("No shutdown is scheduled."))
                                           : std::string(_("Shutdown in ")) + s[0] + " s" +
                                                 (s[1].empty() ? "" : ": " + s[1]));
                return header;
            },
            [] { return std::vector<std::string>{}; },
            {
                {_("St~a~rt service"), [this](long) { ops_.startService(); }, false},
                {_("~S~hut down..."), [this](long) {
                     auto delay = ask(_("Shut down server"), _("Delay in seconds (players see a notice):"), "60");
                     if (!delay) return;
                     auto message = ask(_("Shut down server"), _("Message for players (optional):"));
                     if (!message) return;
                     if (confirm(_("The server service will stop after the delay. Continue?"))) {
                         ops_.scheduleShutdown(std::atoi(delay->c_str()), *message);
                     }
                 }, false},
                {_("~C~ancel shutdown"), [this](long) { ops_.cancelShutdown(); }, false},
            },
            3));
    }

    Ops &ops_;
};

int runApp(const std::string &databaseUrl) {
    try {
        Db db(databaseUrl);
        Ops ops(db);
        AdminApp app(ops);
        app.showStartupStatus();
        app.announceUpdates();
        app.run();
        return 0;
    } catch (const std::exception &e) {
        const std::string message = e.what();
        std::fprintf(stderr, "ishtaria-admin: %s\n", e.what());
        if ((message.find("role") != std::string::npos && message.find("does not exist") != std::string::npos) ||
            message.find("Peer authentication failed") != std::string::npos) {
            std::fprintf(stderr, "Hint: the default connection uses the database role of the current user. "
                                 "Run it as the service user: sudo -u ishtaria ishtaria-admin\n");
        }
        return 1;
    }
}

} // namespace ishtariaadmin
