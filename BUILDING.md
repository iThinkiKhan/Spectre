# Spectre Build Notes

## Windows Locked-File Workaround

This project uses a project-local PlatformIO build folder to avoid intermittent
Windows file-lock errors from external processes:

- `build_dir = .pio_build` in `platformio.ini`

If you still see a lock error:

1. Close any serial monitors or tools that may be watching object/map files.
2. Run `platformio run -t clean`.
3. Run `platformio run`.

This workflow does not delete source files; it only clears build artifacts.
