# Quiet check targets (details: scripts/check.sh). Prerequisite for check-client:
# scripts/bootstrap-vcpkg.sh has been run once.
# HUB_VERSION / HUB_CHANNEL / HUB_COMMIT: ldflags of build-hub; HUB_VERSION is also the .deb version of package-hub-deb.
# APP_VERSION (required), APP_BUILD_DIR (default client/build/linux-release), APP_SERVER (default server/dist/uzume-server-linux-amd64),
# APP_OUT (default client/build/appimage): package-app-appimage builds the app AppImage from a finished release build
# (cmake --preset linux-release, build uzume_app and uzume_cli; make build-hub for the server binary).
.PHONY: check check-hub check-hub-race check-client build-hub package-hub-deb package-app-appimage generate notices fetch-core fetch-desmume fetch-deps fetch-sdl3

check:
	@scripts/check.sh all

check-hub:
	@scripts/check.sh hub

# Hub tests under the race detector (CGO_ENABLED=1, needs gcc); the Linux CI job runs it too.
check-hub-race:
	@scripts/check.sh hub-race

check-client:
	@scripts/check.sh client

build-hub:
	@scripts/check.sh hub-build

package-hub-deb:
	@scripts/check.sh hub-deb

package-app-appimage:
	@scripts/check.sh app-appimage

generate:
	@scripts/check.sh generate

# Regenerates server/THIRD-PARTY-NOTICES.txt (after go.mod/go.sum changes).
notices:
	@scripts/check.sh notices

# melonDS DS from the libretro nightly buildbot (ADR 0020 D10); prints the .so path.
fetch-core:
	@scripts/fetch-buildbot-core.sh melondsds

# DeSmuME from the libretro nightly buildbot (ADR 0020 D10); prints the .so path.
fetch-desmume:
	@scripts/fetch-buildbot-core.sh desmume

fetch-deps:
	@scripts/fetch-libdatachannel.sh

fetch-sdl3:
	@scripts/fetch-sdl3.sh
