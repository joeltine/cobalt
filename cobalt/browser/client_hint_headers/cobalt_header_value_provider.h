// Copyright 2025 The Cobalt Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef COBALT_BROWSER_CLIENT_HINT_HEADERS_COBALT_HEADER_VALUE_PROVIDER_H_
#define COBALT_BROWSER_CLIENT_HINT_HEADERS_COBALT_HEADER_VALUE_PROVIDER_H_

#include <map>
#include <string>

#include "base/no_destructor.h"

namespace cobalt {
namespace browser {

using HeaderMap = std::map<std::string, std::string>;

class CobaltHeaderValueProvider {
 public:
  // Get the singleton instance.
  static CobaltHeaderValueProvider* GetInstance();

  CobaltHeaderValueProvider() = default;

  CobaltHeaderValueProvider(const CobaltHeaderValueProvider&) = delete;
  CobaltHeaderValueProvider& operator=(const CobaltHeaderValueProvider&) =
      delete;

  ~CobaltHeaderValueProvider() = default;

  void SetHeaderValue(const std::string& header_name,
                      const std::string& header_value);
  const HeaderMap& GetHeaderValues() const;

 private:
  friend class base::NoDestructor<CobaltHeaderValueProvider>;

  HeaderMap header_values_;
};

}  // namespace browser
}  // namespace cobalt

#endif  // COBALT_BROWSER_CLIENT_HINT_HEADERS_COBALT_HEADER_VALUE_PROVIDER_H_
