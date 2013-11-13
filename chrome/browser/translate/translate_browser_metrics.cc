// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/translate/translate_browser_metrics.h"

#include <string>

#include "base/basictypes.h"
#include "base/metrics/histogram.h"
#include "base/metrics/sparse_histogram.h"
#include "chrome/browser/language_usage_metrics.h"

namespace {

// Constant string values to indicate UMA names. All entries should have
// a corresponding index in MetricsNameIndex and an entry in |kMetricsEntries|.
const char kTranslateInitiationStatus[] =
    "Translate.InitiationStatus.v2";
const char kTranslateReportLanguageDetectionError[] =
    "Translate.ReportLanguageDetectionError";
const char kTranslateLocalesOnDisabledByPrefs[] =
    "Translate.LocalesOnDisabledByPrefs";
const char kTranslateUndisplayableLanguage[] =
    "Translate.UndisplayableLanguage";
const char kTranslateUnsupportedLanguageAtInitiation[] =
    "Translate.UnsupportedLanguageAtInitiation";
const char kTranslateDeclineTranslate[] = "Translate.DeclineTranslate";
const char kTranslateRevertTranslation[] = "Translate.RevertTranslation";
const char kTranslatePerformTranslate[] = "Translate.Translate";
const char kTranslateNeverTranslateLang[] = "Translate.NeverTranslateLang";
const char kTranslateNeverTranslateSite[] = "Translate.NeverTranslateSite";
const char kTranslateAlwaysTranslateLang[] = "Translate.AlwaysTranslateLang";
const char kTranslateModifyOriginalLang[] = "Translate.ModifyOriginalLang";
const char kTranslateModifyTargetLang[] = "Translate.ModifyTargetLang";

struct MetricsEntry {
  TranslateBrowserMetrics::MetricsNameIndex index;
  const char* const name;
};

// This entry table should be updated when new UMA items are added.
const MetricsEntry kMetricsEntries[] = {
  { TranslateBrowserMetrics::UMA_INITIATION_STATUS,
    kTranslateInitiationStatus },
  { TranslateBrowserMetrics::UMA_LANGUAGE_DETECTION_ERROR,
    kTranslateReportLanguageDetectionError },
  { TranslateBrowserMetrics::UMA_LOCALES_ON_DISABLED_BY_PREFS,
    kTranslateLocalesOnDisabledByPrefs },
  { TranslateBrowserMetrics::UMA_UNDISPLAYABLE_LANGUAGE,
    kTranslateUndisplayableLanguage },
  { TranslateBrowserMetrics::UMA_UNSUPPORTED_LANGUAGE_AT_INITIATION,
    kTranslateUnsupportedLanguageAtInitiation },
  { TranslateBrowserMetrics::UMA_DECLINE_TRANSLATE,
    kTranslateDeclineTranslate },
  { TranslateBrowserMetrics::UMA_REVERT_TRANSLATION,
    kTranslateRevertTranslation },
  { TranslateBrowserMetrics::UMA_PERFORM_TRANSLATE,
    kTranslatePerformTranslate },
  { TranslateBrowserMetrics::UMA_NEVER_TRANSLATE_LANG,
    kTranslateNeverTranslateLang },
  { TranslateBrowserMetrics::UMA_NEVER_TRANSLATE_SITE,
    kTranslateNeverTranslateSite },
  { TranslateBrowserMetrics::UMA_ALWAYS_TRANSLATE_LANG,
    kTranslateAlwaysTranslateLang },
  { TranslateBrowserMetrics::UMA_MODIFY_ORIGINAL_LANG,
    kTranslateModifyOriginalLang },
  { TranslateBrowserMetrics::UMA_MODIFY_TARGET_LANG,
    kTranslateModifyTargetLang },
};

COMPILE_ASSERT(arraysize(kMetricsEntries) == TranslateBrowserMetrics::UMA_MAX,
               arraysize_of_kMetricsEntries_should_be_UMA_MAX);

}  // namespace

namespace TranslateBrowserMetrics {

void ReportInitiationStatus(InitiationStatusType type) {
  UMA_HISTOGRAM_ENUMERATION(kTranslateInitiationStatus,
                            type,
                            INITIATION_STATUS_MAX);
}

void ReportLanguageDetectionError() {
  UMA_HISTOGRAM_BOOLEAN(kTranslateReportLanguageDetectionError, true);
}

void ReportLocalesOnDisabledByPrefs(const std::string& locale) {
  UMA_HISTOGRAM_SPARSE_SLOWLY(kTranslateLocalesOnDisabledByPrefs,
                              LanguageUsageMetrics::ToLanguageCode(locale));
}

void ReportUndisplayableLanguage(const std::string& language) {
  int language_code = LanguageUsageMetrics::ToLanguageCode(language);
  UMA_HISTOGRAM_SPARSE_SLOWLY(kTranslateUndisplayableLanguage,
                              language_code);
}

void ReportUnsupportedLanguageAtInitiation(const std::string& language) {
  int language_code = LanguageUsageMetrics::ToLanguageCode(language);
  UMA_HISTOGRAM_SPARSE_SLOWLY(kTranslateUnsupportedLanguageAtInitiation,
                              language_code);
}

const char* GetMetricsName(MetricsNameIndex index) {
  for (size_t i = 0; i < arraysize(kMetricsEntries); ++i) {
    if (kMetricsEntries[i].index == index)
      return kMetricsEntries[i].name;
  }
  NOTREACHED();
  return NULL;
}

}  // namespace TranslateBrowserMetrics
