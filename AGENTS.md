# Frankie's Karaoke Studio - Codex Repository Instructions

## Product

A simple MP3+CDG karaoke player for an elderly user. Reliability, simplicity
and predictable behaviour matter more than clever architecture.

- C++20, Qt 6 (Widgets), CMake, GStreamer 1.x.
- Development: macOS. Production target: Windows 11 x64.

## Workflow

Claude Code is the lead developer and orchestrator. Codex receives bounded
tasks from Claude. Claude reviews every diff and reruns the tests itself.

- Do only the task you were given. No unrelated refactors or renames.
- Never commit or push.
- End with a concise report: files changed, what changed, commands run and
  their real output, and any remaining risks.

## Hard constraints

- Build against the **official GStreamer SDK/framework**
  (`~/Library/Frameworks/GStreamer.framework` on macOS, the official MSVC SDK
  on Windows). Never Homebrew GStreamer: it lacks the SoundTouch `pitch`
  element. On this Mac, `pkg-config gstreamer-1.0` resolves to Homebrew.
- The audio playback position reported by GStreamer is the only clock for
  CDG lyrics. Lyrics must never run ahead of the audio.
- Never copy real karaoke media into the repository. Tests use synthetic media.
  Real media is only read in place, through `FKS_TEST_MEDIA_DIR`.
- Keep platform-specific code isolated and minimal.
- Do not implement Milestone 2 features: pitch/tempo, persistence, song
  library, playlists, autoplay, ZIP. Do not make them harder to add later.

## Build and test (macOS)

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=$HOME/Qt/6.11.2/macos
cmake --build build -j8
ctest --test-dir build --output-on-failure
```

The build must finish with zero compiler warnings.
