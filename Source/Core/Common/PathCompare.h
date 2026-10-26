// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <string_view>

// Path comparison that ignores case and the kind of slash, for Windows paths that reach the code in both
// spellings ("U:/Users/x" from std::filesystem, "U:\Users\x" from WinRT).
namespace File
{
namespace detail
{
template <typename C>
constexpr bool IsSep(C c)
{
  return c == C('/') || c == C('\\');
}

template <typename C>
constexpr C Lower(C c)
{
  return (c >= C('A') && c <= C('Z')) ? C(c - C('A') + C('a')) : c;
}

template <typename C>
constexpr std::basic_string_view<C> TrimSeps(std::basic_string_view<C> s)
{
  while (!s.empty() && IsSep(s.back()))
    s.remove_suffix(1);
  return s;
}

template <typename C>
constexpr bool StartsWith(std::basic_string_view<C> s, std::basic_string_view<C> prefix)
{
  if (s.size() < prefix.size())
    return false;
  for (size_t i = 0; i < prefix.size(); i++)
  {
    const bool same = IsSep(s[i]) ? IsSep(prefix[i]) : Lower(s[i]) == Lower(prefix[i]);
    if (!same)
      return false;
  }
  return true;
}
}  // namespace detail

// True when `path` is `dir` or lies below it.
template <typename C>
constexpr bool IsSameOrUnder(std::basic_string_view<C> path, std::basic_string_view<C> dir)
{
  dir = detail::TrimSeps(dir);
  if (dir.empty() || !detail::StartsWith(path, dir))
    return false;
  return path.size() == dir.size() || detail::IsSep(path[dir.size()]);
}

// True when `path` is a directory above `dir` (not `dir` itself).
template <typename C>
constexpr bool IsStrictAncestor(std::basic_string_view<C> path, std::basic_string_view<C> dir)
{
  path = detail::TrimSeps(path);
  if (path.empty() || dir.size() <= path.size() || !detail::StartsWith(dir, path))
    return false;
  return detail::IsSep(dir[path.size()]);
}

// The upper-case drive letter of "X:..." or 0 when the path has none.
template <typename C>
constexpr C DriveOf(std::basic_string_view<C> path)
{
  if (path.size() < 2 || path[1] != C(':'))
    return 0;
  const C c = path[0];
  if (c >= C('a') && c <= C('z'))
    return C(c - C('a') + C('A'));
  return (c >= C('A') && c <= C('Z')) ? c : C(0);
}
}  // namespace File
