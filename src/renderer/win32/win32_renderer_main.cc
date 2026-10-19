// Copyright 2010-2021, Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
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

// Mozc renderer process for win32.

#include "absl/log/log.h"
#include "base/system_util.h"
#include "base/win32/scoped_com.h"
#include "base/win32/wide_char.h"
#include "base/win32/winmain.h"
#include "renderer/init_mozc_renderer.h"
#include "renderer/win32/win32_font_resource.h"
#include "renderer/win32/win32_server.h"

int main(int argc, char* argv[]) {
  mozc::renderer::InitMozcRenderer(argv[0], &argc, &argv);

  // The candidate window and its ruby overlay draw with the family the user
  // picked in the config dialog, which is Gen Interface JP Display by default.
  // Registering the bundled faces here is what makes that setting work on a
  // machine where the user never installed the font; without it every face
  // lookup silently falls back to the platform font and the candidate window
  // renders in a different typeface from the one the dialog previews.
  //
  // FR_PRIVATE, so this is process-local: no system font table entry, nothing
  // left behind after an uninstall.  A missing fonts directory is not fatal —
  // the platform fallback below is exactly the old behaviour.
  const std::wstring fonts_directory =
      mozc::renderer::win32::BundledFontsDirectory(
          mozc::win32::Utf8ToWide(mozc::SystemUtil::GetServerDirectory()));
  const int registered = mozc::renderer::win32::RegisterBundledFonts(fonts_directory);
  if (registered > 0) {
    LOG(INFO) << "Registered " << registered
              << " bundled font face(s) from "
              << mozc::win32::WideToUtf8(fonts_directory);
  } else {
    LOG(WARNING) << "No bundled font faces registered from "
                 << mozc::win32::WideToUtf8(fonts_directory)
                 << "; falling back to the platform font";
  }

  mozc::ScopedCOMInitializer com_initializer;

  mozc::renderer::win32::Win32Server server;
  server.SetRendererInterface(&server);
  return server.StartServer();
}
