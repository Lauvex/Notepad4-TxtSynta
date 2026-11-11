/******************************************************************************
*
* Notepad4-TxtSyntax
*
* ConfigCloudAuto.h
*   Automatic encrypted WebDAV snapshots and remote retention.
*
******************************************************************************/
#pragma once

#include <windows.h>
#include <string>

#include "ConfigCloud.h"

struct ConfigCloudAutoSettings {
	bool enabled = false;
	int intervalHours = 24;
	int keepCount = 10;
	bool includeHistory = false;
};

ConfigCloudAutoSettings &ConfigCloudAuto_Settings() noexcept;
void ConfigCloudAuto_SavePolicyToIni() noexcept;

// Uses the DPAPI-protected credentials. Returns true when no upload was needed
// or when the upload succeeded; *uploaded reports whether a snapshot was made.
bool ConfigCloudAuto_MaybeRun(bool *uploaded, std::wstring *error) noexcept;

// Starts the automatic cloud check on a single background worker. The caller is
// never blocked by WebDAV; a second request while the worker runs is ignored.
bool ConfigCloudAuto_MaybeRunAsync() noexcept;

// Signals cancellation and waits at most two seconds for the worker. A worker
// that is still in a blocking network call is left to process exit.
void ConfigCloudAuto_ShutdownAsync() noexcept;

bool ConfigCloudAuto_IsRunning() noexcept;

// Manual execution of the same automatic upload path, including retention.
bool ConfigCloudAuto_UploadNow(ConfigCloudUploadResult *result,
	std::wstring *error) noexcept;

std::wstring ConfigCloudAuto_LastError() noexcept;
