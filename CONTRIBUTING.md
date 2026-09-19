# Contributing to turnip-kgsl-shim

Thanks for your interest. Please read this before opening an issue or pull request.

## What this project is
turnip-kgsl-shim is a personal project, built on and for the **AYN Thor** (Android, Snapdragon 8 Gen 2) and maintained by one
person in their spare time. It may work on other Android devices, but the Thor is the reference device and the only
one it's tested on. There's no guaranteed response time, and not every contribution will be accepted.

## Contributions that are welcome
- **Bug reports** with details: device, Android version, what you did, what you expected, what happened, logs.
- **Device reports** — "it works / doesn't work on my device" with the details above. These are genuinely useful.
- **Small, focused pull requests** that fix a bug or make a clearly-scoped improvement.

## Before you open a pull request
- For anything beyond a small fix, **open an issue first** and describe what you want to change and why.
- **One change per PR.** Keep it small and readable; explain what it does and why.
- **Say how you tested it:** device, Android version, the steps you ran, and what you saw.
- Include `vulkaninfo --summary` from your device and the exact build/run steps. Changes that touch the GPU path must say what you did to avoid crashes or overheating (this code can crash-loop the GPU process if misused).
- Keep the existing Thor setup working, follow the repo's conventions (see `CLAUDE.md` / `README.md`), and don't
  add dependencies without discussing it first.

## AI-assisted contributions
Using AI tools is fine — this project is built with them. But **you're responsible for what you submit**: read it,
understand it, run it, and test it on a real device before opening a PR, and say what you verified. Untested or
unreviewed generated code, large rewrites, restyles, and "feature dumps" will likely be closed without a detailed
review.

## Be kind
Be respectful and patient. Clear, careful reports and small tested changes are the best way to help.

## License
By contributing, you agree that your contributions are licensed under the project's MIT License.
