# voice-console

An inkcell TUI that ties the whole voice pipeline together in one place:

```
wake word → VAD → STT (faster-whisper) → ephemeral harness → TTS (kokoro)
```

## Run it (no-code)

```bash
./run.sh                        # default input mic
VOICE_DEVICE=22 ./run.sh        # USB-C mic (device 22 on this machine)
```

That one command:
1. builds this app,
2. starts `playground/local-transcription/voice.py` as the voice backend
   (writes state to `/tmp/voice_console_state.json`),
3. launches the interactive TUI.

Then just **say "alexa"** (or "hey jarvis" / "hey mycroft" / "hey rhasspy"),
pause, and ask a question. Watch the pipeline panels light up.

You can also drive the harness by hand: type a prompt in the bottom box,
**Enter** to run it, **Esc** to clear, **q** / **Ctrl-C** to quit.

## Panels

| Panel | Shows |
|-------|-------|
| Header | current pipeline stage (wake/listen/stt/llm/tts) |
| Latency line | per-stage ms for the last utterance |
| Conversation | your transcript + the agent's spoken response |
| Harness blocks | the live `[thought]`/`[response]`/`[action]`/`[result]` stream from `cortex-mk3 --ephemeral --no-ansi` |
| Prompt | type a question → runs a headless harness turn on demand |

## Layout / structure

```
examples/voice-console/
├── run.sh            # one-command launch (build + backend + TUI)
├── Makefile          # make / make live / make snapshot
├── README.md
└── src/
    ├── main.cpp      # inkcell App wiring, --live / --snapshot
    ├── model.hpp     # VoiceState (JSON parse) + HarnessRun (fork/exec subprocess)
    └── scene.hpp     # the TUI scene (panels, prompt input, state polling)
```

## How the pieces connect

- **voice backend** (`playground/local-transcription/voice.py`) runs the audio
  loop and writes a tiny JSON state file (stage, per-stage latency, transcript,
  response) — the TUI polls it every ~200ms.
- **headless harness** (`cortex-mk3 -m .../voice/agent.yml run -p "<q>"
  --ephemeral --no-ansi`) is spawned per prompt via `fork`/`exec` (no shell —
  injection-safe) and its rendered blocks stream into the right pane.

## Build

```bash
make            # build
make snapshot   # render one frame (CI-friendly, never hangs)
make live       # interactive TUI
```
Requires `inkcell` (`../../../inkcell`) built (`make -C ../../../inkcell lib`).
