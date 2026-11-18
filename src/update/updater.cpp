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
// Every release is read, a hundred to a page. GitHub does not list them
// newest first as it says: with 99 releases, beta100 came 33rd, and a build
// that read the first 20 told the owner that beta99 was the newest.
constexpr const char *kReleasesPath = "/repos/TechSavage1989/StageKit/releases?per_page=100&page=";
constexpr int kMostPages = 5;

std::mutex g_mutex;
std::thread g_looking;
#ifdef _WIN32
HANDLE g_helper = nullptr; // the install's helper, while it runs
#endif

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

// Markdown made plain for the dock: emphasis, code marks and link targets
// taken out, runs of spaces made one.
std::string plain(const std::string &text)
{
	std::string out;
	auto word = [](char c) {
		return std::isalnum((unsigned char)c) != 0;
	};
	for (size_t i = 0; i < text.size(); i++) {
		const char c = text[i];
		if (c == '*' || c == '`')
			continue;
		// An underscore marks emphasis only at a word's edge (not in snake_case).
		if (c == '_' && !(i > 0 && word(text[i - 1]) && i + 1 < text.size() && word(text[i + 1])))
			continue;
		// [text](target): the text alone.
		if (c == ']' && i + 1 < text.size() && text[i + 1] == '(') {
			const size_t close = text.find(')', i + 2);
			if (close != std::string::npos) {
				i = close;
				continue;
			}
		}
		if (c == '[' && text.find("](", i) != std::string::npos)
			continue;
		if (std::isspace((unsigned char)c)) {
			if (!out.empty() && out.back() != ' ')
				out += ' ';
			continue;
		}
		out += c;
	}
	while (!out.empty() && out.back() == ' ')
		out.pop_back();
	return out;
}

std::string trimmed(const std::string &line)
{
	size_t a = 0, b = line.size();
	while (a < b && std::isspace((unsigned char)line[a]))
		a++;
	while (b > a && std::isspace((unsigned char)line[b - 1]))
		b--;
	return line.substr(a, b - a);
}

// Text cut to about `most` characters at a space, with an ellipsis.
std::string shortened(const std::string &text, size_t most)
{
	if (text.size() <= most)
		return text;
	size_t cut = text.rfind(' ', most);
	if (cut == std::string::npos || cut < most / 2)
		cut = most;
	return text.substr(0, cut) + "...";
}

// What a bullet says at a glance: its bold lead ("- **Lead.** more"), else
// its first sentence.
std::string lead_of(const std::string &bullet)
{
	std::string lead;
	if (bullet.rfind("**", 0) == 0) {
		const size_t end = bullet.find("**", 2);
		if (end != std::string::npos)
			lead = bullet.substr(2, end - 2);
	}
	if (lead.empty()) {
		lead = bullet;
		for (size_t i = 0; i + 1 < lead.size(); i++)
			if (lead[i] == '.' && lead[i + 1] == ' ') {
				lead.resize(i);
				break;
			}
	}
	lead = plain(lead);
	while (!lead.empty() && (lead.back() == '.' || lead.back() == ':' || lead.back() == ' '))
		lead.pop_back();
	return shortened(lead, 110);
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
	// What it did, step by step, for when an update does not go through.
	s += "$log = Join-Path $dir 'update.log'\r\n";
	s += "function Note($m) { Add-Content -Path $log -Value ((Get-Date -Format s) + ' ' + $m) }\r\n";
	s += "Set-Content -Path $log -Value 'StageKit update'\r\n";
	s += "trap { Note ('stopped: ' + $_); Fail ('The update stopped: ' + $_ + ' StageKit was not changed.') }\r\n";
	s += "Note 'downloading'\r\n";
	s += "$setup = Join-Path $dir 'stagekit-setup.exe'\r\n";
	s += "$deck = Join-Path $dir 'stagekit.streamDeckPlugin'\r\n";
	s += "$deckUrl = " + quoted(release.deck_url) + "\r\n";
	s += "$deckHome = Join-Path $env:APPDATA 'Elgato\\StreamDeck\\Plugins\\com.techsavage1989.stagekit.sdPlugin'\r\n";
	s += "$hasDeck = ($deckUrl -ne '') -and (Test-Path $deckHome)\r\n";
	s += "try {\r\n";
	s += "  Fetch " + quoted(release.installer_url) + " " + quoted(release.installer_sha) + " $setup\r\n";
	s += "  if ($hasDeck) { Fetch $deckUrl " + quoted(release.deck_sha) + " $deck }\r\n";
	s += "} catch {\r\n";
	s += "  Note ('download failed: ' + $_)\r\n";
	s += "  Fail 'The update could not be downloaded, or a file did not match its checksum. Nothing was changed.'\r\n";
	s += "}\r\n";
	s += "Note 'downloaded and checked; closing OBS'\r\n";
	s += "$obs = Get-Process obs64 -ErrorAction SilentlyContinue\r\n";
	s += "if ($obs) {\r\n";
	s += "  $obs | ForEach-Object { $_.CloseMainWindow() | Out-Null }\r\n";
	s += "  $obs | Wait-Process -Timeout 120 -ErrorAction SilentlyContinue\r\n";
	s += "  if (Get-Process obs64 -ErrorAction SilentlyContinue) {\r\n";
	s += "    Fail 'OBS did not close, so the update was not installed. Close OBS and start the update again from the StageKit dock.'\r\n";
	s += "  }\r\n";
	s += "}\r\n";
	s += "Note 'installing'\r\n";
	s += "try {\r\n";
	s += "  Start-Process $setup -ArgumentList '/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART' -Wait\r\n";
	s += "} catch {\r\n";
	s += "  Fail 'The installer could not be started. StageKit was not changed.'\r\n";
	s += "}\r\n";
	s += "if ($hasDeck) { Start-Process $deck }\r\n";
	s += "$exe = " + quoted(obs_exe) + "\r\n";
	s += "if (Test-Path $exe) { Start-Process $exe -WorkingDirectory (Split-Path $exe) }\r\n";
	s += "Note 'done'\r\n";
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

ReleaseNotes notes_of(const std::string &version, const std::string &body)
{
	ReleaseNotes notes;
	notes.version = version;
	std::vector<std::string> lines;
	for (size_t at = 0; at <= body.size();) {
		size_t end = body.find('\n', at);
		if (end == std::string::npos)
			end = body.size();
		std::string line = body.substr(at, end - at);
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		lines.push_back(line);
		at = end + 1;
	}
	enum class Part { Top, Summary, Wanted, Other } part = Part::Top;
	std::string summary, bullet;
	auto take = [&] {
		if (!bullet.empty() && notes.points.size() < kMostPoints) {
			const std::string lead = lead_of(trimmed(bullet));
			if (!lead.empty())
				notes.points.push_back(lead);
		}
		bullet.clear();
	};
	for (const std::string &raw : lines) {
		const std::string line = trimmed(raw);
		// The checksums, and the line that says which files are still on
		// their way, come last: nothing after them is notes.
		if (line.rfind("### Checksums", 0) == 0 ||
		    (line.rfind("_The ", 0) == 0 && line.find("still being built") != std::string::npos))
			break;
		if (line.rfind('#', 0) == 0) {
			take();
			std::string title = line;
			while (!title.empty() && title[0] == '#')
				title.erase(0, 1);
			title = trimmed(title);
			std::string low;
			for (char c : title)
				low += (char)std::tolower((unsigned char)c);
			// Older notes wrote "What's new" and "Added".
			if (low.rfind("what's ", 0) == 0)
				low.erase(0, 7);
			// "# StageKit <version>" (a body written in full) is no section.
			if (line.rfind("# ", 0) == 0 && part == Part::Top)
				continue;
			const bool wanted = low.rfind("new", 0) == 0 || low.rfind("changed", 0) == 0 ||
					    low.rfind("fixed", 0) == 0 || low.rfind("added", 0) == 0;
			part = wanted ? Part::Wanted : Part::Other;
			continue;
		}
		if (part == Part::Top || part == Part::Summary) {
			if (line.empty()) {
				if (part == Part::Summary)
					part = Part::Other; // the summary is the first paragraph only
				continue;
			}
			part = Part::Summary;
			summary += (summary.empty() ? "" : " ") + line;
			continue;
		}
		if (part != Part::Wanted)
			continue;
		const bool top_bullet = (raw.rfind("- ", 0) == 0 || raw.rfind("* ", 0) == 0);
		if (top_bullet) {
			take();
			bullet = raw.substr(2);
		} else if (line.empty()) {
			take();
		} else if (!bullet.empty() && raw.rfind("  ", 0) == 0 && trimmed(raw).rfind("- ", 0) != 0 &&
			   trimmed(raw).rfind("* ", 0) != 0) {
			bullet += " " + line; // the bullet goes on, indented
		} else {
			take(); // a bullet under a bullet, or plain text: not a point
		}
	}
	take();
	notes.summary = shortened(plain(summary), 300);
	return notes;
}

void read_changes(const std::string &json, const std::string &installed, std::vector<ReleaseNotes> &out)
{
	obs_data_t *root = obs_data_create_from_json(("{\"releases\":" + json + "}").c_str());
	if (!root)
		return;
	obs_data_array_t *releases = obs_data_get_array(root, "releases");
	for (size_t i = 0; i < obs_data_array_count(releases); i++) {
		obs_data_t *one = obs_data_array_item(releases, i);
		const std::string version = obs_data_get_string(one, "tag_name");
		if (!version.empty() && !obs_data_get_bool(one, "draft") && compare_versions(version, installed) > 0)
			out.push_back(notes_of(version, obs_data_get_string(one, "body")));
		obs_data_release(one);
	}
	obs_data_array_release(releases);
	obs_data_release(root);
}

int order_changes(std::vector<ReleaseNotes> &changes, const std::string &upto, size_t most)
{
	changes.erase(std::remove_if(changes.begin(), changes.end(),
				     [&](const ReleaseNotes &n) { return compare_versions(n.version, upto) > 0; }),
		      changes.end());
	std::stable_sort(changes.begin(), changes.end(), [](const ReleaseNotes &a, const ReleaseNotes &b) {
		return compare_versions(a.version, b.version) > 0;
	});
	changes.erase(std::unique(changes.begin(), changes.end(),
				  [](const ReleaseNotes &a, const ReleaseNotes &b) {
					  return compare_versions(a.version, b.version) == 0;
				  }),
		      changes.end());
	const int left_out = changes.size() > most ? (int)(changes.size() - most) : 0;
	if (left_out)
		changes.resize(most);
	return left_out;
}

std::string changes_text(const std::vector<ReleaseNotes> &changes, int earlier)
{
	std::string out;
	for (const ReleaseNotes &n : changes) {
		out += n.version + (n.summary.empty() ? "" : ": " + n.summary) + "\n";
		for (const std::string &point : n.points)
			out += "  - " + point + "\n";
	}
	if (earlier > 0)
		out += "and " + std::to_string(earlier) + " earlier\n";
	return out;
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
		Release newest;
		bool found = false;
		std::vector<ReleaseNotes> changes; // what came after this build, from the same pages
		for (int page = 1; page <= kMostPages; page++) {
			const std::string json =
				https_get(kReleasesHost, (std::string(kReleasesPath) + std::to_string(page)).c_str());
			// Nothing, or an empty list: that was the last page.
			if (json.find('{') == std::string::npos)
				break;
			read_changes(json, current_release(), changes);
			Release best;
			if (newest_release(json, betas, best) &&
			    (!found || compare_versions(best.version, newest.version) > 0)) {
				newest = best;
				found = true;
			}
			// A page that is not full is the last one: no need to ask again.
			size_t entries = 0;
			for (size_t at = json.find("\"tag_name\""); at != std::string::npos;
			     at = json.find("\"tag_name\"", at + 1))
				entries++;
			if (entries < 100)
				break;
		}
		if (found && compare_versions(newest.version, current_release()) <= 0)
			newest = Release(); // nothing newer than this build
		if (!newest.version.empty()) {
			newest.earlier = order_changes(changes, newest.version, kMostChanges);
			newest.changes = std::move(changes);
		}
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
	// A console of its own that is not shown. Started with no console at all
	// (DETACHED_PROCESS, the first build), PowerShell never got going: the
	// owner's dock said "Downloading" and nothing followed.
	if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
			    &start, &process)) {
		error = obs_module_text("Update.NoHelper");
		return false;
	}
	CloseHandle(process.hThread);
	{
		std::lock_guard lock(g_mutex);
		if (g_helper)
			CloseHandle(g_helper);
		g_helper = process.hProcess;
	}
	obs_log(LOG_INFO, "Updates: installing %s", release.version.c_str());
	return true;
#else
	(void)release;
	error = obs_module_text("Update.WindowsOnly");
	return false;
#endif
}

UpdateState update_state()
{
#ifdef _WIN32
	std::lock_guard lock(g_mutex);
	if (!g_helper)
		return UpdateState::None;
	DWORD code = STILL_ACTIVE;
	if (!GetExitCodeProcess(g_helper, &code) || code == STILL_ACTIVE)
		return UpdateState::Running;
	// It ends by itself only when it gave up: when it works, OBS is closed
	// before it is done.
	CloseHandle(g_helper);
	g_helper = nullptr;
	return UpdateState::Stopped;
#else
	return UpdateState::None;
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
		obs_get_proc_handler(),
		"void stagekit_update_notes(in string body, out string summary, out string points, out int count)",
		[](void *, calldata_t *cd) {
			const ReleaseNotes n = notes_of("", calldata_string(cd, "body"));
			std::string points;
			for (const std::string &p : n.points)
				points += p + "\n";
			calldata_set_string(cd, "summary", n.summary.c_str());
			calldata_set_string(cd, "points", points.c_str());
			calldata_set_int(cd, "count", (long long)n.points.size());
		},
		nullptr);
	proc_handler_add(
		obs_get_proc_handler(),
		"void stagekit_update_changes(in string json, in string installed, in string upto, in int most, "
		"out string text, out int count, out int earlier)",
		[](void *, calldata_t *cd) {
			std::vector<ReleaseNotes> changes;
			read_changes(calldata_string(cd, "json"), calldata_string(cd, "installed"), changes);
			const int earlier =
				order_changes(changes, calldata_string(cd, "upto"), (size_t)calldata_int(cd, "most"));
			calldata_set_string(cd, "text", changes_text(changes, earlier).c_str());
			calldata_set_int(cd, "count", (long long)changes.size());
			calldata_set_int(cd, "earlier", earlier);
		},
		nullptr);
	proc_handler_add(
		obs_get_proc_handler(), "void stagekit_update_current(out string version)",
		[](void *, calldata_t *cd) { calldata_set_string(cd, "version", current_release()); }, nullptr);
}

} // namespace stagekit
