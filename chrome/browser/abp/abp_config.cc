// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/abp/abp_config.h"

#include <optional>

#include "base/command_line.h"
#include "base/environment.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/uuid.h"
#include "build/build_config.h"
#include "chrome/browser/abp/abp_switches.h"
#include "chrome/common/chrome_paths.h"

namespace abp {

namespace {

// Expands ~ to home directory in path strings
base::FilePath ExpandPath(const std::string& path_str) {
  if (path_str.empty()) {
    return base::FilePath();
  }

  std::string expanded = path_str;
  if (base::StartsWith(expanded, "~/", base::CompareCase::SENSITIVE)) {
    base::FilePath home_dir;
    if (base::PathService::Get(base::DIR_HOME, &home_dir)) {
      expanded = home_dir.AsUTF8Unsafe() + expanded.substr(1);
    }
  }
  return base::FilePath::FromUTF8Unsafe(expanded);
}

base::FilePath GetDefaultConfigDir() {
  base::FilePath config_dir;
  if (base::PathService::Get(chrome::DIR_USER_DATA, &config_dir)) {
    return config_dir;
  }
#if BUILDFLAG(IS_WIN)
  // Fallback to %LOCALAPPDATA%\Chromium
  base::FilePath local_app_data;
  if (base::PathService::Get(base::DIR_LOCAL_APP_DATA, &local_app_data)) {
    return local_app_data.AppendASCII("Chromium");
  }
#else
  // Fallback to ~/.config/chromium
  base::FilePath home_dir;
  if (base::PathService::Get(base::DIR_HOME, &home_dir)) {
    return home_dir.AppendASCII(".config").AppendASCII("chromium");
  }
#endif
  return base::FilePath();
}

// Reads a millisecond timing value from env var, then command-line switch.
// Switch takes priority over env var. Returns nullopt if neither is set.
std::optional<base::TimeDelta> ReadTimingMs(const char* env_name,
                                             const char* switch_name) {
  auto* command_line = base::CommandLine::ForCurrentProcess();
  auto env = base::Environment::Create();

  // Check env var first (lower priority)
  std::optional<base::TimeDelta> result;
  std::string env_val;
  if (env->GetVar(env_name, &env_val)) {
    int ms = 0;
    if (base::StringToInt(env_val, &ms) && ms >= 0) {
      result = base::Milliseconds(ms);
    }
  }

  // Switch overrides env var
  if (command_line->HasSwitch(switch_name)) {
    std::string switch_val = command_line->GetSwitchValueASCII(switch_name);
    int ms = 0;
    if (base::StringToInt(switch_val, &ms) && ms >= 0) {
      result = base::Milliseconds(ms);
    }
  }

  return result;
}

}  // namespace

// static
AbpConfig AbpConfig::GetDefaults() {
  // Generate UUID for session directory
  std::string uuid = base::Uuid::GenerateRandomV4().AsLowercaseString();
  base::FilePath temp_dir;
  if (!base::GetTempDir(&temp_dir)) {
    // Fallback - should not happen in normal circumstances
    temp_dir = base::FilePath::FromUTF8Unsafe("/tmp");
  }
  base::FilePath session_dir = temp_dir.AppendASCII("abp-" + uuid);

  return GetDefaultsWithSessionDir(session_dir);
}

// static
AbpConfig AbpConfig::GetDefaultsWithSessionDir(
    const base::FilePath& session_dir) {
  AbpConfig config;
  config.session_dir = session_dir;

  config.history.enabled = true;
  config.history.database_path = session_dir.AppendASCII("history.db");
  config.history.screenshots.enabled = true;
  config.history.screenshots.directory = session_dir.AppendASCII("screenshots");

  return config;
}

AbpConfig LoadAbpConfigFromFile(const base::FilePath& config_path) {
  AbpConfig config = AbpConfig::GetDefaults();

  if (config_path.empty() || !base::PathExists(config_path)) {
    return config;
  }

  std::string contents;
  if (!base::ReadFileToString(config_path, &contents)) {
    LOG(WARNING) << "ABP: Failed to read config file: " << config_path;
    return config;
  }

  auto parsed = base::JSONReader::ReadAndReturnValueWithError(
      contents, base::JSON_PARSE_RFC);
  if (!parsed.has_value()) {
    LOG(WARNING) << "ABP: Failed to parse config file: " << parsed.error().message;
    return config;
  }

  if (!parsed->is_dict()) {
    LOG(WARNING) << "ABP: Config file must be a JSON object";
    return config;
  }

  const base::Value::Dict& root = parsed->GetDict();

  // Parse history section
  const base::Value::Dict* history = root.FindDict("history");
  if (history) {
    // history.enabled
    if (auto enabled = history->FindBool("enabled")) {
      config.history.enabled = *enabled;
    }

    // history.database_path
    if (const std::string* db_path = history->FindString("database_path")) {
      base::FilePath expanded = ExpandPath(*db_path);
      if (!expanded.empty()) {
        config.history.database_path = expanded;
      }
    }

    // history.screenshots section
    const base::Value::Dict* screenshots = history->FindDict("screenshots");
    if (screenshots) {
      // screenshots.enabled
      if (auto enabled = screenshots->FindBool("enabled")) {
        config.history.screenshots.enabled = *enabled;
      }

      // screenshots.directory
      if (const std::string* dir = screenshots->FindString("directory")) {
        base::FilePath expanded = ExpandPath(*dir);
        if (!expanded.empty()) {
          config.history.screenshots.directory = expanded;
        }
      }
    }
  }

  VLOG(1) << "ABP: Loaded config from " << config_path;
  return config;
}

void ApplyTimingOverrides(AbpConfig& config) {
  if (auto v = ReadTimingMs("ABP_MIN_WAIT", switches::kAbpMinWait))
    config.timing.min_wait = *v;
  if (auto v = ReadTimingMs("ABP_TRACKING_TIMEOUT", switches::kAbpTrackingTimeout))
    config.timing.tracking_timeout = *v;
  if (auto v = ReadTimingMs("ABP_POST_SETTLE", switches::kAbpPostSettle))
    config.timing.post_settle = *v;
}

AbpConfig LoadAbpConfig() {
  const base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();

  // Check --abp-session-dir flag for custom session directory
  base::FilePath session_dir;
  if (command_line->HasSwitch(switches::kAbpSessionDir)) {
    session_dir = command_line->GetSwitchValuePath(switches::kAbpSessionDir);
    // Resolve to absolute path now (during startup) since blocking filesystem
    // calls cannot run on the UI thread after DisallowBlocking is set.
    // MakeAbsoluteFilePath requires the path to exist (uses realpath), so
    // prepend the current directory for relative paths instead.
    if (!session_dir.empty() && !session_dir.IsAbsolute()) {
      base::FilePath cwd;
      if (base::GetCurrentDirectory(&cwd)) {
        session_dir = cwd.Append(session_dir);
      }
    }
  }

  // Check --abp-config flag for config file
  if (command_line->HasSwitch(switches::kAbpConfig)) {
    base::FilePath config_path =
        command_line->GetSwitchValuePath(switches::kAbpConfig);
    AbpConfig config = LoadAbpConfigFromFile(config_path);
    // Override session_dir if specified via command line
    if (!session_dir.empty()) {
      config.session_dir = session_dir;
      config.history.database_path = session_dir.AppendASCII("history.db");
      config.history.screenshots.directory = session_dir.AppendASCII("screenshots");
    }
    ApplyTimingOverrides(config);
    return config;
  }

  // Try default config location
  base::FilePath config_dir = GetDefaultConfigDir();
  if (!config_dir.empty()) {
    base::FilePath default_config = config_dir.AppendASCII("abp_config.json");
    if (base::PathExists(default_config)) {
      AbpConfig config = LoadAbpConfigFromFile(default_config);
      // Override session_dir if specified via command line
      if (!session_dir.empty()) {
        config.session_dir = session_dir;
        config.history.database_path = session_dir.AppendASCII("history.db");
        config.history.screenshots.directory = session_dir.AppendASCII("screenshots");
      }
      ApplyTimingOverrides(config);
      return config;
    }
  }

  // Return defaults with custom session dir if provided
  if (!session_dir.empty()) {
    AbpConfig config = AbpConfig::GetDefaultsWithSessionDir(session_dir);
    ApplyTimingOverrides(config);
    return config;
  }
  AbpConfig config = AbpConfig::GetDefaults();
  ApplyTimingOverrides(config);
  return config;
}

}  // namespace abp
