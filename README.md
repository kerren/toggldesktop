### ⚠️ Project Status: Linux revival

Upstream stopped developing this repository — the desktop clients were rewritten as
separate native apps, and this codebase was left behind. **The goal of this fork is to
get Toggl Desktop building and running on Linux again**, on a current distro, against
the current Toggl API.

Everything here is aimed at the Linux (Qt) client and the shared C++ core it sits on.
The macOS and Windows front-ends are out of scope for this fork.

<h1></h1>

<h1 align="center">
  <a href="https://toggl.com"><img src="https://raw.githubusercontent.com/toggl-open-source/toggldesktop/gh-pages/assets/toggl-track-wide.png" alt="Toggl Track"></a>
</h1>

<h4 align="center">Native Linux desktop client for the time tracking tool <a href="https://toggl.com" target="_blank">Toggl</a>.</h4>

<p align="center">
    <a href="https://github.com/toggl-open-source/toggldesktop/commits/master">
    <img src="https://img.shields.io/github/last-commit/toggl-open-source/toggldesktop.svg?style=flat&logo=github&logoColor=white"
         alt="GitHub last commit">
    <a href="https://github.com/toggl/toggldesktop/issues">
    <img src="https://img.shields.io/github/issues-raw/toggl-open-source/toggldesktop.svg?style=flat&logo=github&logoColor=white"
         alt="GitHub issues">
    <a href="https://github.com/toggl/toggldesktop/pulls">
    <img src="https://img.shields.io/github/issues-pr-raw/toggl-open-source/toggldesktop.svg?style=flat&logo=github&logoColor=white"
         alt="GitHub pull requests">
    <img src="https://img.shields.io/badge/licence-BSD--3-green"
         alt="Licence BSD-3">
</p>

<p align="center">
  <a href="#about">About</a> •
  <a href="#goal">Goal</a> •
  <a href="#build-on-linux">Build on Linux</a> •
  <a href="#change-log">Change log</a> •
  <a href="#contribute">Contribute</a>
</p>

# About

  **Toggl Desktop** is a Toggl time tracking client with many helper functions that make tracking time more effortless and smooth. Features such as Idle detection, reminders to track and Pomodoro Timer make this app a great companion when productivity and efficiency is the goal.

The Linux client is a Qt 5 application (`src/ui/linux`) on top of a shared C++ core
library (`TogglDesktopLibrary`) that handles syncing, storage and the Toggl API.

# Goal

Make this build, run and sync on Linux again:

1. **Build from source on a current distro** — the CMake build and the Qt 5 GUI target
   compile without patching.
2. **Talk to the live API** — the old Toggl API v8 was shut down in 2024, so everything
   the app does has to go through `/api/v9`. See [`plan.md`](plan.md) for the migration
   status and remaining work.
3. **Keep the core green** — `TogglDesktopLibrary` and `TogglAppTest` build and all unit
   tests pass. This is what CI gates on (`.github/workflows/linux-core.yml`).

# Build on Linux

_By default the app builds against the testing server. To use the compiled app with the
live server see [this guide](https://github.com/toggl-open-source/toggldesktop/wiki/Building-Toggl-Desktop-from-source-for-usage-with-live-servers)._

### Dependencies

You'll need Qt 5 (5.12 or higher) — in particular **qtbase**, including its private
headers — plus QtNetwork, QtNetworkAuth, QtDBus and QtX11Extras.

On Debian/Ubuntu, the two packages that are easy to miss — and that CMake fails on
first — are **qtbase** and **QtNetworkAuth**:

```bash
$ sudo apt install qtbase5-dev
$ sudo apt install libqt5networkauth5-dev
```

Then the rest of the Qt modules and the mandatory system libraries:

```bash
$ sudo apt install qtbase5-private-dev libqt5x11extras5-dev \
    libxss-dev libxmu-dev libssl-dev build-essential cmake pkg-config \
    libgl-dev libreadline-dev
```

`libXScrnSaver` (`libxss-dev` on deb-based distros, `libXScrnSaver-devel` on rpm-based)
is required for idle detection.

If Qt is not installed from your distribution's package manager, set the
`CMAKE_PREFIX_PATH` environment variable to point at the `lib/cmake` folder of the Qt
version you want to use.

These dependencies are optional and will be bundled if the `USE_BUNDLED_LIBRARIES` CMake
argument is set, or if your system does NOT have their development packages installed:
 * POCO (`libpoco-dev`)
 * Lua
 * jsoncpp (`libjsoncpp-dev`)
 * Qxt

These libraries are bundled regardless of your system:
 * bugsnag-qt
 * qt-oauth-lib

### Build the app

*in the toggldesktop source tree root*
```bash
mkdir -p build && pushd build             # Create build directory
cmake ..                                  # Setup cmake configs
make -j8                                  # Build the app. The number defines the count of parallel jobs (number of your CPU cores is a good value for that)
./src/ui/linux/TogglDesktop/TogglDesktop  # Run the built app
```

### Build just the core and its tests

Useful for headless work on the library — this skips the GUI:

```bash
cmake -S . -B build -DTOGGL_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --target TogglDesktopLibrary TogglAppTest -j"$(nproc)"
cd build && ./src/test/TogglAppTest        # must run from build/, fixtures use ../testdata
```

# Change log

Change log can be viewed at [http://toggl.github.io/toggldesktop/](http://toggl.github.io/toggldesktop/)

# Contribute

Before sending us a pull request, please format the source code:

```bash
$ make fmt
```

Also, please check for any cpplint issues:

```bash
$ make lint
```

Check if unit tests continue to pass:

```bash
$ make test
```
