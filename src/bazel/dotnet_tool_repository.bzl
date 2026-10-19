# Copyright 2010-2021, Google Inc.
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met:
#
#     * Redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above
# copyright notice, this list of conditions and the following disclaimer
# in the documentation and/or other materials provided with the
# distribution.
#     * Neither the name of Google Inc. nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

"""Repository rule for dotnet tool."""

BUILD_TEMPLATE = """
package(
    default_visibility = ["//visibility:public"],
)

exports_files([
    "{executable}",  # version {version}
])
"""

def _stage_tool_from_nuget_cache(repo_ctx, tool_name, version):
    """Copies a cached NuGet tool package into the repository root.

    `dotnet tool install` needs the .NET SDK.  A machine that only has the
    runtimes fails with "No .NET SDKs were found", which leaves this repository
    without its executable and breaks every build that consumes it.  The tool is
    a framework-dependent apphost that runs on those runtimes, and NuGet has
    already cached the package, so unpacking the cache is enough and no SDK has
    to be installed.

    Returns True when the apphost is present afterwards.
    """
    home = repo_ctx.os.environ.get("USERPROFILE", "")
    if not home:
        home = repo_ctx.os.environ.get("HOME", "")
    if not home:
        return False

    nupkg = (home + "/.nuget/packages/" + tool_name + "/" + version + "/" +
             tool_name + "." + version + ".nupkg")
    if not repo_ctx.path(nupkg).exists:
        return False

    # A .nupkg is a zip and Starlark has no unzip, so use the shell.  The
    # payload sits under tools/<tfm>/any/, whose depth differs per package, so
    # the apphost is located by name rather than by an assumed path; the files
    # next to it are flattened into the repository root, which is the same
    # layout `dotnet tool install --tool-path` produces.
    #
    # Expand-Archive dispatches on the file extension and only recognizes .zip,
    # so the package is staged under a .zip name first: calling it directly on
    # the .nupkg fails with NotSupportedArchiveFileExtension and leaves the
    # repository with no executable at all.
    destination = str(repo_ctx.path(".")).replace("\\", "/")
    script = (
        "$ErrorActionPreference='Stop';" +
        "$stage=Join-Path $env:TEMP ('bzl_tool_stage_'+[guid]::NewGuid().ToString('N'));" +
        "$zip=$stage + '.zip';" +
        "try{" +
        "Copy-Item -LiteralPath '" + nupkg + "' -Destination $zip -Force;" +
        "Expand-Archive -LiteralPath $zip -DestinationPath $stage -Force;" +
        "$exe=Get-ChildItem -Path $stage -Recurse -Filter '" + tool_name + ".exe' | Select-Object -First 1;" +
        "if($exe){Copy-Item -Path (Join-Path $exe.DirectoryName '*') -Destination '" + destination + "' -Force -Recurse}" +
        "}finally{Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue;" +
        "Remove-Item -LiteralPath $zip -Force -ErrorAction SilentlyContinue}"
    )
    repo_ctx.execute(["powershell", "-NoProfile", "-Command", script],
                     quiet = False)
    return repo_ctx.path(tool_name + ".exe").exists


def _dotnet_tool_repo_impl(repo_ctx):
    is_windows = repo_ctx.os.name.lower().startswith("win")
    if not is_windows:
        repo_ctx.file("BUILD.bazel", "")
        return

    repo_root = repo_ctx.path(".")
    dotnet_tool = repo_ctx.which("dotnet.exe")
    tool_name = repo_ctx.attr.tool_name
    if not tool_name:
        # In bzlmod, repo_ctx.attr.name has a prefix like "_main~_repo_rules~wix".
        # Note also that Bazel 8.0+ uses "+" instead of "~".
        # https://github.com/bazelbuild/bazel/issues/23127
        tool_name = repo_ctx.attr.name.replace("~", "+").split("+")[-1]
    version = repo_ctx.attr.version

    result = repo_ctx.execute([
        dotnet_tool,
        "tool",
        "install",
        tool_name,
        "--version",
        version,
        "--tool-path",
        repo_root,
    ])

    if not repo_root.get_child(tool_name + ".exe").exists:
        # The install only reaches NuGet through the SDK.  Without one it fails
        # with "No .NET SDKs were found", so fall back to the cached package.
        # The result is reported so a build log shows which path was taken.
        if _stage_tool_from_nuget_cache(repo_ctx, tool_name, version):
            print("%s %s staged from the NuGet cache (dotnet SDK unavailable: %s)"
                  % (tool_name, version, result.stderr.strip()[:200]))
        else:
            print("%s %s could not be installed: %s"
                  % (tool_name, version, result.stderr.strip()[:200]))

    build_file_data = BUILD_TEMPLATE.format(
        executable = tool_name + ".exe",
        version = version,
    )
    repo_ctx.file("BUILD.bazel", build_file_data, executable = False)

dotnet_tool_repository = repository_rule(
    implementation = _dotnet_tool_repo_impl,
    configure = True,
    local = True,
    attrs = {
        "tool_name": attr.string(),
        "version": attr.string(mandatory = True),
    },
)
