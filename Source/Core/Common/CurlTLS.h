// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <string_view>

#include <curl/curl.h>

#include "Common/FileUtil.h"

namespace Common
{
inline void ConfigureCurlTLS(CURL* curl)
{
  const curl_version_info_data* info = curl_version_info(CURLVERSION_NOW);
  if (!info || !info->ssl_version)
    return;
  const std::string_view backend(info->ssl_version);
  if (backend.find("Schannel") != std::string_view::npos)
  {
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_REVOKE_BEST_EFFORT));
    return;
  }
  if (backend.find("mbedTLS") == std::string_view::npos)
    return;
  static constexpr std::array<const char*, 4> bundles{
      "/etc/ssl/cert.pem",
      "/etc/ssl/certs/ca-certificates.crt",
      "/etc/pki/tls/certs/ca-bundle.crt",
      "/etc/ssl/ca-bundle.pem",
  };
  for (const char* bundle : bundles)
  {
    if (File::Exists(bundle))
    {
      curl_easy_setopt(curl, CURLOPT_CAINFO, bundle);
      return;
    }
  }
}
}
