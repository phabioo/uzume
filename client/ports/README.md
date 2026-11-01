# vcpkg overlay ports

Ports here replace the port of the same name from the pinned vcpkg baseline (`client/vcpkg-configuration.json`).
Each one is a verbatim copy of a newer upstream port that fixes a build break at our baseline. Delete it as soon as
the baseline in `client/vcpkg.json` contains the same or a newer port version.

- `amd-amf` 1.5.3#1: AMD replaced the `AMF-headers-v1.5.3.tar.gz` release asset, so the SHA512 in the 2026.10.16 port no
  longer matches. Copied from microsoft/vcpkg `master` (port-version 1 carries the new hash).
