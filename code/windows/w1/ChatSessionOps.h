#pragma once

#include <string>

namespace orange {

void SaveBlockToChatKey(const std::string& chatKey,
                        const std::wstring& role,
                        const std::wstring& text);

} // namespace orange
