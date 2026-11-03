#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <csignal>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <mutex>
#include <atomic>
#include <iomanip>

#include "natsuyumecore.h"

namespace fs = std::filesystem;
using namespace Natsuyume;

// ---------------------------------------------------------------------------
// Signal handling (async-signal-safe volatile sig_atomic_t)
// ---------------------------------------------------------------------------
static volatile std::sig_atomic_t g_running = 1;

static void handleSignal(int /*sig*/)
{
    g_running = 0;
}

// ---------------------------------------------------------------------------
// RAII Stdin Status Flags Restorer
// ---------------------------------------------------------------------------
struct StdinNonBlockingGuard
{
    int origFlags = -1;
    bool active = false;

    bool enable()
    {
        origFlags = fcntl(STDIN_FILENO, F_GETFL, 0);
        if (origFlags < 0) {
            std::perror("fcntl(F_GETFL)");
            return false;
        }
        if (fcntl(STDIN_FILENO, F_SETFL, origFlags | O_NONBLOCK) < 0) {
            std::perror("fcntl(F_SETFL)");
            return false;
        }
        active = true;
        return true;
    }

    ~StdinNonBlockingGuard()
    {
        if (active && origFlags >= 0) {
            fcntl(STDIN_FILENO, F_SETFL, origFlags);
        }
    }
};

// ---------------------------------------------------------------------------
// Thread-safe local status latching (prevents background thread console garble)
// ---------------------------------------------------------------------------
struct LatchedStatus
{
    std::mutex mutex;
    std::atomic<bool> active{true};
    bool hasTrackUpdate = false;
    bool hasStateUpdate = false;
    bool hasScanProgress = false;
    bool isScanning = false;
    int scanCurrent = 0;
    int scanTotal = 0;
    std::string scanningFile;
    CoreTrack currentTrack;
    bool isPlaying = false;
};

static LatchedStatus g_status;

// ---------------------------------------------------------------------------
// Formatting helpers
// ---------------------------------------------------------------------------
static std::string formatTime(int64_t ms)
{
    if (ms < 0) ms = 0;
    int64_t totalSeconds = ms / 1000;
    int64_t minutes = totalSeconds / 60;
    int64_t seconds = totalSeconds % 60;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02lld:%02lld", (long long)minutes, (long long)seconds);
    return buf;
}

static std::string resolveDataDir()
{
    const char *xdg = std::getenv("XDG_DATA_HOME");
    if (xdg && *xdg) {
        return std::string(xdg) + "/natsuyume";
    }
    const char *home = std::getenv("HOME");
    if (home && *home) {
        return std::string(home) + "/.local/share/natsuyume";
    }
    return "";
}

static void printHelp()
{
    std::cout << "\nNatsuyume CLI Commands:\n"
              << "  <number>          Jump to track index\n"
              << "  play              Resume playback\n"
              << "  pause             Pause playback\n"
              << "  next              Next track\n"
              << "  prev              Previous track (or restart track if > 5s)\n"
              << "  seek <sec>        Seek to position in seconds\n"
              << "  volume <0-100>    Set playback volume\n"
              << "  shuffle           Toggle shuffle mode\n"
              << "  repeat            Cycle repeat mode (off/queue/track)\n"
              << "  list              Show current queue tracks\n"
              << "  status            Display playback and current track info\n"
              << "  scan <folder>     Scan audio files in folder\n"
              << "  help              Show this help menu\n"
              << "  quit, q           Exit Natsuyume CLI\n\n";
}

static void printTrackList(const std::vector<CoreTrack> &tracks, int currentIndex = -1)
{
    if (tracks.empty()) {
        std::cout << "No tracks in list.\n";
        return;
    }
    std::cout << "\nTracks (" << tracks.size() << "):\n";
    for (size_t i = 0; i < tracks.size(); ++i) {
        const auto &t = tracks[i];
        std::string title = t.title.empty() ? fs::path(t.path).stem().string() : t.title;
        std::string artist = t.artist.empty() ? "" : t.artist + " — ";
        std::string marker = (static_cast<int>(i) == currentIndex) ? "> " : "  ";
        std::cout << marker << "[" << (i + 1) << "] " << artist << title
                  << " (" << formatTime(t.duration) << ")\n";
    }
    std::cout << "\n";
}

// ---------------------------------------------------------------------------
// Entry Point
// ---------------------------------------------------------------------------
int main(int argc, char *argv[])
{
    // Configure signal handling for clean exit
    struct sigaction sa{};
    sa.sa_handler = handleSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // No SA_RESTART so poll() unblocks with EINTR
    if (sigaction(SIGINT, &sa, nullptr) < 0 || sigaction(SIGTERM, &sa, nullptr) < 0) {
        std::perror("sigaction");
        return 1;
    }

    // Set up non-blocking standard input with RAII restoration
    StdinNonBlockingGuard stdinGuard;
    if (!stdinGuard.enable()) {
        std::cerr << "Failed to configure standard input.\n";
        return 1;
    }

    // Resolve XDG application data directory
    const std::string dataDir = resolveDataDir();
    if (dataDir.empty()) {
        std::cerr << "Error: Neither $XDG_DATA_HOME nor $HOME environment variables are set.\n";
        return 1;
    }

    std::error_code ec;
    fs::create_directories(dataDir, ec);
    if (ec) {
        std::cerr << "Error: Failed to create data directory '" << dataDir << "': " << ec.message() << "\n";
        return 1;
    }

    // Instantiate core and configure callbacks
    NatsuyumeCore core;

    core.callbacks.onTrackChanged = [](const CoreTrack &track) {
        if (!g_status.active.load()) return;
        std::lock_guard<std::mutex> lock(g_status.mutex);
        g_status.currentTrack = track;
        g_status.hasTrackUpdate = true;
    };

    core.callbacks.onPlaybackStateChanged = [](bool isPlaying) {
        if (!g_status.active.load()) return;
        std::lock_guard<std::mutex> lock(g_status.mutex);
        g_status.isPlaying = isPlaying;
        g_status.hasStateUpdate = true;
    };

    core.callbacks.onScanProgressChanged = [](int current, int total, const std::string &file) {
        if (!g_status.active.load()) return;
        std::lock_guard<std::mutex> lock(g_status.mutex);
        g_status.scanCurrent = current;
        g_status.scanTotal = total;
        g_status.scanningFile = file;
        g_status.hasScanProgress = true;
    };

    core.callbacks.onScanningChanged = [](bool isScanning) {
        if (!g_status.active.load()) return;
        std::lock_guard<std::mutex> lock(g_status.mutex);
        g_status.isScanning = isScanning;
    };

    // Initialize core with resolved data directory
    core.setDataDir(dataDir);
    if (!core.init()) {
        std::cerr << "Error: Failed to initialize NatsuyumeCore database at '" << dataDir << "'.\n";
        return 1;
    }

    std::cout << "Natsuyume Engine initialized. Data directory: " << dataDir << "\n";

    // Restore saved session from previous run (if present)
    core.restoreLastSession();

    // Optional initial scan from CLI argument
    if (argc >= 2) {
        std::string folder = argv[1];
        if (!fs::exists(folder) || !fs::is_directory(folder)) {
            std::cerr << "Error: Path '" << folder << "' is not a valid directory.\n";
        } else {
            std::cout << "Scanning folder: " << folder << " ...\n";
            core.scanFolder(folder);

            // Drain until initial scan completes or user interrupts
            while (g_running && core.isScanning()) {
                struct pollfd pfd = { STDIN_FILENO, POLLIN, 0 };
                ::poll(&pfd, 1, 50);
                core.drainLibraryCallbacks();

                std::lock_guard<std::mutex> lock(g_status.mutex);
                if (g_status.hasScanProgress) {
                    std::cout << "\rIndexing: [" << g_status.scanCurrent << "/"
                              << g_status.scanTotal << "] "
                              << g_status.scanningFile << "   " << std::flush;
                    g_status.hasScanProgress = false;
                }
            }
            core.drainLibraryCallbacks();
            std::cout << "\nScan complete.\n";

            // Load scanned library tracks into a new queue only if no session was restored
            if (core.trackList().empty()) {
                auto libraryTracks = core.tracksForPlaylist(NatsuyumeCore::allSongsPlaylistId());
                if (!libraryTracks.empty()) {
                    std::vector<std::string> paths;
                    paths.reserve(libraryTracks.size());
                    for (const auto &t : libraryTracks) paths.push_back(t.path);
                    core.openFilesInNewQueue(paths, fs::path(folder).filename().string());
                }
            }
        }
    }

    // Clear initial latch updates before interactive prompt
    {
        std::lock_guard<std::mutex> lock(g_status.mutex);
        g_status.hasTrackUpdate = false;
        g_status.hasStateUpdate = false;
    }

    // Inspect active or restored queue
    auto currentQueueTracks = core.trackList();
    if (!currentQueueTracks.empty()) {
        std::cout << "Active queue has " << currentQueueTracks.size() << " track(s).\n";
        printTrackList(currentQueueTracks, core.playingTrackIndex());
    } else {
        auto libTracks = core.tracksForPlaylist(NatsuyumeCore::allSongsPlaylistId());
        if (!libTracks.empty()) {
            std::cout << "Library contains " << libTracks.size() << " track(s). Type a number to load and play.\n";
            printTrackList(libTracks);
        } else {
            std::cout << "Library is empty. Use 'scan <folder>' to add music.\n";
        }
    }

    std::cout << "Enter a command or track number. Type 'help' for commands.\n> " << std::flush;

    // Interactive event loop
    std::string inputAccumulator;
    struct pollfd pfd = { STDIN_FILENO, POLLIN, 0 };

    while (g_running) {
        int ret = ::poll(&pfd, 1, 100); // 100ms tick
        if (ret < 0) {
            if (errno == EINTR) {
                if (!g_running) break;
                continue;
            }
            std::perror("poll");
            break;
        }

        // Drain library callbacks on main thread
        core.drainLibraryCallbacks();

        // Print latched status updates if the user is not in the middle of typing
        if (inputAccumulator.empty()) {
            std::lock_guard<std::mutex> lock(g_status.mutex);
            if (g_status.hasTrackUpdate) {
                std::string title = g_status.currentTrack.title.empty()
                    ? fs::path(g_status.currentTrack.path).stem().string()
                    : g_status.currentTrack.title;
                std::string artist = g_status.currentTrack.artist.empty()
                    ? "" : g_status.currentTrack.artist + " — ";
                std::cout << "\nNow playing: " << artist << title << "\n> " << std::flush;
                g_status.hasTrackUpdate = false;
            }
            if (g_status.hasStateUpdate) {
                std::cout << (g_status.isPlaying ? "\n[playing]" : "\n[paused]") << "\n> " << std::flush;
                g_status.hasStateUpdate = false;
            }
            if (g_status.hasScanProgress) {
                std::cout << "\rIndexing: [" << g_status.scanCurrent << "/"
                          << g_status.scanTotal << "] "
                          << g_status.scanningFile << "   " << std::flush;
                g_status.hasScanProgress = false;
            }
        }

        // Read available input from stdin
        if (pfd.revents & (POLLIN | POLLHUP | POLLERR)) {
            char buf[256];
            bool eofReached = false;

            while (true) {
                ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
                if (n > 0) {
                    inputAccumulator.append(buf, n);
                } else if (n == 0) {
                    eofReached = true;
                    break;
                } else {
                    if (errno == EINTR) continue;
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    eofReached = true;
                    break;
                }
            }

            // If EOF was reached and uncommitted input remains without a trailing newline,
            // append a newline so the line processor executes it as a final command
            if (eofReached && !inputAccumulator.empty() && inputAccumulator.back() != '\n') {
                inputAccumulator.push_back('\n');
            }

            // Extract and process complete lines
            size_t newlinePos;
            while ((newlinePos = inputAccumulator.find('\n')) != std::string::npos) {
                std::string line = inputAccumulator.substr(0, newlinePos);
                inputAccumulator.erase(0, newlinePos + 1);

                if (!line.empty() && line.back() == '\r') line.pop_back();

                // Trim leading/trailing whitespace
                while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front())))
                    line.erase(line.begin());
                while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back())))
                    line.pop_back();

                if (line.empty()) {
                    if (!eofReached) std::cout << "> " << std::flush;
                    continue;
                }

                // Check for numeric track selection
                bool isNumber = true;
                for (char c : line) {
                    if (!std::isdigit(static_cast<unsigned char>(c))) {
                        isNumber = false;
                        break;
                    }
                }

                if (isNumber) {
                    int index = std::stoi(line) - 1;
                    auto queueTracks = core.trackList();
                    if (!queueTracks.empty()) {
                        if (index >= 0 && index < static_cast<int>(queueTracks.size())) {
                            core.jumpToTrack(index);
                        } else {
                            std::cout << "Invalid track index.\n> " << std::flush;
                        }
                    } else {
                        // Only fall back to library tracks when the active queue is empty
                        auto libTracks = core.tracksForPlaylist(NatsuyumeCore::allSongsPlaylistId());
                        if (index >= 0 && index < static_cast<int>(libTracks.size())) {
                            std::vector<std::string> paths;
                            paths.reserve(libTracks.size());
                            for (const auto &t : libTracks) paths.push_back(t.path);
                            core.openFilesInNewQueue(paths, "Library");
                            core.jumpToTrack(index);
                        } else {
                            std::cout << "Invalid track index.\n> " << std::flush;
                        }
                    }
                    continue;
                }

                // Command dispatch
                if (line == "play") {
                    core.play();
                } else if (line == "pause") {
                    core.pause();
                } else if (line == "next") {
                    core.playNext();
                } else if (line == "prev") {
                    if (core.position() > 5000)
                        core.seekTo(0);
                    else
                        core.playPrevious();
                } else if (line == "list") {
                    auto queueTracks = core.trackList();
                    if (!queueTracks.empty()) {
                        printTrackList(queueTracks, core.playingTrackIndex());
                    } else {
                        auto libTracks = core.tracksForPlaylist(NatsuyumeCore::allSongsPlaylistId());
                        printTrackList(libTracks);
                    }
                    std::cout << "> " << std::flush;
                } else if (line == "status") {
                    CoreTrack cur = core.currentTrack();
                    std::cout << "\nPlayback Status:\n"
                              << "  State:    " << (core.isPlaying() ? "Playing" : "Paused") << "\n"
                              << "  Track:    " << (cur.title.empty() ? "(none)" : cur.title) << "\n"
                              << "  Artist:   " << (cur.artist.empty() ? "(none)" : cur.artist) << "\n"
                              << "  Album:    " << (cur.album.empty() ? "(none)" : cur.album) << "\n"
                              << "  Time:     " << formatTime(core.position()) << " / " << formatTime(core.duration()) << "\n"
                              << "  Volume:   " << static_cast<int>(core.volume() * 100.0f + 0.5f) << "%\n"
                              << "  Shuffle:  " << (core.isShuffled() ? "On" : "Off") << "\n"
                              << "  Repeat:   " << (core.repeatMode() == 0 ? "Off" : (core.repeatMode() == 1 ? "Queue" : "Track")) << "\n\n> "
                              << std::flush;
                } else if (line.rfind("seek ", 0) == 0) {
                    try {
                        int64_t sec = std::stoll(line.substr(5));
                        core.seekTo(sec * 1000);
                        std::cout << "Seek to " << formatTime(sec * 1000) << "\n> " << std::flush;
                    } catch (...) {
                        std::cout << "Usage: seek <seconds>\n> " << std::flush;
                    }
                } else if (line.rfind("volume ", 0) == 0) {
                    try {
                        int vol = std::stoi(line.substr(7));
                        vol = std::clamp(vol, 0, 100);
                        core.setVolume(vol / 100.0f);
                        std::cout << "Volume set to " << vol << "%\n> " << std::flush;
                    } catch (...) {
                        std::cout << "Usage: volume <0-100>\n> " << std::flush;
                    }
                } else if (line == "shuffle") {
                    core.toggleShuffle();
                    std::cout << "Shuffle: " << (core.isShuffled() ? "On" : "Off") << "\n> " << std::flush;
                } else if (line == "repeat") {
                    core.cycleRepeatMode();
                    int r = core.repeatMode();
                    std::cout << "Repeat: " << (r == 0 ? "Off" : (r == 1 ? "Queue" : "Track")) << "\n> " << std::flush;
                } else if (line == "scan") {
                    std::cout << "Usage: scan <folder>\n> " << std::flush;
                } else if (line.rfind("scan ", 0) == 0) {
                    if (core.isScanning()) {
                        std::cout << "Error: A library scan is already in progress.\n> " << std::flush;
                    } else {
                        std::string folder = line.substr(5);
                        while (!folder.empty() && std::isspace(static_cast<unsigned char>(folder.front())))
                            folder.erase(folder.begin());
                        while (!folder.empty() && std::isspace(static_cast<unsigned char>(folder.back())))
                            folder.pop_back();

                        if (folder.empty()) {
                            std::cout << "Usage: scan <folder>\n> " << std::flush;
                        } else if (!fs::exists(folder) || !fs::is_directory(folder)) {
                            std::cout << "Error: Directory not found: " << folder << "\n> " << std::flush;
                        } else {
                            std::cout << "Scanning folder: " << folder << "\n";
                            core.scanFolder(folder);
                        }
                    }
                } else if (line == "help") {
                    printHelp();
                    std::cout << "> " << std::flush;
                } else if (line == "quit" || line == "q") {
                    g_running = 0;
                    break;
                } else {
                    std::cout << "Unknown command: '" << line << "'. Type 'help' for commands.\n> " << std::flush;
                }
            }

            if (eofReached) {
                g_running = 0;
                break;
            }
        }
    }

    std::cout << "\nShutting down Natsuyume CLI...\n";

    // Deactivate local callback latch to prevent any straggling events
    g_status.active.store(false);

    // Cancel in-flight scan and join worker thread
    core.cancelScan();

    // Drain any remaining library callbacks on main thread
    core.drainLibraryCallbacks();

    // Persist session state to SQLite
    core.saveAndShutdown();

    std::cout << "Shutdown complete.\n";
    return 0;
}
