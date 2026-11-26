# Containerised firmware build

An Ubuntu 24.04 image with everything a firmware build needs: the PebbleOS
SDK (arm-none-eabi GCC 15.2, picolibc, QEMU, sftool) under `/opt`, and a
Python venv at `/opt/venv` with `requirements.txt` installed. It builds on
linux/arm64 and linux/amd64; the SDK bundle is picked by `uname -m` and
checked against its release checksum.

## Usage

From the repository root:

```shell
BOARD=asterix docker/build.sh -DCONFIG_RELEASE=y   # pbl configure + pbl build
BOARD=stm32h747i_disco/m7 docker/build.sh samples/hello_world
docker/build.sh -- pbl bundle                      # any command in the container
docker/build.sh -- bash                            # a shell
```

Extra arguments go to `pbl configure` (`-D...` options, an application
directory, `--variant prf`...). `pbl configure --help` lists the boards.
Outputs land in `build/` as usual (`pebbleos.elf/.bin/.hex`,
`log.dict.json`, and with `pbl bundle` the `.pbz`).

The script builds the image (`unapebble-build:latest`, override with
`IMAGE=`) when it is missing; `REBUILD=1` rebuilds it, which is needed after
changing `requirements.txt` or the SDK version. It uses `sudo docker` when
plain `docker` is not permitted, and puts the containers in
`--cgroup-parent=batch.slice`.

## Notes

- The container runs as the invoking UID/GID, so `build/` stays yours. HOME
  is `/tmp` inside it; ccache keeps its cache in `build/.ccache`.
- The checkout is mounted at `/work` with `--security-opt label=disable`, so
  SELinux (Fedora) allows the access without relabelling the tree.
- `pbl`, `pbl-console` and `pebble-loghash` are editable installs pointing at
  `/work/tools/libs/...`: the copies in the mounted checkout are the ones run.
- Not included: Rust/cargo/bindgen (only Rust applications built with
  `pbl_rust_app()` need them, not the firmware), the emulator UI libraries
  (GTK, SDL), `gcc-multilib` and OpenOCD. Flash from the host.
- The SDK version is the `SDK_VERSION` build argument; a new one needs its
  `SDK_SHA256_aarch64`/`SDK_SHA256_x86_64` from the release's `.sha256` files.
