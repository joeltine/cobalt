// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/safe_browsing/cloud_content_scanning/deep_scanning_browsertest_base.h"

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/enterprise/connectors/analysis/content_analysis_dialog.h"
#include "chrome/browser/enterprise/connectors/analysis/fake_content_analysis_delegate.h"
#include "chrome/browser/enterprise/connectors/analysis/files_request_handler.h"
#include "chrome/browser/enterprise/connectors/connectors_service.h"
#include "chrome/browser/policy/dm_token_utils.h"
#include "chrome/browser/safe_browsing/cloud_content_scanning/deep_scanning_test_utils.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/test/base/testing_browser_process.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "components/safe_browsing/core/common/features.h"
#include "components/safe_browsing/core/common/safe_browsing_prefs.h"

namespace safe_browsing {

namespace {

constexpr char kDmToken[] = "dm_token";

constexpr base::TimeDelta kMinimumPendingDelay = base::Milliseconds(400);
constexpr base::TimeDelta kSuccessTimeout = base::Milliseconds(100);
constexpr base::TimeDelta kShowDialogDelay = base::Milliseconds(0);

class UnresponsiveFilesRequestHandler
    : public enterprise_connectors::FilesRequestHandler {
 public:
  using enterprise_connectors::FilesRequestHandler::FilesRequestHandler;

  static std::unique_ptr<enterprise_connectors::FilesRequestHandler> Create(
      safe_browsing::BinaryUploadService* upload_service,
      Profile* profile,
      const enterprise_connectors::AnalysisSettings& analysis_settings,
      GURL url,
      const std::string& source,
      const std::string& destination,
      const std::string& user_action_id,
      const std::string& tab_title,
      safe_browsing::DeepScanAccessPoint access_point,
      const std::vector<base::FilePath>& paths,
      enterprise_connectors::FilesRequestHandler::CompletionCallback callback) {
    return base::WrapUnique(new UnresponsiveFilesRequestHandler(
        upload_service, profile, analysis_settings, url, source, destination,
        user_action_id, tab_title, access_point, paths, std::move(callback)));
  }

 private:
  void UploadFileForDeepScanning(
      safe_browsing::BinaryUploadService::Result result,
      const base::FilePath& path,
      std::unique_ptr<safe_browsing::BinaryUploadService::Request> request)
      override {
    // Do nothing.
  }
};

class UnresponsiveContentAnalysisDelegate
    : public enterprise_connectors::FakeContentAnalysisDelegate {
 public:
  using enterprise_connectors::FakeContentAnalysisDelegate::
      FakeContentAnalysisDelegate;

  static std::unique_ptr<enterprise_connectors::ContentAnalysisDelegate> Create(
      base::RepeatingClosure delete_closure,
      StatusCallback status_callback,
      std::string dm_token,
      content::WebContents* web_contents,
      Data data,
      CompletionCallback callback) {
    enterprise_connectors::FilesRequestHandler::SetFactoryForTesting(
        base::BindRepeating(&UnresponsiveFilesRequestHandler::Create));
    return std::make_unique<UnresponsiveContentAnalysisDelegate>(
        delete_closure, status_callback, std::move(dm_token), web_contents,
        std::move(data), std::move(callback));
  }

 private:
  void UploadTextForDeepScanning(
      std::unique_ptr<BinaryUploadService::Request> request) override {
    // Do nothing.
  }
};

}  // namespace

DeepScanningBrowserTestBase::DeepScanningBrowserTestBase() {
  // Change the time values of the upload UI to smaller ones to make tests
  // showing it run faster.
  enterprise_connectors::ContentAnalysisDialog::
      SetMinimumPendingDialogTimeForTesting(kMinimumPendingDelay);
  enterprise_connectors::ContentAnalysisDialog::
      SetSuccessDialogTimeoutForTesting(kSuccessTimeout);
  enterprise_connectors::ContentAnalysisDialog::SetShowDialogDelayForTesting(
      kShowDialogDelay);
}

DeepScanningBrowserTestBase::~DeepScanningBrowserTestBase() = default;

void DeepScanningBrowserTestBase::TearDownOnMainThread() {
  enterprise_connectors::ContentAnalysisDelegate::ResetFactoryForTesting();
  enterprise_connectors::FilesRequestHandler::ResetFactoryForTesting();

  ClearAnalysisConnector(browser()->profile()->GetPrefs(),
                         enterprise_connectors::FILE_ATTACHED);
  ClearAnalysisConnector(browser()->profile()->GetPrefs(),
                         enterprise_connectors::FILE_DOWNLOADED);
  ClearAnalysisConnector(browser()->profile()->GetPrefs(),
                         enterprise_connectors::BULK_DATA_ENTRY);
  ClearAnalysisConnector(browser()->profile()->GetPrefs(),
                         enterprise_connectors::PRINT);
  SetOnSecurityEventReporting(browser()->profile()->GetPrefs(), false);
}

void DeepScanningBrowserTestBase::SetUpDelegate() {
  SetDMTokenForTesting(policy::DMToken::CreateValidTokenForTesting(kDmToken));
  enterprise_connectors::ContentAnalysisDelegate::SetFactoryForTesting(
      base::BindRepeating(
          &enterprise_connectors::FakeContentAnalysisDelegate::Create,
          base::DoNothing(),
          base::BindRepeating(&DeepScanningBrowserTestBase::StatusCallback,
                              base::Unretained(this)),
          kDmToken));
}

void DeepScanningBrowserTestBase::SetUpUnresponsiveDelegate() {
  SetDMTokenForTesting(policy::DMToken::CreateValidTokenForTesting(kDmToken));
  enterprise_connectors::ContentAnalysisDelegate::SetFactoryForTesting(
      base::BindRepeating(
          &UnresponsiveContentAnalysisDelegate::Create, base::DoNothing(),
          base::BindRepeating(&DeepScanningBrowserTestBase::StatusCallback,
                              base::Unretained(this)),
          kDmToken));
}

void DeepScanningBrowserTestBase::SetQuitClosure(
    base::RepeatingClosure quit_closure) {
  quit_closure_ = quit_closure;
}

void DeepScanningBrowserTestBase::CallQuitClosure() {
  if (!quit_closure_.is_null())
    quit_closure_.Run();
}

void DeepScanningBrowserTestBase::SetStatusCallbackResponse(
    enterprise_connectors::ContentAnalysisResponse response) {
  connector_status_callback_response_ = response;
}

enterprise_connectors::ContentAnalysisResponse
DeepScanningBrowserTestBase::StatusCallback(const std::string& contents,
                                            const base::FilePath& path) {
  return connector_status_callback_response_;
}

void DeepScanningBrowserTestBase::CreateFilesForTest(
    const std::vector<std::string>& paths,
    const std::vector<std::string>& contents,
    enterprise_connectors::ContentAnalysisDelegate::Data* data) {
  ASSERT_EQ(paths.size(), contents.size());

  ASSERT_TRUE(temp_dir_.CreateUniqueTempDir());

  for (size_t i = 0; i < paths.size(); ++i) {
    base::FilePath path = temp_dir_.GetPath().AppendASCII(paths[i]);
    created_file_paths_.emplace_back(path);
    base::File file(path, base::File::FLAG_CREATE | base::File::FLAG_WRITE);
    file.WriteAtCurrentPos(contents[i].data(), contents[i].size());
    data->paths.emplace_back(path);
  }
}

const std::vector<base::FilePath>&
DeepScanningBrowserTestBase::created_file_paths() const {
  return created_file_paths_;
}

}  // namespace safe_browsing
