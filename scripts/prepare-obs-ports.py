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


def modern_obs_api(text):
    text = text.replace("util/circlebuf.h", "util/deque.h")
    text = re.sub(r"\bcirclebuf\b", "deque", text)
    return re.sub(r"\bcirclebuf_", "deque_", text)


def adapt_file(text, key, filename, settings):
    if "OBS_DECLARE_MODULE()" in text:
        # Plugin Manager reads this export separately from source, filter and Tools labels.
        label = f'return sk_text("Ports.Module.{key}");'
        callback = r'(\bobs_module_name\(\s*(?:void)?\s*\)\s*\{\s*)return [^;]+;'
        text, count = re.subn(callback, lambda match: match[1] + label, text)
        if not count:
            text += '\nMODULE_EXPORT const char *obs_module_name(void)\n{\n\t' + label + '\n}\n'
    types = [row[:3] for row in settings.get("types", [])]
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
    text = text.replace('obs_get_module("move-transition")', 'obs_get_module("stagekit-port-move")')
    for name in ("move_get_transition_filter_function", "get_transition_table_transition"):
        text = text.replace('"' + name + '"', '"stagekit_port_' + name + '"')
        text = text.replace('"void ' + name + '(', '"void stagekit_port_' + name + '(')
    if key == "move":
        if filename in ("move-source-filter.c", "move-source-swap-filter.c"):
            swap = filename == "move-source-swap-filter.c"
            prefix = "move_source_swap" if swap else "move_source"
            marker = "struct obs_source_info " + prefix + "_filter = {"
            detach = (
                '    move_source_swap_source_changed(f, &f->source_name1, "", &f->scene_item1);\n'
                '    move_source_swap_source_changed(f, &f->source_name2, "", &f->scene_item2);'
                if swap else '    move_source_source_changed(f, "");')
            # libobs clears the parent before deferred filter destruction. Disconnect while both objects are live.
            handler = f"""static void sk_detach_move(void *data, obs_source_t *parent)
{{
    struct {prefix}_info *f = data;
    signal_handler_t *signals = obs_source_get_signal_handler(parent);
    signal_handler_disconnect(signals, "item_remove", {prefix}_item_remove, f);
    signal_handler_disconnect(signals, "remove", {prefix}_scene_remove, f);
    signal_handler_disconnect(signals, "destroy", {prefix}_scene_remove, f);
{detach}
}}

"""
            text = replace_once(text, marker, handler + marker + "\n\t.filter_remove = sk_detach_move,")
        text = re.sub(r'strcmp\((id(?:_a|_b)?), "source-clone"\) == 0',
                      lambda m: '(' + m[0] + ' || strcmp(' + m[1] + ', "stagekit_port_source-clone") == 0)', text)
        text = text.replace('obs_get_module("obs-shaderfilter")', 'obs_get_module("stagekit-port-shader")')
    if key == "composite" and filename == "obs-composite-blur-filter.c":
        # The original property dialog selects Gaussian and Area. Filters created by
        # hotkey or restored with missing settings also need a working renderer.
        radius = 'obs_data_set_default_double(settings, "radius", 10.0);'
        text = replace_once(text, radius,
                            'obs_data_set_default_int(settings, "blur_algorithm", ALGO_GAUSSIAN);\n\t'
                            'obs_data_set_default_int(settings, "blur_type", TYPE_AREA);\n\t' + radius)
    if key == "quick":
        text = text.replace('"source-clone"', '"stagekit_port_source-clone"')
        text = text.replace('"source_release"', '"source_destroy"')
        if filename == "quick-access-utility.cpp":
            text = text.replace("std::string uuid = obs_source_get_uuid(newScene);", "if (!newScene) return;\n\tstd::string uuid = obs_source_get_uuid(newScene);")
            text = text.replace("QuickAccessSource *qaNewScene = _allSources[uuid].get();",
                                "auto it = _allSources.find(uuid);\n\tQuickAccessSource *qaNewScene = it != _allSources.end() ? it->second.get() : nullptr;")
            text = text.replace("QuickAccessSearchModal::dialog->InitializeSearch();", "if (QuickAccessSearchModal::dialog) QuickAccessSearchModal::dialog->InitializeSearch();")
        text = text.replace('"quick_access_utility"', '"stagekit.quick_access"')
        text = text.replace('"quick-access-dock_', '"stagekit-quick-access-dock_')
    if key == "table":
        for name in ("transition-table.enable", "transition-table.disable"):
            text = text.replace('"' + name + '"', '"stagekit.port.' + name + '"')
    if key in ("table", "dock"):
        legacy = {"table": "transition-table", "dock": "source-dock"}[key]
        text = re.sub(r'(obs_data_(?:get|set)_obj\(\s*save_data,\s*")' + legacy + '"',
                      r'\1stagekit.' + key + '"', text)
    if key in ("copy", "collections"):
        for name in ("copyTransformHotkey", "pasteTransformHotkey", "sceneCollectionManagerHotkey",
                     "backupHotkey", "loadLastBackupHotkey", "loadFirstBackupHotkey"):
            text = text.replace('"' + name + '"', '"stagekit.port.' + name + '"')
        text = text.replace("source_copy_temp", "stagekit_source_copy_temp").replace("Source Copy Temp", "StageKit Source Copy Temp")
    if key == "dock":
        text = text.replace("obs_frontend_add_dock_by_id(title, title,", 'obs_frontend_add_dock_by_id((QByteArray("stagekit-source-dock-") + title).constData(), title,')
        text = text.replace("obs_frontend_add_dock_by_id(t.constData(), t.constData(),", 'obs_frontend_add_dock_by_id((QByteArray("stagekit-source-dock-") + t).constData(), t.constData(),')
        text = re.sub(r'obs_frontend_remove_dock\((.+?)->objectName\(\).toUtf8\(\).constData\(\)\)',
                      r'obs_frontend_remove_dock((QByteArray("stagekit-source-dock-") + \1->objectName().toUtf8()).constData())', text)
    if key == "collections" and filename == "scene-collection-manager.cpp":
        text = text.replace('"SceneCollectionManager",', '"StageKitCollectionManager",')
        text = text.replace('"scene_collection_manager_temp"', '"stagekit_collection_manager_temp"')
        text = replace_once(text, "static std::string customBackupDir;", """static std::string customBackupDir;
static std::string stagekitBackupDir()
{
    char *path = obs_module_config_path("collection-backups");
    std::string result = path ? path : "";
    bfree(path);
    return result;
}""")
        text = text.replace("std::string backupDir = filename;", "std::string backupDir = stagekitBackupDir();")
        start = text.index("if (customBackupDir.empty())", text.index("std::string GetBackupDirectory(std::string filename)\n{"))
        end = text.index("filename = GetFilenameFromPath", start)
        text = text[:start] + text[end:]
        text = text.replace("std::string dir = customBackupDir;", "std::string dir = customBackupDir.empty() ? stagekitBackupDir() : customBackupDir;")
        text = text.replace("auto ptr = SceneCollectionsPath();", "auto ptr = stagekitBackupDir();")
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


def prepare(origin, reference=False):
    key = origin["key"]
    settings = spec[key]
    source, dest = ports / key, build / key
    if reference:
        dest = build / "reference" / key
    labels = {row[1]: row[2] for row in settings.get("types", [])}
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
        text = modern_obs_api(text)
        if not reference:
            text = adapt_file(text, key, path.name, settings)
        if ('sk_text(' in text or re.search(r'\bsk_\w+get_name|\bget_name\s*=\s*sk_', text)) and path.suffix in (".cpp", ".cc", ".c"):
            text = '#include "stagekit-port-names.h"\n' + text
        write_if_changed(dest / rel, text)


for project in origins:
    if len(sys.argv) < 4:
        prepare(project)
    elif project["key"] in ("move", "shader", "composite"):
        prepare(project, reference=True)
