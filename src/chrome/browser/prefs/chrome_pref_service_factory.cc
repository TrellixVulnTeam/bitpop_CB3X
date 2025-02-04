// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/prefs/chrome_pref_service_factory.h"

#include <string>
#include <vector>

#include "base/bind.h"
#include "base/compiler_specific.h"
#include "base/debug/trace_event.h"
#include "base/files/file_path.h"
#include "base/metrics/field_trial.h"
#include "base/metrics/histogram.h"
#include "base/prefs/default_pref_store.h"
#include "base/prefs/json_pref_store.h"
#include "base/prefs/pref_filter.h"
#include "base/prefs/pref_notifier_impl.h"
#include "base/prefs/pref_registry.h"
#include "base/prefs/pref_registry_simple.h"
#include "base/prefs/pref_service.h"
#include "base/prefs/pref_store.h"
#include "base/prefs/pref_value_store.h"
#include "base/threading/sequenced_worker_pool.h"
#include "base/time/time.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/prefs/command_line_pref_store.h"
#include "chrome/browser/prefs/pref_hash_filter.h"
#include "chrome/browser/prefs/pref_model_associator.h"
#include "chrome/browser/prefs/pref_service_syncable.h"
#include "chrome/browser/prefs/pref_service_syncable_factory.h"
#include "chrome/browser/prefs/profile_pref_store_manager.h"
#include "chrome/browser/profiles/file_path_verifier_win.h"
#include "chrome/browser/profiles/profile_info_cache.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/search_engines/default_search_manager.h"
#include "chrome/browser/search_engines/default_search_pref_migration.h"
#include "chrome/browser/ui/profile_error_dialog.h"
#include "chrome/common/chrome_constants.h"
#include "chrome/common/pref_names.h"
#include "components/user_prefs/pref_registry_syncable.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_thread.h"
#include "extensions/browser/pref_names.h"
#include "grit/browser_resources.h"
#include "grit/chromium_strings.h"
#include "grit/generated_resources.h"
#include "ui/base/resource/resource_bundle.h"

#if defined(ENABLE_CONFIGURATION_POLICY)
#include "components/policy/core/browser/browser_policy_connector.h"
#include "components/policy/core/browser/configuration_policy_pref_store.h"
#include "components/policy/core/common/policy_types.h"
#endif

#if defined(ENABLE_MANAGED_USERS)
#include "chrome/browser/managed_mode/supervised_user_pref_store.h"
#endif

#if defined(OS_WIN)
#include "base/win/win_util.h"
#if defined(ENABLE_RLZ)
#include "rlz/lib/machine_id.h"
#endif  // defined(ENABLE_RLZ)
#endif  // defined(OS_WIN)

using content::BrowserContext;
using content::BrowserThread;

namespace {

// Whether we are in testing mode; can be enabled via
// DisableDelaysAndDomainCheckForTesting(). Forces startup checks to occur
// with no delay and ignores the presence of a domain when determining the
// active SettingsEnforcement group.
bool g_disable_delays_and_domain_check_for_testing = false;

// These preferences must be kept in sync with the TrackedPreference enum in
// tools/metrics/histograms/histograms.xml. To add a new preference, append it
// to the array and add a corresponding value to the histogram enum. Each
// tracked preference must be given a unique reporting ID.
const PrefHashFilter::TrackedPreferenceMetadata kTrackedPrefs[] = {
  {
    0, prefs::kShowHomeButton,
    PrefHashFilter::ENFORCE_ON_LOAD,
    PrefHashFilter::TRACKING_STRATEGY_ATOMIC
  },
  {
    1, prefs::kHomePageIsNewTabPage,
    PrefHashFilter::ENFORCE_ON_LOAD,
    PrefHashFilter::TRACKING_STRATEGY_ATOMIC
  },
  {
    2, prefs::kHomePage,
    PrefHashFilter::ENFORCE_ON_LOAD,
    PrefHashFilter::TRACKING_STRATEGY_ATOMIC
  },
  {
    3, prefs::kRestoreOnStartup,
    PrefHashFilter::ENFORCE_ON_LOAD,
    PrefHashFilter::TRACKING_STRATEGY_ATOMIC
  },
  {
    4, prefs::kURLsToRestoreOnStartup,
    PrefHashFilter::ENFORCE_ON_LOAD,
    PrefHashFilter::TRACKING_STRATEGY_ATOMIC
  },
  {
    5, extensions::pref_names::kExtensions,
    PrefHashFilter::NO_ENFORCEMENT,
    PrefHashFilter::TRACKING_STRATEGY_SPLIT
  },
  {
    6, prefs::kGoogleServicesLastUsername,
    PrefHashFilter::ENFORCE_ON_LOAD,
    PrefHashFilter::TRACKING_STRATEGY_ATOMIC
  },
  {
    7, prefs::kSearchProviderOverrides,
    PrefHashFilter::ENFORCE_ON_LOAD,
    PrefHashFilter::TRACKING_STRATEGY_ATOMIC
  },
  {
    8, prefs::kDefaultSearchProviderSearchURL,
    PrefHashFilter::ENFORCE_ON_LOAD,
    PrefHashFilter::TRACKING_STRATEGY_ATOMIC
  },
  {
    9, prefs::kDefaultSearchProviderKeyword,
    PrefHashFilter::ENFORCE_ON_LOAD,
    PrefHashFilter::TRACKING_STRATEGY_ATOMIC
  },
  {
    10, prefs::kDefaultSearchProviderName,
    PrefHashFilter::ENFORCE_ON_LOAD,
    PrefHashFilter::TRACKING_STRATEGY_ATOMIC
  },
#if !defined(OS_ANDROID)
  {
    11, prefs::kPinnedTabs,
    PrefHashFilter::ENFORCE_ON_LOAD,
    PrefHashFilter::TRACKING_STRATEGY_ATOMIC
  },
#endif
  {
    12, extensions::pref_names::kKnownDisabled,
    PrefHashFilter::NO_ENFORCEMENT,
    PrefHashFilter::TRACKING_STRATEGY_ATOMIC
  },
  {
    13, prefs::kProfileResetPromptMemento,
    PrefHashFilter::ENFORCE_ON_LOAD,
    PrefHashFilter::TRACKING_STRATEGY_ATOMIC
  },
  {
    14, DefaultSearchManager::kDefaultSearchProviderDataPrefName,
    PrefHashFilter::NO_ENFORCEMENT,
    PrefHashFilter::TRACKING_STRATEGY_ATOMIC
  },
  {
    // Protecting kPreferenceResetTime does two things:
    //  1) It ensures this isn't accidently set by someone stomping the pref
    //     file.
    //  2) More importantly, it declares kPreferenceResetTime as a protected
    //     pref which is required for it to be visible when queried via the
    //     SegregatedPrefStore. This is because it's written directly in the
    //     protected JsonPrefStore by that store's PrefHashFilter if there was
    //     a reset in FilterOnLoad and SegregatedPrefStore will not look for it
    //     in the protected JsonPrefStore unless it's declared as a protected
    //     preference here.
    15, prefs::kPreferenceResetTime,
    PrefHashFilter::ENFORCE_ON_LOAD,
    PrefHashFilter::TRACKING_STRATEGY_ATOMIC
  },
};

// The count of tracked preferences IDs across all platforms.
const size_t kTrackedPrefsReportingIDsCount = 16;
COMPILE_ASSERT(kTrackedPrefsReportingIDsCount >= arraysize(kTrackedPrefs),
               need_to_increment_ids_count);

// Each group enforces a superset of the protection provided by the previous
// one.
enum SettingsEnforcementGroup {
  GROUP_NO_ENFORCEMENT,
  // Only enforce settings on profile loads; still allow seeding of unloaded
  // profiles.
  GROUP_ENFORCE_ON_LOAD,
  // Also disallow seeding of unloaded profiles.
  GROUP_ENFORCE_ALWAYS,
  // Also enforce extension default search.
  GROUP_ENFORCE_ALWAYS_WITH_DSE,
  // Also enforce extension settings and default search.
  GROUP_ENFORCE_ALWAYS_WITH_EXTENSIONS_AND_DSE,
  // The default enforcement group contains all protection features.
  GROUP_ENFORCE_DEFAULT
};

SettingsEnforcementGroup GetSettingsEnforcementGroup() {
# if defined(OS_WIN)
  if (!g_disable_delays_and_domain_check_for_testing) {
    static bool first_call = true;
    static const bool is_enrolled_to_domain = base::win::IsEnrolledToDomain();
    if (first_call) {
      UMA_HISTOGRAM_BOOLEAN("Settings.TrackedPreferencesNoEnforcementOnDomain",
                            is_enrolled_to_domain);
      first_call = false;
    }
    if (is_enrolled_to_domain)
      return GROUP_NO_ENFORCEMENT;
  }
#endif

  struct {
    const char* group_name;
    SettingsEnforcementGroup group;
  } static const kEnforcementLevelMap[] = {
    { chrome_prefs::internals::kSettingsEnforcementGroupNoEnforcement,
      GROUP_NO_ENFORCEMENT },
    { chrome_prefs::internals::kSettingsEnforcementGroupEnforceOnload,
      GROUP_ENFORCE_ON_LOAD },
    { chrome_prefs::internals::kSettingsEnforcementGroupEnforceAlways,
      GROUP_ENFORCE_ALWAYS },
    { chrome_prefs::internals::
          kSettingsEnforcementGroupEnforceAlwaysWithDSE,
      GROUP_ENFORCE_ALWAYS_WITH_DSE },
    { chrome_prefs::internals::
          kSettingsEnforcementGroupEnforceAlwaysWithExtensionsAndDSE,
      GROUP_ENFORCE_ALWAYS_WITH_EXTENSIONS_AND_DSE },
  };

  // Use the no enforcement setting in the absence of a field trial
  // config. Remember to update the OFFICIAL_BUILD sections of
  // pref_hash_browsertest.cc and extension_startup_browsertest.cc when updating
  // the default value below.
  // TODO(gab): Change this to the strongest enforcement on all platforms.
  SettingsEnforcementGroup enforcement_group = GROUP_NO_ENFORCEMENT;
  bool group_determined_from_trial = false;
  base::FieldTrial* trial =
      base::FieldTrialList::Find(
          chrome_prefs::internals::kSettingsEnforcementTrialName);
  if (trial) {
    const std::string& group_name = trial->group_name();
    // ARRAYSIZE_UNSAFE must be used since the array is declared locally; it is
    // only unsafe because it could not trigger a compile error on some
    // non-array pointer types; this is fine since kEnforcementLevelMap is
    // clearly an array.
    for (size_t i = 0; i < ARRAYSIZE_UNSAFE(kEnforcementLevelMap); ++i) {
      if (kEnforcementLevelMap[i].group_name == group_name) {
        enforcement_group = kEnforcementLevelMap[i].group;
        group_determined_from_trial = true;
        break;
      }
    }
  }
  UMA_HISTOGRAM_BOOLEAN("Settings.EnforcementGroupDeterminedFromTrial",
                        group_determined_from_trial);
  return enforcement_group;
}

// Returns the effective preference tracking configuration.
std::vector<PrefHashFilter::TrackedPreferenceMetadata>
GetTrackingConfiguration() {
  const SettingsEnforcementGroup enforcement_group =
      GetSettingsEnforcementGroup();

  std::vector<PrefHashFilter::TrackedPreferenceMetadata> result;
  for (size_t i = 0; i < arraysize(kTrackedPrefs); ++i) {
    PrefHashFilter::TrackedPreferenceMetadata data = kTrackedPrefs[i];

    if (GROUP_NO_ENFORCEMENT == enforcement_group) {
      // Remove enforcement for all tracked preferences.
      data.enforcement_level = PrefHashFilter::NO_ENFORCEMENT;
    }

    if (enforcement_group >= GROUP_ENFORCE_ALWAYS_WITH_DSE &&
        data.name == DefaultSearchManager::kDefaultSearchProviderDataPrefName) {
      // Specifically enable default search settings enforcement.
      data.enforcement_level = PrefHashFilter::ENFORCE_ON_LOAD;
    }

    if (enforcement_group >= GROUP_ENFORCE_ALWAYS_WITH_EXTENSIONS_AND_DSE &&
        (data.name == extensions::pref_names::kExtensions ||
         data.name == extensions::pref_names::kKnownDisabled)) {
      // Specifically enable extension settings enforcement and ensure
      // kKnownDisabled follows it in the Protected Preferences.
      // TODO(gab): Get rid of kKnownDisabled altogether.
      data.enforcement_level = PrefHashFilter::ENFORCE_ON_LOAD;
    }

    result.push_back(data);
  }
  return result;
}


// Shows notifications which correspond to PersistentPrefStore's reading errors.
void HandleReadError(PersistentPrefStore::PrefReadError error) {
  // Sample the histogram also for the successful case in order to get a
  // baseline on the success rate in addition to the error distribution.
  UMA_HISTOGRAM_ENUMERATION("PrefService.ReadError", error,
                            PersistentPrefStore::PREF_READ_ERROR_MAX_ENUM);

  if (error != PersistentPrefStore::PREF_READ_ERROR_NONE) {
#if !defined(OS_CHROMEOS)
    // Failing to load prefs on startup is a bad thing(TM). See bug 38352 for
    // an example problem that this can cause.
    // Do some diagnosis and try to avoid losing data.
    int message_id = 0;
    if (error <= PersistentPrefStore::PREF_READ_ERROR_JSON_TYPE) {
      message_id = IDS_PREFERENCES_CORRUPT_ERROR;
    } else if (error != PersistentPrefStore::PREF_READ_ERROR_NO_FILE) {
      message_id = IDS_PREFERENCES_UNREADABLE_ERROR;
    }

    if (message_id) {
      BrowserThread::PostTask(BrowserThread::UI, FROM_HERE,
                              base::Bind(&ShowProfileErrorDialog,
                                         PROFILE_ERROR_PREFERENCES,
                                         message_id));
    }
#else
    // On ChromeOS error screen with message about broken local state
    // will be displayed.
#endif
  }
}

scoped_ptr<ProfilePrefStoreManager> CreateProfilePrefStoreManager(
    const base::FilePath& profile_path) {
  std::string device_id;
#if defined(OS_WIN) && defined(ENABLE_RLZ)
  // This is used by
  // chrome/browser/extensions/api/music_manager_private/device_id_win.cc
  // but that API is private (http://crbug.com/276485) and other platforms are
  // not available synchronously.
  // As part of improving pref metrics on other platforms we may want to find
  // ways to defer preference loading until the device ID can be used.
  rlz_lib::GetMachineId(&device_id);
#endif
  return make_scoped_ptr(new ProfilePrefStoreManager(
      profile_path,
      GetTrackingConfiguration(),
      kTrackedPrefsReportingIDsCount,
      ResourceBundle::GetSharedInstance()
          .GetRawDataResource(IDR_PREF_HASH_SEED_BIN)
          .as_string(),
      device_id,
      g_browser_process->local_state()));
}

void PrepareFactory(
    PrefServiceSyncableFactory* factory,
    policy::PolicyService* policy_service,
    ManagedUserSettingsService* managed_user_settings,
    scoped_refptr<PersistentPrefStore> user_pref_store,
    const scoped_refptr<PrefStore>& extension_prefs,
    bool async) {
#if defined(ENABLE_CONFIGURATION_POLICY)
  using policy::ConfigurationPolicyPrefStore;
  factory->set_managed_prefs(
      make_scoped_refptr(new ConfigurationPolicyPrefStore(
          policy_service,
          g_browser_process->browser_policy_connector()->GetHandlerList(),
          policy::POLICY_LEVEL_MANDATORY)));
  factory->set_recommended_prefs(
      make_scoped_refptr(new ConfigurationPolicyPrefStore(
          policy_service,
          g_browser_process->browser_policy_connector()->GetHandlerList(),
          policy::POLICY_LEVEL_RECOMMENDED)));
#endif  // ENABLE_CONFIGURATION_POLICY

#if defined(ENABLE_MANAGED_USERS)
  if (managed_user_settings) {
    factory->set_supervised_user_prefs(
        make_scoped_refptr(new SupervisedUserPrefStore(managed_user_settings)));
  }
#endif

  factory->set_async(async);
  factory->set_extension_prefs(extension_prefs);
  factory->set_command_line_prefs(
      make_scoped_refptr(
          new CommandLinePrefStore(CommandLine::ForCurrentProcess())));
  factory->set_read_error_callback(base::Bind(&HandleReadError));
  factory->set_user_prefs(user_pref_store);
}

// Initialize/update preference hash stores for all profiles but the one whose
// path matches |ignored_profile_path|.
void UpdateAllPrefHashStoresIfRequired(
    const base::FilePath& ignored_profile_path) {
  const ProfileInfoCache& profile_info_cache =
      g_browser_process->profile_manager()->GetProfileInfoCache();
  const size_t n_profiles = profile_info_cache.GetNumberOfProfiles();
  for (size_t i = 0; i < n_profiles; ++i) {
    const base::FilePath profile_path =
        profile_info_cache.GetPathOfProfileAtIndex(i);
    if (profile_path != ignored_profile_path) {
      CreateProfilePrefStoreManager(profile_path)
          ->UpdateProfileHashStoreIfRequired(
              JsonPrefStore::GetTaskRunnerForFile(
                  profile_path, BrowserThread::GetBlockingPool()));
    }
  }
}

}  // namespace

namespace chrome_prefs {

namespace internals {

const char kSettingsEnforcementTrialName[] = "SettingsEnforcement";
const char kSettingsEnforcementGroupNoEnforcement[] = "no_enforcement";
const char kSettingsEnforcementGroupEnforceOnload[] = "enforce_on_load";
const char kSettingsEnforcementGroupEnforceAlways[] = "enforce_always";
const char kSettingsEnforcementGroupEnforceAlwaysWithDSE[] =
    "enforce_always_with_dse";
const char kSettingsEnforcementGroupEnforceAlwaysWithExtensionsAndDSE[] =
    "enforce_always_with_extensions_and_dse";

}  // namespace internals

scoped_ptr<PrefService> CreateLocalState(
    const base::FilePath& pref_filename,
    base::SequencedTaskRunner* pref_io_task_runner,
    policy::PolicyService* policy_service,
    const scoped_refptr<PrefRegistry>& pref_registry,
    bool async) {
  PrefServiceSyncableFactory factory;
  PrepareFactory(
      &factory,
      policy_service,
      NULL,  // managed_user_settings
      new JsonPrefStore(
          pref_filename, pref_io_task_runner, scoped_ptr<PrefFilter>()),
      NULL,  // extension_prefs
      async);
  return factory.Create(pref_registry.get());
}

scoped_ptr<PrefServiceSyncable> CreateProfilePrefs(
    const base::FilePath& profile_path,
    base::SequencedTaskRunner* pref_io_task_runner,
    policy::PolicyService* policy_service,
    ManagedUserSettingsService* managed_user_settings,
    const scoped_refptr<PrefStore>& extension_prefs,
    const scoped_refptr<user_prefs::PrefRegistrySyncable>& pref_registry,
    bool async) {
  TRACE_EVENT0("browser", "chrome_prefs::CreateProfilePrefs");
  PrefServiceSyncableFactory factory;
  PrepareFactory(&factory,
                 policy_service,
                 managed_user_settings,
                 scoped_refptr<PersistentPrefStore>(
                     CreateProfilePrefStoreManager(profile_path)
                         ->CreateProfilePrefStore(pref_io_task_runner)),
                 extension_prefs,
                 async);
  scoped_ptr<PrefServiceSyncable> pref_service =
      factory.CreateSyncable(pref_registry.get());

  ConfigureDefaultSearchPrefMigrationToDictionaryValue(pref_service.get());

  return pref_service.Pass();
}

void SchedulePrefsFilePathVerification(const base::FilePath& profile_path) {
#if defined(OS_WIN)
  // Only do prefs file verification on Windows.
  const int kVerifyPrefsFileDelaySeconds = 60;
  BrowserThread::GetBlockingPool()->PostDelayedTask(
      FROM_HERE,
      base::Bind(&VerifyPreferencesFile,
                 ProfilePrefStoreManager::GetPrefFilePathFromProfilePath(
                     profile_path)),
      base::TimeDelta::FromSeconds(g_disable_delays_and_domain_check_for_testing
                                       ? 0
                                       : kVerifyPrefsFileDelaySeconds));
#endif
}

void DisableDelaysAndDomainCheckForTesting() {
  g_disable_delays_and_domain_check_for_testing = true;
}

void SchedulePrefHashStoresUpdateCheck(
    const base::FilePath& initial_profile_path) {
  if (!ProfilePrefStoreManager::kPlatformSupportsPreferenceTracking) {
    ProfilePrefStoreManager::ResetAllPrefHashStores(
        g_browser_process->local_state());
    return;
  }

  if (GetSettingsEnforcementGroup() >= GROUP_ENFORCE_ALWAYS)
    return;

  const int kDefaultPrefHashStoresUpdateCheckDelaySeconds = 55;
  BrowserThread::PostDelayedTask(
        BrowserThread::UI,
        FROM_HERE,
        base::Bind(&UpdateAllPrefHashStoresIfRequired,
                   initial_profile_path),
        base::TimeDelta::FromSeconds(
            g_disable_delays_and_domain_check_for_testing ?
                0 : kDefaultPrefHashStoresUpdateCheckDelaySeconds));
}

void ResetPrefHashStore(const base::FilePath& profile_path) {
  CreateProfilePrefStoreManager(profile_path)->ResetPrefHashStore();
}

bool InitializePrefsFromMasterPrefs(
    const base::FilePath& profile_path,
    const base::DictionaryValue& master_prefs) {
  return CreateProfilePrefStoreManager(profile_path)
      ->InitializePrefsFromMasterPrefs(master_prefs);
}

base::Time GetResetTime(Profile* profile) {
  return ProfilePrefStoreManager::GetResetTime(profile->GetPrefs());
}

void ClearResetTime(Profile* profile) {
  ProfilePrefStoreManager::ClearResetTime(profile->GetPrefs());
}

void RegisterProfilePrefs(user_prefs::PrefRegistrySyncable* registry) {
  ProfilePrefStoreManager::RegisterProfilePrefs(registry);
}

void RegisterPrefs(PrefRegistrySimple* registry) {
  ProfilePrefStoreManager::RegisterPrefs(registry);
}

}  // namespace chrome_prefs
