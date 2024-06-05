use_relative_paths = True

vars = {
  "checkout_chromium": False,
}

deps = {
  "third_party/externals/expat"           : "https://android.googlesource.com/platform/external/expat.git@e5aa0a2cb0a5f759ef31c0819dc67d9b14246a4a",
  "third_party/externals/freetype"        : "https://skia.googlesource.com/third_party/freetype2.git@0a3d2bb99b45b72e1d45185ab054efa993d97210",
  "third_party/externals/harfbuzz"        : "https://chromium.googlesource.com/external/github.com/harfbuzz/harfbuzz.git@3a74ee528255cc027d84b204a87b5c25e47bff79",
  "third_party/externals/icu"             : "https://chromium.googlesource.com/chromium/deps/icu.git@a0718d4f121727e30b8d52c7a189ebf5ab52421f",
  "third_party/externals/zlib"            : "https://chromium.googlesource.com/chromium/src/third_party/zlib@47af7c547f8551bd25424e56354a2ae1e9062859",

  "../src": {
    "url": "https://chromium.googlesource.com/chromium/src.git@c0b367bf9cab0b8a8a05ac0e39f88407a0c290c9",
    "condition": "checkout_chromium",
  },
}

recursedeps = [
  "common",
  "../src",
]

gclient_gn_args_from = 'src'
