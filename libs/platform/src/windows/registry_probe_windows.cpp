#include <windows.h>

#include <memory>
#include <string>
#include <vector>

#include "lm/platform/probes.hpp"

namespace lm::platform {
namespace {

HKEY root_key(core::RegistryHive hive) {
    switch (hive) {
        case core::RegistryHive::LocalMachine: return HKEY_LOCAL_MACHINE;
        case core::RegistryHive::CurrentUser:  return HKEY_CURRENT_USER;
        case core::RegistryHive::ClassesRoot:  return HKEY_CLASSES_ROOT;
        case core::RegistryHive::Users:        return HKEY_USERS;
    }
    return HKEY_LOCAL_MACHINE;
}

std::string format_error(LSTATUS status) {
    char* buffer = nullptr;
    const DWORD length = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(status), 0, reinterpret_cast<char*>(&buffer), 0, nullptr);
    if (length == 0 || buffer == nullptr) {
        return "registry error " + std::to_string(status);
    }
    std::string message(buffer, length);
    LocalFree(buffer);
    while (!message.empty() && (message.back() == '\r' || message.back() == '\n')) {
        message.pop_back();
    }
    return message;
}

/// Renders a raw registry payload as the text a rule author would write in an
/// Equals/Contains comparison. Numeric types become plain decimal; REG_MULTI_SZ
/// joins its members with "; " so a Contains rule can match one entry.
std::string render(DWORD type, const std::vector<BYTE>& bytes) {
    switch (type) {
        case REG_DWORD: {
            DWORD value = 0;
            if (bytes.size() >= sizeof(value)) {
                std::memcpy(&value, bytes.data(), sizeof(value));
            }
            return std::to_string(value);
        }
        case REG_QWORD: {
            std::uint64_t value = 0;
            if (bytes.size() >= sizeof(value)) {
                std::memcpy(&value, bytes.data(), sizeof(value));
            }
            return std::to_string(value);
        }
        case REG_SZ:
        case REG_EXPAND_SZ:
        case REG_MULTI_SZ: {
            // RRF_RT_ANY returns wide characters because we call the W API.
            const auto* wide = reinterpret_cast<const wchar_t*>(bytes.data());
            const std::size_t wide_count = bytes.size() / sizeof(wchar_t);

            std::string out;
            std::size_t start = 0;
            for (std::size_t i = 0; i < wide_count; ++i) {
                if (wide[i] != L'\0') {
                    continue;
                }
                if (i > start) {
                    const int needed = WideCharToMultiByte(CP_UTF8, 0, wide + start,
                                                           static_cast<int>(i - start), nullptr, 0,
                                                           nullptr, nullptr);
                    if (needed > 0) {
                        std::string piece(static_cast<std::size_t>(needed), '\0');
                        WideCharToMultiByte(CP_UTF8, 0, wide + start, static_cast<int>(i - start),
                                            piece.data(), needed, nullptr, nullptr);
                        if (!out.empty()) {
                            out += "; ";
                        }
                        out += piece;
                    }
                }
                start = i + 1;
                if (type != REG_MULTI_SZ) {
                    break;  // a single string ends at its first terminator
                }
            }
            return out;
        }
        default:
            // Binary and unknown types have no sensible textual form; report the
            // size so a rule author can see the value exists without pretending
            // it is comparable.
            return "<" + std::to_string(bytes.size()) + "-byte binary value>";
    }
}

std::wstring widen(const std::string& text) {
    if (text.empty()) {
        return std::wstring{};
    }
    const int needed =
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) {
        return std::wstring{};
    }
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), needed);
    return out;
}

class WindowsRegistryProbe : public IRegistryProbe {
public:
    core::RegistryValue read(const core::RegistryRule& rule) override {
        core::RegistryValue result;

        const std::wstring key_path = widen(rule.key_path);
        const std::wstring value_name = widen(rule.value_name);

        // RRF_RT_ANY accepts any value type; RRF_NOEXPAND leaves REG_EXPAND_SZ
        // unexpanded so a rule matches what is actually stored rather than what
        // this particular machine's environment expands it to.
        DWORD type = 0;
        DWORD size = 0;
        LSTATUS status = RegGetValueW(root_key(rule.hive), key_path.c_str(),
                                      rule.value_name.empty() ? nullptr : value_name.c_str(),
                                      RRF_RT_ANY | RRF_NOEXPAND, &type, nullptr, &size);

        if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) {
            return result;  // absent, not an error
        }
        if (status != ERROR_SUCCESS) {
            result.error = format_error(status);
            return result;
        }

        std::vector<BYTE> bytes(size);
        status = RegGetValueW(root_key(rule.hive), key_path.c_str(),
                              rule.value_name.empty() ? nullptr : value_name.c_str(),
                              RRF_RT_ANY | RRF_NOEXPAND, &type, bytes.data(), &size);

        if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) {
            return result;  // raced with a delete between the two calls
        }
        if (status != ERROR_SUCCESS) {
            result.error = format_error(status);
            return result;
        }

        bytes.resize(size);
        result.exists = true;
        result.data = render(type, bytes);
        return result;
    }
};

}  // namespace

std::unique_ptr<IRegistryProbe> make_registry_probe() {
    return std::make_unique<WindowsRegistryProbe>();
}

}  // namespace lm::platform
