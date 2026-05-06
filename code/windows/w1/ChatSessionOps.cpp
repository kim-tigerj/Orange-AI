#include "ChatSessionOps.h"

#include "Persistence.h"

namespace orange {

void SaveBlockToChatKey(const std::string& chatKey,
                        const std::wstring& role,
                        const std::wstring& text) {
    if (chatKey.empty() || text.empty()) return;
    ChatSession session = Persistence::Load(chatKey);
    ChatBlock block;
    block.role = role;
    block.text = text;
    if (!session.blocks.empty() && session.blocks.back().role == role &&
        role != L"user" && role != L"attachment") {
        session.blocks.back().text += text;
    } else {
        session.blocks.push_back(std::move(block));
    }
    Persistence::Save(session, chatKey);
}

} // namespace orange
