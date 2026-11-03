# Security policy

## Reporting a vulnerability

Please **do not** open a public issue for security problems. Use GitHub's [private vulnerability reporting](https://github.com/notyurigagarin/emuhall-player/security/advisories/new) for this repository instead.

Include what you found, how to reproduce it with synthetic content, and the impact you expect. Never include game files, BIOS dumps or credentials. We will acknowledge your report, keep you informed, and credit you in the fix unless you prefer otherwise.

## Scope

The player runs one engine per process so that a crashing or hanging engine cannot take EmuHall down with it. **This is crash isolation, not a security sandbox.** Engines run with the same rights as the player, so only load engines you trust. Reports about what a malicious engine can do are expected behaviour, not vulnerabilities.

In scope:

- the player's handling of game files, save files, save states and its launcher protocol;
- netplay and the relay protocol;
- handling of RetroAchievements tokens.

## Supported versions

Security fixes are made on the latest release only.
