// Copyright 2023 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromeos/ash/components/nearby/presence/credentials/nearby_presence_server_client_impl.h"

#include <memory>

#include "base/base64url.h"
#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/memory/ptr_util.h"
#include "base/strings/string_number_conversions.h"
#include "chromeos/ash/components/nearby/common/client/nearby_api_call_flow_impl.h"
#include "chromeos/ash/components/nearby/common/client/nearby_http_result.h"
#include "chromeos/ash/components/nearby/presence/proto/list_public_certificates_rpc.pb.h"
#include "chromeos/ash/components/nearby/presence/proto/rpc_resources.pb.h"
#include "chromeos/ash/components/nearby/presence/proto/update_device_rpc.pb.h"
#include "components/signin/public/base/consent_level.h"
#include "components/signin/public/identity_manager/access_token_info.h"
#include "components/signin/public/identity_manager/identity_manager.h"
#include "components/signin/public/identity_manager/primary_account_access_token_fetcher.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"

namespace {

// -------------- Nearby Presence Service v1 Endpoints --------------------

const char kDefaultNearbyPresenceV1HTTPHost[] =
    "https://nearbypresence-pa.googleapis.com";

const char kNearbyPresenceV1Path[] = "v1/";

const char kListPublicCertificatesPath[] = "publicCertificates";

const char kPageSize[] = "page_size";
const char kPageToken[] = "page_token";
const char kSecretIds[] = "secret_ids";

const char kNearbyPresenceOAuth2Scope[] =
    "https://www.googleapis.com/auth/nearbypresence-pa";
const char kNearbyPresenceOAthConsumerName[] = "nearby_presence_client";

// Creates the full Nearby Presence v1 URL for endpoint to the API with
// |request_path|.
GURL CreateV1RequestUrl(const std::string& request_path) {
  GURL google_apis_url = GURL(kDefaultNearbyPresenceV1HTTPHost);
  return google_apis_url.Resolve(kNearbyPresenceV1Path + request_path);
}

ash::nearby::NearbyApiCallFlow::QueryParameters
ListPublicCertificatesRequestToQueryParameters(
    const ash::nearby::proto::ListPublicCertificatesRequest& request) {
  ash::nearby::NearbyApiCallFlow::QueryParameters query_parameters;
  if (request.page_size() > 0) {
    query_parameters.emplace_back(kPageSize,
                                  base::NumberToString(request.page_size()));
  }
  if (!request.page_token().empty()) {
    query_parameters.emplace_back(kPageToken, request.page_token());
  }
  for (const std::string& id : request.secret_ids()) {
    // NOTE: One Platform requires that byte fields be URL-safe base64 encoded.
    std::string encoded_id;
    base::Base64UrlEncode(id, base::Base64UrlEncodePolicy::INCLUDE_PADDING,
                          &encoded_id);
    query_parameters.emplace_back(kSecretIds, encoded_id);
  }
  return query_parameters;
}

const net::PartialNetworkTrafficAnnotationTag& GetUpdateDeviceAnnotation() {
  static const net::PartialNetworkTrafficAnnotationTag annotation =
      net::DefinePartialNetworkTrafficAnnotation(
          "nearby_presence_update_device", "oauth2_api_call_flow",
          R"(
      semantics {
        sender: "Nearby Presence"
        description:
          "Used as part of the Nearby Presence feature that allows a scanning "
          "abstraction layer, and a device identity management library. "
          "The call sends the local device's user-defined name and "
          "Nearby Presence specific crypto data from the local device to the "
          "Google-owned Nearby server. This data is also uploaded to the "
          "server and distributed to other devices in the user GAIA to help "
          "establish an authenticated channel during the Nearby Presence flow. "
          "This crypto data can be immediately invalidated by the local device "
          "at any time without needing to communicate with the server. For "
          "example, it expires after five days and new data needs to be "
          "uploaded. The server returns the local device user's full name and "
          "icon URL if available on the Google server."
        trigger:
          "Automatically called daily to retrieve any updates to the user's "
          "full name or icon URL. This request is also sent whenever new "
          "crypto data is generated by the local device and needs to be "
          "shared with other devices in the user GAIA. This request is also "
          "sent when a new device is added to the user GAIA and needs the "
          "local device's crypto data."
        data:
          "Sends an OAuth 2.0 token, the local device's name, contact, and/or "
          "Nearby Presence specific crypto data. Possibly receives the user's "
          "full name and icon URL from the Google server."
        destination: GOOGLE_OWNED_SERVICE
        internal {
            contacts {
              email: "julietlevesque@google.com"
            }
            contacts {
              email: "chromeos-cross-device-eng@google.com"
            }
          }
          user_data {
            type: ACCESS_TOKEN
            type: ARBITRARY_DATA
          }
          last_reviewed: "2023-04-04"
      }
      policy {
        setting:
          "Only sent when Nearby Presence is enabled and the user is signed in "
          "with their Google account."
        chrome_policy {
          SigninAllowed {
            SigninAllowed: false
          }
        }
      })");
  return annotation;
}

const net::PartialNetworkTrafficAnnotationTag&
GetListPublicCertificatesAnnotation() {
  static const net::PartialNetworkTrafficAnnotationTag annotation =
      net::DefinePartialNetworkTrafficAnnotation(
          "nearby_presence_list_public_certificates", "oauth2_api_call_flow",
          R"(
      semantics {
        sender: "Nearby Presence"
        description:
         "Used as part of the Nearby Presence feature that allows a scanning "
          "abstraction layer, and a device identity management library. "
          "The call retrieves Nearby Presence specific crypto data "
          "from the Google-owned Nearby server. The data was uploaded by other "
          "devices and is needed to establish an authenticated connection with "
          "those device during the Nearby Presence flow."
        trigger:
          "Automatically called at least once a day to retrieve any updates to "
          "the list of crypto data. It is also called when Nearby Presence is "
          "in use to ensure up-to-date data."
        data:
          "Sends an OAuth 2.0 token. Receives Nearby Presence specific crypto "
          "necessary for establishing an authenticated channel with other "
          "devices."
        destination: GOOGLE_OWNED_SERVICE
        internal {
            contacts {
              email: "julietlevesque@google.com"
            }
            contacts {
              email: "chromeos-cross-device-eng@google.com"
            }
          }
          user_data {
            type: ACCESS_TOKEN
            type: ARBITRARY_DATA
          }
          last_reviewed: "2023-04-04"
      }
      policy {
        setting:
          "Only sent when Nearby Presence is enabled and the user is signed in "
          "with their Google account."
        chrome_policy {
          SigninAllowed {
            SigninAllowed: false
          }
        }
          })");
  return annotation;
}

}  // namespace

namespace ash::nearby::presence {

// static
NearbyPresenceServerClientImpl::Factory*
    NearbyPresenceServerClientImpl::Factory::g_test_factory_ = nullptr;

// static
std::unique_ptr<NearbyPresenceServerClient>
NearbyPresenceServerClientImpl::Factory::Create(
    std::unique_ptr<NearbyApiCallFlow> api_call_flow,
    signin::IdentityManager* identity_manager,
    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory) {
  if (g_test_factory_) {
    return g_test_factory_->CreateInstance(std::move(api_call_flow),
                                           identity_manager,
                                           std::move(url_loader_factory));
  }

  return base::WrapUnique(new NearbyPresenceServerClientImpl(
      std::move(api_call_flow), identity_manager,
      std::move(url_loader_factory)));
}

// static
void NearbyPresenceServerClientImpl::Factory::SetFactoryForTesting(
    Factory* test_factory) {
  g_test_factory_ = test_factory;
}

NearbyPresenceServerClientImpl::Factory::~Factory() = default;

NearbyPresenceServerClientImpl::NearbyPresenceServerClientImpl(
    std::unique_ptr<NearbyApiCallFlow> api_call_flow,
    signin::IdentityManager* identity_manager,
    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory)
    : api_call_flow_(std::move(api_call_flow)),
      identity_manager_(identity_manager),
      url_loader_factory_(std::move(url_loader_factory)) {}

NearbyPresenceServerClientImpl::~NearbyPresenceServerClientImpl() = default;

void NearbyPresenceServerClientImpl::UpdateDevice(
    const ash::nearby::proto::UpdateDeviceRequest& request,
    UpdateDeviceCallback callback,
    ErrorCallback error_callback) {
  MakeApiCall(CreateV1RequestUrl(request.device().name()), RequestType::kPatch,
              request.SerializeAsString(),
              /*request_as_query_parameters=*/absl::nullopt,
              std::move(callback), std::move(error_callback),
              GetUpdateDeviceAnnotation());
}

void NearbyPresenceServerClientImpl::ListPublicCertificates(
    const ash::nearby::proto::ListPublicCertificatesRequest& request,
    ListPublicCertificatesCallback callback,
    ErrorCallback error_callback) {
  MakeApiCall(
      CreateV1RequestUrl(request.parent() + "/" + kListPublicCertificatesPath),
      RequestType::kGet, /*serialized_request=*/absl::nullopt,
      ListPublicCertificatesRequestToQueryParameters(request),
      std::move(callback), std::move(error_callback),
      GetListPublicCertificatesAnnotation());
}

std::string NearbyPresenceServerClientImpl::GetAccessTokenUsed() {
  return access_token_used_;
}

template <class ResponseProto>
void NearbyPresenceServerClientImpl::MakeApiCall(
    const GURL& request_url,
    RequestType request_type,
    const absl::optional<std::string>& serialized_request,
    const absl::optional<NearbyApiCallFlow::QueryParameters>&
        request_as_query_parameters,
    base::OnceCallback<void(const ResponseProto&)> response_callback,
    ErrorCallback error_callback,
    const net::PartialNetworkTrafficAnnotationTag& partial_traffic_annotation) {
  DCHECK(!has_call_started_)
      << "NearbyPresenceServerClientImpl::MakeApiCall(): Tried to make an API "
      << "call, but the client had already been used.";
  has_call_started_ = true;

  api_call_flow_->SetPartialNetworkTrafficAnnotation(
      partial_traffic_annotation);

  request_url_ = request_url;
  error_callback_ = std::move(error_callback);

  OAuth2AccessTokenManager::ScopeSet scopes;
  scopes.insert(kNearbyPresenceOAuth2Scope);

  access_token_fetcher_ = std::make_unique<
      signin::PrimaryAccountAccessTokenFetcher>(
      kNearbyPresenceOAthConsumerName, identity_manager_, scopes,
      base::BindOnce(
          &NearbyPresenceServerClientImpl::OnAccessTokenFetched<ResponseProto>,
          weak_ptr_factory_.GetWeakPtr(), request_type, serialized_request,
          request_as_query_parameters, std::move(response_callback)),
      signin::PrimaryAccountAccessTokenFetcher::Mode::kWaitUntilAvailable,
      signin::ConsentLevel::kSignin);
}

template <class ResponseProto>
void NearbyPresenceServerClientImpl::OnAccessTokenFetched(
    RequestType request_type,
    const absl::optional<std::string>& serialized_request,
    const absl::optional<NearbyApiCallFlow::QueryParameters>&
        request_as_query_parameters,
    base::OnceCallback<void(const ResponseProto&)> response_callback,
    GoogleServiceAuthError error,
    signin::AccessTokenInfo access_token_info) {
  access_token_fetcher_.reset();

  if (error.state() != GoogleServiceAuthError::NONE) {
    OnApiCallFailed(NearbyHttpError::kAuthenticationError);
    return;
  }
  access_token_used_ = access_token_info.token;

  switch (request_type) {
    case RequestType::kGet:
      DCHECK(request_as_query_parameters && !serialized_request);
      api_call_flow_->StartGetRequest(
          request_url_, *request_as_query_parameters, url_loader_factory_,
          access_token_used_,
          base::BindOnce(
              &NearbyPresenceServerClientImpl::OnFlowSuccess<ResponseProto>,
              weak_ptr_factory_.GetWeakPtr(), std::move(response_callback)),
          base::BindOnce(&NearbyPresenceServerClientImpl::OnApiCallFailed,
                         weak_ptr_factory_.GetWeakPtr()));
      break;
    case RequestType::kPost:
      DCHECK(serialized_request && !request_as_query_parameters);
      api_call_flow_->StartPostRequest(
          request_url_, *serialized_request, url_loader_factory_,
          access_token_used_,
          base::BindOnce(
              &NearbyPresenceServerClientImpl::OnFlowSuccess<ResponseProto>,
              weak_ptr_factory_.GetWeakPtr(), std::move(response_callback)),
          base::BindOnce(&NearbyPresenceServerClientImpl::OnApiCallFailed,
                         weak_ptr_factory_.GetWeakPtr()));
      break;
    case RequestType::kPatch:
      DCHECK(serialized_request && !request_as_query_parameters);
      api_call_flow_->StartPatchRequest(
          request_url_, *serialized_request, url_loader_factory_,
          access_token_used_,
          base::BindOnce(
              &NearbyPresenceServerClientImpl::OnFlowSuccess<ResponseProto>,
              weak_ptr_factory_.GetWeakPtr(), std::move(response_callback)),
          base::BindOnce(&NearbyPresenceServerClientImpl::OnApiCallFailed,
                         weak_ptr_factory_.GetWeakPtr()));
      break;
  }
}

template <class ResponseProto>
void NearbyPresenceServerClientImpl::OnFlowSuccess(
    base::OnceCallback<void(const ResponseProto&)> result_callback,
    const std::string& serialized_response) {
  ResponseProto response;
  if (!response.ParseFromString(serialized_response)) {
    OnApiCallFailed(NearbyHttpError::kResponseMalformed);
    return;
  }

  std::move(result_callback).Run(response);
}

void NearbyPresenceServerClientImpl::OnApiCallFailed(NearbyHttpError error) {
  std::move(error_callback_).Run(error);
}

}  // namespace ash::nearby::presence
