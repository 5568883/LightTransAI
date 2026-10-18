#include "input/hotkey_manager.h"

#include "utils/logger.h"
#include "utils/string_utils.h"

namespace lt {

std::wstring HotkeyManager::Format(unsigned int mods, unsigned int vk) {
    std::wstring s;
    if (mods & MOD_CONTROL) s += L"Ctrl+";
    if (mods & MOD_ALT)     s += L"Alt+";
    if (mods & MOD_SHIFT)   s += L"Shift+";
    if (mods & MOD_WIN)     s += L"Win+";

    if (vk == 0) return s;

    // 功能键
    if (vk >= VK_F1 && vk <= VK_F24) {
        s += L"F" + std::to_wstring(vk - VK_F1 + 1);
        return s;
    }

    switch (vk) {
        case VK_SPACE:  s += L"空格";  return s;
        case VK_RETURN: s += L"回车";  return s;
        case VK_TAB:    s += L"Tab";   return s;
        case VK_ESCAPE: s += L"Esc";   return s;
        case VK_HOME:   s += L"Home";  return s;
        case VK_END:    s += L"End";   return s;
        case VK_PRIOR:  s += L"PageUp";   return s;
        case VK_NEXT:   s += L"PageDown"; return s;
        default: break;
    }

    // 字母 / 数字
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        s.push_back(static_cast<wchar_t>(vk));
        return s;
    }

    // 其余：用 GetKeyNameText 取系统本地化名称
    const UINT scancode = ::MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    wchar_t name[64]{};
    if (scancode != 0 &&
        ::GetKeyNameTextW(static_cast<LONG>(scancode << 16), name,
                          static_cast<int>(std::size(name))) > 0) {
        s += name;
    } else {
        s += L"键码" + std::to_wstring(vk);
    }
    return s;
}

std::wstring HotkeyManager::Format(const HotkeySpec& spec) {
    return Format(spec.mods, spec.vk);
}

bool HotkeyManager::IsSystemReserved(unsigned int mods, unsigned int vk) {
    // Alt+Tab / Alt+Esc / Alt+F4 / Ctrl+Esc 等由系统处理，RegisterHotKey 不会成功
    if (mods == MOD_ALT) {
        if (vk == VK_TAB || vk == VK_ESCAPE || vk == VK_F4) return true;
    }
    if (mods == MOD_CONTROL && vk == VK_ESCAPE) return true;
    return false;
}

namespace {

// 去掉首尾空白与全角空格
std::wstring TrimToken(const std::wstring& s) {
    const std::wstring blanks = L" \t\r\n\u3000";
    const size_t b = s.find_first_not_of(blanks);
    if (b == std::wstring::npos) return L"";
    const size_t e = s.find_last_not_of(blanks);
    return s.substr(b, e - b + 1);
}

// 不区分大小写的 ASCII 比较（键名都是 ASCII，用户可能输入 alt / ALT / Alt）
bool EqualsIgnoreCase(const std::wstring& a, const wchar_t* b) {
    size_t i = 0;
    for (; i < a.size(); ++i) {
        if (b[i] == L'\0') return false;
        wchar_t ca = a[i], cb = b[i];
        if (ca >= L'A' && ca <= L'Z') ca = static_cast<wchar_t>(ca - L'A' + L'a');
        if (cb >= L'A' && cb <= L'Z') cb = static_cast<wchar_t>(cb - L'A' + L'a');
        if (ca != cb) return false;
    }
    return b[i] == L'\0';
}

} // namespace

bool HotkeyManager::Parse(const std::wstring& text, unsigned int& mods,
                          unsigned int& vk, std::wstring& error) {
    mods = 0;
    vk   = 0;
    error.clear();

    const std::wstring trimmed = TrimToken(text);
    if (trimmed.empty()) {
        error = L"快捷键不能为空（例如 Alt+E）。";
        return false;
    }

    // 按 '+' 切分。全角 '＋' 也接受，避免中文输入法状态下输入失败。
    std::vector<std::wstring> tokens;
    std::wstring cur;
    for (const wchar_t c : trimmed) {
        if (c == L'+' || c == L'\uFF0B') {
            tokens.push_back(TrimToken(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    tokens.push_back(TrimToken(cur));

    for (const std::wstring& t : tokens) {
        if (t.empty()) {
            error = L"快捷键格式不正确：“+ ”两侧都必须有内容（例如 Alt+E）。";
            return false;
        }
        if (EqualsIgnoreCase(t, L"ctrl") || EqualsIgnoreCase(t, L"control") ||
            t == L"控制") {
            mods |= MOD_CONTROL;
        } else if (EqualsIgnoreCase(t, L"alt")) {
            mods |= MOD_ALT;
        } else if (EqualsIgnoreCase(t, L"shift")) {
            mods |= MOD_SHIFT;
        } else if (EqualsIgnoreCase(t, L"win") || EqualsIgnoreCase(t, L"windows")) {
            mods |= MOD_WIN;
        } else {
            // 非修饰键：只允许出现一个，且必须是最后一个语义上的主键
            if (vk != 0) {
                error = L"快捷键 “" + trimmed + L"” 里出现了多个主键。";
                return false;
            }
            // 功能键 F1~F24
            if (t.size() >= 2 && (t[0] == L'F' || t[0] == L'f')) {
                const std::wstring num = t.substr(1);
                bool allDigit = !num.empty();
                for (const wchar_t c : num) {
                    if (c < L'0' || c > L'9') { allDigit = false; break; }
                }
                if (allDigit) {
                    const int n = std::stoi(num);
                    if (n >= 1 && n <= 24) {
                        vk = static_cast<unsigned int>(VK_F1 + n - 1);
                        continue;
                    }
                    error = L"功能键只支持 F1~F24。";
                    return false;
                }
            }
            if (t == L"空格" || EqualsIgnoreCase(t, L"space")) {
                vk = VK_SPACE;
            } else if (t == L"回车" || EqualsIgnoreCase(t, L"enter") ||
                       EqualsIgnoreCase(t, L"return")) {
                vk = VK_RETURN;
            } else if (EqualsIgnoreCase(t, L"tab")) {
                vk = VK_TAB;
            } else if (EqualsIgnoreCase(t, L"esc") || EqualsIgnoreCase(t, L"escape")) {
                vk = VK_ESCAPE;
            } else if (EqualsIgnoreCase(t, L"home")) {
                vk = VK_HOME;
            } else if (EqualsIgnoreCase(t, L"end")) {
                vk = VK_END;
            } else if (EqualsIgnoreCase(t, L"pageup")) {
                vk = VK_PRIOR;
            } else if (EqualsIgnoreCase(t, L"pagedown")) {
                vk = VK_NEXT;
            } else if (t.size() == 1 &&
                       ((t[0] >= L'A' && t[0] <= L'Z') ||
                        (t[0] >= L'a' && t[0] <= L'z') ||
                        (t[0] >= L'0' && t[0] <= L'9'))) {
                wchar_t c = t[0];
                if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - L'a' + L'A');
                vk = static_cast<unsigned int>(c);
            } else {
                error = L"无法识别按键 “" + t + L"”。可用示例：Alt+E、Ctrl+Shift+F2。";
                return false;
            }
        }
    }

    if (vk == 0) {
        error = L"快捷键 “" + trimmed + L"” 缺少主键（例如 Alt+E 中的 E）。";
        return false;
    }

    // 单个字母/数字作全局热键会抢走所有程序的输入，必须搭配修饰键
    const bool isFunctionKey = (vk >= VK_F1 && vk <= VK_F24);
    if (mods == 0 && !isFunctionKey) {
        error = L"“" + trimmed + L"” 必须配合 Ctrl / Alt / Shift / Win 使用，"
                L"否则会抢走其他程序的按键输入。";
        return false;
    }

    if (IsSystemReserved(mods, vk)) {
        error = L"“" + trimmed + L"” 是系统保留组合，无法注册。";
        return false;
    }

    return true;
}

bool HotkeyManager::Register(const HotkeySpec& spec, std::wstring& error) {
    error.clear();

    if (spec.vk == 0) {
        error = L"未指定按键。";
        return false;
    }
    if (IsSystemReserved(spec.mods, spec.vk)) {
        error = Format(spec) + L" 是系统保留组合，无法注册。";
        return false;
    }

    // MOD_NOREPEAT：按住不放时只触发一次，避免重复弹窗
    const BOOL ok = ::RegisterHotKey(hwnd_, spec.id,
                                     spec.mods | MOD_NOREPEAT, spec.vk);
    if (!ok) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_HOTKEY_ALREADY_REGISTERED) {
            error = L"热键 " + Format(spec) +
                    L" 已被其他程序占用，请在设置中改用其它组合。";
        } else {
            error = L"注册热键 " + Format(spec) + L" 失败（错误码 " +
                    std::to_wstring(err) + L"）。";
        }
        LT_WARN(error);
        return false;
    }

    registered_.push_back(spec);
    LT_INFO(L"已注册热键 " + Format(spec));
    return true;
}

std::vector<std::wstring> HotkeyManager::RegisterAll(const std::vector<HotkeySpec>& specs) {
    std::vector<std::wstring> failures;
    for (const auto& s : specs) {
        std::wstring err;
        if (!Register(s, err)) {
            failures.push_back(err);
        }
    }
    return failures;
}

void HotkeyManager::Unregister(int id) {
    ::UnregisterHotKey(hwnd_, id);
    for (auto it = registered_.begin(); it != registered_.end(); ++it) {
        if (it->id == id) {
            registered_.erase(it);
            break;
        }
    }
}

void HotkeyManager::UnregisterAll() {
    for (const auto& s : registered_) {
        ::UnregisterHotKey(hwnd_, s.id);
    }
    registered_.clear();
}

void HotkeyManager::OnHotkey(int id) {
    if (callback_) callback_(id);
}

} // namespace lt
