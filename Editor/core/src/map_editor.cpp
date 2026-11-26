#include "map_editor.h"

#include <algorithm>
#include <cmath>
#include <locale>
#include <sstream>

namespace end_editor {

    namespace {
        constexpr double pi = 3.14159265358979323846;
        // How far "Duplicate" puts the copy from the original (world units, two cells).
        constexpr float duplicateOffset = 20;

        std::vector<std::string> split(const std::string &text, char separator) {
            std::vector<std::string> parts;
            std::size_t start = 0;
            while(start <= text.size()) {
                auto end = text.find(separator, start);
                if(end == std::string::npos) {
                    end = text.size();
                }
                if(end > start) {
                    parts.push_back(text.substr(start, end - start));
                }
                start = end + 1;
            }
            return parts;
        }

        std::string count(std::size_t n, const char *what) {
            return std::to_string(n) + " " + what + (n == 1 ? "" : "s");
        }
    }

    bool parseNumber(const std::string &text, double &value) {
        std::istringstream stream(text);
        stream.imbue(std::locale::classic());
        stream >> value;
        return !text.empty() && stream && stream.peek() == std::char_traits<char>::eof() && std::isfinite(value);
    }

    std::optional<MapEditor::Point> MapEditor::point(const std::vector<std::string> &command, std::size_t at) {
        double x = 0, y = 0;
        if(at + 1 < command.size() && parseNumber(command[at], x) && parseNumber(command[at + 1], y)) {
            return Point{float(x), float(y)};
        }
        return std::nullopt;
    }

    MapEditor::Ids MapEditor::ids(const std::string &text) {
        Ids result;
        for(const auto &part: split(text, ',')) {
            double value = 0;
            if(parseNumber(part, value) && value > 0) {
                result.push_back(std::int64_t(value));
            }
        }
        return result;
    }

    void MapEditor::setNote(const std::string &text, const std::string &kind) {
        this->current.note = text;
        this->current.noteKind = text.empty() ? "" : kind;
        ++this->current.noteSerial;
        this->noteSet = true;
    }

    void MapEditor::open(MapDocument *opened, std::vector<std::string> mapTeams, const std::string &defaultTeam,
                         Point mapCenter) {
        this->document = opened;
        this->teams = std::move(mapTeams);
        this->center = mapCenter;
        auto &state = this->current;
        if(!state.selection.empty()) {
            state.selection.clear();
            ++state.selectionRevision;
        }
        state.pending.reset();
        state.roadStart.reset();
        state.picker = false;
        state.gesture.clear();
        state.drag = {};
        this->pressHits.clear();
        if(std::find(this->teams.begin(), this->teams.end(), state.team) == this->teams.end()) {
            state.team = defaultTeam;
        }
    }

    void MapEditor::select(Ids chosen) {
        Ids shown;
        for(const auto id: chosen) {
            if(this->document && this->document->find(id) && std::find(shown.begin(), shown.end(), id) == shown.end()) {
                shown.push_back(id);
            }
        }
        if(shown != this->current.selection) {
            this->current.selection = std::move(shown);
            ++this->current.selectionRevision;
        }
    }

    bool MapEditor::selected(std::int64_t id) const {
        const auto &selection = this->current.selection;
        return std::find(selection.begin(), selection.end(), id) != selection.end();
    }

    void MapEditor::prune() {
        this->select(this->current.selection);
    }

    MapEditor::Point MapEditor::middle(const Ids &chosen) const {
        Point sum;
        int n = 0;
        for(const auto id: chosen) {
            if(const auto *entry = this->document->find(id)) {
                sum.x += entry->object.x;
                sum.y += entry->object.y;
                ++n;
            }
        }
        return n > 0 ? Point{sum.x / float(n), sum.y / float(n)} : this->center;
    }

    bool MapEditor::placeAt(Point at) {
        const auto &state = this->current;
        const auto id = this->document->place(state.templateName, at.x, at.y, float(state.angle * pi / 180), state.team);
        this->select({id});
        return true;
    }

    bool MapEditor::rightClick(std::optional<Point> at, const Ids &hits) {
        auto &state = this->current;
        if(!at) {
            this->setNote("Right-click on the map's ground to place", "error");
            return false;
        }
        if(state.tool == "object") {
            if(state.templateName.empty()) {
                // Placed as soon as an object is chosen.
                state.pending = at;
                state.picker = true;
                this->setNote("Choose the object to place there");
                return true;
            }
            return this->placeAt(*at);
        }
        if(state.tool == "waypoint") {
            std::int64_t hit = 0;
            for(const auto id: hits) {
                if(this->document->waypointId(id) != 0) {
                    hit = id;
                    break;
                }
            }
            const std::int64_t from = state.selection.size() == 1 && this->document->waypointId(state.selection[0]) != 0
                                          ? state.selection[0]
                                          : 0;
            if(hit != 0 && from != 0 && hit != from) {
                const MapDocument::Link link{this->document->waypointId(from), this->document->waypointId(hit)};
                const auto &links = this->document->links();
                const bool linked = std::find(links.begin(), links.end(), link) != links.end();
                this->document->toggleLink(from, hit);
                this->select({hit});
                this->setNote(linked ? "Link removed" : "Linked");
                return true;
            }
            if(hit != 0) {
                this->select({hit});
                this->setNote("Right-click the ground for the next waypoint of the path, or a waypoint to link it");
                return true;
            }
            this->document->begin();
            const auto id = this->document->placeWaypoint(at->x, at->y);
            if(from != 0) {
                this->document->toggleLink(from, id);
            }
            this->document->commit();
            this->select({id});
            return true;
        }
        // Roads and bridges: two right clicks, the start and the end.
        const bool bridge = state.tool == "bridge";
        if(state.roadType.empty()) {
            this->setNote(bridge ? "Choose a bridge type" : "Choose a road type", "error");
            return false;
        }
        if(!state.roadStart) {
            state.roadStart = at;
            this->setNote(bridge ? "Right-click where the bridge ends" : "Right-click where the road ends");
            return true;
        }
        const auto start = *state.roadStart;
        if(std::hypot(at->x - start.x, at->y - start.y) < 1) {
            this->setNote("The end is where the start is: right-click somewhere else", "error");
            return false;
        }
        state.roadStart.reset();
        const auto first = this->document->placeRoad(state.roadType, bridge, start.x, start.y, at->x, at->y);
        this->select({first, first + 1});
        return true;
    }

    void MapEditor::press(std::optional<Point> at, const std::string &modifiers, const Ids &hits) {
        const auto keys = split(modifiers, ',');
        const auto has = [&](const char *key) { return std::find(keys.begin(), keys.end(), key) != keys.end(); };
        this->pressAt = at;
        this->pressHits.clear();
        for(const auto id: hits) {
            if(this->document->find(id)) {
                this->pressHits.push_back(id);
            }
        }
        auto &state = this->current;
        state.drag = {};
        if((has("alt") || has("cmd")) && !has("shift")) {
            state.gesture = "press_orbit";  // turns the camera
        } else if(has("shift")) {
            state.gesture = "press_band";  // a click adds or removes, a drag selects a rectangle
        } else if(std::any_of(this->pressHits.begin(), this->pressHits.end(), [&](auto id) { return this->selected(id); })) {
            state.gesture = "press_move";  // on the selection (also behind another object): a drag moves it
        } else {
            state.gesture = "press_pan";  // a click selects, a drag pans
        }
    }

    void MapEditor::drag(std::optional<Point> at) {
        auto &state = this->current;
        if(state.gesture == "press_orbit") {
            state.gesture = "orbit";
        } else if(state.gesture == "press_band") {
            state.gesture = "band";
        } else if(state.gesture == "press_pan") {
            state.gesture = "pan";
        } else if(state.gesture == "press_move") {
            state.gesture = "move";
        }
        if(state.gesture == "move" && at && this->pressAt) {
            state.drag = {at->x - this->pressAt->x, at->y - this->pressAt->y};
        }
    }

    bool MapEditor::release(const Ids &band) {
        auto &state = this->current;
        const auto gesture = state.gesture;
        const auto drag = state.drag;
        state.gesture.clear();
        state.drag = {};
        const auto nearest = this->pressHits.empty() ? 0 : this->pressHits.front();
        if(gesture == "press_pan" || gesture == "press_move") {
            // A click: the object under the cursor (the nearest), nothing when there is none.
            this->select(nearest != 0 ? Ids{nearest} : Ids{});
            return true;
        }
        if(gesture == "press_band") {
            auto selection = state.selection;
            if(nearest != 0) {
                if(this->selected(nearest)) {
                    std::erase(selection, nearest);
                } else {
                    selection.push_back(nearest);
                }
            }
            this->select(selection);
            return true;
        }
        if(gesture == "band") {
            auto selection = state.selection;
            selection.insert(selection.end(), band.begin(), band.end());
            this->select(selection);
            return true;
        }
        if(gesture == "move" && (drag.x != 0 || drag.y != 0) && !state.selection.empty()) {
            this->document->begin();
            for(const auto id: state.selection) {
                const auto *entry = this->document->find(id);
                this->document->move(id, entry->object.x + drag.x, entry->object.y + drag.y);
            }
            this->document->commit();
            return true;
        }
        return false;
    }

    bool MapEditor::remove() {
        auto &state = this->current;
        if(state.roadStart) {
            state.roadStart.reset();
            this->setNote(state.tool == "bridge" ? "Bridge cancelled" : "Road cancelled");
            return true;
        }
        if(state.selection.empty()) {
            this->setNote("Nothing selected to delete");
            return false;
        }
        this->document->begin();
        for(const auto id: state.selection) {
            this->document->remove(id);  // a road point may have gone with its partner already
        }
        this->document->commit();
        return true;
    }

    bool MapEditor::rotate(double degrees) {
        const auto &selection = this->current.selection;
        if(selection.empty()) {
            this->setNote("Nothing selected to turn");
            return false;
        }
        // Several objects turn together about their middle, each by the same angle.
        const auto turn = float(degrees * pi / 180);
        const auto c = std::cos(turn), s = std::sin(turn);
        const auto pivot = this->middle(selection);
        this->document->begin();
        for(const auto id: selection) {
            const auto object = this->document->find(id)->object;
            if(selection.size() > 1) {
                const float dx = object.x - pivot.x, dy = object.y - pivot.y;
                this->document->move(id, pivot.x + c * dx - s * dy, pivot.y + s * dx + c * dy);
            }
            this->document->rotate(id, object.angle + turn);
        }
        this->document->commit();
        return true;
    }

    std::vector<MapEditor::Clip> MapEditor::copy(const Ids &chosen, std::size_t &skipped) const {
        // Like WorldBuilder: both ends of a road segment when one is chosen, no waypoints (their
        // ids and links are the map's).
        Ids taken;
        skipped = 0;
        for(const auto &entry: this->document->objects()) {
            if(entry.id == 0 || entry.removed) {
                continue;
            }
            const auto partner = this->document->roadPartner(entry.id);
            const bool chosenHere = std::find(chosen.begin(), chosen.end(), entry.id) != chosen.end();
            const bool chosenPartner = partner != 0 && std::find(chosen.begin(), chosen.end(), partner) != chosen.end();
            if(!chosenHere && !chosenPartner) {
                continue;
            }
            if(this->document->waypointId(entry.id) != 0) {
                ++skipped;
                continue;
            }
            taken.push_back(entry.id);
        }
        const auto pivot = this->middle(taken);
        std::vector<Clip> clips;
        for(const auto id: taken) {
            const auto &object = this->document->find(id)->object;
            clips.push_back(Clip{object, object.x - pivot.x, object.y - pivot.y});
        }
        return clips;
    }

    bool MapEditor::paste(const std::vector<Clip> &clips, Point at, const char *what) {
        if(clips.empty()) {
            this->setNote("Nothing to paste: copy objects first");
            return false;
        }
        Ids placed;
        this->document->begin();
        for(const auto &clip: clips) {
            const auto &object = clip.object;
            placed.push_back(this->document->place(object.templateName, at.x + clip.dx, at.y + clip.dy, object.angle, "",
                                                   object.flags, object.z, &object.properties));
        }
        this->document->commit();
        this->select(placed);
        this->setNote(std::string(what) + " " + count(placed.size(), "object"));
        return true;
    }

    bool MapEditor::forSelection(const char *what, const std::function<bool(std::int64_t)> &edit) {
        if(this->current.selection.empty()) {
            this->setNote(std::string("Nothing selected to ") + what, "error");
            return false;
        }
        bool any = false;
        this->document->begin();
        for(const auto id: this->current.selection) {
            any = edit(id) || any;
        }
        this->document->commit();
        return any;
    }

    bool MapEditor::run(const std::vector<std::string> &command) {
        if(command.empty()) {
            return false;
        }
        const auto &name = command.front();
        static const std::string none;
        const auto arg = [&](std::size_t i) -> const std::string & { return i < command.size() ? command[i] : none; };
        const auto number = [&](std::size_t i) {
            double value = 0;
            parseNumber(arg(i), value);
            return value;
        };
        auto &state = this->current;
        this->noteSet = false;
        const auto revision = this->document ? this->document->revision() : 0;
        bool done = true;

        if(name == "placement.template") {
            state.tool = "object";
            state.templateName = arg(1);
            state.picker = false;
            state.roadStart.reset();
            if(state.pending && this->document) {
                const auto at = *state.pending;
                state.pending.reset();
                this->placeAt(at);
            }
        } else if(name == "placement.object" || name == "placement.waypoint" || name == "placement.road" ||
                  name == "placement.bridge") {
            state.tool = name.substr(10);
            if(state.tool == "road" || state.tool == "bridge") {
                // Without a type: the one chosen before if it fits, else the first of Roads.ini.
                const bool bridge = state.tool == "bridge";
                if(!arg(1).empty()) {
                    state.roadType = arg(1);
                } else if(this->game.roadKind(state.roadType) != (bridge ? 2 : 1)) {
                    state.roadType = this->game.firstRoad(bridge);
                }
            }
            state.picker = false;
            state.pending.reset();
            state.roadStart.reset();
        } else if(name == "placement.owner") {
            if(this->document && std::find(this->teams.begin(), this->teams.end(), arg(1)) == this->teams.end()) {
                this->setNote("This map has no team " + arg(1), "error");
                done = false;
            } else {
                state.team = arg(1);
            }
        } else if(name == "placement.angle") {
            state.angle = number(1);
        } else if(name == "placement.pick") {
            state.picker = true;
        } else if(name == "placement.cancel") {
            state.picker = false;
            state.pending.reset();
            state.roadStart.reset();
        } else if(name == "note.clear") {
            this->setNote("");
        } else if(!this->document) {
            this->setNote("No map is open", "error");
            done = false;
        } else if(name == "map.click_right") {
            done = this->rightClick(point(command, 1), ids(arg(3)));
        } else if(name == "map.press") {
            this->press(point(command, 1), arg(3), ids(arg(4)));
        } else if(name == "map.drag") {
            this->drag(point(command, 1));
        } else if(name == "map.release") {
            done = this->release(ids(arg(1)));
        } else if(name == "edit.delete") {
            done = this->remove();
        } else if(name == "edit.rotate") {
            done = this->rotate(number(1));
        } else if(name == "edit.copy") {
            std::size_t skipped = 0;
            auto clips = this->copy(state.selection, skipped);
            if(clips.empty()) {
                this->setNote(skipped > 0 ? "Waypoints are not copied" : "Nothing selected to copy", "error");
                done = false;
            } else {
                this->clipboard = std::move(clips);
                this->setNote("Copied " + count(this->clipboard.size(), "object") +
                              (skipped > 0 ? " (waypoints are not copied)" : ""));
            }
        } else if(name == "edit.paste") {
            done = this->paste(this->clipboard, point(command, 1).value_or(this->center), "Pasted");
        } else if(name == "edit.duplicate") {
            std::size_t skipped = 0;
            const auto clips = this->copy(state.selection, skipped);
            if(clips.empty()) {
                this->setNote(skipped > 0 ? "Waypoints are not duplicated" : "Nothing selected to duplicate", "error");
                done = false;
            } else {
                const auto at = this->middle(state.selection);
                done = this->paste(clips, Point{at.x + duplicateOffset, at.y - duplicateOffset}, "Duplicated");
            }
        } else if(name == "edit.cancel") {
            if(state.gesture == "move") {
                state.gesture.clear();
                state.drag = {};
            }
            state.picker = false;
            state.pending.reset();
            state.roadStart.reset();
        } else if(name == "edit.select_all") {
            Ids all;
            for(const auto &entry: this->document->objects()) {
                if(entry.id != 0 && !entry.removed) {
                    all.push_back(entry.id);
                }
            }
            this->select(all);
        } else if(name == "selection.set") {
            this->select(ids(arg(1)));
        } else if(name == "selection.owner") {
            if(std::find(this->teams.begin(), this->teams.end(), arg(1)) == this->teams.end()) {
                this->setNote("This map has no team " + arg(1), "error");
                done = false;
            } else {
                DictValue owner;
                owner.type = DictValue::ascii;
                owner.key = "originalOwner";
                owner.text = arg(1);
                done = this->forSelection("give an owner",
                                          [&](std::int64_t id) { return this->document->setProperty(id, owner); });
            }
        } else if(name == "selection.height") {
            const auto z = float(number(1));
            done = this->forSelection("raise", [&](std::int64_t id) { return this->document->setHeight(id, z); });
            if(!done && !state.selection.empty()) {
                this->setNote("The game drops objects that far from the ground", "error");
            }
        } else if(name == "selection.angle") {
            const auto angle = float(number(1) * pi / 180);
            done = this->forSelection("turn", [&](std::int64_t id) { return this->document->rotate(id, angle); });
        } else if(name == "selection.property") {
            DictValue value;
            value.key = arg(1);
            const auto &type = arg(2), &text = arg(3);
            double parsed = 0;
            bool valid = true;
            if(type == "bool") {
                value.type = DictValue::boolean;
                valid = text == "true" || text == "false" || text == "1" || text == "0";
                value.intValue = text == "true" || text == "1" ? 1 : 0;
            } else if(type == "int") {
                value.type = DictValue::integer;
                valid = parseNumber(text, parsed) && parsed == std::floor(parsed) && std::abs(parsed) < 2147483648.0;
                value.intValue = valid ? std::int32_t(parsed) : 0;
            } else if(type == "real") {
                value.type = DictValue::real;
                valid = parseNumber(text, parsed);
                value.realValue = float(parsed);
            } else {
                value.type = type == "unicode" ? DictValue::unicode : DictValue::ascii;
                value.text = text;
            }
            if(!valid) {
                this->setNote(value.key + ": \"" + text + "\" is not a " + (type == "bool" ? "yes/no value" : "number"),
                              "error");
                done = false;
            } else {
                done = this->forSelection("change",
                                          [&](std::int64_t id) { return this->document->setProperty(id, value); });
            }
        } else if(name == "selection.unset") {
            done = this->forSelection("change",
                                      [&](std::int64_t id) { return this->document->removeProperty(id, arg(1)); });
        } else if(name == "waypoint.link") {
            const auto &selection = state.selection;
            if(selection.size() != 2 || this->document->waypointId(selection[0]) == 0 ||
               this->document->waypointId(selection[1]) == 0) {
                this->setNote("Select two waypoints to link", "error");
                done = false;
            } else {
                done = this->document->toggleLink(selection[0], selection[1]);
            }
        } else if(name == "map.place") {
            // place <template> <x> <y> <angle radians> <team>: what scripts and checks use.
            const auto id = this->document->place(arg(1), float(number(2)), float(number(3)), float(number(4)), arg(5));
            this->select({id});
        } else if(name == "map.move" || name == "map.rotate" || name == "map.delete") {
            const auto id = std::int64_t(number(1));
            done = name == "map.move"     ? this->document->move(id, float(number(2)), float(number(3)))
                   : name == "map.rotate" ? this->document->rotate(id, float(number(2)))
                                          : this->document->remove(id);
            if(!done) {
                this->setNote("No such object", "error");
            }
        } else if(name == "map.undo" || name == "map.redo") {
            done = name == "map.undo" ? this->document->undo() : this->document->redo();
            if(!done) {
                this->setNote(name == "map.undo" ? "Nothing to undo" : "Nothing to redo");
            } else {
                // What the step changed becomes the selection (nothing after undoing a placement).
                Ids changed;
                for(const auto id: this->document->lastChanged()) {
                    if(this->document->find(id)) {
                        changed.push_back(id);
                    }
                }
                this->select(changed.empty() ? state.selection : changed);
            }
        } else {
            done = false;
        }

        if(this->document && this->document->revision() != revision) {
            this->prune();
            if(!this->noteSet && !state.note.empty()) {
                this->setNote("");  // a note lasts until the map changes
            }
        }
        state.clipboardCount = this->clipboard.size();
        return done;
    }
}
