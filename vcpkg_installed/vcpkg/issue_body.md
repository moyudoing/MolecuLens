Package: openssl:x64-windows@3.5.0

**Host Environment**

- Host: x64-windows
- Compiler: MSVC 19.44.35207.1
-    vcpkg-tool version: 2025-04-07-b8b513ba8778c918cff49c3e837aae5999d5d2aa
    vcpkg-scripts version: 0bf1354d67 2025-04-10 (1 year, 4 months ago)

**To Reproduce**

`vcpkg install `

**Failure logs**

```
Downloading https://github.com/openssl/openssl/archive/openssl-3.5.0.tar.gz -> openssl-openssl-openssl-3.5.0.tar.gz
openssl-openssl-openssl-3.5.0.tar.gz.48028.part: error: download from https://github.com/openssl/openssl/archive/openssl-3.5.0.tar.gz had an unexpected hash
note: Expected: a1ef09ecc810b761b3adcdb79b5746ba06244a10f626a40e495a3df19411546b98d75f6c7e7c13de7c15753caf3db87af7096ed0bb835afed8cc6dbc366b542f
note: Actual  : 9c6e1c38f07bdb2a88c382e894ed030d786fa4ffc727899d1c002db5d7b025499a72ca1cfe806aed660c83d4ed0354c7d215e569935bd0a99a2eb827b20e33a6
CMake Error at scripts/cmake/vcpkg_download_distfile.cmake:136 (message):
  Download failed, halting portfile.
Call Stack (most recent call first):
  scripts/cmake/vcpkg_from_github.cmake:106 (vcpkg_download_distfile)
  buildtrees/versioning_/versions/openssl/7558a3501e44a455ac8fcbe14b622503eb9574ee/portfile.cmake:10 (vcpkg_from_github)
  scripts/ports.cmake:206 (include)



```

**Additional context**

<details><summary>vcpkg.json</summary>

```
{
  "name": "moleculens",
  "version-string": "0.1.0",
  "description": "MolecuLens - cross-platform molecular structure and trajectory display engine",
  "homepage": "https://github.com/your/MolecuLens",
  "builtin-baseline": "0bf1354d6704ec797acea686d0250afc4036a082",
  "dependencies": [
    "glfw3",
    "volk",
    "vulkan-headers",
    "vulkan-memory-allocator",
    "glm",
    {
      "name": "imgui",
      "features": [
        "docking-experimental"
      ]
    },
    "spdlog",
    "nlohmann-json",
    "pybind11",
    {
      "name": "cppzmq",
      "platform": "!windows"
    },
    "cli11",
    "catch2"
  ]
}

```
</details>
