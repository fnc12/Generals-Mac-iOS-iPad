#pragma once

#include "commands.h"
#include "game_files.h"
#include "ini_objects.h"
#include "map_document.h"
#include "map_editor.h"
#include "map_file.h"
#include "model_loader.h"
#include "storage.h"
#include "terrain_ini.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <optional>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

namespace end_editor {

    // The editor logic behind the C ABI (editor_core_c.h). Not thread safe: the client calls it
    // from one thread.
    class EditorCore {
      public:
        // Opens (creates) the store and sets status to "idle". Throws std::system_error on failure.
        explicit EditorCore(std::string dbPath);

        const std::string& dbPath() const {
            return this->path;
        }

        // The map name of the test copy map.play writes into the user Maps folder (the user's own
        // map file is not touched by Play).
        static constexpr const char* testMapName = "__EditorTest";

        // Parses the command (commands::parse) and checks it against the state: returns "" when it
        // is accepted (done now or queued as a job), "error:<code>" otherwise.
        std::string send(std::string_view command);

        // Runs queued work.
        void update();

        bool hasPendingWork() const {
            return !this->jobs.empty();
        }
        std::size_t pendingJobCount() const {
            return this->jobs.size();
        }

      private:
        // Queued work, run by update(). The client's commands that become jobs are queued as they
        // were parsed.
        struct LoadMapModels {};  // the next slice of the open map's models
        struct EditMap {
            commands::Edit edit;
        };
        // map.save (any file) and map.save_as (a file in the user Maps folder).
        struct SaveMapTo {
            std::string path;
            bool saveAs = false;  // does not replace another file unless `replace`
            bool replace = false;
        };
        struct ListMaps {};  // the maps table again (maps.user)
        using Job = std::variant<commands::OpenGame,
                                 commands::OpenModel,
                                 commands::PlayAnimation,
                                 commands::OpenMap,
                                 LoadMapModels,
                                 EditMap,
                                 SaveMapTo,
                                 ListMaps,
                                 commands::PlayMap>;
        // Whether a job of one of these kinds is queued.
        template<class... Kinds>
        bool queued() const {
            return std::any_of(this->jobs.begin(), this->jobs.end(), [](const Job& job) {
                return (std::holds_alternative<Kinds>(job) || ...);
            });
        }

        // send(): one kind of command each, after parsing.
        std::string accept(commands::OpenGame&& command);
        std::string accept(commands::OpenModel&& command);
        std::string accept(commands::PlayAnimation&& command);
        std::string accept(commands::OpenMap&& command);
        std::string accept(commands::SetUserMaps&& command);
        std::string accept(commands::SaveMap&& command);
        std::string accept(commands::SaveMapAs&& command);
        std::string accept(commands::PlayMap&& command);
        std::string accept(commands::PlaySetting&& command);
        std::string accept(commands::Edit&& command);
        // The open map, or one queued to open.
        bool hasMap() const;

        void setStatus(const std::string& state,
                       const std::string& message,
                       const std::string& gameDir,
                       std::int64_t objectCount);
        void setModelState(const std::string& name, const std::string& state, const std::string& message);
        void runOpenGame(const commands::OpenGame& command);
        void runOpenModel(const commands::OpenModel& command);
        void runPlayAnimation(const std::string& name);
        void setMapState(const std::string& path, const std::string& state, const std::string& message);
        void clearMap();
        void listMaps();
        void runOpenMap(const std::string& path);
        void runLoadMapModels();
        void queueBeforeModelLoading(Job job);
        void runEditMap(const commands::Edit& edit);
        void runSaveMap(const SaveMapTo& save);
        // Writes the open document and its preview to `target`; "" when written, else what failed
        // (a failed preview sets only `previewError`).
        std::string writeMapFiles(const std::filesystem::path& target, std::string& previewError);
        // map.play: the open map with its edits to the test copy, then the game started on it.
        void runPlayMap();
        // play.*: a setting of the Play button; "" or an error code (the note says why).
        std::string setPlaySetting(const commands::PlaySetting& setting);
        // The default game app for the opened game (GeneralsXZH or GeneralsX in ~/GeneralsX/Apps,
        // game_launch.h).
        std::string defaultGameApp() const;
        // The game's index of a Multiplayer.ini color (its first entry of that name); none for
        // "Random" and names it does not have.
        std::optional<int> colorIndex(std::string_view name) const;
        void writePlay();
        // The started game ended: the play row says so.
        void pollPlay();
        void relistMaps();
        // Writes one object of the open document to map_objects / map_object_properties (and
        // queues its model when the map has not used it yet); removes the rows when it is gone.
        void writeMapObject(std::int64_t id);
        void updateMapEditState(const std::string& message);
        // map_links from the document's waypoint links and road segments.
        void writeMapLinks();
        // The map_editor row and map_selection, when they changed.
        void publishEditor();
        // A refused command: the note right away (send() returns the error code too).
        std::string refuse(const std::string& error, const std::string& note);
        // An edit that names a template or a road type the game does not have: the error code.
        std::string checkNames(const commands::Edit& edit);
        MapEditor::RoadKind roadKind(const std::string& name) const;
        const ObjectInfo* findObject(const std::string& templateName) const;

        std::string path;
        std::unique_ptr<Storage> storage;
        std::deque<Job> jobs;
        std::int64_t revision = 0;
        GameFiles files;
        std::unique_ptr<ModelLoader> loader;
        LoadedModel model;

        // Template names compared without case (ASCII); looked up by a view, without a copy.
        struct NoCaseLess {
            using is_transparent = void;
            bool operator()(std::string_view a, std::string_view b) const;
        };
        std::vector<ObjectInfo> objectInfos;
        // Template name (a view of objectInfos' name) -> objectInfos.
        std::map<std::string_view, std::size_t, NoCaseLess> objectsByName;
        // Models of the open map still to load, in map_models id order.
        struct PendingMapModel {
            std::int64_t id = 0;
            std::string model;
            ModelLoader::Hiding hiding;
        };
        std::vector<PendingMapModel> mapModels;
        std::size_t nextMapModel = 0;
        std::set<std::string> mapTextures;  // names already in map_model_textures
        // Model, muzzle flashes, hidden sub-objects: one map_models row each.
        using ModelKey = std::tuple<std::string, std::string, std::string>;
        std::map<ModelKey, std::int64_t> mapModelIds;  // -> map_models id
        bool mapModelJobQueued = false;
        std::unique_ptr<MapDocument> document;  // the open map, for editing and saving
        std::vector<std::array<float, 3>> tileColors;  // of the open map, for its preview
        std::vector<RoadType> roadTypes;  // of the opened game
        MapEditor editor;  // the user's tool, selection, clipboard and note; outlives the maps
        MapEditor::State published;
        std::int64_t publishedSelection = -1;
        std::string userMapsFolder;  // the game's user Maps folder, listed with the game's maps
        std::int64_t mapListRevision = 0;
        std::vector<std::filesystem::path> gameFolders;  // map.save never writes inside them
        std::vector<std::string> factionNames;  // PlayerTemplate.ini's playable factions
        // Multiplayer.ini's player colors; the position is the game's color index.
        std::vector<std::string> colorNames;
        PlayRow play;  // the Play button's settings and its last run (the store's play row)
        // play.stop asked the game to quit at this time; it is ended for good 3 s later.
        std::optional<std::chrono::steady_clock::time_point> playStopAsked;
    };
}
