# sysLens

htop/btop-style terminal monitor (processes + ports, realtime) with Gemma-powered diagnostics.

## Demo video link:
[asciinema](https://asciinema.org/a/AzLbCVAciZnI676l)

## Build (Arch)

```sh
git clone https://github.com/73LIX/sysLens.git
cd sysLens
sudo pacman -S --needed base-devel cmake curl
cmake -S . -B build && cmake --build build -j
```

## AI backend

**Gemma through the API (default):** create a key in Google AI Studio, then

```sh
export GEMINI_API_KEY=<key>            # add to ~/.zshrc or ~/.bashrc
export SYSLENS_MODEL=gemma-4-26b-a4b-it   # optional; default is gemma-3n-e2b-it (~2B effective)
```

**Local fallback (llama.cpp):** if the API fails or no key is set, sysLens talks to a local
`llama-server` (`http://127.0.0.1:8080`).

```sh
yay -S llama.cpp                                   # or build from source
# download a small Q4 GGUF of Gemma (e.g. gemma-3-1b-it-Q4_K_M.gguf), then either:
llama-server -m ~/models/gemma-3-1b-it-Q4_K_M.gguf --port 8080
# or let sysLens start it on demand:
export SYSLENS_GGUF=~/models/gemma-3-1b-it-Q4_K_M.gguf
```

Force a backend with `--backend api|local` or `SYSLENS_BACKEND`.

## Usage

```sh
sysLens                              # live TUI
sysLens --process 1108 --analyze     # what is this process and what is it doing
sysLens --port 3000 --analyze        # which app owns the port and why

cargo build 2>&1 | sysLens --errors  # explain piped output
sysLens run -- cmake --build build   # run a command; if it fails, triage + explain
sysLens --run "npm install"          # same, as one quoted string
sysLens --errors build.log --no-ai   # classification only, no model call
```

Errors are classified locally into CRITICAL / ERROR / WARNING (deterministic, works offline);
the model then explains the root cause and gives numbered next steps.

### TUI keys

| Key | Action |
| --- | --- |
| Tab | switch Processes / Ports pane |
| ↑ ↓ / j k, PgUp PgDn | move selection |
| a / Enter | AI-analyze the selected process or port |
| c / m | sort by CPU / memory |
| e | include established sockets (default: listening only) |
| q / Esc | quit (Esc closes the AI popup first) |

Run with `sudo` to see the owning process of other users' sockets.

### Tip: capture the last failed command automatically

```sh
# ~/.bashrc / ~/.zshrc
sl() { sysLens run -- "$@"; }       # then:  sl cargo build
```
