use_relative_paths = True

vars = {
  "checkout_chromium": False,
}

deps = {
  "third_party/externals/expat"           : "https://chromium.googlesource.com/external/github.com/libexpat/libexpat.git@624da0f593bb8d7e146b9f42b06d8e6c80d032a3",
  "third_party/externals/freetype"        : "https://skia.googlesource.com/third_party/freetype2.git@0a3d2bb99b45b72e1d45185ab054efa993d97210",
  "third_party/externals/harfbuzz"        : "https://chromium.googlesource.com/external/github.com/harfbuzz/harfbuzz.git@3a74ee528255cc027d84b204a87b5c25e47bff79",
  "third_party/externals/icu"             : "https://chromium.googlesource.com/chromium/deps/icu.git@a0718d4f121727e30b8d52c7a189ebf5ab52421f",
  "third_party/externals/libgifcodec"     : "https://skia.googlesource.com/libgifcodec@d06d2a6d42baf6c0c91cacc28df2542a911d05fe",
  "third_party/externals/libjpeg-turbo"   : "https://chromium.googlesource.com/chromium/deps/libjpeg_turbo.git@ccfbe1c82a3b6dbe8647ceb36a3f9ee711fba3cf",
  "third_party/externals/libpng"          : "https://skia.googlesource.com/third_party/libpng.git@ed217e3e601d8e462f7fd1e04bed43ac42212429",
  "third_party/externals/libwebp"         : "https://chromium.googlesource.com/webm/libwebp.git@845d5476a866141ba35ac133f856fa62f0b7445f",
  "third_party/externals/zlib"            : "https://chromium.googlesource.com/chromium/src/third_party/zlib@47af7c547f8551bd25424e56354a2ae1e9062859",

  "../src": {
    "url": "https://chromium.googlesource.com/chromium/src.git@a6a8a49a207789813f4b57bd8ec5a006037c1f76",
    "condition": "checkout_chromium",
  },
}

recursedeps = [
  "common",
  "../src",
]

gclient_gn_args_from = 'src'
