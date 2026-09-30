#pragma once
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace po {

inline std::optional<double> PdhRead(PDH_HCOUNTER c) {
  if (!c) return std::nullopt;
  PDH_FMT_COUNTERVALUE v{};
  if (PdhGetFormattedCounterValue(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, nullptr, &v) != ERROR_SUCCESS)
    return std::nullopt;
  if (v.CStatus != PDH_CSTATUS_VALID_DATA && v.CStatus != PDH_CSTATUS_NEW_DATA) return std::nullopt;
  return v.doubleValue;
}

// Legge un contatore con wildcard: (nome istanza, valore).
inline std::vector<std::pair<std::wstring, double>> PdhReadArray(PDH_HCOUNTER c) {
  std::vector<std::pair<std::wstring, double>> out;
  if (!c) return out;
  DWORD bytes = 0, count = 0;
  constexpr DWORD fmt = PDH_FMT_DOUBLE | PDH_FMT_NOCAP100;
  if (PdhGetFormattedCounterArrayW(c, fmt, &bytes, &count, nullptr) != PDH_MORE_DATA) return out;
  std::vector<BYTE> buf(bytes);
  auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
  if (PdhGetFormattedCounterArrayW(c, fmt, &bytes, &count, items) != ERROR_SUCCESS) return out;
  out.reserve(count);
  for (DWORD i = 0; i < count; ++i) {
    const auto& fv = items[i].FmtValue;
    if (fv.CStatus == PDH_CSTATUS_VALID_DATA || fv.CStatus == PDH_CSTATUS_NEW_DATA)
      out.emplace_back(items[i].szName, fv.doubleValue);
  }
  return out;
}

}  // namespace po
