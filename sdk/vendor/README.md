# OSF Settings SDK (vendored)

These headers are exact copies of the public SDK from the OSF Settings Slim
project (`sdk/`), taken at Slim commit `140b93e8074af53e954f028a00122f5be59a9f37`:

- `OSFSettings.h`: settings ABI 1.0
- `OSFSettings_Diagnostics.h`: diagnostics ABI 1.0
- `OSFSettings_Launcher.h`: optional launcher ABI 1.0

No provider implementation is included. To update, copy the same files from
Slim's `sdk/` folder unchanged and update the commit above. Include them as
`"vendor/OSFSettings.h"` and so on; `sdk/` is already on the include path.
