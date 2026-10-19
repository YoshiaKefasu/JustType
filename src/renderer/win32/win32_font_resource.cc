// Copyright 2026 The Mozkey Authors
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
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED.

#include "renderer/win32/win32_font_resource.h"

#include <windows.h>

#include <string>
#include <vector>

namespace mozc {
namespace renderer {
namespace win32 {
namespace {

// EnumFontFamiliesExW hands back the family name of every face that matches
// the LOGFONT it is given, so one flag is enough to answer "is this family
// drawable by this process".
struct FamilyProbe {
  bool found = false;
  std::wstring wanted;
};

int CALLBACK EnumerateFamily(const LOGFONTW* logfont, const TEXTMETRICW*,
                             DWORD /*font_type*/, LPARAM param) {
  auto* probe = reinterpret_cast<FamilyProbe*>(param);
  if (logfont != nullptr && logfont->lfFaceName == probe->wanted) {
    probe->found = true;
  }
  // One hit is all the caller asked for.
  return probe->found ? 0 : 1;
}

}  // namespace

std::wstring BundledFontsDirectory(const std::wstring& server_directory) {
  std::wstring directory = server_directory;
  if (!directory.empty() && directory.back() != L'\\' &&
      directory.back() != L'/') {
    directory += L'\\';
  }
  directory += kBundledFontsSubDirectory;
  return directory;
}

std::vector<std::wstring> BundledFontFileNames() {
  return {kGenInterfaceJpDisplayRegular, kGenInterfaceJpDisplayBold};
}

int RegisterBundledFonts(const std::wstring& fonts_directory) {
  int added = 0;
  for (const std::wstring& file_name : BundledFontFileNames()) {
    const std::wstring path = fonts_directory + L'\\' + file_name;
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
      continue;
    }
    // FR_PRIVATE keeps the faces out of the system font table: they live and
    // die with this process, so an uninstall leaves nothing behind.
    if (::AddFontResourceExW(path.c_str(), FR_PRIVATE, nullptr) > 0) {
      ++added;
    }
  }
  return added;
}

bool IsFontFamilyAvailable(const std::wstring& family) {
  if (family.empty()) return false;

  FamilyProbe probe;
  probe.wanted = family;

  LOGFONTW logfont = {};
  logfont.lfCharSet = DEFAULT_CHARSET;
  logfont.lfFaceName[0] = L'\0';

  HDC screen_dc = ::GetDC(nullptr);
  if (screen_dc == nullptr) return false;
  ::EnumFontFamiliesExW(screen_dc, &logfont, EnumerateFamily,
                        reinterpret_cast<LPARAM>(&probe), 0);
  ::ReleaseDC(nullptr, screen_dc);
  return probe.found;
}

}  // namespace win32
}  // namespace renderer
}  // namespace mozc
