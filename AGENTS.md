# Natsuyume — Canonical Project Instructions

This document is the authoritative guide for all AI coding agents (including Antigravity, Claude Code, Codex, and other automated assistants) and human contributors working on the Natsuyume codebase.

All agents must read and strictly adhere to this document before inspecting, planning, or modifying any code in this repository.

---

## 1. Project Overview

### 1.1 Purpose and Design Philosophy
Natsuyume (夏夢) is a personal, local-first music player originally inspired by Musicolet. Its core design philosophy centers on:
- **Dedicated Local Playback:** Focused entirely on high-fidelity offline audio playback (FLAC, MP3, OGG, Opus, M4A, WAV). No cloud sync, streaming services, or telemetry.
- **Multi-Queue Listening Workflow:** Users can maintain multiple named queues simultaneously and switch between them without losing playback position, active track, or shuffle state.
- **High-Performance Native C++ Engine:** Audio decoding, gapless playback, metadata parsing, and library database management are handled in a pure C++ core (`NatsuyumeCore`).
- **Synchronized Lyrics:** Native support for both embedded lyrics and external `.lrc` sidecar files with millisecond-accurate timestamp parsing.
- **Clean Separation of Concerns:** Database storage separates reproducible library metadata (`library.db`) from persistent user data and queue sessions (`userdata.db`).

### 1.2 Current Implementation Status
- **Historical Baseline:** Natsuyume was initially developed with a Flutter/Dart user interface communicating with `libnatsuyume_core.so` via `dart:ffi` on Android (targeting ARM64-v8a devices).
- **Current Repository State:** The Flutter/Dart frontend and desktop runner scaffolding have been removed from the working tree. The repository currently retains:
  - The C++ audio core (`core/src/`)
  - The Android NDK integration bridge (`android/app/src/main/cpp/natsuyume_bridge.cpp`)
  - The Android background playback service and media notification logic (`android/app/src/main/kotlin/`)
  - Bundled ARM64-v8a prebuilts and third-party headers (`android/jni/`)
  - A prototype CLI entry point (`core/src/cli/main_cli.cpp`)
- **Current Direction:** The immediate objective is to make Natsuyume Core independently executable through a native Linux CLI without requiring Qt, QML, Flutter, Dart, or Kotlin. Once the core and CLI are decoupled, verified, and stabilized, Android and potential future frontends will be re-integrated against the clean C++ engine.

---

## 2. Architecture

### 2.1 Current Architecture (Observed from Code Inspection)

The active codebase is structured into the following modules:

```
Natsuyume Repository
├── core/
│   ├── CMakeLists.txt              # Configured for Android NDK; links prebuilt ARM64 libs
│   └── src/
│       ├── domain/
│       │   └── track.h / track.cpp # Domain Track model (20 fields + raw cover buffer)
│       ├── core/
│       │   ├── coretypes.h         # CoreTrack (struct duplicating Track fields), enums
│       │   ├── lrcparser.h / .cpp  # Regex-based LRC lyric parser
│       │   └── natsuyumecore.h/.cpp# NatsuyumeCore public facade (~60 methods, pImpl Impl)
│       ├── playback/
│       │   ├── playback.h / .cpp   # Playback: libmpv client wrapper, self-pipe, event thread
│       │   └── queue.h / .cpp      # Queue: tracks vector, shuffle, repeat, owns Playback*
│       ├── player/
│       │   ├── queuesession.h/.cpp # QueueSession: vector of Queue*, active/viewed indices
│       │   ├── queuemanager.h/.cpp # QueueManager: high-level queue ops and switching
│       │   ├── playbackmanager.h/.cpp # PlaybackManager: transport, gapless, direct SQLite queries
│       │   ├── librarymanager.h/.cpp  # LibraryManager: scan facade, sort prefs, callback queue
│       │   └── userdatamanager.h/.cpp # UserDataManager: pass-through to UserData
│       ├── library/
│       │   ├── library.h / .cpp    # Library: SQLite wrapper for library.db (tracks schema)
│       │   ├── userdata.h / .cpp   # UserData: SQLite wrapper for userdata.db (user/playlists)
│       │   ├── fileindexer.h/.cpp  # FileIndexer: recursive filesystem scanner on worker thread
│       │   └── metadata.h / .cpp   # Metadata: TagLib + ICU metadata/cover extractor
│       └── cli/
│           └── main_cli.cpp        # Prototype CLI using QCoreApplication & QSocketNotifier
└── android/
    ├── app/src/main/cpp/
    │   ├── CMakeLists.txt          # Builds natsuyume_bridge SHARED library
    │   └── natsuyume_bridge.cpp    # C-FFI exports (ncore_*) wrapping NatsuyumeCore
    ├── app/src/main/kotlin/        # MediaSession, AudioFocus, Foreground service
    └── jni/                        # Prebuilt arm64-v8a libs (libmpv, libtag, ffmpeg) & headers
```

#### Dual Database Strategy
- `library.db` (managed by `Library`): Stores discovered audio files, tags, duration, and file modification timestamps. Completely reproducible via a folder rescan.
- `userdata.db` (managed by `UserData`, but also queried directly by `PlaybackManager` and `LibraryManager`): Stores persistent user state: favorites, play counts, playlists, artist descriptions, sort preferences, and saved queue snapshots. Never wiped during a rescan.

---

### 2.2 Intended Architecture (Target Design)

The intended architecture simplifies the core around clear responsibilities, single state ownership, and platform decoupling:

```
+─────────────────────────────────────────────────────────────+
|                     FRONTENDS / CONSUMERS                   |
|  - Standalone Linux CLI (POSIX poll on stdin + wakeupFd)    |
|  - Android Bridge (C-FFI / NativeCallable)                  |
|  - Future Desktop / Mobile Frontends                        |
+──────────────────────────────┬──────────────────────────────+
                               │ Calls C++ API / Pumps Events
                               ▼
+─────────────────────────────────────────────────────────────+
|                      NatsuyumeCore                          |
|  - Public engine facade                                     |
|  - Owns Core EventQueue and non-blocking wakeup pipe        |
|  - Exposes wakeupFd() and pumpEvents() for main-thread sync |
+───────┬──────────────────────┬──────────────────────┬───────+
        │                      │                      │
        ▼                      ▼                      ▼
+────────────────+    +────────────────+    +─────────────────+
| LibraryManager |    | UserDataManager|    | PlaybackManager |
| - Scan coord   |    | - Playlists    |    | - Single active |
| - Queries      |    | - Favorites    |    |   Playback eng  |
| - Sort prefs   |    | - Play stats   |    | - Coordinates   |
+───────┬────────+    +────────┬───────+    |   QueueSession  |
        │                      │            +────────┬────────+
        ▼                      ▼                     │
+────────────────+    +────────────────+             │ Controls & Reads
|    Library     |    |    UserData    |             ▼
| (library.db)   |    | (userdata.db)  |    +─────────────────+
| Single conn    |    | Single conn    |    |  QueueSession   |
+────────────────+    +────────────────+    |  - Vector of    |
                                            |    Queue (data) |
                                            +────────┬────────+
                                                     │ Reads active
                                                     ▼
                                            +─────────────────+
                                            |    Playback     |
                                            | - 1 mpv_handle  |
                                            | - 1 event thread|
                                            +─────────────────+
```

#### Key Tenets of the Intended Architecture:
1. **Frontend-Agnostic Core:** The core C++ engine contains zero references to Qt, Android NDK headers, or UI frameworks.
2. **CLI as Primary Validation Tool:** A standalone POSIX CLI serves as the initial development, testing, and debugging interface on Linux.
3. **Queue as Pure Data:** `Queue` stores track collections, shuffle permutations, repeat modes, and saved positions. `Queue` owns **zero native audio resources** (`mpv_handle`). `Queue` objects do not pause, resume, or save playback-engine state themselves; all engine manipulation is strictly coordinated by `PlaybackManager`.
4. **Single Active Playback Engine:** `PlaybackManager` owns exactly **one** `Playback` instance for the entire application. Inactive queues are data snapshots in memory and never instantiate playback engines. When an active queue is removed, `PlaybackManager` snapshots and handles the active playback engine before `QueueSession` destroys the queue object.
5. **Single Active mpv Handle & Dedicated Event Thread:** Exactly one active `mpv_handle` and one application-managed background MPV event thread exist across the process lifetime (rather than spawning multiple event threads per queue). Note that libmpv maintains internal decoder/demuxer worker threads independently; this requirement applies strictly to application-managed playback resources and does not imply that libmpv or the entire application must be single-threaded.
6. **Explicit Event Delivery Contract:** Subsystems post events to a core-owned thread-safe queue and signal a non-blocking self-pipe. The consumer explicitly pumps events on its main thread (`core.pumpEvents()`). Callbacks execute strictly on the caller thread outside core locks.
7. **Stale Event Suppression (Proposed Implementation Approach):** A mechanism for invalidating obsolete EOF, position, and seek events from previous tracks or queues is under design. Any implementation must first verify actual libmpv event semantics, asynchronous command completion, and event ordering. Agents must not assume that libmpv command reply IDs automatically identify subsequent property change or end-file events.
8. **Clean Database Ownership:** `UserData` exclusively owns all operations on `userdata.db`; `Library` exclusively owns all operations on `library.db`. Ad-hoc SQLite connections across manager classes are eliminated.

---

## 3. Coding and Architectural Principles

All contributors and AI agents must adhere to these rules:

1. **Focused, Single-Responsibility Components:** Classes must have a single authoritative role. Avoid creating "God objects" that mix transport, database persistence, metadata extraction, and callback dispatch.
2. **No Duplicated Implementations:** Do not maintain parallel data models or dual transport APIs. Data models and operations must have one authoritative implementation.
3. **Minimize External Dependencies:** Rely on the C++17 standard library wherever possible. For desktop audio, utilize system libraries (`libmpv`, `taglib`, `icu-uc`, `sqlite3`) via `pkg-config`.
4. **Isolate Platform-Specific Code:** Android-specific headers (`<android/log.h>`) and JNI bindings must remain strictly isolated within `android/` or behind platform abstraction headers. Core source files must compile identically on Linux desktop and Android.
5. **No Framework Contamination in Core or CLI:** Core libraries and the CLI entry point must not include Qt headers (`<QCoreApplication>`, `<QSocketNotifier>`) or mobile UI framework code.
6. **Preserve Working Behavior and API Stability:** Do not break existing public API signatures or the Android C-FFI bridge without inspecting all callers. Maintain backward compatibility for external consumers.
7. **Explicit State Ownership:** Every heap allocation and native resource must have a clear, documented owner. Never erase pointers from collection containers without defining ownership and deallocation responsibility. `QueueSession` must destroy each removed queue exactly once.
8. **License and Third-Party Compliance:** Respect licenses and distribution terms for bundled third-party code (SQLite amalgamation, TagLib, libmpv, FFmpeg).
9. **Document Architectural Rationale:** Never introduce new architectural abstractions, design patterns, or database schema changes without documenting the rationale and migration strategy.

---

## 4. Known Issues and Audit Findings

> [!NOTE]
> Repository file references below are relative to the repository root. Line numbers reflect the repository state at the time of the initial audit and may shift as files are updated; agents must verify line numbers against current source files before relying on them.

The following issues were identified during the read-only architectural audit. Agents must treat them according to their verified status:

### 4.1 Confirmed Source-Level Observations
These issues have been verified by direct inspection of repository source files:

1. **Android CMake Coupling (Resolved in Stage 0):** `core/CMakeLists.txt` previously hardcoded Android NDK variables (`${JNI_LIBS}`, `${JNI_INCLUDE}`), linked the NDK ICU stub and Android `log-lib`, and could not configure or build standalone on desktop Linux. Resolved in Stage 0 by separating Android and desktop configurations via `if(ANDROID) ... else() ... endif()` with system `pkg-config` imported targets.
2. **Android Logging Leak in Core (Resolved in Stage 0):** `core/src/player/playbackmanager.cpp` and `core/src/playback/queue.cpp` directly included `<android/log.h>`, causing compilation failures on non-Android systems. Resolved in Stage 0 by abstracting logging behind `core/src/core/logger.h`.
3. **Qt Dependency in CLI:** `core/src/cli/main_cli.cpp` (lines 1-3) includes `<QCoreApplication>` and `<QSocketNotifier>`, introducing an undeclared Qt dependency. It is also omitted from all CMake build targets.
4. **CLI Startup Initialization Bug:** `core/src/cli/main_cli.cpp` instantiates `NatsuyumeCore` and calls `core.init()` without first calling `core.setDataDir()`. In `core/src/core/natsuyumecore.cpp` (lines 77-90), `dataDir` defaults to empty string, causing database paths to resolve to `/library.db` and `/userdata.db`. On standard Linux systems without root permissions, this causes database opening to fail and aborts callback wiring.
5. **Engine Duplication in Queues:** `core/src/playback/queue.h` (line 112) declares `Playback *m_currentPlayback`. Each queue instantiates its own `Playback` instance, and `core/src/player/playbackmanager.cpp` (line 259) calls `q->initPlayback()` inside a loop for every restored queue, allocating multiple concurrent MPV handles.
6. **QueueSession Pointer Ownership Defect:** In `core/src/player/queuesession.cpp` (line 69), `removeQueueAt` erases `Queue*` from its internal vector without deleting the object. While `QueueManager::closeQueue` in `core/src/player/queuemanager.cpp` (line 180) manually deletes `toDelete`, direct usage of `QueueSession::removeQueueAt` permanently leaks heap memory.
7. **Database Migration Sequence Bug:** In `core/src/library/userdata.cpp` (line 76), `exec("ALTER TABLE playlists ADD COLUMN description...")` runs before line 85 runs `CREATE TABLE IF NOT EXISTS playlists`, failing on fresh databases.
8. **Duplicate Track Dropping via Composite Primary Keys:** In `core/src/library/userdata.cpp`, `playlist_tracks` has `PRIMARY KEY (playlist_id, path)` (line 98) and `queue_tracks` has `PRIMARY KEY (queue_id, track_path)` (line 126). Inserting the same track path multiple times into the same playlist or queue results in the second instance being silently discarded upon database save/restore via `INSERT OR IGNORE`.
9. **Data Model Duplication:** `core/src/domain/track.h` (`Track`) and `core/src/core/coretypes.h` (`CoreTrack`) define identical 20-field data structures. Deep member-by-member copy loops in `core/src/core/natsuyumecore.cpp` (lines 19-51) translate between them redundantly.
10. **Undrained Library Callbacks:** In `core/src/player/librarymanager.cpp` (lines 40-57), `m_pendingCallbacks` is only processed when `drainCallbacks()` is invoked. Neither `NatsuyumeCore` nor `main_cli.cpp` calls this method during scanning.

### 4.2 Suspected Concurrency and Stability Risks
These issues represent architectural risks identified during static analysis (unverified runtime consequences):

1. **Cross-Thread Callback Execution:** `Playback::m_eventThread` executes `handleMpvEvent()`, which directly calls user callbacks (`onPositionChanged`, `onPlaybackStateChanged`). This violates single-threaded assumptions in consumer code and presents data race hazards.
2. **Callback Re-Entrancy Hazard:** In `PlaybackManager::connectCurrentPlaybackCallbacks`, `onTrackAdvancedGapless` calls `queue->advancePlayback()`, which re-binds `onTrackAdvancedGapless` from within its own execution stack.
3. **Asynchronous Seek Race:** In `PlaybackManager::seekTo()`, `m_isSeeking` is set to `true` and immediately reset to `false` synchronously, while the actual `seek` command sent to MPV is asynchronous.
4. **Uncoordinated Multi-Connection SQLite Access:** `Library`, `UserData`, and `PlaybackManager` each maintain independent SQLite connections to the database files without configuring `sqlite3_busy_timeout`, risking transient `SQLITE_BUSY` errors under concurrent operations.

### 4.3 Confirmed Runtime & Environment Observations
1. **Desktop Host Toolchain Status:**
   - Operating System: `x86_64` Linux (Arch Linux / CachyOS).
   - CMake: Version 4.4.5 available.
   - C++ Compilers: GCC (`/usr/bin/g++`, version 16.2.1) and Clang (`/usr/bin/clang++`, version 23.1.4) available.
   - Build Tools: `pkg-config` (version 3.0.10), `make` (version 4.4.1), and `ninja` (version 1.13.2) available.
   - Available Libraries: `libmpv` (2.5.0), `taglib` (2.3.2), `icu-uc` (78.3), `icu-i18n` (78.3), and `sqlite3` (3.54.0) present and discoverable via `pkg-config`.
2. **Incompatible Bundled Binaries:** The precompiled binaries in `android/jni/libs/arm64-v8a/` are ELF 64-bit ARM aarch64 binaries for Android API 21 and cannot be linked on an `x86_64` Linux host. Desktop builds link system libraries discovered by `pkg-config`.
3. **Execution Verification Status:** The core engine (`libnatsuyume_core.so`) has been compiled, linked, and successfully dynamically loaded via `dlopen` on desktop Linux as of Stage 0. Actual `libmpv` initialization and end-to-end audio playback remain untested until Stage 1.

---

## 5. Development Roadmap

Refactoring is organized into six strictly sequential stages. Each stage has a clear scope, preserves existing functionality, and concludes with an explicit build/test checkpoint.

```
Stage 0 ──► Stage 1 ──► Stage 2 ──► Stage 3 ──► Stage 4 ──► Stage 5
Desktop     POSIX CLI   Event       Single      Database    API Cleanup
Build       & No Qt     Contract    Engine      Ownership   & Types
(COMPLETED) (PENDING)
```

---

### Stage 0: Desktop Build Foundation (COMPLETED)
- **Status:** **Completed & Verified** (Committed in `b7b4c71` on branch `main`).
- **Objective:** Establish a clean desktop Linux build baseline for `natsuyume_core` without modifying Android build scripts or prebuilt binaries.
- **Verified Implementation Details:**
  - Added a desktop-compatible, statement-safe logging implementation in `core/src/core/logger.h` with an `inline` variadic helper using `<cstdio>` and `<cstdarg>`.
  - Preserved Android logging behavior (`__android_log_print(ANDROID_LOG_DEBUG, "NatsuyumePM", ...)`) under `#ifdef __ANDROID__`.
  - Added native Linux CMake dependency configuration in `core/CMakeLists.txt` using `pkg-config` (`mpv`, `taglib`, `icu-uc`, `icu-i18n`, `sqlite3`) with automatic TagLib parent include mapping.
  - Added missing standard-library includes required for strict C++17 compilation under GCC 16 / modern libstdc++ (`<mutex>` in `core/src/library/library.h`, `<algorithm>` in `core/src/player/librarymanager.cpp`).
  - Added repository-root `.gitignore` covering generated build trees (`build/`, `build-*/`, `cmake-build-*/`, `out/`) and in-tree CMake generation artifacts.
  - Successfully configured and cleanly compiled `natsuyume_core` natively on Linux desktop (`build-desktop/libnatsuyume_core.so`, 2.4 MB, 0 compiler warnings, 0 errors).
  - Successfully dynamically loaded `libnatsuyume_core.so` at runtime via `dlopen` (verified via Python `ctypes.CDLL`).
  - Confirmed via `nm -D` and `readelf -d` that the desktop shared library links against standard desktop host libraries and contains zero Android-specific symbols or dependencies.
- **Important Qualifications & Untested Boundaries:**
  - The Android build has not been independently tested in this cycle (bundled ARM64-v8a prebuilts and JNI bridge build scripts were preserved intact, but not compiled on an Android target).
  - Actual `libmpv` initialization (`mpv_create`) and audio playback have not been tested.
  - A successful dynamic library load verifies linker symbols and relocations, but does not establish that every runtime feature or callback functions correctly.
  - Do not treat these untested areas as verified.
- **Files Modified / Added in Stage 0:**
  - `.gitignore` (added)
  - `core/CMakeLists.txt` (modified)
  - `core/src/core/logger.h` (added)
  - `core/src/library/library.h` (modified)
  - `core/src/playback/queue.cpp` (modified)
  - `core/src/player/librarymanager.cpp` (modified)
  - `core/src/player/playbackmanager.cpp` (modified)

---

### Stage 1: CLI Integration & Qt Decoupling (PENDING AUTHORIZATION)
- **Status:** **Pending Authorization** (Next active stage; implementation not yet authorized).
- **Objective:** Provide a working, pure C++ command-line interface for testing playback without Qt, integrated against the native Linux core.
- **Scope & Objectives:**
  - Replace the Qt-dependent CLI input mechanism in `core/src/cli/main_cli.cpp` (`<QCoreApplication>`, `<QSocketNotifier>`, `<QTimer>`) with a pure C++/POSIX approach (`poll()` on `STDIN_FILENO`).
  - Integrate the CLI with existing core APIs (`NatsuyumeCore` public methods).
  - Ensure the CLI uses the core's library-scanning functionality (`core.scanFolders(...)`, `core.scanProgress()`) instead of maintaining a separate scanner implementation.
  - Establish appropriate application data directory:
    - Use `$XDG_DATA_HOME/natsuyume` when `XDG_DATA_HOME` is set.
    - Otherwise, use `~/.local/share/natsuyume`.
    - Do not use the current working directory as the default persistent data directory.
  - Ensure initialization failures (database opening, directory creation) are handled gracefully with clear user diagnostics.
  - Register `add_executable(natsuyume-cli src/cli/main_cli.cpp)` in `core/CMakeLists.txt` (under desktop branch) and link `natsuyume_core`.
- **Strict Architectural Boundaries (Strictly Prohibited in Stage 1):**
  - Do not redesign `Queue` or `PlaybackManager`.
  - Do not introduce the proposed single-playback-engine architecture (deferred to Stage 3).
  - Do not implement the playback event system or wakeup self-pipe (deferred to Stage 2).
  - Do not modify database schemas, migrations, or SQLite access patterns (deferred to Stage 4).
  - Do not begin type consolidation or API cleanup (deferred to Stage 5).
  - Do not expand Stage 1 into unrelated refactoring.
  - The goal is to integrate the existing CLI with the newly portable core, not to redesign the application.
- **Checkpoint:** Build `natsuyume-cli` and run an interactive test session against a directory of audio tracks (scan, play, pause, next, prev, quit).

---

### Stage 2: Playback Event System & Threading Harmonization
- **Objective:** Establish the core-owned thread-safe event queue, non-blocking wakeup pipe, and consumer event pumping contract.
- **Scope:**
  - Implement `EventQueue` in `core/src/core/eventqueue.h/.cpp` with mutex-protected event deque and non-blocking notification self-pipe.
  - Expose `int NatsuyumeCore::wakeupFd() const` and `void NatsuyumeCore::pumpEvents()`.
  - Verify actual libmpv event semantics, asynchronous command completion, and event ordering before implementation.
  - Evaluate the proposed generation-counter approach or alternative stale-event filtering to associate events with specific track loads. Do not assume command reply IDs automatically identify subsequent playback events.
  - Modify `Playback` to enqueue events instead of invoking user callbacks directly from `m_eventThread`.
  - Integrate `core.wakeupFd()` into `main_cli.cpp`'s `poll()` loop.
- **Checkpoint:** Verify via automated assertions that 100% of `CoreCallbacks` execute on the main thread and that rapid track skipping drops stale position and EOF events.

---

### Stage 3: Playback Architecture & Single-Engine Ownership
- **Objective:** Unify playback into a single authoritative engine and decouple `Queue` from audio execution.
- **Scope:**
  - Transfer `Playback` ownership from `Queue` to `PlaybackManager`.
  - Strip transport methods (`play`, `pause`, `seekTo`) from `Queue`, making it a focused in-memory data collection. Queue objects must not pause, resume, or save playback-engine state themselves.
  - Ensure `PlaybackManager` creates exactly one `Playback` instance for the entire application.
  - Implement queue switching protocol: snapshot outgoing queue state, restore incoming queue state through the same engine.
  - Fix session restoration so inactive queues never instantiate native playback resources.
- **Checkpoint:** Test multi-queue switching across 10 queues; verify via system inspection that exactly one `mpv_handle` and one background thread exist throughout the process lifecycle.

---

### Stage 4: Persistence, Database & Ownership Integrity
- **Objective:** Fix database migration ordering, duplicate track handling, pointer ownership leaks, and multi-connection concurrency.
- **Scope:**
  - Inspect actual legacy database schemas and existing user data to ensure all data and relationships are preserved.
  - Implement transactional schema migrations in `UserData` using `PRAGMA user_version` (ensuring tables exist before running alterations; transactions must rollback on failure).
  - Evaluate schema improvements to resolve duplicate-track dropping in `playlist_tracks` and `queue_tracks` (such as surrogate primary keys `id INTEGER PRIMARY KEY AUTOINCREMENT` with position indexes); do not treat this approach as final until verified against existing data, relational constraints, and access patterns.
  - Consolidate all raw SQL queries from `PlaybackManager` and `LibraryManager` into methods on `UserData`.
  - Coordinate queue removal: `PlaybackManager` snapshots and handles the active playback engine before an active queue is removed, and `QueueSession` destroys each `Queue` object exactly once.
  - Apply `sqlite3_busy_timeout` on all database connections.
- **Checkpoint:** Run automated database migration and persistence tests on clean and legacy databases; verify duplicate track insertion, reordering, and restoration.

---

### Stage 5: API Cleanup & Type Consolidation
- **Objective:** Eliminate redundant data structures and clean the public facade.
- **Scope:**
  - Evaluate consolidating `CoreTrack` with `domain::Track` (e.g. via type alias `using CoreTrack = Track;`) while verifying source and binary compatibility for `natsuyume_bridge.cpp` and all public consumers.
  - Remove deep copy translation loops in `natsuyumecore.cpp`.
  - Streamline `NatsuyumeCore` public methods.
- **Checkpoint:** Full regression suite across desktop CLI and compilation verification of Android JNI bridge (`natsuyume_bridge.cpp`).

---

## 6. Agent Workflow and Safety Rules

All AI coding agents must operate strictly within the following rules:

### 6.1 The 13 Core Agent Rules
1. **Read AGENTS.md First:** Read this document before proposing or executing any modifications to the repository.
2. **Inspect Relevant Code Before Proposing Changes:** Never propose architectural changes without first reading the actual source files and verifying existing behaviors.
3. **Check Git Status and Diffs:** Always inspect `git status` and existing diffs before making edits. Never overwrite or discard unrelated user changes.
4. **Avoid Unrelated Refactoring:** Keep changes tightly scoped to the specific requested task. Do not reformat, reorganize, or rename unrelated files.
5. **Explain Architectural Modifications Upfront:** Always explain the rationale, affected files, and potential risks before modifying architecture or interfaces.
6. **Make Small, Logically Grouped Changes:** Commit or propose changes in small, cohesive increments that can be reviewed independently.
7. **Build and Test Affected Components:** Whenever build tools and dependencies are available, compile and test the modified components immediately.
8. **Report Test Results Accurately:** Clearly state what was compiled, what was executed, what passed, and what could not be tested.
9. **Never Claim False Verification:** Never state or imply that a build, test, or run succeeded unless you executed the actual command and verified its output.
10. **Preserve Android Compatibility:** Unless explicitly instructed otherwise, ensure that all changes preserve compatibility with the Android build and C-FFI bridge.
11. **Do Not Delete Functionality Hastily:** Never delete or replace existing functions, classes, or callbacks simply because they appear redundant without first auditing all call sites and consumers.
12. **Document Decisions and Migrations:** Document all architectural decisions, database schema changes, and migration requirements in commit messages and documentation.
13. **Stop and Request Clarification:** When requirements are ambiguous, conflict with existing code, or lack necessary context, stop and ask the user for clarification.

---

### 6.2 Strict Separation of Workflow Phases
Agents must maintain a strict operational boundary between phases:

```
[ Phase 1: Inspection ] ──► [ Phase 2: Planning ] ──► [ User Approval ]
                                                              │
                                                              ▼
[ Phase 5: Runtime Verify ] ◄── [ Phase 4: Build Verify ] ◄── [ Phase 3: Implementation ]
```

1. **Inspection Phase:** Read files, check git history, analyze build scripts, and verify tool availability. **No files are created or modified.**
2. **Planning Phase:** Produce an explicit specification detailing exact files to edit, lines affected, APIs introduced, and tests planned.
3. **Approval Gate:** **Agents must NEVER silently transition from planning to implementation.** Wait for explicit user authorization before writing or editing code.
4. **Implementation Phase:** Apply the approved changes cleanly and minimally.
5. **Build Verification Phase:** Run the build system (e.g. `cmake --build`) and inspect compiler output and return codes.
6. **Runtime Verification Phase:** Run test harnesses, CLI commands, or validation scripts to observe behavior.

---

### 6.3 Standard Agent Execution Workflow (Handoff Protocol)
To ensure seamless collaboration across multi-turn agent sessions, every agent must follow this exact 9-step execution workflow:

1. **Read Project Instructions:** Read `AGENTS.md` and `CLAUDE.md` before inspecting, planning, or modifying code.
2. **Inspect Actual Repository State:** Always verify the active branch, working tree state, and file contents directly rather than relying on previous agent reports or conversation summaries.
3. **Identify Existing APIs & Call Sites:** Inspect existing classes, method signatures, and usages before proposing replacements or new interfaces.
4. **Present Implementation Plan First:** Present a concise, structured implementation plan and obtain explicit user approval before modifying files.
5. **Keep Changes Strictly Within Active Stage:** Never implement features, fixes, or refactoring reserved for subsequent roadmap stages.
6. **Build and Test Incrementally:** Run compiler builds and tests after each meaningful implementation batch rather than deferring verification to the end.
7. **No Silent Architectural Changes:** Never alter data ownership, threading models, or database schemas without explicit documentation and prior approval.
8. **Distinguish Verified from Untested Behavior:** Accurately report what was compiled, linked, and executed; explicitly list unverified assumptions and untested areas.
9. **Deliver Structured Completion Reports:** Report exact modified files, commands executed, compiler/runtime results, Git status, and outstanding issues.

---

## 7. Current Development Status

- **Current Repository State:**
  - Branch: `main` (synchronized with `origin/main`).
  - Active Commit: `b7b4c71` ("Build core natively on Linux").
  - Working Tree: Clean (`nothing to commit, working tree clean`).
  - Native Desktop Build Artifacts: Verified shared library present at `build-desktop/libnatsuyume_core.so` (2.4 MB).
- **Completed Stages:**
  - **Stage 0: Desktop Build Foundation** (Completed, verified, and committed in `b7b4c71`).
- **Current Phase:** Pre-Stage-1 Planning & Authorization.
- **Authorization State:** **Stage 1 is NOT yet authorized.** All source code, build scripts, database schemas, and configuration files must remain unmodified.
- **Next Step:** Explicit user authorization and execution of **Stage 1: CLI Integration & Qt Decoupling**.
