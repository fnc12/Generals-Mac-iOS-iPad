#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace end_editor {

    // One object template from Data/INI/Object (a unit, building, prop...), with the fields the
    // editor lists. Fields the INI does not set stay empty.
    struct ObjectInfo {
        std::string name;
        std::string definition;  // "Object", "ObjectReskin" or "ChildObject"
        std::string parent;  // template this one copies (ObjectReskin, ChildObject)
        std::string side;
        std::string kindOf;
        std::string buildable;
        std::string editorSorting;
        std::string displayName;  // label for the string table, e.g. "OBJECT:Ranger"
        std::string model;  // W3D model of the first Draw module (default condition state first)
        // What that Draw module keeps hidden when idle, space separated: the WeaponMuzzleFlash
        // names (bones "<name>01", "<name>02"... show only while firing) and the default state's
        // HideSubObject names.
        std::string muzzleFlashes;
        std::string hiddenSubObjects;
        std::string sourceFile;
        int line = 0;
    };

    struct ObjectParseResult {
        std::vector<ObjectInfo> objects;
        std::vector<std::string> warnings;
    };

    // Scans INI text for top-level Object / ObjectReskin / ChildObject blocks. This is not the
    // engine's INI parser: it only tracks block nesting (which keywords open a block that ends
    // with "End") and reads a few fields.
    ObjectParseResult parseObjects(std::string_view text, std::string_view sourceFile);

    // Fills empty fields of ObjectReskin / ChildObject entries from their parents.
    void resolveInheritance(std::vector<ObjectInfo> &objects);
}
