#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <ctime>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// Keeps the logs of earlier game sessions.
//
// An SKSE plugin starts its log file anew at every launch, so what a play session logged is gone
// as soon as the game is started again. Before the log is opened, the file of the previous session
// is renamed to "<name>-YYYYMMDD-HHMMSS.log" after the time it was last written, and only the
// newest few of those files are kept.
//
// Nothing here may stop the plugin from loading: every file operation reports through an error
// code, and a log that cannot be moved or an old one that cannot be deleted (open in another
// program, say) is left where it is and named in the result. spdlog's own rotating sink was not
// used for this because it throws when a rename fails, which would end the game at start-up.
namespace LogHistory
{
    struct Result
    {
        std::filesystem::path kept;  // where the previous log went; empty when there was none or it stayed
        std::size_t removed{};       // older logs deleted
        std::string problem;         // what could not be done; empty when nothing went wrong
    };

    // Two logs last written in the same second: the later one gets "-2", "-3", ... up to this.
    inline constexpr int lastSameSecond = 9;

    // "<name>-YYYYMMDD-HHMMSS" for a local time.
    [[nodiscard]] inline std::string DatedStem(std::string_view name, const std::tm& time)
    {
        return std::format("{}-{:04}{:02}{:02}-{:02}{:02}{:02}", name, time.tm_year + 1900, time.tm_mon + 1, time.tm_mday,
            time.tm_hour, time.tm_min, time.tm_sec);
    }

    // Whether `file` is a name this code gives a kept log of `name`: "<name>-########-######.log",
    // with "-N" before the extension when two logs fell into the same second. Anything else in the
    // folder is never touched. `file` is taken in the file system's own characters: the folder also
    // holds other plugins' files, whose names need not exist in the system's narrow code page.
    template <class Char>
    [[nodiscard]] constexpr bool IsKeptLog(std::string_view name, std::basic_string_view<Char> file)
    {
        constexpr std::string_view extension{ ".log" };
        constexpr std::string_view stamp{ "-########-######" };
        const auto is = [](Char actual, char expected) { return actual == static_cast<Char>(expected); };
        const auto digit = [](Char c) { return c >= static_cast<Char>('0') && c <= static_cast<Char>('9'); };
        if (file.size() < name.size() + stamp.size() + extension.size()) {
            return false;
        }
        std::size_t at = 0;
        for (const char c : name) {
            if (!is(file[at++], c)) {
                return false;
            }
        }
        for (const char c : stamp) {
            if (c == '#' ? !digit(file[at]) : !is(file[at], c)) {
                return false;
            }
            ++at;
        }
        for (std::size_t i = 0; i < extension.size(); ++i) {
            if (!is(file[file.size() - extension.size() + i], extension[i])) {
                return false;
            }
        }
        const auto suffix = file.substr(at, file.size() - extension.size() - at);
        return suffix.empty() || (suffix.size() == 2 && is(suffix[0], '-') && suffix[1] >= static_cast<Char>('2') &&
                                     suffix[1] <= static_cast<Char>('0' + lastSameSecond));
    }

    [[nodiscard]] constexpr bool IsKeptLog(std::string_view name, std::string_view file)
    {
        return IsKeptLog<char>(name, file);
    }

    // Moves <directory>/<name>.log aside under a dated name and deletes all but the newest `keep`
    // kept logs. Call before the log is opened. With `keep` zero nothing is kept and nothing is
    // touched: the log is overwritten as it would be without this.
    [[nodiscard]] inline Result Keep(const std::filesystem::path& directory, std::string_view name, std::size_t keep) noexcept
    {
        Result result{};
        if (keep == 0) {
            return result;
        }
        try {
            std::error_code error;
            const auto current = directory / std::format("{}.log", name);
            const auto size = std::filesystem::file_size(current, error);
            if (!error && size > 0) {
                const auto written = std::filesystem::last_write_time(current, error);
                if (error) {
                    result.problem = "the time of the previous log could not be read: " + error.message();
                } else {
                    // The file clock has no portable conversion that cannot throw; the difference to
                    // "now" on both clocks is exact enough for a name.
                    const auto system = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                        written - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
                    const std::time_t seconds = std::chrono::system_clock::to_time_t(system);
                    std::tm local{};
                    if (localtime_s(&local, &seconds) != 0) {
                        result.problem = "the time of the previous log could not be converted";
                    } else {
                        const auto stem = DatedStem(name, local);
                        for (int attempt = 1; attempt <= lastSameSecond; ++attempt) {
                            const auto target = directory / (attempt == 1 ? stem + ".log" : std::format("{}-{}.log", stem, attempt));
                            if (std::filesystem::exists(target, error)) {
                                continue;
                            }
                            std::filesystem::rename(current, target, error);
                            if (error) {
                                result.problem = "the previous log could not be renamed: " + error.message();
                            } else {
                                result.kept = target;
                            }
                            break;
                        }
                        if (result.kept.empty() && result.problem.empty()) {
                            result.problem = "no free name for the previous log";
                        }
                    }
                }
            }

            // Names are compared as the file system has them; converting every name in the folder to
            // the narrow code page would throw at the first one that has no such form.
            using Name = std::filesystem::path::string_type;
            std::vector<std::filesystem::path> kept;
            error.clear();
            for (std::filesystem::directory_iterator entry(directory, error), end; !error && entry != end; entry.increment(error)) {
                const Name file = entry->path().filename().native();
                std::error_code fileError;
                if (IsKeptLog<Name::value_type>(name, file) && entry->is_regular_file(fileError) && !fileError) {
                    kept.push_back(entry->path());
                }
            }
            // Without the extension the names sort by time, a "-2" after the log of the same second.
            // The newest come first.
            std::ranges::sort(kept, std::ranges::greater{}, [](const std::filesystem::path& path) { return path.stem().native(); });
            for (std::size_t i = keep; i < kept.size(); ++i) {
                if (std::filesystem::remove(kept[i], error) && !error) {
                    ++result.removed;
                } else if (result.problem.empty()) {
                    // A kept log's name is plain ASCII (IsKeptLog), so this conversion cannot fail.
                    result.problem = "an old log could not be deleted: " + kept[i].filename().string();
                }
            }
        } catch (...) {
            // Out of memory: the logs stay as they are.
            if (result.problem.empty()) {
                result.problem = "keeping the earlier logs was given up";
            }
        }
        return result;
    }
}
