#!/usr/bin/env bash
# Validate the shipped boolean schemas with Slim's actual parser, without game dependencies.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p .build
"${CXX:-clang++}" -std=c++23 -Wall -Wextra -Werror \
  -I ../../lib/osf-settings/src -I portable-stubs -I ../native/.deps -I ../native/stubs \
  settings_schema_tests.cpp key_stubs.cpp \
  ../../lib/osf-settings/src/Settings/SettingsSchemaJson.cpp \
  ../../lib/osf-settings/src/Settings/SettingsSchema.cpp \
  ../../lib/osf-settings/src/Settings/SettingsJson.cpp \
  -o .build/settings_schema_tests
.build/settings_schema_tests
