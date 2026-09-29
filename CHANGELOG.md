# Changelog

## 2.0.0 (2026-09-29)

- Renamed to **`sinowealth-mouse`**: a generic macOS utility for Sinowealth gaming mice.
  `nos-m700` remains as a symlink to the same binary.
- Split the code into a generic transport/protocol layer (`src/sinowealth.c`), a device table
  with support levels (`src/devices.c`) and the CLI (`src/main.c`).
- Support levels: CONFIRMED / PROTOCOL_MATCH / EXPERIMENTAL / UNKNOWN / UNSUPPORTED.
  Matching uses PID and firmware; the NOS M-700 (258a:0029, firmware "2616") is the only
  CONFIRMED device.
- Writes are allowed only on CONFIRMED devices unless `--experimental` is given; UNSUPPORTED
  devices never receive vendor commands; unknown PIDs are never auto-selected.
- New: `version` / `--version`, `devices`, `--pid` auto-detection, `--experimental`.
- Safety: full-config backup before every write (`~/.sinowealth-mouse/backup-*.bin`),
  config length/header checks, write marker = length − 8, read-back verification for
  `poke` and `restore` too.
- LED state moved to `~/.sinowealth-mouse/state-<vid>-<pid>.bin`; the v1
  `~/.nos-m700-state.bin` is still read for 258a:0029.
- `make install` installs `sinowealth-mouse` + `nos-m700` symlink; `make uninstall` added.

## 1.0.0

- `nos-m700`: turn the NOS M-700 scroll-wheel / DPI-indicator LED off and on.
