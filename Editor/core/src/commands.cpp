#include "commands.h"

#include "overloaded.h"

#include <array>
#include <charconv>
#include <cmath>

namespace end_editor::commands {

    namespace {
        // The tab separated arguments after a command's name. None when the text has no tab at
        // all; "name\t" has one empty argument. Arguments past the last one a command reads are
        // not looked at.
        class Arguments {
          public:
            explicit Arguments(std::string_view text) {
                std::size_t start = 0;
                while (this->count < this->parts.size()) {
                    const auto tab = text.find('\t', start);
                    this->parts[this->count++] = text.substr(start, tab == std::string_view::npos ? tab : tab - start);
                    if (tab == std::string_view::npos) {
                        break;
                    }
                    start = tab + 1;
                }
            }
            Arguments() = default;

            std::size_t size() const {
                return this->count;
            }
            // "" past the end.
            std::string_view operator[](std::size_t index) const {
                return index < this->count ? this->parts[index] : std::string_view();
            }

          private:
            std::array<std::string_view, 6> parts;  // no command has more
            std::size_t count = 0;
        };

        using Parsed = std::variant<Command, Error>;

        Parsed edit(Edit value) {
            return Command{std::move(value)};
        }
        Parsed play(PlaySetting value) {
            return Command{std::move(value)};
        }
        Parsed fail(const char* code) {
            return Error{code, {}};
        }
        Parsed fail(const char* code, std::string note) {
            return Error{code, std::move(note)};
        }
        // The map editor's errors: the code and a note naming the command.
        Parsed missing(std::string_view name) {
            return fail("error:missing_argument", std::string(name) + ": missing argument");
        }
        Parsed notANumber(std::string_view name) {
            return fail("error:bad_argument", std::string(name) + ": not a number");
        }

        // A leading '+' is read like the C locale's stream does; std::from_chars takes only '-'.
        std::string_view withoutPlus(std::string_view text) {
            if (text.size() > 1 && text.front() == '+' && text[1] != '-' && text[1] != '+') {
                text.remove_prefix(1);
            }
            return text;
        }

        template<class Integer>
        std::optional<Integer> parseInteger(std::string_view text) {
            text = withoutPlus(text);
            Integer value = 0;
            const auto end = text.data() + text.size();
            const auto [last, error] = std::from_chars(text.data(), end, value);
            if (text.empty() || error != std::errc() || last != end) {
                return std::nullopt;
            }
            return value;
        }

        // Both empty (the cursor is off the map: nullopt) or two numbers. False when malformed.
        bool parsePoint(std::string_view x, std::string_view y, std::optional<Point>& point) {
            if (x.empty() && y.empty()) {
                point.reset();
                return true;
            }
            const auto px = parseNumber(x), py = parseNumber(y);
            if (!px || !py) {
                return false;
            }
            point = Point{float(*px), float(*py)};
            return true;
        }

        // "1,5,3": object ids (positive); empty parts are skipped, "" is none. False when malformed.
        bool parseIds(std::string_view text, Ids& ids) {
            std::size_t start = 0;
            while (start <= text.size()) {
                auto comma = text.find(',', start);
                if (comma == std::string_view::npos) {
                    comma = text.size();
                }
                if (comma > start) {
                    const auto id = parseInteger<std::int64_t>(text.substr(start, comma - start));
                    if (!id || *id <= 0) {
                        return false;
                    }
                    ids.push_back(*id);
                }
                start = comma + 1;
            }
            return true;
        }

        bool parseModifiers(std::string_view text, Modifiers& modifiers) {
            std::size_t start = 0;
            while (start <= text.size()) {
                auto comma = text.find(',', start);
                if (comma == std::string_view::npos) {
                    comma = text.size();
                }
                const auto key = text.substr(start, comma - start);
                if (key == "shift") {
                    modifiers.shift = true;
                } else if (key == "alt") {
                    modifiers.alt = true;
                } else if (key == "cmd") {
                    modifiers.cmd = true;
                } else if (!key.empty()) {
                    return false;
                }
                start = comma + 1;
            }
            return true;
        }

        Parsed badIds(std::string_view name) {
            return fail("error:bad_argument", std::string(name) + ": not a list of object ids");
        }

        Parsed parseProperty(std::string_view name, const Arguments& arguments) {
            if (arguments.size() < 3 || arguments[0].empty()) {
                return missing(name);
            }
            const auto type = arguments[1], text = arguments[2];
            DictValue value{std::string(arguments[0]), {}};
            bool valid = true;
            if (type == "bool") {
                valid = text == "true" || text == "false" || text == "1" || text == "0";
                value.data = text == "true" || text == "1";
            } else if (type == "int") {
                const auto number = parseInteger<std::int32_t>(text);
                valid = number.has_value();
                value.data = number.value_or(0);
            } else if (type == "real") {
                const auto number = parseNumber(text);
                valid = number.has_value();
                value.data = float(number.value_or(0));
            } else if (type == "string") {
                value.data = AsciiText{std::string(text)};
            } else if (type == "unicode") {
                value.data = UnicodeText{std::string(text)};
            } else {
                return fail("error:bad_argument", std::string(name) + ": unknown type " + std::string(type));
            }
            if (!valid) {
                return fail("error:bad_argument",
                            value.key + ": \"" + std::string(text) + "\" is not a " +
                                (type == "bool" ? "yes/no value" : "number"));
            }
            return edit(SetProperty{std::move(value)});
        }

        Parsed parsePlaySetting(std::string_view name, const Arguments& arguments) {
            const auto argument = arguments[0];
            if (name == "play.app") {
                return play(SetPlayApp{std::string(argument)});
            }
            if (name == "play.faction") {
                if (argument.empty()) {
                    return fail("error:missing_argument", "play.faction: missing argument");
                }
                return play(SetPlayFaction{std::string(argument)});
            }
            if (name == "play.color") {
                if (argument.empty()) {
                    return fail("error:missing_argument", "play.color: missing argument");
                }
                return play(SetPlayColor{std::string(argument)});
            }
            if (name == "play.ai" || name == "play.start") {
                const auto number = parseInteger<int>(argument);
                if (!number || *number < 0 || *number > 7) {
                    return fail("error:bad_argument", std::string(name) + ": a whole number from 0 to 7");
                }
                return name == "play.ai" ? play(SetPlayAiCount{*number}) : play(SetPlayStart{*number});
            }
            if (name == "play.difficulty") {
                if (argument != "easy" && argument != "medium" && argument != "hard") {
                    return fail("error:bad_argument", "play.difficulty: easy, medium or hard");
                }
                return play(SetPlayDifficulty{std::string(argument)});
            }
            if (name == "play.stop") {
                return play(StopPlay{});
            }
            return fail("error:unknown_command");
        }

        Parsed parseSaveAs(const Arguments& arguments) {
            const auto name = arguments[0];
            if (name.find_first_not_of(" .") == std::string_view::npos) {
                return fail("error:missing_argument", "Type a name for the map");
            }
            if (name.find_first_of("/\\:") != std::string_view::npos) {
                return fail("error:bad_argument", "A map name cannot have / \\ or : in it");
            }
            if (!arguments[1].empty() && arguments[1] != "replace") {
                return fail("error:bad_argument", "map.save_as: the second argument is \"replace\" or nothing");
            }
            return Command{SaveMapAs{std::string(name), arguments[1] == "replace"}};
        }

        // The editor's commands: map.*, edit.*, placement.*, selection.*, waypoint.link, note.clear.
        Parsed parseEdit(std::string_view name, const Arguments& arguments) {
            std::optional<Point> at;
            if (name == "map.place") {
                if (arguments.size() < 5 || arguments[0].empty()) {
                    return missing(name);
                }
                const auto x = parseNumber(arguments[1]), y = parseNumber(arguments[2]),
                           angle = parseNumber(arguments[3]);
                if (!x || !y || !angle) {
                    return notANumber(name);
                }
                return edit(PlaceObject{std::string(arguments[0]),
                                        float(*x),
                                        float(*y),
                                        float(*angle),
                                        std::string(arguments[4])});
            }
            if (name == "map.move" || name == "map.rotate" || name == "map.delete") {
                // The id, then as many numbers as the edit takes.
                const std::size_t numbers = name == "map.move" ? 2 : name == "map.rotate" ? 1 : 0;
                if (arguments.size() < numbers + 1 || arguments[0].empty()) {
                    return missing(name);
                }
                const auto id = parseInteger<std::int64_t>(arguments[0]);
                const auto first = parseNumber(arguments[1]), second = parseNumber(arguments[2]);
                if (!id || (numbers > 0 && !first) || (numbers > 1 && !second)) {
                    return notANumber(name);
                }
                if (name == "map.move") {
                    return edit(MoveObject{*id, float(*first), float(*second)});
                }
                if (name == "map.rotate") {
                    return edit(RotateObject{*id, float(*first)});
                }
                return edit(DeleteObject{*id});
            }
            if (name == "map.undo") {
                return edit(Undo{});
            }
            if (name == "map.redo") {
                return edit(Redo{});
            }
            if (name == "map.press") {
                Press press;
                if (!parsePoint(arguments[0], arguments[1], press.at)) {
                    return notANumber(name);
                }
                if (!parseModifiers(arguments[2], press.modifiers)) {
                    return fail("error:bad_argument",
                                std::string(name) + ": unknown modifier in \"" + std::string(arguments[2]) + "\"");
                }
                if (!parseIds(arguments[3], press.hits)) {
                    return badIds(name);
                }
                return edit(std::move(press));
            }
            if (name == "map.drag") {
                if (!parsePoint(arguments[0], arguments[1], at)) {
                    return notANumber(name);
                }
                return edit(Drag{at});
            }
            if (name == "map.release") {
                Release release;
                if (!parseIds(arguments[0], release.band)) {
                    return badIds(name);
                }
                return edit(std::move(release));
            }
            if (name == "map.click_right") {
                RightClick click;
                if (!parsePoint(arguments[0], arguments[1], click.at)) {
                    return notANumber(name);
                }
                if (!parseIds(arguments[2], click.hits)) {
                    return badIds(name);
                }
                return edit(std::move(click));
            }
            if (name == "edit.delete") {
                return edit(DeleteSelection{});
            }
            if (name == "edit.copy") {
                return edit(Copy{});
            }
            if (name == "edit.paste") {
                if (!parsePoint(arguments[0], arguments[1], at)) {
                    return notANumber(name);
                }
                return edit(Paste{at});
            }
            if (name == "edit.duplicate") {
                return edit(Duplicate{});
            }
            if (name == "edit.cancel") {
                return edit(Cancel{});
            }
            if (name == "edit.select_all") {
                return edit(SelectAll{});
            }
            if (name == "placement.object") {
                return edit(UseObjectTool{});
            }
            if (name == "placement.waypoint") {
                return edit(UseWaypointTool{});
            }
            if (name == "placement.road" || name == "placement.bridge") {
                return edit(UseRoadTool{name == "placement.bridge", std::string(arguments[0])});
            }
            if (name == "placement.pick") {
                return edit(ShowPicker{});
            }
            if (name == "placement.cancel") {
                return edit(CancelPlacement{});
            }
            if (name == "note.clear") {
                return edit(ClearNote{});
            }
            if (name == "selection.set") {
                SetSelection selection;
                if (!parseIds(arguments[0], selection.ids)) {
                    return badIds(name);
                }
                return edit(std::move(selection));
            }
            if (name == "waypoint.link") {
                return edit(LinkWaypoints{});
            }
            if (name == "selection.property") {
                return parseProperty(name, arguments);
            }
            // One text argument the command needs.
            if (name == "placement.template" || name == "placement.owner" || name == "selection.owner" ||
                name == "selection.unset") {
                if (arguments[0].empty()) {
                    return missing(name);
                }
                std::string text(arguments[0]);
                if (name == "placement.template") {
                    return edit(ChooseTemplate{std::move(text)});
                }
                if (name == "placement.owner") {
                    return edit(SetPlacementOwner{std::move(text)});
                }
                if (name == "selection.owner") {
                    return edit(SetOwner{std::move(text)});
                }
                return edit(UnsetProperty{std::move(text)});
            }
            // One number argument the command needs.
            if (name == "edit.rotate" || name == "placement.angle" || name == "selection.height" ||
                name == "selection.angle") {
                if (arguments[0].empty()) {
                    return missing(name);
                }
                const auto number = parseNumber(arguments[0]);
                if (!number) {
                    return notANumber(name);
                }
                const double value = *number;
                if (name == "edit.rotate") {
                    return edit(RotateSelection{value});
                }
                if (name == "placement.angle") {
                    return edit(SetPlacementAngle{value});
                }
                if (name == "selection.height") {
                    return edit(SetHeight{float(value)});
                }
                return edit(SetAngle{value});
            }
            return fail("error:unknown_command");
        }

        bool startsWith(std::string_view text, std::string_view prefix) {
            return text.substr(0, prefix.size()) == prefix;
        }
    }

    std::optional<double> parseNumber(std::string_view text) {
        text = withoutPlus(text);
        double value = 0;
        const auto end = text.data() + text.size();
        const auto [last, error] = std::from_chars(text.data(), end, value);
        if (text.empty() || error != std::errc() || last != end || !std::isfinite(value)) {
            return std::nullopt;
        }
        return value;
    }

    bool needsMap(const Edit& edit) {
        return std::visit(Overloaded{[](const UseObjectTool&) {
                                         return false;
                                     },
                                     [](const ChooseTemplate&) {
                                         return false;
                                     },
                                     [](const UseWaypointTool&) {
                                         return false;
                                     },
                                     [](const UseRoadTool&) {
                                         return false;
                                     },
                                     [](const SetPlacementOwner&) {
                                         return false;
                                     },
                                     [](const SetPlacementAngle&) {
                                         return false;
                                     },
                                     [](const ShowPicker&) {
                                         return false;
                                     },
                                     [](const CancelPlacement&) {
                                         return false;
                                     },
                                     [](const ClearNote&) {
                                         return false;
                                     },
                                     [](const auto&) {
                                         return true;
                                     }},
                          edit);
    }

    std::variant<Command, Error> parse(std::string_view text) {
        const auto tab = text.find('\t');
        const auto name = text.substr(0, tab);
        const auto arguments = tab == std::string_view::npos ? Arguments() : Arguments(text.substr(tab + 1));

        if (name == "game.open") {
            if (arguments[0].empty()) {
                return fail("error:missing_argument");
            }
            std::optional<std::string> base;
            if (!arguments[1].empty()) {
                base.emplace(arguments[1]);
            }
            return Command{OpenGame{std::string(arguments[0]), std::move(base)}};
        }
        if (name == "model.open") {
            if (arguments[0].empty()) {
                return fail("error:missing_argument");
            }
            return Command{OpenModel{std::string(arguments[0]), std::string(arguments[1]), std::string(arguments[2])}};
        }
        if (name == "model.animation") {
            return Command{PlayAnimation{std::string(arguments[0])}};
        }
        if (name == "map.open") {
            if (arguments[0].empty()) {
                return fail("error:missing_argument");
            }
            return Command{OpenMap{std::string(arguments[0])}};
        }
        if (name == "maps.user") {
            return Command{SetUserMaps{std::string(arguments[0])}};
        }
        if (name == "map.save") {
            if (arguments[0].empty()) {
                return fail("error:missing_argument");
            }
            return Command{SaveMap{std::string(arguments[0])}};
        }
        if (name == "map.save_as") {
            return parseSaveAs(arguments);
        }
        if (name == "map.play") {
            return Command{PlayMap{}};
        }
        if (startsWith(name, "play.")) {
            return parsePlaySetting(name, arguments);
        }
        if (startsWith(name, "map.") || startsWith(name, "edit.") || startsWith(name, "selection.") ||
            startsWith(name, "placement.") || name == "waypoint.link" || name == "note.clear") {
            return parseEdit(name, arguments);
        }
        return fail("error:unknown_command");
    }
}
