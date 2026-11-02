// Native API surfaces that are not part of the first React builds.
// host_api.cpp installs every namespace; the JS runtime expects an object for
// each one even when the feature is absent. Sound lives in the shared
// sound_api.cpp and is implemented on PS4 through sceAudioOut.

#include "host_api.hpp"

extern "C" {
#include "quickjs.h"
}

namespace {

JSValue empty_object(JSContext* ctx) {
    return JS_NewObject(ctx);
}

} // namespace

JSValue ps4_react_package_api(JSContext* ctx) { return empty_object(ctx); }
JSValue ps4_react_payload_api(JSContext* ctx) { return empty_object(ctx); }
JSValue ps4_react_browser_capture_api(JSContext* ctx) { return empty_object(ctx); }
