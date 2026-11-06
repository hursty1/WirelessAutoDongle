# Building Wireless Android Auto Dongle

This repository contains a docker setup to make the build process easy.

If you choose to build without Docker, refer [the Buildroot user manual
](https://buildroot.org/downloads/manual/manual.html) for more details on dependencies and setup.

## Clone
```shell
$ git clone --recurse-submodules https://github.com/hursty1/WirelessAutoDongle
$ cd WirelessAutoDongle
```

## Build with Docker
```shell
$ docker compose run --rm rpi4 # See docker-compose.yml for available options.
```

You can use `rpi0w`, `rpi02w`, `rpi3a` or `rpi4` to build and generate an sdcard image. Once the build is successful, it'll copy the generated sdcard image in `images/` directory.

You can also use the `bash` service for more control over the build process and experimentation.

```shell
$ docker compose run --rm bash
```

Docker synchronizes the working tree into a Linux `source` volume before each
run. It restores the symlinks and executable permissions recorded by Git, so
Windows checkouts with `core.symlinks=false` work too. This includes Buildroot's
GCC and Linux header hash symlinks and the root filesystem overlay links.
Source edits, including uncommitted edits, are copied on the next run; Git
metadata stays on the host. Edit sources in the host checkout: changes made
only inside `/app` are replaced by the next synchronization. Run builds one at
a time because the services share the source and output volumes.

The existing `dl` and `output` volumes retain downloads and incremental build
artifacts. The generated image is copied to the host's `images/` directory.
Do not use `docker compose down -v` unless you intend to discard these caches.
After updating the Dockerfile, run `docker compose build` once before building.

If GCC reports `No hash found` even though its checksum is in
`buildroot/package/gcc/gcc.hash`, check whether
`package/gcc/gcc-initial/gcc-initial.hash` is a plain file containing
`../gcc.hash`. It must be a symlink in the Linux build tree. Adding duplicate
hashes or disabling `BR2_DOWNLOAD_FORCE_CHECK_HASHES` does not fix the checkout.

## Build with manual setup
Once you have a recursive clone, you can manually build using the following set of commands.

```shell
$ cd buildroot
$ make BR2_EXTERNAL=../aa_wireless_dongle/ O=output/rpi0w raspberrypi0w_defconfig # Change output and defconfig for your board
$ cd output/rpi0w
$ make
```

When successful, this should generate the sd card image at `images/sdcard.img` in your output directory. See the "Install and Run" instructions above to use this image.

Use one of the following defconfig for the board you intend to use:
- `raspberrypi0w_defconfig` - Raspberry Pi Zero W
- `raspberrypizero2w_defconfig` - Raspberry Pi Zero 2 W
- `raspberrypi3a_defconfig` - Raspberry Pi 3A+
- `raspberrypi4_defconfig` - Raspberry Pi 4
