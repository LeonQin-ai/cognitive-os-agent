// EventToken.h — minimal stand-in for the Windows SDK WinRT header of the same
// name. The WebView2 MIDL-generated header (WebView2.h) includes this file but
// only needs the EventRegistrationToken POD type (event handlers are COM
// methods that pass the token through, never compare or construct it). The
// real header lives in the Windows SDK / cppwinrt, which the bundled zig
// toolchain does not ship, so this stub supplies just what is referenced.
#pragma once

struct EventRegistrationToken {
    __int64 value;
};
