#include "monitor/data/wmi.h"

#include <oleauto.h>

using Microsoft::WRL::ComPtr;

namespace po {
namespace {
struct Bstr {
  BSTR b;
  explicit Bstr(const wchar_t* s) : b(SysAllocString(s)) {}
  ~Bstr() { SysFreeString(b); }
  Bstr(const Bstr&) = delete;
  Bstr& operator=(const Bstr&) = delete;
};
}  // namespace

bool Wmi::Connect(const wchar_t* ns) {
  svc_.Reset();
  ComPtr<IWbemLocator> loc;
  if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&loc)))) return false;
  Bstr bns(ns);
  ComPtr<IWbemServices> svc;
  if (FAILED(loc->ConnectServer(bns.b, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &svc))) return false;
  CoSetProxyBlanket(svc.Get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                    RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
  svc_ = svc;
  return true;
}

std::vector<WmiRow> Wmi::Query(const wchar_t* wql, std::initializer_list<const wchar_t*> fields) {
  std::vector<WmiRow> rows;
  if (!svc_) return rows;
  Bstr lang(L"WQL"), q(wql);
  ComPtr<IEnumWbemClassObject> en;
  const HRESULT hr =
      svc_->ExecQuery(lang.b, q.b, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &en);
  if (FAILED(hr)) {
    // Namespace sparito (es. LibreHardwareMonitor chiuso): riconnessione al prossimo giro.
    svc_.Reset();
    return rows;
  }
  while (true) {
    ComPtr<IWbemClassObject> obj;
    ULONG n = 0;
    if (FAILED(en->Next(3000, 1, obj.GetAddressOf(), &n)) || n == 0) break;
    WmiRow row;
    for (const wchar_t* f : fields) {
      VARIANT v;
      VariantInit(&v);
      if (SUCCEEDED(obj->Get(f, 0, &v, nullptr, nullptr))) {
        WmiValue wv;
        if (v.vt == VT_BSTR) {
          wv.str = v.bstrVal ? v.bstrVal : L"";
        } else if (v.vt != VT_NULL && v.vt != VT_EMPTY) {
          VARIANT d;
          VariantInit(&d);
          if (SUCCEEDED(VariantChangeType(&d, &v, 0, VT_R8))) {
            wv.num = d.dblVal;
            wv.isNum = true;
          }
          VariantClear(&d);
        }
        row[f] = std::move(wv);
      }
      VariantClear(&v);
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

}  // namespace po
