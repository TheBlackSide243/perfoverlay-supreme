#pragma once
#include <windows.h>
#include <wbemidl.h>
#include <wrl/client.h>

#include <initializer_list>
#include <string>
#include <unordered_map>
#include <vector>

namespace po {

struct WmiValue {
  std::wstring str;
  double num = 0;
  bool isNum = false;
};
using WmiRow = std::unordered_map<std::wstring, WmiValue>;

// Client WMI minimale. Richiede COM inizializzato sul thread chiamante.
class Wmi {
 public:
  bool Connect(const wchar_t* ns);
  bool Connected() const { return svc_ != nullptr; }
  void Reset() { svc_.Reset(); }
  std::vector<WmiRow> Query(const wchar_t* wql, std::initializer_list<const wchar_t*> fields);

 private:
  Microsoft::WRL::ComPtr<IWbemServices> svc_;
};

}  // namespace po
