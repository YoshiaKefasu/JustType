// Copyright 2026 Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer in
// the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "session/meltype_bridge.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "base/system_util.h"
#include "meltype/scored_segmenter.h"

namespace mozc {
namespace session {

namespace {

// Environment override.  Lets a developer point at a dictionary directory
// without reinstalling, and lets tests run against a small fixture.
constexpr char kDictionaryEnvVar[] = "MOZKEY_MELTYPE_DICT_DIR";

bool HasEnglishList(const std::string& directory) {
  if (directory.empty()) return false;
  std::ifstream stream(directory + "/english.txt", std::ios::binary);
  return stream.good();
}

// Where the bundled dictionaries live.
//
// 1. MOZKEY_MELTYPE_DICT_DIR, when set.
// 2. The source tree, relative to this file.  This is the path taken by a
//    Bazel test, whose runfiles mirror the package layout.
// 3. Installed next to the server binary, so a shipped MSI keeps working
//    without the source tree.
std::string ResolveDictionaryDirectory() {
  if (const char* from_env = std::getenv(kDictionaryEnvVar); from_env && *from_env) {
    return from_env;
  }

  std::error_code error;
  const std::filesystem::path here =
      std::filesystem::absolute(std::filesystem::path(__FILE__), error);
  if (!error) {
    for (const std::filesystem::path& candidate :
         {here.parent_path() / "dictionaries",
          here.parent_path().parent_path() / "dictionaries"}) {
      if (HasEnglishList(candidate.string())) return candidate.string();
    }
  }

  // GetServerDirectory() may or may not end with a separator, so normalize
  // before appending.
  std::string server_dir = mozc::SystemUtil::GetServerDirectory();
  if (!server_dir.empty() && server_dir.back() != '\\' && server_dir.back() != '/') {
    server_dir += '\\';
  }
  const std::string install_dir = server_dir + "meltype_dictionaries";
  if (HasEnglishList(install_dir)) return install_dir;

  return std::string();
}

}  // namespace

std::once_flag MeltypeBridge::init_flag_;
std::unique_ptr<mozkey::meltype::MeltypeDictionaries> MeltypeBridge::dictionaries_;
std::unique_ptr<mozkey::meltype::ScoredSegmenter> MeltypeBridge::segmenter_;

namespace {
// Guards the single background loader thread started by PreloadAsync().
std::mutex g_preload_mutex;
std::thread g_preload_thread;
}  // namespace

void MeltypeBridge::PreloadAsync() {
  std::lock_guard<std::mutex> lock(g_preload_mutex);
  if (g_preload_thread.joinable()) {
    return;
  }
  g_preload_thread = std::thread([] {
    (void)GetSegmenter();  // Triggers the std::call_once dictionary load.
  });
}

void MeltypeBridge::WaitForPreload() {
  std::thread loader;
  {
    std::lock_guard<std::mutex> lock(g_preload_mutex);
    loader = std::move(g_preload_thread);
  }
  if (loader.joinable()) {
    loader.join();
  }
}

const mozkey::meltype::ScoredSegmenter* MeltypeBridge::GetSegmenter() {
  std::call_once(init_flag_, [] {
    const std::string directory = ResolveDictionaryDirectory();
    if (directory.empty()) return;

    auto dictionaries = std::make_unique<mozkey::meltype::MeltypeDictionaries>();
    if (mozkey::meltype::MeltypeDictionaries::LoadFromDirectory(directory,
                                                               *dictionaries) == 0) {
      return;
    }
    dictionaries_ = std::move(dictionaries);
    segmenter_ = std::make_unique<mozkey::meltype::ScoredSegmenter>(*dictionaries_);
  });
  return segmenter_.get();
}

std::vector<mozkey::meltype::ScoredSegmenter::Segment> MeltypeBridge::SegmentRomaji(
    const std::string& romaji, bool final) {
  const mozkey::meltype::ScoredSegmenter* segmenter = GetSegmenter();
  if (segmenter == nullptr) return {};

  mozkey::meltype::ScoredSegmenter::Options options;
  options.final = final;
  return segmenter->SegmentInput(romaji, options);
}

}  // namespace session
}  // namespace mozc
