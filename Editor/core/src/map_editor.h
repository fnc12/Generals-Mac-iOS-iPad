#pragma once

#include "commands.h"
#include "map_document.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace end_editor {

    // What the user is doing with the open map, and what their input means: the placement tool
    // (object template, waypoint, road or bridge type) with its owner and angle, a right click
    // waiting for a template, the first point of a road, the selection, a mouse gesture in
    // progress, the clipboard and the note shown to the user. The client sends input events as
    // commands (EditorCore::send parses them into commands::Edit values) and draws what the store
    // says; every rule is here.
    //
    // The client does what needs its camera: the ground point under the cursor (world x, y; empty
    // when the cursor is off the map), the objects under it (ids, nearest first) and those inside
    // a rubber band. Everything else (what a click, a drag, a right click or a key does) is
    // decided here.
    class MapEditor {
      public:
        using Point = commands::Point;
        struct Clip {
            MapObject object;  // template, flags, height above the ground, angle, properties
            float dx = 0, dy = 0;  // from the middle of what was copied
        };
        // What the store's map_editor row shows (see storage.h).
        struct State {
            std::string tool = "object";  // "object" | "waypoint" | "road" | "bridge"
            std::string templateName;  // the object a right click places ("" none chosen yet)
            std::string roadType;  // road tools: a Roads.ini Road (or Bridge) name
            std::string team;  // owner of placed objects
            double angle = 0;  // degrees, of placed objects
            bool picker = false;  // the client shows the template picker
            std::optional<Point> pending;  // a right click waiting for a template
            std::optional<Point> roadStart;  // road tools: the first point
            // "" | "click" (pressed, not moved yet: press_pan, press_move, press_band, press_orbit)
            // | "pan" | "orbit" | "move" | "band"
            std::string gesture;
            Point drag;  // "move": how far the selection is dragged (world units)
            std::string note;
            std::string noteKind;  // "info" | "error"
            std::int64_t noteSerial = 0;  // grows with every note
            std::vector<std::int64_t> selection;
            std::int64_t selectionRevision = 0;
            std::size_t clipboardCount = 0;
            bool operator==(const State&) const = default;
        };

        enum class RoadKind { unknown, road, bridge };
        struct Game {
            std::function<RoadKind(const std::string&)> roadKind;
            std::function<std::string(bool bridge)> firstRoad;  // the first road (bridge) type, "" none
        };

        explicit MapEditor(Game game) : game(std::move(game)) {}

        const State& state() const {
            return this->current;
        }

        // A map opened (or closed: nullptr): the selection, gesture and points waiting are dropped,
        // the owner becomes one of its teams (`defaultTeam` unless the current one is among `teams`).
        void open(MapDocument* document, std::vector<std::string> teams, const std::string& defaultTeam, Point center);

        // Runs one edit (checked by EditorCore::send). Returns false when it did nothing (the note
        // says why).
        bool run(const commands::Edit& edit);

        void setNote(const std::string& text, const std::string& kind = "info");

      private:
        using Ids = commands::Ids;
        // One edit each; true when it did something.
        bool apply(const commands::PlaceObject& edit);
        bool apply(const commands::MoveObject& edit);
        bool apply(const commands::RotateObject& edit);
        bool apply(const commands::DeleteObject& edit);
        bool apply(const commands::Undo& edit);
        bool apply(const commands::Redo& edit);
        bool apply(const commands::Press& edit);
        bool apply(const commands::Drag& edit);
        bool apply(const commands::Release& edit);
        bool apply(const commands::RightClick& edit);
        bool apply(const commands::DeleteSelection& edit);
        bool apply(const commands::RotateSelection& edit);
        bool apply(const commands::Copy& edit);
        bool apply(const commands::Paste& edit);
        bool apply(const commands::Duplicate& edit);
        bool apply(const commands::Cancel& edit);
        bool apply(const commands::SelectAll& edit);
        bool apply(const commands::UseObjectTool& edit);
        bool apply(const commands::ChooseTemplate& edit);
        bool apply(const commands::UseWaypointTool& edit);
        bool apply(const commands::UseRoadTool& edit);
        bool apply(const commands::SetPlacementOwner& edit);
        bool apply(const commands::SetPlacementAngle& edit);
        bool apply(const commands::ShowPicker& edit);
        bool apply(const commands::CancelPlacement& edit);
        bool apply(const commands::ClearNote& edit);
        bool apply(const commands::SetSelection& edit);
        bool apply(const commands::SetOwner& edit);
        bool apply(const commands::SetHeight& edit);
        bool apply(const commands::SetAngle& edit);
        bool apply(const commands::SetProperty& edit);
        bool apply(const commands::UnsetProperty& edit);
        bool apply(const commands::LinkWaypoints& edit);
        bool undoOrRedo(bool undo);
        void select(Ids ids);
        bool selected(std::int64_t id) const;
        void prune();
        bool placeAt(Point at);
        std::vector<Clip> copy(const Ids& ids, std::size_t& skipped) const;
        bool paste(const std::vector<Clip>& clips, Point at, const char* what);
        bool forSelection(const char* what, const std::function<bool(std::int64_t)>& edit);
        Point middle(const Ids& ids) const;

        Game game;
        MapDocument* document = nullptr;
        std::vector<std::string> teams;
        Point center;
        State current;
        std::vector<Clip> clipboard;
        // The press of a gesture: ground point and objects under it.
        std::optional<Point> pressAt;
        Ids pressHits;
        bool noteSet = false;
    };
}
