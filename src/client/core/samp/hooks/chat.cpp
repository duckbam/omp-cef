#include "chat.hpp"

#include <cstdint>

#include "browser/focus.hpp"
#include "browser/manager.hpp"
#include "hooks/hook_manager.hpp"
#include "network/network_manager.hpp"
#include "samp/addresses.hpp"
#include "system/logger.hpp"
#include "shared/events.hpp"
#include "utf8.hpp"

#include <sampapi/0.3.DL-1/CChat.h>
#include <sampapi/0.3.DL-1/CInput.h>
#include <sampapi/0.3.DL-1/CLocalPlayer.h>
#include <sampapi/0.3.DL-1/CNetGame.h>
#include <sampapi/0.3.DL-1/CPlayerPool.h>

#include <include/base/cef_bind.h>
#include <include/wrapper/cef_closure_task.h>
#include <include/wrapper/cef_helpers.h>

#include <algorithm>
#include <cstring>

bool ChatHook::Initialize()
{
    s_self_ = this;

    auto& addrs = SampAddresses::Instance();
    auto* base = addrs.Base();

    void* addrOpenChat = nullptr;
    void* addrCloseChat = nullptr;

    switch (addrs.Version())
    {
        case SampVersion::V037:
            addrOpenChat = reinterpret_cast<void*>(base + 0x657E0);
            addrCloseChat = reinterpret_cast<void*>(base + 0x658E0);
            break;
        case SampVersion::V037R3:
            addrOpenChat = reinterpret_cast<void*>(base + 0x68D10);
            addrCloseChat = reinterpret_cast<void*>(base + 0x68E10);
            break;
        case SampVersion::V037R5:
            addrOpenChat = reinterpret_cast<void*>(base + 0x69480);
            addrCloseChat = reinterpret_cast<void*>(base + 0x69580);
            break;
        case SampVersion::V03DLR1:
            addrOpenChat = reinterpret_cast<void*>(base + 0x68EC0);
            addrCloseChat = reinterpret_cast<void*>(base + 0x68FC0);
            break;
        default:
            break;
    }

    if (!addrOpenChat)
    {
        LOG_FATAL("[ChatHook] Unsupported SA:MP version for OpenChatInput.");
        return false;
    }

    if (!hooks_.Install("ChatHook::OpenChatInput", addrOpenChat, reinterpret_cast<void*>(&Hook_OpenChatInput)))
    {
        LOG_ERROR("[ChatHook] Failed to install OpenChatInput hook.");
        return false;
    }

    if (!hooks_.Install("ChatHook::CloseChatInput", addrCloseChat, reinterpret_cast<void*>(&Hook_CloseChatInput)))
    {
        LOG_ERROR("[ChatHook] Failed to install CloseChatInput hook.");
        return false;
    }

    s_orig_open_ = reinterpret_cast<FnOpenChatInput>(hooks_.GetOriginal("ChatHook::OpenChatInput"));
    if (!s_orig_open_)
    {
        LOG_FATAL("[ChatHook] Failed to get original function pointer.");
        hooks_.Uninstall("ChatHook::OpenChatInput");
        return false;
    }

    s_orig_close_ = (FnCloseChatInput)hooks_.GetOriginal("ChatHook::CloseChatInput");
    if (!s_orig_close_)
    {
        LOG_FATAL("[ChatHook] Failed to get original function pointer.");
        hooks_.Uninstall("ChatHook::CloseChatInput");
        return false;
    }

    LOG_DEBUG("[ChatHook] OpenChatInput hook installed.");
    LOG_DEBUG("[ChatHook] CloseChatInput hook installed.");

    // CEF chat bridge: only 0.3.DL-R1 for now. Failures are non-fatal; the
    // native chat keeps working untouched.
    if (addrs.Version() == SampVersion::V03DLR1)
    {
        if (hooks_.Install("ChatHook::AddEntry", base + 0x67650, reinterpret_cast<void*>(&Hook_AddEntry)))
            s_orig_add_entry_ = reinterpret_cast<FnAddEntry>(hooks_.GetOriginal("ChatHook::AddEntry"));
        else
            LOG_ERROR("[ChatHook] Failed to install AddEntry hook (CEF chat disabled).");

        if (hooks_.Install("ChatHook::ChatRender", base + 0x673B0, reinterpret_cast<void*>(&Hook_ChatRender)))
            s_orig_render_ = reinterpret_cast<FnChatDraw>(hooks_.GetOriginal("ChatHook::ChatRender"));
        else
            LOG_ERROR("[ChatHook] Failed to install ChatRender hook.");

        if (hooks_.Install("ChatHook::ChatDraw", base + 0x67870, reinterpret_cast<void*>(&Hook_ChatDraw)))
            s_orig_draw_ = reinterpret_cast<FnChatDraw>(hooks_.GetOriginal("ChatHook::ChatDraw"));
        else
            LOG_ERROR("[ChatHook] Failed to install ChatDraw hook.");

        s_bridge_enabled_ = (s_orig_add_entry_ != nullptr);
        LOG_INFO("[ChatHook] Chat bridge enabled={}", s_bridge_enabled_);
    }

    return true;
}

void ChatHook::Shutdown()
{
    hooks_.Uninstall("ChatHook::OpenChatInput");
    hooks_.Uninstall("ChatHook::CloseChatInput");
    hooks_.Uninstall("ChatHook::AddEntry");
    hooks_.Uninstall("ChatHook::ChatRender");
    hooks_.Uninstall("ChatHook::ChatDraw");
    s_orig_render_ = nullptr;
    s_orig_draw_ = nullptr;
    s_bridge_enabled_ = false;
    s_orig_add_entry_ = nullptr;
    s_orig_open_ = nullptr;
    s_orig_close_ = nullptr;
    s_self_ = nullptr;
}

void ChatHook::SetChatInputState(bool open)
{
    // Avoid redundant notifications
    if (focus_.IsChatInputOpen() == open)
    {
        LOG_INFO("[ChatHook] SetChatInputState({}) ignored: already in that state", open);
        return;
    }
    LOG_INFO("[ChatHook] SetChatInputState({})", open);
    KickRepaint();

    focus_.SetChatInputOpen(open);

    // Local JS event to all browsers
    for (const auto& kv : browser_.GetAllBrowsers())
    {
        const int id = kv.first;
        auto* inst = browser_.GetBrowserInstance(id);
        if (!inst || !inst->browser)
            continue;

        CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("emit_event");
        CefRefPtr<CefListValue> list = msg->GetArgumentList();
        list->SetString(0, EnsureUtf8ForCef("omp:cef:internal:chatInputState"));
        list->SetBool(1, open);
        inst->browser->GetMainFrame()->SendProcessMessage(PID_RENDERER, msg);
    }

    // Server callback
    ClientEmitEventPacket ev;
    ev.browserId = -1;
    ev.name = CefEvent::Client::ChatInputState;
    ev.args.emplace_back(open);
    network_.SendPacket(PacketType::ClientEmitEvent, ev);
}

void __fastcall ChatHook::Hook_OpenChatInput(void* pThis, void* _edx)
{
    auto* self = s_self_;
    if (!self)
        return;

    if (self->focus_.ShouldBlockChat())
    {
        LOG_INFO("[ChatHook] Open requested but ShouldBlockChat()=true");
        return;
    }

    bool cef_chat_active = false;
    {
        std::lock_guard<std::mutex> lock(s_mutex_);
        cef_chat_active = !s_chat_browsers_.empty();
    }

    LOG_INFO("[ChatHook] Open requested: cef_chat_active={}", cef_chat_active);

    // With a CEF chat registered the native input box stays closed: the CEF
    // page shows its own input when it receives chatInputState(true).
    if (!cef_chat_active && s_orig_open_)
        s_orig_open_(pThis, _edx);

    self->SetChatInputState(true);
}

void __fastcall ChatHook::Hook_CloseChatInput(void* pThis, void* _edx)
{
    auto* self = s_self_;
    if (!self)
        return;

    if (s_orig_close_)
        s_orig_close_(pThis, _edx);

    self->SetChatInputState(false);
}

// ---------------------------------------------------------------------------
// CEF chat bridge
// ---------------------------------------------------------------------------

namespace
{
    // Cuts an ANSI string to max_bytes without splitting a double-byte char.
    std::string ClipAnsi(const std::string& s, size_t max_bytes)
    {
        if (s.size() <= max_bytes)
            return s;

        size_t i = 0;
        while (i < max_bytes)
        {
            const size_t step = ::IsDBCSLeadByte(static_cast<BYTE>(s[i])) ? 2 : 1;
            if (i + step > max_bytes)
                break;
            i += step;
        }
        return s.substr(0, i);
    }

    std::string Utf8ToAnsi(const std::string& utf8)
    {
        std::wstring wide;
        if (!ConvertMultiByteToWide(CP_UTF8, 0, utf8, wide))
            return utf8;

        std::string out;
        if (!ConvertWideToMultiByte(::GetACP(), 0, wide, out))
            return utf8;

        return out;
    }
}

void ChatHook::QueueSubmit(const std::string& utf8_text)
{
    if (!s_bridge_enabled_)
        return;

    LOG_INFO("[ChatHook] QueueSubmit len={}", utf8_text.size());
    KickRepaint();
    std::lock_guard<std::mutex> lock(s_mutex_);
    if (s_pending_input_.size() < 32)
        s_pending_input_.push_back(utf8_text);
}

void ChatHook::RegisterChatBrowser(int browser_id)
{
    if (!s_bridge_enabled_)
        return;

    LOG_INFO("[ChatHook] RegisterChatBrowser id={}", browser_id);
    std::lock_guard<std::mutex> lock(s_mutex_);
    s_chat_browsers_.insert(browser_id);
    s_replay_requests_.insert(browser_id);   // history is replayed on the game thread
}

void ChatHook::KickRepaint()
{
    s_kick_frames_.store(30);   // ~0.5 s of forced repaints, handled in Tick()
}

void ChatHook::ForwardEntry(const ChatLine& line, int only_browser)
{
    std::vector<int> targets;
    {
        std::lock_guard<std::mutex> lock(s_mutex_);
        if (only_browser >= 0)
            targets.push_back(only_browser);
        else
            targets.assign(s_chat_browsers_.begin(), s_chat_browsers_.end());
    }

    for (const int id : targets)
    {
        auto* inst = browser_.GetBrowserInstance(id);
        if (!inst || !inst->browser)
        {
            std::lock_guard<std::mutex> lock(s_mutex_);
            s_chat_browsers_.erase(id);   // browser is gone
            continue;
        }

        CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("emit_event");
        CefRefPtr<CefListValue> list = msg->GetArgumentList();
        list->SetString(0, "chat:entry");
        list->SetInt(1, line.type);
        list->SetString(2, line.text);
        list->SetString(3, line.prefix);
        list->SetInt(4, line.text_color);
        list->SetInt(5, line.prefix_color);
        inst->browser->GetMainFrame()->SendProcessMessage(PID_RENDERER, msg);
    }
}

void __fastcall ChatHook::Hook_AddEntry(void* pThis, void* _edx, int type, const char* text,
                                        const char* prefix, unsigned long textColor, unsigned long prefixColor)
{
    // Always let the native chat store/log the line first (chatlog, /pagesize...).
    if (s_orig_add_entry_)
        s_orig_add_entry_(pThis, _edx, type, text, prefix, textColor, prefixColor);

    auto* self = s_self_;
    if (!self)
        return;

    ChatLine line;
    line.type = type;
    line.text = EnsureUtf8ForCef(text ? text : "");
    line.prefix = EnsureUtf8ForCef(prefix ? prefix : "");
    line.text_color = static_cast<int>(textColor & 0xFFFFFF);
    line.prefix_color = static_cast<int>(prefixColor & 0xFFFFFF);

    {
        std::lock_guard<std::mutex> lock(s_mutex_);
        s_history_.push_back(line);
        while (s_history_.size() > 100)
            s_history_.pop_front();
    }

    self->ForwardEntry(line);
    KickRepaint();
}

namespace
{
    // The native chat has a scroll bar control (CChat+0x11E) drawn by the game's dialog code, not by
    // CChat::Render/Draw. VERIFIED against the 0.3.DL-R1 samp.dll (entry 0xFDB60): its vtable is at
    // RVA 0x11D550, slot +0x40 is SetVisible(bool) = `mov [ecx+4], al`, and the control's Render
    // (RVA 0x884D0) draws nothing when that byte is 0. CChat::Draw (RVA 0x67870) is the only code that
    // turns it back on each frame, and we skip Draw, so setting it once per frame is enough.
    bool HideScrollbarGuarded(void* scrollbar)
    {
        __try
        {
            void** vtable = *reinterpret_cast<void***>(scrollbar);
            using FnSetVisible = void(__thiscall*)(void*, bool);
            reinterpret_cast<FnSetVisible>(vtable[16])(scrollbar, false);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
}

void __fastcall ChatHook::Hook_ChatRender(void* pThis, void* _edx)
{
    // With the patched client the native chat is not drawn at all (the CEF chat shows every line).
    if (!s_bridge_enabled_ && s_orig_render_)
        s_orig_render_(pThis, _edx);
}

void __fastcall ChatHook::Hook_ChatDraw(void* pThis, void* _edx)
{
    if (!s_bridge_enabled_ && s_orig_draw_)
        s_orig_draw_(pThis, _edx);
}

void ChatHook::Tick()
{
    auto* self = s_self_;
    if (!self || !s_bridge_enabled_)
        return;

    // Forget chat browsers that no longer exist.
    std::vector<int> ids;
    {
        std::lock_guard<std::mutex> lock(s_mutex_);
        ids.assign(s_chat_browsers_.begin(), s_chat_browsers_.end());
    }
    for (const int id : ids)
    {
        auto* inst = self->browser_.GetBrowserInstance(id);
        if (!inst || !inst->browser)
        {
            std::lock_guard<std::mutex> lock(s_mutex_);
            s_chat_browsers_.erase(id);
        }
    }

    std::vector<int> replay;
    std::deque<ChatLine> history;
    {
        std::lock_guard<std::mutex> lock(s_mutex_);
        if (!s_replay_requests_.empty())
        {
            replay.assign(s_replay_requests_.begin(), s_replay_requests_.end());
            s_replay_requests_.clear();
            history = s_history_;
        }
    }

    // Keep the native chat scroll bar hidden (a one-byte write; re-done each frame in case the
    // control was recreated after a device reset).
    if (auto* chat = sampapi::v03dl::RefChat())
    {
        if (chat->m_pScrollbar)
        {
            static bool s_logged = false;
            const bool ok = HideScrollbarGuarded(chat->m_pScrollbar);
            if (!s_logged)
            {
                LOG_INFO("[ChatHook] Hide native scrollbar: {}", ok ? "ok" : "FAILED");
                s_logged = true;
            }
        }
    }

    for (const int id : replay)
        for (const auto& line : history)
            self->ForwardEntry(line, id);

    self->ProcessPendingInput();

    // Forced repaint of the chat pages while something changed recently.
    if (s_kick_frames_.load() > 0)
    {
        s_kick_frames_.fetch_sub(1);

        std::vector<int> targets;
        {
            std::lock_guard<std::mutex> lock(s_mutex_);
            targets.assign(s_chat_browsers_.begin(), s_chat_browsers_.end());
        }

        for (const int id : targets)
        {
            auto* inst = self->browser_.GetBrowserInstance(id);
            if (!inst || !inst->browser)
                continue;

            CefPostTask(TID_UI, base::BindOnce([](CefRefPtr<CefBrowser> browser)
            {
                if (!browser)
                    return;
                if (auto host = browser->GetHost())
                {
                    host->WasHidden(false);
                    host->WasResized();
                    host->Invalidate(PET_VIEW);
                }
            }, inst->browser));
        }
    }
}

void ChatHook::ProcessPendingInput()
{
    std::deque<std::string> batch;
    {
        std::lock_guard<std::mutex> lock(s_mutex_);
        if (s_pending_input_.empty())
            return;
        batch.swap(s_pending_input_);
    }

    LOG_INFO("[ChatHook] ProcessPendingInput: {} item(s)", batch.size());
    for (const auto& utf8 : batch)
    {
        // Special marker from the page: "close without sending".
        if (utf8.empty())
        {
            SetChatInputState(false);
            continue;
        }

        DispatchInput(ClipAnsi(Utf8ToAnsi(utf8), 128));
        SetChatInputState(false);
    }
}

// Mirrors what SA-MP's own input box does on Enter:
//   "/name args" -> client command if one is registered, else sent to the server
//   anything else -> normal chat line
void ChatHook::DispatchInput(const std::string& text)
{
    if (text.empty())
        return;

    LOG_INFO("[ChatHook] DispatchInput '{}'", text);
    using namespace sampapi::v03dl;

    CInput* input = RefInputBox();
    if (!input)
        return;

    if (text[0] == '/')
    {
        char name[CInput::MAX_CMD_LENGTH + 1] = {};
        size_t n = 0;
        while (1 + n < text.size() && text[1 + n] != ' ' && n < CInput::MAX_CMD_LENGTH)
        {
            name[n] = text[1 + n];
            ++n;
        }

        const char* args = text.c_str() + 1 + n;
        if (*args == ' ')
            ++args;

        int count = input->m_nCommandCount;
        if (count > CInput::MAX_CLIENT_CMDS)
            count = CInput::MAX_CLIENT_CMDS;
        for (int i = 0; i < count; ++i)
        {
            if (input->m_pCommandProc[i] && ::lstrcmpiA(input->m_szCommandName[i], name) == 0)
            {
                LOG_INFO("[ChatHook] client command '{}'", name);
                input->m_pCommandProc[i](args);
                return;
            }
        }

        LOG_INFO("[ChatHook] server command '{}'", name);
        input->Send(text.c_str());
        return;
    }

    auto* netgame = RefNetGame();
    auto* players = netgame ? netgame->GetPlayerPool() : nullptr;
    auto* local = players ? players->GetLocalPlayer() : nullptr;
    LOG_INFO("[ChatHook] plain chat, local player={}", local != nullptr);
    if (local)
        local->Chat(text.c_str());
}
