#pragma once

#include <Windows.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <vector>

class HookManager;
class FocusManager;
class BrowserManager;
class NetworkManager;

class ChatHook
{
public:
    ChatHook(HookManager& hooks, FocusManager& focus, BrowserManager& browser, NetworkManager& network)
        : hooks_(hooks)
        , focus_(focus)
        , browser_(browser)
        , network_(network)
    {
    }

    bool Initialize();
    void Shutdown();

    // ---- CEF chat bridge (0.3.DL-R1 only) -------------------------------
    // Called from the CEF UI side (BrowserClient). Thread-safe; the text is
    // processed on the game thread inside the chat render hook.
    static void QueueSubmit(const std::string& utf8_text);
    // A browser announced it renders the chat: replay history to it.
    static void RegisterChatBrowser(int browser_id);
    // Once per frame on the game thread (App::Tick): keeps the native chat hidden,
    // replays history to new chat browsers, runs queued input.
    static void Tick();

private:
    using FnOpenChatInput = void(__fastcall*)(void* pThis, void* _edx);
    static void __fastcall Hook_OpenChatInput(void* pThis, void* _edx);

    using FnCloseChatInput = void(__fastcall*)(void* pThis, void* _edx);
    static void __fastcall Hook_CloseChatInput(void* pThis, void* _edx);

    void SetChatInputState(bool open);

    // Chat bridge (DL-R1)
    // CChat::AddEntry(int type, const char* text, const char* prefix, D3DCOLOR textColor, D3DCOLOR prefixColor)
    using FnAddEntry = void(__fastcall*)(void* pThis, void* _edx, int type, const char* text,
                                         const char* prefix, unsigned long textColor, unsigned long prefixColor);
    static void __fastcall Hook_AddEntry(void* pThis, void* _edx, int type, const char* text,
                                         const char* prefix, unsigned long textColor, unsigned long prefixColor);

    struct ChatLine
    {
        int type = 0;
        std::string text;     // UTF-8
        std::string prefix;   // UTF-8
        int text_color = 0;   // 0xRRGGBB
        int prefix_color = 0; // 0xRRGGBB
    };

    void ForwardEntry(const ChatLine& line, int only_browser = -1);
    // Asks the chat pages to repaint for the next frames (see Tick). OSR pages sometimes keep showing
    // an old frame until something invalidates them.
    static void KickRepaint();
    void ProcessPendingInput();
    void DispatchInput(const std::string& ansi_text);

private:
    HookManager& hooks_;
    FocusManager& focus_;
    BrowserManager& browser_;
    NetworkManager& network_;

    static inline ChatHook* s_self_ = nullptr;
    static inline FnOpenChatInput s_orig_open_ = nullptr;
    static inline FnCloseChatInput s_orig_close_ = nullptr;
    static inline FnAddEntry s_orig_add_entry_ = nullptr;
    static inline std::atomic<int> s_kick_frames_{ 0 };
    static inline bool s_bridge_enabled_ = false;   // true only on 0.3.DL-R1 with the AddEntry hook installed

    static inline std::mutex s_mutex_;
    static inline std::deque<std::string> s_pending_input_;   // UTF-8, from the CEF side
    static inline std::deque<ChatLine> s_history_;            // last lines, for replay
    static inline std::set<int> s_chat_browsers_;
    static inline std::set<int> s_replay_requests_;
};