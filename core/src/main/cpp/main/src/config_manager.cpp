//
// Created by solo on 2019/5/31.
//

#include <cstdio>
#include <dirent.h>
#include <unistd.h>
#include <jni.h>
#include <cstdlib>
#include <array>
#include <thread>
#include <vector>
#include <string>

#include <logging.h>
#include <climits>
#include <fstream>
#include <sstream>
#include "art/runtime/native/native_util.h"
#include "config_manager.h"
#include "utils.h"
#include "rirud_socket.h"

/*
 * Logic:
 * check if /data/adb/lspd exists and is readable, if so, read misc_path and the config base path is
 * /data/misc/$misc_path; if not so, fall back to the installer's path
 * config manager is const. and it should be updated if the module list is updated
 * each user owns one config manager
 * before the process started, update current user
 * module list and modules scopes are preloaded
 * can get app module list by call a const function
 * for installer's pkg name, it's retrieved by /data/misc/$misc_path/$uid/conf/installer.
 * if not exists, fallback
 * blacklist and whitelist are instead set by blacklist.conf and whitelist.conf respectively
 * dynamic mode is always on
 *
 * Permission:
 * /data/adb/lspd should be accessible by zygote by sepolicy
 * /data/misc/$misc_path is random path, and mounted by magisk
 * it should have context `u:object_r:magisk_file:s0`, which should be readable by normal app
 * and zygote
 *
 * /data/misc/$misc_path's owner should be root:root, with permission 771
 * so does /data/misc/$misc_path/$uid
 * /data/misc/$misc_path/$uid/conf should be um:root, with permission 770
 * where um is the user of manager
 * /data/misc/$misc_path/$uid/prefs should be root:root, with permission 771
 * /data/misc/$misc_path/$uid/prefs/$pkg should be up:root, with permission 771
 * this path is used for XSharePreference, where up is the user of package $pkg
 * other's permission is 5 because it should be read by all the other apps but not writable
 * it's only writeable by $pkg itself
 * root group's 7 permission's for updating the permission & deleting it
 *
 * Initialization:
 * This is only done for config path in /data/misc
 * check if /data/misc/$misc_path/$uid exists. if not create one with 771
 * check if /data/misc/$misc_path/$uid/conf exists. if not create one with 770
 * check if /data/misc/$misc_path/$uid/prefs exists. if not create one with 771
 *
 *
 * when the launching app is installer, change the user owner of /data/misc/$misc_path/$uid/conf
 * to be the installer, and change the permission to be 770
 *
 * when the launching app is in the module lists, ensure existence of
 * /data/misc/$misc_path/$uid/prefs/$pkg
 * with the owner the user of the pkg and permission 774
 * if this paths exists but with different owner, delete it recursively and create a new one
 * this is when package reinstalled
 * but I heard that some devices change uid when upgrading system, so I didn't do this
 *
 * Side effect:
 * data exists if the module uninstalled
 * One way to release the storage space is to uninstall lspd
 * because /data/misc/$misc_path is mounted by magisk.
 *
 * lspd works without manager
 *
 * uninstall removes all configs (this can be restored if manager store another copy
 * of the conf on its own data dir)
 *
 */

namespace lspd {
    namespace fs = std::filesystem;

    fs::path ConfigManager::RetrieveBaseConfigPath() const {
        if (!misc_path_.empty()) {
            return misc_path_ / std::to_string(user_);
        } else {
            return {};
        }
    }

    std::string ConfigManager::RetrieveInstallerPkgName() const {
        std::string installer_pkg_name_path = GetConfigPath("installer");
        if (!path_exists<true>(installer_pkg_name_path)) {
            LOGW("installer not set, using default one %s", kPrimaryInstallerPkgName.c_str());
            return kPrimaryInstallerPkgName;
        }
        std::ifstream ifs(installer_pkg_name_path);
        if (!ifs.good()) {
            LOGW("cannot access %s, using default one %s", installer_pkg_name_path.c_str(),
                 kPrimaryInstallerPkgName.c_str());
            return kPrimaryInstallerPkgName;
        }
        return {std::istream_iterator<char>(ifs), std::istream_iterator<char>()};
    }

    std::unordered_set<std::string> ConfigManager::GetAppList(const fs::path &dir) {
        std::unordered_set<std::string> set;
        try {
            for (auto &item: fs::directory_iterator(dir)) {
                if (item.is_regular_file()) {
                    set.emplace(item.path().filename());
                }
            }
        } catch (const fs::filesystem_error &e) {
            LOGE("%s", e.what());
        }
        return set;
    }

    std::string ConfigManager::GetPackageNameFromBaseApkPath(const fs::path &path) {
        std::vector<std::string> paths(path.begin(), path.end());
        if (paths.empty()) return {};
        auto base_apk = paths.back(); // base.apk
        if (base_apk != "base.apk") return {};
        paths.pop_back();
        auto pkg_name_with_obfuscation = paths.back();
        if (auto pos = pkg_name_with_obfuscation.find('-'); pos != std::string::npos) {
            return pkg_name_with_obfuscation.substr(0, pos);
        }
        return {};
    }

    bool ConfigManager::IsAppNeedHook(const std::string &package_name) const {
        if (IsInstaller(package_name)) {
            return true;
        }

        auto res = white_list_.count(package_name);
        LOGD("should hook %s -> %s", package_name.c_str(), BoolToString(res));
        return res;
    }

    ConfigManager::ConfigManager(uid_t user, bool initialized) :
            user_(user),
            data_path_prefix_(fs::path("/data/user_de") /
                              std::to_string(user_)),
            base_config_path_(RetrieveBaseConfigPath()),
            initialized_(initialized || InitConfigPath()),
            installer_pkg_name_(RetrieveInstallerPkgName()),
            deopt_boot_image_enabled_(path_exists(GetConfigPath("deoptbootimage"))),
            no_module_log_enabled_(path_exists(GetConfigPath("disable_modules_log"))),
            resources_hook_enabled_(path_exists(GetConfigPath("enable_resources"))),
            white_list_(GetAppList(GetConfigPath("whitelist/"))),
            modules_list_(GetModuleList()),
            last_write_time_(GetLastWriteTime()),
            variant_(GetVariant(GetMiscPath() / "variant")) {
        // use_white_list snapshot
        LOGI("base config path: %s", base_config_path_.c_str());
        LOGI("  using installer package name: %s", installer_pkg_name_.c_str());
        LOGI("  deopt boot image: %s", BoolToString(deopt_boot_image_enabled_));
        LOGI("  no module log: %s", BoolToString(no_module_log_enabled_));
        LOGI("  resources hook: %s", BoolToString(resources_hook_enabled_));
        LOGI("  white list: \n %s", ([this]() {
            std::ostringstream join;
            std::copy(white_list_.begin(), white_list_.end(),
                      std::ostream_iterator<std::string>(join, "\n"));
            return join.str();
        })().c_str());
        LOGI("  module list: \n %s", ([this]() {
            std::ostringstream join;
            std::vector<std::string> module_list;
            std::transform(modules_list_.begin(), modules_list_.end(),
                           std::back_inserter(module_list),
                           [](auto i) { return i.first; });
            std::copy(module_list.begin(), module_list.end(),
                      std::ostream_iterator<std::string>(join, "\n"));
            return join.str();
        })().c_str());
    }

    int ConfigManager::GetVariant(const fs::path &dir) {
        std::ifstream ifs(dir);
        if (!ifs.good()) {
            return 0;
        }
        int variant;
        ifs >> variant;
        return variant;
    }

    auto ConfigManager::GetModuleList() -> std::remove_const_t<decltype(modules_list_)> {
        std::remove_const_t<decltype(modules_list_)> modules_list;
        auto global_modules_list = GetConfigPath("modules.list");
        if (!path_exists(global_modules_list)) {
            LOGE("Cannot access path %s", global_modules_list.c_str());
            return modules_list;
        }
        std::ifstream ifs(global_modules_list);
        if (!ifs.good()) {
            LOGE("Cannot access path %s", global_modules_list.c_str());
            return modules_list;
        }
        std::string module;
        while (std::getline(ifs, module)) {
            const auto &module_pkg_name = GetPackageNameFromBaseApkPath(module);
            auto &[module_path, scope] = modules_list[module_pkg_name];
            scope.insert(module_pkg_name); // Always add module itself
            module_path.assign(std::move(module));
            const auto &module_scope_conf = GetConfigPath(module_pkg_name + ".conf");
            if (!path_exists<true>(module_scope_conf)) {
                LOGD("module scope is not set for %s", module_pkg_name.c_str());
                continue;
            }
            std::ifstream ifs_c(module_scope_conf);
            if (!ifs_c.good()) {
                LOGE("Cannot access path %s", module_scope_conf.c_str());
                continue;
            }
            std::string app_pkg_name;
            while (std::getline(ifs_c, app_pkg_name)) {
                if (!app_pkg_name.empty())
                    scope.emplace(std::move(app_pkg_name));
            }
            if (IsInstaller(module_pkg_name)) scope.erase("android");
            LOGI("scope of %s is:\n%s", module_pkg_name.c_str(), ([&scope = scope]() {
                std::ostringstream join;
                std::copy(scope.begin(), scope.end(),
                          std::ostream_iterator<std::string>(join, "\n  "));
                return join.str();
            })().c_str());
        }
        return modules_list;
    }

    std::vector<std::string> ConfigManager::GetAppModuleList(const std::string &pkg_name) const {
        std::vector<std::string> app_modules_list;
        for (const auto&[module, scope]: modules_list_) {
            if (scope.second.count(pkg_name))
                app_modules_list.push_back(scope.first);
        }
        return app_modules_list;
    }

    fs::file_time_type ConfigManager::GetLastWriteTime() const {
        auto config_path = GetConfigPath();
        auto list_path = GetConfigPath("modules.list");
        auto blacklist_path = GetConfigPath("blacklist");
        auto whitelist_path = GetConfigPath("whitelist");
        return std::max({path_exists<true>(config_path) ? fs::last_write_time(config_path)
                                                        : fs::file_time_type{},
                         path_exists<true>(list_path) ? fs::last_write_time(list_path)
                                                      : fs::file_time_type{},
                         path_exists<true>(blacklist_path) ? fs::last_write_time(blacklist_path)
                                                           : fs::file_time_type{},
                         path_exists<true>(whitelist_path) ? fs::last_write_time(whitelist_path)
                                                           : fs::file_time_type{}});
    }

    bool ConfigManager::InitConfigPath() const {
        if (base_config_path_.empty()) return false;
        try {
            fs::create_directories(base_config_path_);
            fs::permissions(misc_path_,
                            fs::perms::owner_all | fs::perms::group_all | fs::perms::others_exec);
            fs::permissions(base_config_path_,
                            fs::perms::owner_all | fs::perms::group_all | fs::perms::others_exec);
            fs::create_directories(GetLogPath());
            fs::permissions(GetLogPath(),
                            fs::perms::owner_all | fs::perms::group_all | fs::perms::others_exec);
            fs::create_directories(GetConfigPath());
            fs::permissions(GetConfigPath(), fs::perms::owner_all | fs::perms::group_all);
            fs::create_directories(GetPrefsPath(""));
            fs::permissions(GetPrefsPath(""),
                            fs::perms::owner_all | fs::perms::group_all | fs::perms::others_exec);
        } catch (const fs::filesystem_error &e) {
            LOGE("init: %s", e.what());
            return false;
        }
        return true;
    }

    void ConfigManager::EnsurePermission(const std::string &pkg_name, uid_t uid) const {
        if (!initialized_) return;
        try {
            if (modules_list_.count(pkg_name)) {
                auto prefs_path = GetPrefsPath(pkg_name);
                if (!path_exists<true>(prefs_path)) {
                    fs::create_directories(prefs_path);
                } else {
                    const auto &[r_uid, r_gid] = path_own(prefs_path);
                    if (r_uid != uid) {
                        fs::remove_all(prefs_path);
                        fs::create_directories(prefs_path);
                    }
                }
                fs::permissions(prefs_path, fs::perms::owner_all | fs::perms::group_all |
                                            fs::perms::others_exec | fs::perms::set_gid);
                if (const auto &[r_uid, r_gid] = path_own(prefs_path);
                        (uid != -1 && r_uid != uid) || r_gid != 1000u) {
                    path_chown(prefs_path, uid, 1000u, false);
                }
            }
            if (IsInstaller(pkg_name) || pkg_name == "android") {
                auto conf_path = GetConfigPath();
                if (!path_exists<true>(conf_path)) {
                    fs::create_directories(conf_path);
                }
                auto log_path = GetLogPath();
                if (!path_exists<true>(log_path)) {
                    fs::create_directories(log_path);
                }
                fs::permissions(conf_path, fs::perms::owner_all | fs::perms::group_all | fs::perms::set_gid);
                fs::permissions(log_path, fs::perms::owner_all | fs::perms::group_all | fs::perms::set_gid);
                if (pkg_name == "android") uid = -1;
                path_chown(conf_path, uid, 1000u, true);
                path_chown(log_path, uid, 1000u, true);
                if (current_user_ == 0) {
                    auto variant = GetMiscPath() / "variant";
                    fs::permissions(variant, fs::perms::owner_all | fs::perms::group_all);
                    chown(variant.c_str(), uid, 1000u);
                }

                if (pkg_name == kPrimaryInstallerPkgName) {
                    umask(0007);
                    auto installer_pkg_name_path = GetConfigPath("installer");
                    if (path_exists<true>(installer_pkg_name_path)) {
                        fs::remove(installer_pkg_name_path);
                    }
                }
            }
        } catch (const fs::filesystem_error &e) {
            LOGE("%s", e.what());
        }
    }

    void ConfigManager::Init() {
        fs::path misc_path("/data/adb/lspd/misc_path");
        try {
            RirudSocket rirud_socket{};
            auto path = rirud_socket.ReadFile(misc_path);
            path.erase(std::find_if(path.rbegin(), path.rend(), [](unsigned char ch) {
                return !std::isspace(ch);
            }).base(), path.end());
            misc_path_ = fs::path("/data/misc") / path;
            inject_dex_path_ = GetFrameworkPath(kXposedInjectDexPath);
            LOGI("Got base config path: %s", misc_path_.c_str());
        } catch (const RirudSocket::RirudSocketException &e) {
            LOGE("%s", e.what());
        }
    }
}