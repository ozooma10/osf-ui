# OSF Settings launcher SDK

`OSFSettings_Launcher.h` is an exact copy of the public header from the sibling
OSF Settings Slim project (`sdk/OSFSettings_Launcher.h`), launcher ABI 1.0.
Only this new optional service is vendored while the pinned Settings submodule
predates it. The existing Settings and Diagnostics SDKs still come from that
submodule. No provider implementation is included.

Update this copy with the provider header whenever that ABI changes. Once the
Settings submodule is advanced to a revision containing it, remove this copy and
include the header from the pinned SDK instead.
