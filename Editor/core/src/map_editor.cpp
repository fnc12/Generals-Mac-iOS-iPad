#include "map_editor.h"

#include <algorithm>
#include <cmath>

namespace end_editor {

    namespace {
        constexpr double pi = 3.14159265358979323846;
        // How far "Duplicate" puts the copy from the original (world units, two cells).
        constexpr float duplicateOffset = 20;

        std::string count(std::size_t n, const char* what) {
            return std::to_string(n) + " " + what + (n == 1 ? "" : "s");
        }
    }

    void MapEditor::setNote(const std::string& text, const std::string& kind) {
        this->current.note = text;
        this->current.noteKind = text.empty() ? "" : kind;
        ++this->current.noteSerial;
        this->noteSet = true;
    }

    void MapEditor::open(MapDocument* opened,
                         std::vector<std::string> mapTeams,
                         const std::string& defaultTeam,
                         Point mapCenter) {
        this->document = opened;
        this->teams = std::move(mapTeams);
        this->center = mapCenter;
        auto& state = this->current;
        if (!state.selection.empty()) {
            state.selection.clear();
            ++state.selectionRevision;
        }
        state.pending.reset();
        state.roadStart.reset();
        state.picker = false;
        state.gesture.clear();
        state.drag = {};
        this->pressHits.clear();
        if (std::find(this->teams.begin(), this->teams.end(), state.team) == this->teams.end()) {
            state.team = defaultTeam;
        }
    }

    void MapEditor::select(Ids chosen) {
        Ids shown;
        for (const auto id: chosen) {
            if (this->document && this->document->find(id) &&
                std::find(shown.begin(), shown.end(), id) == shown.end()) {
                shown.push_back(id);
            }
        }
        if (shown != this->current.selection) {
            this->current.selection = std::move(shown);
            ++this->current.selectionRevision;
        }
    }

    bool MapEditor::selected(std::int64_t id) const {
        const auto& selection = this->current.selection;
        return std::find(selection.begin(), selection.end(), id) != selection.end();
    }

    void MapEditor::prune() {
        this->select(this->current.selection);
    }

    MapEditor::Point MapEditor::middle(const Ids& chosen) const {
        Point sum;
        int n = 0;
        for (const auto id: chosen) {
            if (const auto* entry = this->document->find(id)) {
                sum.x += entry->object.x;
                sum.y += entry->object.y;
                ++n;
            }
        }
        return n > 0 ? Point{sum.x / float(n), sum.y / float(n)} : this->center;
    }

    bool MapEditor::placeAt(Point at) {
        const auto& state = this->current;
        const auto id =
            this->document->place(state.templateName, at.x, at.y, float(state.angle * pi / 180), state.team);
        this->select({id});
        return true;
    }

    bool MapEditor::apply(const commands::RightClick& edit) {
        const auto& at = edit.at;
        const auto& hits = edit.hits;
        auto& state = this->current;
        if (!at) {
            this->setNote("Right-click on the map's ground to place", "error");
            return false;
        }
        if (state.tool == "object") {
            if (state.templateName.empty()) {
                // Placed as soon as an object is chosen.
                state.pending = at;
                state.picker = true;
                this->setNote("Choose the object to place there");
                return true;
            }
            return this->placeAt(*at);
        }
        if (state.tool == "waypoint") {
            std::int64_t hit = 0;
            for (const auto id: hits) {
                if (this->document->waypointId(id) != 0) {
                    hit = id;
                    break;
                }
            }
            const std::int64_t from = state.selection.size() == 1 && this->document->waypointId(state.selection[0]) != 0
                                          ? state.selection[0]
                                          : 0;
            if (hit != 0 && from != 0 && hit != from) {
                const MapDocument::Link link{this->document->waypointId(from), this->document->waypointId(hit)};
                const auto& links = this->document->links();
                const bool linked = std::find(links.begin(), links.end(), link) != links.end();
                this->document->toggleLink(from, hit);
                this->select({hit});
                this->setNote(linked ? "Link removed" : "Linked");
                return true;
            }
            if (hit != 0) {
                this->select({hit});
                this->setNote("Right-click the ground for the next waypoint of the path, or a waypoint to link it");
                return true;
            }
            this->document->begin();
            const auto id = this->document->placeWaypoint(at->x, at->y);
            if (from != 0) {
                this->document->toggleLink(from, id);
            }
            this->document->commit();
            this->select({id});
            return true;
        }
        // Roads and bridges: two right clicks, the start and the end.
        const bool bridge = state.tool == "bridge";
        if (state.roadType.empty()) {
            this->setNote(bridge ? "Choose a bridge type" : "Choose a road type", "error");
            return false;
        }
        if (!state.roadStart) {
            state.roadStart = at;
            this->setNote(bridge ? "Right-click where the bridge ends" : "Right-click where the road ends");
            return true;
        }
        const auto start = *state.roadStart;
        if (std::hypot(at->x - start.x, at->y - start.y) < 1) {
            this->setNote("The end is where the start is: right-click somewhere else", "error");
            return false;
        }
        state.roadStart.reset();
        const auto first = this->document->placeRoad(state.roadType, bridge, start.x, start.y, at->x, at->y);
        this->select({first, first + 1});
        return true;
    }

    bool MapEditor::apply(const commands::Press& edit) {
        const auto& keys = edit.modifiers;
        this->pressAt = edit.at;
        this->pressHits.clear();
        for (const auto id: edit.hits) {
            if (this->document->find(id)) {
                this->pressHits.push_back(id);
            }
        }
        auto& state = this->current;
        state.drag = {};
        if ((keys.alt || keys.cmd) && !keys.shift) {
            state.gesture = "press_orbit";  // turns the camera
        } else if (keys.shift) {
            state.gesture = "press_band";  // a click adds or removes, a drag selects a rectangle
        } else if (std::any_of(this->pressHits.begin(), this->pressHits.end(), [&](auto id) {
                       return this->selected(id);
                   })) {
            state.gesture = "press_move";  // on the selection (also behind another object): a drag moves it
        } else {
            state.gesture = "press_pan";  // a click selects, a drag pans
        }
        return true;
    }

    bool MapEditor::apply(const commands::Drag& edit) {
        const auto& at = edit.at;
        auto& state = this->current;
        if (state.gesture == "press_orbit") {
            state.gesture = "orbit";
        } else if (state.gesture == "press_band") {
            state.gesture = "band";
        } else if (state.gesture == "press_pan") {
            state.gesture = "pan";
        } else if (state.gesture == "press_move") {
            state.gesture = "move";
        }
        if (state.gesture == "move" && at && this->pressAt) {
            state.drag = {at->x - this->pressAt->x, at->y - this->pressAt->y};
        }
        return true;
    }

    bool MapEditor::apply(const commands::Release& edit) {
        const auto& band = edit.band;
        auto& state = this->current;
        const auto gesture = state.gesture;
        const auto drag = state.drag;
        state.gesture.clear();
        state.drag = {};
        const auto nearest = this->pressHits.empty() ? 0 : this->pressHits.front();
        if (gesture == "press_pan" || gesture == "press_move") {
            // A click: the object under the cursor (the nearest), nothing when there is none.
            this->select(nearest != 0 ? Ids{nearest} : Ids{});
            return true;
        }
        if (gesture == "press_band") {
            auto selection = state.selection;
            if (nearest != 0) {
                if (this->selected(nearest)) {
                    std::erase(selection, nearest);
                } else {
                    selection.push_back(nearest);
                }
            }
            this->select(selection);
            return true;
        }
        if (gesture == "band") {
            auto selection = state.selection;
            selection.insert(selection.end(), band.begin(), band.end());
            this->select(selection);
            return true;
        }
        if (gesture == "move" && (drag.x != 0 || drag.y != 0) && !state.selection.empty()) {
            this->document->begin();
            for (const auto id: state.selection) {
                const auto* entry = this->document->find(id);
                this->document->move(id, entry->object.x + drag.x, entry->object.y + drag.y);
            }
            this->document->commit();
            return true;
        }
        return false;
    }

    bool MapEditor::apply(const commands::DeleteSelection&) {
        auto& state = this->current;
        if (state.roadStart) {
            state.roadStart.reset();
            this->setNote(state.tool == "bridge" ? "Bridge cancelled" : "Road cancelled");
            return true;
        }
        if (state.selection.empty()) {
            this->setNote("Nothing selected to delete");
            return false;
        }
        this->document->begin();
        for (const auto id: state.selection) {
            this->document->remove(id);  // a road point may have gone with its partner already
        }
        this->document->commit();
        return true;
    }

    bool MapEditor::apply(const commands::RotateSelection& edit) {
        const auto& selection = this->current.selection;
        if (selection.empty()) {
            this->setNote("Nothing selected to turn");
            return false;
        }
        // Several objects turn together about their middle, each by the same angle.
        const auto turn = float(edit.degrees * pi / 180);
        const auto c = std::cos(turn), s = std::sin(turn);
        const auto pivot = this->middle(selection);
        this->document->begin();
        for (const auto id: selection) {
            const auto object = this->document->find(id)->object;
            if (selection.size() > 1) {
                const float dx = object.x - pivot.x, dy = object.y - pivot.y;
                this->document->move(id, pivot.x + c * dx - s * dy, pivot.y + s * dx + c * dy);
            }
            this->document->rotate(id, object.angle + turn);
        }
        this->document->commit();
        return true;
    }

    std::vector<MapEditor::Clip> MapEditor::copy(const Ids& chosen, std::size_t& skipped) const {
        // Like WorldBuilder: both ends of a road segment when one is chosen, no waypoints (their
        // ids and links are the map's).
        Ids taken;
        skipped = 0;
        for (const auto& entry: this->document->objects()) {
            if (entry.id == 0 || entry.removed) {
                continue;
            }
            const auto partner = this->document->roadPartner(entry.id);
            const bool chosenHere = std::find(chosen.begin(), chosen.end(), entry.id) != chosen.end();
            const bool chosenPartner = partner != 0 && std::find(chosen.begin(), chosen.end(), partner) != chosen.end();
            if (!chosenHere && !chosenPartner) {
                continue;
            }
            if (this->document->waypointId(entry.id) != 0) {
                ++skipped;
                continue;
            }
            taken.push_back(entry.id);
        }
        const auto pivot = this->middle(taken);
        std::vector<Clip> clips;
        for (const auto id: taken) {
            const auto& object = this->document->find(id)->object;
            clips.push_back(Clip{object, object.x - pivot.x, object.y - pivot.y});
        }
        return clips;
    }

    bool MapEditor::paste(const std::vector<Clip>& clips, Point at, const char* what) {
        if (clips.empty()) {
            this->setNote("Nothing to paste: copy objects first");
            return false;
        }
        Ids placed;
        this->document->begin();
        for (const auto& clip: clips) {
            const auto& object = clip.object;
            placed.push_back(this->document->place(object.templateName,
                                                   at.x + clip.dx,
                                                   at.y + clip.dy,
                                                   object.angle,
                                                   "",
                                                   object.flags,
                                                   object.z,
                                                   &object.properties));
        }
        this->document->commit();
        this->select(placed);
        this->setNote(std::string(what) + " " + count(placed.size(), "object"));
        return true;
    }

    bool MapEditor::forSelection(const char* what, const std::function<bool(std::int64_t)>& edit) {
        if (this->current.selection.empty()) {
            this->setNote(std::string("Nothing selected to ") + what, "error");
            return false;
        }
        bool any = false;
        this->document->begin();
        for (const auto id: this->current.selection) {
            any = edit(id) || any;
        }
        this->document->commit();
        return any;
    }

    bool MapEditor::apply(const commands::PlaceObject& edit) {
        // What scripts and checks use: the angle in radians, any team.
        this->select({this->document->place(edit.templateName, edit.x, edit.y, edit.angle, edit.team)});
        return true;
    }

    bool MapEditor::apply(const commands::MoveObject& edit) {
        if (!this->document->move(edit.id, edit.x, edit.y)) {
            this->setNote("No such object", "error");
            return false;
        }
        return true;
    }

    bool MapEditor::apply(const commands::RotateObject& edit) {
        if (!this->document->rotate(edit.id, edit.angle)) {
            this->setNote("No such object", "error");
            return false;
        }
        return true;
    }

    bool MapEditor::apply(const commands::DeleteObject& edit) {
        if (!this->document->remove(edit.id)) {
            this->setNote("No such object", "error");
            return false;
        }
        return true;
    }

    bool MapEditor::undoOrRedo(bool undo) {
        if (!(undo ? this->document->undo() : this->document->redo())) {
            this->setNote(undo ? "Nothing to undo" : "Nothing to redo");
            return false;
        }
        // What the step changed becomes the selection (nothing after undoing a placement).
        Ids changed;
        for (const auto id: this->document->lastChanged()) {
            if (this->document->find(id)) {
                changed.push_back(id);
            }
        }
        this->select(changed.empty() ? this->current.selection : changed);
        return true;
    }

    bool MapEditor::apply(const commands::Undo&) {
        return this->undoOrRedo(true);
    }

    bool MapEditor::apply(const commands::Redo&) {
        return this->undoOrRedo(false);
    }

    bool MapEditor::apply(const commands::Copy&) {
        std::size_t skipped = 0;
        auto clips = this->copy(this->current.selection, skipped);
        if (clips.empty()) {
            this->setNote(skipped > 0 ? "Waypoints are not copied" : "Nothing selected to copy", "error");
            return false;
        }
        this->clipboard = std::move(clips);
        this->setNote("Copied " + count(this->clipboard.size(), "object") +
                      (skipped > 0 ? " (waypoints are not copied)" : ""));
        return true;
    }

    bool MapEditor::apply(const commands::Paste& edit) {
        return this->paste(this->clipboard, edit.at.value_or(this->center), "Pasted");
    }

    bool MapEditor::apply(const commands::Duplicate&) {
        std::size_t skipped = 0;
        const auto clips = this->copy(this->current.selection, skipped);
        if (clips.empty()) {
            this->setNote(skipped > 0 ? "Waypoints are not duplicated" : "Nothing selected to duplicate", "error");
            return false;
        }
        const auto at = this->middle(this->current.selection);
        return this->paste(clips, Point{at.x + duplicateOffset, at.y - duplicateOffset}, "Duplicated");
    }

    bool MapEditor::apply(const commands::Cancel&) {
        auto& state = this->current;
        if (state.gesture == "move") {
            state.gesture.clear();
            state.drag = {};
        }
        state.picker = false;
        state.pending.reset();
        state.roadStart.reset();
        return true;
    }

    bool MapEditor::apply(const commands::SelectAll&) {
        Ids all;
        for (const auto& entry: this->document->objects()) {
            if (entry.id != 0 && !entry.removed) {
                all.push_back(entry.id);
            }
        }
        this->select(std::move(all));
        return true;
    }

    bool MapEditor::apply(const commands::UseObjectTool&) {
        auto& state = this->current;
        state.tool = "object";
        state.picker = false;
        state.pending.reset();
        state.roadStart.reset();
        return true;
    }

    bool MapEditor::apply(const commands::ChooseTemplate& edit) {
        auto& state = this->current;
        state.tool = "object";
        state.templateName = edit.templateName;
        state.picker = false;
        state.roadStart.reset();
        if (state.pending && this->document) {
            const auto at = *state.pending;
            state.pending.reset();
            this->placeAt(at);
        }
        return true;
    }

    bool MapEditor::apply(const commands::UseWaypointTool&) {
        auto& state = this->current;
        state.tool = "waypoint";
        state.picker = false;
        state.pending.reset();
        state.roadStart.reset();
        return true;
    }

    bool MapEditor::apply(const commands::UseRoadTool& edit) {
        auto& state = this->current;
        state.tool = edit.bridge ? "bridge" : "road";
        // Without a type: the one chosen before if it fits, else the first of Roads.ini.
        if (!edit.type.empty()) {
            state.roadType = edit.type;
        } else if (this->game.roadKind(state.roadType) != (edit.bridge ? RoadKind::bridge : RoadKind::road)) {
            state.roadType = this->game.firstRoad(edit.bridge);
        }
        state.picker = false;
        state.pending.reset();
        state.roadStart.reset();
        return true;
    }

    bool MapEditor::apply(const commands::SetPlacementOwner& edit) {
        if (this->document && std::find(this->teams.begin(), this->teams.end(), edit.team) == this->teams.end()) {
            this->setNote("This map has no team " + edit.team, "error");
            return false;
        }
        this->current.team = edit.team;
        return true;
    }

    bool MapEditor::apply(const commands::SetPlacementAngle& edit) {
        this->current.angle = edit.degrees;
        return true;
    }

    bool MapEditor::apply(const commands::ShowPicker&) {
        this->current.picker = true;
        return true;
    }

    bool MapEditor::apply(const commands::CancelPlacement&) {
        auto& state = this->current;
        state.picker = false;
        state.pending.reset();
        state.roadStart.reset();
        return true;
    }

    bool MapEditor::apply(const commands::ClearNote&) {
        this->setNote("");
        return true;
    }

    bool MapEditor::apply(const commands::SetSelection& edit) {
        this->select(edit.ids);
        return true;
    }

    bool MapEditor::apply(const commands::SetOwner& edit) {
        if (std::find(this->teams.begin(), this->teams.end(), edit.team) == this->teams.end()) {
            this->setNote("This map has no team " + edit.team, "error");
            return false;
        }
        const DictValue owner{"originalOwner", AsciiText{edit.team}};
        return this->forSelection("give an owner", [&](std::int64_t id) {
            return this->document->setProperty(id, owner);
        });
    }

    bool MapEditor::apply(const commands::SetHeight& edit) {
        const bool done = this->forSelection("raise", [&](std::int64_t id) {
            return this->document->setHeight(id, edit.z);
        });
        if (!done && !this->current.selection.empty()) {
            this->setNote("The game drops objects that far from the ground", "error");
        }
        return done;
    }

    bool MapEditor::apply(const commands::SetAngle& edit) {
        const auto angle = float(edit.degrees * pi / 180);
        return this->forSelection("turn", [&](std::int64_t id) {
            return this->document->rotate(id, angle);
        });
    }

    bool MapEditor::apply(const commands::SetProperty& edit) {
        return this->forSelection("change", [&](std::int64_t id) {
            return this->document->setProperty(id, edit.value);
        });
    }

    bool MapEditor::apply(const commands::UnsetProperty& edit) {
        return this->forSelection("change", [&](std::int64_t id) {
            return this->document->removeProperty(id, edit.key);
        });
    }

    bool MapEditor::apply(const commands::LinkWaypoints&) {
        const auto& selection = this->current.selection;
        if (selection.size() != 2 || this->document->waypointId(selection[0]) == 0 ||
            this->document->waypointId(selection[1]) == 0) {
            this->setNote("Select two waypoints to link", "error");
            return false;
        }
        return this->document->toggleLink(selection[0], selection[1]);
    }

    bool MapEditor::run(const commands::Edit& edit) {
        auto& state = this->current;
        this->noteSet = false;
        const auto revision = this->document ? this->document->revision() : 0;
        bool done = false;
        if (!this->document && commands::needsMap(edit)) {
            this->setNote("No map is open", "error");
        } else {
            done = std::visit(
                [this](const auto& command) {
                    return this->apply(command);
                },
                edit);
        }
        if (this->document && this->document->revision() != revision) {
            this->prune();
            if (!this->noteSet && !state.note.empty()) {
                this->setNote("");  // a note lasts until the map changes
            }
        }
        state.clipboardCount = this->clipboard.size();
        return done;
    }
}
