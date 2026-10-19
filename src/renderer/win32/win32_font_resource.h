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

#ifndef MOZC_RENDERER_WIN32_WIN32_FONT_RESOURCE_H_
#define MOZC_RENDERER_WIN32_WIN32_FONT_RESOURCE_H_

#include <string>
#include <vector>

namespace mozc {
namespace renderer {
namespace win32 {

// The Gen Interface JP Display faces ship inside the MSI under
// <MozcDir>\fonts, so the family the config dialog and the candidate window
// ask for by name resolves whether or not the user installed it system-wide.
//
// The registration is per process and private (FR_PRIVATE): nothing is written
// to the system font table, so there is nothing to clean up on uninstall and
// no reboot is needed.  The alternative — registering the faces for the whole
// machine — would leave font files behind in %WINDIR%\Fonts after a Mozc
// uninstall, which is exactly the kind of leftover a user cannot undo.

// Sub-directory of the Mozc install that holds the bundled faces.
constexpr wchar_t kBundledFontsSubDirectory[] = L"fonts";

// Face file names, relative to BundledFontsDirectory().  Regular and Bold are
// both needed: the candidate window weights its labels, and a weight GDI
// cannot find is synthesised by the rasteriser rather than drawn from the
// shipped outlines.
constexpr wchar_t kGenInterfaceJpDisplayRegular[] =
    L"GenInterfaceJPDisplay-Regular.ttf";
constexpr wchar_t kGenInterfaceJpDisplayBold[] =
    L"GenInterfaceJPDisplay-Bold.ttf";

// The directory the MSI installs the bundled faces into, derived from the
// directory the Mozc binaries live in.
std::wstring BundledFontsDirectory(const std::wstring& server_directory);

// Face file names of the bundled faces, relative to
// BundledFontsDirectory().
std::vector<std::wstring> BundledFontFileNames();

// Registers every bundled face into the calling process.  Returns how many
// faces were added; 0 when the directory or a file is missing, which is a
// condition the caller is expected to survive (the renderer falls back to the
// platform face, exactly as it did before the faces were bundled).
int RegisterBundledFonts(const std::wstring& fonts_directory);

// True when |family| resolves to a face this process can actually draw with.
// EnumFontFamiliesExW answers from the process's own font list, so a face
// registered above is visible here and an unregistered one is not — which is
// what makes this usable as the check for "did the bundled font take effect".
bool IsFontFamilyAvailable(const std::wstring& family);

}  // namespace win32
}  // namespace renderer
}  // namespace mozc

#endif  // MOZC_RENDERER_WIN32_WIN32_FONT_RESOURCE_H_
