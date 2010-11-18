// Copyright (c) 2006-2008 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/proxy/proxy_config_service_win.h"

#include <windows.h>
#include <winhttp.h>

#include "base/logging.h"
#include "base/scoped_ptr.h"
#include "base/string_tokenizer.h"
#include "base/string_util.h"
#include "base/stl_util-inl.h"
#include "base/thread_restrictions.h"
#include "base/win/registry.h"
#include "net/base/net_errors.h"
#include "net/proxy/proxy_config.h"

#pragma comment(lib, "winhttp.lib")

namespace net {

namespace {

const int kPollIntervalSec = 10;

void FreeIEConfig(WINHTTP_CURRENT_USER_IE_PROXY_CONFIG* ie_config) {
  if (ie_config->lpszAutoConfigUrl)
    GlobalFree(ie_config->lpszAutoConfigUrl);
  if (ie_config->lpszProxy)
    GlobalFree(ie_config->lpszProxy);
  if (ie_config->lpszProxyBypass)
    GlobalFree(ie_config->lpszProxyBypass);
}

}  // namespace

// RegKey and ObjectWatcher pair.
class ProxyConfigServiceWin::KeyEntry {
 public:
  bool StartWatching(base::ObjectWatcher::Delegate* delegate) {
    // Try to create a watch event for the registry key (which watches the
    // sibling tree as well).
    if (!key_.StartWatching())
      return false;

    // Now setup an ObjectWatcher for this event, so we get OnObjectSignaled()
    // invoked on this message loop once it is signalled.
    if (!watcher_.StartWatching(key_.watch_event(), delegate))
      return false;

    return true;
  }

  bool CreateRegKey(HKEY rootkey, const wchar_t* subkey) {
    return key_.Create(rootkey, subkey, KEY_NOTIFY);
  }

  HANDLE watch_event() const {
    return key_.watch_event();
  }

 private:
  base::win::RegKey key_;
  base::ObjectWatcher watcher_;
};

ProxyConfigServiceWin::ProxyConfigServiceWin()
    : PollingProxyConfigService(
          base::TimeDelta::FromSeconds(kPollIntervalSec),
          &ProxyConfigServiceWin::GetCurrentProxyConfig) {
}

ProxyConfigServiceWin::~ProxyConfigServiceWin() {
  // The registry functions below will end up going to disk.  Do this on another
  // thread to avoid slowing the IO thread.  http://crbug.com/61453
  base::ThreadRestrictions::ScopedAllowIO allow_io;
  STLDeleteElements(&keys_to_watch_);
}

void ProxyConfigServiceWin::AddObserver(Observer* observer) {
  // Lazily-initialize our registry watcher.
  StartWatchingRegistryForChanges();

  // Let the super-class do its work now.
  PollingProxyConfigService::AddObserver(observer);
}

void ProxyConfigServiceWin::StartWatchingRegistryForChanges() {
  if (!keys_to_watch_.empty())
    return;  // Already initialized.

  // The registry functions below will end up going to disk.  Do this on another
  // thread to avoid slowing the IO thread.  http://crbug.com/61453
  base::ThreadRestrictions::ScopedAllowIO allow_io;

  // There are a number of different places where proxy settings can live
  // in the registry. In some cases it appears in a binary value, in other
  // cases string values. Furthermore winhttp and wininet appear to have
  // separate stores, and proxy settings can be configured per-machine
  // or per-user.
  //
  // This function is probably not exhaustive in the registry locations it
  // watches for changes, however it should catch the majority of the
  // cases. In case we have missed some less common triggers (likely), we
  // will catch them during the periodic (10 second) polling, so things
  // will recover.

  AddKeyToWatchList(
      HKEY_CURRENT_USER,
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings");

  AddKeyToWatchList(
      HKEY_LOCAL_MACHINE,
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings");

  AddKeyToWatchList(
      HKEY_LOCAL_MACHINE,
      L"SOFTWARE\\Policies\\Microsoft\\Windows\\CurrentVersion\\"
      L"Internet Settings");
}

bool ProxyConfigServiceWin::AddKeyToWatchList(HKEY rootkey,
                                              const wchar_t* subkey) {
  scoped_ptr<KeyEntry> entry(new KeyEntry);
  if (!entry->CreateRegKey(rootkey, subkey))
    return false;

  if (!entry->StartWatching(this))
    return false;

  keys_to_watch_.push_back(entry.release());
  return true;
}

void ProxyConfigServiceWin::OnObjectSignaled(HANDLE object) {
  // Figure out which registry key signalled this change.
  KeyEntryList::iterator it;
  for (it = keys_to_watch_.begin(); it != keys_to_watch_.end(); ++it) {
    if ((*it)->watch_event() == object)
      break;
  }

  DCHECK(it != keys_to_watch_.end());

  // Keep watching the registry key.
  if (!(*it)->StartWatching(this))
    keys_to_watch_.erase(it);

  // Have the PollingProxyConfigService test for changes.
  CheckForChangesNow();
}

// static
void ProxyConfigServiceWin::GetCurrentProxyConfig(ProxyConfig* config) {
  WINHTTP_CURRENT_USER_IE_PROXY_CONFIG ie_config = {0};
  if (!WinHttpGetIEProxyConfigForCurrentUser(&ie_config)) {
    LOG(ERROR) << "WinHttpGetIEProxyConfigForCurrentUser failed: " <<
        GetLastError();
    *config = ProxyConfig::CreateDirect();
    return;
  }
  SetFromIEConfig(config, ie_config);
  FreeIEConfig(&ie_config);
}

// static
void ProxyConfigServiceWin::SetFromIEConfig(
    ProxyConfig* config,
    const WINHTTP_CURRENT_USER_IE_PROXY_CONFIG& ie_config) {
  if (ie_config.fAutoDetect)
    config->set_auto_detect(true);
  if (ie_config.lpszProxy) {
    // lpszProxy may be a single proxy, or a proxy per scheme. The format
    // is compatible with ProxyConfig::ProxyRules's string format.
    config->proxy_rules().ParseFromString(WideToASCII(ie_config.lpszProxy));
  }
  if (ie_config.lpszProxyBypass) {
    std::string proxy_bypass = WideToASCII(ie_config.lpszProxyBypass);

    StringTokenizer proxy_server_bypass_list(proxy_bypass, "; \t\n\r");
    while (proxy_server_bypass_list.GetNext()) {
      std::string bypass_url_domain = proxy_server_bypass_list.token();
      config->proxy_rules().bypass_rules.AddRuleFromString(bypass_url_domain);
    }
  }
  if (ie_config.lpszAutoConfigUrl)
    config->set_pac_url(GURL(ie_config.lpszAutoConfigUrl));
}

}  // namespace net
