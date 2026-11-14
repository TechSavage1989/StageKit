/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#endif

#include "updater.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <util/platform.h>
#include <util/threading.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#ifndef STAGEKIT_RELEASE
#define STAGEKIT_RELEASE ""
#endif

namespace stagekit {

namespace {

constexpr const char *kReleasesHost = "api.github.com";
constexpr const char *kReleasesPath = "/repos/TechSavage1989/StageKit/releases?per_page=20";

std::mutex g_mutex;
std::thread g_looking;

// "0.1.0-beta82" as numbers that sort: major, minor, patch, stage (beta 0,
// rc 1, finished 2) and the stage's own number.
struct Parts {
	long n[5] = {0, 0, 0, 2, 0};
};

Parts parts_of(const std::string &version)
{
	Parts p;
	size_t i = 0;
	for (int k = 0; k < 3; k++) {
		char *end = nullptr;
		p.n[k] = std::strtol(version.c_str() + i, &end, 10);
		i = (size_t)(end - version.c_str());
		if (i < version.size() && version[i] == '.')
			i++;
	}
	const size_t dash = version.find('-');
	if (dash != std::string::npos) {
		const std::string tail = version.substr(dash + 1);
		const bool rc = tail.rfind("rc", 0) == 0;
		p.n[3] = rc ? 1 : 0;
		size_t digits = 0;
		while (digits < tail.size() && !std::isdigit((unsigned char)tail[digits]))
			digits++;
		p.n[4] = digits < tail.size() ? std::strtol(tail.c_str() + digits, nullptr, 10) : 0;
	}
	return p;
}

bool ends_with(const std::string &text, const char *tail)
{
	const size_t n = strlen(tail);
	return text.size() >= n && text.compare(text.size() - n, n, tail) == 0;
}

// The SHA-256 the release notes give for a file: "<name>: <64 hex digits>".
std::string checksum_of(const std::string &notes, const std::string &file)
{
	size_t at = notes.find(file + ":");
	while (at != std::string::npos) {
		size_t i = at + file.size() + 1;
		while (i < notes.size() && (notes[i] == ' ' || notes[i] == '\t'))
			i++;
		size_t end = i;
		while (end < notes.size() && std::isxdigit((unsigned char)notes[end]))
			end++;
		if (end - i == 64)
			return notes.substr(i, 64);
		at = notes.find(file + ":", at + 1);
	}
	return {};
}

std::string settings_file()
{
	char *path = obs_module_config_path("updates.json");
	std::string out = path ? path : "";
	bfree(path);
	return out;
}

#ifdef _WIN32
std::wstring wide(const std::string &text)
{
	if (text.empty())
		return {};
	const int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0);
	std::wstring out((size_t)n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), out.data(), n);
	return out;
}

// One page from the web, over HTTPS, with Windows' own HTTP. Empty on failure.
std::string https_get(const char *host, const char *path)
{
	std::string body;
	HINTERNET session = WinHttpOpen(L"StageKit", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
					WINHTTP_NO_PROXY_BYPASS, 0);
	if (!session)
		return body;
	WinHttpSetTimeouts(session, 8000, 8000, 8000, 12000);
	HINTERNET connection = WinHttpConnect(session, wide(host).c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
	HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", wide(path).c_str(), nullptr,
							    WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
							    WINHTTP_FLAG_SECURE)
				       : nullptr;
	const wchar_t *headers = L"Accept: application/vnd.github+json\r\n";
	if (request && WinHttpSendRequest(request, headers, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
	    WinHttpReceiveResponse(request, nullptr)) {
		DWORD status = 0, size = sizeof(status);
		WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
				    WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
		if (status == 200) {
			for (;;) {
				DWORD available = 0;
				if (!WinHttpQueryDataAvailable(request, &available) || available == 0)
					break;
				std::string chunk(available, '\0');
				DWORD read = 0;
				if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0)
					break;
				body.append(chunk, 0, read);
				if (body.size() > 8 * 1024 * 1024)
					break; // far more than a list of releases
			}
		}
	}
	if (request)
		WinHttpCloseHandle(request);
	if (connection)
		WinHttpCloseHandle(connection);
	WinHttpCloseHandle(session);
	return body;
}

#else
std::string https_get(const char *, const char *)
{
	return {};
}
#endif

// Text for a PowerShell script, in single quotes.
std::string quoted(const std::string &text)
{
	std::string out = "'";
	for (char c : text)
		out += c == '\'' ? std::string("''") : std::string(1, c);
	return out + "'";
}

} // namespace

// The helper that installs a release, as a PowerShell script. It outlives
// OBS: it downloads the files and checks them against the checksums from the
// release notes (and stops there, saying so, if that fails: nothing has been
// touched yet), asks OBS to close and waits for it, runs the installer
// without its pages, hands the Stream Deck plugin to Stream Deck if that is
// installed, and starts OBS again.
std::string install_script(const Release &release, const std::string &obs_exe)
{
	std::string s;
	s += "$ErrorActionPreference = 'Stop'\r\n";
	s += "Add-Type -AssemblyName System.Windows.Forms\r\n";
	s += "function Fail($m) { [System.Windows.Forms.MessageBox]::Show($m, 'StageKit update') | Out-Null; exit 1 }\r\n";
	s += "function Fetch($url, $sha, $file) {\r\n";
	s += "  [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12\r\n";
	s += "  $ProgressPreference = 'SilentlyContinue'\r\n";
	s += "  Invoke-WebRequest -Uri $url -OutFile $file -UseBasicParsing\r\n";
	s += "  if ((Get-FileHash $file -Algorithm SHA256).Hash -ne $sha) { Remove-Item $file -Force; throw 'checksum' }\r\n";
	s += "}\r\n";
	s += "$dir = Join-Path $env:TEMP 'StageKitUpdate'\r\n";
	s += "New-Item -ItemType Directory -Force -Path $dir | Out-Null\r\n";
	s += "$setup = Join-Path $dir 'stagekit-setup.exe'\r\n";
	s += "$deck = Join-Path $dir 'stagekit.streamDeckPlugin'\r\n";
	s += "$deckUrl = " + quoted(release.deck_url) + "\r\n";
	s += "$deckHome = Join-Path $env:APPDATA 'Elgato\\StreamDeck\\Plugins\\com.techsavage1989.stagekit.sdPlugin'\r\n";
	s += "$hasDeck = ($deckUrl -ne '') -and (Test-Path $deckHome)\r\n";
	s += "try {\r\n";
	s += "  Fetch " + quoted(release.installer_url) + " " + quoted(release.installer_sha) + " $setup\r\n";
	s += "  if ($hasDeck) { Fetch $deckUrl " + quoted(release.deck_sha) + " $deck }\r\n";
	s += "} catch {\r\n";
	s += "  Fail 'The update could not be downloaded, or a file did not match its checksum. Nothing was changed.'\r\n";
	s += "}\r\n";
	s += "$obs = Get-Process obs64 -ErrorAction SilentlyContinue\r\n";
	s += "if ($obs) {\r\n";
	s += "  $obs | ForEach-Object { $_.CloseMainWindow() | Out-Null }\r\n";
	s += "  $obs | Wait-Process -Timeout 120 -ErrorAction SilentlyContinue\r\n";
	s += "  if (Get-Process obs64 -ErrorAction SilentlyContinue) {\r\n";
	s += "    Fail 'OBS did not close, so the update was not installed. Close OBS and start the update again from the StageKit dock.'\r\n";
	s += "  }\r\n";
	s += "}\r\n";
	s += "try {\r\n";
	s += "  Start-Process $setup -ArgumentList '/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART' -Wait\r\n";
	s += "} catch {\r\n";
	s += "  Fail 'The installer could not be started. StageKit was not changed.'\r\n";
	s += "}\r\n";
	s += "if ($hasDeck) { Start-Process $deck }\r\n";
	s += "$exe = " + quoted(obs_exe) + "\r\n";
	s += "if (Test-Path $exe) { Start-Process $exe -WorkingDirectory (Split-Path $exe) }\r\n";
	return s;
}

const char *current_release()
{
	return STAGEKIT_RELEASE;
}

int compare_versions(const std::string &a, const std::string &b)
{
	const Parts x = parts_of(a), y = parts_of(b);
	for (int k = 0; k < 5; k++)
		if (x.n[k] != y.n[k])
			return x.n[k] < y.n[k] ? -1 : 1;
	return 0;
}

bool newest_release(const std::string &json, bool betas, Release &out)
{
	// GitHub answers with a list; obs_data reads objects, so it is wrapped.
	obs_data_t *root = obs_data_create_from_json(("{\"releases\":" + json + "}").c_str());
	if (!root)
		return false;
	obs_data_array_t *releases = obs_data_get_array(root, "releases");
	bool any = false;
	for (size_t i = 0; i < obs_data_array_count(releases); i++) {
		obs_data_t *one = obs_data_array_item(releases, i);
		Release found;
		found.version = obs_data_get_string(one, "tag_name");
		found.page = obs_data_get_string(one, "html_url");
		const std::string notes = obs_data_get_string(one, "body");
		const bool skip = obs_data_get_bool(one, "draft") || (!betas && obs_data_get_bool(one, "prerelease")) ||
				  found.version.empty();
		obs_data_array_t *assets = obs_data_get_array(one, "assets");
		for (size_t k = 0; !skip && k < obs_data_array_count(assets); k++) {
			obs_data_t *asset = obs_data_array_item(assets, k);
			const std::string name = obs_data_get_string(asset, "name");
			const std::string url = obs_data_get_string(asset, "browser_download_url");
			if (ends_with(name, "-windows-x64-Installer.exe")) {
				found.installer_url = url;
				found.installer_sha = checksum_of(notes, name);
			} else if (ends_with(name, ".streamDeckPlugin")) {
				found.deck_url = url;
				found.deck_sha = checksum_of(notes, name);
			}
			obs_data_release(asset);
		}
		obs_data_array_release(assets);
		obs_data_release(one);
		// A release whose Windows files are not up yet (they are added a
		// few minutes after it appears) is passed over until they are.
		if (skip || found.installer_url.empty() || found.installer_sha.empty())
			continue;
		if (found.deck_sha.empty())
			found.deck_url.clear(); // never a file that cannot be checked
		if (!any || compare_versions(found.version, out.version) > 0) {
			out = found;
			any = true;
		}
	}
	obs_data_array_release(releases);
	obs_data_release(root);
	return any;
}

UpdateSettings update_settings()
{
	UpdateSettings s;
	const std::string file = settings_file();
	obs_data_t *data = file.empty() ? nullptr : obs_data_create_from_json_file(file.c_str());
	if (data) {
		if (obs_data_has_user_value(data, "check_at_start"))
			s.check_at_start = obs_data_get_bool(data, "check_at_start");
		if (obs_data_has_user_value(data, "betas"))
			s.betas = obs_data_get_bool(data, "betas");
		obs_data_release(data);
	}
	return s;
}

void set_update_settings(const UpdateSettings &settings)
{
	const std::string file = settings_file();
	if (file.empty())
		return;
	char *folder = obs_module_config_path("");
	if (folder)
		os_mkdirs(folder);
	bfree(folder);
	obs_data_t *data = obs_data_create();
	obs_data_set_bool(data, "check_at_start", settings.check_at_start);
	obs_data_set_bool(data, "betas", settings.betas);
	obs_data_save_json(data, file.c_str());
	obs_data_release(data);
}

void check_for_update(std::function<void(bool, const Release &)> done)
{
	std::lock_guard lock(g_mutex);
	if (g_looking.joinable())
		g_looking.join(); // the one before has long finished, or is about to
	const bool betas = update_settings().betas;
	g_looking = std::thread([done, betas] {
		os_set_thread_name("stagekit: update check");
		const std::string json = https_get(kReleasesHost, kReleasesPath);
		Release newest;
		const bool found = !json.empty() && newest_release(json, betas, newest);
		if (found && compare_versions(newest.version, current_release()) <= 0)
			newest = Release(); // nothing newer than this build
		if (!found)
			obs_log(LOG_INFO, "Updates: the list of releases could not be read");
		else if (newest.version.empty())
			obs_log(LOG_INFO, "Updates: %s is the newest", current_release());
		else
			obs_log(LOG_INFO, "Updates: %s is available (this is %s)", newest.version.c_str(),
				current_release());
		done(found, newest);
	});
}

bool can_install_updates()
{
#ifdef _WIN32
	return true;
#else
	return false;
#endif
}

bool install_update(const Release &release, std::string &error)
{
	if (obs_frontend_streaming_active() || obs_frontend_recording_active() || obs_frontend_replay_buffer_active()) {
		error = obs_module_text("Update.Busy");
		return false;
	}
#ifdef _WIN32
	wchar_t exe[MAX_PATH] = L"";
	GetModuleFileNameW(nullptr, exe, MAX_PATH);
	std::string obs_path;
	{
		const int n = WideCharToMultiByte(CP_UTF8, 0, exe, -1, nullptr, 0, nullptr, nullptr);
		obs_path.assign((size_t)std::max(0, n - 1), '\0');
		WideCharToMultiByte(CP_UTF8, 0, exe, -1, obs_path.data(), n, nullptr, nullptr);
	}
	const std::string script = install_script(release, obs_path);

	wchar_t temp[MAX_PATH] = L"";
	GetTempPathW(MAX_PATH, temp);
	const std::wstring file = std::wstring(temp) + L"stagekit-update.ps1";
	HANDLE out =
		CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (out == INVALID_HANDLE_VALUE) {
		error = obs_module_text("Update.NoHelper");
		return false;
	}
	DWORD written = 0;
	const char bom[] = "\xEF\xBB\xBF"; // so PowerShell reads it as UTF-8
	WriteFile(out, bom, 3, &written, nullptr);
	WriteFile(out, script.data(), (DWORD)script.size(), &written, nullptr);
	CloseHandle(out);

	std::wstring command =
		L"powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"" + file + L"\"";
	STARTUPINFOW start = {};
	start.cb = sizeof(start);
	PROCESS_INFORMATION process = {};
	if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS,
			    nullptr, nullptr, &start, &process)) {
		error = obs_module_text("Update.NoHelper");
		return false;
	}
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	obs_log(LOG_INFO, "Updates: installing %s", release.version.c_str());
	return true;
#else
	(void)release;
	error = obs_module_text("Update.WindowsOnly");
	return false;
#endif
}

void stop_updater()
{
	std::lock_guard lock(g_mutex);
	if (g_looking.joinable())
		g_looking.join();
}

void register_updater_test_hooks()
{
	proc_handler_add(
		obs_get_proc_handler(),
		"void stagekit_update_newest(in string json, in bool betas, out bool found, out string version, "
		"out string installer, out string sha, out string deck)",
		[](void *, calldata_t *cd) {
			Release r;
			const bool found = newest_release(calldata_string(cd, "json"), calldata_bool(cd, "betas"), r);
			calldata_set_bool(cd, "found", found);
			calldata_set_string(cd, "version", r.version.c_str());
			calldata_set_string(cd, "installer", r.installer_url.c_str());
			calldata_set_string(cd, "sha", r.installer_sha.c_str());
			calldata_set_string(cd, "deck", r.deck_url.c_str());
		},
		nullptr);
	proc_handler_add(
		obs_get_proc_handler(), "void stagekit_update_compare(in string a, in string b, out int order)",
		[](void *, calldata_t *cd) {
			calldata_set_int(cd, "order",
					 compare_versions(calldata_string(cd, "a"), calldata_string(cd, "b")));
		},
		nullptr);
	proc_handler_add(
		obs_get_proc_handler(),
		"void stagekit_update_script(in string installer, in string sha, in string deck, in string deck_sha, "
		"in string obs, out string script)",
		[](void *, calldata_t *cd) {
			Release r;
			r.installer_url = calldata_string(cd, "installer");
			r.installer_sha = calldata_string(cd, "sha");
			r.deck_url = calldata_string(cd, "deck");
			r.deck_sha = calldata_string(cd, "deck_sha");
			calldata_set_string(cd, "script", install_script(r, calldata_string(cd, "obs")).c_str());
		},
		nullptr);
	proc_handler_add(
		obs_get_proc_handler(), "void stagekit_update_current(out string version)",
		[](void *, calldata_t *cd) { calldata_set_string(cd, "version", current_release()); }, nullptr);
}

} // namespace stagekit
