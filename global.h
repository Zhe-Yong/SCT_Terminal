/* Global declarations and definitions */
/* Copyright (c) 2026 ZheYong. All rights reserved. */
/* File Version: 0.0.1 */
#pragma once

// Headers definitions
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#include <windows.h>
#include <shellapi.h>
#include <objbase.h>
#include <sddl.h>
#include <bcrypt.h>
#include <tlhelp32.h>
#include <bits/stdc++.h>

#include "color_print.h"
#include "tool.h"
#include "error_code.h"
#include "command.h"
#include "elevation.h"
#include "cancel.h"
#include "ppl.h"
#include "autoexec.h"
#include "app.h"
#include "shell_host.h"
#include "terminal.h"
#include "plugin_state.h"
#include "plugin_security.h"
#include "plugin_trusted_signers.h"
#include "plugin_manager.h"

// Constant definitions
constexpr int64_t unix_expire = 1800000000;