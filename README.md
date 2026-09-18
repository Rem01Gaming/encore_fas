# Encore FAS

A kernel module Frame Aware Scheduling (FAS) implementation for Android.

## What is Frame Aware Scheduling?

Frame Aware Scheduling improves game performance by monitoring the game's framerate in real time and using that information to optimize performance and efficiency.

If the game runs choppy, FAS automatically boosts the CPU and GPU to compensate for the low FPS. Otherwise, it relaxes the boost to save power.

## User API



## License

- Files under `kernel/`, and `.clang*` are licensed under [GPL-2.0-only](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html).
- All other files are licensed under [Apache-2.0](https://www.apache.org/licenses/LICENSE-2.0).
