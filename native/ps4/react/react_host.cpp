// PS4 React starter host.
//
// Links the real Embedded React engine, QuickJS bridge and the bundled React
// starter, installs the native API, and presents the engine framebuffer through
// VideoOut. Networking, images, archives, audio and runtime text shaping use the
// PS4 implementations in this directory.

#include "actions.hpp"
#include "archives.hpp"
#include "display_video.hpp"
#include "host_api.hpp"
#include "js_heap.hpp"
#include "network.hpp"
#include "pad.hpp"
#include "system.hpp"
#include "text_shaper.hpp"

#include "core/input.hpp"

#include <orbis/Pad.h>

#include "app_config.generated.h"

extern "C" {
#include "assets.generated.h"
#include "er_runtime.h"
#include "er_scene.h"
#include "native_renderer.h"
#include "software_backend.h"
}

#if defined(PS4_REACT_DEV_SERVER)
#include "dev_server.hpp"
#endif

extern "C" const char app_bundle[];
extern "C" const unsigned long app_bundle_length;

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <span>
#include <string>
#include <utility>

namespace {

constexpr int kWidth = 1920;
constexpr int kHeight = 1080;

// Indexed like GamepadState::kButtonNames.
constexpr hui::Action kButtonActions[] = {
    hui::Action::up,       hui::Action::down,     hui::Action::left,
    hui::Action::right,    hui::Action::confirm,  hui::Action::back,
    hui::Action::north,    hui::Action::west,     hui::Action::page_prev,
    hui::Action::page_next, hui::Action::jump_prev, hui::Action::jump_next,
    hui::Action::l3,       hui::Action::r3,       hui::Action::menu,
    hui::Action::touch,
};
static_assert(std::size(kButtonActions) == std::size(kButtonNames));

// Buttons delivered to the app as actions; directions come through input.nav.
constexpr std::pair<hui::Action, HostAction> kExtraActions[] = {
    {hui::Action::confirm, HostAction::confirm},
    {hui::Action::back, HostAction::back},
    {hui::Action::page_prev, HostAction::l1},
    {hui::Action::page_next, HostAction::r1},
    {hui::Action::jump_prev, HostAction::l2},
    {hui::Action::jump_next, HostAction::r2},
    {hui::Action::north, HostAction::triangle},
    {hui::Action::west, HostAction::square},
    {hui::Action::l3, HostAction::l3},
    {hui::Action::r3, HostAction::r3},
    {hui::Action::touch, HostAction::touchpad},
};

const char* nav_action(hui::Direction direction) {
    switch (direction) {
        case hui::Direction::up: return action_name(HostAction::up);
        case hui::Direction::down: return action_name(HostAction::down);
        case hui::Direction::left: return action_name(HostAction::left);
        case hui::Direction::right: return action_name(HostAction::right);
        default: return nullptr;
    }
}

GamepadState gamepad_state(const hui::InputFrame& input) {
    GamepadState state;
    state.connected = input.connected;
    state.left_x = input.stick_x;
    state.left_y = input.stick_y;
    state.right_x = input.stick2_x;
    state.right_y = input.stick2_y;
    state.l2 = input.trigger_l;
    state.r2 = input.trigger_r;
    for (std::size_t i = 0; i < std::size(kButtonActions); ++i)
        if (input.is_held(kButtonActions[i])) state.buttons |= 1u << i;
    return state;
}

bool dispatch(const char* action) {
    JSContext* ctx = er_runtime_context();
    if (!ctx) return false;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue fn = JS_GetPropertyStr(ctx, global, "__ps4ReactDispatch");
    if (!JS_IsFunction(ctx, fn)) {
        JS_FreeValue(ctx, fn);
        JS_FreeValue(ctx, global);
        return false;
    }
    JSValue value = JS_NewString(ctx, action);
    JSValue result = JS_Call(ctx, fn, global, 1, &value);
    const bool ok = !JS_IsException(result);
    if (!ok) {
        JSValue error = JS_GetException(ctx);
        const char* message = JS_ToCString(ctx, error);
        ps4::system::log("[REACT] input exception: %s", message ? message : "unknown");
        if (message) JS_FreeCString(ctx, message);
        JS_FreeValue(ctx, error);
    }
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, value);
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, global);
    return ok;
}

void react_log(const char* line) {
    ps4::system::log("[REACT] %s", line ? line : "");
}

hui::PadSample convert(const OrbisPadData& data) {
    hui::PadSample sample;
    sample.buttons = data.buttons;
    sample.left_x = data.leftStick.x;
    sample.left_y = data.leftStick.y;
    sample.right_x = data.rightStick.x;
    sample.right_y = data.rightStick.y;
    sample.l2 = data.analogButtons.l2;
    sample.r2 = data.analogButtons.r2;
    sample.connected = data.connected != 0;
    sample.timestamp_us = data.timestamp;
    return sample;
}

} // namespace

int main() {
    using namespace ps4;

    ps4::system::start_log("react.log");
    ps4::system::log("PS4 React Starter: starting");
    ps4::system::initialize_services();

    Display display;
    if (!display.open(kWidth, kHeight)) {
        ps4::system::log("PS4 React Starter: display open failed");
        ps4::system::park();
    }

    std::array<Pad, 4> pads;
    int pad_count = 0;
    // Opening the first pad also initializes the user service, which the
    // logged-in user enumeration needs.
    if (pads[0].open()) pad_count = 1;
    host::User users[4];
    const int user_count = host::logged_in_users(users, 4);
    if (user_count > 0) {
        for (Pad& pad : pads) pad.close();
        pad_count = 0;
        for (int i = 0; i < user_count && i < 4; ++i)
            if (pads[pad_count].open_for_user(users[i].id)) ++pad_count;
    }
    if (pad_count == 0 && pads[0].open()) pad_count = 1;
    if (pad_count > 0) host::attach_pad(&pads[0]);
    ps4::system::log("PS4 React Starter: pads=%d users=%d", pad_count, user_count);

    const bool have_audio = ps4_react_start_sound();
    ps4::system::log("PS4 React Starter: sound=%s", have_audio ? "yes" : "no");

    const bool have_network = network::start();
    ps4::system::log("PS4 React Starter: network=%s", have_network ? "yes" : "no");

#if defined(PS4_REACT_DEV_SERVER)
    dev::Server dev_server;
    if (!dev_server.start())
        ps4::system::log("PS4 React Starter: dev server unavailable");
#endif

    const bool have_images = ps4_react_start_images();
    ps4::system::log("PS4 React Starter: images=%s", have_images ? "yes" : "no");

    if (!er_software_backend_init(kWidth, kHeight)) {
        ps4::system::log("PS4 React Starter: software backend failed");
        ps4::system::park();
    }

    ErRuntimeConfig config{};
    config.screen_width = kWidth;
    config.screen_height = kHeight;
    config.screen_scale = 1.5f;
    config.log = react_log;
    config.max_stack_size = 1024 * 1024;
    config.memory_limit = kJsMemoryLimit;
    config.malloc_functions = js_heap_functions();
    config.install_host_globals = ps4_react_install_host_api;

    if (!er_runtime_init(&config)) {
        ps4::system::log("PS4 React Starter: runtime init failed");
        ps4::system::park();
    }
    ps4::system::log("PS4 React Starter: runtime init, gc_accounting=%d",
                er_runtime_gc_accounting_ok() ? 1 : 0);
    ps4::system::log("PS4 React Starter: registering baked assets");
    er_register_assets();
    ps4::system::log("PS4 React Starter: baked assets registered");

    char font_dir[512] = {};
    if (host::resolve_path("/app0/fonts", font_dir, sizeof font_dir)) {
        ps4::system::log("PS4 React Starter: installing text shaper from %s", font_dir);
        text_shaper::install(font_dir, react_log);
        ps4::system::log("PS4 React Starter: text shaper ready");
    } else {
        ps4::system::log("PS4 React Starter: font directory not resolved");
    }

    // Keep the last error on screen instead of exiting to a black screen. The
    // overlay is precompiled Embedded React bytecode and works after a failed
    // bundle load; parking keeps it visible until the user closes the title.
    const auto fatal = [&](const char* message) {
        ps4::system::log("PS4 React Starter: %s", message);
        if (er_runtime_context()) {
            er_runtime_show_error();
            er_commit();
            if (const auto* pixels = er_software_framebuffer())
                display.present(pixels, kWidth, kHeight);
        }
        ps4::system::sleep_us(3000000);
        ps4::system::park();
    };

    ps4::system::log("PS4 React Starter: evaluating bundle (%lu bytes)",
                     app_bundle_length);
    const bool loaded = er_runtime_load_source(app_bundle, app_bundle_length, "app.bundle.js");
    ps4::system::log("PS4 React Starter: bundle=%d error=%s", loaded ? 1 : 0,
                er_runtime_last_error());
    if (!loaded) fatal("bundle failed to load");

    hui::InputTracker tracker;
    std::array<hui::InputTracker, 4> pad_trackers;
    std::array<OrbisPadData, 4> raw{};
    bool have_first_frame = false;
    std::int64_t previous = ps4::system::monotonic_us();
    std::int64_t fps_start = previous;
    unsigned frames = 0;
    unsigned total_frames = 0;

    for (;;) {
        host::tick();
        ps4_react_app_state_pump();
        const std::int64_t now = ps4::system::monotonic_us();
        hui::PadSample samples[4];
        std::size_t count = 0;
        for (int player = 0; player < pad_count; ++player) {
            if (!pads[player].read(raw[player])) {
                ps4_react_set_gamepad(player, GamepadState{});
                continue;
            }
            samples[count] = convert(raw[player]);
            const hui::InputFrame player_input = pad_trackers[player].update(
                std::span<const hui::PadSample>(&samples[count], 1), now);
            ps4_react_set_gamepad(player, gamepad_state(player_input));
            ++count;
        }
        for (int player = pad_count; player < 4; ++player)
            ps4_react_set_gamepad(player, GamepadState{});

        const hui::InputFrame input =
            tracker.update(std::span<const hui::PadSample>(samples, count), now);

        // A system dialog or the on-screen keyboard owns the pad while it is
        // open. Do not also dispatch those presses to the React tree.
        if (!ps4_react_system_dialog_active()) {
            if (input.is_pressed(hui::Action::menu)) {
                ps4::system::log("PS4 React Starter: Options pressed; exiting");
                break;
            }
            if (const char* action = nav_action(input.nav)) dispatch(action);
            for (const auto& [button, action] : kExtraActions)
                if (input.is_pressed(button)) dispatch(action_name(action));
        }

#if defined(PS4_REACT_DEV_SERVER)
        const std::uint8_t* reload = nullptr;
        std::size_t reload_size = 0;
        if (dev_server.poll(reload, reload_size)) {
            bool applied = er_runtime_reset();
            if (applied) {
                er_register_assets();
                applied = er_runtime_load_source(
                    reinterpret_cast<const char*>(reload), reload_size, "dev.bundle.js");
            }
            if (applied) {
                ps4::system::log("PS4 React Starter: hot reload ok (%zu bytes)", reload_size);
            } else {
                ps4::system::log("PS4 React Starter: hot reload failed: %s",
                                 er_runtime_last_error());
                er_runtime_show_error();
            }
            dev_server.ack(applied);
        }
#endif

        er_runtime_pump();
        const double elapsed_ms = std::clamp(
            static_cast<double>(now - previous) / 1000.0, 1.0, 50.0);
        if (!ps4_react_frame(er_runtime_context(), elapsed_ms))
            fatal("frame callback exception");
        er_commit();
        if (*er_runtime_last_error()) {
            ps4::system::log("PS4 React Starter: commit error: %s", er_runtime_last_error());
            fatal("commit error");
        }

        if (!display.present(er_software_framebuffer(), kWidth, kHeight)) {
            ps4::system::log("PS4 React Starter: present failed");
            break;
        }
        if (!have_first_frame) {
            have_first_frame = true;
            ps4::system::hide_splash();
            ps4::system::log("PS4 React Starter: first frame presented");
        }

        embedded_renderer_tick(static_cast<std::uint32_t>(elapsed_ms));
        previous = now;
        ++frames;
        ++total_frames;
        if (now - fps_start >= 1000000) {
            const double seconds = static_cast<double>(now - fps_start) / 1000000.0;
            ps4::system::log("PS4 React Starter: %.1f fps (%u frames, total %u)",
                        static_cast<double>(frames) / seconds, frames, total_frames);
            fps_start = now;
            frames = 0;
        }
        if (ps4_react_exit_requested()) break;
    }

    ps4::system::log("PS4 React Starter: loop ended after %u frames", total_frames);

    // A mounted save-data directory holds a SceSaveData suspend block. KillApp
    // suspends the title before killing it; release the mount first or the
    // suspend times out and the shell remains on the splash.
    ps4::system::log("PS4 React Starter: releasing save data before close");
    ps4_react_save_data_shutdown();

    // Try the close while the display and focus are still ours. The system
    // refuses the request after VideoOut and the rest of the state are gone.
    if (ps4::system::request_close()) {
        ps4::system::log("system: close accepted; waiting for the system");
        ps4::system::park();
    }

    // The close request failed. Keep the UI alive; the user can close the app
    // from the dashboard. Do not call NavigateToGoHome (it crashes the shell
    // here) or _exit (SIGSYS, reported as a crash).
    ps4::system::log("system: close request failed; leaving the app running");
    ps4::system::park();
}
