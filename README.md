# Kagami

Kagami is a new Android metamodule skeleton. The project keeps the old web UI lineage, but the native side is being rewritten in C++ with a Kasumi-first mount pipeline.

Backend priority:

1. Kasumi
2. OverlayFS fallback
3. Magic Mount fallback

The current tree intentionally contains only the project skeleton:

- `webui/`: migrated React/Vite WebUI, renamed to Kagami/Kasumi paths and commands.
- `src/`: thin C++ command/API skeleton for the future daemon.
- `module/`: KernelSU/APatch metamodule packaging files.
- `zygisk/`: optional Zygisk module scaffold based on `5ec1cff/zygisk-module-template`.
- `CMakeLists.txt`: native binary, WebUI, and package targets.

Build locally:

```sh
cmake -S . -B build
cmake --build build --target kagamid
```

Build the WebUI and package:

```sh
cmake --build build --target package
```

Build the optional Zygisk bridge:

```sh
cd zygisk
python3 build.py build -a arm64-v8a
```
