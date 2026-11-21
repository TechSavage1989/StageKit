"""Adapt the vendored OBS sources in the build directory, leaving each upstream file intact."""
import json
import re
import sys
from pathlib import Path

root, build = (Path(p) for p in sys.argv[1:3])
ports = root / "src/third-party/ports"
spec = json.loads((root / "data/ports.json").read_text(encoding="utf-8"))
origins = json.loads((ports / "origins.json").read_text(encoding="utf-8"))["projects"]


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise ValueError("Pinned source no longer matches: " + old)
    return text.replace(old, new, 1)


def write_if_changed(path, text):
    if path.is_file() and path.read_text(encoding="utf-8") == text:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")


def adapt_file(text, key, filename, settings):
    types = settings.get("types", [])
    for old, callback, label in types:
        text = text.replace('"' + old + '"', '"stagekit_port_' + old + '"')
        text = re.sub(r'(\bget_name\s*=\s*)' + callback + r'\b', r'\1sk_' + callback, text)
    if "tool" in settings:
        # The actual action remains connected to the original dialog. Only the menu title changes.
        text = re.sub(r'(obs_frontend_add_tools_menu_qaction\(\s*)(?:obs_module_text\("[^"]*"\)|"[^"]*")(\s*\))',
                      lambda m: m[1] + 'sk_text("' + settings["tool"] + '")' + m[2], text)
        text = text.replace('obs_frontend_add_tools_menu_item("Dock Layout Manager",',
                            'obs_frontend_add_tools_menu_item(sk_text("Ports.Tool.layouts"),')
    text = re.sub(r'(obs_websocket_register_vendor\(")([^"]+)', r'\1stagekit-port-\2', text)
    text = re.sub(r'(obs_hotkey_register_frontend\(\s*")([^"]+)', r'\1stagekit.port.\2', text)
    # Source UUIDs and user-assigned filter names must never be rewritten.
    if key == "shader":
        text = text.replace('obs_get_module("move-transition")', 'obs_get_module("stagekit-port-move")')
    if key == "move":
        text = text.replace('obs_get_module("obs-shaderfilter")', 'obs_get_module("stagekit-port-shader")')
    if key == "quick":
        text = text.replace('"quick_access_utility"', '"stagekit.quick_access"')
        text = text.replace('"quick-access-dock_', '"stagekit-quick-access-dock_')
    if key in ("table", "dock"):
        legacy = {"table": "transition-table", "dock": "source-dock"}[key]
        text = re.sub(r'(obs_data_(?:get|set)_obj\(\s*save_data,\s*")' + legacy + '"',
                      r'\1stagekit.' + key + '"', text)
    if key in ("copy", "collections"):
        for name in ("copyTransformHotkey", "pasteTransformHotkey", "sceneCollectionManagerHotkey",
                     "backupHotkey", "loadLastBackupHotkey", "loadFirstBackupHotkey"):
            text = text.replace('"' + name + '"', '"stagekit.port.' + name + '"')
        text = text.replace("scenes/source_copy_temp.json", "scenes/stagekit_source_copy_temp.json")
    if key == "layouts" and filename == "plugin-main.cpp":
        start = text.index("    const char *moduleFilePathCStr")
        end = text.index("    // Log the settings file path", start)
        text = text[:start] + (
            '    char *file = obs_module_config_path("dock-layouts.ini");\n'
            '    settingsFilePath = QString::fromUtf8(file);\n'
            '    bfree(file);\n'
            '    QString moduleDir = QFileInfo(settingsFilePath).absolutePath();\n\n') + text[end:]
    if key == "ndi" and filename == "plugin-main.cpp":
        # Missing runtimes are reported in the OBS log. Loading StageKit must never open a vendor error dialog.
        text = text.replace("showCriticalUnloadingMessageBoxDelayed(", "sk_optional_ndi_message(")
        text = re.sub(r'(void sk_optional_ndi_message\([^{};]*\)\s*\{)', r'\1\n\treturn;', text)
    if key == "ndi":
        # Use the StageKit updater. Never enable an output saved by a different NDI module.
        text = text.replace('#define SECTION_NAME "NDIPlugin"', '#define SECTION_NAME "StageKitPortsNDI"')
        if filename == "plugin-main.cpp":
            text = text.replace("\tupdateCheckStart();", "")
            text = re.sub(r'\s*alpha_filter_info = create_alpha_filter_info\(\);\s*obs_register_source\(&alpha_filter_info\);', '', text)
    return text


def prepare(origin):
    key = origin["key"]
    settings = spec[key]
    source, dest = ports / key, build / key
    labels = {callback: label for _, callback, label in settings.get("types", [])}
    header = ['#pragma once', '#include <obs-module.h>',
              'static inline const char *sk_text(const char *key) {',
              '    return obs_module_get_locale_text(obs_get_module("stagekit"), key);', '}']
    for callback, label in labels.items():
        header.append(f'static inline const char *sk_{callback}(void *unused) {{ (void)unused; return sk_text("{label}"); }}')
    write_if_changed(dest / "stagekit-port-names.h", "\n".join(header) + "\n")
    variables = {"PROJECT_VERSION": origin["version"], "PROJECT_FULL_NAME": "StageKit " + key,
                 "CMAKE_PROJECT_NAME": "stagekit-port-" + key, "CMAKE_PROJECT_VERSION": origin["version"]}
    for part, value in zip(("MAJOR", "MINOR", "PATCH"), origin["version"].split(".")):
        variables["PROJECT_VERSION_" + part] = value
    for path in sorted(source.rglob("*")):
        if not path.is_file() or not (path.suffix in {".h", ".hpp", ".c", ".cpp", ".cc", ".mm", ".ui", ".qrc", ".inl"} or
                                     path.name in {"version.h.in", "plugin-support.c.in"}):
            continue
        text = path.read_text(encoding="utf-8-sig")
        rel = path.relative_to(source)
        if path.suffix == ".in":
            rel = rel.with_suffix("")
            for name, value in variables.items():
                text = text.replace("${" + name + "}", value).replace("@" + name + "@", value)
        elif path.name == "version.h" and path.with_name("version.h.in").exists():
            continue
        text = adapt_file(text, key, path.name, settings)
        if ('sk_text(' in text or re.search(r'\bsk_\w+get_name|\bget_name\s*=\s*sk_', text)) and path.suffix in (".cpp", ".cc", ".c"):
            text = '#include "stagekit-port-names.h"\n' + text
        write_if_changed(dest / rel, text)


for project in origins:
    prepare(project)
